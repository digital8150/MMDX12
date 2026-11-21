#ifndef MMDX_PBR_COMMON_HLSLI
#define MMDX_PBR_COMMON_HLSLI
#include "dielectric.hlsli"   // S2b: FresnelDielectric / DielectricAbsorb
// Per-material PBR data (work unit P2). Mirror of GpuPbrMaterial in src/render/ShaderInterop.h; the values come
// from Ray-MMD material data decoded by asset/PbrMaterial.h. This is the plumbing only: the P3 lighting uses the
// sampling decoders below.
//
// Rule: a material shaded by an active shader pack ignores its PBR record (the pack wins). The raster pack path is
// PSPack, which never reads this file; only the default PSMain debug branch does.
//
// Resource bindings (must already be declared by the includer):
//   raster: Texture2D gPbrTex[] (t0 space2) + ByteAddressBuffer gPbrBuf[] (t0 space3) + SamplerState gPbrWrap (s4)
//   RT:     include rt_common.hlsli first (it declares gBindlessTex / gBindlessBuf / gRtWrap at the same slots).
// The record buffer's SRV uses an absolute SrvHeap index (its raw descriptors live in the same bindless heap the
// path tracer reads).
#ifndef MMDX_RT_COMMON_HLSLI
// The unbounded gPbrTex / gPbrBuf match the texAll / bufAll tables (params 9/10) the scene root signature declares;
// gPbrWrap is the sampler at s4. Under RT_SHADOWS these map onto rt_common.hlsli's names (see the #else).
Texture2D gPbrTex[] : register(t0, space2);
ByteAddressBuffer gPbrBuf[] : register(t0, space3);
SamplerState gPbrWrap : register(s4);
#else
// rt_common.hlsli already declared the bindless tables under their RT names.
#define gPbrTex gBindlessTex
#define gPbrBuf gBindlessBuf
#define gPbrWrap gRtWrap
#endif

// Slot order mirrors PbrSlotId (asset/PbrMaterial.h): Albedo .. CustomB.
#define PBR_SLOT_ALBEDO      0
#define PBR_SLOT_ALBEDO_SUB  1
#define PBR_SLOT_ALPHA       2
#define PBR_SLOT_NORMAL      3
#define PBR_SLOT_NORMAL_SUB  4
#define PBR_SLOT_SMOOTHNESS  5
#define PBR_SLOT_METALNESS   6
#define PBR_SLOT_SPECULAR    7
#define PBR_SLOT_OCCLUSION   8
#define PBR_SLOT_EMISSIVE    9
#define PBR_SLOT_CUSTOM_A   10
#define PBR_SLOT_CUSTOM_B   11
#define PBR_SLOT_COUNT      12

#define PBR_FLAG_VALID    1u
#define PBR_FLAG_CUTOUT   2u
#define PBR_FLAG_EMISSIVE 4u
#define PBR_FLAG_EMISSIVE_HDR 8u   // the emissive is HDR (> 1): do not clamp it to 1 (S2a KHR_materials_emissive_strength)

// Decoded record: one "slot" (constant + texture) per field; the material header carries the shading model and
// alpha information. The field order matches GpuPbrMaterial / GpuPbrSlot.
struct GpuPbrSlot {
    uint type;         // Ray <PREFIX>_MAP_TYPE
    uint swizzle;      // 0 R, 1 G, 2 B, 3 A
    uint uvFlip;       // 0 none, 1 X, 2 Y, 3 both; NORMAL 4 = flip the shading normal X
    uint applyScale;   // 0 none, 1 map * value, 2 map ^ value, 3 (map ^ value) * value.w
    uint colorFlip;    // CUSTOM_A/B: 1 inverts the value
    int textureSrv;    // absolute SrvHeap index of the slot texture (-1 = none)
    float loopU, loopV;
    float4 value;      // xyz = constant value, w = scale (normalMapScale / ...)
    float4 uvScale;    // xy = <PREFIX>_MAP_UV_SCALE, zw = S2a KHR_texture_transform offset
};

struct GpuPbrMaterial {
    uint flags;
    uint shadingModel;      // CUSTOM_ENABLE (0..9)
    float alphaThreshold;
    float diffuseAlpha;
    float4 params;          // x = ALBEDO_SUB_ENABLE blend mode
    GpuPbrSlot slots[PBR_SLOT_COUNT];
    // S2a glTF material extensions. Neutral defaults (transmission 0, clearcoat 0, uv rotation 0) keep a Ray-MMD
    // record's shading identical.
    float4 transmission;    // x factor, y ior, z volume thickness factor, w attenuation distance (0 = inf)
    float4 attenuation;     // xyz volume attenuation colour (linear), w clearcoat factor
    float4 coat;            // x clearcoat roughness, y specular factor, z uv rotation (radians), w sheen roughness
    float4 sheen;           // xyz sheen colour (linear), w = transmission texture SRV as a NUMBER (-1 = none)
};

// S2a: the glTF KHR_texture_transform rotation is one value per material (GpuPbrMaterial::coat.z), because a material
// has no room for one per slot. PbrLoadMaterial sets it here, like PbrSetUvGradients sets the screen-space gradients
// once per pixel before any slot is sampled; every slot of that record then samples with the same rotation.
static float gPbrUvRotation = 0.0;

GpuPbrSlot PbrLoadSlot(ByteAddressBuffer b, uint base) {
    GpuPbrSlot s;
    s.type = b.Load(base + 0);
    s.swizzle = b.Load(base + 4);
    s.uvFlip = b.Load(base + 8);
    s.applyScale = b.Load(base + 12);
    s.colorFlip = b.Load(base + 16);
    s.textureSrv = asint(b.Load(base + 20));
    s.loopU = asfloat(b.Load(base + 24));
    s.loopV = asfloat(b.Load(base + 28));
    s.value = asfloat(b.Load4(base + 32));
    s.uvScale = asfloat(b.Load4(base + 48));
    return s;
}

GpuPbrMaterial PbrLoadMaterial(int srvIndex, uint index) {
    GpuPbrMaterial m;
    ByteAddressBuffer b = gPbrBuf[NonUniformResourceIndex(SafeSrv(srvIndex))];
    const uint base = index * 864u;   // sizeof(GpuPbrMaterial)
    m.flags = b.Load(base + 0);
    m.shadingModel = b.Load(base + 4);
    m.alphaThreshold = asfloat(b.Load(base + 8));
    m.diffuseAlpha = asfloat(b.Load(base + 12));
    m.params = asfloat(b.Load4(base + 16));
    [unroll] for (uint i = 0; i < PBR_SLOT_COUNT; ++i)
        m.slots[i] = PbrLoadSlot(b, base + 32u + i * 64u);
    // S2a extension block, after the slots (mirror of GpuPbrMaterial).
    const uint ext = base + 32u + PBR_SLOT_COUNT * 64u;
    m.transmission = asfloat(b.Load4(ext + 0));
    m.attenuation = asfloat(b.Load4(ext + 16));
    m.coat = asfloat(b.Load4(ext + 32));
    m.sheen = asfloat(b.Load4(ext + 48));
    gPbrUvRotation = m.coat.z;   // 0 for Ray-MMD / stage records: slots sample exactly as before
    return m;
}

// ---- slot decoding (Ray semantics: docs/raymmd_semantics.md) --------------------------------

// UV transform: loop counts, optional UV_SCALE, then the flip; a glTF KHR_texture_transform offset is added last
// (uv = uv * scale + offset). The rotation is one per material (gPbrUvRotation); it sits between scale and flip.
// Ray records have rotation 0 / offset 0, so their arithmetic is exactly the pre-S2a one.
float2 PbrSlotUv(GpuPbrSlot s, float2 uv) {
    uv *= float2(s.loopU, s.loopV);
    uv *= s.uvScale.xy;
    if (gPbrUvRotation != 0.0) {
        float c, sn;
        sincos(gPbrUvRotation, sn, c);
        uv = float2(uv.x * c - uv.y * sn, uv.x * sn + uv.y * c);
    }
    if (s.uvFlip == 1u || s.uvFlip == 3u) uv.x = 1.0 - uv.x;
    if (s.uvFlip == 2u || s.uvFlip == 3u) uv.y = 1.0 - uv.y;
    return uv + s.uvScale.zw;
}

// The derivative of PbrSlotUv for a screen-space uv step (a scaled / rotated map keeps correct mips).
float2 PbrSlotUvDelta(GpuPbrSlot s, float2 d) {
    d *= float2(s.loopU, s.loopV) * s.uvScale.xy;
    if (gPbrUvRotation != 0.0) {
        float c, sn;
        sincos(gPbrUvRotation, sn, c);
        d = float2(d.x * c - d.y * sn, d.x * sn + d.y * c);
    }
    return d;
}

// Sampled texel, or the slot's constant when it has no texture (the load-time fallback).
// Pixel shaders set the screen-space UV gradients once in uniform control flow (PbrSetUvGradients, before any
// record-dependent branch) so slots sample with mips through SampleGrad; compute users (rtreflect) define
// PBR_SAMPLE_LEVEL0 and read mip 0.
#ifndef PBR_SAMPLE_LEVEL0
static float2 gPbrUvDx = 0, gPbrUvDy = 0;
void PbrSetUvGradients(float2 dx, float2 dy) { gPbrUvDx = dx; gPbrUvDy = dy; }
#endif
float4 PbrSlotTexel(GpuPbrSlot s, float2 uv) {
    if (s.textureSrv >= 0) {
#ifdef PBR_SAMPLE_LEVEL0
        return gPbrTex[NonUniformResourceIndex(SafeSrv(s.textureSrv))].SampleLevel(gPbrWrap, PbrSlotUv(s, uv), 0);
#else
        return gPbrTex[NonUniformResourceIndex(SafeSrv(s.textureSrv))].SampleGrad(gPbrWrap, PbrSlotUv(s, uv),
                                                                             PbrSlotUvDelta(s, gPbrUvDx),
                                                                             PbrSlotUvDelta(s, gPbrUvDy));
#endif
    }
    return float4(s.value.xyz, 1.0);
}

float PbrChannel(float4 c, uint swizzle) { return c[swizzle]; }

// roughness from a SMOOTHNESS slot sample per SMOOTHNESS_MAP_TYPE (0 value = smoothness, 1 roughness,
// 2 linear roughness); SmoothnessToRoughness(s) = (1-s)^2. Single-channel slots apply their swizzle; R8 file
// uploads bake the channel into R (swizzle 0 in the record), PMX maps keep the declared channel.
float PbrSlotRoughness(GpuPbrSlot s, float2 uv) {
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    if (s.type == 0u) return pow(max(0.0, 1.0 - v), 2.0);
    if (s.type == 2u) return v * v;   // linear (perceptual) roughness: s = 1 - r, so (1 - s)^2 = r^2 (glTF too)
    return max(0.0, v);               // type 1: s = 1 - sqrt(r), so (1 - s)^2 = r
}

// metalness scalar (METALNESS has no meaningful MAP_TYPE).
float PbrSlotMetalness(GpuPbrSlot s, float2 uv) { return saturate(PbrChannel(PbrSlotTexel(s, uv), s.swizzle)); }

// occlusion scalar; OCCLUSION_MAP_TYPE 1/3 are sRGB-encoded.
float PbrSlotAo(GpuPbrSlot s, float2 uv) {
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    if (s.type == 1u || s.type == 3u) v = SrgbToLinear(v.xxx).x;
    return saturate(v);
}

// Colour for the colour-carrying slots (albedo / sub / emissive / custom B): sRGB source -> linear is left to the
// caller (P3); the debug view shows the raw value.
float3 PbrSlotRgb(GpuPbrSlot s, float2 uv) { return PbrSlotTexel(s, uv).rgb; }

// Tangent-space normal sample per NORMAL_MAP_TYPE (docs/raymmd_semantics.md §1.3), strength folded in.
float3 PbrDecodeNormal(GpuPbrSlot s, float2 uv) {
    float3 c = PbrSlotTexel(s, uv).rgb;
    if (s.type == 4u) return normalize(c * 2.0 - 1.0);           // world-space normal
    float3 n = c * 2.0 - 1.0;
    n.xy *= max(s.value.w, 0.0) > 0.0 ? s.value.w : 1.0;          // normalMapScale
    if (s.type == 1u) {
        n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
    } else if (s.type == 2u || s.type == 3u) {
        n = float3(0, 0, 1);   // grayscale bump: no tangent-space vector (screen-derivative only, P3)
    }
    return normalize(n);
}

// World normal from a tangent-space vector: a screen-derivative tangent frame (Ray-MMD does the same;
// docs/raymmd_semantics.md §5). P is the world position.
float3 PbrTangentToWorld(float3 N, float3 P, float2 uv, float3 nTangent) {
    float3 dp1 = ddx(P), dp2 = ddy(P);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    // bitangent negated as Ray builds the TBN (mat[1] = -normalize(B))
    return normalize(T * nTangent.x * invmax - B * nTangent.y * invmax + N * nTangent.z);
}

// Triangle-UV tangent frame for the ray path: p0/p1/p2 are the hit triangle's world positions, uv0..2 its UVs.
// PMX has no vertex tangents, so the frame comes from the triangle (the RT counterpart of the raster derivative
// frame above).
float3 PbrTangentToWorldTri(float3 N, float3 p0, float3 p1, float3 p2, float2 uv0, float2 uv1, float2 uv2,
                            float3 nTangent) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    float det = d1.x * d2.y - d1.y * d2.x;
    if (abs(det) < 1e-12) return N;
    float r = 1.0 / det;
    float3 T = (e1 * d2.y - e2 * d1.y) * r;
    float3 B = (e2 * d1.x - e1 * d2.x) * r;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return normalize(T * nTangent.x * invmax - B * nTangent.y * invmax + N * nTangent.z);
}

// Sun F0 per docs/raymmd_semantics.md §4 (SPECULAR_MAP_TYPE 0/1/2/3). Types 2/3 are single-channel; R8 file
// uploads bake the swizzle channel into R (swizzle 0), PMX maps use the declared channel.
float3 PbrSlotF0(GpuPbrSlot s, float2 uv) {
    float4 c = PbrSlotTexel(s, uv);
    float3 rgb = c.rgb;
    if (s.type >= 2u) rgb = PbrChannel(c, s.swizzle).xxx;
    if (s.type == 0u || s.type == 2u) rgb *= 0.08;
    else rgb *= 0.16 * rgb;
    return clamp(rgb, 0.01, 1.0);
}

// Distinct hue per shading model (PBR debug "model").
float3 PbrModelHue(uint model) {
    float h = frac((float)model * 0.61803398875);
    float3 k = frac(float3(1.0, 2.0 / 3.0, 1.0 / 3.0) + h);
    return saturate(abs(k * 6.0 - 3.0) - 1.0);
}

// ---- P3 surface decode (Ray semantics: docs/raymmd_semantics.md §1) -------------------------
// Scalar slots (ALPHA / SMOOTHNESS / METALNESS / OCCLUSION / CUSTOM_A) select their channel with the record's
// swizzle. A File map for such a slot is uploaded R8 with that channel baked into R and the record's swizzle set
// to 0 (GpuModel), while a PMX-sourced map is RGBA and keeps the declared swizzle. A slot without a texture falls
// back to its constant (value.x for a scalar slot).

// <slot>_MAP_APPLY_SCALE for a scalar slot (1 = value * const, 2 = value ^ const).
float PbrApplyScaleS(float v, GpuPbrSlot s) {
    if (s.applyScale == 1u) return v * s.value.x;
    if (s.applyScale == 2u) return pow(max(v, 0.0), s.value.x);
    if (s.applyScale == 3u) return pow(max(v, 0.0), s.value.x) * s.value.w;
    return v;
}

// <slot>_MAP_APPLY_SCALE for a colour slot.
float3 PbrApplyScaleRgb(float3 c, GpuPbrSlot s) {
    if (s.applyScale == 1u) return c * s.value.xyz;
    if (s.applyScale == 2u) return pow(max(c, 0.0), s.value.xyz);
    // 3: an exponent map scaled by a stage strength-only emissive rule (#30), which keeps the record's exponent
    if (s.applyScale == 3u) return pow(max(c, 0.0), s.value.xyz) * s.value.w;
    return c;
}

// ALBEDO (+ ALBEDO_SUB blend, APPLY_DIFFUSE, APPLY_SCALE), linear. `pmxDiffuse` is the PMX material diffuse
// (gamma, as gDiffuse.rgb). Ray order: sample -> APPLY_DIFFUSE (gamma multiply) -> srgb2linear -> APPLY_SCALE ->
// ALBEDO_SUB blend (linear) -> saturate.
float3 PbrAlbedo(GpuPbrMaterial m, float2 uv, float3 pmxDiffuse) {
    GpuPbrSlot sa = m.slots[PBR_SLOT_ALBEDO];
    float3 a = PbrSlotRgb(sa, uv);
    if (m.params.y > 0.5) a *= pmxDiffuse;                 // ALBEDO_MAP_APPLY_DIFFUSE
    a = SrgbToLinear(saturate(a));
    a = PbrApplyScaleRgb(a, sa);

    const int sub = (int)m.params.x;                        // ALBEDO_SUB_ENABLE
    if (sub > 0) {
        GpuPbrSlot ss = m.slots[PBR_SLOT_ALBEDO_SUB];
        float4 s4 = PbrSlotTexel(ss, uv);
        float3 s = PbrApplyScaleRgb(SrgbToLinear(saturate(s4.rgb)), ss);
        if (sub == 1) a = a * s;
        else if (sub == 2) a = pow(max(a, 0.0), max(s, 1e-4));
        else if (sub == 3) a = a + s;
        else if (sub == 4) a = a * lerp(1.0, a * Luminance(a), s);   // ColorSynthesis
        else if (sub == 5) a = lerp(a, s, s4.a);                     // alpha blend
    }
    return saturate(a);
}

// ALPHA slot (Ray GetAlpha): the sampled/constant channel selected by swizzle, times the PMX diffuse alpha (the
// GPU record does not carry FROM; the library is ALPHA_MAP_FROM 3 almost everywhere, where Ray multiplies by
// MaterialDiffuse.a). R8 file uploads bake the channel into R.
float PbrAlpha(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_ALPHA];
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle) * m.diffuseAlpha;
    return saturate(PbrApplyScaleS(v, s));
}

// EMISSIVE colour, linear, with APPLY_SCALE (the morph/blink controls are out of scope in P3). The glTF importer folds
// KHR_materials_emissive_strength into the slot's value and sets PBR_FLAG_EMISSIVE_HDR when the result exceeds 1: such
// a record is not clamped (an LED panel with strength 20 glows and blooms), and a record without a map reads its value
// as a linear HDR colour. Every other record keeps the sRGB constant decode and the old saturate, so Ray-MMD output is
// unchanged.
float3 PbrEmissive(GpuPbrMaterial m, float2 uv) {
    if ((m.flags & PBR_FLAG_EMISSIVE) == 0u) return 0;   // EMISSIVE_ENABLE 0: the slot's value is not light
    GpuPbrSlot s = m.slots[PBR_SLOT_EMISSIVE];
    const bool hdr = (m.flags & PBR_FLAG_EMISSIVE_HDR) != 0u;
    if (hdr && s.textureSrv < 0) return max(s.value.xyz, 0.0);
    float3 e = SrgbToLinear(saturate(PbrSlotRgb(s, uv)));
    e = PbrApplyScaleRgb(e, s);
    return hdr ? e : saturate(e);
}

// ---- S2a glTF material extensions ---------------------------------------------------------------
// KHR_materials_transmission factor (0 = ordinary surface). The transmission map's SRV rides in the record
// (sheen.w as a number, -1 = none); PbrTransmissionAt samples it where a UV is available.
float PbrTransmission(GpuPbrMaterial m) { return saturate(m.transmission.x); }

// The material's transmission at `uv`: the factor times the transmission map's red channel (KHR_materials_transmission
// reads R; a single-channel R8 upload bakes it into R). The map has no per-slot transform in the record, so the
// material's own UV is used.
float PbrTransmissionAt(GpuPbrMaterial m, float2 uv) {
    const int srv = (int)m.sheen.w;
    if (srv < 0) return PbrTransmission(m);
    return saturate(PbrTransmission(m) * gPbrTex[NonUniformResourceIndex(SafeSrv(srv))].SampleLevel(gPbrWrap, uv, 0).r);
}

// KHR_materials_volume thickness factor (the record keeps the factor; the thickness map is not needed before S2b).
float PbrThickness(GpuPbrMaterial m) { return max(m.transmission.z, 0.0); }

// KHR_materials_clearcoat: the coat factor and its roughness (0..1 linear roughness like glTF).
float PbrClearcoat(GpuPbrMaterial m) { return saturate(m.attenuation.w); }
float PbrClearcoatRoughness(GpuPbrMaterial m) { return saturate(m.coat.x); }

// CUSTOM_A scalar (curvature / sheen / threshold ...) with COLOR_FLIP.
float PbrCustomA(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_CUSTOM_A];
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    v = PbrApplyScaleS(v, s);
    if (s.colorFlip == 1u) v = 1.0 - v;
    return saturate(v);
}

// CUSTOM_B colour (transmittance / fuzz / shadow colour) with COLOR_FLIP (the local library uses
// CUSTOM_B_MAP_TYPE 0 = sRGB decode, handled here).
float3 PbrCustomB(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_CUSTOM_B];
    float3 c = SrgbToLinear(saturate(PbrSlotRgb(s, uv)));
    if (s.colorFlip == 1u) c = 1.0 - c;
    return saturate(c);
}

// Screen-derivative tangent frame (Ray-MMD does the same; docs/raymmd_semantics.md §5). P is the world position.
void PbrTangentFrame(float3 N, float3 P, float2 uv, out float3 T, out float3 B) {
    float3 dp1 = ddx(P), dp2 = ddy(P);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    T = dp2perp * duv1.x + dp1perp * duv2.x;
    B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    T *= invmax;
    B *= invmax;
}

// Reoriented normal mapping (RNM) blend of a base and a sub tangent-space normal (Ray RNMBlendUnpacked).
float3 PbrBlendRnm(float3 n1, float3 n2) {
    n1.z += 1.0;
    n2.xy = -n2.xy;
    return normalize(n1 * dot(n1, n2) / max(n1.z, 1e-5) - n2);
}

// World-space mapped normal: NORMAL + NORMAL_SUB (RNM) through the derivative tangent frame. Returns baseN when
// the material has no normal map. NORMAL_MAP_UV_FLIP 4 flips the shading normal X.
float3 PbrWorldNormal(GpuPbrMaterial m, float3 baseN, float3 P, float2 uv) {
    if (m.slots[PBR_SLOT_NORMAL].textureSrv < 0) return baseN;
    float3 nt = PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL], uv);
    if (m.slots[PBR_SLOT_NORMAL_SUB].textureSrv >= 0)
        nt = PbrBlendRnm(nt, PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL_SUB], uv));
    float3 N = PbrTangentToWorld(baseN, P, uv, nt);
    if (m.slots[PBR_SLOT_NORMAL].uvFlip == 4u) N.x = -N.x;
    return normalize(N);
}

// ---- GGX lobe (docs/raymmd_semantics.md §4) -------------------------------------------------
// GGX + the doc's Smith pair + Schlick Fresnel; the geometry already carries N.L / N.V.
float3 PbrSpecularGGX(float3 N, float3 V, float3 L, float rough, float3 f0) {
    float3 H = normalize(L + V);
    float nh = saturate(dot(N, H));
    float nl = saturate(dot(N, L));
    float nv = abs(dot(N, V)) + 1e-5;
    float vh = saturate(dot(V, H));
    float m2 = rough * rough;
    float d = m2 / pow((nh * m2 - nh) * nh + 1.0, 2.0);
    float gv = nl * (nv * (1.0 - rough) + rough);
    float gl = nv * (nl * (1.0 - rough) + rough);
    d *= 0.5 / max(gv + gl, 1e-5);
    if (nl <= 0.0) d = 0.0;   // no light from behind the surface
    float f90 = saturate(dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)) * 50.0);
    float3 f = lerp(f0, f90, pow(1.0 - vh, 5.0));
    return f * d;
}

// Anisotropic GGX (docs/raymmd_semantics.md §2 ANISOTROPY): an elliptical lobe along `T`. `aniso` = customA
// strength, `shift` = customB.r tangent shift.
float3 PbrSpecularGGXAniso(float3 N, float3 V, float3 L, float rough, float3 f0, float aniso,
                           float3 T, float shift) {
    float3 X = normalize(T + N * shift);
    float3 Y = cross(N, X);
    float3 H = normalize(L + V);
    float aspect = rsqrt(max(1.0 - aniso * 0.9, 1e-4));
    float ax = max(rough / max(aspect, 1e-4), 1e-3);
    float ay = max(rough * aspect, 1e-3);
    float tx = dot(X, H) / ax;
    float ty = dot(Y, H) / ay;
    float hn = dot(N, H);
    float d = 1.0 / (3.14159265 * ax * ay * pow(tx * tx + ty * ty + hn * hn, 2.0));
    float nv = abs(dot(N, V)) + 1e-5;
    float nl = saturate(dot(N, L));
    float vh = saturate(dot(V, H));
    float f90 = saturate(dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)) * 50.0);
    float3 f = lerp(f0, f90, pow(1.0 - vh, 5.0));
    return f * (d * 0.25 / max(nv, 1e-5));
}

// Sheen lobe (docs/raymmd_semantics.md §2 CLOTH): `(2 + 1/rough) * sin^2(nh)^(0.5/rough) / 2 / G`.
float3 PbrSpecularSheen(float3 N, float3 V, float3 L, float rough, float3 fuzz) {
    float3 H = normalize(L + V);
    float nh = saturate(dot(N, H));
    float nl = saturate(dot(N, L));
    float nv = abs(dot(N, V)) + 1e-5;
    float s2 = (1.0 - nh) * (1.0 + nh);
    float g = max(4.0 * (nl + nv - nl * nv), 1e-4);
    float s = (2.0 + 1.0 / max(rough, 1e-3)) * pow(max(s2, 1e-6), 0.5 / max(rough, 1e-3)) / (2.0 * g);
    float vh = saturate(dot(V, H));
    return fuzz * (s * pow(1.0 - vh, 5.0));
}

#ifdef MMDX_RT_COMMON_HLSLI
// S2b: tinted shadow visibility. A shadow ray that crosses transmissive mesh geometry (RtGeom_Transmissive) is not
// stopped there: the visibility is multiplied by the interface's transmittance instead, so a glass block casts a
// coloured shadow that darkens toward its rim. Everything else -- opaque hits, alpha-tested geometry -- ends the ray
// exactly like TraceShadowRayMasked, so a scene without transmissive materials returns the same 0 / 1.
// An entry (front-face) crossing carries transmission * baseColour * (1 - Fresnel) plus the volume's Beer-Lambert
// absorption over its thickness factor; an exit (back-face) crossing carries only its own (1 - Fresnel), so the
// surface tint is applied once per body (glTF: the base colour tints the light entering the material) and a closed
// solid loses (1 - F)^2 while a single-sheet pane loses its light once. No caustics: attenuated, never bent.
float3 TransmissiveShadowTint(RtGeometry g, uint prim, float2 bary, float3 dir, bool frontFace) {
    GpuPbrMaterial m = PbrLoadMaterial((int)g.pbrSrv, g.pbrIndex);
    const float2 uv = LoadUv(g, LoadTriangle(g, prim), bary);
    const float transmission = PbrTransmissionAt(m, uv);
    if (transmission <= 0.0) return 0.0;   // no transmission: it blocks like an ordinary surface
    float3 faceN = FetchSurface(g, prim, bary).faceNormal;
    float3 n = frontFace ? faceN : -faceN;             // face the incoming ray
    const float eta = frontFace ? max(m.transmission.y, 1.0) : 1.0 / max(m.transmission.y, 1.0);
    const float fres = FresnelDielectric(saturate(dot(-dir, n)), eta);
    if (!frontFace) return float3(1.0 - fres, 1.0 - fres, 1.0 - fres);   // exit: only the interface loss
    const float3 baseColor = SrgbToLinear(saturate(PbrAlbedo(m, uv, g.diffuse.rgb)));   // tint, once (glTF)
    float3 absorb = 1;
    if (m.transmission.z > 0.0)   // KHR_materials_volume over the material's thickness factor (order-independent)
        absorb = DielectricAbsorb(saturate(m.attenuation.xyz), max(m.transmission.w, 0.0), max(m.transmission.z, 0.0));
    return transmission * baseColor * (1.0 - fres) * absorb;
}

// One traversal, no restart: a non-transmissive candidate is committed as a blocker (alpha-tested exactly like
// TraceShadowRayMasked), a transmissive one is NOT committed but folds its per-crossing tint into `vis`. The query's
// own opaque handling finds the nearest opaque triangle, so a blocker behind glass still shadows: traversal order
// never decides the result.
// Volume absorption is an order-independent approximation: the material's thickness factor is absorbed on its
// front-face (entry) crossing, where the exiting chord is not known yet in a single traversal, and the exit crossing
// carries only its interface loss. The direction of a shadow ray is fixed, so the energy is right; the per-chord
// detail is not (no caustics, by design).
float3 TraceShadowRayTinted(float3 origin, float3 dir, float tMax, uint mask) {
    // Same flags as TraceShadowRayMasked: the search ends at the first committed (blocking) hit, which is what makes a
    // scene without transmissive materials return the identical 0 / 1. Tinted crossings are only accumulated up to
    // that point, and a crossing behind a blocker cannot matter (the result is 0).
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, mask, r);
    float3 vis = 1;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;   // opaque: the query commits the nearest
        RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
        if ((g.flags & RTG_CAST_SHADOW) == 0) continue;
        if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) < 0.5) continue;
        if ((g.flags & RTG_TRANSMISSIVE) != 0 && g.pbrIndex != 0xFFFFFFFFu) {
            // a dielectric crossing: tint the light instead of stopping it, and keep looking for a blocker behind it
            vis *= TransmissiveShadowTint(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), dir,
                                          q.CandidateTriangleFrontFace());
            if (dot(vis, float3(1, 1, 1)) <= 1e-4) return 0.0;   // fully absorbed: nothing to find behind it
            continue;   // do not commit: the traversal continues, a nearer opaque hit still ends the ray
        }
        q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : vis;
}

#endif // MMDX_RT_COMMON_HLSLI (tinted shadow rays exist in the ray paths only)

// S10: the planar reflection of a mirror pixel, sampled through the frame's bindless texture table (the reflection
// textures live in the shared SrvHeap, exactly like a PBR map: SceneConstants::mirrorParams carries their indices).
// `worldPos` lies on the mirror's plane, so projecting it through the plane's reflected camera gives exactly the
// reflected ray through the pixel; the sample is tinted by the material's base colour and a roughness > 0 blurs it
// (3x3 taps, spread with the roughness). `srv` < 0 (no reflection this frame: quad / ortho views, camera feeds,
// RenderToImage, RT / PT / GI) or `slot` < 0 -> the tint alone. `slot` picks the mirror the pixel lies on.
float3 MirrorReflection(int srv, int slot, float3 worldPos, float3 tint, float roughness) {
    if (srv < 0 || slot < 0 || slot > 1) return tint;
    const float4 clip = mul(float4(worldPos, 1.0), gMirrorViewProj[slot]);
    if (clip.w <= 0.0) return tint;
    const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return tint;   // the reflected ray leaves the reflection view
    const uint s = NonUniformResourceIndex(SafeSrv(srv));
    if (roughness <= 0.02) return tint * gPbrTex[s].SampleLevel(gPbrWrap, uv, 0).rgb;
    // Rough mirror: 3x3 taps, spread with the roughness (a "blurrier" reflection at half the render resolution).
    uint tw = 0, th = 0;
    gPbrTex[s].GetDimensions(tw, th);
    const float2 texel = 1.0 / max(float2((float)tw, (float)th), 1.0);
    const float spread = max(roughness, 0.02) * 3.0;
    float3 sum = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += gPbrTex[s].SampleLevel(gPbrWrap, saturate(uv + float2(x, y) * texel * spread), 0).rgb;
    return tint * (sum / 9.0);
}

#endif // MMDX_PBR_COMMON_HLSLI
