#pragma once
// GPU-side representation of one PMX model: static vertex/index buffers, per-material
// constants and descriptor tables, plus a ring of dynamic buffers for skinning matrices and
// morph deltas. The ring holds kRing frames so the previous frame's pose stays readable while
// the GPU may still be working on it (motion vectors need it).
#include "render/Dx12Context.h"
#include "render/ShaderInterop.h"
#include <DirectXMath.h>
#include <string>
#include <vector>

namespace mmdx {

struct PmxModel;
struct ImageRGBA8;

// Textures shared by all models, owned by Renderer.
struct BuiltinTextures {
    ComPtr<ID3D12Resource> white;      // 1x1 (255,255,255,255)
    ComPtr<ID3D12Resource> toon[10];   // generated stand-ins for MMD's toon01..toon10.bmp
};

enum class ModelRole : uint8_t { Character = 0, Stage = 1 };

class GpuModel {
public:
    static constexpr uint32_t kRing = Dx12Context::kFramesInFlight + 1;

    struct Material {
        uint32_t indexStart = 0, indexCount = 0;
        uint32_t srvTable = DescriptorHeap::kInvalid;  // 3 consecutive SRVs in ctx.SrvHeap(): t1 texture, t2 sphere, t3 toon
        D3D12_GPU_VIRTUAL_ADDRESS constants = 0;       // MaterialConstants (256 B)
        bool doubleSided = false;
        bool drawEdge = false;
        bool castShadow = false;     // PMX self-shadow-map / ground-shadow flags
        bool alphaTested = false;    // texture has alpha: shadow pass samples it
        float edgeSize = 0;
    };

    GpuModel() = default;
    ~GpuModel();
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    // `textures` is parallel to pmx.textures; an empty image means "missing" -> white.
    // Creates GPU textures only for indices actually referenced by materials.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role);
    // Frees descriptors. Caller must ensure the GPU no longer uses the model (WaitForGpu).
    void Destroy();

    // `frame` is ctx.FrameNumber(); the ring entry is frame % kRing. The first call after
    // Create() fills every ring entry so the "previous" pose is valid immediately.
    // `skin.size()` must equal BoneCount().
    void UpdateSkinning(uint64_t frame, const std::vector<DirectX::XMFLOAT4X4>& skin);
    // Copies deltas only if `version` differs from the version last written to this entry.
    void UpdateMorphs(uint64_t frame, const std::vector<DirectX::XMFLOAT3>& deltas, uint64_t version);

    const std::string& Name() const { return name_; }
    ModelRole Role() const { return role_; }
    uint32_t BoneCount() const { return boneCount_; }
    uint32_t VertexCount() const { return vertexCount_; }
    uint32_t IndexCount() const { return indexCount_; }
    const std::vector<Material>& Materials() const { return materials_; }
    const D3D12_VERTEX_BUFFER_VIEW& VertexBufferView() const { return vbv_; }
    const D3D12_VERTEX_BUFFER_VIEW& MorphBufferView(uint64_t frame) const { return morphVbv_[frame % kRing]; }
    const D3D12_VERTEX_BUFFER_VIEW& PrevMorphBufferView(uint64_t frame) const { return morphVbv_[(frame + kRing - 1) % kRing]; }
    const D3D12_INDEX_BUFFER_VIEW& IndexBufferView() const { return ibv_; }
    D3D12_GPU_VIRTUAL_ADDRESS BoneBuffer(uint64_t frame) const;      // StructuredBuffer<float4x4> (t0)
    D3D12_GPU_VIRTUAL_ADDRESS PrevBoneBuffer(uint64_t frame) const;  // previous frame (t4)
    // Bind-pose bounds (model space).
    DirectX::XMFLOAT3 BoundsMin() const { return boundsMin_; }
    DirectX::XMFLOAT3 BoundsMax() const { return boundsMax_; }

    // Static buffers. vb: GpuVertex[], state VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE.
    // ib: uint32[], state INDEX_BUFFER | NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE (also read
    // by ray queries through a raw SRV).
    ID3D12Resource* VertexBuffer() const { return vb_.Get(); }
    ID3D12Resource* IndexBuffer() const { return ib_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS MorphBuffer(uint64_t frame) const { return morphVbv_[frame % kRing].BufferLocation; }
    D3D12_GPU_VIRTUAL_ADDRESS PrevMorphBuffer(uint64_t frame) const { return morphVbv_[(frame + kRing - 1) % kRing].BufferLocation; }
    // CPU copy of the per-material constants (parallel to Materials()).
    const std::vector<MaterialConstants>& MaterialConstantsCpu() const { return materialConsts_; }

    // Ray-tracing resources, created and owned by RtScene (render/RayTracing.cpp) on first use
    // and released with the model.
    struct RtResources {
        ComPtr<ID3D12Resource> vertices;      // DEFAULT heap, RtVertex[VertexCount()], UAV-capable
        D3D12_RESOURCE_STATES verticesState = D3D12_RESOURCE_STATE_COMMON;
        ComPtr<ID3D12Resource> blas, scratch; // BLAS result (RAYTRACING_ACCELERATION_STRUCTURE) + scratch (UAV)
        uint64_t blasBytes = 0, scratchBytes = 0;
        uint32_t srv = DescriptorHeap::kInvalid;  // 2 SrvHeap descriptors: +0 raw RtVertex SRV, +1 raw index SRV
        std::vector<uint32_t> geometryMaterials;  // material index of each BLAS geometry, in order
        bool blasBuilt = false;
    };
    RtResources& Rt() { return rt_; }

private:
    Dx12Context* ctx_ = nullptr;
    std::string name_;
    uint32_t boneCount_ = 0, vertexCount_ = 0, indexCount_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, materialCb_;
    std::vector<ComPtr<ID3D12Resource>> textures_;  // parallel to pmx.textures (null if unused)
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    ModelRole role_ = ModelRole::Character;
    DirectX::XMFLOAT3 boundsMin_{}, boundsMax_{};
    ComPtr<ID3D12Resource> boneBuf_[kRing];
    void* boneMapped_[kRing] = {};
    ComPtr<ID3D12Resource> morphBuf_[kRing];
    void* morphMapped_[kRing] = {};
    uint64_t morphVersion_[kRing] = {};
    D3D12_VERTEX_BUFFER_VIEW morphVbv_[kRing]{};
    bool skinInitialized_ = false, morphInitialized_ = false;
    std::vector<Material> materials_;
    std::vector<MaterialConstants> materialConsts_;
    std::vector<uint32_t> srvAllocations_;  // srvTable starts to free
    RtResources rt_;
};

} // namespace mmdx
