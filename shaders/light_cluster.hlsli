// S7c: the fragment's clustered light list (Forward+ with depth slices). The cull pass (shaders/light_cull.hlsl) has
// already tested every light's bounding sphere against this froxel; the scene shaders iterate the list instead of every
// light. The lists are in ascending light order, so a fragment's per-light sums are accumulated in the same order as the
// old full loop (identical image).
//
// Requires common.hlsli (SceneConstants) and a bindless raw ByteAddressBuffer table: include this AFTER pbr_common.hlsli,
// whose gPbrBuf is the raster root signature's bufAll table (t0 space3) and maps onto rt_common.hlsli's gBindlessBuf.
//
// Packing (src/render/ShaderInterop.h kCluster*, Renderer::RecordScene):
//   gLightCluster.x = SrvHeap index of this frame slot's cluster block (negative = no clusters: loop over every light)
//   gLightCluster.y = (tilesX & 0xFF) | (tilesY & 0xFF) << 8 | (slices & 0xFF) << 16 (0 = none)
//   gLightCluster.zw = log-slice scale / bias: slice = clamp(floor(log(max(z, nearZ)) * scale + bias), 0, slices - 1)
// Cluster c starts at c * (1 + kClusterMaxLights) words: word 0 = count (0xFFFF = overflow: loop over every light),
// words 1..count = light indices. Order: (slice * tilesY + ty) * tilesX + tx (tx = 0 left, ty = 0 top).
#ifndef MMDX_LIGHT_CLUSTER_HLSLI
#define MMDX_LIGHT_CLUSTER_HLSLI

#define kClusterMaxLights 64u
#define kClusterOverflow 0xFFFFu

// The froxel cluster of a fragment at view-space position `vp` (world position x gView). `beyondGrid` (returned) marks a
// fragment past the grid's farthest slice (view depth > min(farZ, 1000)): its cluster was only culled up to the far cap,
// so the caller must use the full light list instead.
uint LightClusterIndex(float3 vp, out bool beyondGrid) {
    beyondGrid = false;
    const uint tilesX = ((uint)gLightCluster.y) & 0xFFu;
    const uint tilesY = (((uint)gLightCluster.y) >> 8) & 0xFFu;
    const uint slices = (((uint)gLightCluster.y) >> 16) & 0xFFu;
    if (tilesX == 0u || tilesY == 0u || slices == 0u) {
        beyondGrid = true;
        return 0u;
    }
    // the tile from the projection of the fragment's own view-space position. `vp` is already in view space, so it
    // takes gProj, not a view-projection matrix (that applied the view twice: fragments read another tile's list and
    // lost lights in hard-edged patches). gProj carries the sub-pixel TAA jitter, which the cull's AABB margin absorbs.
    const float4 clip = mul(float4(vp, 1.0), gProj);
    const float2 ndc = clip.w > 1e-6 ? clip.xy / clip.w : float2(0.0, 0.0);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const uint tx = min((uint)(saturate(uv.x) * (float)tilesX), tilesX - 1u);
    const uint ty = min((uint)(saturate(uv.y) * (float)tilesY), tilesY - 1u);
    const float s = log(max(vp.z, max(gNearZ, 0.01))) * gLightCluster.z + gLightCluster.w;
    beyondGrid = s > (float)slices;   // past the culling range: the cluster lists do not cover it
    const uint slice = (uint)clamp(floor(s), 0.0, (float)(slices - 1u));
    return (slice * tilesY + ty) * tilesX + tx;
}

// The fragment's light list (word index of its first light / its length). `all` = the shaders must loop over every
// light: no clusters this frame, or a cluster whose list overflowed. Otherwise `count` <= gNumLights.
struct LightList {
    uint first;   // word index of the first light index in the cluster buffer
    uint count;   // number of light indices
    bool all;     // loop over every light instead (light index = the loop counter)
};

LightList LightListFor(float3 vp) {
    LightList list;
    list.first = 0u;
    list.count = (uint)gNumLights;
    list.all = true;
    const int srv = (int)gLightCluster.x;
    if (srv < 0 || gLightCluster.y <= 0.0) return list;
    bool beyondGrid = false;
    const uint base = LightClusterIndex(vp, beyondGrid) * (1u + kClusterMaxLights);
    if (beyondGrid) return list;   // past the culling range: every light, in order
    ByteAddressBuffer clusters = gPbrBuf[NonUniformResourceIndex(SafeSrv(srv))];
    const uint count = clusters.Load(base * 4u);
    if (count >= kClusterOverflow) return list;   // overflowed: every light, in order
    list.first = base + 1u;
    list.count = min(count, (uint)gNumLights);
    list.all = false;
    return list;
}

// The loop counter -> light index (`k` = the loop counter of the fragment's list).
uint LightListAt(LightList list, uint k) {
    if (list.all) return k;
    return gPbrBuf[NonUniformResourceIndex(SafeSrv(gLightCluster.x))].Load((list.first + k) * 4u);
}

#endif
