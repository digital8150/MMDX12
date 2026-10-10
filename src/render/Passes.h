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
//   SsaoPass      half-res AO (RayTraced: ray-traced AO, temporally accumulated) + depth-aware blur   not PathTraced
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
#include <deque>
#include <map>
#include <string>
#include "render/RenderPass.h"
#include "render/PtPackVariants.h"
#include "render/PackTextures.h"
#include "render/ShaderPack.h"
#include <wrl/client.h>

namespace mmdx {

struct ShaderPack;
struct OfflineJobDesc;
class EffectPipeline;   // PassEffect.cpp: one effect pack's pipeline (root signature + PSO)

// Spot shadow maps are rendered (and sampled by the scene / volumetric shaders) when shadows are on
// and something reads them: the raster and RT scene shaders, or the volumetric march.
inline bool SpotShadowsWanted(const RenderSettings& s, RenderPath path, bool offscreen) {
    return s.shadows && (path != RenderPath::PathTraced || (s.volumetric && !offscreen));
}
// Number of spot shadow slices for these lights (slice i = the i-th spot among the first maxLights).
uint32_t SpotShadowCount(const LightParams& light, uint32_t maxLights);

// Point shadow maps are rendered for the raster path when shadows are on (RT uses ray queries).
inline bool PointShadowsWanted(const RenderSettings& s, RenderPath path, bool offscreen) {
    return s.shadows && (path == RenderPath::Raster);
}
// Number of qualifying point lights (no cap besides maxLights).
uint32_t PointShadowCount(const LightParams& light, uint32_t maxLights);

class ShadowPass final : public IRenderPass {
public:
    const char* Name() const override { return "Shadow"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    void DrawSlice(PassContext& pc, const Texture& map, uint32_t slice, uint32_t matrixIndex, bool charactersOnly,
                   const DirectX::XMMATRIX* overrideMatrix = nullptr);
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
    // path tracer outlines: depth-only pre-pass (PSDepthAlpha, colour writes off); null = PT draws no outlines
    ComPtr<ID3D12PipelineState> psoDepthBack_, psoDepthNoCull_;
    ComPtr<ID3D12RootSignature> rootSig_;
    // PT mode: the outline layer (depth pre-pass + edges, default or PackEdge) for PathTracePass to composite
    void DrawPtEdges(PassContext& pc);

    // Shader packs (render/ShaderPack.h): PSOs compiled on first use from mmd.hlsl's PSPack with the pack's surface,
    // PSOs keyed by pack id (compile defines depend only on the manifest), dropped when the registry generation
    // changes (reload). failed = compile error: the model falls back to the default PSOs. edge = the pack's outline
    // PSO (PACK_HAS_EDGE). The pack's textures (pack.json "textures") live in one texture set per (pack id,
    // resolved folder): each set uploads the pack's textures out of its folder (Game texture sets differ per
    // character) with its own 16-SRV range + white fallback, dropped with the PSOs on reload.
    struct PackPipelines {
        ComPtr<ID3D12PipelineState> back, noCull, backRt, noCullRt, edge;
        bool failed = false, rtTried = false;
        using TextureSet = PackTextures::Set;
        std::map<std::string, TextureSet> sets;        // resolved folder (utf-8, "" = pack folder) -> its set
    };
    const PackPipelines* PackPsos(Dx12Context& ctx, const std::string& id, const std::filesystem::path& textureFolder,
                                  bool rt);
    bool EnsurePackTextures(Dx12Context& ctx, const ShaderPack& pack, PackPipelines::TextureSet& set,
                            const std::filesystem::path& folder);
    void ReleasePackPsos(Dx12Context& ctx);   // waits for the GPU, frees the pack texture SRVs and PSOs
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
    FullscreenPipeline ao_, blur_, temporal_;
    ComputePipeline rtao_;   // rtao.hlsl, writes raw_ as a UAV
    Texture raw_, temp_, out_;
    // RTAO temporal accumulation (ssao.hlsl PSTemporal): hist_[histCur_] receives this frame,
    // hist_[histCur_ ^ 1] holds last frame when histValid_.
    Texture hist_[2];
    uint32_t histCur_ = 0;
    bool histValid_ = false;
    DirectX::XMFLOAT3 histEye_{};   // eye of the last accumulated frame (jump detection)
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
    ComputePipeline edgeComposite_;   // pt_edge.hlsl: ScenePass's outline layer over the denoised colour
    PtPackVariants ptVariants_;
    std::filesystem::path shaderDir_;
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

// The shader packs' whole-screen effects (render/ShaderPack.h, pack.json "type": "effect"): the ordered stack of
// enabled entries from RenderSettings::packEffects at output resolution with ping-pong targets. Two instances
// live in the pass list: preBloom_ = true, before Bloom (its entries are the packs with "stage": "pre-bloom",
// linear HDR, RGBA16F ping-pong), and preBloom_ = false, after Post (the rest, display-referred sRGB into
// targets.ldr, RGBA8 ping-pong). Each entry's PSO is compiled lazily (effect.hlsl + the pack's effect.hlsl
// through MMDX_PACK, DXC ps_6_0 like the surface packs); a compile error marks the pack failed, the entry is
// skipped ([E] + toast). Zero enabled entries: nothing allocates, nothing runs (byte-identical frame).
class PackEffectPass final : public IRenderPass {
public:
    explicit PackEffectPass(bool preBloom);
    ~PackEffectPass() override;
    const char* Name() const override { return preBloom_ ? "PackEffect(pre)" : "PackEffect(post)"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

    // Offline GI (OfflineRenderer::Impl::Finish): does this share have an enabled entry whose pipeline compiles? The offline
    // lit path is taken only then, so a broken pack leaves the image as it is without effects.
    bool Ready(Dx12Context& ctx, const std::vector<EffectStackEntry>& stack);
    // Offline GI: this instance's share of `stack` over the image `io`, ping-ponging through `scratch` (the same format), with
    // the image's effect inputs (depth R32_FLOAT, velocity, oct normal). The result is copied back into `io`; false: nothing ran.
    bool RunOffline(PassContext& pc, const std::vector<EffectStackEntry>& stack, Texture& io, Texture& scratch,
                    Texture& depth, Texture& velocity, Texture& normal, const OfflineJobDesc* job = nullptr);

private:
    // Per-entry GPU state, keyed by pack id: its PSO plus one texture set per resolved folder (pack.json
    // "textures", the v2 API; the white fallback is kept alive with the SRVs).
    struct EffectPipelines {
        std::unique_ptr<EffectPipeline> pipe;   // null until first use / after a reload
        bool failed = false;
        bool texLoaded = false;
        uint32_t texSrv = DescriptorHeap::kInvalid;   // kPackMaxTextures consecutive SRVs in ctx.SrvHeap()
        ComPtr<ID3D12Resource> white;
        std::vector<ComPtr<ID3D12Resource>> tex;
    };
    // `preBloom` = compile / return only the packs whose manifest stage matches this instance's share.
    const EffectPipelines* Pipelines(Dx12Context& ctx, const std::string& id, const std::filesystem::path& folder,
                                     bool preBloom);
    bool EnsurePackTextures(Dx12Context& ctx, const ShaderPack& pack, EffectPipelines& p,
                            const std::filesystem::path& folder);
    void ReleasePackPsos(Dx12Context& ctx);   // waits for the GPU, frees the pack texture SRVs and PSOs
    void ReleaseStateSlots(Dx12Context& ctx);
    void RunStack(PassContext& pc, bool preBloom);
    // One entry's draws (state update, intermediate passes, PackEffect); shared by RunStack and RunOffline.
    struct EntryRun {
        const ShaderPack* pack = nullptr;
        const EffectPipelines* pipes = nullptr;
        const EffectStackEntry* entry = nullptr;
        size_t stackIndex = 0;
        Texture *src = nullptr, *dst = nullptr, *depth = nullptr, *velocity = nullptr, *normal = nullptr;
        uint32_t outW = 0, outH = 0;
        float dt = 0.0f;
        bool resetState = false;      // camera cut / resize / job reset
        uint32_t stateSteps = 0;      // state updates this run (0 = none, unless the slot has no valid state yet)
        float focus[4] = {};          // gP1: focus z, focus aperture, user DoF aperture, max CoC px
    };
    void RunEntry(PassContext& pc, const EntryRun& run);
    void DropStaleStateSlots(Dx12Context& ctx, const std::vector<EffectStackEntry>& stack);
    // API v5 intermediate-pass targets (RGBA16F), pooled by size
    Texture* PassTarget(Dx12Context& ctx, uint32_t w, uint32_t h, uint32_t n);
    void TrimPassPool(Dx12Context& ctx);
    void ReleasePassPool(Dx12Context& ctx);
    struct PoolTexture {
        Texture tex;
        uint64_t lastUse = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, std::deque<PoolTexture>> passPool_;   // deque: stable addresses

    bool AllocTargets(Dx12Context& ctx, uint32_t w, uint32_t h);
    bool AllocPostTargets(Dx12Context& ctx, uint32_t w, uint32_t h);

    struct StateSlot {
        Texture tex[2];     // 4x1 RGBA32F
        uint32_t cur = 0;   // 0 or 1: index of texture written last
        bool hasValidState = false;
        uint64_t lastPassExecution = 0;
    };
    using StateKey = std::pair<size_t, std::string>;

    bool preBloom_;           // this instance's share: pre-bloom (HDR) or post (LDR) entries
    Texture ping_[2];         // RGBA16F ping-pong, outWidth x outHeight (pre-bloom share)
    Texture post_[2];         // RGBA8 ping-pong, outWidth x outHeight (post share)
    uint32_t generation_ = 0;
    std::filesystem::path shaderDir_;
    std::map<std::string, EffectPipelines> psos_;
    std::map<StateKey, StateSlot> stateSlots_;
    uint64_t passExecutionCount_ = 0;
    bool resetRequested_ = false;
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
