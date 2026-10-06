// pack_template: the starting point for your own shader pack (returns it with "새 팩 만들기" in the shader tab).
//
// Edit this file in place (or in build/bin/shaders/packs/<id> for a quick test) and press "팩 다시 불러오기" in the
// app: the pack recompiles on its next draw. The full contract is in shaders/pack_api.hlsli; the guide lives at
// https://mmdx.codingbot.kr/en/docs/shader-packs/ (pack.json fields, PackSurface / PackResult, how it compiles).
//
// A pack implements one function, which replaces the default MMD shading of the scene pass for the whole model:
//
//     PackResult PackShade(PackSurface s);
//
// What is below: the MMD colour model (PackMmdLit: (ambient + diffuse * light) * texture), one hard N.L step for
// the body, and a flat face (faces never shade from the normal). Everything in mmd.hlsl / common.hlsli is visible
// (gLightColor, gSunIntensity, SrgbToLinear, PunctualDiffuse, ...), but only the names in pack_api.hlsli are stable.

// ---- parameters (pack.json "params", same order) ---------------------------------------------------------------------
#define P_MY_PARAM PackParam(0)   // your first slider (0..1); add more #defines in pack.json order

PackResult PackShade(PackSurface s) {
    PackResult r;
    r.alpha = s.alpha;
    r.reflectivity = PackEdgeReflectivity();   // SSR / RT reflection strength (0..1)
    r.noAo = false;

    // The MMD colour model: saturate(ambient + diffuse * light) * texture, gamma space.
    float3 lit = PackMmdLit(s);

    if (s.materialClass == PACK_FACE) {
        // Faces stay flat: no N.L shading (the nose and cheeks would catch it), AO off so eye sockets stay clean.
        r.noAo = true;
        r.reflectivity = 0.0;
        r.color = SrgbToLinear(saturate(lit)) * gSunIntensity;
        return r;
    }

    // One hard step on the half-Lambert terminator: the body becomes two flat tones.
    float halfLambert = dot(s.N, s.L) * 0.5 + 0.5;          // 0 shadow .. 1 light
    float step = halfLambert > P_MY_PARAM ? 1.0 : 0.35;      // hard edge; no smoothstep yet
    float3 c = lit * step;

    // To linear HDR radiance; punctual (point/spot) lights still shade.
    r.color = SrgbToLinear(saturate(c)) * gSunIntensity;
    r.color += SrgbToLinear(saturate(lit)) * PunctualDiffuse(s.worldPos, s.N, 1.0, 1.0);
    return r;
}
