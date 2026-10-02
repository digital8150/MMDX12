#pragma once
// Temporal upscalers: DLSS (NGX), FSR (FidelityFX API) and XeSS behind one interface. Each
// one loads its runtime DLL next to the exe (nvngx_dlss.dll, amd_fidelityfx_loader_dx12.dll +
// amd_fidelityfx_upscaler_dx12.dll, libxess.dll); a missing DLL or unsupported GPU only makes
// that upscaler unavailable. UpscalePass (Passes.h) runs the active one after the composite.
#include "render/RenderTypes.h"
#include <directx/d3d12.h>
#include <memory>

namespace mmdx {

class Dx12Context;

struct UpscaleInputs {
    ID3D12GraphicsCommandList* cmd = nullptr;
    // Render resolution, state D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE:
    ID3D12Resource* color = nullptr;     // RGBA16F linear HDR (pre-exposure 1)
    ID3D12Resource* depth = nullptr;     // R32_FLOAT raw device depth (0 near .. 1 far, not inverted)
    ID3D12Resource* velocity = nullptr;  // R16G16_FLOAT, uv(current) - uv(previous), unjittered
    // Output resolution, RGBA16F, state D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
    ID3D12Resource* output = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    UpscalerQuality quality = UpscalerQuality::Quality;
    // Sub-pixel jitter in render pixels. The projection was offset by
    // (+2 * jitterX / renderWidth, -2 * jitterY / renderHeight) in NDC (FSR convention).
    float jitterX = 0, jitterY = 0;
    bool reset = false;          // camera cut / first frame: drop history
    float frameTimeMs = 16.7f;
    float nearZ = 0.5f, farZ = 3000.0f, fovY = 0.5f;  // radians
};

class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual UpscalerKind Kind() const = 0;
    virtual const char* DisplayName() const = 0;
    // Loads the runtime and checks GPU support. Called once by the Renderer; returns false (and
    // logs why) when unavailable. Must not throw or crash when the DLL is missing.
    virtual bool Initialize(Dx12Context& ctx) = 0;
    // Releases the feature/context and the DLL. The GPU must be idle (Renderer waits).
    virtual void Shutdown() = 0;
    virtual bool IsAvailable() const = 0;
    // Records the upscale into in.cmd. Creates or recreates the internal feature/context when
    // the render size, output size or quality changed (waiting for the GPU first if needed).
    // The SDKs change descriptor heaps and root signatures: the caller rebinds its heap after.
    virtual bool Evaluate(Dx12Context& ctx, const UpscaleInputs& in) = 0;

    // Render size for an output size: out / ratio(quality), rounded, at least 16.
    static void ComputeRenderSize(uint32_t outW, uint32_t outH, UpscalerQuality quality,
                                  uint32_t& renderW, uint32_t& renderH);
    static float Ratio(UpscalerQuality quality);  // 1.0, 1.5, 1.7, 2.0, 3.0
    // Jitter sequence length: ceil(8 * (outW / renderW)^2), at least 8.
    static uint32_t JitterPhaseCount(uint32_t renderW, uint32_t outW);
};

// Never null. None -> an always-available pass-through that is never evaluated.
std::unique_ptr<IUpscaler> CreateUpscaler(UpscalerKind kind);
const char* UpscalerKindName(UpscalerKind kind);  // "None", "DLSS", "FSR", "XeSS"

} // namespace mmdx
