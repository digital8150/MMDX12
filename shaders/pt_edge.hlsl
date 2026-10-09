// Path tracer outlines: ScenePass rasterises the MMD inverted-hull edges in PT mode (a depth pre-pass of the same
// skinned meshes, then the edge draws, default or the pack's PackEdge, into the MSAA scene targets cleared to zero).
// This composites that layer over the denoised path-traced colour, as the offline GI renderer composites its outline
// layer (offline_post.hlsl). Layer: premultiplied linear colour, a = coverage. Where an outline covers most of the
// pixel its velocity replaces the traced one, so TAA reprojects the outline with the outline.
#include "common.hlsli"

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 1
#endif
#if MSAA_SAMPLES > 1
Texture2DMS<float4> gEdgeColor : register(t0);
Texture2DMS<float2> gEdgeVelocity : register(t1);
#else
Texture2D<float4> gEdgeColor : register(t0);
Texture2D<float2> gEdgeVelocity : register(t1);
#endif
RWTexture2D<float4> gColor : register(u0);
RWTexture2D<float2> gVelocityOut : register(u1);

[numthreads(8, 8, 1)]
void CSPtEdgeComposite(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gColor.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = (int2)id.xy;

    float4 e = 0;
    float2 v = 0;
    float vn = 0;
#if MSAA_SAMPLES > 1
    [unroll] for (int s = 0; s < MSAA_SAMPLES; ++s) {
        float4 c = gEdgeColor.Load(p, s);
        e += c;
        if (c.a > 0.5) { v += gEdgeVelocity.Load(p, s); vn += 1.0; }
    }
    e /= (float)MSAA_SAMPLES;
#else
    e = gEdgeColor[p];
    if (e.a > 0.5) { v = gEdgeVelocity[p]; vn = 1.0; }
#endif
    if (e.a <= 0.0) return;

    float4 c = gColor[p];
    gColor[p] = float4(c.rgb * (1.0 - e.a) + e.rgb, c.a * (1.0 - e.a) + e.a);
    if (vn >= 0.5 * (float)MSAA_SAMPLES) gVelocityOut[p] = v / vn;
}
