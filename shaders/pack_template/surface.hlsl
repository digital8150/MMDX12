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

// ---- pack textures (pack.json "textures", optional) ------------------------------------------------------------------
// Declare extra textures in pack.json and sample them with the pack texture API:
//
//     "textures": [
//         { "file": "textures/ramp.png", "address": "clamp" },
//         { "file": "textures/tint.png" }
//     ]
//
//     PackSampleTex(i, uv)             i-th texture with its declared address mode (wrap default)
//     PackSampleTexLevel(i, uv, lod)   explicit LOD (ramps and data maps: lod 0)
//     PackTexSize(i) / PackTexCount()  level-0 size in pixels / declared count
//
// sRGB textures (default) sample as LINEAR values - do not apply SrgbToLinear again; "srgb": false
// returns the stored values (data maps: light maps, face SDFs, LUTs). Missing textures sample white.

// ---- pack edges (optional) -------------------------------------------------------------------------------------------
// A pack can also draw the model's outline: define PACK_HAS_EDGE at the top of this file...
// #define PACK_HAS_EDGE 1
// ...and implement the one function below. The engine then draws this model's outlines with a pack
// variant of the edge pass (colour from e.color, width from the MMD size * e.widthScale); without
// PACK_HAS_EDGE the edges are exactly the app's default.
//
//     PackEdgeResult PackEdge(uint materialClass, float4 mmdEdgeColor, float mmdEdgeSize) {
//         PackEdgeResult e;
//         e.color = mmdEdgeColor;       // gamma space RGBA, like the MMD edge colour
//         e.widthScale = 1.5;           // multiply the MMD outline width in pixels
//         return e;
//     }

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
