// NVIDIA DLSS via the NGX SDK 310.x (static loader, feature DLL next to the exe).
// d3dx12.h first so <d3d12.h> (Upscaler.h) resolves to the vendored DirectX-Headers, whose
// D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT d3dx12_core.h requires.
#include <directx/d3dx12.h>
#include "render/Upscaler.h"
#include "render/Dx12Context.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <filesystem>

namespace mmdx {

namespace {

class DlssUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::DLSS; }
    const char* DisplayName() const override { return "NVIDIA DLSS"; }
    bool IsAvailable() const override { return available_; }

    bool Initialize(Dx12Context& ctx) override {
        if (ctx.Caps().vendorId != 0x10DE) {
            LOG_INFO("DLSS: not an NVIDIA GPU");
            return false;
        }
        std::filesystem::path exeDir = ExecutableDir();
        if (!std::filesystem::exists(exeDir / L"nvngx_dlss.dll")) {
            LOG_WARN("DLSS: nvngx_dlss.dll not found next to the exe");
            return false;
        }
        std::wstring exeDirString = exeDir.wstring();
        NVSDK_NGX_FeatureCommonInfo info{};
        const wchar_t* paths[] = {exeDirString.c_str()};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
            "a3b1c9d2-4e5f-4a6b-8c7d-9e0f1a2b3c4d", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
            exeDirString.c_str(), ctx.Device(), &info, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) {
            LOG_WARN("DLSS: NGX init failed (0x%08X)", (unsigned)r);
            return false;
        }
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params_);
        if (NVSDK_NGX_FAILED(r)) {
            LOG_WARN("DLSS: GetCapabilityParameters failed (0x%08X)", (unsigned)r);
            NVSDK_NGX_D3D12_Shutdown1(ctx.Device());
            return false;
        }
        int ok = 0;
        params_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &ok);
        if (!ok) {
            LOG_WARN("DLSS: SuperSampling not available (driver/snippet missing)");
            device_ = ctx.Device();
            Shutdown();
            return false;
        }
        device_ = ctx.Device();
        available_ = true;
        return true;
    }

    bool Evaluate(Dx12Context& ctx, const UpscaleInputs& in) override {
        if (!available_ || !params_) return false;
        if (feature_ && (in.renderWidth != curRenderW_ || in.renderHeight != curRenderH_ ||
                         in.outputWidth != curOutW_ || in.outputHeight != curOutH_ ||
                         in.quality != curQuality_)) {
            ctx.WaitForGpu();
            NVSDK_NGX_D3D12_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
        if (!feature_) {
            NVSDK_NGX_DLSS_Create_Params cp{};
            cp.Feature.InWidth = in.renderWidth;
            cp.Feature.InHeight = in.renderHeight;
            cp.Feature.InTargetWidth = in.outputWidth;
            cp.Feature.InTargetHeight = in.outputHeight;
            switch (in.quality) {
            case UpscalerQuality::NativeAA: cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA; break;
            case UpscalerQuality::Quality: cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
            case UpscalerQuality::Balanced: cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced; break;
            case UpscalerQuality::Performance: cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
            case UpscalerQuality::UltraPerformance: cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
            }
            cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                      NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(in.cmd, 1, 1, &feature_, params_, &cp);
            if (NVSDK_NGX_FAILED(r)) {
                LOG_ERROR("DLSS: feature creation failed (0x%08X)", (unsigned)r);
                feature_ = nullptr;
                if (!loggedFailure_) {
                    LOG_WARN("DLSS: evaluate unavailable, will not retry until the parameters change");
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
        NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = in.color;
        ep.Feature.pInOutput = in.output;
        ep.pInDepth = in.depth;
        ep.pInMotionVectors = in.velocity;
        ep.InJitterOffsetX = in.jitterX;
        ep.InJitterOffsetY = in.jitterY;
        ep.InRenderSubrectDimensions = {in.renderWidth, in.renderHeight};
        ep.InReset = in.reset ? 1 : 0;
        ep.InMVScaleX = -(float)in.renderWidth;
        ep.InMVScaleY = -(float)in.renderHeight;
        ep.InFrameTimeDeltaInMsec = in.frameTimeMs;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(in.cmd, feature_, params_, &ep);
        if (NVSDK_NGX_FAILED(r)) {
            if (!loggedFailure_) {
                LOG_WARN("DLSS: evaluate failed (0x%08X)", (unsigned)r);
                loggedFailure_ = true;
            }
            return false;
        }
        loggedFailure_ = false;
        return true;
    }

    void Shutdown() override {
        if (feature_) {
            NVSDK_NGX_D3D12_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
        if (params_) {
            NVSDK_NGX_D3D12_DestroyParameters(params_);
            params_ = nullptr;
        }
        if (device_) {
            NVSDK_NGX_D3D12_Shutdown1(device_);
            device_ = nullptr;
        }
        curRenderW_ = curRenderH_ = curOutW_ = curOutH_ = 0;
        loggedFailure_ = false;
        available_ = false;
    }

private:
    ID3D12Device* device_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    uint32_t curRenderW_ = 0, curRenderH_ = 0, curOutW_ = 0, curOutH_ = 0;
    UpscalerQuality curQuality_ = UpscalerQuality::Quality;
    bool available_ = false;
    bool loggedFailure_ = false;
};

} // namespace

std::unique_ptr<IUpscaler> CreateDlssUpscaler() {
    return std::make_unique<DlssUpscaler>();
}

} // namespace mmdx
