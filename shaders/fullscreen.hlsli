// Fullscreen triangle + the FullscreenPipeline root layout (see RenderPass.h):
//   b0 SceneConstants, b1 16 root constants, t0..t7 SRV table,
//   s0 point clamp, s1 linear clamp, s2 linear wrap.
#ifndef MMDX_FULLSCREEN_HLSLI
#define MMDX_FULLSCREEN_HLSLI
#include "common.hlsli"

cbuffer PassCB : register(b1) {
    float4 gP0;
    float4 gP1;
    float4 gP2;
    float4 gP3;
};
SamplerState gPoint  : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);

struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FsOut VSFullscreen(uint id : SV_VertexID) {
    FsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
#endif
