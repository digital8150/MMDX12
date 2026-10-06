// hoyo_toon: HoYoverse-style cel shading for the official Genshin / Star Rail / ZZZ MMD models.
//
// A teaching pack: read it top to bottom next to docs/shader-packs.md. It shows the three things most packs need:
//   1. reading pack parameters      (PackParam, in pack.json order)
//   2. branching on material class  (s.materialClass: face / eye / hair / skin / body, from pack.json rules)
//   3. using the head frame         (s.headPos / headRight / headForward: the face is lit from the head, not from normals)
//
// Why: the default MMDX12 shading models every surface with its normal (N.L terminator, sky/ground gradient, rim,
// ambient occlusion). On these models that turns the face into a sculpted 3D mask: shadows under the nose, gradient
// cheeks. In the games the face shadow is an artist-painted SDF map driven only by the head's angle to the light, and
// the body is a hard two-tone ramp with a flat ambient. The MMD releases do not ship those SDF maps, so the face shadow
// below is a geometric stand-in that behaves the same way (a clean edge sweeping across the face as the light turns).

// ---- parameters (pack.json "params", same order) ---------------------------------------------------------------------
#define P_RAMP_THRESHOLD   PackParam(0)   // body terminator on half-Lambert (0..1)
#define P_RAMP_SOFTNESS    PackParam(1)   // width of the terminator
#define P_SHADOW_DARKNESS  PackParam(2)   // 0 = shadows as bright as light, 1 = full toon shadow colour
#define P_SHADOW_WARMTH    PackParam(3)   // tint the shadow toward warm rose (skin-friendly)
#define P_FACE_SOFTNESS    PackParam(4)   // face shadow edge width
#define P_FACE_WIDTH       PackParam(5)   // half width of the face, in model units (MMD heads are ~1.6 wide)
#define P_FACE_CAST        PackParam(6)   // how much cast shadows (hair, hat) darken the face
#define P_RIM_WIDTH        PackParam(7)   // rim light width (fraction of the silhouette)
#define P_RIM_STRENGTH     PackParam(8)
#define P_HAIR_HIGHLIGHT   PackParam(9)   // stepped specular band on hair / glossy materials
#define P_AMBIENT          PackParam(10)  // flat ambient fill (replaces the sky / ground gradient)
#define P_EYE_BRIGHTNESS   PackParam(11)

// Shadow colour of a lit colour: the material's own MMD toon ramp at its dark end (the artist's shadow tone), tinted.
float3 ShadowColour(float3 lit, float extraWarmth) {
    float3 toonDark = PackSampleToon(1.0);
    float3 warm = lerp(float3(1, 1, 1), float3(1.0, 0.82, 0.84), saturate(P_SHADOW_WARMTH + extraWarmth));
    return lit * lerp(float3(1, 1, 1), toonDark * warm, P_SHADOW_DARKNESS);
}

// Face shadow from the head frame (the SDF stand-in).
// The light is projected into the head's horizontal plane: theta = 0 light in front, pi/2 at the side, pi behind.
// Every face pixel gets a coordinate `side` from -1 (far edge of the face, away from the light) to +1 (near edge).
// The shadow boundary sweeps from -1 to +1 as theta goes 0 -> pi, so a frontal light leaves the face clean, a side light
// shades exactly half, and a back light shades all of it. No per-pixel normal is involved: the nose and cheeks never
// cast or catch modelled shading.
float FaceLight(PackSurface s) {
    if (!s.hasHead) return 1.0;
    float lx = dot(s.L, s.headRight);
    float lz = dot(s.L, s.headForward);
    float theta = atan2(abs(lx), lz);                    // 0..pi
    float boundary = lerp(-1.15, 1.15, theta / 3.14159265);
    float side = dot(s.worldPos - s.headPos, s.headRight) / (P_FACE_WIDTH * s.headScale);
    side = clamp(side, -1.0, 1.0) * (lx >= 0.0 ? 1.0 : -1.0);
    return smoothstep(boundary - P_FACE_SOFTNESS, boundary + P_FACE_SOFTNESS, side);
}

PackResult PackShade(PackSurface s) {
    PackResult r;
    r.alpha = s.alpha;
    r.reflectivity = 0.0;
    r.noAo = false;

    // Base colour: the MMD colour model (texture * material colour * light colour), sphere applied as MMD does.
    float3 lit = PackMmdLit(s);
    float3 albedo = saturate(PackAmbient() + PackDiffuse().rgb) * 0.8 * s.tex.rgb;
    const uint sphereMode = PackSphereMode();
    if (sphereMode != 0u) {
        float3 sp = PackSampleSphere(s.N);
        if (sphereMode == 1u) { lit *= sp; albedo *= sp; } else { lit += sp; }
    }

    float term;            // 0 shadow .. 1 light
    float flatFill;        // 1: punctual lights ignore the normal (faces)
    float warmth = 0.0;
    if (s.materialClass == PACK_FACE || s.materialClass == PACK_EYE) {
        term = FaceLight(s) * lerp(1.0, s.shadow, P_FACE_CAST);
        flatFill = 1.0;
        warmth = 0.15;
        r.noAo = true;     // AO would put the nose and eye sockets back
        if (s.materialClass == PACK_EYE) term = lerp(term, 1.0, 0.6);   // eyes stay readable in shadow
    } else {
        // Two-tone ramp on half-Lambert, cut by the cast shadow. Skin gets a softer edge and a warmer shadow.
        float halfLambert = dot(s.N, s.L) * 0.5 + 0.5;
        float soft = P_RAMP_SOFTNESS * (s.materialClass == PACK_SKIN ? 2.0 : 1.0);
        term = smoothstep(P_RAMP_THRESHOLD - soft, P_RAMP_THRESHOLD + soft, halfLambert) * s.shadow;
        flatFill = 0.0;
        if (s.materialClass == PACK_SKIN) warmth = 0.2;
    }

    float3 c = lerp(ShadowColour(lit, warmth), lit, term);

    // Stepped highlight (hair angel ring / glossy trims): the MMD specular, quantised to a band instead of a gradient.
    if (s.materialClass != PACK_FACE && s.materialClass != PACK_EYE && PackSpecularPower() > 0.0) {
        float3 h = normalize(s.L + s.V);
        float spec = pow(saturate(dot(h, s.N)), PackSpecularPower());
        float band = smoothstep(0.45, 0.55, spec) * (s.materialClass == PACK_HAIR ? P_HAIR_HIGHLIGHT : P_HAIR_HIGHLIGHT * 0.4);
        c += band * PackSpecular() * gLightColor * term;
    }

    if (s.materialClass == PACK_EYE) c *= P_EYE_BRIGHTNESS;

    // To linear HDR. Fill light is flat (one ambient colour, no sky/ground gradient), so it adds brightness, not shape.
    float3 albedoLin = SrgbToLinear(saturate(albedo));
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    float3 ambient = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity;
    color += albedoLin * ambient * gHemiStrength * P_AMBIENT;
    color += albedoLin * PunctualDiffuse(s.worldPos, s.N, 1.0, flatFill);

    // Rim light: a thin, hard-edged band on the silhouette, on the lit side (not on the face).
    if (s.materialClass != PACK_FACE && s.materialClass != PACK_EYE) {
        float fres = 1.0 - saturate(dot(s.N, s.V));
        float rim = smoothstep(1.0 - P_RIM_WIDTH, 1.0 - P_RIM_WIDTH + 0.04, fres);
        float lightSide = smoothstep(-0.1, 0.3, dot(s.N, s.L) + 0.2);
        color += gRimColor * rim * lightSide * s.shadow * P_RIM_STRENGTH * gSunIntensity * albedoLin * 2.0;
    }

    r.color = color;
    r.reflectivity = r.noAo ? 0.0 : PackEdgeReflectivity();
    return r;
}
