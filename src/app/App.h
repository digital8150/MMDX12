#pragma once
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "app/Benchmark.h"
#include "app/LeaderboardClient.h"
#include "app/SceneLoader.h"
#include "app/Settings.h"
#include "asset/AssetLibrary.h"
#include "audio/AudioPlayer.h"
#include "render/Dx12Context.h"
#include "render/Renderer.h"
#include <Windows.h>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mmdx {

// Command line (all optional). Used for normal launches and for automated verification.
//   --library <dir>        override library folder for this run
//   --character <substr>   preselect: first character whose id or displayName contains substr (case-insensitive)
//   --stage <substr>       preselect stage ("none" = no stage)
//   --song <substr>        preselect song
//   --autoplay             after the scan, start playing the preselection immediately
//   --seek <seconds>       start position for --autoplay
//   --benchmark <catId>    after the scan, start the benchmark run for that category immediately
//   --bench-frames <n>     override kBenchMeasuredFrames (testing)
//   --frames <n>           quit after n frames have been rendered in Play/BenchmarkRun state
//   --capture <file.png>   capture the final frame (with --frames) to a PNG
//   --width <w> --height <h>  initial window client size
//   --debug                enable the D3D12 debug layer
//   --free-camera          start with the free orbit camera
//   --screen <select|bench> open that screen after the scan (UI testing; --frames counts all frames)
struct AppOptions {
    std::filesystem::path libraryOverride;
    std::string character, stage, song;
    bool autoplay = false;
    double seekSeconds = 0;
    std::string benchmarkCategory;
    int benchFrames = 0;
    int quitAfterFrames = 0;
    std::filesystem::path capturePath;
    int width = 0, height = 0;
    bool debugLayer = false;
    bool freeCamera = false;
    std::string startScreen;  // --screen select|bench: open that screen after the scan; --frames then counts every frame  // --free-camera: start in the orbit camera instead of the VMD camera
};
AppOptions ParseCommandLine(int argc, wchar_t** argv);  // unknown args are logged and ignored

class App {
public:
    int Run(HINSTANCE instance, const AppOptions& options);  // returns process exit code

    // Window procedure trampoline (public so the static WndProc can call it).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Screen { Scanning, Select, Loading, Play, BenchLobby, BenchRun, BenchResult };
    enum class LoadTarget { Play, Benchmark };

    struct SceneRuntime {
        std::unique_ptr<ModelInstance> character;
        std::unique_ptr<GpuModel> characterGpu;
        std::vector<std::unique_ptr<ModelInstance>> stages;
        std::vector<std::unique_ptr<GpuModel>> stageGpu;
        std::shared_ptr<BoundMotion> motion;
        std::shared_ptr<CameraMotion> camera;
        float endFrame = 0;
        bool hasAudio = false;
    };

    // --- lifecycle (App.cpp)
    bool InitWindow(HINSTANCE instance, int width, int height);
    bool InitImGui();
    void ShutdownImGui();
    void MainLoop();
    void RenderFrame();          // one iteration: ImGui frame, update, record, present
    std::filesystem::path ResolveLibraryPath() const;
    void StartScan();
    void PollScan();
    void ApplyCommandLinePreselection();
    void ApplyRenderSettings();

    // --- scene (App.cpp)
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song);
    void PollLoad();
    bool BuildSceneRuntime(ScenePackage& pkg);  // GPU upload on the main thread
    void UnloadScene();                          // WaitForGpu, release runtime + audio
    void UpdateScene(float frame);               // evaluate motion, update poses, upload to GPU
    void BuildFrameView(float frame, FrameView& view) const;

    // --- screens (UiSelect.cpp / UiPlay.cpp / UiBenchmark.cpp)
    void DrawScanning();
    void DrawSelect();
    void DrawLoading();
    void UpdatePlay(double dt);
    void DrawPlayOverlay();
    void DrawBenchLobby();
    void UpdateBenchRun();
    void DrawBenchRunOverlay();
    void DrawBenchResult();
    void RefreshLeaderboard();
    void FinishBenchmark();

    // --- free camera helpers (UiPlay.cpp)
    void UpdateFreeCamera();

    AppOptions options_;
    AppSettings settings_;
    std::filesystem::path settingsPath_;
    HWND hwnd_ = nullptr;
    bool running_ = true;
    bool minimized_ = false;
    Screen screen_ = Screen::Scanning;

    Dx12Context ctx_;
    Renderer renderer_;
    AudioPlayer audio_;

    // library
    LibraryScanResult library_;
    std::future<LibraryScanResult> scanFuture_;
    ScanProgress scanProgress_;
    int selCharacter_ = -1, selStage_ = -1, selSong_ = -1;  // indices into library_ vectors; stage -1 = none
    char filterCharacter_[128] = {}, filterStage_[128] = {}, filterSong_[128] = {};
    char libraryPathEdit_[512] = {};

    // loading
    LoadTarget loadTarget_ = LoadTarget::Play;
    std::future<bool> loadFuture_;
    std::unique_ptr<ScenePackage> loadPackage_;
    LoadProgress loadProgress_;
    std::string loadError_;

    // scene / play
    std::unique_ptr<SceneRuntime> scene_;
    double playTime_ = 0;         // seconds into the song
    bool playing_ = false;
    bool useMotionCamera_ = true;
    bool overlayVisible_ = true;
    double lastMouseMoveTime_ = 0;
    struct FreeCamera { DirectX::XMFLOAT3 target{0, 10, 0}; float yaw = 0, pitch = 0.1f, distance = 45; float fovDeg = 30; } freeCam_;
    POINT lastMouse_{};
    double timeSeconds_ = 0;      // wall clock since start (QPC)
    int framesInScene_ = 0;       // frames rendered in Play/BenchRun (for --frames)

    // benchmark
    int benchCategory_ = 0;       // index into kBenchCategories
    bool benchOfficial_ = false;  // preset assets found
    std::vector<float> benchFrameTimes_;
    int benchFrameCounter_ = 0;
    int64_t benchLastQpc_ = 0;
    double benchStartTime_ = 0;
    BenchmarkResult benchResult_;
    std::future<LeaderboardPage> leaderboardFuture_;
    std::optional<LeaderboardPage> leaderboard_;
    std::string leaderboardError_;
    std::future<SubmitResponse> submitFuture_;
    std::optional<SubmitResponse> submitResult_;
    char nicknameEdit_[64] = {};
};

} // namespace mmdx
