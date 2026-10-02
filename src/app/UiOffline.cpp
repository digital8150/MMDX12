// Offline ("non-real-time") GI render UI and job logic: high-quality stills and videos.
#include "app/App.h"

#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
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

OfflineJobDesc MakeJobDesc(const AppOptions& o, bool video, bool prepass) {
    OfflineJobDesc j;
    j.width = video ? kOfflineVideoWidth : kOfflineStillWidth;
    j.height = video ? kOfflineVideoHeight : kOfflineStillHeight;
    j.minSamples = video ? kOfflineVideoMinSamples : kOfflineStillMinSamples;
    j.maxSamples = video ? kOfflineVideoMaxSamples : kOfflineStillMaxSamples;
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
    offlineConfirmOpen_ = false;
    screen_ = Screen::Offline;
    LOG_INFO("offline still: %s", PathToUtf8(offline_.output).c_str());
}

void App::StartOfflineVideo(double startSeconds, double endSeconds) {
    if (!scene_ || !renderer_.OfflineSupported() || offline_.mode != OfflineMode::None) return;
    SetPlaying(false);
    const double duration = scene_->endFrame / kMmdFps;
    startSeconds = std::clamp(startSeconds, 0.0, duration);
    endSeconds = std::clamp(endSeconds, startSeconds, duration);
    offline_ = OfflineJob{};
    offline_.mode = OfflineMode::Video;
    offline_.fromCli = !options_.offlineVideo.empty();
    const std::string song = selSong_ >= 0 ? library_.songs[selSong_].displayName : "render";
    offline_.output =
        offline_.fromCli
            ? options_.offlineVideo
            : OfflineOutputDir(true) / Utf8ToPath("MMDX12_" + SanitizeFileName(song) + "_" + Timestamp() + ".mp4");
    offline_.startSeconds = startSeconds;
    offline_.frameCount = std::max(1, (int)std::floor((endSeconds - startSeconds) * kOfflineVideoFps + 1e-6));
    const OfflineJobDesc jd = MakeJobDesc(options_, true, false);
    VideoEncoder::Desc d;
    d.width = jd.width;
    d.height = jd.height;
    d.fps = kOfflineVideoFps;
    d.videoBitrate = jd.height >= 2000 ? 100'000'000 : 40'000'000;
    if (scene_->hasAudio && selSong_ >= 0) d.audioPath = library_.songs[selSong_].audioPath;
    d.audioStartSeconds = startSeconds;
    offline_.encoder = std::make_unique<VideoEncoder>();
    std::string err;
    if (!offline_.encoder->Open(offline_.output, d, &err)) {
        toast_ = {"영상 렌더를 시작할 수 없습니다", err, {}, true, timeSeconds_ + 8.0};
        const bool cli = offline_.fromCli;
        offline_ = OfflineJob{};
        if (cli) running_ = false;
        return;
    }
    scene_->physicsFrame = -1.0f;  // physics restarts from the animated pose at frame 0 of the video
    offline_.startWall = timeSeconds_;
    offline_.beginPending = true;
    offlineConfirmOpen_ = false;
    screen_ = Screen::Offline;
    LOG_INFO("offline video: %d frames -> %s", offline_.frameCount, PathToUtf8(offline_.output).c_str());
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
        (!options_.offlineStill.empty() || !options_.offlineVideo.empty())) {
        cliOfflineStarted_ = true;
        if (!renderer_.OfflineSupported()) {
            LOG_ERROR("offline render unavailable (needs DXR)");
            running_ = false;
            return;
        }
        if (!options_.offlineStill.empty()) {
            StartOfflineStill();
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

void App::RecordOfflineFrame(ID3D12GraphicsCommandList* cmd) {
    if (!offline_.beginPending) {
        renderer_.RenderOffline(cmd);
        return;
    }
    const bool video = offline_.mode == OfflineMode::Video;
    playTime_ = offline_.startSeconds + (video ? (double)offline_.frame / kOfflineVideoFps : 0.0);
    const float frame = (float)(playTime_ * kMmdFps);
    // Motion blur opens the shutter at the previous pose, held in the models' previous ring entry:
    // video re-uploads the previous video frame's pose there; a still taken during playback finds
    // the last live frame there already.
    const bool blur = video ? offline_.frame > 0 : (offline_.wasPlaying && haveLastLiveCamera_);
    if (video && blur) {
        ctx_.WaitForGpu();   // the ring entry may still be read by in-flight offline work
        UploadOfflinePrevPose(ctx_.FrameNumber() - 1);
    }
    UpdateScene(frame);
    FrameView view;
    BuildFrameView(frame, view);
    view.motionBlur = blur;
    view.prevCamera = !blur ? view.camera : (video ? offline_.prevCamera : lastLiveCamera_);
    if (video) {
        SaveOfflinePose();
        offline_.prevCamera = view.camera;
    }
    // the irradiance cache prepass is the render's GI: every image, video frames included
    if (!renderer_.BeginOffline(cmd, view, MakeJobDesc(options_, video, true))) {
        renderer_.Render(cmd, view);  // keep this frame valid
        toast_ = {"고품질 렌더를 시작할 수 없습니다", "레이 트레이싱 장면을 만들지 못했습니다", {}, true,
                  timeSeconds_ + 8.0};
        offline_.cancelRequested = true;  // AfterOfflineFrame finishes the job
        offline_.beginPending = false;
        return;
    }
    offline_.beginPending = false;
    offline_.imageStartWall = timeSeconds_;
}

void App::AfterOfflineFrame() {
    if (offline_.mode == OfflineMode::None) return;
    if (offline_.cancelRequested) {
        renderer_.CancelOffline();
        FinishOffline(true);
        return;
    }
    if (offline_.beginPending || renderer_.OfflineStatus().phase != OfflinePhase::Done) return;
    ImageRGBA8 img;
    if (!renderer_.ReadOfflineImage(img)) {
        toast_ = {"렌더 결과를 읽을 수 없습니다", "", {}, true, timeSeconds_ + 8.0};
        FinishOffline(true);
        return;
    }
    const double secs = std::max(0.0, timeSeconds_ - offline_.imageStartWall);
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
    if (!offline_.encoder || !offline_.encoder->AddFrame(img)) {
        toast_ = {"영상 인코딩에 실패했습니다", "", {}, true, timeSeconds_ + 8.0};
        FinishOffline(true);
        return;
    }
    LOG_INFO("offline video: frame %d/%d (%.1f s)", offline_.frame + 1, offline_.frameCount, secs);
    if (++offline_.frame >= offline_.frameCount)
        FinishOffline(false);
    else
        offline_.beginPending = true;
}

void App::FinishOffline(bool cancelled) {
    const bool video = offline_.mode == OfflineMode::Video;
    const bool cli = offline_.fromCli;
    if (video && offline_.encoder) {
        const uint32_t written = offline_.encoder->FramesWritten();
        offline_.encoder->Finish();
        std::error_code ec;
        if (written == 0) {
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
    } else if (!video && cancelled && (!toast_.error || toast_.until < timeSeconds_)) {
        toast_ = {"스크린샷 렌더를 취소했습니다", "", {}, false, timeSeconds_ + 5.0};
    }
    LOG_INFO("offline render %s: %s", cancelled ? "cancelled" : "finished", PathToUtf8(offline_.output).c_str());
    playTime_ = offline_.startSeconds;
    if (scene_ && scene_->hasAudio) audio_.Seek(playTime_);
    lastRenderedTime_ = -1.0;  // next real-time frame is a camera cut (no stale history)
    offline_ = OfflineJob{};
    screen_ = Screen::Play;
    if (cli) running_ = false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void App::DrawOfflineOverlay() {
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    if (!scene_) return;
    const OfflineProgress& pr = renderer_.OfflineStatus();
    const Palette& p = P();
    const bool video = offline_.mode == OfflineMode::Video;

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
             video ? "고품질 영상 렌더링" : "고품질 스크린샷 렌더링");
    }

    // row 2: status + elapsed
    std::string status;
    if (offline_.beginPending || pr.phase == OfflinePhase::Idle) {
        status = "장면 준비 중…";
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
        const double eta = offline_.avgImageSeconds *
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
        const std::string line = std::to_string(pr.width) + "×" + std::to_string(pr.height) + " · " +
                                 PathToUtf8(offline_.output.filename());
        const ImVec2 ts = TextSize(Font::Regular, size::Caption, line.c_str());
        Text(dl, Font::Regular, size::Caption, ImVec2(cx - ts.x * 0.5f, b.y - Dp(26.0f)), p.ink3, line.c_str());
    }

    // ---- top-left pill
    {
        const char* label = "비실시간 렌더 · 전역 조명(GI) · Esc 취소";
        const ImVec2 ts = TextSize(Font::Semibold, size::Caption, label);
        const float pw = ts.x + Dp(28.0f), ph = Dp(30.0f);
        const ImVec2 pa(Dp(20.0f), Dp(20.0f)), pb(pa.x + pw, pa.y + ph);
        FrostedPanel(dl, pa, pb, ph * 0.5f, (ImTextureID)0, rect0);
        Text(dl, Font::Semibold, size::Caption, ImVec2(pa.x + Dp(14.0f), pa.y + (ph - ts.y) * 0.5f), p.ink2, label);
    }

    ImGui::End();
}

void App::DrawOfflineConfirm() {
    using namespace ui;
    if (!offlineConfirmOpen_) return;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 24, 32, 90));
    ImGui::Begin("##offlineconfirm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ds = io.DisplaySize;

    const double duration = scene_ ? scene_->endFrame / kMmdFps : 0.0;
    const int frames = (int)std::floor(duration * kOfflineVideoFps);
    const std::filesystem::path dir = OfflineOutputDir(true);

    const Palette& p = P();
    const float w = Dp(480.0f), h = Dp(236.0f);
    const ImVec2 a((ds.x - w) * 0.5f, (ds.y - h) * 0.5f), b(a.x + w, a.y + h);
    Panel(dl, a, b, Dp(20.0f));
    const float x0 = a.x + Dp(28.0f), x1 = b.x - Dp(28.0f);
    const float innerW = x1 - x0;

    Text(dl, Font::Bold, size::Heading, ImVec2(x0, a.y + Dp(24.0f)), p.ink, "고품질 영상 렌더링");

    const std::string body1 = "곡 전체를 프레임마다 전역 조명(GI) 기반 비실시간 렌더링으로 그린 뒤, MP4 영상(" +
                              std::to_string(kOfflineVideoWidth) + "×" + std::to_string(kOfflineVideoHeight) +
                              " · " + std::to_string(kOfflineVideoFps) + "fps · H.264 + 음원)으로 저장합니다.";
    // ~10 s per 4K frame on an RTX 3060 Laptop (irradiance cache + path tracing, motion blur, DoF)
    const double days = frames * 10.0 / 86400.0;
    char est[64];
    if (days >= 1.0)
        std::snprintf(est, sizeof(est), "약 %.1f일", days);
    else
        std::snprintf(est, sizeof(est), "약 %.0f시간", std::max(1.0, days * 24.0));
    const std::string body2 =
        "총 " + std::to_string(frames) + "프레임 · 프레임당 10초 안팎이 걸려 전체 렌더는 " + est +
        "(GPU에 따라 다름) 걸릴 수 있습니다. 렌더 중에는 Esc로 언제든 중단할 수 있고, 그때까지의 영상은 저장됩니다.";
    float y = a.y + Dp(60.0f);

    auto drawWrapped = [&](const std::string& text, float yy) {
        PushFont(Font::Regular, size::Body);
        const float wrappedH = ImGui::CalcTextSize(text.c_str(), nullptr, false, innerW).y;
        dl->AddText(nullptr, ImGui::GetFontSize(), ImVec2(x0, yy), p.ink2, text.c_str(), nullptr, innerW);
        PopFont();
        return yy + std::max(wrappedH, ImGui::GetTextLineHeight());
    };
    y = drawWrapped(body1, y) + Dp(10.0f);
    y = drawWrapped(body2, y);

    const std::string loc = "저장 위치: " + PathToUtf8(dir);
    Text(dl, Font::Regular, size::Caption, ImVec2(x0, b.y - Dp(76.0f)), p.ink3, loc.c_str());

    const float startW = ButtonWidth("렌더링 시작", true), cancelW = ButtonWidth("취소", false);
    ImGui::SetCursorScreenPos(ImVec2(x1 - startW - Dp(10.0f) - cancelW, b.y - Dp(56.0f)));
    bool cancel = false, start = false;
    cancel = Button("##confcancel", "취소", nullptr, ButtonKind::Secondary, ImVec2(0, 40));
    ImGui::SameLine(0.0f, Dp(10.0f));
    start = Button("##confstart", "렌더링 시작", icon::FilmStrip, ButtonKind::Primary, ImVec2(0, 40));
    ImGui::End();

    if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        offlineConfirmOpen_ = false;
        return;
    }
    if (start) {
        offlineConfirmOpen_ = false;
        StartOfflineVideo(0.0, duration);
    }
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
