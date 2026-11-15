// PassWorld.cpp: M3-W1 World pack simulation and rendering.
// Manages GPU simulation (particle buffers, field ping-pong textures) and rendering (surfaces, particles)
// for world packs across raster, ray-traced, path-traced, and offline GI render paths.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/RayTracing.h"
#include "render/Renderer.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "render/WorldTypes.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

namespace {

using namespace DirectX;

// A camera-space particle system's tile size: the fixed "wrap", or the "wrapParam" float param (the instance's value,
// else the pack default) times "wrapScale".
float ParticleWrap(const ShaderPack& pack, const PackWorldParticleSystem& p, const std::map<std::string, float>& params) {
    if (p.wrapParam.empty()) return p.wrap;
    float v = p.wrap / std::max(p.wrapScale, 0.01f);
    for (const ShaderPackParam& q : pack.params)
        if (q.key == p.wrapParam) v = q.def;
    if (auto it = params.find(p.wrapParam); it != params.end()) v = it->second;
    return std::clamp(v * p.wrapScale, 1.0f, 4096.0f);
}

// A hash of the instance's pack id + evaluated parameters: a changed param set re-simulates a still / paused frame.
uint64_t HashWorldParams(const WorldInstance& inst) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (unsigned char c : inst.pack) mix(c);
    for (const auto& kv : inst.params) {
        for (unsigned char c : kv.first) mix(c);
        uint32_t bits = 0;
        std::memcpy(&bits, &kv.second, sizeof(bits));
        mix(bits);
    }
    return h;
}

// The colliders a pack sees: body colliders (kColliderBody) only for packs that require "world.body", so the
// existing packs keep their old collider list (count and order) unchanged.
void SampleColliders(const WorldInstance& inst, const ShaderPack& pack, double t, std::vector<WorldCollider>& out) {
    out.clear();
    if (inst.colliders) inst.colliders->Sample(t, out);
    if (!pack.HasCap("world.body")) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const WorldCollider& c) { return (c.flags & kColliderBody) != 0; }),
                  out.end());
    }
}

struct GpuWorldColliderData {
    XMFLOAT3 pos{0, 0, 0};
    float radius = 0.0f;
    XMFLOAT3 vel{0, 0, 0};
    uint32_t flags = 0;
};
static_assert(sizeof(GpuWorldColliderData) == 32);

struct GpuPackParticle {
    XMFLOAT3 pos{0, 0, 0};
    float size = 0.0f;
    XMFLOAT3 vel{0, 0, 0};
    float age = -1.0f;
    XMFLOAT4 color{0, 0, 0, 0};
    XMFLOAT4 data{0, 0, 0, 0};
};
static_assert(sizeof(GpuPackParticle) == 64);

struct WorldCbData {
    XMFLOAT4X4 worldToLocal;
    XMFLOAT4X4 localToWorld;
    XMFLOAT4 worldTime;     // x: time, y: dt, z: simTime, w: stepIndex
    XMFLOAT4 worldParams;   // x: colliderCount, y: fieldCount, z: particleSystemIndex, w: countOrGrid
    XMFLOAT4 sunDir;        // xyz: sunDir, w: hasSun
    XMFLOAT4 sunColor;      // rgb: sunColor, w: shadowDistance
    XMFLOAT4 ambient;       // rgb: ambientColor, w: 0
    XMFLOAT4 fieldExtent[4];// xy: size, z: extent, w: 1/extent
    // x: the particle system's wrap size (0 = local space), y: 1 = camera space, z,w: the field anchor (local xz)
    XMFLOAT4 worldExtra;
    // x: the ground map's size in texels (0 = the flat plane), y: 1/size, z,w: the map's origin (world xz)
    XMFLOAT4 groundMap;
    // x: the map's half extent (world units), y: the map's top y, z: the y range (top - bottom), w: 1 = a ground exists
    XMFLOAT4 groundRect;
    // x: 0 = the field's extent, 1 = camera-centred, y: the camera radius (world units), z: 1 = above the ground
    XMFLOAT4 surfaceParams;
    // Offline GI: this iteration's view-projection (shutter-time camera + thin-lens shear, as the outline layer), so
    // world surfaces / particles take the renderer's DoF and motion blur; offlineParams.x = 1 when set.
    XMFLOAT4X4 offlineViewProj;
    XMFLOAT4 offlineParams;
    // Drawn particles (the fixed-step simulation is interpolated): x: the interpolation factor a = 1 - (simTime -
    // time) / stepDt, y: the frame delta (sceneElapsed, 0 when paused), z: stepDt, w: the teleport bound (0.5 x the
    // camera-space wrap size; 0 = no bound).
    XMFLOAT4 particleParams;
};
static_assert(sizeof(WorldCbData) == 432);   // matches WorldCB in world_api.hlsli

// The ground height map: a top-down orthographic depth render of the stage / prop models.
inline constexpr uint32_t kWorldGroundMapSize = 2048;
inline constexpr float kWorldGroundMaxExtent = 1024.0f;   // half size: at most 2048 x 2048 world units

DXGI_FORMAT ParseFieldDxgiFormat(PackWorldField::Format fmt) {
    switch (fmt) {
        case PackWorldField::Format::R16F: return DXGI_FORMAT_R16_FLOAT;
        case PackWorldField::Format::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case PackWorldField::Format::RGBA16F: default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

// The model's vertex layout (the same one the scene / shadow passes use): the ground map reuses the shadow pass's
// vertex shader, so it needs the same input layout.
const D3D12_INPUT_ELEMENT_DESC kWorldMmdLayout[] = {
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
    {"TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},   // GpuAddUv
    {"TEXCOORD", 9, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

// The model's vertex / index buffers (the same binding the scene and shadow passes use).
void BindGroundModelBuffers(ID3D12GraphicsCommandList* cmd, const GpuModel& model, uint64_t frame) {
    D3D12_VERTEX_BUFFER_VIEW vbs[5] = {model.VertexBufferView(), model.MorphBufferView(frame),
                                       model.PrevMorphBufferView(frame), model.SdefBufferView(),
                                       model.AddUvBufferView()};
    cmd->IASetVertexBuffers(0, 5, vbs);
    cmd->IASetIndexBuffer(&model.IndexBufferView());
}

} // namespace

struct WorldPass::Impl {
    std::filesystem::path shaderDir;
    uint32_t msaa = 1;

    // Root Signatures
    ComPtr<ID3D12RootSignature> particleComputeRootSig;
    ComPtr<ID3D12RootSignature> fieldComputeRootSig;
    ComPtr<ID3D12RootSignature> graphicsRootSig;
    // world.lights packs: the same signature plus the ray-query bindings (TLAS, geometry table, bindless heap, s4).
    // Non-lights packs keep graphicsRootSig, so their PSOs are exactly as before.
    ComPtr<ID3D12RootSignature> graphicsRootSigLights;

    // Default descriptors / fallbacks
    Texture defaultWhiteTex;
    uint32_t defaultPackTexSrv = DescriptorHeap::kInvalid; // 32 consecutive descriptors in SrvHeap
    Texture defaultBlackFieldTex;
    ComPtr<ID3D12Resource> defaultColliderBuffer;

    // Per-frame upload memory (UPLOAD heap, persistently mapped). A re-simulation records hundreds of steps in one
    // command list, each with its own constants / colliders, so a wrapping ring would overwrite steps the GPU has not
    // run yet: every frame slot owns a list of chunks that only grows, rewound when the slot comes round again.
    struct UploadChunk {
        ComPtr<ID3D12Resource> res;
        uint8_t* mapped = nullptr;
        size_t size = 0;
    };
    struct UploadSegment {
        std::vector<UploadChunk> chunks;
        size_t chunk = 0, offset = 0;
    };
    static constexpr uint32_t kUploadSegments = Dx12Context::kFramesInFlight + 1;
    static constexpr size_t kUploadChunkSize = 4u << 20;
    UploadSegment uploads[kUploadSegments];
    uint64_t uploadFrame = ~0ull;
    Dx12Context* ctxp = nullptr;
    static constexpr size_t kExtBlockSize = 128 * sizeof(XMFLOAT4);

    // Pack Textures
    PackTextures packTextures;

    // Pipeline cache per pack
    struct PackPipelines {
        std::string packId;
        uint32_t generation = 0;
        // world.lights: both the raster and the ray-query (RT / PT / offline GI) draw PSOs compiled for this pack.
        bool rtReady = false;

        ComPtr<ID3D12PipelineState> psoClear;
        struct ParticlePso {
            ComPtr<ID3D12PipelineState> psoSimulate;
            ComPtr<ID3D12PipelineState> psoSpawn;
            ComPtr<ID3D12PipelineState> psoDraw;
            ComPtr<ID3D12PipelineState> psoDrawPost;
            ComPtr<ID3D12PipelineState> psoDrawOffline;
            // world.lights in RT / PT / offline GI: the same draws with the ray-query shadow path (ps_6_5).
            ComPtr<ID3D12PipelineState> psoDrawRt;
            ComPtr<ID3D12PipelineState> psoDrawPostRt;
            ComPtr<ID3D12PipelineState> psoDrawOfflineRt;
        };
        std::vector<ParticlePso> particles;

        struct FieldPso {
            ComPtr<ID3D12PipelineState> psoUpdate;
        };
        std::vector<FieldPso> fields;

        struct SurfacePso {
            ComPtr<ID3D12PipelineState> psoDraw;
            ComPtr<ID3D12PipelineState> psoDrawOffline;
            ComPtr<ID3D12PipelineState> psoDrawOverlay;         // "blend": "overlay"
            ComPtr<ID3D12PipelineState> psoDrawOfflineOverlay;
            ComPtr<ID3D12PipelineState> psoDrawRt;               // world.lights: ray-query shadow path
            ComPtr<ID3D12PipelineState> psoDrawOfflineRt;
            ComPtr<ID3D12PipelineState> psoDrawRtOverlay;
            ComPtr<ID3D12PipelineState> psoDrawOfflineRtOverlay;
        };
        std::vector<SurfacePso> surfaces;
    };
    std::map<std::string, PackPipelines> packPipelines;

    // GPU state per instance
    struct InstanceState {
        uint32_t uid = 0;
        std::string packId;
        uint64_t colliderVersion = 0;
        uint64_t paramsHash = 0;   // pack id + evaluated params: a change re-simulates a paused / still frame
        double simTime = 0.0;      // always a grid point: simStep * stepDt
        uint64_t simStep = 0;      // the global grid step (from timeline 0) simTime stands at
        double lastTime = -1.0;    // the previous frame's target time (a param change on the same time re-simulates)
        bool initialized = false;
        // The field's anchor (world.anchor): the mean xz of the foot colliders over the whole collider track, in the
        // entity's local space. Constant for the instance, so the re-simulation stays deterministic.
        float anchorX = 0.0f, anchorZ = 0.0f;
        uint64_t anchorVersion = ~0ull;

        struct ParticleBuf {
            ComPtr<ID3D12Resource> buffer;  // RWStructuredBuffer<PackParticle>
            ComPtr<ID3D12Resource> prevPos; // RWStructuredBuffer<float4>
            uint32_t count = 0;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;   // both buffers move together
        };
        std::vector<ParticleBuf> particles;

        struct FieldTex {
            Texture texA;
            Texture texB;
            bool currentIsB = false;
            uint32_t size = 0;
            DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            bool persistent = false;
            float rate = 10.0f;
            double persistentSimTime = 0.0;

            Texture& Current() { return currentIsB ? texB : texA; }
            Texture& Previous() { return currentIsB ? texA : texB; }
            void Swap() { currentIsB = !currentIsB; }
        };
        std::vector<FieldTex> fields;
        // Persistent shader-visible descriptors (no per-step transient tables: a re-simulation records hundreds of
        // steps): per field i, [i*4 + 0/1] = SRV of texA / texB, [i*4 + 2/3] = UAV of texA / texB; then 16 tables of 4
        // SRVs at [16 + mask*4], bit i of mask = field i's latest result is texB (unused slots: a black texture).
        uint32_t fieldDesc = DescriptorHeap::kInvalid;
        static constexpr uint32_t kFieldDescCount = 16 + 16 * 4;
        uint32_t LatestMask() const {
            uint32_t m = 0;
            for (size_t i = 0; i < fields.size(); ++i)
                if (!fields[i].currentIsB) m |= 1u << i;   // Previous() = the latest result
            return m;
        }
    };
    D3D12_GPU_DESCRIPTOR_HANDLE FieldTable(Dx12Context& ctx, const InstanceState& st) const {
        return ctx.SrvHeap().Gpu(st.fieldDesc + 16 + st.LatestMask() * 4);
    }
    // The ground map's descriptor (t19 space0): the depth map, or the 1x1 fallback when there is no map.
    D3D12_GPU_DESCRIPTOR_HANDLE GroundTable(Dx12Context& ctx) const {
        if (groundSrv != DescriptorHeap::kInvalid) return ctx.SrvHeap().Gpu(groundSrv);
        if (groundFallbackSrv != DescriptorHeap::kInvalid) return ctx.SrvHeap().Gpu(groundFallbackSrv);
        return D3D12_GPU_DESCRIPTOR_HANDLE{};
    }
    // Moves a particle system's two buffers to `to` (UAV for the compute steps, SRV for drawing).
    static void TransitionParticles(ID3D12GraphicsCommandList* cmd, InstanceState::ParticleBuf& b,
                                    D3D12_RESOURCE_STATES to) {
        if (b.state == to) {
            if (to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER u[2] = {CD3DX12_RESOURCE_BARRIER::UAV(b.buffer.Get()),
                                               CD3DX12_RESOURCE_BARRIER::UAV(b.prevPos.Get())};
                cmd->ResourceBarrier(2, u);
            }
            return;
        }
        D3D12_RESOURCE_BARRIER t[2] = {CD3DX12_RESOURCE_BARRIER::Transition(b.buffer.Get(), b.state, to),
                                       CD3DX12_RESOURCE_BARRIER::Transition(b.prevPos.Get(), b.state, to)};
        cmd->ResourceBarrier(2, t);
        b.state = to;
    }
    std::map<std::pair<uint32_t, std::string>, InstanceState> instances;

    // The ground height map (world.ground): a top-down orthographic depth render of the stage / prop models, read by
    // the world shaders as R32_FLOAT. Only allocated when a pack requires the capability.
    Texture groundDepth;
    uint32_t groundSrv = DescriptorHeap::kInvalid;
    // DrawOffline's iteration camera (see WorldCbData::offlineViewProj)
    XMFLOAT4X4 offlineVp{};
    bool offlineVpSet = false;        // the depth map's SRV (t19 space0)
    Texture groundFallback;                                // 1x1 R32_FLOAT, bound without a map
    uint32_t groundFallbackSrv = DescriptorHeap::kInvalid;
    uint64_t groundVersion = ~0ull;                       // the models / pose the map was rendered from
    bool groundValid = false;
    XMFLOAT4 groundMapParams{0, 0, 0, 0};                 // x: the map size (texels), y: 1/size, z,w: the origin
    XMFLOAT4 groundRectParams{0, 0, 0, 0};                // x: the half extent, y: the top y, z: the y range, w: on
    // The ground render's pipeline: the shadow pass's VS (skinning) with an orthographic override matrix.
    ComPtr<ID3D12RootSignature> groundRootSig;
    ComPtr<ID3D12PipelineState> groundPso;

    ~Impl() {
        for (UploadSegment& seg : uploads)
            for (UploadChunk& c : seg.chunks)
                if (c.res && c.mapped) c.res->Unmap(0, nullptr);
    }

    bool Init(Dx12Context& ctx, const std::filesystem::path& dir, uint32_t msaaSamples);
    void EnsureRings(Dx12Context& ctx);
    void CreateDefaultResources(Dx12Context& ctx);
    PackPipelines* GetOrCompilePipelines(Dx12Context& ctx, const ShaderPack& pack);
    InstanceState& GetOrCreateInstanceState(Dx12Context& ctx, const WorldInstance& inst, const ShaderPack& pack);

    D3D12_GPU_VIRTUAL_ADDRESS Upload(const void* data, size_t bytes, size_t align);
    D3D12_GPU_VIRTUAL_ADDRESS AllocParamsExt(const ShaderPack& pack, const std::map<std::string, float>& params);
    D3D12_GPU_VIRTUAL_ADDRESS AllocColliders(const std::vector<WorldCollider>& colliders);
    D3D12_GPU_VIRTUAL_ADDRESS AllocWorldCb(const WorldCbData& cb);

    // The ground map (world.ground).
    void EnsureGroundResources(Dx12Context& ctx);
    void RenderGroundMap(PassContext& pc);
    void ReleaseGroundResources(Dx12Context& ctx);
    // Renders the ground map when a world pack needs it (a pack that requires the "world.ground" capability).
    void EnsureGround(PassContext& pc);
    // The field's anchor (world.anchor): the mean xz of the foot colliders over the whole collider track.
    void UpdateAnchor(const WorldInstance& inst, InstanceState& state);

    void SimulateInstance(PassContext& pc, const WorldInstance& inst, const ShaderPack& pack,
                          PackPipelines& pipes, InstanceState& state);
    void DrawInstance(PassContext& pc, const WorldInstance& inst, const ShaderPack& pack,
                      PackPipelines& pipes, InstanceState& state, bool offline, bool drawParticles = true);
    void DrawParticlesPost(PassContext& pc);
};

bool WorldPass::Impl::Init(Dx12Context& ctx, const std::filesystem::path& dir, uint32_t msaaSamples) {
    shaderDir = dir;
    msaa = msaaSamples;
    ID3D12Device* device = ctx.Device();

    // Static samplers: s0 Point, s1 Linear Clamp, s2 Linear Wrap, s3 Comparison Clamp
    CD3DX12_STATIC_SAMPLER_DESC samplers[4];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    samplers[3].Init(3, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     0.0f, 16, D3D12_COMPARISON_FUNC_LESS_EQUAL);

    // Common descriptor ranges
    CD3DX12_DESCRIPTOR_RANGE fieldRange;
    fieldRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPackWorldMaxFields, 0, 4); // t0..t3 space4

    CD3DX12_DESCRIPTOR_RANGE packTexRange;
    packTexRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPackMaxTexturesWorld, 0, 5); // t0..t31 space5

    // The ground height map (the host's top-down depth render of the stage / props), t19 space0.
    CD3DX12_DESCRIPTOR_RANGE groundRange;
    groundRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 19, 0);

    // 1. Particle Compute Root Signature
    {
        CD3DX12_ROOT_PARAMETER p[9];
        p[0].InitAsConstantBufferView(0);                 // b0 SceneConstants
        p[1].InitAsConstantBufferView(1);                 // b1 WorldCB
        p[2].InitAsConstantBufferView(4);                 // b4 PackParamsExt
        p[3].InitAsShaderResourceView(10, 0);             // t10 space0 Colliders
        p[4].InitAsDescriptorTable(1, &fieldRange);       // t0..t3 space4 Fields
        p[5].InitAsDescriptorTable(1, &packTexRange);     // t0..t31 space5 PackTex
        p[6].InitAsUnorderedAccessView(0, 0);             // u0 space0 gParticles
        p[7].InitAsUnorderedAccessView(1, 0);             // u1 space0 gPrevPos
        p[8].InitAsDescriptorTable(1, &groundRange);      // t19 space0 GroundMap

        CD3DX12_ROOT_SIGNATURE_DESC rs;
        rs.Init(9, p, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
            LOG_ERROR("WorldPass: failed to serialize particleComputeRootSig: %s",
                      err ? (const char*)err->GetBufferPointer() : "unknown");
            return false;
        }
        if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&particleComputeRootSig)),
                     "WorldPass: CreateRootSignature particleCompute"))
            return false;
    }

    // 2. Field Compute Root Signature
    {
        CD3DX12_DESCRIPTOR_RANGE prevRange;
        prevRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1); // t0 space1 gFieldPrev
        CD3DX12_DESCRIPTOR_RANGE nextRange;
        nextRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0); // u0 space0 gFieldNext

        CD3DX12_ROOT_PARAMETER p[9];
        p[0].InitAsConstantBufferView(0);                 // b0 SceneConstants
        p[1].InitAsConstantBufferView(1);                 // b1 WorldCB
        p[2].InitAsConstantBufferView(4);                 // b4 PackParamsExt
        p[3].InitAsShaderResourceView(10, 0);             // t10 space0 Colliders
        p[4].InitAsDescriptorTable(1, &fieldRange);       // t0..t3 space4 Fields
        p[5].InitAsDescriptorTable(1, &packTexRange);     // t0..t31 space5 PackTex
        p[6].InitAsDescriptorTable(1, &prevRange);        // t0 space1 FieldPrev
        p[7].InitAsDescriptorTable(1, &nextRange);        // u0 space0 FieldNext
        p[8].InitAsDescriptorTable(1, &groundRange);      // t19 space0 GroundMap

        CD3DX12_ROOT_SIGNATURE_DESC rs;
        rs.Init(9, p, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
            LOG_ERROR("WorldPass: failed to serialize fieldComputeRootSig: %s",
                      err ? (const char*)err->GetBufferPointer() : "unknown");
            return false;
        }
        if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&fieldComputeRootSig)),
                     "WorldPass: CreateRootSignature fieldCompute"))
            return false;
    }

    // 3. Graphics Root Signatures: the common one, and (world.lights) one with the ray-query bindings.
    {
        CD3DX12_DESCRIPTOR_RANGE shadowRange;
        shadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 15, 0); // t15 space0 ShadowMap

        CD3DX12_DESCRIPTOR_RANGE sceneDepthRange;
        sceneDepthRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 6); // t0 space6 SceneDepth (post particles)

        // The scene's punctual lights for the world surfaces: the light buffer (root SRV), the spot / point
        // shadow maps and the binding flags (a root constant: bit0 lights, bit1 spot map, bit2 point map).
        CD3DX12_DESCRIPTOR_RANGE spotShadowRange;
        spotShadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 17, 0); // t17 space0 SpotShadowMap

        CD3DX12_DESCRIPTOR_RANGE pointShadowRange;
        pointShadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 18, 0); // t18 space0 PointShadowMap

        // world.lights ray-query path: the TLAS, its geometry table, the bindless heap segments (the world registers
        // of rt_common.hlsli RT_COMMON_ALT_SPACES: t20 / t21 space0, t0 space7 / space8).
        CD3DX12_DESCRIPTOR_RANGE texAll, bufAll;
        texAll.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 7, 0);
        bufAll.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 8, 0);

        auto createGraphicsRootSig = [&](bool lights, ComPtr<ID3D12RootSignature>& out) -> bool {
            CD3DX12_ROOT_PARAMETER p[19];
            p[0].InitAsConstantBufferView(0);                 // b0 SceneConstants
            p[1].InitAsConstantBufferView(1);                 // b1 WorldCB
            p[2].InitAsConstantBufferView(4);                 // b4 PackParamsExt
            p[3].InitAsShaderResourceView(10, 0);             // t10 space0 Colliders
            p[4].InitAsDescriptorTable(1, &fieldRange);       // t0..t3 space4 Fields
            p[5].InitAsDescriptorTable(1, &packTexRange);     // t0..t31 space5 PackTex
            p[6].InitAsShaderResourceView(0, 2);              // t0 space2 gParticles
            p[7].InitAsShaderResourceView(1, 2);              // t1 space2 gPrevPos
            p[8].InitAsDescriptorTable(1, &shadowRange);      // t15 space0 ShadowMap
            p[9].InitAsDescriptorTable(1, &sceneDepthRange);  // t0 space6 SceneDepth
            p[10].InitAsShaderResourceView(16, 0, D3D12_SHADER_VISIBILITY_PIXEL);   // t16 space0 Lights
            p[11].InitAsDescriptorTable(1, &spotShadowRange, D3D12_SHADER_VISIBILITY_PIXEL);   // t17 space0
            p[12].InitAsDescriptorTable(1, &pointShadowRange, D3D12_SHADER_VISIBILITY_PIXEL); // t18 space0
            p[13].InitAsConstants(1, 5, 0, D3D12_SHADER_VISIBILITY_PIXEL); // b5 world light flags
            p[14].InitAsDescriptorTable(1, &groundRange);     // t19 space0 GroundMap
            if (lights) {
                p[15].InitAsShaderResourceView(20, 0, D3D12_SHADER_VISIBILITY_PIXEL);   // t20 space0 TLAS
                p[16].InitAsShaderResourceView(21, 0, D3D12_SHADER_VISIBILITY_PIXEL);   // t21 space0 RtGeometry[]
                p[17].InitAsDescriptorTable(1, &texAll, D3D12_SHADER_VISIBILITY_PIXEL);   // t0 space7
                p[18].InitAsDescriptorTable(1, &bufAll, D3D12_SHADER_VISIBILITY_PIXEL);   // t0 space8
            }

            CD3DX12_STATIC_SAMPLER_DESC rsSamplers[5];
            for (int i = 0; i < 4; ++i) rsSamplers[i] = samplers[i];
            if (lights) {
                // rt_common.hlsli's gRtWrap (bindless texture sampling in the shadow ray's alpha test).
                rsSamplers[4].Init(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                   D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 16,
                                   D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0.0f,
                                   D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
            }

            CD3DX12_ROOT_SIGNATURE_DESC rs;
            rs.Init(lights ? 19 : 15, p, lights ? 5 : 4, rsSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
                LOG_ERROR("WorldPass: failed to serialize the graphics root signature: %s",
                          err ? (const char*)err->GetBufferPointer() : "unknown");
                return false;
            }
            return CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&out)),
                           "WorldPass: CreateRootSignature graphics");
        };
        if (!createGraphicsRootSig(false, graphicsRootSig)) return false;
        if (!createGraphicsRootSig(true, graphicsRootSigLights)) return false;
    }

    // 4. The ground map's pipeline: the shadow pass's VS (mmd.hlsl's VSShadow, with the skinning) drawn with an
    // orthographic override matrix into a depth-only target (the stage / prop models, back faces culled).
    {
        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        CD3DX12_DESCRIPTOR_RANGE table;
        table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1);
        params[3].InitAsDescriptorTable(1, &table, D3D12_SHADER_VISIBILITY_PIXEL);
        params[4].InitAsConstants(20, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        CD3DX12_ROOT_SIGNATURE_DESC rs;
        rs.Init(5, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
            LOG_ERROR("WorldPass: failed to serialize the ground root signature: %s",
                      err ? (const char*)err->GetBufferPointer() : "unknown");
        } else if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        IID_PPV_ARGS(&groundRootSig)),
                            "WorldPass: CreateRootSignature ground")) {
            groundRootSig.Reset();
        }

        std::string shaderErr;
        ComPtr<ID3DBlob> vs = CompileShaderDxc(shaderDir / L"mmd.hlsl", "VSShadow", "vs_6_0", {}, &shaderErr);
        ComPtr<ID3DBlob> ps = vs ? CompileShaderDxc(shaderDir / L"mmd.hlsl", "PSShadowAlpha", "ps_6_0", {}, &shaderErr)
                                 : nullptr;
        if (!vs || !ps) {
            LOG_ERROR("WorldPass: the ground map's shaders failed: %s", shaderErr.c_str());
        } else {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.InputLayout = {kWorldMmdLayout, (UINT)std::size(kWorldMmdLayout)};
            pso.pRootSignature = groundRootSig.Get();
            pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;   // a sky dome (inward faces) is not a roof
            pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            pso.SampleMask = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 0;
            pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pso.SampleDesc.Count = 1;
            CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&groundPso)),
                    "WorldPass: the ground map PSO");
        }
    }

    EnsureRings(ctx);
    CreateDefaultResources(ctx);
    return true;
}

void WorldPass::Impl::EnsureRings(Dx12Context& ctx) { ctxp = &ctx; }

D3D12_GPU_VIRTUAL_ADDRESS WorldPass::Impl::Upload(const void* data, size_t bytes, size_t align) {
    if (!ctxp) return 0;
    const uint64_t frame = ctxp->FrameNumber();
    UploadSegment& seg = uploads[frame % kUploadSegments];
    if (frame != uploadFrame) {   // first upload of this frame: the slot's previous use has finished on the GPU
        uploadFrame = frame;
        seg.chunk = 0;
        seg.offset = 0;
    }
    for (;;) {
        if (seg.chunk < seg.chunks.size()) {
            UploadChunk& c = seg.chunks[seg.chunk];
            const size_t at = (seg.offset + align - 1) / align * align;
            if (at + bytes <= c.size) {
                if (data) std::memcpy(c.mapped + at, data, bytes);
                seg.offset = at + bytes;
                return c.res->GetGPUVirtualAddress() + at;
            }
            ++seg.chunk;
            seg.offset = 0;
            continue;
        }
        UploadChunk c;
        c.size = std::max(kUploadChunkSize, bytes + align);
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(c.size);
        if (FAILED(ctxp->Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&c.res))) ||
            FAILED(c.res->Map(0, nullptr, reinterpret_cast<void**>(&c.mapped)))) {
            LOG_ERROR("WorldPass: upload chunk allocation failed");
            return 0;
        }
        c.res->SetName(L"world.upload");
        seg.chunks.push_back(std::move(c));
    }
}

void WorldPass::Impl::CreateDefaultResources(Dx12Context& ctx) {
    ID3D12Device* device = ctx.Device();
    UploadBatch batch(ctx);

    // 1x1 white texture
    ImageRGBA8 whiteImg;
    whiteImg.mips.push_back({1, 1, {255, 255, 255, 255}});
    defaultWhiteTex.res = batch.CreateTexture(whiteImg, L"world.defaultWhite");
    defaultWhiteTex.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultWhiteTex.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultWhiteTex.width = defaultWhiteTex.height = 1;

    // 1x1 black/transparent field texture
    ImageRGBA8 blackImg;
    blackImg.mips.push_back({1, 1, {0, 0, 0, 0}});
    defaultBlackFieldTex.res = batch.CreateTexture(blackImg, L"world.defaultBlackField");
    defaultBlackFieldTex.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultBlackFieldTex.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultBlackFieldTex.width = defaultBlackFieldTex.height = 1;

    // The ground map's fallback: a 1x1 R32_FLOAT, bound when no pack needs a real map.
    groundFallback.Create(ctx, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COMMON, L"world.groundFallback");
    groundFallbackSrv = ctx.SrvHeap().Allocate(1);
    if (groundFallbackSrv != DescriptorHeap::kInvalid && groundFallback.res)
        groundFallback.WriteSrv(device, ctx.SrvHeap().Cpu(groundFallbackSrv));

    // Default dummy collider buffer (1 entry)
    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    const auto colDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GpuWorldColliderData));
    device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colDesc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&defaultColliderBuffer));

    batch.Submit();

    // Allocate 32 descriptors in SrvHeap pointing to defaultWhiteTex
    if (defaultPackTexSrv == DescriptorHeap::kInvalid) {
        defaultPackTexSrv = ctx.SrvHeap().Allocate(kPackMaxTexturesWorld);
        if (defaultPackTexSrv != DescriptorHeap::kInvalid && defaultWhiteTex.res) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < kPackMaxTexturesWorld; ++i) {
                device->CreateShaderResourceView(defaultWhiteTex.res.Get(), &srv,
                                                 ctx.SrvHeap().Cpu(defaultPackTexSrv + i));
            }
        }
    }
}

D3D12_GPU_VIRTUAL_ADDRESS WorldPass::Impl::AllocParamsExt(const ShaderPack& pack,
                                                          const std::map<std::string, float>& params) {
    XMFLOAT4 slots[128] = {};
    std::vector<XMFLOAT4> built;
    BuildPackParamSlots(pack, params, built);
    std::memcpy(slots, built.data(), std::min(built.size(), (size_t)128) * sizeof(XMFLOAT4));
    return Upload(slots, kExtBlockSize, 256);
}

D3D12_GPU_VIRTUAL_ADDRESS WorldPass::Impl::AllocColliders(const std::vector<WorldCollider>& colliders) {
    const uint32_t count = std::min<uint32_t>((uint32_t)colliders.size(), kWorldMaxColliders);
    GpuWorldColliderData gpu[kWorldMaxColliders] = {};
    for (uint32_t i = 0; i < count; ++i) {
        gpu[i].pos = colliders[i].pos;
        gpu[i].radius = colliders[i].radius;
        gpu[i].vel = colliders[i].vel;
        gpu[i].flags = colliders[i].flags;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va = Upload(gpu, std::max(count, 1u) * sizeof(GpuWorldColliderData), 16);
    return va ? va : (defaultColliderBuffer ? defaultColliderBuffer->GetGPUVirtualAddress() : 0);
}

D3D12_GPU_VIRTUAL_ADDRESS WorldPass::Impl::AllocWorldCb(const WorldCbData& cb) {
    return Upload(&cb, sizeof(WorldCbData), 256);
}

// ---- the ground height map (world.ground) ---------------------------------------------------

// The ground map's resources: the depth texture (read by the world shaders as R32_FLOAT). The 1x1 fallback lives in
// CreateDefaultResources (it is always bound, so the descriptor is never null).
void WorldPass::Impl::EnsureGroundResources(Dx12Context& ctx) {
    if (groundSrv != DescriptorHeap::kInvalid) return;
    ID3D12Device* device = ctx.Device();
    if (!groundDepth.Create(ctx, kWorldGroundMapSize, kWorldGroundMapSize, RenderTargets::kDepthFormat,
                            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COMMON, L"world.ground")) {
        LOG_ERROR("WorldPass: the ground map could not be created");
        return;
    }
    groundSrv = ctx.SrvHeap().Allocate(1);
    if (groundSrv != DescriptorHeap::kInvalid)
        groundDepth.WriteSrv(device, ctx.SrvHeap().Cpu(groundSrv));
}

void WorldPass::Impl::ReleaseGroundResources(Dx12Context& ctx) {
    if (groundSrv != DescriptorHeap::kInvalid) {
        ctx.SrvHeap().Free(groundSrv, 1);
        groundSrv = DescriptorHeap::kInvalid;
    }
    groundDepth.Release(ctx);
    groundValid = false;
    groundVersion = ~0ull;
}

// The stage / prop models that make the ground (never the performers) and their world AABB.
static void CollectGroundModels(const PassContext& pc, XMFLOAT3& bmin, XMFLOAT3& bmax, uint64_t& version) {
    bmin = XMFLOAT3(1e30f, 1e30f, 1e30f);
    bmax = XMFLOAT3(-1e30f, -1e30f, -1e30f);
    version = 1469598103934665603ull;
    auto mix = [&version](uint64_t v) {
        version ^= v;
        version *= 1099511628211ull;
    };
    for (GpuModel* model : pc.view.models) {
        if (!model) continue;
        if (model->Role() != ModelRole::Stage && !model->IsProp()) continue;
        mix(model->Uid());
        const XMFLOAT4X4* bones = model->BoneMatricesCpu(pc.frame);
        if (!bones || model->BoneCount() == 0) continue;
        const XMMATRIX root = XMLoadFloat4x4(&bones[0]);
        const uint32_t* raw = reinterpret_cast<const uint32_t*>(&root);
        for (int i = 0; i < 16; ++i) mix(raw[i]);
        const XMFLOAT3 mn = model->BoundsMin();
        const XMFLOAT3 mx = model->BoundsMax();
        for (int i = 0; i < 8; ++i) {
            const XMVECTOR corner = XMVectorSet((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y,
                                                (i & 4) ? mx.z : mn.z, 1.0f);
            const XMVECTOR w = XMVector3Transform(corner, root);
            XMFLOAT3 f;
            XMStoreFloat3(&f, w);
            bmin.x = std::min(bmin.x, f.x); bmin.y = std::min(bmin.y, f.y); bmin.z = std::min(bmin.z, f.z);
            bmax.x = std::max(bmax.x, f.x); bmax.y = std::max(bmax.y, f.y); bmax.z = std::max(bmax.z, f.z);
        }
    }
}

// The ground map: a top-down orthographic depth render of the stage / prop models (the shadow pass's VS with an
// override matrix, back faces culled so a sky dome is not a roof). It is re-rendered when the models or their pose
// change, at most once per frame.
void WorldPass::Impl::RenderGroundMap(PassContext& pc) {
    XMFLOAT3 bmin, bmax;
    uint64_t version = 0;
    CollectGroundModels(pc, bmin, bmax, version);

    if (bmin.x > bmax.x) {   // no stage / prop model: the ground is the plane y = 0
        groundMapParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        groundRectParams = XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f);
        groundValid = true;
        groundVersion = version;
        return;
    }

    if (groundValid && version == groundVersion) return;   // nothing moved

    const float cx = (bmin.x + bmax.x) * 0.5f;
    const float cz = (bmin.z + bmax.z) * 0.5f;
    float half = std::max({bmax.x - bmin.x, bmax.z - bmin.z, 1.0f}) * 0.5f;
    half = std::min(half, kWorldGroundMaxExtent);
    const float yTop = bmax.y + 1.0f;
    const float yBottom = bmin.y - 1.0f;
    const float yRange = std::max(yTop - yBottom, 0.01f);

    // The camera looks straight down (-Y): the view x / y axes are the world x / z, the view z is the height.
    const XMMATRIX view = XMMatrixLookToLH(XMVectorSet(cx, yTop, cz, 1.0f), XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
                                            XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
    const XMMATRIX proj = XMMatrixOrthographicOffCenterLH(-half, half, -half, half, 0.0f, yRange);
    const XMMATRIX vp = view * proj;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    groundDepth.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(groundDepth.dsv);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)kWorldGroundMapSize, (float)kWorldGroundMapSize, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)kWorldGroundMapSize, (LONG)kWorldGroundMapSize};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(groundRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // The shadow CB's matrix override: gCascade 19 selects gPointViewProj, which the matrix below replaces
    // (the matrix sits at dword 4 of the shadow constant buffer: gCascade + the pad).
    cmd->SetGraphicsRoot32BitConstant(4, 19, 0);
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);
    cmd->SetGraphicsRoot32BitConstants(4, 16, &m, 4);   // 16 dwords at offset 4

    for (GpuModel* model : pc.view.models) {
        if (!model) continue;
        if (model->Role() != ModelRole::Stage && !model->IsProp()) continue;
        BindGroundModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        const auto& consts = model->MaterialConstantsCpu();
        ID3D12PipelineState* bound = nullptr;
        for (size_t i = 0; i < model->Materials().size() && i < consts.size(); ++i) {
            const GpuModel::Material& mat = model->Materials()[i];
            if (mat.indexCount == 0) continue;
            const MaterialConstants& mc = consts[i];
            if (mc.diffuse.w < 0.5f) continue;                                        // invisible
            if (mc.flags & (MatFlag_Additive | MatFlag_NoDepthWrite)) continue;       // no depth write
            ID3D12PipelineState* want = groundPso.Get();
            if (!want) continue;
            if (want != bound) {
                cmd->SetPipelineState(want);
                bound = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, mat.constants);
            cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(mat.srvTable));
            cmd->DrawIndexedInstanced(mat.indexCount, 1, mat.indexStart, 0, 0);
            pc.stats.drawCalls++;
        }
    }
    groundDepth.Transition(cmd, kSrv);

    groundMapParams = XMFLOAT4((float)kWorldGroundMapSize, 1.0f / (float)kWorldGroundMapSize, cx, cz);
    groundRectParams = XMFLOAT4(half, yTop, yRange, 1.0f);
    groundVersion = version;
    groundValid = true;
}

// The ground map, when a world pack needs it (a pack that requires the "world.ground" capability). The map is
// rendered before the simulation, so a simulation can query the ground.
void WorldPass::Impl::EnsureGround(PassContext& pc) {
    bool needed = false;
    for (const WorldInstance& inst : pc.view.worlds) {
        const ShaderPack* pack = ShaderPacks().Find(inst.pack);
        if (pack && pack->type == PackType::World && pack->world && pack->HasCap("world.ground")) {
            needed = true;
            break;
        }
    }
    if (!needed) return;
    EnsureGroundResources(pc.ctx);
    if (!groundRootSig || !groundPso) return;   // the shaders failed to compile: no ground
    RenderGroundMap(pc);
}

// The field's anchor (world.anchor): the mean xz of the foot colliders over the whole collider track, in the
// entity's local space. It is computed once (the collider track changes with the cast, not with the time).
void WorldPass::Impl::UpdateAnchor(const WorldInstance& inst, InstanceState& state) {
    const uint64_t ver = inst.colliders ? inst.colliders->version : 0;
    if (state.anchorVersion == ver) return;
    state.anchorVersion = ver;
    state.anchorX = 0.0f;
    state.anchorZ = 0.0f;
    if (!inst.colliders) return;

    const WorldColliderTrack& track = *inst.colliders;
    double sumX = 0.0, sumZ = 0.0;
    uint64_t count = 0;
    for (uint32_t s = 0; s < track.sampleCount; ++s) {
        const WorldCollider* c = &track.samples[(size_t)s * track.perSample];
        for (uint32_t i = 0; i < track.perSample; ++i) {
            if (!(c[i].flags & kColliderFoot)) continue;
            sumX += c[i].pos.x;
            sumZ += c[i].pos.z;
            ++count;
        }
    }
    if (count == 0) return;

    // The mean world xz -> the entity's local space (the field's centre).
    const XMFLOAT3 mean = XMFLOAT3((float)(sumX / (double)count), 0.0f, (float)(sumZ / (double)count));
    const XMMATRIX mWorld = XMLoadFloat4x4(&inst.transform);
    const XMMATRIX mInvWorld = XMMatrixInverse(nullptr, mWorld);
    const XMVECTOR local = XMVector3Transform(XMLoadFloat3(&mean), mInvWorld);
    XMFLOAT3 l;
    XMStoreFloat3(&l, local);
    state.anchorX = l.x;
    state.anchorZ = l.z;
}

WorldPass::Impl::PackPipelines* WorldPass::Impl::GetOrCompilePipelines(Dx12Context& ctx, const ShaderPack& pack) {
    if (pack.type != PackType::World || !pack.world) return nullptr;
    ID3D12Device* device = ctx.Device();

    auto it = packPipelines.find(pack.id);
    if (it != packPipelines.end()) {
        if (it->second.generation == ShaderPacks().Generation()) return &it->second;
    }

    PackPipelines pipes;
    pipes.packId = pack.id;
    pipes.generation = ShaderPacks().Generation();

    const std::filesystem::path hostShader = shaderDir / L"world.hlsl";
    std::filesystem::path hlslPath = pack.dir / L"world.hlsl";
    std::string inc = PathToUtf8(hlslPath);
    std::replace(inc.begin(), inc.end(), '\\', '/');
    const uint32_t texCount = (uint32_t)std::min<size_t>(pack.textures.size(), kPackMaxTexturesWorld);
    uint32_t clampMask = 0, srgbMask = 0;
    for (uint32_t i = 0; i < texCount; ++i) {
        if (pack.textures[i].clamp) clampMask |= (1u << i);
        if (pack.textures[i].srgb) srgbMask |= (1u << i);
    }

    ShaderDefines baseDefines = {
        {"MMDX_PACK", "\"" + inc + "\""},
        {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
        {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"},
        {"PACK_TEX_COUNT", std::to_string(texCount)}
    };
    // The world capabilities (docs/shader_world_api.md): the pack declares them in "requires".
    if (pack.HasCap("world.camera")) baseDefines.push_back({"PACK_CAP_WORLD_CAMERA", "1"});
    if (pack.HasCap("world.anchor")) baseDefines.push_back({"PACK_CAP_WORLD_ANCHOR", "1"});
    if (pack.HasCap("world.ground")) baseDefines.push_back({"PACK_CAP_WORLD_GROUND", "1"});
    if (pack.HasCap("world.overlay")) baseDefines.push_back({"PACK_CAP_WORLD_OVERLAY", "1"});
    if (pack.HasCap("world.body")) baseDefines.push_back({"PACK_CAP_WORLD_BODY", "1"});
    const bool worldLights = pack.HasCap("world.lights");
    if (worldLights) baseDefines.push_back({"PACK_CAP_WORLD_LIGHTS", "1"});
    // The audio inputs (pack_inputs.hlsli): gPackAudio0 / 1 of the scene constants every world entry point binds.
    if (pack.HasCap("inputs.audio")) baseDefines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
    // The ray-query shadow path (RT / PT / offline GI): the same PSOs with the ps_6_5 variant, only for lights packs.
    const bool lightsRt = worldLights && RtPipelinesSupported(ctx);

    // 1. CSWorldClear
    {
        ShaderDefines d = baseDefines;
        d.push_back({"WORLD_CLEAR", "1"});
        std::string err;
        ComPtr<ID3DBlob> cs = CompileShaderDxc(hostShader, "CSWorldClear", "cs_6_0", d, &err);
        if (cs) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = particleComputeRootSig.Get();
            pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipes.psoClear)), "WorldPass: pipes.psoClear");
        } else {
            LOG_ERROR("WorldPack '%s': CSWorldClear failed: %s", pack.id.c_str(), err.c_str());
        }
    }

    // 2. Particle systems
    pipes.particles.resize(pack.world->particles.size());
    for (size_t i = 0; i < pack.world->particles.size(); ++i) {
        const auto& p = pack.world->particles[i];
        auto& pPipe = pipes.particles[i];

        if (!p.simulate.empty()) {
            ShaderDefines d = baseDefines;
            d.push_back({"WORLD_SIMULATE", "1"});
            d.push_back({"WORLD_SIMULATE_FN", p.simulate});
            std::string err;
            ComPtr<ID3DBlob> cs = CompileShaderDxc(hostShader, "CSWorldSimulate", "cs_6_0", d, &err);
            if (cs) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = particleComputeRootSig.Get();
                pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
                CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pPipe.psoSimulate)), "WorldPass: pPipe.psoSimulate");
            } else {
                LOG_ERROR("WorldPack '%s' particle '%s': CSWorldSimulate failed: %s",
                          pack.id.c_str(), p.name.c_str(), err.c_str());
            }
        } else if (!p.spawn.empty()) {
            ShaderDefines d = baseDefines;
            d.push_back({"WORLD_SPAWN", "1"});
            d.push_back({"WORLD_SPAWN_FN", p.spawn});
            std::string err;
            ComPtr<ID3DBlob> cs = CompileShaderDxc(hostShader, "CSWorldSpawn", "cs_6_0", d, &err);
            if (cs) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = particleComputeRootSig.Get();
                pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
                CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pPipe.psoSpawn)), "WorldPass: pPipe.psoSpawn");
            }
        }

        // Draw particle
        if (!p.shade.empty()) {
            ShaderDefines d = baseDefines;
            d.push_back({"WORLD_PARTICLE", "1"});
            d.push_back({"WORLD_PARTICLE_SHADE_FN", p.shade});
            if (p.simulate.empty() && !p.spawn.empty()) {
                d.push_back({"WORLD_STATELESS", "1"});
                d.push_back({"WORLD_SPAWN_FN", p.spawn});
            }
            std::string err;
            ComPtr<ID3DBlob> vs = CompileShaderDxc(hostShader, "VSWorldParticle", "vs_6_0", d, &err);
            ComPtr<ID3DBlob> ps = CompileShaderDxc(hostShader, "PSWorldParticle", "ps_6_0", d, &err);
            ShaderDefines dPost = d;
            if (p.blend == PackWorldParticleSystem::Blend::Add) dPost.push_back({"WORLD_PARTICLE_ADD", "1"});
            ComPtr<ID3DBlob> psPost = CompileShaderDxc(hostShader, "PSWorldParticlePost", "ps_6_0", dPost, &err);
            ComPtr<ID3DBlob> vsRt, psRt, psPostRt;
            if (lightsRt) {
                ShaderDefines dRt = d;
                dRt.push_back({"RT_SHADOWS", "1"});
                ShaderDefines dPostRt = dPost;
                dPostRt.push_back({"RT_SHADOWS", "1"});
                vsRt = CompileShaderDxc(hostShader, "VSWorldParticle", "vs_6_5", dRt, &err);
                psRt = CompileShaderDxc(hostShader, "PSWorldParticle", "ps_6_5", dRt, &err);
                psPostRt = CompileShaderDxc(hostShader, "PSWorldParticlePost", "ps_6_5", dPostRt, &err);
            }
            if (vs && ps) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = graphicsRootSig.Get();
                pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
                pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // no depth write for particles
                pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

                D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
                rtBlend.BlendEnable = TRUE;
                rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                if (p.blend == PackWorldParticleSystem::Blend::Add) {
                    rtBlend.SrcBlend = D3D12_BLEND_ONE;
                    rtBlend.DestBlend = D3D12_BLEND_ONE;
                    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
                    rtBlend.SrcBlendAlpha = D3D12_BLEND_ZERO;
                    rtBlend.DestBlendAlpha = D3D12_BLEND_ONE;
                    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                } else { // alpha
                    rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                    rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
                    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rtBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                }
                pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
                pso.BlendState.RenderTarget[0] = rtBlend;
                pso.BlendState.RenderTarget[1].BlendEnable = FALSE;
                pso.BlendState.RenderTarget[1].RenderTargetWriteMask = 0; // normal untouched
                pso.BlendState.RenderTarget[2].BlendEnable = FALSE;
                pso.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // velocity

                pso.SampleMask = UINT_MAX;
                pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pso.NumRenderTargets = 3;
                pso.RTVFormats[0] = RenderTargets::kColorFormat;
                pso.RTVFormats[1] = RenderTargets::kNormalFormat;
                pso.RTVFormats[2] = RenderTargets::kVelocityFormat;
                pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
                pso.SampleDesc.Count = msaa;
                CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pPipe.psoDraw)), "WorldPass: pPipe.psoDraw");
                const D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDrawDesc = pso;

                // Post-composite variant (1 RTV = lit, no MSAA, no depth target; depth comes from the depth SRV)
                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoPostDesc{};
                bool havePostDesc = false;
                if (psPost) {
                    pso.PS = {psPost->GetBufferPointer(), psPost->GetBufferSize()};
                    pso.NumRenderTargets = 1;
                    pso.RTVFormats[0] = RenderTargets::kColorFormat;
                    pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
                    pso.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
                    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
                    pso.DepthStencilState.DepthEnable = FALSE;
                    pso.SampleDesc.Count = 1;
                    pso.BlendState.RenderTarget[0] = rtBlend;
                    CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pPipe.psoDrawPost)),
                            "WorldPass: pPipe.psoDrawPost");
                    psoPostDesc = pso;
                    havePostDesc = true;
                    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                    pso.DepthStencilState.DepthEnable = TRUE;
                } else {
                    LOG_ERROR("WorldPack '%s' particle '%s': PSWorldParticlePost failed: %s",
                              pack.id.c_str(), p.name.c_str(), err.c_str());
                }

                // Offline GI variant (1 RTV, 4 MSAA)
                pso.NumRenderTargets = 1;
                pso.RTVFormats[0] = RenderTargets::kColorFormat;
                pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
                pso.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
                pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
                pso.SampleDesc.Count = 4;
                CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pPipe.psoDrawOffline)), "WorldPass: pPipe.psoDrawOffline");

                // world.lights: the ray-query shadow variants (RT / PT / offline GI), same descriptors.
                if (vsRt && psRt) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC rtDesc = psoDrawDesc;
                    rtDesc.pRootSignature = graphicsRootSigLights.Get();
                    rtDesc.VS = {vsRt->GetBufferPointer(), vsRt->GetBufferSize()};
                    rtDesc.PS = {psRt->GetBufferPointer(), psRt->GetBufferSize()};
                    CheckHr(device->CreateGraphicsPipelineState(&rtDesc, IID_PPV_ARGS(&pPipe.psoDrawRt)),
                            "WorldPass: pPipe.psoDrawRt");
                    if (havePostDesc && psPostRt) {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC rtPost = psoPostDesc;
                        rtPost.pRootSignature = graphicsRootSigLights.Get();
                        rtPost.VS = rtDesc.VS;
                        rtPost.PS = {psPostRt->GetBufferPointer(), psPostRt->GetBufferSize()};
                        CheckHr(device->CreateGraphicsPipelineState(&rtPost, IID_PPV_ARGS(&pPipe.psoDrawPostRt)),
                                "WorldPass: pPipe.psoDrawPostRt");
                    }
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC rtOff = pso;
                    rtOff.pRootSignature = graphicsRootSigLights.Get();
                    rtOff.VS = rtDesc.VS;
                    rtOff.PS = rtDesc.PS;
                    CheckHr(device->CreateGraphicsPipelineState(&rtOff, IID_PPV_ARGS(&pPipe.psoDrawOfflineRt)),
                            "WorldPass: pPipe.psoDrawOfflineRt");
                }
            } else {
                LOG_ERROR("WorldPack '%s' particle '%s': Draw PSO failed: %s",
                          pack.id.c_str(), p.name.c_str(), err.c_str());
            }
        }
    }

    // 3. Fields
    pipes.fields.resize(pack.world->fields.size());
    for (size_t i = 0; i < pack.world->fields.size(); ++i) {
        const auto& f = pack.world->fields[i];
        auto& fPipe = pipes.fields[i];
        if (!f.update.empty()) {
            ShaderDefines d = baseDefines;
            d.push_back({"WORLD_FIELD_UPDATE", "1"});
            d.push_back({"WORLD_FIELD_UPDATE_FN", f.update});
            std::string err;
            ComPtr<ID3DBlob> cs = CompileShaderDxc(hostShader, "CSWorldFieldUpdate", "cs_6_0", d, &err);
            if (cs) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = fieldComputeRootSig.Get();
                pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
                CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&fPipe.psoUpdate)), "WorldPass: fPipe.psoUpdate");
            } else {
                LOG_ERROR("WorldPack '%s' field '%s': CSWorldFieldUpdate failed: %s",
                          pack.id.c_str(), f.name.c_str(), err.c_str());
            }
        }
    }

    // 4. Surfaces
    pipes.surfaces.resize(pack.world->surfaces.size());
    for (size_t i = 0; i < pack.world->surfaces.size(); ++i) {
        const auto& s = pack.world->surfaces[i];
        auto& sPipe = pipes.surfaces[i];
        if (!s.height.empty() && !s.shade.empty()) {
            ShaderDefines d = baseDefines;
            d.push_back({"WORLD_SURFACE", "1"});
            d.push_back({"WORLD_SURFACE_HEIGHT_FN", s.height});
            d.push_back({"WORLD_SURFACE_SHADE_FN", s.shade});
            const bool overlay = s.blend == PackWorldSurface::Blend::Overlay;
            if (overlay) d.push_back({"WORLD_SURFACE_OVERLAY", "1"});
            std::string err;
            ComPtr<ID3DBlob> vs = CompileShaderDxc(hostShader, "VSWorldSurface", "vs_6_0", d, &err);
            ComPtr<ID3DBlob> ps = CompileShaderDxc(hostShader, "PSWorldSurface", "ps_6_0", d, &err);
            ComPtr<ID3DBlob> vsRt, psRt;
            if (lightsRt) {
                ShaderDefines dRt = d;
                dRt.push_back({"RT_SHADOWS", "1"});
                vsRt = CompileShaderDxc(hostShader, "VSWorldSurface", "vs_6_5", dRt, &err);
                psRt = CompileShaderDxc(hostShader, "PSWorldSurface", "ps_6_5", dRt, &err);
            }
            if (vs && ps) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = graphicsRootSig.Get();
                pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
                pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // opaque

                pso.SampleMask = UINT_MAX;
                pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pso.NumRenderTargets = 3;
                pso.RTVFormats[0] = RenderTargets::kColorFormat;
                pso.RTVFormats[1] = RenderTargets::kNormalFormat;
                pso.RTVFormats[2] = RenderTargets::kVelocityFormat;
                pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
                pso.SampleDesc.Count = msaa;
                CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sPipe.psoDraw)), "WorldPass: sPipe.psoDraw");
                const D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDrawDesc = pso;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDrawOverlayDesc{};
                bool haveOverlayDesc = false;

                if (overlay) {
                    // The overlay blend: the premultiplied colour over the already-lit scene colour (the destination
                    // keeps its alpha, so the scene's coverage is untouched), the depth test on but no depth write and
                    // a small slope-scaled depth bias, so a surface a few mm above the ground does not z-fight. The
                    // normal / velocity targets keep the stage's own values.
                    pso.BlendState.IndependentBlendEnable = TRUE;   // RT1 / RT2 keep the stage's normal / velocity
                    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
                    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
                    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
                    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
                    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
                    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    pso.BlendState.RenderTarget[1].BlendEnable = FALSE;
                    pso.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    pso.BlendState.RenderTarget[2].BlendEnable = FALSE;
                    pso.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
                    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    pso.RasterizerState.SlopeScaledDepthBias = -2.0f;   // standard Z: negative = towards the camera
                    CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sPipe.psoDrawOverlay)),
                            "WorldPass: sPipe.psoDrawOverlay");
                    psoDrawOverlayDesc = pso;
                    haveOverlayDesc = true;
                    pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
                    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
                }

                // Offline GI variant (1 RTV, 4 MSAA)
                pso.NumRenderTargets = 1;
                pso.RTVFormats[0] = RenderTargets::kColorFormat;
                pso.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
                pso.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
                pso.SampleDesc.Count = 4;
                CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sPipe.psoDrawOffline)), "WorldPass: sPipe.psoDrawOffline");
                const D3D12_GRAPHICS_PIPELINE_STATE_DESC psoOfflineDesc = pso;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoOfflineOverlayDesc{};
                bool haveOfflineOverlayDesc = false;
                if (overlay) {
                    pso.BlendState.IndependentBlendEnable = TRUE;   // RT1 / RT2 keep the stage's normal / velocity
                    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
                    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
                    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
                    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
                    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
                    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    pso.RasterizerState.SlopeScaledDepthBias = -2.0f;   // standard Z: negative = towards the camera
                    CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sPipe.psoDrawOfflineOverlay)),
                            "WorldPass: sPipe.psoDrawOfflineOverlay");
                    psoOfflineOverlayDesc = pso;
                    haveOfflineOverlayDesc = true;
                }

                // world.lights: the ray-query shadow variants (RT / PT / offline GI), same descriptors.
                if (vsRt && psRt) {
                    auto makeRt = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base,
                                      ComPtr<ID3D12PipelineState>& out, const char* what) {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC rt = base;
                        rt.pRootSignature = graphicsRootSigLights.Get();
                        rt.VS = {vsRt->GetBufferPointer(), vsRt->GetBufferSize()};
                        rt.PS = {psRt->GetBufferPointer(), psRt->GetBufferSize()};
                        CheckHr(device->CreateGraphicsPipelineState(&rt, IID_PPV_ARGS(&out)), what);
                    };
                    makeRt(psoDrawDesc, sPipe.psoDrawRt, "WorldPass: sPipe.psoDrawRt");
                    makeRt(psoOfflineDesc, sPipe.psoDrawOfflineRt, "WorldPass: sPipe.psoDrawOfflineRt");
                    if (haveOverlayDesc)
                        makeRt(psoDrawOverlayDesc, sPipe.psoDrawRtOverlay, "WorldPass: sPipe.psoDrawRtOverlay");
                    if (haveOfflineOverlayDesc)
                        makeRt(psoOfflineOverlayDesc, sPipe.psoDrawOfflineRtOverlay,
                               "WorldPass: sPipe.psoDrawOfflineRtOverlay");
                }
            } else {
                LOG_ERROR("WorldPack '%s' surface: Draw PSO failed: %s", pack.id.c_str(), err.c_str());
            }
        }
    }

    // world.lights: the ray-query variants are all-or-nothing. A failed compile falls back to the raster variants
    // (and the common root signature) for every draw of this pack.
    if (lightsRt) {
        bool ok = graphicsRootSigLights != nullptr;
        for (const auto& pp : pipes.particles) {
            ok = ok && pp.psoDrawRt && pp.psoDrawOfflineRt;
            if (pp.psoDrawPost) ok = ok && pp.psoDrawPostRt;
        }
        for (const auto& sp : pipes.surfaces) {
            ok = ok && sp.psoDrawRt && sp.psoDrawOfflineRt;
            if (sp.psoDrawOverlay) ok = ok && sp.psoDrawRtOverlay;
            if (sp.psoDrawOfflineOverlay) ok = ok && sp.psoDrawOfflineRtOverlay;
        }
        pipes.rtReady = ok;
        if (!ok) {
            for (auto& pp : pipes.particles) {
                pp.psoDrawRt.Reset();
                pp.psoDrawPostRt.Reset();
                pp.psoDrawOfflineRt.Reset();
            }
            for (auto& sp : pipes.surfaces) {
                sp.psoDrawRt.Reset();
                sp.psoDrawOfflineRt.Reset();
                sp.psoDrawRtOverlay.Reset();
                sp.psoDrawOfflineRtOverlay.Reset();
            }
        }
    }

    packPipelines[pack.id] = std::move(pipes);
    return &packPipelines[pack.id];
}

WorldPass::Impl::InstanceState& WorldPass::Impl::GetOrCreateInstanceState(Dx12Context& ctx,
                                                                          const WorldInstance& inst,
                                                                          const ShaderPack& pack) {
    const auto key = std::make_pair(inst.uid, pack.id);
    auto it = instances.find(key);
    if (it != instances.end()) return it->second;

    ID3D12Device* device = ctx.Device();
    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    InstanceState state;
    state.uid = inst.uid;
    state.packId = pack.id;
    state.colliderVersion = inst.colliders ? inst.colliders->version : 0;
    state.simTime = 0.0;
    state.initialized = false;

    // Allocate particle buffers
    state.particles.resize(pack.world->particles.size());
    for (size_t i = 0; i < pack.world->particles.size(); ++i) {
        const auto& p = pack.world->particles[i];
        auto& pBuf = state.particles[i];
        pBuf.count = p.count;

        const auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(
            p.count * sizeof(GpuPackParticle), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&pBuf.buffer));

        const auto prevDesc = CD3DX12_RESOURCE_DESC::Buffer(
            p.count * sizeof(XMFLOAT4), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &prevDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&pBuf.prevPos));
    }

    // Allocate field textures
    state.fields.resize(pack.world->fields.size());
    for (size_t i = 0; i < pack.world->fields.size(); ++i) {
        const auto& f = pack.world->fields[i];
        auto& fTex = state.fields[i];
        fTex.size = f.size;
        fTex.format = ParseFieldDxgiFormat(f.format);
        fTex.persistent = f.persistent;
        fTex.rate = f.rate > 0.0f ? f.rate : 10.0f;
        fTex.persistentSimTime = 0.0;

        const auto flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        fTex.texA.Create(ctx, f.size, f.size, fTex.format, flags, D3D12_RESOURCE_STATE_COMMON, L"world.fieldA");
        fTex.texB.Create(ctx, f.size, f.size, fTex.format, flags, D3D12_RESOURCE_STATE_COMMON, L"world.fieldB");
    }
    state.fieldDesc = ctx.SrvHeap().Allocate(InstanceState::kFieldDescCount);
    if (state.fieldDesc != DescriptorHeap::kInvalid) {
        DescriptorHeap& heap = ctx.SrvHeap();
        for (size_t i = 0; i < state.fields.size(); ++i) {
            auto& fTex = state.fields[i];
            fTex.texA.WriteSrv(device, heap.Cpu(state.fieldDesc + (uint32_t)i * 4 + 0));
            fTex.texB.WriteSrv(device, heap.Cpu(state.fieldDesc + (uint32_t)i * 4 + 1));
            fTex.texA.WriteUav(device, heap.Cpu(state.fieldDesc + (uint32_t)i * 4 + 2));
            fTex.texB.WriteUav(device, heap.Cpu(state.fieldDesc + (uint32_t)i * 4 + 3));
        }
        for (uint32_t mask = 0; mask < 16; ++mask)
            for (uint32_t k = 0; k < 4; ++k) {
                const D3D12_CPU_DESCRIPTOR_HANDLE dst = heap.Cpu(state.fieldDesc + 16 + mask * 4 + k);
                if (k < state.fields.size())
                    ((mask >> k) & 1 ? state.fields[k].texB : state.fields[k].texA).WriteSrv(device, dst);
                else
                    defaultBlackFieldTex.WriteSrv(device, dst);
            }
    }

    instances[key] = std::move(state);
    return instances[key];
}

void WorldPass::Impl::SimulateInstance(PassContext& pc, const WorldInstance& inst, const ShaderPack& pack,
                                       PackPipelines& pipes, InstanceState& state) {
    if (!pack.world) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;

    const float stepDt = pack.world->step > 0.0f ? pack.world->step : (1.0f / 60.0f);
    const float rewindSec = pack.world->rewind >= 0.0f ? pack.world->rewind : 4.0f;
    const uint64_t curColliderVer = inst.colliders ? inst.colliders->version : 0;

    bool needResim = !state.initialized || (state.colliderVersion != curColliderVer);
    // A parameter edit (density, petal size ...) shows at once on a paused / repeated frame (the same time as the last
    // one); while the time moves (playback, video frames) a change (keyed params) does not force a re-simulation.
    // simTime cannot tell: it runs up to one grid step ahead of the drawn time during playback too.
    const uint64_t curParamsHash = HashWorldParams(inst);
    if (state.paramsHash != curParamsHash) {
        state.paramsHash = curParamsHash;
        if (state.initialized && std::fabs(inst.time - state.lastTime) < 1e-9) needResim = true;
    }
    state.lastTime = inst.time;
    if (!needResim) {
        // The drawn time may lie up to one step before simTime (the interpolation between the last two grid steps);
        // older than prevPos's step needs the history of a re-simulation.
        if (inst.time < state.simTime - (double)stepDt - 1e-6) needResim = true;
        else if (inst.time - state.simTime > 0.25) needResim = true;
    }

    // The field's anchor (world.anchor): the mean xz of the foot colliders, in the entity's local space.
    UpdateAnchor(inst, state);

    // The current particle system's wrap (camera space): 0 for a local-space system.
    float wrapSize = 0.0f;
    float cameraSpace = 0.0f;
    auto setWrap = [&](size_t pi) {
        const auto& p = pack.world->particles[pi];
        const bool cam = p.space == PackWorldParticleSystem::Space::Camera;
        wrapSize = cam ? ParticleWrap(pack, p, inst.params) : 0.0f;
        cameraSpace = cam ? 1.0f : 0.0f;
    };

    // Helper: Build WorldCbData for a step
    auto makeWorldCb = [&](float timeVal, float dtVal, float simVal, uint32_t stepIdx,
                           uint32_t colCount, uint32_t fieldCount, uint32_t partIdx, uint32_t countOrGrid) {
        WorldCbData cb{};
        XMMATRIX mWorld = XMLoadFloat4x4(&inst.transform);
        XMMATRIX mInvWorld = XMMatrixInverse(nullptr, mWorld);
        XMStoreFloat4x4(&cb.localToWorld, mWorld);
        XMStoreFloat4x4(&cb.worldToLocal, mInvWorld);
        cb.worldTime = XMFLOAT4(timeVal, dtVal, simVal, (float)stepIdx);
        cb.worldParams = XMFLOAT4((float)colCount, (float)fieldCount, (float)partIdx, (float)countOrGrid);
        cb.worldExtra = XMFLOAT4(wrapSize, cameraSpace, state.anchorX, state.anchorZ);
        cb.groundMap = groundMapParams;
        cb.groundRect = groundRectParams;

        const LightParams& lp = pc.view.light;
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lp.direction));
        XMFLOAT3 lNorm;
        XMStoreFloat3(&lNorm, L);
        const float hasSun = (lp.sunIntensity > 0.0f) ? 1.0f : 0.0f;
        cb.sunDir = XMFLOAT4(-lNorm.x, -lNorm.y, -lNorm.z, hasSun);
        cb.sunColor = XMFLOAT4(lp.color.x * lp.sunIntensity, lp.color.y * lp.sunIntensity, lp.color.z * lp.sunIntensity, pc.view.shadowDistance);
        cb.ambient = XMFLOAT4(lp.groundColor.x, lp.groundColor.y, lp.groundColor.z, 0.0f);

        for (size_t fi = 0; fi < std::min<size_t>(pack.world->fields.size(), 4); ++fi) {
            const auto& f = pack.world->fields[fi];
            cb.fieldExtent[fi] = XMFLOAT4((float)f.size, (float)f.size, f.extent, f.extent > 0 ? (1.0f / f.extent) : 0);
        }
        return cb;
    };

    // Helper: Pack texture descriptor table
    D3D12_GPU_DESCRIPTOR_HANDLE packTexTable{};
    const PackTextures::Set* ptSet = packTextures.Acquire(ctx, pack, pack.dir);
    if (ptSet && ptSet->srv != DescriptorHeap::kInvalid) {
        packTexTable = ctx.SrvHeap().Gpu(ptSet->srv);
    } else if (defaultPackTexSrv != DescriptorHeap::kInvalid) {
        packTexTable = ctx.SrvHeap().Gpu(defaultPackTexSrv);
    }

    // Field tables: the persistent per-instance descriptors (see InstanceState::fieldDesc)
    if (state.fieldDesc == DescriptorHeap::kInvalid) return;
    auto makeFieldTable = [&]() { return FieldTable(ctx, state); };
    // One field step's prev (SRV) / next (UAV) single-descriptor tables
    auto bindFieldPrevNext = [&](size_t fi) {
        auto& fTex = state.fields[fi];
        const uint32_t base = state.fieldDesc + (uint32_t)fi * 4;
        cmd->SetComputeRootDescriptorTable(6, ctx.SrvHeap().Gpu(base + (fTex.currentIsB ? 0 : 1)));
        cmd->SetComputeRootDescriptorTable(7, ctx.SrvHeap().Gpu(base + 2 + (fTex.currentIsB ? 1 : 0)));
    };
    for (auto& fTex : state.fields) fTex.Previous().Transition(cmd, kSrv);

    const D3D12_GPU_VIRTUAL_ADDRESS paramsExtVa = AllocParamsExt(pack, inst.params);
    std::vector<WorldCollider> sampledColliders;

    // One persistent-field step to grid time k / rate (reset = the field starts from zero)
    auto stepPersistent = [&](size_t fi, uint64_t k, bool reset) {
        auto& fTex = state.fields[fi];
        const double pDt = 1.0 / fTex.rate;
        const double t = (double)k * pDt;
        if (inst.colliders) SampleColliders(inst, pack, t, sampledColliders);
        else sampledColliders.clear();
        const D3D12_GPU_VIRTUAL_ADDRESS colVa = AllocColliders(sampledColliders);
        wrapSize = 0.0f;   // a field step never runs in camera space
        cameraSpace = 0.0f;
        const auto cb = makeWorldCb((float)t, k ? (float)pDt : 0.0f, (float)(t - pDt), (uint32_t)k,
                                    (uint32_t)sampledColliders.size(), (uint32_t)state.fields.size(),
                                    (uint32_t)fi + (reset ? 16u : 0u), fTex.size);
        cmd->SetComputeRootSignature(fieldComputeRootSig.Get());
        cmd->SetPipelineState(pipes.fields[fi].psoUpdate.Get());
        cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
        cmd->SetComputeRootConstantBufferView(1, AllocWorldCb(cb));
        cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
        cmd->SetComputeRootShaderResourceView(3, colVa);
        cmd->SetComputeRootDescriptorTable(4, makeFieldTable());
        cmd->SetComputeRootDescriptorTable(5, packTexTable);
        cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));
        fTex.Current().Transition(cmd, kUav);
        bindFieldPrevNext(fi);
        const uint32_t gx = (fTex.size + 7) / 8;
        cmd->Dispatch(gx, gx, 1);
        D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(fTex.Current().res.Get());
        cmd->ResourceBarrier(1, &uav);
        fTex.Swap();
        fTex.Previous().Transition(cmd, kSrv);
    };

    if (needResim) {
        state.colliderVersion = curColliderVer;
        state.initialized = true;

        // 1. Persistent fields: re-simulate from 0 on their own grid (step k at time k / rate), the same grid live
        // playback follows below, so a seek and continuous playback reach the same field at the same time
        for (size_t fi = 0; fi < state.fields.size(); ++fi) {
            auto& fTex = state.fields[fi];
            if (!fTex.persistent || pipes.fields[fi].psoUpdate == nullptr) continue;
            const double pDt = 1.0 / fTex.rate;
            const uint64_t pSteps = (uint64_t)std::floor(inst.time / pDt + 1e-6);
            if (pSteps == 0) stepPersistent(fi, 0, true);   // no step yet: still start from a zero field
            for (uint64_t k = 1; k <= pSteps; ++k) stepPersistent(fi, k, k == 1);
            fTex.persistentSimTime = (double)pSteps * pDt;
        }

        // 2. Clear stateful particles
        if (pipes.psoClear) {
            cmd->SetComputeRootSignature(particleComputeRootSig.Get());
            cmd->SetPipelineState(pipes.psoClear.Get());
            for (size_t pi = 0; pi < state.particles.size(); ++pi) {
                const auto& p = pack.world->particles[pi];
                if (p.simulate.empty()) continue; // stateless particles don't clear/simulate
                auto& pBuf = state.particles[pi];
                setWrap(pi);

                const auto cb = makeWorldCb((float)inst.time, stepDt, 0.0f, 0,
                                            0, (uint32_t)state.fields.size(), (uint32_t)pi, pBuf.count);
                const D3D12_GPU_VIRTUAL_ADDRESS cbVa = AllocWorldCb(cb);

                cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
                cmd->SetComputeRootConstantBufferView(1, cbVa);
                cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
                cmd->SetComputeRootShaderResourceView(3, defaultColliderBuffer->GetGPUVirtualAddress());
                cmd->SetComputeRootDescriptorTable(4, makeFieldTable());
                cmd->SetComputeRootDescriptorTable(5, packTexTable);
                cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));
                TransitionParticles(cmd, pBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetComputeRootUnorderedAccessView(6, pBuf.buffer->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(7, pBuf.prevPos->GetGPUVirtualAddress());

                const uint32_t gx = (pBuf.count + 63) / 64;
                cmd->Dispatch(gx, 1, 1);

                D3D12_RESOURCE_BARRIER b[2] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.buffer.Get()),
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.prevPos.Get())
                };
                cmd->ResourceBarrier(2, b);
            }
        }

        // 3. Re-simulate particles + transient fields over the rewind window, on the global step grid: every step ends
        // at k * stepDt (k from timeline 0), from the grid point at or before inst.time - rewind to the first grid
        // point at or after inst.time (no partial steps).
        const double stepD = (double)stepDt;
        const double tStart = std::max(0.0, inst.time - (double)rewindSec);
        uint64_t kStart = (uint64_t)std::floor(tStart / stepD + 1e-9);
        const uint64_t kEnd = (uint64_t)std::ceil(inst.time / stepD - 1e-9);
        uint32_t steps = kEnd > kStart ? (uint32_t)(kEnd - kStart) : 0u;
        if (!pc.offscreen && steps > 4000) {   // keep the part of the window closest to the drawn time
            steps = 4000;
            kStart = kEnd - steps;
        }

        LOG_INFO("WORLD resim uid=%u pack='%s': %u steps (%.2f s -> %.2f s)",
                 inst.uid, pack.id.c_str(), steps, (float)((double)kStart * stepD), (float)((double)kEnd * stepD));

        uint64_t kCur = kStart;
        for (uint32_t s = 0; s < steps; ++s) {
            const double tCur = (double)kCur * stepD;
            const double tNext = (double)(kCur + 1) * stepD;
            ++kCur;
            if (inst.colliders) SampleColliders(inst, pack, tNext, sampledColliders);
            const D3D12_GPU_VIRTUAL_ADDRESS colVa = AllocColliders(sampledColliders);

            // Step transient fields
            for (size_t fi = 0; fi < state.fields.size(); ++fi) {
                auto& fTex = state.fields[fi];
                if (fTex.persistent || pipes.fields[fi].psoUpdate == nullptr) continue;

                cmd->SetComputeRootSignature(fieldComputeRootSig.Get());
                cmd->SetPipelineState(pipes.fields[fi].psoUpdate.Get());
                cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
                cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
                cmd->SetComputeRootDescriptorTable(5, packTexTable);
                cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));

                wrapSize = 0.0f;   // a field step never runs in camera space
                cameraSpace = 0.0f;
                const auto cb = makeWorldCb((float)tNext, stepDt, (float)tCur, s, (uint32_t)sampledColliders.size(),
                                            (uint32_t)state.fields.size(), (uint32_t)fi + (s == 0 ? 16u : 0u), fTex.size);
                const D3D12_GPU_VIRTUAL_ADDRESS cbVa = AllocWorldCb(cb);

                cmd->SetComputeRootConstantBufferView(1, cbVa);
                cmd->SetComputeRootShaderResourceView(3, colVa);
                cmd->SetComputeRootDescriptorTable(4, makeFieldTable());

                fTex.Previous().Transition(cmd, kSrv);
                fTex.Current().Transition(cmd, kUav);
                bindFieldPrevNext(fi);

                const uint32_t gx = (fTex.size + 7) / 8;
                cmd->Dispatch(gx, gx, 1);

                D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(fTex.Current().res.Get());
                cmd->ResourceBarrier(1, &uav);
                fTex.Swap();
                fTex.Previous().Transition(cmd, kSrv);   // the result just written: readable by the next steps
            }

            // Step stateful particles
            for (size_t pi = 0; pi < state.particles.size(); ++pi) {
                const auto& p = pack.world->particles[pi];
                if (p.simulate.empty() || pipes.particles[pi].psoSimulate == nullptr) continue;
                auto& pBuf = state.particles[pi];
                setWrap(pi);

                cmd->SetComputeRootSignature(particleComputeRootSig.Get());
                cmd->SetPipelineState(pipes.particles[pi].psoSimulate.Get());
                cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
                cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
                cmd->SetComputeRootDescriptorTable(5, packTexTable);
                cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));

                const auto cb = makeWorldCb((float)tNext, stepDt, (float)tCur, s,
                                            (uint32_t)sampledColliders.size(), (uint32_t)state.fields.size(),
                                            (uint32_t)pi, pBuf.count);
                const D3D12_GPU_VIRTUAL_ADDRESS cbVa = AllocWorldCb(cb);

                cmd->SetComputeRootConstantBufferView(1, cbVa);
                cmd->SetComputeRootShaderResourceView(3, colVa);
                cmd->SetComputeRootDescriptorTable(4, makeFieldTable());
                cmd->SetComputeRootUnorderedAccessView(6, pBuf.buffer->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(7, pBuf.prevPos->GetGPUVirtualAddress());

                const uint32_t gx = (pBuf.count + 63) / 64;
                cmd->Dispatch(gx, 1, 1);

                D3D12_RESOURCE_BARRIER b[2] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.buffer.Get()),
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.prevPos.Get())
                };
                cmd->ResourceBarrier(2, b);
            }
        }
        state.simStep = kEnd;
        state.simTime = (double)kEnd * stepD;
    } else {
        // Normal live playback forward steps, on the global grid: each step ends at gridStep * stepDt and the loop
        // stops at the first grid point >= inst.time (simTime is always a grid point).
        while (state.simTime < inst.time - 1e-9) {
            const double tCur = state.simTime;
            const double tNext = (double)(state.simStep + 1) * (double)stepDt;
            if (inst.colliders) SampleColliders(inst, pack, tNext, sampledColliders);
            const D3D12_GPU_VIRTUAL_ADDRESS colVa = AllocColliders(sampledColliders);

            // Step transient fields (persistent ones follow their own grid below)
            for (size_t fi = 0; fi < state.fields.size(); ++fi) {
                auto& fTex = state.fields[fi];
                if (fTex.persistent || pipes.fields[fi].psoUpdate == nullptr) continue;

                cmd->SetComputeRootSignature(fieldComputeRootSig.Get());
                cmd->SetPipelineState(pipes.fields[fi].psoUpdate.Get());
                cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
                cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
                cmd->SetComputeRootDescriptorTable(5, packTexTable);
                cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));

                wrapSize = 0.0f;   // a field step never runs in camera space
                cameraSpace = 0.0f;
                const auto cb = makeWorldCb((float)tNext, stepDt, (float)tCur, 0,
                                            (uint32_t)sampledColliders.size(), (uint32_t)state.fields.size(),
                                            (uint32_t)fi, fTex.size);
                const D3D12_GPU_VIRTUAL_ADDRESS cbVa = AllocWorldCb(cb);

                cmd->SetComputeRootConstantBufferView(1, cbVa);
                cmd->SetComputeRootShaderResourceView(3, colVa);
                cmd->SetComputeRootDescriptorTable(4, makeFieldTable());

                fTex.Previous().Transition(cmd, kSrv);
                fTex.Current().Transition(cmd, kUav);
                bindFieldPrevNext(fi);

                const uint32_t gx = (fTex.size + 7) / 8;
                cmd->Dispatch(gx, gx, 1);

                D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(fTex.Current().res.Get());
                cmd->ResourceBarrier(1, &uav);
                fTex.Swap();
                fTex.Previous().Transition(cmd, kSrv);   // the result just written: readable by the next steps
            }

            // Step particles
            for (size_t pi = 0; pi < state.particles.size(); ++pi) {
                const auto& p = pack.world->particles[pi];
                if (p.simulate.empty() || pipes.particles[pi].psoSimulate == nullptr) continue;
                auto& pBuf = state.particles[pi];
                setWrap(pi);

                cmd->SetComputeRootSignature(particleComputeRootSig.Get());
                cmd->SetPipelineState(pipes.particles[pi].psoSimulate.Get());
                cmd->SetComputeRootConstantBufferView(0, pc.sceneConstants);
                cmd->SetComputeRootConstantBufferView(2, paramsExtVa);
                cmd->SetComputeRootDescriptorTable(5, packTexTable);
                cmd->SetComputeRootDescriptorTable(8, GroundTable(ctx));

                const auto cb = makeWorldCb((float)tNext, stepDt, (float)tCur, 0,
                                            (uint32_t)sampledColliders.size(), (uint32_t)state.fields.size(),
                                            (uint32_t)pi, pBuf.count);
                const D3D12_GPU_VIRTUAL_ADDRESS cbVa = AllocWorldCb(cb);

                cmd->SetComputeRootConstantBufferView(1, cbVa);
                cmd->SetComputeRootShaderResourceView(3, colVa);
                cmd->SetComputeRootDescriptorTable(4, makeFieldTable());
                TransitionParticles(cmd, pBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetComputeRootUnorderedAccessView(6, pBuf.buffer->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(7, pBuf.prevPos->GetGPUVirtualAddress());

                const uint32_t gx = (pBuf.count + 63) / 64;
                cmd->Dispatch(gx, 1, 1);

                D3D12_RESOURCE_BARRIER b[2] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.buffer.Get()),
                    CD3DX12_RESOURCE_BARRIER::UAV(pBuf.prevPos.Get())
                };
                cmd->ResourceBarrier(2, b);
            }
            state.simStep += 1;
            state.simTime = tNext;
        }
        for (size_t fi = 0; fi < state.fields.size(); ++fi) {
            auto& fTex = state.fields[fi];
            if (!fTex.persistent || pipes.fields[fi].psoUpdate == nullptr) continue;
            const double pDt = 1.0 / fTex.rate;
            uint64_t k = (uint64_t)std::floor(fTex.persistentSimTime / pDt + 1e-6);
            while ((double)(k + 1) * pDt <= inst.time + 1e-6) stepPersistent(fi, ++k, false);
            fTex.persistentSimTime = (double)k * pDt;
        }
    }
}

void WorldPass::Impl::DrawInstance(PassContext& pc, const WorldInstance& inst, const ShaderPack& pack,
                                   PackPipelines& pipes, InstanceState& state, bool offline, bool drawParticles) {
    if (!pack.world) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;

    // world.lights: in RT / PT / offline GI (a TLAS for this frame) the pack's ray-query PSOs trace the scene's sun
    // and punctual shadows. Without a TLAS (raster) the raster PSOs use the shadow maps, exactly as before.
    const bool worldLights = pack.HasCap("world.lights");
    const bool useRt = worldLights && pipes.rtReady && pc.rt != nullptr;
    cmd->SetGraphicsRootSignature(useRt ? graphicsRootSigLights.Get() : graphicsRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_GPU_VIRTUAL_ADDRESS paramsExtVa = AllocParamsExt(pack, inst.params);

    // Pack texture table
    D3D12_GPU_DESCRIPTOR_HANDLE packTexTable{};
    const PackTextures::Set* ptSet = packTextures.Acquire(ctx, pack, pack.dir);
    if (ptSet && ptSet->srv != DescriptorHeap::kInvalid) {
        packTexTable = ctx.SrvHeap().Gpu(ptSet->srv);
    } else if (defaultPackTexSrv != DescriptorHeap::kInvalid) {
        packTexTable = ctx.SrvHeap().Gpu(defaultPackTexSrv);
    }

    // Field table: every field's latest result
    if (state.fieldDesc == DescriptorHeap::kInvalid) return;
    const D3D12_GPU_DESCRIPTOR_HANDLE fieldTable = FieldTable(ctx, state);

    // One transient table for the sun / spot / point shadow maps (slots 0 / 1 / 2): root params 8, 11 and 12 point at
    // its descriptors. Three separate 8-slot tables per draw overflowed the frame's transient window in PT frames.
    const D3D12_GPU_DESCRIPTOR_HANDLE shadowTable = pc.transient.SrvTable(
        ctx, {pc.targets.shadowMap.res ? &pc.targets.shadowMap : nullptr,
              pc.targets.spotShadowMap.res ? &pc.targets.spotShadowMap : nullptr,
              pc.targets.pointShadowMap.res ? &pc.targets.pointShadowMap : nullptr});
    const UINT srvInc = ctx.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The scene's punctual lights for the world surfaces: the lights buffer (root SRV), the spot / point
    // shadow maps and the binding flags the shader reads. The offline GI path binds its own lights buffer too (no
    // spot / point shadow maps there: those lights reach the surfaces unshadowed instead of not at all).
    const bool haveLights = pc.lights != 0;
    const D3D12_GPU_DESCRIPTOR_HANDLE spotShadowTable{shadowTable.ptr ? shadowTable.ptr + srvInc : 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE pointShadowTable{shadowTable.ptr ? shadowTable.ptr + 2ull * srvInc : 0};
    uint32_t lightFlags = 0;
    if (haveLights) lightFlags |= 1u;   // the lights buffer is bound
    if (pc.targets.spotShadowMap.res) lightFlags |= 2u;   // the spot shadow map is bound
    if (pc.targets.pointShadowMap.res) lightFlags |= 4u;  // the point shadow map is bound

    std::vector<WorldCollider> sampledColliders;
    if (inst.colliders) SampleColliders(inst, pack, inst.time, sampledColliders);
    const D3D12_GPU_VIRTUAL_ADDRESS colVa = AllocColliders(sampledColliders);

    const float stepDt = pack.world->step > 0.0f ? pack.world->step : (1.0f / 60.0f);
    // The drawn state is interpolated between the last two grid steps (docs/shader_world_api.md): a = 1 - (simTime -
    // time) / stepDt; the motion vectors use the same rule at time - frameDelta (sceneElapsed, 0 when paused).
    const float interpA = (float)std::clamp(1.0 - (state.simTime - (double)inst.time) / (double)stepDt, 0.0, 1.0);
    const float frameDelta = pc.view.sceneElapsed > 0.0f ? pc.view.sceneElapsed : 0.0f;

    // The field's anchor (world.anchor) and the current draw's wrap / surface parameters.
    UpdateAnchor(inst, state);
    float wrapSize = 0.0f;
    float cameraSpace = 0.0f;
    XMFLOAT4 surfaceParams(0.0f, 0.0f, 0.0f, 0.0f);

    auto makeWorldCb = [&](uint32_t partIdx, uint32_t countOrGrid) {
        WorldCbData cb{};
        XMMATRIX mWorld = XMLoadFloat4x4(&inst.transform);
        XMMATRIX mInvWorld = XMMatrixInverse(nullptr, mWorld);
        XMStoreFloat4x4(&cb.localToWorld, mWorld);
        XMStoreFloat4x4(&cb.worldToLocal, mInvWorld);
        cb.worldTime = XMFLOAT4((float)inst.time, stepDt, (float)state.simTime, 0.0f);
        cb.worldParams = XMFLOAT4((float)sampledColliders.size(), (float)state.fields.size(),
                                  (float)partIdx, (float)countOrGrid);
        cb.worldExtra = XMFLOAT4(wrapSize, cameraSpace, state.anchorX, state.anchorZ);
        cb.groundMap = groundMapParams;
        cb.groundRect = groundRectParams;
        cb.surfaceParams = surfaceParams;
        cb.particleParams = XMFLOAT4(interpA, frameDelta, stepDt, wrapSize > 0.0f ? 0.5f * wrapSize : 0.0f);
        if (offline && offlineVpSet) {
            cb.offlineViewProj = offlineVp;
            cb.offlineParams = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);
        }

        const LightParams& lp = pc.view.light;
        XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lp.direction));
        XMFLOAT3 lNorm;
        XMStoreFloat3(&lNorm, L);
        const float hasSun = (lp.sunIntensity > 0.0f) ? 1.0f : 0.0f;
        cb.sunDir = XMFLOAT4(-lNorm.x, -lNorm.y, -lNorm.z, hasSun);
        cb.sunColor = XMFLOAT4(lp.color.x * lp.sunIntensity, lp.color.y * lp.sunIntensity, lp.color.z * lp.sunIntensity, pc.view.shadowDistance);
        cb.ambient = XMFLOAT4(lp.groundColor.x, lp.groundColor.y, lp.groundColor.z, 0.0f);

        for (size_t fi = 0; fi < std::min<size_t>(pack.world->fields.size(), 4); ++fi) {
            const auto& f = pack.world->fields[fi];
            cb.fieldExtent[fi] = XMFLOAT4((float)f.size, (float)f.size, f.extent, f.extent > 0 ? (1.0f / f.extent) : 0);
        }
        return cb;
    };

    // Transition fields / particle buffers to shader-resource
    for (auto& f : state.fields) f.Previous().Transition(cmd, kSrv);
    for (size_t pi = 0; pi < state.particles.size(); ++pi)
        if (!pack.world->particles[pi].simulate.empty())
            TransitionParticles(cmd, state.particles[pi], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->SetGraphicsRootConstantBufferView(2, paramsExtVa);
    cmd->SetGraphicsRootShaderResourceView(3, colVa);
    cmd->SetGraphicsRootDescriptorTable(4, fieldTable);
    cmd->SetGraphicsRootDescriptorTable(5, packTexTable);
    cmd->SetGraphicsRootDescriptorTable(8, shadowTable);
    // The punctual lights (root param 10 = the lights buffer, 11 / 12 = the spot / point shadow maps,
    // 13 = the binding flags). The root SRV must never be 0: without a lights buffer the collider buffer
    // (or the parameter block) is bound instead, and the shader reads no light (flag bit0 = 0).
    cmd->SetGraphicsRootShaderResourceView(10, haveLights ? pc.lights : (colVa ? colVa : paramsExtVa));
    cmd->SetGraphicsRootDescriptorTable(11, spotShadowTable);
    cmd->SetGraphicsRootDescriptorTable(12, pointShadowTable);
    cmd->SetGraphicsRoot32BitConstant(13, lightFlags, 0);
    // The ground map (t19 space0): the depth map, or the 1x1 fallback when there is none.
    cmd->SetGraphicsRootDescriptorTable(14, GroundTable(ctx));
    if (useRt) {
        // The ray-query bindings of graphicsRootSigLights (rt_common.hlsli): TLAS, geometry table, bindless heap.
        cmd->SetGraphicsRootShaderResourceView(15, pc.rt->Tlas());
        cmd->SetGraphicsRootShaderResourceView(16, pc.rt->Geometries());
        const D3D12_GPU_DESCRIPTOR_HANDLE heapBase = ctx.SrvHeap().Gpu(0);
        cmd->SetGraphicsRootDescriptorTable(17, heapBase);
        cmd->SetGraphicsRootDescriptorTable(18, heapBase);
    }

    // 1. Draw Surfaces (opaque, depth write)
    for (size_t si = 0; si < pack.world->surfaces.size(); ++si) {
        const auto& s = pack.world->surfaces[si];
        auto& sPipe = pipes.surfaces[si];
        const bool overlay = s.blend == PackWorldSurface::Blend::Overlay;
        ID3D12PipelineState* pso = nullptr;
        if (overlay)
            pso = offline ? sPipe.psoDrawOfflineOverlay.Get() : sPipe.psoDrawOverlay.Get();
        else
            pso = offline ? sPipe.psoDrawOffline.Get() : sPipe.psoDraw.Get();
        if (useRt) {
            ID3D12PipelineState* rtPso = nullptr;
            if (overlay)
                rtPso = offline ? sPipe.psoDrawOfflineRtOverlay.Get() : sPipe.psoDrawRtOverlay.Get();
            else
                rtPso = offline ? sPipe.psoDrawOfflineRt.Get() : sPipe.psoDrawRt.Get();
            if (rtPso) pso = rtPso;
        }
        if (!pso) continue;

        uint32_t fieldIndex = 0;   // the surface's "field" by name (worldParams.z)
        for (size_t fi = 0; fi < pack.world->fields.size(); ++fi)
            if (pack.world->fields[fi].name == s.field) fieldIndex = (uint32_t)fi;

        // The camera-centred grid ("extent": "camera"): 3 LOD levels of grid x grid quads. The levels tile exactly
        // only with a grid that is a multiple of 8, so the grid is rounded down to one.
        uint32_t grid = s.grid;
        uint32_t vertexCount = s.grid * s.grid * 6;
        float extentMode = 0.0f;
        if (s.extent == PackWorldSurface::Extent::Camera) {
            grid = std::max(8u, (s.grid / 8u) * 8u);
            vertexCount = 3u * grid * grid * 6u;
            extentMode = 1.0f;
        }
        surfaceParams = XMFLOAT4(extentMode, s.radius, s.ground ? 1.0f : 0.0f, 0.0f);
        wrapSize = 0.0f;
        cameraSpace = 0.0f;

        const auto cb = makeWorldCb(fieldIndex, grid);
        cmd->SetGraphicsRootConstantBufferView(1, AllocWorldCb(cb));
        cmd->SetPipelineState(pso);

        cmd->DrawInstanced(vertexCount, 1, 0, 0);
        pc.stats.drawCalls++;
    }
    surfaceParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

    // 2. Draw Particles (alpha / add, no depth write)
    if (!drawParticles) return;
    for (size_t pi = 0; pi < pack.world->particles.size(); ++pi) {
        const auto& p = pack.world->particles[pi];
        auto& pPipe = pipes.particles[pi];
        auto& pBuf = state.particles[pi];
        ID3D12PipelineState* pso = offline ? pPipe.psoDrawOffline.Get() : pPipe.psoDraw.Get();
        if (useRt) {
            ID3D12PipelineState* rtPso = offline ? pPipe.psoDrawOfflineRt.Get() : pPipe.psoDrawRt.Get();
            if (rtPso) pso = rtPso;
        }
        if (!pso) continue;

        // The camera-space wrap (world.camera): the host draws the particle in a box around the camera.
        const bool cam = p.space == PackWorldParticleSystem::Space::Camera;
        wrapSize = cam ? ParticleWrap(pack, p, inst.params) : 0.0f;
        cameraSpace = cam ? 1.0f : 0.0f;

        const auto cb = makeWorldCb((uint32_t)pi, p.count);
        cmd->SetGraphicsRootConstantBufferView(1, AllocWorldCb(cb));
        cmd->SetPipelineState(pso);

        if (!p.simulate.empty()) {
            cmd->SetGraphicsRootShaderResourceView(6, pBuf.buffer->GetGPUVirtualAddress());
            cmd->SetGraphicsRootShaderResourceView(7, pBuf.prevPos->GetGPUVirtualAddress());
        }

        const uint32_t vertexCount = p.count * 6;
        cmd->DrawInstanced(vertexCount, 1, 0, 0);
        pc.stats.drawCalls++;
    }
}

// ---- WorldPass public interface -------------------------------------------------------------

void WorldPass::Impl::DrawParticlesPost(PassContext& pc) {
    if (pc.view.worlds.empty()) return;
    if (pc.settings.shading == ViewShading::Wireframe || pc.settings.shading == ViewShading::Unlit ||
        !pc.view.extraViews.empty()) {
        return;
    }
    if (pc.path != RenderPath::Raster && pc.path != RenderPath::RayTraced) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& t = pc.targets;
    if (!t.lit || !t.depth) return;

    t.lit.Transition(cmd, kRt);
    t.depth.Transition(cmd, kSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.RtvHeap().Cpu(t.lit.rtv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float* r = pc.view.mainRect;
    const float px = std::floor(r[0] * (float)t.width), py = std::floor(r[1] * (float)t.height);
    const float pw = std::floor((r[0] + r[2]) * (float)t.width) - px;
    const float ph = std::floor((r[1] + r[3]) * (float)t.height) - py;
    D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
    D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(graphicsRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const D3D12_GPU_DESCRIPTOR_HANDLE depthTable = pc.transient.SrvTable(ctx, {&t.depth});
    // The sun / spot / point shadow maps in one table (slots 0 / 1 / 2), as in DrawInstance; bound here as well so
    // every root parameter of this signature has a value at the particle draw (the particle shaders do not read them).
    const D3D12_GPU_DESCRIPTOR_HANDLE shadowTable = pc.transient.SrvTable(
        ctx, {t.shadowMap.res ? &t.shadowMap : nullptr, t.spotShadowMap.res ? &t.spotShadowMap : nullptr,
              t.pointShadowMap.res ? &t.pointShadowMap : nullptr});
    const UINT srvInc = ctx.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_GPU_DESCRIPTOR_HANDLE spotShadowTable{shadowTable.ptr ? shadowTable.ptr + srvInc : 0};
    const D3D12_GPU_DESCRIPTOR_HANDLE pointShadowTable{shadowTable.ptr ? shadowTable.ptr + 2ull * srvInc : 0};
    uint32_t lightFlags = 0;
    if (pc.lights != 0) lightFlags |= 1u;
    if (t.spotShadowMap.res) lightFlags |= 2u;
    if (t.pointShadowMap.res) lightFlags |= 4u;

    for (const WorldInstance& inst : pc.view.worlds) {
        const ShaderPack* pack = ShaderPacks().Find(inst.pack);
        if (!pack || pack->type != PackType::World || !pack->world) continue;

        auto* pipes = GetOrCompilePipelines(ctx, *pack);
        if (!pipes) continue;

        const bool worldLights = pack->HasCap("world.lights");
        const bool useRt = worldLights && pipes->rtReady && pc.rt != nullptr;
        cmd->SetGraphicsRootSignature(useRt ? graphicsRootSigLights.Get() : graphicsRootSig.Get());

        auto& state = GetOrCreateInstanceState(ctx, inst, *pack);
        if (state.fieldDesc == DescriptorHeap::kInvalid) continue;

        const D3D12_GPU_VIRTUAL_ADDRESS paramsExtVa = AllocParamsExt(*pack, inst.params);
        D3D12_GPU_DESCRIPTOR_HANDLE packTexTable{};
        const PackTextures::Set* ptSet = packTextures.Acquire(ctx, *pack, pack->dir);
        if (ptSet && ptSet->srv != DescriptorHeap::kInvalid) {
            packTexTable = ctx.SrvHeap().Gpu(ptSet->srv);
        } else if (defaultPackTexSrv != DescriptorHeap::kInvalid) {
            packTexTable = ctx.SrvHeap().Gpu(defaultPackTexSrv);
        }

        std::vector<WorldCollider> sampledColliders;
        if (inst.colliders) SampleColliders(inst, *pack, inst.time, sampledColliders);
        const D3D12_GPU_VIRTUAL_ADDRESS colVa = AllocColliders(sampledColliders);

        const float instStepDt = pack->world->step > 0.0f ? pack->world->step : (1.0f / 60.0f);
        // The same interpolation the scene pass draws with (the particles' lit result must match the depth / colour
        // the scene pass wrote for them).
        const float interpA = (float)std::clamp(1.0 - (state.simTime - (double)inst.time) / (double)instStepDt, 0.0, 1.0);
        const float frameDelta = pc.view.sceneElapsed > 0.0f ? pc.view.sceneElapsed : 0.0f;
        UpdateAnchor(inst, state);
        float wrapSize = 0.0f;
        float cameraSpace = 0.0f;
        auto makeWorldCb = [&](uint32_t partIdx, uint32_t countOrGrid) {
            WorldCbData cb{};
            XMMATRIX mWorld = XMLoadFloat4x4(&inst.transform);
            XMMATRIX mInvWorld = XMMatrixInverse(nullptr, mWorld);
            XMStoreFloat4x4(&cb.localToWorld, mWorld);
            XMStoreFloat4x4(&cb.worldToLocal, mInvWorld);
            cb.worldTime = XMFLOAT4((float)inst.time, instStepDt, (float)state.simTime, 0.0f);
            cb.worldParams = XMFLOAT4((float)sampledColliders.size(), (float)state.fields.size(),
                                      (float)partIdx, (float)countOrGrid);
            cb.worldExtra = XMFLOAT4(wrapSize, cameraSpace, state.anchorX, state.anchorZ);
            cb.groundMap = groundMapParams;
            cb.groundRect = groundRectParams;
            cb.particleParams = XMFLOAT4(interpA, frameDelta, instStepDt, wrapSize > 0.0f ? 0.5f * wrapSize : 0.0f);

            const LightParams& lp = pc.view.light;
            XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lp.direction));
            XMFLOAT3 lNorm;
            XMStoreFloat3(&lNorm, L);
            const float hasSun = (lp.sunIntensity > 0.0f) ? 1.0f : 0.0f;
            cb.sunDir = XMFLOAT4(-lNorm.x, -lNorm.y, -lNorm.z, hasSun);
            cb.sunColor = XMFLOAT4(lp.color.x * lp.sunIntensity, lp.color.y * lp.sunIntensity,
                                   lp.color.z * lp.sunIntensity, pc.view.shadowDistance);
            cb.ambient = XMFLOAT4(lp.groundColor.x, lp.groundColor.y, lp.groundColor.z, 0.0f);

            for (size_t fi = 0; fi < std::min<size_t>(pack->world->fields.size(), 4); ++fi) {
                const auto& f = pack->world->fields[fi];
                cb.fieldExtent[fi] = XMFLOAT4((float)f.size, (float)f.size, f.extent,
                                              f.extent > 0 ? (1.0f / f.extent) : 0);
            }
            return cb;
        };

        for (auto& f : state.fields) f.Previous().Transition(cmd, kSrv);
        for (size_t pi = 0; pi < state.particles.size(); ++pi)
            if (!pack->world->particles[pi].simulate.empty())
                TransitionParticles(cmd, state.particles[pi],
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
        cmd->SetGraphicsRootConstantBufferView(2, paramsExtVa);
        cmd->SetGraphicsRootShaderResourceView(3, colVa);
        cmd->SetGraphicsRootDescriptorTable(4, FieldTable(ctx, state));
        cmd->SetGraphicsRootDescriptorTable(5, packTexTable);
        cmd->SetGraphicsRootDescriptorTable(8, shadowTable);
        cmd->SetGraphicsRootDescriptorTable(9, depthTable);
        // The punctual lights (root param 10 = the lights buffer, 11 / 12 = the spot / point shadow maps,
        // 13 = the binding flags); the root SRV is never 0 (the collider buffer stands in for it).
        cmd->SetGraphicsRootShaderResourceView(10, pc.lights ? pc.lights : (colVa ? colVa : paramsExtVa));
        cmd->SetGraphicsRootDescriptorTable(11, spotShadowTable);
        cmd->SetGraphicsRootDescriptorTable(12, pointShadowTable);
        cmd->SetGraphicsRoot32BitConstant(13, lightFlags, 0);
        cmd->SetGraphicsRootDescriptorTable(14, GroundTable(ctx));
        if (useRt) {
            cmd->SetGraphicsRootShaderResourceView(15, pc.rt->Tlas());
            cmd->SetGraphicsRootShaderResourceView(16, pc.rt->Geometries());
            const D3D12_GPU_DESCRIPTOR_HANDLE heapBase = ctx.SrvHeap().Gpu(0);
            cmd->SetGraphicsRootDescriptorTable(17, heapBase);
            cmd->SetGraphicsRootDescriptorTable(18, heapBase);
        }

        for (size_t pi = 0; pi < pack->world->particles.size(); ++pi) {
            const auto& p = pack->world->particles[pi];
            auto& pPipe = pipes->particles[pi];
            auto& pBuf = state.particles[pi];
            if (!pPipe.psoDrawPost) continue;

            // The camera-space wrap (world.camera): the host draws the particle in a box around the camera.
            const bool cam = p.space == PackWorldParticleSystem::Space::Camera;
            wrapSize = cam ? ParticleWrap(*pack, p, inst.params) : 0.0f;
            cameraSpace = cam ? 1.0f : 0.0f;

            const auto cb = makeWorldCb((uint32_t)pi, p.count);
            cmd->SetGraphicsRootConstantBufferView(1, AllocWorldCb(cb));
            ID3D12PipelineState* drawPso = pPipe.psoDrawPost.Get();
            if (useRt && pPipe.psoDrawPostRt) drawPso = pPipe.psoDrawPostRt.Get();
            cmd->SetPipelineState(drawPso);

            if (!p.simulate.empty()) {
                cmd->SetGraphicsRootShaderResourceView(6, pBuf.buffer->GetGPUVirtualAddress());
                cmd->SetGraphicsRootShaderResourceView(7, pBuf.prevPos->GetGPUVirtualAddress());
            }

            const uint32_t vertexCount = p.count * 6;
            cmd->DrawInstanced(vertexCount, 1, 0, 0);
            pc.stats.drawCalls++;
        }
    }

    t.lit.Transition(cmd, kSrv);
}

WorldPass::WorldPass() : impl_(std::make_unique<Impl>()) {}
WorldPass::~WorldPass() = default;

bool WorldPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    return impl_->Init(ctx, shaderDir, msaa);
}

void WorldPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    impl_->EnsureRings(ctx);
}

void WorldPass::ReleaseTargets(Dx12Context& ctx) {
    impl_->packTextures.Clear(ctx);
    impl_->ReleaseGroundResources(ctx);
    for (auto& [k, inst] : impl_->instances) {
        for (auto& f : inst.fields) {
            f.texA.Release(ctx);
            f.texB.Release(ctx);
        }
        for (auto& b : inst.particles) {
            if (b.buffer) ctx.DeferRelease(b.buffer);
            if (b.prevPos) ctx.DeferRelease(b.prevPos);
        }
        if (inst.fieldDesc != DescriptorHeap::kInvalid)
            ctx.SrvHeap().Free(inst.fieldDesc, Impl::InstanceState::kFieldDescCount);
    }
    impl_->instances.clear();
}

void WorldPass::Execute(PassContext& pc) {
    // Zero-cost rule: when no world instances exist, return immediately with zero GPU work
    if (pc.view.worlds.empty()) return;

    // View shading: unlit / wireframe / quad views skip world drawing per spec
    if (pc.settings.shading == ViewShading::Wireframe || pc.settings.shading == ViewShading::Unlit ||
        !pc.view.extraViews.empty()) {
        return;
    }

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& t = pc.targets;

    // 0. The ground map (world.ground): the stage's real surfaces, before the simulation.
    impl_->EnsureGround(pc);

    // 1. Simulation Phase (Compute)
    for (const WorldInstance& inst : pc.view.worlds) {
        const ShaderPack* pack = ShaderPacks().Find(inst.pack);
        if (!pack || pack->type != PackType::World || !pack->world) continue;

        auto* pipes = impl_->GetOrCompilePipelines(ctx, *pack);
        if (!pipes) continue;

        auto& state = impl_->GetOrCreateInstanceState(ctx, inst, *pack);
        impl_->SimulateInstance(pc, inst, *pack, *pipes, state);
    }

    // 2. Render Phase (Graphics)
    // Setup Render Targets
    if (pc.path == RenderPath::Raster || pc.path == RenderPath::RayTraced) {
        t.colorMsaa.Transition(cmd, kRt);
        t.normalMsaa.Transition(cmd, kRt);
        t.velocityMsaa.Transition(cmd, kRt);
        t.depthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
            ctx.RtvHeap().Cpu(t.colorMsaa.rtv),
            ctx.RtvHeap().Cpu(t.normalMsaa.rtv),
            ctx.RtvHeap().Cpu(t.velocityMsaa.rtv)
        };
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(t.depthMsaa.dsv);
        cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

        const float* r = pc.view.mainRect;
        const float px = std::floor(r[0] * (float)t.width), py = std::floor(r[1] * (float)t.height);
        const float pw = std::floor((r[0] + r[2]) * (float)t.width) - px;
        const float ph = std::floor((r[1] + r[3]) * (float)t.height) - py;
        D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
        D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);

        for (const WorldInstance& inst : pc.view.worlds) {
            const ShaderPack* pack = ShaderPacks().Find(inst.pack);
            if (!pack || pack->type != PackType::World || !pack->world) continue;

            auto* pipes = impl_->GetOrCompilePipelines(ctx, *pack);
            if (!pipes) continue;

            auto& state = impl_->GetOrCreateInstanceState(ctx, inst, *pack);
            impl_->DrawInstance(pc, inst, *pack, *pipes, state, false, false);   // surfaces only; particles draw after composite
        }
    } else if (pc.path == RenderPath::PathTraced && t.ptEdges) {
        // In PathTraced mode, ScenePass::DrawPtEdges prepared colorMsaa, normalMsaa, velocityMsaa, depthMsaa
        t.colorMsaa.Transition(cmd, kRt);
        t.normalMsaa.Transition(cmd, kRt);
        t.velocityMsaa.Transition(cmd, kRt);
        t.depthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
            ctx.RtvHeap().Cpu(t.colorMsaa.rtv),
            ctx.RtvHeap().Cpu(t.normalMsaa.rtv),
            ctx.RtvHeap().Cpu(t.velocityMsaa.rtv)
        };
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(t.depthMsaa.dsv);
        cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

        const float* r = pc.view.mainRect;
        const float px = std::floor(r[0] * (float)t.width), py = std::floor(r[1] * (float)t.height);
        const float pw = std::floor((r[0] + r[2]) * (float)t.width) - px;
        const float ph = std::floor((r[1] + r[3]) * (float)t.height) - py;
        D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
        D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);

        for (const WorldInstance& inst : pc.view.worlds) {
            const ShaderPack* pack = ShaderPacks().Find(inst.pack);
            if (!pack || pack->type != PackType::World || !pack->world) continue;

            auto* pipes = impl_->GetOrCompilePipelines(ctx, *pack);
            if (!pipes) continue;

            auto& state = impl_->GetOrCreateInstanceState(ctx, inst, *pack);
            impl_->DrawInstance(pc, inst, *pack, *pipes, state, false);
        }
    }
}

void WorldPass::DrawParticlesPost(PassContext& pc) { impl_->DrawParticlesPost(pc); }

void WorldPass::DrawOffline(PassContext& pc, const Texture& edgeColorMsaa, const Texture& edgeDepthMsaa,
                            const DirectX::XMFLOAT4X4* iterationViewProj) {
    if (pc.view.worlds.empty()) return;
    impl_->offlineVpSet = iterationViewProj != nullptr;
    if (iterationViewProj) impl_->offlineVp = *iterationViewProj;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;

    // The ground map (world.ground), before the simulation.
    impl_->EnsureGround(pc);

    // Simulate instances
    for (const WorldInstance& inst : pc.view.worlds) {
        const ShaderPack* pack = ShaderPacks().Find(inst.pack);
        if (!pack || pack->type != PackType::World || !pack->world) continue;

        auto* pipes = impl_->GetOrCompilePipelines(ctx, *pack);
        if (!pipes) continue;

        auto& state = impl_->GetOrCreateInstanceState(ctx, inst, *pack);
        impl_->SimulateInstance(pc, inst, *pack, *pipes, state);
    }

    // Set render targets for offline edge layer (1 RTV, 4 MSAA)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.RtvHeap().Cpu(edgeColorMsaa.rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(edgeDepthMsaa.dsv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp{0.0f, 0.0f, (float)edgeColorMsaa.width, (float)edgeColorMsaa.height, 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, (LONG)edgeColorMsaa.width, (LONG)edgeColorMsaa.height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    for (const WorldInstance& inst : pc.view.worlds) {
        const ShaderPack* pack = ShaderPacks().Find(inst.pack);
        if (!pack || pack->type != PackType::World || !pack->world) continue;

        auto* pipes = impl_->GetOrCompilePipelines(ctx, *pack);
        if (!pipes) continue;

        auto& state = impl_->GetOrCreateInstanceState(ctx, inst, *pack);
        impl_->DrawInstance(pc, inst, *pack, *pipes, state, true);
    }
}

} // namespace mmdx
