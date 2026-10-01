#pragma pack_matrix(row_major)
cbuffer SceneCB : register(b0) {
    float4x4 gView; float4x4 gProj; float4x4 gViewProj;
    float3 gEyePos; float _p0; float3 gLightDir; float _p1; float3 gLightColor; float _p2;
    float2 gViewportSize; float gEdgeScale; float _p3;
};
cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; uint3 _mp;
};
#define MAT_HAS_TEXTURE 1u
#define MAT_HAS_TOON    2u
#define MAT_SPHERE_MUL  4u
#define MAT_SPHERE_ADD  8u
// FXC ignores pack_matrix for structured-buffer elements: state row_major explicitly.
struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
Texture2D gTexture : register(t1);
Texture2D gSphere  : register(t2);
Texture2D gToon    : register(t3);
SamplerState gWrap  : register(s0);
SamplerState gClamp : register(s1);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2;
};
struct VSOut { float4 pos : SV_Position; float3 worldPos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

float4x4 SkinMatrix(VSIn v) {
    return gBones[v.bones.x].m * v.weights.x + gBones[v.bones.y].m * v.weights.y
         + gBones[v.bones.z].m * v.weights.z + gBones[v.bones.w].m * v.weights.w;
}

VSOut VSMain(VSIn v) {
    VSOut o;
    float4x4 m = SkinMatrix(v);
    float4 wp = mul(float4(v.pos + v.morph, 1.0), m);
    o.pos = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.nrm = mul(v.nrm, (float3x3)m);
    o.uv = v.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 n = normalize(i.nrm);
    float3 l = -normalize(gLightDir);
    float3 vdir = normalize(gEyePos - i.worldPos);
    float4 color = float4(saturate(gAmbient + gDiffuse.rgb * gLightColor), gDiffuse.a);
    if (gFlags & MAT_HAS_TEXTURE) color *= gTexture.Sample(gWrap, i.uv);
    if (gFlags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 s = gSphere.Sample(gClamp, suv).rgb;
        if (gFlags & MAT_SPHERE_MUL) color.rgb *= s; else color.rgb += s;
    }
    if (gFlags & MAT_HAS_TOON) {
        float ln = dot(n, l);
        color.rgb *= gToon.Sample(gClamp, float2(0.0, 0.5 - ln * 0.5)).rgb;
    }
    if (gSpecularPower > 0.0) {
        float3 h = normalize(l + vdir);
        color.rgb += pow(saturate(dot(h, n)), gSpecularPower) * gSpecular * gLightColor;
    }
    if (color.a < 0.004) discard;
    return float4(saturate(color.rgb), color.a);
}

struct EdgeOut { float4 pos : SV_Position; };
EdgeOut VSEdge(VSIn v) {
    EdgeOut o;
    float4x4 m = SkinMatrix(v);
    float4 wp = mul(float4(v.pos + v.morph, 1.0), m);
    float3 wn = normalize(mul(v.nrm, (float3x3)m));
    float4 cp = mul(wp, gViewProj);
    float2 dirPx = mul(float4(wn, 0.0), gViewProj).xy * gViewportSize;   // clip-space normal -> pixel space
    float len = length(dirPx);
    dirPx = len > 1e-6 ? dirPx / len : float2(0, 0);
    float px = gEdgeSize * v.edge * gEdgeScale;                           // outline width in pixels
    cp.xy += dirPx * px * 2.0 / gViewportSize * cp.w;
    o.pos = cp;
    return o;
}
float4 PSEdge(EdgeOut i) : SV_Target { return gEdgeColor; }
