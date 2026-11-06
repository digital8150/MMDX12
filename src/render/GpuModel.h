#pragma once
// GPU-side representation of one PMX model: static vertex/index buffers, per-material
// constants and descriptor tables, plus a ring of dynamic buffers for skinning matrices and
// morph deltas. The ring holds kRing frames so the previous frame's pose stays readable while
// the GPU may still be working on it (motion vectors need it).
#include "render/Dx12Context.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "asset/ModelTraits.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmdx {

struct ImageRGBA8;
struct CompressedImage;

// Textures shared by all models, owned by Renderer.
struct BuiltinTextures {
    ComPtr<ID3D12Resource> white;      // 1x1 (255,255,255,255)
    ComPtr<ID3D12Resource> toon[10];   // generated stand-ins for MMD's toon01..toon10.bmp
};

enum class ModelRole : uint8_t { Character = 0, Stage = 1 };

// One material's PBR record values for this frame (MMDX stage `materials.<rule>.<field>` targets, docs/stage_format.md).
// A set field replaces the record's value: baseColor is LINEAR and stored sRGB-encoded like an authored constant,
// roughness goes to the record's smoothness slot as linear roughness (TYPE 2, like imported glTF), metallic to metalness,
// emissive is linear and already multiplied by its strength, alpha is the record's diffuseAlpha (coverage).
// S2a: transmission / ior write the record's glTF extension block (raster / RT blend the surface see-through until S2b
// refracts it). castShadow / receiveShadow are the material's shadow flags, not record fields.
struct PbrOverride {
    size_t material = 0;
    std::optional<DirectX::XMFLOAT3> baseColor;
    std::optional<float> roughness;
    std::optional<float> metallic;
    std::optional<DirectX::XMFLOAT3> emissive;
    std::optional<float> alpha;
    std::optional<float> transmission;
    std::optional<float> ior;
    std::optional<bool> castShadow;
    std::optional<bool> receiveShadow;
    std::optional<bool> mirror;   // S10: MatFlag_Mirror (a planar mirror; see GpuModel::MirrorPlanes)
};

// S10 planar mirror: the plane a mirror material's rest-pose triangles lie on (model space), fitted once at load.
// `valid` false = the material's triangles are not coplanar (1 % of the material's extent) or degenerate: the material
// still shows its reflection tint, but no reflection is rendered for it.
struct MirrorPlane {
    size_t material = 0;                  // index into GpuModel::Materials()
    DirectX::XMFLOAT3 centroid{0, 0, 0};
    DirectX::XMFLOAT3 normal{0, 1, 0};    // unit, model space
    float area = 0.0f;                    // rest-pose triangle area (model space): ranks the frame's two slots
    bool valid = false;
};

// S8b raster / RT screen lights: the rectangle an S5 screen material's rest-pose triangles fit into (model space),
// fitted once at load and transformed by the part's root each frame (Renderer::FillScreenLights). A screen whose
// triangles are not planar (a curved LED wall) keeps the single fitted rectangle: the area-light term is an
// approximation there, and the plan caps a screen at one rectangle.
struct ScreenRect {
    size_t material = 0;                  // index into GpuModel::Materials()
    DirectX::XMFLOAT3 center{0, 0, 0};    // area-weighted centroid
    DirectX::XMFLOAT3 normal{0, 0, 1};    // unit, model space: the side the screen shines from
    DirectX::XMFLOAT3 axisA{1, 0, 0};     // unit in-plane axes (model space)
    DirectX::XMFLOAT3 axisB{0, 1, 0};
    float halfA = 0.0f, halfB = 0.0f;     // half extents along axisA / axisB
    float area = 0.0f;                    // rest-pose triangle area (model space): ranks the frame's screen lights
    bool valid = false;                   // false = degenerate (no lit rectangle)
};

// Per-material PBR source (MaterialSource) and the PBR debug channel (PbrDebug) live in render/RenderTypes.h
// (with the rest of the render settings); GpuModel reads them through RenderSettings.

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
        bool additive = false;       // the model's own .emd effect is additive: ScenePass uses the additive PSO
        bool noDepthWrite = false;   // the model's own .emd effect draws with ZWriteEnable = false (no depth write)
        uint8_t sphereMode = 0;      // PMX sphere mode (0 none, 1 multiply, 2 add, 3 subtexture): MME UseSphereMap
        float edgeSize = 0;
    };

    GpuModel();
    ~GpuModel();
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    // `textures` is parallel to pmx.textures; an empty image means "missing" -> white.
    // Creates GPU textures only for indices actually referenced by materials.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role);
    // `pbr` is the model's decoded Ray PBR payload (PbrModelData, may be empty / all-invalid). Creates the
    // per-material StructuredBuffer<GpuPbrMaterial> and its SRV when at least one material has a record.
    // `compressed` (S11) is parallel to `textures`: a non-empty entry is a block-compressed DDS the model's texture
    // upload takes as it is (no RGBA8 copy on the GPU). The RGBA8 image supplies the CPU-side checks (alpha test /
    // toon ramp) and is uploaded when the entry is empty or not a colour block format; a BC4/5/6H file (no RGBA8
    // decode) counts as missing there.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role,
                const PbrModelData& pbr);
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role,
                const PbrModelData& pbr, const std::vector<CompressedImage>* compressed);
    // Per-material .emd blend hints (asset/MmeHints::MmeBlendHint: 0 normal, 1 no depth write, 2 additive). Call
    // after Create; sets Material::additive / Material::noDepthWrite and the MatFlag_* bits in the constants.
    // `def` applies to materials past the end of `hints` that have no own entry. Never changes ordinary models.
    void SetMaterialBlendHints(const std::vector<uint8_t>& hints, uint8_t def);
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
    // `opacity` is a per-model multiplier on every material's diffuse alpha (studio accessories, 1 = unchanged);
    // it is folded into the version, so 1 stays bit-identical to the call without it.
    void UpdateMaterials(uint64_t frame, const std::vector<PmxMorph::MaterialOffset>& mul,
                         const std::vector<PmxMorph::MaterialOffset>& add, uint64_t version, float opacity = 1.0f);

    // Shader pack (render/ShaderPack.h): nullptr = the default shading. Writes the material class, head bone and
    // parameters into the material constants; a no-op when nothing changed, so call it every frame before
    // UpdateMaterials. The scene pass draws the model with the pack's PSOs while ShaderPackId() is not empty.
    // `textureFolder` is the model's resolved per-character texture folder (empty = the pack folder only); `rawParams`
    // (key -> value, typed components as key#0..2) feed the params.ext slots.
    void SetShaderPack(const ShaderPack* pack, const PackParamValues& params, const std::filesystem::path& textureFolder,
                       const MaterialClassOverrides& materials = {}, const std::map<std::string, float>& rawParams = {});
    void SetShaderPack(const ShaderPack* pack, const std::map<std::string, float>& rawParams,
                       const std::filesystem::path& textureFolder, const MaterialClassOverrides& materials = {}) {
        SetShaderPack(pack, pack ? pack->Resolve(rawParams) : PackParamValues{}, textureFolder, materials, rawParams);
    }
    const std::string& ShaderPackId() const { return packId_; }
    // the pack shades at least one material (false without a pack, or with every material switched off)
    bool PackActive() const;
    const std::filesystem::path& ShaderTextureFolder() const { return packTextureFolder_; }
    bool HasPackParamsExt() const { return hasPackParamsExt_; }
    D3D12_GPU_VIRTUAL_ADDRESS PackParamsExtBuffer(uint64_t frame) const;
    const std::vector<DirectX::XMFLOAT4>& PackParamSlots() const { return packParamsExtSlots_; }

    // Background AVI (背景AVI): materials whose diffuse texture file name is screen.bmp show the current video
    // frame. True when the model has any (render/ScreenVideo.h binds the texture; the renderer keys bind state by
    // Uid()). `tex` != null rewrites srvTable + 0 and sets MatFlag_HasTexture; null restores the model's own
    // texture. Waits for the GPU at the caller (descriptor rewrite).
    bool HasScreenMaterials() const { return !screenMaterials_.empty(); }
    void BindScreenTexture(Dx12Context& ctx, ID3D12Resource* tex);
    // S5: the per-material form of the above (render/StageScreens.h). Rewrites material `materialIndex`'s diffuse
    // descriptor (srvTable + 0) to `tex` and sets MatFlag_HasTexture, or restores the model's own texture / flag
    // when `tex` is null. A material without its own texture and no `tex` keeps the white fallback. The caller must
    // wait for the GPU (a descriptor read by frames in flight is rewritten).
    void BindMaterialTexture(Dx12Context& ctx, size_t materialIndex, ID3D12Resource* tex);
    // S5: marks a material as a stage screen surface (MatFlag_Screen: the bound texture is the emission). The flag
    // and the brightness (kept in the reflectivity slot, which a screen's shading reads as its exposure) live in the
    // material constants, so call it before the frame's UpdateMaterials; `on` false restores the shading.
    void SetMaterialScreen(size_t materialIndex, bool on, float brightness);
    // S5: writes the current material constants into ring entry `frame % kRing` now (the screen bindings change the
    // constants after the frame's UpdateMaterials ran, before the passes draw).
    void FlushMaterialConstants(uint64_t frame);

    // The model's default pack texture folder: where its face SDF / light maps live (found through the model's own MME
    // files by asset/MmeHints), used when a ShaderChoice has no explicit textureFolder. Empty when none was found.
    void SetDefaultPackTextureFolder(const std::filesystem::path& folder) { defaultPackTextureFolder_ = folder; }
    const std::filesystem::path& DefaultPackTextureFolder() const { return defaultPackTextureFolder_; }

    // M6a: per-material MME object effects (docs/mme/mme_design.md §2.12 / §2.13). `perMaterial` is parallel to
    // Materials(); an empty path = native shading for that material, an empty vector = no MME at all. An MME object
    // effect wins over a shader pack for the materials it owns. Mirrors SetShaderPack.
    void SetMmeEffects(std::vector<std::filesystem::path> perMaterial);
    // The effect file owning `material` (empty = native shading). The reference is stable for the model's lifetime.
    const std::filesystem::path& MmeEffectOf(size_t material) const {
        static const std::filesystem::path kEmpty;
        return material < mmeEffects_.size() ? mmeEffects_[material] : kEmpty;
    }
    bool HasMmeEffects() const;
    // The model's effect files, unique, in material order.
    std::vector<std::filesystem::path> MmeEffectFiles() const;
    bool MmeMaterial(size_t i) const { return i < mmeEffects_.size() && !mmeEffects_[i].empty(); }
    // M5c: the mesh of an MME effect object (an accessory `.x` its effect owns: particles, floors, sprites). Only the
    // MME draws show it: it stays out of the ray-traced scene (TLAS) and the offline depth / edge passes.
    void SetMmeEffectMesh(bool on) { mmeEffectMesh_ = on; }
    bool MmeEffectMesh() const { return mmeEffectMesh_; }
    // M4b CLI wrapper: one effect for the listed materials (empty `materialMask` = every material).
    void SetMmeEffect(const std::filesystem::path& fx, std::vector<int> materialMask = {});
    // Per-material TOONCOLOR (spec §4.2): the bottom-left texel of the material's toon texture (PMX toon or
    // built-in toon01..10), white when none. Read at model load; parallel to Materials().
    const DirectX::XMFLOAT3& MmeToonColor(size_t i) const {
        static const DirectX::XMFLOAT3 kWhite{1, 1, 1};
        return i < mmeToonColor_.size() ? mmeToonColor_[i] : kWhite;
    }

    const std::string& Name() const { return name_; }
    // Per-process unique id (1, 2, ...), stable for the model's lifetime. MME caches key by this instead of the
    // address: a freed model's address may be reused by a new model, which would otherwise hit a stale cache.
    uint64_t Uid() const { return uid_; }
    ModelRole Role() const { return role_; }
    // M8a: an animated stage (a song-bundled stage with a stage motion) is not a static stage: its BLAS is rebuilt
    // when it moves and its own shaders shade it as geometry that can move. Set after creation (stages are built
    // static, then a motion may be bound).
    void SetDynamic(bool dynamic) { dynamic_ = dynamic; }
    bool Dynamic() const { return dynamic_; }
    // A prop (a studio accessory): drawn like a character (it follows bones), but the world's ground map includes it
    // (it is a real surface the effects may lie on) while the performers never are.
    void SetProp(bool prop) { prop_ = prop; }
    bool IsProp() const { return prop_; }
    uint32_t BoneCount() const { return boneCount_; }
    // The material's PMX name (display name; nameEn only when the PMX has no display name is not merged: this is
    // the name material globs match, as StageGlobMatch does for `materials`).
    const std::string& MaterialName(size_t index) const {
        static const std::string kEmpty;
        return index < materialNames_.size() ? materialNames_[index].first : kEmpty;
    }
    const std::string& MaterialNameEn(size_t index) const {
        static const std::string kEmpty;
        return index < materialNames_.size() ? materialNames_[index].second : kEmpty;
    }
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
    // Slot 4: GpuAddUv per vertex (stride 0, one zero element, when the model has no additional UV).
    const D3D12_VERTEX_BUFFER_VIEW& AddUvBufferView() const { return addUvVbv_; }
    bool HasAdditionalUv() const { return hasAdditionalUv_; }
    // Raw (ByteAddress) SRV heap index over the add-UV buffer; 0xFFFFFFFF when the model has none.
    uint32_t AddUvSrv() const { return addUvSrv_; }
    // Per-material PBR record buffer (StructuredBuffer<GpuPbrMaterial>, absolute SRV heap index; 0xFFFFFFFF when
    // the model has none, e.g. with --material-source default). pbrIndexOfMaterial = material index or 0xFFFFFFFF.
    uint32_t PbrSrv() const { return pbrSrv_; }
    bool HasPbr() const { return pbrSrv_ != DescriptorHeap::kInvalid; }
    // The model was created from a PbrModelData with at least one valid Ray record (its PBR payload was not empty
    // when the model loaded). Per-model PBR gate for the scene pass (P5): a model renders PBR when it was loaded
    // with records, whatever the global setting. Set by Renderer::CreateModel.
    bool HasPbrRecords() const { return hasPbrRecords_; }
    void SetHasPbrRecords(bool v) { hasPbrRecords_ = v; }
    uint32_t PbrIndex(size_t material) const {
        return material < pbrIndex_.size() ? pbrIndex_[material] : DescriptorHeap::kInvalid;
    }
    // S2b mesh dielectrics: the material's PBR transmission factor (KHR_materials_transmission, runtime overrides
    // included; 0 = opaque or no record). Renderer::RtScene sets RtGeom_Transmissive for a material that has one.
    float PbrTransmission(size_t material) const {
        return material < pbrTransmission_.size() ? pbrTransmission_[material] : 0.0f;
    }
    // S10 planar mirrors: the planes of the model's mirror materials, fitted once at Create from their rest-pose
    // triangles (model space; the caller applies the model's root transform). A material whose triangles are not
    // coplanar within 1 % of the material's extent is listed with valid = false (it shows its tint only).
    const std::vector<MirrorPlane>& MirrorPlanes() const { return mirrorPlanes_; }
    // S8b: the fitted rectangles of the model's materials (stage parts only; model space - the caller applies the root
    // transform). A screen claims a material at frame time (StageScreens::SetMaterialScreen sets MatFlag_Screen), so the
    // renderer filters this list by the material's current flags (Renderer::UpdateScreenLights).
    const std::vector<ScreenRect>& ScreenRects() const { return screenRects_; }
    // Runtime PBR overrides (MMDX stage material targets): applies `list` for ring entry `frame % 3` and only when
    // `version` differs from the one that entry was written with (same contract as UpdateMaterials). The FIRST call
    // with a non-empty list moves the PBR records into a 3-entry UPLOAD-heap ring with one SRV each (a model that
    // never gets overrides keeps its single DEFAULT-heap buffer, bit-identical to before); PbrSrv() then returns the
    // entry's SRV, so raster / RT / PT / offline GI all follow. An empty list restores the model's own record values.
    // Call before the passes read PbrSrv() (i.e. every frame, in UpdateScene).
    void SetPbrOverrides(uint64_t frame, const std::vector<PbrOverride>& list, uint64_t version);
    // Generic data traits of the model (additional UVs, toon-map materials) for ShaderPack::Suggests.
    ModelTraits Traits() const;
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
    // World-space bounding sphere of the pose last given to UpdateSkinning (characters / props only: the casters of the
    // spot and point shadow slices). False = no pose yet or another role.
    bool LiveBoundingSphere(DirectX::XMFLOAT3& center, float& radius) const {
        center = liveCenter_;
        radius = liveRadius_;
        return liveRadius_ > 0.0f;
    }

    // S8a: rest-pose area of every triangle (parallel to the index list / 3), and the material's emission estimate:
    // the luminance the shaders' emission terms (ScreenEmission / AutoLumEmission / PBR emissive) add at an average
    // texel (0 = the material does not emit). The path tracers' emissive-triangle CDF weighs a triangle by
    // area x estimate; the exact radiance is evaluated in the shader at the sampled point, so the estimate is only a
    // sampling heuristic.
    const std::vector<float>& TriAreas() const { return triAreas_; }
    float EmitEstimate(size_t material) const;

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
        // S2b: hash of the per-material "transmission > 0" set the BLAS was built with. A static stage's BLAS is built
        // once, but a runtime stage target (SetPbrOverrides) can turn a material transmissive later; RtScene rebuilds
        // when this changes, so the OPAQUE / NON_OPAQUE geometry flags never go stale.
        uint64_t transmissiveSig = 0;
    };
    RtResources& Rt() { return rt_; }

private:
    Dx12Context* ctx_ = nullptr;
    uint64_t uid_ = 0;   // assigned in the constructor from a process-wide atomic counter
    std::string name_;
    uint32_t boneCount_ = 0, vertexCount_ = 0, indexCount_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, sdef_, addUv_;
    // MaterialConstants ring (UPLOAD heap, persistently mapped): kRing entries of materialCount * 256 B.
    ComPtr<ID3D12Resource> materialCb_;
    uint8_t* materialMapped_ = nullptr;
    uint64_t materialEntryVersion_[kRing] = {};
    uint64_t materialVersion_ = 0;              // version of materialConsts_
    std::vector<MaterialConstants> baseConsts_; // without material morphs
    std::vector<char> autoLum_;                 // per material: AutoLuminous convention matched at load (packs clear it)
    struct MaterialBase { bool edgeFlag = false, castFlag = false; };
    std::vector<MaterialBase> materialBase_;
    std::vector<uint8_t> materialBlend_;        // per material: 0 normal, 1 no-depth-write, 2 additive (MmeBlendHint)
    std::vector<ComPtr<ID3D12Resource>> textures_;  // parallel to pmx.textures (null if unused)
    // S11: the format each texture_ was created with (the material SRV must match it) and the VRAM totals.
    std::vector<DXGI_FORMAT> textureFormats_;
    uint32_t textureCount_ = 0;
    uint64_t textureRgba8Bytes_ = 0;        // bytes of the RGBA8 textures actually uploaded
    uint64_t textureCompressedBytes_ = 0;   // bytes of the BCn blocks actually uploaded
    uint64_t textureWouldBeRgba8Bytes_ = 0; // what the BCn textures would have taken as RGBA8
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    D3D12_VERTEX_BUFFER_VIEW sdefVbv_{};
    bool hasSdef_ = false;
    D3D12_VERTEX_BUFFER_VIEW addUvVbv_{};
    bool hasAdditionalUv_ = false;
    uint32_t addUvSrv_ = DescriptorHeap::kInvalid;
    int additionalUvCount_ = 0;   // PMX header count (0..4)
    // Per-material PBR records (work unit P2): one SRV heap descriptor over a raw GpuPbrMaterial buffer, plus a
    // map material -> record index (kInvalid when the material has no record). Empty when the model has none.
    ComPtr<ID3D12Resource> pbrBuf_;
    std::vector<ComPtr<ID3D12Resource>> pbrTextures_;   // decoded map textures referenced by the records
    std::vector<uint32_t> pbrTextureSrvs_;              // SRV heap index per pbrTextures_ entry
    uint32_t pbrSrv_ = DescriptorHeap::kInvalid;
    uint32_t pbrStaticSrv_ = DescriptorHeap::kInvalid;   // the DEFAULT-heap buffer's SRV (kept for Destroy)
    std::vector<uint32_t> pbrIndex_;
    // Per material transmission factor (0 = opaque / no record): the base records, kept current by SetPbrOverrides
    // (a stage `transmission` target must move the ray paths' RtGeom_Transmissive too).
    std::vector<float> pbrTransmission_;
    bool hasPbrRecords_ = false;
    // Base PBR records (the ones Create built) and the 3-entry UPLOAD-heap ring of SetPbrOverrides. Lazily created.
    std::vector<GpuPbrMaterial> pbrRecords_;
    ComPtr<ID3D12Resource> pbrRingBuf_;
    void* pbrRingMapped_ = nullptr;
    static_assert(kRing == 3, "pbrRingSrvs_ initialiser lists kRing entries");
    uint32_t pbrRingSrvs_[kRing] = {DescriptorHeap::kInvalid, DescriptorHeap::kInvalid, DescriptorHeap::kInvalid};
    uint64_t pbrRingVersion_[kRing] = {};
    uint64_t pbrRingBytes_ = 0;
    bool pbrOverridesActive_ = false;   // the ring exists (PbrSrv() follows it)
    // Per material receive-shadow override from SetPbrOverrides (-1 none, 0 off, 1 on); applied in UpdateMaterials.
    std::vector<int8_t> stageReceive_;   // per material receiveShadow override (-1 none, 0 off, 1 on)
    // S10: per material mirror override (-1 none, 0 off, 1 on) from SetPbrOverrides, applied in UpdateMaterials like
    // stageReceive_, plus the mirror planes fitted once at Create.
    std::vector<int8_t> stageMirror_;
    std::vector<MirrorPlane> mirrorPlanes_;
    // S8b: the fitted rectangles of every stage material that has rest-pose triangles (see ScreenRect). A screen claims
    // a material at frame time, so the renderer filters this list by the material's MatFlag_Screen.
    std::vector<ScreenRect> screenRects_;
    uint64_t stageFlagsGen_ = 0;         // bumped when a shadow flag override changes (UpdateMaterials version)
    ModelRole role_ = ModelRole::Character;
    bool dynamic_ = false;   // M8a: an animated stage rebuilds its BLAS
    bool prop_ = false;      // a studio prop (the world's ground map includes it)
    DirectX::XMFLOAT3 boundsMin_{}, boundsMax_{};
    // LiveBoundingSphere: the bones' rest positions and how far a skinned vertex can lie outside their hull.
    std::vector<DirectX::XMFLOAT3> boneRest_;
    float skinMargin_ = 0.0f;
    DirectX::XMFLOAT3 liveCenter_{};
    float liveRadius_ = 0.0f;
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
    std::filesystem::path defaultPackTextureFolder_;   // model's own MME-derived pack texture folder (fallback)
    PackParamValues packParams_{};
    MaterialClassOverrides packOverrides_{};
    // MME object effect (M4b/M6a)
    bool mmeEffectMesh_ = false;
    std::vector<std::filesystem::path> mmeEffects_;   // per material (empty path = native shading)
    std::vector<DirectX::XMFLOAT3> mmeToonColor_;   // per material: bottom-left toon texel (white when none)
    // Background AVI: the material indices whose diffuse texture file name is screen.bmp (case-insensitive).
    std::vector<uint32_t> screenMaterials_;
    // S5 / background AVI: per-material original diffuse SRV resource, its MatFlag_HasTexture and the original
    // reflectivity (BindMaterialTexture / SetMaterialScreen restore them; parallel to Materials()).
    std::vector<ID3D12Resource*> materialOrigTex_;
    std::vector<DXGI_FORMAT> materialOrigTexFormat_;   // S11: format the diffuse texture resource was created with
    std::vector<char> materialOrigHasTex_;   // the material's MatFlag_HasTexture before a texture was bound over it
    std::vector<float> materialOrigReflect_;
    // S8a: per-triangle rest-pose area (parallel to the index list / 3) and per-material emission estimate (0 = not
    // an emitter; see EmitEstimate).
    std::vector<float> triAreas_;
    ComPtr<ID3D12Resource> packParamsExtCb_;
    uint8_t* packParamsExtMapped_ = nullptr;
    std::vector<DirectX::XMFLOAT4> packParamsExtSlots_;
    bool hasPackParamsExt_ = false;
    static constexpr size_t kPackExtCbBytes = 128 * sizeof(DirectX::XMFLOAT4);   // pack_api.hlsli gPackParamSlots
    uint64_t packParamsExtVersion_ = 0;
    uint64_t packExtEntryVersion_[kRing] = {};
};

} // namespace mmdx
