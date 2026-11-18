#pragma once
// C++ mirror of the constant-buffer / vertex layouts used by shaders/*.hlsl.
// Keep both sides in sync (shaders/common.hlsli). HLSL uses `#pragma pack_matrix(row_major)`
// and mul(v, M), so DirectXMath matrices are uploaded as-is (no transpose).
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mmdx {

inline constexpr uint32_t kShadowCascades = 3;
// S7a: the 16 most important spot lights get a shadow slice (Renderer::PickSpotShadowSlices); the rest are unshadowed.
inline constexpr uint32_t kSpotShadowSlices = 16;
// S7a: a light CDF lives in a raw ByteAddressBuffer SRV (bindless buffer table): one float per light, the normalised
// inclusive running sum of the pick weight (Renderer::FillLightCdf). At most one per Renderer::kMaxPunctualLights.
inline constexpr uint32_t kLightCdfMax = 256;
// S8a: emissive-triangle next-event estimation (LED screens / AutoLuminous / PBR emissive). One entry per emitcing
// triangle: geometry id (index into the RtScene geometry table) and the triangle's index inside that geometry. The
// pick CDF holds one float per entry (normalised inclusive running sum of area x emission estimate), and a per-geometry
// base table maps a geometry id to its first entry (0xFFFFFFFF = the geometry does not emit).
struct GpuEmissiveTri {
    uint32_t geometry;
    uint32_t triangle;
};
static_assert(sizeof(GpuEmissiveTri) == 8, "GpuEmissiveTri layout");
inline constexpr uint32_t kEmissiveTriMax = 262144;   // entries in the pick table (largest-weight geometries first)
inline constexpr uint32_t kEmissiveTriBytes = kEmissiveTriMax * sizeof(GpuEmissiveTri);
inline constexpr uint32_t kEmissiveCdfBytes = kEmissiveTriMax * sizeof(float);
inline constexpr uint32_t kEmissiveBaseMax = 8192;    // geometries the base table can address
inline constexpr uint32_t kEmissiveBaseBytes = kEmissiveBaseMax * sizeof(uint32_t);
inline constexpr uint32_t kEmissiveTriNone = 0xFFFFFFFFu;
// S7c: clustered light culling (Forward+ with depth slices). The view is cut into a fixed froxel grid: screen tiles x
// exponential (log) view-depth slices. Each cluster stores one header uint (the light count, or 0xFFFF = "too many:
// the shaders loop over every light") followed by up to kClusterMaxLights light indices in ASCENDING light order, so a
// fragment's per-light sum is accumulated in the same order as the full loop. Buffer word offsets (raw uint buffer):
// cluster c starts at c * (1 + kClusterMaxLights). Cluster order: (slice * kClusterTilesY + ty) * kClusterTilesX + tx,
// with tx = 0 at the left edge, ty = 0 at the TOP edge.
inline constexpr uint32_t kClusterTilesX = 16;
inline constexpr uint32_t kClusterTilesY = 9;
inline constexpr uint32_t kClusterSlices = 24;
inline constexpr uint32_t kClusterMaxLights = 64;
inline constexpr uint32_t kClusterCount = kClusterTilesX * kClusterTilesY * kClusterSlices;
inline constexpr uint32_t kClusterDwords = kClusterCount * (1 + kClusterMaxLights);   // 224640, x4 = 256-aligned
inline constexpr uint32_t kClusterBytes = kClusterDwords * sizeof(uint32_t);          // 898560
// S7c: above this many punctual lights the raster / RT scene shaders read their fragment's cluster list instead of
// looping over every light (Renderer::RecordScene sets SceneConstants::lightClusterParams.x, else -1).
inline constexpr uint32_t kClusterMinLights = 16;
// S7c: the farthest slice ends at min(camera far, this): beyond ~1000 units the exponential slices get too coarse.
inline constexpr float kClusterFarZ = 1000.0f;
// Raster point shadows: every shadowed point light gets 6 slices (no light cap). Faces stay at
// kPointShadowMapSize while the map fits the memory of kPointShadowFullResLights lights (twice that
// at shadow quality 4096); more lights halve the face size, down to kPointShadowMinSize.
inline constexpr uint32_t kPointShadowFaces = 6;
inline constexpr uint32_t kPointShadowMapSize = 1024;
inline constexpr uint32_t kPointShadowMinSize = 256;
inline constexpr uint32_t kPointShadowFullResLights = 4;
// 256-aligned slot; supports spot matrices + pack inputs + the S9b-1 physical sky (8 float4s added in S9b-1)
inline constexpr uint32_t kSceneCbSize = 3328;

// Typed scene role for pack inputs (ShaderPack::PackInput order)
struct GpuPackInputRole {
    DirectX::XMFLOAT4 posValid; // xyz = world pos (MMD units), w = 1.0f (valid) or 0.0f
    DirectX::XMFLOAT4 right;    // frame X axis (normalized)
    DirectX::XMFLOAT4 up;       // frame Y axis (normalized)
    DirectX::XMFLOAT4 forward;  // frame Z axis (normalized)
};
static_assert(sizeof(GpuPackInputRole) == 64, "GpuPackInputRole layout");

struct GpuPackInputs {
    GpuPackInputRole roles[8];  // PerformerHead..CameraFocus
    DirectX::XMFLOAT4 audio0;   // x = beat, y = downbeat, z = onset, w = energy
    DirectX::XMFLOAT4 audio1;   // x = section, y = bpm, z = valid, w = beat count (beat index + phase)
};
static_assert(sizeof(GpuPackInputs) == 544, "GpuPackInputs layout");

// S10: the planar mirrors of one frame (Renderer::UpdateMirrors). Two slots at most: the largest on-screen planes
// that face the camera. Mirrors are raster-only; the ray-traced paths reflect at the hit instead.
inline constexpr uint32_t kMaxMirrorPlanes = 2;

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
    DirectX::XMFLOAT4 cascadeTexel;    // xyz: world size of one shadow texel per cascade, w = depth reach behind each cascade
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
    DirectX::XMFLOAT4 sunShadowParams;   // x = shadowType (0=NoCast, 1=Hard, 2=Soft), y = softness, z = density, w = unused
    DirectX::XMFLOAT4 sunShadowColor;    // xyz = shadow colour (linear RGB), w = unused
    DirectX::XMFLOAT4 pointShadowParams; // x = point lights shadowed this frame (0 = none), y = 1/pointMapSize
    DirectX::XMFLOAT4 autoLumParams;     // x = AutoLuminous glow enabled (0/1), y = RT glass reflection rays (0/1), zw unused
    // Transparent-background export (offline GI): primary camera-ray misses output xyz (linear); w = enabled (0/1).
    // Real-time renderers use RenderSettings::transparentBackground for the same effect. 0 = the real sky.
    DirectX::XMFLOAT4 bgOverride;
    // Shader pack typed inputs (requires "inputs.scene" / "inputs.audio")
    GpuPackInputs packInputs;
    // S7a: the frame's punctual-light CDF (Renderer::FillLightCdf), read by the path tracer through the bindless
    // buffer table. x = SrvHeap index of the raw ByteAddressBuffer holding the float CDF (negative = none, use the
    // uniform pick), y = light count, zw = unused.
    DirectX::XMFLOAT4 lightCdfParams;
    // S7c: the frame's clustered light lists (PassLightCull, Renderer::lightClusterBuf_), read by the raster / RT scene
    // pixel shaders through the same bindless buffer table. x = SrvHeap index of the raw ByteAddressBuffer holding this
    // frame slot's cluster block (negative = none: the shaders keep the full per-light loop), y = the grid packed as
    // (tilesX & 0xFF) | (tilesY & 0xFF) << 8 | (slices & 0xFF) << 16 (0 = no clusters), z = log-slice scale,
    // w = log-slice bias: a fragment at view depth z belongs to slice clamp(floor(log(max(z, nearZ)) * scale + bias),
    // 0, slices - 1); slice k starts at view depth exp((k - bias) / scale). Layout / order: see kCluster* above.
    DirectX::XMFLOAT4 lightClusterParams;
    // S8a: the frame's emissive-triangle pick table (Renderer::BuildEmissiveTable), read by the path tracers through
    // the bindless buffer table. x = SrvHeap index of the RawBuffer of GpuEmissiveTri (uint2 per entry, negative =
    // none: no NEE, no MIS), y = SrvHeap index of the float pick CDF (normalised inclusive, one entry per triangle),
    // z = triangle count, w = SrvHeap index of the per-geometry base table (a uint per geometry id: index of the
    // geometry's first entry in the triangle list, or 0xFFFFFFFF when the geometry does not emit).
    DirectX::XMFLOAT4 emissiveParams;
    // S10: planar mirrors of the frame (Renderer::UpdateMirrors), raster only. mirrorViewProj[i] is the frame camera
    // reflected about mirror slot i's plane, with the frame camera's own projection: a mirror pixel at world position
    // P samples slot i's texture at project(P, mirrorViewProj[i]) (P lies on the plane, so the reflected view sees
    // exactly the reflected ray). mirrorParams: x = mirror count (0 = none: a mirror material shows its tint only),
    // y = SrvHeap index of slot 0's reflection texture, z = slot 1's (-1 = none), w = unused.
    DirectX::XMFLOAT4X4 mirrorViewProj[kMaxMirrorPlanes];
    // mirrorPlane[i]: xyz = unit world normal, w = -dot(normal, point on the plane). A mirror pixel picks the slot
    // whose plane it lies on.
    DirectX::XMFLOAT4 mirrorPlane[kMaxMirrorPlanes];
    DirectX::XMFLOAT4 mirrorParams;
    // S10: the mirror pass's clip plane (world space, front side positive: the lit PS discards fragments behind it);
    // (0,0,0,0) = off, which every other view / pass keeps.
    DirectX::XMFLOAT4 clipPlane;
    // S7b: the frame's spot gobo table (Renderer::FillGoboTable / OfflineRenderer::FillGoboTable), one float4 per
    // punctual light in a raw buffer reached through the bindless buffer table (shaders/gobo.hlsli): x = SrvHeap index
    // of the gobo texture (-1 = this light has no gobo), y = rotation radians including spin x time, z = scale,
    // w = mip bias from the focus (0 = sharp). x = SrvHeap index of the raw ByteAddressBuffer holding that table,
    // negative = no light of the frame has a gobo: the old image, zero cost.
    DirectX::XMFLOAT4 goboParams;
    // S8b: the frame's screen lights (raster / RT only; PT / offline GI use S8a's emissive NEE instead).
    // x = SrvHeap index of the raw ByteAddressBuffer holding kMaxScreenLights GpuScreenLight entries (-1 = no screen
    // light this frame: every screen term is skipped and the frame is the old one), y = the screen light count
    // (at most kMaxScreenLights), z = SrvHeap index of the average-colour buffer (Renderer::screenAverageBuf_, one
    // float4 per screen light: xyz = that screen texture's average in linear light), w = unused.
    DirectX::XMFLOAT4 screenLightParams;
    // S9b-1: the Physical Sky light (LightParams::physicalSky / sky, render/PhysicalSky.h). skyParams.x = 1 with a
    // physical sky this frame (0 = today's hand-set gradient: every other field is then 0). y = the radiance scale
    // (0.04 x the light's intensity), z = horizon haze 0..1, w = the day factor (scales the model's sky: 1 with the sun
    // up, 0 below the horizon where only skyNight is left). The Perez coefficients and the zenith quotient of
    // shaders/physical_sky.hlsli:
    //   skyPerezY / X / Yy = A, B, C, D of the Y / x / y distribution, skyPerezE = (E_Y, E_x, E_y, 0),
    //   skyZenithOverF = (zenith Y, zenith x, zenith y) each divided by its F(0, sun), skySunToward = the unit vector
    //   toward the sun, skyNight = the night floor's linear rgb.
    DirectX::XMFLOAT4 skyParams;
    DirectX::XMFLOAT4 skyPerezY;
    DirectX::XMFLOAT4 skyPerezX;
    DirectX::XMFLOAT4 skyPerezYy;
    DirectX::XMFLOAT4 skyPerezE;
    DirectX::XMFLOAT4 skyZenithOverF;
    DirectX::XMFLOAT4 skySunToward;
    DirectX::XMFLOAT4 skyNight;
    // S9b-2: the sky environment buffer (render/SkyEnv.h, shaders/sky_env.hlsl, read by shaders/sky_env.hlsli through
    // the bindless buffer table). x = SrvHeap index of the raw ByteAddressBuffer (render/SkyEnv.h layout:
    // the row CDF, the marginal CDF, the total and the 9 SH coefficients) or -1 = no sky environment this frame (no
    // Physical Sky, the build failed, or the offline background override: the ambient stays the two-colour hemisphere
    // and the path tracers add no sky light sample), y = W, z = H, w = unused.
    DirectX::XMFLOAT4 skyEnvParams;
    // S9b-3: the Sky light's IMAGE (render/SkyMap.h, shaders/sky_map.hlsli). skyParams.x = 2 with a loaded image (1 =
    // the procedural S9b-1 sky, 0 = the hand-set gradient). skyMapParams: x = SrvHeap index of the RGBA16F equirect
    // texture (-1 = none: SkyMapRadiance returns 0), y = the rotation around +y in radians (the panorama's u offset),
    // z = the radiance scale (skyMapIntensity), w = mirror (1 = flipped left / right). skyMapSize: x / y = the
    // texture's width / height in texels (the v clamp keeps sampling off the edge rows), zw = 0.
    DirectX::XMFLOAT4 skyMapParams;
    DirectX::XMFLOAT4 skyMapSize;
    // S9b-4: the physical sky's cloud layer (shaders/physical_sky.hlsli PhysicalSkyClouds). cloudParams = (coverage,
    // opacity, softness, heightKm); coverage is 0 unless the procedural physical sky draws (never on an image sky, so
    // 0 coverage leaves every frame byte-identical). cloudParams2 = (scaleKm, windOffsetX_km, windOffsetZ_km, 0).
    DirectX::XMFLOAT4 cloudParams;
    DirectX::XMFLOAT4 cloudParams2;
};
static_assert(sizeof(SceneConstants) <= kSceneCbSize, "SceneConstants layout");

// S7b: one float4 per punctual light in the frame's gobo table (see SceneConstants::goboParams / shaders/gobo.hlsli).
struct GpuGobo {
    float srv;        // SrvHeap index of the gobo texture, -1 = no gobo on this light
    float rotation;   // radians (authored rotation + spin x song seconds)
    float scale;      // uv scale (1 = the texture fills the cone's inscribed square)
    float mipBias;    // focus: 0 = sharp, > 0 = a blurred (higher) mip
};
static_assert(sizeof(GpuGobo) == 16, "GpuGobo layout");

// S8b: the frame's screen lights (raster / RT only). One entry per stage screen material instance: the rectangle its
// rest-pose triangles fit into (GpuModel::ScreenRect) transformed into world space, the emissive strength (the scale
// ScreenEmission uses) and the heap index of the screen texture the material shows (the colour source for the average
// reduction and for the specular reflection). Read by shaders/screen_light.hlsli through the bindless buffer / texture
// tables (SceneConstants::screenLightParams.x = this table's SrvHeap index). 64 B, 16-aligned.
inline constexpr uint32_t kMaxScreenLights = 16;

// S9b-2: the sky environment buffer (render/SkyEnv.h, shaders/sky_env.hlsl / sky_env.hlsli). One raw buffer of
// kSkyEnvFloats floats, W x H texels of an equirectangular sky grid plus the marginal CDF, the total and the 9 SH
// radiance coefficients. Texel (i, j): v = (i + 0.5) / H, u = (j + 0.5) / W, theta = PI v from +Y, phi = 2 PI u,
// f = (luminance(SkyColor(dir)) + 1e-4) sin(theta).
inline constexpr uint32_t kSkyEnvW = 128;
inline constexpr uint32_t kSkyEnvH = 64;
inline constexpr uint32_t kSkyEnvMargOffset = kSkyEnvW * kSkyEnvH;   // 8192: the row CDF, one float per row
inline constexpr uint32_t kSkyEnvTotalOffset = kSkyEnvMargOffset + kSkyEnvH;   // 8256: the sum of every f
inline constexpr uint32_t kSkyEnvShOffset = kSkyEnvTotalOffset + 1;            // 8260: 9 float4 SH coefficients
inline constexpr uint32_t kSkyEnvFloats = kSkyEnvShOffset + 9 * 4;             // 8296

struct GpuScreenLight {
    DirectX::XMFLOAT4 center;   // xyz = rectangle centre (world), w = the screen's emissive strength (brightness)
    DirectX::XMFLOAT4 normal;   // xyz = unit normal (the side the screen shines from), w = area (world^2)
    // axisA.xyz = half axis A (world), w = the screen texture's SrvHeap index (the average + specular colour source)
    DirectX::XMFLOAT4 axisA;
    DirectX::XMFLOAT4 axisB;    // xyz = half axis B (world), w unused
};
static_assert(sizeof(GpuScreenLight) == 64, "GpuScreenLight layout");

// Shader-pack alpha key (pack.json class rule "alphaKey", 0..1; 0 = off). PackClass stays in bits 0..7 of
// MaterialConstants::packClass, the key lives in bits 8..15 as round(key * 255). The HLSL side (mmd.hlsl,
// pack_api.hlsli, rt_common.hlsli) mirrors this with PackClassLow / PackAlphaKey.
inline constexpr float kPackAlphaKeyScale = 255.0f;
inline uint32_t PackClassLow(uint32_t packClass) { return packClass & 0xFFu; }
inline float PackAlphaKeyFromClass(uint32_t packClass) {
    return (float)((packClass >> 8) & 0xFFu) / kPackAlphaKeyScale;
}
inline uint32_t PackClassEncode(uint32_t cls, float alphaKey) {
    const uint32_t k = alphaKey > 0.0f
                           ? (uint32_t)std::lround(std::clamp(alphaKey, 0.0f, 1.0f) * kPackAlphaKeyScale)
                           : 0u;
    return (cls & 0xFFu) | (k << 8);
}
// The key a material's final class carries (`key[cls]`), or 0 for the off class / a null pack.
inline float PackAlphaKeyForClass(const float alphaKey[6], uint32_t cls) {
    return cls < 6u ? alphaKey[cls] : 0.0f;
}

// S7c: the froxel grid packed into SceneConstants::lightClusterParams.y (0 = no clusters). Mirrors the decode in
// shaders/light_cluster.hlsli.
inline float ClusterGridPacked() {
    return (float)((kClusterTilesX & 0xFFu) | ((kClusterTilesY & 0xFFu) << 8) | ((kClusterSlices & 0xFFu) << 16));
}
// S7c: the exponential view-depth slice mapping (SceneConstants::lightClusterParams.zw): a view depth z belongs to
// slice clamp(floor(log(max(z, nearZ)) * scale + bias), 0, slices - 1), and slice k starts at exp((k - bias) / scale).
// False (scale = bias = 0) when the camera cannot be sliced (degenerate near / far): the caller then keeps -1.
inline bool ClusterSliceParams(float nearZ, float farZ, float& scale, float& bias) {
    const float z0 = std::max(nearZ, 0.01f);
    const float z1 = std::min(farZ, kClusterFarZ);
    scale = 0.0f;
    bias = 0.0f;
    if (!(z1 > z0 * 1.001f)) return false;
    scale = (float)kClusterSlices / std::log(z1 / z0);
    bias = -std::log(z0) * scale;
    return true;
}

enum MaterialShaderFlags : uint32_t {
    MatFlag_HasTexture    = 1u << 0,
    MatFlag_HasToon       = 1u << 1,
    MatFlag_SphereMul     = 1u << 2,
    MatFlag_SphereAdd     = 1u << 3,
    MatFlag_ReceiveShadow = 1u << 4,
    MatFlag_Stage         = 1u << 5,  // material belongs to a stage model (floor gloss, no rim)
    MatFlag_ToonMap       = 1u << 6,  // "toon" is a UV-mapped shadow-colour texture (an MME data layout)
    MatFlag_FlatShade     = 1u << 7,  // no N.L modelling (faces): only cast shadows darken
    MatFlag_HasSphereTex  = 1u << 8,  // a real sphere-slot texture is bound (not the white fallback)
    MatFlag_AutoLuminous  = 1u << 9,  // そぼろ AutoLuminous convention: this material glows (black specular, power >= 100)
    MatFlag_Additive      = 1u << 10, // the model's own .emd effect is additive (SrcBlend SRC_ALPHA, DestBlend ONE, no depth write)
    MatFlag_NoDepthWrite  = 1u << 11, // the model's own .emd effect draws with ZWriteEnable = false (alpha blend, no depth write)
    // S5: an MMDX stage screen surface: the bound texture is the emission (HDR, x the material's reflectivity slot as
    // brightness), the material itself is black (no diffuse / specular / AO).
    MatFlag_Screen        = 1u << 12,
    // S10: a planar mirror (docs/stage_format.md "mirror"): a perfect specular surface whose base colour is the
    // reflection tint and whose roughness blurs it (0 = sharp). Raster samples the planar reflection texture of the
    // frame (SceneConstants::mirrorParams), RT / PT / offline GI reflect natively; without a reflection texture
    // (quad / ortho views, camera feeds, RenderToImage) the material shows the tint only.
    MatFlag_Mirror        = 1u << 13,
};

struct MaterialConstants {            // b1, one 256-byte slot per material
    DirectX::XMFLOAT4 diffuse;
    DirectX::XMFLOAT3 specular;  float specularPower;
    DirectX::XMFLOAT3 ambient;   float edgeSize;
    DirectX::XMFLOAT4 edgeColor;
    uint32_t flags;              // MaterialShaderFlags
    float reflectivity;          // base SSR reflectivity derived from the MMD specular
    // Shader packs (GpuModel::SetShaderPack, pack_api.hlsli); only the pack PSOs read them.
    uint32_t packClass;          // PackClass of the material
    uint32_t packHeadBone;       // head bone index
    // Material morph texture factors (PMX): sampled colour = saturate(colour * mul + add) for the
    // texture, sphere and toon maps that the material has. Identity: mul 1, add 0.
    DirectX::XMFLOAT4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;
    DirectX::XMFLOAT4 packHead;       // xyz = head bone bind position, w = 1 when there is a head bone
    DirectX::XMFLOAT4 packParams[4];  // the pack's 16 parameters
};
static_assert(sizeof(MaterialConstants) == 256, "MaterialConstants layout");

// StructuredBuffer element for punctual lights (t6 in the scene pass). 112 bytes.
struct GpuLight {
    DirectX::XMFLOAT3 position; float invRange;
    DirectX::XMFLOAT3 color;    float spotCosOuter;   // colour premultiplied by intensity
    DirectX::XMFLOAT3 direction; float spotCosInner;
    float shadowSlice;           // spot shadow map slice (valid when < spotShadowParams.x), -1 = none
    float shadowType;            // 0 = NoCast, 1 = Hard, 2 = Soft
    float shadowSoftness;        // 0..1
    float shadowDensity;         // 0..1
    DirectX::XMFLOAT3 shadowColor; float falloff;     // falloff: 0 = None, 1 = Linear, 2 = InverseSquare
    float affectDiffuse;         // 1.0 = on, 0.0 = off
    float affectSpecular;        // 1.0 = on, 0.0 = off
    float pointShadowSlice;      // point shadow base slice (6 * shadowed point index), -1 = none
    float _pad;                  // S9: how much of the light shows in the haze (1 = physical, 0 = invisible in the medium)
    float areaWidth;
    float areaHeight;
    float isArea;                // 1.0 = area light, 0.0 = punctual point/spot
    float _pad2;                 // S9: this light's scattering anisotropy (VolLight::aniso); -1 = the fog's own
};
static_assert(sizeof(GpuLight) == 112, "GpuLight layout");

// S9: StructuredBuffer element for local participating-medium volumes (Renderer::kMaxFogVolumes, t3 space1).
// centre/radius describe the shape; box uses the half extents, sphere the radii.
struct GpuFogVolume {
    DirectX::XMFLOAT3 center; float density;   // density = multiplier added to the global medium inside
    DirectX::XMFLOAT3 extent; float edge;      // half size; 0..1 soft border band
    DirectX::XMFLOAT3 color; int sphere;       // medium tint (linear); 1 = ellipsoid, 0 = box
};
static_assert(sizeof(GpuFogVolume) == 48, "GpuFogVolume layout");

// S9c: the frame's participating medium for the offline GI path tracer (one element, t5 space1). mode 0 = the medium
// is not computed in the paths (the single-scatter pass or no haze at all); 1 = in-path media (full).
struct GpuMedium {
    float sigma0;    // height fog extinction at y <= 0 (already scaled by the density slider)
    float falloff;   // height falloff
    float radius;    // the haze sphere around the stage (VolumetricMediumRadius)
    float g;         // the fog's Henyey-Greenstein anisotropy (punctual / path continuation)
    DirectX::XMFLOAT3 tint;  // scattering albedo of the height fog (the volume colours blend over it)
    uint32_t mode;   // 0 = off, 1 = full (in-path media)
    uint32_t numFog;     // local fog volumes in the t4 space1 buffer
    uint32_t maxEvents;  // medium scattering events per path (0 = the shader default)
    float sunG;          // the sun's Henyey-Greenstein g (g above is the fog's, for punctual lights and continuations)
    float sunVolumetric; // the sun's share of the haze (LightParams::sunVolumetric)
};
static_assert(sizeof(GpuMedium) == 48, "GpuMedium layout");

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

// Vertex buffer slot 4 (static): additional UV1 / UV2 per vertex (MME models that use them). Models
// without additional UVs bind one zero element with stride 0. Also exposed to ray tracing as a raw
// (ByteAddress) SRV (GpuModel::AddUvSrv()).
struct GpuAddUv {
    float uv1[4];        // TEXCOORD8  additional UV1
    float uv2[4];        // TEXCOORD9  additional UV2
};
static_assert(sizeof(GpuAddUv) == 32, "GpuAddUv layout");

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
    RtGeom_PtPack     = 1u << 19,
    // S2b mesh dielectrics: the material's PBR record has transmission > 0. Built non-opaque in the BLAS so shadow
    // rays can see it (TraceShadowRayTinted) and the ray paths refract through it.
    RtGeom_Transmissive = 1u << 20,
};

// Pack record read by offline GI path tracer for geometries with RtGeom_PtPack. 160 bytes: a multiple of 16, because
// each record is a raw SRV (RtScene) whose byte offset must be 16-byte aligned.
struct RtPtPackRecord {
    uint32_t materialClass;
    uint32_t headValid;
    uint32_t texBase;
    uint32_t texInfo;
    DirectX::XMFLOAT4 headRight;   // xyz, w unused
    DirectX::XMFLOAT4 headUp;      // xyz, w unused
    DirectX::XMFLOAT4 headForward; // xyz, w unused
    float params[16];
    DirectX::XMFLOAT4 headPos;     // xyz = head bone world position, w = headScale (world units per model unit)
    uint32_t extSlotBase;          // params.ext: first float4 of this model's slots in gPackExtSlots (t3 space1)
    uint32_t extSlotCount;
    uint32_t pad[2];
};
static_assert(sizeof(RtPtPackRecord) == 160 && sizeof(RtPtPackRecord) % 16 == 0, "RtPtPackRecord layout");

// One entry per BLAS geometry. TLAS InstanceID = index of the model's first entry, so a hit's
// entry is gGeometries[CommittedInstanceID() + CommittedGeometryIndex()]. 192 bytes.
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
    uint32_t packSrv;      // SrvHeap index of raw ByteAddressBuffer holding RtPtPackRecord (when RtGeom_PtPack)
    uint32_t addUvSrv;     // SrvHeap index of raw ByteAddressBuffer with the add-UV stream (0xFFFFFFFF = none)
    float alphaKey;        // pack alpha key (0 = off): cut pixels whose texture red < alphaKey at alpha evaluation
    uint32_t pbrSrv;       // SrvHeap index of the model's GpuPbrMaterial StructuredBuffer (0xFFFFFFFF = none)
    uint32_t pbrIndex;     // material index into that buffer (0xFFFFFFFF = no PBR record)
    // material morph texture factors, as MaterialConstants::texMul..toonAdd
    DirectX::XMFLOAT4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;
};
static_assert(sizeof(RtGeometry) == 192, "RtGeometry layout");

// ---- per-material PBR record (work unit P2) ---------------------------------------
// Mirrors shaders/pbr_common.hlsli. One StructuredBuffer<GpuPbrMaterial> per model that has Ray maps; the raster
// PS reads it through its model SRV index (bound as root param 14) + material index, the ray paths through
// RtGeometry::pbrSrv / pbrIndex. From 0..9 and the UV hints are engine-neutral asset/PbrMaterial.h values.
inline constexpr uint32_t kGpuPbrSlotCount = 12;   // mirror of kPbrSlotCount (asset/PbrMaterial.h)
struct GpuPbrSlot {
    uint32_t type;         // Ray <PREFIX>_MAP_TYPE
    uint32_t swizzle;      // 0 R, 1 G, 2 B, 3 A
    uint32_t uvFlip;       // 0 none, 1 X, 2 Y, 3 both; NORMAL 4 = flip the shading normal X
    uint32_t applyScale;   // 0 none, 1 map * value, 2 map ^ value
    uint32_t colorFlip;    // CUSTOM_A/B: 1 inverts the value
    int32_t textureSrv;    // absolute SrvHeap index of the slot texture (None falls back to the constant)
    float loopU, loopV;    // <slot>MapLoopNum
    DirectX::XMFLOAT4 value;    // xyz = constant value, w = scale (normalMapScale / parallaxMapScale)
    DirectX::XMFLOAT4 uvScale;  // xy = <PREFIX>_MAP_UV_SCALE, zw = S2a KHR_texture_transform offset
};
static_assert(sizeof(GpuPbrSlot) == 64, "GpuPbrSlot layout");

struct GpuPbrMaterial {
    uint32_t flags;        // GpuPbrFlags: bit 0 valid, bit 1 cutout alpha, bit 2 emissive, bit 3 HDR emissive
    uint32_t shadingModel; // CUSTOM_ENABLE (0..9)
    float alphaThreshold;  // material_common ALPHA_THRESHOLD
    float diffuseAlpha;    // PMX material diffuse alpha (coverage / blend)
    DirectX::XMFLOAT4 params;   // x = ALBEDO_SUB_ENABLE blend mode (0 none)
    GpuPbrSlot slots[kGpuPbrSlotCount];
    // S2a glTF material extensions (asset/PbrMaterial.h PbrMaterial::Extensions). Neutral defaults as above; every
    // shader term multiplies by one of them (transmission 0, clearcoat 0, rotation 0), so Ray-MMD records render
    // exactly as before.
    DirectX::XMFLOAT4 transmission;   // x factor, y ior, z volume thickness factor, w attenuation distance (0 = inf)
    DirectX::XMFLOAT4 attenuation;    // xyz volume attenuation colour (linear), w clearcoat factor
    DirectX::XMFLOAT4 coat;           // x clearcoat roughness, y specular factor, z uv rotation, w sheen roughness
    DirectX::XMFLOAT4 sheen;          // xyz sheen colour (linear), w = transmission texture SRV as a NUMBER (-1 = none)
};
static_assert(sizeof(GpuPbrMaterial) == 864, "GpuPbrMaterial layout");

} // namespace mmdx
