// pt_surface.hlsl: offline GI and real-time path tracer support for hoyo_toon_v2 (docs/shader_pt_api.md).
#include "hoyo_toon_v2_core.hlsli"

#define P_SOFTNESS        i.params[0]
#define P_NIGHT           i.params[1]
#define P_SPECULAR        i.params[2]
#define P_SHININESS       i.params[3]
#define P_METAL           i.params[4]
#define P_FACE_SOFTNESS   i.params[5]
#define P_FACE_CAST       i.params[6]
#define P_FACE_FLIP       i.params[7]
#define P_RIM_WIDTH       i.params[8]
#define P_RIM_STRENGTH    i.params[9]
#define P_AMBIENT         i.params[10]
#define P_EYE_BRIGHTNESS  i.params[11]
#define P_FB_DARKNESS     i.params[12]
#define P_FB_WARMTH       i.params[13]
#define P_FB_FACE_WIDTH   i.params[14]

bool HasMap(uint tex) {
    return PtPackTexSize(tex).x > 1u;
}

float3 RampColour(uint tex, uint row, float u, float night) {
    float v = (row * 2.0 + 1.0) / 20.0;
    float3 day = PtPackSampleTexLevel(tex, float2(u, v), 0).rgb;
    float3 nightCol = PtPackSampleTexLevel(tex, float2(u, v + 0.5), 0).rgb;
    return lerp(day, nightCol, night);
}

float3 FallbackShadowTint(float extraWarmth, float fbWarmth, float fbDarkness) {
    float3 toonDark = float3(0.5, 0.5, 0.5);
    float3 warm = HoyoV2WarmFactor(extraWarmth, fbWarmth);
    return lerp(float3(1.0, 1.0, 1.0), toonDark * warm, fbDarkness);
}

PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.specular = float3(0.0, 0.0, 0.0);
    o.shadowBias = 0.0;
    o.terminator = float2(0.0, 0.0);
    o.shadowTint = float3(0.7, 0.7, 0.7);
    o.flatFace = false;

    const bool face = (i.materialClass == PACK_FACE);
    const bool eye = (i.materialClass == PACK_EYE);

    if (face || eye) {
        if (eye) {
            o.flatFace = true;
            o.terminator = float2(0.0, 0.0);
            o.shadowBias = 0.0;
        } else {
            o.flatFace = false;
            if (HasMap(T_FACE_SDF) && i.headValid) {
                float3 F = i.headForward - i.headUp * dot(i.headForward, i.headUp);
                F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
                float3 L = i.L - i.headUp * dot(i.L, i.headUp);
                L = dot(L, L) > 1e-6 ? normalize(L) : F;
                float rdl = dot(i.headRight, L);
                bool mirror = (rdl > 0.0) != (P_FACE_FLIP > 0.5);
                float sdf = PtPackSampleTexLevel(T_FACE_SDF, mirror ? float2(1.0 - i.uv.x, i.uv.y) : i.uv, 0).r;
                float threshold = 0.5 - 0.5 * dot(F, L);
                o.terminator = float2(-P_FACE_SOFTNESS, P_FACE_SOFTNESS);
                o.shadowBias = (sdf - threshold) - dot(i.normal, i.L);
            } else if (i.headValid) {
                float3 Nface = HoyoFaceNormal(i.pos, i.headPos, i.headScale, i.headRight, i.headForward, P_FB_FACE_WIDTH);
                float3 Lhoriz = HoyoHeadHorizontalLight(i.L, i.headUp, i.headForward);
                o.terminator = float2(-P_FACE_SOFTNESS, P_FACE_SOFTNESS);
                o.shadowBias = dot(Nface, Lhoriz) - dot(i.normal, i.L);
            } else {
                o.terminator = float2(-P_FACE_SOFTNESS, P_FACE_SOFTNESS);
                o.shadowBias = 0.0;
            }
        }
        o.shadowTint = HasMap(T_BODY_RAMP) ? RampColour(T_BODY_RAMP, 0u, 0.5, P_NIGHT) : FallbackShadowTint(0.15, P_FB_WARMTH, P_FB_DARKNESS);
    } else {
        const bool hair = (i.materialClass == PACK_HAIR);
        const uint lmTex = hair ? T_HAIR_LM : T_BODY_LM;
        const uint rampTex = hair ? T_HAIR_RAMP : T_BODY_RAMP;
        float halfLambert = dot(i.normal, i.L) * 0.5 + 0.5;
        if (HasMap(lmTex)) {
            float4 lm = PtPackSampleTex(lmTex, i.uv);
            float factor = halfLambert * saturate(lm.g * 2.0);
            o.terminator = float2(-2.0 * P_SOFTNESS, 2.0 * P_SOFTNESS);
            if (lm.g < 0.05) {
                o.shadowBias = -10.0;
            } else if (lm.g > 0.95) {
                o.shadowBias = 10.0;
            } else {
                float effective_ndl = 2.0 * factor - 1.0;
                o.shadowBias = effective_ndl - dot(i.normal, i.L);
            }
            uint row = MaterialRow(lm.a);
            float u = saturate(factor * 2.0) * 0.9;
            o.shadowTint = HasMap(rampTex) ? RampColour(rampTex, row, u, P_NIGHT) : FallbackShadowTint(0.0, P_FB_WARMTH, P_FB_DARKNESS);
        } else {
            o.terminator = float2(-2.0 * P_SOFTNESS, 2.0 * P_SOFTNESS);
            o.shadowBias = 0.0;
            o.shadowTint = FallbackShadowTint(i.materialClass == PACK_SKIN ? 0.2 : 0.0, P_FB_WARMTH, P_FB_DARKNESS);
        }
        o.flatFace = false;
    }

    return o;
}
