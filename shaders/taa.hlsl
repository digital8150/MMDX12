// Temporal anti-aliasing: reprojected history (Catmull-Rom), YCoCg variance clipping.
//   t0 current (HDR), t1 history (HDR), t2 velocity, t3 depth.  gP0.x = history valid.
#include "fullscreen.hlsli"

Texture2D<float4> gCurrent : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float2> gVelocityTex : register(t2);
Texture2D<float> gDepthTex : register(t3);

float3 RgbToYCoCg(float3 c) { return float3(c.r * 0.25 + c.g * 0.5 + c.b * 0.25, c.r * 0.5 - c.b * 0.5, -c.r * 0.25 + c.g * 0.5 - c.b * 0.25); }
float3 YCoCgToRgb(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 Tm(float3 c) { return c / (1.0 + Luminance(c)); }
float3 InvTm(float3 c) { return c / max(1.0 - Luminance(c), 1e-4); }

float4 SampleHistory(float2 uv) {
    // 5-tap Catmull-Rom (Jimenez)
    float2 size = gViewportSize;
    float2 pos = uv * size;
    float2 c = floor(pos - 0.5) + 0.5;
    float2 f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc12 = (c + w2 / w12) / size;
    float2 tc0 = (c - 1.0) / size;
    float2 tc3 = (c + 2.0) / size;
    float4 r = gHistory.SampleLevel(gLinear, float2(tc12.x, tc0.y), 0) * (w12.x * w0.y)
             + gHistory.SampleLevel(gLinear, float2(tc0.x, tc12.y), 0) * (w0.x * w12.y)
             + gHistory.SampleLevel(gLinear, tc12, 0) * (w12.x * w12.y)
             + gHistory.SampleLevel(gLinear, float2(tc3.x, tc12.y), 0) * (w3.x * w12.y)
             + gHistory.SampleLevel(gLinear, float2(tc12.x, tc3.y), 0) * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

float4 PSTaa(FsOut i) : SV_Target {
    float2 texel = gInvViewportSize;
    float4 cur = gCurrent.SampleLevel(gPoint, i.uv, 0);
    if (gP0.x < 0.5) return cur;

    // neighbourhood statistics + closest-depth velocity
    float3 m1 = 0, m2 = 0;
    float closest = 2.0;
    float2 vel = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float2 uv = i.uv + float2(x, y) * texel;
        float3 c = RgbToYCoCg(Tm(gCurrent.SampleLevel(gPoint, uv, 0).rgb));
        m1 += c;
        m2 += c * c;
        float d = gDepthTex.SampleLevel(gPoint, uv, 0);
        if (d < closest) { closest = d; vel = gVelocityTex.SampleLevel(gPoint, uv, 0); }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(abs(m2 / 9.0 - mean * mean));
    float3 lo = mean - sigma * 1.25, hi = mean + sigma * 1.25;

    float2 prevUv = i.uv - vel;
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) return cur;
    float4 hist = SampleHistory(prevUv);
    float3 h = RgbToYCoCg(Tm(hist.rgb));
    // clip toward the mean (AABB clip)
    float3 center = 0.5 * (hi + lo), ext = 0.5 * (hi - lo) + 1e-4;
    float3 v = h - center;
    float3 a = abs(v / ext);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) h = center + v / m;

    float speed = length(vel * gViewportSize);
    float blend = lerp(0.9, 0.75, saturate(speed / 24.0));
    float3 outc = InvTm(YCoCgToRgb(lerp(RgbToYCoCg(Tm(cur.rgb)), h, blend)));
    return float4(outc, lerp(cur.a, hist.a, blend));
}
