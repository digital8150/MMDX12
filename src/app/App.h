#pragma once
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "app/Benchmark.h"
#include "app/RenderBench.h"
#include "app/LeaderboardClient.h"
#include "app/SceneLoader.h"
#include "app/Settings.h"
#include "app/ThumbnailCache.h"
#include "app/VideoEncoder.h"
#include "asset/AssetLibrary.h"
#include "audio/AudioPlayer.h"
#include "render/ColorLut.h"
#include "render/Dx12Context.h"
#include "render/Renderer.h"
#include <Windows.h>
#include <chrono>
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
//   --bench-spp <n>        render benchmark: override kRenderBenchSamples (testing; the result cannot be submitted)
//   --frames <n>           quit after n frames have been rendered in Play/BenchmarkRun state
//   --capture <file.png>   capture the final frame (with --frames) to a PNG
//   --width <w> --height <h>  initial window client size
//   --debug                enable the D3D12 debug layer
//   --free-camera          start with the free orbit camera
//   --screen <select|stages|songs|settings|video|bench|bench-gi> open that screen after the scan (UI testing;
//                          --frames counts all frames)
//   --lighting <0..3>      lighting preset for this run (Studio, Sunset, Concert, Night)
//   --quality <0..3>       graphics preset for this run (low, medium, high, ultra)
//   --render <raster|rt|pt>            render path for this run (override settings)
//   --upscaler <none|dlss|fsr|xess>    upscaler for this run (override settings)
//   --upscale-quality <native|quality|balanced|performance|ultra>  upscaler quality for this run
//   --no-physics           disable rigid-body physics for this run
//   --dof <0|1>  --volumetric <0|1>  --bloom-conv <0|1>   post effects for this run
//   --volumetric-density <0.25..4>  volumetric medium density for this run
//   --lut <substr|none>    colour LUT for this run (first LUT whose id or name contains substr)
//   --offline-still <file.png>   with --autoplay: render the --seek frame with the offline GI renderer,
//                                save it to the file and quit
//   --offline-video <file.mp4>   with --autoplay: offline video render, then quit
//   --offline-range <a> <b>      video range in song seconds (default: the whole song)
//   --offline-spp <n>            testing: cap samples per pixel (min samples = min(min, n))
//   --offline-size <w> <h>       testing: output size override for stills and videos
//   --offline-fps <n>            video frame rate for this run (24, 30, 60; default: the lobby dialog's)
//   --offline-bitrate <mbps>     video bit rate for this run
//   --offline-quality <0..3>     video quality preset for this run (draft, standard, high, best)
//   --offline-renderer <r>       video renderer for this run: raster, rt, pt or gi (default: the lobby dialog's)
//   --offline-probe              with --autoplay: sample render from the middle of the song (the lobby dialog's
//                                time measurement) with the dialog's settings, log "VIDEO PROBE", then quit
struct AppOptions {
    std::filesystem::path libraryOverride;
    std::string character, stage, song;
    bool autoplay = false;
    double seekSeconds = 0;
    std::string benchmarkCategory;
    int benchFrames = 0;
    int benchSpp = 0;          // --bench-spp (0 = kRenderBenchSamples)
    int quitAfterFrames = 0;
    std::filesystem::path capturePath;
    int width = 0, height = 0;
    bool debugLayer = false;
    bool freeCamera = false;
    float camera[6] = {};        // --camera tx,ty,tz,yawDeg,pitchDeg,dist (implies free camera)
    bool hasCamera = false;
    bool paused = false;        // --paused: start --autoplay paused (overlay captures)
    int lighting = -1;         // --lighting: override settings for this run
    int quality = -1;          // --quality: override settings for this run
    int renderPath = -1;       // --render <raster|rt|pt>: override settings for this run
    int upscaler = -1;         // --upscaler <none|dlss|fsr|xess>: override settings for this run
    int upscalerQuality = -1;  // --upscale-quality <native|quality|balanced|performance|ultra>: override settings for this run
    bool noPhysics = false;    // --no-physics: override settings for this run
    int dof = -1, volumetric = -1, bloomConv = -1;  // --dof/--volumetric/--bloom-conv <0|1>: override for this run
    float volumetricDensity = -1.0f;                 // --volumetric-density (< 0 = settings)
    std::string lut;           // --lut <substr|none>: override for this run (resolved against the LUT list)
    std::filesystem::path offlineStill, offlineVideo;  // --offline-still / --offline-video
    double offlineRange[2] = {-1.0, -1.0};             // --offline-range (seconds; < 0 = unset)
    int offlineSpp = 0;                                // --offline-spp (0 = default)
    int offlineSize[2] = {0, 0};                       // --offline-size (0 = default)
    int offlineFps = 0, offlineBitrate = 0;            // --offline-fps / --offline-bitrate (0 = settings)
    int offlineQuality = -1;                           // --offline-quality (-1 = settings)
    int offlineRenderer = -1;                          // --offline-renderer <raster|rt|pt|gi> (-1 = settings)
    bool offlineProbe = false;                         // --offline-probe: sample render for the time estimate, then quit
    int language = -1;  // --lang auto|ko|en|ja|zh for this run only (-1: keep the saved setting)
    std::string startScreen;  // --screen select|bench: open that screen after the scan; --frames then counts every frame  // --free-camera: start in the orbit camera instead of the VMD camera
};
AppOptions ParseCommandLine(int argc, wchar_t** argv);  // unknown args are logged and ignored

class App {
public:
    int Run(HINSTANCE instance, const AppOptions& options);  // returns process exit code

    // Window procedure trampoline (public so the static WndProc can call it).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Screen { Scanning, Select, Loading, Play, BenchLobby, BenchRun, BenchResult, Offline, BenchRender };
    enum class LoadTarget { Play, Benchmark, RenderBench, OfflineVideo };

    struct SceneRuntime {
        std::unique_ptr<ModelInstance> character;
        std::unique_ptr<GpuModel> characterGpu;
        std::vector<std::unique_ptr<ModelInstance>> stages;
        std::vector<std::unique_ptr<GpuModel>> stageGpu;
        std::shared_ptr<BoundMotion> motion;
        std::shared_ptr<CameraMotion> camera;
        float endFrame = 0;
        bool hasAudio = false;
        float physicsFrame = -1;  // motion frame of the last physics step (-1: reset on next update)
        // Render benchmark: performers after `character` (posed once by PoseRenderBench, never
        // updated per frame). Empty for normal playback.
        struct Performer {
            std::unique_ptr<ModelInstance> instance;
            std::unique_ptr<GpuModel> gpu;
            std::shared_ptr<BoundMotion> motion;
        };
        std::vector<Performer> extras;
    };

    // --- lifecycle (App.cpp)
    bool InitWindow(HINSTANCE instance, int width, int height);
    bool InitImGui();
    void ShutdownImGui();
    void MainLoop();
    void RenderFrame();          // one iteration: ImGui frame, update, record, present
    std::filesystem::path ResolveLibraryPath() const;
    void StartScan();
    // Per-file classification overrides chosen in the app (library_overrides.json next to the
    // ini, keyed by library folder). Changing them rescans the library.
    LibraryOverrides LoadLibraryOverrides() const;
    void SetLibraryOverride(const std::vector<std::filesystem::path>& files, AssetKind kind);  // Auto removes
    void ClearHiddenOverrides();
    void AssetContextMenu(const std::vector<std::filesystem::path>& files, bool character);
    void PollScan();
    void ApplyCommandLinePreselection();
    void ApplyRenderSettings();
    void ApplyGraphicsPreset(int preset);  // sets the effect toggles of AppSettings
    void RefreshColorLuts();            // rescans luts_, resolves options_.lut
    void ApplyColorLut();               // uploads settings_.colorLut when it differs from appliedLut_

    // --- scene (App.cpp)
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song);
    void PollLoad();
    bool BuildSceneRuntime(ScenePackage& pkg);  // GPU upload on the main thread
    void UnloadScene();                          // WaitForGpu, release runtime + audio
    void UpdateScene(float frame);               // evaluate motion, update poses, upload to GPU
    void BuildFrameView(float frame, FrameView& view);

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
    void DrawAppBar(int activeNav);  // 0 = library, 1 = benchmark
    void DrawScenePreview(const CharacterAsset* ch, const StageAsset* st, float x0, float y0, float x1, float y1,
                          float rounding);
    void StartBenchmarkLoad();       // the library-picked scene (PickBenchmarkScene)
    void SetPlaying(bool play);
    void FinishBenchmark();

    // --- render benchmark (AppRenderBench.cpp; overlay in UiBenchmark.cpp)
    void StartRenderBenchLoad();     // -> Loading (LoadTarget::RenderBench) -> BenchRender
    void PoseRenderBench();          // after BuildSceneRuntime: placement, physics pre-roll, final pose
    void RecordRenderBenchFrame(ID3D12GraphicsCommandList* cmd);  // instead of renderer_.Render (BenchRender)
    void AfterRenderBenchFrame();    // after ctx_.EndFrame (BenchRender): finish / cancel
    void FinishRenderBench(bool cancelled);
    void ReleaseRenderBenchImage();  // result texture (WaitForGpu first)
    void DrawRenderBenchOverlay();   // progress over the preview (UiBenchmark.cpp)
    // Lobby / loading card for the render benchmark scene: the last finished image when there is
    // one, else a dark studio with the three performers' thumbnails (UiBenchmark.cpp).
    void DrawRenderBenchPreview(float x0, float y0, float x1, float y1, float rounding);

    // --- thumbnails (AppThumbnails.cpp)
    std::filesystem::path ThumbnailCacheDir() const;
    bool RenderThumbnail(ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out);
    uint64_t CharacterThumb(int index);  // ImTextureID, 0 while not ready
    uint64_t StageThumb(int index);

    // --- free camera helpers (UiPlay.cpp)
    void UpdateFreeCamera();

    // --- offline GI render: stills and videos (UiOffline.cpp)
    enum class OfflineMode { None, Still, Video, Probe };
    void StartOfflineStill();   // Play -> Offline: pauses and renders the current frame
    // Play -> Offline: frames of [start, end) in settings_.video's format; fromLobby: the render was started
    // from the select screen, so it returns there (scene unloaded) when done
    void StartOfflineVideo(double startSeconds, double endSeconds, bool fromLobby = false, bool probe = false,
                           bool background = false);
    void StartVideoRenderLoad();    // Select -> Loading (LoadTarget::OfflineVideo) -> StartOfflineVideo
    // Time estimate by sample render, in the background while the render dialog is open: whenever the
    // dialog's settings (or the assets) form a combination that has not been measured, the scene is
    // loaded (once) and one sample frame from the middle of the song is rendered behind the UI; the
    // result is stored in settings_.videoProbes and reused for every later identical combination.
    // Called every frame (also when the dialog is closed, which stops the probe).
    void UpdateVideoProbe();
    void PollProbeScene();            // finishes the background scene load (any screen)
    void StopVideoProbe(bool unloadScene);   // cancels a running probe, forgets pending ones
    void CancelBackgroundProbe();            // ends a running background probe and restores the renderer
    void StartBackgroundProbe(uint64_t key);
    struct VideoProbeStatus {
        enum class Phase { Idle, Preparing, Measuring } phase = Phase::Idle;
        float fraction = 0;           // Measuring: 0..1 progress of the sample render
    };
    VideoProbeStatus ProbeStatus() const;
    void StartOfflineProbe();         // --offline-probe: the same measurement from a scene loaded for playing
    bool VideoRendererAvailable(VideoRenderer r) const;  // the dialog greys out unavailable renderers
    // Time to render the dialog's current selection: measured by a sample render when one exists for this
    // exact combination, else a rough estimate.
    struct VideoEstimate {
        int frames = 0;
        double secondsPerFrame = 0;
        double totalSeconds = 0;
        double fileGigabytes = 0;
        bool measured = false;
    };
    VideoEstimate EstimateVideoRender() const;
    uint64_t CurrentVideoProbeKey() const;
    // Real-time renderers: the renderer settings for a video frame of `cfg` (fixed resolution, no vsync).
    RenderSettings VideoRealtimeSettings(const VideoRenderConfig& cfg) const;
    void RecordRealtimeVideoFrame(ID3D12GraphicsCommandList* cmd);
    VideoRenderConfig ActiveVideoConfig() const;  // settings_.video with the --offline-* overrides
    void UpdateOffline();       // per frame, before UI drawing: CLI triggers, cancel handling
    // Instead of renderer_.Render while screen_ == Offline: poses the next image's frame and
    // begins it, or continues the current one.
    void RecordOfflineFrame(ID3D12GraphicsCommandList* cmd);
    void AfterOfflineFrame();   // after ctx_.EndFrame: collects a finished image (save / encode), advances
    void FinishOffline(bool cancelled);  // Offline -> Play (paused); closes the encoder; toast
    void DrawOfflineOverlay();  // progress panel over the preview (screen_ == Offline)
    void DrawVideoRenderDialog();  // video render settings + start (Select, UiVideoDialog.cpp)
    void DrawToast();           // completion / error toast (Select, Play)
    std::filesystem::path OfflineOutputDir(bool video) const;  // Pictures\MMDX12 or Videos\MMDX12
    DirectX::XMFLOAT3 CharacterCenter() const;  // world position of the character's center bone (teleport detection)
    void SaveOfflinePose();                     // current scene pose -> offline_.prevPose (video motion blur)
    void UploadOfflinePrevPose(uint64_t slot);  // offline_.prevPose -> the models' bone/morph ring entry `slot`

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
    ThumbnailCache thumbs_;
    bool thumbsClearPending_ = false;
    int libraryTab_ = 0;          // 0 characters, 1 stages, 2 songs
    bool advancedOpen_ = false;
    std::filesystem::path assetsDir_;
    std::vector<ColorLutEntry> luts_;   // built-in looks + .cube files (ListColorLuts)
    std::string appliedLut_ = "\x01";   // id currently uploaded to the renderer ("\x01" = nothing applied yet)

    // loading
    LoadTarget loadTarget_ = LoadTarget::Play;
    std::future<bool> loadFuture_;
    std::unique_ptr<ScenePackage> loadPackage_;
    LoadProgress loadProgress_;
    std::string loadError_;

    // scene / play
    std::unique_ptr<SceneRuntime> scene_;
    double playTime_ = 0;         // seconds into the song
    double lastRenderedTime_ = -1; // for camera-cut / seek detection
    bool playing_ = false;
    bool useMotionCamera_ = true;
    bool overlayVisible_ = true;
    double lastMouseMoveTime_ = 0;
    struct FreeCamera { DirectX::XMFLOAT3 target{0, 10, 0}; float yaw = 0, pitch = 0.1f, distance = 45; float fovDeg = 30; } freeCam_;
    POINT lastMouse_{};
    double timeSeconds_ = 0;      // wall clock since start (QPC)
    int framesInScene_ = 0;       // frames rendered in Play/BenchRun (for --frames)
    double gpuMsSum_ = 0.0;       // --frames: GPU time of the second half, logged at quit
    int gpuMsCount_ = 0;

    // offline render
    struct OfflineJob {
        OfflineMode mode = OfflineMode::None;
        bool beginPending = false;     // the next frame poses and begins a new image
        bool cancelRequested = false;
        bool fromCli = false;          // quit the app when the job ends
        bool fromLobby = false;        // video started from the select screen: back to it when done
        VideoRenderConfig video;       // format of a video job (fixed when it starts)
        std::filesystem::path output;  // .png (still) or .mp4 (video)
        double startWall = 0;          // timeSeconds_ when the job started
        double imageStartWall = 0;     // timeSeconds_ when the current image began
        double startSeconds = 0;       // song time of image 0
        int frame = 0, frameCount = 1; // images done / total (still: 1)
        double avgImageSeconds = 0;    // wall time per finished image (ETA)
        std::unique_ptr<VideoEncoder> encoder;
        bool background = false;       // a probe behind the select screen: screen_ stays Select, nothing is presented
        std::chrono::steady_clock::time_point imageStartClock;  // precise start of the current image
        double probeSum = 0;           // probe: seconds per image (render + encode) of the measured images
        int probeN = 0;
        uint64_t probeKey = 0;         // probe: VideoProbeKey of the measured combination
        double avgEncodeSeconds = 0;   // wall time of VideoEncoder::AddFrame per image (ETA)
        int iter = 0, iterCount = 1;   // real-time renderers: passes done / passes accumulated for this frame
        float frameMmd = 0;            // real-time renderers: MMD frame of the image in progress
        bool realtime = false;         // frames come from the real-time renderer (Renderer::Render)
        bool wasPlaying = false;       // still: started during playback (the last live frame opens the shutter)
        // video motion blur: the previous video frame's pose (character, then stages) and camera
        struct Pose {
            std::vector<DirectX::XMFLOAT4X4> skin;
            std::vector<DirectX::XMFLOAT3> morph;
            uint64_t morphVersion = 0;
        };
        std::vector<Pose> prevPose;
        CameraParams prevCamera;
        DirectX::XMFLOAT3 prevCenter{};   // character center bone at prevPose
    } offline_;
    CameraParams lastLiveCamera_;       // camera of the last real-time frame (still motion blur)
    bool haveLastLiveCamera_ = false;
    bool videoDialogOpen_ = false;      // the select screen's video render dialog
    // background probe scene (UpdateVideoProbe)
    struct BackgroundProbe {
        std::future<bool> loadFuture;
        std::unique_ptr<ScenePackage> package;
        LoadProgress progress;
        std::string error;
        bool loading = false;
        uint64_t loadingSceneKey = 0;  // scene being loaded
        uint64_t sceneKey = 0;         // scene held in scene_ for probing (0: none)
        uint64_t pendingKey = 0;       // combination waiting for the settings to settle
        double pendingSince = 0;       // timeSeconds_ it was first seen
        uint64_t failedKey = 0;        // a combination whose sample render failed (not retried)
    } bgProbe_;
    bool cliOfflineStarted_ = false;
    struct Toast {
        std::string title, detail;
        std::filesystem::path path;   // file to reveal in Explorer (empty = none)
        bool error = false;
        double until = 0;             // timeSeconds_ when it disappears
    } toast_;

    // benchmark
    int benchCategory_ = 0;       // index into kBenchCategories
    bool benchSubmittable_ = false;  // default workload (leaderboard submit allowed)
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

    // render benchmark run (AppRenderBench.cpp)
    struct RenderBenchRun {
        bool beginPending = false;     // the next BenchRender frame uploads the poses and begins the image
        bool cancelRequested = false;  // Esc / cancel button
        bool fromCli = false;          // started by --benchmark dx12-gi-render
        uint32_t width = kRenderBenchWidth, height = kRenderBenchHeight, samples = kRenderBenchSamples;
        bool official = true;          // default size and samples (submittable)
        RenderBenchCast cast;          // picked when the run starts
        int64_t startQpc = 0;          // QueryPerformanceCounter right after BeginOffline
        double elapsed = 0;            // seconds since start (updated every frame while rendering)
    } renderBench_;
    // finished image for the result screen (ImTextureID 0 = none) and where it was saved
    ComPtr<ID3D12Resource> renderBenchTex_;
    uint32_t renderBenchSrv_ = DescriptorHeap::kInvalid;
    uint64_t renderBenchImage_ = 0;
    uint32_t renderBenchImageW_ = 0, renderBenchImageH_ = 0;
    std::filesystem::path renderBenchSaved_;
};

} // namespace mmdx
