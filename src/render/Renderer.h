#pragma once
#include "render/Dx12Context.h"
#include "render/GpuModel.h"
#include "render/OfflineRenderer.h"
#include "render/RayTracing.h"
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include "render/ShaderInterop.h"
#include "render/Upscaler.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace mmdx {

struct PmxModel;
struct ImageRGBA8;

// Frame orchestration: see Passes.h for the pass list. Without models (menus) the renderer
// only clears the back buffer, so the UI can draw on a clean surface.
class Renderer {
public:
    static constexpr uint32_t kMaxPunctualLights = 16;

    Renderer() = default;
    ~Renderer();

    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir);
    void Shutdown();  // waits for GPU

    std::unique_ptr<GpuModel> CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                          const std::vector<ImageRGBA8>& textures,
                                          ModelRole role = ModelRole::Character);

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

    // Renders `view` into a w x h RGBA8 image (level 0 only) with a transparent background
    // (alpha = coverage, straight alpha). Uses its own command list and waits for the GPU.
    // Must be called outside of frame recording (before ctx.BeginFrame()).
    bool RenderToImage(const FrameView& view, uint32_t w, uint32_t h, ImageRGBA8& out);

    // Blurred copy of the last presented frame for frosted UI panels (ImTextureID, 0 if none)
    // and the back-buffer rectangle the scene image occupies (letterbox), to map UVs.
    uint64_t UiBackdropTexture() const;
    void PresentRect(float& x, float& y, float& w, float& h) const;
    bool SceneVisible() const { return sceneVisible_; }

    const RenderStats& Stats() const { return stats_; }
    // DXR paths (RayTraced / PathTraced) need tier 1.1, shader model 6.5 and dxcompiler.dll;
    // otherwise Render falls back to Raster.
    bool RayTracingSupported() const { return rtSupported_; }
    // Probed once in Initialize (DLL present, GPU supported). None is always available.
    bool UpscalerAvailable(UpscalerKind kind) const;

    // ---- offline GI renderer (render/OfflineRenderer.h) ------------------------------------
    // Available with ray tracing when the offline pipelines compiled.
    bool OfflineSupported() const { return offline_ != nullptr; }
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
    bool ReadOfflineImage(ImageRGBA8& out);
    void CancelOffline();

private:
    void EnsureTargets(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, uint32_t msaa);
    void EnsureShadowMap(uint32_t size);
    void ReleaseTargets();
    void CreateBuiltinTextures();
    void ReadGpuTimer();
    void RecordScene(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t w, uint32_t h,
                     uint32_t cbSlot, uint64_t frame, bool offscreen);
    // Fills jitterPx_ (render pixels, FSR convention) for this frame and the constants.
    void FillSceneConstants(const FrameView& view, uint32_t w, uint32_t h, bool offscreen, SceneConstants& sc);
    RenderPath EffectivePath() const;          // settings_.renderPath, or Raster without DXR support
    IUpscaler* EffectiveUpscaler() const;      // null for None or an unavailable upscaler
    // Punctual lights -> GpuLight (premultiplied colour, normalized direction); returns the count
    // written (<= kMaxPunctualLights).
    static uint32_t FillGpuLights(const LightParams& light, GpuLight* out);

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
    std::unique_ptr<RtScene> rt_;
    std::unique_ptr<OfflineRenderer> offline_;   // null without ray tracing / offline pipelines
    bool rtSupported_ = false;
    float jitterPx_[2] = {};
    int64_t lastFrameQpc_ = 0;
    float frameTimeMs_ = 16.7f;
    std::vector<std::unique_ptr<IRenderPass>> passes_;
    uint32_t pipelineMsaa_ = 0;
    // per-frame scene constants + punctual lights (UPLOAD heap, persistently mapped);
    // kFramesInFlight slots for on-screen frames + 1 for RenderToImage
    ComPtr<ID3D12Resource> sceneCb_, lightBuf_;
    uint8_t* sceneCbMapped_ = nullptr;
    uint8_t* lightBufMapped_ = nullptr;
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
    // offscreen rendering
    ComPtr<ID3D12CommandAllocator> offAlloc_;
    ComPtr<ID3D12GraphicsCommandList> offList_;
    // GPU timing
    ComPtr<ID3D12QueryHeap> timestampHeap_;
    ComPtr<ID3D12Resource> timestampReadback_;
    uint64_t timestampFrequency_ = 0;
};

} // namespace mmdx
