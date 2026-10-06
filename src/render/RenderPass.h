#pragma once
// Render pass plumbing. The Renderer owns an ordered list of passes; new techniques
// (ray-traced reflections, upscalers, ...) are added as passes without touching the
// existing ones.
#include "render/Dx12Context.h"
#include "render/RenderTypes.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace mmdx {

struct BuiltinTextures;

// A GPU texture with its tracked resource state and optional views.
struct Texture {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;     // resource format (may be typeless)
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;  // view format for SRVs
    uint32_t width = 0, height = 0, samples = 1, arraySize = 1;
    uint32_t rtv = DescriptorHeap::kInvalid;      // ctx.RtvHeap(), arraySize entries for arrays
    uint32_t dsv = DescriptorHeap::kInvalid;      // ctx.DsvHeap(), arraySize entries for arrays

    explicit operator bool() const { return res != nullptr; }
    // Creates a 2D texture (array when arraySize > 1). `flags` decides which views exist:
    // ALLOW_RENDER_TARGET -> rtv, ALLOW_DEPTH_STENCIL -> dsv (format must then be typeless
    // R32_TYPELESS; DSV uses D32_FLOAT and SRVs R32_FLOAT).
    bool Create(Dx12Context& ctx, uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                D3D12_RESOURCE_STATES initial, const wchar_t* name, uint32_t samples = 1,
                uint32_t arraySize = 1, const float* clearColor = nullptr);
    void Release(Dx12Context& ctx);  // frees views; the resource is released via DeferRelease
    // Records a transition barrier if the state differs.
    void Transition(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES to);
    // Writes an SRV for this texture into `cpu`.
    void WriteSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
    // Writes a UAV (Texture2D, mip 0) into `cpu`. Needs D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.
    void WriteUav(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
};

// Per-frame scratch descriptors in the shader-visible heap. Each frame slot owns a fixed
// window; Begin(slot) rewinds it (the slot's previous GPU work is complete by then).
class TransientDescriptors {
public:
    static constexpr uint32_t kPerSlot = 512;
    bool Create(Dx12Context& ctx, uint32_t slots);
    void Release(Dx12Context& ctx);
    void Begin(uint32_t slot);
    // Allocates `count` contiguous descriptors; returns the first heap index.
    uint32_t Alloc(uint32_t count);
    // Allocates a table of SRVs for `textures` (null entries get a null 2D SRV).
    D3D12_GPU_DESCRIPTOR_HANDLE SrvTable(Dx12Context& ctx, std::initializer_list<const Texture*> textures);
    // Allocates a table of UAVs for `textures` (null entries get a null RGBA16F Texture2D UAV).
    D3D12_GPU_DESCRIPTOR_HANDLE UavTable(Dx12Context& ctx, std::initializer_list<const Texture*> textures);

private:
    uint32_t base_ = DescriptorHeap::kInvalid, slots_ = 0, slot_ = 0, used_ = 0;
};

// Targets shared between passes. Everything up to the upscaler runs at the render resolution
// (width x height); the upscaler output, bloom, ldr and the UI backdrop use the output
// resolution (outWidth x outHeight, equal to width x height without an upscaler).
struct RenderTargets {
    uint32_t width = 0, height = 0, msaa = 1;
    uint32_t outWidth = 0, outHeight = 0;
    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kNormalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // oct normal (view), reflectivity, coverage
    static constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16_FLOAT;      // uv(current) - uv(previous)
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_TYPELESS;
    static constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // MSAA scene targets (written by ScenePass)
    Texture colorMsaa, normalMsaa, velocityMsaa, depthMsaa;
    // Resolved (1 sample) copies (written by ResolvePass, or as UAVs by PathTracePass).
    // color/normal/velocity/depth/lit also allow unordered access.
    Texture color, normal, velocity, depth;   // depth: R32_FLOAT, raw device depth (closest sample)
    Texture lit;      // CompositePass: color * AO + SSR (+ fog)
    Texture ldr;      // PostPass output (output resolution), sRGB-encoded 8-bit (what PresentPass shows)
    // Shadow cascades (owned here so the scene pass can bind them)
    Texture shadowMap;  // R32_TYPELESS array, kShadowCascades slices
    Texture spotShadowMap;  // R32_TYPELESS array, kSpotShadowSlices perspective slices (ShadowPass)
    // Outputs of optional passes for this frame (null when the pass is disabled)
    Texture* ao = nullptr;        // R8 (half res), SsaoPass after blur
    Texture* ssr = nullptr;       // RGBA16F (half res), SsrPass
    Texture* hdrFinal = nullptr;  // input of PostPass: &lit, the TAA output, or the upscaler output (output res)
    Texture* bloom = nullptr;     // half output res, BloomPass
    Texture* uiBackdrop = nullptr;  // RGBA8, blurred final image for frosted UI panels
    Texture* lut = nullptr;         // colour LUT strip (kColorLutSize^2 x kColorLutSize RGBA8), Renderer::SetColorLut; null = none
};

class RtScene;
class IUpscaler;

struct PassContext {
    Dx12Context& ctx;
    ID3D12GraphicsCommandList* cmd;
    const FrameView& view;
    const RenderSettings& settings;
    RenderTargets& targets;
    RenderStats& stats;
    TransientDescriptors& transient;
    D3D12_GPU_VIRTUAL_ADDRESS sceneConstants;  // SceneConstants for this frame
    D3D12_GPU_VIRTUAL_ADDRESS lights;          // StructuredBuffer<GpuLight> for this frame
    uint64_t frame;                            // ctx.FrameNumber() (bone/morph ring index)
    bool historyValid;                         // temporal passes may read last frame's history
    bool offscreen;                            // RenderToImage: no back buffer, no UI backdrop
    const BuiltinTextures* builtin;
    RenderPath path;                           // effective path (Raster when DXR is unavailable)
    RtScene* rt;                               // built acceleration structures; null for Raster
    IUpscaler* upscaler;                       // active upscaler; null when none / unavailable
    float jitterPxX, jitterPxY;                // projection jitter in render pixels (see Renderer.cpp)
    float frameTimeMs;                         // wall time since the previous on-screen frame
    D3D12_GPU_VIRTUAL_ADDRESS extraSceneConstants[3] = {};  // one per FrameView::extraViews entry (set after construction)
};

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual const char* Name() const = 0;
    // Called once, and again whenever RenderTargets.msaa changes (PSOs depend on it).
    virtual bool CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) = 0;
    // Called after the shared targets were (re)created; passes (re)create their own targets.
    virtual void OnResize(Dx12Context& ctx, RenderTargets& targets) {}
    virtual void ReleaseTargets(Dx12Context& ctx) {}
    virtual void Execute(PassContext& pc) = 0;
};

using ShaderDefines = std::vector<std::pair<std::string, std::string>>;

// Compiles with DXC (dxcompiler.dll + dxil.dll next to the exe, loaded on first use) for
// shader model 6.x targets such as "cs_6_5" / "ps_6_5" (needed for inline ray queries).
// Same defines and error logging as CompileShader; includes resolve relative to `file`.
// Returns null (and logs once) when dxcompiler.dll is missing. `errors` (optional) receives the compiler's error
// text on failure.
ComPtr<ID3DBlob> CompileShaderDxc(const std::filesystem::path& file, const char* entry, const char* target,
                                  const ShaderDefines& defines = {}, std::string* errors = nullptr);

// Both compilers cache bytecode in <exe>/shader_cache, keyed by the sources of the shader's directory (and, with a
// MMDX_PACK define, the pack's directory). The directory hashes are computed once per run: call this after shader
// files change on disk (shader pack reload).
void ResetShaderSourceHashes();

// Compiles an HLSL entry point with D3DCompileFromFile (D3DCOMPILE_ENABLE_STRICTNESS,
// plus DEBUG|SKIP_OPTIMIZATION in debug builds, OPTIMIZATION_LEVEL3 otherwise).
// `target` e.g. "vs_5_1". Logs compiler errors via LOG_ERROR and returns null on failure. `errors` (optional)
// receives the compiler's error text on failure.
ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target,
                               const ShaderDefines& defines = {}, std::string* errors = nullptr);

// A fullscreen-triangle pixel shader pass. Root signature:
//   0: CBV b0 (SceneConstants), 1: 16 root constants b1, 2: SRV table t0..t7,
//   static samplers s0 point clamp, s1 linear clamp, s2 linear wrap.
// The vertex shader is VSFullscreen from fullscreen.hlsli (include it from the .hlsl).
class FullscreenPipeline {
public:
    // Transmittance: dst = src.rgb + dst.rgb * src.a (dst alpha kept), for participating media.
    enum class Blend { Opaque, Additive, Alpha, Transmittance };
    bool Create(Dx12Context& ctx, const std::filesystem::path& file, const char* psEntry,
                std::initializer_list<DXGI_FORMAT> rtvFormats, const ShaderDefines& defines = {},
                Blend blend = Blend::Opaque);
    // Binds RTVs (all targets must already be in RENDER_TARGET state), viewport = first
    // target's size, and draws.
    void Draw(PassContext& pc, std::initializer_list<Texture*> targets, D3D12_GPU_DESCRIPTOR_HANDLE srvTable,
              const float* constants = nullptr, uint32_t constantCount = 0) const;
    explicit operator bool() const { return pso_ != nullptr; }

private:
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
};

// A compute pass with the shared ray-tracing root layout (see shaders/rt_common.hlsli):
//   0: CBV b0 SceneConstants         1: 16 root constants b1
//   2: SRV table t0..t7 (space0)     3: UAV table u0..u7 (space0)
//   4: root SRV t0 space1 TLAS       5: root SRV t1 space1 StructuredBuffer<RtGeometry>
//   6: root SRV t2 space1 StructuredBuffer<GpuLight> (pc.lights)
//   7: table, unbounded Texture2D t0 space2 starting at SrvHeap index 0
//   8: table, unbounded ByteAddressBuffer t0 space3 starting at SrvHeap index 0
//   static samplers s0 point clamp, s1 linear clamp, s2 linear wrap, s4 linear wrap.
// Shaders are compiled with CompileShaderDxc as cs_6_5. Root parameters 4/5 are bound to
// pc.rt (0 when null; shaders must not touch the TLAS then).
class ComputePipeline {
public:
    bool Create(Dx12Context& ctx, const std::filesystem::path& file, const char* entry,
                const ShaderDefines& defines = {});
    // Binds the root signature, PSO and all root parameters, then Dispatch(groupsX, groupsY, 1).
    // Pass {} for unused tables (a null-descriptor table is bound instead).
    void Dispatch(PassContext& pc, D3D12_GPU_DESCRIPTOR_HANDLE srvTable, D3D12_GPU_DESCRIPTOR_HANDLE uavTable,
                  const float* constants, uint32_t constantCount, uint32_t groupsX, uint32_t groupsY) const;
    explicit operator bool() const { return pso_ != nullptr; }

private:
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
};

} // namespace mmdx
