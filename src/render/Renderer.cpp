// Frame orchestration (see Passes.h for the pass list).
#include "render/Renderer.h"
#include "render/Passes.h"
#include "render/MmeRuntime.h"
#include "render/MmeSkin.h"
#include "render/SpotShadowPick.h"
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

namespace mmdx {

using namespace DirectX;

namespace {

constexpr uint32_t kCbSlots = Dx12Context::kFramesInFlight + 1;  // + RenderToImage
constexpr uint32_t kOffscreenSlot = Dx12Context::kFramesInFlight;
constexpr uint32_t kLightBytes = Renderer::kMaxPunctualLights * sizeof(GpuLight);
constexpr uint32_t kFogBytes = Renderer::kMaxFogVolumes * sizeof(GpuFogVolume);
// S7a: one float per light slot (the normalised inclusive CDF).
constexpr uint32_t kLightCdfBytes = kLightCdfMax * sizeof(float);
// S5: the camera feeds get their own constant-buffer / light slots, after the on-screen + offscreen + extra-view slots
// (never RenderToImage's kOffscreenSlot, which a feed would overwrite while the main scene still reads it).
constexpr uint32_t kFeedCbBase = kCbSlots * (1 + kMaxExtraViews);
// S10: the planar mirror passes come after the feeds (they sample the frame's screen textures), one slot set per mirror.
constexpr uint32_t kMirrorCbBase = kFeedCbBase + kCbSlots * StageScreens::kMaxFeedsPerFrame;
constexpr uint32_t kTotalCbSlots = kMirrorCbBase + kCbSlots * kMaxMirrorPlanes;

// post.hlsl's interleaved gradient noise (float2 unjitter(pos.xy)). Mirrored here to invert the per-pixel
// dither when a transparent export is read back, so a flat background decodes to alpha 0 exactly.
float Ign(float x, float y) {
    return std::fmod(52.9829189f * std::fmod(0.06711056f * x + 0.00583715f * y, 1.0f), 1.0f);
}

// Cache: same requested value -> same result.
uint32_t g_cachedRequestedMsaa = 0;
uint32_t g_cachedMsaaResult = 0;

uint32_t SupportedMsaa(Dx12Context& ctx, uint32_t requested) {
    if (requested == g_cachedRequestedMsaa && g_cachedMsaaResult != 0) return g_cachedMsaaResult;
    ID3D12Device* device = ctx.Device();
    uint32_t n = requested;
    const DXGI_FORMAT formats[] = {RenderTargets::kColorFormat, RenderTargets::kNormalFormat,
                                   RenderTargets::kVelocityFormat, DXGI_FORMAT_D32_FLOAT};
    while (n > 1) {
        bool ok = true;
        for (DXGI_FORMAT f : formats) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
            q.Format = f;
            q.SampleCount = n;
            ok = ok && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
                 q.NumQualityLevels > 0;
        }
        if (ok) break;
        n /= 2;
    }
    g_cachedRequestedMsaa = requested;
    g_cachedMsaaResult = std::max(1u, n);
    return g_cachedMsaaResult;
}

float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

ComPtr<ID3D12Resource> CreateMappedUpload(ID3D12Device* device, uint64_t bytes, uint8_t** mapped, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(upload)"))
        return {};
    res->SetName(name);
    if (FAILED(res->Map(0, nullptr, (void**)mapped))) return {};
    return res;
}

} // namespace

void Renderer::CreateBuiltinTextures() {
    UploadBatch batch(*ctx_);

    // White 1x1 (255,255,255,255).
    ImageRGBA8 white;
    white.mips.push_back({1, 1, {}});
    white.mips[0].pixels = {255, 255, 255, 255};
    builtin_.white = batch.CreateTexture(white, L"builtin.white");

    // Toon ramps: 32x32, single mip.
    for (int i = 0; i < 10; ++i) {
        ImageRGBA8 img;
        img.mips.push_back({32, 32, {}});
        ImageRGBA8::Level& lv = img.mips[0];
        lv.pixels.resize(32 * 32 * 4);
        for (uint32_t y = 0; y < 32; ++y) {
            float t = 0;
            if (y < 12)
                t = 0;
            else if (y > 20)
                t = 1;
            else
                t = (float)(y - 12) / 8.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            uint8_t v = (uint8_t)std::lround(255.0f + (209.0f - 255.0f) * t);
            for (uint32_t x = 0; x < 32; ++x) {
                uint8_t* px = &lv.pixels[(y * 32 + x) * 4];
                px[0] = v;
                px[1] = v;
                px[2] = v;
                px[3] = 255;
            }
        }
        wchar_t name[32];
        swprintf(name, 32, L"builtin.toon%02d", i);
        builtin_.toon[i] = batch.CreateTexture(img, name);
    }
    batch.Submit();
}

bool Renderer::Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir, RenderInitProgress* progress) {
    ctx_ = &ctx;
    shaderDir_ = shaderDir;
    if (progress) {
        progress->fraction.store(0.02f);
        progress->SetStatus(Tr("업스케일러 초기화 중"));
    }
    for (UpscalerKind k : {UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS}) {
        const size_t i = (size_t)k;
        upscalers_[i] = CreateUpscaler(k);
        upscalerAvailable_[i] = upscalers_[i]->Initialize(ctx);
        LOG_INFO("upscaler %s: %s", upscalers_[i]->DisplayName(), upscalerAvailable_[i] ? "available" : "unavailable");
    }
    if (progress) {
        progress->fraction.store(std::max(progress->fraction.load(), 0.08f));
        progress->SetStatus(Tr("렌더러 엔진 준비 중"));
    }
    frameGen_ = std::make_unique<DlssFrameGen>();
    if (!frameGen_->Initialize(ctx)) LOG_INFO("frame generation: unavailable (%s)", frameGen_->Reason());
    packTextures_ = std::make_unique<PackTextures>();
    // MME runtime + object skinning (M4b): owned here, used by ScenePass and MmePostPass through PassContext.
    // The runtime only allocates its root signature / sampler heap / rings; without any effect it stays idle.
    mme_ = std::make_unique<MmeRuntime>();
    if (!mme_->Init(ctx, shaderDir)) {
        LOG_WARN("MME runtime unavailable");
        mme_.reset();
    }
    mmeSkin_ = std::make_unique<MmeSkin>();
    if (!mmeSkin_->Init(ctx, shaderDir)) {
        mmeSkin_.reset();   // DXC unavailable: object effects draw natively
    }
    rt_ = std::make_unique<RtScene>();
    rtSupported_ = rt_->Initialize(ctx, shaderDir);
    if (!rtSupported_) rt_.reset();
    else rt_->SetPackTextures(packTextures_.get());
    screenVideo_ = std::make_unique<ScreenVideo>();
    stageScreens_ = std::make_unique<StageScreens>();
    stageScreens_->Init(shaderDir);

    CreateBuiltinTextures();
    if (!transient_.Create(ctx, kCbSlots)) return false;
    if (!feedTransient_.Create(ctx, kCbSlots * StageScreens::kMaxFeedsPerFrame)) return false;
    if (!mirrorTransient_.Create(ctx, kCbSlots * kMaxMirrorPlanes)) return false;

    passes_.clear();
    passes_.push_back(std::make_unique<ShadowPass>());
    // M5a: the MMD-style self-shadow map (before ScenePass so object_ss draws see it) then the offscreen scene
    // renders. Both skip entirely when no effect needs them (byte-identical frame).
    passes_.push_back(std::make_unique<MmeShadowPass>());
    passes_.push_back(std::make_unique<MmeOffscreenPass>());
    // S7c: the clustered light cull (a no-op unless the frame has more than kClusterMinLights lights)
    passes_.push_back(std::make_unique<LightCullPass>());
    auto* scenePass = new ScenePass();
    scenePass_ = scenePass;   // S5: the stage screen camera feeds draw through it (owned by passes_)
    passes_.push_back(std::unique_ptr<ScenePass>(scenePass));
    auto* worldPass = new WorldPass();   // also owned by passes_; WorldParticlesPass holds a non-owning pointer
    passes_.push_back(std::unique_ptr<WorldPass>(worldPass));
    passes_.push_back(std::make_unique<ResolvePass>());
    passes_.push_back(std::make_unique<PathTracePass>());
    passes_.push_back(std::make_unique<SsaoPass>());
    passes_.push_back(std::make_unique<SsrPass>());
    passes_.push_back(std::make_unique<CompositePass>());
    // World particles go after the composite (so SSR / SSAO / RT reflections never see them) and before volumetrics.
    passes_.push_back(std::make_unique<WorldParticlesPass>(worldPass));
    passes_.push_back(std::make_unique<VolumetricPass>());
    passes_.push_back(std::make_unique<TaaPass>());
    passes_.push_back(std::make_unique<UpscalePass>());
    passes_.push_back(std::make_unique<DofPass>());
    // "pre-bloom" effect packs: linear HDR, before Bloom (Bloom reads hdrFinal, which this share writes). The
    // "post" share runs after PostPass (below).
    passes_.push_back(std::make_unique<PackEffectPass>(true));
    passes_.push_back(std::make_unique<BloomPass>());
    passes_.push_back(std::make_unique<PostPass>());
    // M8b: the song's video layers. Background where the scene has no geometry (right after post, before the effect
    // packs), overlay composited over the final image (after the effect packs / MME post chain, before the UI backdrop).
    passes_.push_back(std::make_unique<VideoPass>(VideoStage::Background));
    passes_.push_back(std::make_unique<PackEffectPass>(false));
    passes_.push_back(std::make_unique<MmePostPass>());
    passes_.push_back(std::make_unique<VideoPass>(VideoStage::Overlay));
    passes_.push_back(std::make_unique<BackdropPass>());
    passes_.push_back(std::make_unique<PresentPass>());

    if (progress) {
        progress->fraction.store(std::max(progress->fraction.load(), 0.15f));
        progress->SetStatus(GetColdShaderCount() > 0 ? Tr("셰이더 컴파일 중 (첫 실행은 시간이 걸립니다)") : Tr("셰이더 로딩 중"));
    }

    // The passes create their pipelines on several threads (the splash follows the shader counters, which App::Run
    // polls meanwhile). Here a pass only compiles shaders and creates objects of its own; the shader cache, the
    // compilers and the descriptor heaps are thread-safe. A re-creation for another MSAA count (Render) stays serial:
    // there a pass may also release resources through the context (WaitForGpu / DeferRelease).
    uint32_t msaa = SupportedMsaa(ctx, settings_.msaaSamples);
    {
        const auto start = std::chrono::steady_clock::now();
        std::vector<double> passMs(passes_.size(), 0.0);
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        const auto work = [&] {
            for (size_t p; !failed && (p = next.fetch_add(1)) < passes_.size();) {
                const auto t0 = std::chrono::steady_clock::now();
                if (!passes_[p]->CreatePipelines(ctx, shaderDir, msaa)) {
                    LOG_ERROR("Renderer: failed to create pipelines for pass '%s'", passes_[p]->Name());
                    failed = true;
                }
                passMs[p] = MsSince(t0);
            }
        };
        // One thread per logical processor (0 = unknown: serial), at most one per pass. The scene pass creates its
        // shaders and PSOs one after another and is by far the longest one: it bounds the time, not the thread count.
        const unsigned threads =
            (unsigned)std::min<size_t>(std::max(std::thread::hardware_concurrency(), 1u), passes_.size());
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
        if (failed) return false;
        const size_t slowest = (size_t)(std::max_element(passMs.begin(), passMs.end()) - passMs.begin());
        LOG_INFO("STARTUP renderer passes: %.0f ms (%zu passes, %u threads; longest %s %.0f ms)", MsSince(start),
                 passes_.size(), threads, passes_[slowest]->Name(), passMs[slowest]);
    }
    pipelineMsaa_ = msaa;

    if (progress) {
        progress->fraction.store(std::max(progress->fraction.load(), 0.95f));
        progress->SetStatus(Tr("초기화 완료 중"));
    }

    sceneCb_ = CreateMappedUpload(ctx.Device(), (uint64_t)kSceneCbSize * kTotalCbSlots, &sceneCbMapped_, L"scene.cb");
    lightBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kLightBytes * kTotalCbSlots, &lightBufMapped_, L"scene.lights");
    fogBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kFogBytes * kTotalCbSlots, &fogBufMapped_, L"scene.fogvolumes");
    // S7a: the light CDF (raw ByteAddressBuffer SRV per slot, in the bindless buffer table the shaders index).
    lightCdfBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kLightCdfBytes * kTotalCbSlots,
                                      (uint8_t**)&lightCdfMapped_, L"scene.lightcdf");
    if (!sceneCb_ || !lightBuf_ || !fogBuf_ || !lightCdfBuf_) return false;
    if (kTotalCbSlots > (uint32_t)std::size(lightCdfSrv_)) {
        LOG_ERROR("light CDF SRV table too small for %u slots", kTotalCbSlots);
        return false;
    }
    for (uint32_t slot = 0; slot < kTotalCbSlots; ++slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kLightCdfMax;
        srv.Buffer.FirstElement = (UINT)slot * kLightCdfMax;   // this slot's CDF (kLightCdfMax floats per slot)
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t idx = ctx.SrvHeap().Allocate(1);
        if (idx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(lightCdfBuf_.Get(), &srv, ctx.SrvHeap().Cpu(idx));
        lightCdfSrv_[slot] = idx;
    }
    lightCdfSrvCount_ = kTotalCbSlots;

    // S7b: the frame's gobo table (one GpuGobo per punctual light, raw SRV per slot through the bindless buffer table,
    // exactly like the light CDF). The gobo textures themselves are decoded lazily on the first frame a light uses one.
    goboBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)(kMaxPunctualLights * sizeof(GpuGobo)) * kTotalCbSlots,
                                  &goboBufMapped_, L"scene.gobos");
    if (!goboBuf_) return false;
    if (kTotalCbSlots > (uint32_t)std::size(goboSrv_)) {
        LOG_ERROR("gobo table SRV table too small for %u slots", kTotalCbSlots);
        return false;
    }
    for (uint32_t slot = 0; slot < kTotalCbSlots; ++slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kMaxPunctualLights * (uint32_t)(sizeof(GpuGobo) / sizeof(uint32_t));
        srv.Buffer.FirstElement = (UINT)slot * kMaxPunctualLights * (uint32_t)(sizeof(GpuGobo) / sizeof(uint32_t));
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t idx = ctx.SrvHeap().Allocate(1);
        if (idx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(goboBuf_.Get(), &srv, ctx.SrvHeap().Cpu(idx));
        goboSrv_[slot] = idx;
    }
    goboSrvCount_ = kTotalCbSlots;
    goboTable_.resize(kMaxPunctualLights);

    // S8b: the frame's screen-light table (one GpuScreenLight per screen light, raw SRV per slot through the bindless
    // buffer table, exactly like the gobo table) and the average-colour buffer the compute pass writes. A frame without
    // a screen light leaves SceneConstants::screenLightParams.x = -1 and every screen term is skipped.
    screenLightBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)(kMaxScreenLights * sizeof(GpuScreenLight)) * kTotalCbSlots,
                                         &screenLightBufMapped_, L"scene.screenlights");
    if (!screenLightBuf_) return false;
    if (kTotalCbSlots > (uint32_t)std::size(screenLightSrv_)) {
        LOG_ERROR("screen light table SRV table too small for %u slots", kTotalCbSlots);
        return false;
    }
    for (uint32_t slot = 0; slot < kTotalCbSlots; ++slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kMaxScreenLights * (uint32_t)(sizeof(GpuScreenLight) / sizeof(uint32_t));
        srv.Buffer.FirstElement = (UINT)slot * kMaxScreenLights * (uint32_t)(sizeof(GpuScreenLight) / sizeof(uint32_t));
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t idx = ctx.SrvHeap().Allocate(1);
        if (idx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(screenLightBuf_.Get(), &srv, ctx.SrvHeap().Cpu(idx));
        screenLightSrv_[slot] = idx;
    }
    screenLightSrvCount_ = kTotalCbSlots;
    screenLightTable_.resize(kMaxScreenLights);
    // The screen lights' average colours: a DEFAULT-heap raw buffer of kMaxScreenLights float4 (the screen-light compute
    // pass writes it as a UAV, the scene shaders read it as a raw SRV through the bindless buffer table). A buffer is
    // created in COMMON whatever the requested initial state, so screenAverageState_ tracks the real state.
    {
        const CD3DX12_RESOURCE_DESC avgDesc = CD3DX12_RESOURCE_DESC::Buffer(
            kMaxScreenLights * 16u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CD3DX12_HEAP_PROPERTIES avgHeap{D3D12_HEAP_TYPE_DEFAULT};
        if (!CheckHr(ctx.Device()->CreateCommittedResource(&avgHeap, D3D12_HEAP_FLAG_NONE, &avgDesc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&screenAverageBuf_)),
                     "CreateCommittedResource(screen average colours)"))
            return false;
        screenAverageBuf_->SetName(L"scene.screenaverage");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kMaxScreenLights * 4u;
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        screenAverageSrv_ = ctx.SrvHeap().Allocate(1);
        if (screenAverageSrv_ == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(screenAverageBuf_.Get(), &srv, ctx.SrvHeap().Cpu(screenAverageSrv_));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = kMaxScreenLights * 4u;
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        screenAverageUav_ = ctx.SrvHeap().Allocate(1);
        if (screenAverageUav_ == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateUnorderedAccessView(screenAverageBuf_.Get(), nullptr, &uav,
                                                ctx.SrvHeap().Cpu(screenAverageUav_));
    }
    // S9b-2: the sky environment buffer (the Physical Sky's CDFs and SH irradiance, shaders/sky_env.hlsl)
    if (!skyEnv_.Create(ctx, shaderDir_)) return false;
    // S8a: the emissive-triangle pick table (one raw SRV each for the triangle list, its CDF and the per-geometry base
    // table; same slot layout as the light CDF). A frame without emitters leaves SceneConstants::emissiveParams.x = -1
    // and the path tracers skip every NEE / MIS branch.
    emissiveTriBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kEmissiveTriBytes * kTotalCbSlots,
                                         &emissiveTriMapped_, L"scene.emissivetris");
    emissiveCdfBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kEmissiveCdfBytes * kTotalCbSlots,
                                         (uint8_t**)&emissiveCdfMapped_, L"scene.emissivecdf");
    emissiveBaseBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kEmissiveBaseBytes * kTotalCbSlots,
                                          (uint8_t**)&emissiveBaseMapped_, L"scene.emissivebase");
    if (!emissiveTriBuf_ || !emissiveCdfBuf_ || !emissiveBaseBuf_) return false;
    if (kTotalCbSlots > (uint32_t)std::size(emissiveTriSrv_)) {
        LOG_ERROR("emissive NEE: SRV table too small for %u slots", kTotalCbSlots);
        return false;
    }
    for (uint32_t slot = 0; slot < kTotalCbSlots; ++slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kEmissiveTriMax * (sizeof(GpuEmissiveTri) / 4);
        srv.Buffer.FirstElement = (UINT)((uint64_t)slot * kEmissiveTriMax * (sizeof(GpuEmissiveTri) / 4));
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t triIdx = ctx.SrvHeap().Allocate(1);
        if (triIdx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(emissiveTriBuf_.Get(), &srv, ctx.SrvHeap().Cpu(triIdx));
        emissiveTriSrv_[slot] = triIdx;

        srv.Buffer.NumElements = kEmissiveTriMax;
        srv.Buffer.FirstElement = (UINT)((uint64_t)slot * kEmissiveTriMax);
        const uint32_t cdfIdx = ctx.SrvHeap().Allocate(1);
        if (cdfIdx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(emissiveCdfBuf_.Get(), &srv, ctx.SrvHeap().Cpu(cdfIdx));
        emissiveCdfSrv_[slot] = cdfIdx;

        srv.Buffer.NumElements = kEmissiveBaseMax;
        srv.Buffer.FirstElement = (UINT)((uint64_t)slot * kEmissiveBaseMax);
        const uint32_t baseIdx = ctx.SrvHeap().Allocate(1);
        if (baseIdx == DescriptorHeap::kInvalid) return false;
        ctx.Device()->CreateShaderResourceView(emissiveBaseBuf_.Get(), &srv, ctx.SrvHeap().Cpu(baseIdx));
        emissiveBaseSrv_[slot] = baseIdx;
    }
    emissiveSrvCount_ = kTotalCbSlots;

    // S7c: the per-slot light-cluster blocks (PassLightCull writes them as a RAW UAV, the scene shaders read them as a
    // raw ByteAddressBuffer through the bindless buffer table). A DEFAULT-heap buffer: the UAV is not allowed on an
    // upload heap, and it is written / read on the GPU only.
    {
        CD3DX12_HEAP_PROPERTIES heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
            (UINT64)kClusterBytes * kTotalCbSlots, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!CheckHr(ctx.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
                                                           IID_PPV_ARGS(&lightClusterBuf_)),
                      "CreateCommittedResource(light clusters)"))
            return false;
        if (kTotalCbSlots > (uint32_t)std::size(lightClusterSrv_)) {
            LOG_ERROR("light cluster SRV table too small for %u slots", kTotalCbSlots);
            return false;
        }
        for (uint32_t slot = 0; slot < kTotalCbSlots; ++slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = kClusterDwords;
            srv.Buffer.FirstElement = (UINT)((uint64_t)slot * kClusterDwords);
            srv.Buffer.StructureByteStride = 0;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            const uint32_t idx = ctx.SrvHeap().Allocate(1);
            if (idx == DescriptorHeap::kInvalid) return false;
            ctx.Device()->CreateShaderResourceView(lightClusterBuf_.Get(), &srv, ctx.SrvHeap().Cpu(idx));
            lightClusterSrv_[slot] = idx;
        }
        lightClusterSrvCount_ = kTotalCbSlots;
    }

    backdropSrv_ = ctx.SrvHeap().Allocate(1);

    // GPU timing.
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * Dx12Context::kFramesInFlight;
    if (!CheckHr(ctx.Device()->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestampHeap_)), "CreateQueryHeap"))
        return false;
    CD3DX12_RESOURCE_DESC rbDesc = CD3DX12_RESOURCE_DESC::Buffer(16 * Dx12Context::kFramesInFlight);
    D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&timestampReadback_)),
                 "CreateCommittedResource(timestamp readback)"))
        return false;
    ctx.Queue()->GetTimestampFrequency(&timestampFrequency_);
    SaveExpectedShaderCount(GetCompiledShaderCount());
    // The offline GI renderer is created in the background once the rest is ready: only GI renders need it, and its
    // pipelines are the slowest to create (offline_gi.hlsl). Its Initialize only creates objects (device, descriptor
    // heaps, its own upload batch); it records nothing into a frame and never waits on the frame fence, so the frames
    // go on meanwhile. OfflineReady says when it is done; BeginOffline / Shutdown wait for it (EnsureOffline), and a
    // failed one is dropped there.
    if (rtSupported_) {
        offlineInit_ = std::async(std::launch::async, [this, &ctx, shaderDir] {
            const auto t0 = std::chrono::steady_clock::now();
            auto offline = std::make_unique<OfflineRenderer>();
            if (!offline->Initialize(ctx, shaderDir, &offlineInitCancel_)) {
                if (offlineInitCancel_) LOG_INFO("offline renderer: background creation cancelled (shutdown)");
                else LOG_WARN("offline renderer unavailable");
                offlineInitFailed_ = true;
            }
            LOG_INFO("STARTUP offline renderer (background): %.0f ms", MsSince(t0));
            return offline;
        });
    }
    if (progress) {
        progress->fraction.store(1.0f);
        progress->SetStatus(Tr("완료"));
    }
    return true;
}

std::unique_ptr<GpuModel> Renderer::CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                                const std::vector<ImageRGBA8>& textures, ModelRole role,
                                                const PbrModelData* pbr,
                                                const std::vector<CompressedImage>* compressed) {
    auto model = std::make_unique<GpuModel>();
    static const PbrModelData kEmptyPbr;
    if (!model->Create(*ctx_, batch, pmx, textures, builtin_, role, pbr ? *pbr : kEmptyPbr, compressed)) return nullptr;
    model->SetHasPbrRecords(pbr && pbr->materialsWithRecord > 0);
    return model;
}

void Renderer::ForgetModel(uint64_t modelUid) {
    if (modelUid == 0 || !ctx_) return;
    if (screenVideo_) screenVideo_->ForgetModel(modelUid);
    if (stageScreens_) stageScreens_->ForgetModel(modelUid);
    // Frees the model's MME resources (object SRV tables + skinned buffer). Keyed by uid, they may live in
    // in-flight command lists, so the free is deferred by the number of frames in flight plus one (matching the
    // GpuModel ring logic). DrainMmeForget runs it once those frames are done.
    for (const PendingMmeForget& p : pendingMmeForget_)
        if (p.modelUid == modelUid) return;
    pendingMmeForget_.push_back({modelUid, ctx_->FrameNumber() + Dx12Context::kFramesInFlight + 1});
}

void Renderer::DrainMmeForget() {
    if (pendingMmeForget_.empty() || !ctx_) return;
    const uint64_t now = ctx_->FrameNumber();
    pendingMmeForget_.erase(
        std::remove_if(pendingMmeForget_.begin(), pendingMmeForget_.end(),
                       [&](const PendingMmeForget& p) {
                           if (p.frame > now) return false;
                           if (mme_) mme_->ForgetModel(*ctx_, p.modelUid);
                           if (mmeSkin_) mmeSkin_->Forget(*ctx_, p.modelUid);
                           return true;
                       }),
        pendingMmeForget_.end());
}

// S5: the HDR targets one camera feed draws into. The feed size is the screen's own texture size (already capped at
// 960x540 by StageScreens), so the targets follow the screen's size.
bool Renderer::EnsureFeedTargets(uint32_t feedIndex, uint32_t width, uint32_t height) {
    if (feedIndex >= kFeedSlots) return false;
    FeedTargets& f = feeds_[feedIndex];
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (f.color && f.width == width && f.height == height) return true;
    ctx_->WaitForGpu();   // targets still referenced by frames in flight
    f.color.Release(*ctx_);
    f.normal.Release(*ctx_);
    f.velocity.Release(*ctx_);
    f.depth.Release(*ctx_);
    const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!f.color.Create(*ctx_, width, height, RenderTargets::kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        srvState, L"feed.color") ||
        !f.normal.Create(*ctx_, width, height, RenderTargets::kNormalFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         srvState, L"feed.normal") ||
        !f.velocity.Create(*ctx_, width, height, RenderTargets::kVelocityFormat,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, srvState, L"feed.velocity") ||
        !f.depth.Create(*ctx_, width, height, RenderTargets::kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                        D3D12_RESOURCE_STATE_DEPTH_WRITE, L"feed.depth")) {
        f.color.Release(*ctx_);
        f.normal.Release(*ctx_);
        f.velocity.Release(*ctx_);
        f.depth.Release(*ctx_);
        LOG_WARN("stage screen feed: target creation failed (%ux%u)", width, height);
        return false;
    }
    f.width = width;
    f.height = height;
    return true;
}

bool Renderer::RenderScreenFeed(ID3D12GraphicsCommandList* cmd, const FrameView& view, const ScreenView& screen,
                                Texture& target, uint32_t feedIndex, const SceneConstants& base) {
    if (!cmd || !scenePass_ || !target || feedIndex >= kFeedSlots) return false;
    const uint32_t w = target.width, h = target.height;
    if (!EnsureFeedTargets(feedIndex, w, h)) return false;
    FeedTargets& f = feeds_[feedIndex];
    if (!feedResolveTried_) {
        feedResolveTried_ = true;
        if (!feedResolve_.Create(*ctx_, shaderDir_ / L"screenfeed.hlsl", "PSFeed",
                                 {DXGI_FORMAT_R8G8B8A8_UNORM}))
            LOG_WARN("stage screen feed: screenfeed.hlsl unavailable, the feeds stay black");
    }

    // The feed's own descriptor window (the scene passes keep their own; a feed runs before them), rewound for this
    // frame slot / feed index.
    feedTransient_.Begin(feedIndex * kCbSlots + ctx_->FrameSlot());
    // The feed's own constant-buffer / light slots (never RenderToImage's kOffscreenSlot: the main scene still reads
    // the frame's slots while this runs). The camera comes from the screen; everything else is the frame's.
    const uint32_t cbSlot = kFeedCbBase + feedIndex * kCbSlots + ctx_->FrameSlot();
    SceneConstants sc = base;
    const CameraParams& cam = screen.camera;
    const XMMATRIX viewM = XMLoadFloat4x4(&cam.view);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(cam.fovYRadians, (float)w / (float)h, cam.nearZ, cam.farZ);
    const XMMATRIX vp = viewM * proj;
    XMStoreFloat4x4(&sc.view, viewM);
    XMStoreFloat4x4(&sc.proj, proj);
    XMStoreFloat4x4(&sc.viewProj, vp);
    XMStoreFloat4x4(&sc.viewProjNoJitter, vp);
    XMStoreFloat4x4(&sc.prevViewProjNoJitter, vp);   // a feed has no temporal history
    XMStoreFloat4x4(&sc.invProj, XMMatrixInverse(nullptr, proj));
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat4x4(&sc.invView, invView);
    XMStoreFloat3(&sc.eyePos, invView.r[3]);
    sc.jitterUv = {0, 0};
    sc.nearZ = cam.nearZ;
    sc.farZ = cam.farZ;
    sc.viewportSize = {(float)w, (float)h};
    sc.invViewportSize = {1.0f / (float)w, 1.0f / (float)h};
    sc.edgeScale = (float)h / 1080.0f;
    sc.shading = 0.0f;   // always lit
    sc.lightClusterParams = {-1.0f, 0.0f, 0.0f, 0.0f};   // S7c: the feed's camera is not the frame's cluster grid
    sc.emissiveParams = emissiveParams_;   // S8a: the frame's emissive-triangle pick table (raster feed: unused)
    sc.goboParams = {goboActive_ ? (float)goboSrv_[cbSlot] : -1.0f, 0.0f, 0.0f, 0.0f};   // S7b: the frame's gobos
    // S8b: the feed's own slot copies the frame's screen-light table (an LED wall inside a feed is a rare recursion the
    // screen itself does not render; the table is the frame's, the same as the gobos)
    sc.screenLightParams = {-1.0f, 0.0f, -1.0f, 0.0f};
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));

    // The feed reads the frame's lights + shadow maps: its own slot in the light buffer is a copy of the frame's.
    memcpy(lightBufMapped_ + (size_t)cbSlot * kLightBytes,
           lightBufMapped_ + (size_t)ctx_->FrameSlot() * kLightBytes, kLightBytes);
    // S7b: the feed's gobo table is the frame's own (same light list), and the feed needs a copy in its own slot
    if (goboActive_)
        memcpy(goboBufMapped_ + (size_t)cbSlot * kMaxPunctualLights * sizeof(GpuGobo),
               goboBufMapped_ + (size_t)ctx_->FrameSlot() * kMaxPunctualLights * sizeof(GpuGobo),
               kMaxPunctualLights * sizeof(GpuGobo));

    bool drew = false;
    RenderSettings feedSettings = settings_;
    feedSettings.renderPath = RenderPath::Raster;
    feedSettings.taa = false;
    feedSettings.dof = false;
    feedSettings.volumetric = false;
    feedSettings.ssr = false;   // the feed is a display image: no screen-space reflection recursion
    feedSettings.packEffects.clear();
    feedSettings.mmeEffects.clear();
    // The feed's own draw calls must not distort the frame's stats.
    const RenderStats savedStats = stats_;
    PassContext pc{*ctx_, cmd, view, feedSettings, targets_, stats_, feedTransient_,
                   sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                   lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes,
                   ctx_->FrameNumber(), false, false, &builtin_, RenderPath::Raster, nullptr, nullptr,
                   0.0f, 0.0f, frameTimeMs_};
    pc.fogVolumes = 0;   // a feed draws no local fog volumes
    stats_.drawCalls = 0;
    stats_.triangles = 0;
    // The screens inside a feed show their PREVIOUS frame's texture: StageScreens binds after the feeds ran, so the
    // materials of this feed draw already carry the previous binding (no recursion into another feed).
    if (scenePass_->DrawFeed(pc, f.color, f.normal, f.velocity, f.depth) && feedResolve_) {
        // tonemap + sRGB into the screen's own RGBA8 texture (the LED image every path samples)
        target.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = feedTransient_.SrvTable(*ctx_, {&f.color});
        feedResolve_.Draw(pc, {&target}, table, nullptr, 0);
        // every reader: the scene PS, the screen-light average and the path tracers (compute); StageScreens records the
        // same state (it used to record ALL while the texture was left in PIXEL_SHADER_RESOURCE)
        target.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        drew = true;
    }
    stats_ = savedStats;
    return drew;
}

void Renderer::UpdateStageScreens(ID3D12GraphicsCommandList* cmd, const FrameView& view, const SceneConstants& baseSc) {
    if (!stageScreens_) return;
    if (view.screens.empty()) {
        // no screens this frame: whatever was bound must go back to the models' own textures
        stageScreens_->Render(*ctx_, cmd, view, {});
        return;
    }
    const auto renderFeed = [&](uint32_t feedIndex, const ScreenView& screen, Texture& target) {
        return RenderScreenFeed(cmd, view, screen, target, feedIndex, baseSc);
    };
    stageScreens_->Render(*ctx_, cmd, view, renderFeed);
}

// S8b: this frame's screen lights. A screen light is the rectangle a stage screen's material fits (GpuModel::ScreenRect,
// model space, made world by the same root transform the mirrors use), its emissive strength (the material's
// reflectivity slot, the scale ScreenEmission uses) and the screen texture the material shows right now. A small compute
// pass (shaders/screen_light_avg.hlsl) reduces each texture to one average linear colour into the frame's average
// buffer, which the scene shaders use as the light's radiance (so the light follows the video / camera feed / program
// content with no CPU readback); the specular reflection reads the texture itself at the representative point.
//
// Switch off (RenderSettings::screenLights) or no screen in the frame: screenLightParams.x = -1, no table, no work at
// all, and the frame is the one without the feature byte for byte. At most kMaxScreenLights (16) lights per frame: the
// largest emitted extent (area x strength) first, since the content colour is not known on the CPU. The cap is a shader
// constant (ShaderInterop.h kMaxScreenLights).
void Renderer::UpdateScreenLights(ID3D12GraphicsCommandList* cmd, const FrameView& view, SceneConstants& sc,
                                  uint32_t cbSlot) {
    screenReady_ = false;
    screenLightCount_ = 0;
    sc.screenLightParams = {-1.0f, 0.0f, -1.0f, 0.0f};
    if (!settings_.screenLights || view.screens.empty() || !screenLightBuf_ || cbSlot >= screenLightSrvCount_)
        return;
    // the screen texture each screen material shows right now (StageScreens bound it already this frame)
    const auto modelOf = [&](uint64_t uid) -> const GpuModel* {
        for (const GpuModel* m : view.models)
            if (m && m->Uid() == uid) return m;
        return nullptr;
    };
    struct Candidate {
        DirectX::XMFLOAT4 center;   // xyz world centre, w strength
        DirectX::XMFLOAT4 normal;   // xyz unit world normal, w area
        DirectX::XMFLOAT4 axisA;    // xyz world half axis A, w the screen texture's SrvHeap index
        DirectX::XMFLOAT4 axisB;    // xyz world half axis B
        float score = 0.0f;         // area x strength (the CPU's power ranking)
    };
    static thread_local std::vector<Candidate> candidates;
    candidates.clear();
    for (const ScreenView& sv : view.screens) {
        if (!sv.enabled) continue;   // source none: the surface is black, no light
        const GpuModel* model = modelOf(sv.modelUid);
        if (!model) continue;
        // the root transform (a stage part root is identity unless the part moves)
        XMMATRIX root = XMMatrixIdentity();
        for (const MmeSceneObject& o : view.mmeObjects) {
            if (o.gpu != model) continue;
            root = XMLoadFloat4x4(&o.rootWorld);
            break;
        }
        const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, root));
        const float det = XMVectorGetX(XMMatrixDeterminant(root));
        const float scale = std::cbrt(std::fabs(det));   // the root's uniform scale (1 for a PMX stage)
        // The texture the screen shows: image / video / feed / program, black when it failed to load. StageScreens owns
        // it; the model's material SRV table is the CPU handle StageScreens re-pointed, so +0 is the screen's texture.
        uint32_t textureSrv = DescriptorHeap::kInvalid;
        const std::vector<GpuModel::Material>& mats = model->Materials();
        for (uint32_t mi : sv.materials) {
            if (mi < mats.size() && mats[mi].srvTable != DescriptorHeap::kInvalid) {
                textureSrv = mats[mi].srvTable;
                break;
            }
        }
        if (textureSrv == DescriptorHeap::kInvalid) continue;
        const std::vector<MaterialConstants>& consts = model->MaterialConstantsCpu();
        for (const ScreenRect& r : model->ScreenRects()) {
            // only the materials this screen claims (MatFlag_Screen is set on exactly those this frame)
            if (std::find(sv.materials.begin(), sv.materials.end(), (uint32_t)r.material) == sv.materials.end())
                continue;
            if (r.material >= consts.size()) continue;
            const float strength = std::max(consts[r.material].reflectivity, 0.0f);
            if (!(strength > 0.0f)) continue;
            Candidate c;
            const XMVECTOR center = XMVector3TransformCoord(XMLoadFloat3(&r.center), root);
            const XMVECTOR normal = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&r.normal), normalMat));
            const XMVECTOR a = XMVector3TransformNormal(XMLoadFloat3(&r.axisA), root);
            const XMVECTOR b = XMVector3TransformNormal(XMLoadFloat3(&r.axisB), root);
            XMStoreFloat4(&c.center, center);
            c.center.w = strength;
            XMStoreFloat4(&c.normal, normal);
            // the world area: the rectangle's model area scaled by the root's uniform scale squared
            c.normal.w = 4.0f * r.halfA * r.halfB * scale * scale;
            XMStoreFloat4(&c.axisA, a * (r.halfA * scale));
            c.axisA.w = (float)textureSrv;
            XMStoreFloat4(&c.axisB, b * (r.halfB * scale));
            // largest emitted extent first (area x strength): the content colour is not known on the CPU
            c.score = c.normal.w * strength;
            if (c.score > 1e-6f) candidates.push_back(c);
        }
    }
    if (candidates.empty()) return;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (candidates.size() > kMaxScreenLights) candidates.resize(kMaxScreenLights);
    const uint32_t count = (uint32_t)candidates.size();

    // The frame's screen-light table (the per-slot raw upload the shaders and the average-colour pass read through the
    // bindless buffer table).
    std::vector<GpuScreenLight>& table = screenLightTable_;
    for (GpuScreenLight& l : table) l = GpuScreenLight{{0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    for (uint32_t i = 0; i < count; ++i) {
        table[i].center = candidates[i].center;
        table[i].normal = candidates[i].normal;
        table[i].axisA = candidates[i].axisA;
        table[i].axisB = candidates[i].axisB;
    }
    memcpy(screenLightBufMapped_ + (size_t)cbSlot * kMaxScreenLights * sizeof(GpuScreenLight), table.data(),
           kMaxScreenLights * sizeof(GpuScreenLight));

    // The average-colour pass (shaders/screen_light_avg.hlsl): one 16x16 group per screen light reduces its texture
    // to one linear colour. The buffer goes UAV -> SSV around the dispatch, which is how the scene shaders read it.
    if (!screenAvgTried_) {
        screenAvgTried_ = true;
        if (!screenAvgPipe_.Create(*ctx_, shaderDir_ / L"screen_light_avg.hlsl", "CSScreenAvg"))
            LOG_WARN("screen lights: screen_light_avg.hlsl unavailable, the screens' colour stays black");
    }
    if (screenAvgPipe_) {
        const D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            screenAverageBuf_.Get(), screenAverageState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &toUav);
        PassContext pc{*ctx_, cmd, view, settings_, targets_, stats_, transient_,
                       sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                       lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes,
                       ctx_->FrameNumber(), false, false, &builtin_, RenderPath::Raster, nullptr, nullptr,
                       0.0f, 0.0f, frameTimeMs_};
        const float constants[16] = {(float)count, (float)screenLightSrv_[cbSlot], 0.0f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, 0.0f, 0.0f};
        screenAvgPipe_.Dispatch(pc, {}, ctx_->SrvHeap().Gpu(screenAverageUav_), constants, 16, count, 1);
        const D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            screenAverageBuf_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &toSrv);
        screenAverageState_ = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    sc.screenLightParams = {(float)screenLightSrv_[cbSlot], (float)count, (float)screenAverageSrv_, 0.0f};
    screenReady_ = true;
    screenLightCount_ = count;
    if (!loggedScreenLights_) {
        loggedScreenLights_ = true;
        LOG_INFO("SCREENLIGHTS count=%u (cap %u)", count, kMaxScreenLights);
    }
}

// S10 planar mirrors: the reflection targets one slot draws into (half the render resolution, RGBA16F colour like the
// scene) and their persistent bindless SRV. Created on the first mirror frame, then kept.
bool Renderer::EnsureMirrorTargets(uint32_t slot, uint32_t width, uint32_t height) {
    if (slot >= kMaxMirrorPlanes) return false;
    MirrorTargets& m = mirrors_[slot];
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (m.color && m.width == width && m.height == height) return true;
    ctx_->WaitForGpu();   // targets still referenced by frames in flight
    m.color.Release(*ctx_);
    m.normal.Release(*ctx_);
    m.velocity.Release(*ctx_);
    m.depth.Release(*ctx_);
    const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!m.color.Create(*ctx_, width, height, RenderTargets::kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        srvState, L"mirror.color") ||
        !m.normal.Create(*ctx_, width, height, RenderTargets::kNormalFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         srvState, L"mirror.normal") ||
        !m.velocity.Create(*ctx_, width, height, RenderTargets::kVelocityFormat,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, srvState, L"mirror.velocity") ||
        !m.depth.Create(*ctx_, width, height, RenderTargets::kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                        D3D12_RESOURCE_STATE_DEPTH_WRITE, L"mirror.depth")) {
        m.color.Release(*ctx_);
        m.normal.Release(*ctx_);
        m.velocity.Release(*ctx_);
        m.depth.Release(*ctx_);
        m.width = m.height = 0;
        LOG_WARN("planar mirror: target creation failed (%ux%u)", width, height);
        return false;
    }
    if (m.srv == DescriptorHeap::kInvalid) {
        m.srv = ctx_->SrvHeap().Allocate(1);
        if (m.srv == DescriptorHeap::kInvalid) {
            LOG_WARN("planar mirror: no SRV for the reflection texture");
            m.color.Release(*ctx_);
            m.normal.Release(*ctx_);
            m.velocity.Release(*ctx_);
            m.depth.Release(*ctx_);
            m.width = m.height = 0;
            return false;
        }
    }
    m.color.WriteSrv(ctx_->Device(), ctx_->SrvHeap().Cpu(m.srv));   // the slot's texture changed: re-point the SRV
    m.width = width;
    m.height = height;
    return true;
}

// S10: the scene drawn from the camera reflected about `plane` into the slot's reflection texture. Modelled on
// RenderScreenFeed: lit raster at half the render resolution, the frame's lights / shadow maps, no TAA / upscaler /
// DoF / volumetrics / effects / MME / packs; mirror materials inside it draw their tint only (mirrorParams.x = 0) and
// `clipPlane` drops everything behind the mirror plane.
bool Renderer::RenderMirror(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t slot,
                            const DirectX::XMFLOAT4& plane, const DirectX::XMMATRIX& reflectedView,
                            const DirectX::XMMATRIX& proj, const SceneConstants& base) {
    if (!cmd || !scenePass_ || slot >= kMaxMirrorPlanes || !mirrors_[slot].color) return false;
    MirrorTargets& m = mirrors_[slot];
    const uint32_t w = m.width, h = m.height;

    mirrorTransient_.Begin(slot * kCbSlots + ctx_->FrameSlot());
    const uint32_t cbSlot = kMirrorCbBase + slot * kCbSlots + ctx_->FrameSlot();
    SceneConstants sc = base;
    const XMMATRIX vp = reflectedView * proj;
    XMStoreFloat4x4(&sc.view, reflectedView);
    XMStoreFloat4x4(&sc.proj, proj);
    XMStoreFloat4x4(&sc.viewProj, vp);
    XMStoreFloat4x4(&sc.viewProjNoJitter, vp);
    XMStoreFloat4x4(&sc.prevViewProjNoJitter, vp);   // a reflection has no temporal history
    XMStoreFloat4x4(&sc.invProj, XMMatrixInverse(nullptr, proj));
    const XMMATRIX invView = XMMatrixInverse(nullptr, reflectedView);
    XMStoreFloat4x4(&sc.invView, invView);
    XMStoreFloat3(&sc.eyePos, invView.r[3]);
    sc.jitterUv = {0, 0};
    sc.nearZ = view.camera.nearZ;
    sc.farZ = view.camera.farZ;
    sc.viewportSize = {(float)w, (float)h};
    sc.invViewportSize = {1.0f / (float)w, 1.0f / (float)h};
    sc.edgeScale = (float)h / 1080.0f;
    sc.shading = 0.0f;   // always lit
    sc.lightClusterParams = {-1.0f, 0.0f, 0.0f, 0.0f};   // S7c: the reflected camera is not the frame's cluster grid
    sc.emissiveParams = {-1.0f, -1.0f, 0.0f, -1.0f};     // raster: unused
    sc.mirrorParams = {0.0f, -1.0f, -1.0f, 0.0f};        // no recursion: a mirror inside the reflection shows its tint
    sc.clipPlane = plane;                                // drop everything behind the mirror plane
    // S7b: the reflection shows the frame's gobos (same light list, its own cb slot); copy the table into this slot
    sc.goboParams = {goboActive_ ? (float)goboSrv_[cbSlot] : -1.0f, 0.0f, 0.0f, 0.0f};
    // S8b: the reflection sees the frame's screen lights too (the same table, a copy in this slot), so a floor mirror
    // shows the LED wall's light in its reflection
    sc.screenLightParams = screenReady_ ? sc.screenLightParams : DirectX::XMFLOAT4{-1.0f, 0.0f, -1.0f, 0.0f};
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));
    memcpy(lightBufMapped_ + (size_t)cbSlot * kLightBytes,
           lightBufMapped_ + (size_t)ctx_->FrameSlot() * kLightBytes, kLightBytes);
    if (goboActive_)
        memcpy(goboBufMapped_ + (size_t)cbSlot * kMaxPunctualLights * sizeof(GpuGobo),
               goboBufMapped_ + (size_t)ctx_->FrameSlot() * kMaxPunctualLights * sizeof(GpuGobo),
               kMaxPunctualLights * sizeof(GpuGobo));
    if (screenReady_)
        memcpy(screenLightBufMapped_ + (size_t)cbSlot * kMaxScreenLights * sizeof(GpuScreenLight),
               screenLightBufMapped_ + (size_t)ctx_->FrameSlot() * kMaxScreenLights * sizeof(GpuScreenLight),
               kMaxScreenLights * sizeof(GpuScreenLight));

    RenderSettings mirrorSettings = settings_;
    mirrorSettings.renderPath = RenderPath::Raster;
    mirrorSettings.taa = false;
    mirrorSettings.dof = false;
    mirrorSettings.volumetric = false;
    mirrorSettings.ssr = false;
    mirrorSettings.packEffects.clear();
    mirrorSettings.mmeEffects.clear();
    const RenderStats savedStats = stats_;
    PassContext pc{*ctx_, cmd, view, mirrorSettings, targets_, stats_, mirrorTransient_,
                   sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                   lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes,
                   ctx_->FrameNumber(), false, false, &builtin_, RenderPath::Raster, nullptr, nullptr,
                   0.0f, 0.0f, frameTimeMs_};
    pc.fogVolumes = 0;
    stats_.drawCalls = 0;
    stats_.triangles = 0;
    const bool drew = scenePass_->DrawFeed(pc, m.color, m.normal, m.velocity, m.depth);
    stats_ = savedStats;
    return drew;
}

// S10: picks this frame's mirror planes and renders their reflections. A mirror material's plane was fitted once at
// load in model space (GpuModel::MirrorPlanes); here the root transform makes it a world plane, the planes facing the
// eye and on screen are ranked by projected size, and the at most kMaxMirrorPlanes largest get a slot. Zero cost
// without mirror materials: no plane, no texture, no extra render, and the constants keep mirrorParams.x = 0.
void Renderer::UpdateMirrors(ID3D12GraphicsCommandList* cmd, const FrameView& view, SceneConstants& sc, uint32_t w,
                             uint32_t h) {
    mirrorCount_ = 0;
    // Zero cost without mirror materials: no model has a fitted mirror plane, so the constants keep what
    // FillSceneConstants left (zero: no mirror, clip off) and the frame is byte for byte the one without the feature.
    bool anyMirror = false;
    for (const GpuModel* model : view.models)
        if (model && !model->MirrorPlanes().empty()) {
            anyMirror = true;
            break;
        }
    if (!anyMirror) return;
    sc.mirrorParams = {0.0f, -1.0f, -1.0f, 0.0f};
    sc.clipPlane = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < kMaxMirrorPlanes; ++i) {
        XMStoreFloat4x4(&sc.mirrorViewProj[i], XMMatrixIdentity());
        sc.mirrorPlane[i] = {0.0f, 0.0f, 0.0f, 0.0f};
    }
    if (!cmd || !scenePass_) return;

    const XMMATRIX viewM = XMLoadFloat4x4(&view.camera.view);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(view.camera.fovYRadians, (float)std::max(1u, w) / (float)std::max(1u, h),
                                                   view.camera.nearZ, view.camera.farZ);
    const XMMATRIX vp = viewM * proj;
    const XMVECTOR eye = XMLoadFloat3(&view.camera.eye);
    // the camera's world axes are the rows of the INVERSE view matrix (the view matrix's rows hold their components)
    const XMMATRIX camWorld = XMMatrixInverse(nullptr, viewM);
    const XMVECTOR fwd = XMVector3Normalize(camWorld.r[2]);
    const XMVECTOR up = XMVector3Normalize(camWorld.r[1]);

    struct Candidate {
        XMFLOAT4 plane{};
        float score = 0.0f;
    };
    Candidate cand[kMaxMirrorPlanes];
    uint32_t candCount = 0;
    for (const GpuModel* model : view.models) {
        if (!model || model->MirrorPlanes().empty()) continue;
        // The planes are model space: the model root (nesting the root transform the scene reports for this model,
        // identity when it has none) makes them world space.
        XMMATRIX root = XMMatrixIdentity();
        for (const MmeSceneObject& o : view.mmeObjects) {
            if (o.gpu != model) continue;
            root = XMLoadFloat4x4(&o.rootWorld);
            break;
        }
        const float det = XMVectorGetX(XMMatrixDeterminant(root));
        const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, root));
        const float scale = std::cbrt(std::fabs(det));
        for (const MirrorPlane& p : model->MirrorPlanes()) {
            if (!p.valid || !(p.area > 0.0f)) continue;
            const XMVECTOR c = XMVector3TransformCoord(XMLoadFloat3(&p.centroid), root);
            // the ordinary normal transform (inverse transpose), which is also what the scene shaders use, so a
            // model root with a reflection in it keeps the side the surface is actually drawn from
            const XMVECTOR n = XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&p.normal), normalMat));
            // the reflection is visible from the side the eye is on
            if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(eye, c))) <= 0.0f) continue;
            // ... and the plane has to reach into the view frustum: test the sphere (centroid, sqrt(area)) against the
            // six frustum planes, so a wall whose centre is off screen but which fills the view is kept.
            const float radius = std::sqrt(p.area) * scale;
            const XMMATRIX vpT = XMMatrixTranspose(vp);   // row-vector convention: a column of vp is a plane
            const XMVECTOR fr[6] = {
                vpT.r[3] + vpT.r[0], vpT.r[3] - vpT.r[0],   // left, right
                vpT.r[3] + vpT.r[1], vpT.r[3] - vpT.r[1],   // bottom, top
                vpT.r[2], vpT.r[3] - vpT.r[2]};             // near (z >= 0), far (z <= w)
            bool inView = true;
            for (const XMVECTOR& pl : fr) {
                const float len = XMVectorGetX(XMVector3Length(pl));
                if (!(len > 1e-6f)) continue;
                const float dist = XMVectorGetX(XMVector4Dot(XMVectorSetW(c, 1.0f), pl)) / len;
                if (dist < -radius) { inView = false; break; }
            }
            if (!inView) continue;
            // projected size: world area x foreshortening / distance^2
            const float dist = std::max(XMVectorGetX(XMVector3Length(XMVectorSubtract(c, eye))), 1.0f);
            const float face = std::fabs(XMVectorGetX(XMVector3Dot(n, XMVector3Normalize(XMVectorSubtract(eye, c)))));
            const float score = p.area * scale * scale * std::max(face, 0.05f) / (dist * dist);
            if (candCount < kMaxMirrorPlanes) {
                cand[candCount].plane = {XMVectorGetX(n), XMVectorGetY(n), XMVectorGetZ(n),
                                         -XMVectorGetX(XMVector3Dot(n, c))};
                cand[candCount].score = score;
                ++candCount;
            } else {
                uint32_t worst = 0;
                for (uint32_t i = 1; i < candCount; ++i)
                    if (cand[i].score < cand[worst].score) worst = i;
                if (score > cand[worst].score) {
                    cand[worst].plane = {XMVectorGetX(n), XMVectorGetY(n), XMVectorGetZ(n),
                                         -XMVectorGetX(XMVector3Dot(n, c))};
                    cand[worst].score = score;
                }
            }
        }
    }
    if (candCount == 0) {
        if (loggedMirrors_ != 0) {
            loggedMirrors_ = 0;
            LOG_INFO("MIRRORS count=0");
        }
        return;
    }
    // largest first (kMaxMirrorPlanes is 2: a fixed 2-slot sort keeps this allocation-free)
    if (candCount == 2 && cand[0].score < cand[1].score) std::swap(cand[0], cand[1]);

    const uint32_t mw = std::max(1u, w / 2), mh = std::max(1u, h / 2);
    uint32_t count = 0;
    for (uint32_t s = 0; s < candCount; ++s) {
        if (!EnsureMirrorTargets(s, mw, mh)) break;
        const XMVECTOR n = XMVectorSet(cand[s].plane.x, cand[s].plane.y, cand[s].plane.z, 0.0f);
        const float d = cand[s].plane.w;
        // the mirror plane through the reflection of the eye: reflect points / directions about it
        const auto reflectPoint = [&](XMVECTOR p) {
            return XMVectorSubtract(p, XMVectorScale(n, 2.0f * (XMVectorGetX(XMVector3Dot(n, p)) + d)));
        };
        const auto reflectDir = [&](XMVECTOR v) {
            return XMVectorSubtract(v, XMVectorScale(n, 2.0f * XMVectorGetX(XMVector3Dot(n, v))));
        };
        const XMVECTOR mEye = reflectPoint(eye);
        const XMVECTOR mTarget = reflectPoint(XMVectorAdd(eye, fwd));
        XMVECTOR mUp = reflectDir(up);
        if (XMVectorGetX(XMVector3Length(XMVector3Cross(XMVector3Normalize(XMVectorSubtract(mTarget, mEye)), mUp))) < 1e-3f)
            mUp = XMVectorSet(0, 1, 0, 0);   // looking straight at the plane: any up will do
        // An ordinary LookAt of the reflected camera: the winding stays normal, so the mirror pass needs no cull flip.
        const XMMATRIX mView = XMMatrixLookAtLH(mEye, mTarget, mUp);
        if (!RenderMirror(cmd, view, s, cand[s].plane, mView, proj, sc)) break;
        XMStoreFloat4x4(&sc.mirrorViewProj[s], mView * proj);
        sc.mirrorPlane[s] = cand[s].plane;
        ++count;
    }
    mirrorCount_ = count;
    if (count == 0) {
        sc.mirrorParams = {0.0f, -1.0f, -1.0f, 0.0f};
        if (loggedMirrors_ != 0) {
            loggedMirrors_ = 0;
            LOG_INFO("MIRRORS count=0");
        }
        return;
    }
    sc.mirrorParams = {(float)count, (float)mirrors_[0].srv,
                       count > 1 ? (float)mirrors_[1].srv : -1.0f, 0.0f};
    if (loggedMirrors_ != count) {
        loggedMirrors_ = count;
        LOG_INFO("MIRRORS count=%u resolution=%ux%u", count, mw, mh);
    }
}

void Renderer::ReadGpuTimer() {
    if (!timestampReadback_ || timestampFrequency_ == 0) return;
    uint32_t slot = ctx_->FrameSlot();
    D3D12_RANGE range{slot * 16, slot * 16 + 16};
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &range, &mapped))) return;
    const uint64_t* data = (const uint64_t*)mapped + slot * 2;
    if (data[1] > data[0]) stats_.gpuFrameMs = (float)((double)(data[1] - data[0]) * 1000.0 / (double)timestampFrequency_);
    D3D12_RANGE readRange{0, 0};
    timestampReadback_->Unmap(0, &readRange);
}

void Renderer::EnsureShadowMap(uint32_t size) {
    size = std::clamp(size, 512u, 4096u);
    if (targets_.shadowMap && targets_.shadowMap.width == size) return;
    ctx_->WaitForGpu();
    targets_.shadowMap.Create(*ctx_, size, size, RenderTargets::kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE, L"shadow.cascades", 1, kShadowCascades);
    const uint32_t spotSize = std::clamp(size / 4, 512u, 1024u);
    targets_.spotShadowMap.Create(*ctx_, spotSize, spotSize, RenderTargets::kDepthFormat,
                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                  L"shadow.spots", 1, kSpotShadowSlices);
}

void Renderer::EnsurePointShadowMap(const LightParams& light, bool offscreen) {
    const uint32_t count = PointShadowsWanted(settings_, EffectivePath(), offscreen)
                               ? PointShadowCount(light, kMaxPunctualLights) : 0;
    if (count == 0) return;
    // Faces shrink only when the lights no longer fit the full-resolution budget (see ShaderInterop.h).
    const uint64_t budgetLights = (uint64_t)kPointShadowFullResLights * (settings_.shadowMapSize >= 4096 ? 2 : 1);
    uint32_t size = kPointShadowMapSize;
    while (size > kPointShadowMinSize && (uint64_t)count * size * size > budgetLights * kPointShadowMapSize * kPointShadowMapSize)
        size /= 2;
    const uint32_t slices = count * kPointShadowFaces;
    // Grow only (no shrink when lights are switched off) unless the face size changes.
    if (targets_.pointShadowMap && targets_.pointShadowMap.width == size && targets_.pointShadowMap.arraySize >= slices) return;
    ctx_->WaitForGpu();
    if (!targets_.pointShadowMap.Create(*ctx_, size, size, RenderTargets::kDepthFormat,
                                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        L"shadow.points", 1, slices))
        targets_.pointShadowMap.Release(*ctx_);
}

void Renderer::EnsureTargets(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, uint32_t msaa) {
    if (targets_.width == width && targets_.height == height && targets_.msaa == msaa && targets_.colorMsaa &&
        targets_.outWidth == outWidth && targets_.outHeight == outHeight)
        return;
    ctx_->WaitForGpu();
    ReleaseTargets();
    Dx12Context& c = *ctx_;
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto rtUav = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const float zero[4] = {0, 0, 0, 0};
    bool ok = true;
    ok &= targets_.colorMsaa.Create(c, width, height, RenderTargets::kColorFormat, rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    L"scene.color.msaa", msaa, 1, zero);
    ok &= targets_.normalMsaa.Create(c, width, height, RenderTargets::kNormalFormat, rt, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     L"scene.normal.msaa", msaa, 1, zero);
    ok &= targets_.velocityMsaa.Create(c, width, height, RenderTargets::kVelocityFormat, rt,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, L"scene.velocity.msaa", msaa, 1, zero);
    ok &= targets_.depthMsaa.Create(c, width, height, RenderTargets::kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, L"scene.depth.msaa", msaa);
    ok &= targets_.color.Create(c, width, height, RenderTargets::kColorFormat, rtUav, srv, L"scene.color");
    ok &= targets_.normal.Create(c, width, height, RenderTargets::kNormalFormat, rtUav, srv, L"scene.normal");
    ok &= targets_.velocity.Create(c, width, height, RenderTargets::kVelocityFormat, rtUav, srv, L"scene.velocity");
    ok &= targets_.depth.Create(c, width, height, DXGI_FORMAT_R32_FLOAT, rtUav, srv, L"scene.depth");
    ok &= targets_.lit.Create(c, width, height, RenderTargets::kColorFormat, rtUav, srv, L"scene.lit");
    ok &= targets_.ldr.Create(c, outWidth, outHeight, RenderTargets::kLdrFormat, rt, srv, L"scene.ldr");
    if (!ok) {
        LOG_ERROR("render target creation failed (%ux%u -> %ux%u msaa=%u)", width, height, outWidth, outHeight, msaa);
        ReleaseTargets();
        return;
    }
    targets_.width = width;
    targets_.height = height;
    targets_.outWidth = outWidth;
    targets_.outHeight = outHeight;
    targets_.msaa = msaa;
    for (auto& pass : passes_) pass->OnResize(c, targets_);
    havePrev_ = false;
    LOG_INFO("render targets %ux%u -> %ux%u msaa=%u", width, height, outWidth, outHeight, msaa);
}

void Renderer::ReleaseTargets() {
    for (auto& pass : passes_) pass->ReleaseTargets(*ctx_);
    for (Texture* t : {&targets_.colorMsaa, &targets_.normalMsaa, &targets_.velocityMsaa, &targets_.depthMsaa,
                       &targets_.idMsaa, &targets_.color, &targets_.normal, &targets_.velocity, &targets_.depth,
                       &targets_.id, &targets_.lit, &targets_.ldr})
        t->Release(*ctx_);
    targets_.ao = targets_.ssr = targets_.hdrFinal = targets_.bloom = targets_.uiBackdrop = nullptr;
    targets_.idTarget = nullptr;
    targets_.width = targets_.height = targets_.outWidth = targets_.outHeight = 0;
    targets_.msaa = 1;
}

void Renderer::FillSceneConstants(const FrameView& view, uint32_t w, uint32_t h, bool offscreen, SceneConstants& sc) {
    const CameraParams& cam = view.camera;
    const float aspect = (float)w / (float)h;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(cam.fovYRadians, aspect, cam.nearZ, cam.farZ);
    XMMATRIX viewM = XMLoadFloat4x4(&cam.view);
    XMMATRIX vpNoJitter = viewM * proj;

    // A quad view (extra views) is a flat raster composite: no temporal jitter, no upscaler.
    const bool quad = !offscreen && !view.extraViews.empty() && EffectivePath() == RenderPath::Raster;
    IUpscaler* upscaler = (offscreen || quad) ? nullptr : EffectiveUpscaler();
    const bool jitter = !offscreen && !quad && (settings_.taa || upscaler || EffectivePath() == RenderPath::PathTraced);
    float jxPx = 0, jyPx = 0;
    if (jitter) {
        const uint32_t phases = upscaler ? IUpscaler::JitterPhaseCount(w, targets_.outWidth) : 8u;
        const uint32_t idx = temporalIndex_ % phases + 1;
        jxPx = Halton(idx, 2) - 0.5f;
        jyPx = Halton(idx, 3) - 0.5f;
    }
    jitterPx_[0] = jxPx;
    jitterPx_[1] = jyPx;
    const float jx = 2.0f * jxPx / (float)w;   // FSR convention
    const float jy = -2.0f * jyPx / (float)h;
    XMMATRIX projJ = proj;
    projJ.r[2] = XMVectorAdd(projJ.r[2], XMVectorSet(jx, jy, 0, 0));
    XMMATRIX viewProj = viewM * projJ;

    XMStoreFloat4x4(&sc.view, viewM);
    XMStoreFloat4x4(&sc.proj, projJ);
    XMStoreFloat4x4(&sc.viewProj, viewProj);
    XMStoreFloat4x4(&sc.invProj, XMMatrixInverse(nullptr, projJ));
    XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat4x4(&sc.invView, invView);
    XMStoreFloat4x4(&sc.viewProjNoJitter, vpNoJitter);
    const bool usePrev = !offscreen && havePrev_ && !view.cameraCut;
    if (usePrev)
        sc.prevViewProjNoJitter = prevViewProj_;
    else
        XMStoreFloat4x4(&sc.prevViewProjNoJitter, vpNoJitter);

    // --- cascaded shadow maps
    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&view.light.direction));
    const uint32_t mapSize = targets_.shadowMap ? targets_.shadowMap.width : 2048;
    const float n = cam.nearZ;
    const float f = std::max(n + 1.0f, std::min(view.shadowDistance > 0.0f ? view.shadowDistance : settings_.shadowDistance, cam.farZ));
    float splits[kShadowCascades + 1];
    for (uint32_t i = 0; i <= kShadowCascades; ++i) {
        const float p = (float)i / kShadowCascades;
        const float lin = n + (f - n) * p;
        const float lg = n * std::pow(f / n, p);
        splits[i] = 0.72f * lg + 0.28f * lin;
    }
    const float tanY = std::tan(cam.fovYRadians * 0.5f), tanX = tanY * aspect;
    const XMVECTOR up = std::fabs(view.light.direction.y) > 0.99f * XMVectorGetX(XMVector3Length(XMLoadFloat3(&view.light.direction)))
                            ? XMVectorSet(0, 0, 1, 0)
                            : XMVectorSet(0, 1, 0, 0);
    const float back = 250.0f;  // reach toward the light for casters outside the view
    float texel[4] = {};
    for (uint32_t c = 0; c < kShadowCascades; ++c) {
        XMVECTOR corners[8];
        int k = 0;
        for (float z : {splits[c], splits[c + 1]})
            for (float sy : {-1.0f, 1.0f})
                for (float sx : {-1.0f, 1.0f})
                    corners[k++] = XMVector3TransformCoord(XMVectorSet(sx * tanX * z, sy * tanY * z, z, 1), invView);
        XMVECTOR center = XMVectorZero();
        for (XMVECTOR v : corners) center = XMVectorAdd(center, v);
        center = XMVectorScale(center, 1.0f / 8.0f);
        float radius = 0;
        for (XMVECTOR v : corners) radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(v, center))));
        radius = std::ceil(radius * 16.0f) / 16.0f;
        XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(L, radius + back));
        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);
        XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back);
        // snap the projection to whole texels so the shadow does not shimmer while the camera moves
        XMMATRIX vp = lightView * ortho;
        XMVECTOR origin = XMVectorScale(XMVector3TransformCoord(XMVectorZero(), vp), mapSize * 0.5f);
        XMVECTOR offset = XMVectorScale(XMVectorSubtract(XMVectorRound(origin), origin), 2.0f / mapSize);
        ortho.r[3] = XMVectorAdd(ortho.r[3], XMVectorSet(XMVectorGetX(offset), XMVectorGetY(offset), 0, 0));
        XMStoreFloat4x4(&sc.shadowViewProj[c], lightView * ortho);
        texel[c] = 2.0f * radius / mapSize;
    }
    // spot shadow maps: a perspective frustum per spot cone (S7a: the 16 most important spots by
    // PickSpotShadowSlices; slice = the light's entry in that choice)
    uint32_t spotSlices = 0;
    std::vector<int> pick;
    if (targets_.spotShadowMap && SpotShadowsWanted(settings_, EffectivePath(), offscreen)) {
        // Offscreen renders (thumbnails, offline) must not move the on-screen choice's hysteresis state.
        const std::vector<int> keepPrev = offscreen ? prevSpotPick_ : std::vector<int>{};
        pick = PickSpotShadowSlices(view.light, cam, view.models, aspect);
        if (offscreen) prevSpotPick_ = keepPrev;
        for (size_t i = 0; i < pick.size(); ++i) {
            if (pick[i] < 0) continue;
            const PunctualLight& p = view.light.punctual[i];
            const XMVECTOR pos = XMLoadFloat3(&p.position);
            const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&p.direction));
            const XMVECTOR upV = std::fabs(XMVectorGetY(dir)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
            const float halfAngle = std::acos(std::clamp(p.spotCosOuter, -0.99f, 1.0f));
            const float fov = std::min(2.0f * halfAngle * 1.08f + 0.02f, XMConvertToRadians(170.0f));
            const XMMATRIX lv = XMMatrixLookToLH(pos, dir, upV);
            const XMMATRIX lp = XMMatrixPerspectiveFovLH(fov, 1.0f, std::max(0.05f, p.range * 0.004f), std::max(p.range, 1.0f));
            XMStoreFloat4x4(&sc.spotViewProj[pick[i]], lv * lp);
            spotSlices = std::max(spotSlices, (uint32_t)pick[i] + 1);
        }
    }
    spotSlices_ = std::move(pick);
    sc.spotShadowParams = {(float)spotSlices, 1.0f / (float)std::max(targets_.spotShadowMap.width, 1u), 0, 0};
    const bool sunShadowsOff = view.shadowsOff || (view.light.sunShadow == LightShadowType::NoCast);
    sc.cascadeSplits = {splits[1], splits[2], splits[3], settings_.shadows && !sunShadowsOff ? 1.0f : 0.0f};
    // z: the Hard filter radius in texels (also Soft's narrowest; Soft's PCSS widens it from the softness, mmd.hlsl)
    sc.shadowParams = {1.0f / mapSize, 1.2f, 1.6f, sunShadowsOff ? 1.0f : 0.0f};  // w: path tracer sun shadows off
    sc.cascadeTexel = {texel[0], texel[1], texel[2], back};
    sc.sunShadowParams = {(float)(uint8_t)view.light.sunShadow, view.light.sunShadowSoftness, view.light.sunShadowDensity, 0.0f};
    sc.sunShadowColor = {view.light.sunShadowColor.x, view.light.sunShadowColor.y, view.light.sunShadowColor.z, 0.0f};
    uint32_t pointCount = 0;
    if (targets_.pointShadowMap && PointShadowsWanted(settings_, EffectivePath(), offscreen)) {
        pointCount = PointShadowCount(view.light, kMaxPunctualLights);
    }
    sc.pointShadowParams = {(float)pointCount, 1.0f / (float)std::max(targets_.pointShadowMap.width ? targets_.pointShadowMap.width : kPointShadowMapSize, 1u), 0, 0};

    const LightParams& lp = view.light;
    sc.eyePos = cam.eye;
    sc.time = (float)temporalIndex_ / 60.0f;
    XMStoreFloat3(&sc.lightDir, L);
    sc.sunIntensity = lp.sunIntensity;
    sc.lightColor = lp.color;
    sc.hemiStrength = lp.hemiStrength;
    sc.skyZenith = lp.skyZenith;
    sc.rimStrength = lp.rimStrength;
    sc.skyHorizon = lp.skyHorizon;
    sc.numLights = (float)std::min<size_t>(lp.punctual.size(), kMaxPunctualLights);
    sc.groundColor = lp.groundColor;
    // S9b-1: the Physical Sky light (render/PhysicalSky.h evaluated the model in app/Lighting.cpp). With no Sky light
    // the fields stay zero and SkyColor keeps today's gradient: the frame is byte for byte the one before S9b-1.
    sc.skyParams = lp.physicalSky ? lp.sky.params : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skyPerezY = lp.physicalSky ? lp.sky.perezY : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skyPerezX = lp.physicalSky ? lp.sky.perezX : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skyPerezYy = lp.physicalSky ? lp.sky.perezYy : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skyPerezE = lp.physicalSky ? lp.sky.perezE : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skyZenithOverF = lp.physicalSky ? lp.sky.zenithOverF : DirectX::XMFLOAT4{0, 0, 0, 0};
    sc.skySunToward = lp.physicalSky ? lp.sky.sunToward : DirectX::XMFLOAT4{0, 1, 0, 0};
    sc.skyNight = lp.physicalSky ? lp.sky.night : DirectX::XMFLOAT4{0, 0, 0, 0};
    // S9b-3: the Sky light's image. A loaded map switches SkyColor to gSkyParams.x = 2 (the S9b-1 model stays behind it:
    // the sun direction / colour still follow the time of day) and its row averages replace the ambient colours, so the
    // hemisphere fallback and the haze follow the image. No image / a failed decode keeps the frame exactly the S9b-1 one.
    sc.skyMapParams = {-1.0f, 0.0f, 0.0f, 0.0f};
    sc.skyMapSize = {0.0f, 0.0f, 0.0f, 0.0f};
    if (lp.physicalSky && !lp.skyMap.empty()) {
        if (const SkyMapGpu* map = EnsureSkyMap(lp)) {
            sc.skyParams.x = 2.0f;
            sc.skyMapParams = {(float)(int32_t)map->srv, lp.skyMapRotationRad, lp.skyMapIntensity,
                               lp.skyMapMirror ? 1.0f : 0.0f};
            sc.skyMapSize = {(float)map->width, (float)map->height, 0.0f, 0.0f};
            const float k = lp.skyMapIntensity;
            sc.skyZenith = {map->avg[0].x * k, map->avg[0].y * k, map->avg[0].z * k};
            sc.skyHorizon = {map->avg[1].x * k, map->avg[1].y * k, map->avg[1].z * k};
            sc.groundColor = {map->avg[2].x * k, map->avg[2].y * k, map->avg[2].z * k};
        }
    }
    // S9b-4: the cloud layer of the procedural physical sky. Only when the S9b-1 model itself draws (x = 1): an image
    // sky (x = 2) and no sky at all leave both zero, so those frames stay byte-identical.
    sc.cloudParams = {0.0f, 0.0f, 0.0f, 0.0f};
    sc.cloudParams2 = {0.0f, 0.0f, 0.0f, 0.0f};
    if (lp.physicalSky && sc.skyParams.x > 0.5f && sc.skyParams.x < 1.5f) {
        sc.cloudParams = {lp.cloudCoverage, lp.cloudOpacity, lp.cloudSoftness, lp.cloudHeightKm};
        sc.cloudParams2 = {lp.cloudScaleKm, lp.cloudWindOffset.x, lp.cloudWindOffset.y, 0.0f};
    }
    // S9b-2: no sky environment until RecordScene (or the offline image) builds one for this frame
    sc.skyEnvParams = {(float)(int32_t)DescriptorHeap::kInvalid, (float)kSkyEnvW, (float)kSkyEnvH, 0.0f};
    sc.floorGloss = (settings_.ssr || EffectivePath() == RenderPath::PathTraced) ? settings_.floorGloss : 0.0f;
    sc.rimColor = lp.rimColor;
    sc.fog = settings_.fog;
    sc.viewportSize = {(float)w, (float)h};
    sc.edgeScale = (float)h / 1080.0f;
    sc.transparentBg = (settings_.transparentBackground || settings_.exportAlpha) ? 1.0f : 0.0f;
    sc.jitterUv = {jx * 0.5f, -jy * 0.5f};
    sc.invViewportSize = {1.0f / w, 1.0f / h};
    sc.nearZ = cam.nearZ;
    sc.farZ = cam.farZ;
    sc.frameIndex = (float)(temporalIndex_ % 64);
    // AutoLuminous (そぼろ): the global glow switch (settings_.autoLuminous) reaches every path through SceneConstants.
    sc.autoLumParams = {settings_.autoLuminous ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    // The DXR scene shaders read shading as 0 (Lit): RayTraced / PathTraced ignore the setting.
    // The quad view's extra views get their own shading from their SceneConstants below.
    uint32_t shading = (EffectivePath() == RenderPath::Raster) ? (uint32_t)settings_.shading : 0u;
    sc.shading = (float)shading;
    // Distance haze is a lighting-style contribution; Unlit/Wireframe scenes are drawn flat
    // (RecordScene turns the lighting-adjacent effects off for these frames).
    if (shading != 0u) sc.fog = 0.0f;
    sc.packInputs = view.packInputs;
    // S7c: no light clusters by default (FillSceneConstants is also the base for the camera feeds and the extra views,
    // whose cameras differ from the frame's cluster grid); RecordScene turns them on for the main view.
    sc.lightClusterParams = {-1.0f, 0.0f, 0.0f, 0.0f};
    sc.emissiveParams = {-1.0f, -1.0f, 0.0f, -1.0f};   // S8a: RecordScene sets it after the RtScene build
    sc.goboParams = {-1.0f, 0.0f, 0.0f, 0.0f};         // S7b: no gobos unless RecordScene filled the table
    // S8b: no screen lights unless RecordScene filled the table (a feed / extra view / RenderToImage keeps -1)
    sc.screenLightParams = {-1.0f, 0.0f, -1.0f, 0.0f};
}

uint32_t Renderer::FillGpuLights(const LightParams& light, GpuLight* out, const std::vector<int>* spotSlices,
                                uint32_t pointCap) {
    const size_t count = std::min<size_t>(light.punctual.size(), kMaxPunctualLights);
    uint32_t spot = 0;
    uint32_t pointIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        const PunctualLight& p = light.punctual[i];
        GpuLight& g = out[i];
        g.position = p.position;
        g.invRange = 1.0f / std::max(p.range, 0.01f);
        g.color = {p.color.x * p.intensity, p.color.y * p.intensity, p.color.z * p.intensity};
        g.spotCosOuter = p.spotCosOuter;
        XMStoreFloat3(&g.direction, XMVector3Normalize(XMLoadFloat3(&p.direction)));
        g.spotCosInner = std::max(p.spotCosInner, p.spotCosOuter + 1e-3f);
        // S7a: the renderer's importance choice decides the slice; without it, the first kSpotShadowSlices spots.
        if (spotSlices) {
            g.shadowSlice = (i < spotSlices->size() && (*spotSlices)[i] >= 0) ? (float)(*spotSlices)[i] : -1.0f;
        } else {
            g.shadowSlice = (p.spotCosOuter > -1.0f && p.shadow != LightShadowType::NoCast && spot < kSpotShadowSlices)
                                ? (float)spot : -1.0f;
            if (p.spotCosOuter > -1.0f && p.shadow != LightShadowType::NoCast) ++spot;
        }
        g.shadowType = (float)(uint8_t)p.shadow;
        g.shadowSoftness = std::clamp(p.shadowSoftness, 0.0f, 1.0f);
        g.shadowDensity = std::clamp(p.shadowDensity, 0.0f, 1.0f);
        g.shadowColor = p.shadowColor;
        g.falloff = (float)(uint8_t)p.falloff;
        g.affectDiffuse = p.affectDiffuse ? 1.0f : 0.0f;
        g.affectSpecular = p.affectSpecular ? 1.0f : 0.0f;
        // Raster samples slices of the point shadow map (sized for every shadowed light by
        // EnsurePointShadowMap); the ray-traced paths only read the slice as a "casts" flag.
        if (p.spotCosOuter <= -1.0f && p.castPointShadow && p.shadow != LightShadowType::NoCast && pointIndex < pointCap) {
            g.pointShadowSlice = (float)(pointIndex * 6);
            ++pointIndex;
        } else {
            g.pointShadowSlice = -1.0f;
        }
        g._pad = p.volumetric;
        g.areaWidth = p.areaSize.x;
        g.areaHeight = p.areaSize.y;
        g.isArea = (p.areaSize.x > 0.0f && p.areaSize.y > 0.0f) ? 1.0f : 0.0f;
        g._pad2 = p.volumetricG;
    }
    return (uint32_t)count;
}

// S9: the local fog volumes of the frame (GpuFogVolume, see ShaderInterop.h). Zeroed beyond the count by the caller.
uint32_t Renderer::FillGpuFogVolumes(const LightParams& light, GpuFogVolume* out) {
    const size_t count = std::min<size_t>(light.fogVolumes.size(), kMaxFogVolumes);
    for (size_t i = 0; i < count; ++i) {
        const FogVolume& f = light.fogVolumes[i];
        GpuFogVolume& g = out[i];
        g.center = f.position;
        g.density = std::max(f.density, 0.0f);
        g.extent = {std::max(std::fabs(f.extent.x), 1e-3f), std::max(std::fabs(f.extent.y), 1e-3f),
                    std::max(std::fabs(f.extent.z), 1e-3f)};
        g.edge = std::clamp(f.edge, 0.0f, 1.0f);
        g.color = f.color;
        g.sphere = f.sphere ? 1 : 0;
    }
    return (uint32_t)count;
}

// S7a: the spot shadow budget. At most kSpotShadowSlices spots cast shadows (the map has one slice per light); the
// rest are unshadowed in raster. A spot that reaches no caster takes no slice: its map would be empty.
std::vector<int> Renderer::PickSpotShadowSlices(const LightParams& light, const CameraParams& camera,
                                                const std::vector<GpuModel*>& models, float aspect) {
    const size_t count = std::min<size_t>(light.punctual.size(), kMaxPunctualLights);
    std::vector<SpotCaster> casters;
    for (const GpuModel* model : models) {
        if (!model || model->Role() != ModelRole::Character) continue;
        bool casts = false;
        for (const GpuModel::Material& m : model->Materials()) casts = casts || (m.castShadow && m.indexCount > 0);
        SpotCaster c;
        if (!casts || !model->LiveBoundingSphere(c.center, c.radius)) continue;
        c.viewWeight = CasterViewWeight(camera, aspect, c.center, c.radius);
        casters.push_back(c);
    }
    std::vector<float> score(count, 0.0f);
    for (size_t i = 0; i < count; ++i) score[i] = SpotShadowScore(light.punctual[i], casters, settings_.volumetric);
    std::vector<int> pick = PickSpotSlices(score, prevSpotPick_, kSpotShadowSlices);
    prevSpotPick_ = pick;
    return pick;
}

// S7a: the light pick CDF of the frame (see ShaderInterop.h): weight = intensity * luminance(colour) * range^2 and,
// for a spot, the cone solid-angle proxy (1 - cosOuter) * 2 + 0.05. Inclusive running sum normalised to 1; a frame
// without any weight gets a uniform CDF, so the sample is still defined.
uint32_t Renderer::FillLightCdf(const LightParams& light, float* out, uint32_t capacity) {
    const size_t count = std::min<size_t>(light.punctual.size(), std::min<size_t>(kMaxPunctualLights, capacity));
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const PunctualLight& p = light.punctual[i];
        const float lum = 0.2126f * p.color.x + 0.7152f * p.color.y + 0.0722f * p.color.z;
        float w = p.intensity * lum * std::max(p.range, 0.0f) * std::max(p.range, 0.0f);
        if (p.spotCosOuter > -1.0f) w *= (1.0f - std::clamp(p.spotCosOuter, -1.0f, 1.0f)) * 2.0f + 0.05f;
        if (!(w > 0.0f)) w = 0.0f;
        out[i] = w;
        total += w;
    }
    if (count == 0) return 0;
    if (!(total > 0.0)) {
        for (size_t i = 0; i < count; ++i) out[i] = (float)(i + 1) / (float)count;
        return (uint32_t)count;
    }
    double acc = 0.0;
    for (size_t i = 0; i < count; ++i) {
        acc += out[i];
        out[i] = (float)(acc / total);
    }
    out[count - 1] = 1.0f;
    return (uint32_t)count;
}

// S7b: the frame's spot gobo table (see ShaderInterop.h GpuGobo / shaders/gobo.hlsli). One entry per punctual light: the
// gobo texture's SrvHeap index (-1 = this light has none), the rotation in radians (authored degrees + spin x time),
// the uv scale and the focus's mip bias. A file is decoded and uploaded once (one texture, full mip chain, an _SRGB view
// so the sample decodes to the linear space the light colour is in); a missing / undecodable file warns once and leaves
// the light without a gobo. Returns false when the frame has no gobo at all, which leaves
// SceneConstants::goboParams.x = -1 and the shaders skip the fetch entirely.
bool Renderer::FillGoboTable(Dx12Context& ctx, const LightParams& light, GpuGobo* out, float time) {
    const size_t count = std::min<size_t>(light.punctual.size(), kMaxPunctualLights);
    // the whole table is reset (the upload copies every entry: a stale slot must read "no gobo", not heap index 0)
    for (size_t i = 0; i < kMaxPunctualLights; ++i) out[i] = GpuGobo{-1.0f, 0.0f, 1.0f, 0.0f};
    bool any = false;
    goboUsed_.clear();
    for (size_t i = 0; i < count; ++i) {
        const PunctualLight& p = light.punctual[i];
        GpuGobo& g = out[i];
        // only a spot projects: a point / area light has no cone for a stencil
        if (p.gobo.empty() || p.spotCosOuter <= -1.0f) continue;
        const std::wstring key = Utf8ToWide(p.gobo);
        auto it = gobos_.find(key);
        if (it == gobos_.end()) {
            GoboTexture tex;
            ImageRGBA8 image;
            std::string error;
            if (!LoadImageRGBA8(Utf8ToPath(p.gobo), image, &error) || image.Empty()) {
                LOG_WARN("gobo %s: %s (the light shines without a gobo)", p.gobo.c_str(),
                         error.empty() ? "cannot decode the image" : error.c_str());
                tex.failed = true;
            } else {
                UploadBatch batch(ctx);
                // a typeless resource so the SRV below is _SRGB: the sample then decodes to linear, the space the
                // light's colour is in (a stencil authored in sRGB stays perceptually correct on a floor)
                tex.res = batch.CreateTextureTyped(image, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"gobo");
                batch.Submit();
                if (!tex.res) {
                    LOG_WARN("gobo %s: texture upload failed (the light shines without a gobo)", p.gobo.c_str());
                    tex.failed = true;
                } else {
                    tex.srv = ctx.SrvHeap().Allocate(1);
                    if (tex.srv == DescriptorHeap::kInvalid) {
                        LOG_WARN("gobo %s: no free descriptor (the light shines without a gobo)", p.gobo.c_str());
                        tex.res.Reset();
                        tex.failed = true;
                    } else {
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv.Texture2D.MipLevels = UINT(-1);
                        ctx.Device()->CreateShaderResourceView(tex.res.Get(), &srv, ctx.SrvHeap().Cpu(tex.srv));
                        LOG_INFO("GOBO file=%s %ux%u", p.gobo.c_str(), image.Width(), image.Height());
                    }
                }
            }
            it = gobos_.emplace(key, std::move(tex)).first;
        }
        goboUsed_.push_back(key);
        if (it->second.failed || it->second.srv == DescriptorHeap::kInvalid) continue;
        g.srv = (float)it->second.srv;
        g.rotation = DirectX::XMConvertToRadians(p.goboRotation) + DirectX::XMConvertToRadians(p.goboSpin) * time;
        g.scale = std::max(p.goboScale, 1e-3f);
        // focus 1 = sharp (mip 0); below it blurs toward the coarsest mip (6 covers a 2048-wide texture)
        g.mipBias = std::max(0.0f, std::min(1.0f, p.goboFocus));
        g.mipBias = (1.0f - g.mipBias) * 6.0f;
        any = true;
    }
    // a file that no light of the frame uses any more frees its texture (a stage change, a new gobo picked)
    if (gobos_.size() > goboUsed_.size()) {
        std::vector<std::wstring> stale;
        for (auto& [path, g] : gobos_)
            if (std::find(goboUsed_.begin(), goboUsed_.end(), path) == goboUsed_.end()) stale.push_back(path);
        for (const std::wstring& path : stale) {
            auto it = gobos_.find(path);
            if (it != gobos_.end()) {
                if (it->second.res) ctx.DeferRelease(std::move(it->second.res));
                if (it->second.srv != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(it->second.srv, 1);
                gobos_.erase(it);
            }
        }
    }
    return any;
}

// S9b-3: the Sky light's image (render/SkyMap.h). One decode + upload per (path, encoding), kept for the renderer's
// lifetime: a sky map is a single texture, so unlike the gobos there is no per-frame used list to free. A failed file is
// remembered (failed = true): one [W] per broken file, then the procedural sky draws as if no image were set.
const Renderer::SkyMapGpu* Renderer::EnsureSkyMap(const LightParams& light) {
    if (light.skyMap.empty()) return nullptr;
    const std::string key = light.skyMap + "|" + std::to_string(light.skyMapEncoding);
    auto it = skyMaps_.find(key);
    if (it == skyMaps_.end()) {
        SkyMapGpu map;
        std::vector<uint16_t> halves;
        std::string error;
        if (!LoadSkyMap(Utf8ToPath(light.skyMap), light.skyMapEncoding, halves, map.width, map.height, map.avg,
                        &error)) {
            LOG_WARN("sky map %s: %s (the sky draws procedurally)", light.skyMap.c_str(),
                     error.empty() ? "cannot decode the image" : error.c_str());
            map.failed = true;
        } else {
            UploadBatch batch(*ctx_);
            map.res = batch.CreateTextureRaw(halves.data(), map.width, map.height, 8, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             L"skyMap");
            batch.Submit();
            if (!map.res) {
                LOG_WARN("sky map %s: texture upload failed (the sky draws procedurally)", light.skyMap.c_str());
                map.failed = true;
            } else {
                map.srv = ctx_->SrvHeap().Allocate(1);
                if (map.srv == DescriptorHeap::kInvalid) {
                    LOG_WARN("sky map %s: no free descriptor (the sky draws procedurally)", light.skyMap.c_str());
                    map.res.Reset();
                    map.failed = true;
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    ctx_->Device()->CreateShaderResourceView(map.res.Get(), &srv, ctx_->SrvHeap().Cpu(map.srv));
                    LOG_INFO("SKYMAP %s %ux%u enc=%d", light.skyMap.c_str(), map.width, map.height,
                             light.skyMapEncoding);
                }
            }
        }
        it = skyMaps_.emplace(key, std::move(map)).first;
    }
    if (it->second.failed || it->second.srv == DescriptorHeap::kInvalid) return nullptr;
    return &it->second;
} 

// S8a: the emissive-triangle pick table of the last RtScene build. Per geometry (index = geometry id) the material's
// rest-pose triangles are appended with weight = rest-pose area x EmitEstimate, and the running sum is normalised into an
// inclusive CDF. Above kEmissiveTriMax entries the largest-weight geometries win (a geometry's triangles stay together,
// so the per-geometry base table stays a contiguous range); a zero total falls back to a uniform CDF over the entries.
uint32_t Renderer::BuildEmissiveTable(const std::vector<RtScene::GeometryRef>& geometries, EmissiveTable& out,
                                      bool& warned) {
    out.tris.clear();
    out.cdf.clear();
    out.base.assign(std::min<size_t>(std::max<size_t>(geometries.size(), 1), kEmissiveBaseMax), kEmissiveTriNone);
    if (geometries.size() > kEmissiveBaseMax) {
        if (!warned) LOG_WARN("emissive NEE: %zu geometries exceed the base table (%u), the extra ones do not emit",
                              geometries.size(), kEmissiveBaseMax);
        warned = true;
    }

    // Per geometry: its first triangle's weight and its triangle count (a material's triangles are contiguous).
    struct Cand {
        uint32_t geometry;
        uint32_t first;
        uint32_t count;
        double weight;
    };
    std::vector<Cand> cands;
    cands.reserve(geometries.size());
    for (size_t gi = 0; gi < geometries.size() && gi < kEmissiveBaseMax; ++gi) {
        const RtScene::GeometryRef& ref = geometries[gi];
        if (!ref.model) continue;
        const float estimate = ref.model->EmitEstimate(ref.material);
        if (!(estimate > 0.0f)) continue;
        const auto& mats = ref.model->Materials();
        if (ref.material >= mats.size()) continue;
        const std::vector<float>& areas = ref.model->TriAreas();
        const uint32_t triCount = mats[ref.material].indexCount / 3;
        const uint32_t firstTri = mats[ref.material].indexStart / 3;
        if (triCount == 0 || (size_t)firstTri + triCount > areas.size()) continue;
        double w = 0.0;
        for (uint32_t t = 0; t < triCount; ++t) w += std::max(areas[firstTri + t], 0.0f);
        if (!(w > 0.0)) continue;
        cands.push_back({(uint32_t)gi, firstTri, triCount, w * (double)estimate});
    }
    if (cands.empty()) return 0;

    // Cap: keep the heaviest geometries (stable: ties keep geometry order) until the entry budget is spent.
    size_t total = 0;
    for (const Cand& c : cands) total += c.count;
    if (total > kEmissiveTriMax) {
        if (!warned) {
            LOG_WARN("emissive NEE: %zu triangles exceed the pick table (%u), the largest-area materials are kept",
                     total, kEmissiveTriMax);
            warned = true;
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.weight > b.weight; });
        size_t kept = 0, count = 0;
        while (kept < cands.size() && count + cands[kept].count <= kEmissiveTriMax) count += cands[kept++].count;
        if (kept == 0) {
            // a single material larger than the whole table: keep the start of it (a partial CDF over its triangles)
            cands[0].count = kEmissiveTriMax;
            kept = 1;
        }
        cands.resize(kept);
        // Back to geometry order, so the base table and the entry list read in one pass.
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.geometry < b.geometry; });
    }

    size_t totalEntries = 0;
    for (const Cand& c : cands) totalEntries += c.count;
    out.tris.reserve(totalEntries);
    out.cdf.resize(totalEntries);
    std::vector<double> weights(totalEntries, 0.0);
    double totalWeight = 0.0;
    for (const Cand& c : cands) {
        const uint32_t first = (uint32_t)out.tris.size();
        out.base[c.geometry] = first;
        const float estimate = geometries[c.geometry].model->EmitEstimate(geometries[c.geometry].material);
        const std::vector<float>& areas = geometries[c.geometry].model->TriAreas();
        for (uint32_t t = 0; t < c.count; ++t) {
            out.tris.push_back({c.geometry, t});
            const double w = (double)std::max(areas[c.first + t], 0.0f) * (double)estimate;
            weights[first + t] = w;
            totalWeight += w;
        }
    }
    // Inclusive running sum, normalised to 1 (the last entry is exactly 1).
    double acc = 0.0;
    for (size_t i = 0; i < totalEntries; ++i) {
        acc += weights[i];
        out.cdf[i] = totalWeight > 0.0 ? (float)(acc / totalWeight) : (float)(i + 1) / (float)totalEntries;
    }
    out.cdf[totalEntries - 1] = 1.0f;
    return (uint32_t)totalEntries;
}

// S8a: uploads the emissive pick table of the geometry table the last RtScene::Build produced into `cbSlot`'s buffers.
// The table itself is rebuilt only when its key changes (geometry list + per-material emission estimates), so a static
// stage does not rebuild it every frame; a bound screen texture or a runtime PBR override changes the key and the next
// frame picks it up. A slot that does not hold the current key gets it uploaded again (the slots rotate), so a slot bound
// by a frame in flight always reads the table this frame's SceneConstants describe.
void Renderer::UpdateEmissiveTable(uint32_t cbSlot) {
    emissiveParams_ = {-1.0f, -1.0f, 0.0f, -1.0f};
    if (!rt_ || !emissiveTriBuf_ || cbSlot >= emissiveSrvCount_) return;
    const std::vector<RtScene::GeometryRef>& refs = rt_->GeometryRefs();
    if (refs.empty()) return;

    // Key: geometry count plus, per geometry, the model uid, the material and the emission estimate (bit pattern).
    uint64_t key = 1469598103934665603ull;   // FNV-1a
    const auto fold = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    fold(refs.size());
    for (const RtScene::GeometryRef& r : refs) {
        fold(r.model ? r.model->Uid() : 0ull);
        fold((uint64_t)r.material);
        const float e = r.model ? r.model->EmitEstimate(r.material) : 0.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &e, sizeof(bits));
        fold(bits);
    }
    // A key of 0 would read as "nothing built": keep it out of the way by folding it to 1.
    if (key == 0) key = 1;
    if (key != emissiveKey_) {
        emissiveKey_ = key;
        emissiveCount_ = BuildEmissiveTable(refs, emissiveTable_, emissiveWarned_);
        std::fill(std::begin(emissiveSlotKey_), std::end(emissiveSlotKey_), 0ull);
        if (emissiveCount_ > 0)
            LOG_INFO("EMISSIVE tris=%u emitterGeometries=%u", emissiveCount_, (uint32_t)emissiveTable_.base.size());
    }
    if (emissiveCount_ == 0) return;
    if (emissiveSlotKey_[cbSlot] != emissiveKey_) {
        std::memcpy(emissiveTriMapped_ + (size_t)cbSlot * kEmissiveTriBytes, emissiveTable_.tris.data(),
                    (size_t)emissiveCount_ * sizeof(GpuEmissiveTri));
        std::memcpy(emissiveCdfMapped_ + (size_t)cbSlot * kEmissiveTriMax, emissiveTable_.cdf.data(),
                    (size_t)emissiveCount_ * sizeof(float));
        // the base table is indexed by geometry id: unset entries must read 0xFFFFFFFF, not a stale range
        std::fill_n(emissiveBaseMapped_ + (size_t)cbSlot * kEmissiveBaseMax, kEmissiveBaseMax, kEmissiveTriNone);
        const size_t baseCount = std::min<size_t>(emissiveTable_.base.size(), kEmissiveBaseMax);
        if (baseCount)
            std::memcpy(emissiveBaseMapped_ + (size_t)cbSlot * kEmissiveBaseMax, emissiveTable_.base.data(),
                        baseCount * sizeof(uint32_t));
        emissiveSlotKey_[cbSlot] = emissiveKey_;
    }
    emissiveParams_ = {(float)emissiveTriSrv_[cbSlot], (float)emissiveCdfSrv_[cbSlot], (float)emissiveCount_,
                       (float)emissiveBaseSrv_[cbSlot]};
}

void Renderer::RecordScene(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t w, uint32_t h,
                           uint32_t cbSlot, uint64_t frame, bool offscreen) {
    EnsurePointShadowMap(view.light, offscreen);
    DrainMmeForget();
    SceneConstants sc{};
    FillSceneConstants(view, w, h, offscreen, sc);
    // S7a: the frame's light CDF (the PT / GI pick) lives in a raw SRV buffer of this slot
    const uint32_t cdfCount = FillLightCdf(view.light, lightCdfMapped_ + (size_t)cbSlot * kLightCdfMax, kLightCdfMax);
    sc.lightCdfParams = {(float)lightCdfSrv_[cbSlot], (float)cdfCount, 0.0f, 0.0f};
    // S7b: the frame's spot gobos (the table of this slot through the bindless buffer table; -1 = no gobo in the frame,
    // which keeps the old image at zero cost). Spin uses the scene clock, so a seeked offline / capture frame is exact.
    {
        std::vector<GpuGobo>& gobos = goboTable_;
        const bool any = FillGoboTable(*ctx_, view.light, gobos.data(), view.sceneTime);
        goboActive_ = any;
        if (any) {
            memcpy(goboBufMapped_ + (size_t)cbSlot * kMaxPunctualLights * sizeof(GpuGobo), gobos.data(),
                   kMaxPunctualLights * sizeof(GpuGobo));
            sc.goboParams = {(float)goboSrv_[cbSlot], 0.0f, 0.0f, 0.0f};
        } else {
            sc.goboParams = {-1.0f, 0.0f, 0.0f, 0.0f};
        }
    }
    // S7c: clustered light culling for the main camera view: only above kClusterMinLights lights (a frame with fewer
    // keeps the full per-light loop, byte for byte), never offscreen (RenderToImage), and only with the compute path
    // (PassLightCull's pipeline; the shadow map of the frame is not needed: the cull is frustum-only).
    const uint32_t lightCount = (uint32_t)std::min<size_t>(view.light.punctual.size(), kMaxPunctualLights);
    float clusterScale = 0.0f, clusterBias = 0.0f;
    const bool clusterSlices = ClusterSliceParams(sc.nearZ, sc.farZ, clusterScale, clusterBias);
    const bool clusters = lightCount > kClusterMinLights && !offscreen && clusterSlices &&
                          EffectivePath() != RenderPath::PathTraced && lightClusterBuf_ &&
                          cbSlot < lightClusterSrvCount_;
    sc.lightClusterParams = {(float)(clusters ? (int32_t)lightClusterSrv_[cbSlot] : -1), clusters ? ClusterGridPacked() : 0.0f,
                             clusterScale, clusterBias};
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));
    // S7a: 256 lights x 112 B does not belong on the stack
    static thread_local std::vector<GpuLight> lights(kMaxPunctualLights);
    std::fill(lights.begin(), lights.end(), GpuLight{});
    const uint32_t pointCap = EffectivePath() == RenderPath::Raster
                                  ? (targets_.pointShadowMap ? targets_.pointShadowMap.arraySize / kPointShadowFaces : 0u)
                                  : kMaxPunctualLights;
    FillGpuLights(view.light, lights.data(), &spotSlices_, pointCap);
    memcpy(lightBufMapped_ + (size_t)cbSlot * kLightBytes, lights.data(), lights.size() * sizeof(GpuLight));
    // S7a: one LIGHTS line per scene (the punctual setup changed): count, shadowed spots and shadowed points. The
    // shadowed spots follow the performers, so a change of that count alone is not a new line; it only logs a new high.
    {
        uint32_t spotCount = 0;
        for (int s : spotSlices_) spotCount += s >= 0 ? 1u : 0u;
        const uint32_t pointCount = PointShadowCount(view.light, kMaxPunctualLights);
        const uint32_t clusterGrid = (uint32_t)sc.lightClusterParams.y;
        if (lightCount != loggedLights_[0] || spotCount > loggedLights_[1] || pointCount != loggedLights_[2] ||
            clusterGrid != loggedClusterGrid_) {
            if (lightCount != loggedLights_[0]) loggedLights_[1] = 0;
            loggedLights_[0] = lightCount;
            loggedLights_[1] = std::max(loggedLights_[1], spotCount);
            loggedLights_[2] = pointCount;
            loggedClusterGrid_ = clusterGrid;
            // S7c: clusters = the scene shaders read a froxel list this frame; maxPerCluster is not read back from the
            // GPU (no GPU->CPU stall per scene), so the field stays a '?'
            LOG_INFO("LIGHTS count=%u spots=%u points=%u clusters=%u maxPerCluster=?", lightCount, spotCount,
                     pointCount, clusters ? 1u : 0u);
        }
    }
    // S9: the frame's local fog volumes (zeroed beyond the count, so a stale slot never lingers)
    GpuFogVolume fog[kMaxFogVolumes] = {};
    const uint32_t fogCount = FillGpuFogVolumes(view.light, fog);
    memcpy(fogBufMapped_ + (size_t)cbSlot * kFogBytes, fog, sizeof(fog));

    // S5: the stage screens' sources + camera feeds, then their texture bindings on the stage materials. Runs before
    // the acceleration structures are built so a ray-traced frame sees this frame's screen material flags.
    UpdateStageScreens(cmd, view, sc);

    // S8b: the stage screens as rectangular area lights (raster / RT only; PT / offline GI keep S8a's emissive NEE).
    // Fills sc.screenLightParams and the frame's screen-light table. Runs after the screens bound their textures (the
    // average-colour pass samples them) and before the scene passes read the table; the constants go up again here.
    UpdateScreenLights(cmd, view, sc, cbSlot);
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));

    // S9b-2: the sky environment (the Physical Sky's CDFs and SH irradiance) for the scene shaders and the path
    // tracers. Built after this slot's constants hold this frame's sky (the passes read b0) and before the mirror
    // views draw (they shade with the same slot's constants), then the constants go up once more.
    {
        PassContext skyPc{*ctx_, cmd, view, settings_, targets_, stats_, transient_,
                          sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                          lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes, frame, false, offscreen,
                          &builtin_, RenderPath::Raster, nullptr, nullptr, 0.0f, 0.0f, frameTimeMs_};
        skyEnv_.Build(skyPc, sc);
    }
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));

    // S10: the frame's planar mirrors (raster main views only; the ray paths reflect at the hit instead, and a quad /
    // ortho view, a camera feed or RenderToImage shows the tint). Rendered after the screens bound their textures so a
    // mirror sees this frame's LED walls, and before the scene passes that sample the reflection textures. Fills the
    // mirror fields of `sc`, so the constants are uploaded again right after.
    const bool mirrorView = !offscreen && EffectivePath() == RenderPath::Raster && view.extraViews.empty() &&
                            settings_.shading == ViewShading::Lit;
    if (mirrorView) {
        UpdateMirrors(cmd, view, sc, w, h);
        memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));
    }

    // Temporal history survives only consecutive on-screen frames without a camera cut.
    bool historyValid = !offscreen && havePrev_ && prevTaa_ && settings_.taa && !view.cameraCut;
    if (historyValid && XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&view.camera.eye),
                                                                      XMLoadFloat3(&prevEye_)))) > 12.0f)
        historyValid = false;

    // Build the acceleration structures for this frame; without them there is nothing to trace,
    // so fall back to raster for this frame.
    RenderPath path = offscreen ? RenderPath::Raster : EffectivePath();
    RtScene* rt = nullptr;
    if (path != RenderPath::Raster && rt_) {
        if (rt_->Build(cmd, view.models, frame, cbSlot))
            rt = rt_.get();
        else
            path = RenderPath::Raster;
    }
    // S8a: the emissive-triangle pick table of the geometry table just built (the path tracers' emissive NEE). Filled
    // after the build (it walks the same RtGeometry order) and before the scene constants are uploaded a second time.
    if (rt) {
        UpdateEmissiveTable(cbSlot);
        sc.emissiveParams = emissiveParams_;
        memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));
    }
    const bool quad = !offscreen && !view.extraViews.empty() && path == RenderPath::Raster;
    IUpscaler* up = (offscreen || quad) ? nullptr : EffectiveUpscaler();

    // Unlit / Wireframe draw the models flat, so the lighting-adjacent effects have nothing to
    // contribute this frame: run the frame with them off (a copy, RenderSettings is per-call here).
    // The shading mode is the camera view's: in a quad view the extra views are drawn flat through
    // their own SceneConstants, and the camera view keeps the selected shading.
    RenderSettings frameSettings = settings_;
    if (settings_.exportAlpha) {
        frameSettings.transparentBackground = true;   // sky writes alpha 0, colour premultiplied
        frameSettings.fog = 0.0f;                     // no distance haze: it would tint the transparent background
    }
    const bool nonLit = path == RenderPath::Raster && settings_.shading != ViewShading::Lit;
    if (quad) {
        frameSettings.taa = false;
        frameSettings.volumetric = false;  // the march follows the frame camera: wrong in the extra views
        historyValid = false;
    }
    if (nonLit) {
        frameSettings.shadows = false;
        frameSettings.ssao = false;
        frameSettings.ssr = false;
        frameSettings.volumetric = false;
        frameSettings.bloom = false;
        frameSettings.dof = false;   // an editing view, not a lens image
    }
    // Wireframe / unlit (and quad views) skip the effect packs: flat frames have nothing for them to read
    // (RunStack checks the same conditions per frame; this also skips their target allocation).
    if (nonLit || quad) frameSettings.packEffects.clear();

    bool needsId = false;
    for (const auto& entry : frameSettings.packEffects) {
        if (entry.enabled) {
            if (const ShaderPack* p = ShaderPacks().Find(entry.pack)) {
                if (p->HasCap("inputs.id")) {
                    needsId = true;
                    break;
                }
            }
        }
    }
    if (needsId) {
        const auto rtUav = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!targets_.id || targets_.id.width != w || targets_.id.height != h) {
            targets_.id.Create(*ctx_, w, h, RenderTargets::kIdFormat, rtUav,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"scene.id");
        }
        if (targets_.msaa > 1 && (!targets_.idMsaa || targets_.idMsaa.width != w || targets_.idMsaa.height != h || targets_.idMsaa.samples != targets_.msaa)) {
            targets_.idMsaa.Create(*ctx_, w, h, RenderTargets::kIdFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET, L"scene.id.msaa", targets_.msaa);
        }
        targets_.idTarget = &targets_.id;
    } else {
        if (targets_.id) targets_.id.Release(*ctx_);
        if (targets_.idMsaa) targets_.idMsaa.Release(*ctx_);
        targets_.idTarget = nullptr;
    }

    stats_.drawCalls = 0;
    stats_.triangles = 0;
    stats_.internalWidth = w;
    stats_.internalHeight = h;
    stats_.outputWidth = targets_.outWidth;
    stats_.outputHeight = targets_.outHeight;
    stats_.renderPath = path;
    stats_.upscaler = up ? up->Kind() : UpscalerKind::None;
    stats_.dlssModel = frameSettings.dlssModel;
    PassContext pc{*ctx_, cmd, view, frameSettings, targets_, stats_, transient_,
                   sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                   lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes,
                   frame, historyValid, offscreen, &builtin_, path, rt, up,
                   jitterPx_[0], jitterPx_[1], frameTimeMs_};
    // S9: the frame's local fog volumes for the volumetric passes (0 = none: they read no buffer)
    pc.fogVolumes = fogCount ? fogBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kFogBytes : 0;
    pc.mme = mme_.get();
    pc.mmeSkin = mmeSkin_.get();
    pc.sceneConstantsCpu = &sc;
    pc.spotSlices = &spotSlices_;
    // S7c: the light-cluster block PassLightCull fills for this frame (null when clustering is off)
    pc.lightCluster = clusters ? lightClusterBuf_.Get() : nullptr;
    pc.lightClusterSlot = cbSlot;
    // DLSS Ray Reconstruction needs the noisy ray-traced signals and their guides; it replaces the PT
    // temporal / a-trous denoiser, the RT AO / reflection filtering and the upscaler for the frame.
    pc.dlssRr = frameSettings.dlssRayReconstruction &&
                (path == RenderPath::PathTraced || path == RenderPath::RayTraced) && up &&
                up->Kind() == UpscalerKind::DLSS && up->RayReconstructionAvailable() && !offscreen;
    stats_.dlssRayReconstruction = pc.dlssRr;
    // A model destroyed or replaced since the last drawn frame leaves the model set: release its per-model MME
    // resources (object SRV tables + skinned buffer) after the frames that still reference them.
    if (!offscreen) {
        for (uint64_t old : frameModels_) {
            bool stillDrawn = false;
            for (const GpuModel* m : view.models)
                if (m && m->Uid() == old) { stillDrawn = true; break; }
            if (!stillDrawn) ForgetModel(old);
        }
        frameModels_.clear();
        for (const GpuModel* m : view.models)
            if (m) frameModels_.push_back(m->Uid());
    }
    // MME runtime: one Update per frame with the union of the post effects and every model's object effect (M4b).
    // The MME instance lifetime / async loads / target (re)creation live here, not in a pass.
    if (mme_) {
        mme_->PollHotReload();   // M6b: recompile changed .fx files at <= 1 Hz
        std::vector<MmeWanted> wanted;
        wanted.reserve(frameSettings.mmeEffects.size() + view.models.size());
        for (const MmeEffectEntry& e : frameSettings.mmeEffects) {
            if (e.enabled && !e.file.empty()) wanted.push_back({e.file, false});
        }
        for (const GpuModel* m : view.models)
            if (m)
                for (const std::filesystem::path& fx : m->MmeEffectFiles()) wanted.push_back({fx, true});
        // M5c: effect objects have a post/scene part and (with a mesh) an object part.
        for (const MmeEffectObject& o : view.mmeEffectObjects) {
            if (o.fx.empty()) continue;
            wanted.push_back({o.fx, false});
            if (o.mesh) wanted.push_back({o.fx, true});
        }
        const uint32_t mmeOutW = targets_.outWidth ? targets_.outWidth : w;
        const uint32_t mmeOutH = targets_.outHeight ? targets_.outHeight : h;
        if (!wanted.empty()) mme_->Update(*ctx_, wanted, mmeOutW, mmeOutH);
        mme_->BeginFrame(*ctx_, frame);
    }
    // quad view: one SceneConstants per extra view (orthographic, no jitter)
    if (quad) {
        const size_t count = std::min<size_t>(view.extraViews.size(), kMaxExtraViews);
        for (size_t e = 0; e < count; ++e) {
            const ExtraView& ev = view.extraViews[e];
            SceneConstants esc = sc;
            // per-view shading: the ortho views are always drawn flat (Unlit, or Wireframe when
            // that shading mode is selected); only the camera quadrant renders lit
            if (esc.shading < (float)ViewShading::Unlit) esc.shading = (float)ViewShading::Unlit;
            const XMMATRIX viewM = XMLoadFloat4x4(&ev.view);
            const float pxW = std::max(1.0f, ev.rect[2] * (float)w), pxH = std::max(1.0f, ev.rect[3] * (float)h);
            const XMMATRIX proj = XMMatrixOrthographicLH(ev.height * pxW / pxH, ev.height, ev.nearZ, ev.farZ);
            const XMMATRIX vp = viewM * proj;
            XMStoreFloat4x4(&esc.view, viewM);
            XMStoreFloat4x4(&esc.proj, proj);
            XMStoreFloat4x4(&esc.viewProj, vp);
            XMStoreFloat4x4(&esc.viewProjNoJitter, vp);
            XMStoreFloat4x4(&esc.prevViewProjNoJitter, vp);  // no motion
            XMStoreFloat4x4(&esc.invProj, XMMatrixInverse(nullptr, proj));
            const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
            XMStoreFloat4x4(&esc.invView, invView);
            XMStoreFloat3(&esc.eyePos, invView.r[3]);
            esc.jitterUv = {0, 0};
            esc.nearZ = ev.nearZ;
            esc.farZ = ev.farZ;
            esc.viewportSize = {pxW, pxH};
            esc.invViewportSize = {1.0f / pxW, 1.0f / pxH};
            esc.edgeScale = pxH / 1080.0f;
            esc.lightClusterParams = {-1.0f, 0.0f, 0.0f, 0.0f};   // S7c: an extra view has its own camera
            esc.emissiveParams = {-1.0f, -1.0f, 0.0f, -1.0f};    // S8a: the ortho views draw raster
            // S7b: the frame's gobo table is per light, so the extra view points at the frame's own table slot
            esc.goboParams = sc.goboParams;
            // S8b: an extra view draws flat (Unlit), so the screen area lights are not used; the table stays off there
            // (the orthogonal view's own camera would make the same rectangles but the flat shading ignores them)
            esc.screenLightParams = {-1.0f, 0.0f, -1.0f, 0.0f};
            const size_t slot = (size_t)kCbSlots + (size_t)cbSlot * kMaxExtraViews + e;
            memcpy(sceneCbMapped_ + slot * kSceneCbSize, &esc, sizeof(esc));
            pc.extraSceneConstants[e] = sceneCb_->GetGPUVirtualAddress() + (uint64_t)slot * kSceneCbSize;
        }
    }
    // Frame Generation: remember this frame's camera data (the final evaluate runs after the UI draw).
    if (!offscreen) {
        if (quad || settings_.headless || !frameGen_ || !frameGen_->Available() || settings_.frameGen < 2 ||
            !view.frameGen) {
            frameGenPending_ = false;
        } else {
            frameGenPending_ = true;
            const CameraParams& cam = view.camera;
            const bool sizeChanged = frameGenData_.width != w || frameGenData_.height != h;
            frameGenData_.viewProjNoJitter = sc.viewProjNoJitter;
            frameGenData_.prevViewProjNoJitter = sc.prevViewProjNoJitter;
            frameGenData_.invView = sc.invView;
            XMStoreFloat4x4(&frameGenData_.proj,
                            XMMatrixPerspectiveFovLH(cam.fovYRadians, (float)w / (float)h, cam.nearZ, cam.farZ));
            frameGenData_.eye = cam.eye;
            frameGenData_.nearZ = cam.nearZ;
            frameGenData_.farZ = cam.farZ;
            frameGenData_.fov = cam.fovYRadians;
            frameGenData_.jitterX = jitterPx_[0];
            frameGenData_.jitterY = jitterPx_[1];
            frameGenData_.width = w;
            frameGenData_.height = h;
            const XMMATRIX invView = XMLoadFloat4x4(&frameGenData_.invView);
            XMStoreFloat3(&frameGenData_.right, invView.r[0]);
            XMStoreFloat3(&frameGenData_.up, invView.r[1]);
            XMStoreFloat3(&frameGenData_.fwd, invView.r[2]);
            frameGenReset_ = view.cameraCut || !havePrev_ || !frameGenWasPending_ || sizeChanged;
            frameGenWasPending_ = true;
        }
    } else {
        frameGenPending_ = false;
    }
    if (!frameGenPending_) {
        frameGenWasPending_ = false;
        if (frameGen_) frameGen_->SetActive(*ctx_, false);   // toggled off / left the play scene: free the feature
    }

    for (auto& pass : passes_) pass->Execute(pc);

    if (!offscreen) {
        prevViewProj_ = sc.viewProjNoJitter;
        prevEye_ = view.camera.eye;
        havePrev_ = true;
        prevTaa_ = settings_.taa;
        ++temporalIndex_;
    }
}

void Renderer::ReleaseVideoLayers() {
    if (!ctx_) return;
    ctx_->WaitForGpu();
    for (auto& pass : passes_)
        if (VideoPass* vp = dynamic_cast<VideoPass*>(pass.get())) vp->Clear(*ctx_);
    if (screenVideo_) screenVideo_->Clear(*ctx_);
    if (stageScreens_) stageScreens_->Clear(*ctx_);
    if (feedResolve_) {
        ctx_->WaitForGpu();   // no command list in flight may still reference the feed PSO
        feedResolve_ = FullscreenPipeline{};
    }
    // the next scene's first feed creates the resolve PSO again (with the flag left set, every feed after leaving a
    // scene / the studio stayed black: the PSO was gone and never retried)
    feedResolveTried_ = false;
}

void Renderer::ClearBackBuffer(ID3D12GraphicsCommandList* cmd) {
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    const float bw = (float)ctx_->Width(), bh = (float)ctx_->Height();
    D3D12_VIEWPORT fullViewport{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    D3D12_RECT fullScissor{0, 0, (LONG)bw, (LONG)bh};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx_->BackBufferRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float bg[4] = {0.955f, 0.965f, 0.975f, 1.0f};
    cmd->ClearRenderTargetView(rtv, bg, 0, nullptr);
    cmd->RSSetViewports(1, &fullViewport);
    cmd->RSSetScissorRects(1, &fullScissor);
}

void Renderer::Render(ID3D12GraphicsCommandList* cmd, const FrameView& view) {
    if (!cmd || !ctx_) return;
    stats_.frameGenMultiplier = 0;
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    transient_.Begin(ctx_->FrameSlot());

    const float bw = (float)ctx_->Width(), bh = (float)ctx_->Height();
    D3D12_VIEWPORT fullViewport{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    D3D12_RECT fullScissor{0, 0, (LONG)bw, (LONG)bh};

    if (view.models.empty() && !view.studioFloor) {
        // Menus: nothing to render, just a clean surface for the UI.
        sceneVisible_ = false;
        havePrev_ = false;
        frameGenPending_ = false;
        frameGenActive_ = false;
        frameGenWasPending_ = false;
        if (frameGen_) frameGen_->SetActive(*ctx_, false);
        lastFrameQpc_ = 0;
        // No model is drawable: release the MME resources of the model set that just left the frame. Schedule
        // before clearing, then drain here (today the drain only ran from RecordScene) so the frees complete during
        // menu / loading frames instead of leaking until the next scene.
        for (uint64_t uid : frameModels_) ForgetModel(uid);
        frameModels_.clear();
        DrainMmeForget();
        ClearBackBuffer(cmd);
        return;
    }

    // Wall time since the previous on-screen frame (upscaler input; clamped against pauses).
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    frameTimeMs_ = lastFrameQpc_ ? std::clamp((float)((now.QuadPart - lastFrameQpc_) * 1000.0 / freq.QuadPart),
                                             0.1f, 100.0f)
                                 : 16.7f;
    lastFrameQpc_ = now.QuadPart;

    // 1. Output and internal resolution.
    // A studio quad view never runs the upscaler (RecordScene), so it renders at the output size: sizing the targets
    // for the upscaler would leave a low-resolution image stretched to the viewport (pixelated).
    const RenderPath path = EffectivePath();
    const bool quadView = !view.extraViews.empty() && path == RenderPath::Raster;
    IUpscaler* up = quadView ? nullptr : EffectiveUpscaler();
    uint32_t outW, outH;
    float area[4];
    ViewportArea(settings_, (float)ctx_->Width(), (float)ctx_->Height(), area);
    const uint32_t areaW = std::max(16u, (uint32_t)area[2]), areaH = std::max(16u, (uint32_t)area[3]);
    if (settings_.fixedResolution) {
        outW = settings_.fixedWidth;
        outH = settings_.fixedHeight;
    } else if (up) {
        outW = areaW;
        outH = areaH;
    } else {
        float scale = std::clamp(settings_.renderScale, 0.25f, 2.0f);
        outW = std::max(16u, (uint32_t)(areaW * scale));
        outH = std::max(16u, (uint32_t)(areaH * scale));
    }
    uint32_t w = outW, h = outH;
    if (up) IUpscaler::ComputeRenderSize(outW, outH, settings_.upscalerQuality, w, h);

    // 2. MSAA / pipelines / targets.
    uint32_t msaa = (up || path == RenderPath::PathTraced) ? 1u : SupportedMsaa(*ctx_, settings_.msaaSamples);
    if (msaa != pipelineMsaa_) {
        ctx_->WaitForGpu();
        for (auto& pass : passes_) {
            if (!pass->CreatePipelines(*ctx_, shaderDir_, msaa)) {
                LOG_ERROR("Renderer: failed to recreate pipelines for pass '%s'", pass->Name());
                return;
            }
        }
        pipelineMsaa_ = msaa;
    }
    EnsureTargets(w, h, outW, outH, msaa);
    EnsureShadowMap(settings_.shadowMapSize);
    if (!targets_.colorMsaa) return;

    // 3. GPU timer for the previous frame of this slot, then the begin timestamp.
    ReadGpuTimer();
    const uint32_t slot = ctx_->FrameSlot();
    cmd->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);

    // Background AVI (背景AVI): update the screen.bmp texture before anything draws models.
    if (screenVideo_) screenVideo_->Update(*ctx_, cmd, view);
    RecordScene(cmd, view, w, h, slot, ctx_->FrameNumber(), false);
    if (frameGenPending_) FrameGenCopyHudless(cmd);

    cmd->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    cmd->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                          timestampReadback_.Get(), slot * 16);

    // UI backdrop descriptor follows the backdrop texture (recreated only on resize).
    if (targets_.uiBackdrop && targets_.uiBackdrop->res.Get() != backdropSrvRes_ &&
        backdropSrv_ != DescriptorHeap::kInvalid) {
        targets_.uiBackdrop->WriteSrv(ctx_->Device(), ctx_->SrvHeap().Cpu(backdropSrv_));
        backdropSrvRes_ = targets_.uiBackdrop->res.Get();
    }
    FitInArea(area, (float)outW, (float)outH, presentRect_);
    sceneVisible_ = !settings_.headless;
    hasFinal_ = true;
}

bool Renderer::RenderToImage(const FrameView& view, uint32_t w, uint32_t h, ImageRGBA8& out) {
    if (!ctx_ || w == 0 || h == 0) return false;
    ID3D12Device* device = ctx_->Device();
    ctx_->WaitForGpu();
    if (!offAlloc_) {
        if (!CheckHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&offAlloc_)),
                     "RenderToImage: allocator") ||
            !CheckHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, offAlloc_.Get(), nullptr,
                                               IID_PPV_ARGS(&offList_)),
                     "RenderToImage: list"))
            return false;
        offList_->Close();
    }
    offAlloc_->Reset();
    offList_->Reset(offAlloc_.Get(), nullptr);

    // Thumbnails are a fixed portrait look, not a view of the user's current scene: pin everything that would
    // otherwise come from the user's ini (exposure, effect stack, MME, bloom shape, ...). The effect stack in
    // particular must never run offscreen: a metering / auto-exposure pack sees the transparent background and
    // can blow the exposure up (the washed-out cards).
    // Thumbnails are deterministic: the default look, not the user's (an effect stack with eye adaptation metered the
    // mostly transparent frame and blew the portrait out; exposure / LUT / MME post changed every card).
    const RenderSettings saved = settings_;
    settings_ = RenderSettings{};
    settings_.msaaSamples = saved.msaaSamples;
    settings_.shadowMapSize = saved.shadowMapSize;
    settings_.lutIntensity = 0.0f;
    settings_.transparentBackground = true;
    settings_.taa = false;
    settings_.fog = 0.0f;
    settings_.vignette = 0.0f;
    settings_.ssr = false;
    settings_.renderPath = RenderPath::Raster;
    settings_.upscaler = UpscalerKind::None;
    settings_.shading = ViewShading::Lit;
    settings_.drawEdges = true;
    settings_.shadows = true;
    settings_.shadowMapSize = 2048;
    settings_.ssao = true;
    settings_.autoLuminous = true;
    settings_.exposure = 1.0f;
    settings_.contrast = 1.06f;
    settings_.saturation = 1.06f;
    settings_.bloom = true;
    settings_.bloomIntensity = 0.35f;
    settings_.bloomThreshold = 1.1f;
    settings_.bloomConvolution = false;
    settings_.bloomLook = {};
    settings_.dof = false;
    settings_.volumetric = false;
    settings_.packEffects.clear();   // shader packs are opt-in per character and never applied to thumbnails
    settings_.mmeEffects.clear();
    sceneVisible_ = false;

    const uint32_t msaa = pipelineMsaa_ ? pipelineMsaa_ : 1;
    EnsureTargets(w, h, w, h, msaa);
    EnsureShadowMap(settings_.shadowMapSize);
    bool ok = targets_.colorMsaa && targets_.ldr;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    if (ok) {
        ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
        offList_->SetDescriptorHeaps(1, heaps);
        transient_.Begin(kOffscreenSlot);
        RecordScene(offList_.Get(), view, w, h, kOffscreenSlot, 0, true);

        D3D12_RESOURCE_DESC desc = targets_.ldr.res->GetDesc();
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
        CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
        ok = CheckHr(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&readback)),
                     "RenderToImage: readback");
        if (ok) {
            targets_.ldr.Transition(offList_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), fp);
            CD3DX12_TEXTURE_COPY_LOCATION src(targets_.ldr.res.Get(), 0);
            offList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            targets_.ldr.Transition(offList_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    offList_->Close();
    if (ok) {
        ID3D12CommandList* lists[] = {offList_.Get()};
        ctx_->Queue()->ExecuteCommandLists(1, lists);
        ctx_->WaitForGpu();
        uint8_t* data = nullptr;
        D3D12_RANGE range{0, (SIZE_T)total};   // the buffer's size (the last row has no pitch padding)
        if (SUCCEEDED(readback->Map(0, &range, (void**)&data))) {
            out = ImageRGBA8{};
            out.mips.push_back({w, h, std::vector<uint8_t>((size_t)w * h * 4)});
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* srcRow = data + (size_t)fp.Footprint.RowPitch * y;
                uint8_t* dstRow = out.mips[0].pixels.data() + (size_t)w * 4 * y;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* s = srcRow + x * 4;
                    uint8_t* d = dstRow + x * 4;
                    const uint32_t a = s[3];
                    for (int ch = 0; ch < 3; ++ch)
                        d[ch] = a == 0 ? 0 : (uint8_t)std::min(255u, (uint32_t)s[ch] * 255u / a);
                    d[3] = (uint8_t)a;
                }
            }
            out.hasAlpha = true;
            D3D12_RANGE none{0, 0};
            readback->Unmap(0, &none);
        } else {
            ok = false;
        }
    }
    settings_ = saved;
    return ok;
}

bool Renderer::DecodeReadback(ReadbackSlot& slot, bool preserveAlpha, ImageRGBA8& out) {
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, (SIZE_T)slot.total};   // the buffer's size (the last row has no pitch padding)
    if (FAILED(slot.res->Map(0, &range, (void**)&data))) return false;
    const uint32_t w = slot.width, h = slot.height;
    out = ImageRGBA8{};
    out.mips.push_back({w, h, std::vector<uint8_t>((size_t)w * h * 4)});
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* srcRow = data + (size_t)slot.fp.Footprint.RowPitch * y;
        uint8_t* dstRow = out.mips[0].pixels.data() + (size_t)w * 4 * y;
        memcpy(dstRow, srcRow, (size_t)w * 4);
        for (uint32_t x = 0; x < w; ++x) {
            if (!preserveAlpha) {
                dstRow[x * 4 + 3] = 255;
                continue;
            }
            // straight (non-premultiplied) sRGB colour: post.hlsl (transparentBg) kept the coverage alpha and the
            // unjittered scene targets premultiply the colour. Un-premultiply here, then un-jitter with the exact
            // inverse of post.hlsl's +1/255 dither (same per-pixel seed), so a flat background decodes to alpha 0.
            const uint8_t a = dstRow[x * 4 + 3];
            if (a == 0) {
                dstRow[x * 4 + 0] = dstRow[x * 4 + 1] = dstRow[x * 4 + 2] = 0;
                continue;
            }
            const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            const float j = Ign(fx, fy) - 0.5f;
            for (int ch = 0; ch < 3; ++ch) {
                // the dither is j / 255 in normalized units = j in 8-bit units
                const int encoded = std::clamp((int)std::lround((float)dstRow[x * 4 + ch] - j), 0, 255);
                dstRow[x * 4 + ch] = (uint8_t)std::min(255, (int)std::lround(encoded * 255.0f / a));
            }
        }
    }
    out.hasAlpha = preserveAlpha;
    D3D12_RANGE none{0, 0};
    slot.res->Unmap(0, &none);
    return true;
}

bool Renderer::ReadFinalImage(ImageRGBA8& out, bool preserveAlpha) {
    if (!ctx_ || !targets_.ldr || !hasFinal_) return false;
    ID3D12Device* device = ctx_->Device();
    ctx_->WaitForGpu();
    if (!offAlloc_) {
        if (!CheckHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&offAlloc_)),
                     "ReadFinalImage: allocator") ||
            !CheckHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, offAlloc_.Get(), nullptr,
                                               IID_PPV_ARGS(&offList_)),
                     "ReadFinalImage: list"))
            return false;
        offList_->Close();
    }
    offAlloc_->Reset();
    offList_->Reset(offAlloc_.Get(), nullptr);

    const uint32_t w = targets_.ldr.width, h = targets_.ldr.height;
    D3D12_RESOURCE_DESC desc = targets_.ldr.res->GetDesc();
    UINT64 total = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> readback;
    if (!CheckHr(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&readback)),
                 "ReadFinalImage: readback"))
        return false;
    targets_.ldr.Transition(offList_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), fp), src(targets_.ldr.res.Get(), 0);
    offList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    targets_.ldr.Transition(offList_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    offList_->Close();
    ID3D12CommandList* lists[] = {offList_.Get()};
    ctx_->Queue()->ExecuteCommandLists(1, lists);
    ctx_->WaitForGpu();

    ReadbackSlot slot;
    slot.res = readback;
    slot.fp = fp;
    slot.total = total;
    slot.width = w;
    slot.height = h;
    const bool ok = DecodeReadback(slot, preserveAlpha, out);
    return ok;
}

bool Renderer::QueueFinalReadback(ID3D12GraphicsCommandList* cmd, bool preserveAlpha) {
    if (!ctx_ || !cmd || !targets_.ldr || !hasFinal_) return false;
    if (readbackQueue_.size() >= kReadbackRing) return false;  // caller falls back to the blocking read
    ID3D12Device* device = ctx_->Device();
    if (!readbackFence_) {
        if (!CheckHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readbackFence_)),
                     "QueueFinalReadback: fence"))
            return false;
        readbackEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!readbackEvent_) return false;
    }
    // first ring slot not in the queue
    uint32_t index = DescriptorHeap::kInvalid;
    for (uint32_t i = 0; i < kReadbackRing; ++i) {
        bool used = false;
        for (uint32_t q : readbackQueue_)
            if (q == i) { used = true; break; }
        if (!used) { index = i; break; }
    }
    if (index == DescriptorHeap::kInvalid) return false;
    ReadbackSlot& slot = readback_[index];

    const uint32_t w = targets_.ldr.width, h = targets_.ldr.height;
    D3D12_RESOURCE_DESC desc = targets_.ldr.res->GetDesc();
    UINT64 total = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!slot.res || slot.total != total) {
        ctx_->DeferRelease(slot.res);
        slot.res.Reset();
        D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
        CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
        if (!CheckHr(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&slot.res)),
                     "QueueFinalReadback: readback"))
            return false;
    }
    slot.fp = fp;
    slot.total = total;
    slot.width = w;
    slot.height = h;
    slot.preserveAlpha = preserveAlpha;
    slot.fence = readbackFenceValue_ + 1;   // the value EndFrameReadback signals after this frame's submit
    targets_.ldr.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CD3DX12_TEXTURE_COPY_LOCATION dst(slot.res.Get(), fp), src(targets_.ldr.res.Get(), 0);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    targets_.ldr.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    readbackQueue_.push_back(index);
    ++readbackRecorded_;
    return true;
}

void Renderer::EndFrameReadback() {
    if (!ctx_ || !readbackFence_ || readbackRecorded_ == 0) return;
    readbackRecorded_ = 0;
    ++readbackFenceValue_;
    ctx_->Queue()->Signal(readbackFence_.Get(), readbackFenceValue_);
}

bool Renderer::PollFinalReadback(ImageRGBA8& out, bool wait) {
    if (!ctx_ || readbackQueue_.empty()) return false;
    const uint32_t index = readbackQueue_.front();
    ReadbackSlot& slot = readback_[index];
    if (!slot.res) {
        readbackQueue_.erase(readbackQueue_.begin());
        return false;
    }
    if (wait) {
        if (readbackFence_->GetCompletedValue() < slot.fence) {
            readbackFence_->SetEventOnCompletion(slot.fence, readbackEvent_);
            WaitForSingleObject(readbackEvent_, INFINITE);
        }
    } else if (readbackFence_->GetCompletedValue() < slot.fence) {
        return false;   // this frame's GPU work is still running
    }
    const bool ok = DecodeReadback(slot, slot.preserveAlpha, out);
    readbackQueue_.erase(readbackQueue_.begin());
    return ok;
}

uint64_t Renderer::SetOfflinePreviewImage(const ImageRGBA8& image) {
    if (!ctx_ || image.Empty()) return 0;
    UploadBatch batch(*ctx_);
    ComPtr<ID3D12Resource> tex = batch.CreateTexture(image, L"offline.preview");
    batch.Submit();
    if (!tex) return 0;
    const uint32_t slot = previewSlot_;
    previewSlot_ ^= 1;
    if (previewSrv_[slot] == DescriptorHeap::kInvalid) previewSrv_[slot] = ctx_->SrvHeap().Allocate(1);
    if (previewSrv_[slot] == DescriptorHeap::kInvalid) {
        ctx_->DeferRelease(tex);
        return 0;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    ctx_->Device()->CreateShaderResourceView(tex.Get(), &d, ctx_->SrvHeap().Cpu(previewSrv_[slot]));
    if (previewRes_[slot]) ctx_->DeferRelease(previewRes_[slot]);   // 2 previews old: the GPU is long past it
    previewRes_[slot] = tex;
    return ctx_->SrvHeap().Gpu(previewSrv_[slot]).ptr;
}

uint64_t Renderer::OfflinePreviewTexture() const {
    const uint32_t slot = previewSlot_ ^ 1;   // the last uploaded one
    if (!ctx_ || !previewRes_[slot] || previewSrv_[slot] == DescriptorHeap::kInvalid) return 0;
    return ctx_->SrvHeap().Gpu(previewSrv_[slot]).ptr;
}

uint64_t Renderer::UiBackdropTexture() const {
    if (!sceneVisible_ || !backdropSrvRes_ || backdropSrv_ == DescriptorHeap::kInvalid) return 0;
    return ctx_->SrvHeap().Gpu(backdropSrv_).ptr;
}

void Renderer::PresentRect(float& x, float& y, float& w, float& h) const {
    x = presentRect_[0];
    y = presentRect_[1];
    w = presentRect_[2];
    h = presentRect_[3];
}

RenderPath Renderer::EffectivePath() const { return rtSupported_ ? settings_.renderPath : RenderPath::Raster; }

IUpscaler* Renderer::EffectiveUpscaler() const {
    const size_t k = (size_t)settings_.upscaler;
    if (k == 0 || k >= 4 || !upscalerAvailable_[k] || !upscalers_[k]) return nullptr;
    return upscalers_[k].get();
}

bool Renderer::UpscalerAvailable(UpscalerKind kind) const {
    const size_t k = (size_t)kind;
    return k < 4 && upscalerAvailable_[k];
}

bool Renderer::FrameGenAvailable() const { return frameGen_ && frameGen_->Available(); }
int Renderer::FrameGenMaxMultiplier() const { return frameGen_ ? frameGen_->MaxMultiplier() : 0; }
const char* Renderer::FrameGenReason() const { return frameGen_ ? frameGen_->Reason() : TrKey("사용할 수 없음"); }

void Renderer::FrameGenCopyHudless(ID3D12GraphicsCommandList* cmd) {
    if (!frameGen_ || !frameGenPending_ || !ctx_) return;
    // The back buffer is in RENDER_TARGET state right now (RenderPass contract).
    frameGen_->CopyHudless(*ctx_, cmd, ctx_->BackBuffer());
}

bool Renderer::FrameGenEvaluate(ID3D12GraphicsCommandList* cmd) {
    frameGenActive_ = false;
    stats_.frameGenMultiplier = 0;
    if (!frameGen_ || !frameGenPending_ || !ctx_ || !cmd || !targets_.depth || !targets_.velocity) return false;
    if (ctx_->DeviceLost()) return false;
    frameGenPending_ = false;
    ID3D12Resource* bb = ctx_->BackBuffer();
    if (!bb || !frameGen_->CopyFinal(*ctx_, cmd, bb)) return false;

    FrameGenInputs in{};
    in.cmd = cmd;
    in.backbuffer = bb;
    in.depth = targets_.depth.res.Get();
    in.velocity = targets_.velocity.res.Get();
    in.depthState = targets_.depth.state;
    in.velocityState = targets_.velocity.state;
    in.width = ctx_->Width();
    in.height = ctx_->Height();
    in.renderWidth = frameGenData_.width;
    in.renderHeight = frameGenData_.height;
    in.multiplier = std::clamp(settings_.frameGen, 0, std::max(1, frameGen_->MaxMultiplier()));
    in.reset = frameGenReset_;
    in.frameTimeMs = frameTimeMs_;
    // Jitter in render pixels (the same value/sign convention the DLSS-SR path passes as InJitterOffsetX).
    in.jitterX = frameGenData_.jitterX;
    in.jitterY = frameGenData_.jitterY;
    // Matrices are row-vector (DirectX) row-major: v_clip = v * matrix. The SDK takes the same layout.
    std::memcpy(in.cameraViewToClip, &frameGenData_.proj, sizeof(in.cameraViewToClip));
    DirectX::XMFLOAT4X4 invProj;
    XMStoreFloat4x4(&invProj,
                    XMMatrixInverse(nullptr, XMLoadFloat4x4(&frameGenData_.proj)));
    std::memcpy(in.clipToCameraView, &invProj, sizeof(in.clipToCameraView));
    {
        // clip(current) -> clip(previous) = inv(viewProj_cur) * viewProj_prev (row-vector convention).
        const XMMATRIX vpCur = XMLoadFloat4x4(&frameGenData_.viewProjNoJitter);
        const XMMATRIX vpPrev = XMLoadFloat4x4(&frameGenData_.prevViewProjNoJitter);
        const XMMATRIX invCur = XMMatrixInverse(nullptr, vpCur);
        const XMMATRIX invPrev = XMMatrixInverse(nullptr, vpPrev);
        DirectX::XMFLOAT4X4 clipToPrev, prevToClip;
        XMStoreFloat4x4(&clipToPrev, invCur * vpPrev);
        XMStoreFloat4x4(&prevToClip, invPrev * vpCur);
        std::memcpy(in.clipToPrevClip, &clipToPrev, sizeof(in.clipToPrevClip));
        std::memcpy(in.prevClipToClip, &prevToClip, sizeof(in.prevClipToClip));
    }
    in.cameraPos[0] = frameGenData_.eye.x;
    in.cameraPos[1] = frameGenData_.eye.y;
    in.cameraPos[2] = frameGenData_.eye.z;
    in.cameraUp[0] = frameGenData_.up.x;
    in.cameraUp[1] = frameGenData_.up.y;
    in.cameraUp[2] = frameGenData_.up.z;
    in.cameraRight[0] = frameGenData_.right.x;
    in.cameraRight[1] = frameGenData_.right.y;
    in.cameraRight[2] = frameGenData_.right.z;
    in.cameraFwd[0] = frameGenData_.fwd.x;
    in.cameraFwd[1] = frameGenData_.fwd.y;
    in.cameraFwd[2] = frameGenData_.fwd.z;
    in.nearZ = frameGenData_.nearZ;
    in.farZ = frameGenData_.farZ;
    in.fov = frameGenData_.fov;
    in.aspect = (float)frameGenData_.width / (float)std::max(1u, frameGenData_.height);
    frameGenReset_ = false;
    if (!frameGen_->Evaluate(*ctx_, in)) return false;
    frameGenActive_ = true;
    stats_.frameGenMultiplier = frameGen_->GeneratedCount() + 1;
    return true;
}

int Renderer::FrameGenGeneratedCount() const { return frameGen_ ? frameGen_->GeneratedCount() : 0; }
ID3D12Resource* Renderer::FrameGenGeneratedFrame(int i) const {
    return frameGen_ ? frameGen_->GeneratedFrame(i) : nullptr;
}
ID3D12Resource* Renderer::FrameGenRealFrame() const { return frameGen_ ? frameGen_->RealFrame() : nullptr; }

bool Renderer::RayReconstructionAvailable() const {
    const size_t k = (size_t)UpscalerKind::DLSS;
    return upscalerAvailable_[k] && upscalers_[k] && upscalers_[k]->RayReconstructionAvailable();
}

void Renderer::SetColorLut(const ImageRGBA8* strip) {
    if (!ctx_) return;
    lut_.Release(*ctx_);  // deferred: frames in flight may still sample it
    targets_.lut = nullptr;
    if (!strip || strip->Empty()) return;
    UploadBatch batch(*ctx_);
    lut_.res = batch.CreateTexture(*strip, L"color.lut");
    batch.Submit();
    if (!lut_.res) return;
    lut_.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    lut_.format = lut_.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    lut_.width = strip->Width();
    lut_.height = strip->Height();
    targets_.lut = &lut_;
}

void Renderer::Shutdown() {
    if (!ctx_) return;
    ctx_->WaitForGpu();
    offlineInitCancel_ = true;   // a background creation still running stops before its next shader...
    EnsureOffline();             // ...and is waited for before anything goes away
    if (offline_) {
        offline_->Shutdown();
        offline_.reset();
    }
    if (rt_) {
        rt_->Shutdown();
        rt_.reset();
    }
    if (packTextures_) {
        packTextures_->Clear(ctx_);
        packTextures_.reset();
    }
    if (mme_) {
        mme_->Release(*ctx_);
        mme_.reset();
    }
    mmeSkin_.reset();
    rtSupported_ = false;
    if (frameGen_) {
        frameGen_->Shutdown();
        frameGen_.reset();
    }
    frameGenPending_ = false;
    frameGenActive_ = false;
    for (size_t k = 1; k < 4; ++k) {
        if (upscalers_[k]) {
            upscalers_[k]->Shutdown();
            upscalers_[k].reset();
        }
        upscalerAvailable_[k] = false;
    }
    ReleaseTargets();
    // M8b: release the video layers (decode threads, staging buffers, textures) with the renderer.
    for (auto& pass : passes_)
        if (VideoPass* vp = dynamic_cast<VideoPass*>(pass.get())) vp->Clear(*ctx_);
    if (screenVideo_) screenVideo_->Clear(*ctx_);
    if (stageScreens_) {
        stageScreens_->Clear(*ctx_);
        stageScreens_.reset();
    }
    if (feedResolve_) ctx_->WaitForGpu();   // no command list in flight may still reference the feed PSO
    for (FeedTargets& f : feeds_) {
        f.color.Release(*ctx_);
        f.normal.Release(*ctx_);
        f.velocity.Release(*ctx_);
        f.depth.Release(*ctx_);
    }
    feedResolve_ = FullscreenPipeline{};
    for (MirrorTargets& m : mirrors_) {
        m.color.Release(*ctx_);
        m.normal.Release(*ctx_);
        m.velocity.Release(*ctx_);
        m.depth.Release(*ctx_);
        if (m.srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(m.srv, 1);
        m.srv = DescriptorHeap::kInvalid;
        m.width = m.height = 0;
    }
    mirrorCount_ = 0;
    loggedMirrors_ = UINT32_MAX;
    targets_.shadowMap.Release(*ctx_);
    targets_.spotShadowMap.Release(*ctx_);
    targets_.pointShadowMap.Release(*ctx_);
    lut_.Release(*ctx_);
    targets_.lut = nullptr;
    passes_.clear();
    pipelineMsaa_ = 0;
    transient_.Release(*ctx_);
    feedTransient_.Release(*ctx_);
    mirrorTransient_.Release(*ctx_);
    if (backdropSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(backdropSrv_, 1);
    backdropSrv_ = DescriptorHeap::kInvalid;
    for (auto& t : builtin_.toon) t.Reset();
    builtin_.white.Reset();
    if (sceneCb_ && sceneCbMapped_) sceneCb_->Unmap(0, nullptr);
    if (lightBuf_ && lightBufMapped_) lightBuf_->Unmap(0, nullptr);
    if (fogBuf_ && fogBufMapped_) fogBuf_->Unmap(0, nullptr);
    if (lightCdfBuf_ && lightCdfMapped_) lightCdfBuf_->Unmap(0, nullptr);
    if (lightCdfBuf_ && ctx_) {
        for (uint32_t i = 0; i < lightCdfSrvCount_; ++i)
            if (lightCdfSrv_[i] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(lightCdfSrv_[i], 1);
    }
    std::fill(std::begin(lightCdfSrv_), std::end(lightCdfSrv_), DescriptorHeap::kInvalid);
    lightCdfSrvCount_ = 0;
    // S7c: the light-cluster blocks (the SRVs and the DEFAULT-heap buffer)
    if (lightClusterBuf_ && ctx_) {
        for (uint32_t i = 0; i < lightClusterSrvCount_; ++i)
            if (lightClusterSrv_[i] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(lightClusterSrv_[i], 1);
    }
    std::fill(std::begin(lightClusterSrv_), std::end(lightClusterSrv_), DescriptorHeap::kInvalid);
    lightClusterSrvCount_ = 0;
    // S8a: the emissive-triangle pick table (three raw SRVs per slot + the UPLOAD buffers)
    if (emissiveTriBuf_ && emissiveTriMapped_) emissiveTriBuf_->Unmap(0, nullptr);
    if (emissiveCdfBuf_ && emissiveCdfMapped_) emissiveCdfBuf_->Unmap(0, nullptr);
    if (emissiveBaseBuf_ && emissiveBaseMapped_) emissiveBaseBuf_->Unmap(0, nullptr);
    if (ctx_) {
        for (uint32_t i = 0; i < emissiveSrvCount_; ++i) {
            for (uint32_t* srv : {&emissiveTriSrv_[i], &emissiveCdfSrv_[i], &emissiveBaseSrv_[i]})
                if (*srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(*srv, 1);
        }
    }
    std::fill(std::begin(emissiveTriSrv_), std::end(emissiveTriSrv_), DescriptorHeap::kInvalid);
    std::fill(std::begin(emissiveCdfSrv_), std::end(emissiveCdfSrv_), DescriptorHeap::kInvalid);
    std::fill(std::begin(emissiveBaseSrv_), std::end(emissiveBaseSrv_), DescriptorHeap::kInvalid);
    emissiveSrvCount_ = 0;
    emissiveTriMapped_ = nullptr;
    emissiveCdfMapped_ = nullptr;
    emissiveBaseMapped_ = nullptr;
    emissiveTriBuf_.Reset();
    emissiveCdfBuf_.Reset();
    emissiveBaseBuf_.Reset();
    emissiveKey_ = 0;
    emissiveTable_ = EmissiveTable{};
    emissiveCount_ = 0;
    std::fill(std::begin(emissiveSlotKey_), std::end(emissiveSlotKey_), 0ull);
    emissiveParams_ = {-1.0f, -1.0f, 0.0f, -1.0f};
    // S7b: the gobo table's SRVs / upload buffer and the cached gobo textures
    if (goboBuf_ && goboBufMapped_) goboBuf_->Unmap(0, nullptr);
    if (ctx_) {
        for (uint32_t i = 0; i < goboSrvCount_; ++i)
            if (goboSrv_[i] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(goboSrv_[i], 1);
        for (auto& [path, g] : gobos_)
            if (g.srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(g.srv, 1);
    }
    std::fill(std::begin(goboSrv_), std::end(goboSrv_), DescriptorHeap::kInvalid);
    goboSrvCount_ = 0;
    goboBufMapped_ = nullptr;
    goboBuf_.Reset();
    gobos_.clear();
    goboTable_.clear();
    // S9b-3: the sky map textures and their SRVs
    if (ctx_) {
        for (auto& [key, m] : skyMaps_)
            if (m.srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(m.srv, 1);
    }
    skyMaps_.clear();
    // S8b: the screen-light table's SRVs and its upload buffer
    if (screenLightBuf_ && screenLightBufMapped_) screenLightBuf_->Unmap(0, nullptr);
    if (ctx_) {
        for (uint32_t i = 0; i < screenLightSrvCount_; ++i)
            if (screenLightSrv_[i] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(screenLightSrv_[i], 1);
    }
    std::fill(std::begin(screenLightSrv_), std::end(screenLightSrv_), DescriptorHeap::kInvalid);
    screenLightSrvCount_ = 0;
    screenLightBufMapped_ = nullptr;
    screenLightBuf_.Reset();
    screenLightTable_.clear();
    if (ctx_) {
        if (screenAverageSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(screenAverageSrv_, 1);
        if (screenAverageUav_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(screenAverageUav_, 1);
    }
    screenAverageSrv_ = DescriptorHeap::kInvalid;
    screenAverageUav_ = DescriptorHeap::kInvalid;
    screenAverageBuf_.Reset();
    screenAverageState_ = D3D12_RESOURCE_STATE_COMMON;
    screenAvgPipe_ = ComputePipeline{};
    screenAvgTried_ = false;
    screenReady_ = false;
    screenLightCount_ = 0;
    // S9b-2: the sky environment buffer and its build passes
    if (ctx_) skyEnv_.Release(*ctx_);
    sceneCbMapped_ = lightBufMapped_ = fogBufMapped_ = nullptr;
    lightCdfMapped_ = nullptr;
    sceneCb_.Reset();
    lightBuf_.Reset();
    fogBuf_.Reset();
    lightCdfBuf_.Reset();
    lightClusterBuf_.Reset();
    offList_.Reset();
    offAlloc_.Reset();
    if (readbackFence_) {
        for (ReadbackSlot& slot : readback_) slot.res.Reset();
        readbackQueue_.clear();
        readbackFence_.Reset();
        readbackFenceValue_ = 0;
        readbackRecorded_ = 0;
    }
    if (readbackEvent_) {
        CloseHandle(readbackEvent_);
        readbackEvent_ = nullptr;
    }
    if (previewRes_[0]) previewRes_[0].Reset();
    if (previewRes_[1]) previewRes_[1].Reset();
    for (uint32_t& srv : previewSrv_) {
        if (srv != DescriptorHeap::kInvalid) {
            ctx_->SrvHeap().Free(srv, 1);
            srv = DescriptorHeap::kInvalid;
        }
    }
    timestampReadback_.Reset();
    timestampHeap_.Reset();
    timestampFrequency_ = 0;
    ctx_ = nullptr;
}

Renderer::~Renderer() {
    Shutdown();
}

} // namespace mmdx
