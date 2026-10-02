#include "app/App.h"

#include <ShlObj.h>
#include <directx/d3dx12.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/Lighting.h"
#include "app/UiKit.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mmdx {

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

AppOptions ParseCommandLine(int argc, wchar_t** argv) {
    AppOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        if (arg == L"--library") {
            opt.libraryOverride = next();
        } else if (arg == L"--character") {
            opt.character = WideToUtf8(next());
        } else if (arg == L"--stage") {
            opt.stage = WideToUtf8(next());
        } else if (arg == L"--song") {
            opt.song = WideToUtf8(next());
        } else if (arg == L"--autoplay") {
            opt.autoplay = true;
        } else if (arg == L"--seek") {
            opt.seekSeconds = _wtof(next().c_str());
        } else if (arg == L"--benchmark") {
            opt.benchmarkCategory = WideToUtf8(next());
        } else if (arg == L"--bench-frames") {
            opt.benchFrames = _wtoi(next().c_str());
        } else if (arg == L"--frames") {
            opt.quitAfterFrames = _wtoi(next().c_str());
        } else if (arg == L"--capture") {
            opt.capturePath = next();
        } else if (arg == L"--width") {
            opt.width = _wtoi(next().c_str());
        } else if (arg == L"--height") {
            opt.height = _wtoi(next().c_str());
        } else if (arg == L"--debug") {
            opt.debugLayer = true;
        } else if (arg == L"--screen") {
            opt.startScreen = WideToUtf8(next());
        } else if (arg == L"--lighting") {
            opt.lighting = _wtoi(next().c_str());
        } else if (arg == L"--quality") {
            opt.quality = _wtoi(next().c_str());
        } else if (arg == L"--render") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "raster") opt.renderPath = 0;
            else if (v == "rt") opt.renderPath = 1;
            else if (v == "pt") opt.renderPath = 2;
            else LOG_WARN("unknown --render value: %s (want raster|rt|pt)", v.c_str());
        } else if (arg == L"--upscaler") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "none") opt.upscaler = 0;
            else if (v == "dlss") opt.upscaler = 1;
            else if (v == "fsr") opt.upscaler = 2;
            else if (v == "xess") opt.upscaler = 3;
            else LOG_WARN("unknown --upscaler value: %s (want none|dlss|fsr|xess)", v.c_str());
        } else if (arg == L"--upscale-quality") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "native") opt.upscalerQuality = 0;
            else if (v == "quality") opt.upscalerQuality = 1;
            else if (v == "balanced") opt.upscalerQuality = 2;
            else if (v == "performance") opt.upscalerQuality = 3;
            else if (v == "ultra") opt.upscalerQuality = 4;
            else LOG_WARN("unknown --upscale-quality value: %s (want native|quality|balanced|performance|ultra)", v.c_str());
        } else if (arg == L"--dof") {
            opt.dof = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--volumetric") {
            opt.volumetric = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--bloom-conv") {
            opt.bloomConv = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--lut") {
            opt.lut = WideToUtf8(next());
        } else if (arg == L"--no-physics") {
            opt.noPhysics = true;
        } else if (arg == L"--offline-still") {
            opt.offlineStill = next();
        } else if (arg == L"--offline-video") {
            opt.offlineVideo = next();
        } else if (arg == L"--offline-range") {
            opt.offlineRange[0] = _wtof(next().c_str());
            opt.offlineRange[1] = _wtof(next().c_str());
        } else if (arg == L"--offline-spp") {
            opt.offlineSpp = _wtoi(next().c_str());
        } else if (arg == L"--offline-size") {
            opt.offlineSize[0] = _wtoi(next().c_str());
            opt.offlineSize[1] = _wtoi(next().c_str());
        } else if (arg == L"--paused") {
            opt.paused = true;
        } else if (arg == L"--free-camera") {
            opt.freeCamera = true;
        } else if (arg == L"--camera") {
            std::string v = WideToUtf8(next());
            if (sscanf_s(v.c_str(), "%f,%f,%f,%f,%f,%f", &opt.camera[0], &opt.camera[1], &opt.camera[2],
                         &opt.camera[3], &opt.camera[4], &opt.camera[5]) == 6) {
                opt.hasCamera = true;
                opt.freeCamera = true;
            }
        } else {
            LOG_WARN("unknown command line argument: %s", WideToUtf8(arg).c_str());
        }
    }
    return opt;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int App::Run(HINSTANCE instance, const AppOptions& options) {
    options_ = options;
    settingsPath_ = ExecutableDir() / L"mmdx12.ini";
    settings_.Load(settingsPath_);
    const AppSettings persisted = settings_;  // CLI overrides below are for this run only
    if (options_.lighting >= 0) settings_.lighting = std::clamp(options_.lighting, 0, kLightingPresetCount - 1);
    if (options_.quality >= 0) ApplyGraphicsPreset(std::clamp(options_.quality, 0, 3));
    if (options_.renderPath >= 0) settings_.renderPath = std::clamp(options_.renderPath, 0, 2);
    if (options_.upscaler >= 0) settings_.upscaler = std::clamp(options_.upscaler, 0, 3);
    if (options_.upscalerQuality >= 0) settings_.upscalerQuality = std::clamp(options_.upscalerQuality, 0, 4);
    if (options_.dof >= 0) settings_.dof = options_.dof != 0;
    if (options_.volumetric >= 0) settings_.volumetric = options_.volumetric != 0;
    if (options_.bloomConv >= 0) settings_.bloomConvolution = options_.bloomConv != 0;

    ImGui_ImplWin32_EnableDpiAwareness();

    int w = options_.width > 0 ? options_.width : settings_.windowWidth;
    int h = options_.height > 0 ? options_.height : settings_.windowHeight;
    if (!InitWindow(instance, w, h)) return 1;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!ctx_.Initialize(hwnd_, (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top),
                         options_.debugLayer)) {
        MessageBoxW(hwnd_, L"Direct3D 12 초기화에 실패했습니다.", L"MMDX12", MB_ICONERROR);
        return 1;
    }

    std::filesystem::path shaderDir = ExecutableDir() / L"shaders";
    if (!std::filesystem::exists(shaderDir / L"mmd.hlsl")) {
        std::filesystem::path found = FindUpward(ExecutableDir(), L"shaders/mmd.hlsl");
        if (!found.empty()) shaderDir = found / L"shaders";
    }
    if (!renderer_.Initialize(ctx_, shaderDir)) {
        MessageBoxW(hwnd_, L"렌더러 초기화에 실패했습니다.", L"MMDX12", MB_ICONERROR);
        return 1;
    }

    ApplyRenderSettings();

    if (!audio_.Initialize()) LOG_WARN("audio disabled");
    audio_.SetVolume(settings_.volume);

    if (!InitImGui()) return 1;
    thumbs_.Initialize(ctx_, ThumbnailCacheDir(),
                       [this](ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out) {
                           return RenderThumbnail(kind, models, out);
                       });

    std::strncpy(nicknameEdit_, settings_.nickname.c_str(), sizeof(nicknameEdit_) - 1);
    nicknameEdit_[sizeof(nicknameEdit_) - 1] = '\0';
    std::filesystem::path libPath = ResolveLibraryPath();
    RefreshColorLuts();
    std::string libUtf8 = PathToUtf8(libPath);
    std::strncpy(libraryPathEdit_, libUtf8.c_str(), sizeof(libraryPathEdit_) - 1);
    libraryPathEdit_[sizeof(libraryPathEdit_) - 1] = '\0';

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    timeSeconds_ = (double)start.QuadPart / (double)freq.QuadPart;

    StartScan();
    MainLoop();

    ctx_.WaitForGpu();
    UnloadScene();
    thumbs_.Shutdown();
    ShutdownImGui();
    renderer_.Shutdown();

    RECT crc;
    if (GetClientRect(hwnd_, &crc) && !IsIconic(hwnd_)) {
        settings_.windowWidth = crc.right - crc.left;
        settings_.windowHeight = crc.bottom - crc.top;
    }
    settings_.volume = audio_.Volume();
    settings_.nickname = nicknameEdit_;
    if (options_.lighting >= 0) settings_.lighting = persisted.lighting;
    if (options_.quality >= 0) {
        settings_.graphicsPreset = persisted.graphicsPreset;
        settings_.shadows = persisted.shadows;
        settings_.ssao = persisted.ssao;
        settings_.ssr = persisted.ssr;
        settings_.bloom = persisted.bloom;
        settings_.taa = persisted.taa;
        settings_.shadowMapSize = persisted.shadowMapSize;
    }
    if (options_.renderPath >= 0) settings_.renderPath = persisted.renderPath;
    if (options_.upscaler >= 0) settings_.upscaler = persisted.upscaler;
    if (options_.upscalerQuality >= 0) settings_.upscalerQuality = persisted.upscalerQuality;
    if (options_.dof >= 0) settings_.dof = persisted.dof;
    if (options_.volumetric >= 0) settings_.volumetric = persisted.volumetric;
    if (options_.bloomConv >= 0) settings_.bloomConvolution = persisted.bloomConvolution;
    if (!options_.lut.empty()) settings_.colorLut = persisted.colorLut;
    settings_.Save(settingsPath_);

    ctx_.Shutdown();
    if (hwnd_) DestroyWindow(hwnd_);
    return 0;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            minimized_ = true;
        } else {
            minimized_ = false;
            if (ctx_.Device()) ctx_.Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_CLOSE:
        running_ = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        running_ = false;
        PostQuitMessage(0);
        hwnd_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool App::InitWindow(HINSTANCE instance, int width, int height) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (msg == WM_NCCREATE) {
            auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app) return app->HandleMessage(hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MMDX12Window";
    if (!RegisterClassExW(&wc)) return false;

    RECT rc{0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"MMDX12", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

// ---------------------------------------------------------------------------
// ImGui
// ---------------------------------------------------------------------------

bool App::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    assetsDir_ = ExecutableDir() / L"assets";
    if (!std::filesystem::exists(assetsDir_ / L"fonts")) {
        std::filesystem::path found = FindUpward(ExecutableDir(), L"assets/fonts");
        if (!found.empty()) assetsDir_ = found / L"assets";
    }
    if (!ui::LoadFonts(assetsDir_)) LOG_WARN("UI fonts not found under %s, using system fonts", PathToUtf8(assetsDir_).c_str());
    ui::ApplyStyle(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_));

    if (!ImGui_ImplWin32_Init(hwnd_)) return false;

    ImGui_ImplDX12_InitInfo info;
    info.Device = ctx_.Device();
    info.CommandQueue = ctx_.Queue();
    info.NumFramesInFlight = (int)Dx12Context::kFramesInFlight;
    info.RTVFormat = Dx12Context::kBackBufferFormat;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = &ctx_.SrvHeap();
    info.SrvDescriptorHeap = ctx_.SrvHeap().Heap();
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* c,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* g) {
        auto* h = static_cast<DescriptorHeap*>(i->UserData);
        uint32_t idx = h->Allocate(1);
        *c = h->Cpu(idx);
        *g = h->Gpu(idx);
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE c,
                                  D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto* h = static_cast<DescriptorHeap*>(i->UserData);
        h->Free(h->IndexOf(c), 1);
    };
    return ImGui_ImplDX12_Init(&info);
}

void App::ShutdownImGui() {
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::MainLoop() {
    while (running_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running_ = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;
        if (minimized_) {
            Sleep(16);
            continue;
        }
        RenderFrame();
    }
}

void App::RenderFrame() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double nowSec = (double)now.QuadPart / (double)freq.QuadPart;
    double dt = nowSec - timeSeconds_;
    dt = std::clamp(dt, 0.0, 0.25);
    timeSeconds_ = nowSec;

    if (screen_ == Screen::BenchRun) UpdateBenchRun(); // frame timing first thing in the frame

    // Thumbnails: renders/uploads happen outside frame recording, only on menu screens.
    if (thumbsClearPending_) {
        thumbs_.Clear();
        thumbsClearPending_ = false;
    }
    if (screen_ == Screen::Select || screen_ == Screen::BenchLobby || screen_ == Screen::Loading) thumbs_.Pump();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ui::NewFrame((float)dt);

    PollScan();
    PollLoad();

    switch (screen_) {
    case Screen::Scanning: DrawScanning(); break;
    case Screen::Select: DrawSelect(); break;
    case Screen::Loading: DrawLoading(); break;
    case Screen::Play:
        UpdatePlay(dt);
        UpdateOffline();          // CLI trigger (may switch to Screen::Offline)
        if (screen_ == Screen::Play) {
            DrawPlayOverlay();
            DrawOfflineConfirm();
            DrawToast();
        }
        break;
    case Screen::Offline:
        UpdateOffline();
        DrawOfflineOverlay();
        break;
    case Screen::BenchLobby: DrawBenchLobby(); break;
    case Screen::BenchRun: DrawBenchRunOverlay(); break;
    case Screen::BenchResult: DrawBenchResult(); break;
    }

    ImGui::Render();

    if (!running_) return;

    ApplyColorLut();
    ID3D12GraphicsCommandList* cmd = ctx_.BeginFrame();
    FrameView view;
    const bool inScene = scene_ && (screen_ == Screen::Play || screen_ == Screen::BenchRun);
    if (screen_ == Screen::Offline && scene_) {
        RecordOfflineFrame(cmd);
    } else {
        if (inScene) {
            const float frame = (float)(playTime_ * kMmdFps);
            UpdateScene(frame);
            BuildFrameView(frame, view);
            lastLiveCamera_ = view.camera;
            haveLastLiveCamera_ = true;
        }
        renderer_.Render(cmd, view);
    }
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

    bool quit = false;
    if (inScene || screen_ == Screen::Offline || !options_.startScreen.empty()) {
        ++framesInScene_;
        if (options_.quitAfterFrames > 0 && framesInScene_ >= options_.quitAfterFrames) {
            if (!options_.capturePath.empty()) ctx_.RequestCapture(options_.capturePath);
            quit = true;
        }
    }
    ctx_.EndFrame(renderer_.Settings().vsync);
    if (screen_ == Screen::Offline) AfterOfflineFrame();

    if (quit) {
        ctx_.WaitForGpu();
        LOG_INFO("quit after %d frames", framesInScene_);
        running_ = false;
    }
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

std::filesystem::path App::ResolveLibraryPath() const {
    if (!options_.libraryOverride.empty()) return options_.libraryOverride;
    std::filesystem::path fromSettings = Utf8ToPath(settings_.libraryPath);
    if (!fromSettings.empty() && settings_.libraryPath.find_first_not_of(" \t") != std::string::npos)
        return fromSettings;
    std::filesystem::path exe = ExecutableDir();
    std::filesystem::path local = exe / L"library";
    if (std::filesystem::exists(local)) return local;
    std::filesystem::path found = FindUpward(exe, L"library");
    if (!found.empty()) return found / L"library";
    std::error_code ec;
    std::filesystem::create_directories(local, ec);
    return local;
}

void App::StartScan() {
    screen_ = Screen::Scanning;
    thumbsClearPending_ = true;
    scanProgress_.filesVisited.store(0, std::memory_order_relaxed);
    scanProgress_.filesTotal.store(0, std::memory_order_relaxed);
    const std::filesystem::path path = ResolveLibraryPath();
    scanFuture_ = std::async(std::launch::async, [this, path] {
        return ScanLibrary(path, &scanProgress_);
    });
}

void App::PollScan() {
    if (!scanFuture_.valid()) return;
    if (scanFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    library_ = scanFuture_.get();

    // Restore selections by matching settings ids against asset ids.
    selCharacter_ = -1;
    if (!settings_.lastCharacter.empty()) {
        const std::string needle = ToLowerAscii(settings_.lastCharacter);
        for (size_t i = 0; i < library_.characters.size(); ++i)
            if (ToLowerAscii(library_.characters[i].id) == needle) { selCharacter_ = (int)i; break; }
    }
    selStage_ = -1;
    if (!settings_.lastStage.empty()) {
        const std::string needle = ToLowerAscii(settings_.lastStage);
        for (size_t i = 0; i < library_.stages.size(); ++i)
            if (ToLowerAscii(library_.stages[i].id) == needle) { selStage_ = (int)i; break; }
    }
    selSong_ = -1;
    if (!settings_.lastSong.empty()) {
        const std::string needle = ToLowerAscii(settings_.lastSong);
        for (size_t i = 0; i < library_.songs.size(); ++i)
            if (ToLowerAscii(library_.songs[i].id) == needle) { selSong_ = (int)i; break; }
    }

    ApplyCommandLinePreselection();

    screen_ = Screen::Select;
    if (options_.startScreen == "bench") {
        screen_ = Screen::BenchLobby;
        RefreshLeaderboard();
    } else if (options_.startScreen == "stages") {
        libraryTab_ = 1;
    } else if (options_.startScreen == "songs") {
        libraryTab_ = 2;
    } else if (options_.startScreen == "settings") {
        advancedOpen_ = true;
    }

    // One-shot command line auto-actions.
    if (options_.autoplay && selCharacter_ >= 0 && selSong_ >= 0) {
        StartLoad(LoadTarget::Play, &library_.characters[selCharacter_],
                  selStage_ >= 0 ? &library_.stages[selStage_] : nullptr, &library_.songs[selSong_]);
        options_.autoplay = false;
    } else if (!options_.benchmarkCategory.empty()) {
        int cat = 0;
        for (size_t i = 0; i < std::size(kBenchCategories); ++i)
            if (ToLowerAscii(kBenchCategories[i].id) == ToLowerAscii(options_.benchmarkCategory)) { cat = (int)i; break; }
        if (kBenchCategories[cat].path != RenderPath::Raster && !renderer_.RayTracingSupported()) {
            LOG_ERROR("benchmark %s needs DXR 1.1", kBenchCategories[cat].id);
            options_.benchmarkCategory.clear();
            return;
        }
        benchCategory_ = cat;
        // Same preset-or-selection logic as the lobby's "측정 시작" button.
        const auto findByPreset = [](const auto& list, const char* key) -> int {
            const std::string needle = ToLowerAscii(key);
            for (size_t i = 0; i < list.size(); ++i)
                if (ToLowerAscii(list[i].id).find(needle) != std::string::npos) return (int)i;
            return -1;
        };
        const int presetChar = findByPreset(library_.characters, kBenchPresetCharacter);
        const int presetStage = findByPreset(library_.stages, kBenchPresetStage);
        const int presetSong = findByPreset(library_.songs, kBenchPresetSong);
        benchOfficial_ = presetChar >= 0 && presetStage >= 0 && presetSong >= 0;
        if (benchOfficial_) {
            StartLoad(LoadTarget::Benchmark, &library_.characters[presetChar],
                      &library_.stages[presetStage], &library_.songs[presetSong]);
        } else if (selCharacter_ >= 0 && selSong_ >= 0) {
            StartLoad(LoadTarget::Benchmark, &library_.characters[selCharacter_],
                      selStage_ >= 0 ? &library_.stages[selStage_] : nullptr,
                      &library_.songs[selSong_]);
        }
        options_.benchmarkCategory.clear();
    }
}

void App::ApplyCommandLinePreselection() {
    const auto match = [&](bool character, const std::string& option) -> int {
        if (option.empty()) return -1;
        const std::string needle = ToLowerAscii(option);
        if (character) {
            for (size_t i = 0; i < library_.characters.size(); ++i) {
                if (ToLowerAscii(library_.characters[i].id).find(needle) != std::string::npos ||
                    ToLowerAscii(library_.characters[i].displayName).find(needle) != std::string::npos)
                    return (int)i;
            }
        } else {
            for (size_t i = 0; i < library_.stages.size(); ++i) {
                if (ToLowerAscii(library_.stages[i].id).find(needle) != std::string::npos ||
                    ToLowerAscii(library_.stages[i].displayName).find(needle) != std::string::npos)
                    return (int)i;
            }
        }
        return -1;
    };

    if (!options_.character.empty()) selCharacter_ = match(true, options_.character);
    if (!options_.stage.empty() && ToLowerAscii(options_.stage) == "none") {
        selStage_ = -1;
    } else if (!options_.stage.empty()) {
        selStage_ = match(false, options_.stage);
    }
    if (!options_.song.empty()) {
        selSong_ = -1;
        const std::string needle = ToLowerAscii(options_.song);
        for (size_t i = 0; i < library_.songs.size(); ++i) {
            if (ToLowerAscii(library_.songs[i].id).find(needle) != std::string::npos ||
                ToLowerAscii(library_.songs[i].displayName).find(needle) != std::string::npos) {
                selSong_ = (int)i;
                break;
            }
        }
    }
}

void App::ApplyGraphicsPreset(int preset) {
    settings_.graphicsPreset = preset;
    switch (preset) {
    case 0:  // low
        settings_.shadows = false; settings_.ssao = false; settings_.ssr = false; settings_.bloom = true;
        settings_.taa = false; settings_.shadowMapSize = 1024;
        break;
    case 1:  // medium
        settings_.shadows = true; settings_.ssao = true; settings_.ssr = false; settings_.bloom = true;
        settings_.taa = false; settings_.shadowMapSize = 1024;
        break;
    case 2:  // high
        settings_.shadows = true; settings_.ssao = true; settings_.ssr = true; settings_.bloom = true;
        settings_.taa = false; settings_.shadowMapSize = 2048;
        break;
    case 3:  // ultra
        settings_.shadows = true; settings_.ssao = true; settings_.ssr = true; settings_.bloom = true;
        settings_.taa = true; settings_.shadowMapSize = 4096;
        break;
    default: break;  // custom: keep the toggles
    }
}

void App::ApplyRenderSettings() {
    RenderSettings rs = renderer_.Settings();
    rs.msaaSamples = settings_.msaa;
    rs.renderScale = settings_.renderScale;
    rs.vsync = settings_.vsync;
    rs.drawEdges = settings_.drawEdges;
    rs.shadows = settings_.shadows;
    rs.shadowMapSize = (uint32_t)settings_.shadowMapSize;
    rs.ssao = settings_.ssao;
    rs.ssr = settings_.ssr;
    rs.bloom = settings_.bloom;
    rs.taa = settings_.taa;
    rs.exposure = settings_.exposure;
    rs.renderPath = (RenderPath)settings_.renderPath;
    rs.upscaler = (UpscalerKind)settings_.upscaler;
    rs.upscalerQuality = (UpscalerQuality)settings_.upscalerQuality;
    rs.ptSamples = (uint32_t)settings_.ptSamples;
    rs.ptBounces = (uint32_t)settings_.ptBounces;
    rs.dof = settings_.dof;
    rs.dofAperture = settings_.dofAperture;
    rs.volumetric = settings_.volumetric;
    rs.volumetricDensity = settings_.volumetricDensity;
    rs.bloomConvolution = settings_.bloomConvolution;
    rs.lutIntensity = settings_.lutIntensity;
    rs.fixedResolution = false;
    renderer_.SetSettings(rs);
}

void App::RefreshColorLuts() {
    luts_ = ListColorLuts({ExecutableDir() / L"luts", ResolveLibraryPath() / L"luts"});
    if (!options_.lut.empty()) {
        if (ToLowerAscii(options_.lut) == "none") {
            settings_.colorLut.clear();
        } else {
            settings_.colorLut.clear();
            const std::string needle = ToLowerAscii(options_.lut);
            for (const ColorLutEntry& e : luts_) {
                if (ToLowerAscii(e.id).find(needle) != std::string::npos ||
                    ToLowerAscii(e.displayName).find(needle) != std::string::npos) {
                    settings_.colorLut = e.id;
                    break;
                }
            }
            if (settings_.colorLut.empty()) LOG_WARN("unknown --lut value: %s", options_.lut.c_str());
        }
    }
    if (!settings_.colorLut.empty()) {
        bool found = false;
        for (const ColorLutEntry& e : luts_) {
            if (e.id == settings_.colorLut) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_WARN("unknown color LUT in settings: %s", settings_.colorLut.c_str());
            settings_.colorLut.clear();
        }
    }
    LOG_INFO("color LUTs: %zu", luts_.size());
}

void App::ApplyColorLut() {
    if (settings_.colorLut == appliedLut_) return;
    appliedLut_ = settings_.colorLut;
    const ColorLutEntry* entry = nullptr;
    for (const ColorLutEntry& e : luts_) {
        if (e.id == settings_.colorLut) {
            entry = &e;
            break;
        }
    }
    if (settings_.colorLut.empty() || !entry) {
        renderer_.SetColorLut(nullptr);
        return;
    }
    ImageRGBA8 strip;
    if (BuildColorLut(*entry, strip))
        renderer_.SetColorLut(&strip);
    else
        renderer_.SetColorLut(nullptr);
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

void App::StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st,
                    const SongAsset* song) {
    UnloadScene();
    screen_ = Screen::Loading;
    loadTarget_ = target;
    loadError_.clear();
    loadProgress_.fraction.store(0.0f, std::memory_order_relaxed);
    loadProgress_.SetStatus("");
    loadPackage_ = std::make_unique<ScenePackage>();

    const CharacterAsset& c = *ch;
    const bool hasStage = st != nullptr;
    const StageAsset s = hasStage ? *st : StageAsset{};
    const SongAsset so = *song;
    loadFuture_ = std::async(std::launch::async, [=, pkg = loadPackage_.get(), this] {
        return LoadScenePackage(c, hasStage ? &s : nullptr, so, *pkg, &loadProgress_, &loadError_);
    });
}

void App::PollLoad() {
    if (screen_ != Screen::Loading) return;
    if (!loadFuture_.valid()) return;
    if (loadFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    bool ok = loadFuture_.get();
    if (ok) {
        ok = BuildSceneRuntime(*loadPackage_);
        if (!ok) loadError_ = "GPU 리소스 생성 실패";
    }
    loadPackage_.reset();

    if (!ok) {
        LOG_ERROR("%s", loadError_.c_str());
        return; // stay on Loading; DrawLoading shows the error + "돌아가기"
    }

    if (loadTarget_ == LoadTarget::Play) {
        playTime_ = options_.seekSeconds;
        options_.seekSeconds = 0;
        playing_ = true;
        useMotionCamera_ = scene_->camera != nullptr && !options_.freeCamera;
        if (options_.hasCamera) {
            const float* c = options_.camera;
            freeCam_.target = {c[0], c[1], c[2]};
            freeCam_.yaw = DirectX::XMConvertToRadians(c[3]);
            freeCam_.pitch = DirectX::XMConvertToRadians(c[4]);
            freeCam_.distance = c[5];
        }
        if (scene_->hasAudio) {
            audio_.SetMuted(false);
            audio_.Seek(playTime_);
            audio_.Play();
        }
        framesInScene_ = 0;
        screen_ = Screen::Play;
        // --offline-still renders exactly the --seek frame: hold playback there.
        if (!options_.offlineStill.empty() && !cliOfflineStarted_) options_.paused = true;
        if (options_.paused) {
            options_.paused = false;
            SetPlaying(false);
        }
        return;
    }

    // Benchmark run begin.
    benchFrameTimes_.clear();
    benchFrameTimes_.reserve(options_.benchFrames > 0 ? (size_t)options_.benchFrames
                                                      : (size_t)kBenchMeasuredFrames);
    benchFrameCounter_ = 0;
    benchLastQpc_ = 0;
    playTime_ = 0;
    playing_ = true;
    useMotionCamera_ = scene_->camera != nullptr && !options_.freeCamera;

    const BenchmarkCategory& cat = kBenchCategories[benchCategory_];
    RenderSettings rs = renderer_.Settings();
    rs.fixedResolution = true;
    rs.fixedWidth = cat.width;
    rs.fixedHeight = cat.height;
    rs.vsync = false;
    rs.msaaSamples = 4;
    rs.drawEdges = true;
    rs.renderScale = 1.0f;
    rs.renderPath = cat.path;
    rs.upscaler = UpscalerKind::None;
    rs.ptSamples = 1;
    rs.ptBounces = 3;
    // Fixed workload: the "high" effect set, studio lighting, regardless of user settings.
    rs.shadows = true;
    rs.shadowMapSize = 2048;
    rs.ssao = true;
    rs.ssr = true;
    rs.bloom = true;
    rs.taa = false;
    rs.exposure = 1.0f;
    rs.dof = false;
    rs.volumetric = false;
    rs.bloomConvolution = false;
    rs.lutIntensity = 0.0f;
    renderer_.SetSettings(rs);

    benchStartTime_ = timeSeconds_;
    framesInScene_ = 0;
    screen_ = Screen::BenchRun;
}

bool App::BuildSceneRuntime(ScenePackage& pkg) {
    auto s = std::make_unique<SceneRuntime>();
    UploadBatch batch(ctx_);

    s->character = std::make_unique<ModelInstance>(pkg.character.pmx);
    s->characterGpu = renderer_.CreateModel(batch, *pkg.character.pmx, pkg.character.textures);
    if (!s->characterGpu) return false;

    for (LoadedModelCpu& part : pkg.stageParts) {
        auto inst = std::make_unique<ModelInstance>(part.pmx);
        inst->UpdatePose();
        auto gpu = renderer_.CreateModel(batch, *part.pmx, part.textures, ModelRole::Stage);
        if (!gpu) {
            LOG_WARN("stage part GPU upload failed: %s", part.pmx->name.c_str());
            continue;
        }
        s->stages.push_back(std::move(inst));
        s->stageGpu.push_back(std::move(gpu));
    }
    batch.Submit();

    s->motion = pkg.motion;
    s->camera = pkg.camera;
    s->endFrame = pkg.endFrame;
    s->hasAudio = !pkg.audioPath.empty() && audio_.Load(pkg.audioPath);
    if (s->hasAudio) s->endFrame = std::max(s->endFrame, (float)(audio_.DurationSeconds() * kMmdFps));

    scene_ = std::move(s);
    return true;
}

void App::UnloadScene() {
    if (!scene_) return;
    if (offline_.mode != OfflineMode::None) {
        renderer_.CancelOffline();
        offline_.encoder.reset();
        offline_ = OfflineJob{};
    }
    ctx_.WaitForGpu();
    haveLastLiveCamera_ = false;
    scene_.reset();
    audio_.Unload();
    playing_ = false;
}

void App::UpdateScene(float frame) {
    const uint64_t slot = ctx_.FrameNumber();
    if (scene_->motion) {
        scene_->motion->Evaluate(frame, *scene_->character);
    } else {
        scene_->character->ResetPose();
    }
    // Physics follows the motion clock. Backward jumps (seek, loop) and forward jumps beyond a
    // normal frame step reset the bodies to the animated pose instead of simulating the jump.
    // The benchmark always simulates (fixed workload).
    ModelInstance& ch = *scene_->character;
    ch.EnablePhysics(screen_ == Screen::BenchRun || (settings_.physics && !options_.noPhysics));
    float physicsDt = 0.0f;
    if (scene_->physicsFrame >= 0.0f) physicsDt = (frame - scene_->physicsFrame) / kMmdFps;
    if (scene_->physicsFrame < 0.0f || physicsDt < 0.0f || physicsDt > 0.25f) {
        ch.ResetPhysics();
        physicsDt = 0.0f;
    }
    scene_->physicsFrame = frame;
    ch.UpdatePose(physicsDt);
    scene_->characterGpu->UpdateSkinning(slot, scene_->character->SkinMatrices());
    scene_->characterGpu->UpdateMorphs(slot, scene_->character->VertexMorphDeltas(),
                                       scene_->character->MorphVersion());
    for (size_t i = 0; i < scene_->stages.size(); ++i) {
        scene_->stageGpu[i]->UpdateSkinning(slot, scene_->stages[i]->SkinMatrices());
        scene_->stageGpu[i]->UpdateMorphs(slot, scene_->stages[i]->VertexMorphDeltas(),
                                          scene_->stages[i]->MorphVersion());
    }
}

void App::BuildFrameView(float frame, FrameView& view) {
    if (useMotionCamera_ && scene_->camera) {
        CameraPose pose = scene_->camera->Evaluate(frame);
        CameraMotion::ToView(pose, &view.camera.view, &view.camera.eye);
        view.camera.fovYRadians = DirectX::XMConvertToRadians(pose.fovDeg);
    } else {
        const FreeCamera& cam = freeCam_;
        DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
        float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw);
        float sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
        DirectX::XMVECTOR eye =
            DirectX::XMVectorAdd(target,
                                 DirectX::XMVectorScale(
                                     DirectX::XMVectorSet(sy * cp, sp, -cy * cp, 0), cam.distance));
        DirectX::XMMATRIX viewMat = DirectX::XMMatrixLookAtLH(
            eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
        DirectX::XMStoreFloat4x4(&view.camera.view, viewMat);
        DirectX::XMStoreFloat3(&view.camera.eye, eye);
        view.camera.fovYRadians = DirectX::XMConvertToRadians(cam.fovDeg);
    }
    for (const auto& g : scene_->stageGpu) view.models.push_back(g.get());
    if (scene_->characterGpu) view.models.push_back(scene_->characterGpu.get());
    view.studioFloor = scene_->stageGpu.empty();

    // Spotlights follow the performer (center bone when present).
    DirectX::XMFLOAT3 focus{0, 10, 0};
    if (scene_->character) {
        const int center = scene_->character->Model().FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
        if (center >= 0) focus = scene_->character->BoneWorldPosition(center);
    }
    const LightingPreset preset = screen_ == Screen::BenchRun ? LightingPreset::Studio : (LightingPreset)settings_.lighting;
    BuildLighting(preset, playTime_, focus, view.light);

    // DoF focus plane: the character's head (center bone + 8 without one), as view-space z.
    if (scene_->character) {
        const int head = scene_->character->Model().FindBone("\xE9\xA0\xAD");
        DirectX::XMFLOAT3 target = focus;
        if (head >= 0)
            target = scene_->character->BoneWorldPosition(head);
        else
            target.y += 8.0f;
        const DirectX::XMMATRIX viewMat = DirectX::XMLoadFloat4x4(&view.camera.view);
        const float z = DirectX::XMVectorGetZ(DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&target), viewMat));
        view.focusDistance = z > view.camera.nearZ ? z : 0.0f;
    }

    // Seeking (or a restart) breaks temporal history.
    const double step = std::fabs(playTime_ - lastRenderedTime_);
    view.cameraCut = lastRenderedTime_ < 0 || step > 0.25;
    lastRenderedTime_ = playTime_;
}

} // namespace mmdx
