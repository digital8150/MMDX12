// PackEffectPass (effect.hlsl + a pack's effect.hlsl): the shader packs' whole-screen effects. Two instances
// live in the pass list (Passes.h): one before Bloom ("pre-bloom" entries, linear HDR, works in RGBA16F
// ping-pong buffers) and one after Post ("post" entries, display-referred sRGB, works in RGBA8 ping-pong
// buffers and copies back into targets.ldr). The ordered stack of enabled entries is
// RenderSettings::packEffects. Every PSO is compiled lazily (DXC ps_6_0, MMDX_PACK = the pack's effect.hlsl,
// like the surface packs); a compile error marks the pack failed and its entry is skipped ([E] + toast).
// Zero enabled entries: nothing allocates, nothing runs (a byte-identical frame). See docs/shader_effect_api.md.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/Renderer.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "render/OfflineRenderer.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <set>

namespace mmdx {

// One effect pack's pipeline: the FullscreenPipeline root layout (b0 SceneConstants, s0/s1/s2 samplers) with
// the effect-specific bindings on top: the t0..t3 space6 table holds the effect's inputs (t0 = the frame so far,
// t1/t2/t3 = depth / velocity / normal), b1 = PassCB root constants (gP0.xy = output size), b2 = the pack's 16
// params, and a t0..t15 space5 table = the pack textures (pack.json "textures", exactly the surface packs'
// table). Drawn as a fullscreen triangle (VSFullscreen); DXC (FXC cannot #include a macro path).
class EffectPipeline {
public:
    bool Create(Dx12Context& ctx, const std::filesystem::path& file, bool preBloom, const ShaderDefines& defines,
                std::string* errors, uint32_t stateFloats = 0) {
        ID3D12Device* device = ctx.Device();
        CD3DX12_DESCRIPTOR_RANGE inputs;
        inputs.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 6);   // t0..t3, space6 (effect_api.hlsli)
        CD3DX12_DESCRIPTOR_RANGE packTex;
        packTex.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 5);
        CD3DX12_DESCRIPTOR_RANGE stateRange;
        stateRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 7); // t0, space7 (effect_api.hlsli)

        CD3DX12_ROOT_PARAMETER params[6];
        params[0].InitAsConstantBufferView(0);                 // SceneConstants (gTime, gFrameIndex, ...)
        params[1].InitAsDescriptorTable(1, &inputs, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstants(16, 1);                      // PassCB: gP0.xy = (outW, outH), gP0.z = dt, gP0.w = reset
        params[3].InitAsConstants(16, 2);                      // the pack's 16 params
        params[4].InitAsDescriptorTable(1, &packTex, D3D12_SHADER_VISIBILITY_PIXEL);
        uint32_t paramCount = 5;
        if (stateFloats > 0) {
            params[5].InitAsDescriptorTable(1, &stateRange, D3D12_SHADER_VISIBILITY_PIXEL);
            paramCount = 6;
        }

        CD3DX12_STATIC_SAMPLER_DESC samplers[3];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                         D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        CD3DX12_ROOT_SIGNATURE_DESC rs;
        rs.Init(paramCount, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
            if (errors) *errors = "root signature serialization failed";
            return false;
        }
        if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&rootSig_)),
                     "PackEffectPass: CreateRootSignature"))
            return false;

        ComPtr<ID3DBlob> vs = CompileShaderDxc(file, "VSFullscreen", "vs_6_0", defines, errors);
        ComPtr<ID3DBlob> ps = vs ? CompileShaderDxc(file, "PSEffect", "ps_6_0", defines, errors) : nullptr;
        if (!vs || !ps) return false;

        ComPtr<ID3DBlob> psState;
        if (stateFloats > 0) {
            psState = CompileShaderDxc(file, "PSEffectState", "ps_6_0", defines, errors);
            if (!psState) return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSig_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = preBloom ? RenderTargets::kColorFormat : RenderTargets::kLdrFormat;
        pso.SampleDesc.Count = 1;
        if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)),
                     "PackEffectPass: CreateGraphicsPipelineState"))
            return false;

        if (stateFloats > 0) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC statePsoDesc = pso;
            statePsoDesc.PS = {psState->GetBufferPointer(), psState->GetBufferSize()};
            statePsoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (!CheckHr(device->CreateGraphicsPipelineState(&statePsoDesc, IID_PPV_ARGS(&psoState_)),
                         "PackEffectPass: CreateGraphicsPipelineState (PSEffectState)"))
                return false;
            hasState_ = true;
        }
        return true;
    }

    // Binds the RTV (the target must already be in RENDER_TARGET state), viewport = target size, draws.
    void Draw(PassContext& pc, Texture& target, D3D12_GPU_DESCRIPTOR_HANDLE inputs, D3D12_GPU_DESCRIPTOR_HANDLE packTex,
              const float outConst[16], const float* params, D3D12_GPU_DESCRIPTOR_HANDLE stateSrv = {}) const {
        if (!pso_) return;
        ID3D12GraphicsCommandList* cmd = pc.cmd;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = pc.ctx.RtvHeap().Cpu(target.rtv);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, (float)target.width, (float)target.height, 0, 1};
        D3D12_RECT sc{0, 0, (LONG)target.width, (LONG)target.height};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->SetGraphicsRootSignature(rootSig_.Get());
        cmd->SetPipelineState(pso_.Get());
        cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
        cmd->SetGraphicsRootDescriptorTable(1, inputs);
        cmd->SetGraphicsRoot32BitConstants(2, 16, outConst, 0);
        cmd->SetGraphicsRoot32BitConstants(3, 16, params, 0);
        if (packTex.ptr) cmd->SetGraphicsRootDescriptorTable(4, packTex);
        if (hasState_ && stateSrv.ptr) cmd->SetGraphicsRootDescriptorTable(5, stateSrv);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    void DrawState(PassContext& pc, Texture& target, D3D12_GPU_DESCRIPTOR_HANDLE inputs, D3D12_GPU_DESCRIPTOR_HANDLE packTex,
                   const float outConst[16], const float* params, D3D12_GPU_DESCRIPTOR_HANDLE prevStateSrv) const {
        if (!psoState_) return;
        ID3D12GraphicsCommandList* cmd = pc.cmd;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = pc.ctx.RtvHeap().Cpu(target.rtv);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, (float)target.width, (float)target.height, 0, 1};
        D3D12_RECT sc{0, 0, (LONG)target.width, (LONG)target.height};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->SetGraphicsRootSignature(rootSig_.Get());
        cmd->SetPipelineState(psoState_.Get());
        cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
        cmd->SetGraphicsRootDescriptorTable(1, inputs);
        cmd->SetGraphicsRoot32BitConstants(2, 16, outConst, 0);
        cmd->SetGraphicsRoot32BitConstants(3, 16, params, 0);
        if (packTex.ptr) cmd->SetGraphicsRootDescriptorTable(4, packTex);
        if (prevStateSrv.ptr) cmd->SetGraphicsRootDescriptorTable(5, prevStateSrv);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    explicit operator bool() const { return pso_ != nullptr; }
    bool HasState() const { return hasState_; }

private:
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> psoState_;
    bool hasState_ = false;
};

PackEffectPass::PackEffectPass(bool preBloom) : preBloom_(preBloom) {}
PackEffectPass::~PackEffectPass() = default;

bool PackEffectPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    shaderDir_ = shaderDir;
    return true;   // the per-pack PSOs are compiled lazily (Pipelines), like the surface packs
}

void PackEffectPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    resetRequested_ = true;
    if (ping_[0]) AllocTargets(ctx, targets.outWidth, targets.outHeight);   // follows the output size while alive
}

void PackEffectPass::ReleaseTargets(Dx12Context& ctx) {
    ping_[0].Release(ctx);
    ping_[1].Release(ctx);
    post_[0].Release(ctx);
    post_[1].Release(ctx);
    ReleaseStateSlots(ctx);
}

void PackEffectPass::ReleaseStateSlots(Dx12Context& ctx) {
    for (auto& [key, slot] : stateSlots_) {
        slot.tex[0].Release(ctx);
        slot.tex[1].Release(ctx);
    }
    stateSlots_.clear();
}

// Ping-pong targets at output resolution: RGBA16F (the pre-bloom share) and RGBA8 (the post share). Created
// on the first frame with a pending effect; zero effects = zero cost (no targets, byte-identical frame).
bool PackEffectPass::AllocTargets(Dx12Context& ctx, uint32_t w, uint32_t h) {
    if (ping_[0] && ping_[0].width == w && ping_[0].height == h) return true;
    resetRequested_ = true;
    ctx.WaitForGpu();
    ReleaseTargets(ctx);
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    bool ok = true;
    ok &= ping_[0].Create(ctx, w, h, RenderTargets::kColorFormat, rt, kSrv, L"packeffect.hdr.a");
    ok &= ping_[1].Create(ctx, w, h, RenderTargets::kColorFormat, rt, kSrv, L"packeffect.hdr.b");
    if (!ok) {
        LOG_ERROR("PackEffectPass: target creation failed (%ux%u)", w, h);
        ReleaseTargets(ctx);
        return false;
    }
    LOG_INFO("PackEffectPass: %ux%u targets", w, h);
    return true;
}

bool PackEffectPass::AllocPostTargets(Dx12Context& ctx, uint32_t w, uint32_t h) {
    if (post_[0] && post_[0].width == w && post_[0].height == h) return true;
    resetRequested_ = true;
    ctx.WaitForGpu();
    post_[0].Release(ctx);
    post_[1].Release(ctx);
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    bool ok = true;
    ok &= post_[0].Create(ctx, w, h, RenderTargets::kLdrFormat, rt, kSrv, L"packeffect.ldr.a");
    ok &= post_[1].Create(ctx, w, h, RenderTargets::kLdrFormat, rt, kSrv, L"packeffect.ldr.b");
    if (!ok) {
        LOG_ERROR("PackEffectPass: post target creation failed (%ux%u)", w, h);
        post_[0].Release(ctx);
        post_[1].Release(ctx);
        return false;
    }
    return true;
}

// ---- per-pack compile / textures ---------------------------------------------------------------------

// The pack's textures (pack.json "textures") once per (pack, folder) set: the same upload path as the surface
// packs' ScenePass::EnsurePackTextures. The pack texture state enters the pack's PSO as compile defines.
bool PackEffectPass::EnsurePackTextures(Dx12Context& ctx, const ShaderPack& pack, EffectPipelines& p,
                                        const std::filesystem::path& folder) {
    p.texLoaded = true;
    const uint32_t count = (uint32_t)std::min<size_t>(pack.textures.size(), kPackMaxTextures);
    if (count == 0) return true;
    ShaderPackRegistry& reg = ShaderPacks();
    p.tex.assign(kPackMaxTextures, {});
    p.texSrv = ctx.SrvHeap().Allocate(kPackMaxTextures);
    if (p.texSrv == DescriptorHeap::kInvalid) {
        LOG_ERROR("shader effect '%s': out of SRV descriptors for pack textures", pack.id.c_str());
        p.tex.clear();
        return false;
    }
    UploadBatch batch(ctx);
    ImageRGBA8 white;
    white.mips.push_back({1, 1, {255, 255, 255, 255}});
    p.white = batch.CreateTexture(white, L"packfx.white");   // kept alive: the SRVs point at it
    uint32_t missing = 0;
    ImageRGBA8 img;
    std::string loadError;
    for (uint32_t i = 0; i < count; ++i) {
        const PackTexture& t = pack.textures[i];
        std::filesystem::path path;
        if (!ResolvePackTextureFile(pack, t.file, folder, path)) {
            LOG_WARN("shader effect '%s': texture '%s' not found, white is used", pack.id.c_str(), t.file.c_str());
            ++missing;
            continue;
        }
        img = {};
        loadError.clear();
        if (LoadImageRGBA8(path, img, &loadError) && img.Width() <= kPackMaxTextureSize &&
            img.Height() <= kPackMaxTextureSize) {
            p.tex[i] = batch.CreateTextureTyped(img, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"packfx.texture");
            if (!p.tex[i])
                LOG_WARN("shader effect '%s': texture '%s' upload failed, white is used", pack.id.c_str(),
                         t.file.c_str());
        } else if (!img.Empty()) {
            LOG_WARN("shader effect '%s': texture '%s' is %ux%u (max %u), white is used", pack.id.c_str(),
                     t.file.c_str(), img.Width(), img.Height(), kPackMaxTextureSize);
        } else {
            LOG_WARN("shader effect '%s': texture '%s' cannot be decoded (%s), white is used", pack.id.c_str(),
                     t.file.c_str(), loadError.c_str());
        }
        if (!p.tex[i]) ++missing;
    }
    batch.Submit();
    reg.ReportMissingTextures(pack.id, folder, missing);
    if (missing)
        LOG_WARN("shader effect '%s': %u of %u textures missing (white is used)", pack.id.c_str(), missing, count);
    if (!p.white) {
        LOG_ERROR("shader effect '%s': pack texture upload failed", pack.id.c_str());
        ctx.SrvHeap().Free(p.texSrv, kPackMaxTextures);
        p.texSrv = DescriptorHeap::kInvalid;
        p.tex.clear();
        return false;
    }
    ID3D12Device* device = ctx.Device();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = UINT(-1);
    for (uint32_t i = 0; i < kPackMaxTextures; ++i) {
        srv.Format = i < count && pack.textures[i].srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        ID3D12Resource* res = i < count && p.tex[i] ? p.tex[i].Get() : p.white.Get();
        device->CreateShaderResourceView(res, &srv, ctx.SrvHeap().Cpu(p.texSrv + i));
    }
    return true;
}

const PackEffectPass::EffectPipelines* PackEffectPass::Pipelines(Dx12Context& ctx, const std::string& id,
                                                                 const std::filesystem::path& folder, bool preBloom) {
    ShaderPackRegistry& reg = ShaderPacks();
    if (reg.Generation() != generation_) {
        ReleasePackPsos(ctx);   // reload: the old PSOs / pack textures may still be in flight
        ReleaseStateSlots(ctx);
        generation_ = reg.Generation();
    }
    const ShaderPack* pack = reg.Find(id);
    if (!pack || !pack->Selectable() || pack->type != PackType::Effect) return nullptr;
    if (preBloom != (pack->stage == PackEffectStage::PreBloom)) return nullptr;   // the other instance's stage
    EffectPipelines& p = psos_[id];
    if (p.failed) return nullptr;
    if (!p.texLoaded && !EnsurePackTextures(ctx, *pack, p, folder)) {
        reg.ReportError(id, "pack textures");
        p.pipe = {};
        p.failed = true;
        return nullptr;
    }
    if (p.pipe) return &p;

    // the pack's effect.hlsl as an include path relative to effect.hlsl (built-in packs/<id>, user ../shader_packs/<id>)
    const std::filesystem::path file = shaderDir_ / L"effect.hlsl";
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(pack->dir / L"effect.hlsl", shaderDir_, ec);
    if (ec || rel.empty()) rel = pack->dir / L"effect.hlsl";
    std::string inc = PathToUtf8(rel);
    std::replace(inc.begin(), inc.end(), '\\', '/');
    const uint32_t texCount = (uint32_t)std::min<size_t>(pack->textures.size(), kPackMaxTextures);
    uint32_t clampMask = 0, srgbMask = 0;
    for (uint32_t i = 0; i < texCount; ++i) {
        if (pack->textures[i].clamp) clampMask |= 1u << i;
        if (pack->textures[i].srgb) srgbMask |= 1u << i;
    }
    ShaderDefines defines = {{"MMDX_PACK", "\"" + inc + "\""},
                             {"PACK_TEX_COUNT", std::to_string(texCount)},
                             {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
                             {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"}};
    if (pack->stateFloats > 0) {
        defines.push_back({"PACK_STATE", "1"});
        defines.push_back({"PACK_STATE_FLOATS", std::to_string(pack->stateFloats)});
    }
    std::string errors;
    auto pipe = std::make_unique<EffectPipeline>();
    if (!pipe->Create(ctx, file, preBloom, defines, &errors, pack->stateFloats)) {
        LOG_ERROR("shader effect '%s': compile failed, the effect is skipped%s%s", id.c_str(),
                  errors.empty() ? "" : ":\n", errors.c_str());
        reg.ReportError(id, errors.empty() ? "effect compile" : errors);
        p.pipe = {};
        p.failed = true;
        return nullptr;
    }
    p.pipe = std::move(pipe);
    reg.ReportCompiled(id);
    LOG_INFO("shader effect '%s': compiled (%s)", id.c_str(), inc.c_str());
    return &p;
}

void PackEffectPass::ReleasePackPsos(Dx12Context& ctx) {
    if (psos_.empty()) return;
    ctx.WaitForGpu();
    for (auto& [id, p] : psos_)
        if (p.texSrv != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(p.texSrv, kPackMaxTextures);
    psos_.clear();
}

// ---- execute -----------------------------------------------------------------------------------------

// Runs this instance's share of the stack in order, ping-ponging between the two share targets, then copies
// the last result back into the source (a plain CopyResource: same formats on both sides).
void PackEffectPass::RunStack(PassContext& pc, bool preBloom) {
    RenderTargets& t = pc.targets;
    // counts every execution of this instance (even frames that skip the effects), so an entry that did not run in
    // the previous frame (disabled, unlit view ...) gets its state reset when it comes back
    ++passExecutionCount_;
    // Unlit / wireframe / quad views draw flat, so the effects would read shading the frame does not have:
    // skip everything (RecordScene already turns the lighting-adjacent effects off for those frames).
    if (pc.settings.shading != ViewShading::Lit || !pc.view.extraViews.empty()) return;

    // this share's enabled entries, in stack order
    struct StackItem {
        size_t index;
        const EffectStackEntry* entry;
    };
    std::vector<StackItem> entries;
    entries.reserve(pc.settings.packEffects.size());
    for (size_t i = 0; i < pc.settings.packEffects.size(); ++i) {
        const EffectStackEntry& e = pc.settings.packEffects[i];
        const ShaderPack* pack = ShaderPacks().Find(e.pack);
        if (!pack || pack->type != PackType::Effect || !pack->Selectable() || !e.enabled) continue;
        if ((pack->stage == PackEffectStage::PreBloom) != preBloom) continue;
        entries.push_back({i, &e});
    }
    if (entries.empty()) return;

    if (!AllocTargets(pc.ctx, t.outWidth, t.outHeight)) return;
    if (!preBloom && !AllocPostTargets(pc.ctx, t.outWidth, t.outHeight)) return;

    const bool outputSizeChanged = resetRequested_;
    resetRequested_ = false;

    float dt = 0.0f;
    if (pc.view.effectDt > 0.0f) {
        dt = pc.view.effectDt;
    } else {
        dt = std::clamp(pc.frameTimeMs * 0.001f, 1.0f / 240.0f, 0.1f);
    }

    Texture* src = preBloom ? (t.hdrFinal ? t.hdrFinal : &t.lit) : &t.ldr;
    Texture* pings[2] = {preBloom ? &ping_[0] : &post_[0], preBloom ? &ping_[1] : &post_[1]};
    src->Transition(pc.cmd, kSrv);
    t.depth.Transition(pc.cmd, kSrv);
    t.velocity.Transition(pc.cmd, kSrv);
    t.normal.Transition(pc.cmd, kSrv);

    uint32_t cur = 0, ran = 0;
    Texture* dst = pings[0];
    for (const StackItem& item : entries) {
        const EffectStackEntry* e = item.entry;
        const ShaderPack* pack = ShaderPacks().Find(e->pack);
        // per-effect texture folder: the entry's, else the pack-level folder (the registry setting)
        const std::filesystem::path folder =
            e->textureFolder.empty() ? ShaderPacks().TextureFolder(e->pack) : Utf8ToPath(e->textureFolder);
        const EffectPipelines* p = Pipelines(pc.ctx, e->pack, folder, preBloom);
        if (!p || !*p->pipe) continue;   // compile error (already logged + reported): the entry is skipped

        D3D12_GPU_DESCRIPTOR_HANDLE stateSrv = {};
        D3D12_GPU_DESCRIPTOR_HANDLE inputs = pc.transient.SrvTable(pc.ctx, {src, &t.depth, &t.velocity, &t.normal});
        D3D12_GPU_DESCRIPTOR_HANDLE packTex = p->texSrv != DescriptorHeap::kInvalid
                                                  ? pc.ctx.SrvHeap().Gpu(p->texSrv)
                                                  : D3D12_GPU_DESCRIPTOR_HANDLE{};
        const PackParamValues values = pack->Resolve(e->params);

        if (pack->stateFloats > 0) {
            StateKey key{item.index, e->pack};
            auto [it, inserted] = stateSlots_.try_emplace(key);
            StateSlot& slot = it->second;
            if (inserted) {
                const float zero[4] = {0, 0, 0, 0};
                slot.tex[0].Create(pc.ctx, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"packeffect.state.0", 1, 1, zero);
                slot.tex[1].Create(pc.ctx, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kRt, L"packeffect.state.1", 1, 1, zero);
                slot.cur = 0;
                slot.hasValidState = false;
                slot.lastPassExecution = 0;
            }

            const bool reset = inserted || !slot.hasValidState ||
                               (slot.lastPassExecution + 1 != passExecutionCount_) ||
                               pc.view.cameraCut || pc.view.effectStateReset || outputSizeChanged;

            const bool advance = (pc.view.effectStateAdvance && !pc.offscreen) || !slot.hasValidState;
            if (advance) {
                uint32_t prev = slot.cur;
                uint32_t next = slot.cur ^ 1;
                slot.tex[prev].Transition(pc.cmd, kSrv);
                slot.tex[next].Transition(pc.cmd, kRt);

                const float stateOutConst[16] = {
                    (float)t.outWidth, (float)t.outHeight, dt, reset ? 1.0f : 0.0f
                };
                D3D12_GPU_DESCRIPTOR_HANDLE prevSrv = pc.transient.SrvTable(pc.ctx, {&slot.tex[prev]});
                p->pipe->DrawState(pc, slot.tex[next], inputs, packTex, stateOutConst, values.data(), prevSrv);

                slot.tex[next].Transition(pc.cmd, kSrv);
                slot.cur ^= 1;
                slot.hasValidState = true;
            } else {
                slot.tex[slot.cur].Transition(pc.cmd, kSrv);
            }
            slot.lastPassExecution = passExecutionCount_;
            stateSrv = pc.transient.SrvTable(pc.ctx, {&slot.tex[slot.cur]});
        }

        const float outConst[16] = {(float)t.outWidth, (float)t.outHeight, dt, 0.0f};
        dst->Transition(pc.cmd, kRt);
        p->pipe->Draw(pc, *dst, inputs, packTex, outConst, values.data(), stateSrv);
        dst->Transition(pc.cmd, kSrv);
        src = dst;
        ++ran;
        cur ^= 1;
        dst = pings[cur];
    }

    std::set<StateKey> activeKeys;
    for (size_t i = 0; i < pc.settings.packEffects.size(); ++i) {
        activeKeys.insert({i, pc.settings.packEffects[i].pack});
    }
    for (auto it = stateSlots_.begin(); it != stateSlots_.end(); ) {
        if (activeKeys.find(it->first) == activeKeys.end()) {
            it->second.tex[0].Release(pc.ctx);
            it->second.tex[1].Release(pc.ctx);
            it = stateSlots_.erase(it);
        } else {
            ++it;
        }
    }

    if (ran) {
        // at least one effect ran: ping-pong back into the frame (a plain copy, same format on both sides)
        Texture& out = preBloom ? (t.hdrFinal ? *t.hdrFinal : t.lit) : t.ldr;
        out.Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        src->Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pc.cmd->CopyResource(out.res.Get(), src->res.Get());
        out.Transition(pc.cmd, kSrv);
        src->Transition(pc.cmd, kSrv);
    }
}

// Offline GI: does this share have an enabled entry whose pipeline compiles (the PSO is built here, once, as in RunStack)?
bool PackEffectPass::Ready(Dx12Context& ctx, const std::vector<EffectStackEntry>& stack) {
    for (const EffectStackEntry& e : stack) {
        const ShaderPack* pack = ShaderPacks().Find(e.pack);
        if (!pack || pack->type != PackType::Effect || !pack->Selectable() || !e.enabled) continue;
        if ((pack->stage == PackEffectStage::PreBloom) != preBloom_) continue;
        const std::filesystem::path folder =
            e.textureFolder.empty() ? ShaderPacks().TextureFolder(e.pack) : Utf8ToPath(e.textureFolder);
        const EffectPipelines* p = Pipelines(ctx, e.pack, folder, preBloom_);
        if (p && *p->pipe) return true;
    }
    return false;
}

// Offline GI: this share over `io`, ping-ponging through `scratch`. Same entries, pipelines and draws as RunStack, with
// the offline image as the frame; the effect inputs are the image's own G-buffer copies. The offline renderer keeps the
// frame in its own targets, so no RenderTargets are involved.
bool PackEffectPass::RunOffline(PassContext& pc, const std::vector<EffectStackEntry>& stack, Texture& io, Texture& scratch,
                                Texture& depth, Texture& velocity, Texture& normal, const OfflineJobDesc* job) {
    struct StackItem {
        size_t index;
        const EffectStackEntry* entry;
    };
    std::vector<StackItem> entries;
    entries.reserve(stack.size());
    for (size_t i = 0; i < stack.size(); ++i) {
        const EffectStackEntry& e = stack[i];
        const ShaderPack* pack = ShaderPacks().Find(e.pack);
        if (!pack || pack->type != PackType::Effect || !pack->Selectable() || !e.enabled) continue;
        if ((pack->stage == PackEffectStage::PreBloom) != preBloom_) continue;
        entries.push_back({i, &e});
    }
    if (entries.empty()) return false;

    ++passExecutionCount_;
    const bool isStill = job ? job->still : false;
    const float dt = isStill ? (1.0f / 60.0f) : (job && job->effectDt > 0.0f ? job->effectDt : (pc.view.effectDt > 0.0f ? pc.view.effectDt : (1.0f / 60.0f)));
    const bool jobReset = job ? job->effectReset : (pc.view.effectStateReset || pc.view.cameraCut);

    io.Transition(pc.cmd, kSrv);
    depth.Transition(pc.cmd, kSrv);
    velocity.Transition(pc.cmd, kSrv);
    normal.Transition(pc.cmd, kSrv);
    Texture* src = &io;         // read by the next entry
    Texture* other = &scratch;  // drawn into by the next entry
    bool ran = false;
    for (const StackItem& item : entries) {
        const EffectStackEntry* e = item.entry;
        const ShaderPack* pack = ShaderPacks().Find(e->pack);
        // per-effect texture folder: the entry's, else the pack-level folder (the registry setting)
        const std::filesystem::path folder =
            e->textureFolder.empty() ? ShaderPacks().TextureFolder(e->pack) : Utf8ToPath(e->textureFolder);
        const EffectPipelines* p = Pipelines(pc.ctx, e->pack, folder, preBloom_);
        if (!p || !*p->pipe) continue;   // compile error (already logged + reported): the entry is skipped

        D3D12_GPU_DESCRIPTOR_HANDLE stateSrv = {};
        D3D12_GPU_DESCRIPTOR_HANDLE inputs = pc.transient.SrvTable(pc.ctx, {src, &depth, &velocity, &normal});
        D3D12_GPU_DESCRIPTOR_HANDLE packTex = p->texSrv != DescriptorHeap::kInvalid
                                                  ? pc.ctx.SrvHeap().Gpu(p->texSrv)
                                                  : D3D12_GPU_DESCRIPTOR_HANDLE{};
        const PackParamValues values = pack->Resolve(e->params);

        if (pack->stateFloats > 0) {
            StateKey key{item.index, e->pack};
            auto [it, inserted] = stateSlots_.try_emplace(key);
            StateSlot& slot = it->second;
            if (inserted) {
                const float zero[4] = {0, 0, 0, 0};
                slot.tex[0].Create(pc.ctx, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"packeffect.state.0", 1, 1, zero);
                slot.tex[1].Create(pc.ctx, 4, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kRt, L"packeffect.state.1", 1, 1, zero);
                slot.cur = 0;
                slot.hasValidState = false;
                slot.lastPassExecution = 0;
            }

            const bool reset = inserted || !slot.hasValidState ||
                               (slot.lastPassExecution + 1 != passExecutionCount_) || jobReset;

            const uint32_t stateSteps = isStill ? 64 : 1;
            // one descriptor table per slot texture, reused by every step (the transient window is small)
            const D3D12_GPU_DESCRIPTOR_HANDLE slotSrv[2] = {pc.transient.SrvTable(pc.ctx, {&slot.tex[0]}),
                                                            pc.transient.SrvTable(pc.ctx, {&slot.tex[1]})};
            for (uint32_t step = 0; step < stateSteps; ++step) {
                uint32_t prev = slot.cur;
                uint32_t next = slot.cur ^ 1;
                slot.tex[prev].Transition(pc.cmd, kSrv);
                slot.tex[next].Transition(pc.cmd, kRt);

                const bool stepReset = (step == 0) && reset;
                const float stateOutConst[16] = {
                    (float)io.width, (float)io.height, dt, stepReset ? 1.0f : 0.0f
                };
                const D3D12_GPU_DESCRIPTOR_HANDLE prevSrv = slotSrv[prev];
                p->pipe->DrawState(pc, slot.tex[next], inputs, packTex, stateOutConst, values.data(), prevSrv);

                slot.tex[next].Transition(pc.cmd, kSrv);
                slot.cur ^= 1;
                slot.hasValidState = true;
            }
            slot.lastPassExecution = passExecutionCount_;
            stateSrv = slotSrv[slot.cur];
        }

        const float outConst[16] = {(float)io.width, (float)io.height, dt, 0.0f};
        other->Transition(pc.cmd, kRt);
        p->pipe->Draw(pc, *other, inputs, packTex, outConst, values.data(), stateSrv);
        other->Transition(pc.cmd, kSrv);
        std::swap(src, other);   // the result is now `src`, the texture it was read from is free
        ran = true;
    }

    std::set<StateKey> activeKeys;
    for (size_t i = 0; i < stack.size(); ++i) {
        activeKeys.insert({i, stack[i].pack});
    }
    for (auto it = stateSlots_.begin(); it != stateSlots_.end(); ) {
        if (activeKeys.find(it->first) == activeKeys.end()) {
            it->second.tex[0].Release(pc.ctx);
            it->second.tex[1].Release(pc.ctx);
            it = stateSlots_.erase(it);
        } else {
            ++it;
        }
    }

    if (ran && src != &io) {
        // an odd number of effects: copy the last result back into the image (same format on both sides)
        io.Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        src->Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pc.cmd->CopyResource(io.res.Get(), src->res.Get());
        io.Transition(pc.cmd, kSrv);
        src->Transition(pc.cmd, kSrv);
    }
    return ran;
}

void PackEffectPass::Execute(PassContext& pc) { RunStack(pc, preBloom_); }

} // namespace mmdx
