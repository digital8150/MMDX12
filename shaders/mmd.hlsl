// Scene pass: MMD toon materials with modern lighting (cascaded shadows through the toon
// ramp, punctual lights, hemispheric fill, rim), plus the sky backdrop and studio floor.
// Writes MRT: 0 = linear HDR colour (alpha = coverage), 1 = oct view normal / reflectivity /
// coverage, 2 = velocity (uv current - uv previous).
#include "common.hlsli"
#include "shadow_common.hlsli"
#include "skinning.hlsli"
#ifdef RT_SHADOWS
#include "rt_common.hlsli"
#endif
#include "pbr_common.hlsli"   // includes dielectric.hlsli (S2b FresnelDielectric)
#include "light_cluster.hlsli"   // S7c: the fragment's froxel light list (after pbr_common: gPbrBuf)
#include "gobo.hlsli"            // S7b: spot gobos (the light loops below)
#include "screen_light.hlsli"    // S8b: stage screens as rectangular area lights (raster / RT)
#include "sky_env.hlsli"         // S9b-2: the Physical Sky's SH irradiance (the ambient fill below)
// S9b-3: the Sky light's image (SkyColor's gSkyParams.x > 1.5 branch); the raster / RT bindless texture table
#define SKY_MAP_TEX(i) gPbrTex[NonUniformResourceIndex((uint)(i))]
#define SKY_MAP_SAMPLER gPbrWrap
#include "sky_map.hlsli"

cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; float gReflectivity;
    uint gPackClass; uint gPackHeadBone;   // shader packs only (pack_api.hlsli); the default shading ignores them
    // material morph factors (ApplyTexFactor): texture, sphere, toon
    float4 gTexMul; float4 gTexAdd; float4 gSphereMul; float4 gSphereAdd; float4 gToonMul; float4 gToonAdd;
    float4 gPackHead;        // xyz = head bone bind position (model space), w = 1 when the model has a head bone
    float4 gPackParams[4];   // the pack's 16 parameters (pack.json order)
};

// Per-material PBR root constants (b3, root param 14 in ScenePass). Set every draw; gPbrActive is 0 unless the run
// uses --material-source ray, so the default raster stays byte-identical (the branch is not taken and the buffer
// reads dead-code eliminate). A shader pack shades from its own path and ignores these.
cbuffer PbrRoot : register(b3) {
    uint gPbrSrv;      // SrvHeap index of the model's GpuPbrMaterial buffer (0xFFFFFFFF = none)
    uint gPbrIndex;    // material index into that buffer (0xFFFFFFFF = no record)
    uint gPbrDebug;    // PbrDebug channel 0..7
    uint gPbrActive;   // 0 = no PBR record for this draw
    uint gModelIdInfo; // bit 7: isCharacter, bits 8-15: 1 + modelIndex
};
#ifndef MAT_HAS_TEXTURE
#define MAT_HAS_TEXTURE 1u
#define MAT_HAS_TOON    2u
#define MAT_SPHERE_MUL  4u
#define MAT_SPHERE_ADD  8u
#define MAT_RECEIVE     16u
#define MAT_STAGE       32u
#define MAT_TOON_MAP    64u
#define MAT_FLAT        128u
#define MAT_HAS_SPHERE_TEX 256u
#define MAT_AUTO_LUMINOUS 512u
#define MAT_ADDITIVE      1024u
#define MAT_NO_DEPTH_WRITE 2048u
#define MAT_SCREEN        4096u
#endif

// Shader-pack material class: the PackClass lives in bits 0..7 of gPackClass, bits 8..15 hold the class's alpha key
// (round(key * 255), pack.json "alphaKey"). Every consumer masks the class with PackClassLow; the key is PackAlphaKey.
#define PackClassLow(gc)    ((gc) & 0xFFu)
#define PackAlphaKey(gc)    (float(((gc) >> 8) & 0xFFu) * (1.0 / 255.0))

// FXC ignores pack_matrix for structured-buffer elements: state row_major explicitly.
struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
Texture2D gTexture : register(t1);
Texture2D gSphere  : register(t2);
Texture2D gToon    : register(t3);
StructuredBuffer<BoneMatrix> gPrevBones : register(t4);
Texture2DArray<float> gShadowMap : register(t5);
StructuredBuffer<Light> gLights : register(t6);
Texture2DArray<float> gSpotShadowMap : register(t7);
Texture2DArray<float> gPointShadowMap : register(t8);
SamplerState gWrap  : register(s0);
SamplerState gClamp : register(s1);
SamplerComparisonState gShadowCmp : register(s2);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2; float3 prevMorph : TEXCOORD3;
    float3 sdefC : TEXCOORD4; float3 sdefR0 : TEXCOORD5; float3 sdefR1 : TEXCOORD6; float sdef : TEXCOORD7;
    float4 uv1 : TEXCOORD8; float4 uv2 : TEXCOORD9;   // additional UV1/UV2 (zero without them)
};
struct VSOut {
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD1;
    float4 curClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    float viewZ : TEXCOORD4;
    float4 uv1 : TEXCOORD5;   // additional UV1 (zero without)
    float4 uv2 : TEXCOORD6;   // additional UV2 (zero without)
};
struct PSOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float2 velocity : SV_Target2;
#if defined(SCENE_HAS_ID) && SCENE_HAS_ID
    uint id : SV_Target3;
#endif
};

// Current / previous frame skinning (skinning.hlsli: linear blend or SDEF).
void Skin(VSIn v, out float3 wp, out float3 wn) {
    SkinVertex(gBones[v.bones.x].m, gBones[v.bones.y].m, gBones[v.bones.z].m, gBones[v.bones.w].m, v.weights,
               v.pos + v.morph, v.nrm, v.sdef, v.sdefC, v.sdefR0, v.sdefR1, wp, wn);
}
float3 PrevSkinPosition(VSIn v) {
    float3 wp, wn;
    SkinVertex(gPrevBones[v.bones.x].m, gPrevBones[v.bones.y].m, gPrevBones[v.bones.z].m, gPrevBones[v.bones.w].m,
               v.weights, v.pos + v.prevMorph, v.nrm, v.sdef, v.sdefC, v.sdefR0, v.sdefR1, wp, wn);
    return wp;
}

float2 Velocity(float4 curClip, float4 prevClip) {
    float2 c = curClip.xy / curClip.w;
    float2 p = prevClip.xy / prevClip.w;
    return (c - p) * float2(0.5, -0.5);
}

VSOut VSMain(VSIn v) {
    VSOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    float4 wp = float4(wp3, 1.0);
    float4 pwp = float4(PrevSkinPosition(v), 1.0);
    // precise: the depth pre-pass and the lit draw must rasterise the very same depth (the compiler may not contract
    // this differently for a PSO without a pixel shader)
    precise float4 clipPos = mul(wp, gViewProj);
    o.pos = clipPos;
    o.worldPos = wp.xyz;
    o.nrm = wn;
    o.uv = v.uv;
    o.curClip = mul(wp, gViewProjNoJitter);
    o.prevClip = mul(pwp, gPrevViewProjNoJitter);
    o.viewZ = mul(wp, gView).z;
    o.uv1 = v.uv1;
    o.uv2 = v.uv2;
    return o;
}

// ---- shadows ------------------------------------------------------------------
// (the punctual shadow-map code lives in shadow_common.hlsli: the scene pass and the world surfaces share it)

float ShadowCascade(float3 wp, float3 n, int c, float2 pixel) {
    float3 L = -gLightDir;
    float ndl = saturate(dot(n, L));
    float offset = gCascadeTexel[c] * gShadowParams.y * (1.5 - ndl);
    float4 sp = mul(float4(wp + n * offset, 1.0), gShadowViewProj[c]);
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    float a = Ign(pixel) * 6.2831853;
    if (gSunShadowParams.x > 1.5) {
        // Soft: orthographic cascade, so depth is linear over (2 * radius + reach) world units
        float tanA = tan(SoftShadowAngle(0.0, gSunShadowParams.y));
        float worldPerUv = gCascadeTexel[c] / gShadowParams.x;    // 2 * cascade radius
        float depthRange = worldPerUv + gCascadeTexel.w;
        float minUv = gShadowParams.z * gShadowParams.x;
        float z = p.z - 0.0004;
        float searchUv = clamp(tanA * 20.0 / worldPerUv, minUv, kPcssMaxUv);   // blockers up to ~20 units away
        float zb = PcssBlocker(gShadowMap, uv, c, z, searchUv, a, 1.0 / gShadowParams.x);
        if (zb < 0.0) return 1.0;
        float radius = clamp((z - zb) * depthRange * tanA / worldPerUv, minUv, kPcssMaxUv);
        return PcssFilter(gShadowMap, gShadowCmp, uv, c, z, radius, worldPerUv * ReceiverSlope(ndl) / depthRange, a);
    }
    float2x2 rot = float2x2(cos(a), -sin(a), sin(a), cos(a));
    float radius = gShadowParams.z * gShadowParams.x;
    float sum = 0;
    [unroll] for (int i = 0; i < 12; ++i) {
        float2 o = mul(kPoisson[i], rot) * radius;
        sum += gShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, c), p.z - 0.0004);
    }
    return sum / 12.0;
}

float Shadow(float3 wp, float3 n, float viewZ, float2 pixel) {
    if (gCascadeSplits.w < 0.5) return 1.0;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    if (viewZ > gCascadeSplits.z) return 1.0;
    float s = ShadowCascade(wp, n, c, pixel);
    // fade out at the far end of the last cascade
    float fade = saturate((gCascadeSplits.z - viewZ) / (gCascadeSplits.z * 0.15));
    return lerp(1.0, s, fade);
}

#ifdef RT_SHADOWS
// Two cone-jittered rays toward the sun (soft penumbra), honouring alpha-tested casters. S2b: a transmissive mesh
// tints the visibility instead of stopping the ray, so glass casts a coloured shadow here too.
float ShadowRt(float3 wp, float3 n, float viewZ, float2 pixel) {
    if (gCascadeSplits.w < 0.5 || gSunShadowParams.x < 0.5) return 1.0;
    float3 L = -gLightDir;
    float3 origin = wp + n * (0.02 + viewZ * 0.0004) + L * 0.01;
    uint rng = RngSeed((uint2)pixel, (uint)gFrameIndex, 7u);
    float vis = 0;
    float cosCone = SunShadowConeCos(0.99993);
    // Soft cones reach 15 degrees: four rays keep the wide penumbra from going grainy
    int rays = (gSunShadowParams.x > 1.5) ? 4 : 2;
    [loop] for (int k = 0; k < rays; ++k)
        vis += Luminance(saturate(TraceShadowRayTinted(origin, SampleCone(float2(Rand(rng), Rand(rng)), L, cosCone),
                                                       2000.0, 0xFFu)));
    return vis / (float)rays;
}
#define SHADOW_TERM(wp, n, viewZ, pixel) ShadowRt(wp, n, viewZ, pixel)
#else
#define SHADOW_TERM(wp, n, viewZ, pixel) Shadow(wp, n, viewZ, pixel)
#endif

// ---- punctual lights ------------------------------------------------------------

#ifdef RT_SHADOWS
// Two cone-jittered rays toward the point light, or random points on the area light, honouring alpha-tested casters.
float PointShadowRt(Light l, float3 wp, float3 n, float dist, uint lightIndex) {
    if (l.shadowType < 0.5 || l.pointShadowSlice < 0.0) return 1.0;
    float3 ld = (l.pos - wp) / max(dist, 1e-4);
    float3 origin = wp + n * 0.02 + ld * 0.01;
    uint2 seedCoord = (uint2)abs(float2(wp.x * 37.0 + wp.y * 17.0, wp.z * 37.0 + wp.y * 11.0));
    uint rng = RngSeed(seedCoord, (uint)gFrameIndex, 17u + lightIndex);
    float vis = 0;
    if (l.isArea > 0.5) {
        float3 norm = l.dir;
        float3 c = cross(float3(0, 1, 0), norm);
        float cLen = length(c);
        float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
        float3 up = cross(norm, right);
        float softMul = (l.shadowType > 1.5) ? (1.0 + l.shadowSoftness * 1.5) : 1.0;
        float w = l.areaWidth * softMul;
        float h = l.areaHeight * softMul;
        [unroll] for (int k = 0; k < 2; ++k) {
            float u = (Rand(rng) - 0.5) * w;
            float v = (Rand(rng) - 0.5) * h;
            float3 samplePos = l.pos + right * u + up * v;
            float3 rayDelta = samplePos - origin;
            float rayDist = length(rayDelta);
            float3 rayDir = rayDelta / max(rayDist, 1e-4);
            vis += Luminance(saturate(TraceShadowRayTinted(origin, rayDir, max(rayDist - 0.05, 0.0),
                                                           RT_MASK_CHARACTER)));
        }
        return vis * 0.5;
    }
    float cosCone = (l.shadowType > 1.5) ? PunctualShadowConeCos(0.99993, l.shadowSoftness) : 0.99993;
    [unroll] for (int k = 0; k < 2; ++k) {
        float3 rayDir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosCone);
        vis += Luminance(saturate(TraceShadowRayTinted(origin, rayDir, max(dist - 0.05, 0.0), RT_MASK_CHARACTER)));
    }
    return vis * 0.5;
}
#endif

float3 PunctualDiffuse(float3 wp, float3 n, float toonSoft, float flat) {
    float3 sum = 0;
    // S7c: the fragment's froxel light list (all lights when clustering is off)
    const LightList list = LightListFor(mul(float4(wp, 1.0), gView).xyz);
    for (uint k = 0; k < list.count; ++k) {
        const uint i = LightListAt(list, k);
        Light l = gLights[i];
        if (l.affectDiffuse <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        // S7b: the spot's gobo multiplies the light's colour (a coloured stencil tints it; 1 without one)
        const float3 lc = (l.cosOuter > -1.0) ? l.color * GoboSample(GoboOf(i), wp, l.pos, l.dir, l.cosOuter, 0.0)
                                              : l.color;
        float rawVis = 1.0;
        if (atten > 0.0) {
            if (l.cosOuter > -1.0) {
                rawVis = SpotShadow(l, wp, n, dist, gShadowCmp, gSpotShadowMap);
            } else {
#ifdef RT_SHADOWS
                rawVis = PointShadowRt(l, wp, n, dist, i);
#else
                rawVis = PointShadow(l, wp, n, dist, gShadowCmp, gPointShadowMap);
#endif
            }
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        float ndl = dot(n, ld);
        float diff = lerp(saturate(ndl), smoothstep(-0.05, 0.25, ndl), toonSoft);
        diff = lerp(diff, saturate(ndl * 0.3 + 0.7), flat);
        sum += lc * shadowTerm * atten * diff;
    }
    return sum;
}

float3 PunctualSpecular(float3 wp, float3 n, float3 V, float power) {
    float3 sum = 0;
    // S7c: the fragment's froxel light list (all lights when clustering is off)
    const LightList list = LightListFor(mul(float4(wp, 1.0), gView).xyz);
    for (uint k = 0; k < list.count; ++k) {
        const uint i = LightListAt(list, k);
        Light l = gLights[i];
        if (l.affectSpecular <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        float rawVis = 1.0;
        if (atten > 0.0) {
            if (l.cosOuter > -1.0) {
                rawVis = SpotShadow(l, wp, n, dist, gShadowCmp, gSpotShadowMap);
            } else {
#ifdef RT_SHADOWS
                rawVis = PointShadowRt(l, wp, n, dist, i);
#else
                rawVis = PointShadow(l, wp, n, dist, gShadowCmp, gPointShadowMap);
#endif
            }
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        float3 h = normalize(ld + V);
        // S7b: a spot's gobo multiplies the light's colour (the pattern shows in the specular lobe too)
        const float3 lc = (l.cosOuter > -1.0) ? l.color * GoboSample(GoboOf(i), wp, l.pos, l.dir, l.cosOuter, 0.0)
                                              : l.color;
        sum += lc * shadowTerm * atten * pow(saturate(dot(n, h)), power) * saturate(dot(n, ld));
    }
    return sum;
}

float3 Hemisphere(float3 n) {
    // S9b-2: with a Physical Sky the ambient is the sky's real irradiance (order-2 SH, shaders/sky_env.hlsli). SkyColor
    // already carries gSunIntensity, so the coefficient sum does too: do not scale it again.
    if (SkyEnvOn()) return SkyShIrradiance(n) / PI;
    return lerp(gGroundColor, gSkyZenith, saturate(n.y * 0.5 + 0.5)) * gSunIntensity;
}

// GGX specular for the punctual lights (P3): the same attenuation / shadow term as PunctualSpecular, but with the
// GGX lobe instead of Blinn. Added + scaled by the caller (SunIntensity lives in the light colour there).
float3 PunctualSpecularGGX(float3 wp, float3 n, float3 V, float rough, float3 f0) {
    float3 sum = 0;
    // S7c: the fragment's froxel light list (all lights when clustering is off)
    const LightList list = LightListFor(mul(float4(wp, 1.0), gView).xyz);
    for (uint k = 0; k < list.count; ++k) {
        const uint i = LightListAt(list, k);
        Light l = gLights[i];
        if (l.affectSpecular <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        if (atten <= 0.0) continue;
        float rawVis;
        if (l.cosOuter > -1.0) {
            rawVis = SpotShadow(l, wp, n, dist, gShadowCmp, gSpotShadowMap);
        } else {
#ifdef RT_SHADOWS
            rawVis = PointShadowRt(l, wp, n, dist, i);
#else
            rawVis = PointShadow(l, wp, n, dist, gShadowCmp, gPointShadowMap);
#endif
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        // S7b: a spot's gobo multiplies the light's colour
        const float3 lc = (l.cosOuter > -1.0) ? l.color * GoboSample(GoboOf(i), wp, l.pos, l.dir, l.cosOuter, 0.0)
                                              : l.color;
        sum += SrgbToLinear(saturate(lc)) * shadowTerm * atten * PbrSpecularGGX(n, V, ld, rough, f0);
    }
    return sum;
}

// Normal-target z: the reflectivity, or < 0 = "no AO / no SSR here" (composite.hlsl). Translucent stage pixels
// (alpha < 0.98) take the tag, so SSAO does not grey sheer curtains. Character and prop pixels keep their reflectivity:
// the normal target is alpha-blended, so a faint overlay on a face (blush, cheek lines, tears; drawn from alpha 0.004)
// pulled the face's value below zero and the face lost its AO in the overlay's whole footprint (pale rectangles).
// `glass` (S2b): a transmissive surface keeps its Fresnel reflectivity even where alpha < 0.98, so SSR / RT
// reflections show on it.
float NormalTag(float alpha, float reflectivity, bool glass) {
    return (alpha < 0.98 && !glass && (gFlags & MAT_STAGE) != 0u) ? -1.0 : reflectivity;
}

#if defined(SCENE_HAS_ID) && SCENE_HAS_ID
PSOut PackOutput(float3 color, float alpha, float3 worldNormal, float reflectivity, float4 curClip, float4 prevClip, uint customId = 0, bool glass = false) {
    PSOut o;
    o.color = float4(color, alpha);
    float3 vn = normalize(mul(worldNormal, (float3x3)gView));
    o.normal = float4(OctEncode(vn), NormalTag(alpha, reflectivity, glass), alpha);
    o.velocity = Velocity(curClip, prevClip);
    if (customId != 0u) {
        o.id = (alpha < 0.98) ? 0u : customId;
    } else {
        o.id = (alpha < 0.98) ? 0u : (((7u & 7u) << 4) | (gModelIdInfo & 0xFF80u));
    }
    return o;
}
#else
// Same parameter list as the ID variant (a caller may pass both), the id target is simply absent here.
PSOut PackOutput(float3 color, float alpha, float3 worldNormal, float reflectivity, float4 curClip, float4 prevClip,
                 uint customId = 0, bool glass = false) {
    PSOut o;
    customId = customId;
    o.color = float4(color, alpha);
    float3 vn = normalize(mul(worldNormal, (float3x3)gView));
    o.normal = float4(OctEncode(vn), NormalTag(alpha, reflectivity, glass), alpha);
    o.velocity = Velocity(curClip, prevClip);
    return o;
}
#endif

// ---- MMD material -----------------------------------------------------------------

// S10: the mirror plane pick for a mirror pixel (the slot the pixel lies on; -1 = none). A mirror pixel is drawn by
// MatFlag_Mirror: the frame's planar reflection is tinted by the material's base colour and blurred by its roughness.
int MirrorSlotOf(float3 worldPos, float3 normal) {
    if (gMirrorParams.x < 0.5) return -1;
    const int count = min((int)gMirrorParams.x, 2);
    [unroll] for (int i = 0; i < 2; ++i) {
        if (i >= count) break;
        // the interpolated position may sit a hair off the fitted plane (0.5 MMD units is well below a pixel at any
        // usable distance); the surface normal must be the plane's, so another plane's pixels never match
        if (abs(dot(gMirrorPlane[i].xyz, worldPos) + gMirrorPlane[i].w) < 0.5 && dot(gMirrorPlane[i].xyz, normal) > 0.5)
            return i;
    }
    return -1;
}
// The reflection texture's SrvHeap index for a slot (-1 = none this frame: the mirror shows its tint only).
int MirrorSrv(int slot) {
    if (slot == 0) return (int)gMirrorParams.y;
    if (slot == 1) return (int)gMirrorParams.z;
    return -1;
}

// ---- per-material PBR debug view (work unit P2) ---------------------------------------------
// A material with a PBR record outputs one channel (CLI --pbr-debug); materials without one render as before.
// The buffer is read through the model's SRV (absolute SrvHeap index), as the ray paths do.
PSOut PbrDebugOutput(in VSOut i, in GpuPbrMaterial m, float3 n, float alpha) {
    float2 uv = i.uv;
    float3 outColor;
    if (gPbrDebug == 1u) {          // albedo (Ray's linear albedo)
        outColor = saturate(PbrAlbedo(m, uv, gDiffuse.rgb));
    } else if (gPbrDebug == 2u) {   // normal: tangent-space normal into the world
        outColor = PbrWorldNormal(m, n, i.worldPos, uv) * 0.5 + 0.5;
    } else if (gPbrDebug == 3u) {   // roughness = SmoothnessToRoughness(smoothness)
        outColor = PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], uv).xxx;
    } else if (gPbrDebug == 4u) {   // metalness
        outColor = PbrSlotMetalness(m.slots[PBR_SLOT_METALNESS], uv).xxx;
    } else if (gPbrDebug == 5u) {   // ambient occlusion
        outColor = PbrSlotAo(m.slots[PBR_SLOT_OCCLUSION], uv).xxx;
    } else if (gPbrDebug == 6u) {   // emissive
        outColor = saturate(PbrEmissive(m, uv));
    } else {                        // shading model as a distinct hue
        outColor = PbrModelHue(m.shadingModel);
    }
    return PackOutput(outColor, alpha, n, gReflectivity, i.curClip, i.prevClip);
}

// MMD colour model, unchanged (P2 keeps the default look byte-identical).
PSOut MmdShade(VSOut i, bool front, float3 n, float3 L, float3 V, float4 tex, float alpha) {
    // MMD colour model (gamma space): saturate(ambient + diffuse * light) * texture * sphere.
    float3 lit = saturate(gAmbient + gDiffuse.rgb * gLightColor) * tex.rgb;
    // Albedo for the modern fill terms. MMD diffuse is a light-response factor, and rigs use
    // either d=1.0/a=0.5 or d=0.8/a=0.6 for the same "fully lit = texture" look; base the
    // characters' albedo on that fully-lit colour so both conventions get the same fill.
    float3 albedo = (gFlags & MAT_STAGE) ? gDiffuse.rgb * tex.rgb
                                         : saturate(gAmbient + gDiffuse.rgb) * 0.8 * tex.rgb;
    if (gFlags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 s = ApplyTexFactor3(gSphere.Sample(gClamp, suv).rgb, gSphereMul, gSphereAdd, (gFlags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (gFlags & MAT_SPHERE_MUL) { lit *= s; albedo *= s; } else { lit += s; }
    }

    float rawSh = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy) : 1.0;
    float occ = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occ;
    float ndl = dot(n, L);
    float flat = (gFlags & MAT_FLAT) ? 1.0 : 0.0;
    float3 c;
    if (gFlags & MAT_TOON_MAP) {
        // Project Sekai layout: the "toon" is the painted shadow colour at the same UV.
        // Hard terminator around N.L = 0 (their _SekaiShadowThreshold 0.5 on half-Lambert).
        float3 shadowTex = ApplyTexFactor3(gToon.Sample(gWrap, i.uv).rgb, gToonMul, gToonAdd, 1.0);
        float term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        float3 light = saturate(gAmbient + gDiffuse.rgb * gLightColor);
        c = lerp(light * shadowTex, lit, term);
    } else if (gFlags & MAT_HAS_TOON) {
        // The cast shadow pushes the lookup toward the dark end of the material's own ramp.
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = lit * ApplyTexFactor3(gToon.Sample(gClamp, float2(0.5, v)).rgb, gToonMul, gToonAdd, 1.0);
    } else if (gFlags & MAT_STAGE) {
        // `c` is the sun-lit MMD colour (scaled by gSunIntensity below); the hemisphere / punctual fill added later
        // stays, so full shadow at 0.42 reads about as dark as the studio floor (PSFloor).
        c = lerp(lit * 0.42, lit, sh);
    } else {
        c = lit;   // unmodelled (flat) character material: only cast shadows below
    }
    if (!(gFlags & (MAT_STAGE | MAT_TOON_MAP))) {
        // Hue-preserving shadow (c * c, slightly cooled): light MMD toon ramps alone read as
        // unlit; this keeps skin warm and hair rich on the shaded side. Flat materials skip
        // the N.L terminator and take a softened cast shadow only.
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += lit * gSunShadowColor.rgb * occ;
    if (gSpecularPower > 0.0) {
        float3 h = normalize(L + V);
        c += pow(saturate(dot(h, n)), gSpecularPower) * gSpecular * gLightColor * sh;
    }

    float3 albedoLin = SrgbToLinear(saturate(albedo));
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    // Flat materials take an even fill (no sky/ground gradient modelling the face).
    float3 hemi = lerp(Hemisphere(n), lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity, flat * 0.85);
    color += albedoLin * hemi * gHemiStrength;
    color += albedoLin * PunctualDiffuse(i.worldPos, n, 1.0, flat);
    // S8b: the stage screens' area-light term (LED walls / video surfaces light the performer and the floor). Zero cost
    // without a screen light in the frame.
    color += albedoLin * ScreenLightsDiffuse(i.worldPos, n);

    float reflectivity = gReflectivity;
    if (gFlags & MAT_STAGE) {
        float floorMask = smoothstep(0.82, 0.97, n.y);
        reflectivity = saturate(reflectivity + gFloorGloss * floorMask);
        color += PunctualSpecular(i.worldPos, n, V, 48.0) * reflectivity;
        color += ScreenLightsSpecularBlinn(i.worldPos, n, V, 48.0) * reflectivity;
    } else {
        // anime rim light: grazing angles on the side the key light comes from
        float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
        float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    }
    // AutoLuminous (そぼろ): a marked material adds its own colour as HDR radiance after lighting so bloom picks it
    // up. Off (gAutoLumParams.x 0) or unmarked = 0, the pre-existing frame.
    color += AutoLumEmission(gFlags, gDiffuse, gSpecularPower, SrgbToLinear(saturate(tex.rgb)));
    return PackOutput(color, alpha, n, alpha > 0.9 ? reflectivity : 0.0, i.curClip, i.prevClip);
}

// ---- PBR material shading (P3) --------------------------------------------------------------
// Analytic split-sum environment BRDF (Karis 2014 after Lazarov 2013): F0 * A + B for perceptual roughness `pr`.
float3 PbrEnvBrdfApprox(float3 f0, float pr, float nv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = pr * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// "PBR on top of the MMD look": the MMD structure above stays; for a material with a PBR record only its inputs /
// terms change (mapped normal, albedo / alpha, sphere ignored, metalness, GGX + env specular, AO, emissive, and the
// per-model lobes). Sun / hemispheric / punctual diffuse keeps the MMD look.
PSOut MmdShadePbr(VSOut i, bool front, GpuPbrMaterial m, float3 n0, float4 tex, float alpha) {
    const uint model = m.shadingModel;
    const bool celLike = (model == 8u || model == 9u);   // CEL / TONEBASED: MMD shading + normal + emissive only
    float2 uv = i.uv;

    // Mapped normal (NORMAL + NORMAL_SUB) replaces n for every term, including the written normal.
    float3 n = PbrWorldNormal(m, n0, i.worldPos, uv);
    float3 L = -gLightDir;
    float3 V = normalize(gEyePos - i.worldPos);

    // ALPHA slot cut-out: only the Cutout variants of the material file enforce a hard alpha threshold (0.2/0.5/
    // 0.75, PBR_FLAG_CUTOUT). The standard 0.999 threshold is an opaque-pass detail that would otherwise kill
    // blended materials in this single-pass engine; those keep the caller's gDiffuse.a * texture alpha coverage.
    if ((m.flags & PBR_FLAG_CUTOUT) && PbrAlpha(m, uv) < m.alphaThreshold) discard;

    // ALBEDO (+ sub blend) replaces the texture; a slot that reads the PMX texture (FROM 3) is unchanged by
    // construction. Sphere map is ignored for PBR materials (Ray-MMD does not use it). CEL / TONEBASED keep the
    // MMD shading, so they keep the plain texture as the diffuse colour. The MMD colour pipeline below is
    // gamma-space, so the linear PBR albedo goes back through LinearToSrgb for the `c` term.
    float3 pbrAlbedo = PbrAlbedo(m, uv, gDiffuse.rgb);          // linear
    float3 pbrAlbedoGamma = LinearToSrgb(saturate(pbrAlbedo));
    float3 celAlbedo = (gFlags & MAT_STAGE) ? gDiffuse.rgb * tex.rgb
                                            : saturate(gAmbient + gDiffuse.rgb) * 0.8 * tex.rgb;
    float3 diffAlbedo = celLike ? celAlbedo : pbrAlbedoGamma;   // gamma, for the MMD diffuse term

    // S10 planar mirror: the surface is the frame's planar reflection tinted by the material's own colour and blurred
    // by its roughness (no diffuse / specular of its own). The reflection texture is the scene drawn from the camera
    // reflected about this material's plane; a pixel samples it by its own world position (which lies on the plane),
    // so the projection is exact. Without a reflection texture for this frame (quad / ortho views, camera feeds,
    // RenderToImage) the material shows its tint only. Inside the mirror pass itself no slot matches, so the mirror
    // surfaces of the reflected view draw their tint (no recursion). ISO rough -> the record's perceptual roughness.
    if ((gFlags & MAT_MIRROR) != 0) {
        const int slot = MirrorSlotOf(i.worldPos, n);
        const float rough = PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], uv);
        const float3 refl = MirrorReflection(MirrorSrv(slot), slot, i.worldPos, pbrAlbedo, rough);
        // A planar reflection this frame owns the pixel (reflectivity 0: the screen-space / ray-traced reflection must
        // not add a second one over it). Without a slot (RT / PT / GI, quad, feeds) reflectivity 2 marks a mirror pixel
        // for the ray-traced pass (2 = full-resolution opaque mirror reflection; every other reader clamps to 1).
        return PackOutput(refl, alpha, n, slot >= 0 ? 0.0 : 2.0, i.curClip, i.prevClip);
    }

    float rawSh = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy) : 1.0;
    float occAmt = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occAmt;
    float ndl = dot(n, L);
    const float flat = (gFlags & MAT_FLAT) ? 1.0 : 0.0;
    const float3 mddLight = saturate(gAmbient + gDiffuse.rgb * gLightColor);   // MMD diffuse * light (gamma)
    const float3 sunLin = SrgbToLinear(saturate(gLightColor)) * gSunIntensity;

    // TONEBASED (9): the MMD shading is kept, but the diffuse is replaced by a hardness toon toward the customB
    // shadow colour. Normal + emissive only otherwise, per the plan.
    if (model == 9u) {
        float hard = saturate(PbrCustomA(m, uv));
        float3 shadowColor = PbrCustomB(m, uv);
        float litAmt = saturate(ndl * hard + 1.0 - hard) * sh;
        float3 c9 = SrgbToLinear(saturate(mddLight * tex.rgb)) * lerp(shadowColor, 1.0, litAmt) * gSunIntensity;
        float3 rim9 = pow(1.0 - saturate(dot(n, V)), 4.0) * smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        c9 += gRimColor * rim9 * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
        c9 += saturate(PbrEmissive(m, uv));
        return PackOutput(c9, alpha, n, alpha > 0.9 ? gReflectivity : 0.0, i.curClip, i.prevClip);
    }

    // MMD diffuse colour model (gamma). CEL keeps the toon ramp.
    float metal = celLike ? 0.0 : saturate(PbrSlotMetalness(m.slots[PBR_SLOT_METALNESS], uv));
    float3 cAlbedo = diffAlbedo * (1.0 - metal);   // metalness removes the diffuse
    float3 c = mddLight * cAlbedo;
    if (model == 8u) {
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = mddLight * tex.rgb * ApplyTexFactor3(gToon.Sample(gClamp, float2(0.5, v)).rgb, gToonMul, gToonAdd, 1.0);
    } else if (!celLike) {
        // Hue-preserving shadow, kept for the PBR albedo too (MMD look).
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += mddLight * cAlbedo * gSunShadowColor.rgb * occAmt;

    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;

    // Lambert metalness: the fill terms use (1 - metalness) too.
    float3 f0mma = PbrSlotF0(m.slots[PBR_SLOT_SPECULAR], uv);
    float3 albedoLin = celLike ? SrgbToLinear(saturate(celAlbedo)) : saturate(pbrAlbedo);
    float3 diffLin = albedoLin * (1.0 - metal);
    float3 f0 = celLike ? f0mma : lerp(f0mma, max(0.02, albedoLin), metal);
    float rough = max(PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], uv), 0.03);
    float ao = PbrSlotAo(m.slots[PBR_SLOT_OCCLUSION], uv);

    bool specularOn = (model != 8u);
    bool envSpecOn = !celLike;
    float specScale = 1.0;
    if (model == 1u) {   // SKIN: strict look policy (soft SSS tint, specular x <= 0.15, no env specular)
        float curve = PbrCustomA(m, uv);
        float3 trans = PbrCustomB(m, uv);
        float ss = pow(1.0 - saturate(ndl), 2.0) * pow(1.0 - saturate(-ndl), 3.0 / (curve + 0.001)) *
                   saturate(curve - 0.04);
        color += trans * (ss * 0.7) * sunLin * sh;
        rough = max(rough, 0.6);
        specScale = 0.15;
        envSpecOn = false;
    }

    // Hemispheric + punctual diffuse fill, scaled by (1 - metalness) and the PBR AO.
    float3 hemi = lerp(Hemisphere(n), lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity, flat * 0.85);
    color += diffLin * hemi * gHemiStrength * ao;
    color += diffLin * PunctualDiffuse(i.worldPos, n, 1.0, flat) * ao;
    // S8b: the screen area lights (one-sided, unshadowed; the same average-colour scale as the screen's emission)
    color += diffLin * ScreenLightsDiffuse(i.worldPos, n) * ao;

    // Environment specular: the sky the path tracer reflects (SkyColor at full radiance, not the hemiStrength fill),
    // seen along the GGX dominant direction and pre-blurred toward the normal's sky for rough lobes, weighted by the
    // analytic split-sum DFG (Karis, "Physically Based Shading on Mobile") x AO. Metals get their colour from here.
    // The same DFG weight goes to the reflection passes (NormalTag's PBR tag): where a reflection ray hits something,
    // the composite swaps this sky for what the ray hit, so a wet road mirrors the performers as strongly as the sky.
    // One sharp ray stands in for the lobe, so rough lobes fade the swap out (they keep the pre-blurred sky).
    float pbrReflW = 0.0;
    if (envSpecOn) {
        float nv = saturate(dot(n, V));
        float pr = sqrt(saturate(rough));   // perceptual roughness (rough is GGX alpha)
        float3 R = reflect(-V, n);
        float3 Rd = normalize(lerp(n, R, (1.0 - pr) * (sqrt(1.0 - pr) + pr)));
        float3 env = lerp(SkyColor(Rd), Hemisphere(n), pr * pr);
        const float3 dfg = PbrEnvBrdfApprox(f0, pr, nv);
        color += dfg * env * ao;
        pbrReflW = saturate(dot(dfg, float3(0.2126, 0.7152, 0.0722)) * ao * (1.0 - smoothstep(0.3, 0.65, pr)));
    }

    // Sun specular (replaces the MMD Blinn), x N.L x light colour x shadow, per shading model.
    float nl = saturate(ndl);
    if (specularOn) {
        float3 spec;
        if (model == 3u) {                       // ANISOTROPY (hair): anisotropic GGX along the tangent
            float3 T, B;
            PbrTangentFrame(n, i.worldPos, uv, T, B);
            float aniso = PbrCustomA(m, uv);
            float3 cb = PbrCustomB(m, uv);
            float shift = cb.r;
            spec = PbrSpecularGGXAniso(n, V, L, rough, f0, aniso, T, shift);
        } else if (model == 5u) {                // CLOTH: GGX lerped with a sheen lobe (customB = fuzz colour)
            float3 fuzz = PbrCustomB(m, uv);
            float sheenAmt = PbrCustomA(m, uv);
            float3 ggx = PbrSpecularGGX(n, V, L, rough, f0);
            float3 sheen = PbrSpecularSheen(n, V, L, rough, fuzz);
            spec = lerp(ggx, sheen, saturate(sheenAmt));
        } else {
            spec = PbrSpecularGGX(n, V, L, rough, f0);
        }
        color += spec * sh * sunLin * nl * specScale;
        color += PunctualSpecularGGX(i.worldPos, n, V, rough, f0) * specScale;
        // S8b: the screen area lights' GGX lobe (the representative point, widened by the screen's angular radius)
        color += ScreenLightsSpecularGGX(i.worldPos, n, V, rough, f0) * specScale;
    }
    // S2a KHR_materials_clearcoat: a second, smoother GGX lobe over the base layer with F0 0.04 (glTF's coat
    // reflectance). Zero cost when the factor is 0 (Ray-MMD / plain glTF materials never enter).
    const float coat = PbrClearcoat(m);
    if (coat > 0.0f && !celLike) {
        const float coatRough = max(PbrClearcoatRoughness(m), 0.03);
        const float3 coatF0 = 0.04;
        color += PbrSpecularGGX(n, V, L, coatRough, coatF0) * (coat * sh * sunLin * nl);
        color += PunctualSpecularGGX(i.worldPos, n, V, coatRough, coatF0) * coat;
        // The coat reflects part of the base light: attenuate the base layer by (1 - coat Fresnel).
        const float coatF = 0.04 + 0.96 * pow(1.0 - saturate(dot(n, V)), 5.0);
        color *= 1.0 - coat * coatF;
    }

    // Reflectivity output (SSR / RT reflections): F0 luminance scaled by smoothness, so smooth / metal materials
    // mirror and rough dielectrics barely do. CEL keeps the MMD value. A stage with a record drops its floor gloss
    // (floorGloss is only for stage materials without a PBR record, per the plan).
    float reflectivity = celLike ? gReflectivity
                                 : saturate(dot(f0, float3(0.2126, 0.7152, 0.0722)) * (1.0 - rough));
    if (gFlags & MAT_STAGE) {
        color += PunctualSpecular(i.worldPos, n, V, 48.0) * reflectivity;
        color += ScreenLightsSpecularBlinn(i.worldPos, n, V, 48.0) * reflectivity;
    } else {
        // Rim stays; metals tint it by F0 (lerp toward F0's luminance).
        float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
        float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        float3 rimTint = lerp(float3(1, 1, 1), dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)).xxx, metal);
        color += gRimColor * rimTint * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    }

    // Emissive (linear * intensity) after lighting. EMISSIVE promotion (custom 2) is the same term; the value is
    // 0 for the library materials that leave EMISSIVE_ENABLE off. PbrEmissive returns linear already and does its own
    // saturate for non-HDR records (S2a: an LED panel's strength is not clamped).
    color += PbrEmissive(m, uv);
    // S2b mesh dielectric, raster / RT camera view: no screen-space refraction in this milestone. The see-through is
    // physically flavoured instead of S2a's flat 0.85: alpha = 1 - transmission * (1 - Fresnel(N.V, ior)), what passes
    // through is tinted by the base colour (glTF), and the reflectivity output is the interface Fresnel, so SSR / RT
    // reflections show on the glass. Reflectivity always carries at least the S2a value (smooth dielectrics mirror).
    const float transp = PbrTransmissionAt(m, uv);
    if (transp > 0.0) {
        const float nv = saturate(dot(n, V));
        const float fres = FresnelDielectric(nv, max(m.transmission.y, 1.0));
        const float coverage = saturate(1.0 - transp * (1.0 - fres));
        // the blended layer keeps the refracted (tinted) light rather than the shaded opaque surface
        color = lerp(color, color * pbrAlbedo, transp * (1.0 - fres));
        reflectivity = max(reflectivity, fres);
        alpha *= coverage;
        return PackOutput(color, alpha, n, reflectivity, i.curClip, i.prevClip, 0u, true);
    }
    // Environment-lit PBR surfaces carry the DFG weight (PBR tag); CEL / skin keep the MMD reflectivity.
    const float tag = envSpecOn ? kReflTagPbrBase - pbrReflW : reflectivity;
    return PackOutput(color, alpha, n, alpha > 0.9 ? tag : 0.0, i.curClip, i.prevClip);
}

// Per-material PBR branch entry (gPbrActive is 0 unless the run has Ray records for this draw). --pbr-debug wins
// over the PBR shading; the lit path only. The default run never enters either, so its frame is unchanged.
PSOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    PbrSetUvGradients(ddx(i.uv), ddy(i.uv));
    // S10: the mirror pass's clip plane: geometry behind the mirror must not leak into the reflection (the oblique
    // near plane is approximated by this world-space test). Every other pass keeps (0,0,0,0).
    if (dot(gClipPlane, float4(i.worldPos, 1.0)) < 0.0) discard;
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    float3 L = -gLightDir;
    float3 V = normalize(gEyePos - i.worldPos);

    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd)
                                            : float4(1, 1, 1, 1);
    float alpha = gDiffuse.a * tex.a;
    if (alpha < 0.004) discard;

    // S5: a stage screen surface shows only its bound texture, as HDR emission (see ScreenEmission).
    // nt.z = -1: emission takes no AO (RTAO's per-frame noise dimmed whole far screens in TAA-less video renders).
    if (gFlags & MAT_SCREEN) {
        float3 emission = ScreenEmission(gFlags, SrgbToLinear(saturate(tex.rgb)), gReflectivity);
        return PackOutput(emission, alpha, n, -1.0, i.curClip, i.prevClip);
    }

    if (gPbrActive != 0u && gShading < 0.5) {
        GpuPbrMaterial pbr = PbrLoadMaterial(gPbrSrv, gPbrIndex);
        if (gPbrDebug != 0u) return PbrDebugOutput(i, pbr, n, alpha);
        return MmdShadePbr(i, front, pbr, n, tex, alpha);
    }

    // Unlit (gShading 1): texture * material diffuse colour only, still alpha-tested/blended.
    if (gShading > 0.5 && gShading < 1.5)
        return PackOutput(SrgbToLinear(saturate(gDiffuse.rgb * tex.rgb)), alpha, n, gReflectivity,
                          i.curClip, i.prevClip);

    return MmdShade(i, front, n, L, V, tex, alpha);
}

// DLSS Ray Reconstruction guides (RayTraced path, ScenePass::DrawGuides): default material data even for
// pack-shaded / MME-owned materials. t0 = diffuse albedo (linear), t1 = specular albedo (EnvBRDFApprox2),
// t2 = world normal, a = linear roughness. Sky / no coverage stays at the cleared defaults.
struct GuideOut {
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
    float4 normalRough : SV_Target2;
};

float3 GuideSpecularAlbedo(float3 specColor, float alpha, float NoV) {
    NoV = abs(NoV);
    float4 X = float4(1.0, NoV, NoV * NoV, NoV * NoV * NoV);
    float4 Y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specColor.g * 50.0);
    return mad(specColor, max(0.0, scale), max(0.0, bias));
}

GuideOut PSGuides(VSOut i, bool front : SV_IsFrontFace) {
    PbrSetUvGradients(ddx(i.uv), ddy(i.uv));
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    float3 V = normalize(gEyePos - i.worldPos);
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd)
                                            : float4(1, 1, 1, 1);
    float alpha = gDiffuse.a * tex.a;
    clip(alpha - 0.004);

    float3 albedo = SrgbToLinear(saturate(gDiffuse.rgb * tex.rgb));
    float3 spec = gReflectivity.xxx;
    float rough = clamp(sqrt(2.0 / (max(gSpecularPower, 0.0) + 2.0)), 0.03, 0.6);
    if (gPbrActive != 0u && gShading < 0.5) {
        GpuPbrMaterial pbr = PbrLoadMaterial(gPbrSrv, gPbrIndex);
        const bool cel = pbr.shadingModel == 8u || pbr.shadingModel == 9u;
        const float3 albedoLin = PbrAlbedo(pbr, i.uv, gDiffuse.rgb);
        const float metal = cel ? 0.0 : saturate(PbrSlotMetalness(pbr.slots[PBR_SLOT_METALNESS], i.uv));
        const float3 f0mma = PbrSlotF0(pbr.slots[PBR_SLOT_SPECULAR], i.uv);
        albedo = albedoLin * (1.0 - metal);
        spec = cel ? f0mma : lerp(f0mma, max(0.02, albedoLin), metal);
        rough = max(PbrSlotRoughness(pbr.slots[PBR_SLOT_SMOOTHNESS], i.uv), 0.03);
        if (pbr.shadingModel == 1u) rough = max(rough, 0.6);   // SKIN: never glossy
    }
    GuideOut o;
    // alpha = coverage: the PSO blends the albedo guides of translucent layers over what is below
    o.diffuse = float4(saturate(albedo), alpha);
    o.specular = float4(GuideSpecularAlbedo(spec, rough * rough, dot(n, V)), alpha);
    o.normalRough = float4(n, saturate(rough));   // opaque: w is RR's packed roughness
    return o;
}

// ---- shader packs ---------------------------------------------------------------------
// Compiled only for a pack's PSO: MMDX_PACK is the pack's surface.hlsl (a quoted include path), which implements
// PackShade (contract: pack_api.hlsli). PSMain above is never affected.
#ifdef MMDX_PACK
#include "pack_api.hlsli"
#include MMDX_PACK

// The pack sets PACK_HAS_EDGE itself (surface.hlsl); without it the edge pass is unchanged.
#ifndef PACK_HAS_EDGE
#define PACK_HAS_EDGE 0
#endif

PSOut PSPack(VSOut i, bool front : SV_IsFrontFace) {
    PackSurface s;
    s.N = normalize(i.nrm);
    if (!front) s.N = -s.N;
    s.worldPos = i.worldPos;
    s.V = normalize(gEyePos - i.worldPos);
    s.L = -gLightDir;
    s.uv = i.uv;
    s.uv1 = i.uv1;
    s.uv2 = i.uv2;
    s.pixel = i.pos.xy;
    s.viewZ = i.viewZ;
    s.tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd) : float4(1, 1, 1, 1);
    s.alpha = gDiffuse.a * s.tex.a;
    if (s.alpha < 0.004) discard;
    // pack alpha key (colour-key cut-out): cut pixels whose texture red is below the key, before the pack shading
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0 && s.tex.r < alphaKey) discard;
    s.materialClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    float rawShadow = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, s.N, i.viewZ, i.pos.xy) : 1.0;
    float occ = (1.0 - rawShadow) * gSunShadowParams.z;
    s.shadow = 1.0 - occ;
    PackHeadFrame(s);
    PackResult r = PackShade(s);
    r.color += SrgbToLinear(saturate(gDiffuse.rgb * s.tex.rgb)) * gSunShadowColor.rgb * occ * gSunIntensity;
#if defined(PACK_CAP_SURFACE_OUTPUT) && PACK_CAP_SURFACE_OUTPUT
    s.N = normalize(r.normal);
    r.color += r.emission;
#endif
    // negative reflectivity = "no ambient occlusion here" (composite.hlsl); SSR / RT reflections skip it as well
    // (a pack's own negative value keeps meaning "no AO": it must not reach the PBR tag band, common.hlsli)
    float refl = (r.noAo || r.reflectivity < 0.0) ? -1.0 : (r.alpha > 0.9 ? r.reflectivity : 0.0);
#if defined(SCENE_HAS_ID) && SCENE_HAS_ID
    uint tag = 0;
#if defined(PACK_CAP_SURFACE_OUTPUT) && PACK_CAP_SURFACE_OUTPUT
    tag = r.tag;
#endif
    uint packClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? 7u : PackClassLow(gPackClass);
    uint customId = (tag & 0xFu) | ((packClass & 0x7u) << 4) | (gModelIdInfo & 0xFF80u);
    return PackOutput(r.color, r.alpha, s.N, refl, i.curClip, i.prevClip, customId);
#else
    return PackOutput(r.color, r.alpha, s.N, refl, i.curClip, i.prevClip);
#endif
}
#endif

// ---- inverted-hull edges ------------------------------------------------------------

struct EdgeOut { float4 pos : SV_Position; float4 curClip : TEXCOORD0; float4 prevClip : TEXCOORD1; };

float4 ExpandEdge(float4 clip, float3 wn, float px) {
    float2 dirPx = mul(float4(wn, 0.0), gViewProj).xy * gViewportSize;   // clip-space normal -> pixel space
    float len = length(dirPx);
    dirPx = len > 1e-6 ? dirPx / len : float2(0, 0);
    clip.xy += dirPx * px * 2.0 / gViewportSize * clip.w;
    return clip;
}

EdgeOut VSEdge(VSIn v) {
    EdgeOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    wn = normalize(wn);
    float4 wp = float4(wp3, 1.0);
    float4 pwp = float4(PrevSkinPosition(v), 1.0);
    float px = gEdgeSize * v.edge * gEdgeScale;                           // outline width in pixels
#if PACK_HAS_EDGE
    // pack outlines (PackEdge): per-material width scale; PackEdgeUv2 carries this vertex's uv2
    PackEdgeUv2 = v.uv2;
    uint edgeClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    px *= max(PackEdge(edgeClass, gEdgeColor, gEdgeSize).widthScale, 0.0);
#endif
    o.pos = ExpandEdge(mul(wp, gViewProj), wn, px);
    o.curClip = ExpandEdge(mul(wp, gViewProjNoJitter), wn, px);
    o.prevClip = ExpandEdge(mul(pwp, gPrevViewProjNoJitter), wn, px);
    return o;
}

PSOut PSEdge(EdgeOut i) {
    PSOut o;
    o.color = float4(SrgbToLinear(gEdgeColor.rgb) * gSunIntensity * 0.85, gEdgeColor.a);
    o.normal = float4(OctEncode(float3(0, 0, -1)), 0.0, gEdgeColor.a);
    o.velocity = Velocity(i.curClip, i.prevClip);
    return o;
}

// Depth only (path tracer outlines, ScenePass in PT mode): the edges' depth pre-pass, the alpha test of the ray
// tracer's binary alpha (>= 0.5), as offline_edge.hlsl PSDepth. Colour writes are masked off by the PSO.
// Pack alpha key (pack.json "alphaKey"): a keyed material does not write depth for its cut pixels.
void PSDepthAlpha(VSOut i) {
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv) : float4(1, 1, 1, 1);
    float a = gDiffuse.a * tex.a;
    clip(a - 0.5);
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0) clip(tex.r - alphaKey);
}

// Pack outlines (PACK_HAS_EDGE in the pack's surface.hlsl): PackEdge sets the colour and width scale.
// Compiled only for the pack's edge PSO (MMDX_PACK); otherwise the default PSEdge above draws.
#if PACK_HAS_EDGE
PSOut PSEdgePack(EdgeOut i) {
    uint edgeClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    PackEdgeResult e = PackEdge(edgeClass, gEdgeColor, gEdgeSize);
    PSOut o;
    o.color = float4(SrgbToLinear(saturate(e.color.rgb)) * gSunIntensity * 0.85, saturate(e.color.a));
    o.normal = float4(OctEncode(float3(0, 0, -1)), 0.0, saturate(e.color.a));
    o.velocity = Velocity(i.curClip, i.prevClip);
    return o;
}
#endif

// ---- sky backdrop -----------------------------------------------------------------

struct SkyOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };
SkyOut VSSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

PSOut PSSky(SkyOut i) {
    float4 v = mul(float4(i.ndc, 1.0, 1.0), gInvProj);
    float3 viewDir = normalize(v.xyz / v.w);
    float3 dir = normalize(mul(viewDir, (float3x3)gInvView));
    PSOut o;
    o.color = float4(SkyColor(dir), gTransparentBg > 0.5 ? 0.0 : 1.0);
    o.normal = float4(0, 0, 0, 0);
    float4 cur = mul(float4(dir, 0.0), gViewProjNoJitter);
    float4 prev = mul(float4(dir, 0.0), gPrevViewProjNoJitter);
    o.velocity = Velocity(cur, prev);
    return o;
}

// ---- studio floor (no stage) ---------------------------------------------------------

static const float kFloorExtent = 900.0;

VSOut VSFloor(uint id : SV_VertexID) {
    // two triangles: (0,1,2) (2,1,3) over a square at y = 0
    uint k = id < 3 ? id : (id == 3 ? 2 : (id == 4 ? 1 : 3));
    float2 c = float2((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0) * kFloorExtent;
    float4 wp = float4(c.x, 0.0, c.y, 1.0);
    VSOut o;
    o.pos = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.nrm = float3(0, 1, 0);
    o.uv = c;
    o.curClip = mul(wp, gViewProjNoJitter);
    o.prevClip = mul(wp, gPrevViewProjNoJitter);
    o.viewZ = mul(wp, gView).z;
    o.uv1 = 0;
    o.uv2 = 0;
    return o;
}

PSOut PSFloor(VSOut i) {
    // S10: the mirror pass's clip plane (see PSMain): the floor behind the mirror must not leak into the reflection.
    if (dot(gClipPlane, float4(i.worldPos, 1.0)) < 0.0) discard;
    float3 n = float3(0, 1, 0);
    float3 L = -gLightDir;
    float sh = SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy);
    float r = length(i.worldPos.xz);
    // Unlit (gShading 1): flat albedo, no sun / hemispheric / punctual lighting; the fade into
    // the sky at the edge stays so there is no visible horizon seam.
    if (gShading > 0.5 && gShading < 1.5) {
        float3 unlit = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
        float3 viewDir = normalize(i.worldPos - gEyePos);
        unlit = lerp(unlit, SkyColor(float3(viewDir.x, 0.0, viewDir.z)), smoothstep(260.0, 820.0, r));
        return PackOutput(SrgbToLinear(unlit), 1.0, n, 0.0, i.curClip, i.prevClip);
    }
    // cyclorama: brightest under the performer, easing into the horizon colour
    float3 albedo = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
    float3 sunLin = SrgbToLinear(gLightColor) * 1.65;
    float ndl = saturate(dot(n, L));
    float3 color = albedo * (sunLin * ndl * lerp(0.42, 1.0, sh) + Hemisphere(n) * 0.55) * gSunIntensity;
    color += albedo * PunctualDiffuse(i.worldPos, n, 0.0, 0.0);
    color += albedo * ScreenLightsDiffuse(i.worldPos, n);   // S8b: the stage screens light the studio floor
    float3 V = normalize(gEyePos - i.worldPos);
    color += PunctualSpecular(i.worldPos, n, V, 64.0) * 0.35;
    color += ScreenLightsSpecularBlinn(i.worldPos, n, V, 64.0) * 0.35;
    float reflectivity = 0.42 * (1.0 - smoothstep(120.0, 600.0, r));
    // dissolve into the sky toward the edge so there is no visible horizon seam
    float3 viewDir = normalize(i.worldPos - gEyePos);
    color = lerp(color, SkyColor(float3(viewDir.x, 0.0, viewDir.z)), smoothstep(260.0, 820.0, r));
    return PackOutput(color, 1.0, n, reflectivity, i.curClip, i.prevClip);
}

// ---- wireframe shading mode (ViewShading::Wireframe, raster only) ------------------

// The wireframe PSOs reuse the lit MRT layout (PSOut) and VSM's VSMain so the resolve /
// composite passes run unchanged; the fill mode draws the triangle edges instead. Lines are
// flat so nothing lights them, and alpha-tested/blended materials keep their coverage.

PSOut PSWire(VSOut i) {
    // dark grey on light, light grey on dark backgrounds read equally; HDR linear value < 1
    // so the composite/post passes pass it through without blooming.
    return PackOutput(float3(0.09, 0.10, 0.12), 1.0, float3(0, 1, 0), 0.0, i.curClip, i.prevClip);
}

// Flat background over a wireframe scene: same fullscreen far-plane draw as the sky, one colour.
PSOut PSWireBg(SkyOut i) {
    PSOut o;
    // mid grey (linear); the post pass tonemaps it to a neutral paper-like value
    o.color = float4(0.72, 0.74, 0.76, gTransparentBg > 0.5 ? 0.0 : 1.0);
    o.normal = float4(0, 0, 0, 0);
    o.velocity = float2(0, 0);   // uniform colour: no visible parallax, keep it temporally stable
    return o;
}

PSOut PSWireFloor(VSOut i) {
    return PackOutput(float3(0.16, 0.17, 0.19), 1.0, float3(0, 1, 0), 0.0, i.curClip, i.prevClip);
}

// ---- shadow map ------------------------------------------------------------------
// (left out by includers that use b2 themselves: offline_edge_pack.hlsl)
#ifndef MMDX_NO_SHADOW_PASS

cbuffer ShadowCB : register(b2) {
    uint gCascade;
    uint3 gShadowPad;
    row_major float4x4 gPointViewProj;
};
struct ShadowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

ShadowOut VSShadow(VSIn v) {
    ShadowOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    float4 wp = float4(wp3, 1.0);
    float4x4 m = gCascade < 3 ? gShadowViewProj[gCascade]
               : (gCascade < 19 ? gSpotViewProj[gCascade - 3]
               : gPointViewProj);
    o.pos = mul(wp, m);
    o.uv = v.uv;
    return o;
}

void PSShadowAlpha(ShadowOut i) {
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv) : float4(1, 1, 1, 1);
    float a = gDiffuse.a * tex.a;
    clip(a - 0.5);
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0) clip(tex.r - alphaKey);
}
#endif
