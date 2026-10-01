// Frame orchestration: ScenePass -> ResolvePass -> PresentPass.
#include "render/Renderer.h"
#include "render/Passes.h"
#include "render/ShaderInterop.h"
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cstring>

namespace mmdx {

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

namespace {

// Cache: same requested value -> same result.
uint32_t g_cachedRequestedMsaa = 0;
uint32_t g_cachedMsaaResult = 0;

uint32_t SupportedMsaa(Dx12Context& ctx, uint32_t requested) {
    if (requested == g_cachedRequestedMsaa && g_cachedMsaaResult != 0) return g_cachedMsaaResult;

    ID3D12Device* device = ctx.Device();
    uint32_t n = requested;
    while (n > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = RenderTargets::kColorFormat;
        q.SampleCount = n;
        BOOL okColor = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
                       q.NumQualityLevels > 0;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qd{};
        qd.Format = RenderTargets::kDepthFormat;
        qd.SampleCount = n;
        BOOL okDepth = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qd, sizeof(qd))) &&
                       qd.NumQualityLevels > 0;
        if (okColor && okDepth) break;
        n /= 2;
    }
    g_cachedRequestedMsaa = requested;
    g_cachedMsaaResult = n;
    return n;
}

} // namespace

bool Renderer::Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    shaderDir_ = shaderDir;
    upscaler_ = CreateUpscaler(settings_.upscaler);

    CreateBuiltinTextures();

    passes_.clear();
    passes_.push_back(std::make_unique<ScenePass>());
    passes_.push_back(std::make_unique<ResolvePass>());
    passes_.push_back(std::make_unique<PresentPass>());

    uint32_t msaa = SupportedMsaa(ctx, settings_.msaaSamples);
    for (auto& pass : passes_) {
        if (!pass->CreatePipelines(ctx, shaderDir, msaa)) {
            LOG_ERROR("Renderer: failed to create pipelines for pass '%s'", pass->Name());
            return false;
        }
    }
    pipelineMsaa_ = msaa;

    // Per-frame scene constants.
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256 * Dx12Context::kFramesInFlight);
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&sceneCb_)),
                 "CreateCommittedResource(scene constants)"))
        return false;
    if (FAILED(sceneCb_->Map(0, nullptr, (void**)&sceneCbMapped_))) {
        LOG_ERROR("failed to map scene constant buffer");
        return false;
    }
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
                                                const std::vector<ImageRGBA8>& textures) {
    auto model = std::make_unique<GpuModel>();
    if (!model->Create(*ctx_, batch, pmx, textures, builtin_)) return nullptr;
    return model;
}

void Renderer::ReadGpuTimer() {
    if (!timestampReadback_ || timestampFrequency_ == 0) return;
    uint32_t slot = ctx_->FrameSlot();
    const uint64_t* data = nullptr;
    D3D12_RANGE range{slot * 16, slot * 16 + 16};
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &range, &mapped))) return;
    data = (const uint64_t*)mapped;
    if (data) {
        uint64_t begin = data[0];
        uint64_t end = data[1];
        if (end > begin) stats_.gpuFrameMs = (float)((double)(end - begin) * 1000.0 / (double)timestampFrequency_);
    }
    D3D12_RANGE readRange{0, 0};
    timestampReadback_->Unmap(0, &readRange);
}

void Renderer::EnsureTargets(uint32_t width, uint32_t height, uint32_t msaa) {
    if (targets_.width == width && targets_.height == height && targets_.msaa == msaa && targets_.colorMsaa &&
        targets_.colorResolved && targets_.colorMsaaRtv != DescriptorHeap::kInvalid &&
        targets_.depthMsaaDsv != DescriptorHeap::kInvalid && targets_.colorResolvedSrv != DescriptorHeap::kInvalid)
        return;
    ctx_->WaitForGpu();
    ReleaseTargets();

    ID3D12Device* device = ctx_->Device();
    UINT msaaCount = msaa > 1 ? msaa : 1;

    // colorMsaa
    {
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            RenderTargets::kColorFormat, width, height, 1, 1, msaaCount, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clearValue{RenderTargets::kColorFormat, {}};
        memcpy(clearValue.Color, &settings_.clearColor, sizeof(float) * 4);
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        if (!CheckHr(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                                                     IID_PPV_ARGS(&targets_.colorMsaa)),
                     "CreateCommittedResource(colorMsaa)"))
            return;
        targets_.colorMsaaRtv = ctx_->RtvHeap().Allocate(1);
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = RenderTargets::kColorFormat;
        rtv.ViewDimension = msaa > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(targets_.colorMsaa.Get(), &rtv, ctx_->RtvHeap().Cpu(targets_.colorMsaaRtv));
    }
    // depthMsaa
    {
        CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            RenderTargets::kDepthFormat, width, height, 1, 1, msaaCount, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clearValue{RenderTargets::kDepthFormat};
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        if (!CheckHr(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                     IID_PPV_ARGS(&targets_.depthMsaa)),
                     "CreateCommittedResource(depthMsaa)"))
            return;
        targets_.depthMsaaDsv = ctx_->DsvHeap().Allocate(1);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = RenderTargets::kDepthFormat;
        dsv.ViewDimension = msaa > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(targets_.depthMsaa.Get(), &dsv, ctx_->DsvHeap().Cpu(targets_.depthMsaaDsv));
    }
    // colorResolved
    {
        CD3DX12_RESOURCE_DESC desc =
            CD3DX12_RESOURCE_DESC::Tex2D(RenderTargets::kColorFormat, width, height, 1, 1, 1, 0,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
        if (!CheckHr(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&targets_.colorResolved)),
                     "CreateCommittedResource(colorResolved)"))
            return;
        targets_.colorResolvedSrv = ctx_->SrvHeap().Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = RenderTargets::kColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(targets_.colorResolved.Get(), &srv,
                                         ctx_->SrvHeap().Cpu(targets_.colorResolvedSrv));
    }
    targets_.width = width;
    targets_.height = height;
    targets_.msaa = msaa;
    LOG_INFO("render targets %ux%u msaa=%u", width, height, msaa);
}

void Renderer::ReleaseTargets() {
    if (targets_.colorMsaaRtv != DescriptorHeap::kInvalid) {
        ctx_->RtvHeap().Free(targets_.colorMsaaRtv, 1);
        targets_.colorMsaaRtv = DescriptorHeap::kInvalid;
    }
    if (targets_.depthMsaaDsv != DescriptorHeap::kInvalid) {
        ctx_->DsvHeap().Free(targets_.depthMsaaDsv, 1);
        targets_.depthMsaaDsv = DescriptorHeap::kInvalid;
    }
    if (targets_.colorResolvedSrv != DescriptorHeap::kInvalid) {
        ctx_->SrvHeap().Free(targets_.colorResolvedSrv, 1);
        targets_.colorResolvedSrv = DescriptorHeap::kInvalid;
    }
    targets_.colorMsaa.Reset();
    targets_.depthMsaa.Reset();
    targets_.colorResolved.Reset();
    targets_.width = targets_.height = 0;
    targets_.msaa = 1;
}

void Renderer::Render(ID3D12GraphicsCommandList* cmd, const FrameView& view) {
    if (!cmd || !ctx_) return;

    // 1. Internal resolution.
    uint32_t w = 0, h = 0;
    if (settings_.fixedResolution) {
        w = settings_.fixedWidth;
        h = settings_.fixedHeight;
    } else {
        float scale = std::clamp(settings_.renderScale, 0.25f, 2.0f);
        upscaler_->ComputeRenderSize(ctx_->Width(), ctx_->Height(), scale, w, h);
    }

    // 2. MSAA / pipelines / targets.
    uint32_t msaa = SupportedMsaa(*ctx_, settings_.msaaSamples);
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
    EnsureTargets(w, h, msaa);

    // 3. GPU timer for the previous frame of this slot.
    ReadGpuTimer();

    // 4. Begin timestamp.
    uint32_t slot = ctx_->FrameSlot();
    cmd->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);

    // 5. Scene constants.
    CameraParams cam = view.camera;
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(cam.fovYRadians, (float)w / (float)h, cam.nearZ, cam.farZ);
    DirectX::XMMATRIX viewM = DirectX::XMLoadFloat4x4(&cam.view);
    DirectX::XMMATRIX viewProj = viewM * proj;
    SceneConstants sc{};
    XMStoreFloat4x4(&sc.view, viewM);
    XMStoreFloat4x4(&sc.proj, proj);
    XMStoreFloat4x4(&sc.viewProj, viewProj);
    sc.eyePos = cam.eye;
    DirectX::XMVECTOR dir = DirectX::XMLoadFloat3(&view.light.direction);
    DirectX::XMVECTOR n = DirectX::XMVector3Normalize(dir);
    XMStoreFloat3(&sc.lightDir, n);
    sc.lightColor = view.light.color;
    sc.viewportSize = {(float)w, (float)h};
    sc.edgeScale = (float)h / 1080.0f;
    memcpy(sceneCbMapped_ + slot * 256, &sc, sizeof(SceneConstants));
    D3D12_GPU_VIRTUAL_ADDRESS scAddress = sceneCb_->GetGPUVirtualAddress() + slot * 256;


    // 6. Descriptor heap.
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);

    // 7. Passes.
    stats_.drawCalls = 0;
    stats_.triangles = 0;
    stats_.internalWidth = w;
    stats_.internalHeight = h;
    PassContext pc{*ctx_, cmd, view, settings_, targets_, stats_, scAddress};

    for (auto& pass : passes_) pass->Execute(pc);

    // 8. End timestamp + resolve.
    cmd->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    cmd->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                          timestampReadback_.Get(), slot * 16);
}

void Renderer::Shutdown() {
    if (!ctx_) return;
    ctx_->WaitForGpu();
    ReleaseTargets();
    passes_.clear();
    pipelineMsaa_ = 0;
    for (auto& t : builtin_.toon) t.Reset();
    builtin_.white.Reset();
    if (sceneCb_ && sceneCbMapped_) {
        sceneCb_->Unmap(0, nullptr);
        sceneCbMapped_ = nullptr;
    }
    sceneCb_.Reset();
    timestampReadback_.Reset();
    timestampHeap_.Reset();
    timestampFrequency_ = 0;
    ctx_ = nullptr;
}

Renderer::~Renderer() {
    Shutdown();
}

} // namespace mmdx
