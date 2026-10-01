// render_smoke: end-to-end self test, no asset files required.
#include "render/Dx12Context.h"
#include "render/Renderer.h"
#include "render/GpuModel.h"
#include "asset/PmxModel.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <windows.h>
#include <directx/d3dx12.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

} // namespace

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{}; sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat; sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "CRASH code=0x%08lX\n", ep->ExceptionRecord->ExceptionCode);
    for (int i = 0; i < 20 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256]; auto* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0; IMAGEHLP_LINE64 line{sizeof(line)}; DWORD ld = 0;
        const char* name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, si) ? si->Name : "?";
        if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line)) fprintf(stderr, "  %s %s:%lu\n", name, line.FileName, line.LineNumber);
        else fprintf(stderr, "  %s\n", name);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(CrashFilter);
    std::filesystem::path outPng = L"render_smoke.png";
    if (argc > 1 && argv[1][0] != L'\0') outPng = argv[1];

    // 1. Window.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MMDX12RenderSmoke";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        printf("RegisterClassEx failed\n");
        return 1;
    }
    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"MMDX12 render smoke", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        printf("CreateWindowEx failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 2. Device.
    mmdx::Dx12Context ctx;
    if (!ctx.Initialize(hwnd, 1280, 720, /*enableDebugLayer*/ true)) {
        printf("Dx12Context::Initialize failed\n");
        return 1;
    }
    const mmdx::DeviceCaps& caps = ctx.Caps();
    printf("adapter: %s (%.0f MB)\n", caps.adapterName.c_str(),
           (double)caps.dedicatedVideoMemory / (1024.0 * 1024.0));
    printf("feature level: 0x%04X shader model: 0x%04X\n", (unsigned)caps.featureLevel,
           (unsigned)caps.shaderModel);
    printf("rt tier: %d mesh tier: %d vrs tier: %d tearing: %d\n", (int)caps.raytracingTier,
           (int)caps.meshShaderTier, (int)caps.vrsTier, caps.tearingSupported ? 1 : 0);

    // 3. Renderer.
    std::filesystem::path shaderDir =
        mmdx::FindUpward(mmdx::ExecutableDir(), L"shaders/mmd.hlsl") / L"shaders";
    mmdx::Renderer r;
    if (!r.Initialize(ctx, shaderDir)) {
        printf("Renderer::Initialize failed (%s)\n", mmdx::PathToUtf8(shaderDir).c_str());
        return 1;
    }

    // 4. Cube model built in code.
    mmdx::PmxModel pmx;
    pmx.name = "smoke cube";
    const float h = 5.0f; // half size

    using mmdx::XMFLOAT2;
    using mmdx::XMFLOAT3;

    struct FaceDef {
        XMFLOAT3 u; // screen-x axis on the face plane
        XMFLOAT3 v; // screen-y axis on the face plane
        XMFLOAT3 center;
        XMFLOAT3 normal;
    };
    const FaceDef faces[6] = {
        // RH-cross(u, v) == normal, viewed from outside => quad is clockwise for D3D.
        {XMFLOAT3(1, 0, 0), XMFLOAT3(0, 1, 0), XMFLOAT3(0, h, h), XMFLOAT3(0, 0, 1)},   // +Z
        {XMFLOAT3(-1, 0, 0), XMFLOAT3(0, 1, 0), XMFLOAT3(0, h, -h), XMFLOAT3(0, 0, -1)},// -Z
        {XMFLOAT3(0, 1, 0), XMFLOAT3(0, 0, 1), XMFLOAT3(h, h, 0), XMFLOAT3(1, 0, 0)},   // +X
        {XMFLOAT3(0, 0, 1), XMFLOAT3(0, 1, 0), XMFLOAT3(-h, h, 0), XMFLOAT3(-1, 0, 0)}, // -X
        {XMFLOAT3(0, 0, 1), XMFLOAT3(1, 0, 0), XMFLOAT3(0, 2 * h, 0), XMFLOAT3(0, 1, 0)},// +Y
        {XMFLOAT3(1, 0, 0), XMFLOAT3(0, 0, 1), XMFLOAT3(0, 0, 0), XMFLOAT3(0, -1, 0)},  // -Y
    };
    struct Quad { mmdx::PmxVertex v[4]; };
    for (const FaceDef& f : faces) {
        Quad q{};
        const mmdx::PmxVertex corners[4] = {
            // TL, TR, BR, BL in the face plane
            {{f.center.x - h * f.u.x + h * f.v.x, f.center.y - h * f.u.y + h * f.v.y,
              f.center.z - h * f.u.z + h * f.v.z},
             f.normal, XMFLOAT2(0, 0), mmdx::PmxDeform::BDEF1, {0, -1, -1, -1}, {1, 0, 0, 0},
             {}, {}, {}, 1.0f},
            {{f.center.x + h * f.u.x + h * f.v.x, f.center.y + h * f.u.y + h * f.v.y,
              f.center.z + h * f.u.z + h * f.v.z},
             f.normal, XMFLOAT2(1, 0), mmdx::PmxDeform::BDEF1, {0, -1, -1, -1}, {1, 0, 0, 0},
             {}, {}, {}, 1.0f},
            {{f.center.x + h * f.u.x - h * f.v.x, f.center.y + h * f.u.y - h * f.v.y,
              f.center.z + h * f.u.z - h * f.v.z},
             f.normal, XMFLOAT2(1, 1), mmdx::PmxDeform::BDEF1, {0, -1, -1, -1}, {1, 0, 0, 0},
             {}, {}, {}, 1.0f},
            {{f.center.x - h * f.u.x - h * f.v.x, f.center.y - h * f.u.y - h * f.v.y,
              f.center.z - h * f.u.z - h * f.v.z},
             f.normal, XMFLOAT2(0, 1), mmdx::PmxDeform::BDEF1, {0, -1, -1, -1}, {1, 0, 0, 0},
             {}, {}, {}, 1.0f},
        };
        for (const mmdx::PmxVertex& v : corners) pmx.vertices.push_back(v);
    }
    for (uint32_t face = 0; face < 6; ++face) {
        uint32_t b = face * 4;
        const uint32_t quad[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        for (uint32_t idx : quad) pmx.indices.push_back(idx);
    }

    mmdx::PmxBone centerBone;
    centerBone.name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"; // センター
    centerBone.position = XMFLOAT3(0, 0, 0);
    centerBone.parentIndex = -1;
    mmdx::PmxBone upperBone;
    upperBone.name = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB"; // 上半身
    upperBone.position = XMFLOAT3(0, h, 0);
    upperBone.parentIndex = 0;
    pmx.bones.push_back(centerBone);
    pmx.bones.push_back(upperBone);

    pmx.textures.push_back("checker.png");

    mmdx::PmxMaterial mat0; // toon-shaded, with edge
    mat0.name = "front";
    mat0.diffuse = mmdx::XMFLOAT4(1, 0.6f, 0.7f, 1);
    mat0.ambient = mmdx::XMFLOAT3(0.5f, 0.3f, 0.35f);
    mat0.flags = mmdx::PmxMat_Edge;
    mat0.edgeSize = 1;
    mat0.edgeColor = mmdx::XMFLOAT4(0, 0, 0, 1);
    mat0.sharedToon = true;
    mat0.toonIndex = 0;
    mat0.indexCount = 18;
    mmdx::PmxMaterial mat1; // textured, no edge
    mat1.name = "back";
    mat1.diffuse = mmdx::XMFLOAT4(0.4f, 0.8f, 1, 1);
    mat1.ambient = mmdx::XMFLOAT3(0.2f, 0.4f, 0.5f);
    mat1.flags = 0;
    mat1.textureIndex = 0;
    mat1.indexCount = 18;
    pmx.materials.push_back(mat0);
    pmx.materials.push_back(mat1);

    // 64x64 checker texture (8px squares white/grey), 1 mip level.
    std::vector<mmdx::ImageRGBA8> imgs(1);
    mmdx::ImageRGBA8& checker = imgs[0];
    checker.mips.push_back({64, 64, {}});
    checker.mips[0].pixels.resize(64 * 64 * 4);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            bool whiteSq = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t v = whiteSq ? 255 : 128;
            uint8_t* px = &checker.mips[0].pixels[(y * 64 + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 255;
        }
    }

    // 5. Upload.
    mmdx::UploadBatch b(ctx);
    auto gm = r.CreateModel(b, pmx, imgs);
    b.Submit();
    if (!gm) {
        printf("CreateModel failed\n");
        return 1;
    }

    // 6. Render loop.
    std::vector<mmdx::XMFLOAT3> zeroMorphs(24, mmdx::XMFLOAT3(0, 0, 0));
    for (uint32_t frame = 0; frame < 120; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                frame = 120;
                goto pump_end;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        float a = frame * 0.03f;
        DirectX::XMMATRIX rotM = DirectX::XMMatrixRotationY(a);
        DirectX::XMFLOAT4X4 rot;
        DirectX::XMStoreFloat4x4(&rot, rotM);
        std::vector<DirectX::XMFLOAT4X4> skin = {rot, rot};

        ID3D12GraphicsCommandList* cmd = ctx.BeginFrame();
        gm->UpdateSkinning(ctx.FrameSlot(), skin);
        gm->UpdateMorphs(ctx.FrameSlot(), zeroMorphs, 1);

        mmdx::FrameView view;
        DirectX::XMStoreFloat4x4(&view.camera.view,
                                 DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 15, -40, 1),
                                                           DirectX::XMVectorSet(0, 5, 0, 1),
                                                           DirectX::XMVectorSet(0, 1, 0, 1)));
        view.camera.eye = mmdx::XMFLOAT3(0, 15, -40);
        view.models = {gm.get()};

        r.Render(cmd, view);
        if (frame == 119) ctx.RequestCapture(outPng);
        ctx.EndFrame(true); // vsync
    }
pump_end:
    (void)0;

    // 7. Done.
    ctx.WaitForGpu();
    const mmdx::RenderStats& stats = r.Stats();
    printf("drawCalls=%u triangles=%llu gpuMs=%.3f\n", stats.drawCalls,
           (unsigned long long)stats.triangles, stats.gpuFrameMs);

    gm.reset();
    r.Shutdown();
    ctx.Shutdown();
    return 0;
}
