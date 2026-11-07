// Shared scene constants and helpers. Mirror of src/render/ShaderInterop.h.
#ifndef MMDX_COMMON_HLSLI
#define MMDX_COMMON_HLSLI
#pragma pack_matrix(row_major)

// Bindless table indices that carry a "none" sentinel (-1 from a constant, 0xFFFFFFFF = DescriptorHeap::kInvalid) are
// guarded by a branch, but compilers / drivers may hoist the descriptor fetch above that branch. On AMD (issue #8) a
// hoisted fetch at index 0xFFFFFFFF read far outside the descriptor heap: page fault, device hung. Every such index goes
// through these, so a speculated fetch stays inside the heap (slot 0); the guard still decides whether it is used.
uint SafeSrv(int i) { return (uint)max(i, 0); }
uint SafeSrv(float f) { return (uint)max((int)f, 0); }
uint SafeSrvU(uint i) { return i == 0xFFFFFFFFu ? 0u : i; }

// PMX material morph texture factors, as MMD's default shader (full.fx) applies them:
// rgb = lerp(neutral, rgb * mul + add, mul.a + add.a); the factor alphas weight the effect and the
// texture alpha is not changed. Identity: mul 1, add 0. neutral = 1 (texture, toon, multiply
// sphere) or 0 (additive sphere), so a factor alpha of 0 turns the map off.
float3 ApplyTexFactor3(float3 c, float4 mulF, float4 addF, float neutral) {
    return saturate(lerp(neutral.xxx, c * mulF.rgb + addF.rgb, mulF.a + addF.a));
}
float4 ApplyTexFactor(float4 c, float4 mulF, float4 addF) {
    return float4(ApplyTexFactor3(c.rgb, mulF, addF, 1.0), c.a);
}

struct GpuPackInputRoleHLSL {
    float4 posValid; // xyz = world pos, w = valid (1 or 0)
    float4 right;    // xyz = right
    float4 up;       // xyz = up
    float4 forward;  // xyz = forward
};

cbuffer SceneCB : register(b0) {
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gInvProj;
    float4x4 gInvView;
    float4x4 gViewProjNoJitter;
    float4x4 gPrevViewProjNoJitter;
    float4x4 gShadowViewProj[3];
    float4 gCascadeSplits;   // view z far of cascade 0..2, w = shadows on
    float4 gShadowParams;    // x = 1/size, y = normal offset scale, z = softness (texels)
    float4 gCascadeTexel;    // xyz: world size of one shadow texel per cascade, w = depth reach behind each cascade
    float3 gEyePos;      float gTime;
    float3 gLightDir;    float gSunIntensity;
    float3 gLightColor;  float gHemiStrength;
    float3 gSkyZenith;   float gRimStrength;
    float3 gSkyHorizon;  float gNumLights;
    float3 gGroundColor; float gFloorGloss;
    float3 gRimColor;    float gFog;
    float2 gViewportSize; float gEdgeScale; float gTransparentBg;
    float2 gJitterUv;    float2 gInvViewportSize;
    float gNearZ; float gFarZ; float gFrameIndex; float gShading;   // shading: 0 Lit, 1 Unlit, 2 Wireframe
    float4x4 gPrevInvView;   // offline renderer: camera-to-world at shutter open
    // offline renderer scene extras (render benchmark): glass box, softboxes, floor
    float4 gGlassCenter;     // xyz, w = enabled
    float4 gGlassHalf;       // xyz half extents, w = corner radius
    float4 gGlassParams;     // cos yaw, sin yaw, ior (green), ior(blue) - ior(red)
    float4 gGlassAbsorb;     // xyz absorption per unit
    float4 gSoftbox[8];      // [i * 4 + 0..3]: centre (w = enabled), half U, half V, radiance
    float4 gFloorParams;     // xyz albedo, w = reflectivity (< 0: default floor)
    float4x4 gSpotViewProj[16];   // spot shadow map slices (Light.pad.x = slice)
    float4 gSpotShadowParams;    // x = slices rendered this frame, y = 1/mapSize
    float4 gSunShadowParams;     // x = shadowType (0=NoCast, 1=Hard, 2=Soft), y = softness, z = density, w = unused (sun shadows: only x/y/z are read)
    float4 gSunShadowColor;      // xyz = shadow colour (linear RGB), w = unused
    float4 gPointShadowParams;   // x = point lights shadowed this frame (0 = none), y = 1/pointMapSize
    float4 gAutoLumParams;       // x = AutoLuminous glow enabled (0/1), yzw unused
    float4 gBgOverride;          // xyz = transparent-export background colour (linear), w = enabled (offline GI)
    // Shader pack typed inputs (PACK_CAP_INPUTS_SCENE / PACK_CAP_INPUTS_AUDIO)
    GpuPackInputRoleHLSL gPackRoles[8];
    float4 gPackAudio0;          // x: beat, y: downbeat, z: onset, w: energy
    float4 gPackAudio1;          // x: section, y: bpm, z: valid, w: unused
    // S7a: the punctual-light CDF (Renderer::FillLightCdf): x = SrvHeap index of the raw ByteAddressBuffer of floats
    // (negative = none, use the uniform pick), y = light count.
    float4 gLightCdf;
    // S7c: the clustered light lists (Renderer::lightClusterBuf_): x = SrvHeap index of the raw ByteAddressBuffer
    // (negative = no clusters: loop over every light), y = the froxel grid packed as (tilesX & 0xFF) |
    // (tilesY & 0xFF) << 8 | (slices & 0xFF) << 16 (0 = none), z = log-slice scale, w = log-slice bias
    // (shaders/light_cluster.hlsli). Layout: ShaderInterop.h kCluster*.
    float4 gLightCluster;
    // S8a: the emissive-triangle pick table (ShaderInterop.h emissiveParams): x = SrvHeap index of the RawBuffer of
    // GpuEmissiveTri (negative = no emitters: no NEE, no MIS), y = the pick CDF index, z = entry count, w = the
    // per-geometry base table index. See shaders/emissive_nee.hlsli.
    float4 gEmissive;
    // S10: the frame's planar mirrors (ShaderInterop.h mirrorViewProj / mirrorPlane / mirrorParams): the frame
    // camera reflected about each mirror plane, with the frame camera's own projection. x = mirror count (0 = none),
    // y / z = SrvHeap index of each slot's reflection texture (-1 = none). Mirrors are raster only (RT / PT / GI
    // reflect natively).
    float4x4 gMirrorViewProj[2];
    float4 gMirrorPlane[2];   // xyz = unit world normal, w = -dot(normal, point on the plane)
    float4 gMirrorParams;
    // S10: the mirror pass's clip plane (world space, front side positive; 0 = off). Only the mirror pass sets it.
    float4 gClipPlane;
    // S7b: the frame's spot gobo table (ShaderInterop.h goboParams / shaders/gobo.hlsli): x = SrvHeap index of the raw
    // ByteAddressBuffer holding one GpuGobo per punctual light (negative = no gobo in the frame: no fetch, the old
    // image). A light's entry is (gobo texture SrvHeap index or -1, rotation radians, uv scale, focus mip bias).
    float4 gGoboParams;
    // S8b: the frame's screen lights (ShaderInterop.h GpuScreenLight / shaders/screen_light.hlsli): x = SrvHeap index
    // of the raw ByteAddressBuffer holding one GpuScreenLight per screen light (-1 = none: every screen term is
    // skipped), y = the count (at most kMaxScreenLights), z = SrvHeap index of the average-colour buffer (one float4
    // per light, shaders/screen_light_avg.hlsl), w unused.
    float4 gScreenLights;
    // S9b-1: the Physical Sky light (ShaderInterop.h skyParams / skyPerez* / shaders/physical_sky.hlsli): x = 1 with a
    // physical sky this frame (0 = today's hand-set gradient), y = the radiance scale, z = horizon haze 0..1,
    // w = the day factor (1 with the sun up, 0 below the horizon: only the night floor's rgb is left).
    float4 gSkyParams;
    float4 gSkyPerezY;       // A, B, C, D of the Y distribution
    float4 gSkyPerezX;       // A, B, C, D of the x distribution
    float4 gSkyPerezYy;      // A, B, C, D of the y distribution
    float4 gSkyPerezE;       // E of Y, x, y, unused
    float4 gSkyZenithOverF;  // zenith Y, x, y, each divided by its F(0, sun)
    float4 gSkySunToward;    // xyz = unit vector toward the sun
    float4 gSkyNight;        // xyz = the night floor's linear rgb
    // S9b-2: the sky environment buffer (ShaderInterop.h skyEnvParams / shaders/sky_env.hlsli): x = SrvHeap index of
    // the raw ByteAddressBuffer holding the sky's luminance CDF and SH irradiance (negative = none: the ambient stays
    // the hemisphere fill and the path tracers skip every sky light sample), y = W, z = H, w unused.
    float4 gSkyEnvParams;
    // S9b-3: the Sky light's IMAGE (ShaderInterop.h skyMapParams / shaders/sky_map.hlsli): x = SrvHeap index of the
    // RGBA16F equirect texture (-1 = none), y = the rotation around +y in radians, z = the radiance scale,
    // w = mirror (1 = flipped left / right). gSkyMapSize: x / y = the texture's width / height in texels, zw = 0.
    // gSkyParams.x = 2 while an image is loaded (1 = the procedural S9b-1 sky, 0 = the hand-set gradient).
    float4 gSkyMapParams;
    float4 gSkyMapSize;
    // S9b-4: the physical sky's cloud layer (ShaderInterop.h cloudParams / shaders/physical_sky.hlsli). gCloudParams =
    // (coverage, opacity, softness, heightKm) with coverage 0 = no clouds (the procedural sky draws as before);
    // gCloudParams2 = (scaleKm, windOffsetX_km, windOffsetZ_km, 0).
    float4 gCloudParams;
    float4 gCloudParams2;
};

static const float PI = 3.14159265;

float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(max(c, 0.0) * (1.0 / 1.055) + 0.055 / 1.055, 2.4); }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055; }
float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Interleaved gradient noise (Jimenez 2014).
float Ign(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

float2 OctWrap(float2 v) { return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0); }
float2 OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy;
}
float3 OctDecode(float2 f) {
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

// Normal-target z = the pixel's reflection tag (mmd.hlsl NormalTag, read by ssr / rtreflect / composite):
//   [0, 1]       MMD reflectivity: an F0-like value, the composite adds Schlick and blends the reflection over the pixel.
//   2            planar mirror (RT path: the full-resolution mirror pass owns the pixel).
//   (-1.75, 0)   no AO, no reflection (translucent stage pixels, world surfaces, emission).
//   <= -2        PBR: -(2 + W). The scene PS already added the sky as environment specular weighted by the split-sum
//                DFG; W is that DFG weight (luminance, x AO, faded out on rough lobes), and a traced / screen-space hit
//                replaces W x sky by W x the hit. A miss leaves the PS's sky alone.
// The target is alpha-blended like the colour: a blend of a PBR tag with 0 / -1 lands in the no-AO band (no
// reflection), never in the mirror or the MMD band.
static const float kReflTagPbrBase = -2.0;
bool ReflTagIsPbr(float z) { return z <= -1.75; }
float ReflTagPbrWeight(float z) { return saturate(-z - 2.0); }
bool ReflTagNoAo(float z) { return z < 0.0 && !ReflTagIsPbr(z); }
// The weight the reflection passes test before tracing (PBR: W, MMD: the reflectivity, mirror: 1).
float ReflTagGate(float z) { return ReflTagIsPbr(z) ? ReflTagPbrWeight(z) : min(z, 1.0); }

// Raw device depth -> positive view-space z (LH perspective).
float LinearZ(float d) { return gNearZ * gFarZ / (gFarZ - d * (gFarZ - gNearZ)); }

float3 ViewPosFromDepth(float2 uv, float d) {
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 v = mul(ndc, gInvProj);
    return v.xyz / v.w;
}

float2 ViewToUv(float3 v) {
    float4 c = mul(float4(v, 1.0), gProj);
    float2 ndc = c.xy / c.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// S9b-1: the Physical Sky light's radiance (needs the SceneCB fields above; read by SkyColor only).
#include "physical_sky.hlsli"

// S9b-3: the Sky light's image (shaders/sky_map.hlsli). Declared here and defined by each shader that calls SkyColor
// (after its bindless declarations): the six callers each supply their own texture table / sampler. composite.hlsl,
// which has no bindless table, defines the CPU-averages gradient instead (only the fog colour uses it there).
float3 SkyMapRadiance(float3 dir);

// Linear sky radiance for a world direction (also the fog colour source).
float3 SkyColor(float3 dir) {
    float y = dir.y;
    float up = saturate(y);
    float3 sky = lerp(gSkyHorizon, gSkyZenith, pow(up, 0.55));
    float3 below = lerp(gSkyHorizon, gGroundColor, saturate(-y * 6.0));
    float3 c = y >= 0.0 ? sky : below;
    float3 glowTint = gSkyHorizon;
    // S9b-3: an image sky replaces the model entirely (no procedural glow: the image has its own sun).
    if (gSkyParams.x > 1.5) {
        c = SkyMapRadiance(dir);
        glowTint = 0.0;
    } else if (gSkyParams.x > 0.5) {
        // S9b-1: a Physical Sky light replaces the hand-set gradient (its ground colour is written to gGroundColor by the
        // CPU, so the below-horizon blend stays the same) and draws the sun's glow in the sun's own colour.
        c = PhysicalSkyRadiance(dir);
        glowTint = SrgbToLinear(saturate(gLightColor));
    }
    // soft sun glow toward the key light (driven by sun colour so black = no glow)
    float s = saturate(dot(dir, -gLightDir));
    c += glowTint * ((gLightColor.r + gLightColor.g + gLightColor.b) > 1e-4 ? 1.0 : 0.0) * (pow(s, 24.0) * 0.35 + pow(s, 600.0) * 2.0) * (y > -0.05 ? 1.0 : 0.0);
    // S9b-4: the cloud layer over the procedural physical sky (coverage 0 / an image sky = no change)
    if (gSkyParams.x > 0.5 && gSkyParams.x < 1.5) c = PhysicalSkyClouds(dir, c);
    return c * gSunIntensity;
}

// Shared punctual light falloff curves (common.hlsli).
// falloffType: 0 = None (today's curve), 1 = Linear, 2 = InverseSquare.
// InverseSquare is normalised to a reference distance: the intensity is the light's brightness at
// kInverseSquareRefDist (25 units = 2 m, where it matches None within ~20 %), so the same intensity values work for
// every curve. A raw 1 / d^2 in MMD units made an intensity of 2 read 0.003 on a wall 27 units away.
static const float kInverseSquareRefDist = 25.0;
static const float kInverseSquareMinDist = 12.5;  // 1 m: caps the near-source peak at 4x (haze beams, fixtures)
float PunctualFalloff(float dist, float invRange, float falloffType) {
    float d = dist * invRange;
    if (d >= 1.0) return 0.0;
    float x = saturate(1.0 - pow(d, 4.0));
    if (falloffType > 1.5) {
        const float r = kInverseSquareRefDist / max(dist, kInverseSquareMinDist);
        return (x * x) * r * r;
    } else if (falloffType > 0.5) {
        return saturate(1.0 - d);
    } else {
        return (x * x) / (1.0 + dist * dist * 0.0004);
    }
}

// Soft shadows (sun and punctual lights, every path): softness 0..1 widens the light's angular radius from the
// path's hard light (`baseAngle`) to 15 degrees. The 1.5 power keeps the low end fine (0.5 = 5.3 degrees).
static const float kSoftShadowMaxAngle = 0.2618;
float SoftShadowAngle(float baseAngle, float softness) {
    return lerp(baseAngle, kSoftShadowMaxAngle, pow(saturate(softness), 1.5));
}
float SoftShadowConeCos(float baseCos, float softness) { return cos(SoftShadowAngle(acos(baseCos), softness)); }

// Punctual lights (spots / points) are small sources: 15 degrees is the sun's cap, so a punctual light's soft
// cone is a fraction of it (kPunctualSoftScale). Without the scale a default spot (softness 0.5) is a 5.3 degree
// light and its PCSS penumbra blurs the character's shadow away.
static const float kPunctualSoftScale = 0.25;
float PunctualSoftAngle(float softness) { return SoftShadowAngle(0.0, softness) * kPunctualSoftScale; }
// The punctual cone cos: the base cone combined with the punctual soft angle the same way SoftShadowConeCos
// combines its base cos with the sun's soft angle.
float PunctualShadowConeCos(float baseCos, float softness) {
    float t = pow(saturate(softness), 1.5);
    return cos(acos(baseCos) * (1.0 - t) + PunctualSoftAngle(softness));
}

// Sun shadow cone for the ray paths: Hard keeps `baseCos` (each path's own sun disc), Soft widens it with softness.
float SunShadowConeCos(float baseCos) {
    return (gSunShadowParams.x > 1.5) ? SoftShadowConeCos(baseCos, gSunShadowParams.y) : baseCos;
}

// Shared shadow transmission factoring in density and shadow colour tint.
// Returns a 0..1 RGB multiplier for the light term.
float3 ShadowTransmission(float vis, float density, float3 shadowColor) {
    float occ = (1.0 - vis) * density;
    return (1.0 - occ) + shadowColor * occ;
}

// そぼろ AutoLuminous convention (MME's most used glow effect): a PMX material the model author marked with a black
// specular colour (reflection colour 0,0,0) and a specular power (反射強度) of 100 or more emits. The strength scales
// with specularPower - 100 (110 = the documented standard, "1.0"); the emission colour is the material colour times
// the texture (the author paints the glow colour into the diffuse / texture). Returns added HDR radiance; 0 when the
// material carries no MAT_AUTO_LUMINOUS flag or the glow is off. `texLinear` is the linear base texture colour.
#ifndef MAT_AUTO_LUMINOUS
#define MAT_AUTO_LUMINOUS 512u
#endif
// The model's own .emd blend hints (render/ShaderInterop.h MaterialShaderFlags; a hinted material uses an additive
// / no-depth-write PSO). The raster PSO already carries the blend / write-mask / depth state.
#ifndef MAT_ADDITIVE
#define MAT_ADDITIVE 1024u
#endif
#ifndef MAT_NO_DEPTH_WRITE
#define MAT_NO_DEPTH_WRITE 2048u
#endif
// S5 stage screen (render/ShaderInterop.h MatFlag_Screen): an LED / video surface. The material's bound texture IS the
// emission: HDR radiance, scaled by the screen's brightness (kept in the material's reflectivity slot, which a screen's
// shading never reads as a reflection). No diffuse / specular / AO behind it.
#ifndef MAT_SCREEN
#define MAT_SCREEN 4096u
#endif
// S10 planar mirror (render/ShaderInterop.h MatFlag_Mirror): a perfect specular surface. The raster PS samples the
// frame's planar reflection texture (MirrorReflection); the ray paths reflect as a delta event. The material's base
// colour is the reflection tint, its roughness blurs the reflection (0 = sharp).
#ifndef MAT_MIRROR
#define MAT_MIRROR 8192u
#endif
float3 ScreenEmission(uint flags, float3 texLinear, float brightness) {
    if ((flags & MAT_SCREEN) == 0u) return 0.0;
    return max(texLinear, 0.0) * max(brightness, 0.0);
}
float3 AutoLumEmission(uint flags, float4 diffuse, float specPower, float3 texLinear) {
    if ((flags & MAT_AUTO_LUMINOUS) == 0u || gAutoLumParams.x < 0.5) return 0.0;
    float strength = clamp((specPower - 100.0) * 0.1, 0.0, 20.0);   // 110 -> 1.0 (documented standard), 100 -> 0
    float3 base = saturate(diffuse.rgb) * saturate(texLinear);      // author's emission colour (diffuse x texture)
    // HDR gain: AutoLuminous blurs every emitter with its own threshold-free glow; our bloom starts at luminance 1.1,
    // so the standard 110 must land above it for a typical saturated colour (pink 1, .4, .6 -> ~1.4).
    const float kAutoLumGain = 4.0;
    return base * strength * kAutoLumGain;
}

#endif
