// Offline GI renderer, part 2: denoise, bloom and the final grade (see OfflineRenderer.h).
//   t0 accum RGBA32F (radiance sum, a = samples)   t1 albedo accum (sum incl. 1 for sky, a = hits)
//   t2 moments RG32F (sum t, sum t^2)               t3 gbuffer (world normal, view depth; 1e6 = sky)
//   t4 outline accumulation RGBA32F (sum of per-iteration layers: premultiplied linear colour, a = coverage)
//   t5 denoiser input (CSDenoise, iteration > 0) / denoised result (CSFinalize) / outline layer (CSEdgeAccum)
//   t6 bloom input (CSBloomBlur) / bloom result at quarter resolution (CSFinalize)
//   t7 volumetric light at half resolution (CSFinalize, gP2.x > 0.5)
//   u0 output
// Root constants:
//   CSDenoise    gP0 = (step, iteration), gP1.xy = image size
//   CSBloomDown  gP0.x = threshold, gP1.xy = image size (dispatch covers the quarter-res target)
//   CSBloomBlur  gP0.xy = direction (1,0) / (0,1), gP1.xy = quarter-res size
//   CSFinalize   gP0 = (useDenoised, useBloom, bloomIntensity, exposure), gP1 = (w, h, vignette, outline layers)
//   CSEdgeAccum  gP1.xy = image size; u0 = outline accumulation (+= t5)
#include "common.hlsli"
#include "offline_glass.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

Texture2D<float4> gAccumT : register(t0);
Texture2D<float4> gAlbedoT : register(t1);
Texture2D<float2> gMomentsT : register(t2);
Texture2D<float4> gGbufT : register(t3);
Texture2D<float4> gEdgeT : register(t4);
Texture2D<float4> gInT : register(t5);
Texture2D<float4> gBloomT : register(t6);
Texture2D<float4> gVolT : register(t7);
RWTexture2D<float4> gOut : register(u0);

float Perceptual(float3 c) {
    float l = Luminance(max(c, 0.0));
    return pow(l / (1.0 + l), 1.0 / 2.2);
}

float3 MeanRadiance(int2 p) {
    float4 a = gAccumT.Load(int3(p, 0));
    return a.rgb / max(a.a, 1.0);
}

float3 MeanAlbedo(int2 p) {
    float n = max(gAccumT.Load(int3(p, 0)).a, 1.0);
    return max(gAlbedoT.Load(int3(p, 0)).rgb / n, 0.03);
}

// Standard error of the mean perceptual luminance.
float MeanError(int2 p) {
    float n = max(gAccumT.Load(int3(p, 0)).a, 1.0);
    float2 m = gMomentsT.Load(int3(p, 0)) / n;
    return sqrt(max(m.y - m.x * m.x, 0.0) / n);
}

// Albedo-demodulated radiance, a = its error estimate.
float4 DenoiseInput(int2 p, bool first) {
    if (first) return float4(MeanRadiance(p) / MeanAlbedo(p), MeanError(p));
    return gInT.Load(int3(p, 0));
}

// Edge-aware a-trous step (5x5 B3 spline) on the demodulated radiance. The luminance stop scales
// with the pixel's own error, so converged pixels are left alone and only residual grain blurs.
[numthreads(8, 8, 1)]
void CSDenoise(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    int step = (int)gP0.x;
    bool first = gP0.y < 0.5;
    float4 g0 = gGbufT.Load(int3(p, 0));
    float4 c0 = DenoiseInput(p, first);
    if (g0.w > 1e5) {
        gOut[p] = c0;
        return;
    }
    const float k[3] = {3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};
    float l0 = Perceptual(c0.rgb);
    float sigmaL = 4.0 * c0.a + 1e-3;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            int2 q = clamp(p + int2(dx, dy) * step, int2(0, 0), size - 1);
            float4 g = gGbufT.Load(int3(q, 0));
            if (g.w > 1e5) continue;
            float4 cq = DenoiseInput(q, first);
            float wn = pow(saturate(dot(g0.xyz, g.xyz)), 64.0);
            float wz = exp(-abs(g.w - g0.w) / (0.01 * g0.w * step + 1e-3));
            float wl = exp(-abs(Perceptual(cq.rgb) - l0) / sigmaL);
            float w = k[abs(dx)] * k[abs(dy)] * wn * wz * wl;
            sum += cq.rgb * w;
            wsum += w;
        }
    }
    gOut[p] = float4(sum / max(wsum, 1e-6), c0.a * 0.5);
}

[numthreads(8, 8, 1)]
void CSBloomDown(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 qs = (size + 3) / 4;
    if ((int)id.x >= qs.x || (int)id.y >= qs.y) return;
    float thr = gP0.x;
    float3 s = 0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            int2 p = min(int2(id.xy) * 4 + int2(x, y), size - 1);
            float3 c;
            if (gP0.y > 0.5) c = gInT.Load(int3(p, 0)).rgb;   // lit HDR: the pre-bloom effects' output (CSLitCompose)
            else c = MeanRadiance(p);
            float l = Luminance(c);
            s += c * (max(l - thr, 0.0) / max(l, 1e-4));
        }
    }
    gOut[id.xy] = float4(s / 16.0, 1.0);
}

[numthreads(8, 8, 1)]
void CSBloomBlur(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    int2 dir = int2(gP0.xy);
    float3 s = 0;
    float ws = 0;
    [loop] for (int i = -16; i <= 16; ++i) {
        float w = exp(-(float)(i * i) / (2.0 * 7.0 * 7.0));
        s += gBloomT.Load(int3(clamp(p + dir * i, int2(0, 0), size - 1), 0)).rgb * w;
        ws += w;
    }
    gOut[p] = float4(s / ws, 1.0);
}

// Khronos PBR Neutral (post.hlsl): keeps MMD albedo colours intact below ~0.8.
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

// The camera ray through the pixel centre meets the glass box first: the raster outline layer cannot
// see through it (the traced image shows the refracted characters), so no outlines there.
bool SeesGlass(int2 p, int2 size) {
    if (!GlassOn()) return false;
    float2 uv = (float2(p) + 0.5) / float2(size);
    float4 v = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0), gInvProj);
    float3 d = normalize(mul(normalize(v.xyz / v.w), (float3x3)gInvView));
    float t;
    float3 n;
    bool inside;
    return GlassIntersect(gInvView[3].xyz, d, 0.0, 1e5, t, n, inside);
}

// Half-resolution volumetric light (rgb in-scattered, a transmittance), upsampled depth-aware (bilinear weights times a depth similarity).
float4 VolUpsample(int2 p, int2 size) {
    const int2 vs = (size + 1) / 2;
    const float zc = min(gGbufT.Load(int3(p, 0)).w, 1e4);
    const float2 f = (float2(p) + 0.5) * 0.5 - 0.5;
    const int2 b = (int2)floor(f);
    const float2 t = f - (float2)b;
    float4 sum = 0;
    float wsum = 0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 q = clamp(b + int2(x, y), int2(0, 0), vs - 1);
            float zq = min(gGbufT.Load(int3(min(q * 2, size - 1), 0)).w, 1e4);
            float wl = max((x ? t.x : 1.0 - t.x) * (y ? t.y : 1.0 - t.y), 1e-3);
            float w = wl / (0.01 + abs(zc - zq) / (0.02 * zc + 0.5));
            sum += gVolT.Load(int3(q, 0)) * w;
            wsum += w;
        }
    }
    return sum / wsum;
}

// HDR -> display: denoised (or raw) radiance, bloom, outlines (MMD ink, composited before the
// tonemap like the raster edges), a soft filmic grade, sRGB with dithering. Opaque output.
[numthreads(8, 8, 1)]
void CSFinalize(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    // lit = 1: gInT is the lit HDR image (CSLitCompose: albedo, volumetric light and outlines are already in it)
    const bool lit = gP2.y > 0.5;
    float3 c;
    if (lit) c = gInT.Load(int3(p, 0)).rgb;
    else c = gP0.x > 0.5 ? gInT.Load(int3(p, 0)).rgb * MeanAlbedo(p) : MeanRadiance(p);
    if (gP2.x > 0.5 && !lit) {
        float4 vol = VolUpsample(p, size);   // rgb in-scattered light, a transmittance
        c = c * saturate(vol.a) + vol.rgb;
    }
    if (gP0.y > 0.5) c += gBloomT.SampleLevel(gLinear, (float2(p) + 0.5) / float2(size), 0).rgb * gP0.z;
    if (gP1.w > 0.0 && !lit && !SeesGlass(p, size)) {
        float4 e = gEdgeT.Load(int3(p, 0)) / gP1.w;   // mean outline layer over the iterations
        c = c * (1.0 - e.a) + e.rgb;
    }
    c = max(c, 0.0) * gP0.w;
    // a touch of saturation; the GI image carries its own contrast
    c = max(lerp(Luminance(c).xxx, c, 1.03), 0.0);
    c = PbrNeutral(c);
    float2 v = (float2(p) / float2(size) - 0.5) * float2(size.x / (float)size.y, 1.0);
    c *= 1.0 - gP1.z * smoothstep(0.35, 1.05, length(v));
    c = LinearToSrgb(saturate(c));
    c += (Ign(float2(p)) - 0.5) / 255.0;
    gOut[p] = float4(saturate(c), 1.0);
}

// Lit HDR for the pre-bloom effects, in the real-time frame's order: the denoised radiance back in albedo, the
// volumetric light and the MMD outlines composed in. CSFinalize (lit = 1) then adds the bloom of the effect output.
[numthreads(8, 8, 1)]
void CSLitCompose(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    float3 c = gInT.Load(int3(p, 0)).rgb * MeanAlbedo(p);
    if (gP2.x > 0.5) {
        float4 vol = VolUpsample(p, size);
        c = c * saturate(vol.a) + vol.rgb;
    }
    if (gP1.w > 0.0 && !SeesGlass(p, size)) {
        float4 e = gEdgeT.Load(int3(p, 0)) / gP1.w;
        c = c * (1.0 - e.a) + e.rgb;
    }
    gOut[p] = float4(max(c, 0.0), 1.0);
}

// Effect inputs (effect_api.hlsli): the oct-encoded view-space normal, as the real-time normal target holds it
// (mmd.hlsl PackOutput: normalize(mul(worldNormal, gView))); 0 on the background. The G-buffer normal is world space.
// The raw device depth is offline_effect.hlsl's CSEffectDepth (an R32_FLOAT target needs its own declaration).
[numthreads(8, 8, 1)]
void CSEffectNormal(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    float4 g = gGbufT.Load(int3(p, 0));
    float2 e = 0.0;
    const float len = length(g.xyz);
    if (g.w < 1e5 && len > 1e-6) e = OctEncode(normalize(mul(g.xyz / len, (float3x3)gView)));
    gOut[p] = float4(e, 0.0, 0.0);
}

// Zero motion: the offline renderer has no per-pixel motion vectors (its motion blur is integrated over the
// shutter). The target is 1x1: every uv of the effect reads texel 0.
[numthreads(8, 8, 1)]
void CSEffectVelocity(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    if ((int)id.x >= size.x || (int)id.y >= size.y) return;
    gOut[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
}

// Adds this iteration's resolved outline layer (camera at the iteration's shutter time and lens
// position) to the accumulation; CSFinalize divides by the layer count.
[numthreads(8, 8, 1)]
void CSEdgeAccum(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gP1.xy);
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y) return;
    gOut[p] = gOut[p] + gInT.Load(int3(p, 0));
}
