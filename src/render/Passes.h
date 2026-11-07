// The frame's pass list, in execution order (pc.path decides which ones do work):
//   ShadowPass    cascaded shadow map (depth only, skinned)                       Raster, or any path
//                 with volumetrics on (the volumetric march samples it); spot shadow maps of the
//                 characters for the spots Renderer::PickSpotShadowSlices chose (SpotShadowsWanted:
//                 raster/RT scene shading and volumetrics)
//   LightCull     S7c: clustered light lists (16 x 9 x 24 froxels over the main camera), a no-op            raster/RT
//                 at <= kClusterMinLights lights or offscreen (runs right before ScenePass)
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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include "render/BloomFft.h"
#include "render/RenderPass.h"
#include "render/PtPackVariants.h"
#include "render/PackTextures.h"
#include "render/ShaderPack.h"
#include "render/MmeScriptVM.h"
#include "render/MmeSemantics.h"
#include <wrl/client.h>

namespace mmdx {

struct ShaderPack;
struct OfflineJobDesc;
class EffectPipeline;   // PassEffect.cpp: one effect pack's pipeline (root signature + PSO)
class MmeRuntime;       // MmeRuntime.h: the MME per-frame driver (owned by MmePostPass)
class MmeConstantWriter;  // MmeSemantics.h: one stage's $Globals staging buffer

// Spot shadow maps are rendered (and sampled by the scene / volumetric shaders) when shadows are on
// and something reads them: the raster and RT scene shaders, or the volumetric march.
inline bool SpotShadowsWanted(const RenderSettings& s, RenderPath path, bool offscreen) {
    return s.shadows && (path != RenderPath::PathTraced || (s.volumetric && !offscreen));
}
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
    // `light`: skip the models outside this light's lit volume (their shadow could not land on anything it lights).
    void DrawSlice(PassContext& pc, const Texture& map, uint32_t slice, uint32_t matrixIndex, bool charactersOnly,
                   const DirectX::XMMATRIX* overrideMatrix = nullptr, const PunctualLight* light = nullptr);
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> psoOpaque_, psoAlpha_;
};

// S7c: clustered light cull (shaders/light_cull.hlsl): one thread group per froxel of the main camera's 16 x 9 x 24
// grid writes the cluster's light list into the frame slot's raw buffer (Renderer::lightClusterBuf_, bound through the
// bindless buffer table). Runs right before ScenePass, from the camera frustum only (no depth pre-pass); the scene
// shaders read the lists through shaders/light_cluster.hlsli. Only dispatched when the frame has more than
// kClusterMinLights lights (PassContext::lightCluster); otherwise nothing runs and the shaders keep the full loop.
class LightCullPass final : public IRenderPass {
public:
    const char* Name() const override { return "LightCull"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    ComputePipeline cull_;   // light_cull.hlsl CSLightCull
};

class ScenePass final : public IRenderPass {
public:
    ScenePass() = default;
    ~ScenePass() override;
    const char* Name() const override { return "Scene"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

    // S5: draws the scene into `color` (3 MRTs: `color`, `normal`, `velocity`) for a stage screen's camera feed:
    // raster, lit, no packs / MME / ID / other views. `pc.sceneConstants` / `pc.lights` are the feed's own slots.
    // False when the pass has no PSOs (nothing is drawn).
    bool DrawFeed(PassContext& pc, Texture& color, Texture& normal, Texture& velocity, Texture& depth);

private:
    // DLSS-RR guides on the RayTraced path: a second geometry pass with the default material data (packs / MME
    // keep their own colour shading). Created on the first RR frame only; absent with RR off.
    void DrawGuides(PassContext& pc);
    // S5: single-sample variants of the lit PSOs for a stage screen's camera feed (the feed targets are 1x whatever
    // the frame's MSAA is). Built on the first feed only; the feed draws the plain lit shading.
    bool EnsureFeedPipelines(Dx12Context& ctx);
    bool feedTried_ = false;
    ComPtr<ID3D12PipelineState> psoFeedBack_, psoFeedNoCull_, psoFeedAddBack_, psoFeedAddNoCull_,
        psoFeedNdBack_, psoFeedNdNoCull_;
    ComPtr<ID3D12PipelineState> psoGuideBack_, psoGuideNoCull_;
    Texture guideDiffuse_, guideSpecular_, guideNormal_;
    ComPtr<ID3D12PipelineState> psoCullBack_, psoNoCull_, psoEdge_, psoSky_, psoFloor_;
    ComPtr<ID3D12PipelineState> psoSkyFeed_;   // psoSky_ for the single-sample feed / mirror targets
    ComPtr<ID3D12PipelineState> psoWireBack_, psoWireNoCull_, psoWireFloor_, psoFlatBg_;
    // model .emd blend hints (MatFlag_Additive / MatFlag_NoDepthWrite): additive (SrcAlpha/One, no depth write)
    // and no-depth-write (same blend, depth write off) variants of the lit PSOs. Normal/velocity MRT writes off.
    ComPtr<ID3D12PipelineState> psoAddBack_, psoAddNoCull_, psoNdBack_, psoNdNoCull_;
    // RayTracing variants (DXC, ps_6_5, RT_SHADOWS=1); null when DXC/DXR is unavailable
    ComPtr<ID3D12PipelineState> psoCullBackRt_, psoNoCullRt_, psoFloorRt_;
    ComPtr<ID3D12PipelineState> psoAddBackRt_, psoAddNoCullRt_, psoNdBackRt_, psoNdNoCullRt_;
    // ID target variants (4 MRTs: Color, Normal, Velocity, ID):
    bool EnsureIdPipelines(Dx12Context& ctx);   // lazily builds the *Id_ PSOs; false = no ID image this frame
    bool idTried_ = false;
    ComPtr<ID3D12PipelineState> psoCullBackId_, psoNoCullId_;
    ComPtr<ID3D12PipelineState> psoCullBackRtId_, psoNoCullRtId_;
    ComPtr<ID3D12PipelineState> psoAddBackId_, psoAddNoCullId_, psoNdBackId_, psoNdNoCullId_;
    ComPtr<ID3D12PipelineState> psoAddBackRtId_, psoAddNoCullRtId_, psoNdBackRtId_, psoNdNoCullRtId_;
    // path tracer outlines: depth-only pre-pass (PSDepthAlpha, colour writes off); null = PT draws no outlines
    ComPtr<ID3D12PipelineState> psoDepthBack_, psoDepthNoCull_;
    // raster camera view: depth pre-pass of the opaque materials (no pixel shader), so the stage's lit pixel shader runs
    // once per visible surface instead of once per layer of overdraw. Null = no pre-pass.
    ComPtr<ID3D12PipelineState> psoPrepassBack_, psoPrepassNoCull_;
    ComPtr<ID3D12RootSignature> rootSig_;
    // PT mode: the outline layer (depth pre-pass + edges, default or PackEdge) for PathTracePass to composite
    void DrawPtEdges(PassContext& pc);
    // The opaque materials' depth, with only the depth target bound (the caller re-binds the colour targets).
    void DrawDepthPrepass(PassContext& pc);

    // M4b: MME object effects drawn inside Execute (lit camera view only). DrawMmeModel draws one model's owned
    // materials with its effect. `handled[i]` per material: 0 = draw natively (existing code), 1 = drawn by MME
    // (skip the native draw), 2 = an empty technique matched (the material is not drawn at all). DrawMmeEdges
    // draws the model's edge materials with the effect's edge technique. Both return true when they issued a draw
    // (the caller rebinds ScenePass's root signature + tables after switching back).
    void DrawMmeModel(PassContext& pc, GpuModel& model, bool rt, std::vector<uint8_t>& handled);
    bool DrawMmeEdges(PassContext& pc, GpuModel& model);
    // M4b: per-(instance, program, stage) $Globals writers for object draws (owned; freed with the pass).
    std::map<uint64_t, std::unique_ptr<MmeConstantWriter>> mmeWriters_;
    std::set<std::string> mmeDiag_;   // once-per-message diagnostics (draw-skip reasons)
    // used technique names per (instance, model) for the [I] OBJECT line
    std::map<uint64_t, std::set<std::string>> mmeUsed_;   // key: (instance ptr, model ptr)
    std::set<uint64_t> mmeLogged_;                        // (instance, model) the [I] OBJECT line was printed for

    // Shader packs (render/ShaderPack.h): PSOs compiled on first use from mmd.hlsl's PSPack with the pack's surface,
    // PSOs keyed by pack id (compile defines depend only on the manifest), dropped when the registry generation
    // changes (reload). failed = compile error: the model falls back to the default PSOs. edge = the pack's outline
    // PSO (PACK_HAS_EDGE). The pack's textures (pack.json "textures") live in one texture set per (pack id,
    // resolved folder): each set uploads the pack's textures out of its folder (Game texture sets differ per
    // character) with its own 16-SRV range + white fallback, dropped with the PSOs on reload.
    struct PackPipelines {
        ComPtr<ID3D12PipelineState> back, noCull, backRt, noCullRt, edge;
        ComPtr<ID3D12PipelineState> backId, noCullId, backRtId, noCullRtId;
        bool failed = false, rtTried = false;
        using TextureSet = PackTextures::Set;
        std::map<std::string, TextureSet> sets;        // resolved folder (utf-8, "" = pack folder) -> its set
    };
    const PackPipelines* PackPsos(Dx12Context& ctx, const std::string& id, const std::filesystem::path& textureFolder,
                                  bool rt, bool hasId = false);
    bool EnsurePackTextures(Dx12Context& ctx, const ShaderPack& pack, PackPipelines::TextureSet& set,
                            const std::filesystem::path& folder);
    void ReleasePackPsos(Dx12Context& ctx);   // waits for the GPU, frees the pack texture SRVs and PSOs
    std::map<std::string, PackPipelines> packPsos_;
    uint32_t packGeneration_ = 0;
    std::filesystem::path shaderDir_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC litDesc_{}, litDescId_{}; // the lit PSO descs (3 MRTs and 4 MRTs)
    ComPtr<ID3DBlob> vs_, vsRt_, vsDxc_, psLit_;             // kept alive for litDesc_ / the pack PSOs
};

class ResolvePass final : public IRenderPass {
public:
    const char* Name() const override { return "Resolve"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline pipe_, pipeId_;
    std::filesystem::path shaderDir_;
    uint32_t msaa_ = 1;
    bool idTried_ = false;   // pipeId_ is built on the first frame with an ID target
};

class SsaoPass final : public IRenderPass {
public:
    const char* Name() const override { return "SSAO"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    FullscreenPipeline ao_, blur_, blurRt_, temporal_;
    ComputePipeline rtao_;   // rtao.hlsl, writes raw_ as a UAV
    Texture raw_, temp_, out_;
    // RTAO temporal accumulation (ssao.hlsl PSTemporal): hist_[histCur_] receives this frame,
    // hist_[histCur_ ^ 1] holds last frame when histValid_.
    Texture hist_[2];
    Texture histZ_[2];
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
    ComputePipeline rtMirror_;   // rtreflect.hlsl CSReflectMirror, writes mirrorOut_ as a UAV (full res)
    Texture out_;
    Texture mirrorOut_;
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
    ComputePipeline temporalAlbedo_;  // optional (RenderSettings::ptAccumAlbedo): accumulates the radiance, derives the effective albedo
    ComputePipeline edgeComposite_;   // pt_edge.hlsl: ScenePass's outline layer over the denoised colour
    PtPackVariants ptVariants_;
    std::filesystem::path shaderDir_;
    Texture light_, albedo_;        // noisy demodulated radiance, primary albedo (RGBA16F)
    Texture history_[2];            // accumulated light, a = history length (RGBA16F)
    Texture histDepth_[2];          // R32_FLOAT raw depth of the frame each history belongs to
    Texture histNormal_[2];         // RGBA16F oct normal of that frame
    // offline video only (RenderSettings::ptAccumAlbedo), created on first use:
    Texture histAlbedo_[2];         // RGBA16F accumulated radiance (light * albedo) with the light's weights
    Texture rCur_, albEff_;         // RGBA16F this pass's radiance, the effective albedo R_acc / L_acc
    Texture edgeAcc_;               // RGBA16F outline layer averaged over the passes of one video frame
    Texture filterA_, filterB_;     // a-trous ping-pong (RGBA16F)
    // DLSS-RR guides (created on first RR frame): diffuse albedo, specular albedo, normal + roughness.
    Texture rrDiffuse_, rrSpecular_, rrNormal_;   // RGBA16F render res
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
    Texture mips_[kMips];   // RGBA16F: rgb bloom, a = luminance x linear depth (the depth mask)
    // FFT convolution bloom (null without the compute path). Its grids are created on first use.
    FftBloom fft_;
    std::filesystem::path shaderDir_;
    bool fftTried_ = false;
};

// Half-res ray-marched participating medium (height fog, volumetric_common.hlsli) lit by the sun
// (shadowed through the cascaded shadow map) and the spot lights (shadowed through the spot shadow
// maps), so occluders cut light shafts out of the beams. Jittered per frame and accumulated
// temporally; composited as lit * transmittance + in-scattered light.
// S9d: the default mode is the froxel grid (volumetric_froxel.hlsl: 160 x 90 x 64, inject + light from the S7c cluster
// lists, temporal reuse, front-to-back integration, sampled per pixel); RenderSettings::volumetricMode 1 keeps this
// march as the A/B and fallback path.
// Needs the compute path (cs_6_5); does nothing without it, offscreen, or when settings.volumetric is off.
class VolumetricPass final : public IRenderPass {
public:
    const char* Name() const override { return "Volumetric"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    void ExecuteMarch(PassContext& pc);
    void ExecuteFroxel(PassContext& pc);
    bool EnsureFroxelTargets(Dx12Context& ctx);

    ComputePipeline march_;       // volumetric.hlsl CSMarch -> raw_ (UAV)
    ComputePipeline temporal_;    // volumetric.hlsl CSTemporal raw_ + history_[prev] -> history_[cur]
    ComputePipeline blur_;        // volumetric.hlsl CSBlur  -> depth-aware separable blur history_[cur] -> temp_ -> raw_
    FullscreenPipeline apply_;    // volumetric_apply.hlsl PSApply, lit * T + L (depth-aware upsample)
    Texture raw_, temp_;          // half render res RGBA16F, rgb = in-scattered radiance, a = transmittance
    Texture history_[2];          // temporal accumulation ping-pong (same format)
    uint32_t historyIndex_ = 0;   // history_[historyIndex_] holds the last frame's result
    uint64_t lastFrame_ = 0;      // pc.frame of the last executed frame (history validity)
    bool historyReady_ = false;
    // S9d froxel grid (its textures are created on the first enabled frame, so volumetrics off cost nothing)
    ComputePipeline froxelLight_[2];   // volumetric_froxel.hlsl CSFroxelLight: 0 raster (shadow maps), 1 RT (TLAS)
    ComputePipeline froxelTemporal_;
    ComputePipeline froxelIntegrate_;
    FullscreenPipeline froxelApply_;   // volumetric_froxel_apply.hlsl PSApplyFroxel
    Texture froxelRaw_;                // this frame's injected grid (scattered light + extinction, RGBA16F 3D)
    Texture froxelHistory_[2];         // temporal ping-pong of the blended grid
    Texture froxelLit_;                // integrated (L, T) grid the apply pass reads
    uint32_t froxelHistoryIndex_ = 0;  // froxelHistory_[froxelHistoryIndex_] holds last frame's blended grid
    uint64_t froxelLastFrame_ = 0;     // pc.frame of the last froxel frame (history validity)
    float froxelLastNearZ_ = 0.5f;     // the previous frame's grid mapping (history reprojection)
    float froxelLastMaxDistance_ = 400.0f;
    bool froxelReady_ = false;
    bool froxelHistoryValid_ = false;  // a blended grid exists (else the first frame keeps its own injection)
    // Spot beams: the spots' in-scattering at the full render resolution, added on top of the froxel result
    // (volumetric_beams.hlsl; the froxel grid skips the spot lights while these run)
    ComputePipeline beams_;        // volumetric_beams.hlsl CSBeams -> beamRaw_ (one thread per full-res pixel)
    ComputePipeline beamTemporal_; // volumetric_beams.hlsl CSBeamTemporal beamRaw_ + beamHistory_[prev] -> [cur]
    FullscreenPipeline beamApply_; // volumetric_beams_apply.hlsl PSApplyBeams, lit + L (additive)
    Texture beamRaw_;              // full render res RGBA16F: this frame's beam march
    Texture beamHistory_[2];       // temporal ping-pong of the blended beams (full render res RGBA16F)
    uint32_t beamHistoryIndex_ = 0;   // beamHistory_[beamHistoryIndex_] holds last frame's blended beams
    uint64_t beamLastFrame_ = 0;      // pc.frame of the last beam frame (history validity)
    bool beamHistoryReady_ = false;
    void ExecuteBeams(PassContext& pc, const float params[16], float maxDistance);
};

// S9d: the froxel grid's dimensions (screen tiles x exponential view-depth slices): the light / integrate passes and the
// apply shader agree through these constants (volumetric_froxel.hlsl, volumetric_froxel_apply.hlsl).
inline constexpr uint32_t kFroxelX = 160;
inline constexpr uint32_t kFroxelY = 90;
inline constexpr uint32_t kFroxelZ = 64;

// M8b: video layers of the MV packages (docs/mme/mme_design.md §5b). One instance draws the background (behind the
// scene, where the depth is far), a second the overlay (over the final image, after the post stack). Each instance
// serves the matching layers of FrameView::videoLayers and owns its own decode thread + upload ring. In play mode
// the decode runs ahead of the requested time; the offline entry points decode synchronously, frame-exact.
enum class VideoStage : uint8_t { Background, Overlay };
class VideoPass final : public IRenderPass {
public:
    explicit VideoPass(VideoStage stage);
    ~VideoPass() override;
    const char* Name() const override { return stage_ == VideoStage::Background ? "Video(bg)" : "Video(overlay)"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

    // Offline GI: decode the matching layers frame-exact for `time` of `pc.view`. The background is drawn onto the
    // linear HDR `lit` (where `depth` is far, R32_FLOAT raw device depth); the overlay is composited over `image`
    // (ping-ponging through `scratch`, same format). Return true when a layer was drawn.
    bool RunOffline(PassContext& pc, Texture& target, Texture& depthOrScratch);

    // Releases decode threads, staging buffers and textures (scene end / unload).
    void Clear(Dx12Context& ctx);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VideoStage stage_;
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
                    Texture& depth, Texture& velocity, Texture& normal, const OfflineJobDesc* job = nullptr,
                    Texture* id = nullptr);

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
        Texture *src = nullptr, *dst = nullptr, *depth = nullptr, *velocity = nullptr, *normal = nullptr, *id = nullptr;
        uint32_t outW = 0, outH = 0;
        float dt = 0.0f;
        float sceneTime = 0.0f;       // gP3.x: the frame's scene / timeline seconds (effect time)
        bool resetState = false;      // camera cut / resize / job reset
        uint32_t stateSteps = 0;      // state updates this run (0 = none, unless the slot has no valid state yet)
        float focus[4] = {};          // gP1: focus z, focus aperture, user DoF aperture, max CoC px
    };
    bool RunEntry(PassContext& pc, const EntryRun& run);   // false = skipped (dst untouched)
    void DropStaleStateSlots(Dx12Context& ctx, const std::vector<EffectStackEntry>& stack);
    // API v5 intermediate-pass targets (RGBA16F), pooled by size and mips
    Texture* PassTarget(Dx12Context& ctx, uint32_t w, uint32_t h, uint32_t n, bool mips = false);
    void TrimPassPool(Dx12Context& ctx);
    void ReleasePassPool(Dx12Context& ctx);
    struct PoolTexture {
        Texture tex;
        uint64_t lastUse = 0;
    };
    std::map<std::tuple<uint32_t, uint32_t, bool>, std::deque<PoolTexture>> passPool_;   // deque: stable addresses

    bool AllocTargets(Dx12Context& ctx, uint32_t w, uint32_t h);
    bool AllocPostTargets(Dx12Context& ctx, uint32_t w, uint32_t h);

    struct StateSlot {
        Texture tex[2];     // 4x1 RGBA32F
        uint32_t cur = 0;   // 0 or 1: index of texture written last
        bool hasValidState = false;
        uint64_t lastPassExecution = 0;
    };
    using StateKey = std::pair<size_t, std::string>;

    struct HistorySlot {
        Texture tex[kPackMaxPasses];
        bool valid[kPackMaxPasses] = {};
        uint64_t lastPassExecution = 0;
    };

    void GenerateMips(PassContext& pc, Texture& tex);
    ComPtr<ID3D12RootSignature> downsampleRootSig_;
    ComPtr<ID3D12PipelineState> downsamplePso_;

    bool preBloom_;           // this instance's share: pre-bloom (HDR) or post (LDR) entries
    Texture ping_[2];         // RGBA16F ping-pong, outWidth x outHeight (pre-bloom share)
    Texture post_[2];         // RGBA8 ping-pong, outWidth x outHeight (post share)
    uint32_t generation_ = 0;
    std::filesystem::path shaderDir_;
    std::map<std::string, EffectPipelines> psos_;
    std::map<StateKey, StateSlot> stateSlots_;
    std::map<StateKey, HistorySlot> historySlots_;
    uint64_t passExecutionCount_ = 0;
    bool resetRequested_ = false;
    // The previous frame's scene time (seek / backwards jumps reset the effect state; see RunStack).
    float lastSceneTime_ = 0.0f;
    bool hasLastSceneTime_ = false;
    ComPtr<ID3D12Resource> extParamsRing_;
    uint8_t* extParamsMapped_ = nullptr;
    size_t extParamsOffset_ = 0;
};

// M5a: the MME technique selection of M4b (first technique whose MMDPass / Subset / use-flags match; object_ss
// falls back to object), shared with the offscreen / shadow passes; and the static zero/white vertex buffer slot.
const MmeTechnique* SelectMmeTechnique(const MmeEffectDesc& fx, const std::string& wantedPass, const GpuModel& model,
                                       size_t material, bool exactPass = false);
D3D12_VERTEX_BUFFER_VIEW MmeZeroWhiteVb(Dx12Context& ctx);

// M5b fix 1: true when the MME layer owns material `material` of `model` in this frame -- the effect's object /
// object_ss technique applies (drawn by the MME layer) or an empty technique matches ("not drawn" by MMD either).
// The native depth pre-pass of an MME layer (ScenePass::DrawPtEdges, OfflineRenderer::DrawEdges) must not write
// those materials' depth, or the MME draw (a different vertex path) z-fights its own surfaces. Mirrors the
// per-material ownership ScenePass::DrawMmeModel records in its `handled` vector (same technique selection).
// `mme` is the renderer's runtime (PassContext::mme); false without one / when the instance is not ready yet.
bool MmeMaterialOwned(MmeRuntime* mme, const GpuModel& model, size_t material);

// M5b: the per-material input for one MME object draw of `model` (material semantics + by-name uniforms), filled
// from MaterialConstantsCpu / GpuModel. Shared by ScenePass's raster/PT draws, the offscreen pass and the offline
// renderer's MME layer (OfflineRenderer.cpp).
struct MmeObjectValues;
void FillMmeObjectValues(const GpuModel& model, size_t material, MmeObjectValues& out);
// M5b: the MME frame values for a camera draw with an explicit view / projection and viewport (PT: the tracer's
// jittered projection; offline: the iteration's lens / shutter camera). One MMD self-shadow light camera per call.
struct MmeFrameValues;
void FillMmeFrameValues(PassContext& pc, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
                        float viewportW, float viewportH, MmeFrameValues& out);

// M5b: draws the MME-owned materials of `model` into the currently bound 3 MRTs (colour / normal / velocity)
// through the runtime (`pc.mme` / `pc.mmeSkin`), with `frame`'s camera values and the host cbuffer's prevViewProj.
// `rtFormats` / `dsFormat` / `sampleCount` describe the bound targets. Used by the offline GI MME layer; ScenePass's
// raster / PT draws use the same per-material body inline. Returns true when at least one material was drawn.
// M5b fix 2: `generation` + `shutter` reach MmeSkin::Skin so the offline renderer re-skins the iteration's pose
// (motion blur) instead of keeping the first iteration's; the real-time callers keep it a frame-level skin.
bool DrawMmeModelToMrt(PassContext& pc, GpuModel& model, const MmeFrameValues& frame,
                       const DirectX::XMFLOAT4X4& prevViewProjNoJitter, const DXGI_FORMAT* rtFormats,
                       DXGI_FORMAT dsFormat, uint32_t sampleCount, uint64_t generation = 0, float shutter = 1.0f);

// M5a: the CONTROLOBJECT lookup over FrameView::mmeObjects (models); shared by ScenePass's M4b draws and the
// offscreen pass. Fills up to 16 floats (a float4x4 fills all 16); outCount receives the number written.
std::function<bool(const std::string&, const std::string&, int, float*, int&)> MmeSceneLookup(const FrameView& view);

// M5b fix 2: offline entry point of the MMD-style self-shadow map. The real-time shadow pass (MmeShadowPass) is not
// part of the offline renderer's pass list, so the offline MME layer renders the map itself (once per image, before
// its first object_ss draw, so it never samples an unrendered map). Draws each MME model with its effect's zplot
// (else the built-in standard effect) into caller-owned `map` (R32_FLOAT size x size) + `depth`, through the same
// light camera (MmeSemantics' BuildLightCamera) the object_ss compare uses, then binds it via
// pc.mme->SetObjectShadowMap. False when nothing was drawn. See PassMmeOffscreen.cpp.
bool RenderMmeSelfShadowMap(PassContext& pc, Texture& map, Texture& depth, uint32_t size,
                            const std::filesystem::path& shaderDir);

// The MME post effects (--mme): runs the enabled, ready, post-class MMEEffectEntry list from RenderSettings::mmeEffects
// on the LDR image (targets.ldr, output resolution) through an MmeRuntime driver, chaining the effects in list order
// via ping-pong RGBA8 targets. Zero effects: nothing allocates, nothing runs (a byte-identical frame). A non-lit
// view skips like PackEffectPass. The pass is the IMmeHost for the script VM; the work lives in PassMme.cpp.
class MmePostPass final : public IRenderPass, public IMmeHost {
public:
    MmePostPass();
    ~MmePostPass() override;
    const char* Name() const override { return "Mme"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

    // M5b: runs this pass's MME post chain on an offline image (OfflineRenderer::Impl::Finish). `io` is the sRGB
    // image (chain input / output), `scratch` a second texture of the same size and format for ping-ponging. The
    // effects come from OfflineJobDesc::mmeEffects; `pc.mme` must be the renderer's runtime. Effects that need a
    // larger target render it at their own output size (the chain runs where the offline renderer calls it).
    bool RunOffline(PassContext& pc, const std::vector<MmeEffectEntry>& effects, Texture& io, Texture& scratch);
    // M7 fix 3: copy a chain result into `out` as an opaque (rgb, alpha = 1) fullscreen blit. The scene LDR image's
    // alpha is coverage, and MMD's back buffer is XRGB, so the MME chain output must not change it.
    void CopyBackOpaque(PassContext& pc, Texture& out, Texture& src);

    // IMmeHost (called by RunMmeTechnique during Execute)
    void SetColorTarget(int index, const std::string& name) override;
    void SetDepthTarget(const std::string& name) override;
    void Clear(bool color, bool depth, const float color4[4], float depthValue) override;
    void ScriptExternal() override;
    void DrawBuffer(const MmeTechnique& t, const MmePass& p) override;
    void DrawGeometry(const MmeTechnique& t, const MmePass& p) override;
    bool ReadParam(const std::string& name, float out[4]) override;
    void SetLoopIndex(const std::string& name, int index) override;
    void Diag(const std::string& message) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// M5a: offscreen scene re-render (docs/mme/mme_design.md 2.3). For every OFFSCREENRENDERTARGET of every ready MME
// instance (post or object), clears it and draws every FrameView::models model matched by the target's parsed
// DefaultEffect table (first match wins; `hide`, `none` = the built-in standard effect, else the sub-effect), using
// the object draw path without the host MRT. Registered right before ScenePass; a no-op (byte-identical frame) when
// no instance declares an offscreen target. The work lives in PassMmeOffscreen.cpp.
class MmeOffscreenPass final : public IRenderPass {
public:
    MmeOffscreenPass();
    ~MmeOffscreenPass() override;
    const char* Name() const override { return "MME Offscreen"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void Execute(PassContext& pc) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// M5a: the MMD-style self-shadow map (docs/mme/mme_design.md 2.7). When any ready MME instance used by a model has
// an object_ss or zplot technique and shadows are on, renders an R32_FLOAT 2048^2 light-space depth map (each MME
// model with its effect's zplot, each non-MME model with the built-in standard effect's zplot) through the light
// camera of MmeSemantics' BuildLightCamera, then binds it at register(s0) for object_ss draws. Registered right
// before ScenePass (its own depth, no colour output).
class MmeShadowPass final : public IRenderPass {
public:
    MmeShadowPass();
    ~MmeShadowPass() override;
    const char* Name() const override { return "MME Shadow"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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

class WorldPass final : public IRenderPass {
public:
    WorldPass();
    ~WorldPass() override;
    const char* Name() const override { return "World"; }
    bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) override;
    void OnResize(Dx12Context& ctx, RenderTargets& targets) override;
    void ReleaseTargets(Dx12Context& ctx) override;
    void Execute(PassContext& pc) override;

    // Offline GI renderer draws world surfaces and particles into edgeColorMsaa / edgeDepthMsaa
    // iterationViewProj: the offline iteration's lens / shutter camera (nullptr = the scene camera)
    void DrawOffline(PassContext& pc, const Texture& edgeColorMsaa, const Texture& edgeDepthMsaa,
                     const DirectX::XMFLOAT4X4* iterationViewProj = nullptr);

    // Post-composite particle draw (raster / RT): alpha / additive blended into targets.lit, depth-tested against
    // targets.depth (R32_FLOAT raw device depth). Uses the same per-instance state as Execute.
    void DrawParticlesPost(PassContext& pc);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Draws world-effect particles after CompositePass (and before VolumetricPass / TaaPass), so they never appear in
// SSR / SSAO / RT reflections. Holds the renderer's WorldPass (no second simulation state).
class WorldParticlesPass final : public IRenderPass {
public:
    explicit WorldParticlesPass(WorldPass* world) : world_(world) {}
    const char* Name() const override { return "WorldParticles"; }
    bool CreatePipelines(Dx12Context&, const std::filesystem::path&, uint32_t) override { return true; }
    void Execute(PassContext& pc) override { if (world_) world_->DrawParticlesPost(pc); }

private:
    WorldPass* world_;
};

} // namespace mmdx

