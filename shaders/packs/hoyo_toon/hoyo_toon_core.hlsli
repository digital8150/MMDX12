// hoyo_toon_core.hlsli: shared shading math for hoyo_toon v1 (raster and PT/GI).
#ifndef HOYO_TOON_CORE_HLSLI
#define HOYO_TOON_CORE_HLSLI

// Computes the horizontal face normal from the head frame.
// Projects the head-local horizontal offset into a cylindrical normal.
float3 HoyoFaceNormal(float3 worldPos, float3 headPos, float headScale, float3 headRight, float3 headForward, float faceWidth) {
    float3 p = worldPos - headPos;
    float w = max(faceWidth, 0.01);
    float s = max(headScale, 1e-4);
    float x = dot(p, headRight) / (w * s);
    float z = dot(p, headForward) / s;
    float2 n = float2(x, z);
    float len = length(n);
    n = len > 1e-4 ? n / len : float2(0.0, 1.0);
    return normalize(n.x * headRight + n.y * headForward);
}

// Projects the light direction into the head's horizontal plane.
float3 HoyoHeadHorizontalLight(float3 L, float3 headUp, float3 headForward) {
    float3 Lhoriz = L - headUp * dot(L, headUp);
    float len = length(Lhoriz);
    return len > 1e-4 ? Lhoriz / len : headForward;
}

// Face light value: smoothstep of dot(Nface, Lhoriz).
// Under frontal light, ndl > 0 across the entire face front, leaving it cleanly lit.
// Under side light, the terminator smoothly sweeps across the center of the face.
float HoyoFaceGeometricLight(float3 worldPos, float3 headPos, float headScale, float3 headRight, float3 headUp, float3 headForward, float3 L, float faceWidth, float faceSoftness) {
    float3 Nface = HoyoFaceNormal(worldPos, headPos, headScale, headRight, headForward, faceWidth);
    float3 Lhoriz = HoyoHeadHorizontalLight(L, headUp, headForward);
    float ndl = dot(Nface, Lhoriz);
    return smoothstep(-faceSoftness, faceSoftness, ndl);
}

// Shadow tint factor (warm rose tint).
float3 HoyoShadowWarmFactor(float extraWarmth, float shadowWarmth) {
    return lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.82, 0.84), saturate(shadowWarmth + extraWarmth));
}

#endif // HOYO_TOON_CORE_HLSLI
