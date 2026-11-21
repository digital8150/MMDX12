#pragma once
// Engine-neutral per-material PBR data (work unit P2). One `PbrMaterial` per PMX material holds one "slot" per
// PBR field (albedo, normal, smoothness, ...): a constant value and/or a texture source, plus the raw Ray-MMD
// TYPE / SWIZZLE / UV transform the shader needs to decode it. `RayToPbr` fills the struct from the parsed Ray
// material data (asset/RayMaterial.h); the glTF import (P6) will fill the same struct later. Nothing here is
// render-specific: this header stays free of render includes.
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "asset/RayMaterial.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

// One PBR slot per field, in a fixed order mirrored by the GPU record (render/ShaderInterop.h, shaders/pbr_common.hlsli).
enum class PbrSlotId : int {
    Albedo, AlbedoSub, Alpha, Normal, NormalSub, Smoothness, Metalness, Specular, Occlusion, Emissive,
    CustomA, CustomB, Count
};
inline constexpr int kPbrSlotCount = (int)PbrSlotId::Count;
const char* PbrSlotName(PbrSlotId slot);

// Where a slot's texture comes from. `File` is a decoded file path (deduped per model); `Pmx*` reuse the
// PMX material's own texture / sphere / toon slot (already uploaded by GpuModel); `Embedded` reads the encoded
// bytes from `PmxModel::embeddedTextures[pmxIndex]` (GLB/VRM/FBX maps that live inside the model file).
enum class PbrTexKind : uint8_t { None = 0, File, PmxDiffuse, PmxSphere, PmxToon, Embedded };

struct PbrTextureRef {
    PbrTexKind kind = PbrTexKind::None;
    std::filesystem::path path;   // kind == File, before DecodePbrTextures resolves it
    int fileIndex = -1;           // kind == File, after DecodePbrTextures
    int pmxIndex = -1;            // PMX texture index for Pmx* kinds; PmxModel::textures index for Embedded
    // kind == Embedded: the encoded image bytes (points into the PmxModel's embeddedTextures during decode only;
    // cleared once DecodePbrTextures has decoded it).
    const std::vector<uint8_t>* bytes = nullptr;
};

// One field as it reaches the shader: constant value, texture source, and the raw decode hints.
struct PbrSlot {
    int from = 0;          // Ray <PREFIX>_MAP_FROM (0 none/value, 1 file, 2 animated file, 3 PMX tex, 4 sphere,
                           // 5 toon, 6 screen dummy, 7 PMX ambient colour, 8 PMX specular colour, 9 specular power)
    int type = 0;          // Ray <PREFIX>_MAP_TYPE
    int swizzle = 0;       // 0 R, 1 G, 2 B, 3 A
    int uvFlip = 0;        // 0 none, 1 X, 2 Y, 3 both; NORMAL 4 = flip the shading normal X
    int applyScale = 0;    // 0 none, 1 map * value, 2 map ^ value
    int colorFlip = 0;     // CUSTOM_A/B: 1 inverts the value
    bool hasValue = false; // the slot's const value was declared
    float value[3] = {0, 0, 0};
    bool hasScale = false;  // normalMapScale / parallaxMapScale
    float scale = 0;
    float loopU = 1, loopV = 1;        // <slot>MapLoopNum
    float uvScaleU = 1, uvScaleV = 1;  // <PREFIX>_MAP_UV_SCALE
    // S2a: glTF KHR_texture_transform offset (uv = uv * scale + offset; the rotation is per material, PbrExtensions).
    float uvOffsetU = 0, uvOffsetV = 0;
    PbrTextureRef tex;
    // P6 imported maps (Ray never sets these; they do not reach GpuPbrMaterial). `mapScale` multiplies the decoded
    // channel (glTF roughnessFactor / metallicFactor), `aoStrength` bakes lerp(1, map, strength) into the channel
    // (< 0 = none). Both are load-time instructions applied by DecodePbrTextures.
    float mapScale = 1.0f;
    float aoStrength = -1.0f;
};

struct PbrMaterial {
    bool valid = false;         // a Ray record exists for this material (else it renders as before)
    int shadingModel = 0;       // CUSTOM_ENABLE (0..9; 10 rejected by Ray-MMD)
    float alphaThreshold = 0.999f;  // material_common ALPHA_THRESHOLD (per include chain)
    int albedoSubEnable = 0;    // ALBEDO_SUB_ENABLE blend mode
    int albedoApplyDiffuse = 0; // ALBEDO_MAP_APPLY_DIFFUSE: multiply the sampled albedo by the PMX diffuse
    bool emissiveEnable = false;
    // S2a glTF material extensions (KHR_materials_transmission / ior / volume / clearcoat / specular / sheen and
    // KHR_texture_transform). Every field keeps its neutral default for a Ray-MMD record, so a Ray material is
    // unchanged: the shaders only add a term when a factor is > 0.
    struct Extensions {
        float transmission = 0.0f;      // KHR_materials_transmission factor
        PbrTextureRef transmissionTex;  // its texture (R channel), decoded like a slot map
        // KHR_materials_volume thickness map. Carried (and resolved on the CPU) for S2b; the S2a GPU record has no
        // slot for it, and nothing samples it yet.
        PbrTextureRef volumeThicknessTex;
        float ior = 1.5f;               // KHR_materials_ior
        float volumeThickness = 0.0f;   // KHR_materials_volume thicknessFactor
        DirectX::XMFLOAT3 volumeAttenuationColor{1, 1, 1};  // linear
        float volumeAttenuationDistance = 0.0f;             // 0 = infinite (no attenuation)
        float clearcoat = 0.0f;         // KHR_materials_clearcoat factor
        float clearcoatRoughness = 0.0f;
        float specularFactor = 1.0f;    // KHR_materials_specular (stored, not rendered)
        DirectX::XMFLOAT3 specularColorFactor{1, 1, 1};
        DirectX::XMFLOAT3 sheenColorFactor{0, 0, 0};   // KHR_materials_sheen (stored, not rendered)
        float sheenRoughness = 0.0f;
        float uvRotation = 0.0f;        // KHR_texture_transform rotation in radians (one per material)
        float emissiveStrength = 1.0f;  // KHR_materials_emissive_strength (HDR multiplier on the emissive slot)
        // The material's own emission before an MMDX stage rule changed it (linear: the emissive map's factor, or the
        // constant colour), filled by ApplyStageMaterialRules. A rule / runtime binding that sets only
        // emissiveStrength scales this. w = 1 when the material emitted on its own, 0 otherwise (nothing to scale).
        DirectX::XMFLOAT4 emissiveOwn{0, 0, 0, 0};
        // S10 planar mirror (an MMDX stage rule's "mirror"): the material is a perfect specular surface in every
        // render path. Raster samples the frame's planar reflection, RT / PT / offline GI reflect at the hit. The
        // base colour is the reflection tint and smoothness / roughness the blur.
        bool mirror = false;
    } ext;
    PbrSlot slots[kPbrSlotCount];
};

// Per-model PBR payload produced on the loader worker thread: the material records plus the decoded file textures.
struct PbrModelData {
    std::string modelName;
    std::vector<PbrMaterial> materials;      // parallel to PmxModel::materials
    std::vector<std::filesystem::path> texturePaths;  // unique file textures (deduped by resolved path)
    std::vector<int> textureChannels;        // parallel: channel read (-1 = full RGBA, 0..3 = one channel -> R8)
    std::vector<ImageRGBA8> textures;        // parallel to texturePaths (empty = missing)
    int materialsWithRecord = 0;
    int filesMissing = 0;
    double loadMs = 0.0;
    // Bytes of the decoded textures (rough VRAM estimate for the measurement log).
    uint64_t textureBytes = 0;
};

// Converts one parsed Ray material into the engine-neutral per-material record. `pmxMat` provides the PMX
// ambient / specular colour and specular power for FROM 7/8/9. Slots resolve constants on the CPU; textures
// keep their raw FROM/TYPE/UV hints and are resolved later by DecodePbrTextures.
PbrMaterial RayToPbr(const RayMaterialDesc& desc, const PmxMaterial& pmxMat);

// Dedups every `File` texture the materials reference by resolved path, decodes them once each (parallel) and
// rewrites the slots to indices into `texturePaths` / `textures`. A file that is missing / undecodable falls the
// slot back to its constant (`tex.kind = None`) with one [W] per file.
void DecodePbrTextures(PbrModelData& data, const char* label);

// Finds the model's Ray .emd (content rule only), parses it and converts every PMX material, then decodes the
// referenced file textures. `out` is filled in place; when the model has no Ray data `out.materialsWithRecord`
// stays 0 and nothing is logged. Runs on the loader worker thread. Logs the per-model measurement line when the
// model has at least one record.
// `forcedEmd` (P5): when non-empty, that .emd is used instead of the best auto candidate. It is validated (it must
// be readable and reach at least one Ray material .fx, i.e. Obj count / Ray share); on failure it warns and falls
// back to the default candidate, or to no records when none exists.
void BuildPbrModelData(const std::filesystem::path& modelPath, const std::vector<PmxMaterial>& pmxMaterials,
                       const std::string& modelName, PbrModelData& out,
                       const std::filesystem::path& forcedEmd = {});

// P6: builds the PbrModelData for an imported (glTF/GLB/VRM/FBX) model from its PmxModel::importedPbr records.
// One PbrMaterial per PMX material, `valid` only where the source material carried PBR data. `File` maps use the
// resolved path, `Embedded` maps the PmxModel texture bytes. Logs the same `PBR model=` line as BuildPbrModelData.
void BuildImportedPbrModelData(const PmxModel& pmx, PbrModelData& out);

} // namespace mmdx
