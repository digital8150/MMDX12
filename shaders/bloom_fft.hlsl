// FFT convolution bloom (see PassBloom.cpp): the thresholded HDR image is placed into a 512x512
// grid (two complex pairs per texel: .xy = (R, G), .zw = (B, 0)), row/column FFTs run as radix-2
// Stockham passes in groupshared memory, the columns are multiplied by the spectrum of a
// procedural starburst kernel (t1, normalised by K(0,0) = kernel sum), and the inverse transform
// writes the convolved bloom. t0 input grid, gP0 = mode / content rect, gP1 = kernel constants.
#ifndef FFT_N
#define FFT_N 512
#endif
#ifndef FFT_LOG2
#define FFT_LOG2 9
#endif
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gLinear : register(s1);
Texture2D<float4> gIn : register(t0);
Texture2D<float4> gKernelSpec : register(t1);
RWTexture2D<float4> gOut : register(u0);
static const float kPi = 3.14159265;
groupshared float4 gs[2][FFT_N];
float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// FFT of one line held in gs[src], executed by FFT_N/2 threads (thread index j).
// Runs log2(N) passes; returns the buffer index holding the result. dirSign = -1 forward, +1 inverse.
uint FftLine(uint j, uint src, float dirSign) {
    for (uint Ns = 1; Ns < FFT_N; Ns <<= 1) {
        float4 v0 = gs[src][j];
        float4 v1 = gs[src][j + FFT_N / 2];
        uint k = j & (Ns - 1);
        float a = dirSign * 2.0 * kPi * (float)k / (float)(Ns * 2);
        float2 w = float2(cos(a), sin(a));
        v1 = float4(CMul(v1.xy, w), CMul(v1.zw, w));
        uint d = (j / Ns) * Ns * 2 + k;
        gs[src ^ 1][d] = v0 + v1;
        gs[src ^ 1][d + Ns] = v0 - v1;
        GroupMemoryBarrierWithGroupSync();
        src ^= 1;
    }
    return src;
}

// Forward or inverse FFT of row gid.y. gP0.x > 0.5 selects inverse (results scaled by 1/N).
[numthreads(FFT_N / 2, 1, 1)]
void CSFftRows(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint j = tid.x;
    uint ln = gid.y;
    gs[0][j] = gIn[int2(j, ln)];
    gs[0][j + FFT_N / 2] = gIn[int2(j + FFT_N / 2, ln)];
    GroupMemoryBarrierWithGroupSync();
    uint res = FftLine(j, 0, gP0.x > 0.5 ? 1.0 : -1.0);
    float4 a = gs[res][j], b = gs[res][j + FFT_N / 2];
    if (gP0.x > 0.5) {
        a /= (float)FFT_N;
        b /= (float)FFT_N;
    }
    gOut[int2(j, ln)] = a;
    gOut[int2(j + FFT_N / 2, ln)] = b;
}

// Forward FFT of column gid.y; in convolve mode (gP0.x > 0.5) the spectrum is multiplied by the
// kernel spectrum and the line is transformed back (scaled by 1/N).
[numthreads(FFT_N / 2, 1, 1)]
void CSFftCols(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint j = tid.x;
    uint ln = gid.y;
    gs[0][j] = gIn[int2(ln, j)];
    gs[0][j + FFT_N / 2] = gIn[int2(ln, j + FFT_N / 2)];
    GroupMemoryBarrierWithGroupSync();
    uint res = FftLine(j, 0, -1.0);
    if (gP0.x > 0.5) {
        // the kernel is real, so both complex pairs of a texel use the same K
        float4 v0 = gs[res][j];
        float4 v1 = gs[res][j + FFT_N / 2];
        float2 k0 = gKernelSpec[int2(ln, j)].xy / gKernelSpec[int2(0, 0)].x;
        float2 k1 = gKernelSpec[int2(ln, j + FFT_N / 2)].xy / gKernelSpec[int2(0, 0)].x;
        gs[res][j] = float4(CMul(v0.xy, k0), CMul(v0.zw, k0));
        gs[res][j + FFT_N / 2] = float4(CMul(v1.xy, k1), CMul(v1.zw, k1));
        GroupMemoryBarrierWithGroupSync();
        res = FftLine(j, res, 1.0);
        gs[res][j] /= (float)FFT_N;
        gs[res][j + FFT_N / 2] /= (float)FFT_N;
    }
    gOut[int2(ln, j)] = gs[res][j];
    gOut[int2(ln, j + FFT_N / 2)] = gs[res][j + FFT_N / 2];
}

// Thresholded HDR image into the grid. gP0 = content offset xy, content size zw, in grid pixels.
[numthreads(8, 8, 1)]
void CSInput(uint3 id : SV_DispatchThreadID) {
    float2 uv = (float2(id.xy) + 0.5 - gP0.xy) / gP0.zw;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gOut[id.xy] = 0;
        return;
    }
    float3 c = max(gIn.SampleLevel(gLinear, uv, 0).rgb, 0.0);
    gOut[id.xy] = float4(c.r, c.g, c.b, 0.0);
}

// Procedural starburst kernel centred at grid pixel (0,0) with wrap-around.
// gP0 = (haloAmp, haloRadius, spikeAmp, spikeLength), gP1 = (spikeWidth, spikeLines, spikeRotation, coreSigma).
[numthreads(8, 8, 1)]
void CSKernel(uint3 id : SV_DispatchThreadID) {
    float2 d = float2((int2(id.xy) + FFT_N / 2) % FFT_N) - FFT_N / 2;
    float r = length(d);
    float k = exp(-r * r / (2.0 * gP1.w * gP1.w));                  // core
    k += gP0.x * exp(-r / gP0.y);                                    // halo
    for (int s = 0; s < (int)gP1.y; ++s) {                           // spike lines through the centre (2 rays each)
        float a = gP1.z + s * kPi / gP1.y;
        float2 axis = float2(cos(a), sin(a));
        float along = abs(dot(d, axis));
        float perp = abs(dot(d, float2(-axis.y, axis.x)));
        k += gP0.z * exp(-perp * perp / (2.0 * gP1.x * gP1.x)) * exp(-along / gP0.w);
    }
    k *= saturate((FFT_N * 0.5 - r) / 32.0);                         // window: no wrap-around at the grid edge
    gOut[id.xy] = float4(k, 0, 0, 0);
}
