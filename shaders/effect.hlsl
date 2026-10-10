// The whole-screen effect pass's shader (PackEffectPass). Compiled per effect pack with the pack's effect.hlsl
// appended: MMDX_PACK is the quoted include path, exactly as mmd.hlsl does for the surface packs. The pack
// implements PackEffect(PackEffectInput) (contract: effect_api.hlsli).
#include "fullscreen.hlsli"
#include "effect_api.hlsli"
#include MMDX_PACK

float4 PSEffect(FsOut i) : SV_Target {
    PackEffectInput In;
    In.uv = i.uv;
    In.pixel = i.pos.xy;
    In.outputSize = gEffectOutputSize;
    In.time = gEffectTime;
    In.frameIndex = gEffectFrameIndex;
    In.color = gEffectSource.SampleLevel(gPoint, i.uv, 0);
    In.depth = gEffectDepth.SampleLevel(gPoint, i.uv, 0);
    In.motion = gEffectVelocity.SampleLevel(gPoint, i.uv, 0);
    In.normal = OctDecode(gEffectNormal.SampleLevel(gPoint, i.uv, 0).xy);
    In.dt = gP0.z;
    return float4(PackEffect(In), In.color.a);
}

#if PACK_STATE
float4 PSEffectState(FsOut i) : SV_Target {
    PackStateInput In;
    In.reset = (gP0.w != 0.0);
    In.dt = gP0.z;
    In.time = gEffectTime;
    In.frameIndex = gEffectFrameIndex;
    In.outputSize = gEffectOutputSize;
    [unroll]
    for (int k = 0; k < 16; ++k) {
        In.prevState[k] = In.reset ? 0.0 : PackState(k);
    }
    float newState[16];
    [unroll]
    for (int j = 0; j < 16; ++j) newState[j] = 0.0;
    PackEffectState(In, newState);
#if defined(PACK_STATE_FLOATS)
    [unroll]
    for (int m = PACK_STATE_FLOATS; m < 16; ++m) {
        newState[m] = 0.0;
    }
#endif
    uint px = min((uint)i.pos.x, 3u);
    uint base = px * 4;
    return float4(newState[base], newState[base + 1], newState[base + 2], newState[base + 3]);
}
#endif

#ifdef PACK_PASS_ENTRY
// One of the pack's intermediate passes (pack.json "passes", API v5): PACK_PASS_ENTRY is its function, the result is
// stored in the pass's own RGBA16F target (gP2.xy = its size, gP2.z = its index).
float4 PSPackPass(FsOut i) : SV_Target {
    PackPassInput In;
    In.uv = i.uv;
    In.pixel = i.pos.xy;
    In.size = gP2.xy;
    In.outputSize = gEffectOutputSize;
    In.time = gEffectTime;
    In.frameIndex = gEffectFrameIndex;
    In.dt = gP0.z;
    In.pass = (uint)gP2.z;
    return PACK_PASS_ENTRY(In);
}
#endif
