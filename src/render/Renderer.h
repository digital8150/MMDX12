#pragma once
#include "render/Dx12Context.h"
#include "render/GpuModel.h"
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include "render/Upscaler.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace mmdx {

struct PmxModel;
struct ImageRGBA8;

// Frame orchestration:
//   ScenePass  (MMD forward shading + edges -> colorMsaa/depthMsaa at internal res)
//   ResolvePass(colorMsaa -> colorResolved)
//   PresentPass(colorResolved -> back buffer, aspect-preserving letterbox, bilinear)
// Future passes slot in between (shadow map before ScenePass, upscaler / post FX before
// PresentPass).
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir);
    void Shutdown();  // waits for GPU

    std::unique_ptr<GpuModel> CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                          const std::vector<ImageRGBA8>& textures);

    void SetSettings(const RenderSettings& s) { settings_ = s; }
    const RenderSettings& Settings() const { return settings_; }

    // Records the frame into `cmd` (from ctx.BeginFrame()). The caller must already have
    // called model->UpdateSkinning/UpdateMorphs for ctx.FrameSlot(). On return the back
    // buffer holds the final image, is in RENDER_TARGET state, its RTV is bound, the
    // viewport/scissor cover the whole back buffer and ctx.SrvHeap() is the bound heap,
    // so the caller can draw UI (ImGui) directly afterwards.
    void Render(ID3D12GraphicsCommandList* cmd, const FrameView& view);

    const RenderStats& Stats() const { return stats_; }
    const IUpscaler& Upscaler() const { return *upscaler_; }

private:
    void EnsureTargets(uint32_t width, uint32_t height, uint32_t msaa);
    void ReleaseTargets();
    void CreateBuiltinTextures();
    void ReadGpuTimer();

    Dx12Context* ctx_ = nullptr;
    std::filesystem::path shaderDir_;
    RenderSettings settings_;
    RenderStats stats_;
    RenderTargets targets_;
    BuiltinTextures builtin_;
    std::unique_ptr<IUpscaler> upscaler_;
    std::vector<std::unique_ptr<IRenderPass>> passes_;  // ScenePass, ResolvePass, PresentPass
    uint32_t pipelineMsaa_ = 0;
    // per-frame scene constants (UPLOAD heap, persistently mapped, 256 B per slot)
    ComPtr<ID3D12Resource> sceneCb_;
    uint8_t* sceneCbMapped_ = nullptr;
    // GPU timing
    ComPtr<ID3D12QueryHeap> timestampHeap_;
    ComPtr<ID3D12Resource> timestampReadback_;
    uint64_t timestampFrequency_ = 0;
};

} // namespace mmdx
