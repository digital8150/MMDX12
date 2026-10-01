#include "render/Upscaler.h"

namespace mmdx {

std::unique_ptr<IUpscaler> CreateUpscaler(UpscalerKind kind) {
    // Real DLSS / FSR / XeSS integrations plug in here later.
    return std::make_unique<NullUpscaler>();
}

} // namespace mmdx
