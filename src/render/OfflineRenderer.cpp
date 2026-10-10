// Offline GI renderer (see src/render/OfflineRenderer.h for the design and dispatch order).
#include "render/OfflineRenderer.h"
#include "render/Renderer.h"
#include "render/PassCommon.h"
#include "render/GpuModel.h"
#include "asset/ImageLoader.h"
#include "render/ShaderPack.h"
#include "render/PtPackVariants.h"
#include "render/PackTextures.h"
#include "render/RayTracing.h"
#include "render/Passes.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace mmdx {

namespace {

constexpr uint32_t kSlots = Dx12Context::kFramesInFlight;
constexpr uint32_t kLightBytes = Renderer::kMaxPunctualLights * sizeof(GpuLight);
constexpr uint32_t kEdgeIterations = 256;
constexpr uint32_t kOfflineFftSize = 512;            // convolution bloom grid (bloom_fft.hlsl FFT_N)
constexpr float kConvolutionBloomGain = 6.0f;        // as the real-time convolution bloom output
constexpr float kConvolutionBloomIntensity = 0.12f;  // CSFinalize multiplier of the convolved bloom  // outline layers averaged for motion blur / depth of field
constexpr float kLensScale = 0.007f;       // lens radius / focus distance (~8 px background blur at 1080p)

float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

// Uniform unit-disk sample (concentric map), same as offline_gi.hlsl ConcentricDisk.
void ConcentricDisk(float u, float v, float& x, float& y) {
    const float ox = u * 2.0f - 1.0f, oy = v * 2.0f - 1.0f;
    x = y = 0.0f;
    if (ox == 0.0f && oy == 0.0f) return;
    float r, phi;
    if (std::fabs(ox) > std::fabs(oy)) {
        r = ox;
        phi = 0.78539816f * (oy / ox);
    } else {
        r = oy;
        phi = 1.57079633f - 0.78539816f * (ox / oy);
    }
    x = r * std::cos(phi);
    y = r * std::sin(phi);
}

const D3D12_INPUT_ELEMENT_DESC kMmdLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},   // GpuSdef
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32_FLOAT, 3, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 3, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32_FLOAT, 3, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

bool CreateRootSignature(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& desc,
                         ComPtr<ID3D12RootSignature>& out, const char* what) {
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("%s: root signature serialization failed: %s", what,
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }
    return CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&out)),
                   what);
}

ComPtr<ID3D12Resource> CreateMappedUpload(ID3D12Device* device, uint64_t bytes, uint8_t** mapped, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(upload)"))
        return {};
    res->SetName(name);
    if (FAILED(res->Map(0, nullptr, (void**)mapped))) return {};
    return res;
}

} // namespace

struct OfflineRenderer::Impl {
    Dx12Context* ctx = nullptr;
    std::filesystem::path shaderDir;
    // offline_gi.hlsl (ComputePipeline, cs_6_5)
    ComputePipeline clearImage, clearCounter, defaultRender, prepassLevel, prepassDisplay, prepassSmooth;
    ComputePipeline* render = &defaultRender;
    PtPackVariants ptVariants;
    // offline_post.hlsl
    ComputePipeline denoise, bloomDown, bloomBlur, finalize, edgeAccumulate;
    // offline_post.hlsl (lit compose, effect normal / velocity) and offline_effect.hlsl (effect depth): the effect stage
    ComputePipeline litCompose, effectDepth, effectNormal, effectVelocity;
    PackEffectPass effectPre{true}, effectPost{false};   // the shader packs' pre-bloom and post shares (RunOffline)
    // offline_volumetric.hlsl, bloom_fft.hlsl (optional effects: empty pipelines disable them)
    ComputePipeline volMarch, volBlur;
    ComputePipeline fftInput, fftRows, fftCols, fftKernel, fftOutput;
    bool fftKernelReady = false;
    bool volReady = false;             // volA holds this image's volumetric light (CSFinalize adds it)
    Texture volA, volB;                // half image size RGBA16F
    Texture gridA, gridB, kernelSpec;  // FFT grids (kFftSize^2 RGBA32F, created once)
    // offline_edge.hlsl (FXC, 4x MSAA raster)
    ComPtr<ID3D12RootSignature> edgeRootSig;
    ComPtr<ID3D12PipelineState> depthCullBack, depthNoCull, edgePso;
    // offline_edge_pack.hlsl (DXC): the edge PSO of each pack that draws its own outlines (PACK_HAS_EDGE + PackEdge),
    // compiled in Begin (GPU idle) and dropped when the pack registry reloads; null = the pack failed (default edges)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC edgeDesc{};
    std::map<std::string, ComPtr<ID3D12PipelineState>> packEdgePsos;
    uint32_t packEdgeGeneration = 0;
    // present.hlsl
    ComPtr<ID3D12RootSignature> presentRootSig;
    ComPtr<ID3D12PipelineState> presentPso;
    // image targets (job size; recreated in Begin when the size changes)
    uint32_t width = 0, height = 0;
    Texture accum, albedo, moments, gbuf, edgeLayer, edgeAccum, denoiseA, denoiseB, ldr, bloomA, bloomB;
    Texture litA, litB, ldrB, fxDepth, fxNormal, fxVel;   // effect stage: lit HDR and post ping-pong, effect inputs
    Texture edgeColorMsaa, edgeDepthMsaa;
    Texture counter;                   // 1x1 R32_UINT
    Texture preE[kOfflinePrepassLevels], preG[kOfflinePrepassLevels];  // prepass levels (irradiance, geometry)
    uint32_t prepassFine = 2;          // stride of the finest prepass level (pixels)
    bool clearPending = false;         // the prepass display used accum: clear before the render
    Texture icFinal;                   // smoothed finest level: the irradiance cache CSRender reads
    bool icValid = false;              // icFinal holds this image's cache
    // per-image constants (UPLOAD heap, persistently mapped, written only in Begin while the GPU is idle)
    ComPtr<ID3D12Resource> sceneCb, lightBuf;
    uint8_t* sceneCbMapped = nullptr;
    uint8_t* lightMapped = nullptr;
    // readbacks, one entry per frame slot
    ComPtr<ID3D12Resource> counterReadback;       // kFramesInFlight * 512 bytes
    ComPtr<ID3D12QueryHeap> timestampHeap;        // 2 * kFramesInFlight
    ComPtr<ID3D12Resource> timestampReadback;     // kFramesInFlight * 16 bytes
    uint64_t timestampFrequency = 0;
    struct Slot { uint32_t serial = 0; bool counted = false; uint32_t countedSample = 0; bool timed = false; uint32_t items = 0; };
    Slot slots[Dx12Context::kFramesInFlight];
    // job state
    FrameView view;                    // copy of the job's view (PassContext needs one)
    OfflineJobDesc job;
    uint32_t serial = 0;               // incremented per Begin
    uint64_t frame = 0;                // ctx.FrameNumber() at Begin
    bool motion = false;               // view.motionBlur: geometry and camera move inside the shutter
    float focus = 0.0f, lensRadius = 0.0f;  // thin lens (lensRadius 0: pinhole)
    uint32_t edgeLayers = 0;           // outline layers accumulated into edgeAccum
    uint32_t samples = 0;
    uint32_t lastActive = UINT32_MAX;
    float itemsPerFrame = 1.0f;
    uint32_t dispatchSeed = 0;         // running per-image dispatch counter (unique seed per dispatch)
    // dummies so a PassContext can be formed (ComputePipeline::Dispatch takes one)
    RenderSettings settings;
    RenderTargets targets;
    RenderStats stats;
    // readback command list (ReadImage)
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;

    PassContext Pc(ID3D12GraphicsCommandList* cmd, TransientDescriptors& t, const BuiltinTextures* b, RtScene& rt) {
        return PassContext{*ctx, cmd, view, settings, targets, stats, t,
                           sceneCb->GetGPUVirtualAddress(), lightBuf->GetGPUVirtualAddress(), frame,
                           false, true, b, RenderPath::PathTraced, &rt, nullptr, 0.0f, 0.0f, 16.7f};
    }
    void UavBarrier(ID3D12GraphicsCommandList* cmd) {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        cmd->ResourceBarrier(1, &b);
    }
    float NextSeed() { return (float)((serial * 7919u + ++dispatchSeed) & 0xFFFFFFu); }

    D3D12_GPU_DESCRIPTOR_HANDLE GiTable(ID3D12GraphicsCommandList* cmd, TransientDescriptors& t);
    void DispatchPost(PassContext& pc, const ComputePipeline& pipe, Texture* target, Texture* in5, Texture* in6,
                      const float* c, uint32_t groupsX, uint32_t groupsY, float lit = 0.0f);
    void Volumetric(PassContext& pc);
    void ConvolveBloom(PassContext& pc);
    void EffectInputs(PassContext& pc);
    bool EnsureTargets(uint32_t w, uint32_t h);
    void DrawEdges(ID3D12GraphicsCommandList* cmd, RtScene& rt, float shutter, float lensX, float lensY);
    void PreparePackEdges();
    void Work(ID3D12GraphicsCommandList* cmd, TransientDescriptors& t, const BuiltinTextures* b, RtScene& rt,
              OfflineProgress& pg);
    void Finish(PassContext& pc);
    void Prepass(PassContext& pc, OfflineProgress& pg);
};

D3D12_GPU_DESCRIPTOR_HANDLE OfflineRenderer::Impl::GiTable(ID3D12GraphicsCommandList* cmd, TransientDescriptors& t) {
    accum.Transition(cmd, kUav);
    albedo.Transition(cmd, kUav);
    moments.Transition(cmd, kUav);
    gbuf.Transition(cmd, kUav);
    counter.Transition(cmd, kUav);
    edgeAccum.Transition(cmd, kUav);
    return t.UavTable(*ctx, {&accum, &albedo, &moments, &gbuf, &counter, &edgeAccum});
}

void OfflineRenderer::Impl::DispatchPost(PassContext& pc, const ComputePipeline& pipe, Texture* target,
                                         Texture* in5, Texture* in6, const float* c, uint32_t groupsX,
                                         uint32_t groupsY, float lit) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    accum.Transition(cmd, kSrvAll);
    albedo.Transition(cmd, kSrvAll);
    moments.Transition(cmd, kSrvAll);
    gbuf.Transition(cmd, kSrvAll);
    edgeAccum.Transition(cmd, kSrvAll);
    if (in5) in5->Transition(cmd, kSrvAll);
    if (in6) in6->Transition(cmd, kSrvAll);
    target->Transition(cmd, kUav);
    const bool vol = volReady && volA;
    if (vol) volA.Transition(cmd, kSrvAll);
    D3D12_GPU_DESCRIPTOR_HANDLE srv =
        pc.transient.SrvTable(*ctx, {&accum, &albedo, &moments, &gbuf, &edgeAccum, in5, in6, vol ? &volA : nullptr});
    D3D12_GPU_DESCRIPTOR_HANDLE uav = pc.transient.UavTable(*ctx, {target});
    float c12[12] = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], vol ? 1.0f : 0.0f, lit, 0.0f, 0.0f};
    pipe.Dispatch(pc, srv, uav, c12, 12, groupsX, groupsY);
    UavBarrier(cmd);
}

// The effect inputs of this image (effect_api.hlsli), from the G-buffer: the raw device depth (offline_effect.hlsl), the
// oct-encoded view-space normal (CSEffectNormal) and zero motion (a 1x1 texture: the offline renderer has no per-pixel
// motion vectors).
void OfflineRenderer::Impl::EffectInputs(PassContext& pc) {
    const float proj[8] = {view.camera.nearZ, view.camera.farZ, 0.0f, 0.0f, (float)width, (float)height, 0.0f, 0.0f};
    DispatchPost(pc, effectDepth, &fxDepth, nullptr, nullptr, proj, Groups(width), Groups(height));
    DispatchPost(pc, effectNormal, &fxNormal, nullptr, nullptr, proj, Groups(width), Groups(height));
    const float one[8] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f};
    DispatchPost(pc, effectVelocity, &fxVel, nullptr, nullptr, one, 1, 1);
}

bool OfflineRenderer::Impl::EnsureTargets(uint32_t w, uint32_t h) {
    if (width == w && height == h && accum) return false;
    Dx12Context& c = *ctx;
    accum.Release(c);
    albedo.Release(c);
    moments.Release(c);
    gbuf.Release(c);
    edgeLayer.Release(c);
    edgeAccum.Release(c);
    for (Texture& tex : preE) tex.Release(c);
    for (Texture& tex : preG) tex.Release(c);
    icFinal.Release(c);
    denoiseA.Release(c);
    denoiseB.Release(c);
    ldr.Release(c);
    bloomA.Release(c);
    bloomB.Release(c);
    volA.Release(c);
    volB.Release(c);
    edgeColorMsaa.Release(c);
    edgeDepthMsaa.Release(c);
    litA.Release(c);
    litB.Release(c);
    ldrB.Release(c);
    fxDepth.Release(c);
    fxNormal.Release(c);
    fxVel.Release(c);
    effectPre.ReleaseTargets(c);    // the effect packs' pass targets / state slots of the old size
    effectPost.ReleaseTargets(c);
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4;
    const float zero[4] = {0, 0, 0, 0};
    bool ok = true;
    ok &= accum.Create(c, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kUav, L"offline.accum");
    ok &= albedo.Create(c, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kUav, L"offline.albedo");
    ok &= gbuf.Create(c, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kUav, L"offline.gbuffer");
    ok &= moments.Create(c, w, h, DXGI_FORMAT_R32G32_FLOAT, uav, kUav, L"offline.moments");
    ok &= denoiseA.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.denoiseA");
    ok &= denoiseB.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.denoiseB");
    ok &= bloomA.Create(c, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.bloomA");
    ok &= bloomB.Create(c, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.bloomB");
    ok &= ldr.Create(c, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, uav, kSrvAll, L"offline.ldr");
    // effect stage (shader packs): the lit HDR image and its ping-pong (UAV for CSLitCompose, RT for the effects), the
    // post ping-pong (RT), and the effect inputs; fxVel is 1x1 (zero motion, see EffectInputs)
    const D3D12_RESOURCE_FLAGS uavRt = (D3D12_RESOURCE_FLAGS)(uav | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ok &= litA.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uavRt, kSrvAll, L"offline.litA");
    ok &= litB.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uavRt, kSrvAll, L"offline.litB");
    ok &= ldrB.Create(c, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrvAll,
                      L"offline.ldrB");
    ok &= fxDepth.Create(c, w, h, DXGI_FORMAT_R32_FLOAT, uav, kSrvAll, L"offline.fxDepth");
    ok &= fxNormal.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.fxNormal");
    ok &= fxVel.Create(c, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.fxVel");
    if (volMarch) {
        ok &= volA.Create(c, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.volA");
        ok &= volB.Create(c, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.volB");
    }
    ok &= edgeLayer.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kSrvAll,
                           L"offline.edgeLayer");
    ok &= edgeAccum.Create(c, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kUav, L"offline.edgeAccum");
    prepassFine = std::max(2u, (uint32_t)std::lround(h / 540.0));
    for (uint32_t l = 0; l < kOfflinePrepassLevels; ++l) {
        const uint32_t st = prepassFine << (kOfflinePrepassLevels - 1 - l);
        const uint32_t lw = (w + st - 1) / st, lh = (h + st - 1) / st;
        ok &= preE[l].Create(c, lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.prepass.E");
        ok &= preG[l].Create(c, lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.prepass.G");
    }
    ok &= icFinal.Create(c, preE[kOfflinePrepassLevels - 1].width, preE[kOfflinePrepassLevels - 1].height,
                         DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrvAll, L"offline.icFinal");
    ok &= edgeColorMsaa.Create(c, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               kRt, L"offline.edge.color", 4, 1, zero);
    ok &= edgeDepthMsaa.Create(c, w, h, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE, L"offline.edge.depth", 4);
    if (!ok) {
        LOG_ERROR("offline: target creation failed (%ux%u)", w, h);
        width = height = 0;
        return false;
    }
    width = w;
    height = h;
    targets.width = w;
    targets.height = h;
    targets.outWidth = w;
    targets.outHeight = h;
    return true;
}

void OfflineRenderer::Impl::PreparePackEdges() {
    const ShaderPackRegistry& reg = ShaderPacks();
    if (reg.Generation() != packEdgeGeneration) {   // reload: Begin runs with the GPU idle, the old PSOs are free
        packEdgePsos.clear();
        packEdgeGeneration = reg.Generation();
    }
    for (GpuModel* model : view.models) {
        if (!model || model->ShaderPackId().empty() || packEdgePsos.count(model->ShaderPackId())) continue;
        const std::string& id = model->ShaderPackId();
        ComPtr<ID3D12PipelineState>& pso = packEdgePsos[id];   // stays null on failure: default edges
        const ShaderPack* pack = reg.Find(id);
        if (!pack || !pack->Selectable() || !pack->hasEdge) continue;
        // the surface as an include path relative to the shader directory (as ScenePass::PackPsos)
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(pack->dir / L"surface.hlsl", shaderDir, ec);
        if (ec || rel.empty()) rel = pack->dir / L"surface.hlsl";
        std::string inc = PathToUtf8(rel);
        std::replace(inc.begin(), inc.end(), '\\', '/');
        const uint32_t texCount = (uint32_t)std::min<size_t>(pack->textures.size(), kPackMaxTextures);
        uint32_t clampMask = 0, srgbMask = 0;
        for (uint32_t i = 0; i < texCount; ++i) {
            if (pack->textures[i].clamp) clampMask |= 1u << i;
            if (pack->textures[i].srgb) srgbMask |= 1u << i;
        }
        const ShaderDefines defines = {{"MMDX_PACK", "\"" + inc + "\""},
                                       {"PACK_TEX_COUNT", std::to_string(texCount)},
                                       {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
                                       {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"}};
        const std::filesystem::path file = shaderDir / L"offline_edge_pack.hlsl";
        std::string errors;
        ComPtr<ID3DBlob> vs = CompileShaderDxc(file, "VSEdgeOfflinePack", "vs_6_0", defines, &errors);
        ComPtr<ID3DBlob> ps = vs ? CompileShaderDxc(file, "PSEdgeOfflinePack", "ps_6_0", defines, &errors) : nullptr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = edgeDesc;
        if (vs && ps) {
            d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            if (FAILED(ctx->Device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) pso.Reset();
        }
        if (pso) LOG_INFO("offline: pack '%s' outlines (PackEdge)", id.c_str());
        else LOG_ERROR("offline: pack '%s' outline variant failed, using the default outlines", id.c_str());
    }
}

void OfflineRenderer::Impl::DrawEdges(ID3D12GraphicsCommandList* cmd, RtScene& rt, float shutter, float lensX,
                                      float lensY) {
    Dx12Context& c = *ctx;
    edgeColorMsaa.Transition(cmd, kRt);
    edgeDepthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.RtvHeap().Cpu(edgeColorMsaa.rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = c.DsvHeap().Cpu(edgeDepthMsaa.dsv);
    const float zero[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp{0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT sc{0, 0, (LONG)width, (LONG)height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetGraphicsRootSignature(edgeRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, sceneCb->GetGPUVirtualAddress());
    const float edgeCb[4] = {shutter, lensX, lensY, lensRadius > 0.0f ? focus : 0.0f};
    cmd->SetGraphicsRoot32BitConstants(4, 4, edgeCb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (int p = 0; p < 2; ++p) {  // pass 0: depth pre-pass, pass 1: edges
        for (GpuModel* model : view.models) {
            if (!model) continue;
            // pass 1: the model's pack outlines (PackEdge) when its pack draws them, else the default edges
            ID3D12PipelineState* edge = edgePso.Get();
            if (p == 1 && !model->ShaderPackId().empty()) {
                const auto it = packEdgePsos.find(model->ShaderPackId());
                const ShaderPack* pack = ShaderPacks().Find(model->ShaderPackId());
                if (it != packEdgePsos.end() && it->second && pack) {
                    bool texturesOk = true;
                    if (!pack->textures.empty()) {   // pack_api.hlsli gPackTex (t0, space5)
                        PackTextures* pt = rt.GetPackTextures();
                        const PackTextures::Set* set = pt ? pt->Acquire(c, *pack, model->ShaderTextureFolder()) : nullptr;
                        texturesOk = set && set->srv != DescriptorHeap::kInvalid;
                        if (texturesOk) cmd->SetGraphicsRootDescriptorTable(6, c.SrvHeap().Gpu(set->srv));
                    }
                    if (texturesOk) edge = it->second.Get();
                }
            }
            D3D12_VERTEX_BUFFER_VIEW vbs[4] = {model->VertexBufferView(), model->MorphBufferView(frame),
                                               model->PrevMorphBufferView(frame), model->SdefBufferView()};
            cmd->IASetVertexBuffers(0, 4, vbs);
            cmd->IASetIndexBuffer(&model->IndexBufferView());
            cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(frame));
            cmd->SetGraphicsRootShaderResourceView(5, model->PrevBoneBuffer(frame));
            for (const GpuModel::Material& mat : model->Materials()) {
                if (p == 0) {
                    if (mat.indexCount == 0 || !mat.visible) continue;
                    cmd->SetPipelineState(mat.doubleSided ? depthNoCull.Get() : depthCullBack.Get());
                } else {
                    if (!mat.drawEdge || mat.indexCount == 0) continue;
                    cmd->SetPipelineState(edge);
                }
                cmd->SetGraphicsRootConstantBufferView(1, mat.constants);
                cmd->SetGraphicsRootDescriptorTable(3, c.SrvHeap().Gpu(mat.srvTable));
                cmd->DrawIndexedInstanced(mat.indexCount, 1, mat.indexStart, 0, 0);
            }
        }
    }
    edgeColorMsaa.Transition(cmd, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    edgeLayer.Transition(cmd, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    cmd->ResolveSubresource(edgeLayer.res.Get(), 0, edgeColorMsaa.res.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    edgeLayer.Transition(cmd, kSrvAll);
}

void OfflineRenderer::Impl::Work(ID3D12GraphicsCommandList* cmd, TransientDescriptors& t, const BuiltinTextures* b,
                                 RtScene& rt, OfflineProgress& pg) {
    if (pg.phase == OfflinePhase::Idle || pg.phase == OfflinePhase::Done) return;
    const uint32_t slot = ctx->FrameSlot();

    // a) results of the frame that used this slot kFramesInFlight frames ago (BeginFrame already waited for it)
    if (slots[slot].serial == serial) {
        if (slots[slot].timed && slots[slot].items > 0 && timestampFrequency) {
            D3D12_RANGE range{slot * 16, slot * 16 + 16};
            void* mapped = nullptr;
            if (SUCCEEDED(timestampReadback->Map(0, &range, &mapped))) {
                const uint64_t* data = (const uint64_t*)mapped + slot * 2;
                if (data[1] > data[0]) {
                    const float ms = (float)((double)(data[1] - data[0]) * 1000.0 / (double)timestampFrequency);
                    const float perItem = ms / (float)slots[slot].items;
                    itemsPerFrame = std::clamp(90.0f / std::max(perItem, 0.01f), 1.0f, 64.0f);
                }
                D3D12_RANGE none{0, 0};
                timestampReadback->Unmap(0, &none);
            }
        }
        if (slots[slot].counted) {
            D3D12_RANGE range{slot * 512, slot * 512 + sizeof(uint32_t)};
            void* mapped = nullptr;
            if (SUCCEEDED(counterReadback->Map(0, &range, &mapped))) {
                const uint32_t active = *(const uint32_t*)((const uint8_t*)mapped + slot * 512);
                lastActive = active;
                pg.activeFraction = width && height ? (float)active / (float)(width * height) : 0.0f;
                D3D12_RANGE none{0, 0};
                counterReadback->Unmap(0, &none);
            }
        }
    }
    slots[slot] = {serial, false, 0, true, 0};
    cmd->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);

    PassContext pc = Pc(cmd, t, b, rt);
    D3D12_GPU_DESCRIPTOR_HANDLE table = GiTable(cmd, t);  // allocated once, reused by every gi dispatch
    // outline accumulation tables (t5 = this iteration's layer, u0 = accumulation), also once per call
    D3D12_GPU_DESCRIPTOR_HANDLE edgeSrv = t.SrvTable(*ctx, {nullptr, nullptr, nullptr, nullptr, nullptr, &edgeLayer});
    D3D12_GPU_DESCRIPTOR_HANDLE edgeUav = t.UavTable(*ctx, {&edgeAccum});
    const float studioFloor = view.studioFloor ? 1.0f : 0.0f;
    if (clearPending) {   // the prepass display wrote accum
        const float c[8] = {NextSeed(), studioFloor, 0.0f, (float)job.minSamples, (float)width, (float)height,
                            0.0f, job.errorThreshold};
        clearImage.Dispatch(pc, {}, table, c, 8, Groups(width), Groups(height));
        UavBarrier(cmd);
        clearPending = false;
    }
    if (pg.phase == OfflinePhase::Prepass) {
        Prepass(pc, pg);
        cmd->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        cmd->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                              timestampReadback.Get(), slot * 16);
        pg.fraction = 0.0f;
        return;
    }
    const uint32_t edgeTarget = (motion || lensRadius > 0.0f) ? kEdgeIterations : 1u;
    // the irradiance cache (finest level geometry t6, smoothed cache t7) for the render's indirect diffuse
    D3D12_GPU_DESCRIPTOR_HANDLE icSrv = {};
    if (icValid)
        icSrv = t.SrvTable(*ctx, {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                  &preG[kOfflinePrepassLevels - 1], &icFinal});
    // Progressive start: 1, 2, 3 ... samples per frame, so the preview visibly refines from noise.
    const uint32_t items = std::min(std::max(1u, (uint32_t)itemsPerFrame), 1u + samples / 4u);

    for (uint32_t i = 0; i < items; ++i) {
        if (samples >= job.maxSamples || lastActive == 0) break;
        const uint32_t s = samples;
        // shutter time inside the second half of the frame interval (180 degree shutter), golden-ratio
        // sequence over the iterations; geometry and camera are rebuilt at that time
        float shutter = 1.0f;
        if (motion) {
            const float r1 = 0.5f + 0.6180339887f * (float)s;
            shutter = 0.5f + 0.5f * (r1 - std::floor(r1));
            rt.Build(cmd, view.models, frame, ctx->FrameSlot(), shutter);
        }
        if (edgeLayers < edgeTarget) {
            float lx = 0.0f, ly = 0.0f;
            if (lensRadius > 0.0f) {
                ConcentricDisk(Halton(s + 1, 2), Halton(s + 1, 3), lx, ly);
                lx *= lensRadius;
                ly *= lensRadius;
            }
            DrawEdges(cmd, rt, shutter, lx, ly);
            edgeAccum.Transition(cmd, kUav);
            const float c[8] = {0, 0, 0, 0, (float)width, (float)height, 0, 0};
            edgeAccumulate.Dispatch(pc, edgeSrv, edgeUav, c, 8, Groups(width), Groups(height));
            UavBarrier(cmd);
            ++edgeLayers;
        }
        const bool count = (i == items - 1);   // count the last iteration of the frame
        if (count) {
            const float c[8] = {NextSeed(), studioFloor, 0.0f, (float)job.minSamples, (float)width,
                                (float)height, 0.0f, job.errorThreshold};
            clearCounter.Dispatch(pc, {}, table, c, 8, 1, 1);
            UavBarrier(cmd);
        }
        const float c[16] = {NextSeed(), studioFloor, (float)s, (float)job.minSamples,
                             (float)width, (float)height, 0.0f, job.errorThreshold,
                             shutter, focus, lensRadius, icValid ? 1.0f : 0.0f,
                             (float)job.maxBounces, (float)prepassFine, 0.0f, 0.0f};
        render->Dispatch(pc, icSrv, table, c, 16, Groups(width), Groups(height));
        UavBarrier(cmd);
        ++samples;
        if (count) {
            counter.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            fp.Offset = slot * 512;
            fp.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, 256};
            CD3DX12_TEXTURE_COPY_LOCATION dst(counterReadback.Get(), fp), src(counter.res.Get(), 0);
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            counter.Transition(cmd, kUav);
            slots[slot].counted = true;
            slots[slot].countedSample = s;
        }
        ++slots[slot].items;
    }

    // b) finish or preview
    if (samples >= job.maxSamples || lastActive == 0) {
        Finish(pc);
        pg.phase = OfflinePhase::Done;
    } else {
        const float c[8] = {0.0f, 0.0f, 0.08f, 1.0f, (float)width, (float)height, 0.12f, (float)edgeLayers};
        DispatchPost(pc, finalize, &ldr, nullptr, nullptr, c, Groups(width), Groups(height));
    }
    cmd->EndQuery(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    cmd->ResolveQueryData(timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                          timestampReadback.Get(), slot * 16);

    pg.samples = samples;
    if (pg.phase == OfflinePhase::Render) {
        const float f = std::max((float)samples / (float)job.maxSamples,
                                 samples >= job.minSamples ? 1.0f - pg.activeFraction : 0.0f);
        pg.fraction = std::clamp(f, 0.0f, 0.99f);
    } else {
        pg.fraction = 1.0f;
    }
}

// One prepass level: adaptive irradiance samples, then the cache-lit display (written into accum
// as one sample) and the preview. The last level hands over to the render (accum cleared first).
void OfflineRenderer::Impl::Prepass(PassContext& pc, OfflineProgress& pg) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t k = pg.prepassStep;
    const uint32_t stride = prepassFine << (kOfflinePrepassLevels - 1 - k);
    const float studioFloor = view.studioFloor ? 1.0f : 0.0f;
    Texture* E[kOfflinePrepassLevels];
    for (uint32_t l = 0; l < kOfflinePrepassLevels; ++l) E[l] = &preE[l];

    for (uint32_t l = 0; l < kOfflinePrepassLevels; ++l)
        if (l != k) preE[l].Transition(cmd, kSrvAll);
    if (k > 0) preG[k - 1].Transition(cmd, kSrvAll);
    preE[k].Transition(cmd, kUav);
    preG[k].Transition(cmd, kUav);
    D3D12_GPU_DESCRIPTOR_HANDLE uav = pc.transient.UavTable(
        *ctx, {&accum, &albedo, &moments, &gbuf, &counter, &edgeAccum, &preE[k], &preG[k]});
    D3D12_GPU_DESCRIPTOR_HANDLE srv = pc.transient.SrvTable(
        *ctx, {E[0], E[1], E[2], E[3], E[4], E[5], k > 0 ? &preG[k - 1] : nullptr});
    const float c[16] = {NextSeed(), studioFloor, (float)k, (float)stride,
                         (float)width, (float)height, (float)(stride * 2), (float)job.prepassRays,
                         1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    prepassLevel.Dispatch(pc, srv, uav, c, 16, Groups(preE[k].width), Groups(preE[k].height));
    UavBarrier(cmd);

    preE[k].Transition(cmd, kSrvAll);
    preG[k].Transition(cmd, kSrvAll);
    D3D12_GPU_DESCRIPTOR_HANDLE uav2 =
        pc.transient.UavTable(*ctx, {&accum, &albedo, &moments, &gbuf, &counter, &edgeAccum});
    D3D12_GPU_DESCRIPTOR_HANDLE srv2 =
        pc.transient.SrvTable(*ctx, {E[0], E[1], E[2], E[3], E[4], E[5], &preG[k]});
    const float dot = std::max(1.0f, (float)height / 1080.0f * 1.3f);
    const float c2[16] = {NextSeed(), studioFloor, (float)k, (float)stride,
                          (float)width, (float)height, 0.0f, 0.0f,
                          1.0f, 0.0f, 0.0f, 0.0f, dot, 0.0f, 0.0f, 0.0f};
    prepassDisplay.Dispatch(pc, srv2, uav2, c2, 16, Groups(width), Groups(height));
    UavBarrier(cmd);

    const float fin[8] = {0.0f, 0.0f, 0.08f, 1.0f, (float)width, (float)height, 0.12f, 0.0f};
    DispatchPost(pc, finalize, &ldr, nullptr, nullptr, fin, Groups(width), Groups(height));

    if (++pg.prepassStep >= kOfflinePrepassLevels) {
        // edge-aware smoothing of the finest level -> the cache the render reads
        Texture& fineE = preE[kOfflinePrepassLevels - 1];
        Texture& fineG = preG[kOfflinePrepassLevels - 1];
        icFinal.Transition(cmd, kUav);
        D3D12_GPU_DESCRIPTOR_HANDLE uav3 = pc.transient.UavTable(
            *ctx, {&accum, &albedo, &moments, &gbuf, &counter, &edgeAccum, &icFinal});
        D3D12_GPU_DESCRIPTOR_HANDLE srv3 =
            pc.transient.SrvTable(*ctx, {E[0], E[1], E[2], E[3], E[4], E[5], &fineG});
        prepassSmooth.Dispatch(pc, srv3, uav3, c2, 16, Groups(fineE.width), Groups(fineE.height));
        UavBarrier(cmd);
        icFinal.Transition(cmd, kSrvAll);
        icValid = true;
        pg.phase = OfflinePhase::Render;
        clearPending = true;
    }
}

// Sun shafts and spotlight beams: half-resolution ray march (offline_volumetric.hlsl) + depth-aware blur;
// CSFinalize composites image * transmittance + in-scattered light (volReady).
void OfflineRenderer::Impl::Volumetric(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    gbuf.Transition(cmd, kSrvAll);
    volA.Transition(cmd, kUav);
    const float c0[16] = {0.005f * job.volumetricDensity, 0.02f, 400.0f, 0.55f,
                          (float)width, (float)height, 3.0f, 0.3f,
                          (float)(serial & 0xFFFFu), 0.25f, 0.45f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    volMarch.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gbuf}), pc.transient.UavTable(*ctx, {&volA}), c0, 16,
                      Groups(volA.width), Groups(volA.height));
    UavBarrier(cmd);
    volA.Transition(cmd, kSrvAll);
    volB.Transition(cmd, kUav);
    const float ch[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    volBlur.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gbuf, &volA}), pc.transient.UavTable(*ctx, {&volB}), ch, 8,
                     Groups(volB.width), Groups(volB.height));
    UavBarrier(cmd);
    volB.Transition(cmd, kSrvAll);
    volA.Transition(cmd, kUav);
    const float cv[8] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    volBlur.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gbuf, &volB}), pc.transient.UavTable(*ctx, {&volA}), cv, 8,
                     Groups(volA.width), Groups(volA.height));
    UavBarrier(cmd);
    volReady = true;
}

// FFT convolution bloom: the thresholded quarter-resolution image (bloomA) goes into a kOfflineFftSize^2
// grid, is multiplied by the spectrum of a starburst kernel and the convolved result replaces bloomA.
void OfflineRenderer::Impl::ConvolveBloom(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t n = kOfflineFftSize;
    const float zero[4] = {0, 0, 0, 0};
    if (!fftKernelReady) {   // kernel spectrum, once
        const float kernelC[8] = {0.003f, 36.0f, 0.03f, 64.0f, 0.6f, 3.0f, 0.5236f, 0.9f};
        gridA.Transition(cmd, kUav);
        fftKernel.Dispatch(pc, {}, pc.transient.UavTable(*ctx, {&gridA}), kernelC, 8, Groups(n), Groups(n));
        UavBarrier(cmd);
        gridA.Transition(cmd, kSrvAll);
        gridB.Transition(cmd, kUav);
        fftRows.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridA}), pc.transient.UavTable(*ctx, {&gridB}), zero, 4, 1, n);
        UavBarrier(cmd);
        gridB.Transition(cmd, kSrvAll);
        kernelSpec.Transition(cmd, kUav);
        fftCols.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridB}), pc.transient.UavTable(*ctx, {&kernelSpec}), zero, 4, 1, n);
        UavBarrier(cmd);
        kernelSpec.Transition(cmd, kSrvAll);
        fftKernelReady = true;
    }
    const float aspect = (float)width / (float)height;
    float cw, ch;
    if (aspect >= 1.0f) {
        cw = 0.75f * n;
        ch = cw / aspect;
    } else {
        ch = 0.75f * n;
        cw = ch * aspect;
    }
    const float ox = (n - cw) * 0.5f, oy = (n - ch) * 0.5f;

    bloomA.Transition(cmd, kSrvAll);
    gridA.Transition(cmd, kUav);
    const float inC[4] = {ox, oy, cw, ch};
    fftInput.Dispatch(pc, pc.transient.SrvTable(*ctx, {&bloomA}), pc.transient.UavTable(*ctx, {&gridA}), inC, 4,
                      Groups(n), Groups(n));
    UavBarrier(cmd);
    gridA.Transition(cmd, kSrvAll);
    gridB.Transition(cmd, kUav);
    fftRows.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridA}), pc.transient.UavTable(*ctx, {&gridB}), zero, 4, 1, n);
    UavBarrier(cmd);
    gridB.Transition(cmd, kSrvAll);
    gridA.Transition(cmd, kUav);
    const float convC[4] = {1, 0, 0, 0};
    fftCols.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridB, &kernelSpec}), pc.transient.UavTable(*ctx, {&gridA}),
                     convC, 4, 1, n);
    UavBarrier(cmd);
    gridA.Transition(cmd, kSrvAll);
    gridB.Transition(cmd, kUav);
    fftRows.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridA}), pc.transient.UavTable(*ctx, {&gridB}), convC, 4, 1, n);
    UavBarrier(cmd);
    gridB.Transition(cmd, kSrvAll);
    bloomA.Transition(cmd, kUav);
    const float outC[8] = {ox / n, oy / n, cw / n, ch / n, kConvolutionBloomGain, (float)bloomA.width,
                           (float)bloomA.height, 0.0f};
    fftOutput.Dispatch(pc, pc.transient.SrvTable(*ctx, {&gridB}), pc.transient.UavTable(*ctx, {&bloomA}), outC, 8,
                       Groups(bloomA.width), Groups(bloomA.height));
    UavBarrier(cmd);
}

void OfflineRenderer::Impl::Finish(PassContext& pc) {
    const uint32_t w = width, h = height;
    const uint32_t qw = bloomA.width, qh = bloomA.height;
    ID3D12GraphicsCommandList* cmd = pc.cmd;

    const float d0[8] = {1.0f, 0.0f, 0.0f, 0.0f, (float)w, (float)h, 0.0f, 0.0f};
    DispatchPost(pc, denoise, &denoiseA, nullptr, nullptr, d0, Groups(w), Groups(h));
    const float d1[8] = {2.0f, 1.0f, 0.0f, 0.0f, (float)w, (float)h, 0.0f, 0.0f};
    DispatchPost(pc, denoise, &denoiseB, &denoiseA, nullptr, d1, Groups(w), Groups(h));
    const float d2[8] = {4.0f, 2.0f, 0.0f, 0.0f, (float)w, (float)h, 0.0f, 0.0f};
    DispatchPost(pc, denoise, &denoiseA, &denoiseB, nullptr, d2, Groups(w), Groups(h));

    if (job.volumetric && volMarch && volA) Volumetric(pc);

    // Shader packs (job.effects; none = nothing below changes). The pre-bloom share runs on the lit HDR image, in the
    // real-time frame's order: albedo, volumetric light and outlines composed in (CSLitCompose). Bloom then reads the
    // effect's output and CSFinalize skips that composition (lit = 1). The post share runs on the finished sRGB image.
    const bool preFx = effectPre.Ready(*ctx, job.effects);
    const bool postFx = effectPost.Ready(*ctx, job.effects);
    const bool lit = preFx;
    if (preFx || postFx) EffectInputs(pc);
    if (preFx) {
        const float lc[8] = {0.0f, 0.0f, 0.0f, 0.0f, (float)w, (float)h, 0.0f, (float)edgeLayers};
        DispatchPost(pc, litCompose, &litA, &denoiseA, nullptr, lc, Groups(w), Groups(h));
        effectPre.RunOffline(pc, job.effects, litA, litB, fxDepth, fxVel, fxNormal, &job);
    }

    const bool convolve = job.bloomConvolution && fftRows && gridA;
    if (job.bloom) {
        const float bd[8] = {1.0f, lit ? 1.0f : 0.0f, 0.0f, 0.0f, (float)w, (float)h, 0.0f, 0.0f};
        DispatchPost(pc, bloomDown, &bloomA, lit ? &litA : nullptr, nullptr, bd, Groups(qw), Groups(qh));
        if (convolve) {
            ConvolveBloom(pc);
        } else {
            const float bh[8] = {1.0f, 0.0f, 0.0f, 0.0f, (float)qw, (float)qh, 0.0f, 0.0f};
            const float bv[8] = {0.0f, 1.0f, 0.0f, 0.0f, (float)qw, (float)qh, 0.0f, 0.0f};
            DispatchPost(pc, bloomBlur, &bloomB, nullptr, &bloomA, bh, Groups(qw), Groups(qh));
            DispatchPost(pc, bloomBlur, &bloomA, nullptr, &bloomB, bv, Groups(qw), Groups(qh));
            DispatchPost(pc, bloomBlur, &bloomB, nullptr, &bloomA, bh, Groups(qw), Groups(qh));
            DispatchPost(pc, bloomBlur, &bloomA, nullptr, &bloomB, bv, Groups(qw), Groups(qh));
        }
    }

    const float fin[8] = {1.0f, job.bloom ? 1.0f : 0.0f, convolve ? kConvolutionBloomIntensity : 0.08f, 1.0f,
                          (float)w, (float)h, 0.12f, (float)edgeLayers};
    DispatchPost(pc, finalize, &ldr, lit ? &litA : &denoiseA, &bloomA, fin, Groups(w), Groups(h), lit ? 1.0f : 0.0f);
    if (postFx) effectPost.RunOffline(pc, job.effects, ldr, ldrB, fxDepth, fxVel, fxNormal, &job);
}

OfflineRenderer::OfflineRenderer() = default;
OfflineRenderer::~OfflineRenderer() = default;

bool OfflineRenderer::Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    impl_ = std::make_unique<Impl>();
    impl_->ctx = &ctx;
    if (!RtPipelinesSupported(ctx)) return false;
    Impl& m = *impl_;

    m.shaderDir = shaderDir;
    const std::filesystem::path gi = shaderDir / L"offline_gi.hlsl";
    const std::filesystem::path post = shaderDir / L"offline_post.hlsl";
    bool ok = true;
    ok &= m.clearImage.Create(ctx, gi, "CSClearImage");
    ok &= m.clearCounter.Create(ctx, gi, "CSClearCounter");
    ok &= m.defaultRender.Create(ctx, gi, "CSRender");
    m.render = &m.defaultRender;
    ok &= m.prepassLevel.Create(ctx, gi, "CSPrepassLevel");
    ok &= m.prepassDisplay.Create(ctx, gi, "CSPrepassDisplay");
    ok &= m.prepassSmooth.Create(ctx, gi, "CSPrepassSmooth");
    ok &= m.denoise.Create(ctx, post, "CSDenoise");
    ok &= m.bloomDown.Create(ctx, post, "CSBloomDown");
    ok &= m.bloomBlur.Create(ctx, post, "CSBloomBlur");
    ok &= m.finalize.Create(ctx, post, "CSFinalize");
    ok &= m.edgeAccumulate.Create(ctx, post, "CSEdgeAccum");
    ok &= m.litCompose.Create(ctx, post, "CSLitCompose");
    ok &= m.effectNormal.Create(ctx, post, "CSEffectNormal");
    ok &= m.effectVelocity.Create(ctx, post, "CSEffectVelocity");
    ok &= m.effectDepth.Create(ctx, shaderDir / L"offline_effect.hlsl", "CSEffectDepth");
    // the packs' effect.hlsl is compiled lazily from the shader directory (as the real-time passes do)
    m.effectPre.CreatePipelines(ctx, shaderDir, 0);
    m.effectPost.CreatePipelines(ctx, shaderDir, 0);
    if (!ok) {
        LOG_ERROR("offline renderer: compute pipeline creation failed");
        return false;
    }
    // Optional effects: a failure only disables the effect.
    {
        const std::filesystem::path vol = shaderDir / L"offline_volumetric.hlsl";
        if (!(m.volMarch.Create(ctx, vol, "CSVolMarch") && m.volBlur.Create(ctx, vol, "CSVolBlur"))) {
            LOG_WARN("offline renderer: volumetric light unavailable");
            m.volMarch = {};
            m.volBlur = {};
        }
        const ShaderDefines defs = {{"FFT_N", std::to_string(kOfflineFftSize)}, {"FFT_LOG2", "9"}};
        const std::filesystem::path ff = shaderDir / L"bloom_fft.hlsl";
        bool fftOk = m.fftInput.Create(ctx, ff, "CSInput", defs) && m.fftRows.Create(ctx, ff, "CSFftRows", defs) &&
                     m.fftCols.Create(ctx, ff, "CSFftCols", defs) && m.fftKernel.Create(ctx, ff, "CSKernel", defs) &&
                     m.fftOutput.Create(ctx, ff, "CSFftOutput", defs);
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (fftOk) {
            fftOk = m.gridA.Create(ctx, kOfflineFftSize, kOfflineFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kSrvAll,
                                   L"offline.fftA") &&
                    m.gridB.Create(ctx, kOfflineFftSize, kOfflineFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT, uav, kSrvAll,
                                   L"offline.fftB") &&
                    m.kernelSpec.Create(ctx, kOfflineFftSize, kOfflineFftSize, DXGI_FORMAT_R32G32B32A32_FLOAT, uav,
                                        kSrvAll, L"offline.fftKernel");
        }
        if (!fftOk) {
            LOG_WARN("offline renderer: convolution bloom unavailable");
            m.fftInput = {};
            m.fftRows = {};
            m.fftCols = {};
            m.fftKernel = {};
            m.fftOutput = {};
        }
    }

    // Outline layer: raster inverted hull at the offline image size (4x MSAA).
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER params[7];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    params[2].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_DESCRIPTOR_RANGE table;
    table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1);
    params[3].InitAsDescriptorTable(1, &table, D3D12_SHADER_VISIBILITY_ALL);   // ALL: PackEdge may run in the VS
    params[4].InitAsConstants(4, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);        // EdgeCB b2
    params[5].InitAsShaderResourceView(4, 0, D3D12_SHADER_VISIBILITY_VERTEX);   // previous bones t4
    CD3DX12_DESCRIPTOR_RANGE packTable;   // pack outlines: pack_api.hlsli gPackTex (t0, space5)
    packTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPackMaxTextures, 0, 5);
    params[6].InitAsDescriptorTable(1, &packTable, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 8);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,   // gClamp (pack API)
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(7, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (!CreateRootSignature(device, rs, m.edgeRootSig, "OfflineRenderer: edge root signature")) return false;

    const std::filesystem::path edge = shaderDir / L"offline_edge.hlsl";
    ComPtr<ID3DBlob> vsDepth = CompileShader(edge, "VSDepth", "vs_5_1");
    ComPtr<ID3DBlob> psDepth = CompileShader(edge, "PSDepth", "ps_5_1");
    ComPtr<ID3DBlob> vsEdge = CompileShader(edge, "VSEdge", "vs_5_1");
    ComPtr<ID3DBlob> psEdge = CompileShader(edge, "PSEdge", "ps_5_1");
    if (!vsDepth || !psDepth || !vsEdge || !psEdge) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {kMmdLayout, (UINT)std::size(kMmdLayout)};
    pso.pRootSignature = m.edgeRootSig.Get();
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.MultisampleEnable = TRUE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 4;

    pso.VS = {vsDepth->GetBufferPointer(), vsDepth->GetBufferSize()};
    pso.PS = {psDepth->GetBufferPointer(), psDepth->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;  // depth only
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m.depthCullBack)),
                 "OfflineRenderer: PSO depth back"))
        return false;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m.depthNoCull)),
                 "OfflineRenderer: PSO depth no cull"))
        return false;

    pso.VS = {vsEdge->GetBufferPointer(), vsEdge->GetBufferSize()};
    pso.PS = {psEdge->GetBufferPointer(), psEdge->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    {
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m.edgePso)), "OfflineRenderer: PSO edge"))
        return false;
    m.edgeDesc = pso;   // pack outline PSOs (PreparePackEdges) replace VS / PS
    m.edgeDesc.VS = {};
    m.edgeDesc.PS = {};

    // Present (present.hlsl, same as PresentPass::CreatePipelines).
    CD3DX12_DESCRIPTOR_RANGE presentRange[1];
    presentRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_ROOT_PARAMETER presentParams[1];
    presentParams[0].InitAsDescriptorTable(1, presentRange, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC presentSampler[1];
    presentSampler[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                           D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC presentRs;
    presentRs.Init(1, presentParams, 1, presentSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    if (!CreateRootSignature(device, presentRs, m.presentRootSig, "OfflineRenderer: present root signature"))
        return false;

    ComPtr<ID3DBlob> vsPresent = CompileShader(shaderDir / L"present.hlsl", "VSFullscreen", "vs_5_1");
    ComPtr<ID3DBlob> psPresent = CompileShader(shaderDir / L"present.hlsl", "PSPresent", "ps_5_1");
    if (!vsPresent || !psPresent) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC presentPso{};
    presentPso.pRootSignature = m.presentRootSig.Get();
    presentPso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    presentPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    presentPso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    presentPso.DepthStencilState.DepthEnable = FALSE;
    presentPso.DepthStencilState.StencilEnable = FALSE;
    presentPso.SampleMask = UINT_MAX;
    presentPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    presentPso.NumRenderTargets = 1;
    presentPso.RTVFormats[0] = Dx12Context::kBackBufferFormat;
    presentPso.SampleDesc.Count = 1;
    presentPso.VS = {vsPresent->GetBufferPointer(), vsPresent->GetBufferSize()};
    presentPso.PS = {psPresent->GetBufferPointer(), psPresent->GetBufferSize()};
    if (!CheckHr(device->CreateGraphicsPipelineState(&presentPso, IID_PPV_ARGS(&m.presentPso)),
                 "OfflineRenderer: present PSO"))
        return false;

    // Per-image constants (persistently mapped; written only in Begin while the GPU is idle).
    m.sceneCb = CreateMappedUpload(device, kSceneCbSize, &m.sceneCbMapped, L"offline.sceneCb");
    m.lightBuf = CreateMappedUpload(device, kLightBytes, &m.lightMapped, L"offline.lights");
    if (!m.sceneCb || !m.lightBuf) return false;

    // Readbacks, one entry per frame slot.
    D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    CD3DX12_RESOURCE_DESC crDesc = CD3DX12_RESOURCE_DESC::Buffer((uint64_t)kSlots * 512);
    if (!CheckHr(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &crDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&m.counterReadback)),
                 "OfflineRenderer: counter readback"))
        return false;
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * kSlots;
    if (!CheckHr(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m.timestampHeap)), "OfflineRenderer: timestamp heap"))
        return false;
    CD3DX12_RESOURCE_DESC trDesc = CD3DX12_RESOURCE_DESC::Buffer((uint64_t)kSlots * 16);
    if (!CheckHr(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &trDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&m.timestampReadback)),
                 "OfflineRenderer: timestamp readback"))
        return false;
    ctx.Queue()->GetTimestampFrequency(&m.timestampFrequency);

    if (!m.counter.Create(ctx, 1, 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kUav,
                          L"offline.counter"))
        return false;
    return true;
}

void OfflineRenderer::Shutdown() {
    if (!impl_) return;
    Impl& m = *impl_;
    Dx12Context& ctx = *m.ctx;
    for (Texture& tex : m.preE) tex.Release(ctx);
    for (Texture& tex : m.preG) tex.Release(ctx);
    m.icFinal.Release(ctx);
    for (Texture* t : {&m.accum, &m.albedo, &m.moments, &m.gbuf, &m.edgeLayer, &m.edgeAccum, &m.denoiseA, &m.denoiseB, &m.ldr, &m.bloomA, &m.bloomB, &m.edgeColorMsaa,
                       &m.edgeDepthMsaa, &m.counter, &m.volA, &m.volB, &m.gridA, &m.gridB, &m.kernelSpec,
                       &m.litA, &m.litB, &m.ldrB, &m.fxDepth, &m.fxNormal, &m.fxVel})
        t->Release(ctx);
    if (m.sceneCb && m.sceneCbMapped) m.sceneCb->Unmap(0, nullptr);
    if (m.lightBuf && m.lightMapped) m.lightBuf->Unmap(0, nullptr);
    m.sceneCbMapped = m.lightMapped = nullptr;
    m.sceneCb.Reset();
    m.lightBuf.Reset();
    m.counterReadback.Reset();
    m.timestampReadback.Reset();
    m.timestampHeap.Reset();
    m.timestampFrequency = 0;
    m.edgeRootSig.Reset();
    m.depthCullBack.Reset();
    m.depthNoCull.Reset();
    m.edgePso.Reset();
    m.packEdgePsos.clear();
    m.presentRootSig.Reset();
    m.presentPso.Reset();
    m.list.Reset();
    m.alloc.Reset();
    m.ptVariants.Clear(m.ctx);
    m.render = &m.defaultRender;
    m.ctx = nullptr;
    impl_.reset();
    progress_ = {};
}

void OfflineRenderer::Begin(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient,
                            const BuiltinTextures* builtin, RtScene& rt, const FrameView& view,
                            const SceneConstants& sc, const GpuLight* lights, uint32_t lightCount,
                            const OfflineJobDesc& job, uint64_t frame) {
    if (!impl_) return;
    Impl& m = *impl_;

    // 1. job state
    m.view = view;
    m.job = job;
    m.job.maxSamples = std::max(1u, m.job.maxSamples);
    m.job.minSamples = std::min(m.job.minSamples, m.job.maxSamples);
    m.frame = frame;
    ++m.serial;
    m.dispatchSeed = 0;
    m.samples = 0;
    m.lastActive = UINT32_MAX;
    m.edgeLayers = 0;
    m.volReady = false;
    m.motion = view.motionBlur;
    m.focus = view.focusDistance;
    // an effect pack that replaces the depth of field (pack.json "replaces": ["dof"]) draws it on the image instead
    const bool lensDof = job.dof && !EffectStackReplacesDof(job.effects);
    const float aperture = lensDof ? job.dofAperture * std::max(0.0f, view.apertureScale) : 0.0f;
    m.lensRadius = view.focusDistance > 0.0f && aperture > 0.0f ? kLensScale * aperture * view.focusDistance : 0.0f;
    // itemsPerFrame carries over: consecutive images (video) cost about the same per dispatch
    for (Impl::Slot& s : m.slots) { s.counted = false; s.timed = false; }

    // 1b. PT pack pipeline selection
    m.render = m.ptVariants.Resolve(*m.ctx, m.shaderDir, m.view.models, m.defaultRender, "offline_gi.hlsl", "CSRender", "offline");
    m.PreparePackEdges();

    // 2. per-image constants
    memcpy(m.sceneCbMapped, &sc, sizeof(sc));
    memset(m.lightMapped, 0, kLightBytes);
    memcpy(m.lightMapped, lights, (size_t)std::min(lightCount, Renderer::kMaxPunctualLights) * sizeof(GpuLight));

    // 3. image targets
    const bool created = m.EnsureTargets(m.job.width, m.job.height);
    if (!m.ldr) return;

    // 6. clears
    PassContext pc = m.Pc(cmd, transient, builtin, rt);
    D3D12_GPU_DESCRIPTOR_HANDLE table = m.GiTable(cmd, transient);
    const float studioFloor = m.view.studioFloor ? 1.0f : 0.0f;
    {
        const float c[8] = {m.NextSeed(), studioFloor, 0.0f, (float)m.job.minSamples, (float)m.job.width,
                            (float)m.job.height, 0.0f, m.job.errorThreshold};
        m.clearImage.Dispatch(pc, {}, table, c, 8, Groups(m.job.width), Groups(m.job.height));
        m.UavBarrier(cmd);
    }

    // 7. preview (only when ldr was just created: otherwise the previous image stays until the first samples)
    if (created) {
        const float c[8] = {0.0f, 0.0f, 0.08f, 1.0f, (float)m.job.width, (float)m.job.height, 0.12f, 0.0f};
        m.DispatchPost(pc, m.finalize, &m.ldr, nullptr, nullptr, c, Groups(m.job.width), Groups(m.job.height));
    }

    // 8. progress
    progress_ = {};
    progress_.phase = m.job.prepass ? OfflinePhase::Prepass : OfflinePhase::Render;
    progress_.prepassSteps = m.job.prepass ? kOfflinePrepassLevels : 0;
    m.clearPending = false;
    m.icValid = false;
    progress_.width = m.job.width;
    progress_.height = m.job.height;
    progress_.minSamples = m.job.minSamples;
    progress_.maxSamples = m.job.maxSamples;
    progress_.fraction = 0.0f;

    // 9. this frame's first share of work
    m.Work(cmd, transient, builtin, rt, progress_);
}

void OfflineRenderer::Render(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient,
                             const BuiltinTextures* builtin, RtScene& rt) {
    if (!impl_ || progress_.phase == OfflinePhase::Idle || progress_.phase == OfflinePhase::Done) return;
    impl_->Work(cmd, transient, builtin, rt, progress_);
}

void OfflineRenderer::Present(ID3D12GraphicsCommandList* cmd, TransientDescriptors& transient, float rect[4]) {
    rect[0] = rect[1] = rect[2] = rect[3] = 0;
    if (!impl_) return;
    Impl& m = *impl_;
    Dx12Context& ctx = *m.ctx;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.BackBufferRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

    const float bw = (float)ctx.Width();
    const float bh = (float)ctx.Height();
    D3D12_VIEWPORT fullViewport{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    D3D12_RECT fullScissor{0, 0, (LONG)bw, (LONG)bh};
    if (!m.ldr) {  // nothing rendered yet: keep the clean black surface
        cmd->RSSetViewports(1, &fullViewport);
        cmd->RSSetScissorRects(1, &fullScissor);
        return;
    }
    Texture& ldr = m.ldr;
    const float s = std::min(bw / (float)ldr.width, bh / (float)ldr.height);
    const float vw = (float)ldr.width * s;
    const float vh = (float)ldr.height * s;
    rect[0] = (bw - vw) / 2.0f;
    rect[1] = (bh - vh) / 2.0f;
    rect[2] = vw;
    rect[3] = vh;
    D3D12_VIEWPORT viewport{rect[0], rect[1], vw, vh, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &fullScissor);

    ldr.Transition(cmd, kSrvAll);  // normally already in ALL_SHADER_RESOURCE
    cmd->SetGraphicsRootSignature(m.presentRootSig.Get());
    cmd->SetPipelineState(m.presentPso.Get());
    cmd->SetGraphicsRootDescriptorTable(0, transient.SrvTable(ctx, {&ldr}));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Contract: UI draws next with the full back buffer visible.
    cmd->RSSetViewports(1, &fullViewport);
    cmd->RSSetScissorRects(1, &fullScissor);
}

void OfflineRenderer::Cancel() { progress_ = {}; }

bool OfflineRenderer::ReadImage(ImageRGBA8& out) {
    if (!impl_ || progress_.phase != OfflinePhase::Done || !impl_->ldr) return false;
    Impl& m = *impl_;
    Dx12Context& ctx = *m.ctx;
    ID3D12Device* device = ctx.Device();
    ctx.WaitForGpu();
    if (!m.alloc) {
        if (!CheckHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m.alloc)),
                     "OfflineRenderer: allocator") ||
            !CheckHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.alloc.Get(), nullptr,
                                               IID_PPV_ARGS(&m.list)),
                     "OfflineRenderer: list"))
            return false;
        m.list->Close();
    }
    m.alloc->Reset();
    m.list->Reset(m.alloc.Get(), nullptr);

    D3D12_RESOURCE_DESC desc = m.ldr.res->GetDesc();
    UINT64 total = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> readback;
    if (!CheckHr(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&readback)),
                 "OfflineRenderer: readback"))
        return false;

    m.ldr.Transition(m.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), fp), src(m.ldr.res.Get(), 0);
    m.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    m.ldr.Transition(m.list.Get(), kSrvAll);
    m.list->Close();
    ID3D12CommandList* lists[] = {m.list.Get()};
    ctx.Queue()->ExecuteCommandLists(1, lists);
    ctx.WaitForGpu();

    const uint32_t w = m.ldr.width, h = m.ldr.height;
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, (SIZE_T)(fp.Footprint.RowPitch * h)};
    if (FAILED(readback->Map(0, &range, (void**)&data))) return false;
    out = ImageRGBA8{};
    out.mips.push_back({w, h, std::vector<uint8_t>((size_t)w * h * 4)});
    for (uint32_t y = 0; y < h; ++y)
        memcpy(out.mips[0].pixels.data() + (size_t)w * 4 * y, data + (size_t)fp.Footprint.RowPitch * y,
               (size_t)w * 4);
    out.hasAlpha = false;  // alpha is already 255 everywhere
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return true;
}

} // namespace mmdx
