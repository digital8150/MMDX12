#include "render/Upscaler.h"
#include <algorithm>
#include <cmath>

namespace mmdx {

std::unique_ptr<IUpscaler> CreateDlssUpscaler();   // UpscalerDlss.cpp
std::unique_ptr<IUpscaler> CreateFsrUpscaler();    // UpscalerFsr.cpp
std::unique_ptr<IUpscaler> CreateXessUpscaler();   // UpscalerXess.cpp

float IUpscaler::Ratio(UpscalerQuality q) {
    switch (q) {
    case UpscalerQuality::NativeAA: return 1.0f;
    case UpscalerQuality::Quality: return 1.5f;
    case UpscalerQuality::Balanced: return 1.7f;
    case UpscalerQuality::Performance: return 2.0f;
    case UpscalerQuality::UltraPerformance: return 3.0f;
    }
    return 1.0f;
}

void IUpscaler::ComputeRenderSize(uint32_t outW, uint32_t outH, UpscalerQuality quality,
                                  uint32_t& renderW, uint32_t& renderH) {
    renderW = std::max(16u, (uint32_t)std::lround(outW / Ratio(quality)));
    renderH = std::max(16u, (uint32_t)std::lround(outH / Ratio(quality)));
}

uint32_t IUpscaler::JitterPhaseCount(uint32_t renderW, uint32_t outW) {
    float r = (float)outW / std::max(1u, renderW);
    return std::max(8u, (uint32_t)std::ceil(8.0f * r * r));
}

namespace {

class NullUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::None; }
    const char* DisplayName() const override { return "None"; }
    bool Initialize(Dx12Context&) override { return true; }
    void Shutdown() override {}
    bool IsAvailable() const override { return true; }
    bool Evaluate(Dx12Context&, const UpscaleInputs&) override { return false; }
};

} // namespace

std::unique_ptr<IUpscaler> CreateUpscaler(UpscalerKind kind) {
    switch (kind) {
    case UpscalerKind::DLSS: return CreateDlssUpscaler();
    case UpscalerKind::FSR: return CreateFsrUpscaler();
    case UpscalerKind::XeSS: return CreateXessUpscaler();
    case UpscalerKind::None:
    default: return std::make_unique<NullUpscaler>();
    }
}

const char* UpscalerKindName(UpscalerKind kind) {
    switch (kind) {
    case UpscalerKind::DLSS: return "DLSS";
    case UpscalerKind::FSR: return "FSR";
    case UpscalerKind::XeSS: return "XeSS";
    case UpscalerKind::None:
    default: return "None";
    }
}

} // namespace mmdx
