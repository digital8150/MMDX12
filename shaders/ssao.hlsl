// Screen-space ambient occlusion (normal-oriented hemisphere) at half resolution, followed
// by a depth-aware separable blur. The ray-traced AO (rtao.hlsl) goes through PSTemporal first.
//   PSAo:   t0 depth (full res, raw), t1 normal (full res). gP0.x = radius, gP0.y = sample count,
//           gP0.zw = full-res texel size.
//   PSBlur: t0 depth, t2 ao. gP0.xy = blur direction in half-res texels (uv units).
//   PSTemporal: t0 depth (full res), t2 this frame's AO (half res), t3 last frame's accumulated AO,
//           t4 velocity (full res, uv(cur) - uv(prev)), t5 previous linear depth.
//           gP0.x = history valid, gP0.y = history cap; MRT outputs AO/length and linear depth.
#include "fullscreen.hlsli"

Texture2D<float> gDepthTex : register(t0);
Texture2D<float4> gNormalTex : register(t1);

static const float3 kKernel[16] = {
    float3(0.536, 0.218, 0.213), float3(-0.398, 0.507, 0.146), float3(0.065, -0.414, 0.351),
    float3(-0.169, -0.103, 0.087), float3(0.311, -0.622, 0.517), float3(-0.704, -0.211, 0.341),
    float3(0.146, 0.698, 0.469), float3(0.018, 0.044, 0.157), float3(-0.288, 0.252, 0.645),
    float3(0.774, 0.233, 0.443), float3(-0.462, -0.598, 0.212), float3(0.119, 0.092, 0.394),
    float3(0.257, -0.143, 0.083), float3(-0.087, -0.311, 0.728), float3(-0.633, 0.508, 0.211),
    float3(0.401, 0.400, 0.098)
};

float PSAo(FsOut i) : SV_Target {
    float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    float4 nt = gNormalTex.SampleLevel(gPoint, i.uv, 0);
    if (d >= 1.0 || nt.w < 0.5) return 1.0;
    float3 P = ViewPosFromDepth(i.uv, d);
    float3 N = OctDecode(nt.xy);
    float radius = gP0.x;

    float a = Ign(i.pos.xy + gFrameIndex * 5.588238) * 6.2831853;
    float3 rnd = float3(cos(a), sin(a), 0.0);
    float3 T = normalize(rnd - N * dot(rnd, N));
    float3 B = cross(N, T);
    float3x3 tbn = float3x3(T, B, N);

    float occlusion = 0;
    const int count = 16;
    [unroll] for (int s = 0; s < count; ++s) {
        float3 sp = P + mul(kKernel[s], tbn) * radius;
        float2 suv = ViewToUv(sp);
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float sd = gDepthTex.SampleLevel(gPoint, suv, 0);
        float sz = LinearZ(sd);
        float range = smoothstep(0.0, 1.0, radius / max(abs(P.z - sz), 1e-3));
        occlusion += (sz <= sp.z - 0.02 * P.z * 0.01 - 0.03 ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - occlusion / count;
    return saturate(pow(ao, 1.6));
}

Texture2D<float> gAoTex : register(t2);

float PSBlur(FsOut i) : SV_Target {
    float2 dir = gP0.xy;
    float centerZ = LinearZ(gDepthTex.SampleLevel(gPoint, i.uv, 0));
    float sum = 0, wsum = 0;
    [unroll] for (int k = -3; k <= 3; ++k) {
        float2 uv = i.uv + dir * k;
        float z = LinearZ(gDepthTex.SampleLevel(gPoint, uv, 0));
        float w = exp(-k * k / 8.0) * saturate(1.0 - abs(z - centerZ) / (centerZ * 0.04 + 0.2));
        sum += gAoTex.SampleLevel(gPoint, uv, 0) * w;
        wsum += w;
    }
    return wsum > 1e-4 ? sum / wsum : gAoTex.SampleLevel(gPoint, i.uv, 0);
}

Texture2D<float2> gHistTex : register(t3);
Texture2D<float2> gVelTex : register(t4);
Texture2D<float> gHistZTex : register(t5);

struct TemporalOut { float2 acc : SV_Target0; float z : SV_Target1; };

TemporalOut PSTemporal(FsOut i) {
    float cur = gAoTex.SampleLevel(gPoint, i.uv, 0);
    uint hw, hh, fw, fh;
    gAoTex.GetDimensions(hw, hh);
    gDepthTex.GetDimensions(fw, fh);
    int2 fp = int2(uint2(i.pos.xy) * 2), chosen = fp;
    float closest = 2.0;
    float2 vel = 0;
    [unroll] for (int dy = 0; dy < 2; ++dy)
    [unroll] for (int dx = 0; dx < 2; ++dx) {
        int2 q = min(fp + int2(dx, dy), int2(fw, fh) - 1);
        float d = gDepthTex.Load(int3(q, 0));
        if (d < closest) { closest = d; chosen = q; vel = gVelTex.Load(int3(q, 0)); }
    }
    TemporalOut result;
    if (closest >= 1.0) { result.acc = float2(1, 1); result.z = 0; return result; }
    float zCur = LinearZ(closest);
    float2 texel = 1.0 / float2(hw, hh);
    float m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float v = gAoTex.SampleLevel(gPoint, i.uv + float2(x, y) * texel, 0);
        m1 += v; m2 += v * v;
    }
    float mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float lo = mean - 2.0 * sigma, hi = mean + 2.0 * sigma;
    // A pixel without usable history (first frame, disocclusion, thin alpha-tested hair) starts from the 3x3 mean:
    // the single pixel's few rays alone flicker frame to frame.
    result.acc = float2(mean, 1); result.z = zCur;
    if (gP0.x < 0.5) return result;
    float2 fuv = (float2(chosen) + 0.5) / float2(fw, fh);
    float3 vp = ViewPosFromDepth(fuv, closest);
    float3 wp = mul(float4(vp, 1), gInvView).xyz;
    float zExp = mul(float4(wp, 1), gPrevViewProjNoJitter).w;
    if (zExp <= 1e-3) zExp = zCur;
    float2 prevUv = i.uv - vel;
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) return result;
    float2 pp = prevUv * float2(hw, hh) - 0.5;
    int2 b = (int2)floor(pp);
    float2 f = pp - b;
    float tol = 0.05 * zExp + 0.25;
    float2 s = 0; float wsum = 0;
    [unroll] for (int oy = 0; oy <= 1; ++oy)
    [unroll] for (int ox = 0; ox <= 1; ++ox) {
        int2 q = b + int2(ox, oy);
        if (any(q < 0) || q.x >= (int)hw || q.y >= (int)hh) continue;
        float bw = (ox ? f.x : 1 - f.x) * (oy ? f.y : 1 - f.y);
        float zq = gHistZTex.Load(int3(q, 0));
        if (abs(zq - zExp) > tol) continue;
        s += bw * gHistTex.Load(int3(q, 0)); wsum += bw;
    }
    if (wsum < 1e-3) return result;
    float hAo = s.x / wsum, hLen = s.y / wsum;
    float hC = clamp(hAo, lo, hi);
    if (abs(hC - hAo) > 0.15) hLen = min(hLen, 4.0);
    float len = min(hLen + 1.0, max(gP0.y, 1.0));
    // while the history is short the new sample leans on the neighbourhood mean (less variance, slightly blurrier)
    float sampleAo = lerp(mean, cur, saturate((len - 1.0) / 4.0));
    result.acc = float2(lerp(hC, sampleAo, 1.0 / len), len);
    return result;
}

float PSBlurRt(FsOut i) : SV_Target {
    float centerZ = LinearZ(gDepthTex.SampleLevel(gPoint, i.uv, 0));
    float len = gHistTex.SampleLevel(gPoint, i.uv, 0).y;
    float4 ntC = gNormalTex.SampleLevel(gPoint, i.uv, 0);
    bool useN = ntC.w >= 0.5 && len >= 4.0;   // fresh pixels blur across normals too
    float3 nC = OctDecode(ntC.xy);
    float stepScale = lerp(2.5, 1.0, saturate((len - 1.0) / 7.0));
    float sum = 0, wsum = 0;
    [unroll] for (int k = -3; k <= 3; ++k) {
        float2 uv = i.uv + gP0.xy * k * stepScale;
        float z = LinearZ(gDepthTex.SampleLevel(gPoint, uv, 0));
        float w = exp(-k * k / 8.0) * saturate(1 - abs(z - centerZ) / (centerZ * 0.04 + 0.2));
        if (useN) {
            float4 nt = gNormalTex.SampleLevel(gPoint, uv, 0);
            if (nt.w >= 0.5) w *= pow(saturate(dot(nC, OctDecode(nt.xy))), 8.0);
        }
        sum += gAoTex.SampleLevel(gPoint, uv, 0) * w; wsum += w;
    }
    return wsum > 1e-4 ? sum / wsum : gAoTex.SampleLevel(gPoint, i.uv, 0);
}
