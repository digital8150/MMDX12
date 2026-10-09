// Offline GI outline layer, shader-pack variant: the camera of offline_edge.hlsl (shutter time, lens) with the pack's
// PackEdge colour and width scale, as mmd.hlsl's VSEdge / PSEdgePack do for the real-time edge pass. Compiled with DXC
// per pack that sets PACK_HAS_EDGE (MMDX_PACK = the quoted surface.hlsl path), by OfflineRenderer.
// Root signature: offline_edge.hlsl's, plus s1 clamp and the pack texture table (t0, space5).
#define MMDX_NO_SHADOW_PASS   // mmd.hlsl's ShadowCB is b2, EdgeCB here
#include "mmd.hlsl"
#include "offline_common.hlsli"
#include "offline_edge_skin.hlsli"

#if PACK_HAS_EDGE
float4 VSEdgeOfflinePack(VSIn v) : SV_Position {
    float3 wp, wn;
    SkinAt(v, wp, wn);
    float px = gEdgeSize * v.edge * gEdgeScale;   // outline width in pixels (scaled with height / 1080)
    px *= max(PackEdge(gPackClass, gEdgeColor, gEdgeSize).widthScale, 0.0);
    return OfflineEdgeClip(wp, wn, px);
}

// Premultiplied output (blend ONE / INV_SRC_ALPHA), the colour of mmd.hlsl PSEdgePack.
float4 PSEdgeOfflinePack(float4 pos : SV_Position) : SV_Target {
    PackEdgeResult e = PackEdge(gPackClass, gEdgeColor, gEdgeSize);
    float a = saturate(e.color.a);
    return float4(SrgbToLinear(saturate(e.color.rgb)) * gSunIntensity * 0.85 * a, a);
}
#endif
