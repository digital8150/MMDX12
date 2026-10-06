// Vertex skinning shared by the raster (mmd.hlsl, offline_edge.hlsl, FXC) and ray-tracing
// (skin.hlsl, DXC) paths: linear blend (BDEF1/2/4, QDEF) and SDEF (spherical deform, as MMD and
// saba compute it). Matrices are row-vector skinning matrices (v' = v * M, pack_matrix row_major):
// M = translate(-bindPosition) * boneWorld, possibly with the model's uniform display scale and root.
// SDEF data (GpuSdef, src/render/ShaderInterop.h): centre C and the precomputed (C + R0') / 2,
// (C + R1') / 2; bones 0/1 with weights w0 = weights.x and w1 = 1 - w0.
#ifndef MMDX_SKINNING_HLSLI
#define MMDX_SKINNING_HLSLI

// Quaternion (xyz, w) of a row-vector rotation matrix (v' = v * m), Shepperd's method.
float4 QuatFromRowMatrix(float3x3 m) {
    float tr = m[0][0] + m[1][1] + m[2][2];
    float4 q;
    if (tr > 0.0) {
        float s = sqrt(tr + 1.0) * 2.0;
        q = float4(m[1][2] - m[2][1], m[2][0] - m[0][2], m[0][1] - m[1][0], 0.25 * s * s) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q = float4(0.25 * s * s, m[1][0] + m[0][1], m[2][0] + m[0][2], m[1][2] - m[2][1]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q = float4(m[0][1] + m[1][0], 0.25 * s * s, m[2][1] + m[1][2], m[2][0] - m[0][2]) / s;
    } else {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q = float4(m[0][2] + m[2][0], m[1][2] + m[2][1], 0.25 * s * s, m[0][1] - m[1][0]) / s;
    }
    return normalize(q);
}

// Row-vector rotation matrix of a unit quaternion (inverse of QuatFromRowMatrix).
float3x3 RowMatrixFromQuat(float4 q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return float3x3(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz), 2.0 * (xz - wy),
                    2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),
                    2.0 * (xz + wy), 2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy));
}

float4 QuatSlerp(float4 a, float4 b, float t) {
    float d = dot(a, b);
    if (d < 0.0) { b = -b; d = -d; }
    if (d > 0.9995) return normalize(lerp(a, b, t));
    float th = acos(d);
    return (a * sin((1.0 - t) * th) + b * sin(t * th)) / sin(th);
}

// SDEF: the rotation part blends by slerp around C; C itself follows the linear blend of the two
// bones through the precomputed cr0/cr1. pos/nrm are model space (morph applied).
void SkinSdef(float4x4 m0, float4x4 m1, float w0, float3 pos, float3 nrm, float3 c, float3 cr0, float3 cr1,
              out float3 wp, out float3 wn) {
    float w1 = 1.0 - w0;
    float3x3 r0 = (float3x3)m0, r1 = (float3x3)m1;
    float s0 = length(r0[0]), s1 = length(r1[0]);   // uniform display scale
    s0 = max(s0, 1e-8);
    s1 = max(s1, 1e-8);
    float4 q = QuatSlerp(QuatFromRowMatrix(r0 / s0), QuatFromRowMatrix(r1 / s1), w1);
    float3x3 r = RowMatrixFromQuat(q) * (s0 * w0 + s1 * w1);
    wp = mul(pos - c, r) + mul(float4(cr0, 1.0), m0).xyz * w0 + mul(float4(cr1, 1.0), m1).xyz * w1;
    wn = mul(nrm, r);
}

// One vertex with the four bone matrices of its bones. sdef > 0.5 selects SDEF (bones 0 and 1).
void SkinVertex(float4x4 b0, float4x4 b1, float4x4 b2, float4x4 b3, float4 w, float3 pos, float3 nrm,
                float sdef, float3 c, float3 cr0, float3 cr1, out float3 wp, out float3 wn) {
    if (sdef > 0.5) {
        SkinSdef(b0, b1, w.x, pos, nrm, c, cr0, cr1, wp, wn);
        return;
    }
    float4x4 m = b0 * w.x + b1 * w.y + b2 * w.z + b3 * w.w;
    wp = mul(float4(pos, 1.0), m).xyz;
    wn = mul(nrm, (float3x3)m);
}

#endif
