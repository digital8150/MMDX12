// Screen-space ambient occlusion (normal-oriented hemisphere) at half resolution, followed
// by a depth-aware separable blur. The ray-traced AO (rtao.hlsl) goes through PSTemporal first.
//   PSAo:   t0 depth (full res, raw), t1 normal (full res). gP0.x = radius, gP0.y = sample count,
//           gP0.zw = full-res texel size.
//   PSBlur: t0 depth, t2 ao. gP0.xy = blur direction in half-res texels (uv units).
//   PSTemporal: t0 depth (full res), t2 this frame's AO (half res), t3 last frame's accumulated AO,
//           t4 velocity (full res, uv(cur) - uv(prev)). gP0.x = history valid.
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

Texture2D<float> gHistTex : register(t3);
Texture2D<float2> gVelTex : register(t4);

// RTAO accumulation. The rays are redrawn every frame, so a single frame's AO shimmers; this
// averages it over frames. Last frame's value is reprojected with the velocity of the nearest
// full-res pixel, clamped to this frame's 3x3 neighbourhood (mean +- 1.25 sigma, like TAA's colour
// clip) so disocclusions and moving occluders do not smear, and blended with a weight that drops
// while the surface moves across the pixel grid.
float PSTemporal(FsOut i) : SV_Target {
    float cur = gAoTex.SampleLevel(gPoint, i.uv, 0);
    if (gP0.x < 0.5) return cur;

    uint hw, hh;
    gAoTex.GetDimensions(hw, hh);
    const float2 texel = 1.0 / float2(hw, hh);

    float m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float v = gAoTex.SampleLevel(gPoint, i.uv + float2(x, y) * texel, 0);
        m1 += v;
        m2 += v * v;
    }
    const float mean = m1 / 9.0;
    const float sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float lo = mean - sigma * 1.25, hi = mean + sigma * 1.25;

    // velocity of the nearest of the 2x2 full-res pixels behind this half-res pixel (edges stay sharp)
    uint fw, fh;
    gDepthTex.GetDimensions(fw, fh);
    const int2 fp = int2(uint2(i.pos.xy) * 2);
    float closest = 2.0;
    float2 vel = 0;
    [unroll] for (int dy = 0; dy < 2; ++dy)
    [unroll] for (int dx = 0; dx < 2; ++dx) {
        int2 q = min(fp + int2(dx, dy), int2(fw, fh) - 1);
        float d = gDepthTex.Load(int3(q, 0));
        if (d < closest) { closest = d; vel = gVelTex.Load(int3(q, 0)); }
    }

    const float2 prevUv = i.uv - vel;
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) return cur;
    const float hist = clamp(gHistTex.SampleLevel(gLinear, prevUv, 0), lo, hi);

    // history weight: high when the pixel is still, lower while it moves (at 4 half-res texels per frame it is 0.6)
    const float speed = length(vel * float2(hw, hh));
    const float histW = lerp(0.92, 0.6, saturate(speed / 4.0));
    return lerp(cur, hist, histW);
}
