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
    return float4(PackEffect(In), In.color.a);
}
