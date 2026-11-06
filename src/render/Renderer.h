#pragma once
#include "render/Dx12Context.h"
#include "render/GpuModel.h"
#include "render/OfflineRenderer.h"
#include "render/PackTextures.h"
#include "render/RayTracing.h"
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include "render/ShaderInterop.h"
#include "render/Upscaler.h"
#include "render/DlssFrameGen.h"
#include "render/MmeRuntime.h"
#include "render/MmeSkin.h"
#include "render/ScreenVideo.h"
#include "render/SkyEnv.h"
#include "render/SkyMap.h"
#include "render/StageScreens.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmdx {

class MmeRuntime;
class MmeSkin;
class ScenePass;

struct PmxModel;
struct ImageRGBA8;
struct CompressedImage;
struct PbrModelData;

struct RenderInitProgress {
    std::atomic<float> fraction{0.0f};  // 0..1
    std::mutex mutex;
    std::string status;                 // guarded by mutex, UTF-8
    void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(mutex); status = s; }
    std::string Status() { std::lock_guard<std::mutex> l(mutex); return status; }
};

// Frame orchestration: see Passes.h for the pass list. Without models (menus) the renderer
// only clears the back buffer, so the UI can draw on a clean surface.
class Renderer {
public:
    static constexpr uint32_t kMaxPunctualLights = 256;  // S7a: GpuLight upload size / LightParams cap
    static constexpr uint32_t kMaxFogVolumes = 16;   // S9: local fog volumes bound to the volumetric passes
    Renderer() = default;
    ~Renderer();

    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir, RenderInitProgress* progress = nullptr);
    void Shutdown();  // waits for GPU

    std::unique_ptr<GpuModel> CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                          const std::vector<ImageRGBA8>& textures,
                                          ModelRole role = ModelRole::Character,
                                          const PbrModelData* pbr = nullptr,
                                          const std::vector<CompressedImage>* compressed = nullptr);

    // M6b: hot-reload failure / success messages (the UI shows a toast). Set once at startup.
    void SetMmeMessage(std::function<void(const std::string&, bool /*error*/)> cb) {
        if (mme_) mme_->SetErrorCallback([cb](const std::string& m) { if (cb) cb(m, true); });
    }

    void SetSettings(const RenderSettings& s) { settings_ = s; }
    // Colour LUT for PostPass (strip layout in render/ColorLut.h); null or empty clears it.
    // Uploads and waits for the GPU: call outside of frame recording.
    void SetColorLut(const ImageRGBA8* strip);
    bool HasColorLut() const { return targets_.lut != nullptr; }
    const RenderSettings& Settings() const { return settings_; }

    // Records the frame into `cmd` (from ctx.BeginFrame()). The caller must already have
    // called model->UpdateSkinning/UpdateMorphs with ctx.FrameNumber(). On return the back
    // buffer holds the final image, is in RENDER_TARGET state, its RTV is bound, the
    // viewport/scissor cover the whole back buffer and ctx.SrvHeap() is the bound heap,
    // so the caller can draw UI (ImGui) directly afterwards.
    void Render(ID3D12GraphicsCommandList* cmd, const FrameView& view);
    // The menu surface Render() leaves without models: the back buffer cleared and bound for the UI. Used
    // after background work (a headless sample render) in the same frame; keeps temporal history.
    void ClearBackBuffer(ID3D12GraphicsCommandList* cmd);

    // Renders `view` into a w x h RGBA8 image (level 0 only) with a transparent background
    // (alpha = coverage, straight alpha). Uses its own command list and waits for the GPU.
    // Must be called outside of frame recording (before ctx.BeginFrame()).
    bool RenderToImage(const FrameView& view, uint32_t w, uint32_t h, ImageRGBA8& out);

    // The final (post-processed, sRGB) image of the last Render() call at output resolution, as an
    // opaque RGBA8 image. Waits for the GPU; call after the frame was submitted (ctx.EndFrame),
    // outside frame recording. Used to render videos with the real-time renderers.
    // preserveAlpha: keep the frame's coverage alpha (RenderSettings::transparentBackground) and un-premultiply the
    // colour, giving straight RGBA8 for a transparent export; false forces opaque (today's behaviour).
    bool ReadFinalImage(ImageRGBA8& out, bool preserveAlpha = false);

    // ---- asynchronous final-image readback (offline video) ----------------------------------
    // Records a copy of the last Render() result into a small ring of readback buffers. Call after Render()
    // and before ctx.EndFrame() (the command list is still open). The colour/alpha decode happens later, so a
    // video frame no longer waits for the GPU between rendering and encoding. False when no ring slot is free
    // (the caller should fall back to ReadFinalImage) or there is no frame.
    bool QueueFinalReadback(ID3D12GraphicsCommandList* cmd, bool preserveAlpha);
    // After ctx.EndFrame(): signals the readback fence (the queue is ordered after the frame's work).
    void EndFrameReadback();
    // Maps the oldest recorded readback once its GPU work has finished. `wait`: block until it is (drain at the
    // end of a render). Frames are returned in the order they were recorded; false when none is ready.
    // Call outside frame recording.
    bool PollFinalReadback(ImageRGBA8& out, bool wait);
    // Pending readbacks not collected yet.
    uint32_t PendingReadbacks() const { return (uint32_t)readbackQueue_.size(); }
    // Offline video preview: uploads the exact image handed to the encoder. Call outside frame recording;
    // returns an ImTextureID (0 when the image is empty).
    uint64_t SetOfflinePreviewImage(const ImageRGBA8& image);
    uint64_t OfflinePreviewTexture() const;

    // Blurred copy of the last presented frame for frosted UI panels (ImTextureID, 0 if none)
    // and the back-buffer rectangle the scene image occupies (letterbox), to map UVs.
    uint64_t UiBackdropTexture() const;
    void PresentRect(float& x, float& y, float& w, float& h) const;
    bool SceneVisible() const { return sceneVisible_; }

    const RenderStats& Stats() const { return stats_; }
    // M8b: releases the video layers (decode threads, staging buffers, textures). Call when the scene ends
    // (App::UnloadScene) so nothing is left running; the passes reload on the next frame with layers.
    void ReleaseVideoLayers();
    // DXR paths (RayTraced / PathTraced) need tier 1.1, shader model 6.5 and dxcompiler.dll;
    // otherwise Render falls back to Raster.
    bool RayTracingSupported() const { return rtSupported_; }
    // Probed once in Initialize (DLL present, GPU supported). None is always available.
    bool UpscalerAvailable(UpscalerKind kind) const;
    // DLSS Frame Generation (render/DlssFrameGen.h). Available on RTX 40+; MaxMultiplier() is 2 on RTX 40,
    // 4 on RTX 50. Off = no cost.
    bool FrameGenAvailable() const;
    int FrameGenMaxMultiplier() const;
    const char* FrameGenReason() const;
    // True when this frame recorded DLSS-G output (the caller presents it with Dx12Context::EndFrameFrameGen).
    bool FrameGenActive() const { return frameGenActive_; }
    int FrameGenGeneratedCount() const;
    ID3D12Resource* FrameGenGeneratedFrame(int i) const;
    ID3D12Resource* FrameGenRealFrame() const;
    // The hudless copy for this frame (Renderer::Render calls it before the UI); the caller signals the final
    // copy after ImGui via FrameGenEvaluate.
    void FrameGenCopyHudless(ID3D12GraphicsCommandList* cmd);
    bool FrameGenEvaluate(ID3D12GraphicsCommandList* cmd);
    // DLSS Ray Reconstruction (nvngx_dlssd.dll + GPU support): the UI enables its switch only then.
    bool RayReconstructionAvailable() const;

    // ---- offline GI renderer (render/OfflineRenderer.h) ------------------------------------
    // Available with ray tracing when the offline pipelines compiled. Only GI renders use it, so Initialize leaves its
    // creation to a background thread: until that is done it counts as available, and BeginOffline waits for it.
    bool OfflineSupported() const { return offline_ != nullptr || (offlineInit_.valid() && !offlineInitFailed_); }
    // Created and usable now. The UI waits for this (a pending job, the studio's GI viewport) instead of blocking the
    // window in BeginOffline while the background creation still runs.
    bool OfflineReady() const {
        return offline_ != nullptr ||
               (offlineInit_.valid() && offlineInit_.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                !offlineInitFailed_);
    }
    // Starts an offline image of `view` (the pose uploaded for ctx.FrameNumber()) and records its
    // first share of work into `cmd` (from ctx.BeginFrame()), then presents the preview: call it
    // instead of Render() for that frame (same back-buffer contract for the UI). Waits for the GPU
    // first. False when unsupported or the acceleration structures could not be built.
    bool BeginOffline(ID3D12GraphicsCommandList* cmd, const FrameView& view, const OfflineJobDesc& job);
    // Instead of Render() on the following frames: records the next share of offline work and
    // presents the current preview (or the final image once Done).
    void RenderOffline(ID3D12GraphicsCommandList* cmd);
    const OfflineProgress& OfflineStatus() const;
    // Final image of a Done job (RGBA8, opaque). Waits for the GPU; call outside frame recording.
    // preserveAlpha: keep the coverage alpha (OfflineJobDesc::exportAlpha) and un-premultiply the colour.
    bool ReadOfflineImage(ImageRGBA8& out, bool preserveAlpha = false);
    void CancelOffline();
    // false: BeginOffline / RenderOffline leave the back buffer alone (a sample render behind the UI).
    void SetOfflinePresent(bool present) { offlinePresent_ = present; }

private:
    void EnsureTargets(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, uint32_t msaa);
    void EnsureShadowMap(uint32_t size);
    void EnsurePointShadowMap(const LightParams& light, bool offscreen);
    void EnsureOffline();   // takes over the background-created offline renderer, waiting while it is created
    void ReleaseTargets();
    void CreateBuiltinTextures();
    void ReadGpuTimer();
    void DrainMmeForget();   // frees MME resources of models destroyed kFramesInFlight+1 frames ago
    // Schedules the per-model MME resource release for a model that left the drawn set (uids only, never pointers:
    // a freed address may be reused). Deduped against the pending list.
    void ForgetModel(uint64_t modelUid);
    // S5: renders one stage screen's camera feed into `target` (raster, own constant-buffer / light slots). Runs
    // before the scene passes; the screens inside the feed keep their previous texture (no recursion). `target` is
    // the screen's own RGBA8 texture; `base` is the frame's SceneConstants (the feed overrides the camera). False when
    // nothing was drawn (the screen keeps its content).
    bool RenderScreenFeed(ID3D12GraphicsCommandList* cmd, const FrameView& view, const ScreenView& screen,
                          Texture& target, uint32_t feedIndex, const SceneConstants& base);    // S5: the HDR targets a camera feed draws into (one set per feed slot, recreated when the feed size changes).
    bool EnsureFeedTargets(uint32_t feedIndex, uint32_t width, uint32_t height);
    // S5: reconciles the stage screens of `view` / renders their camera feeds / binds the textures (before the
    // scene passes, so the ray-traced geometry sees the screen material flags). `baseSc` is the frame's
    // SceneConstants.
    void UpdateStageScreens(ID3D12GraphicsCommandList* cmd, const FrameView& view, const SceneConstants& baseSc);
    // S8b: this frame's screen lights (one rectangle per screen material, largest emitted power first, at most
    // kMaxScreenLights) + the compute pass that averages every screen texture into their colour. Fills
    // `sc.screenLightParams` (x = -1 with no screen light: the shaders skip every screen term). Runs after
    // UpdateStageScreens (the screen textures are bound then) and before the scene passes.
    void UpdateScreenLights(ID3D12GraphicsCommandList* cmd, const FrameView& view, SceneConstants& sc, uint32_t cbSlot);
    // S10 planar mirrors: picks the frame's mirror planes (at most kMaxMirrorPlanes: the largest on-screen ones
    // facing the eye), renders each one's reflection into its slot's target and fills `sc`'s mirror fields. Raster
    // main views only: RT / PT / offline GI reflect at the hit, quad / ortho / feeds / RenderToImage show the tint.
    // Runs before the scene passes (they sample the reflection textures through the bindless table).
    void UpdateMirrors(ID3D12GraphicsCommandList* cmd, const FrameView& view, SceneConstants& sc, uint32_t w,
                       uint32_t h);
    bool EnsureMirrorTargets(uint32_t slot, uint32_t width, uint32_t height);
    bool RenderMirror(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t slot,
                      const DirectX::XMFLOAT4& plane, const DirectX::XMMATRIX& reflectedView,
                      const DirectX::XMMATRIX& proj, const SceneConstants& base);
    void RecordScene(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t w, uint32_t h,
                     uint32_t cbSlot, uint64_t frame, bool offscreen);
    // Fills jitterPx_ (render pixels, FSR convention) for this frame and the constants.
    void FillSceneConstants(const FrameView& view, uint32_t w, uint32_t h, bool offscreen, SceneConstants& sc);
    RenderPath EffectivePath() const;          // settings_.renderPath, or Raster without DXR support
    IUpscaler* EffectiveUpscaler() const;      // null for None or an unavailable upscaler
    // Punctual lights -> GpuLight (premultiplied colour, normalized direction); returns the count
    // written (<= kMaxPunctualLights).
    // S7a: spotSlices (per punctual light: the spot shadow slice or -1) decides which spot lights are shadowed;
    // null = the first kSpotShadowSlices spots, as before.
    // pointCap: point lights that get a shadow slice (raster: the map's capacity; ray-traced paths: no cap).
    static uint32_t FillGpuLights(const LightParams& light, GpuLight* out, const std::vector<int>* spotSlices = nullptr,
                                  uint32_t pointCap = kMaxPunctualLights);
    // S7a: the shadow budget over the spot lights: per punctual light index the shadow slice (0..kSpotShadowSlices-1)
    // or -1. Candidates are the casting spots that reach a shadow caster of `models` (a slice holds nothing else);
    // the score and the hysteresis are render/SpotShadowPick.h.
    std::vector<int> PickSpotShadowSlices(const LightParams& light, const CameraParams& camera,
                                          const std::vector<GpuModel*>& models, float aspect);
    // S7a: the light pick CDF of the frame (normalised inclusive running sum of the pick weight, one float per light);
    // returns the light count written (<= capacity). Read by the path tracer / offline GI through the bindless buffer
    // table. Zero weights fall back to a uniform CDF.
    static uint32_t FillLightCdf(const LightParams& light, float* out, uint32_t capacity);

    // S8a: the frame's emissive-triangle pick table from the last RtScene build (`geometries`, in buffer order = the
    // geometry id). Fills the slot's entry list, its normalised inclusive CDF (float per entry) and the per-geometry
    // base table (first entry of a geometry, or kEmissiveTriNone). Returns the entry count (0 = no emitters). Capped at
    // kEmissiveTriMax entries: the largest-weight geometries are kept (one [W] the first time). Pure CPU.
    struct EmissiveTable {
        std::vector<GpuEmissiveTri> tris;
        std::vector<float> cdf;
        std::vector<uint32_t> base;
    };
    static uint32_t BuildEmissiveTable(const std::vector<RtScene::GeometryRef>& geometries, EmissiveTable& out,
                                       bool& warned);
    // S8a: builds the emissive table for the last RtScene build into `cbSlot`'s buffers (only when its key changed) and
    // sets emissiveParams_ (x = -1 when the frame has no emitters). Called after RtScene::Build in RecordScene and
    // BeginOffline.
    void UpdateEmissiveTable(uint32_t cbSlot);

    // S9: the same for the local fog volumes (<= kMaxFogVolumes); returns the count written.
    static uint32_t FillGpuFogVolumes(const LightParams& light, GpuFogVolume* out);

    // S7b: the frame's spot gobo table (one GpuGobo per punctual light, x = the gobo texture's SrvHeap index or -1);
    // returns false when no light of the frame has a gobo (SceneConstants::goboParams.x = -1: zero cost, no fetch).
    // Light paths are absolute; the textures are decoded / uploaded once per path and kept in `gobos_`.
    bool FillGoboTable(Dx12Context& ctx, const LightParams& light, GpuGobo* out, float time);

    Dx12Context* ctx_ = nullptr;
    std::filesystem::path shaderDir_;
    RenderSettings settings_;
    RenderStats stats_;
    RenderTargets targets_;
    Texture lut_;   // colour LUT strip (targets_.lut points here when set)
    BuiltinTextures builtin_;
    TransientDescriptors transient_;
    std::unique_ptr<IUpscaler> upscalers_[4];  // indexed by UpscalerKind; [0] unused
    bool upscalerAvailable_[4] = {true, false, false, false};
    std::unique_ptr<DlssFrameGen> frameGen_;
    bool frameGenActive_ = false;
    // Camera / jitter / depth data of the frame Render() last recorded, for FrameGenEvaluate (which runs after
    // the UI draw). Valid when frameGenPending_ is set.
    struct FrameGenFrameData {
        DirectX::XMFLOAT4X4 viewProjNoJitter{};
        DirectX::XMFLOAT4X4 prevViewProjNoJitter{};
        DirectX::XMFLOAT4X4 invView{};
        DirectX::XMFLOAT4X4 proj{};
        DirectX::XMFLOAT3 eye{};
        DirectX::XMFLOAT3 fwd{}, up{}, right{};
        float nearZ = 0.5f, farZ = 3000.0f, fov = 0.5f;
        float jitterX = 0, jitterY = 0;
        uint32_t width = 0, height = 0;
    };
    FrameGenFrameData frameGenData_{};
    bool frameGenPending_ = false;
    bool frameGenReset_ = false;
    bool frameGenWasPending_ = false;
    std::unique_ptr<PackTextures> packTextures_;
    std::unique_ptr<RtScene> rt_;
    // Models whose MME resources were released with ForgetModel; freed kFramesInFlight frames later so the
    // in-flight command lists that still reference their descriptors are done (same idea as GpuModel's ring).
    struct PendingMmeForget { uint64_t modelUid; uint64_t frame; };
    std::vector<PendingMmeForget> pendingMmeForget_;
    // The on-screen frame's model uids (RecordScene). A model that drops out of it (destroyed or replaced) has
    // its per-model MME resources released; see Renderer::ForgetModel.
    std::vector<uint64_t> frameModels_;
    // MME runtime + object skinning (M4b): created in Initialize, released with the renderer. Both passes read
    // them through PassContext::mme / mmeSkin.
    std::unique_ptr<MmeRuntime> mme_;
    std::unique_ptr<MmeSkin> mmeSkin_;
    std::unique_ptr<OfflineRenderer> offline_;   // null without ray tracing / offline pipelines
    // The background creation of offline_ (Initialize; EnsureOffline takes it over on the main thread). Declared after
    // the flag it sets: its destructor waits for the task.
    std::atomic<bool> offlineInitFailed_{false};
    std::atomic<bool> offlineInitCancel_{false};   // Shutdown: the background creation stops before its next shader
    std::future<std::unique_ptr<OfflineRenderer>> offlineInit_;
    std::unique_ptr<ScreenVideo> screenVideo_;   // background AVI (背景AVI) feeding screen.bmp materials
    std::unique_ptr<StageScreens> stageScreens_;  // S5: MMDX stage screens (image / video / camera feed)
    bool offlinePresent_ = true;
    bool rtSupported_ = false;
    float jitterPx_[2] = {};
    int64_t lastFrameQpc_ = 0;
    float frameTimeMs_ = 16.7f;
    std::vector<std::unique_ptr<IRenderPass>> passes_;
    uint32_t pipelineMsaa_ = 0;
    // per-frame scene constants + punctual lights (UPLOAD heap, persistently mapped):
    // kCbSlots slots (on-screen frames + RenderToImage), the extra views of a quad view, then the S5 camera feeds
    // (StageScreens::kMaxFeedsPerFrame x kCbSlots; see kFeedCbBase in Renderer.cpp).
    static constexpr uint32_t kFeedSlots = StageScreens::kMaxFeedsPerFrame;
    ComPtr<ID3D12Resource> sceneCb_, lightBuf_;
    uint8_t* sceneCbMapped_ = nullptr;
    uint8_t* lightBufMapped_ = nullptr;
    // S9: local fog volumes of the frame (StructuredBuffer<GpuFogVolume>, same slot layout as the lights)
    ComPtr<ID3D12Resource> fogBuf_;
    uint8_t* fogBufMapped_ = nullptr;
    // S7a: the frame's punctual-light CDF (kLightCdfMax floats per slot, same slot layout as the lights) and one raw
    // SRV per slot in the bindless buffer table (SceneConstants::lightCdfParams.x = the slot's heap index).
    ComPtr<ID3D12Resource> lightCdfBuf_;
    float* lightCdfMapped_ = nullptr;
    uint32_t lightCdfSrv_[64] = {};   // >= kTotalCbSlots (Renderer.cpp)
    uint32_t lightCdfSrvCount_ = 0;   // entries of lightCdfSrv_ that are allocated
    // S7c: this frame's light clusters (PassLightCull): one raw uint block of kClusterBytes per slot, bound through the
    // bindless buffer table (SceneConstants::lightClusterParams.x = the slot's heap index) and written as a RAW UAV.
    ComPtr<ID3D12Resource> lightClusterBuf_;
    uint32_t lightClusterSrv_[64] = {};   // >= kTotalCbSlots (Renderer.cpp)
    uint32_t lightClusterSrvCount_ = 0;   // entries of lightClusterSrv_ that are allocated
    // S8a: the frame's emissive-triangle pick table (the path tracers' next-event estimation over LED screens /
    // AutoLuminous / PBR emissive materials). One entry per emitting triangle of an RtScene geometry, the CDF on the
    // CPU (Renderer::BuildEmissiveTable) and a per-geometry base table, all bound through the bindless buffer table
    // (SceneConstants::emissiveParams). Three raw SRVs per slot, like the light CDF.
    ComPtr<ID3D12Resource> emissiveTriBuf_, emissiveCdfBuf_, emissiveBaseBuf_;
    uint8_t* emissiveTriMapped_ = nullptr;
    float* emissiveCdfMapped_ = nullptr;
    uint32_t* emissiveBaseMapped_ = nullptr;
    uint32_t emissiveTriSrv_[64] = {};
    uint32_t emissiveCdfSrv_[64] = {};
    uint32_t emissiveBaseSrv_[64] = {};
    uint32_t emissiveSrvCount_ = 0;
    // The pick table is rebuilt only when its key (geometry table + per-material emission estimates) changes; each
    // buffer slot keeps its own copy, so a slot is (re)uploaded when it does not hold the current key yet.
    uint64_t emissiveKey_ = 0;                                     // key of emissiveTable_ (0 = nothing built yet)
    EmissiveTable emissiveTable_;                                  // the table of emissiveKey_, kept for slot uploads
    uint64_t emissiveSlotKey_[64] = {};                            // >= kTotalCbSlots, 0 = the slot holds no table
    uint32_t emissiveCount_ = 0;                                   // entries of the current table (0 = no emitters)
    bool emissiveWarned_ = false;
    DirectX::XMFLOAT4 emissiveParams_ = {-1.0f, -1.0f, 0.0f, -1.0f};   // this frame's SceneConstants::emissiveParams
    // S7b: the gobo texture cache (one entry per file: the texture, its SRV in the shared heap) and the frame's gobo
    // table (kMaxPunctualLights GpuGobo entries, one raw SRV per constant-buffer slot like the light CDF).
    struct GoboTexture {
        ComPtr<ID3D12Resource> res;
        uint32_t srv = DescriptorHeap::kInvalid;
        bool failed = false;   // one [W] per broken file, then the light shines without a gobo
    };
    std::unordered_map<std::wstring, GoboTexture> gobos_;
    std::vector<GpuGobo> goboTable_;
    ComPtr<ID3D12Resource> goboBuf_;
    uint8_t* goboBufMapped_ = nullptr;
    uint32_t goboSrv_[64] = {};
    uint32_t goboSrvCount_ = 0;
    // S7b: the gobo paths that produced the texture cache (a path that disappears from the frame frees its texture).
    std::vector<std::wstring> goboUsed_;
    bool goboActive_ = false;   // this frame's scene has a gobo (RecordScene fills the table of its slot)
    // S9b-3: the Sky light's image cache (one entry per file + encoding: the RGBA16F equirect texture, its SRV and the
    // row averages FillSceneConstants writes over the ambient colours). Decoded once; a failed file is remembered so a
    // broken path logs one [W] instead of retrying every frame.
    struct SkyMapGpu {
        ComPtr<ID3D12Resource> res;
        uint32_t srv = DescriptorHeap::kInvalid;
        uint32_t width = 0, height = 0;
        DirectX::XMFLOAT3 avg[3] = {};   // zenith, horizon, ground (linear, before the intensity)
        bool failed = false;
    };
    std::unordered_map<std::string, SkyMapGpu> skyMaps_;
    // The frame's sky map (decode + upload on first use; null when the light has no image or it failed).
    const SkyMapGpu* EnsureSkyMap(const LightParams& light);
    // S8b: the frame's screen-light table (kMaxScreenLights GpuScreenLight entries, a raw SRV per constant-buffer slot
    // through the bindless table, exactly like the gobo table). The lights' colour follows the screen's content: the
    // scene shaders read the screen texture the material shows (the table's axisA.w = its SrvHeap index), so there is
    // no per-frame reduction buffer and no CPU readback.
    std::vector<GpuScreenLight> screenLightTable_;
    ComPtr<ID3D12Resource> screenLightBuf_;
    uint8_t* screenLightBufMapped_ = nullptr;
    uint32_t screenLightSrv_[64] = {};   // >= kTotalCbSlots (Renderer.cpp)
    uint32_t screenLightSrvCount_ = 0;
    // S8b: the screen lights' average colour (shaders/screen_light_avg.hlsl, one 16x16 group per light). A DEFAULT-heap
    // raw buffer of kMaxScreenLights float4: a UAV for the compute pass, a raw SRV for the scene shaders
    // (SceneConstants::screenLightParams.z). A buffer is created in COMMON, so the state is tracked explicitly.
    ComPtr<ID3D12Resource> screenAverageBuf_;
    uint32_t screenAverageSrv_ = DescriptorHeap::kInvalid;
    uint32_t screenAverageUav_ = DescriptorHeap::kInvalid;
    D3D12_RESOURCE_STATES screenAverageState_ = D3D12_RESOURCE_STATE_COMMON;
    ComputePipeline screenAvgPipe_;
    bool screenAvgTried_ = false;       // the average pass compiled (or failed: one [W], the screens stay black)
    bool screenReady_ = false;          // this frame's scene has screen lights (RecordScene filled the table)
    bool loggedScreenLights_ = false;   // one SCREENLIGHTS log line per run (not per frame)
    uint32_t screenLightCount_ = 0;     // entries of screenLightTable_ this frame (for the mirror slot copies)
    // S9b-2: this renderer's sky environment (the Physical Sky's luminance CDFs + SH irradiance, render/SkyEnv.h). Built
    // every frame a Physical Sky is on; SceneConstants::skyEnvParams.x = -1 otherwise, which skips every sky term.
    SkyEnvBuilder skyEnv_;
    // S7c: the LIGHTS line's cluster fields of the last log (the packed grid, 0 = no clusters): one line per change.
    uint32_t loggedClusterGrid_ = UINT32_MAX;
    std::vector<int> prevSpotPick_;
    // S7a: this frame's spot shadow slices (per punctual light), filled by FillSceneConstants and read by
    // FillGpuLights / ShadowPass (PassContext::spotSlices).
    std::vector<int> spotSlices_;
    // S7a: the last logged LIGHTS line (count, shadowed spots, shadowed points): one line per scene, not per frame.
    uint32_t loggedLights_[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    // S5: a camera feed draws into its own HDR targets (colour / normal / velocity / depth) at the feed size; the
    // tonemapped copy goes into the screen's RGBA8 texture (shaders/screenfeed.hlsl). The feed's SceneConstants /
    // lights live in the kFeedCbBase slots, after every other slot.
    struct FeedTargets {
        Texture color, normal, velocity, depth;
        uint32_t width = 0, height = 0;
    };
    FeedTargets feeds_[StageScreens::kMaxFeedsPerFrame];
    // S10 planar mirrors: one RGBA16F/reflect-target set per slot at half the render resolution (created on the
    // first mirror frame, never freed until the renderer goes away: a frame that has no mirror just does not bind
    // them). `srv` is the persistent SrvHeap index the shaders sample through the bindless table.
    struct MirrorTargets {
        Texture color, normal, velocity, depth;
        uint32_t width = 0, height = 0;
        uint32_t srv = DescriptorHeap::kInvalid;
    };
    MirrorTargets mirrors_[kMaxMirrorPlanes];
    // The feeds' own descriptor windows (the main window belongs to the scene passes; a feed runs before them and
    // must not consume their per-frame budget). kFramesInFlight x kMaxFeedsPerFrame windows.
    TransientDescriptors feedTransient_;
    // S10: the mirror passes' own descriptor window (same reason: they run before the scene passes).
    TransientDescriptors mirrorTransient_;
    // This frame's mirror count (SceneConstants::mirrorParams.x); loggedMirrors_ keeps the last logged value so the
    // debug log gets one MIRRORS line per change of the set, not one per frame.
    uint32_t mirrorCount_ = 0;
    uint32_t loggedMirrors_ = UINT32_MAX;
    FullscreenPipeline feedResolve_;
    bool feedResolveTried_ = false;
    ScenePass* scenePass_ = nullptr;   // the scene pass (owned by passes_): the S5 camera feeds draw with it
    // temporal state
    DirectX::XMFLOAT4X4 prevViewProj_{};
    DirectX::XMFLOAT3 prevEye_{};
    bool havePrev_ = false, prevTaa_ = false;
    uint32_t temporalIndex_ = 0;
    // UI backdrop SRV (persistent, for ImGui)
    uint32_t backdropSrv_ = DescriptorHeap::kInvalid;
    ID3D12Resource* backdropSrvRes_ = nullptr;
    float presentRect_[4] = {};
    bool sceneVisible_ = false;
    bool hasFinal_ = false;   // targets_.ldr holds a finished Render() image (ReadFinalImage)
    // asynchronous final-image readback ring (RecordFinalReadback / TryCollectFinalImage)
    struct ReadbackSlot {
        ComPtr<ID3D12Resource> res;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        uint32_t width = 0, height = 0;
        bool preserveAlpha = false;
        bool pending = false;
        uint64_t fence = 0;
    };
    static constexpr uint32_t kReadbackRing = 3;
    ReadbackSlot readback_[kReadbackRing];
    std::vector<uint32_t> readbackQueue_;   // ring indices, oldest first
    ComPtr<ID3D12Fence> readbackFence_;
    HANDLE readbackEvent_ = nullptr;
    uint64_t readbackFenceValue_ = 0;
    uint64_t readbackRecorded_ = 0;         // slots recorded since the last EndFrameReadback
    bool DecodeReadback(ReadbackSlot& slot, bool preserveAlpha, ImageRGBA8& out);
    // offline video preview texture (SetOfflinePreviewImage); two SRV slots so overwriting a descriptor never
    // races the GPU still drawing the previous preview
    ComPtr<ID3D12Resource> previewRes_[2];
    uint32_t previewSrv_[2] = {DescriptorHeap::kInvalid, DescriptorHeap::kInvalid};
    uint32_t previewSlot_ = 0;
    // offscreen rendering
    ComPtr<ID3D12CommandAllocator> offAlloc_;
    ComPtr<ID3D12GraphicsCommandList> offList_;
    // GPU timing
    ComPtr<ID3D12QueryHeap> timestampHeap_;
    ComPtr<ID3D12Resource> timestampReadback_;
    uint64_t timestampFrequency_ = 0;
};

} // namespace mmdx
