// Ray-traced reflections at half resolution: one mirror ray per pixel, closest
// hit shaded with the MMD material model, sky on miss. Written linear; alpha = blend weight
// (fades with the hit distance as a stand-in for gloss).
// Also the full-resolution, fully opaque mirror pass (CSReflectMirror) for the pixels mmd.hlsl marked
// with reflectivity 2 (planar mirror materials in the RayTraced path).
#include "rt_common.hlsli"   // includes common.hlsli
#define PBR_SAMPLE_LEVEL0   // compute: no screen-space gradients
#include "pbr_common.hlsli"
// S9b-3: the Sky light's image (SkyColor's gSkyParams.x > 1.5 branch); the RT bindless texture table
#define SKY_MAP_TEX(i) gBindlessTex[NonUniformResourceIndex((uint)(i))]
#define SKY_MAP_SAMPLER gRtWrap
#include "sky_map.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);

Texture2D<float> gDepthTex : register(t0);
Texture2D<float4> gNormalTex : register(t1);
RWTexture2D<float4> gReflOut : register(u0);

// The frame's punctual lights: same layout as PtLight in pathtrace.hlsl (copied: that file's shading model is not
// included here). Bound at t2 space1, like gPtLights there.
struct ReflLight {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float pad;
    float areaWidth; float areaHeight; float isArea; float pad2;
};
StructuredBuffer<ReflLight> gReflLights : register(t2, space1);

// Mirror pixels are marked with reflectivity 2 (mmd.hlsl MAT_MIRROR); every other reader clamps to 1.
static const float kMirrorReflectivity = 1.5;
static const uint kMaxMirrorBounces = 3u;

#define REFLECT_LIGHTS gReflLights
#define REFLECT_LIGHT_T ReflLight
#include "reflect_hit.hlsli"

[numthreads(8, 8, 1)]
void CSReflect(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gReflOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    uint2 fp = p * 2;   // full-res source pixel, loaded without filtering
    float d = gDepthTex.Load(int3(fp, 0));
    float4 nt = gNormalTex.Load(int3(fp, 0));
    // nt.z = the reflection tag (common.hlsli); mirror pixels (nt.z > 1.5) belong to CSReflectMirror
    if (d >= 1.0 || ReflTagGate(nt.z) <= 0.01 || nt.z > kMirrorReflectivity) { gReflOut[p] = float4(0, 0, 0, 0); return; }
    // PBR pixels already show the sky (the scene PS's environment specular): a miss adds nothing (alpha 0) and a hit
    // is a full-confidence replacement of that sky (the composite subtracts it); the PS's DFG weight carries gloss.
    const bool pbr = ReflTagIsPbr(nt.z);

    float2 uv = (fp + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d);
    float3 wp = mul(float4(vp, 1.0), gInvView).xyz;
    float3 wn = normalize(mul(OctDecode(nt.xy), (float3x3)gInvView));

    float3 V = normalize(wp - gEyePos);
    float3 R = reflect(V, wn);
    if (dot(R, wn) <= 0) { gReflOut[p] = float4(0, 0, 0, 0); return; }

    // One sharp ray: a jittered (glossy) ray is speckled noise without temporal accumulation.
    // Gloss is approximated instead by blurrier texture lookups and a weaker blend with distance.
    RtHit hit;
    if (!TraceClosest(OffsetRayOrigin(wp, wn), R, 0.0, gP0.z, 0.5, hit)) {
        gReflOut[p] = pbr ? float4(0, 0, 0, 0) : float4(SkyColor(R), 0.6);
        return;
    }

    gReflOut[p] = float4(ReflectHitRadiance(hit, R), pbr ? 1.0 : lerp(0.8, 0.35, saturate(hit.t / 300.0)));
}

// Full-resolution mirror pixels: one sharp ray, fully opaque output, refraction-free mirror bounces. The caller
// multiplies the scene colour (the material's tint) by this: the mirror shows what it reflects, not a blend.
[numthreads(8, 8, 1)]
void CSReflectMirror(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gReflOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    float d = gDepthTex.Load(int3(p, 0));
    float4 nt = gNormalTex.Load(int3(p, 0));
    if (d >= 1.0 || nt.z <= kMirrorReflectivity) { gReflOut[p] = float4(0, 0, 0, 0); return; }

    float2 uv = (p + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d);
    float3 wp = mul(float4(vp, 1.0), gInvView).xyz;
    float3 wn = normalize(mul(OctDecode(nt.xy), (float3x3)gInvView));

    float3 V = normalize(wp - gEyePos);
    float3 R = reflect(V, wn);
    // composite multiplies the tint by this: 1 = the tint alone (a grazing normal, never black)
    if (dot(R, wn) <= 0) { gReflOut[p] = float4(1, 1, 1, 1); return; }

    float3 throughput = 1.0;
    float3 origin = OffsetRayOrigin(wp, wn);
    float3 color;
    RtHit hit;
    uint bounces = 0u;
    [loop] for (;;) {
        if (!TraceClosest(origin, R, 0.0, gP0.z, 0.5, hit)) {
            color = throughput * SkyColor(R);
            break;
        }
        RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
        if ((g.flags & MAT_MIRROR) == 0) {
            color = throughput * ReflectHitRadiance(hit, R);
            break;
        }
        // A mirror hit continues the ray: tint by its linear base colour and reflect about its shading normal.
        RtSurface s = FetchSurface(g, hit.prim, hit.bary);
        if (dot(s.faceNormal, R) > 0) s.faceNormal = -s.faceNormal;
        if (dot(s.normal, s.faceNormal) < 0) s.normal = -s.normal;
        float3 tint;
        if (g.pbrIndex != 0xFFFFFFFFu) {
            GpuPbrMaterial pm = PbrLoadMaterial((int)g.pbrSrv, g.pbrIndex);
            tint = PbrAlbedo(pm, s.uv, g.diffuse.rgb);
        } else {
            tint = SrgbToLinear(g.diffuse.rgb);
        }
        throughput *= tint;
        if (++bounces >= kMaxMirrorBounces) {   // the last mirror shows its own tint only
            color = throughput;
            break;
        }
        origin = OffsetRayOrigin(s.pos, s.faceNormal);
        R = reflect(R, s.normal);
    }
    gReflOut[p] = float4(color, 1.0);
}
