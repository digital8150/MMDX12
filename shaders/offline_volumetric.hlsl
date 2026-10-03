// Offline GI renderer: volumetric light (sun shafts + spotlight cones through a height fog), the same
// medium as the real-time volumetric pass, but the sun is shadowed with ray queries against the
// scene (offline_post.hlsl CSFinalize adds the result to the image before outlines and grading).
//   CSVolMarch: t0 gbuffer (world normal, view depth; 1e6 = sky), u0 out (half resolution RGBA16F: in-scattered radiance)
//     gP0 = (sigma0, heightFalloff, maxDistance, sunG), gP1 = (image width, image height, spotBoost, punctualG)
//     gP2.x = per-image seed
//   CSVolBlur: t0 gbuffer, t1 src (half resolution), u0 out; gP0.xy = integer pixel direction
#include "rt_common.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };

struct PtLight { float3 pos; float invRange; float3 color; float cosOuter; float3 dir; float cosInner; float4 pad; };
StructuredBuffer<PtLight> gPtLights : register(t2, space1);

Texture2D<float4> gGbufT : register(t0);
Texture2D<float4> gSrcT : register(t1);
RWTexture2D<float4> gOut : register(u0);

static const int kSteps = 32;

float HG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
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
    float stepLen = dist / (float)kSteps;
    float j = frac(Ign(float2(id.xy) + 5.588238 * fmod(gP2.x, 64.0)));

    float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    float phaseSun = HG(dot(dir, -gLightDir), gP0.w);

    float3 L = 0;
    float T = 1.0;
    for (int i = 0; i < kSteps; ++i) {
        float t = ((float)i + j) * stepLen;
        float3 wp = eye + dir * t;
        float sigma = gP0.x * exp(-max(wp.y, 0.0) * gP0.y);
        float vis = TraceShadowRay(wp, -gLightDir, 4000.0);
        float3 punctual = 0;
        for (uint li = 0; li < (uint)gNumLights; ++li) {
            float3 dv = gPtLights[li].pos - wp;
            float dL = length(dv);
            float3 ld = dv / max(dL, 1e-4);
            float x = saturate(1.0 - pow(dL * gPtLights[li].invRange, 4.0));
            float atten = x * x / (1.0 + dL * dL * 0.0004);
            float boost = 0.15;   // omni fills would only add a uniform veil; spot cones get the full boost
            if (gPtLights[li].cosOuter > -1.0) {
                atten *= smoothstep(gPtLights[li].cosOuter, gPtLights[li].cosInner, dot(-ld, gPtLights[li].dir));
                boost = 1.0;
            }
            punctual += gPtLights[li].color * atten * HG(dot(dir, ld), gP1.w) * boost;
        }
        punctual *= gP1.z;
        float3 S = sigma * (sunCol * vis * phaseSun + punctual);
        float e = exp(-sigma * stepLen);
        L += T * S * (1.0 - e) / max(sigma, 1e-6);
        T *= e;
    }
    gOut[id.xy] = float4(L, T);
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
