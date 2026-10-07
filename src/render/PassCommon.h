#pragma once
// Small helpers shared by the pass implementations (Passes.cpp, Pass*.cpp). Internal to render/.
#include "render/RenderPass.h"
#include <algorithm>
#include <filesystem>
#include <string>

namespace mmdx {


inline constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
inline constexpr D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET;
inline constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
inline constexpr D3D12_RESOURCE_STATES kSrvAll = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;  // NON_PIXEL | PIXEL

// ComputePipeline shaders are cs_6_5 (DXC); PSO creation needs these device caps.
inline bool RtPipelinesSupported(Dx12Context& ctx) {
    return ctx.Caps().raytracingTier >= D3D12_RAYTRACING_TIER_1_1 && ctx.Caps().shaderModel >= D3D_SHADER_MODEL_6_5;
}

inline uint32_t Groups(uint32_t n) { return (n + 7) / 8; }   // 8x8 thread groups
inline uint32_t Half(uint32_t v) { return std::max(1u, v / 2); }

} // namespace mmdx
