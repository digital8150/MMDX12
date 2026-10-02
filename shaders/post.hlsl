// Final grade: bloom, exposure, contrast/saturation, Khronos PBR Neutral tonemap (keeps
// MMD albedo colours intact below ~0.8), vignette, sRGB encode with dithering.
//   t0 HDR, t1 bloom (may be null).
//   gP0 = (exposure, bloomIntensity, contrast, saturation), gP1 = (vignette, transparentBg, aspect, bloomOn)
#include "fullscreen.hlsli"

Texture2D<float4> gHdr : register(t0);
Texture2D<float4> gBloomTex : register(t1);

float3 PbrNeutral(float3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

float4 PSPost(FsOut i) : SV_Target {
    float4 src = gHdr.SampleLevel(gPoint, i.uv, 0);
    float3 c = max(src.rgb, 0.0);
    if (gP1.w > 0.5) c += gBloomTex.SampleLevel(gLinear, i.uv, 0).rgb * gP0.y;
    c *= gP0.x;
    // contrast around middle grey in log space, then saturation
    float3 lc = log2(max(c, 1e-5) / 0.18);
    c = 0.18 * exp2(lc * gP0.z);
    float l = Luminance(c);
    c = max(lerp(l.xxx, c, gP0.w), 0.0);
    c = PbrNeutral(c);
    // vignette
    float2 v = (i.uv - 0.5) * float2(gP1.z, 1.0);
    c *= 1.0 - gP1.x * smoothstep(0.35, 1.05, length(v));
    c = LinearToSrgb(saturate(c));
    c += (Ign(i.pos.xy) - 0.5) / 255.0;
    float a = gP1.y > 0.5 ? saturate(src.a) : 1.0;
    return float4(c, a);
}

// UI backdrop: downsample + separable gaussian of the final LDR image.
//   gP0.xy = source texel size, gP0.zw = blur direction (uv units, 0 for the downsample).
float4 PSBackdrop(FsOut i) : SV_Target {
    if (gP0.z == 0.0 && gP0.w == 0.0) {
        float2 t = gP0.xy;
        float4 s = gHdr.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0) + gHdr.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0) +
                   gHdr.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0) + gHdr.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0);
        return s * 0.25;
    }
    static const float w[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};
    float4 s = gHdr.SampleLevel(gLinear, i.uv, 0) * w[0];
    [unroll] for (int k = 1; k < 5; ++k) {
        s += gHdr.SampleLevel(gLinear, i.uv + gP0.zw * k * 1.4, 0) * w[k];
        s += gHdr.SampleLevel(gLinear, i.uv - gP0.zw * k * 1.4, 0) * w[k];
    }
    return s;
}
