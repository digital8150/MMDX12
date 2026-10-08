// Shader pack effect template (API 3). Implement PackEffect; it runs once per pixel over the whole screen, in the
// order of the user's effect stack. Save this file while the app runs and the effect reloads immediately.
//
// pack.json "stage": "post" = after tonemap, display-referred sRGB (grain, scanlines, lens fringes ...)
//                    "pre-bloom" = linear HDR before the bloom (glow-friendly, exposure-aware effects)
//
// PackEffectInput i:  i.uv (0..1, top left = 0,0)   i.pixel   i.outputSize   i.time (seconds)   i.frameIndex
//                     i.color (the frame so far, alpha kept)   i.depth (raw device depth, 1 = background)
//                     i.normal (view space; garbage on the background)   i.motion (uv motion vector)
// Neighbouring pixels: gEffectSource.SampleLevel(gLinear, uv, 0).   Parameters: PackParam(0..15) = the sliders in the
// pack.json "params" order.   Textures (pack.json "textures"): PackFxSampleTex(index, uv), PackFxSampleTexLevel,
// PackFxTexSize, PackFxTexCount.   Helpers from common.hlsli: Luminance, LinearZ(depth), Ign, SrgbToLinear ...
// Note: `line` is a reserved word in HLSL; do not use it as a variable name.
float3 PackEffect(PackEffectInput i) {
    // example: a soft vignette whose strength is the "amount" slider
    const float2 d = i.uv - 0.5;
    const float vignette = 1.0 - PackParam(0) * dot(d, d) * 1.6;
    return i.color.rgb * vignette;
}
