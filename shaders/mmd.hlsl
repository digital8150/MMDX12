// Scene pass: MMD toon materials with modern lighting (cascaded shadows through the toon
// ramp, punctual lights, hemispheric fill, rim), plus the sky backdrop and studio floor.
// Writes MRT: 0 = linear HDR colour (alpha = coverage), 1 = oct view normal / reflectivity /
// coverage, 2 = velocity (uv current - uv previous).
#include "common.hlsli"
#include "skinning.hlsli"
#ifdef RT_SHADOWS
#include "rt_common.hlsli"
#endif

cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; float gReflectivity;
    uint gPackClass; uint gPackHeadBone;   // shader packs only (pack_api.hlsli); the default shading ignores them
    // material morph factors (ApplyTexFactor): texture, sphere, toon
    float4 gTexMul; float4 gTexAdd; float4 gSphereMul; float4 gSphereAdd; float4 gToonMul; float4 gToonAdd;
    float4 gPackHead;        // xyz = head bone bind position (model space), w = 1 when the model has a head bone
    float4 gPackParams[4];   // the pack's 16 parameters (pack.json order)
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
#endif

// FXC ignores pack_matrix for structured-buffer elements: state row_major explicitly.
struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
Texture2D gTexture : register(t1);
Texture2D gSphere  : register(t2);
Texture2D gToon    : register(t3);
StructuredBuffer<BoneMatrix> gPrevBones : register(t4);
Texture2DArray<float> gShadowMap : register(t5);
struct Light { float3 pos; float invRange; float3 color; float cosOuter; float3 dir; float cosInner; float4 pad; };
StructuredBuffer<Light> gLights : register(t6);   // pad.x = spot shadow slice (-1 = none)
Texture2DArray<float> gSpotShadowMap : register(t7);
SamplerState gWrap  : register(s0);
SamplerState gClamp : register(s1);
SamplerComparisonState gShadowCmp : register(s2);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2; float3 prevMorph : TEXCOORD3;
    float3 sdefC : TEXCOORD4; float3 sdefR0 : TEXCOORD5; float3 sdefR1 : TEXCOORD6; float sdef : TEXCOORD7;
};
struct VSOut {
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD1;
    float4 curClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    float viewZ : TEXCOORD4;
};
struct PSOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float2 velocity : SV_Target2;
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
    o.pos = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.nrm = wn;
    o.uv = v.uv;
    o.curClip = mul(wp, gViewProjNoJitter);
    o.prevClip = mul(pwp, gPrevViewProjNoJitter);
    o.viewZ = mul(wp, gView).z;
    return o;
}

// ---- shadows ------------------------------------------------------------------

static const float2 kPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)
};

float ShadowCascade(float3 wp, float3 n, int c, float2 pixel) {
    float3 L = -gLightDir;
    float ndl = saturate(dot(n, L));
    float offset = gCascadeTexel[c] * gShadowParams.y * (1.5 - ndl);
    float4 sp = mul(float4(wp + n * offset, 1.0), gShadowViewProj[c]);
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    float a = Ign(pixel) * 6.2831853;
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
// Two cone-jittered rays toward the sun (soft penumbra), honouring alpha-tested casters.
float ShadowRt(float3 wp, float3 n, float viewZ, float2 pixel) {
    if (gCascadeSplits.w < 0.5) return 1.0;
    float3 L = -gLightDir;
    float3 origin = wp + n * (0.02 + viewZ * 0.0004) + L * 0.01;
    uint rng = RngSeed((uint2)pixel, (uint)gFrameIndex, 7u);
    float vis = 0;
    [unroll] for (int k = 0; k < 2; ++k)
        vis += TraceShadowRay(origin, SampleCone(float2(Rand(rng), Rand(rng)), L, 0.99993), 2000.0);
    return vis * 0.5;
}
#define SHADOW_TERM(wp, n, viewZ, pixel) ShadowRt(wp, n, viewZ, pixel)
#else
#define SHADOW_TERM(wp, n, viewZ, pixel) Shadow(wp, n, viewZ, pixel)
#endif

// ---- punctual lights ------------------------------------------------------------

// Spot shadow map (perspective slice per spot): normal offset scaled with the texel footprint,
// 2x2 hardware PCF taps.
float SpotShadow(Light l, float3 wp, float3 n, float dist) {
    float slice = l.pad.x;
    if (slice < 0.0 || slice >= gSpotShadowParams.x) return 1.0;
    float c = max(l.cosOuter, 0.05);
    float texel = dist * 2.0 * sqrt(1.0 - c * c) / c * gSpotShadowParams.y;
    float3 ld = (l.pos - wp) / max(dist, 1e-4);
    float offset = texel * (2.0 - saturate(dot(n, ld)));
    float4 sp = mul(float4(wp + n * offset, 1.0), gSpotViewProj[(uint)slice]);
    if (sp.w <= 0.0) return 1.0;
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    float r = 0.75 * gSpotShadowParams.y;
    float sum = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 o = float2((k & 1) ? r : -r, (k & 2) ? r : -r);
        sum += gSpotShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, slice), p.z - 0.00002);
    }
    return sum * 0.25;
}

float3 PunctualDiffuse(float3 wp, float3 n, float toonSoft, float flat) {
    float3 sum = 0;
    uint count = (uint)gNumLights;
    for (uint i = 0; i < count; ++i) {
        Light l = gLights[i];
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float x = saturate(1.0 - pow(dist * l.invRange, 4.0));
        float atten = x * x / (1.0 + dist * dist * 0.0004);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        if (atten > 0.0) atten *= SpotShadow(l, wp, n, dist);
        float ndl = dot(n, ld);
        float diff = lerp(saturate(ndl), smoothstep(-0.05, 0.25, ndl), toonSoft);
        diff = lerp(diff, saturate(ndl * 0.3 + 0.7), flat);
        sum += l.color * atten * diff;
    }
    return sum;
}

float3 PunctualSpecular(float3 wp, float3 n, float3 V, float power) {
    float3 sum = 0;
    uint count = (uint)gNumLights;
    for (uint i = 0; i < count; ++i) {
        Light l = gLights[i];
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float x = saturate(1.0 - pow(dist * l.invRange, 4.0));
        float atten = x * x / (1.0 + dist * dist * 0.0004);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        if (atten > 0.0) atten *= SpotShadow(l, wp, n, dist);
        float3 h = normalize(ld + V);
        sum += l.color * atten * pow(saturate(dot(n, h)), power) * saturate(dot(n, ld));
    }
    return sum;
}

float3 Hemisphere(float3 n) {
    return lerp(gGroundColor, gSkyZenith, saturate(n.y * 0.5 + 0.5)) * gSunIntensity;
}

PSOut PackOutput(float3 color, float alpha, float3 worldNormal, float reflectivity, float4 curClip, float4 prevClip) {
    PSOut o;
    o.color = float4(color, alpha);
    float3 vn = normalize(mul(worldNormal, (float3x3)gView));
    o.normal = float4(OctEncode(vn), reflectivity, alpha);
    o.velocity = Velocity(curClip, prevClip);
    return o;
}

// ---- MMD material -----------------------------------------------------------------

PSOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    float3 L = -gLightDir;
    float3 V = normalize(gEyePos - i.worldPos);

    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd)
                                            : float4(1, 1, 1, 1);
    float alpha = gDiffuse.a * tex.a;
    if (alpha < 0.004) discard;

    // Unlit (gShading 1): texture * material diffuse colour only, still alpha-tested/blended.
    // The lit path below is untouched when gShading == 0.
    if (gShading > 0.5 && gShading < 1.5)
        return PackOutput(SrgbToLinear(saturate(gDiffuse.rgb * tex.rgb)), alpha, n, gReflectivity,
                          i.curClip, i.prevClip);

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

    float sh = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy) : 1.0;
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
        c = lerp(lit * 0.62, lit, sh);
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

    float reflectivity = gReflectivity;
    if (gFlags & MAT_STAGE) {
        float floorMask = smoothstep(0.82, 0.97, n.y);
        reflectivity = saturate(reflectivity + gFloorGloss * floorMask);
        color += PunctualSpecular(i.worldPos, n, V, 48.0) * reflectivity;
    } else {
        // anime rim light: grazing angles on the side the key light comes from
        float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
        float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    }
    return PackOutput(color, alpha, n, alpha > 0.9 ? reflectivity : 0.0, i.curClip, i.prevClip);
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
    s.pixel = i.pos.xy;
    s.viewZ = i.viewZ;
    s.tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd) : float4(1, 1, 1, 1);
    s.alpha = gDiffuse.a * s.tex.a;
    if (s.alpha < 0.004) discard;
    s.materialClass = gPackClass;
    s.shadow = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, s.N, i.viewZ, i.pos.xy) : 1.0;
    PackHeadFrame(s);
    PackResult r = PackShade(s);
    // negative reflectivity = "no ambient occlusion here" (composite.hlsl); SSR / RT reflections skip it as well
    float refl = r.noAo ? -1.0 : (r.alpha > 0.9 ? r.reflectivity : 0.0);
    return PackOutput(r.color, r.alpha, s.N, refl, i.curClip, i.prevClip);
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
    // pack outlines (PackEdge): per-material width scale
    px *= max(PackEdge(gPackClass, gEdgeColor, gEdgeSize).widthScale, 0.0);
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

// Pack outlines (PACK_HAS_EDGE in the pack's surface.hlsl): PackEdge sets the colour and width scale.
// Compiled only for the pack's edge PSO (MMDX_PACK); otherwise the default PSEdge above draws.
#if PACK_HAS_EDGE
PSOut PSEdgePack(EdgeOut i) {
    PackEdgeResult e = PackEdge(gPackClass, gEdgeColor, gEdgeSize);
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
    return o;
}

PSOut PSFloor(VSOut i) {
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
    float3 V = normalize(gEyePos - i.worldPos);
    color += PunctualSpecular(i.worldPos, n, V, 64.0) * 0.35;
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

cbuffer ShadowCB : register(b2) { uint gCascade; };
struct ShadowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

ShadowOut VSShadow(VSIn v) {
    ShadowOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    float4 wp = float4(wp3, 1.0);
    o.pos = mul(wp, gCascade < 3 ? gShadowViewProj[gCascade] : gSpotViewProj[gCascade - 3]);
    o.uv = v.uv;
    return o;
}

void PSShadowAlpha(ShadowOut i) {
    float a = gDiffuse.a * ((gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv).a : 1.0);
    clip(a - 0.5);
}
