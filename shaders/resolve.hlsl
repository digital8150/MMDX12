// MSAA -> single sample. Colour: luminance-weighted average (keeps bright edges from
// aliasing after tonemapping). Normal / velocity / depth: the closest sample.
#include "fullscreen.hlsli"

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 4
#endif

#if MSAA_SAMPLES > 1
Texture2DMS<float4> gColor : register(t0);
Texture2DMS<float4> gNormal : register(t1);
Texture2DMS<float2> gVelocity : register(t2);
Texture2DMS<float> gDepth : register(t3);
#else
Texture2D<float4> gColor : register(t0);
Texture2D<float4> gNormal : register(t1);
Texture2D<float2> gVelocity : register(t2);
Texture2D<float> gDepth : register(t3);
#endif

struct ResolveOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float2 velocity : SV_Target2;
    float depth : SV_Target3;
};

ResolveOut PSResolve(FsOut i) {
    int2 p = int2(i.pos.xy);
    ResolveOut o;
#if MSAA_SAMPLES > 1
    float4 sum = 0;
    float wsum = 0;
    float best = 2.0;
    int bestIdx = 0;
    [unroll] for (int s = 0; s < MSAA_SAMPLES; ++s) {
        float4 c = gColor.Load(p, s);
        float w = 1.0 / (1.0 + Luminance(max(c.rgb, 0.0)));
        sum += float4(c.rgb * w, c.a * w);
        wsum += w;
        float d = gDepth.Load(p, s);
        if (d < best) { best = d; bestIdx = s; }
    }
    o.color = sum / wsum;
    o.normal = gNormal.Load(p, bestIdx);
    // coverage: average so partially covered edge pixels keep partial reflectivity weight
    float cov = 0;
    [unroll] for (int k = 0; k < MSAA_SAMPLES; ++k) cov += gNormal.Load(p, k).w;
    o.normal.w = cov / MSAA_SAMPLES;
    o.velocity = gVelocity.Load(p, bestIdx);
    o.depth = best;
#else
    o.color = gColor.Load(int3(p, 0));
    o.normal = gNormal.Load(int3(p, 0));
    o.velocity = gVelocity.Load(int3(p, 0));
    o.depth = gDepth.Load(int3(p, 0));
#endif
    return o;
}
