#pragma once
// GPU-side representation of one PMX model: static vertex/index buffers, per-material
// constants and descriptor tables, plus per-frame-slot dynamic buffers for skinning
// matrices and morph deltas.
#include "render/Dx12Context.h"
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

class GpuModel {
public:
    struct Material {
        uint32_t indexStart = 0, indexCount = 0;
        uint32_t srvTable = DescriptorHeap::kInvalid;  // 3 consecutive SRVs in ctx.SrvHeap(): t1 texture, t2 sphere, t3 toon
        D3D12_GPU_VIRTUAL_ADDRESS constants = 0;       // MaterialConstants (256 B)
        bool doubleSided = false;
        bool drawEdge = false;
        float edgeSize = 0;
    };

    GpuModel() = default;
    ~GpuModel();
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    // `textures` is parallel to pmx.textures; an empty image means "missing" -> white.
    // Creates GPU textures only for indices actually referenced by materials.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin);
    // Frees descriptors. Caller must ensure the GPU no longer uses the model (WaitForGpu).
    void Destroy();

    // memcpy into the persistently mapped buffer of `frameSlot`. `skin.size()` must equal BoneCount().
    void UpdateSkinning(uint32_t frameSlot, const std::vector<DirectX::XMFLOAT4X4>& skin);
    // Copies deltas only if `version` differs from the version last written to this slot.
    void UpdateMorphs(uint32_t frameSlot, const std::vector<DirectX::XMFLOAT3>& deltas, uint64_t version);

    const std::string& Name() const { return name_; }
    uint32_t BoneCount() const { return boneCount_; }
    uint32_t VertexCount() const { return vertexCount_; }
    uint32_t IndexCount() const { return indexCount_; }
    const std::vector<Material>& Materials() const { return materials_; }
    const D3D12_VERTEX_BUFFER_VIEW& VertexBufferView() const { return vbv_; }
    const D3D12_VERTEX_BUFFER_VIEW& MorphBufferView(uint32_t frameSlot) const { return morphVbv_[frameSlot]; }
    const D3D12_INDEX_BUFFER_VIEW& IndexBufferView() const { return ibv_; }
    D3D12_GPU_VIRTUAL_ADDRESS BoneBuffer(uint32_t frameSlot) const;  // StructuredBuffer<float4x4> (t0)

private:
    Dx12Context* ctx_ = nullptr;
    std::string name_;
    uint32_t boneCount_ = 0, vertexCount_ = 0, indexCount_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, materialCb_;
    std::vector<ComPtr<ID3D12Resource>> textures_;  // parallel to pmx.textures (null if unused)
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    ComPtr<ID3D12Resource> boneBuf_[Dx12Context::kFramesInFlight];
    void* boneMapped_[Dx12Context::kFramesInFlight] = {};
    ComPtr<ID3D12Resource> morphBuf_[Dx12Context::kFramesInFlight];
    void* morphMapped_[Dx12Context::kFramesInFlight] = {};
    uint64_t morphVersion_[Dx12Context::kFramesInFlight] = {};
    D3D12_VERTEX_BUFFER_VIEW morphVbv_[Dx12Context::kFramesInFlight]{};
    std::vector<Material> materials_;
    std::vector<uint32_t> srvAllocations_;  // srvTable starts to free
};

} // namespace mmdx
