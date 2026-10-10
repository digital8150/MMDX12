#pragma once
// GPU-side representation of one PMX model: static vertex/index buffers, per-material
// constants and descriptor tables, plus a ring of dynamic buffers for skinning matrices and
// morph deltas. The ring holds kRing frames so the previous frame's pose stays readable while
// the GPU may still be working on it (motion vectors need it).
#include "render/Dx12Context.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <string>
#include <vector>

namespace mmdx {

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
        D3D12_GPU_VIRTUAL_ADDRESS constants = 0;       // MaterialConstants (256 B), the current ring entry
        bool doubleSided = false;
        bool drawEdge = false;       // edge flag, size > 0, edge alpha > 0, material visible (material morphs)
        bool castShadow = false;     // PMX self-shadow-map / ground-shadow flags, material visible
        bool alphaTested = false;    // texture has alpha: shadow pass samples it
        bool visible = true;         // diffuse alpha > 0 (MMD skips invisible materials, edges included)
        bool morphable = false;      // a material morph targets it (constants change at run time)
        bool packOff = false;        // shader pack switched off for this material (ShaderChoice::materials): default shading
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
    // Material morphs (ModelInstance::MaterialMul/MaterialAdd/MaterialVersion): final = base * mul + add.
    // Empty vectors mean identity. Rewrites ring entry frame % kRing when it holds another version and
    // points Materials()[i].constants at it, so call it every frame like UpdateSkinning.
    void UpdateMaterials(uint64_t frame, const std::vector<PmxMorph::MaterialOffset>& mul,
                         const std::vector<PmxMorph::MaterialOffset>& add, uint64_t version);

    // Shader pack (render/ShaderPack.h): nullptr = the default shading. Writes the material class, head bone and
    // parameters into the material constants; a no-op when nothing changed, so call it every frame before
    // UpdateMaterials. The scene pass draws the model with the pack's PSOs while ShaderPackId() is not empty.
    // `textureFolder` is the model's resolved per-character texture folder (empty = the pack folder only).
    void SetShaderPack(const ShaderPack* pack, const PackParamValues& params, const std::filesystem::path& textureFolder,
                       const MaterialClassOverrides& materials = {});
    const std::string& ShaderPackId() const { return packId_; }
    // the pack shades at least one material (false without a pack, or with every material switched off)
    bool PackActive() const;
    const std::filesystem::path& ShaderTextureFolder() const { return packTextureFolder_; }

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
    // Slot 3: GpuSdef per vertex (stride 0, one zero element, when the model has no SDEF vertex).
    const D3D12_VERTEX_BUFFER_VIEW& SdefBufferView() const { return sdefVbv_; }
    bool HasSdef() const { return hasSdef_; }
    ID3D12Resource* SdefBuffer() const { return sdef_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS BoneBuffer(uint64_t frame) const;      // StructuredBuffer<float4x4> (t0)
    D3D12_GPU_VIRTUAL_ADDRESS PrevBoneBuffer(uint64_t frame) const;  // previous frame (t4)
    const DirectX::XMFLOAT4X4* BoneMatricesCpu(uint64_t frame) const {
        return reinterpret_cast<const DirectX::XMFLOAT4X4*>(boneMapped_[frame % kRing]);
    }
    const DirectX::XMFLOAT4X4* PrevBoneMatricesCpu(uint64_t frame) const {
        return reinterpret_cast<const DirectX::XMFLOAT4X4*>(boneMapped_[(frame + kRing - 1) % kRing]);
    }
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
    // CPU copy of the current per-material constants (parallel to Materials(); material morphs applied).
    const std::vector<MaterialConstants>& MaterialConstantsCpu() const { return materialConsts_; }

    // Ray-tracing resources, created and owned by RtScene (render/RayTracing.cpp) on first use
    // and released with the model.
    struct RtResources {
        ComPtr<ID3D12Resource> vertices;      // DEFAULT heap, RtVertex[VertexCount()], UAV-capable
        D3D12_RESOURCE_STATES verticesState = D3D12_RESOURCE_STATE_COMMON;
        // Two BLAS parts so closest-hit rays can cull back faces like the raster pass:
        // [0] single-sided materials (culling on), [1] double-sided (instance TRIANGLE_CULL_DISABLE).
        struct BlasPart {
            ComPtr<ID3D12Resource> blas, scratch; // result (RAYTRACING_ACCELERATION_STRUCTURE) + scratch (UAV)
            uint64_t blasBytes = 0, scratchBytes = 0;
        };
        BlasPart parts[2];
        uint32_t srv = DescriptorHeap::kInvalid;  // 2 SrvHeap descriptors: +0 raw RtVertex SRV, +1 raw index SRV
        // Material index of each BLAS geometry: part 0's, then part 1's starting at partSplit.
        std::vector<uint32_t> geometryMaterials;
        uint32_t partSplit = 0;
        bool blasBuilt = false;
    };
    RtResources& Rt() { return rt_; }

private:
    Dx12Context* ctx_ = nullptr;
    std::string name_;
    uint32_t boneCount_ = 0, vertexCount_ = 0, indexCount_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, sdef_;
    // MaterialConstants ring (UPLOAD heap, persistently mapped): kRing entries of materialCount * 256 B.
    ComPtr<ID3D12Resource> materialCb_;
    uint8_t* materialMapped_ = nullptr;
    uint64_t materialEntryVersion_[kRing] = {};
    uint64_t materialVersion_ = 0;              // version of materialConsts_
    std::vector<MaterialConstants> baseConsts_; // without material morphs
    struct MaterialBase { bool edgeFlag = false, castFlag = false; };
    std::vector<MaterialBase> materialBase_;
    std::vector<ComPtr<ID3D12Resource>> textures_;  // parallel to pmx.textures (null if unused)
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    D3D12_VERTEX_BUFFER_VIEW sdefVbv_{};
    bool hasSdef_ = false;
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
    // shader pack state
    std::vector<std::pair<std::string, std::string>> materialNames_;  // (name, nameEn) per material
    std::vector<std::string> materialTextures_;                       // diffuse texture path per material (pack classes)
    int32_t headBone_ = -1;
    DirectX::XMFLOAT3 headPos_{};
    std::string packId_;
    uint32_t packGeneration_ = 0;
    std::filesystem::path packTextureFolder_;
    PackParamValues packParams_{};
    MaterialClassOverrides packOverrides_{};
};

} // namespace mmdx
