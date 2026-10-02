// Ray-traced ambient occlusion at half resolution: cosine-weighted hemisphere rays against
// the TLAS, occlusion falloff quadratic in the hit distance.
#include "rt_common.hlsli"   // includes common.hlsli
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);

Texture2D<float> gDepthTex : register(t0);
Texture2D<float4> gNormalTex : register(t1);
RWTexture2D<float> gAoOut : register(u0);

[numthreads(8, 8, 1)]
void CSRtao(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gAoOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    uint2 fp = p * 2;   // full-res source pixel, loaded without filtering
    float d = gDepthTex.Load(int3(fp, 0));
    if (d >= 1.0) { gAoOut[p] = 1.0; return; }
    float4 nt = gNormalTex.Load(int3(fp, 0));
    if (nt.w < 0.01) { gAoOut[p] = 1.0; return; }   // no coverage / edges

    float2 uv = (fp + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d);
    float3 wp = mul(float4(vp, 1.0), gInvView).xyz;
    float3 wn = normalize(mul(OctDecode(nt.xy), (float3x3)gInvView));

    uint rng = RngSeed(p, (uint)gFrameIndex, 1u);
    float3 origin = OffsetRayOrigin(wp, wn);
    uint n = max((uint)gP0.w, 1u);

    float occ = 0;
    for (uint i = 0; i < n; ++i) {
        float3 dir = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), wn);
        float t = TraceAnyHitDistance(origin, dir, gP0.x);
        if (t >= 0.0) {
            float f = saturate(t / gP0.x);
            occ += 1.0 - f * f;
        }
    }
    gAoOut[p] = saturate(1.0 - occ / n);
}
