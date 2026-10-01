#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace mmdx {

class GpuModel;

// Future: DLSS / FSR / XeSS plug in through IUpscaler (see Upscaler.h). MVP: None.
enum class UpscalerKind : uint8_t { None = 0, DLSS, FSR, XeSS };

struct RenderSettings {
    uint32_t msaaSamples = 4;      // 1/2/4/8, clamped to what the device supports
    float renderScale = 1.0f;      // internal resolution = output size * scale (0.25..2.0)
    bool fixedResolution = false;  // benchmark: render at exactly fixedWidth x fixedHeight
    uint32_t fixedWidth = 1920, fixedHeight = 1080;
    bool vsync = true;
    bool drawEdges = true;
    UpscalerKind upscaler = UpscalerKind::None;
    DirectX::XMFLOAT4 clearColor{0.035f, 0.04f, 0.07f, 1.0f};
};

struct CameraParams {
    DirectX::XMFLOAT4X4 view{};   // LH view matrix (row-vector convention)
    DirectX::XMFLOAT3 eye{};
    float fovYRadians = DirectX::XMConvertToRadians(30.0f);
    float nearZ = 0.5f, farZ = 3000.0f;  // projection aspect comes from the internal render target
};

struct LightParams {
    DirectX::XMFLOAT3 direction{-0.5f, -1.0f, 0.5f};  // MMD default light direction (toward scene)
    DirectX::XMFLOAT3 color{0.6f, 0.6f, 0.6f};        // MMD default light color (154/255)
};

struct FrameView {
    CameraParams camera;
    LightParams light;
    std::vector<GpuModel*> models;  // drawn in this order (stage parts first, then characters)
};

struct RenderStats {
    uint32_t internalWidth = 0, internalHeight = 0;
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
    float gpuFrameMs = 0;  // from timestamp queries, ~kFramesInFlight frames old
};

} // namespace mmdx
