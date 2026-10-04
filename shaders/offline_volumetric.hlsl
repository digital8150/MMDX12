// Offline GI renderer: volumetric light (sun shafts + spotlight beams through a height fog), the same
// medium and integrator as the real-time volumetric pass, but the sun and the spot lights are shadowed
// with ray queries against the scene (offline_post.hlsl CSFinalize composites image * T + L before
// outlines and grading).
//   CSVolMarch: t0 gbuffer (world normal, view depth; 1e6 = sky), u0 out (half resolution RGBA16F:
//               in-scattered radiance, transmittance); volumetric_common.hlsli, spots shadowed with ray queries
//     gP0 = (sigma0, heightFalloff, maxDistance, sunG), gP1 = (image width, image height, spotBoost, punctualG)
//     gP2 = (per-image seed, ambient scale, pointBoost, 0)
//   CSVolBlur: t0 gbuffer, t1 src (half resolution), u0 out; gP0.xy = integer pixel direction
#include "rt_common.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };

Texture2D<float4> gGbufT : register(t0);
Texture2D<float4> gSrcT : register(t1);
RWTexture2D<float4> gOut : register(u0);

#include "volumetric_common.hlsli"

float VolSunVisibility(float3 wp) { return TraceShadowRay(wp, -gLightDir, 4000.0); }

float VolSpotVisibility(VolLight l, float3 wp) {
    float3 dv = l.pos - wp;
    float dL = length(dv);
    return TraceShadowRay(wp, dv / max(dL, 1e-4), max(dL - 0.05, 0.0));
}

[numthreads(8, 8, 1)]
void CSVolMarch(uint3 id : SV_DispatchThreadID) {
    uint ow, oh;
    gOut.GetDimensions(ow, oh);
    if (id.x >= ow || id.y >= oh) return;
    const int2 size = int2(gP1.xy);
    const int2 fp = min(int2(id.xy) * 2, size - 1);
    const float z = gGbufT.Load(int3(fp, 0)).w;

    float2 uv = (float2(fp) + 1.0) / float2(size);   // centre of the 2x2 block
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 v = mul(float4(ndc, 1.0, 1.0), gInvProj);
    float3 vd = normalize(v.xyz / v.w);
    float3 dir = normalize(mul(vd, (float3x3)gInvView));
    float3 eye = gInvView[3].xyz;
    float dist = z > 1e5 ? gP0.z : min(z / max(vd.z, 1e-3), gP0.z);

    VolParams vp;
    vp.sigma0 = gP0.x;
    vp.falloff = gP0.y;
    vp.sunG = gP0.w;
    vp.punctualG = gP1.w;
    vp.spotBoost = gP1.z;
    vp.pointBoost = gP2.z;
    vp.sunShadowDist = 1e9;   // ray queries: shadowed over the whole ray
    vp.sunSteps = 96;
    vp.spotSteps = 48;
    vp.pointSteps = 8;
    vp.sunStepLen = 1.0;
    vp.spotStepLen = 0.75;
    vp.jitter = frac(Ign(float2(id.xy) + 5.588238 * fmod(gP2.x, 64.0)));
    vp.ambient = (gSkyZenith + gSkyHorizon + gGroundColor) * (1.0 / 3.0) * gSunIntensity * gP2.y;
    vp.mediumCenter = float3(0.0, 0.0, 0.0);
    vp.mediumRadius = 180.0;

    float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    gOut[id.xy] = IntegrateVolume(eye, dir, dist, (uint)gNumLights, gLightDir, sunCol, vp);
}

[numthreads(8, 8, 1)]
void CSVolBlur(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy);
    const int2 dir = int2(gP0.xy);
    const int2 full = int2(w, h) * 2;
    float zc = min(gGbufT.Load(int3(min(p * 2, full - 1), 0)).w, 1e4);
    float4 sum = 0;
    float wsum = 0;
    [unroll] for (int k = -3; k <= 3; ++k) {
        int2 q = clamp(p + k * dir, int2(0, 0), int2(w, h) - 1);
        float zs = min(gGbufT.Load(int3(min(q * 2, full - 1), 0)).w, 1e4);
        float wt = exp(-k * k / 8.0) * exp(-abs(zs - zc) / (0.05 * zc + 0.5));
        sum += gSrcT.Load(int3(q, 0)) * wt;
        wsum += wt;
    }
    gOut[p] = sum / wsum;
}
