// Offline ("non-real-time") GI render UI and job logic: high-quality stills and videos.
#include "app/App.h"

#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "app/Icons.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

namespace {

OfflineJobDesc MakeJobDesc(const AppOptions& o, const AppSettings& st, const VideoRenderConfig& v, bool video,
                           bool prepass) {
    OfflineJobDesc j;
    // Effects: a video takes the render dialog's; a still takes the ones chosen when entering the scene.
    j.bloom = video ? v.bloom : st.bloom;
    j.bloomConvolution = video ? v.bloomConvolution : st.bloomConvolution;
    j.volumetric = video ? v.volumetric : st.volumetric;
    j.volumetricDensity = video ? v.volumetricDensity : st.volumetricDensity;
    if (video) {
        const VideoResolution& r = kVideoResolutions[v.resolution];
        const VideoQualityPreset& q = kVideoQualities[v.quality];
        j.width = r.width;
        j.height = r.height;
        j.minSamples = q.minSamples;
        j.maxSamples = q.maxSamples;
        j.errorThreshold = q.errorThreshold;
        j.prepassRays = q.prepassRays;
        j.maxBounces = q.maxBounces;
    }
    if (o.offlineSize[0] > 0 && o.offlineSize[1] > 0) {
        j.width = (uint32_t)(o.offlineSize[0] & ~1);
        j.height = (uint32_t)(o.offlineSize[1] & ~1);
    }
    j.prepass = prepass;
    if (o.offlineSpp > 0) {
        j.maxSamples = (uint32_t)o.offlineSpp;
        j.minSamples = std::min(j.minSamples, j.maxSamples);
    }
    return j;
}

std::string Timestamp() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

// Replace characters Windows file names reject and control characters with '_', trim to
// 60 bytes without cutting a UTF-8 sequence.
std::string SanitizeFileName(std::string s) {
    const char* bad = "<>:\"/\\|?*";
    for (char& c : s)
        if (c < 0x20 || std::strchr(bad, c)) c = '_';
    if (s.size() > 60) {
        size_t n = 60;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;  // keep whole UTF-8 sequences
        s.resize(n);
    }
    if (s.find_first_not_of(" \t_") == std::string::npos) s = "render";
    return s;
}

// Width ui::Button gives itself for `label` (+ optional icon) with size.x == 0.
float ButtonWidth(const char* label, bool withIcon) {
    return ui::Dp(32.0f) + (withIcon ? ui::Dp(18.0f + 8.0f) : 0.0f) +
           ui::TextSize(ui::Font::Semibold, ui::size::Body, label).x;
}

// "H:MM:SS" when >= 1 hour, else "M:SS" (MinSec).
std::string Hms(double seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 3600.0) return ui::MinSec(seconds);
    const int t = (int)(seconds + 0.5);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

// A VMD camera cut (or a character teleport) between two video frames is not motion: blurring across it smears the whole
// frame over the jump. Thresholds are per video frame at 30 fps and scale with the frame interval.
bool CameraJumped(const CameraParams& a, const CameraParams& b, double fps) {
    using namespace DirectX;
    const float scale = 30.0f / (float)std::max(fps, 1.0);
    const XMMATRIX ia = XMMatrixInverse(nullptr, XMLoadFloat4x4(&a.view));
    const XMMATRIX ib = XMMatrixInverse(nullptr, XMLoadFloat4x4(&b.view));
    const float cosAngle = XMVectorGetX(XMVector3Dot(XMVector3Normalize(ia.r[2]), XMVector3Normalize(ib.r[2])));
    const float angle = std::acos(std::clamp(cosAngle, -1.0f, 1.0f));
    const float shift = XMVectorGetX(XMVector3Length(XMVectorSubtract(ia.r[3], ib.r[3])));
    const float zoom = std::tan(a.fovYRadians * 0.5f) / std::max(std::tan(b.fovYRadians * 0.5f), 1e-4f);
    return angle > XMConvertToRadians(12.0f) * scale || shift > 40.0f * scale || zoom > 1.6f || zoom < 0.625f;
}

constexpr float kPoseJumpUnits = 15.0f;   // character centre bone travel per 30 fps frame treated as a teleport

}  // namespace

// ---------------------------------------------------------------------------
// Offline output directory
// ---------------------------------------------------------------------------

std::filesystem::path App::OfflineOutputDir(bool video) const {
    std::filesystem::path dir;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(video ? FOLDERID_Videos : FOLDERID_Pictures, 0, nullptr, &p))) {
        dir = std::filesystem::path(p) / L"MMDX12";
        CoTaskMemFree(p);
    } else {
        dir = ExecutableDir() / (video ? L"videos" : L"screenshots");
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// ---------------------------------------------------------------------------
// Job starts
// ---------------------------------------------------------------------------

void App::StartOfflineStill() {
    if (!scene_ || !renderer_.OfflineSupported() || offline_.mode != OfflineMode::None) return;
    const bool wasPlaying = playing_;
    SetPlaying(false);
    offline_ = OfflineJob{};
    offline_.mode = OfflineMode::Still;
    offline_.wasPlaying = wasPlaying;
    offline_.fromCli = !options_.offlineStill.empty();
    offline_.output = offline_.fromCli ? options_.offlineStill
                                       : OfflineOutputDir(false) / Utf8ToPath("MMDX12_" + Timestamp() + ".png");
    offline_.startSeconds = playTime_;
    offline_.frame = 0;
    offline_.frameCount = 1;
    offline_.startWall = timeSeconds_;
    offline_.beginPending = true;
    screen_ = Screen::Offline;
    LOG_INFO("offline still: %s", PathToUtf8(offline_.output).c_str());
}

void App::StartOfflineVideo(double startSeconds, double endSeconds, bool fromLobby, bool probe, bool background) {
    if (!scene_ || offline_.mode != OfflineMode::None) return;
    const VideoRenderConfig cfg = ActiveVideoConfig();
    const bool cli = !options_.offlineVideo.empty() || (probe && options_.offlineProbe);
    // Leaves a render that cannot start: back to the lobby (with the dialog open again after a probe).
    const auto abort = [&](const char* title, const std::string& detail) {
        if (background) {   // a sample render behind the dialog fails quietly; the estimate stays a rough one
            bgProbe_.failedKey = CurrentVideoProbeKey();
            offline_ = OfflineJob{};
            return;
        }
        toast_ = {title, detail, {}, true, timeSeconds_ + 8.0};
        offline_ = OfflineJob{};
        if (fromLobby) {
            UnloadScene();
            screen_ = Screen::Select;
            videoDialogOpen_ = probe;
        }
        if (cli) running_ = false;
    };
    if (cfg.Gi() && !renderer_.OfflineSupported()) {
        abort("영상 렌더를 시작할 수 없습니다", "이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다");
        return;
    }
    if ((cfg.Renderer() == VideoRenderer::RayTraced || cfg.Renderer() == VideoRenderer::PathTraced) &&
        !renderer_.RayTracingSupported()) {
        abort("영상 렌더를 시작할 수 없습니다", "이 그래픽 카드에서는 레이 트레이싱을 사용할 수 없습니다");
        return;
    }
    if (!background) SetPlaying(false);
    const double duration = scene_->endFrame / kMmdFps;
    startSeconds = std::clamp(startSeconds, 0.0, duration);
    endSeconds = std::clamp(endSeconds, startSeconds, duration);
    offline_ = OfflineJob{};
    offline_.mode = probe ? OfflineMode::Probe : OfflineMode::Video;
    offline_.background = background;
    offline_.fromCli = cli;
    offline_.fromLobby = fromLobby;
    offline_.video = cfg;
    offline_.realtime = !cfg.Gi();
    const std::string song = selSong_ >= 0 ? library_.songs[selSong_].displayName : "render";
    if (probe)
        offline_.output = std::filesystem::temp_directory_path() / L"mmdx12_probe.mp4";
    else
        offline_.output =
            offline_.fromCli
                ? options_.offlineVideo
                : OfflineOutputDir(true) / Utf8ToPath("MMDX12_" + SanitizeFileName(song) + "_" + Timestamp() + ".mp4");
    offline_.startSeconds = startSeconds;
    offline_.frameCount =
        probe ? (offline_.realtime ? 3 : 1)
              : std::max(1, (int)std::floor((endSeconds - startSeconds) * offline_.video.fps + 1e-6));
    if (probe) offline_.probeKey = CurrentVideoProbeKey();
    const OfflineJobDesc jd = MakeJobDesc(options_, settings_, offline_.video, true, false);
    VideoEncoder::Desc d;
    d.width = jd.width;
    d.height = jd.height;
    d.fps = (uint32_t)offline_.video.fps;
    d.videoBitrate = (uint32_t)offline_.video.bitrateMbps * 1000000u;
    if (!probe && scene_->hasAudio && selSong_ >= 0) d.audioPath = library_.songs[selSong_].audioPath;
    d.audioStartSeconds = startSeconds;
    offline_.encoder = std::make_unique<VideoEncoder>();
    std::string err;
    if (!offline_.encoder->Open(offline_.output, d, &err)) {
        abort(probe ? "시간을 측정할 수 없습니다" : "영상 렌더를 시작할 수 없습니다", err);
        return;
    }
    if (offline_.realtime) {
        // the real-time renderer produces the frames at the video's resolution; frame 0 is a warm-up frame
        RenderSettings rs = VideoRealtimeSettings(cfg);
        rs.fixedWidth = jd.width;
        rs.fixedHeight = jd.height;
        rs.headless = background;
        renderer_.SetSettings(rs);
        offline_.iterCount =
            cfg.Renderer() == VideoRenderer::PathTraced ? (int)kVideoRealtimeQualities[cfg.quality].ptPasses : 1;
        lastRenderedTime_ = -1.0;  // the first frame has no temporal history
    }
    scene_->physicsFrame = -1.0f;  // physics restarts from the animated pose at frame 0 of the video
    offline_.startWall = timeSeconds_;
    offline_.beginPending = true;
    if (background) {
        renderer_.SetOfflinePresent(false);   // nothing may reach the back buffer: the dialog is on screen
        return;
    }
    screen_ = Screen::Offline;
    LOG_INFO("offline %s: %s, %d frames -> %s", probe ? "probe" : "video", kVideoRenderers[cfg.renderer].label,
             offline_.frameCount, PathToUtf8(offline_.output).c_str());
}

// ---------------------------------------------------------------------------
// Pose ring for motion blur
// ---------------------------------------------------------------------------

void App::SaveOfflinePose() {
    offline_.prevPose.clear();
    if (!scene_) return;
    const auto save = [&](const ModelInstance& m) {
        offline_.prevPose.push_back({m.SkinMatrices(), m.VertexMorphDeltas(), m.MorphVersion()});
    };
    save(*scene_->character);
    for (const auto& st : scene_->stages) save(*st);
    offline_.prevCenter = CharacterCenter();
}

DirectX::XMFLOAT3 App::CharacterCenter() const {
    if (!scene_ || !scene_->character) return {};
    const int center = scene_->character->Model().FindBone("ã»ã³ã¿ã¼");
    return center >= 0 ? scene_->character->BoneWorldPosition(center) : DirectX::XMFLOAT3{};
}

void App::UploadOfflinePrevPose(uint64_t slot) {
    if (!scene_ || offline_.prevPose.size() != 1 + scene_->stages.size()) return;
    const auto upload = [&](GpuModel& g, const OfflineJob::Pose& p) {
        g.UpdateSkinning(slot, p.skin);
        g.UpdateMorphs(slot, p.morph, p.morphVersion);
    };
    upload(*scene_->characterGpu, offline_.prevPose[0]);
    for (size_t i = 0; i < scene_->stages.size(); ++i) upload(*scene_->stageGpu[i], offline_.prevPose[1 + i]);
}

// ---------------------------------------------------------------------------
// Per-frame job logic
// ---------------------------------------------------------------------------

void App::UpdateOffline() {
    if (screen_ == Screen::Play && !cliOfflineStarted_ && scene_ &&
        (!options_.offlineStill.empty() || !options_.offlineVideo.empty() || options_.offlineProbe)) {
        cliOfflineStarted_ = true;
        if (!options_.offlineStill.empty()) {
            if (!renderer_.OfflineSupported()) {
                LOG_ERROR("offline render unavailable (needs DXR)");
                running_ = false;
                return;
            }
            StartOfflineStill();
        } else if (options_.offlineVideo.empty()) {
            StartOfflineProbe();
        } else {
            const double duration = scene_->endFrame / kMmdFps;
            const double a = options_.offlineRange[0] >= 0 ? options_.offlineRange[0] : 0.0;
            const double b = options_.offlineRange[1] >= 0 ? options_.offlineRange[1] : duration;
            StartOfflineVideo(a, b);
        }
        return;
    }
    if (screen_ == Screen::Offline && !ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape))
        offline_.cancelRequested = true;
}

void App::RecordRealtimeVideoFrame(ID3D12GraphicsCommandList* cmd) {
    const bool newFrame = offline_.beginPending;
    if (newFrame) {
        // a new video frame: pose it once; its further passes (path tracer history) re-upload the same pose
        playTime_ = offline_.startSeconds + (double)offline_.frame / offline_.video.fps;
        offline_.frameMmd = (float)(playTime_ * kMmdFps);
        UpdateScene(offline_.frameMmd);
        offline_.iter = 0;
        offline_.beginPending = false;
        offline_.imageStartWall = timeSeconds_;
        offline_.imageStartClock = std::chrono::steady_clock::now();
    } else {
        UploadOfflinePrevPose(ctx_.FrameNumber());
    }
    FrameView view;
    BuildFrameView(offline_.frameMmd, view);   // a camera cut (first frame, seek) discards temporal history
    if (newFrame) {
        const VideoRenderConfig& cfg = offline_.video;
        if (cfg.Renderer() == VideoRenderer::PathTraced) {
            const int q = std::clamp(cfg.quality, 0, kVideoQualityCount - 1);
            bool jumpCut = false;
            if (offline_.frame > 0) {
                if (CameraJumped(offline_.prevCamera, view.camera, cfg.fps)) jumpCut = true;
                const DirectX::XMFLOAT3 c = CharacterCenter();
                const DirectX::XMVECTOR d = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&c),
                                                                      DirectX::XMLoadFloat3(&offline_.prevCenter));
                if (DirectX::XMVectorGetX(DirectX::XMVector3Length(d)) > kPoseJumpUnits * 30.0f / (float)cfg.fps) {
                    jumpCut = true;
                }
            }
            if (jumpCut) view.cameraCut = true;
            const bool warmUp = offline_.frame == 0 || view.cameraCut;
            offline_.iterCount = (int)(warmUp ? kVideoRealtimeQualities[q].ptPasses
                                              : kVideoRealtimeQualities[q].ptSteadyPasses);
        } else {
            offline_.iterCount = 1;
        }
        offline_.prevCamera = view.camera;
        SaveOfflinePose();
    }
    renderer_.Render(cmd, view);
}

void App::RecordOfflineFrame(ID3D12GraphicsCommandList* cmd) {
    if (offline_.realtime) {
        RecordRealtimeVideoFrame(cmd);
        return;
    }
    if (!offline_.beginPending) {
        renderer_.RenderOffline(cmd);
        return;
    }
    const bool probe = offline_.mode == OfflineMode::Probe;
    const bool video = offline_.mode == OfflineMode::Video || probe;
    playTime_ = offline_.startSeconds + (video ? (double)offline_.frame / offline_.video.fps : 0.0);
    const float frame = (float)(playTime_ * kMmdFps);
    if (probe) {
        // a sample frame from the middle of a video: the shutter opens at the previous video frame's pose
        const float prevFrame = frame - kMmdFps / (float)offline_.video.fps;
        UpdateScene(prevFrame);
        FrameView pv;
        BuildFrameView(prevFrame, pv);
        offline_.prevCamera = pv.camera;
        SaveOfflinePose();
    }
    // Motion blur opens the shutter at the previous pose, held in the models' previous ring entry:
    // video re-uploads the previous video frame's pose there; a still taken during playback finds
    // the last live frame there already.
    const bool blur = video ? (offline_.frame > 0 || probe) : (offline_.wasPlaying && haveLastLiveCamera_);
    if (video && blur) {
        ctx_.WaitForGpu();   // the ring entry may still be read by in-flight offline work
        UploadOfflinePrevPose(ctx_.FrameNumber() - 1);
    }
    UpdateScene(frame);
    FrameView view;
    BuildFrameView(frame, view);
    view.motionBlur = blur;
    view.prevCamera = !blur ? view.camera : (video ? offline_.prevCamera : lastLiveCamera_);
    if (video && blur && !probe) {
        // camera cut / teleport since the previous video frame: no blur across the jump
        if (CameraJumped(view.prevCamera, view.camera, offline_.video.fps)) view.prevCamera = view.camera;
        const DirectX::XMFLOAT3 c = CharacterCenter();
        const DirectX::XMVECTOR d = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&c),
                                                              DirectX::XMLoadFloat3(&offline_.prevCenter));
        if (DirectX::XMVectorGetX(DirectX::XMVector3Length(d)) > kPoseJumpUnits * 30.0f / (float)offline_.video.fps) {
            SaveOfflinePose();                         // the shutter opens at the new pose
            UploadOfflinePrevPose(ctx_.FrameNumber() - 1);
        }
    }
    if (video) {
        SaveOfflinePose();
        offline_.prevCamera = view.camera;
    }
    // the irradiance cache prepass is the render's GI: every image, video frames included
    if (!renderer_.BeginOffline(cmd, view, MakeJobDesc(options_, settings_, offline_.video, video, true))) {
        if (!offline_.background) {
            renderer_.Render(cmd, view);  // keep this frame valid
            toast_ = {"고품질 렌더를 시작할 수 없습니다", "레이 트레이싱 장면을 만들지 못했습니다", {}, true,
                      timeSeconds_ + 8.0};
        }
        offline_.cancelRequested = true;  // AfterOfflineFrame finishes the job
        offline_.beginPending = false;
        return;
    }
    offline_.beginPending = false;
    offline_.imageStartWall = timeSeconds_;
    offline_.imageStartClock = std::chrono::steady_clock::now();
}

void App::AfterOfflineFrame() {
    if (offline_.mode == OfflineMode::None) return;
    if (offline_.cancelRequested) {
        renderer_.CancelOffline();
        FinishOffline(true);
        return;
    }
    if (offline_.beginPending) return;
    ImageRGBA8 img;
    if (offline_.realtime) {
        if (++offline_.iter < offline_.iterCount) return;   // more passes accumulate into this frame
        if (!renderer_.ReadFinalImage(img)) {
            toast_ = {"렌더 결과를 읽을 수 없습니다", "", {}, true, timeSeconds_ + 8.0};
            FinishOffline(true);
            return;
        }
    } else {
        if (renderer_.OfflineStatus().phase != OfflinePhase::Done) return;
        if (!renderer_.ReadOfflineImage(img)) {
            toast_ = {"렌더 결과를 읽을 수 없습니다", "", {}, true, timeSeconds_ + 8.0};
            FinishOffline(true);
            return;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - offline_.imageStartClock).count();
    offline_.avgImageSeconds =
        offline_.frame == 0 ? secs : (offline_.avgImageSeconds * offline_.frame + secs) / (offline_.frame + 1);
    if (offline_.mode == OfflineMode::Still) {
        std::error_code ec;
        std::filesystem::create_directories(offline_.output.parent_path(), ec);
        const bool ok = SavePngRGBA8(offline_.output, img.Width(), img.Height(), img.mips[0].pixels.data(), img.Width() * 4);
        toast_ = ok ? Toast{"고품질 스크린샷을 저장했습니다", PathToUtf8(offline_.output.filename()), offline_.output,
                            false, timeSeconds_ + 8.0}
                    : Toast{"스크린샷을 저장할 수 없습니다", PathToUtf8(offline_.output), {}, true, timeSeconds_ + 8.0};
        offline_.frame = 1;
        FinishOffline(!ok);
        return;
    }
    const auto encodeStart = std::chrono::steady_clock::now();
    const bool added = offline_.encoder && offline_.encoder->AddFrame(img);
    const double encodeSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - encodeStart).count();
    offline_.avgEncodeSeconds = offline_.frame == 0 ? encodeSecs
                                                    : (offline_.avgEncodeSeconds * offline_.frame + encodeSecs) /
                                                          (offline_.frame + 1);
    if (!added) {
        toast_ = {"영상 인코딩에 실패했습니다", "", {}, true, timeSeconds_ + 8.0};
        FinishOffline(true);
        return;
    }
    // a sample render measures the frames after the first of the real-time renderers (the first one warms
    // the GPU up); the GI renderer's single image is long enough that warming up does not matter
    if (offline_.mode == OfflineMode::Probe && (!offline_.realtime || offline_.frame >= 1)) {
        offline_.probeSum += secs + encodeSecs;
        ++offline_.probeN;
    }
    LOG_INFO("offline video: frame %d/%d (%.3f s, encode %.3f s)", offline_.frame + 1, offline_.frameCount, secs,
             encodeSecs);
    if (++offline_.frame >= offline_.frameCount)
        FinishOffline(false);
    else
        offline_.beginPending = true;
}

void App::FinishOffline(bool cancelled) {
    if (offline_.background) {
        // a sample render behind the dialog: keep the measurement, leave the screen and the scene alone
        if (!cancelled && offline_.probeN >= 1) {
            const double perFrame = offline_.probeSum / offline_.probeN;
            settings_.SetVideoProbe(offline_.probeKey, perFrame);
            settings_.Save(settingsPath_);
            LOG_INFO("VIDEO PROBE %s: %.3f s per frame (%d measured)", kVideoRenderers[offline_.video.renderer].label,
                     perFrame, offline_.probeN);
        } else {
            bgProbe_.failedKey = offline_.probeKey;
        }
        CancelBackgroundProbe();
        return;
    }
    const bool probe = offline_.mode == OfflineMode::Probe;
    const bool video = offline_.mode == OfflineMode::Video;
    const bool cli = offline_.fromCli;
    const bool lobby = (video || probe) && offline_.fromLobby;
    if (offline_.realtime) ApplyRenderSettings();   // back from the video's fixed-resolution settings
    if ((video || probe) && offline_.encoder) {
        const uint32_t written = offline_.encoder->FramesWritten();
        offline_.encoder->Finish();
        std::error_code ec;
        if (probe) {
            std::filesystem::remove(offline_.output, ec);   // the sample frame is only measured, never kept
        } else if (written == 0) {
            std::filesystem::remove(offline_.output, ec);
            if (!toast_.error || toast_.until < timeSeconds_)  // keep an error toast set just before
                toast_ = {"영상 렌더를 취소했습니다", "", {}, false, timeSeconds_ + 6.0};
        } else if (cancelled) {
            if (!toast_.error || toast_.until < timeSeconds_)
                toast_ = {"영상 렌더를 중단했습니다",
                          std::to_string(written) + "프레임까지 저장했습니다 · " + PathToUtf8(offline_.output.filename()),
                          offline_.output, false, timeSeconds_ + 10.0};
        } else {
            toast_ = {"고품질 영상을 저장했습니다", PathToUtf8(offline_.output.filename()), offline_.output, false,
                      timeSeconds_ + 10.0};
        }
    } else if (!video && !probe && cancelled && (!toast_.error || toast_.until < timeSeconds_)) {
        toast_ = {"스크린샷 렌더를 취소했습니다", "", {}, false, timeSeconds_ + 5.0};
    }
    if (probe) {
        if (!cancelled && offline_.probeN >= 1) {
            const double perFrame = offline_.probeSum / offline_.probeN;
            settings_.SetVideoProbe(offline_.probeKey, perFrame);
            settings_.Save(settingsPath_);
            LOG_INFO("VIDEO PROBE %s: %.3f s per frame (%d measured)", kVideoRenderers[offline_.video.renderer].label,
                     perFrame, offline_.probeN);
            if (!toast_.error || toast_.until < timeSeconds_) {
                char detail[96];
                std::snprintf(detail, sizeof(detail), "프레임당 %.1f초 기준으로 예상 시간을 계산했습니다", perFrame);
                toast_ = {"시간 측정을 마쳤습니다", detail, {}, false, timeSeconds_ + 6.0};
            }
        } else if (!toast_.error || toast_.until < timeSeconds_) {
            toast_ = {"시간 측정을 취소했습니다", "", {}, false, timeSeconds_ + 5.0};
        }
    }
    LOG_INFO("offline render %s: %s", cancelled ? "cancelled" : "finished", PathToUtf8(offline_.output).c_str());
    playTime_ = offline_.startSeconds;
    if (scene_ && scene_->hasAudio) audio_.Seek(playTime_);
    lastRenderedTime_ = -1.0;  // next real-time frame is a camera cut (no stale history)
    offline_ = OfflineJob{};
    if (lobby) {
        UnloadScene();   // a render started from the select screen goes back to it
        screen_ = Screen::Select;
        if (probe) videoDialogOpen_ = true;   // the measurement answers the dialog it came from
    } else {
        screen_ = Screen::Play;
    }
    if (cli) running_ = false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void App::DrawOfflineOverlay() {
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    if (!scene_) return;
    OfflineProgress pr = renderer_.OfflineStatus();
    const Palette& p = P();
    const bool video = offline_.mode == OfflineMode::Video;
    const bool probe = offline_.mode == OfflineMode::Probe;
    if (offline_.realtime) {   // the real-time renderers report their own progress
        pr = {};
        pr.phase = OfflinePhase::Render;
        pr.width = renderer_.Stats().outputWidth;
        pr.height = renderer_.Stats().outputHeight;
        pr.fraction = offline_.iterCount > 0 ? (float)offline_.iter / (float)offline_.iterCount : 0.0f;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##offlineoverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = io.DisplaySize;

    // ---- bottom-centred progress panel
    const float w = std::min(ds.x - Dp(40.0f), Dp(720.0f));
    const float h = Dp(video ? 168.0f : 128.0f);
    const ImVec2 a((ds.x - w) * 0.5f, ds.y - Dp(24.0f) - h), b(a.x + w, a.y + h);
    const float rect0[4] = {0, 0, 0, 0};
    FrostedPanel(dl, a, b, Dp(20.0f), (ImTextureID)0, rect0);
    const float x0 = a.x + Dp(20.0f), x1 = b.x - Dp(20.0f);
    const float cx = (a.x + b.x) * 0.5f;

    // row 1: icon + title, cancel button on the right
    const float y = a.y;
    {
        const float cancelW = ButtonWidth("취소", true);
        ImGui::SetCursorScreenPos(ImVec2(x1 - cancelW, y + Dp(14.0f)));
        if (Button("##offcancel", "취소", icon::X, ButtonKind::Secondary, ImVec2(0, 34)))
            offline_.cancelRequested = true;
        Icon(dl, video ? icon::FilmStrip : icon::Image, Dp(20.0f), ImVec2(x0 + Dp(10.0f), y + Dp(28.0f)), p.accent);
        Text(dl, Font::Semibold, size::Title, ImVec2(x0 + Dp(30.0f), y + Dp(18.0f)), p.ink,
             video ? "고품질 영상 렌더링" : probe ? "샘플 렌더링으로 시간 측정 중" : "고품질 스크린샷 렌더링");
    }

    // row 2: status + elapsed
    std::string status;
    if (offline_.beginPending || pr.phase == OfflinePhase::Idle) {
        status = "장면 준비 중…";
    } else if (offline_.realtime) {
        status = offline_.iterCount > 1 ? "프레임 렌더링 · " + std::to_string(std::min(offline_.iter + 1, offline_.iterCount)) +
                                              "/" + std::to_string(offline_.iterCount)
                                        : "프레임 렌더링 중";
    } else if (pr.phase == OfflinePhase::Prepass) {
        status = "이래디언스 캐시 프리패스 · 메인 패스 (" + std::to_string(pr.prepassStep + 1) + "/" +
                 std::to_string(pr.prepassSteps) + ")";
    } else if (pr.phase == OfflinePhase::Render) {
        status = "패스 트레이싱 · " + std::to_string(pr.samples) + " spp · 수렴 " +
                 std::to_string((int)(100.0f * (1.0f - pr.activeFraction))) + "%";
    } else {
        status = "마무리 중…";
    }
    Text(dl, Font::Regular, size::Body, ImVec2(x0, y + Dp(56.0f)), p.ink2, status.c_str());
    {
        const std::string right = video
                                      ? "이번 프레임 " + MinSec(timeSeconds_ - offline_.imageStartWall)
                                      : "경과 " + MinSec(timeSeconds_ - offline_.startWall);
        const ImVec2 ts = TextSize(Font::Regular, size::Caption, right.c_str());
        Text(dl, Font::Regular, size::Caption, ImVec2(x1 - ts.x, y + Dp(57.0f)), p.ink3, right.c_str());
    }

    // row 3: per-image progress bar
    ProgressBar(dl, ImVec2(x0, y + Dp(84.0f)), ImVec2(x1, y + Dp(90.0f)), pr.fraction);

    // rows 4/5 (video only): frame counts, total ETA, overall bar
    if (video) {
        const std::string left =
            "프레임 " + std::to_string(std::min(offline_.frame + 1, offline_.frameCount)) + " / " +
            std::to_string(offline_.frameCount);
        Text(dl, Font::Semibold, size::Body, ImVec2(x0, y + Dp(102.0f)), p.ink, left.c_str());
        const double elapsed = timeSeconds_ - offline_.startWall;
        const double eta = (offline_.avgImageSeconds + offline_.avgEncodeSeconds) *
                           (offline_.frameCount - offline_.frame - (double)pr.fraction);
        const std::string right = "경과 " + Hms(elapsed) + " · 남은 시간 " +
                                  (offline_.frame >= 1 ? "약 " + Hms(eta) : "계산 중");
        const ImVec2 ts = TextSize(Font::Regular, size::Caption, right.c_str());
        Text(dl, Font::Regular, size::Caption, ImVec2(x1 - ts.x, y + Dp(104.0f)), p.ink3, right.c_str());
        const float frac = offline_.frameCount > 0
                               ? (float)((offline_.frame + (double)pr.fraction) / offline_.frameCount)
                               : 0.0f;
        ProgressBar(dl, ImVec2(x0, y + Dp(128.0f)), ImVec2(x1, y + Dp(134.0f)), frac);
    }

    // last line: format + output file name
    {
        std::string line = std::to_string(pr.width) + "×" + std::to_string(pr.height) + " · ";
        if (video)
            line += std::to_string(offline_.video.fps) + " fps · " + std::to_string(offline_.video.bitrateMbps) + " Mbps · ";
        line += PathToUtf8(offline_.output.filename());
        const ImVec2 ts = TextSize(Font::Regular, size::Caption, line.c_str());
        Text(dl, Font::Regular, size::Caption, ImVec2(cx - ts.x * 0.5f, b.y - Dp(26.0f)), p.ink3, line.c_str());
    }

    // ---- top-left pill
    {
        const std::string labelText =
            offline_.realtime ? std::string("영상 렌더 · ") + kVideoRenderers[offline_.video.renderer].label + " · Esc 취소"
                              : "비실시간 렌더 · 전역 조명(GI) · Esc 취소";
        const char* label = labelText.c_str();
        const ImVec2 ts = TextSize(Font::Semibold, size::Caption, label);
        const float pw = ts.x + Dp(28.0f), ph = Dp(30.0f);
        const ImVec2 pa(Dp(20.0f), Dp(20.0f)), pb(pa.x + pw, pa.y + ph);
        FrostedPanel(dl, pa, pb, ph * 0.5f, (ImTextureID)0, rect0);
        Text(dl, Font::Semibold, size::Caption, ImVec2(pa.x + Dp(14.0f), pa.y + (ph - ts.y) * 0.5f), p.ink2, label);
    }

    ImGui::End();
}

VideoRenderConfig App::ActiveVideoConfig() const {
    VideoRenderConfig c = settings_.video;
    if (options_.offlineFps == 24 || options_.offlineFps == 30 || options_.offlineFps == 60) c.fps = options_.offlineFps;
    if (options_.offlineBitrate > 0) c.bitrateMbps = options_.offlineBitrate;
    if (options_.offlineQuality >= 0) c.quality = options_.offlineQuality;
    if (options_.offlineRenderer >= 0) c.renderer = options_.offlineRenderer;
    if (options_.dof >= 0) c.dof = options_.dof != 0;
    if (options_.volumetric >= 0) c.volumetric = options_.volumetric != 0;
    if (options_.bloomConv >= 0) c.bloomConvolution = options_.bloomConv != 0;
    c.Clamp();
    return c;
}

RenderSettings App::VideoRealtimeSettings(const VideoRenderConfig& cfg) const {
    const VideoResolution& res = kVideoResolutions[std::clamp(cfg.resolution, 0, kVideoResolutionCount - 1)];
    const VideoRealtimeQuality& q = kVideoRealtimeQualities[std::clamp(cfg.quality, 0, kVideoQualityCount - 1)];
    RenderSettings rs = renderer_.Settings();
    rs.fixedResolution = true;
    rs.fixedWidth = res.width;
    rs.fixedHeight = res.height;
    rs.vsync = false;                    // frames are rendered as fast as they complete
    rs.upscaler = UpscalerKind::None;
    rs.renderScale = 1.0f;
    rs.taa = false;
    rs.renderPath = cfg.Renderer() == VideoRenderer::PathTraced  ? RenderPath::PathTraced
                    : cfg.Renderer() == VideoRenderer::RayTraced ? RenderPath::RayTraced
                                                                  : RenderPath::Raster;
    rs.msaaSamples = q.msaa;
    rs.shadows = true;
    rs.shadowMapSize = q.shadowMapSize;
    rs.ssao = true;
    rs.ssr = true;
    rs.ptSamples = q.ptSamples;
    rs.ptBounces = q.ptBounces;
    rs.bloom = cfg.bloom;
    rs.bloomConvolution = cfg.bloomConvolution;
    rs.volumetric = cfg.volumetric;
    rs.volumetricDensity = cfg.volumetricDensity;
    rs.dof = cfg.dof;
    rs.dofAperture = cfg.dofAperture;
    return rs;
}

void App::StartVideoRenderLoad() {
    if (selCharacter_ < 0 || selSong_ < 0 || !VideoRendererAvailable(settings_.video.Renderer())) return;
    settings_.video.Clamp();
    settings_.Save(settingsPath_);
    videoDialogOpen_ = false;
    StartLoad(LoadTarget::OfflineVideo, &library_.characters[(size_t)selCharacter_],
              selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr, &library_.songs[(size_t)selSong_]);
}

void App::StartOfflineProbe() {
    if (!scene_) return;
    const double t = scene_->endFrame / kMmdFps * 0.45;   // a representative moment: neither the intro nor the outro
    StartOfflineVideo(t, t + 1.0 / std::max(1, ActiveVideoConfig().fps), /*fromLobby*/ false, /*probe*/ true);
}

// ---------------------------------------------------------------------------
// Background sample render for the time estimate of the render dialog
// ---------------------------------------------------------------------------

void App::CancelBackgroundProbe() {
    if (!offline_.background) return;
    const bool realtime = offline_.realtime;
    if (!realtime) renderer_.CancelOffline();
    if (offline_.encoder) {
        offline_.encoder->Finish();
        std::error_code ec;
        std::filesystem::remove(offline_.output, ec);   // the sample frame is only measured, never kept
    }
    offline_ = OfflineJob{};
    renderer_.SetOfflinePresent(true);
    if (realtime) {
        RenderSettings rs = renderer_.Settings();
        rs.headless = false;
        renderer_.SetSettings(rs);
        ApplyRenderSettings();
    }
    lastRenderedTime_ = -1.0;
}

void App::StopVideoProbe(bool unloadScene) {
    CancelBackgroundProbe();
    bgProbe_.pendingKey = 0;
    if (unloadScene && scene_ && bgProbe_.sceneKey != 0 && screen_ == Screen::Select) UnloadScene();
    if (!scene_) bgProbe_.sceneKey = 0;
}

void App::PollProbeScene() {
    if (!bgProbe_.loading || !bgProbe_.loadFuture.valid()) return;
    if (bgProbe_.loadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    bool ok = bgProbe_.loadFuture.get();
    bgProbe_.loading = false;
    const bool wanted = videoDialogOpen_ && screen_ == Screen::Select && !scene_;
    if (ok && wanted) {
        bgProbe_.package->audioPath.clear();   // the sample render needs no sound
        ok = BuildSceneRuntime(*bgProbe_.package);
        if (ok) {
            bgProbe_.sceneKey = bgProbe_.loadingSceneKey;
            playing_ = false;
            useMotionCamera_ = scene_->camera != nullptr && !options_.freeCamera;
        }
    } else if (!wanted) {
        ok = true;   // nobody waits for it any more: just drop the package
    }
    if (!ok) {
        LOG_WARN("video probe: scene unavailable (%s)", bgProbe_.error.c_str());
        bgProbe_.failedKey = CurrentVideoProbeKey();
    }
    bgProbe_.package.reset();
}

void App::StartBackgroundProbe(uint64_t key) {
    (void)key;
    if (!scene_ || offline_.mode != OfflineMode::None) return;
    useMotionCamera_ = scene_->camera != nullptr && !options_.freeCamera;
    const double t = scene_->endFrame / kMmdFps * 0.45;
    StartOfflineVideo(t, t + 1.0 / std::max(1, ActiveVideoConfig().fps), /*fromLobby*/ false, /*probe*/ true,
                      /*background*/ true);
}

void App::UpdateVideoProbe() {
    const bool want = videoDialogOpen_ && screen_ == Screen::Select && selCharacter_ >= 0 && selSong_ >= 0;
    if (!want) {
        if (offline_.background || bgProbe_.sceneKey != 0) StopVideoProbe(true);
        bgProbe_.pendingKey = 0;
        return;
    }
    if (offline_.mode != OfflineMode::None && !offline_.background) return;   // a real render owns the renderer
    if (!VideoRendererAvailable(ActiveVideoConfig().Renderer())) {
        CancelBackgroundProbe();
        bgProbe_.pendingKey = 0;
        return;
    }
    const uint64_t key = CurrentVideoProbeKey();
    if (settings_.FindVideoProbe(key)) {   // this combination was measured before: reuse it
        CancelBackgroundProbe();
        bgProbe_.pendingKey = 0;
        return;
    }
    if (offline_.background) {
        if (offline_.probeKey == key) return;   // already measuring exactly this
        CancelBackgroundProbe();                // the settings changed meanwhile: start over
    }
    if (bgProbe_.failedKey == key) return;

    // the scene is loaded once, at the first combination that needs measuring
    const CharacterAsset& ch = library_.characters[(size_t)selCharacter_];
    const std::string stageId = selStage_ >= 0 ? library_.stages[(size_t)selStage_].id : std::string();
    const SongAsset& song = library_.songs[(size_t)selSong_];
    const uint64_t sceneKey = VideoProbeKey(VideoRenderConfig{}, ch.id, stageId, song.id) | 1u;
    if (!scene_ || bgProbe_.sceneKey != sceneKey) {
        if (scene_ && bgProbe_.sceneKey != 0) UnloadScene();
        if (bgProbe_.loading || scene_) return;
        bgProbe_.package = std::make_unique<ScenePackage>();
        bgProbe_.error.clear();
        bgProbe_.progress.fraction.store(0.0f, std::memory_order_relaxed);
        bgProbe_.loadingSceneKey = sceneKey;
        bgProbe_.loading = true;
        const CharacterAsset c = ch;
        const bool hasStage = selStage_ >= 0;
        const StageAsset st = hasStage ? library_.stages[(size_t)selStage_] : StageAsset{};
        const SongAsset so = song;
        bgProbe_.loadFuture = std::async(std::launch::async, [=, pkg = bgProbe_.package.get(), this] {
            return LoadScenePackage(c, hasStage ? &st : nullptr, so, *pkg, &bgProbe_.progress, &bgProbe_.error);
        });
        return;
    }

    // let a slider drag or a quick series of clicks settle before spending GPU time on a sample
    if (key != bgProbe_.pendingKey) {
        bgProbe_.pendingKey = key;
        bgProbe_.pendingSince = timeSeconds_;
        return;
    }
    if (timeSeconds_ - bgProbe_.pendingSince < 0.8) return;
    StartBackgroundProbe(key);
}

App::VideoProbeStatus App::ProbeStatus() const {
    VideoProbeStatus st;
    if (!videoDialogOpen_ || selCharacter_ < 0 || selSong_ < 0) return st;
    if (offline_.background) {
        st.phase = VideoProbeStatus::Phase::Measuring;
        const double image = offline_.realtime
                                 ? (offline_.iterCount > 0 ? (double)offline_.iter / offline_.iterCount : 0.0)
                                 : (double)renderer_.OfflineStatus().fraction;
        st.fraction = offline_.frameCount > 0
                          ? (float)std::clamp((offline_.frame + image) / offline_.frameCount, 0.0, 1.0)
                          : 0.0f;
        return st;
    }
    const uint64_t key = CurrentVideoProbeKey();
    if (settings_.FindVideoProbe(key) || bgProbe_.failedKey == key) return st;
    if (!VideoRendererAvailable(ActiveVideoConfig().Renderer())) return st;
    st.phase = VideoProbeStatus::Phase::Preparing;
    return st;
}

bool App::VideoRendererAvailable(VideoRenderer r) const {
    switch (r) {
    case VideoRenderer::Raster: return true;
    case VideoRenderer::RayTraced:
    case VideoRenderer::PathTraced: return renderer_.RayTracingSupported();
    case VideoRenderer::OfflineGI: return renderer_.OfflineSupported();
    }
    return false;
}

uint64_t App::CurrentVideoProbeKey() const {
    const std::string ch = selCharacter_ >= 0 ? library_.characters[(size_t)selCharacter_].id : std::string();
    const std::string st = selStage_ >= 0 ? library_.stages[(size_t)selStage_].id : std::string();
    const std::string so = selSong_ >= 0 ? library_.songs[(size_t)selSong_].id : std::string();
    return VideoProbeKey(ActiveVideoConfig(), ch, st, so);
}

App::VideoEstimate App::EstimateVideoRender() const {
    VideoEstimate e;
    const VideoRenderConfig cfg = ActiveVideoConfig();
    const double duration = selSong_ >= 0 ? library_.songs[(size_t)selSong_].durationSec : 0.0;
    e.frames = std::max(1, (int)std::floor(duration * cfg.fps));
    const VideoProbe* probe = settings_.FindVideoProbe(CurrentVideoProbeKey());
    e.measured = probe != nullptr;
    e.secondsPerFrame = probe ? probe->secondsPerFrame : EstimatedSecondsPerFrame(cfg);
    e.totalSeconds = e.frames * e.secondsPerFrame;
    e.fileGigabytes = (double)cfg.bitrateMbps * duration / 8.0 / 1000.0;
    return e;
}

void App::DrawToast() {
    using namespace ui;
    if (toast_.title.empty()) return;
    const bool visible = timeSeconds_ < toast_.until;
    const float vis = Anim(ImGui::GetID("##toast"), visible, 8.0f);
    if (vis < 0.01f) {
        if (!visible) toast_ = Toast{};  // clear after fading out
        return;
    }
    const Palette& p = P();
    const ImVec2 tS = TextSize(Font::Semibold, size::Body, toast_.title.c_str());
    const ImVec2 dS = toast_.detail.empty() ? ImVec2(0, 0) : TextSize(Font::Regular, size::Caption, toast_.detail.c_str());
    const float openW = toast_.path.empty() ? 0.0f : ButtonWidth("폴더 열기", true);
    const float contentW = std::max(tS.x, dS.x);
    float w = contentW + Dp(16.0f + 24.0f + 12.0f + 20.0f);
    if (!toast_.path.empty()) w += openW + Dp(16.0f);
    const float h = Dp(toast_.detail.empty() ? 52.0f : 66.0f);
    const float y = Dp(20.0f) - Dp(30.0f) * (1.0f - vis);
    const ImVec2 a((ImGui::GetIO().DisplaySize.x - w) * 0.5f, y), b(a.x + w, y + h);
    const float rect0[4] = {0, 0, 0, 0};

    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FrostedPanel(dl, a, b, Dp(16.0f), (ImTextureID)0, rect0);
    Icon(dl, toast_.error ? icon::Warning : icon::Check, Dp(20.0f), ImVec2(a.x + Dp(20.0f) + Dp(10.0f), (a.y + b.y) * 0.5f),
         toast_.error ? p.danger : p.accent);
    const float tx = a.x + Dp(16.0f + 24.0f + 12.0f);
    Text(dl, Font::Semibold, size::Body, ImVec2(tx, a.y + Dp(8.0f)), p.ink, toast_.title.c_str());
    if (!toast_.detail.empty())
        Text(dl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(34.0f)), p.ink2, toast_.detail.c_str());
    if (!toast_.path.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(14.0f) - openW, (a.y + b.y - Dp(34.0f)) * 0.5f));
        if (Button("##toastopen", "폴더 열기", icon::FolderOpen, ButtonKind::Ghost, ImVec2(0, 34))) {
            const std::wstring arg = L"/select,\"" + toast_.path.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::End();
}

}  // namespace mmdx
