// Pass implementations (see Passes.h for the order and data flow).
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/GpuModel.h"
#include "render/Renderer.h"
#include "render/SpotShadowPick.h"
#include "render/RayTracing.h"
#include "render/Upscaler.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "render/MmeRuntime.h"
#include "render/MmeSemantics.h"
#include "render/MmeSkin.h"
#include "render/MmeScriptVM.h"
#include "asset/MmeText.h"
#include "asset/ImageLoader.h"
#include "core/TextUtil.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>

namespace mmdx {

namespace {

namespace fs = std::filesystem;

const D3D12_INPUT_ELEMENT_DESC kMmdLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},   // GpuSdef
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32_FLOAT, 3, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 3, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32_FLOAT, 3, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},   // GpuAddUv
    {"TEXCOORD", 9, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

bool CreateRootSignature(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& desc,
                         ComPtr<ID3D12RootSignature>& out, const char* what) {
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("%s: root signature serialization failed: %s", what,
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }
    return CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&out)),
                   what);
}

bool BindModelBuffers(ID3D12GraphicsCommandList* cmd, const GpuModel& model, uint64_t frame) {
    D3D12_VERTEX_BUFFER_VIEW vbs[5] = {model.VertexBufferView(), model.MorphBufferView(frame),
                                       model.PrevMorphBufferView(frame), model.SdefBufferView(),
                                       model.AddUvBufferView()};
    cmd->IASetVertexBuffers(0, 5, vbs);
    cmd->IASetIndexBuffer(&model.IndexBufferView());
    return true;
}

// ASCII-only lowercase (multibyte UTF-8/UTF-16 bytes are never folded): for the case-insensitive
// pack texture file-name comparisons, which only need to match through the ASCII subset on Windows.
std::wstring LowerAsciiWide(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return out;
}

} // namespace

// pack.json "textures" lookup: the user texture folder first (same relative path, then the same
// file name, then a file in that folder — not recursive — whose name ends with "_" + the declared
// file name, case-insensitive: users drop their own game rips in), then the pack folder (exact
// path only). False = missing (white). Shared with PackEffectPass (declared in ShaderPack.h).
bool ResolvePackTextureFile(const ShaderPack& pack, const std::string& file, const std::filesystem::path& userFolder,
                            std::filesystem::path& out) {
    const std::filesystem::path rel = Utf8ToPath(file);
    std::error_code ec;
    if (!userFolder.empty()) {
        for (const std::filesystem::path& c : {userFolder / rel, userFolder / rel.filename()})
            if (fs::is_regular_file(c, ec)) {
                out = c;
                return true;
            }
        // suffix match: <anything>_<declared file name>, not recursive, shortest name wins, ties by name order
        const std::wstring suffix = L"_" + rel.filename().wstring();
        const std::wstring suffixLower = LowerAsciiWide(suffix);
        std::filesystem::path best;
        for (const std::filesystem::directory_entry& e : fs::directory_iterator(userFolder, ec)) {
            if (ec) break;
            const std::wstring name = e.path().filename().wstring();
            if (name.size() <= suffix.size() || !e.is_regular_file(ec)) continue;
            if (LowerAsciiWide(name.substr(name.size() - suffix.size())) != suffixLower) continue;
            if (best.empty() || name.size() < best.filename().wstring().size() ||
                (name.size() == best.filename().wstring().size() && name < best.filename().wstring()))
                best = e.path();
        }
        if (!best.empty()) {
            out = std::move(best);
            return true;
        }
    }
    const std::filesystem::path inPack = pack.dir / rel;
    if (fs::is_regular_file(inPack, ec)) {
        out = inPack;
        return true;
    }
    return false;
}

// ---- ShadowPass -------------------------------------------------------------------------

bool ShadowPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER params[5];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    params[2].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_DESCRIPTOR_RANGE table;
    table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1);
    params[3].InitAsDescriptorTable(1, &table, D3D12_SHADER_VISIBILITY_PIXEL);
    params[4].InitAsConstants(20, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(5, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (!CreateRootSignature(device, rs, rootSig_, "ShadowPass: CreateRootSignature")) return false;

    ComPtr<ID3DBlob> vs = CompileShader(shaderDir / L"mmd.hlsl", "VSShadow", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderDir / L"mmd.hlsl", "PSShadowAlpha", "ps_5_1");
    if (!vs || !ps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {kMmdLayout, (UINT)std::size(kMmdLayout)};
    pso.pRootSignature = rootSig_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.SlopeScaledDepthBias = 1.5f;
    pso.RasterizerState.DepthBiasClamp = 0.01f;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoOpaque_)), "ShadowPass: PSO"))
        return false;
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    return CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoAlpha_)), "ShadowPass: PSO alpha");
}

uint32_t PointShadowCount(const LightParams& light, uint32_t maxLights) {
    uint32_t n = 0;
    const size_t count = std::min<size_t>(light.punctual.size(), maxLights);
    for (size_t i = 0; i < count; ++i)
        if (light.punctual[i].spotCosOuter <= -1.0f && light.punctual[i].castPointShadow &&
            light.punctual[i].shadow != LightShadowType::NoCast)
            ++n;
    return n;
}

// Renders every shadow-casting material into one depth slice (matrixIndex = gCascade: 0..2 the sun
// cascades, 3 + i spot slice i, 19 + i point slice i). Spot/point slices take characters only:
// preset lights hang on a truss, and re-drawing the stage per slice would be wasteful.
void ShadowPass::DrawSlice(PassContext& pc, const Texture& map, uint32_t slice, uint32_t matrixIndex,
                           bool charactersOnly, const DirectX::XMMATRIX* overrideMatrix, const PunctualLight* light) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    D3D12_VIEWPORT vp{0, 0, (float)map.width, (float)map.height, 0, 1};
    D3D12_RECT sc{0, 0, (LONG)map.width, (LONG)map.height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pc.ctx.DsvHeap().Cpu(map.dsv + slice);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmd->SetGraphicsRoot32BitConstant(4, matrixIndex, 0);
    if (overrideMatrix) {
        DirectX::XMFLOAT4X4 m;
        DirectX::XMStoreFloat4x4(&m, *overrideMatrix);
        cmd->SetGraphicsRoot32BitConstants(4, 16, &m, 4);
    }
    for (GpuModel* model : pc.view.models) {
        if (!model || (charactersOnly && model->Role() != ModelRole::Character)) continue;
        if (light) {
            DirectX::XMFLOAT3 center;
            float radius = 0.0f;
            if (model->LiveBoundingSphere(center, radius) && !LightReachesSphere(*light, center, radius)) continue;
        }
        // A sun cascade draws only the stage cells inside its rectangle (x / y: a caster in front of the near plane
        // still shadows). The spot / point slices draw characters and props only.
        GpuModel::CullFrustum frustum;
        const bool cull = !light && !overrideMatrix && matrixIndex < kShadowCascades && pc.sceneConstantsCpu &&
                          model->BuildCullFrustum(pc.sceneConstantsCpu->shadowViewProj[matrixIndex], false, frustum);
        BindModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        ID3D12PipelineState* bound = nullptr;
        for (const GpuModel::Material& m : model->Materials()) {
            if (!m.castShadow || m.indexCount == 0) continue;
            ID3D12PipelineState* want = m.alphaTested ? psoAlpha_.Get() : psoOpaque_.Get();
            if (want != bound) {
                cmd->SetPipelineState(want);
                bound = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            cmd->SetGraphicsRootDescriptorTable(3, pc.ctx.SrvHeap().Gpu(m.srvTable));
            pc.stats.drawCalls += model->DrawMaterial(cmd, m, cull ? &frustum : nullptr);
        }
    }
}

void ShadowPass::Execute(PassContext& pc) {
    Texture& sm = pc.targets.shadowMap;
    Texture& spot = pc.targets.spotShadowMap;
    Texture& point = pc.targets.pointShadowMap;
    if (!sm) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const bool cascades = pc.settings.shadows && !pc.view.shadowsOff && (pc.view.light.sunShadow != LightShadowType::NoCast) &&
                          !pc.view.models.empty() && (pc.path == RenderPath::Raster || (pc.settings.volumetric && !pc.offscreen));
    // must match Renderer::FillSceneConstants (spotShadowParams.x / pointShadowParams.x)
    // S7a: the spots whose slice the renderer picked (PassContext::spotSlices), else the first casting spots.
    std::vector<int> spotPick;
    if (spot && SpotShadowsWanted(pc.settings, pc.path, pc.offscreen)) {
        if (pc.spotSlices) {
            spotPick = *pc.spotSlices;
        } else {
            const size_t count = std::min<size_t>(pc.view.light.punctual.size(), Renderer::kMaxPunctualLights);
            spotPick.assign(count, -1);
            uint32_t slice = 0;
            for (size_t i = 0; i < count && slice < kSpotShadowSlices; ++i) {
                const PunctualLight& p = pc.view.light.punctual[i];
                if (p.spotCosOuter <= -1.0f || p.shadow == LightShadowType::NoCast) continue;
                spotPick[i] = (int)slice++;
            }
        }
    }
    uint32_t spots = 0;
    for (int s : spotPick) spots = std::max(spots, s < 0 ? 0u : (uint32_t)s + 1);
    const uint32_t points = (point && PointShadowsWanted(pc.settings, pc.path, pc.offscreen))
                                ? PointShadowCount(pc.view.light, Renderer::kMaxPunctualLights)
                                : 0;
    if (cascades || spots || points) {
        cmd->SetGraphicsRootSignature(rootSig_.Get());
        cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    if (cascades) {
        sm.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        for (uint32_t c = 0; c < kShadowCascades; ++c) DrawSlice(pc, sm, c, c, false);
    }
    sm.Transition(cmd, kSrv);
    if (spot) {
        if (spots) {
            spot.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            for (size_t i = 0; i < spotPick.size() && i < pc.view.light.punctual.size(); ++i)
                if (spotPick[i] >= 0)
                    DrawSlice(pc, spot, (uint32_t)spotPick[i], kShadowCascades + (uint32_t)spotPick[i], true, nullptr,
                              &pc.view.light.punctual[i]);
        }
        spot.Transition(cmd, kSrv);
    }
    if (point) {
        if (points) {
            point.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            uint32_t pointIdx = 0;
            const size_t count = std::min<size_t>(pc.view.light.punctual.size(), Renderer::kMaxPunctualLights);
            for (size_t i = 0; i < count && (pointIdx + 1) * kPointShadowFaces <= point.arraySize; ++i) {
                const PunctualLight& p = pc.view.light.punctual[i];
                if (p.spotCosOuter > -1.0f || !p.castPointShadow || p.shadow == LightShadowType::NoCast) continue;
                const float nearZ = std::max(0.05f, p.range * 0.004f);
                const float farZ = std::max(p.range, 1.0f);
                const DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1.0f, nearZ, farZ);
                const DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&p.position);
                static const struct { DirectX::XMVECTOR dir; DirectX::XMVECTOR up; } kFaces[6] = {
                    { DirectX::XMVectorSet( 1, 0, 0, 0), DirectX::XMVectorSet(0, 1, 0, 0) },
                    { DirectX::XMVectorSet(-1, 0, 0, 0), DirectX::XMVectorSet(0, 1, 0, 0) },
                    { DirectX::XMVectorSet( 0, 1, 0, 0), DirectX::XMVectorSet(0, 0,-1, 0) },
                    { DirectX::XMVectorSet( 0,-1, 0, 0), DirectX::XMVectorSet(0, 0, 1, 0) },
                    { DirectX::XMVectorSet( 0, 0, 1, 0), DirectX::XMVectorSet(0, 1, 0, 0) },
                    { DirectX::XMVectorSet( 0, 0,-1, 0), DirectX::XMVectorSet(0, 1, 0, 0) },
                };
                for (uint32_t f = 0; f < 6; ++f) {
                    const DirectX::XMMATRIX view = DirectX::XMMatrixLookToLH(eye, kFaces[f].dir, kFaces[f].up);
                    const DirectX::XMMATRIX vp = view * proj;
                    DrawSlice(pc, point, pointIdx * 6 + f, kShadowCascades + kSpotShadowSlices + pointIdx * 6 + f, true, &vp, &p);
                }
                ++pointIdx;
            }
        }
        point.Transition(cmd, kSrv);
    }
}

// ---- ScenePass ---------------------------------------------------------------------------

ScenePass::~ScenePass() = default;

// Model .emd blend-hint variants of the lit PSOs (see ScenePass::CreatePipelines); hasId = a 4-MRT ID variant.
static bool MakeBlendVariant(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base, bool additive,
                             ComPtr<ID3D12PipelineState>& back, ComPtr<ID3D12PipelineState>& noCull, const char* what,
                             bool hasId) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base;
    auto& rt0 = d.BlendState.RenderTarget[0];
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (additive) {
        rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;   // MMD additive colour blend; the alpha channel keeps the base
        rt0.DestBlend = D3D12_BLEND_ONE;
        rt0.BlendOp = D3D12_BLEND_OP_ADD;
    }
    d.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;   // normal target
    d.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;   // velocity target
    if (hasId && d.NumRenderTargets >= 4) {
        // Without independent blending (the non-ID variants) every target follows RT0, so the two lines above have
        // no effect there; the ID variants need independent blending (UINT target), so mirror that behaviour
        // exactly: normal / velocity follow RT0, the ID target is not written by blended passes.
        d.BlendState.RenderTarget[1] = rt0;
        d.BlendState.RenderTarget[2] = rt0;
        d.BlendState.RenderTarget[3].RenderTargetWriteMask = 0;
    }
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!CheckHr(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&back)), what)) return false;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    return CheckHr(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&noCull)), what);
}

bool ScenePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER params[16];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);   // SceneConstants
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);   // MaterialConstants
    params[2].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL);    // bones (shader packs read the head)
    CD3DX12_DESCRIPTOR_RANGE matTable, shadowTable;
    matTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1);
    params[3].InitAsDescriptorTable(1, &matTable, D3D12_SHADER_VISIBILITY_PIXEL);
    params[4].InitAsShaderResourceView(4, 0, D3D12_SHADER_VISIBILITY_VERTEX); // previous bones
    shadowTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
    params[5].InitAsDescriptorTable(1, &shadowTable, D3D12_SHADER_VISIBILITY_PIXEL);
    params[6].InitAsShaderResourceView(6, 0, D3D12_SHADER_VISIBILITY_PIXEL);  // lights
    params[7].InitAsShaderResourceView(0, 1, D3D12_SHADER_VISIBILITY_PIXEL);  // TLAS
    params[8].InitAsShaderResourceView(1, 1, D3D12_SHADER_VISIBILITY_PIXEL);  // RtGeometry[]
    // bindless segments of the shader-visible heap (must outlive rs.Init + serialization)
    CD3DX12_DESCRIPTOR_RANGE texAll, bufAll;
    texAll.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0);
    bufAll.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, 0);
    params[9].InitAsDescriptorTable(1, &texAll, D3D12_SHADER_VISIBILITY_PIXEL);
    params[10].InitAsDescriptorTable(1, &bufAll, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_DESCRIPTOR_RANGE spotTable;
    spotTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);
    params[11].InitAsDescriptorTable(1, &spotTable, D3D12_SHADER_VISIBILITY_PIXEL);  // spot shadow maps
    // pack textures (pack.json "textures", pack_api.hlsli gPackTex): a fixed 16-SRV table in its
    // own space, bound only when a model uses a pack
    CD3DX12_DESCRIPTOR_RANGE packTexTable;
    packTexTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kPackMaxTextures, 0, 5);
    params[12].InitAsDescriptorTable(1, &packTexTable, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_DESCRIPTOR_RANGE pointTable;
    pointTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);
    params[13].InitAsDescriptorTable(1, &pointTable, D3D12_SHADER_VISIBILITY_PIXEL); // point shadow maps
    // Per-material PBR record selection (work unit P2): model record-buffer SRV index + material index +
    // --pbr-debug channel + active flag, as root constants (b3).
    params[14].InitAsConstants(8, 3, 0, D3D12_SHADER_VISIBILITY_ALL);
    // params.ext: constant buffer for extended pack parameter slots (b6).
    params[15].InitAsConstantBufferView(6, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[4];
    samplers[0].Init(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 8);
    // s1 (gClamp) is ALL: the pack's edge vertex stage may sample pack textures with it (PackEdge)
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 16,
                     D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0.0f,
                     D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_ALL);
    samplers[2].Init(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                     D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0, 1,
                     D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0.0f,
                     D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
    // Init's 10th / 11th arguments are MinLOD / MaxLOD: passing the visibility there made MinLOD = 5 (PIXEL), so every
    // PBR map sampled mip 5 or coarser (a 2048 map looked like 64 px).
    samplers[3].Init(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 16,
                     D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0.0f,
                     D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(16, params, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (!CreateRootSignature(device, rs, rootSig_, "ScenePass: CreateRootSignature")) return false;

    const std::filesystem::path file = shaderDir / L"mmd.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(file, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> vsEdge = CompileShader(file, "VSEdge", "vs_5_1");
    ComPtr<ID3DBlob> psEdge = CompileShader(file, "PSEdge", "ps_5_1");
    ComPtr<ID3DBlob> vsSky = CompileShader(file, "VSSky", "vs_5_1");
    ComPtr<ID3DBlob> psSky = CompileShader(file, "PSSky", "ps_5_1");
    ComPtr<ID3DBlob> vsFloor = CompileShader(file, "VSFloor", "vs_5_1");
    ComPtr<ID3DBlob> psWire = CompileShader(file, "PSWire", "ps_5_1");
    ComPtr<ID3DBlob> psWireBg = CompileShader(file, "PSWireBg", "ps_5_1");
    ComPtr<ID3DBlob> psWireFloor = CompileShader(file, "PSWireFloor", "ps_5_1");
    if (!vs || !vsEdge || !psEdge || !vsSky || !psSky || !vsFloor || !psWire || !psWireBg || !psWireFloor)
        return false;
    // The lit and floor PSOs: FXC needs minutes for PSMain since the PBR branch (2.0.0: 332 s cold vs 1.2 s with
    // DXC, PSFloor 14 s), so SM 6.0 devices get DXC bytecode for both stages (a PSO cannot mix DXBC and DXIL; the
    // other PSOs below keep the FXC `vs` / `vsFloor`). FXC stays the fallback.
    ComPtr<ID3DBlob> vsLit, ps, vsFloorLit, psFloor;
    if (ctx.Caps().shaderModel >= D3D_SHADER_MODEL_6_0) {
        vsLit = CompileShaderDxc(file, "VSMain", "vs_6_0");
        ps = vsLit ? CompileShaderDxc(file, "PSMain", "ps_6_0") : nullptr;
        vsFloorLit = ps ? CompileShaderDxc(file, "VSFloor", "vs_6_0") : nullptr;
        psFloor = vsFloorLit ? CompileShaderDxc(file, "PSFloor", "ps_6_0") : nullptr;
        if (!psFloor) LOG_WARN("ScenePass: DXC lit shaders unavailable, compiling with FXC");
    }
    if (!psFloor) {
        vsLit = vs;
        ps = CompileShader(file, "PSMain", "ps_5_1");
        vsFloorLit = vsFloor;
        psFloor = CompileShader(file, "PSFloor", "ps_5_1");
        if (!ps || !psFloor) return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {kMmdLayout, (UINT)std::size(kMmdLayout)};
    pso.pRootSignature = rootSig_.Get();
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.MultisampleEnable = msaa > 1 ? TRUE : FALSE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    {
        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 3;
    pso.RTVFormats[0] = RenderTargets::kColorFormat;
    pso.RTVFormats[1] = RenderTargets::kNormalFormat;
    pso.RTVFormats[2] = RenderTargets::kVelocityFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = msaa > 1 ? msaa : 1;

    pso.VS = {vsLit->GetBufferPointer(), vsLit->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    shaderDir_ = shaderDir;
    litDesc_ = pso;
    psLit_ = ps;   // litDesc_.PS points into it: the S5 feed PSOs derive from litDesc_ after this call returns
    litDescId_ = pso;
    litDescId_.NumRenderTargets = 4;
    litDescId_.RTVFormats[3] = RenderTargets::kIdFormat;
    // A UINT target cannot blend: independent blend states, with the colour / normal / velocity targets keeping the
    // shared RT0 state they get without independent blending.
    litDescId_.BlendState.IndependentBlendEnable = TRUE;
    litDescId_.BlendState.RenderTarget[1] = litDescId_.BlendState.RenderTarget[0];
    litDescId_.BlendState.RenderTarget[2] = litDescId_.BlendState.RenderTarget[0];
    litDescId_.BlendState.RenderTarget[3] = CD3DX12_BLEND_DESC(D3D12_DEFAULT).RenderTarget[0];
    litDescId_.BlendState.RenderTarget[3].BlendEnable = FALSE;
    litDescId_.BlendState.RenderTarget[3].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    vs_ = vsLit;
    vsRt_.Reset();
    vsDxc_.Reset();
    ReleasePackPsos(ctx);
    // S5: the feed PSOs are derived from litDesc_: rebuild them with the lit PSOs.
    feedTried_ = false;
    psoFeedBack_.Reset();
    psoFeedNoCull_.Reset();
    psoFeedAddBack_.Reset();
    psoFeedAddNoCull_.Reset();
    psoFeedNdBack_.Reset();
    psoFeedNdNoCull_.Reset();
    // ID-target variants (inputs.id effects) are built on first use (EnsureIdPipelines): no startup cost otherwise.
    idTried_ = false;
    for (ComPtr<ID3D12PipelineState>* p : {std::addressof(psoCullBackId_), std::addressof(psoNoCullId_), std::addressof(psoAddBackId_), std::addressof(psoAddNoCullId_),
                                           std::addressof(psoNdBackId_), std::addressof(psoNdNoCullId_), std::addressof(psoCullBackRtId_), std::addressof(psoNoCullRtId_),
                                           std::addressof(psoAddBackRtId_), std::addressof(psoAddNoCullRtId_), std::addressof(psoNdBackRtId_), std::addressof(psoNdNoCullRtId_)})
        p->Reset();
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoCullBack_)), "ScenePass: PSO back"))
        return false;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoNoCull_)), "ScenePass: PSO no cull"))
        return false;


    // Model .emd blend-hint variants of the lit PSOs (MatFlag_Additive / MatFlag_NoDepthWrite; see GpuModel).
    // NoDepthWrite: the lit blend with depth write OFF. Additive: SrcBlend SRC_ALPHA / DestBlend ONE / ADD (MMD
    // additive), depth write OFF. Both write neither the normal (MRT1) nor the velocity (MRT2) target, so
    // SSAO / SSR / TAA / DoF keep reading the underlying surface. `base` supplies the VS / PS (lit, or the RT pair).
    const auto makeBlendVariant = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base, bool additive,
                                      ComPtr<ID3D12PipelineState>& back, ComPtr<ID3D12PipelineState>& noCull,
                                      const char* what, bool hasId = false) -> bool {
        return MakeBlendVariant(device, base, additive, back, noCull, what, hasId);
    };
    if (!makeBlendVariant(pso, false, psoNdBack_, psoNdNoCull_, "ScenePass: PSO no-depth-write")) return false;
    if (!makeBlendVariant(pso, true, psoAddBack_, psoAddNoCull_, "ScenePass: PSO additive")) return false;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};   // FXC: the descs derived below pair it with FXC PSs

    // Floor desc: needed by both the raster and RT floor PSOs; built here so the RT block below
    // can copy it before the edge/sky descs mutate `pso`.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC floor = pso;
    floor.InputLayout = {nullptr, 0};
    floor.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    floor.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // Ray-traced shadow variants (RT_SHADOWS=1): same raster state, DXC bytecodes. Built from a
    // copy before the edge/sky descs below mutate `pso`.
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC rasterPso = pso;
    if (RtPipelinesSupported(ctx)) {
        const ShaderDefines rtDefines = {{"RT_SHADOWS", "1"}};
        ComPtr<ID3DBlob> vsRt = CompileShaderDxc(file, "VSMain", "vs_6_5", rtDefines);
        ComPtr<ID3DBlob> psRt = CompileShaderDxc(file, "PSMain", "ps_6_5", rtDefines);
        ComPtr<ID3DBlob> vsFloorRt = CompileShaderDxc(file, "VSFloor", "vs_6_5", rtDefines);
        ComPtr<ID3DBlob> psFloorRt = CompileShaderDxc(file, "PSFloor", "ps_6_5", rtDefines);
        if (vsRt && psRt && vsFloorRt && psFloorRt) {
            vsRt_ = vsRt;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoRt = rasterPso;
            psoRt.VS = {vsRt->GetBufferPointer(), vsRt->GetBufferSize()};
            psoRt.PS = {psRt->GetBufferPointer(), psRt->GetBufferSize()};
            psoRt.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            bool okRt = CheckHr(device->CreateGraphicsPipelineState(&psoRt, IID_PPV_ARGS(&psoCullBackRt_)),
                                "ScenePass: PSO back rt");
            psoRt.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            okRt = okRt && CheckHr(device->CreateGraphicsPipelineState(&psoRt, IID_PPV_ARGS(&psoNoCullRt_)),
                                   "ScenePass: PSO no cull rt");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC floorRt = floor;
            floorRt.VS = {vsFloorRt->GetBufferPointer(), vsFloorRt->GetBufferSize()};
            floorRt.PS = {psFloorRt->GetBufferPointer(), psFloorRt->GetBufferSize()};
            okRt = okRt && CheckHr(device->CreateGraphicsPipelineState(&floorRt, IID_PPV_ARGS(&psoFloorRt_)),
                                   "ScenePass: PSO floor rt");
            // additive / no-depth-write lit variants (same ps_6_5 RT_SHADOWS pair)
            D3D12_GRAPHICS_PIPELINE_STATE_DESC litRt = rasterPso;
            litRt.VS = {vsRt->GetBufferPointer(), vsRt->GetBufferSize()};
            litRt.PS = {psRt->GetBufferPointer(), psRt->GetBufferSize()};
            okRt = okRt && makeBlendVariant(litRt, false, psoNdBackRt_, psoNdNoCullRt_, "ScenePass: PSO no-depth-write rt");
            okRt = okRt && makeBlendVariant(litRt, true, psoAddBackRt_, psoAddNoCullRt_, "ScenePass: PSO additive rt");
            if (!okRt) {
                psoCullBackRt_.Reset();
                psoNoCullRt_.Reset();
                psoFloorRt_.Reset();
                psoAddBackRt_.Reset();
                psoAddNoCullRt_.Reset();
                psoNdBackRt_.Reset();
                psoNdNoCullRt_.Reset();
            }
        }
        if (!psoCullBackRt_ || !psoNoCullRt_ || !psoFloorRt_)
            LOG_WARN("ScenePass: ray-traced shadow pipelines unavailable");
    }

    pso.VS = {vsEdge->GetBufferPointer(), vsEdge->GetBufferSize()};
    pso.PS = {psEdge->GetBufferPointer(), psEdge->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoEdge_)), "ScenePass: PSO edge"))
        return false;

    // Path tracer outlines: the depth pre-pass (lit desc, colour writes off; optional: PT then draws no outlines)
    psoDepthBack_.Reset();
    psoDepthNoCull_.Reset();
    if (ComPtr<ID3DBlob> psDepth = CompileShader(file, "PSDepthAlpha", "ps_5_1")) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC depth = rasterPso;
        depth.PS = {psDepth->GetBufferPointer(), psDepth->GetBufferSize()};
        depth.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        for (auto& r : depth.BlendState.RenderTarget) r.RenderTargetWriteMask = 0;
        depth.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        if (FAILED(device->CreateGraphicsPipelineState(&depth, IID_PPV_ARGS(&psoDepthBack_)))) psoDepthBack_.Reset();
        depth.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (FAILED(device->CreateGraphicsPipelineState(&depth, IID_PPV_ARGS(&psoDepthNoCull_)))) psoDepthBack_.Reset();
    }
    if (!psoDepthBack_) LOG_WARN("ScenePass: depth pre-pass unavailable, the path tracer draws no outlines");

    // The raster camera view's depth pre-pass: the lit PSO's own vertex shader (the same clip positions, LESS_EQUAL lets
    // the lit draw through), no pixel shader, no colour targets. Optional: without it the view just overdraws.
    psoPrepassBack_.Reset();
    psoPrepassNoCull_.Reset();
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC prepass = rasterPso;
        prepass.VS = {vsLit->GetBufferPointer(), vsLit->GetBufferSize()};
        prepass.PS = {nullptr, 0};
        prepass.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        prepass.NumRenderTargets = 0;
        for (DXGI_FORMAT& format : prepass.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
        prepass.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        prepass.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        if (FAILED(device->CreateGraphicsPipelineState(&prepass, IID_PPV_ARGS(&psoPrepassBack_))))
            psoPrepassBack_.Reset();
        prepass.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (FAILED(device->CreateGraphicsPipelineState(&prepass, IID_PPV_ARGS(&psoPrepassNoCull_))))
            psoPrepassNoCull_.Reset();
    }

    // DLSS-RR guides (RayTraced): the lit MRT layout with PSGuides, compiled like PSMain (DXC when available).
    // Built only here; ScenePass::DrawGuides runs only while RR is active, so RR-off frames have no extra cost.
    psoGuideBack_.Reset();
    psoGuideNoCull_.Reset();
    {
        ComPtr<ID3DBlob> psGuide;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC guide = rasterPso;
        if (ctx.Caps().shaderModel >= D3D_SHADER_MODEL_6_0 && litDesc_.VS.pShaderBytecode) {
            psGuide = CompileShaderDxc(file, "PSGuides", "ps_6_0");
            guide.VS = litDesc_.VS;
        } else {
            psGuide = CompileShader(file, "PSGuides", "ps_5_1");
            guide.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        }
        if (psGuide) {
            guide.PS = {psGuide->GetBufferPointer(), psGuide->GetBufferSize()};
            guide.NumRenderTargets = 3;
            guide.RTVFormats[0] = RenderTargets::kColorFormat;
            guide.RTVFormats[1] = RenderTargets::kColorFormat;
            guide.RTVFormats[2] = RenderTargets::kColorFormat;
            // RR requires the DLSS upscaler, which forces single-sample targets: the guides are 1x.
            guide.SampleDesc.Count = 1;
            guide.RasterizerState.MultisampleEnable = FALSE;
            // Translucent layers (eye shadow, blush, highlights over the face) blend into the guides by their
            // coverage like the colour does: written opaque, a faint dark overlay became the face's albedo and RR
            // demodulated it into dark patches ("panda eyes"). Destination alpha is kept. Only the two albedo
            // targets blend: the normal target packs roughness in w (RR's packed roughness mode), written opaque.
            guide.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            for (int rt = 0; rt < 2; ++rt) {
                D3D12_RENDER_TARGET_BLEND_DESC& b = guide.BlendState.RenderTarget[rt];
                b.BlendEnable = TRUE;
                b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                b.BlendOp = D3D12_BLEND_OP_ADD;
                b.SrcBlendAlpha = D3D12_BLEND_ZERO;
                b.DestBlendAlpha = D3D12_BLEND_ONE;
                b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            guide.BlendState.IndependentBlendEnable = TRUE;
            guide.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            guide.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            guide.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            if (FAILED(device->CreateGraphicsPipelineState(&guide, IID_PPV_ARGS(&psoGuideBack_)))) psoGuideBack_.Reset();
            guide.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            if (FAILED(device->CreateGraphicsPipelineState(&guide, IID_PPV_ARGS(&psoGuideNoCull_)))) psoGuideNoCull_.Reset();
        }
    }
    if (!psoGuideBack_) LOG_WARN("ScenePass: DLSS-RR guides unavailable");

    // Wireframe model variants (D3D12_FILL_MODE_WIREFRAME, flat PSWire): same MRT layout and
    // blend as the lit PSO so the resolve / composite passes run unchanged. Line width is fixed
    // at 1 px (the rasterizer cannot draw wider lines); the vertex stage is VSMain (same skinning).
    // Built after the edge PSOs, from the same `pso` desc (the RT block above already copied it).
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {psWire->GetBufferPointer(), psWire->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoWireBack_)), "ScenePass: PSO wire back"))
        return false;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoWireNoCull_)), "ScenePass: PSO wire no cull"))
        return false;

    // Sky: fullscreen at the far plane, drawn first, no depth test/write, opaque.
    // SOLID: the desc inherits the wireframe fill mode from the wire block above.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC sky = pso;
    sky.InputLayout = {nullptr, 0};
    sky.VS = {vsSky->GetBufferPointer(), vsSky->GetBufferSize()};
    sky.PS = {psSky->GetBufferPointer(), psSky->GetBufferSize()};
    sky.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    sky.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    sky.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    sky.DepthStencilState.DepthEnable = FALSE;
    sky.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    if (!CheckHr(device->CreateGraphicsPipelineState(&sky, IID_PPV_ARGS(&psoSky_)), "ScenePass: PSO sky"))
        return false;
    {   // S5 / S10: camera feeds and mirrors draw the sky into single-sample targets (DrawFeed): a 1x twin. The MSAA sky
        // PSO there is a sample-count mismatch (undefined behaviour; flagged by the debug layer, issue #8).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC sky1 = sky;
        sky1.SampleDesc = {1, 0};
        sky1.RasterizerState.MultisampleEnable = FALSE;
        if (!CheckHr(device->CreateGraphicsPipelineState(&sky1, IID_PPV_ARGS(&psoSkyFeed_)), "ScenePass: PSO sky feed"))
            return false;
    }
    // Flat background (wireframe): the same draw with one flat colour.
    sky.PS = {psWireBg->GetBufferPointer(), psWireBg->GetBufferSize()};
    if (!CheckHr(device->CreateGraphicsPipelineState(&sky, IID_PPV_ARGS(&psoFlatBg_)), "ScenePass: PSO flat bg"))
        return false;

    floor.VS = {vsFloorLit->GetBufferPointer(), vsFloorLit->GetBufferSize()};
    floor.PS = {psFloor->GetBufferPointer(), psFloor->GetBufferSize()};
    bool okFloor = CheckHr(device->CreateGraphicsPipelineState(&floor, IID_PPV_ARGS(&psoFloor_)), "ScenePass: PSO floor");
    // Wireframe floor: the two-triangle quad as its 5 edges, flat colour.
    floor.VS = {vsFloor->GetBufferPointer(), vsFloor->GetBufferSize()};
    floor.PS = {psWireFloor->GetBufferPointer(), psWireFloor->GetBufferSize()};
    floor.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    okFloor = okFloor &&
              CheckHr(device->CreateGraphicsPipelineState(&floor, IID_PPV_ARGS(&psoWireFloor_)), "ScenePass: PSO wire floor");
    return okFloor;
}

namespace {

// packPsos_ texture-set key for one resolved per-model folder ("" = the pack folder only).
std::string PackSetKey(const std::filesystem::path& folder) {
    return PathToUtf8(folder);
}

} // namespace

const ScenePass::PackPipelines* ScenePass::PackPsos(Dx12Context& ctx, const std::string& id,
                                                    const std::filesystem::path& textureFolder, bool rt, bool hasId) {
    ShaderPackRegistry& reg = ShaderPacks();
    if (reg.Generation() != packGeneration_) {
        ReleasePackPsos(ctx);   // reload: the old PSOs / pack textures may still be in flight
        packGeneration_ = reg.Generation();
    }
    const ShaderPack* pack = reg.Find(id);
    if (!pack || !pack->Selectable()) return nullptr;
    PackPipelines& p = packPsos_[id];
    if (p.failed) return nullptr;
    const std::filesystem::path file = shaderDir_ / L"mmd.hlsl";
    // the surface as an include path relative to mmd.hlsl (built-in packs/<id>, user ../shader_packs/<id>)
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(pack->dir / L"surface.hlsl", shaderDir_, ec);
    if (ec || rel.empty()) rel = pack->dir / L"surface.hlsl";
    std::string inc = PathToUtf8(rel);
    std::replace(inc.begin(), inc.end(), '\\', '/');
    const std::string incDefine = "\"" + inc + "\"";
    const auto fail = [&](const char* what) -> const PackPipelines* {
        LOG_ERROR("shader pack '%s': %s failed, the model uses the default shading", id.c_str(), what);
        reg.ReportError(id, what);
        for (auto& [key, set] : p.sets)
            if (set.srv != DescriptorHeap::kInvalid) {
                ctx.SrvHeap().Free(set.srv, kPackMaxTextures);
                set.srv = DescriptorHeap::kInvalid;
            }
        p = {};
        p.failed = true;
        return nullptr;
    };
    ID3D12Device* device = ctx.Device();
    // the pack's textures (pack.json "textures"): uploaded once per texture set (pack id + resolved
    // folder, so game texture sets can differ per character), state as compile defines
    PackPipelines::TextureSet& set = p.sets[PackSetKey(textureFolder)];
    if (!set.loaded && !EnsurePackTextures(ctx, *pack, set, textureFolder)) return fail("pack textures");
    const uint32_t texCount = (uint32_t)std::min<size_t>(pack->textures.size(), kPackMaxTextures);
    uint32_t clampMask = 0, srgbMask = 0;
    for (uint32_t i = 0; i < texCount; ++i) {
        if (pack->textures[i].clamp) clampMask |= 1u << i;
        if (pack->textures[i].srgb) srgbMask |= 1u << i;
    }
    ShaderDefines packDefines = {{"MMDX_PACK", incDefine},
                                 {"PACK_TEX_COUNT", std::to_string(texCount)},
                                 {"PACK_TEX_CLAMP_MASK", std::to_string(clampMask) + "u"},
                                 {"PACK_TEX_SRGB_MASK", std::to_string(srgbMask) + "u"}};
    if (pack->HasCap("inputs.scene")) packDefines.push_back({"PACK_CAP_INPUTS_SCENE", "1"});
    if (pack->HasCap("inputs.audio")) packDefines.push_back({"PACK_CAP_INPUTS_AUDIO", "1"});
    if (pack->HasCap("surface.output")) packDefines.push_back({"PACK_CAP_SURFACE_OUTPUT", "1"});
    if (pack->HasCap("inputs.id")) packDefines.push_back({"PACK_CAP_INPUTS_ID", "1"});
    if (pack->HasCap("passes.history")) packDefines.push_back({"PACK_CAP_PASSES_HISTORY", "1"});
    if (pack->HasCap("passes.mips")) packDefines.push_back({"PACK_CAP_PASSES_MIPS", "1"});
    if (pack->HasCap("params.ext")) {
        packDefines.push_back({"PACK_CAP_PARAMS_EXT", "1"});
        packDefines.push_back({"PACK_PARAM_COUNT", std::to_string(pack->params.size())});
    }
    if (!p.back) {
        // DXC (FXC cannot #include a macro): shader model 6.0, so the VS is a DXC build of VSMain as well
        if (!vsDxc_) vsDxc_ = CompileShaderDxc(file, "VSMain", "vs_6_0");
        std::string errors;
        ComPtr<ID3DBlob> ps = vsDxc_ ? CompileShaderDxc(file, "PSPack", "ps_6_0", packDefines, &errors) : nullptr;
        if (!ps) return fail(errors.empty() ? "shader compile" : errors.c_str());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDesc_;
        d.VS = {vsDxc_->GetBufferPointer(), vsDxc_->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.back)))) return fail("PSO");
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.noCull)))) return fail("PSO");
        reg.ReportCompiled(id);
        LOG_INFO("shader pack '%s': compiled (%s)", id.c_str(), inc.c_str());
    }
    if (pack->hasEdge && !p.edge) {
        // pack outlines (PACK_HAS_EDGE + PackEdge in surface.hlsl): a pack variant of the edge pass;
        // without it the model draws with the default edge PSOs
        std::string errors;
        ComPtr<ID3DBlob> vsE = CompileShaderDxc(file, "VSEdge", "vs_6_0", packDefines, &errors);
        ComPtr<ID3DBlob> psE = vsE ? CompileShaderDxc(file, "PSEdgePack", "ps_6_0", packDefines, &errors) : nullptr;
        if (vsE && psE) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDesc_;
            d.VS = {vsE->GetBufferPointer(), vsE->GetBufferSize()};
            d.PS = {psE->GetBufferPointer(), psE->GetBufferSize()};
            d.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
            if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.edge)))) p.edge.Reset();
        }
        if (!p.edge) LOG_ERROR("shader pack '%s': edge variant failed, using the default edges", id.c_str());
    }
    if (rt && !p.rtTried && vsRt_) {
        // ray-traced sun shadows: DXC variant; without it the raster pack PSOs draw (shadow maps)
        p.rtTried = true;
        ShaderDefines rtDefines = packDefines;
        rtDefines.push_back({"RT_SHADOWS", "1"});
        ComPtr<ID3DBlob> ps = CompileShaderDxc(file, "PSPack", "ps_6_5", rtDefines);
        if (ps) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDesc_;
            d.VS = {vsRt_->GetBufferPointer(), vsRt_->GetBufferSize()};
            d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            if (SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.backRt)))) {
                d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.noCullRt)))) p.backRt.Reset();
            }
        }
        if (!p.backRt) LOG_ERROR("shader pack '%s': ray-traced variant failed, using shadow maps", id.c_str());
    }
    if (hasId && !p.backId) {
        if (!vsDxc_) vsDxc_ = CompileShaderDxc(file, "VSMain", "vs_6_0");
        ShaderDefines packIdDefines = packDefines;
        packIdDefines.push_back({"SCENE_HAS_ID", "1"});
        std::string errors;
        ComPtr<ID3DBlob> ps = vsDxc_ ? CompileShaderDxc(file, "PSPack", "ps_6_0", packIdDefines, &errors) : nullptr;
        if (ps) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDescId_;
            d.VS = {vsDxc_->GetBufferPointer(), vsDxc_->GetBufferSize()};
            d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            if (SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.backId)))) {
                d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.noCullId)))) p.backId.Reset();
            }
        }
    }
    if (hasId && rt && vsRt_ && !p.backRtId) {
        ShaderDefines rtIdDefines = packDefines;
        rtIdDefines.push_back({"RT_SHADOWS", "1"});
        rtIdDefines.push_back({"SCENE_HAS_ID", "1"});
        ComPtr<ID3DBlob> ps = CompileShaderDxc(file, "PSPack", "ps_6_5", rtIdDefines);
        if (ps) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDescId_;
            d.VS = {vsRt_->GetBufferPointer(), vsRt_->GetBufferSize()};
            d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            if (SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.backRtId)))) {
                d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.noCullRtId)))) p.backRtId.Reset();
            }
        }
    }
    return &p;
}

void ScenePass::ReleasePackPsos(Dx12Context& ctx) {
    if (!packPsos_.empty()) ctx.WaitForGpu();
    for (auto& [id, p] : packPsos_)
        for (auto& [key, set] : p.sets)
            if (set.srv != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(set.srv, kPackMaxTextures);
    packPsos_.clear();
}

// pack.json "textures" for one texture set: loaded with the image loader, uploaded in one batch per
// set (DEFAULT heap) and shared by every model resolving to the same folder. A missing / unusable
// texture falls back to white ([W] per texture, once per load; the manager shows the pack-level
// folder's count). Runs once per set per generation (registry reload), right before the pack's PSOs
// are (re)compiled.
bool ScenePass::EnsurePackTextures(Dx12Context& ctx, const ShaderPack& pack, PackPipelines::TextureSet& set,
                                   const std::filesystem::path& folder) {
    return PackTextures::Upload(ctx, pack, set, folder);
}

// Path tracer outlines, as the offline GI renderer draws them (OfflineRenderer::DrawEdges): the skinned meshes'
// depth, then the inverted-hull edges (default, or the pack's PackEdge in the lit view) into the MSAA scene targets
// cleared to zero, with the same jittered camera the tracer uses. PathTracePass composites the layer over its
// denoised colour (pt_edge.hlsl). Camera view only (PT has no extra views).
void ScenePass::DrawPtEdges(PassContext& pc) {
    if (!psoDepthBack_ || !psoDepthNoCull_) return;
    // M5b: MME-owned materials are rasterised into this same layer, so it is produced even with outlines off when an
    // MME object effect exists (the depth pre-pass occludes both the outlines and the MME layer).
    bool mmeAny = false;
    if (pc.mme && pc.mmeSkin)
        for (GpuModel* model : pc.view.models)
            if (model && model->HasMmeEffects()) { mmeAny = true; break; }
    if (!pc.settings.drawEdges && !mmeAny && pc.view.worlds.empty()) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& t = pc.targets;

    t.colorMsaa.Transition(cmd, kRt);
    t.normalMsaa.Transition(cmd, kRt);
    t.velocityMsaa.Transition(cmd, kRt);
    t.depthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {ctx.RtvHeap().Cpu(t.colorMsaa.rtv), ctx.RtvHeap().Cpu(t.normalMsaa.rtv),
                                           ctx.RtvHeap().Cpu(t.velocityMsaa.rtv)};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(t.depthMsaa.dsv);
    const float zero[4] = {0, 0, 0, 0};
    for (auto& r : rtvs) cmd->ClearRenderTargetView(r, zero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

    const D3D12_GPU_DESCRIPTOR_HANDLE shadowTable = pc.transient.SrvTable(ctx, {&t.shadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE spotTable = pc.transient.SrvTable(ctx, {&t.spotShadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE pointTable = pc.transient.SrvTable(ctx, {&t.pointShadowMap});
    const float* r = pc.view.mainRect;
    const float px = std::floor(r[0] * (float)t.width), py = std::floor(r[1] * (float)t.height);
    const float pw = std::floor((r[0] + r[2]) * (float)t.width) - px;
    const float ph = std::floor((r[1] + r[3]) * (float)t.height) - py;
    D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
    D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
    // ScenePass's own root state for this layer; the MME draws switch root signature + heaps, so it is re-bound after
    // them (the same pattern Execute uses around DrawMmeModel / DrawMmeEdges).
    const auto bindLayerRoot = [&]() {
        cmd->SetGraphicsRootSignature(rootSig_.Get());
        cmd->SetGraphicsRootDescriptorTable(5, shadowTable);
        cmd->SetGraphicsRootDescriptorTable(11, spotTable);
        cmd->SetGraphicsRootDescriptorTable(13, pointTable);
        cmd->SetGraphicsRootShaderResourceView(6, pc.lights);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    };
    bindLayerRoot();

    // pass 0: depth pre-pass of every model (occludes the MME layer and the outlines). M5b fix 1: materials the
    // MME layer owns are skipped, so the MME draw depth-tests against the non-MME geometry (and other MME
    // materials) instead of its own differently-vertexed surfaces (z-fighting).
    for (GpuModel* model : pc.view.models) {
        if (!model) continue;
        BindModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
        ID3D12PipelineState* curPso = nullptr;
        for (const GpuModel::Material& m : model->Materials()) {
            if (m.indexCount == 0) continue;
            if (!m.visible) continue;
            if (m.additive || m.noDepthWrite) continue;   // .emd blend hints: no edge draw (See ScenePass::Execute)
            if (MmeMaterialOwned(pc.mme, *model, (size_t)(&m - model->Materials().data()))) continue;   // MME owns it
            ID3D12PipelineState* want = m.doubleSided ? psoDepthNoCull_.Get() : psoDepthBack_.Get();
            if (want != curPso) {
                cmd->SetPipelineState(want);
                curPso = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
            cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
            pc.stats.drawCalls++;
        }
    }

    // M5b: the MME primary layer, after the depth pre-pass and before the outlines. DrawMmeModel writes the host MRT
    // (linearised colour, normal, velocity) with the tracer's jittered camera; an MME edge technique replaces the
    // native outline for its model in this layer.
    std::vector<uint8_t> mmeEdgeDrew(pc.view.models.size(), 0);
    if (mmeAny) {
        for (size_t i = 0; i < pc.view.models.size(); ++i) {
            GpuModel* model = pc.view.models[i];
            if (!model || !model->HasMmeEffects()) continue;
            std::vector<uint8_t> handled;
            DrawMmeModel(pc, *model, false, handled);
            if (DrawMmeEdges(pc, *model)) mmeEdgeDrew[i] = 1;
        }
        ID3D12DescriptorHeap* restore[1] = {ctx.SrvHeap().Heap()};
        cmd->SetDescriptorHeaps(1, restore);
        bindLayerRoot();
    }

    // pass 1: outlines (skipped when outlines are off, or replaced by the model's MME edge technique)
    if (pc.settings.drawEdges) {
        for (size_t mi = 0; mi < pc.view.models.size(); ++mi) {
            GpuModel* model = pc.view.models[mi];
            if (!model || mmeEdgeDrew[mi]) continue;
            BindModelBuffers(cmd, *model, pc.frame);
            cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
            cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
            if (model->HasPackParamsExt())
                cmd->SetGraphicsRootConstantBufferView(15, model->PackParamsExtBuffer(pc.frame));
            ID3D12PipelineState* defaultEdge = psoEdge_.Get();
            ID3D12PipelineState* packEdge = defaultEdge;
            if (!model->ShaderPackId().empty() && pc.settings.shading == ViewShading::Lit) {
                // the pack's outlines, as the raster lit view draws them
                const PackPipelines* pack = PackPsos(ctx, model->ShaderPackId(), model->ShaderTextureFolder(), false);
                if (pack && pack->edge) {
                    const auto it = pack->sets.find(PackSetKey(model->ShaderTextureFolder()));
                    if (it != pack->sets.end() && it->second.srv != DescriptorHeap::kInvalid)   // pack_api.hlsli gPackTex
                        cmd->SetGraphicsRootDescriptorTable(12, ctx.SrvHeap().Gpu(it->second.srv));
                    packEdge = pack->edge.Get();
                }
            }
            ID3D12PipelineState* curPso = nullptr;
            for (const GpuModel::Material& m : model->Materials()) {
                if (!m.drawEdge || m.indexCount == 0) continue;
                ID3D12PipelineState* want = m.packOff ? defaultEdge : packEdge;
                if (want != curPso) {
                    cmd->SetPipelineState(want);
                    curPso = want;
                }
                cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
                pc.stats.drawCalls++;
            }
        }
    }
    t.ptEdges = true;
}

// ==================================================================================================================
// M4b: MME object effects drawn inside ScenePass (docs/mme/mme_design.md §2.6, spec draft §3 / §4.2 / §4.4 / §5).
// ==================================================================================================================
namespace {

using namespace DirectX;

// `Subset` ("0-6,8"), in material index space. Empty / "*" = all subsets.
bool SubsetMatches(const std::string& raw, int material) {
    if (raw.empty() || raw == "*") return true;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t comma = raw.find(',', pos);
        if (comma == std::string::npos) comma = raw.size();
        std::string part = raw.substr(pos, comma - pos);
        pos = comma + 1;
        // trim
        while (!part.empty() && std::isspace((unsigned char)part.front())) part.erase(part.begin());
        while (!part.empty() && std::isspace((unsigned char)part.back())) part.pop_back();
        if (part.empty()) continue;
        const size_t dash = part.find('-');
        if (dash == std::string::npos) {
            if (std::atoi(part.c_str()) == material) return true;
        } else {
            const int lo = std::atoi(part.substr(0, dash).c_str());
            const int hi = dash + 1 >= part.size() ? INT32_MAX : std::atoi(part.c_str() + dash + 1);
            if (material >= lo && material <= hi) return true;
        }
    }
    return false;
}

bool BoolMatches(int tri, bool value) { return tri < 0 || (tri != 0) == value; }

// MME's technique selection (spec 3): first technique in file order whose MMDPass (object_ss falls back to object),
// Subset and use-flags all match. Returns null when nothing matches. When `exactPass` is set, the `object_ss` ->
// `object` fallback is disabled (used when a self-shadow map exists and MMD would pick the object_ss technique).
const MmeTechnique* SelectTechnique(const MmeEffectDesc& fx, const std::string& wantedPass, const GpuModel& model,
                                    size_t material, bool exactPass = false) {
    const GpuModel::Material& m = model.Materials()[material];
    const bool hasTexture = (model.MaterialConstantsCpu()[material].flags & MatFlag_HasTexture) != 0;
    const bool sphereMode = m.sphereMode != 0;
    const bool useToon = true;   // PMX / PMD models use toon rendering
    for (const MmeTechnique& t : fx.techniques) {
        const bool passOk = t.mmdPass == wantedPass || (!exactPass && wantedPass == "object_ss" && t.mmdPass == "object");
        if (!passOk) continue;
        if (!BoolMatches(t.useTexture, hasTexture)) continue;
        if (!BoolMatches(t.useSphereMap, sphereMode)) continue;
        if (!BoolMatches(t.useToon, useToon)) continue;
        if (!SubsetMatches(t.subset, (int)material)) continue;
        return &t;
    }
    return nullptr;
}

// M5c fix: object techniques whose script switches RenderColorTarget / RenderDepthStencilTarget. The VM drives the
// technique; the host draws each pass with the file's plain (non-object) instance into the currently bound effect
// target, or through the object-mode instance's host MRT when the script binds the default target. Mirrors
// DrawMmeModel's host-MRT per-material body and PassMmeOffscreen's DrawMmeObjectMaterials (plain instance, object
// input layout). See docs/mme/mme_design.md 2.6.
struct MmeObjectScriptCtx {
    PassContext* pc = nullptr;
    GpuModel* model = nullptr;
    size_t material = 0;
    MmeEffectInstance* objInst = nullptr;    // object mode: the default-target host-MRT draw
    MmeEffectInstance* plainInst = nullptr;  // non-object mode: effect-target / Draw=Buffer draws
    MmeFrameValues frame;
    DXGI_FORMAT sceneRts[3] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT sceneDs = DXGI_FORMAT_UNKNOWN;
    uint32_t sceneRtCount = 0;
    uint32_t sceneSamples = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtvs[3] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    D3D12_VERTEX_BUFFER_VIEW vbs[3] = {};
    std::map<uint64_t, std::unique_ptr<MmeConstantWriter>>* writers = nullptr;
};

class MmeObjectScriptHost final : public IMmeHost {
public:
    explicit MmeObjectScriptHost(MmeObjectScriptCtx& c) : c_(c) {
        FillMmeObjectValues(*c_.model, c_.material, ov_);
    }

    uint32_t Draws() const { return draws_; }

    void SetColorTarget(int index, const std::string& name) override {
        if (index < 0 || index > 3) return;
        colorBound_[index] = true;
        if (name.empty()) { color_[index] = nullptr; return; }
        MmeTarget* t = c_.plainInst ? c_.plainInst->FindTarget(name) : nullptr;
        if (!t || !t->tex) { Diag("RenderColorTarget '" + name + "' not found"); color_[index] = nullptr; return; }
        color_[index] = &t->tex;
    }

    void SetDepthTarget(const std::string& name) override {
        depthBound_ = true;
        if (name.empty()) { depth_ = nullptr; return; }
        MmeTarget* t = c_.plainInst ? c_.plainInst->FindTarget(name) : nullptr;
        if (!t || !t->tex) { Diag("RenderDepthStencilTarget '" + name + "' not found"); depth_ = nullptr; return; }
        depth_ = &t->tex;
        depthDsvFormat_ = t->dsvFormat;
    }

    void Clear(bool color, bool depth, const float color4[4], float depthValue) override {
        ID3D12GraphicsCommandList* cmd = c_.pc->cmd;
        if (color) {
            for (int i = 0; i < 4; ++i) {
                Texture* t = color_[i];
                if (!t || !t->res || t->rtv == DescriptorHeap::kInvalid) continue;
                t->Transition(cmd, kRt);
                cmd->ClearRenderTargetView(c_.pc->ctx.RtvHeap().Cpu(t->rtv), color4, 0, nullptr);
            }
        }
        if (depth && depth_ && depth_->res && depth_->dsv != DescriptorHeap::kInvalid) {
            depth_->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ClearDepthStencilView(c_.pc->ctx.DsvHeap().Cpu(depth_->dsv), D3D12_CLEAR_FLAG_DEPTH, depthValue, 0, 0,
                                       nullptr);
        }
    }

    void ScriptExternal() override { Diag("ScriptExternal=Color is not supported in an object technique, skipped"); }

    void DrawBuffer(const MmeTechnique& t, const MmePass& p) override { DrawPass(t, p, /*geometry=*/false); }
    void DrawGeometry(const MmeTechnique& t, const MmePass& p) override { DrawPass(t, p, /*geometry=*/true); }

    bool ReadParam(const std::string& name, float out[4]) override {
        const MmeEffectDesc& fx = c_.plainInst ? c_.plainInst->Desc() : c_.objInst->Desc();
        return MmeConstantWriter::ReadParam(fx, made_, name, out);
    }

    void SetLoopIndex(const std::string& name, int index) override { loopOverrides_[name] = (float)index; }

    void Diag(const std::string& message) override {
        static std::set<std::string> logged;
        const std::string file = c_.plainInst ? PathToUtf8(c_.plainInst->File()) : std::string("(object)");
        if (logged.insert(file + "\n" + message).second) LOG_WARN("MME %s: %s", file.c_str(), message.c_str());
    }

    // Restores the scene targets / viewport the caller's native draws expect, and leaves every bound effect target a
    // shader resource again (the script's later passes / the scene read them).
    void Finish() {
        ID3D12GraphicsCommandList* cmd = c_.pc->cmd;
        for (int i = 0; i < 4; ++i)
            if (colorBound_[i] && color_[i] && color_[i]->res) color_[i]->Transition(cmd, kSrv);
        if (depth_ && depth_->res) depth_->Transition(cmd, kSrv);
        cmd->OMSetRenderTargets(c_.sceneRtCount, c_.sceneRtvs, FALSE, c_.sceneDsv.ptr ? &c_.sceneDsv : nullptr);
        cmd->RSSetViewports(1, &c_.viewport);
        cmd->RSSetScissorRects(1, &c_.scissor);
    }

private:
    // Binds the currently bound target and reports its formats. Returns true for an effect target (plain program),
    // false for the default target (the scene MRTs, the object-mode host-MRT draw).
    bool BindTargets(DXGI_FORMAT fmts[4], uint32_t& rtCount, DXGI_FORMAT& ds, uint32_t& samples, uint32_t& w,
                     uint32_t& h) {
        ID3D12GraphicsCommandList* cmd = c_.pc->cmd;
        const bool isDefault = !colorBound_[0] || color_[0] == nullptr;
        if (isDefault) {
            for (uint32_t i = 0; i < c_.sceneRtCount && i < 4; ++i) fmts[i] = c_.sceneRts[i];
            rtCount = c_.sceneRtCount;
            ds = c_.sceneDs;
            samples = c_.sceneSamples;
            w = (uint32_t)c_.viewport.Width;
            h = (uint32_t)c_.viewport.Height;
            cmd->OMSetRenderTargets(c_.sceneRtCount, c_.sceneRtvs, FALSE, c_.sceneDsv.ptr ? &c_.sceneDsv : nullptr);
            cmd->RSSetViewports(1, &c_.viewport);
            cmd->RSSetScissorRects(1, &c_.scissor);
            return false;
        }
        Texture* rts[4] = {};
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i) {
            if (!colorBound_[i] || !color_[i] || !color_[i]->res) continue;
            if (color_[i]->rtv == DescriptorHeap::kInvalid) continue;
            rts[n] = color_[i];
            fmts[n] = color_[i]->format;
            ++n;
        }
        if (!n) {   // every bound target failed: draw nothing rather than bind a null view
            rtCount = 0;
            return true;
        }
        for (uint32_t i = 0; i < n; ++i) rts[i]->Transition(cmd, kRt);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        for (uint32_t i = 0; i < n; ++i) rtvs[i] = c_.pc->ctx.RtvHeap().Cpu(rts[i]->rtv);
        const bool hasDepth = depth_ && depth_->res && depth_->dsv != DescriptorHeap::kInvalid;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (hasDepth) {
            depth_->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsv = c_.pc->ctx.DsvHeap().Cpu(depth_->dsv);
        }
        cmd->OMSetRenderTargets(n, rtvs, FALSE, hasDepth ? &dsv : nullptr);
        w = rts[0]->width;
        h = rts[0]->height;
        D3D12_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, (LONG)w, (LONG)h};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        rtCount = n;
        samples = 1;
        ds = hasDepth ? depthDsvFormat_ : DXGI_FORMAT_UNKNOWN;
        return true;
    }

    MmeConstantWriter* Writer(MmeEffectInstance* inst, int program, int stage, const MmeStage& st) {
        const uint64_t key = ((uint64_t)(uintptr_t)inst << 32) ^ ((uint64_t)program << 1) ^ (uint64_t)(stage ? 1 : 0);
        auto it = c_.writers->find(key);
        if (it != c_.writers->end()) {
            made_.push_back(it->second.get());
            return it->second.get();
        }
        auto w = std::make_unique<MmeConstantWriter>();
        std::vector<MmeDiag> diags;
        w->Bind(inst->Desc(), st, diags);
        for (const MmeDiag& d : diags) Diag(d.message);
        MmeConstantWriter* raw = w.get();
        c_.writers->emplace(key, std::move(w));
        made_.push_back(raw);
        return raw;
    }

    bool BindStages(MmeEffectInstance* inst, int program, const MmeProgram& prog) {
        for (int pixel = 0; pixel < 2; ++pixel) {
            const MmeStage& st = pixel ? prog.ps : prog.vs;
            MmeConstantWriter* w = Writer(inst, program, pixel, st);
            if (!w) continue;
            MmeFrameValues fv = c_.frame;
            fv.textureValue = [inst](const std::string& name, uint32_t& outCount) -> const float* {
                return inst->TexValueFor(name, outCount);
            };
            w->Write(fv);
            w->WriteObject(ov_);
            for (const auto& [name, value] : loopOverrides_) w->SetScalar(name, value);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = c_.pc->sceneConstants;
            if (!w->Bytes().empty()) {
                cbAddr = c_.pc->mme->AllocConstants(w->Bytes().data(), (uint32_t)w->Bytes().size());
                if (cbAddr == 0) return false;
            }
            const int base = pixel ? 3 : 0;
            c_.pc->cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
            const uint32_t materialSrvTable = c_.model->Materials()[c_.material].srvTable;
            const D3D12_GPU_DESCRIPTOR_HANDLE srv =
                inst->StageSrvTableObject(*c_.pc, program, pixel != 0, materialSrvTable, c_.model);
            if (srv.ptr) c_.pc->cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
            const D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel != 0);
            if (smp.ptr) c_.pc->cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        }
        return true;
    }

    void DrawPass(const MmeTechnique& t, const MmePass& p, bool geometry) {
        if (!c_.pc || !c_.model || !c_.plainInst) return;
        DXGI_FORMAT fmts[4] = {};
        uint32_t rtCount = 0, samples = 1, w = 0, h = 0;
        DXGI_FORMAT ds = DXGI_FORMAT_UNKNOWN;
        const bool effectTargets = BindTargets(fmts, rtCount, ds, samples, w, h);
        if (!rtCount) { Diag("no usable render target, pass skipped"); return; }
        // Effect targets draw with the plain (non-object) instance; the default target keeps the host MRT.
        MmeEffectInstance* inst = effectTargets ? c_.plainInst : c_.objInst;
        const int program = inst->FindProgram(t.name, p.name);
        if (program < 0) { Diag("pass '" + p.name + "' has no compiled program, skipped"); return; }
        const MmeProgram& prog = inst->Programs()[program];
        if (!prog.ok || !prog.ps.code) return;
        ID3D12GraphicsCommandList* cmd = c_.pc->cmd;
        if (geometry) {
            const GpuModel::Material& m = c_.model->Materials()[c_.material];
            if (m.indexCount == 0) return;
            std::vector<D3D12_INPUT_ELEMENT_DESC> layout = MmeObjectInputLayout(prog.vs);
            ID3D12PipelineState* pso = inst->Pso(c_.pc->ctx, program, p, fmts, rtCount, ds, samples, &layout);
            if (!pso) { Diag("PSO creation failed, skipped"); return; }
            cmd->SetPipelineState(pso);
            if (!BindStages(inst, program, prog)) return;
            cmd->IASetVertexBuffers(0, 3, c_.vbs);
            cmd->IASetIndexBuffer(&c_.model->IndexBufferView());
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
            c_.pc->stats.drawCalls++;
            c_.pc->stats.triangles += m.indexCount / 3;
        } else {
            ID3D12PipelineState* pso = inst->Pso(c_.pc->ctx, program, p, fmts, rtCount, ds, samples, nullptr);
            if (!pso) { Diag("PSO creation failed, skipped"); return; }
            cmd->SetPipelineState(pso);
            if (!BindStages(inst, program, prog)) return;
            const D3D12_VERTEX_BUFFER_VIEW& vb = c_.pc->mme->QuadVb(c_.pc->ctx, w, h);
            cmd->IASetVertexBuffers(0, 1, &vb);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            cmd->DrawInstanced(4, 1, 0, 0);
            c_.pc->stats.drawCalls++;
        }
        ++draws_;
    }

    MmeObjectScriptCtx& c_;
    MmeObjectValues ov_;
    Texture* color_[4] = {nullptr, nullptr, nullptr, nullptr};
    bool colorBound_[4] = {false, false, false, false};
    Texture* depth_ = nullptr;
    bool depthBound_ = false;
    DXGI_FORMAT depthDsvFormat_ = DXGI_FORMAT_D32_FLOAT;
    std::map<std::string, float> loopOverrides_;
    std::vector<MmeConstantWriter*> made_;
    uint32_t draws_ = 0;
};

// Runs one scripted object technique (its script switches RenderColorTarget to an effect target). Returns the number
// of draws issued. The file's plain instance is created on demand; it draws on a later frame while it loads.
uint32_t RunMmeObjectScriptTechnique(PassContext& pc, GpuModel& model, size_t mi, const MmeTechnique& tech,
                                     MmeEffectInstance* objInst, const MmeFrameValues& frame, uint32_t sceneSamples,
                                     std::map<uint64_t, std::unique_ptr<MmeConstantWriter>>& writers) {
    if (!pc.mme || !pc.mmeSkin || !objInst) return 0;
    // Only an object-class effect: a scene / post effect the caller assigned to a mesh material (a `.x` whose
    // same-stem `.fx` is a post effect) keeps the pre-M5c skip -- running its ScriptExternal-carrying script as an
    // object technique would paint a black fullscreen quad over the scene.
    if (objInst->Desc().kind() != "object") return 0;
    MmeEffectInstance* plain = pc.mme->EnsureInstance(objInst->File(), false);
    if (!plain || !plain->Ready() || plain->Failed()) return 0;
    if (pc.targets.colorMsaa.rtv == DescriptorHeap::kInvalid || pc.targets.normalMsaa.rtv == DescriptorHeap::kInvalid ||
        pc.targets.velocityMsaa.rtv == DescriptorHeap::kInvalid || pc.targets.depthMsaa.dsv == DescriptorHeap::kInvalid)
        return 0;
    pc.mmeSkin->Skin(pc, model);
    D3D12_VERTEX_BUFFER_VIEW vb0 = pc.mmeSkin->Vb(model);
    if (!vb0.BufferLocation) return 0;

    MmeObjectScriptCtx c;
    c.pc = &pc;
    c.model = &model;
    c.material = mi;
    c.objInst = objInst;
    c.plainInst = plain;
    c.frame = frame;
    c.sceneRts[0] = RenderTargets::kColorFormat;
    c.sceneRts[1] = RenderTargets::kNormalFormat;
    c.sceneRts[2] = RenderTargets::kVelocityFormat;
    c.sceneDs = DXGI_FORMAT_D32_FLOAT;
    c.sceneRtCount = 3;
    c.sceneSamples = sceneSamples ? sceneSamples : 1;
    c.sceneRtvs[0] = pc.ctx.RtvHeap().Cpu(pc.targets.colorMsaa.rtv);
    c.sceneRtvs[1] = pc.ctx.RtvHeap().Cpu(pc.targets.normalMsaa.rtv);
    c.sceneRtvs[2] = pc.ctx.RtvHeap().Cpu(pc.targets.velocityMsaa.rtv);
    c.sceneDsv = pc.ctx.DsvHeap().Cpu(pc.targets.depthMsaa.dsv);
    const float* r = pc.view.mainRect;
    const float px = std::floor(r[0] * (float)pc.targets.width);
    const float py = std::floor(r[1] * (float)pc.targets.height);
    const float pw = std::floor((r[0] + r[2]) * (float)pc.targets.width) - px;
    const float ph = std::floor((r[1] + r[3]) * (float)pc.targets.height) - py;
    c.viewport = D3D12_VIEWPORT{px, py, pw, ph, 0.0f, 1.0f};
    c.scissor = D3D12_RECT{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
    c.vbs[0] = vb0;
    c.vbs[1] = model.AddUvBufferView();
    c.vbs[2] = MmeZeroWhiteVb(pc.ctx);
    if (!c.vbs[2].BufferLocation) return 0;
    c.writers = &writers;

    MmeObjectScriptHost host(c);
    // "sceneorobject": an object-class effect that switches RenderColorTarget uses Draw=Buffer passes (FootmarkSnow),
    // which the VM rejects under class "object"; the target-switching technique is the hybrid case the class is for.
    RunMmeTechnique(plain->Desc(), tech, "sceneorobject", host);
    host.Finish();
    return host.Draws();
}

} // namespace

// M5a: shared with the offscreen / shadow passes (see Passes.h).
const MmeTechnique* SelectMmeTechnique(const MmeEffectDesc& fx, const std::string& wantedPass, const GpuModel& model,
                                       size_t material, bool exactPass) {
    return SelectTechnique(fx, wantedPass, model, material, exactPass);
}

// M5b: the shared per-material / per-frame helpers of the MME object draw, used by ScenePass's raster / PT draws,
// the offscreen pass and the offline GI MME layer (OfflineRenderer.cpp).
void FillMmeObjectValues(const GpuModel& model, size_t mi, MmeObjectValues& ov) {
    const GpuModel::Material& m = model.Materials()[mi];
    const MaterialConstants& mc = model.MaterialConstantsCpu()[mi];
    ov.diffuse = mc.diffuse;
    ov.ambient = {mc.diffuse.x, mc.diffuse.y, mc.diffuse.z, 1.0f};
    ov.specular = {mc.specular.x, mc.specular.y, mc.specular.z, 1.0f};
    ov.specularPower = mc.specularPower;
    ov.emissive = {mc.ambient.x, mc.ambient.y, mc.ambient.z, 1.0f};
    const XMFLOAT3& toon = model.MmeToonColor(mi);
    ov.toonColor = {toon.x, toon.y, toon.z, 1.0f};
    ov.edgeColor = mc.edgeColor;
    ov.texMul = mc.texMul;
    ov.texAdd = mc.texAdd;
    ov.sphereMul = mc.sphereMul;
    ov.sphereAdd = mc.sphereAdd;
    ov.useTexture = (mc.flags & MatFlag_HasTexture) != 0;
    ov.useSpheremap = m.sphereMode != 0;
    ov.useSubtexture = m.sphereMode == 3;
    ov.useToon = true;
    ov.spadd = m.sphereMode == 2;
    ov.parthf = false;
}

void FillMmeFrameValues(PassContext& pc, const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float viewportW,
                        float viewportH, MmeFrameValues& out) {
    out = MmeFrameValues{};
    const XMMATRIX viewM = XMLoadFloat4x4(&view);
    XMStoreFloat4x4(&out.view, viewM);
    XMStoreFloat4x4(&out.proj, XMLoadFloat4x4(&proj));
    out.cameraPos = pc.view.camera.eye;
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat3(&out.cameraDir,
                  XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invView)));
    XMStoreFloat3(&out.lightDir, XMVector3Normalize(XMLoadFloat3(&pc.view.light.direction)));
    out.lightColor = pc.view.light.color;
    std::vector<const GpuModel*> chars;
    chars.reserve(pc.view.models.size());
    for (const GpuModel* m : pc.view.models) chars.push_back(m);
    BuildLightCamera(out.lightDir, chars, pc.view.mmeObjects, out.lightView, out.lightProj);
    out.viewportW = viewportW;
    out.viewportH = viewportH;
    out.time = (float)pc.frame / 60.0f;
    out.elapsed = pc.frameTimeMs * 0.001f;
    // M8a: the scene clock (see FrameView). Not edit mode while playing / rendering: MME TIME is the frame time.
    out.sceneTime = pc.view.sceneTime;
    out.sceneElapsed = pc.view.sceneElapsed;
    if (!pc.view.mmeEditMode) {
        out.time = out.sceneTime;
        out.elapsed = out.sceneElapsed;
    }
    out.self.si = 0.1f;
    out.lookup = MmeSceneLookup(pc.view);
}

// M5b: one model's MME-owned materials into the bound MRTs, with `frame`'s camera. The per-material body is the M4b
// object draw ScenePass::DrawMmeModel ran inline (kept in sync there for the raster / PT layers).
bool DrawMmeModelToMrt(PassContext& pc, GpuModel& model, const MmeFrameValues& frame,
                       const XMFLOAT4X4& prevViewProjNoJitter, const DXGI_FORMAT* rtFormats, DXGI_FORMAT dsFormat,
                       uint32_t sampleCount, uint64_t generation, float shutter) {
    if (!pc.mme || !pc.mmeSkin) return false;
    if (!model.HasMmeEffects()) return false;

    pc.mmeSkin->Skin(pc, model, generation, shutter);
    D3D12_VERTEX_BUFFER_VIEW vb0 = pc.mmeSkin->Vb(model);
    if (!vb0.BufferLocation) return false;
    const D3D12_VERTEX_BUFFER_VIEW vbs[3] = {vb0, model.AddUvBufferView(), MmeZeroWhiteVb(pc.ctx)};
    if (!vbs[2].BufferLocation) return false;

    struct HostCb {
        XMFLOAT4X4 view;
        XMFLOAT4X4 viewProjNoJitter;
        XMFLOAT4X4 prevViewProjNoJitter;
        XMFLOAT4 params;
    };
    HostCb host;
    host.view = frame.view;
    XMStoreFloat4x4(&host.viewProjNoJitter, XMLoadFloat4x4(&frame.view) * XMLoadFloat4x4(&frame.proj));
    host.prevViewProjNoJitter = prevViewProjNoJitter;
    host.params = {pc.view.light.sunIntensity, 1.0f, 0.0f, 0.0f};
    const D3D12_GPU_VIRTUAL_ADDRESS hostAddr = pc.mme->AllocConstants(&host, sizeof(host));
    if (hostAddr == 0) return false;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());
    cmd->SetGraphicsRootConstantBufferView(6, hostAddr);

    bool drew = false;
    for (size_t mi = 0; mi < model.Materials().size(); ++mi) {
        const std::filesystem::path& file = model.MmeEffectOf(mi);
        if (file.empty()) continue;
        MmeEffectInstance* inst = pc.mme->Instance(file, true);
        if (!inst || !inst->Ready() || inst->Failed()) continue;
        const MmeEffectDesc& fx = inst->Desc();
        if (fx.techniques.empty()) continue;
        const GpuModel::Material& m = model.Materials()[mi];
        if (m.indexCount == 0 || !m.visible) continue;
        const bool haveShadow = pc.mme->HasObjectShadowMap();
        const MmeTechnique* t = haveShadow ? SelectTechnique(fx, "object_ss", model, mi, /*exactPass=*/true) : nullptr;
        if (!t) t = SelectTechnique(fx, "object", model, mi);
        if (!t || t->passes.empty()) continue;
        if (const MmeAnnotation* sc = FindAnnotation(t->annotations, "Script")) {
            if (sc->value.str.find("RenderColorTarget") != std::string::npos) continue;
        }
        const MmePass& pass = t->passes[0];
        const int program = inst->FindProgram(t->name, pass.name);
        if (program < 0) continue;
        const MmeProgram& prog = inst->Programs()[program];
        if (!prog.ok || !prog.ps.code || !prog.hostMrt) continue;
        MmeObjectValues ov;
        FillMmeObjectValues(model, mi, ov);
        // fix 4/5: advance this instance's animated textures and give the binder the TEXTUREVALUE provider.
        inst->UpdateAnimatedTextures(pc);
        MmeFrameValues frameV = frame;
        frameV.textureValue = [inst](const std::string& name, uint32_t& outCount) -> const float* {
            return inst->TexValueFor(name, outCount);
        };
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = MmeObjectInputLayout(prog.vs);
        ID3D12PipelineState* pso = inst->Pso(pc.ctx, program, pass, rtFormats, 3, dsFormat, sampleCount, &layout);
        if (!pso) continue;
        cmd->SetPipelineState(pso);
        bool ok = true;
        // $Globals writers: ScenePass caches them in mmeWriters_; a per-draw writer keeps this helper self-contained.
        for (int pixel = 0; pixel < 2 && ok; ++pixel) {
            const MmeStage& st = pixel ? prog.ps : prog.vs;
            MmeConstantWriter writer;
            std::vector<MmeDiag> diags;
            writer.Bind(fx, st, diags);
            writer.Write(frameV);
            writer.WriteObject(ov);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = pc.sceneConstants;
            if (!writer.Bytes().empty()) {
                cbAddr = pc.mme->AllocConstants(writer.Bytes().data(), (uint32_t)writer.Bytes().size());
                if (cbAddr == 0) ok = false;
            }
            if (!ok) break;
            const int base = pixel ? 3 : 0;
            cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv =
                inst->StageSrvTableObject(pc, program, pixel != 0, m.srvTable, &model);
            if (srv.ptr) cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
            const D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel != 0);
            if (smp.ptr) cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        }
        if (!ok) continue;
        cmd->IASetVertexBuffers(0, 3, vbs);
        cmd->IASetIndexBuffer(&model.IndexBufferView());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
        pc.stats.drawCalls++;
        pc.stats.triangles += m.indexCount / 3;
        drew = true;
    }
    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
    return drew;
}

// M5b fix 1: the MME-layer ownership of one material (the same technique selection / empty-technique rule the MME
// draw applies). A material whose technique is empty is "not drawn" by MMD either, so the depth pre-pass skips it
// too (its surface would otherwise write depth that nothing draws over).
bool MmeMaterialOwned(MmeRuntime* mme, const GpuModel& model, size_t material) {
    const std::filesystem::path& file = model.MmeEffectOf(material);
    if (!mme || file.empty()) return false;
    MmeEffectInstance* inst = mme->Instance(file, true);
    if (!inst || !inst->Ready() || inst->Failed()) return false;
    const MmeEffectDesc& fx = inst->Desc();
    if (fx.techniques.empty()) return false;
    const bool haveShadow = mme->HasObjectShadowMap();
    const MmeTechnique* t = haveShadow ? SelectTechnique(fx, "object_ss", model, material, /*exactPass=*/true)
                                       : nullptr;
    if (!t) t = SelectTechnique(fx, "object", model, material);
    return t != nullptr;   // null (no technique, or an empty technique skipped by the draw) -> not owned
}

// The zero/white vertex buffer (slot 2): four zeros then four ones, stride 16. TEXCOORD3/4 read offset 0 (zeros),
// COLOR0 reads offset 16 (white). Created once and reused; the MmeSkin host slots are slots 0 (skinned) / 1 (addUV).
D3D12_VERTEX_BUFFER_VIEW MmeZeroWhiteVb(Dx12Context& ctx) {
    static ComPtr<ID3D12Resource> res;
    static D3D12_VERTEX_BUFFER_VIEW view{};
    if (!res) {
        static const float kData[8] = {0, 0, 0, 0, 1, 1, 1, 1};
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(kData));
        if (SUCCEEDED(ctx.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                            IID_PPV_ARGS(&res)))) {
            res->SetName(L"mme.zeroWhite");
            void* mapped = nullptr;
            if (SUCCEEDED(res->Map(0, nullptr, &mapped))) {
                memcpy(mapped, kData, sizeof(kData));
                res->Unmap(0, nullptr);
                view.BufferLocation = res->GetGPUVirtualAddress();
                view.SizeInBytes = sizeof(kData);
                view.StrideInBytes = 16;
            }
        }
    }
    return view;
}

void ScenePass::DrawMmeModel(PassContext& pc, GpuModel& model, bool rt, std::vector<uint8_t>& handled) {
    // Runs the object-effect draws for one model. Uses the runtime's root signature + tables; the caller rebinds
    // ScenePass's root signature + descriptor heap afterwards. Returns the per-material ownership in `handled`.
    handled.assign(model.Materials().size(), 0);    if (!pc.mme || !pc.mmeSkin) return;
    if (!model.HasMmeEffects()) return;

    // M5c: an accessory `.x` (an MME effect object's mesh) is model space with the controller panel as its world;
    // its materials are drawn only by the effect's object technique (no object technique = invisible, as in MMD).
    const MmeEffectObject* accessory = nullptr;
    for (const MmeEffectObject& o : pc.view.mmeEffectObjects)
        if (o.mesh == &model) { accessory = &o; break; }
    if (accessory) handled.assign(model.Materials().size(), 2);

    const bool isPmx = true;   // MME object effects run on PMX/PMD models (world-space vertices): WORLD = identity
    (void)isPmx;
    const SceneConstants& sc = *pc.sceneConstantsCpu;
    const XMMATRIX viewM = XMLoadFloat4x4(&sc.view);
    const XMMATRIX projM = XMLoadFloat4x4(&sc.proj);          // jittered (TAA): the surfaces line up with the scene
    const CameraParams& cam = pc.view.camera;

    // Frame values: WORLD = identity for PMX models, so WORLDVIEWPROJECTION = identity * view * proj and the MME
    // surfaces land on the scene's jittered projection.
    MmeFrameValues frame{};
    XMStoreFloat4x4(&frame.view, viewM);
    XMStoreFloat4x4(&frame.proj, projM);
    frame.cameraPos = cam.eye;
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat3(&frame.cameraDir, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invView)));
    XMStoreFloat3(&frame.lightDir, XMVector3Normalize(XMLoadFloat3(&pc.view.light.direction)));
    frame.lightColor = pc.view.light.color;
    // M5a fix 2: one MMD self-shadow light camera per frame, shared with the shadow map pass.
    {
        std::vector<const GpuModel*> chars;
        chars.reserve(pc.view.models.size());
        for (const GpuModel* m : pc.view.models) chars.push_back(m);
        BuildLightCamera(frame.lightDir, chars, pc.view.mmeObjects, frame.lightView, frame.lightProj);
    }
    frame.viewportW = (float)pc.targets.width;
    frame.viewportH = (float)pc.targets.height;
    frame.lookup = MmeSceneLookup(pc.view);
    frame.time = (float)pc.frame / 60.0f;
    frame.elapsed = pc.frameTimeMs * 0.001f;
    // M8a: the scene clock (see FrameView); MME TIME is the frame time while playing / rendering.
    frame.sceneTime = pc.view.sceneTime;
    frame.sceneElapsed = pc.view.sceneElapsed;
    if (!pc.view.mmeEditMode) {
        frame.time = frame.sceneTime;
        frame.elapsed = frame.sceneElapsed;
    }
    // The binder's WORLD comes from the accessory panel; PMX object draws are world-space, so make
    // AccessoryWorld (scale = si*10, no offset / rotation) the identity.
    frame.self.si = 0.1f;
    // M5c: an accessory `.x` keeps model-space vertices (its skin matrices are identity), so the effect places it
    // through `CONTROLOBJECT (self)` / its WORLD: hand the binder the controller panel as `self`.
    if (accessory) frame.self = accessory->panel;

    // Host cbuffer: view, viewProjNoJitter, prevViewProjNoJitter, params (x sun intensity, y = 1).
    struct HostCb {
        XMFLOAT4X4 view;
        XMFLOAT4X4 viewProjNoJitter;
        XMFLOAT4X4 prevViewProjNoJitter;
        XMFLOAT4 params;
    };
    HostCb host;
    XMStoreFloat4x4(&host.view, viewM);
    host.viewProjNoJitter = sc.viewProjNoJitter;
    host.prevViewProjNoJitter = sc.prevViewProjNoJitter;
    host.params = {pc.view.light.sunIntensity, 1.0f, 0.0f, 0.0f};
    const D3D12_GPU_VIRTUAL_ADDRESS hostAddr = pc.mme->AllocConstants(&host, sizeof(host));

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());
    cmd->SetGraphicsRootConstantBufferView(6, hostAddr);
    // CPU-style skinning into the world-space MME vertex buffer (idempotent within the frame).
    pc.mmeSkin->Skin(pc, model);

    const DXGI_FORMAT rtFormats[3] = {RenderTargets::kColorFormat, RenderTargets::kNormalFormat,
                                      RenderTargets::kVelocityFormat};
    const DXGI_FORMAT dsFormat = DXGI_FORMAT_D32_FLOAT;
    const uint32_t msaa = pc.targets.msaa ? pc.targets.msaa : 1;

    std::set<std::string> used;
    std::set<MmeEffectInstance*> logged;
    size_t owned = 0, drawn = 0;
    for (size_t mi = 0; mi < model.Materials().size(); ++mi) {
        const std::filesystem::path& file = model.MmeEffectOf(mi);
        if (file.empty()) continue;
        ++owned;
        const GpuModel::Material& m = model.Materials()[mi];
        if (m.indexCount == 0) continue;
        // M5c: an accessory `.x` material's own alpha is not a visibility gate -- the effect owns the mesh's look
        // (particle fields ship alpha-0 materials and the effect's technique shades them).
        if (!accessory && !m.visible) continue;
        MmeEffectInstance* inst = pc.mme->Instance(file, true);
        if (!inst || !inst->Ready() || inst->Failed()) continue;
        const MmeEffectDesc& fx = inst->Desc();
        if (fx.techniques.empty()) continue;

        // object_ss only when a self-shadow map is available (spec 3.4); else object. The shadow map is bound at
        // register(s0) by the runtime's SRV table when an object_ss draw needs it.
        const bool haveShadow = pc.mme->HasObjectShadowMap();
        const MmeTechnique* t = haveShadow ? SelectTechnique(fx, "object_ss", model, mi, /*exactPass=*/true) : nullptr;
        if (!t) t = SelectTechnique(fx, "object", model, mi);
        if (!t) continue;
        if (t->passes.empty()) { handled[mi] = 2; continue; }   // empty technique: not drawn
        // M5c fix: an object-class technique whose script switches RenderColorTarget wants an effect target, not the
        // scene's G-buffer. Run it through the script VM with a host that draws each pass into the currently bound
        // target with the file's plain instance (default-target passes keep the object-mode host-MRT draw below).
        // A scene / post-class effect the caller assigned to a mesh material is not an object technique: it keeps the
        // pre-M5c skip (running its ScriptExternal-carrying script as an object one would paint the whole frame).
        if (const MmeAnnotation* sc = FindAnnotation(t->annotations, "Script")) {
            if (sc->value.str.find("RenderColorTarget") != std::string::npos) {
                if (inst->Desc().kind() != "object") {
                    // an effect object's controller `.x` of a post effect is invisible in MMD (expected: no warning)
                    if (model.MmeEffectMesh()) {
                        handled[mi] = 2;
                        continue;
                    }
                    if (mmeDiag_.insert("objtarget:" + mmetext::PathKey(file)).second)
                        LOG_WARN("MME %s: object technique '%s' switches RenderColorTarget; pass skipped (M5)",
                                 PathToUtf8(file).c_str(), t->name.c_str());
                    continue;
                }
                const uint32_t n = RunMmeObjectScriptTechnique(pc, model, mi, *t, inst, frame, msaa, mmeWriters_);
                if (n) {
                    handled[mi] = 1;
                    ++drawn;
                    used.insert(t->name);
                    logged.insert(inst);
                } else {
                    handled[mi] = 2;   // nothing drawn: hide the material rather than draw it natively
                }
                continue;
            }
        }
        const MmePass& pass = t->passes[0];
        const int program = inst->FindProgram(t->name, pass.name);
        if (program < 0) continue;
        const MmeProgram& prog = inst->Programs()[program];
        if (!prog.ok || !prog.ps.code) continue;
        if (!prog.hostMrt) continue;   // PS writes MRT / DEPTH itself: not host-drawable, draw natively

        // per-material binder values (spec §4.2 MMD quirk, as nanoem's Effect::setMaterialParameters)
        MmeObjectValues ov;
        FillMmeObjectValues(model, mi, ov);
        // fix 4/5: advance this instance's animated textures and give the binder the TEXTUREVALUE provider.
        inst->UpdateAnimatedTextures(pc);
        MmeFrameValues frameV = frame;
        frameV.textureValue = [inst](const std::string& name, uint32_t& outCount) -> const float* {
            return inst->TexValueFor(name, outCount);
        };

        // PS input layout from the VS output signature (object draw uses the model's own IB).
        const MmeStage& vs = prog.vs;
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = MmeObjectInputLayout(vs);
        ID3D12PipelineState* pso = inst->Pso(pc.ctx, program, pass, rtFormats, 3, dsFormat, msaa, &layout);
        if (!pso) continue;

        // The host VBs: slot 0 skinned (MmeSkin), slot 1 add-UV view, slot 2 zero/white.
        D3D12_VERTEX_BUFFER_VIEW vb0 = pc.mmeSkin->Vb(model);
        if (!vb0.BufferLocation) continue;   // skinning unavailable
        const D3D12_VERTEX_BUFFER_VIEW vbs[3] = {vb0, model.AddUvBufferView(), MmeZeroWhiteVb(pc.ctx)};
        if (!vbs[2].BufferLocation) continue;

        cmd->SetPipelineState(pso);
        // stage constants (VS / PS $Globals) + SRV / sampler tables
        for (int pixel = 0; pixel < 2; ++pixel) {
            const MmeStage& st = pixel ? prog.ps : prog.vs;
            const uint64_t key = ((uint64_t)(uintptr_t)inst << 32) ^ ((uint64_t)program << 1) ^ (uint64_t)pixel;
            auto it = mmeWriters_.find(key);
            if (it == mmeWriters_.end()) {
                auto w = std::make_unique<MmeConstantWriter>();
                std::vector<MmeDiag> diags;
                w->Bind(fx, st, diags);
                for (const MmeDiag& d : diags)
                    LOG_WARN("MME %s: %s", PathToUtf8(inst->File()).c_str(), d.message.c_str());
                it = mmeWriters_.emplace(key, std::move(w)).first;
            }
            MmeConstantWriter* writer = it->second.get();
            writer->Write(frameV);
            writer->WriteObject(ov);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = pc.sceneConstants;
            if (!writer->Bytes().empty()) {
                cbAddr = pc.mme->AllocConstants(writer->Bytes().data(), (uint32_t)writer->Bytes().size());
                if (cbAddr == 0) break;
            }
            const int base = pixel ? 3 : 0;
            cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv = inst->StageSrvTableObject(pc, program, pixel != 0, m.srvTable, &model);
            if (srv.ptr) cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
            const D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel != 0);
            if (smp.ptr) cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        }
        cmd->IASetVertexBuffers(0, 3, vbs);
        cmd->IASetIndexBuffer(&model.IndexBufferView());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
        pc.stats.drawCalls++;
        pc.stats.triangles += m.indexCount / 3;
        handled[mi] = 1;
        ++drawn;
        used.insert(t->name);
        logged.insert(inst);
    }
    (void)rt;

    // M6a: one MME OBJECT line per (effect file, model): a model may use several per-material effects.
    for (MmeEffectInstance* inst : logged) {
        const uint64_t k = ((uint64_t)(uintptr_t)inst << 32) ^ (uint64_t)(uintptr_t)&model;
        auto& names = mmeUsed_[k];
        names.insert(used.begin(), used.end());
        if (mmeLogged_.insert(k).second) {
            std::string list;
            for (const std::string& n : names) list += (list.empty() ? "" : ",") + n;
            LOG_INFO("MME OBJECT %s model=%s materials=%zu/%zu techniques=%s", PathToUtf8(inst->File()).c_str(),
                     model.Name().c_str(), owned, model.Materials().size(), list.c_str());
        }
    }

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
}

bool ScenePass::DrawMmeEdges(PassContext& pc, GpuModel& model) {
    if (!pc.mme || !pc.mmeSkin) return false;
    if (!model.HasMmeEffects()) return false;

    const SceneConstants& sc = *pc.sceneConstantsCpu;
    const XMMATRIX viewM = XMLoadFloat4x4(&sc.view);
    const XMMATRIX projM = XMLoadFloat4x4(&sc.proj);
    const CameraParams& cam = pc.view.camera;
    MmeFrameValues frame{};
    XMStoreFloat4x4(&frame.view, viewM);
    XMStoreFloat4x4(&frame.proj, projM);
    frame.cameraPos = cam.eye;
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat3(&frame.cameraDir, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invView)));
    XMStoreFloat3(&frame.lightDir, XMVector3Normalize(XMLoadFloat3(&pc.view.light.direction)));
    frame.lightColor = pc.view.light.color;
    // M5a fix 2: one MMD self-shadow light camera per frame, shared with the shadow map pass.
    {
        std::vector<const GpuModel*> chars;
        chars.reserve(pc.view.models.size());
        for (const GpuModel* m : pc.view.models) chars.push_back(m);
        BuildLightCamera(frame.lightDir, chars, pc.view.mmeObjects, frame.lightView, frame.lightProj);
    }
    frame.viewportW = (float)pc.targets.width;
    frame.viewportH = (float)pc.targets.height;
    frame.time = (float)pc.frame / 60.0f;
    frame.elapsed = pc.frameTimeMs * 0.001f;
    // M8a: the scene clock (see FrameView); MME TIME is the frame time while playing / rendering.
    frame.sceneTime = pc.view.sceneTime;
    frame.sceneElapsed = pc.view.sceneElapsed;
    if (!pc.view.mmeEditMode) {
        frame.time = frame.sceneTime;
        frame.elapsed = frame.sceneElapsed;
    }

    struct HostCb { XMFLOAT4X4 view, viewProjNoJitter, prevViewProjNoJitter; XMFLOAT4 params; };
    HostCb host;
    XMStoreFloat4x4(&host.view, viewM);
    host.viewProjNoJitter = sc.viewProjNoJitter;
    host.prevViewProjNoJitter = sc.prevViewProjNoJitter;
    host.params = {pc.view.light.sunIntensity, 1.0f, 0.0f, 0.0f};
    const D3D12_GPU_VIRTUAL_ADDRESS hostAddr = pc.mme->AllocConstants(&host, sizeof(host));

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());
    cmd->SetGraphicsRootConstantBufferView(6, hostAddr);
    pc.mmeSkin->Skin(pc, model);

    const DXGI_FORMAT rtFormats[3] = {RenderTargets::kColorFormat, RenderTargets::kNormalFormat,
                                      RenderTargets::kVelocityFormat};
    const DXGI_FORMAT dsFormat = DXGI_FORMAT_D32_FLOAT;
    const uint32_t msaa = pc.targets.msaa ? pc.targets.msaa : 1;
    bool drew = false;
    for (size_t mi = 0; mi < model.Materials().size(); ++mi) {
        const std::filesystem::path& file = model.MmeEffectOf(mi);
        if (file.empty()) continue;
        const GpuModel::Material& m = model.Materials()[mi];
        if (!m.drawEdge || m.indexCount == 0) continue;
        MmeEffectInstance* inst = pc.mme->Instance(file, true);
        if (!inst || !inst->Ready() || inst->Failed()) continue;
        const MmeEffectDesc& fx = inst->Desc();
        // edge: per material, the edge technique of that material's effect.
        const MmeTechnique* edge = nullptr;
        for (const MmeTechnique& t : fx.techniques)
            if (t.mmdPass == "edge" && !t.passes.empty()) { edge = &t; break; }
        if (!edge) continue;
        const int program = inst->FindProgram(edge->name, edge->passes[0].name);
        if (program < 0) continue;
        const MmeProgram& prog = inst->Programs()[program];
        if (!prog.ok || !prog.ps.code || !prog.hostMrt) continue;
        // fix 4/5: animated textures + TEXTUREVALUE provider for the edge pass too.
        inst->UpdateAnimatedTextures(pc);
        MmeFrameValues frameV = frame;
        frameV.textureValue = [inst](const std::string& name, uint32_t& outCount) -> const float* {
            return inst->TexValueFor(name, outCount);
        };
        const MaterialConstants& mc = model.MaterialConstantsCpu()[mi];
        MmeObjectValues ov;
        ov.diffuse = mc.diffuse;
        ov.ambient = {mc.diffuse.x, mc.diffuse.y, mc.diffuse.z, 1.0f};
        ov.specular = {mc.specular.x, mc.specular.y, mc.specular.z, 1.0f};
        ov.specularPower = mc.specularPower;
        ov.emissive = {mc.ambient.x, mc.ambient.y, mc.ambient.z, 1.0f};
        const XMFLOAT3& toon = model.MmeToonColor(mi);
        ov.toonColor = {toon.x, toon.y, toon.z, 1.0f};
        ov.edgeColor = mc.edgeColor;
        ov.texMul = mc.texMul;
        ov.texAdd = mc.texAdd;
        ov.sphereMul = mc.sphereMul;
        ov.sphereAdd = mc.sphereAdd;
        ov.useTexture = (mc.flags & MatFlag_HasTexture) != 0;
        ov.useSpheremap = m.sphereMode != 0;
        ov.useSubtexture = m.sphereMode == 3;
        ov.useToon = true;
        ov.spadd = m.sphereMode == 2;

        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = MmeObjectInputLayout(prog.vs);
        ID3D12PipelineState* pso = inst->Pso(pc.ctx, program, edge->passes[0], rtFormats, 3, dsFormat, msaa, &layout);
        if (!pso) continue;
        cmd->SetPipelineState(pso);
        for (int pixel = 0; pixel < 2; ++pixel) {
            const MmeStage& st = pixel ? prog.ps : prog.vs;
            const uint64_t key = ((uint64_t)(uintptr_t)inst << 32) ^ ((uint64_t)program << 1) ^ (uint64_t)pixel;
            auto it = mmeWriters_.find(key);
            if (it == mmeWriters_.end()) {
                auto w = std::make_unique<MmeConstantWriter>();
                std::vector<MmeDiag> diags;
                w->Bind(fx, st, diags);
                it = mmeWriters_.emplace(key, std::move(w)).first;
            }
            MmeConstantWriter* writer = it->second.get();
            writer->Write(frameV);
            writer->WriteObject(ov);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = pc.sceneConstants;
            if (!writer->Bytes().empty()) {
                cbAddr = pc.mme->AllocConstants(writer->Bytes().data(), (uint32_t)writer->Bytes().size());
                if (cbAddr == 0) break;
            }
            const int base = pixel ? 3 : 0;
            cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv = inst->StageSrvTableObject(pc, program, pixel != 0, m.srvTable, &model);
            if (srv.ptr) cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
            const D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel != 0);
            if (smp.ptr) cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        }
        const D3D12_VERTEX_BUFFER_VIEW vbs[3] = {pc.mmeSkin->Vb(model), model.AddUvBufferView(),
                                                 MmeZeroWhiteVb(pc.ctx)};
        cmd->IASetVertexBuffers(0, 3, vbs);
        cmd->IASetIndexBuffer(&model.IndexBufferView());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
        pc.stats.drawCalls++;
        drew = true;
    }
    // Restore the ScenePass descriptor heap (the caller restores the root signature / tables). Mirrors
    // DrawMmeModel: without this the native edge loop below would run with the MME sampler heap bound.
    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
    return drew;
}

// The 4-MRT (object / class ID) variants of the lit PSOs, built the first time an inputs.id effect is enabled.
// A failure only disables the ID image (the frame keeps the 3-MRT PSOs); RT variants failing keep the raster ones.
bool ScenePass::EnsureIdPipelines(Dx12Context& ctx) {
    if (idTried_) return psoCullBackId_ != nullptr;
    idTried_ = true;
    ID3D12Device* device = ctx.Device();
    const std::filesystem::path file = shaderDir_ / L"mmd.hlsl";
    const ShaderDefines idDefines = {{"SCENE_HAS_ID", "1"}};
    ComPtr<ID3DBlob> psId = ctx.Caps().shaderModel >= D3D_SHADER_MODEL_6_0
                                ? CompileShaderDxc(file, "PSMain", "ps_6_0", idDefines)
                                : CompileShader(file, "PSMain", "ps_5_1", idDefines);
    if (!psId || !vs_) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dId = litDescId_;
    dId.VS = {vs_->GetBufferPointer(), vs_->GetBufferSize()};
    dId.PS = {psId->GetBufferPointer(), psId->GetBufferSize()};
    dId.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    bool ok = CheckHr(device->CreateGraphicsPipelineState(&dId, IID_PPV_ARGS(&psoCullBackId_)), "ScenePass: PSO back id");
    dId.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ok = ok && CheckHr(device->CreateGraphicsPipelineState(&dId, IID_PPV_ARGS(&psoNoCullId_)), "ScenePass: PSO no cull id");
    ok = ok && MakeBlendVariant(device, dId, false, psoNdBackId_, psoNdNoCullId_, "ScenePass: PSO no-depth-write id", true);
    ok = ok && MakeBlendVariant(device, dId, true, psoAddBackId_, psoAddNoCullId_, "ScenePass: PSO additive id", true);
    if (!ok) {
        for (ComPtr<ID3D12PipelineState>* p : {std::addressof(psoCullBackId_), std::addressof(psoNoCullId_), std::addressof(psoAddBackId_), std::addressof(psoAddNoCullId_),
                                               std::addressof(psoNdBackId_), std::addressof(psoNdNoCullId_)})
            p->Reset();
        return false;
    }
    if (vsRt_) {
        const ShaderDefines rtIdDefines = {{"RT_SHADOWS", "1"}, {"SCENE_HAS_ID", "1"}};
        ComPtr<ID3DBlob> psRtId = CompileShaderDxc(file, "PSMain", "ps_6_5", rtIdDefines);
        bool okRt = psRtId != nullptr;
        if (okRt) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDescId_;
            d.VS = {vsRt_->GetBufferPointer(), vsRt_->GetBufferSize()};
            d.PS = {psRtId->GetBufferPointer(), psRtId->GetBufferSize()};
            d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            okRt = CheckHr(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoCullBackRtId_)), "ScenePass: PSO back rt id");
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            okRt = okRt && CheckHr(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoNoCullRtId_)), "ScenePass: PSO no cull rt id");
            okRt = okRt && MakeBlendVariant(device, d, false, psoNdBackRtId_, psoNdNoCullRtId_, "ScenePass: PSO no-depth-write rt id", true);
            okRt = okRt && MakeBlendVariant(device, d, true, psoAddBackRtId_, psoAddNoCullRtId_, "ScenePass: PSO additive rt id", true);
        }
        if (!okRt)
            for (ComPtr<ID3D12PipelineState>* p : {std::addressof(psoCullBackRtId_), std::addressof(psoNoCullRtId_), std::addressof(psoAddBackRtId_),
                                                   std::addressof(psoAddNoCullRtId_), std::addressof(psoNdBackRtId_), std::addressof(psoNdNoCullRtId_)})
                p->Reset();
    }
    return true;
}

void ScenePass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    // The RR guides are created on the first RR frame (ScenePass::DrawGuides): a resize drops them.
    guideDiffuse_.Release(ctx);
    guideSpecular_.Release(ctx);
    guideNormal_.Release(ctx);
}

void ScenePass::ReleaseTargets(Dx12Context& ctx) {
    guideDiffuse_.Release(ctx);
    guideSpecular_.Release(ctx);
    guideNormal_.Release(ctx);
}

void ScenePass::DrawGuides(PassContext& pc) {
    if (!psoGuideBack_ || !psoGuideNoCull_) return;
    RenderTargets& t = pc.targets;
    if (t.msaa != 1) return;   // RR runs with the DLSS upscaler (single-sample targets) only
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    const D3D12_RESOURCE_FLAGS guideFlags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!guideDiffuse_ || guideDiffuse_.width != t.width || guideDiffuse_.height != t.height) {
        guideDiffuse_.Create(ctx, t.width, t.height, RenderTargets::kColorFormat, guideFlags, kSrv,
                             L"scene.guideDiffuse");
        guideSpecular_.Create(ctx, t.width, t.height, RenderTargets::kColorFormat, guideFlags, kSrv,
                              L"scene.guideSpecular");
        guideNormal_.Create(ctx, t.width, t.height, RenderTargets::kColorFormat, guideFlags, kSrv,
                            L"scene.guideNormal");
    }
    guideDiffuse_.Transition(cmd, kRt);
    guideSpecular_.Transition(cmd, kRt);
    guideNormal_.Transition(cmd, kRt);
    // The scene's own depth target (cleared again at the start of the scene draw right after).
    t.depthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {ctx.RtvHeap().Cpu(guideDiffuse_.rtv),
                                           ctx.RtvHeap().Cpu(guideSpecular_.rtv),
                                           ctx.RtvHeap().Cpu(guideNormal_.rtv)};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(t.depthMsaa.dsv);
    // Sky: RR's documented grey (the same default the PT guides use); no coverage beyond the drawn surfaces.
    const float clearDiffuse[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    const float clearZero[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(rtvs[0], clearDiffuse, 0, nullptr);
    cmd->ClearRenderTargetView(rtvs[1], clearZero, 0, nullptr);
    cmd->ClearRenderTargetView(rtvs[2], clearZero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

    const D3D12_GPU_DESCRIPTOR_HANDLE shadowTable = pc.transient.SrvTable(ctx, {&t.shadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE spotTable = pc.transient.SrvTable(ctx, {&t.spotShadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE pointTable = pc.transient.SrvTable(ctx, {&t.pointShadowMap});
    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetGraphicsRootDescriptorTable(5, shadowTable);
    cmd->SetGraphicsRootDescriptorTable(11, spotTable);
    cmd->SetGraphicsRootDescriptorTable(13, pointTable);
    cmd->SetGraphicsRootShaderResourceView(6, pc.lights);
    if (pc.rt) {
        cmd->SetGraphicsRootShaderResourceView(7, pc.rt->Tlas());
        cmd->SetGraphicsRootShaderResourceView(8, pc.rt->Geometries());
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE heapBase = ctx.SrvHeap().Gpu(0);
    cmd->SetGraphicsRootDescriptorTable(9, heapBase);
    cmd->SetGraphicsRootDescriptorTable(10, heapBase);
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float px = std::floor(pc.view.mainRect[0] * (float)t.width);
    const float py = std::floor(pc.view.mainRect[1] * (float)t.height);
    const float pw = std::floor((pc.view.mainRect[0] + pc.view.mainRect[2]) * (float)t.width) - px;
    const float ph = std::floor((pc.view.mainRect[1] + pc.view.mainRect[3]) * (float)t.height) - py;
    D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
    D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    const uint32_t pbrDebugChannel = (uint32_t)pc.settings.pbrDebug;
    for (GpuModel* model : pc.view.models) {
        if (!model) continue;
        BindModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
        ID3D12PipelineState* curPso = nullptr;
        for (const GpuModel::Material& m : model->Materials()) {
            if (m.indexCount == 0 || !m.visible) continue;
            ID3D12PipelineState* want = m.doubleSided ? psoGuideNoCull_.Get() : psoGuideBack_.Get();
            if (want != curPso) {
                cmd->SetPipelineState(want);
                curPso = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
            const size_t matIndex = (size_t)(&m - model->Materials().data());
            const bool pbrRecord = model->HasPbrRecords() && model->HasPbr() &&
                                   model->PbrIndex(matIndex) != DescriptorHeap::kInvalid;
            const uint32_t c[4] = {pbrRecord ? model->PbrSrv() : 0xFFFFFFFFu,
                                   pbrRecord ? model->PbrIndex(matIndex) : 0xFFFFFFFFu, pbrDebugChannel,
                                   pbrRecord ? 1u : 0u};
            cmd->SetGraphicsRoot32BitConstants(14, 4, c, 0);
            cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
            pc.stats.drawCalls++;
            pc.stats.triangles += m.indexCount / 3;
        }
    }
    guideDiffuse_.Transition(cmd, kSrv);
    guideSpecular_.Transition(cmd, kSrv);
    guideNormal_.Transition(cmd, kSrv);
    t.ptDiffuseAlbedo = &guideDiffuse_;
    t.ptSpecularAlbedo = &guideSpecular_;
    t.ptNormalRoughness = &guideNormal_;
}

// S5: a stage screen's camera feed. Raster, lit, default material shading only (no packs, MME, edges, outlines or ID
// image): the feed is what an LED wall shows, so it must be cheap and must not recurse into another feed. The models
// are drawn with the frame's own per-material constants (the screens already bound their textures) into `color` (HDR)
// plus the normal / velocity / depth targets, then tonemapped by the caller into the screen's RGBA8 texture.
bool ScenePass::EnsureFeedPipelines(Dx12Context& ctx) {
    if (feedTried_) return psoFeedBack_ != nullptr;
    feedTried_ = true;
    if (!litDesc_.VS.pShaderBytecode || !litDesc_.PS.pShaderBytecode) return false;    // The feed targets are single sample whatever the frame's MSAA is: derive a 1x desc from the lit one.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = litDesc_;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.RasterizerState.MultisampleEnable = FALSE;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!CheckHr(ctx.Device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoFeedBack_)), "ScenePass: PSO feed back"))
        return false;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(ctx.Device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psoFeedNoCull_)),
                 "ScenePass: PSO feed no cull"))
        return false;
    // the .emd blend hints keep their own state in the feed too
    d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!MakeBlendVariant(ctx.Device(), d, false, psoFeedNdBack_, psoFeedNdNoCull_, "ScenePass: PSO feed nd", false)) {
        psoFeedBack_.Reset();
        psoFeedNoCull_.Reset();
        return false;
    }
    d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    if (!MakeBlendVariant(ctx.Device(), d, true, psoFeedAddBack_, psoFeedAddNoCull_, "ScenePass: PSO feed add", false)) {
        psoFeedBack_.Reset();
        psoFeedNoCull_.Reset();
        psoFeedNdBack_.Reset();
        psoFeedNdNoCull_.Reset();
        return false;
    }
    return true;
}

bool ScenePass::DrawFeed(PassContext& pc, Texture& color, Texture& normal, Texture& velocity, Texture& depth) {
    if (!psoCullBack_ || !psoNoCull_ || pc.view.models.empty()) return false;
    if (!EnsureFeedPipelines(pc.ctx)) return false;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    const uint32_t w = color.width, h = color.height;
    const RenderTargets& t = pc.targets;

    color.Transition(cmd, kRt);
    normal.Transition(cmd, kRt);
    velocity.Transition(cmd, kRt);
    depth.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {ctx.RtvHeap().Cpu(color.rtv), ctx.RtvHeap().Cpu(normal.rtv),
                                           ctx.RtvHeap().Cpu(velocity.rtv)};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(depth.dsv);
    const float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) cmd->ClearRenderTargetView(rtvs[i], zero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)w, (LONG)h};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    // The sky (the scene's gradient) then the models; no outline layer (a feed is a lit image, not an editing view).
    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetGraphicsRootDescriptorTable(5, pc.transient.SrvTable(ctx, {&t.shadowMap}));
    cmd->SetGraphicsRootDescriptorTable(11, pc.transient.SrvTable(ctx, {&t.spotShadowMap}));
    cmd->SetGraphicsRootDescriptorTable(13, pc.transient.SrvTable(ctx, {&t.pointShadowMap}));
    cmd->SetGraphicsRootShaderResourceView(6, pc.lights);
    cmd->SetGraphicsRootDescriptorTable(9, ctx.SrvHeap().Gpu(0));
    cmd->SetGraphicsRootDescriptorTable(10, ctx.SrvHeap().Gpu(0));
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (psoSkyFeed_) {   // the single-sample sky (the feed / mirror targets are 1x whatever the frame's MSAA is)
        cmd->SetPipelineState(psoSkyFeed_.Get());
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    const uint32_t pbrDebugChannel = (uint32_t)pc.settings.pbrDebug;
    ID3D12PipelineState* curPso = nullptr;
    for (size_t mi = 0; mi < pc.view.models.size(); ++mi) {
        GpuModel* model = pc.view.models[mi];
        if (!model) continue;
        const uint32_t modelIdInfo = (((uint32_t)(mi + 1) & 0xFFu) << 8) | (model->Role() == ModelRole::Character ? 0x80u : 0u);
        GpuModel::CullFrustum frustum;
        const bool cull = pc.sceneConstantsCpu && model->BuildCullFrustum(pc.sceneConstantsCpu->viewProj, true, frustum);
        BindModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
        if (model->HasPackParamsExt())
            cmd->SetGraphicsRootConstantBufferView(15, model->PackParamsExtBuffer(pc.frame));
        for (const GpuModel::Material& m : model->Materials()) {
            if (m.indexCount == 0 || !m.visible) continue;   // MMD skips materials with alpha 0
            // The material's .emd blend hints keep their own PSOs (an additive surface must not become opaque).
            ID3D12PipelineState* want;
            if (m.additive) want = m.doubleSided ? psoFeedAddNoCull_.Get() : psoFeedAddBack_.Get();
            else if (m.noDepthWrite) want = m.doubleSided ? psoFeedNdNoCull_.Get() : psoFeedNdBack_.Get();
            else want = m.doubleSided ? psoFeedNoCull_.Get() : psoFeedBack_.Get();
            if (!want) want = m.doubleSided ? psoFeedNoCull_.Get() : psoFeedBack_.Get();
            if (want != curPso) {
                cmd->SetPipelineState(want);
                curPso = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
            const size_t matIndex = (size_t)(&m - model->Materials().data());
            const bool pbrRecord = model->HasPbrRecords() && model->HasPbr() &&
                                   model->PbrIndex(matIndex) != DescriptorHeap::kInvalid;
            const uint32_t c[5] = {pbrRecord ? model->PbrSrv() : 0xFFFFFFFFu,
                                   pbrRecord ? model->PbrIndex(matIndex) : 0xFFFFFFFFu, pbrDebugChannel,
                                   pbrRecord ? 1u : 0u, modelIdInfo};
            cmd->SetGraphicsRoot32BitConstants(14, 5, c, 0);
            pc.stats.drawCalls += model->DrawMaterial(cmd, m, cull ? &frustum : nullptr);
            pc.stats.triangles += m.indexCount / 3;
        }
    }
    color.Transition(cmd, kSrv);
    normal.Transition(cmd, kSrv);
    velocity.Transition(cmd, kSrv);
    depth.Transition(cmd, kSrv);
    return true;
}

void ScenePass::DrawDepthPrepass(PassContext& pc) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    for (GpuModel* model : pc.view.models) {
        // A surface pack or an MME effect owns the model's shading (it may discard): those models keep overdrawing.
        if (!model || !model->ShaderPackId().empty() || model->HasMmeEffects()) continue;
        GpuModel::CullFrustum frustum;
        const bool cull = pc.sceneConstantsCpu && model->BuildCullFrustum(pc.sceneConstantsCpu->viewProj, true, frustum);
        BindModelBuffers(cmd, *model, pc.frame);
        cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
        cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
        ID3D12PipelineState* bound = nullptr;
        for (const GpuModel::Material& m : model->Materials()) {
            if (!model->DepthPrepassMaterial(m)) continue;
            ID3D12PipelineState* want = m.doubleSided ? psoPrepassNoCull_.Get() : psoPrepassBack_.Get();
            if (want != bound) {
                cmd->SetPipelineState(want);
                bound = want;
            }
            cmd->SetGraphicsRootConstantBufferView(1, m.constants);
            pc.stats.drawCalls += model->DrawMaterial(cmd, m, cull ? &frustum : nullptr);
        }
    }
}

void ScenePass::Execute(PassContext& pc) {
    pc.targets.ptEdges = false;
    pc.targets.ptDiffuseAlbedo = pc.targets.ptSpecularAlbedo = pc.targets.ptNormalRoughness = nullptr;
    if (pc.path == RenderPath::PathTraced) {
        DrawPtEdges(pc);
        return;
    }
    // RayTraced + RR: the guides first (their own depth), then the scene as usual.
    if (pc.dlssRr && pc.path == RenderPath::RayTraced) DrawGuides(pc);
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& t = pc.targets;

    const bool hasId = t.idTarget != nullptr && EnsureIdPipelines(ctx);
    Texture* idTargetRes = hasId ? ((t.msaa > 1) ? &t.idMsaa : t.idTarget) : nullptr;

    t.colorMsaa.Transition(cmd, kRt);
    t.normalMsaa.Transition(cmd, kRt);
    t.velocityMsaa.Transition(cmd, kRt);
    t.depthMsaa.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (hasId) idTargetRes->Transition(cmd, kRt);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {ctx.RtvHeap().Cpu(t.colorMsaa.rtv), ctx.RtvHeap().Cpu(t.normalMsaa.rtv),
                                           ctx.RtvHeap().Cpu(t.velocityMsaa.rtv),
                                           hasId ? ctx.RtvHeap().Cpu(idTargetRes->rtv) : D3D12_CPU_DESCRIPTOR_HANDLE{}};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvHeap().Cpu(t.depthMsaa.dsv);
    const float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) cmd->ClearRenderTargetView(rtvs[i], zero, 0, nullptr);
    if (hasId) cmd->ClearRenderTargetView(rtvs[3], zero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(hasId ? 4 : 3, rtvs, FALSE, &dsv);

    const bool rt = pc.path == RenderPath::RayTraced && pc.rt && psoCullBackRt_;
    const bool wire = pc.path == RenderPath::Raster && pc.settings.shading == ViewShading::Wireframe;
    // The shadow-map SRV tables are allocated once; rebinding them (and the root signature) is what MME draws need
    // to undo when switching back.
    const D3D12_GPU_DESCRIPTOR_HANDLE shadowTable = pc.transient.SrvTable(ctx, {&t.shadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE spotTable = pc.transient.SrvTable(ctx, {&t.spotShadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE pointTable = pc.transient.SrvTable(ctx, {&t.pointShadowMap});
    const D3D12_GPU_DESCRIPTOR_HANDLE heapBase = ctx.SrvHeap().Gpu(0);
    const auto bindSceneRoot = [&]() {
        cmd->SetGraphicsRootSignature(rootSig_.Get());
        cmd->SetGraphicsRootDescriptorTable(5, shadowTable);
        cmd->SetGraphicsRootDescriptorTable(11, spotTable);
        cmd->SetGraphicsRootDescriptorTable(13, pointTable);
        cmd->SetGraphicsRootShaderResourceView(6, pc.lights);
        if (rt) {
            cmd->SetGraphicsRootShaderResourceView(7, pc.rt->Tlas());
            cmd->SetGraphicsRootShaderResourceView(8, pc.rt->Geometries());
        }
        cmd->SetGraphicsRootDescriptorTable(9, heapBase);
        cmd->SetGraphicsRootDescriptorTable(10, heapBase);
    };
    bindSceneRoot();
    // PBR record selection (b3) is per material; the debug channel is frame-wide (bound per draw).
    const uint32_t pbrDebugChannel = (uint32_t)pc.settings.pbrDebug;
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The camera view, plus (Studio quad view) the orthographic extra views: the same scene drawn again into other
    // rectangles of the target, each with its own SceneConstants. Extra views are flat and have a flat background.
    struct ViewDraw {
        float rect[4];
        D3D12_GPU_VIRTUAL_ADDRESS constants;
        bool ortho;
    };
    ViewDraw draws[1 + kMaxExtraViews];
    uint32_t drawCount = 0;
    draws[drawCount++] = {{pc.view.mainRect[0], pc.view.mainRect[1], pc.view.mainRect[2], pc.view.mainRect[3]}, pc.sceneConstants, false};
    const uint32_t extras = pc.path == RenderPath::Raster ? (uint32_t)std::min<size_t>(pc.view.extraViews.size(), kMaxExtraViews) : 0u;
    for (uint32_t e = 0; e < extras; ++e) {
        const ExtraView& ev = pc.view.extraViews[e];
        draws[drawCount++] = {{ev.rect[0], ev.rect[1], ev.rect[2], ev.rect[3]}, pc.extraSceneConstants[e], true};
    }

    for (uint32_t v = 0; v < drawCount; ++v) {
        const ViewDraw& vd = draws[v];
        const float px = std::floor(vd.rect[0] * (float)t.width), py = std::floor(vd.rect[1] * (float)t.height);
        const float pw = std::floor((vd.rect[0] + vd.rect[2]) * (float)t.width) - px;
        const float ph = std::floor((vd.rect[1] + vd.rect[3]) * (float)t.height) - py;
        D3D12_VIEWPORT viewport{px, py, pw, ph, 0.0f, 1.0f};
        D3D12_RECT scissor{(LONG)px, (LONG)py, (LONG)(px + pw), (LONG)(py + ph)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->SetGraphicsRootConstantBufferView(0, vd.constants);

        if (v == 0 && drawCount == 1 && pc.path == RenderPath::Raster && !pc.offscreen && !wire &&
            pc.settings.shading == ViewShading::Lit && psoPrepassBack_ && psoPrepassNoCull_) {
            cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            DrawDepthPrepass(pc);
            cmd->OMSetRenderTargets(hasId ? 4 : 3, rtvs, FALSE, &dsv);
        }

        if (!pc.settings.transparentBackground) {
            // Wireframe / ortho views: one flat colour instead of the sky gradient (which needs a perspective camera)
            cmd->SetPipelineState((wire || vd.ortho) ? psoFlatBg_.Get() : psoSky_.Get());
            cmd->DrawInstanced(3, 1, 0, 0);
        }
        // the procedural floor: lit / wire in the camera view; an ortho view shows it as lines only when wireframe
        // (a solid plane would hide everything in the top view). A transparent-background export keeps only the
        // dancer: the default floor is skipped (a stage model stays and is the user's to remove).
        if (pc.view.studioFloor && !pc.settings.exportAlpha && (!vd.ortho || wire)) {
            cmd->SetPipelineState(wire ? psoWireFloor_.Get() : (rt ? psoFloorRt_.Get() : psoFloor_.Get()));
            cmd->DrawInstanced(6, 1, 0, 0);
        }

        for (size_t mi = 0; mi < pc.view.models.size(); ++mi) {
            GpuModel* model = pc.view.models[mi];
            if (!model) continue;
            const uint32_t modelIdInfo = (((uint32_t)(mi + 1) & 0xFFu) << 8) | (model->Role() == ModelRole::Character ? 0x80u : 0u);
            GpuModel::CullFrustum frustum;
            const bool cull = !vd.ortho && pc.sceneConstantsCpu &&
                              model->BuildCullFrustum(pc.sceneConstantsCpu->viewProj, true, frustum);
            BindModelBuffers(cmd, *model, pc.frame);
            cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
            cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
            if (model->HasPackParamsExt())
                cmd->SetGraphicsRootConstantBufferView(15, model->PackParamsExtBuffer(pc.frame));

            // shader pack: lit camera view only (unlit / wireframe / the flat ortho views keep the default shading)
            const PackPipelines* pack = nullptr;
            if (!model->ShaderPackId().empty() && !wire && !vd.ortho && pc.settings.shading == ViewShading::Lit)
                pack = PackPsos(ctx, model->ShaderPackId(), model->ShaderTextureFolder(), rt, hasId);
            const bool packRt = pack && rt && pack->backRt;
            const bool packRtId = pack && rt && pack->backRtId;
            if (pack) {   // the model's texture set (pack id + resolved folder), material AND edge draws
                const auto it = pack->sets.find(PackSetKey(model->ShaderTextureFolder()));
                if (it != pack->sets.end() && it->second.srv != DescriptorHeap::kInvalid)   // pack_api.hlsli gPackTex
                    cmd->SetGraphicsRootDescriptorTable(12, ctx.SrvHeap().Gpu(it->second.srv));
            }
            ID3D12PipelineState* curPso = nullptr;
            // M4b: MME object effect for this model (lit camera view only). The effect draws the materials it owns;
            // `mmeHandled[i]` marks those so the native loop skips them. The MME draw binds its own root signature /
            // descriptor heaps, so ScenePass's bindings are restored right after.
            std::vector<uint8_t> mmeHandled;
            const bool mmeView = !wire && !vd.ortho && pc.settings.shading == ViewShading::Lit;
            if (mmeView && model->HasMmeEffects() && pc.mme && pc.mmeSkin) {
                DrawMmeModel(pc, *model, rt, mmeHandled);
                bindSceneRoot();
                // the root signature switch dropped every root argument: the view's scene constants too (stale ones
                // projected the remaining native materials through the MME constants: black wedges / frames)
                cmd->SetGraphicsRootConstantBufferView(0, vd.constants);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->RSSetViewports(1, &viewport);
                cmd->RSSetScissorRects(1, &scissor);
                if (pack) {
                    const auto pset = pack->sets.find(PackSetKey(model->ShaderTextureFolder()));
                    if (pset != pack->sets.end() && pset->second.srv != DescriptorHeap::kInvalid)
                        cmd->SetGraphicsRootDescriptorTable(12, ctx.SrvHeap().Gpu(pset->second.srv));
                }
                BindModelBuffers(cmd, *model, pc.frame);
                cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
                cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
                if (model->HasPackParamsExt())
                    cmd->SetGraphicsRootConstantBufferView(15, model->PackParamsExtBuffer(pc.frame));
            }
            for (const GpuModel::Material& m : model->Materials()) {
                if (m.indexCount == 0 || !m.visible) continue;   // MMD skips materials with alpha 0
                const size_t thisMat = (size_t)(&m - model->Materials().data());
                if (thisMat < mmeHandled.size() && mmeHandled[thisMat] != 0) continue;   // MME owns / hides this material
                ID3D12PipelineState* want;
                // Model .emd blend hints: an additive / no-depth-write material uses the matching lit PSO and the
                // default (no-pack) shading, so a shader pack does not own that draw.
                const bool blendHint = m.additive || m.noDepthWrite;
                const bool usePack = pack && !m.packOff && !blendHint;
                if (wire)
                    want = m.doubleSided ? psoWireNoCull_.Get() : psoWireBack_.Get();
                else if (m.additive)
                    want = m.doubleSided ? (hasId && psoAddNoCullId_ ? (rt && psoAddNoCullRtId_ ? psoAddNoCullRtId_.Get() : psoAddNoCullId_.Get()) : (rt && psoAddNoCullRt_ ? psoAddNoCullRt_.Get() : psoAddNoCull_.Get()))
                                         : (hasId && psoAddBackId_ ? (rt && psoAddBackRtId_ ? psoAddBackRtId_.Get() : psoAddBackId_.Get()) : (rt && psoAddBackRt_ ? psoAddBackRt_.Get() : psoAddBack_.Get()));
                else if (m.noDepthWrite)
                    want = m.doubleSided ? (hasId && psoNdNoCullId_ ? (rt && psoNdNoCullRtId_ ? psoNdNoCullRtId_.Get() : psoNdNoCullId_.Get()) : (rt && psoNdNoCullRt_ ? psoNdNoCullRt_.Get() : psoNdNoCull_.Get()))
                                         : (hasId && psoNdBackId_ ? (rt && psoNdBackRtId_ ? psoNdBackRtId_.Get() : psoNdBackId_.Get()) : (rt && psoNdBackRt_ ? psoNdBackRt_.Get() : psoNdBack_.Get()));
                else if (usePack)
                    want = m.doubleSided ? (hasId && (packRtId ? pack->noCullRtId : pack->noCullId) ? (packRtId ? pack->noCullRtId.Get() : pack->noCullId.Get()) : (packRt ? pack->noCullRt.Get() : pack->noCull.Get()))
                                         : (hasId && (packRtId ? pack->backRtId : pack->backId) ? (packRtId ? pack->backRtId.Get() : pack->backId.Get()) : (packRt ? pack->backRt.Get() : pack->back.Get()));
                else
                    want = m.doubleSided ? (hasId && psoNoCullId_ ? (rt && psoNoCullRtId_ ? psoNoCullRtId_.Get() : psoNoCullId_.Get()) : (rt ? psoNoCullRt_.Get() : psoNoCull_.Get()))
                                         : (hasId && psoCullBackId_ ? (rt && psoCullBackRtId_ ? psoCullBackRtId_.Get() : psoCullBackId_.Get()) : (rt ? psoCullBackRt_.Get() : psoCullBack_.Get()));
                if (want != curPso) {
                    cmd->SetPipelineState(want);
                    curPso = want;
                }
                cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                // Per-material PBR record (P5): a model loaded with Ray records renders PBR per material (no global
                // setting), unless a pack owns that draw. The --pbr-debug channel is passed along and wins inside
                // PSMain. Any other draw keeps gPbrActive 0, i.e. the default look.
                const size_t matIndex = (size_t)(&m - model->Materials().data());
                const bool pbrRecord = model->HasPbrRecords() && !usePack && model->HasPbr() &&
                                       model->PbrIndex(matIndex) != DescriptorHeap::kInvalid;
                if (pbrRecord) {
                    const uint32_t c[5] = {model->PbrSrv(), model->PbrIndex(matIndex), pbrDebugChannel, 1u, modelIdInfo};
                    cmd->SetGraphicsRoot32BitConstants(14, 5, c, 0);
                } else {
                    const uint32_t c[5] = {0xFFFFFFFFu, 0xFFFFFFFFu, pbrDebugChannel, 0u, modelIdInfo};
                    cmd->SetGraphicsRoot32BitConstants(14, 5, c, 0);
                }
                pc.stats.drawCalls += model->DrawMaterial(cmd, m, cull ? &frustum : nullptr);
                pc.stats.triangles += m.indexCount / 3;
            }

            if (pc.settings.drawEdges && !wire) {   // wireframe: the model is drawn as edges already
                // M4b: an `edge` technique in the model's MME object effect replaces the native edge pass for this model.
                if (mmeView && model->HasMmeEffects() && pc.mme && pc.mmeSkin) {
                    // DrawMmeEdges binds the MME root signature; restore ScenePass's state whether or not it drew an
                    // edge (it may bind and then find no edge technique for any material).
                    const bool drewEdge = DrawMmeEdges(pc, *model);
                    bindSceneRoot();
                    cmd->SetGraphicsRootConstantBufferView(0, vd.constants);
                    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cmd->RSSetViewports(1, &viewport);
                    cmd->RSSetScissorRects(1, &scissor);
                    BindModelBuffers(cmd, *model, pc.frame);
                    cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
                    cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));
                    if (model->HasPackParamsExt())
                        cmd->SetGraphicsRootConstantBufferView(15, model->PackParamsExtBuffer(pc.frame));
                    if (drewEdge) continue;   // skip the native edge loop for this model
                }
                ID3D12PipelineState* curEdgePso = nullptr;
                ID3D12PipelineState* defaultEdge = psoEdge_.Get();
                ID3D12PipelineState* packEdge = (pack && pack->edge) ? pack->edge.Get() : defaultEdge;
                for (const GpuModel::Material& m : model->Materials()) {
                    if (!m.drawEdge || m.indexCount == 0) continue;
                    ID3D12PipelineState* want = m.packOff ? defaultEdge : packEdge;
                    if (want != curEdgePso) {
                        cmd->SetPipelineState(want);
                        curEdgePso = want;
                    }
                    cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                    cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                    cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
                    pc.stats.drawCalls++;
                }
            }
        }
    }
    if (hasId) {
        idTargetRes->Transition(cmd, kSrv);
    }
}

// ---- ResolvePass ---------------------------------------------------------------------------

bool ResolvePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    bool ok = pipe_.Create(ctx, shaderDir / L"resolve.hlsl", "PSResolve",
                           {RenderTargets::kColorFormat, RenderTargets::kNormalFormat, RenderTargets::kVelocityFormat,
                            DXGI_FORMAT_R32_FLOAT},
                           {{"MSAA_SAMPLES", std::to_string(msaa > 1 ? msaa : 1)}});
    // The ID variant (inputs.id effects with MSAA) is compiled on first use in Execute.
    shaderDir_ = shaderDir;
    msaa_ = msaa;
    idTried_ = false;
    pipeId_ = FullscreenPipeline{};
    return ok;
}

void ResolvePass::Execute(PassContext& pc) {
    if (pc.path == RenderPath::PathTraced) return;
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    if (t.idTarget && t.msaa > 1 && !idTried_) {
        idTried_ = true;
        pipeId_.Create(pc.ctx, shaderDir_ / L"resolve.hlsl", "PSResolve",
                       {RenderTargets::kColorFormat, RenderTargets::kNormalFormat, RenderTargets::kVelocityFormat,
                        DXGI_FORMAT_R32_FLOAT, RenderTargets::kIdFormat},
                       {{"MSAA_SAMPLES", std::to_string(msaa_)}, {"RESOLVE_HAS_ID", "1"}});
    }
    const bool hasId = (t.idTarget != nullptr && t.msaa > 1 && pipeId_);
    t.colorMsaa.Transition(cmd, kSrv);
    t.normalMsaa.Transition(cmd, kSrv);
    t.velocityMsaa.Transition(cmd, kSrv);
    t.depthMsaa.Transition(cmd, kSrv);
    if (hasId) t.idMsaa.Transition(cmd, kSrv);
    t.color.Transition(cmd, kRt);
    t.normal.Transition(cmd, kRt);
    t.velocity.Transition(cmd, kRt);
    t.depth.Transition(cmd, kRt);
    if (hasId) t.idTarget->Transition(cmd, kRt);
    if (hasId) {
        auto table = pc.transient.SrvTable(pc.ctx, {&t.colorMsaa, &t.normalMsaa, &t.velocityMsaa, &t.depthMsaa, &t.idMsaa});
        pipeId_.Draw(pc, {&t.color, &t.normal, &t.velocity, &t.depth, t.idTarget}, table);
    } else {
        auto table = pc.transient.SrvTable(pc.ctx, {&t.colorMsaa, &t.normalMsaa, &t.velocityMsaa, &t.depthMsaa});
        pipe_.Draw(pc, {&t.color, &t.normal, &t.velocity, &t.depth}, table);
    }
    t.color.Transition(cmd, kSrv);
    t.normal.Transition(cmd, kSrv);
    t.velocity.Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    if (hasId) t.idTarget->Transition(cmd, kSrv);
}

// ---- SsaoPass -------------------------------------------------------------------------------

bool SsaoPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const bool ok = ao_.Create(ctx, shaderDir / L"ssao.hlsl", "PSAo", {DXGI_FORMAT_R8_UNORM}) &&
                    blur_.Create(ctx, shaderDir / L"ssao.hlsl", "PSBlur", {DXGI_FORMAT_R8_UNORM}) &&
                    blurRt_.Create(ctx, shaderDir / L"ssao.hlsl", "PSBlurRt", {DXGI_FORMAT_R8_UNORM}) &&
                    temporal_.Create(ctx, shaderDir / L"ssao.hlsl", "PSTemporal", {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT});
    if (!ok) return false;
    if (RtPipelinesSupported(ctx) && !rtao_.Create(ctx, shaderDir / L"rtao.hlsl", "CSRtao"))
        LOG_WARN("SSAO: ray-traced AO unavailable");  // never fails the pass
    return true;
}

void SsaoPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    const uint32_t w = Half(targets.width), h = Half(targets.height);
    const float one[4] = {1, 1, 1, 1};
    const D3D12_RESOURCE_FLAGS rtUav =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    raw_.Create(ctx, w, h, DXGI_FORMAT_R8_UNORM, rtUav, kSrv, L"ssao.raw", 1, 1, one);
    temp_.Create(ctx, w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"ssao.temp", 1, 1, one);
    out_.Create(ctx, w, h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"ssao.out", 1, 1, one);
    const float zero[4] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        hist_[i].Create(ctx, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv,
                        i ? L"ssao.hist1" : L"ssao.hist0", 1, 1, one);
        histZ_[i].Create(ctx, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv,
                         i ? L"ssao.histZ1" : L"ssao.histZ0", 1, 1, zero);
    }
    histValid_ = false;
}

void SsaoPass::ReleaseTargets(Dx12Context& ctx) {
    raw_.Release(ctx);
    temp_.Release(ctx);
    out_.Release(ctx);
    for (Texture& hist : hist_) hist.Release(ctx);
    for (Texture& hist : histZ_) hist.Release(ctx);
    histValid_ = false;
}

void SsaoPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.ao = nullptr;
    if (pc.path == RenderPath::PathTraced || !pc.settings.ssao || !out_) {
        histValid_ = false;
        return;
    }
    const bool rtAo = pc.path == RenderPath::RayTraced && pc.rt && rtao_;
    // Blur input: the raw raster AO, or the ray-traced AO after temporal accumulation.
    Texture* src = &raw_;
    if (rtAo) {
        raw_.Transition(cmd, kUav);
        t.depth.Transition(cmd, kSrvAll);
        t.normal.Transition(cmd, kSrvAll);
        const float c0[4] = {pc.settings.ssaoRadius * 2.5f, 1.0f / raw_.width, 1.0f / raw_.height, (float)std::clamp(pc.settings.rtaoRays, 1u, 64u)};
        rtao_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}),
                       pc.transient.UavTable(pc.ctx, {&raw_}), c0, 4, Groups(raw_.width), Groups(raw_.height));
        raw_.Transition(cmd, kSrv);
        t.depth.Transition(cmd, kSrv);
        t.normal.Transition(cmd, kSrv);

        if (pc.dlssRr) {
            // DLSS-RR denoises the raw per-frame AO: no temporal accumulation, no depth-aware blur.
            histValid_ = false;
            t.ao = &raw_;
            return;
        }

        // Reprojection needs last frame on screen: no offscreen renders (their history would depend on
        // whatever rendered before) and no camera cut or jump (the history is another view).
        const float dx = pc.view.camera.eye.x - histEye_.x, dy = pc.view.camera.eye.y - histEye_.y;
        const float dz = pc.view.camera.eye.z - histEye_.z;
        const bool histOk = histValid_ && !pc.offscreen && !pc.view.cameraCut && dx * dx + dy * dy + dz * dz <= 144.0f;
        histEye_ = pc.view.camera.eye;

        Texture& acc = hist_[histCur_];
        Texture& prev = hist_[histCur_ ^ 1];
        Texture& accZ = histZ_[histCur_];
        Texture& prevZ = histZ_[histCur_ ^ 1];
        const float tc[4] = {histOk ? 1.0f : 0.0f, pc.settings.rtaoHistoryMax, 0, 0};
        acc.Transition(cmd, kRt);
        accZ.Transition(cmd, kRt);
        temporal_.Draw(pc, {&acc, &accZ}, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, &raw_, &prev, &t.velocity, &prevZ}), tc, 4);
        acc.Transition(cmd, kSrv);
        accZ.Transition(cmd, kSrv);
        histCur_ ^= 1;
        histValid_ = true;
        src = &acc;
    } else {
        const float c0[4] = {pc.settings.ssaoRadius, 16.0f, 1.0f / t.width, 1.0f / t.height};
        raw_.Transition(cmd, kRt);
        ao_.Draw(pc, {&raw_}, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}), c0, 4);
        raw_.Transition(cmd, kSrv);
        histValid_ = false;
    }

    const float h[4] = {1.0f / raw_.width, 0, 0, 0};
    temp_.Transition(cmd, kRt);
    if (rtAo) blurRt_.Draw(pc, {&temp_}, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal, src, src}), h, 4);
    else blur_.Draw(pc, {&temp_}, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, src}), h, 4);
    temp_.Transition(cmd, kSrv);

    const float v[4] = {0, 1.0f / raw_.height, 0, 0};
    out_.Transition(cmd, kRt);
    if (rtAo) blurRt_.Draw(pc, {&out_}, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal, &temp_, src}), v, 4);
    else blur_.Draw(pc, {&out_}, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, &temp_}), v, 4);
    out_.Transition(cmd, kSrv);
    t.ao = &out_;
}

// ---- SsrPass --------------------------------------------------------------------------------

bool SsrPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!pipe_.Create(ctx, shaderDir / L"ssr.hlsl", "PSSsr", {DXGI_FORMAT_R16G16B16A16_FLOAT})) return false;
    if (RtPipelinesSupported(ctx) && !rt_.Create(ctx, shaderDir / L"rtreflect.hlsl", "CSReflect"))
        LOG_WARN("SSR: ray-traced reflections unavailable");  // never fails the pass
    if (RtPipelinesSupported(ctx) && !rtMirror_.Create(ctx, shaderDir / L"rtreflect.hlsl", "CSReflectMirror"))
        LOG_WARN("SSR: ray-traced mirror reflections unavailable");  // never fails the pass
    return true;
}

void SsrPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    out_.Create(ctx, Half(targets.width), Half(targets.height), DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv,
                L"ssr.out");
    mirrorOut_.Create(ctx, targets.width, targets.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv,
                      L"ssr.mirror");
}

void SsrPass::ReleaseTargets(Dx12Context& ctx) {
    out_.Release(ctx);
    mirrorOut_.Release(ctx);
}

void SsrPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.ssr = nullptr;
    t.ssrMirror = nullptr;
    if (pc.path == RenderPath::PathTraced || !pc.settings.ssr || !out_) return;
    if (pc.path == RenderPath::RayTraced && pc.rt && rt_) {
        out_.Transition(cmd, kUav);
        t.depth.Transition(cmd, kSrvAll);
        t.normal.Transition(cmd, kSrvAll);
        const float c0[4] = {1.0f / out_.width, 1.0f / out_.height, 1200.0f, 0};
        rt_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}),
                     pc.transient.UavTable(pc.ctx, {&out_}), c0, 4, Groups(out_.width), Groups(out_.height));
        out_.Transition(cmd, kSrv);
        if (rtMirror_ && mirrorOut_) {
            mirrorOut_.Transition(cmd, kUav);
            const float c0m[4] = {1.0f / mirrorOut_.width, 1.0f / mirrorOut_.height, 1200.0f, 0};
            rtMirror_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}),
                               pc.transient.UavTable(pc.ctx, {&mirrorOut_}), c0m, 4, Groups(mirrorOut_.width),
                               Groups(mirrorOut_.height));
            mirrorOut_.Transition(cmd, kSrv);
            t.ssrMirror = &mirrorOut_;
        }
        t.depth.Transition(cmd, kSrv);
        t.normal.Transition(cmd, kSrv);
    } else {
        const float c0[4] = {260.0f, 56.0f, 0, 0};
        out_.Transition(cmd, kRt);
        pipe_.Draw(pc, {&out_}, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal, &t.color}), c0, 4);
        out_.Transition(cmd, kSrv);
    }
    t.ssr = &out_;
}

// ---- PathTracePass ---------------------------------------------------------------------------

bool PathTracePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    if (!RtPipelinesSupported(ctx)) return true;
    shaderDir_ = shaderDir;
    const std::filesystem::path f = shaderDir / L"pathtrace.hlsl";
    const std::filesystem::path d = shaderDir / L"pt_denoise.hlsl";
    const bool ok = trace_.Create(ctx, f, "CSPathTrace") && temporal_.Create(ctx, d, "CSTemporal") &&
                    atrous_.Create(ctx, d, "CSAtrous") && modulate_.Create(ctx, d, "CSModulate");
    if (!ok) {
        trace_ = ComputePipeline{};
        temporal_ = ComputePipeline{};
        atrous_ = ComputePipeline{};
        modulate_ = ComputePipeline{};
        LOG_WARN("PathTrace: pipelines unavailable");
    }
    temporalAlbedo_ = ComputePipeline{};   // optional: offline video only, falls back to the per-pass albedo
    if (ok && !temporalAlbedo_.Create(ctx, d, "CSTemporalAlbedo")) LOG_WARN("PathTrace: albedo accumulation unavailable");
    // outlines (ScenePass::DrawPtEdges): optional, the image is drawn without them on failure
    edgeComposite_ = ComputePipeline{};
    if (ok && !edgeComposite_.Create(ctx, shaderDir / L"pt_edge.hlsl", "CSPtEdgeComposite",
                                     {{"MSAA_SAMPLES", std::to_string(msaa > 1 ? msaa : 1)}}))
        LOG_WARN("PathTrace: outline composite unavailable");
    ptVariants_.Clear(&ctx);
    return true;  // Execute checks the pipelines; never fails the pass
}

void PathTracePass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    const uint32_t w = targets.width, h = targets.height;
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    light_.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.light");
    albedo_.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.albedo");
    for (Texture& tex : history_) tex.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.history");
    for (Texture& tex : histDepth_)
        tex.Create(ctx, w, h, DXGI_FORMAT_R32_FLOAT, uav, kSrv, L"pt.histDepth");
    for (Texture& tex : histNormal_)
        tex.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.histNormal");
    filterA_.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.filterA");
    filterB_.Create(ctx, w, h, RenderTargets::kColorFormat, uav, kSrv, L"pt.filterB");
    for (Texture& tex : histAlbedo_) tex.Release(ctx);   // recreated by the next accumulating render
    rCur_.Release(ctx);
    albEff_.Release(ctx);
    edgeAcc_.Release(ctx);
    rrDiffuse_.Release(ctx);
    rrSpecular_.Release(ctx);
    rrNormal_.Release(ctx);
    historyValid_ = false;
    current_ = 0;
}

void PathTracePass::ReleaseTargets(Dx12Context& ctx) {
    light_.Release(ctx);
    albedo_.Release(ctx);
    for (Texture& tex : history_) tex.Release(ctx);
    for (Texture& tex : histDepth_) tex.Release(ctx);
    for (Texture& tex : histNormal_) tex.Release(ctx);
    for (Texture& tex : histAlbedo_) tex.Release(ctx);
    rCur_.Release(ctx);
    albEff_.Release(ctx);
    edgeAcc_.Release(ctx);
    filterA_.Release(ctx);
    filterB_.Release(ctx);
    rrDiffuse_.Release(ctx);
    rrSpecular_.Release(ctx);
    rrNormal_.Release(ctx);
    historyValid_ = false;
}

void PathTracePass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    if (pc.path != RenderPath::PathTraced || !pc.rt || !trace_ || !temporal_ || !atrous_ || !modulate_ || !light_)
        return;

    ComputePipeline* pipe = ptVariants_.Resolve(pc.ctx, shaderDir_, pc.view.models, trace_, "pathtrace.hlsl", "CSPathTrace", "pathtrace");
    if (!pipe || !*pipe) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t W = t.width, H = t.height;
    const uint32_t gx = Groups(W), gy = Groups(H);

    // 1. trace: noisy light + primary albedo; rewrites the G-buffer targets as UAVs.
    const bool rr = pc.dlssRr;
    if (rr && !rrDiffuse_) {   // DLSS-RR guides, created on the first RR frame
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        rrDiffuse_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.rrDiffuse");
        rrSpecular_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.rrSpecular");
        rrNormal_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.rrNormal");
    }
    light_.Transition(cmd, kUav);
    albedo_.Transition(cmd, kUav);
    t.normal.Transition(cmd, kUav);
    t.velocity.Transition(cmd, kUav);
    t.depth.Transition(cmd, kUav);
    if (rr) {
        rrDiffuse_.Transition(cmd, kUav);
        rrSpecular_.Transition(cmd, kUav);
        rrNormal_.Transition(cmd, kUav);
    }
    const bool hasId = (t.idTarget != nullptr);
    if (hasId) t.idTarget->Transition(cmd, kUav);
    const float c0[12] = {(float)std::clamp(pc.settings.ptSamples, 1u, 4u),
                          (float)std::clamp(pc.settings.ptBounces, 1u, 6u),
                          (pc.view.studioFloor && !pc.settings.exportAlpha) ? 1.0f : 0.0f,   // transparent export: no default floor
                          pc.settings.transparentBackground ? 1.0f : 0.0f,
                          (float)(pc.frame % 65536), 0, 0, 0,
                          rr ? 1.0f : 0.0f, 0, hasId ? 1.0f : 0.0f, 0};
    // The guide UAVs are unbound (null) when RR is off; the shader never writes them then.
    pipe->Dispatch(pc, {},
                   pc.transient.UavTable(pc.ctx, {&light_, &albedo_, &t.normal, &t.velocity, &t.depth,
                                                  rr ? &rrDiffuse_ : nullptr, rr ? &rrSpecular_ : nullptr,
                                                  rr ? &rrNormal_ : nullptr,
                                                  hasId ? t.idTarget : nullptr}),
                   c0, 12, gx, gy);
    if (hasId) t.idTarget->Transition(cmd, kSrv);

    if (rr) {
        // DLSS-RR replaces the temporal / a-trous denoiser: hand it the noisy radiance (light * albedo)
        // and the guide buffers, then let the upscale pass run RR.
        light_.Transition(cmd, kSrvAll);
        albedo_.Transition(cmd, kSrvAll);
        t.depth.Transition(cmd, kSrvAll);
        t.color.Transition(cmd, kUav);
        const float cr[4] = {pc.settings.transparentBackground ? 1.0f : 0.0f, 0, 0, 0};
        modulate_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&light_, &albedo_, &t.depth}),
                           pc.transient.UavTable(pc.ctx, {&t.color}), cr, 4, gx, gy);
        // outline layer over the noisy colour (its velocity where it covers), as the SR path does
        if (t.ptEdges && edgeComposite_) {
            D3D12_RESOURCE_BARRIER uavB = CD3DX12_RESOURCE_BARRIER::UAV(t.color.res.Get());
            cmd->ResourceBarrier(1, &uavB);
            t.colorMsaa.Transition(cmd, kSrvAll);
            t.velocityMsaa.Transition(cmd, kSrvAll);
            t.velocity.Transition(cmd, kUav);
            filterB_.Transition(cmd, kUav);   // dummy accumulation target
            const float ce[4] = {0.0f, 1.0f, 0, 0};
            edgeComposite_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.colorMsaa, &t.velocityMsaa}),
                                    pc.transient.UavTable(pc.ctx, {&t.color, &t.velocity, &filterB_}), ce, 4, gx, gy);
        }
        rrDiffuse_.Transition(cmd, kSrv);
        rrSpecular_.Transition(cmd, kSrv);
        rrNormal_.Transition(cmd, kSrv);
        t.color.Transition(cmd, kSrv);
        t.normal.Transition(cmd, kSrv);
        t.velocity.Transition(cmd, kSrv);
        t.depth.Transition(cmd, kSrv);
        t.ptDiffuseAlbedo = &rrDiffuse_;
        t.ptSpecularAlbedo = &rrSpecular_;
        t.ptNormalRoughness = &rrNormal_;
        historyValid_ = false;   // RR owns its own history; the PT accumulator restarts when it is switched off
        return;                  // RR replaces the temporal / a-trous denoiser
    }
    t.ptDiffuseAlbedo = t.ptSpecularAlbedo = t.ptNormalRoughness = nullptr;

    // 2. temporal accumulation into this frame's history slots.
    const uint32_t cur = current_, prev = current_ ^ 1;
    light_.Transition(cmd, kSrvAll);
    t.velocity.Transition(cmd, kSrvAll);
    t.depth.Transition(cmd, kSrvAll);
    t.normal.Transition(cmd, kSrvAll);
    for (Texture* tex : {&history_[prev], &histDepth_[prev], &histNormal_[prev]}) tex->Transition(cmd, kSrvAll);
    for (Texture* tex : {&history_[cur], &histDepth_[cur], &histNormal_[cur]}) tex->Transition(cmd, kUav);
    const bool accum = pc.settings.ptAccumAlbedo && temporalAlbedo_;
    if (accum && !rCur_) {   // offline video: radiance accumulation targets, created on first use
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (Texture& tex : histAlbedo_) tex.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.histRadiance");
        rCur_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.radiance");
        albEff_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.albedoEff");
        edgeAcc_.Create(pc.ctx, W, H, RenderTargets::kColorFormat, uav, kSrv, L"pt.edgeAcc");
    }
    Texture* radTarget = accum ? &rCur_ : &filterB_;   // filterB_ is a dummy binding when nothing accumulates
    albedo_.Transition(cmd, kSrvAll);
    radTarget->Transition(cmd, kUav);
    const bool valid = historyValid_ && !pc.view.cameraCut && !pc.offscreen;
    const float c1[8] = {valid ? 1.0f : 0.0f, pc.settings.ptHistoryMax, pc.settings.ptHistoryMinAlpha,
                         pc.settings.ptHistoryCarry, pc.view.ptFirstPass ? 1.0f : 0.0f,
                         pc.view.ptStaticPass ? 1.0f : 0.0f, 0, 0};
    temporal_.Dispatch(pc, pc.transient.SrvTable(pc.ctx,
                                                {&light_, &history_[prev], &t.velocity, &t.depth, &t.normal,
                                                 &histDepth_[prev], &histNormal_[prev], &albedo_}),
                       pc.transient.UavTable(pc.ctx, {&history_[cur], &histDepth_[cur], &histNormal_[cur], radTarget}),
                       c1, 8, gx, gy);

    // 2b. offline video: accumulate the radiance with the light's weights, effective albedo R_acc / L_acc
    Texture* albedoSrc = &albedo_;
    if (accum) {
        history_[cur].Transition(cmd, kSrvAll);
        rCur_.Transition(cmd, kSrvAll);
        histAlbedo_[prev].Transition(cmd, kSrvAll);
        histAlbedo_[cur].Transition(cmd, kUav);
        albEff_.Transition(cmd, kUav);
        temporalAlbedo_.Dispatch(pc, pc.transient.SrvTable(pc.ctx,
                                                          {&history_[cur], &rCur_, &histAlbedo_[prev], &t.velocity,
                                                           &t.depth, &t.normal, &histDepth_[prev], &histNormal_[prev]}),
                                 pc.transient.UavTable(pc.ctx, {&histAlbedo_[cur], &albEff_}), c1, 8, gx, gy);
        histAlbedo_[cur].Transition(cmd, kSrvAll);
        albEff_.Transition(cmd, kSrvAll);
        albedoSrc = &albEff_;
    }

    // 3. a-trous: history_[cur] -> filterA_ (step 1) -> filterB_ (step 2) -> filterA_ (step 4).
    Texture* src = &history_[cur];
    for (uint32_t i = 0; i < 3; ++i) {
        Texture* dst = i == 0 ? &filterA_ : (i == 1 ? &filterB_ : &filterA_);
        src->Transition(cmd, kSrvAll);
        dst->Transition(cmd, kUav);
        const float c[4] = {(float)(1 << i), 0.6f, 0, 0};
        atrous_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {src, &t.depth, &t.normal}),
                         pc.transient.UavTable(pc.ctx, {dst}), c, 4, gx, gy);
        src = dst;
    }

    // 4. modulate the filtered light by the albedo into the G-buffer colour.
    filterA_.Transition(cmd, kSrvAll);
    albedoSrc->Transition(cmd, kSrvAll);
    t.color.Transition(cmd, kUav);
    const float c3[4] = {pc.settings.transparentBackground ? 1.0f : 0.0f, 0, 0, 0};
    modulate_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&filterA_, albedoSrc, &t.depth}),
                       pc.transient.UavTable(pc.ctx, {&t.color}), c3, 4, gx, gy);

    // 4b. outlines: ScenePass's edge layer (DrawPtEdges) over the denoised colour, its velocity where it covers
    if (t.ptEdges && edgeComposite_) {
        D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(t.color.res.Get());
        cmd->ResourceBarrier(1, &uav);
        t.colorMsaa.Transition(cmd, kSrvAll);
        t.velocityMsaa.Transition(cmd, kSrvAll);
        t.velocity.Transition(cmd, kUav);
        Texture* edgeAcc = accum ? &edgeAcc_ : &filterB_;   // filterB_ is a dummy binding when nothing accumulates
        edgeAcc->Transition(cmd, kUav);
        const float ce[4] = {accum ? 1.0f : 0.0f, 1.0f / (float)(std::max(pc.view.ptPassIndex, 0) + 1), 0, 0};
        edgeComposite_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.colorMsaa, &t.velocityMsaa}),
                                pc.transient.UavTable(pc.ctx, {&t.color, &t.velocity, edgeAcc}), ce, 4, gx, gy);
    }

    // 5. hand the G-buffer to the pixel-shader passes.
    t.color.Transition(cmd, kSrv);
    t.normal.Transition(cmd, kSrv);
    t.velocity.Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    if (!pc.offscreen) {
        current_ ^= 1;
        historyValid_ = true;
    }
}

// ---- UpscalePass -----------------------------------------------------------------------------

void UpscalePass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    output_.Create(ctx, targets.outWidth, targets.outHeight, RenderTargets::kColorFormat,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv, L"upscale.output");
    reset_ = true;
}

void UpscalePass::ReleaseTargets(Dx12Context& ctx) { output_.Release(ctx); }

void UpscalePass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    if (!pc.upscaler || !output_ || pc.offscreen) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.lit.Transition(cmd, kSrvAll);
    t.depth.Transition(cmd, kSrvAll);
    t.velocity.Transition(cmd, kSrvAll);
    output_.Transition(cmd, kUav);
    UpscaleInputs in;
    in.cmd = cmd;
    in.color = t.lit.res.Get();   // the noisy PT radiance (composite only adds the distance haze)
    in.depth = t.depth.res.Get();
    in.velocity = t.velocity.res.Get();
    in.output = output_.res.Get();
    in.renderWidth = t.width;
    in.renderHeight = t.height;
    in.outputWidth = t.outWidth;
    in.outputHeight = t.outHeight;
    in.quality = pc.settings.upscalerQuality;
    in.jitterX = pc.jitterPxX;
    in.jitterY = pc.jitterPxY;
    in.reset = reset_ || pc.view.cameraCut;
    in.frameTimeMs = pc.frameTimeMs;
    in.nearZ = pc.view.camera.nearZ;
    in.farZ = pc.view.camera.farZ;
    in.fovY = pc.view.camera.fovYRadians;
    in.dlssModel = pc.settings.dlssModel;
    in.rayReconstruction = pc.dlssRr;
    if (pc.dlssRr) {
        in.diffuseAlbedo = t.ptDiffuseAlbedo ? t.ptDiffuseAlbedo->res.Get() : nullptr;
        in.specularAlbedo = t.ptSpecularAlbedo ? t.ptSpecularAlbedo->res.Get() : nullptr;
        in.roughness = t.ptNormalRoughness ? t.ptNormalRoughness->res.Get() : nullptr;
        in.normals = t.ptNormalRoughness ? t.ptNormalRoughness->res.Get() : nullptr;
    }
    const bool ok = pc.upscaler->Evaluate(pc.ctx, in);
    ID3D12DescriptorHeap* heaps[] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);  // SDKs change the heap
    reset_ = false;
    output_.Transition(cmd, kSrv);
    t.lit.Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    t.velocity.Transition(cmd, kSrv);
    if (ok) t.hdrFinal = &output_;
}

// ---- CompositePass ----------------------------------------------------------------------------

bool CompositePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    return pipe_.Create(ctx, shaderDir / L"composite.hlsl", "PSComposite", {RenderTargets::kColorFormat});
}

void CompositePass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    // Quad view: AO / SSR / distance haze unproject through the frame camera, so they only apply
    // inside the camera quadrant (gP1 = the rect in uv; gP1.z <= 0: the whole target).
    const bool quad = !pc.view.extraViews.empty();
    const float c0[12] = {t.ao ? pc.settings.ssaoIntensity : 0.0f, t.ssr ? 1.0f : 0.0f,
                          1.0f / Half(t.width), 1.0f / Half(t.height),
                          quad ? pc.view.mainRect[0] : 0.0f, quad ? pc.view.mainRect[1] : 0.0f,
                          quad ? pc.view.mainRect[2] : 0.0f, quad ? pc.view.mainRect[3] : 0.0f,
                          // gP2.x: DLSS-RR runs on the RayTraced path (raw AO + raw reflections, no spatial gathers).
                          pc.dlssRr && pc.path == RenderPath::RayTraced ? 1.0f : 0.0f,
                          // gP2.y: the full-res mirror reflection texture is bound this frame.
                          t.ssrMirror ? 1.0f : 0.0f, 0, 0};
    t.lit.Transition(pc.cmd, kRt);
    pipe_.Draw(pc, {&t.lit},
               pc.transient.SrvTable(pc.ctx, {&t.color, &t.depth, &t.normal, t.ao, t.ssr, t.ssrMirror}), c0, 12);
    t.lit.Transition(pc.cmd, kSrv);
    t.hdrFinal = &t.lit;
}

// ---- TaaPass ---------------------------------------------------------------------------------

bool TaaPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    return pipe_.Create(ctx, shaderDir / L"taa.hlsl", "PSTaa", {RenderTargets::kColorFormat});
}

void TaaPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    for (Texture& h : history_)
        h.Create(ctx, targets.width, targets.height, RenderTargets::kColorFormat,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"taa.history");
}

void TaaPass::ReleaseTargets(Dx12Context& ctx) {
    for (Texture& h : history_) h.Release(ctx);
}

void TaaPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    if (!pc.settings.taa || pc.upscaler || pc.dlssRr || !history_[0]) return;
    Texture& out = history_[current_];
    Texture& prev = history_[current_ ^ 1];
    const float c0[4] = {pc.historyValid ? 1.0f : 0.0f, 0, 0, 0};
    out.Transition(pc.cmd, kRt);
    pipe_.Draw(pc, {&out}, pc.transient.SrvTable(pc.ctx, {&t.lit, &prev, &t.velocity, &t.depth}), c0, 4);
    out.Transition(pc.cmd, kSrv);
    t.hdrFinal = &out;
    current_ ^= 1;
}

// ---- PostPass ----------------------------------------------------------------------------------

bool PostPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    return pipe_.Create(ctx, shaderDir / L"post.hlsl", "PSPost", {RenderTargets::kLdrFormat});
}

void PostPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    const RenderSettings& s = pc.settings;
    const bool lutOn = t.lut && !pc.offscreen && s.lutIntensity > 0.0f;
    const BloomLook& bl = s.bloomLook;
    const bool energy = bl.blend == 1;
    const float bloomGain = (energy ? std::min(s.bloomIntensity, 1.0f) : s.bloomIntensity) * t.bloomScale;
    const bool depthMask = t.bloom && t.depth && bl.depthMask > 0.0f;
    const float c[16] = {s.exposure, bloomGain, s.contrast, s.saturation,
                         s.vignette, s.transparentBackground ? 1.0f : 0.0f,
                         (float)t.ldr.width / (float)t.ldr.height, t.bloom ? 1.0f : 0.0f,
                         lutOn ? s.lutIntensity : 0.0f, depthMask ? bl.depthMask : 0.0f, energy ? 1.0f : 0.0f,
                         s.bloomThreshold, std::clamp(s.bloomIntensity, 0.0f, 1.0f), 0, 0, 0};
    t.ldr.Transition(pc.cmd, kRt);
    pipe_.Draw(pc, {&t.ldr},
               pc.transient.SrvTable(pc.ctx, {t.hdrFinal ? t.hdrFinal : &t.lit, t.bloom, lutOn ? t.lut : nullptr,
                                              depthMask ? &t.depth : nullptr}),
               c, 16);
    t.ldr.Transition(pc.cmd, kSrv);
}

// ---- BackdropPass -------------------------------------------------------------------------------

bool BackdropPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    return pipe_.Create(ctx, shaderDir / L"post.hlsl", "PSBackdrop", {RenderTargets::kLdrFormat});
}

void BackdropPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    const uint32_t w = std::max(1u, targets.outWidth / 4), h = std::max(1u, targets.outHeight / 4);
    a_.Create(ctx, w, h, RenderTargets::kLdrFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"backdrop.a");
    b_.Create(ctx, w, h, RenderTargets::kLdrFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"backdrop.b");
    targets.uiBackdrop = &a_;
}

void BackdropPass::ReleaseTargets(Dx12Context& ctx) {
    a_.Release(ctx);
    b_.Release(ctx);
}

void BackdropPass::Execute(PassContext& pc) {
    if (pc.offscreen || pc.settings.headless || !a_) return;
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const float down[4] = {1.0f / t.ldr.width, 1.0f / t.ldr.height, 0, 0};
    a_.Transition(cmd, kRt);
    pipe_.Draw(pc, {&a_}, pc.transient.SrvTable(pc.ctx, {&t.ldr}), down, 4);
    a_.Transition(cmd, kSrv);
    for (int pass = 1; pass <= 2; ++pass) {
        const float hx[4] = {0, 0, pass / (float)a_.width, 0};
        b_.Transition(cmd, kRt);
        pipe_.Draw(pc, {&b_}, pc.transient.SrvTable(pc.ctx, {&a_}), hx, 4);
        b_.Transition(cmd, kSrv);
        const float vy[4] = {0, 0, 0, pass / (float)a_.height};
        a_.Transition(cmd, kRt);
        pipe_.Draw(pc, {&a_}, pc.transient.SrvTable(pc.ctx, {&b_}), vy, 4);
        a_.Transition(cmd, kSrv);
    }
    t.uiBackdrop = &a_;
}

// ---- PresentPass ----------------------------------------------------------------------------------

bool PresentPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_DESCRIPTOR_RANGE range[1];
    range[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsDescriptorTable(1, range, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler[1];
    sampler[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(1, params, 1, sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    if (!CreateRootSignature(device, rs, rootSig_, "PresentPass: CreateRootSignature")) return false;

    ComPtr<ID3DBlob> vs = CompileShader(shaderDir / L"present.hlsl", "VSFullscreen", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(shaderDir / L"present.hlsl", "PSPresent", "ps_5_1");
    if (!vs || !ps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = Dx12Context::kBackBufferFormat;
    pso.SampleDesc.Count = 1;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    return CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)), "PresentPass: PSO");
}

void PresentPass::Execute(PassContext& pc) {
    if (pc.offscreen || pc.settings.headless) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    Dx12Context& ctx = pc.ctx;
    RenderTargets& t = pc.targets;
    t.ldr.Transition(cmd, kSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.BackBufferRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

    const float bw = (float)ctx.Width();
    const float bh = (float)ctx.Height();
    float area[4], fit[4];
    ViewportArea(pc.settings, bw, bh, area);
    FitInArea(area, (float)t.ldr.width, (float)t.ldr.height, fit);
    D3D12_VIEWPORT viewport{fit[0], fit[1], fit[2], fit[3], 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)bw, (LONG)bh};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->SetGraphicsRootDescriptorTable(0, pc.transient.SrvTable(ctx, {&t.ldr}));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Contract: UI draws next with the full back buffer visible.
    D3D12_VIEWPORT fullViewport{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &fullViewport);
    cmd->RSSetScissorRects(1, &scissor);
}

} // namespace mmdx
