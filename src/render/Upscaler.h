#pragma once
// Upscaler abstraction. MVP ships only NullUpscaler (the present pass does a bilinear
// stretch). DLSS (NGX), FSR (FidelityFX SDK) and XeSS implement this interface later;
// they will additionally need motion vectors + jitter from the scene pass.
#include "render/RenderTypes.h"
#include <memory>

namespace mmdx {

class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    virtual UpscalerKind Kind() const = 0;
    virtual const char* DisplayName() const = 0;
    virtual bool IsAvailable() const = 0;
    // Internal render size for a given output size (quality mode decides the ratio).
    virtual void ComputeRenderSize(uint32_t outW, uint32_t outH, float renderScale,
                                   uint32_t& renderW, uint32_t& renderH) const = 0;
};

class NullUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::None; }
    const char* DisplayName() const override { return "None (bilinear)"; }
    bool IsAvailable() const override { return true; }
    void ComputeRenderSize(uint32_t outW, uint32_t outH, float renderScale,
                           uint32_t& renderW, uint32_t& renderH) const override {
        renderW = outW * renderScale < 16 ? 16u : (uint32_t)(outW * renderScale);
        renderH = outH * renderScale < 16 ? 16u : (uint32_t)(outH * renderScale);
    }
};

// Returns NullUpscaler for every kind until real integrations exist.
std::unique_ptr<IUpscaler> CreateUpscaler(UpscalerKind kind);

} // namespace mmdx
