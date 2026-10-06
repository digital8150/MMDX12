#pragma once
// C++ mirror of the constant-buffer / vertex layouts used by shaders/*.hlsl.
// Keep both sides in sync (shaders/common.hlsli). HLSL uses `#pragma pack_matrix(row_major)`
// and mul(v, M), so DirectXMath matrices are uploaded as-is (no transpose).
#include <DirectXMath.h>
#include <cstdint>

namespace mmdx {

inline constexpr uint32_t kShadowCascades = 3;
inline constexpr uint32_t kSpotShadowSlices = 8;  // spot lights with a shadow map (the first 8 spots)
inline constexpr uint32_t kSceneCbSize = 2048;

struct SceneConstants {               // b0, kSceneCbSize-byte slot per frame
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 proj;          // jittered when TAA is on
    DirectX::XMFLOAT4X4 viewProj;      // jittered
    DirectX::XMFLOAT4X4 invProj;       // inverse of proj (jittered)
    DirectX::XMFLOAT4X4 invView;
    DirectX::XMFLOAT4X4 viewProjNoJitter;
    DirectX::XMFLOAT4X4 prevViewProjNoJitter;
    DirectX::XMFLOAT4X4 shadowViewProj[kShadowCascades];
    DirectX::XMFLOAT4 cascadeSplits;   // view-space far z of cascade 0..2, w = shadows enabled (0/1)
    DirectX::XMFLOAT4 shadowParams;    // x = 1/mapSize, y = normal offset scale, z = softness (texels), w = 1: no sun shadows (studio self-shadow track off; the path tracer reads it)
    DirectX::XMFLOAT4 cascadeTexel;    // world size of one shadow texel per cascade
    DirectX::XMFLOAT3 eyePos;      float time;
    DirectX::XMFLOAT3 lightDir;    float sunIntensity;   // lightDir normalized, from light toward scene
    DirectX::XMFLOAT3 lightColor;  float hemiStrength;   // MMD light colour (gamma space)
    DirectX::XMFLOAT3 skyZenith;   float rimStrength;
    DirectX::XMFLOAT3 skyHorizon;  float numLights;
    DirectX::XMFLOAT3 groundColor; float floorGloss;
    DirectX::XMFLOAT3 rimColor;    float fog;
    DirectX::XMFLOAT2 viewportSize; float edgeScale; float transparentBg;  // edgeScale = viewportHeight / 1080
    DirectX::XMFLOAT2 jitterUv;    DirectX::XMFLOAT2 invViewportSize;      // jitter in uv units
    float nearZ, farZ, frameIndex, shading;  // shading: 0 Lit, 1 Unlit, 2 Wireframe (raster scene shading)
    DirectX::XMFLOAT4X4 prevInvView;   // offline renderer: camera-to-world at shutter open (motion blur)
    // Offline renderer scene extras (OfflineSceneProps, zero = none). See shaders/offline_gi.hlsl.
    DirectX::XMFLOAT4 glassCenter;     // xyz centre, w = enabled (0/1)
    DirectX::XMFLOAT4 glassHalf;       // xyz half extents (before rounding), w = corner radius
    DirectX::XMFLOAT4 glassParams;     // x = cos yaw, y = sin yaw, z = ior (green), w = ior(blue) - ior(red)
    DirectX::XMFLOAT4 glassAbsorb;     // xyz Beer-Lambert absorption per MMD unit (linear rgb)
    DirectX::XMFLOAT4 softbox[2][4];   // per softbox: centre (w = enabled), half U, half V, radiance
    DirectX::XMFLOAT4 floorParams;     // studio floor: xyz albedo (linear), w = reflectivity; w < 0: default floor
    // Spot light shadow maps (ShadowPass, slice = GpuLight::_pad[0]): perspective view-projection per slice.
    DirectX::XMFLOAT4X4 spotViewProj[kSpotShadowSlices];
    DirectX::XMFLOAT4 spotShadowParams;  // x = slices rendered this frame (0 = none), y = 1/mapSize
};
static_assert(sizeof(SceneConstants) <= kSceneCbSize, "SceneConstants layout");

enum MaterialShaderFlags : uint32_t {
    MatFlag_HasTexture    = 1u << 0,
    MatFlag_HasToon       = 1u << 1,
    MatFlag_SphereMul     = 1u << 2,
    MatFlag_SphereAdd     = 1u << 3,
    MatFlag_ReceiveShadow = 1u << 4,
    MatFlag_Stage         = 1u << 5,  // material belongs to a stage model (floor gloss, no rim)
    MatFlag_ToonMap       = 1u << 6,  // "toon" is a UV-mapped shadow-colour texture (Project Sekai style)
    MatFlag_FlatShade     = 1u << 7,  // no N.L modelling (faces): only cast shadows darken
};

struct MaterialConstants {            // b1, one 256-byte slot per material
    DirectX::XMFLOAT4 diffuse;
    DirectX::XMFLOAT3 specular;  float specularPower;
    DirectX::XMFLOAT3 ambient;   float edgeSize;
    DirectX::XMFLOAT4 edgeColor;
    uint32_t flags;              // MaterialShaderFlags
    float reflectivity;          // base SSR reflectivity derived from the MMD specular
    uint32_t _pad[2];
    // Material morph texture factors (PMX): sampled colour = saturate(colour * mul + add) for the
    // texture, sphere and toon maps that the material has. Identity: mul 1, add 0.
    DirectX::XMFLOAT4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;
    uint8_t _reserve[256 - 176];
};
static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants layout");

// StructuredBuffer element for punctual lights (t6 in the scene pass). 64 bytes.
struct GpuLight {
    DirectX::XMFLOAT3 position; float invRange;
    DirectX::XMFLOAT3 color;    float spotCosOuter;   // colour premultiplied by intensity
    DirectX::XMFLOAT3 direction; float spotCosInner;
    float shadowSlice;           // spot shadow map slice (valid when < spotShadowParams.x), -1 = none
    float _pad[3];
};
static_assert(sizeof(GpuLight) == 64, "GpuLight layout");

// Vertex buffer slot 0 (static, DEFAULT heap). 60 bytes.
struct GpuVertex {
    float position[3];   // POSITION      R32G32B32_FLOAT
    float normal[3];     // NORMAL        R32G32B32_FLOAT
    float uv[2];         // TEXCOORD0     R32G32_FLOAT
    uint16_t bones[4];   // BLENDINDICES  R16G16B16A16_UINT  (unused slots = 0 with weight 0)
    float weights[4];    // BLENDWEIGHT   R32G32B32A32_FLOAT (sum = 1)
    float edgeScale;     // TEXCOORD1     R32_FLOAT
};
static_assert(sizeof(GpuVertex) == 60, "GpuVertex layout");
// Vertex buffer slot 1 (per frame ring, UPLOAD heap): float3 morph position delta, TEXCOORD2.
// Vertex buffer slot 2 (previous frame's morph deltas, same layout): TEXCOORD3.

// Vertex buffer slot 3 (static): SDEF parameters per vertex (shaders/skinning.hlsli). Models without
// SDEF vertices bind one zero element with stride 0. Also read raw by shaders/skin.hlsl (t5).
struct GpuSdef {
    float c[3];          // TEXCOORD4  SDEF centre C
    float cr0[3];        // TEXCOORD5  (C + R0') / 2, R0' = C + R0 - (R0 w0 + R1 w1) (as saba/MMD)
    float cr1[3];        // TEXCOORD6  (C + R1') / 2
    float sdef;          // TEXCOORD7  1 = SDEF vertex (bones[0], bones[1], weights.x), 0 = linear blend
};
static_assert(sizeof(GpuSdef) == 40, "GpuSdef layout");

// ---- ray tracing (mirror of shaders/rt_common.hlsli) -------------------------------------

// Skinned world-space vertex written by shaders/skin.hlsl, read by BLAS builds and ray
// queries (raw buffer, 48-byte stride, position at offset 0).
struct RtVertex {
    float position[3];
    float normal[3];       // normalized
    float uv[2];
    float prevPosition[3]; // previous frame (previous bones + previous morphs), for motion vectors
    float _pad;
};
static_assert(sizeof(RtVertex) == 48, "RtVertex layout");

// Flags stored in RtGeometry::flags next to the MaterialShaderFlags (bits 0..15).
enum RtGeometryFlags : uint32_t {
    RtGeom_CastShadow = 1u << 16,  // occludes shadow rays
    RtGeom_AlphaTest  = 1u << 17,  // non-opaque in the BLAS: ray queries evaluate texture alpha
    RtGeom_Character  = 1u << 18,
};

// One entry per BLAS geometry. TLAS InstanceID = index of the model's first entry, so a hit's
// entry is gGeometries[CommittedInstanceID() + CommittedGeometryIndex()]. 176 bytes.
struct RtGeometry {
    uint32_t vertexSrv;    // SrvHeap index of a raw SRV over the model's RtVertex buffer
    uint32_t indexSrv;     // SrvHeap index of a raw SRV over the model's uint32 index buffer
    uint32_t indexStart;   // first index of this material in the index buffer
    uint32_t flags;        // MaterialShaderFlags | RtGeometryFlags
    DirectX::XMFLOAT4 diffuse;
    DirectX::XMFLOAT3 specular; float specularPower;
    DirectX::XMFLOAT3 ambient;  float reflectivity;
    uint32_t textureSrv;   // SrvHeap index of the material texture (GpuModel::Material::srvTable + 0)
    uint32_t sphereSrv;    // srvTable + 1
    uint32_t toonSrv;      // srvTable + 2
    uint32_t _pad;
    // material morph texture factors, as MaterialConstants::texMul..toonAdd
    DirectX::XMFLOAT4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;
};
static_assert(sizeof(RtGeometry) == 176, "RtGeometry layout");

} // namespace mmdx
