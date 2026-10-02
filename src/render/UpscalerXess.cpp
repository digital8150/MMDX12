// Intel XeSS 3.x (delay-loaded libxess.dll next to the exe; never touch xess* unless it was found).
// d3dx12.h first so <d3d12.h> (Upscaler.h) resolves to the vendored DirectX-Headers, whose
// D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT d3dx12_core.h requires.
#include <directx/d3dx12.h>
#include "render/Upscaler.h"
#include "render/Dx12Context.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <xess/xess_d3d12.h>
#include <filesystem>

namespace mmdx {

namespace {

class XessUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::XeSS; }
    const char* DisplayName() const override { return "Intel XeSS"; }
    bool IsAvailable() const override { return available_; }

    bool Initialize(Dx12Context& ctx) override {
        // libxess.dll is delay-loaded: calling any xess* function with the DLL missing throws.
        if (!std::filesystem::exists(ExecutableDir() / L"libxess.dll")) {
            LOG_WARN("XeSS: libxess.dll not found next to the exe");
            return false;
        }
        dllPresent_ = true;
        xess_result_t r = xessD3D12CreateContext(ctx.Device(), &context_);
        if (r != XESS_RESULT_SUCCESS) {
            LOG_WARN("XeSS: context creation failed (%d)", (int)r);
            context_ = nullptr;
            return false;
        }
        xessForceLegacyScaleFactors(context_, true);  // quality presets match IUpscaler::Ratio
        available_ = true;
        return true;
    }

    bool Evaluate(Dx12Context& ctx, const UpscaleInputs& in) override {
        if (!available_ || !context_) return false;
        // XeSS supports re-calling xessD3D12Init on the same context.
        bool needsInit = !initialised_ || in.renderWidth != curRenderW_ || in.renderHeight != curRenderH_ ||
                         in.outputWidth != curOutW_ || in.outputHeight != curOutH_ || in.quality != curQuality_;
        if (needsInit) {
            if (initialised_) ctx.WaitForGpu();
            xess_d3d12_init_params_t ip{};
            ip.outputResolution = {in.outputWidth, in.outputHeight};
            switch (in.quality) {
            case UpscalerQuality::NativeAA: ip.qualitySetting = XESS_QUALITY_SETTING_AA; break;
            case UpscalerQuality::Quality: ip.qualitySetting = XESS_QUALITY_SETTING_QUALITY; break;
            case UpscalerQuality::Balanced: ip.qualitySetting = XESS_QUALITY_SETTING_BALANCED; break;
            case UpscalerQuality::Performance: ip.qualitySetting = XESS_QUALITY_SETTING_PERFORMANCE; break;
            case UpscalerQuality::UltraPerformance: ip.qualitySetting = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; break;
            }
            ip.initFlags = XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
            ip.creationNodeMask = 1;
            ip.visibleNodeMask = 1;
            xess_result_t r = xessD3D12Init(context_, &ip);
            if (r != XESS_RESULT_SUCCESS) {
                if (!loggedFailure_) {
                    LOG_WARN("XeSS: init failed (%d)", (int)r);
                    loggedFailure_ = true;
                }
                return false;
            }
            xessSetVelocityScale(context_, -(float)in.renderWidth, -(float)in.renderHeight);
            loggedFailure_ = false;
            initialised_ = true;
            curRenderW_ = in.renderWidth;
            curRenderH_ = in.renderHeight;
            curOutW_ = in.outputWidth;
            curOutH_ = in.outputHeight;
            curQuality_ = in.quality;
        }
        xess_d3d12_execute_params_t ep{};
        ep.pColorTexture = in.color;
        ep.pVelocityTexture = in.velocity;
        ep.pDepthTexture = in.depth;
        ep.pOutputTexture = in.output;
        ep.jitterOffsetX = in.jitterX;
        ep.jitterOffsetY = in.jitterY;
        ep.exposureScale = 1.0f;
        ep.resetHistory = in.reset ? 1 : 0;
        ep.inputWidth = in.renderWidth;
        ep.inputHeight = in.renderHeight;
        xess_result_t r = xessD3D12Execute(context_, in.cmd, &ep);
        if (r != XESS_RESULT_SUCCESS) {
            if (!loggedFailure_) {
                LOG_WARN("XeSS: execute failed (%d)", (int)r);
                loggedFailure_ = true;
            }
            return false;
        }
        loggedFailure_ = false;
        return true;
    }

    void Shutdown() override {
        if (context_) {
            xessDestroyContext(context_);
            context_ = nullptr;
        }
        initialised_ = false;
        dllPresent_ = false;
        curRenderW_ = curRenderH_ = curOutW_ = curOutH_ = 0;
        loggedFailure_ = false;
        available_ = false;
    }

private:
    xess_context_handle_t context_ = nullptr;
    bool initialised_ = false;
    bool dllPresent_ = false;
    uint32_t curRenderW_ = 0, curRenderH_ = 0, curOutW_ = 0, curOutH_ = 0;
    UpscalerQuality curQuality_ = UpscalerQuality::Quality;
    bool available_ = false;
    bool loggedFailure_ = false;
};

} // namespace

std::unique_ptr<IUpscaler> CreateXessUpscaler() {
    return std::make_unique<XessUpscaler>();
}

} // namespace mmdx
