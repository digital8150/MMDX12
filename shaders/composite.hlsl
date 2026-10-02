// Lighting composite: colour * AO (depth-aware upsample), blended SSR, distance haze.
//   t0 colour (HDR), t1 depth (raw), t2 normal, t3 ao (half res, may be null), t4 ssr (half res, may be null).
//   gP0.x = AO intensity (0 = off), gP0.y = SSR on, gP0.zw = half-res texel size.
#include "fullscreen.hlsli"

Texture2D<float4> gColorTex : register(t0);
Texture2D<float> gDepthTex : register(t1);
Texture2D<float4> gNormalTex : register(t2);
Texture2D<float> gAoTex : register(t3);
Texture2D<float4> gSsrTex : register(t4);

float UpsampleAo(float2 uv, float z) {
    float2 halfTexel = gP0.zw;
    float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float sum = 0, wsum = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        float2 suv = base + float2(x, y) * halfTexel;
        float sz = LinearZ(gDepthTex.SampleLevel(gPoint, suv, 0));
        float2 f = 1.0 - abs(uv - suv) / halfTexel;
        float w = max(f.x * f.y, 1e-3) / (1e-3 + abs(sz - z));
        sum += gAoTex.SampleLevel(gPoint, suv, 0) * w;
        wsum += w;
    }
    return sum / wsum;
}

float4 PSComposite(FsOut i) : SV_Target {
    float4 c = gColorTex.SampleLevel(gPoint, i.uv, 0);
    float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    if (d >= 1.0) return c;
    float z = LinearZ(d);
    float4 nt = gNormalTex.SampleLevel(gPoint, i.uv, 0);

    if (gP0.x > 0.0) {
        float ao = UpsampleAo(i.uv, z);
        c.rgb *= lerp(1.0, ao, gP0.x);
    }
    if (gP0.y > 0.0 && nt.z > 0.01) {
        // 3x3 alpha-weighted gather hides the per-pixel jitter of the half-res march
        float4 r = 0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float4 sr = gSsrTex.SampleLevel(gLinear, i.uv + float2(x, y) * gP0.zw * 1.2, 0);
            float w = (x == 0 && y == 0) ? 2.0 : 1.0;
            r.rgb += sr.rgb * sr.a * w;
            r.a += sr.a * w;
        }
        r.rgb /= max(r.a, 1e-4);
        r.a /= 10.0;
        float3 V = normalize(ViewPosFromDepth(i.uv, d));
        float3 N = OctDecode(nt.xy);
        float fres = nt.z + (1.0 - nt.z) * pow(1.0 - saturate(dot(-V, N)), 5.0);
        float w = saturate(fres * r.a * nt.w);
        c.rgb = lerp(c.rgb, r.rgb, w);
    }
    if (gFog > 0.0) {
        float3 vp = ViewPosFromDepth(i.uv, d);
        float dist = length(vp);
        float f = (1.0 - exp(-dist * gFog * 0.0011)) * 0.85;
        float3 dir = normalize(mul(normalize(vp), (float3x3)gInvView));
        float3 fogColor = SkyColor(float3(dir.x, max(dir.y, 0.0) * 0.3, dir.z));
        c.rgb = lerp(c.rgb, fogColor, f * c.a);
    }
    return c;
}
