// Half-res volumetric light (volumetric_common.hlsli): sun shafts through the cascaded shadow map,
// spot light beams through the spot shadow maps, height-fog transmittance. Then temporal
// accumulation (the march is jittered per frame) and a light depth-aware separable blur.
//   CSMarch:    t0 depth (full res), t1 sun cascades, t2 spot shadow maps, u0 out (half res: L, T)
//     gP0 = (sigma0, heightFalloff, maxDistance, sunG), gP1 = (1/outW, 1/outH, spotBoost, punctualG)
//     gP2 = (ambient scale, pointBoost, 0, 0)
//   CSTemporal: t0 current march, t1 history (previous frame), t2 depth, u0 out history
//     gP0 = (history valid 0/1, current weight, maxDistance, 0)
//   CSBlur:     t0 src (half res), t1 depth (full res), u0 out; gP0.xy = integer pixel direction.
#include "common.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

Texture2D<float> gDepthTex : register(t0);
Texture2DArray<float> gShadowMap : register(t1);
Texture2DArray<float> gSpotShadowMap : register(t2);
Texture2D<float4> gSrc : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float> gDepthBlur : register(t1);   // CSBlur
Texture2D<float> gDepthTemporal : register(t2);
RWTexture2D<float4> gOut : register(u0);

// Shadow-map visibility: four point compares of the 2x2 footprint (smooth shaft edges).
float CompareGather(Texture2DArray<float> map, float2 uv, float slice, float z) {
    float4 d = map.GatherRed(gPoint, float3(uv, slice));
    float4 lit = z <= d ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    return dot(lit, float4(0.25, 0.25, 0.25, 0.25));
}

float VolSunVisibility(float3 wp) {
    if (gSunShadowParams.x < 0.5) return 1.0;
    float viewZ = mul(float4(wp, 1.0), gView).z;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    float4 sp = mul(float4(wp, 1.0), gShadowViewProj[c]);
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gShadowMap, suv, c, q.z - 0.0015);
}

#include "volumetric_common.hlsli"

float VolSpotVisibility(VolLight l, float3 wp) {
    if (l.shadowType == 0.0) return 1.0;
    float slice = l.shadowSlice;
    if (slice < 0.0 || slice >= gSpotShadowParams.x) return 1.0;
    float4 sp = mul(float4(wp, 1.0), gSpotViewProj[(uint)slice]);
    if (sp.w <= 0.0) return 1.0;
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gSpotShadowMap, suv, slice, q.z - 0.00005);
}

// The view ray of half-res pixel p (centre of its 2x2 full-res block) and its march length.
void MarchRay(uint2 p, Texture2D<float> depth, float maxDist, out float3 dir, out float dist, out float3 dirV) {
    uint2 fp = p * 2;
    float d = depth.Load(int3(fp, 0));
    float2 uv = (fp + 1.0) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d >= 1.0 ? 1.0 : d);
    dirV = normalize(vp);
    dist = d >= 1.0 ? maxDist : min(length(vp), maxDist);
    dir = normalize(mul(dirV, (float3x3)gInvView));
}

[numthreads(8, 8, 1)]
void CSMarch(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    float3 dir, dirV;
    float dist;
    MarchRay(p, gDepthTex, gP0.z, dir, dist, dirV);

    VolParams vp;
    vp.sigma0 = gP0.x;
    vp.falloff = gP0.y;
    vp.sunG = gP0.w;
    vp.punctualG = gP1.w;
    vp.spotBoost = gP1.z;
    vp.pointBoost = gP2.y;
    // sun shadows exist up to the far end of the last cascade (view z)
    vp.sunShadowDist = gCascadeSplits.w > 0.5 ? gCascadeSplits.z / max(dirV.z, 1e-3) : 0.0;
    vp.sunSteps = 32;
    vp.spotSteps = 16;
    vp.pointSteps = 4;
    vp.sunStepLen = 2.5;
    vp.spotStepLen = 2.0;
    vp.jitter = frac(Ign(float2(p) + 5.588238 * (float)((uint)gFrameIndex & 63u)));
    vp.ambient = (gSkyZenith + gSkyHorizon + gGroundColor) * (1.0 / 3.0) * gSunIntensity * gP2.x;
    vp.mediumCenter = float3(0.0, 0.0, 0.0);
    vp.mediumRadius = 180.0;

    // sun colour relative to the default 0.6 grey MMD light
    float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    gOut[p] = IntegrateVolume(gEyePos, dir, dist, (uint)gNumLights, gLightDir, sunCol, vp);
}

[numthreads(8, 8, 1)]
void CSTemporal(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = int2(id.xy);
    float4 cur = gSrc.Load(int3(p, 0));
    if (gP0.x < 0.5) {
        gOut[p] = cur;
        return;
    }
    // neighbourhood range of the (jittered, noisy) current march
    float4 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float4 s = gSrc.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(w, h) - 1), 0));
        mn = min(mn, s);
        mx = max(mx, s);
    }
    // reproject the march end point (surface, or the far end for the sky)
    float3 dir, dirV;
    float dist;
    MarchRay(uint2(p), gDepthTemporal, gP0.z, dir, dist, dirV);
    float4 clip = mul(float4(gEyePos + dir * dist, 1.0), gPrevViewProjNoJitter);
    float2 puv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    if (clip.w <= 0.0 || any(puv < 0.0) || any(puv > 1.0)) {
        gOut[p] = cur;
        return;
    }
    float4 hist = clamp(gHistory.SampleLevel(gLinear, puv, 0), mn, mx);
    gOut[p] = lerp(hist, cur, gP0.y);
}

[numthreads(8, 8, 1)]
void CSBlur(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    int2 dir = int2(gP0.xy);
    float zc = LinearZ(gDepthBlur.Load(int3(p * 2, 0)));
    float4 sum = 0;
    float wsum = 0;
    [unroll] for (int k = -2; k <= 2; ++k) {
        int2 q = clamp(int2(p) + k * dir, int2(0, 0), int2(w, h) - 1);
        float zs = LinearZ(gDepthBlur.Load(int3(q * 2, 0)));
        float wt = exp(-k * k / 4.0) * exp(-abs(zs - zc) / (0.05 * zc + 0.5));
        sum += gSrc.Load(int3(q, 0)) * wt;
        wsum += wt;
    }
    gOut[p] = sum / wsum;
}
