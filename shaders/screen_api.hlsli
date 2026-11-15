// Shader pack SCREEN API (docs/shader_screen_api.md). Included by screen.hlsl, which the app compiles per "type":
// "screen" pack with the pack's own screen.hlsl appended (MMDX_PACK), like effect.hlsl does for effect packs. A pack
// implements one function:
//
//     float4 PackScreen(PackScreenInput i);
//
// It draws the content of one MMDX stage screen (docs/stage_format.md `screens`, source "program") into the screen's
// texture, once per frame before the scene passes. The result is LINEAR RGB (alpha is ignored); the pass sRGB-encodes
// it, and the screen's own `brightness` is applied by the material that shows the texture, not here.
#ifndef SCREEN_API_HLSLI
#define SCREEN_API_HLSLI

// Must equal kScreenApiVersion (render/ShaderPack.h).
#define SCREEN_API_VERSION 1

struct PackScreenInput {
    float2 uv;          // 0..1 across the screen, (0, 0) at the top left
    float2 size;        // the screen texture size in pixels (the manifest's width / height, capped at 2048)
    float time;         // seconds on the song / timeline clock (the same value an offline render uses: exact)
    float frameIndex;   // frame counter, wraps at 64
};

// The pack's parameters (pack.json "params"), the manifest `packs[].params` initial values with the stage's
// `packs.<index>.params.<key>` binding targets applied on top. Index = pack.json order; the typed accessors follow the
// effect API's (a float / int / bool in .x, a colour in .rgba converted sRGB -> linear, a vec3 in .xyz).
cbuffer PackScreenParams : register(b2) { float4 gPackScreenParams[4]; }
#define PackParam(i) (gPackScreenParams[(i) >> 2][(i) & 3])
inline int PackParamInt(uint i) { return (int)round(gPackScreenParams[(i) >> 2][(i) & 3]); }
inline bool PackParamBool(uint i) { return gPackScreenParams[(i) >> 2][(i) & 3] > 0.5; }
inline float3 PackParamColor(uint i) { return gPackScreenParams[i >> 2].rgb; }
inline float4 PackParamColor4(uint i) { return gPackScreenParams[i >> 2]; }
inline float3 PackParamVec3(uint i) { return gPackScreenParams[i >> 2].xyz; }

// The pack's "textures" (pack.json order, at most kPackMaxTextures). Indices are 0-based; out-of-range and missing
// textures sample as white. sRGB textures (the default) sample as LINEAR values (do not apply SrgbToLinear again),
// "srgb": false ones return the stored values.
#ifndef PACK_TEX_COUNT
#define PACK_TEX_COUNT 0
#endif
#ifndef PACK_TEX_CLAMP_MASK
#define PACK_TEX_CLAMP_MASK 0u
#endif

Texture2D gPackTex[16] : register(t0, space5);

float4 PackSampleTex(uint i, float2 uv) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    return ((PACK_TEX_CLAMP_MASK >> i) & 1u) ? gPackTex[i].Sample(gLinear, uv) : gPackTex[i].Sample(gLinearWrap, uv);
}

float4 PackSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 15u;
    return ((PACK_TEX_CLAMP_MASK >> i) & 1u) ? gPackTex[i].SampleLevel(gLinear, uv, lod)
                                             : gPackTex[i].SampleLevel(gLinearWrap, uv, lod);
}

uint2 PackTexSize(uint i) {
    if (i >= (uint)PACK_TEX_COUNT) return uint2(0, 0);
    uint2 s;
    gPackTex[i & 15u].GetDimensions(s.x, s.y);
    return s;
}

uint PackTexCount() { return (uint)PACK_TEX_COUNT; }

// The audio inputs ("requires": ["inputs.audio"], PACK_CAP_INPUTS_AUDIO): PackAudioBeat / Downbeat / Onset / Energy /
// Section / Bpm / BeatCount and PackInputValid(PACK_IN_AUDIO), the same values the scene's packs read. The pass has no
// scene constants: the host passes them in the pass constants gP1 / gP2 (StageScreens), so pack_inputs.hlsli's
// gPackAudio0 / 1 are mapped onto those (after common.hlsli declared the scene's own, which stay unbound here).
#define gPackAudio0 gP1
#define gPackAudio1 gP2
#include "pack_inputs.hlsli"

#endif
