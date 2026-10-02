// Additive depth-aware upsample of the half-res volumetric buffer into lit.
//   t0 gVol (half res), t1 depth (full res); gP0.xy = half-res texel size.
#include "fullscreen.hlsli"

Texture2D<float4> gVol : register(t0);
Texture2D<float> gDepthTex : register(t1);

float3 UpsampleVol(float2 uv, float z) {
    float2 halfTexel = gP0.xy;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        float2 suv = base + float2(x, y) * halfTexel;
        float sz = LinearZ(gDepthTex.SampleLevel(gPoint, suv, 0));
        float2 f = 1.0 - abs(uv - suv) / halfTexel;
        float w = max(f.x * f.y, 1e-3) / (1e-3 + abs(sz - z));
        sum += gVol.SampleLevel(gPoint, suv, 0).rgb * w;
        wsum += w;
    }
    return sum / wsum;
}

float4 PSApply(FsOut i) : SV_Target {
    float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    float3 rgb = UpsampleVol(i.uv, LinearZ(d));
    return float4(rgb, 0.0);
}
