// Render benchmark (Cinebench style): loads the dedicated scene, poses the performers, runs the
// offline GI renderer once with a fixed workload and shows the result on the benchmark screen.
#include "app/App.h"

#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <directx/d3dx12.h>

#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"

namespace mmdx {

namespace {

// "YYYYMMDD_HHMMSS".
std::string Timestamp() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Start (-> Loading -> BenchRender)
// ---------------------------------------------------------------------------

void App::StartRenderBenchLoad() {
    RenderBenchCast cast;
    if (!PickRenderBenchCast(library_, cast)) {
        LOG_ERROR("render benchmark: the library needs at least one character and one song");
        if (renderBench_.fromCli) running_ = false;
        return;
    }
    renderBench_.cast = cast;

    renderBench_.width = kRenderBenchWidth;
    renderBench_.height = kRenderBenchHeight;
    renderBench_.samples = kRenderBenchSamples;
    bool official = true;
    if (options_.offlineSize[0] > 0 && options_.offlineSize[1] > 0) {
        renderBench_.width = (uint32_t)(options_.offlineSize[0] & ~1);
        renderBench_.height = (uint32_t)(options_.offlineSize[1] & ~1);
        official = false;
    }
    if (options_.benchSpp > 0) {
        renderBench_.samples = (uint32_t)options_.benchSpp;
        official = false;
    }
    renderBench_.official = official;
    benchCategory_ = kBenchGiRender;
    benchSubmittable_ = renderBench_.official;
    for (int k = 0; k < kRenderBenchPerformerCount; ++k)
        LOG_INFO("render bench cast %d: %s / %s", k, library_.characters[(size_t)cast.characters[k]].id.c_str(),
                 library_.songs[(size_t)cast.songs[k]].id.c_str());
    LOG_INFO("render bench cast seed %u", cast.seed);

    // The previous result image stays (the lobby may have drawn it this frame); it is replaced
    // when the new render finishes.
    UnloadScene();
    screen_ = Screen::Loading;
    loadTarget_ = LoadTarget::RenderBench;
    loadError_.clear();
    loadProgress_.fraction.store(0.0f, std::memory_order_relaxed);
    loadProgress_.SetStatus("");
    loadPackage_ = std::make_unique<ScenePackage>();
    std::vector<CharacterAsset> chars;
    std::vector<SongAsset> songs;
    for (int k = 0; k < kRenderBenchPerformerCount; ++k) {
        chars.push_back(library_.characters[(size_t)cast.characters[k]]);
        songs.push_back(library_.songs[(size_t)cast.songs[k]]);
    }
    loadFuture_ = std::async(std::launch::async, [chars, songs, pkg = loadPackage_.get(), this] {
        return LoadRenderBenchPackage(chars, songs, *pkg, &loadProgress_, &loadError_);
    });
}

// ---------------------------------------------------------------------------
// Posing
// ---------------------------------------------------------------------------

void App::PoseRenderBench() {
    if (!scene_) return;
    using namespace DirectX;
    const int count = 1 + (int)scene_->extras.size();
    for (int k = 0; k < count; ++k) {
        ModelInstance* inst = k == 0 ? scene_->character.get() : scene_->extras[k - 1].instance.get();
        const BoundMotion* mo =
            k == 0 ? scene_->motion.get() : scene_->extras[k - 1].motion.get();
        if (!inst) continue;
        if (!mo) {
            XMFLOAT4X4 identity;
            XMStoreFloat4x4(&identity, XMMatrixIdentity());
            inst->SetRootTransform(identity);
            inst->EnablePhysics(false);
            inst->ResetPose();
            inst->UpdatePose();
            continue;
        }
        const RenderBenchSlot& slot = kRenderBenchSlots[k];

        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        inst->SetRootTransform(identity);

        // 1. rest head height: standing pose with physics off
        inst->EnablePhysics(false);
        inst->ResetPose();
        inst->UpdatePose(0.0f);
        const int head = inst->Model().FindBone("\xE9\xA0\xAD");  // 頭
        const bool hasHead = head >= 0;
        const float restHead = hasHead ? inst->BoneWorldPosition(head).y : 0.0f;

        // 2. pose search: the first candidate frame inside the middle of the dance whose head is
        //    high enough (standing, not crouching or lying), else the highest-headed frame
        float danceSeconds = library_.songs[(size_t)renderBench_.cast.songs[k]].durationSec;
        if (danceSeconds <= 0.0f) danceSeconds = 60.0f;
        int chosen = -1;
        float bestHead = -1.0f, bestFrame = 0.0f;
        for (int attempt = 0; attempt < kRenderBenchPoseAttempts; ++attempt) {
            const float frame = RenderBenchPoseFrame(renderBench_.cast.seed, k, attempt, danceSeconds);
            mo->Evaluate(frame, *inst);
            inst->UpdatePose(0.0f);
            const float y = hasHead ? inst->BoneWorldPosition(head).y : 0.0f;
            if (chosen < 0 || y > bestHead) {
                bestHead = y;
                bestFrame = frame;
            }
            if (chosen < 0 && y >= 0.85f * restHead) chosen = (int)attempt;
        }
        const float poseFrame = chosen >= 0
                                    ? RenderBenchPoseFrame(renderBench_.cast.seed, k, chosen, danceSeconds)
                                    : bestFrame;

        // 3. evaluate the chosen frame (identity root, physics off): centre bone and body yaw
        inst->EnablePhysics(false);
        mo->Evaluate(poseFrame, *inst);
        inst->UpdatePose(0.0f);
        const int center = inst->Model().FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");  // センター
        XMFLOAT3 c = center >= 0 ? inst->BoneWorldPosition(center) : XMFLOAT3{0, 0, 0};
        const int armL = inst->Model().FindBone("\xE5\xB7\xA6\xE8\x85\x95");  // 左腕
        const int armR = inst->Model().FindBone("\xE5\x8F\xB3\xE8\x85\x95");  // 右腕
        float bodyYaw = 0.0f;
        if (armL >= 0 && armR >= 0) {
            const XMFLOAT3 pl = inst->BoneWorldPosition(armL), pr = inst->BoneWorldPosition(armR);
            const float dx = pl.x - pr.x, dz = pl.z - pr.z;
            if (dx * dx + dz * dz > 1e-6f) bodyYaw = std::atan2f(-dz, dx);
        }

        // 4. placement: move the centre over the origin, face the camera (upper body), move to the slot
        XMFLOAT4X4 root;
        XMStoreFloat4x4(&root, XMMatrixTranslation(-c.x, 0.0f, -c.z) *
                                   XMMatrixRotationY(XMConvertToRadians(slot.yawDeg) - bodyYaw) *
                                   XMMatrixTranslation(slot.x, 0.0f, slot.z));
        inst->SetRootTransform(root);

        // 5. physics pre-roll: kRenderBenchPrerollFrames of motion in 60 Hz steps, ending exactly
        //    at the pose frame
        inst->EnablePhysics(true);
        inst->ResetPhysics();
        const int steps = (int)std::lround(kRenderBenchPrerollFrames * 2.0f);
        for (int s = 0; s <= steps; ++s) {
            const float f = poseFrame - kRenderBenchPrerollFrames + (float)s * 0.5f;
            mo->Evaluate(std::max(f, 0.0f), *inst);
            inst->UpdatePose(s == 0 ? 0.0f : 1.0f / 60.0f);
        }
        LOG_INFO("render bench: performer %d frame %.0f bodyYaw %.1f deg", k, (double)poseFrame,
                 (double)(XMConvertToDegrees(bodyYaw)));
    }
    LOG_INFO("render bench: posed %d performers", count);
}

// ---------------------------------------------------------------------------
// Per-frame job logic
// ---------------------------------------------------------------------------

void App::RecordRenderBenchFrame(ID3D12GraphicsCommandList* cmd) {
    if (!renderBench_.beginPending) {
        renderer_.RenderOffline(cmd);
        return;
    }
    ctx_.WaitForGpu();   // the bone ring entries we overwrite may still be read by in-flight frames
    const uint64_t slot = ctx_.FrameNumber();
    // upload the final pose of every performer to this frame's ring entry AND the previous one
    // (the outline pass reads the previous entry with weight 0; keep it finite)
    auto upload = [&](ModelInstance& inst, GpuModel& gpu) {
        for (uint64_t f : {slot, slot > 0 ? slot - 1 : slot}) {
            gpu.UpdateSkinning(f, inst.SkinMatrices());
            gpu.UpdateMorphs(f, inst.VertexMorphDeltas(), inst.MorphVersion());
        }
    };
    upload(*scene_->character, *scene_->characterGpu);
    for (auto& e : scene_->extras) upload(*e.instance, *e.gpu);
    FrameView view;
    const int head = scene_->character->Model().FindBone("\xE9\xA0\xAD");  // 頭
    DirectX::XMFLOAT3 h = head >= 0 ? scene_->character->BoneWorldPosition(head)
                                    : DirectX::XMFLOAT3{0, 15, 0};
    BuildRenderBenchView(h, view);
    view.models.push_back(scene_->characterGpu.get());
    for (auto& e : scene_->extras) view.models.push_back(e.gpu.get());
    OfflineJobDesc job;
    job.width = renderBench_.width;
    job.height = renderBench_.height;
    job.minSamples = job.maxSamples = renderBench_.samples;
    job.prepass = true;
    job.errorThreshold = 0.0f;   // fixed workload: every pixel takes all samples
    if (!renderer_.BeginOffline(cmd, view, job)) {
        renderer_.Render(cmd, FrameView{});   // keep the frame valid (clears)
        LOG_ERROR("render bench: BeginOffline failed");
        renderBench_.cancelRequested = true;
        renderBench_.beginPending = false;
        return;
    }
    renderBench_.beginPending = false;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    renderBench_.startQpc = q.QuadPart;
}

void App::AfterRenderBenchFrame() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    if (!renderBench_.beginPending && renderBench_.startQpc) {
        renderBench_.elapsed =
            (double)(now.QuadPart - renderBench_.startQpc) / (double)freq.QuadPart;
    }
    if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        renderBench_.cancelRequested = true;
    if (renderBench_.cancelRequested) {
        renderer_.CancelOffline();
        FinishRenderBench(true);
        return;
    }
    if (renderBench_.beginPending || renderer_.OfflineStatus().phase != OfflinePhase::Done) return;

    ImageRGBA8 img;
    const bool ok = renderer_.ReadOfflineImage(img);   // waits for the GPU: the render is complete afterwards
    QueryPerformanceCounter(&now);
    const double seconds = (double)(now.QuadPart - renderBench_.startQpc) / (double)freq.QuadPart;
    renderBench_.elapsed = seconds;
    if (!ok) {
        LOG_ERROR("render bench: readback failed");
        FinishRenderBench(true);
        return;
    }
    benchResult_ = ComputeRenderBenchResult(seconds, renderBench_.width, renderBench_.height,
                                            renderBench_.samples);

    // Save the PNG.
    const std::filesystem::path path =
        OfflineOutputDir(false) / Utf8ToPath("MMDX12_RenderBench_" + Timestamp() + ".png");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (SavePngRGBA8(path, img.Width(), img.Height(), img.mips[0].pixels.data(), img.Width() * 4)) {
        renderBenchSaved_ = path;
    } else {
        renderBenchSaved_.clear();
        LOG_WARN("render bench png save failed: %s", PathToUtf8(path).c_str());
    }

    // Result texture for the result screen.
    ReleaseRenderBenchImage();
    renderBenchImageW_ = 0;
    renderBenchImageH_ = 0;
    UploadBatch batch(ctx_);
    renderBenchTex_ = batch.CreateTexture(img, L"renderbench.result");
    batch.Submit();
    if (renderBenchTex_) {
        renderBenchSrv_ = ctx_.SrvHeap().Allocate(1);
        if (renderBenchSrv_ != DescriptorHeap::kInvalid) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = (UINT)-1;
            ctx_.Device()->CreateShaderResourceView(renderBenchTex_.Get(), &d, ctx_.SrvHeap().Cpu(renderBenchSrv_));
            renderBenchImage_ = ctx_.SrvHeap().Gpu(renderBenchSrv_).ptr;
            renderBenchImageW_ = img.Width();
            renderBenchImageH_ = img.Height();
        } else {
            renderBenchImage_ = 0;
        }
    } else {
        renderBenchImage_ = 0;
    }

    LOG_INFO("BENCHMARK %s score=%d tier=%s seconds=%.2f msps=%.2f spp=%u size=%ux%u official=%d",
             benchResult_.category.c_str(), benchResult_.score, benchResult_.tier.c_str(), seconds,
             benchResult_.avgFps, renderBench_.samples, renderBench_.width, renderBench_.height,
             renderBench_.official ? 1 : 0);
    FinishRenderBench(false);
}

void App::FinishRenderBench(bool cancelled) {
    const bool cli = renderBench_.fromCli;
    UnloadScene();
    ApplyRenderSettings();          // restores vsync etc.
    renderBench_ = RenderBenchRun{};
    submitResult_.reset();
    if (cancelled) {
        screen_ = Screen::BenchLobby;
        RefreshLeaderboard();
        if (cli) running_ = false;
        return;
    }
    screen_ = Screen::BenchResult;
    if (cli) {
        if (options_.capturePath.empty()) {
            running_ = false;
        } else {
            // show the result screen ~30 frames, capture, quit
            options_.startScreen = "result";
            options_.quitAfterFrames = std::max(options_.quitAfterFrames, 30);
            framesInScene_ = options_.quitAfterFrames - 30;
        }
    }
}

void App::ReleaseRenderBenchImage() {
    if (renderBenchSrv_ != DescriptorHeap::kInvalid) {
        ctx_.SrvHeap().Free(renderBenchSrv_, 1);
        renderBenchSrv_ = DescriptorHeap::kInvalid;
    }
    renderBenchTex_.Reset();
    renderBenchImage_ = 0;
    renderBenchImageW_ = 0;
    renderBenchImageH_ = 0;
}

} // namespace mmdx
