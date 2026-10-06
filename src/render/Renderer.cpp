// Frame orchestration (see Passes.h for the pass list).
#include "render/Renderer.h"
#include "render/Passes.h"
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mmdx {

using namespace DirectX;

namespace {

constexpr uint32_t kCbSlots = Dx12Context::kFramesInFlight + 1;  // + RenderToImage
constexpr uint32_t kOffscreenSlot = Dx12Context::kFramesInFlight;
constexpr uint32_t kLightBytes = Renderer::kMaxPunctualLights * sizeof(GpuLight);

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

bool Renderer::Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    shaderDir_ = shaderDir;
    for (UpscalerKind k : {UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS}) {
        const size_t i = (size_t)k;
        upscalers_[i] = CreateUpscaler(k);
        upscalerAvailable_[i] = upscalers_[i]->Initialize(ctx);
        LOG_INFO("upscaler %s: %s", upscalers_[i]->DisplayName(), upscalerAvailable_[i] ? "available" : "unavailable");
    }
    rt_ = std::make_unique<RtScene>();
    rtSupported_ = rt_->Initialize(ctx, shaderDir);
    if (!rtSupported_) rt_.reset();
    if (rtSupported_) {
        offline_ = std::make_unique<OfflineRenderer>();
        if (!offline_->Initialize(ctx, shaderDir)) {
            LOG_WARN("offline renderer unavailable");
            offline_.reset();
        }
    }

    CreateBuiltinTextures();
    if (!transient_.Create(ctx, kCbSlots)) return false;

    passes_.clear();
    passes_.push_back(std::make_unique<ShadowPass>());
    passes_.push_back(std::make_unique<ScenePass>());
    passes_.push_back(std::make_unique<ResolvePass>());
    passes_.push_back(std::make_unique<PathTracePass>());
    passes_.push_back(std::make_unique<SsaoPass>());
    passes_.push_back(std::make_unique<SsrPass>());
    passes_.push_back(std::make_unique<CompositePass>());
    passes_.push_back(std::make_unique<VolumetricPass>());
    passes_.push_back(std::make_unique<TaaPass>());
    passes_.push_back(std::make_unique<UpscalePass>());
    passes_.push_back(std::make_unique<DofPass>());
    passes_.push_back(std::make_unique<BloomPass>());
    passes_.push_back(std::make_unique<PostPass>());
    passes_.push_back(std::make_unique<BackdropPass>());
    passes_.push_back(std::make_unique<PresentPass>());

    uint32_t msaa = SupportedMsaa(ctx, settings_.msaaSamples);
    for (auto& pass : passes_) {
        if (!pass->CreatePipelines(ctx, shaderDir, msaa)) {
            LOG_ERROR("Renderer: failed to create pipelines for pass '%s'", pass->Name());
            return false;
        }
    }
    pipelineMsaa_ = msaa;

    sceneCb_ = CreateMappedUpload(ctx.Device(), (uint64_t)kSceneCbSize * kCbSlots * (1 + kMaxExtraViews), &sceneCbMapped_, L"scene.cb");
    lightBuf_ = CreateMappedUpload(ctx.Device(), (uint64_t)kLightBytes * kCbSlots, &lightBufMapped_, L"scene.lights");
    if (!sceneCb_ || !lightBuf_) return false;

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
    return true;
}

std::unique_ptr<GpuModel> Renderer::CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                                const std::vector<ImageRGBA8>& textures, ModelRole role) {
    auto model = std::make_unique<GpuModel>();
    if (!model->Create(*ctx_, batch, pmx, textures, builtin_, role)) return nullptr;
    return model;
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
                       &targets_.color, &targets_.normal, &targets_.velocity, &targets_.depth, &targets_.lit,
                       &targets_.ldr})
        t->Release(*ctx_);
    targets_.ao = targets_.ssr = targets_.hdrFinal = targets_.bloom = targets_.uiBackdrop = nullptr;
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
    // spot shadow maps: a perspective frustum per spot cone (slice order = FillGpuLights)
    uint32_t spotSlices = 0;
    if (targets_.spotShadowMap && SpotShadowsWanted(settings_, EffectivePath(), offscreen)) {
        uint32_t slice = 0;
        const size_t count = std::min<size_t>(view.light.punctual.size(), kMaxPunctualLights);
        for (size_t i = 0; i < count && slice < kSpotShadowSlices; ++i) {
            const PunctualLight& p = view.light.punctual[i];
            if (p.spotCosOuter <= -1.0f) continue;
            const XMVECTOR pos = XMLoadFloat3(&p.position);
            const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&p.direction));
            const XMVECTOR upV = std::fabs(XMVectorGetY(dir)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
            const float halfAngle = std::acos(std::clamp(p.spotCosOuter, -0.99f, 1.0f));
            const float fov = std::min(2.0f * halfAngle * 1.08f + 0.02f, XMConvertToRadians(170.0f));
            const XMMATRIX lv = XMMatrixLookToLH(pos, dir, upV);
            const XMMATRIX lp = XMMatrixPerspectiveFovLH(fov, 1.0f, std::max(0.05f, p.range * 0.004f), std::max(p.range, 1.0f));
            XMStoreFloat4x4(&sc.spotViewProj[slice], lv * lp);
            ++slice;
        }
        spotSlices = slice;
    }
    sc.spotShadowParams = {(float)spotSlices, 1.0f / (float)std::max(targets_.spotShadowMap.width, 1u), 0, 0};
    sc.cascadeSplits = {splits[1], splits[2], splits[3], settings_.shadows && !view.shadowsOff ? 1.0f : 0.0f};
    sc.shadowParams = {1.0f / mapSize, 1.2f, 1.6f, view.shadowsOff ? 1.0f : 0.0f};  // w: path tracer sun shadows off
    sc.cascadeTexel = {texel[0], texel[1], texel[2], 0};

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
    sc.floorGloss = (settings_.ssr || EffectivePath() == RenderPath::PathTraced) ? settings_.floorGloss : 0.0f;
    sc.rimColor = lp.rimColor;
    sc.fog = settings_.fog;
    sc.viewportSize = {(float)w, (float)h};
    sc.edgeScale = (float)h / 1080.0f;
    sc.transparentBg = settings_.transparentBackground ? 1.0f : 0.0f;
    sc.jitterUv = {jx * 0.5f, -jy * 0.5f};
    sc.invViewportSize = {1.0f / w, 1.0f / h};
    sc.nearZ = cam.nearZ;
    sc.farZ = cam.farZ;
    sc.frameIndex = (float)(temporalIndex_ % 64);
    // The DXR scene shaders read shading as 0 (Lit): RayTraced / PathTraced ignore the setting.
    uint32_t shading = (EffectivePath() == RenderPath::Raster) ? (uint32_t)settings_.shading : 0u;
    if (quad && shading == 0u) shading = (uint32_t)ViewShading::Unlit;  // the ortho views cannot be lit
    sc.shading = (float)shading;
    // Distance haze is a lighting-style contribution; Unlit/Wireframe scenes are drawn flat
    // (RecordScene turns the lighting-adjacent effects off for these frames).
    if (shading != 0u) sc.fog = 0.0f;
}

uint32_t Renderer::FillGpuLights(const LightParams& light, GpuLight* out) {
    const size_t count = std::min<size_t>(light.punctual.size(), kMaxPunctualLights);
    uint32_t spot = 0;
    for (size_t i = 0; i < count; ++i) {
        const PunctualLight& p = light.punctual[i];
        GpuLight& g = out[i];
        g.position = p.position;
        g.invRange = 1.0f / std::max(p.range, 0.01f);
        g.color = {p.color.x * p.intensity, p.color.y * p.intensity, p.color.z * p.intensity};
        g.spotCosOuter = p.spotCosOuter;
        XMStoreFloat3(&g.direction, XMVector3Normalize(XMLoadFloat3(&p.direction)));
        g.spotCosInner = std::max(p.spotCosInner, p.spotCosOuter + 1e-3f);
        g.shadowSlice = (p.spotCosOuter > -1.0f && spot < kSpotShadowSlices) ? (float)spot : -1.0f;
        if (p.spotCosOuter > -1.0f) ++spot;
    }
    return (uint32_t)count;
}

void Renderer::RecordScene(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t w, uint32_t h,
                           uint32_t cbSlot, uint64_t frame, bool offscreen) {
    SceneConstants sc{};
    FillSceneConstants(view, w, h, offscreen, sc);
    memcpy(sceneCbMapped_ + (size_t)cbSlot * kSceneCbSize, &sc, sizeof(sc));
    GpuLight lights[kMaxPunctualLights] = {};
    FillGpuLights(view.light, lights);
    memcpy(lightBufMapped_ + (size_t)cbSlot * kLightBytes, lights, sizeof(lights));

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
    const bool quad = !offscreen && !view.extraViews.empty() && path == RenderPath::Raster;
    IUpscaler* up = (offscreen || quad) ? nullptr : EffectiveUpscaler();

    // Unlit / Wireframe draw the models flat, so the lighting-adjacent effects have nothing to
    // contribute this frame: run the frame with them off (a copy, RenderSettings is per-call here).
    RenderSettings frameSettings = settings_;
    const bool nonLit = path == RenderPath::Raster && (settings_.shading != ViewShading::Lit || quad);
    if (quad) {
        frameSettings.taa = false;
        if (frameSettings.shading == ViewShading::Lit) frameSettings.shading = ViewShading::Unlit;
        historyValid = false;
    }
    if (nonLit) {
        frameSettings.shadows = false;
        frameSettings.ssao = false;
        frameSettings.ssr = false;
        frameSettings.volumetric = false;
        frameSettings.bloom = false;
    }

    stats_.drawCalls = 0;
    stats_.triangles = 0;
    stats_.internalWidth = w;
    stats_.internalHeight = h;
    stats_.outputWidth = targets_.outWidth;
    stats_.outputHeight = targets_.outHeight;
    stats_.renderPath = path;
    stats_.upscaler = up ? up->Kind() : UpscalerKind::None;
    PassContext pc{*ctx_, cmd, view, frameSettings, targets_, stats_, transient_,
                   sceneCb_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kSceneCbSize,
                   lightBuf_->GetGPUVirtualAddress() + (uint64_t)cbSlot * kLightBytes,
                   frame, historyValid, offscreen, &builtin_, path, rt, up,
                   jitterPx_[0], jitterPx_[1], frameTimeMs_};
    // quad view: one SceneConstants per extra view (orthographic, no jitter)
    if (quad) {
        const size_t count = std::min<size_t>(view.extraViews.size(), kMaxExtraViews);
        for (size_t e = 0; e < count; ++e) {
            const ExtraView& ev = view.extraViews[e];
            SceneConstants esc = sc;
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
            const size_t slot = (size_t)kCbSlots + (size_t)cbSlot * kMaxExtraViews + e;
            memcpy(sceneCbMapped_ + slot * kSceneCbSize, &esc, sizeof(esc));
            pc.extraSceneConstants[e] = sceneCb_->GetGPUVirtualAddress() + (uint64_t)slot * kSceneCbSize;
        }
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
        lastFrameQpc_ = 0;
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
    IUpscaler* up = EffectiveUpscaler();
    const RenderPath path = EffectivePath();
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

    RecordScene(cmd, view, w, h, slot, ctx_->FrameNumber(), false);

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

    const RenderSettings saved = settings_;
    settings_.transparentBackground = true;
    settings_.taa = false;
    settings_.fog = 0.0f;
    settings_.vignette = 0.0f;
    settings_.ssr = false;
    settings_.renderPath = RenderPath::Raster;
    settings_.upscaler = UpscalerKind::None;
    sceneVisible_ = false;

    const uint32_t msaa = pipelineMsaa_ ? pipelineMsaa_ : 1;
    EnsureTargets(w, h, w, h, msaa);
    EnsureShadowMap(settings_.shadowMapSize);
    bool ok = targets_.colorMsaa && targets_.ldr;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    if (ok) {
        ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
        offList_->SetDescriptorHeaps(1, heaps);
        transient_.Begin(kOffscreenSlot);
        RecordScene(offList_.Get(), view, w, h, kOffscreenSlot, 0, true);

        D3D12_RESOURCE_DESC desc = targets_.ldr.res->GetDesc();
        UINT64 total = 0;
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
        D3D12_RANGE range{0, (SIZE_T)(fp.Footprint.RowPitch * h)};
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

bool Renderer::ReadFinalImage(ImageRGBA8& out) {
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

    uint8_t* data = nullptr;
    D3D12_RANGE range{0, (SIZE_T)(fp.Footprint.RowPitch * h)};
    if (FAILED(readback->Map(0, &range, (void**)&data))) return false;
    out = ImageRGBA8{};
    out.mips.push_back({w, h, std::vector<uint8_t>((size_t)w * h * 4)});
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* srcRow = data + (size_t)fp.Footprint.RowPitch * y;
        uint8_t* dstRow = out.mips[0].pixels.data() + (size_t)w * 4 * y;
        memcpy(dstRow, srcRow, (size_t)w * 4);
        for (uint32_t x = 0; x < w; ++x) dstRow[x * 4 + 3] = 255;
    }
    out.hasAlpha = false;
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return true;
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
    if (offline_) {
        offline_->Shutdown();
        offline_.reset();
    }
    if (rt_) {
        rt_->Shutdown();
        rt_.reset();
    }
    rtSupported_ = false;
    for (size_t k = 1; k < 4; ++k) {
        if (upscalers_[k]) {
            upscalers_[k]->Shutdown();
            upscalers_[k].reset();
        }
        upscalerAvailable_[k] = false;
    }
    ReleaseTargets();
    targets_.shadowMap.Release(*ctx_);
    targets_.spotShadowMap.Release(*ctx_);
    lut_.Release(*ctx_);
    targets_.lut = nullptr;
    passes_.clear();
    pipelineMsaa_ = 0;
    transient_.Release(*ctx_);
    if (backdropSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(backdropSrv_, 1);
    backdropSrv_ = DescriptorHeap::kInvalid;
    for (auto& t : builtin_.toon) t.Reset();
    builtin_.white.Reset();
    if (sceneCb_ && sceneCbMapped_) sceneCb_->Unmap(0, nullptr);
    if (lightBuf_ && lightBufMapped_) lightBuf_->Unmap(0, nullptr);
    sceneCbMapped_ = lightBufMapped_ = nullptr;
    sceneCb_.Reset();
    lightBuf_.Reset();
    offList_.Reset();
    offAlloc_.Reset();
    timestampReadback_.Reset();
    timestampHeap_.Reset();
    timestampFrequency_ = 0;
    ctx_ = nullptr;
}

Renderer::~Renderer() {
    Shutdown();
}

} // namespace mmdx
