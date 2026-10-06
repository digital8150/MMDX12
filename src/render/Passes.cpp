// Pass implementations (see Passes.h for the order and data flow).
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/GpuModel.h"
#include "render/Renderer.h"
#include "render/RayTracing.h"
#include "render/Upscaler.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "core/TextUtil.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>

namespace mmdx {

namespace {

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

void BindModelBuffers(ID3D12GraphicsCommandList* cmd, const GpuModel& model, uint64_t frame) {
    D3D12_VERTEX_BUFFER_VIEW vbs[4] = {model.VertexBufferView(), model.MorphBufferView(frame),
                                       model.PrevMorphBufferView(frame), model.SdefBufferView()};
    cmd->IASetVertexBuffers(0, 4, vbs);
    cmd->IASetIndexBuffer(&model.IndexBufferView());
}

} // namespace

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
    params[4].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
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

uint32_t SpotShadowCount(const LightParams& light, uint32_t maxLights) {
    uint32_t n = 0;
    const size_t count = std::min<size_t>(light.punctual.size(), maxLights);
    for (size_t i = 0; i < count && n < kSpotShadowSlices; ++i)
        if (light.punctual[i].spotCosOuter > -1.0f) ++n;
    return n;
}

// Renders every shadow-casting material into one depth slice (matrixIndex = gCascade: 0..2 the sun
// cascades, 3 + i spot slice i). Spot slices take characters only: the performers are what cuts
// shafts out of the beams, and re-drawing the whole stage per spot would cost more than the march.
void ShadowPass::DrawSlice(PassContext& pc, const Texture& map, uint32_t slice, uint32_t matrixIndex,
                           bool charactersOnly) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    D3D12_VIEWPORT vp{0, 0, (float)map.width, (float)map.height, 0, 1};
    D3D12_RECT sc{0, 0, (LONG)map.width, (LONG)map.height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pc.ctx.DsvHeap().Cpu(map.dsv + slice);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmd->SetGraphicsRoot32BitConstant(4, matrixIndex, 0);
    for (GpuModel* model : pc.view.models) {
        if (!model || (charactersOnly && model->Role() != ModelRole::Character)) continue;
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
            cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
            pc.stats.drawCalls++;
        }
    }
}

void ShadowPass::Execute(PassContext& pc) {
    Texture& sm = pc.targets.shadowMap;
    Texture& spot = pc.targets.spotShadowMap;
    if (!sm) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const bool cascades = pc.settings.shadows && !pc.view.shadowsOff && !pc.view.models.empty() &&
                          (pc.path == RenderPath::Raster || (pc.settings.volumetric && !pc.offscreen));
    // must match Renderer::FillSceneConstants (spotShadowParams.x)
    const uint32_t spots = (spot && SpotShadowsWanted(pc.settings, pc.path, pc.offscreen))
                               ? SpotShadowCount(pc.view.light, Renderer::kMaxPunctualLights)
                               : 0;
    if (cascades || spots) {
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
            for (uint32_t s = 0; s < spots; ++s) DrawSlice(pc, spot, s, kShadowCascades + s, true);
        }
        spot.Transition(cmd, kSrv);
    }
}

// ---- ScenePass ---------------------------------------------------------------------------

bool ScenePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    ID3D12Device* device = ctx.Device();
    CD3DX12_ROOT_PARAMETER params[12];
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

    CD3DX12_STATIC_SAMPLER_DESC samplers[4];
    samplers[0].Init(0, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 8);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 16,
                     D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
                     D3D12_SHADER_VISIBILITY_PIXEL);
    samplers[2].Init(2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                     D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0, 1,
                     D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
                     D3D12_SHADER_VISIBILITY_PIXEL);
    samplers[3].Init(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                     D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 16,
                     D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
                     D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(12, params, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (!CreateRootSignature(device, rs, rootSig_, "ScenePass: CreateRootSignature")) return false;

    const std::filesystem::path file = shaderDir / L"mmd.hlsl";
    ComPtr<ID3DBlob> vs = CompileShader(file, "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> ps = CompileShader(file, "PSMain", "ps_5_1");
    ComPtr<ID3DBlob> vsEdge = CompileShader(file, "VSEdge", "vs_5_1");
    ComPtr<ID3DBlob> psEdge = CompileShader(file, "PSEdge", "ps_5_1");
    ComPtr<ID3DBlob> vsSky = CompileShader(file, "VSSky", "vs_5_1");
    ComPtr<ID3DBlob> psSky = CompileShader(file, "PSSky", "ps_5_1");
    ComPtr<ID3DBlob> vsFloor = CompileShader(file, "VSFloor", "vs_5_1");
    ComPtr<ID3DBlob> psFloor = CompileShader(file, "PSFloor", "ps_5_1");
    ComPtr<ID3DBlob> psWire = CompileShader(file, "PSWire", "ps_5_1");
    ComPtr<ID3DBlob> psWireBg = CompileShader(file, "PSWireBg", "ps_5_1");
    ComPtr<ID3DBlob> psWireFloor = CompileShader(file, "PSWireFloor", "ps_5_1");
    if (!vs || !ps || !vsEdge || !psEdge || !vsSky || !psSky || !vsFloor || !psFloor || !psWire ||
        !psWireBg || !psWireFloor)
        return false;

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

    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    shaderDir_ = shaderDir;
    litDesc_ = pso;
    vs_ = vs;
    vsRt_.Reset();
    vsDxc_.Reset();
    packPsos_.clear();
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoCullBack_)), "ScenePass: PSO back"))
        return false;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (!CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoNoCull_)), "ScenePass: PSO no cull"))
        return false;

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
            if (!okRt) {
                psoCullBackRt_.Reset();
                psoNoCullRt_.Reset();
                psoFloorRt_.Reset();
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
    // Flat background (wireframe): the same draw with one flat colour.
    sky.PS = {psWireBg->GetBufferPointer(), psWireBg->GetBufferSize()};
    if (!CheckHr(device->CreateGraphicsPipelineState(&sky, IID_PPV_ARGS(&psoFlatBg_)), "ScenePass: PSO flat bg"))
        return false;

    floor.VS = {vsFloor->GetBufferPointer(), vsFloor->GetBufferSize()};
    floor.PS = {psFloor->GetBufferPointer(), psFloor->GetBufferSize()};
    bool okFloor = CheckHr(device->CreateGraphicsPipelineState(&floor, IID_PPV_ARGS(&psoFloor_)), "ScenePass: PSO floor");
    // Wireframe floor: the two-triangle quad as its 5 edges, flat colour.
    floor.PS = {psWireFloor->GetBufferPointer(), psWireFloor->GetBufferSize()};
    floor.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    okFloor = okFloor &&
              CheckHr(device->CreateGraphicsPipelineState(&floor, IID_PPV_ARGS(&psoWireFloor_)), "ScenePass: PSO wire floor");
    return okFloor;
}

const ScenePass::PackPipelines* ScenePass::PackPsos(Dx12Context& ctx, const std::string& id, bool rt) {
    ShaderPackRegistry& reg = ShaderPacks();
    if (reg.Generation() != packGeneration_) {
        // reload: the old PSOs may still be in flight
        if (!packPsos_.empty()) ctx.WaitForGpu();
        packPsos_.clear();
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
        p = {};
        p.failed = true;
        return nullptr;
    };
    ID3D12Device* device = ctx.Device();
    if (!p.back) {
        // DXC (FXC cannot #include a macro): shader model 6.0, so the VS is a DXC build of VSMain as well
        if (!vsDxc_) vsDxc_ = CompileShaderDxc(file, "VSMain", "vs_6_0");
        std::string errors;
        ComPtr<ID3DBlob> ps =
            vsDxc_ ? CompileShaderDxc(file, "PSPack", "ps_6_0", {{"MMDX_PACK", incDefine}}, &errors) : nullptr;
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
    if (rt && !p.rtTried && vsRt_) {
        // ray-traced sun shadows: DXC variant; without it the raster pack PSOs draw (shadow maps)
        p.rtTried = true;
        ComPtr<ID3DBlob> ps = CompileShaderDxc(file, "PSPack", "ps_6_5", {{"RT_SHADOWS", "1"}, {"MMDX_PACK", incDefine}});
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
    return &p;
}

void ScenePass::Execute(PassContext& pc) {
    if (pc.path == RenderPath::PathTraced) return;
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

    const bool rt = pc.path == RenderPath::RayTraced && pc.rt && psoCullBackRt_;
    const bool wire = pc.path == RenderPath::Raster && pc.settings.shading == ViewShading::Wireframe;
    cmd->SetGraphicsRootSignature(rootSig_.Get());
    cmd->SetGraphicsRootDescriptorTable(5, pc.transient.SrvTable(ctx, {&t.shadowMap}));
    cmd->SetGraphicsRootDescriptorTable(11, pc.transient.SrvTable(ctx, {&t.spotShadowMap}));
    cmd->SetGraphicsRootShaderResourceView(6, pc.lights);
    if (rt) {
        cmd->SetGraphicsRootShaderResourceView(7, pc.rt->Tlas());
        cmd->SetGraphicsRootShaderResourceView(8, pc.rt->Geometries());
        cmd->SetGraphicsRootDescriptorTable(9, ctx.SrvHeap().Gpu(0));
        cmd->SetGraphicsRootDescriptorTable(10, ctx.SrvHeap().Gpu(0));
    }
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

        if (!pc.settings.transparentBackground) {
            // Wireframe / ortho views: one flat colour instead of the sky gradient (which needs a perspective camera)
            cmd->SetPipelineState((wire || vd.ortho) ? psoFlatBg_.Get() : psoSky_.Get());
            cmd->DrawInstanced(3, 1, 0, 0);
        }
        // the procedural floor: lit / wire in the camera view; an ortho view shows it as lines only when wireframe
        // (a solid plane would hide everything in the top view)
        if (pc.view.studioFloor && (!vd.ortho || wire)) {
            cmd->SetPipelineState(wire ? psoWireFloor_.Get() : (rt ? psoFloorRt_.Get() : psoFloor_.Get()));
            cmd->DrawInstanced(6, 1, 0, 0);
        }

        for (GpuModel* model : pc.view.models) {
            if (!model) continue;
            BindModelBuffers(cmd, *model, pc.frame);
            cmd->SetGraphicsRootShaderResourceView(2, model->BoneBuffer(pc.frame));
            cmd->SetGraphicsRootShaderResourceView(4, model->PrevBoneBuffer(pc.frame));

            // shader pack: lit camera view only (unlit / wireframe / the flat ortho views keep the default shading)
            const PackPipelines* pack = nullptr;
            if (!model->ShaderPackId().empty() && !wire && !vd.ortho && pc.settings.shading == ViewShading::Lit)
                pack = PackPsos(ctx, model->ShaderPackId(), rt);
            const bool packRt = pack && rt && pack->backRt;
            for (const GpuModel::Material& m : model->Materials()) {
                if (m.indexCount == 0 || !m.visible) continue;   // MMD skips materials with alpha 0
                ID3D12PipelineState* want;
                if (wire)
                    want = m.doubleSided ? psoWireNoCull_.Get() : psoWireBack_.Get();
                else if (pack)
                    want = m.doubleSided ? (packRt ? pack->noCullRt.Get() : pack->noCull.Get())
                                         : (packRt ? pack->backRt.Get() : pack->back.Get());
                else
                    want = m.doubleSided ? (rt ? psoNoCullRt_.Get() : psoNoCull_.Get())
                                         : (rt ? psoCullBackRt_.Get() : psoCullBack_.Get());
                cmd->SetPipelineState(want);
                cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
                pc.stats.drawCalls++;
                pc.stats.triangles += m.indexCount / 3;
            }

            if (pc.settings.drawEdges && !wire) {   // wireframe: the model is drawn as edges already
                cmd->SetPipelineState(psoEdge_.Get());
                for (const GpuModel::Material& m : model->Materials()) {
                    if (!m.drawEdge || m.indexCount == 0) continue;
                    cmd->SetGraphicsRootConstantBufferView(1, m.constants);
                    cmd->SetGraphicsRootDescriptorTable(3, ctx.SrvHeap().Gpu(m.srvTable));
                    cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
                    pc.stats.drawCalls++;
                }
            }
        }
    }
}

// ---- ResolvePass ---------------------------------------------------------------------------

bool ResolvePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t msaa) {
    return pipe_.Create(ctx, shaderDir / L"resolve.hlsl", "PSResolve",
                        {RenderTargets::kColorFormat, RenderTargets::kNormalFormat, RenderTargets::kVelocityFormat,
                         DXGI_FORMAT_R32_FLOAT},
                        {{"MSAA_SAMPLES", std::to_string(msaa > 1 ? msaa : 1)}});
}

void ResolvePass::Execute(PassContext& pc) {
    if (pc.path == RenderPath::PathTraced) return;
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.colorMsaa.Transition(cmd, kSrv);
    t.normalMsaa.Transition(cmd, kSrv);
    t.velocityMsaa.Transition(cmd, kSrv);
    t.depthMsaa.Transition(cmd, kSrv);
    t.color.Transition(cmd, kRt);
    t.normal.Transition(cmd, kRt);
    t.velocity.Transition(cmd, kRt);
    t.depth.Transition(cmd, kRt);
    auto table = pc.transient.SrvTable(pc.ctx, {&t.colorMsaa, &t.normalMsaa, &t.velocityMsaa, &t.depthMsaa});
    pipe_.Draw(pc, {&t.color, &t.normal, &t.velocity, &t.depth}, table);
    t.color.Transition(cmd, kSrv);
    t.normal.Transition(cmd, kSrv);
    t.velocity.Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
}

// ---- SsaoPass -------------------------------------------------------------------------------

bool SsaoPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const bool ok = ao_.Create(ctx, shaderDir / L"ssao.hlsl", "PSAo", {DXGI_FORMAT_R8_UNORM}) &&
                    blur_.Create(ctx, shaderDir / L"ssao.hlsl", "PSBlur", {DXGI_FORMAT_R8_UNORM});
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
}

void SsaoPass::ReleaseTargets(Dx12Context& ctx) {
    raw_.Release(ctx);
    temp_.Release(ctx);
    out_.Release(ctx);
}

void SsaoPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.ao = nullptr;
    if (pc.path == RenderPath::PathTraced || !pc.settings.ssao || !out_) return;
    if (pc.path == RenderPath::RayTraced && pc.rt && rtao_) {
        raw_.Transition(cmd, kUav);
        t.depth.Transition(cmd, kSrvAll);
        t.normal.Transition(cmd, kSrvAll);
        const float c0[4] = {pc.settings.ssaoRadius * 2.5f, 1.0f / raw_.width, 1.0f / raw_.height, 4.0f};
        rtao_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}),
                       pc.transient.UavTable(pc.ctx, {&raw_}), c0, 4, Groups(raw_.width), Groups(raw_.height));
        raw_.Transition(cmd, kSrv);
        t.depth.Transition(cmd, kSrv);
        t.normal.Transition(cmd, kSrv);
    } else {
        const float c0[4] = {pc.settings.ssaoRadius, 16.0f, 1.0f / t.width, 1.0f / t.height};
        raw_.Transition(cmd, kRt);
        ao_.Draw(pc, {&raw_}, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}), c0, 4);
        raw_.Transition(cmd, kSrv);
    }

    const float h[4] = {1.0f / raw_.width, 0, 0, 0};
    temp_.Transition(cmd, kRt);
    blur_.Draw(pc, {&temp_}, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, &raw_}), h, 4);
    temp_.Transition(cmd, kSrv);

    const float v[4] = {0, 1.0f / raw_.height, 0, 0};
    out_.Transition(cmd, kRt);
    blur_.Draw(pc, {&out_}, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, &temp_}), v, 4);
    out_.Transition(cmd, kSrv);
    t.ao = &out_;
}

// ---- SsrPass --------------------------------------------------------------------------------

bool SsrPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!pipe_.Create(ctx, shaderDir / L"ssr.hlsl", "PSSsr", {DXGI_FORMAT_R16G16B16A16_FLOAT})) return false;
    if (RtPipelinesSupported(ctx) && !rt_.Create(ctx, shaderDir / L"rtreflect.hlsl", "CSReflect"))
        LOG_WARN("SSR: ray-traced reflections unavailable");  // never fails the pass
    return true;
}

void SsrPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    out_.Create(ctx, Half(targets.width), Half(targets.height), DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kSrv,
                L"ssr.out");
}

void SsrPass::ReleaseTargets(Dx12Context& ctx) { out_.Release(ctx); }

void SsrPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    t.ssr = nullptr;
    if (pc.path == RenderPath::PathTraced || !pc.settings.ssr || !out_) return;
    if (pc.path == RenderPath::RayTraced && pc.rt && rt_) {
        out_.Transition(cmd, kUav);
        t.depth.Transition(cmd, kSrvAll);
        t.normal.Transition(cmd, kSrvAll);
        const float c0[4] = {1.0f / out_.width, 1.0f / out_.height, 1200.0f, 0};
        rt_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.normal}),
                     pc.transient.UavTable(pc.ctx, {&out_}), c0, 4, Groups(out_.width), Groups(out_.height));
        out_.Transition(cmd, kSrv);
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

bool PathTracePass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!RtPipelinesSupported(ctx)) return true;
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
    historyValid_ = false;
    current_ = 0;
}

void PathTracePass::ReleaseTargets(Dx12Context& ctx) {
    light_.Release(ctx);
    albedo_.Release(ctx);
    for (Texture& tex : history_) tex.Release(ctx);
    for (Texture& tex : histDepth_) tex.Release(ctx);
    for (Texture& tex : histNormal_) tex.Release(ctx);
    filterA_.Release(ctx);
    filterB_.Release(ctx);
    historyValid_ = false;
}

void PathTracePass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    if (pc.path != RenderPath::PathTraced || !pc.rt || !trace_ || !temporal_ || !atrous_ || !modulate_ || !light_)
        return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t W = t.width, H = t.height;
    const uint32_t gx = Groups(W), gy = Groups(H);

    // 1. trace: noisy light + primary albedo; rewrites the G-buffer targets as UAVs.
    light_.Transition(cmd, kUav);
    albedo_.Transition(cmd, kUav);
    t.normal.Transition(cmd, kUav);
    t.velocity.Transition(cmd, kUav);
    t.depth.Transition(cmd, kUav);
    const float c0[8] = {(float)std::clamp(pc.settings.ptSamples, 1u, 4u),
                         (float)std::clamp(pc.settings.ptBounces, 1u, 6u),
                         pc.view.studioFloor ? 1.0f : 0.0f,
                         pc.settings.transparentBackground ? 1.0f : 0.0f,
                         (float)(pc.frame % 65536), 0, 0, 0};
    trace_.Dispatch(pc, {}, pc.transient.UavTable(pc.ctx, {&light_, &albedo_, &t.normal, &t.velocity, &t.depth}),
                    c0, 8, gx, gy);

    // 2. temporal accumulation into this frame's history slots.
    const uint32_t cur = current_, prev = current_ ^ 1;
    light_.Transition(cmd, kSrvAll);
    t.velocity.Transition(cmd, kSrvAll);
    t.depth.Transition(cmd, kSrvAll);
    t.normal.Transition(cmd, kSrvAll);
    for (Texture* tex : {&history_[prev], &histDepth_[prev], &histNormal_[prev]}) tex->Transition(cmd, kSrvAll);
    for (Texture* tex : {&history_[cur], &histDepth_[cur], &histNormal_[cur]}) tex->Transition(cmd, kUav);
    const bool valid = historyValid_ && !pc.view.cameraCut && !pc.offscreen;
    const float c1[4] = {valid ? 1.0f : 0.0f, 24.0f, 0.08f, 0};
    temporal_.Dispatch(pc, pc.transient.SrvTable(pc.ctx,
                                                {&light_, &history_[prev], &t.velocity, &t.depth, &t.normal,
                                                 &histDepth_[prev], &histNormal_[prev]}),
                       pc.transient.UavTable(pc.ctx, {&history_[cur], &histDepth_[cur], &histNormal_[cur]}),
                       c1, 4, gx, gy);

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
    albedo_.Transition(cmd, kSrvAll);
    t.color.Transition(cmd, kUav);
    const float c3[4] = {pc.settings.transparentBackground ? 1.0f : 0.0f, 0, 0, 0};
    modulate_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&filterA_, &albedo_, &t.depth}),
                       pc.transient.UavTable(pc.ctx, {&t.color}), c3, 4, gx, gy);

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
    in.color = t.lit.res.Get();
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
    const float c0[8] = {t.ao ? pc.settings.ssaoIntensity : 0.0f, t.ssr ? 1.0f : 0.0f,
                         1.0f / Half(t.width), 1.0f / Half(t.height),
                         quad ? pc.view.mainRect[0] : 0.0f, quad ? pc.view.mainRect[1] : 0.0f,
                         quad ? pc.view.mainRect[2] : 0.0f, quad ? pc.view.mainRect[3] : 0.0f};
    t.lit.Transition(pc.cmd, kRt);
    pipe_.Draw(pc, {&t.lit}, pc.transient.SrvTable(pc.ctx, {&t.color, &t.depth, &t.normal, t.ao, t.ssr}), c0, 8);
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
    if (!pc.settings.taa || pc.upscaler || !history_[0]) return;
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
    const float c[12] = {s.exposure, s.bloomIntensity / (float)BloomPass::kMips, s.contrast, s.saturation,
                         s.vignette, s.transparentBackground ? 1.0f : 0.0f,
                         (float)t.ldr.width / (float)t.ldr.height, t.bloom ? 1.0f : 0.0f,
                         lutOn ? s.lutIntensity : 0.0f, 0, 0, 0};
    t.ldr.Transition(pc.cmd, kRt);
    pipe_.Draw(pc, {&t.ldr}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal ? t.hdrFinal : &t.lit, t.bloom, lutOn ? t.lut : nullptr}), c, 12);
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
