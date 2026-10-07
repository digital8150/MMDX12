// Inline ray tracing helpers (RayQuery, shader model 6.5, compiled with DXC).
// Mirror of the RtVertex / RtGeometry layouts in src/render/ShaderInterop.h.
//
// Bindings (ComputePipeline in RenderPass.h, and the ScenePass root signature):
//   t0 space1  RaytracingAccelerationStructure (TLAS)
//   t1 space1  StructuredBuffer<RtGeometry>, entry = InstanceID + GeometryIndex
//   t0 space2  Texture2D[]         unbounded, index = SrvHeap index
//   t0 space3  ByteAddressBuffer[] unbounded, index = SrvHeap index
//   s4         linear wrap sampler
// Include common.hlsli first is not required; it is included here.
#ifndef MMDX_RT_COMMON_HLSLI
#define MMDX_RT_COMMON_HLSLI
#include "common.hlsli"

#ifndef MAT_HAS_TEXTURE
#define MAT_HAS_TEXTURE 1u
#define MAT_HAS_TOON    2u
#define MAT_SPHERE_MUL  4u
#define MAT_SPHERE_ADD  8u
#define MAT_RECEIVE     16u
#define MAT_STAGE       32u
#define MAT_TOON_MAP    64u
#define MAT_FLAT        128u
#endif
#define RTG_CAST_SHADOW (1u << 16)
#define RTG_ALPHA_TEST  (1u << 17)
#define RTG_CHARACTER   (1u << 18)
#define RTG_PT_PACK     (1u << 19)

struct RtPtPackRecord {
    uint materialClass;
    uint headValid;
    uint2 _pad0;
    float4 headRight;
    float4 headUp;
    float4 headForward;
    float4 params[4];
};

struct RtGeometry {
    uint vertexSrv;
    uint indexSrv;
    uint indexStart;
    uint flags;
    float4 diffuse;
    float3 specular; float specularPower;
    float3 ambient;  float reflectivity;
    uint textureSrv;
    uint sphereSrv;
    uint toonSrv;
    uint packSrv;
    float4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;   // material morph factors
};

RaytracingAccelerationStructure gTlas : register(t0, space1);
StructuredBuffer<RtGeometry> gGeometries : register(t1, space1);
Texture2D gBindlessTex[] : register(t0, space2);
ByteAddressBuffer gBindlessBuf[] : register(t0, space3);
SamplerState gRtWrap : register(s4);

static const uint kRtVertexStride = 48;

struct RtSurface {
    float3 pos;        // world position
    float3 prevPos;    // previous frame world position
    float3 normal;     // interpolated shading normal (not flipped toward the ray)
    float3 faceNormal; // geometric normal from the triangle winding (not flipped)
    float2 uv;
};

RtGeometry LoadGeometry(uint instanceId, uint geometryIndex) {
    return gGeometries[instanceId + geometryIndex];
}

uint3 LoadTriangle(RtGeometry g, uint prim) {
    ByteAddressBuffer ib = gBindlessBuf[NonUniformResourceIndex(g.indexSrv)];
    return ib.Load3((g.indexStart + prim * 3) * 4);
}

float2 LoadUv(RtGeometry g, uint3 tri, float2 bary) {
    ByteAddressBuffer vb = gBindlessBuf[NonUniformResourceIndex(g.vertexSrv)];
    float2 uv0 = asfloat(vb.Load2(tri.x * kRtVertexStride + 24));
    float2 uv1 = asfloat(vb.Load2(tri.y * kRtVertexStride + 24));
    float2 uv2 = asfloat(vb.Load2(tri.z * kRtVertexStride + 24));
    return uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
}

RtSurface FetchSurface(RtGeometry g, uint prim, float2 bary) {
    ByteAddressBuffer vb = gBindlessBuf[NonUniformResourceIndex(g.vertexSrv)];
    uint3 tri = LoadTriangle(g, prim);
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    uint3 a = tri * kRtVertexStride;
    float3 p0 = asfloat(vb.Load3(a.x)), p1 = asfloat(vb.Load3(a.y)), p2 = asfloat(vb.Load3(a.z));
    float3 n0 = asfloat(vb.Load3(a.x + 12)), n1 = asfloat(vb.Load3(a.y + 12)), n2 = asfloat(vb.Load3(a.z + 12));
    float2 t0 = asfloat(vb.Load2(a.x + 24)), t1 = asfloat(vb.Load2(a.y + 24)), t2 = asfloat(vb.Load2(a.z + 24));
    float3 q0 = asfloat(vb.Load3(a.x + 32)), q1 = asfloat(vb.Load3(a.y + 32)), q2 = asfloat(vb.Load3(a.z + 32));
    RtSurface s;
    s.pos = p0 * w.x + p1 * w.y + p2 * w.z;
    s.prevPos = q0 * w.x + q1 * w.y + q2 * w.z;
    float3 n = n0 * w.x + n1 * w.y + n2 * w.z;
    float3 fn = cross(p1 - p0, p2 - p0);
    // MMD winding is clockwise front (D3D default): the face normal of a clockwise triangle in
    // a left-handed system is cross(p1 - p0, p2 - p0).
    s.faceNormal = dot(fn, fn) > 1e-20 ? normalize(fn) : float3(0, 1, 0);
    s.normal = dot(n, n) > 1e-12 ? normalize(n) : s.faceNormal;
    s.uv = t0 * w.x + t1 * w.y + t2 * w.z;
    return s;
}

float4 SampleBaseTexture(RtGeometry g, float2 uv, float lod) {
    if ((g.flags & MAT_HAS_TEXTURE) == 0) return float4(1, 1, 1, 1);
    return ApplyTexFactor(gBindlessTex[NonUniformResourceIndex(g.textureSrv)].SampleLevel(gRtWrap, uv, lod),
                          g.texMul, g.texAdd);
}

// Coverage of a candidate (diffuse alpha * texture alpha), mip 0.
float CandidateAlpha(RtGeometry g, uint prim, float2 bary) {
    if ((g.flags & MAT_HAS_TEXTURE) == 0) return g.diffuse.a;
    float2 uv = LoadUv(g, LoadTriangle(g, prim), bary);
    return g.diffuse.a * SampleBaseTexture(g, uv, 0).a;
}

// Albedo in gamma space, the same model as the raster pass (mmd.hlsl PSMain).
float3 MaterialAlbedo(RtGeometry g, float3 tex) {
    return (g.flags & MAT_STAGE) ? g.diffuse.rgb * tex : saturate(g.ambient + g.diffuse.rgb) * 0.8 * tex;
}

// MMD "fully lit" colour in gamma space: saturate(ambient + diffuse * light) * texture.
float3 MaterialLit(RtGeometry g, float3 tex) {
    return saturate(g.ambient + g.diffuse.rgb * gLightColor) * tex;
}

#define RT_MASK_STAGE     0x01u
#define RT_MASK_CHARACTER 0x02u

// 1 = unoccluded. Honours RTG_CAST_SHADOW and alpha (>= 0.5) on non-opaque geometry.
// `mask` selects instances (RT_MASK_*).
float TraceShadowRayMasked(float3 origin, float3 dir, float tMax, uint mask) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, mask, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if ((g.flags & RTG_CAST_SHADOW) != 0 &&
                CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float TraceShadowRay(float3 origin, float3 dir, float tMax) {
    return TraceShadowRayMasked(origin, dir, tMax, 0xFFu);
}

struct RtHit {
    uint instanceId, geometryIndex, prim;
    float2 bary;
    float t;
    bool frontFace;
};

// Closest hit. Non-opaque candidates are accepted when their alpha >= alphaThreshold
// (0.5 for a binary alpha test, a random number in (0,1] for stochastic transparency).
// Back faces of single-sided materials are culled like the raster pass does (stages are often
// closed rooms whose ceiling or walls the camera looks through from outside, and some layer a
// back-facing copy under a visible surface). Double-sided materials live in instances with
// TRIANGLE_CULL_DISABLE (RtScene), so the ray flag only affects single-sided ones.
bool TraceClosest(float3 origin, float3 dir, float tMin, float tMax, float alphaThreshold, out RtHit hit) {
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= alphaThreshold)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    hit.instanceId = 0;
    hit.geometryIndex = 0;
    hit.prim = 0;
    hit.bary = float2(0, 0);
    hit.t = tMax;
    hit.frontFace = true;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    hit.instanceId = q.CommittedInstanceID();
    hit.geometryIndex = q.CommittedGeometryIndex();
    hit.prim = q.CommittedPrimitiveIndex();
    hit.bary = q.CommittedTriangleBarycentrics();
    hit.t = q.CommittedRayT();
    hit.frontFace = q.CommittedTriangleFrontFace();
    return true;
}

// Distance to any hit (alpha >= 0.5 on non-opaque geometry), or -1 when nothing is hit.
float TraceAnyHitDistance(float3 origin, float3 dir, float tMax) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
}

// ---- sampling -----------------------------------------------------------------------

uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint RngSeed(uint2 pixel, uint frame, uint salt) {
    return PcgHash(pixel.x + PcgHash(pixel.y + PcgHash(frame * 16u + salt)));
}
float Rand(inout uint state) {
    state = PcgHash(state);
    return (state >> 8) * (1.0 / 16777216.0);
}

void BuildBasis(float3 n, out float3 t, out float3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 CosineSampleHemisphere(float2 u, float3 n) {
    float r = sqrt(u.x);
    float phi = 6.2831853 * u.y;
    float3 t, b;
    BuildBasis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
}

// Uniform direction inside a cone of half-angle acos(cosMax) around `axis`.
float3 SampleCone(float2 u, float3 axis, float cosMax) {
    float cosT = 1.0 - u.x * (1.0 - cosMax);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 6.2831853 * u.y;
    float3 t, b;
    BuildBasis(axis, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}

// Offset along the geometric normal to avoid self-intersection, scaled with distance from the
// origin (MMD scenes span roughly +-500 units).
float3 OffsetRayOrigin(float3 p, float3 geoNormal) {
    float scale = 0.002 + 1.5e-5 * max(max(abs(p.x), abs(p.y)), abs(p.z));
    return p + geoNormal * scale * 8.0;
}

#endif
