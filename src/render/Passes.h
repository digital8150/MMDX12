// The frame's pass list, in execution order (pc.path decides which ones do work):
//   ShadowPass    cascaded shadow map (depth only, skinned)                       Raster, or any path
//                 with volumetrics on (the volumetric march samples it); spot shadow maps of the
//                 characters for the first 8 spot lights (SpotShadowsWanted: raster/RT scene
//                 shading and volumetrics)
//   ScenePass     sky, studio floor, MMD materials + edges -> MSAA colour/normal/velocity/depth
//                 (RayTraced: ray-query sun shadows instead of the shadow map; Raster honours
//                 ViewShading: Unlit = flat, Wireframe = D3D12_FILL_MODE_WIREFRAME)  not PathTraced
//   ResolvePass   MSAA -> single sample (colour, normal, velocity, closest depth) not PathTraced
//   PathTracePass path tracer + temporal/a-trous denoiser -> colour/normal/velocity/depth
//                                                                                 PathTraced only
//   SsaoPass      half-res AO (RayTraced: ray-traced AO) + depth-aware blur       not PathTraced
//   SsrPass       half-res reflections (RayTraced: ray-traced)                    not PathTraced
//   CompositePass colour * AO + reflections + haze -> lit
//   VolumetricPass half-res ray-marched in-scattering (sun via the shadow map, spots via spot shadow maps),
//                 lit * transmittance + in-scattered light                        optional
//   TaaPass       temporal AA (optional, skipped when an upscaler runs) -> hdrFinal
//   UpscalePass   DLSS / FSR / XeSS: lit -> output resolution -> hdrFinal
//   DofPass       depth of field: CoC + half-res bokeh gather + composite -> hdrFinal (output res)
//   BloomPass     downsample/upsample chain, or FFT convolution with a starburst kernel (output res)
//   PostPass      exposure, grade, tonemap, colour LUT, vignette -> ldr (output resolution)
//   BackdropPass  blurred copy of ldr for frosted UI panels (on screen only)
//   PresentPass   letterboxed stretch to the back buffer (on screen only)
#pragma once
#include <map>
#include <string>
#include "render/RenderPass.h"
#include <wrl/client.h>

namespace mmdx {

// Spot shadow maps are rendered (and sampled by the scene / volumetric shaders) when shadows are on
// and something reads them: the raster and RT scene shaders, or the volumetric march.
inline bool SpotShadowsWanted(const RenderSettings& s, RenderPath path, bool offscreen) {
    return s.shadows && (path != RenderPath::PathTraced || (s.volumetric && !offscreen));
}
// Number of spot shadow slices for these lights (slice i = the i-th spot among the first maxLights).
uint32_t SpotShadowCount(const LightParams& light, uint32_t maxLights);

class ShadowPass final : public IRenderPass {
public:
    const char* Name() const override { return "Shadow"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    void DrawSlice(PassContext& pc, const Texture& map, uint32_t slice, uint32_t matrixIndex, bool charactersOnly);
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
    ComPtr<ID3D12PipelineState> psoWireBack_, psoWireNoCull_, psoWireFloor_, psoFlatBg_;
    // RayTracing variants (DXC, ps_6_5, RT_SHADOWS=1); null when DXC/DXR is unavailable
    ComPtr<ID3D12PipelineState> psoCullBackRt_, psoNoCullRt_, psoFloorRt_;
    ComPtr<ID3D12RootSignature> rootSig_;

    // Shader packs (render/ShaderPack.h): PSOs compiled on first use from mmd.hlsl's PSPack with the pack's surface,
    // keyed by pack id, dropped when the registry generation changes (reload). failed = compile error: the model
    // falls back to the default PSOs.
    struct PackPipelines {
        ComPtr<ID3D12PipelineState> back, noCull, backRt, noCullRt;
        bool failed = false, rtTried = false;
    };
    const PackPipelines* PackPsos(Dx12Context& ctx, const std::string& id, bool rt);
    std::map<std::string, PackPipelines> packPsos_;
    uint32_t packGeneration_ = 0;
    std::filesystem::path shaderDir_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC litDesc_{};   // the lit PSO desc (PS replaced per pack)
    ComPtr<ID3DBlob> vs_, vsRt_, vsDxc_;             // kept alive for litDesc_ / the pack PSOs
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
    static constexpr uint32_t kFftSize = 512;   // convolution grid (power of two; bloom_fft.hlsl FFT_N)
    const char* Name() const override { return "Bloom"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    void ExecuteConvolution(PassContext& pc);   // settings.bloomConvolution && fft pipelines exist

    FullscreenPipeline prefilter_, down_, up_;
    Texture mips_[kMips];
    // FFT convolution (bloom_fft.hlsl, ComputePipeline; all null when compute is unavailable)
    ComputePipeline fftInput_, fftRows_, fftCols_, fftKernel_;
    FullscreenPipeline fftOutput_;              // writes mips_[0] from the convolved grid
    Texture gridA_, gridB_;                     // kFftSize^2 RGBA32F (R+iG, B+i0), UAV + SRV
    Texture kernelSpec_;                        // kFftSize^2 RGBA32F kernel spectrum (rg used), UAV + SRV
    bool kernelReady_ = false;
};

// Half-res ray-marched participating medium (height fog, volumetric_common.hlsli) lit by the sun
// (shadowed through the cascaded shadow map) and the spot lights (shadowed through the spot shadow
// maps), so occluders cut light shafts out of the beams. Jittered per frame and accumulated
// temporally; composited as lit * transmittance + in-scattered light.
// Needs the compute path (cs_6_5); does nothing without it, offscreen, or when settings.volumetric is off.
class VolumetricPass final : public IRenderPass {
public:
    const char* Name() const override { return "Volumetric"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    ComputePipeline march_;       // volumetric.hlsl CSMarch -> raw_ (UAV)
    ComputePipeline temporal_;    // volumetric.hlsl CSTemporal raw_ + history_[prev] -> history_[cur]
    ComputePipeline blur_;        // volumetric.hlsl CSBlur  -> depth-aware separable blur history_[cur] -> temp_ -> raw_
    FullscreenPipeline apply_;    // volumetric_apply.hlsl PSApply, lit * T + L (depth-aware upsample)
    Texture raw_, temp_;          // half render res RGBA16F, rgb = in-scattered radiance, a = transmittance
    Texture history_[2];          // temporal accumulation ping-pong (same format)
    uint32_t historyIndex_ = 0;   // history_[historyIndex_] holds the last frame's result
    uint64_t lastFrame_ = 0;      // pc.frame of the last executed frame (history validity)
    bool historyReady_ = false;
};

// Depth of field at output resolution on hdrFinal: CoC from depth (render res, sampled by uv),
// half-res golden-angle bokeh gather, then a full-res blend. Sets hdrFinal = out_.
// Does nothing offscreen or when settings.dof is off.
class DofPass final : public IRenderPass {
public:
    const char* Name() const override { return "DoF"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline prepare_, gather_, tent_, combine_;
    Texture half_;      // half output res RGBA16F: colour, a = signed CoC (half-res pixels)
    Texture blurA_;     // half output res RGBA16F: gathered colour, a = effective blend radius
    Texture blurB_;     // half output res RGBA16F: tent-filtered blurA_
    Texture out_;       // output res RGBA16F (kColorFormat), the new hdrFinal
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
