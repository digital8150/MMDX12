// S5 stage screens: one texture per LED surface, its source (image / video / camera feed), and the per-material
// binding. See StageScreens.h. The camera feed itself is rendered by the Renderer (the callback): it owns the scene
// passes and the constant-buffer slots.
#include "render/StageScreens.h"
#include "render/GpuModel.h"
#include "render/PackTextures.h"
#include "render/ShaderPack.h"
#include "render/VideoTexture.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mmdx {

namespace {

constexpr uint32_t kScreenCap = 2048;          // a screen texture is at most this wide / tall
constexpr uint32_t kFeedCapW = 960;            // a camera feed renders at most this large
constexpr uint32_t kFeedCapH = 540;

uint64_t HashMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
uint64_t HashText(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = HashMix(h, c);
    return HashMix(h, s.size());
}
uint64_t HashPath(uint64_t h, const std::filesystem::path& p) {
    return HashText(h, PathToUtf8(p));
}

// One frame's binding of a model: the signature of what was bound, so a change restores the model's own materials.
struct ModelBinding {
    uint64_t sig = 0;
    std::vector<uint32_t> materials;
};

// S6: one screen pack's PSO (a fullscreen pass into the screen texture). Compiled on first use; a compile error is one
// [W] and the screen keeps its last content (black before the first frame).
struct ProgramPipe {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    PackTextures::Set textures;       // the pack's "textures" (t0..t15, space5)
    bool failed = false;
};

} // namespace

struct StageScreens::Impl {
    struct Slot {
        std::string key;
        uint64_t modelUid = 0;
        std::vector<uint32_t> materials;
        StageScreenSource source = StageScreenSource::None;
        std::filesystem::path path;
        std::string pack;                 // S6 "program": the bundled screen pack id
        uint32_t width = 0, height = 0;   // texture size (capped)
        float brightness = 1.0f;
        bool enabled = true;
        uint64_t generation = 0;          // bumped when the bound texture changes
        Texture tex;                      // image / camera feed / program texture (owned)
        ComPtr<ID3D12Resource> imageRes;  // UploadBatch image texture (no RTV; the SRV source)
        VideoTexture video;               // video source (one decoder per video screen)
        std::filesystem::path failedPath; // a file whose load failed: warn once, show black

        Slot() = default;
        Slot(Slot&&) = default;
        Slot& operator=(Slot&&) = default;
    };

    std::vector<Slot> slots;
    ComPtr<ID3D12Resource> black;          // 1x1 black texture for source none / off
    std::unordered_map<uint64_t, ModelBinding> bound;
    std::map<std::string, ProgramPipe> programs;   // pack id -> its pipeline (stage packs never hot reload)
    uint32_t feedCursor = 0;               // round robin over the camera feeds
};

StageScreens::StageScreens() : impl_(std::make_unique<Impl>()) {}
StageScreens::~StageScreens() = default;

void StageScreens::ForgetModel(uint64_t uid) {
    if (impl_) impl_->bound.erase(uid);
}

namespace {

// S6: the fullscreen PSO of one screen pack: b1 16 root constants (gP0 = size, time, frame), b2 the pack's 16
// params, t0..t15 space5 its textures. No b0: a screen pack reads no scene constants (screen_api.hlsli), so the pass
// needs nothing from the frame's constant buffer and works in every render path (raster / PT / offline GI).
bool CreateProgramPipe(Dx12Context& ctx, const std::filesystem::path& shaderDir, const ShaderPack& pack,
                       ProgramPipe& out) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_DESCRIPTOR_RANGE packTex;
    packTex.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPackMaxTextures, 0, 5);
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstants(16, 1);                    // gP0 = (width, height, time, frameIndex)
    params[1].InitAsConstants(16, 2);                    // the pack's 16 parameters (PackParam)
    params[2].InitAsDescriptorTable(1, &packTex, D3D12_SHADER_VISIBILITY_PIXEL);
    // s0 point clamp, s1 linear clamp, s2 linear wrap: fullscreen.hlsli's gPoint / gLinear / gLinearWrap, which
    // screen_api.hlsli samples through.
    CD3DX12_STATIC_SAMPLER_DESC samplers[3];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(3, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_WARN("stage screen pack '%s': root signature serialization failed", pack.id.c_str());
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&out.rootSig)),
                 "stage screen pack: CreateRootSignature"))
        return false;
    // the pack's screen.hlsl as an include path relative to stage_screen.hlsl (a stage pack lives anywhere)
    const std::filesystem::path file = shaderDir / L"stage_screen.hlsl";
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(pack.dir / L"screen.hlsl", shaderDir, ec);
    if (ec || rel.empty()) rel = pack.dir / L"screen.hlsl";
    std::string inc = PathToUtf8(rel);
    std::replace(inc.begin(), inc.end(), '\\', '/');
    const uint32_t texCount = (uint32_t)std::min<size_t>(pack.textures.size(), kPackMaxTextures);
    uint32_t clampMask = 0;
    for (uint32_t i = 0; i < texCount; ++i)
        if (pack.textures[i].clamp) clampMask |= 1u << i;
    ShaderDefines defines = {{"MMDX_PACK", "\"" + inc + "\""},
                             {"PACK_TEX_COUNT", std::to_string(texCount)},
                             {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"}};
    std::string errors;
    ComPtr<ID3DBlob> vs = CompileShaderDxc(file, "VSFullscreen", "vs_6_0", defines, &errors);
    ComPtr<ID3DBlob> ps = vs ? CompileShaderDxc(file, "PSPackScreen", "ps_6_0", defines, &errors) : nullptr;
    if (!vs || !ps) {
        LOG_WARN("stage screen pack '%s': compile failed (the screen stays black)%s%s", pack.id.c_str(),
                 errors.empty() ? "" : ": ", errors.c_str());
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = out.rootSig.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // the screen texture (display-referred, as image / video)
    pso.SampleDesc.Count = 1;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out.pso)), "stage screen pack: PSO"))
        return false;
    if (!pack.textures.empty()) {
        PackTextures::Upload(ctx, pack, out.textures, {});
        if (out.textures.srv == DescriptorHeap::kInvalid) {
            LOG_WARN("stage screen pack '%s': pack textures unavailable (white is used)", pack.id.c_str());
            out.textures.srv = DescriptorHeap::kInvalid;
            out.textures.tex.clear();
        }
    }
    LOG_INFO("stage screen pack '%s': compiled (%s)", pack.id.c_str(), inc.c_str());
    return true;
}

// Draws one program screen: the pack's PSO over the screen's own texture, at `time` seconds. The texture is left in
// ALL_SHADER_RESOURCE (the raster material shaders and the PT / GI compute shaders read it).
bool DrawProgramScreen(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, ProgramPipe& pipe, const ShaderPack& pack,
                       Texture& target, float time, uint64_t frame, const std::map<std::string, float>& params) {
    if (!pipe.pso || !target) return false;
    ID3D12DescriptorHeap* heaps[] = {ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(pipe.rootSig.Get());
    cmd->SetPipelineState(pipe.pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    const float cb[16] = {(float)target.width, (float)target.height, time, (float)(frame & 63ull), 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 0};
    cmd->SetGraphicsRoot32BitConstants(0, 16, cb, 0);
    const PackParamValues values = pack.Resolve(params);
    cmd->SetGraphicsRoot32BitConstants(1, (uint32_t)kPackMaxParams, values.data(), 0);
    if (pipe.textures.srv != DescriptorHeap::kInvalid)
        cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvHeap().Gpu(pipe.textures.srv));
    target.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.RtvHeap().Cpu(target.rtv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, (float)target.width, (float)target.height, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    const D3D12_RECT sc{0, 0, (LONG)target.width, (LONG)target.height};
    cmd->RSSetScissorRects(1, &sc);
    cmd->DrawInstanced(3, 1, 0, 0);
    target.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);   // PT / offline GI read it from compute
    return true;
}

// A 1x1 black texture for a screen with no source (none / program without a pack / off): the surface stays black
// instead of showing the material's own texture. A texture cannot live in an UPLOAD heap, so the value is copied in
// once through a tiny upload buffer on the frame's command list.
bool EnsureBlack(ComPtr<ID3D12Resource>& black, Dx12Context& ctx, ID3D12GraphicsCommandList* cmd) {
    if (black) return true;
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, 0,
                                                              D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&black)),
                 "stage screen: black texture"))
        return false;
    black->SetName(L"screen.black");
    const D3D12_RESOURCE_DESC td = black->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    ctx.Device()->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    const CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&staging)),
                 "stage screen: black staging"))
        { black.Reset(); return false; }
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        black.Reset();
        return false;
    }
    std::memset(mapped, 0, (size_t)total);
    staging->Unmap(0, nullptr);
    if (cmd) {
        CD3DX12_TEXTURE_COPY_LOCATION dst(black.Get(), 0);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), fp);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            black.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &barrier);
    }
    ctx.DeferRelease(staging);   // freed once the frame that copies from it is done
    return true;
}

// Loads `path` into one texture, capped at kScreenCap (a larger image is downscaled, aspect kept).
bool LoadScreenImage(Dx12Context& ctx, const std::filesystem::path& path, ComPtr<ID3D12Resource>& out,
                     std::string& error) {
    ImageRGBA8 image;
    if (!LoadImageRGBA8(path, image, &error) || image.Empty()) {
        if (error.empty()) error = "cannot decode the image";
        return false;
    }
    uint32_t w = image.Width(), h = image.Height();
    if (w > kScreenCap || h > kScreenCap) {
        // DownscaleRgba8 keeps the aspect and only takes a width: pick the width that also caps the height.
        const uint32_t maxW = w >= h ? kScreenCap : std::max(1u, (uint32_t)((uint64_t)w * kScreenCap / h));
        uint32_t nw = 0, nh = 0;
        std::vector<uint8_t> px = DownscaleRgba8(w, h, image.mips[0].pixels.data(), maxW, nw, nh);
        if (nw == 0 || nh == 0 || px.empty()) {
            error = "downscale failed";
            return false;
        }
        ImageRGBA8 resized;
        resized.mips.push_back({nw, nh, std::move(px)});
        resized.hasAlpha = image.hasAlpha;
        image = std::move(resized);
    }
    UploadBatch batch(ctx);
    out = batch.CreateTexture(image, L"stage.screen");
    batch.Submit();
    if (!out) {
        error = "texture upload failed";
        return false;
    }
    return true;
}

} // namespace

void StageScreens::Clear(Dx12Context& ctx) {
    Impl& impl = *impl_;
    if (!ctx.Device()) return;
    for (Impl::Slot& s : impl.slots) {
        s.video.Release(ctx);
        s.tex.Release(ctx);
        s.imageRes.Reset();
    }
    impl.slots.clear();
    // S6: the program packs' PSOs and their texture sets (SRVs freed once the GPU is done)
    if (!impl.programs.empty()) {
        ctx.WaitForGpu();
        for (auto& [id, pipe] : impl.programs) {
            if (pipe.textures.srv != DescriptorHeap::kInvalid)
                ctx.SrvHeap().Free(pipe.textures.srv, pipe.textures.capacity);
            pipe.textures.srv = DescriptorHeap::kInvalid;
            pipe.pso.Reset();
            pipe.rootSig.Reset();
        }
        impl.programs.clear();
    }
    impl.black.Reset();
    impl.bound.clear();
    impl.feedCursor = 0;
}

void StageScreens::Render(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, const FrameView& view,
                          const FeedRender& renderFeed) {
    Impl& impl = *impl_;
    if (!ctx.Device() || !cmd) return;

    // 1. Reconcile the slot list with this frame's screens (a screen keeps its texture by key).
    {
        std::vector<Impl::Slot> next;
        next.reserve(view.screens.size());
        for (const ScreenView& sv : view.screens) {
            auto it = std::find_if(impl.slots.begin(), impl.slots.end(),
                                   [&](const Impl::Slot& s) { return s.key == sv.key; });
            if (it != impl.slots.end()) {
                Impl::Slot slot = std::move(*it);
                impl.slots.erase(it);
                next.push_back(std::move(slot));
            } else {
                Impl::Slot slot;
                slot.key = sv.key;
                next.push_back(std::move(slot));
            }
        }
        // what is left belongs to no screen anymore: free it
        for (Impl::Slot& gone : impl.slots) {
            gone.video.Release(ctx);
            gone.tex.Release(ctx);
        }
        impl.slots = std::move(next);
    }

    // 2. Sources: create / free the textures the screens need this frame.
    bool needWait = false;
    for (size_t i = 0; i < impl.slots.size(); ++i) {
        Impl::Slot& s = impl.slots[i];
        const ScreenView& sv = view.screens[i];
        s.modelUid = sv.modelUid;
        s.materials = sv.materials;
        s.brightness = sv.brightness;
        s.enabled = sv.enabled;
        s.width = std::clamp(sv.width, 1u, kScreenCap);
        s.height = std::clamp(sv.height, 1u, kScreenCap);
        const StageScreenSource source = sv.enabled ? sv.source : StageScreenSource::None;
        // drops whatever the slot held (the video decoder, the image / feed / program texture): a source switch and a
        // program pack switch both need the old content gone
        const auto releaseContent = [&]() {
            // Frames in flight still sample the old picture (the materials' descriptors point at it): wait BEFORE
            // anything is freed. The image used to be freed with no wait at all (only the feed / program texture set
            // needWait, and that wait came after the release): switching a screen from an image to a camera in the
            // studio removed the device (page fault at VA 0).
            if ((s.video.IsOpen() || s.tex || s.imageRes) && !needWait) {
                ctx.WaitForGpu();
                needWait = true;
            }
            if (s.video.IsOpen()) s.video.Release(ctx);
            if (s.tex) s.tex.Release(ctx);
            s.imageRes.Reset();
            ++s.generation;
        };
        if (source != s.source) {
            releaseContent();
            s.source = source;
            s.path.clear();
        }
        if (source == StageScreenSource::Program && s.pack != sv.pack) {
            releaseContent();   // a different pack draws a different picture
            s.pack = sv.pack;
        }
        if (source == StageScreenSource::Image || source == StageScreenSource::Video) {
            if (s.path != sv.path) {
                if (s.video.IsOpen() || s.tex || s.imageRes) releaseContent();
                s.path = sv.path;
                s.failedPath.clear();
            }
        } else {
            s.path = sv.path;   // recorded for the log only
        }
        switch (source) {
        case StageScreenSource::Image:
            if (!s.imageRes && !s.path.empty() && s.failedPath != s.path) {
                std::string error;
                if (LoadScreenImage(ctx, s.path, s.imageRes, error)) {
                    LOG_INFO("stage screen '%s': image %s", s.key.c_str(), PathToUtf8(s.path.filename()).c_str());
                    ++s.generation;
                } else {
                    LOG_WARN("stage screen '%s': image %s: %s", s.key.c_str(), PathToUtf8(s.path).c_str(),
                             error.c_str());
                    s.failedPath = s.path;
                    ++s.generation;
                }
            }
            if (!s.imageRes) EnsureBlack(impl.black, ctx, cmd);
            break;
        case StageScreenSource::Video:
            if (!s.video.IsOpen() && !s.path.empty() && s.failedPath != s.path) {
                if (s.video.Open(ctx, s.path)) {
                    LOG_INFO("stage screen '%s': video %s %ux%u", s.key.c_str(),
                             PathToUtf8(s.path.filename()).c_str(), s.video.Width(), s.video.Height());
                    ++s.generation;
                } else {
                    s.failedPath = s.path;   // Open logged the reason; one warning per file
                    ++s.generation;
                }
            }
            if (!s.video.IsOpen()) EnsureBlack(impl.black, ctx, cmd);
            break;
        case StageScreenSource::Camera:
            if (!s.tex) {
                const uint32_t w = std::min(s.width, kFeedCapW), h = std::min(s.height, kFeedCapH);
                const float clear[4] = {0, 0, 0, 1};
                if (s.tex.Create(ctx, std::max(1u, w), std::max(1u, h), DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"screen.feed", 1, 1, clear))
                {
                    LOG_INFO("stage screen '%s': camera feed %ux%u", s.key.c_str(), w, h);
                    ++s.generation;
                }
                else
                    LOG_WARN("stage screen '%s': feed texture creation failed", s.key.c_str());
            }
            break;
        case StageScreenSource::Program: {
            // S6: an RGBA8 render-target texture the pack draws into (same size rule as a screen; at most 2048).
            if (s.pack.empty()) {
                // no pack (the manifest's screen has no `file`, or the pack failed to register): black, one [W] at load
                EnsureBlack(impl.black, ctx, cmd);
                break;
            }
            if (!s.tex) {
                const float clear[4] = {0, 0, 0, 1};
                if (s.tex.Create(ctx, s.width, s.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"screen.program", 1, 1, clear))
                    ++s.generation;
                else
                    LOG_WARN("stage screen '%s': program texture creation failed", s.key.c_str());
            }
            break;
        }
        case StageScreenSource::None:
        default:
            EnsureBlack(impl.black, ctx, cmd);
            break;
        }
    }

    // 3. Video decode / upload for this frame (loops by duration; exact for offline renders).
    for (Impl::Slot& s : impl.slots) {
        if (s.source != StageScreenSource::Video || !s.video.IsOpen()) continue;
        if (!s.video.Update(ctx, cmd, ctx.FrameSlot(), (double)view.sceneTime, view.videoExact, view.cameraCut))
            continue;   // no frame yet: the previous texture stays
    }

    // 3b. S6 program screens: one fullscreen draw per screen pack into the screen's own texture, at the song / timeline
    // time (exact for offline renders). A pack registers once per screen folder, so two screens sharing one pack share
    // its PSO (each draws into its own texture).
    for (size_t i = 0; i < impl.slots.size(); ++i) {
        Impl::Slot& s = impl.slots[i];
        if (s.source != StageScreenSource::Program || !s.tex || s.pack.empty()) continue;
        const ShaderPack* pack = ShaderPacks().Find(s.pack);
        if (!pack || pack->type != PackType::Screen || !pack->Selectable()) continue;   // one [W] at registration
        auto it = impl.programs.find(s.pack);
        if (it == impl.programs.end()) {
            ProgramPipe pipe;
            if (!CreateProgramPipe(ctx, shaderDir_, *pack, pipe)) pipe.failed = true;
            it = impl.programs.emplace(s.pack, std::move(pipe)).first;
        }
        if (it->second.failed) continue;   // the screen keeps its previous content (black before the first frame)
        DrawProgramScreen(ctx, cmd, it->second, *pack, s.tex, view.sceneTime, ctx.FrameNumber(), view.screens[i].params);
    }

    // 4. Camera feeds: render at most kMaxFeedsPerFrame per frame, round robin over the rest. Screens showing the same
    // camera at the same size are one feed: it renders once and the others copy its texture (a stage's two jumbotrons
    // on one camera cost one scene render), so the budget counts distinct cameras.
    {
        struct FeedGroup { std::vector<size_t> screens; };
        std::vector<FeedGroup> groups;
        const auto sameFeed = [&](size_t a, size_t b) {
            const CameraParams& ca = view.screens[a].camera;
            const CameraParams& cb = view.screens[b].camera;
            return impl.slots[a].tex.width == impl.slots[b].tex.width &&
                   impl.slots[a].tex.height == impl.slots[b].tex.height &&
                   std::memcmp(&ca.view, &cb.view, sizeof(ca.view)) == 0 && ca.fovYRadians == cb.fovYRadians &&
                   ca.nearZ == cb.nearZ && ca.farZ == cb.farZ;
        };
        for (size_t i = 0; i < impl.slots.size(); ++i) {
            if (impl.slots[i].source != StageScreenSource::Camera || !impl.slots[i].tex || !impl.slots[i].enabled)
                continue;
            FeedGroup* group = nullptr;
            for (FeedGroup& g : groups)
                if (sameFeed(g.screens.front(), i)) { group = &g; break; }
            if (!group) {
                groups.emplace_back();
                group = &groups.back();
            }
            group->screens.push_back(i);
        }
        if (!groups.empty()) {
            const uint32_t count = std::min<uint32_t>((uint32_t)groups.size(), kMaxFeedsPerFrame);
            for (uint32_t f = 0; f < count; ++f) {
                const FeedGroup& group = groups[(impl.feedCursor + f) % groups.size()];
                Impl::Slot& s = impl.slots[group.screens.front()];
                if (!renderFeed(f, view.screens[group.screens.front()], s.tex)) continue;
                s.tex.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                for (size_t c = 1; c < group.screens.size(); ++c) {
                    Impl::Slot& other = impl.slots[group.screens[c]];
                    s.tex.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    other.tex.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyResource(other.tex.res.Get(), s.tex.res.Get());
                    other.tex.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    s.tex.Transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                }
            }
            impl.feedCursor = (impl.feedCursor + count) % (uint32_t)groups.size();
        }
    }

    // 5. Bind: every screen's texture goes on the materials it claims (MatFlag_Screen = emission).
    // The descriptor rewrite and the constants flush wait for the GPU (frames in flight read both).
    bool bindChanged = false;
    std::unordered_map<uint64_t, ModelBinding> next;
    for (size_t i = 0; i < impl.slots.size(); ++i) {
        const Impl::Slot& s = impl.slots[i];
        uint64_t sig = 0x9e3779b97f4a7c15ull;
        sig = HashMix(sig, s.modelUid);
        sig = HashText(sig, s.key);
        sig = HashMix(sig, s.generation);
        uint32_t bits = 0;
        std::memcpy(&bits, &s.brightness, sizeof(bits));
        sig = HashMix(sig, bits);
        sig = HashMix(sig, s.materials.size());
        for (uint32_t m : s.materials) sig = HashMix(sig, m);
        auto it = next.find(s.modelUid);
        if (it == next.end()) {
            ModelBinding mb;
            mb.sig = sig;
            mb.materials = s.materials;
            next.emplace(s.modelUid, std::move(mb));
        } else {
            it->second.sig = HashMix(it->second.sig, sig);
            for (uint32_t m : s.materials)
                if (std::find(it->second.materials.begin(), it->second.materials.end(), m) ==
                    it->second.materials.end())
                    it->second.materials.push_back(m);
        }
    }
    for (const auto& [uid, mb] : next) {
        const auto old = impl.bound.find(uid);
        if (old != impl.bound.end() && old->second.sig == mb.sig) continue;
        bindChanged = true;
    }
    for (const auto& [uid, mb] : impl.bound) {
        if (!next.count(uid)) bindChanged = true;
    }
    if (bindChanged || (impl.bound.empty() && !next.empty())) {
        ctx.WaitForGpu();
        // look the models up in this frame's drawn set (never keep GpuModel pointers: a model can be freed)
        const auto modelOf = [&](uint64_t uid) -> GpuModel* {
            for (GpuModel* m : view.models)
                if (m && m->Uid() == uid) return m;
            return nullptr;
        };
        // restore the materials of models whose screens went away / changed
        for (const auto& [uid, mb] : impl.bound) {
            GpuModel* model = modelOf(uid);
            const auto nit = next.find(uid);
            if (!model) continue;
            if (nit != next.end() && nit->second.sig == mb.sig) continue;
            for (uint32_t m : mb.materials) {
                bool still = false;
                if (nit != next.end())
                    still = std::find(nit->second.materials.begin(), nit->second.materials.end(), m) !=
                            nit->second.materials.end();
                if (!still) {
                    model->SetMaterialScreen(m, false, 1.0f);
                    model->BindMaterialTexture(ctx, m, nullptr);
                }
            }
        }
        for (size_t i = 0; i < impl.slots.size(); ++i) {
            const Impl::Slot& s = impl.slots[i];
            GpuModel* model = modelOf(s.modelUid);
            if (!model) continue;
            ID3D12Resource* res = nullptr;
            if (s.source == StageScreenSource::Image)
                res = s.imageRes.Get();
            else if (s.source == StageScreenSource::Video)
                res = s.video.TextureResource();
            else if (s.source == StageScreenSource::Camera || s.source == StageScreenSource::Program)
                res = s.tex.res.Get();
            if (!res) res = impl.black.Get();
            if (!res) continue;
            for (uint32_t m : s.materials) {
                model->SetMaterialScreen(m, true, s.brightness);
                model->BindMaterialTexture(ctx, m, res);
            }
        }
        for (const auto& [uid, mb] : next) {
            GpuModel* model = modelOf(uid);
            if (model) model->FlushMaterialConstants(ctx.FrameNumber());
        }
        for (const auto& [uid, mb] : impl.bound) {
            GpuModel* model = modelOf(uid);
            if (model) model->FlushMaterialConstants(ctx.FrameNumber());
        }
        impl.bound = std::move(next);
    }
}

} // namespace mmdx
