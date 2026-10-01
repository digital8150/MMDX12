#pragma once
// Render pass plumbing. The Renderer owns an ordered list of passes; new techniques
// (shadow maps, SSAO, ray-traced reflections, upscalers, post FX) are added as passes
// without touching the existing ones.
#include "render/Dx12Context.h"
#include "render/RenderTypes.h"
#include <d3dcompiler.h>
#include <filesystem>

namespace mmdx {

// Internal-resolution render targets shared between passes.
struct RenderTargets {
    uint32_t width = 0, height = 0, msaa = 1;
    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
    ComPtr<ID3D12Resource> colorMsaa;   // kColorFormat, `msaa` samples (== colorResolved when msaa == 1? no: always separate)
    ComPtr<ID3D12Resource> depthMsaa;   // kDepthFormat, `msaa` samples
    ComPtr<ID3D12Resource> colorResolved;  // kColorFormat, 1 sample, SRV
    uint32_t colorMsaaRtv = DescriptorHeap::kInvalid;   // in ctx.RtvHeap()
    uint32_t depthMsaaDsv = DescriptorHeap::kInvalid;   // in ctx.DsvHeap()
    uint32_t colorResolvedSrv = DescriptorHeap::kInvalid;  // in ctx.SrvHeap()
};

struct PassContext {
    Dx12Context& ctx;
    ID3D12GraphicsCommandList* cmd;
    const FrameView& view;
    const RenderSettings& settings;
    RenderTargets& targets;
    RenderStats& stats;
    D3D12_GPU_VIRTUAL_ADDRESS sceneConstants;  // SceneConstants for this frame
};

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual const char* Name() const = 0;
    // Called once, and again whenever RenderTargets.msaa changes (PSOs depend on it).
    virtual bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) = 0;
    virtual void Execute(PassContext& pc) = 0;
};

// Compiles an HLSL entry point with D3DCompileFromFile (D3DCOMPILE_ENABLE_STRICTNESS,
// plus DEBUG|SKIP_OPTIMIZATION in debug builds, OPTIMIZATION_LEVEL3 otherwise).
// `target` e.g. "vs_5_1". Logs compiler errors via LOG_ERROR and returns null on failure.
ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target);

} // namespace mmdx
