// AMD FSR 3.x via the FidelityFX API (runtime-loaded amd_fidelityfx_loader_dx12.dll + the effect DLLs).
// _WINDOWS must be defined before ffx_api_loader.h includes <windows.h>.
#ifndef _WINDOWS
#define _WINDOWS
#endif
// d3dx12.h first so <d3d12.h> (Upscaler.h, ffx_api_dx12.h) resolves to the vendored
// DirectX-Headers, whose D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT d3dx12_core.h requires.
#include <directx/d3dx12.h>
#include "render/Upscaler.h"
#include "render/Dx12Context.h"
#include "core/Log.h"
#include "core/TextUtil.h"
// ffx_upscale.h (in upscalers/include) pulls in the api/ tree via relative includes; those
// resolve now that the SDK uses its canonical two-root layout.
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_api_loader.h>
#include <filesystem>
#include <windows.h>

namespace mmdx {

namespace {

// The SDK hands us wide messages; the project log is UTF-8.
void FsrMessage(uint32_t type, const wchar_t* message) {
    if (message && type == FFX_API_MESSAGE_TYPE_WARNING) LOG_WARN("FSR: %s", WideToUtf8(message).c_str());
}

class FsrUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::FSR; }
    const char* DisplayName() const override { return "AMD FSR"; }
    bool IsAvailable() const override { return available_; }

    bool Initialize(Dx12Context& ctx) override {
        module_ = LoadLibraryW((ExecutableDir() / L"amd_fidelityfx_loader_dx12.dll").c_str());
        if (!module_) {
            LOG_WARN("FSR: amd_fidelityfx_loader_dx12.dll not found next to the exe");
            return false;
        }
        ffxLoadFunctions(&fns_, module_);
        if (!fns_.CreateContext || !fns_.DestroyContext || !fns_.Configure || !fns_.Query || !fns_.Dispatch) {
            LOG_WARN("FSR: loader DLL is missing exports");
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }
        device_ = ctx.Device();
        // Probe: make sure a context can actually be created (effect DLL present, GPU supported).
        if (!CreateContext(960, 540, 1920, 1080)) {
            LOG_WARN("FSR: context creation failed (%d)", (int)lastResult_);
            fns_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
            Shutdown();
            return false;
        }
        if (fns_.DestroyContext(&context_, nullptr) != FFX_API_RETURN_OK)
            LOG_WARN("FSR: probe context destroy failed (%d)", (int)lastResult_);
        context_ = nullptr;
        available_ = true;
        return true;
    }

    bool Evaluate(Dx12Context& ctx, const UpscaleInputs& in) override {
        if (!available_) return false;
        if (context_ && (in.renderWidth != curRenderW_ || in.renderHeight != curRenderH_ ||
                         in.outputWidth != curOutW_ || in.outputHeight != curOutH_ ||
                         in.quality != curQuality_)) {
            ctx.WaitForGpu();
            fns_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
        }
        if (!context_) {
            if (!CreateContext(in.renderWidth, in.renderHeight, in.outputWidth, in.outputHeight)) {
                LOG_ERROR("FSR: context creation failed (%d)", (int)lastResult_);
                context_ = nullptr;
                if (!loggedFailure_) {
                    LOG_WARN("FSR: evaluate unavailable, will not retry until the parameters change");
                    loggedFailure_ = true;
                }
                return false;
            }
            loggedFailure_ = false;
            curRenderW_ = in.renderWidth;
            curRenderH_ = in.renderHeight;
            curOutW_ = in.outputWidth;
            curOutH_ = in.outputHeight;
            curQuality_ = in.quality;
        }
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = in.cmd;
        d.color = ffxApiGetResourceDX12(in.color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(in.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(in.velocity, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        // exposure / reactive / transparencyAndComposition left zero-initialised = none
        d.jitterOffset = {in.jitterX, in.jitterY};
        d.motionVectorScale = {-(float)in.renderWidth, -(float)in.renderHeight};
        d.renderSize = {in.renderWidth, in.renderHeight};
        d.upscaleSize = {in.outputWidth, in.outputHeight};
        d.enableSharpening = false;
        d.sharpness = 0;
        d.frameTimeDelta = in.frameTimeMs;
        d.preExposure = 1.0f;
        d.reset = in.reset;
        d.cameraNear = in.nearZ;
        d.cameraFar = in.farZ;
        d.cameraFovAngleVertical = in.fovY;
        d.viewSpaceToMetersFactor = 0.08f;  // 1 MMD unit ~ 8 cm
        d.flags = 0;
        lastResult_ = fns_.Dispatch(&context_, &d.header);
        if (lastResult_ != FFX_API_RETURN_OK) {
            if (!loggedFailure_) {
                LOG_WARN("FSR: dispatch failed (%d)", (int)lastResult_);
                loggedFailure_ = true;
            }
            return false;
        }
        loggedFailure_ = false;
        return true;
    }

    void Shutdown() override {
        if (context_ && fns_.DestroyContext) {
            fns_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
        }
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        fns_ = {};
        device_ = nullptr;
        curRenderW_ = curRenderH_ = curOutW_ = curOutH_ = 0;
        loggedFailure_ = false;
        available_ = false;
    }

private:
    // maxRenderSize is the current render size (we recreate on change); quality only
    // changes the render size computed by the caller.
    bool CreateContext(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device_;
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &backend.header;
        cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        cd.maxRenderSize = {rw, rh};
        cd.maxUpscaleSize = {ow, oh};
        cd.fpMessage = &FsrMessage;
        lastResult_ = fns_.CreateContext(&context_, &cd.header, nullptr);
        return lastResult_ == FFX_API_RETURN_OK;
    }

    ID3D12Device* device_ = nullptr;
    HMODULE module_ = nullptr;
    ffxFunctions fns_{};
    ffxContext context_ = nullptr;
    uint32_t curRenderW_ = 0, curRenderH_ = 0, curOutW_ = 0, curOutH_ = 0;
    UpscalerQuality curQuality_ = UpscalerQuality::Quality;
    ffxReturnCode_t lastResult_ = FFX_API_RETURN_OK;
    bool available_ = false;
    bool loggedFailure_ = false;
};

} // namespace

std::unique_ptr<IUpscaler> CreateFsrUpscaler() {
    return std::make_unique<FsrUpscaler>();
}

} // namespace mmdx
