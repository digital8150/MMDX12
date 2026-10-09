// pt_surface.hlsl: offline GI and real-time path tracer support for hoyo_toon (docs/shader_pt_api.md).
#include "hoyo_toon_core.hlsli"

#define P_RAMP_THRESHOLD   i.params[0]
#define P_RAMP_SOFTNESS    i.params[1]
#define P_SHADOW_DARKNESS  i.params[2]
#define P_SHADOW_WARMTH    i.params[3]
#define P_FACE_SOFTNESS    i.params[4]
#define P_FACE_WIDTH       i.params[5]
#define P_FACE_CAST        i.params[6]
#define P_RIM_WIDTH        i.params[7]
#define P_RIM_STRENGTH     i.params[8]
#define P_HAIR_HIGHLIGHT   i.params[9]
#define P_AMBIENT          i.params[10]
#define P_EYE_BRIGHTNESS   i.params[11]

PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.specular = float3(0.0, 0.0, 0.0);
    o.shadowBias = 0.0;
    o.terminator = float2(0.0, 0.0);
    o.shadowTint = float3(0.7, 0.7, 0.7);
    o.flatFace = false;

    float extraWarmth = 0.0;
    if (i.materialClass == PACK_SKIN) {
        extraWarmth = 0.2;
    } else if (i.materialClass == PACK_FACE || i.materialClass == PACK_EYE) {
        extraWarmth = 0.15;
    }

    float3 warm = HoyoShadowWarmFactor(extraWarmth, P_SHADOW_WARMTH);
    float3 toonDark = float3(0.5, 0.5, 0.5);
    o.shadowTint = lerp(float3(1.0, 1.0, 1.0), toonDark * warm, P_SHADOW_DARKNESS);

    if (i.materialClass == PACK_FACE) {
        if (i.headValid) {
            float3 Nface = HoyoFaceNormal(i.pos, i.headPos, i.headScale, i.headRight, i.headForward, P_FACE_WIDTH);
            float3 Lhoriz = HoyoHeadHorizontalLight(i.L, i.headUp, i.headForward);
            o.shadowBias = dot(Nface, Lhoriz) - dot(i.normal, i.L);
            o.terminator = float2(-P_FACE_SOFTNESS, P_FACE_SOFTNESS);
        } else {
            o.shadowBias = 0.0;
            o.terminator = float2(-P_FACE_SOFTNESS, P_FACE_SOFTNESS);
        }
        o.flatFace = false;
    } else if (i.materialClass == PACK_EYE) {
        o.shadowBias = 0.0;
        o.terminator = float2(0.0, 0.0);
        o.flatFace = true;
    } else {
        float soft = P_RAMP_SOFTNESS * (i.materialClass == PACK_SKIN ? 2.0 : 1.0);
        o.terminator = float2(2.0 * (P_RAMP_THRESHOLD - soft) - 1.0, 2.0 * (P_RAMP_THRESHOLD + soft) - 1.0);
        o.shadowBias = 0.0;
        o.flatFace = false;
    }

    return o;
}
