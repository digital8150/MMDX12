#include "app/App.h"

#include <ShlObj.h>
#include <directx/d3dx12.h>

#include <cmath>
#include <cstdio>
#include <cstring>

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
        } else if (arg == L"--free-camera") {
            opt.freeCamera = true;
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

    std::strncpy(nicknameEdit_, settings_.nickname.c_str(), sizeof(nicknameEdit_) - 1);
    nicknameEdit_[sizeof(nicknameEdit_) - 1] = '\0';
    std::filesystem::path libPath = ResolveLibraryPath();
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
    ShutdownImGui();
    renderer_.Shutdown();

    RECT crc;
    if (GetClientRect(hwnd_, &crc) && !IsIconic(hwnd_)) {
        settings_.windowWidth = crc.right - crc.left;
        settings_.windowHeight = crc.bottom - crc.top;
    }
    settings_.volume = audio_.Volume();
    settings_.nickname = nicknameEdit_;
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

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"MMDX12 — MikuMikuDance DX12", WS_OVERLAPPEDWINDOW,
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

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8;
    style.FrameRounding = 5;
    style.GrabRounding = 5;
    const ImVec4 accent(0.22f, 0.77f, 0.73f, 1.0f); // Miku teal #39C5BB
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.77f, 0.73f, 0.45f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.95f);
    style.Colors[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.95f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(accent.x, accent.y, accent.z, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(accent.x, accent.y, accent.z, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.94f);

    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;

    // Fonts: 18 px base, Korean-capable, with CJK fallback merges.
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 18.0f);
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.MergeMode = true;
    if (std::filesystem::exists(L"C:\\Windows\\Fonts\\YuGothM.ttc"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\YuGothM.ttc", 18.0f, &cfg);
    else if (std::filesystem::exists(L"C:\\Windows\\Fonts\\msgothic.ttc"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msgothic.ttc", 18.0f, &cfg);
    if (std::filesystem::exists(L"C:\\Windows\\Fonts\\msyh.ttc"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, &cfg);

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

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    PollScan();
    PollLoad();

    switch (screen_) {
    case Screen::Scanning: DrawScanning(); break;
    case Screen::Select: DrawSelect(); break;
    case Screen::Loading: DrawLoading(); break;
    case Screen::Play:
        UpdatePlay(dt);
        DrawPlayOverlay();
        break;
    case Screen::BenchLobby: DrawBenchLobby(); break;
    case Screen::BenchRun: DrawBenchRunOverlay(); break;
    case Screen::BenchResult: DrawBenchResult(); break;
    }

    ImGui::Render();

    if (!running_) return;

    ID3D12GraphicsCommandList* cmd = ctx_.BeginFrame();
    FrameView view;
    const bool inScene = scene_ && (screen_ == Screen::Play || screen_ == Screen::BenchRun);
    if (inScene) {
        const float frame = (float)(playTime_ * kMmdFps);
        UpdateScene(frame);
        BuildFrameView(frame, view);
    }
    renderer_.Render(cmd, view);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

    bool quit = false;
    if (inScene || !options_.startScreen.empty()) {
        ++framesInScene_;
        if (options_.quitAfterFrames > 0 && framesInScene_ >= options_.quitAfterFrames) {
            if (!options_.capturePath.empty()) ctx_.RequestCapture(options_.capturePath);
            quit = true;
        }
    }
    ctx_.EndFrame(renderer_.Settings().vsync);

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

void App::ApplyRenderSettings() {
    RenderSettings rs = renderer_.Settings();
    rs.msaaSamples = settings_.msaa;
    rs.renderScale = settings_.renderScale;
    rs.vsync = settings_.vsync;
    rs.drawEdges = settings_.drawEdges;
    rs.fixedResolution = false;
    renderer_.SetSettings(rs);
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
        if (scene_->hasAudio) {
            audio_.SetMuted(false);
            audio_.Seek(playTime_);
            audio_.Play();
        }
        framesInScene_ = 0;
        screen_ = Screen::Play;
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
        auto gpu = renderer_.CreateModel(batch, *part.pmx, part.textures);
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
    ctx_.WaitForGpu();
    scene_.reset();
    audio_.Unload();
    playing_ = false;
}

void App::UpdateScene(float frame) {
    const uint32_t slot = ctx_.FrameSlot();
    if (scene_->motion) {
        scene_->motion->Evaluate(frame, *scene_->character);
    } else {
        scene_->character->ResetPose();
    }
    scene_->character->UpdatePose();
    scene_->characterGpu->UpdateSkinning(slot, scene_->character->SkinMatrices());
    scene_->characterGpu->UpdateMorphs(slot, scene_->character->VertexMorphDeltas(),
                                       scene_->character->MorphVersion());
    for (size_t i = 0; i < scene_->stages.size(); ++i) {
        scene_->stageGpu[i]->UpdateSkinning(slot, scene_->stages[i]->SkinMatrices());
        scene_->stageGpu[i]->UpdateMorphs(slot, scene_->stages[i]->VertexMorphDeltas(),
                                          scene_->stages[i]->MorphVersion());
    }
}

void App::BuildFrameView(float frame, FrameView& view) const {
    (void)frame;
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
}

} // namespace mmdx
