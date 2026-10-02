// Physically based bloom (Jimenez 2014): 13-tap downsample chain with a Karis-averaged,
// soft-thresholded first step, then 3x3 tent upsampling added back up the chain.
//   t0 source.  gP0.xy = source texel size, gP0.z = threshold, gP0.w = upsample radius scale.
// PSFftOutput: convolution bloom output (bloom_fft.hlsl), reads the convolved grid from the SRV table.
#include "fullscreen.hlsli"

Texture2D<float4> gSrc : register(t0);

float3 Down13(float2 uv, float2 t) {
    float3 a = gSrc.SampleLevel(gLinear, uv + t * float2(-2, -2), 0).rgb;
    float3 b = gSrc.SampleLevel(gLinear, uv + t * float2(0, -2), 0).rgb;
    float3 c = gSrc.SampleLevel(gLinear, uv + t * float2(2, -2), 0).rgb;
    float3 d = gSrc.SampleLevel(gLinear, uv + t * float2(-2, 0), 0).rgb;
    float3 e = gSrc.SampleLevel(gLinear, uv, 0).rgb;
    float3 f = gSrc.SampleLevel(gLinear, uv + t * float2(2, 0), 0).rgb;
    float3 g = gSrc.SampleLevel(gLinear, uv + t * float2(-2, 2), 0).rgb;
    float3 h = gSrc.SampleLevel(gLinear, uv + t * float2(0, 2), 0).rgb;
    float3 k = gSrc.SampleLevel(gLinear, uv + t * float2(2, 2), 0).rgb;
    float3 l = gSrc.SampleLevel(gLinear, uv + t * float2(-1, -1), 0).rgb;
    float3 m = gSrc.SampleLevel(gLinear, uv + t * float2(1, -1), 0).rgb;
    float3 n = gSrc.SampleLevel(gLinear, uv + t * float2(-1, 1), 0).rgb;
    float3 o = gSrc.SampleLevel(gLinear, uv + t * float2(1, 1), 0).rgb;
    return e * 0.125 + (a + c + g + k) * 0.03125 + (b + d + f + h) * 0.0625 + (l + m + n + o) * 0.125;
}

float KarisWeight(float3 c) { return 1.0 / (1.0 + Luminance(c)); }

float4 PSPrefilter(FsOut i) : SV_Target {
    float2 t = gP0.xy;
    // Karis average over the four 2x2 groups suppresses single-pixel fireflies.
    float3 g0 = gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0).rgb;
    float3 g1 = gSrc.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0).rgb;
    float3 g2 = gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0).rgb;
    float3 g3 = gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0).rgb;
    float w0 = KarisWeight(g0), w1 = KarisWeight(g1), w2 = KarisWeight(g2), w3 = KarisWeight(g3);
    float3 c = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3) / (w0 + w1 + w2 + w3);
    // soft knee threshold
    float threshold = gP0.z;
    float knee = threshold * 0.5;
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    rq = rq * rq / (4.0 * knee + 1e-4);
    float contrib = max(rq, br - threshold) / max(br, 1e-4);
    return float4(c * contrib, 1.0);
}

float4 PSDown(FsOut i) : SV_Target {
    return float4(Down13(i.uv, gP0.xy), 1.0);
}

float4 PSUp(FsOut i) : SV_Target {
    float2 t = gP0.xy * gP0.w;
    float3 s = gSrc.SampleLevel(gLinear, i.uv, 0).rgb * 4.0;
    s += (gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 0), 0).rgb + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 0), 0).rgb +
          gSrc.SampleLevel(gLinear, i.uv + t * float2(0, -1), 0).rgb + gSrc.SampleLevel(gLinear, i.uv + t * float2(0, 1), 0).rgb) * 2.0;
    s += gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0).rgb + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0).rgb +
         gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0).rgb + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0).rgb;
    return float4(s / 16.0, 1.0);
}

// Convolution bloom output: t0 = convolved FFT grid (RGBA32F). gP0.xy = content offset / N, gP0.zw = content size / N,
// gP1.x = gain.
float4 PSFftOutput(FsOut i) : SV_Target {
    float2 g = gP0.xy + i.uv * gP0.zw;
    float3 c = gSrc.SampleLevel(gLinear, g, 0).xyz;
    return float4(max(c, 0.0) * gP1.x, 1.0);
}
