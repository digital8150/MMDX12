// Half-res volumetric fog ray march (sun through the cascaded shadow map, unshadowed
// punctual lights, energy-conserving integration) + depth-aware separable blur.
//   CSMarch: t0 depth (full res), t1 shadow map, u0 out (half res)
//     gP0 = (sigma0, heightFalloff, maxDistance, sunG), gP1 = (1/outW, 1/outH, spotBoost (omni lights get 15%), punctualG)
//   CSBlur:  t0 src (half res), t1 depth (full res), u0 out; gP0.xy = integer pixel direction.
#include "common.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

struct Light {
    float3 pos;       float invRange;
    float3 color;     float cosOuter;
    float3 dir;       float cosInner;
    float4 pad;
};
StructuredBuffer<Light> gLights : register(t2, space1);

Texture2D<float> gDepthTex : register(t0);
Texture2DArray<float> gShadowMap : register(t1);
Texture2D<float4> gSrc : register(t0);
Texture2D<float> gDepthBlur : register(t1);   // gDepthTex for CSBlur (t1 there is the shadow map in CSMarch)
RWTexture2D<float4> gOut : register(u0);

float HG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float SunVisibility(float3 wp) {
    if (gCascadeSplits.w < 0.5) return 1.0;
    float viewZ = mul(float4(wp, 1.0), gView).z;
    if (viewZ > gCascadeSplits.z) return 1.0;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    float4 sp = mul(float4(wp, 1.0), gShadowViewProj[c]);
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    float stored = gShadowMap.SampleLevel(gPoint, float3(suv, c), 0);
    return q.z - 0.0015 <= stored ? 1.0 : 0.0;
}

[numthreads(8, 8, 1)]
void CSMarch(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    uint2 fp = p * 2;   // full-res source pixel
    float d = gDepthTex.Load(int3(fp, 0));
    float2 uv = (fp + 1.0) * gInvViewportSize;   // centre of the 2x2 block
    float3 vp = ViewPosFromDepth(uv, d >= 1.0 ? 1.0 : d);
    float3 dirV = normalize(vp);
    float dist = d >= 1.0 ? gP0.z : min(length(vp), gP0.z);
    float3 dir = normalize(mul(dirV, (float3x3)gInvView));
    float stepLen = dist / 32.0;
    float j = frac(Ign(float2(p) + 5.588238 * (float)((uint)gFrameIndex & 63u)));

    // Sun colour relative to the default 0.6 grey MMD light.
    float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    float phaseSun = HG(dot(dir, -gLightDir), gP0.w);

    float3 L = 0;
    float T = 1.0;
    for (int i = 0; i < 32; ++i) {
        float t = (i + j) * stepLen;
        float3 wp = gEyePos + dir * t;
        float sigma = gP0.x * exp(-max(wp.y, 0.0) * gP0.y);
        float vis = SunVisibility(wp);
        float3 punctual = 0;
        for (uint li = 0; li < (uint)gNumLights; ++li) {
            float3 dv = gLights[li].pos - wp;
            float dL = length(dv);
            float3 ld = dv / max(dL, 1e-4);
            float x = saturate(1.0 - pow(dL * gLights[li].invRange, 4.0));
            float atten = x * x / (1.0 + dL * dL * 0.0004);
            // spot cones get the full boost; omni fills would only add a uniform veil
            float boost = 0.15;
            if (gLights[li].cosOuter > -1.0) {
                atten *= smoothstep(gLights[li].cosOuter, gLights[li].cosInner, dot(-ld, gLights[li].dir));
                boost = 1.0;
            }
            punctual += gLights[li].color * atten * HG(dot(dir, ld), gP1.w) * boost;
        }
        punctual *= gP1.z;
        float3 S = sigma * (sunCol * vis * phaseSun + punctual);
        float e = exp(-sigma * stepLen);
        L += T * S * (1.0 - e) / max(sigma, 1e-6);
        T *= e;
    }
    gOut[p] = float4(L, T);
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
    [unroll] for (int k = -3; k <= 3; ++k) {
        int2 q = clamp(int2(p) + k * dir, int2(0, 0), int2(w, h) - 1);
        float zs = LinearZ(gDepthBlur.Load(int3(q * 2, 0)));
        float wt = exp(-k * k / 8.0) * exp(-abs(zs - zc) / (0.05 * zc + 0.5));
        sum += gSrc.Load(int3(q, 0)) * wt;
        wsum += wt;
    }
    gOut[p] = sum / wsum;
}
