// The frame's pass list, in execution order (pc.path decides which ones do work):
//   ShadowPass    cascaded shadow map (depth only, skinned)                       Raster only
//   ScenePass     sky, studio floor, MMD materials + edges -> MSAA colour/normal/velocity/depth
//                 (RayTraced: ray-query sun shadows instead of the shadow map)    not PathTraced
//   ResolvePass   MSAA -> single sample (colour, normal, velocity, closest depth) not PathTraced
//   PathTracePass path tracer + temporal/a-trous denoiser -> colour/normal/velocity/depth
//                                                                                 PathTraced only
//   SsaoPass      half-res AO (RayTraced: ray-traced AO) + depth-aware blur       not PathTraced
//   SsrPass       half-res reflections (RayTraced: ray-traced)                    not PathTraced
//   CompositePass colour * AO + reflections + haze -> lit
//   TaaPass       temporal AA (optional, skipped when an upscaler runs) -> hdrFinal
//   UpscalePass   DLSS / FSR / XeSS: lit -> output resolution -> hdrFinal
//   BloomPass     downsample/upsample chain (output resolution)
//   PostPass      exposure, grade, tonemap, vignette -> ldr (output resolution)
//   BackdropPass  blurred copy of ldr for frosted UI panels (on screen only)
//   PresentPass   letterboxed stretch to the back buffer (on screen only)
#pragma once
#include "render/RenderPass.h"
#include <wrl/client.h>

namespace mmdx {

class ShadowPass final : public IRenderPass {
public:
    const char* Name() const override { return "Shadow"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoOpaque_, psoAlpha_;
};

class ScenePass final : public IRenderPass {
public:
    const char* Name() const override { return "Scene"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComPtr<ID3D12PipelineState> psoCullBack_, psoNoCull_, psoEdge_, psoSky_, psoFloor_;
    // RayTracing variants (DXC, ps_6_5, RT_SHADOWS=1); null when DXC/DXR is unavailable
    ComPtr<ID3D12PipelineState> psoCullBackRt_, psoNoCullRt_, psoFloorRt_;
    ComPtr<ID3D12RootSignature> rootSig_;
};

class ResolvePass final : public IRenderPass {
public:
    const char* Name() const override { return "Resolve"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
};

class SsaoPass final : public IRenderPass {
public:
    const char* Name() const override { return "SSAO"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline ao_, blur_;
    ComputePipeline rtao_;   // rtao.hlsl, writes raw_ as a UAV
    Texture raw_, temp_, out_;
};

class SsrPass final : public IRenderPass {
public:
    const char* Name() const override { return "SSR"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
    ComputePipeline rt_;     // rtreflect.hlsl, writes out_ as a UAV
    Texture out_;
};

// Path tracer (pathtrace.hlsl) and denoiser (pt_denoise.hlsl), PathTraced only. Writes
// targets.color (denoised radiance, alpha = coverage) and targets.normal/velocity/depth in
// the G-buffer encodings, so the remaining passes work unchanged.
class PathTracePass final : public IRenderPass {
public:
    const char* Name() const override { return "PathTrace"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    ComputePipeline trace_, temporal_, atrous_, modulate_;
    Texture light_, albedo_;        // noisy demodulated radiance, primary albedo (RGBA16F)
    Texture history_[2];            // accumulated light, a = history length (RGBA16F)
    Texture histDepth_[2];          // R32_FLOAT raw depth of the frame each history belongs to
    Texture histNormal_[2];         // RGBA16F oct normal of that frame
    Texture filterA_, filterB_;     // a-trous ping-pong (RGBA16F)
    uint32_t current_ = 0;
    bool historyValid_ = false;
};

class CompositePass final : public IRenderPass {
public:
    const char* Name() const override { return "Composite"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
};

class TaaPass final : public IRenderPass {
public:
    const char* Name() const override { return "TAA"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
    Texture history_[2];
    uint32_t current_ = 0;
};

// Runs pc.upscaler (if any): lit (render res) -> output_ (output res) = hdrFinal.
class UpscalePass final : public IRenderPass {
public:
    const char* Name() const override { return "Upscale"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override { return true; }
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    Texture output_;   // RGBA16F, outWidth x outHeight, UAV + SRV
    bool reset_ = true;
};

class BloomPass final : public IRenderPass {
public:
    static constexpr uint32_t kMips = 6;
    const char* Name() const override { return "Bloom"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline prefilter_, down_, up_;
    Texture mips_[kMips];
};

class PostPass final : public IRenderPass {
public:
    const char* Name() const override { return "Post"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
};

class BackdropPass final : public IRenderPass {
public:
    const char* Name() const override { return "Backdrop"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_;
    Texture a_, b_;
};

class PresentPass final : public IRenderPass {
public:
    const char* Name() const override { return "Present"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12RootSignature> rootSig_;
};

} // namespace mmdx
