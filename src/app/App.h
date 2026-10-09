#pragma once
#include <atomic>
#include <functional>
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "app/Benchmark.h"
#include "app/Lighting.h"
#include "app/McpServer.h"
#include "app/RenderBench.h"
#include "app/LeaderboardClient.h"
#include "app/SceneLoader.h"
#include "app/Settings.h"
#include "app/ShaderPackStore.h"
#include "app/ThumbnailCache.h"
#include "app/Updater.h"
#include "app/VideoEncoder.h"
#include "asset/AssetLibrary.h"
#include "audio/AudioPlayer.h"
#include "render/ColorLut.h"
#include "render/Dx12Context.h"
#include "render/Renderer.h"
#include "studio/Gizmo.h"
#include "studio/StudioDoc.h"
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
//   --ui-script <file>     scripted ImGui input for UI tests (UiScript.cpp): lines "<frame> <command> [args]",
//                          frame = frames counted like --frames; commands: move x y | down [l|r|m] | up [l|r|m] |
//                          click x y | dblclick x y | wheel dy | key <ImGuiKey name> [ctrl] [shift] | text <chars> |
//                          mod <ctrl|shift> <down|up> (held across mouse steps) |
//                          capture <file.png> | log <text> |
//                          studiostate (logs the Studio's frame, selection, rows, range, key counts, undo) |
//                          studioexport <file.vmd> | studioimport <file.vmd> (the Studio's VMD export/import
//                          without the dialogs) | studiovpdexport/studiovpdimport <file.vpd> (pose files, current
//                          scope) | studiobone <name> (clicks that bone's joint in the viewport) |
//                          studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [steps] (drags that gizmo part by dx,dy px).
//                          studiostate also logs a STUDIOPOSE line (active bone + value, pose layer, tool) and a
//                          STUDIOPROJ line + one STUDIOMODEL line per model (kind, keys, attach, root position).
//                          updatecheck (synchronous update-feed check, logs UPDATECHECK) |
//                          updateinstall (stage the feed's update; the app exits when staged)
//                          Projects / models without dialogs (UiStudioProject.cpp): studionew | studiosave <file> |
//                          studioopen <file> | studioautosave (writes the recovery file now) | studioadd
//                          <character|stage|prop> <file> | studioaddlib <character|stage> <substr> | studiosong <substr>
//                          (library song onto the selected model + camera + audio) | studioaudio <file|none> [offset s] |
//                          studioselect <model index|-1> | studioremove <model index> | studiorename <text> |
//                          studioattach <parent index|-1> <bone|-> <tx ty tz rx ry rz s> (selected prop).
//                          Coordinates in window pixels.
//   --width <w> --height <h>  initial window client size
//   --debug                enable the D3D12 debug layer
//   --free-camera          start with the free orbit camera
//   --screen <select|stages|songs|settings|video|bench|bench-gi|studio> open that screen after the scan (UI testing;
//                          --frames counts all frames; studio: opens --character/--stage/--song in the Studio at
//                          --seek and counts frames from the Studio on; without --character: a new empty project)
//   --project <file.mmdxproj>  open that Studio project after the scan (implies --screen studio)
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
//                                Studio (--project / --screen studio): --offline-video renders the project once it is
//                                open (range: --offline-range in timeline seconds, else the timeline range, else the
//                                whole project; motion camera; project audio) and quits; --offline-still renders the
//                                --seek frame (GI).
//   --ui-scale <f>               UI scale for this run instead of the monitor's DPI scale (1.5 = 144 dpi; captures)
//   --update-feed <url-or-file>  auto-update feed override: an https URL, a file: URL or a local
//                                latest.json path (testing); the zip url may also be file:/local
//   --apply-update               internal: apply a staged update with no window, then exit
//   --apply-wait <pid>           internal: process id the applier waits for before the swap
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
    std::filesystem::path uiScript;
    int width = 0, height = 0;
    bool debugLayer = false;
    bool freeCamera = false;
    float camera[6] = {};        // --camera tx,ty,tz,yawDeg,pitchDeg,dist (implies free camera)
    bool hasCamera = false;
    bool paused = false;        // --paused: start --autoplay paused (overlay captures)
    int lighting = -1;         // --lighting: override settings for this run
    int quality = -1;          // --quality: override settings for this run
    int renderPath = -1;       // --render <raster|rt|pt>: override settings for this run
    int shading = -1;          // --shading <lit|unlit|wire>: raster ViewShading override for this run (hidden)
    int upscaler = -1;         // --upscaler <none|dlss|fsr|xess>: override settings for this run
    int upscalerQuality = -1;  // --upscale-quality <native|quality|balanced|performance|ultra>: override settings for this run
    bool noPhysics = false;    // --no-physics: override settings for this run
    std::string shaderPack;    // --shader-pack <id|none>: the character's shader pack for this run (play and studio)
    bool shaderPackSet = false;
    std::string packIndex;     // --pack-index <url|path>: the online shader pack gallery index (testing)
    std::vector<EffectStackEntry> effectStack;  // --effect <id>[,<id>...] / none: the effect stack, this run
    bool effectSet = false;
    int dof = -1, volumetric = -1, bloomConv = -1;  // --dof/--volumetric/--bloom-conv <0|1>: override for this run
    int motionLighting = -1;   // --motion-lighting <0|1>: camera VMD light/self-shadow tracks in play mode, this run
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
    std::filesystem::path project;  // --project
    float uiScale = 0.0f;           // --ui-scale (0 = the window's DPI scale)
    std::string startScreen;  // --screen select|bench: open that screen after the scan; --frames then counts every frame  // --free-camera: start in the orbit camera instead of the VMD camera
    std::string updateFeed;   // --update-feed: feed URL / file override (empty: the default feed)
    bool applyUpdate = false; // --apply-update (internal): apply a staged update, then exit
    uint32_t applyWaitPid = 0;        // --apply-wait (internal): pid the applier waits for
    int mcp = -1;                     // --mcp (1), --no-mcp (0), auto (-1)
};
AppOptions ParseCommandLine(int argc, wchar_t** argv);  // unknown args are logged and ignored

// Folder picker for a pack's user texture folder (FOS_PICKFOLDERS): runs on its own STA thread,
// keeping the owner's messages pumped. Defined in UiShaderPack.cpp, also used by UiShaders.cpp.
std::filesystem::path PickPackTextureFolder(HWND owner);

// Popup width for the pack parameter grid (columns from the pack's param count), clamped to the viewport.
// Defined in UiShaderPack.cpp, also used by UiPlay.cpp.
float PackParamsPopupWidth(const ShaderChoice& choice);

class App {
public:
    int Run(HINSTANCE instance, const AppOptions& options);  // returns process exit code

    // Window procedure trampoline (public so the static WndProc can call it).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Screen { Scanning, Select, Loading, Play, BenchLobby, BenchRun, BenchResult, Offline, BenchRender, Studio, Shaders };
    enum class LoadTarget { Play, Benchmark, RenderBench, OfflineVideo, Studio };

    struct SceneRuntime {
        std::unique_ptr<ModelInstance> character;
        std::string characterId;  // library id, key of the saved display scale
        std::unique_ptr<GpuModel> characterGpu;
        std::vector<std::unique_ptr<ModelInstance>> stages;
        std::vector<std::unique_ptr<GpuModel>> stageGpu;
        std::shared_ptr<BoundMotion> motion;
        std::shared_ptr<CameraMotion> camera;
        // The camera VMD's light / self-shadow tracks (empty when absent or only MMD's defaults); used when
        // AppSettings::motionLighting is on.
        std::vector<studio::LightKf> lightTrack;
        std::vector<studio::ShadowKf> shadowTrack;
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

    // --- studio (UiStudio.cpp): the editor screen for keyframe work on a multi-model scene
    void StartStudioLoad(const CharacterAsset* ch, const StageAsset* st, const SongAsset* song);  // -> Loading -> Studio
    bool FinishStudioLoad();          // main thread: GPU upload of studioPackage_, enters Screen::Studio
    void LeaveStudio();               // -> Select (unloads)
    void UpdateStudio(double dt);     // input, playback clock
    void DrawStudio();                // panels; sets the renderer's viewport to the free area
    void UpdateStudioScene();         // evaluate motions at the current frame, upload poses
    void BuildStudioFrameView(FrameView& view);
    void StudioSeek(double seconds);
    void StudioSetPlaying(bool play);
    void StudioRebuildRows();
    void StudioHandleTimeline(const studio::TimelineEvents& ev);
    void StudioPushTrackEdit(const char* name, const std::vector<studio::TrackState>& before);  // after = current state
    std::vector<studio::TrackState> StudioCaptureSelectedTracks();
    bool StudioTrackOfRow(uint64_t row, studio::RowKind& kind, std::string& name) const;
    void StudioInsertKeys(const std::vector<uint64_t>& rows, int frame);  // key the current value of each row
    std::vector<int> StudioRowFrames(uint64_t row) const;                 // key frames of a bone/morph/camera row
    std::vector<uint64_t> StudioExpandRows(const std::set<uint64_t>& rows) const;  // groups -> their key rows
    std::vector<uint64_t> StudioTargetRows() const;  // picked rows + rows of selected keys (expanded)
    void StudioSelectAll();
    void StudioDeleteSelected(const char* undoName);
    void StudioCopySelected();
    void StudioPaste(bool curvesOnly);               // at the current frame; curvesOnly: interpolation onto existing keys
    void StudioRegisterKeys();                       // I: key the target rows at the current frame
    void StudioShiftFrames(bool remove);             // MMD frame insert/delete (range length or 1 frame)
    void StudioCopyCurve();                          // inspector: interpolation of the first selected key
    void StudioPasteCurve();                         // onto every selected key of the same kind
    void StudioJumpKey(int dir);                     // previous/next key of the shown rows
    void StudioImportVmd();                                   // open dialog, then StudioImportVmdFrom
    void StudioImportVmdFrom(const std::filesystem::path& path);  // merge into the selected model (camera VMDs: camera)
    void StudioExportVmd();                                   // save dialog, then StudioExportVmdTo
    bool StudioExportVmdTo(const std::filesystem::path& path);  // selected model's motion (or the camera)
    void DrawStudioTopBar(float x0, float y0, float x1, float y1);
    void DrawStudioOutliner(float x0, float y0, float x1, float y1);
    void DrawStudioInspector(float x0, float y0, float x1, float y1);
    void DrawStudioTimeline(float x0, float y0, float x1, float y1);
    void DrawStudioViewport(float x0, float y0, float x1, float y1);
    void StudioCamera(CameraParams& cam) const;      // the view the viewport shows (motion or free camera)
    void StudioEnter(std::unique_ptr<studio::StudioDoc> doc);  // common tail of every way into the studio
    void StudioUpdateModel(studio::StudioModel& m, uint64_t slot, float frame, float physicsDt, bool resetPhysics);
    // The sun's light as the viewport shows it at `frame` (fractional): the camera VMD's light track while the sun is
    // linked to it, else the sun's own keyed values; black with the default direction without a sun.
    studio::LightKf StudioSunLight(float frame) const;
    // --- camera / light / self-shadow (UiStudioCamera.cpp): inspector panel, key fields, camera path, render tracks
    bool StudioCameraPerspective(float frame) const;  // the camera key's perspective switch in effect at `frame`
    void StudioOrthoCamera(CameraPose& pose, CameraParams& camera) const;  // "perspective off" view (approximation)
    void StudioApplyLightTracks(FrameView& view) const;
    // A VMD light / self-shadow track at `frame` -> the frame's key light (direction, colour) and shadows; an empty
    // track leaves the view as it is (UiStudioCamera.cpp). Studio tracks, and in play mode the camera VMD's tracks.
    static void ApplyLightShadowTracks(const std::vector<studio::LightKf>& light,
                                       const std::vector<studio::ShadowKf>& shadow, float frame, FrameView& view);  // light / self-shadow tracks -> the frame's light and shadows
    studio::LightKf StudioCurrentLight() const;      // the sun's light in effect at the playhead (StudioSunLight)
    void StudioKeyCameraFromView();                  // camera key at the current frame from the view being shown
    // The part of the viewport the 3D image fills: the whole rect, or its 16:9 fit when the motion camera is the view.
    void StudioRenderRect(float x0, float y0, float x1, float y1, float out[4]) const;
    void DrawStudioPlacePanel(float w);              // character transform (position / rotation / scale)
    void StudioCommitPlace(uint32_t uid, const studio::PropAttach& before, const studio::PropAttach& after);  // one undo step
    bool studioPlaceDragging_ = false;               // a transform field / gizmo drag is in progress ...
    studio::PropAttach studioPlaceBefore_;                   // ... that started from this placement
    void DrawStudioFrameMask(float x0, float y0, float x1, float y1);  // dims outside the render frame + guides
    struct FreeCamera;                               // orbit camera (defined below)
    void StudioViewportNavigate(bool hovered, bool active);  // viewport mouse / fly navigation (free or possessed camera)
    static FreeCamera FreeFromKey(const studio::CameraKf& k);  // orbit form of a camera key
    FreeCamera StudioViewedFree(int frame) const;    // the camera as shown now, in orbit form (target, yaw, pitch, distance)
    // possession: store `cam` as the camera key at the playhead. base: the key to start from when none exists there
    void StudioWriteCamera(const FreeCamera& cam, const studio::CameraKf* base = nullptr, bool takeView = true);
    void StudioNavEditTick();                        // closes the undo step of a possession gesture when it went quiet
    void StudioPossess(bool on);
    void StudioViewportCameraHandles(bool hovered);  // eye / target handles of the camera (free view, camera selected)
    std::string iniPath_;                            // ImGui ini (panel layout); must outlive the context
    int studioActiveView_ = 0;                       // quad view: 0 perspective, 1 top, 2 front, 3 left (the view under the mouse)
    void StudioDrawPoseOverlay(const studio::ViewProj& vp, float x0, float y0, float x1, float y1);  // bones + gizmo of another view
    void StudioOrthoNavigate(const studio::ViewProj& vp, bool hovered, bool active);  // pan / zoom of the orthographic views
    void StudioAddQuadViews(FrameView& view) const;  // the quad view's extra views for the live viewport frame
    bool studioResetLayout_ = false;                 // rebuild the default panel arrangement next frame
    int studioNavIdle_ = 0;                          // frames since the last possessed edit (undo grouping)
    bool studioNavWrote_ = false;                    // a possessed edit was written this frame
    int studioCamHandle_ = 0;                        // 0 none, 1 eye, 2 target
    studio::GizmoDrag studioCamDrag_;
    studio::GizmoFrame studioCamFrame_;
    studio::GizmoPart studioCamHot_ = studio::GizmoPart::None;
    void StudioTakeFreeCamera();                    // leave the motion camera at its current view (viewport navigation)
    void StudioFocusSelection();                     // F: frame the picked bone / selected model
    float studioFlyMul_ = 1.0f;                      // RMB + WASD fly speed multiplier (wheel while flying)
    studio::CameraKf StudioViewedCamera(int frame) const;  // the camera as shown now (free view or motion camera) as a key at `frame`
    void StudioBeginKeyEdit(studio::RowKind kind);   // inspector key fields: one undo step per edit
    void StudioEndKeyEdit();
    void DrawStudioCameraPanel(float w);             // view / light / shadow sections of the camera inspector
    // true: no curve editor follows. live (camera only): the playhead's fields, shown with or without a key
    bool DrawStudioCameraKeyFields(float w, studio::RowKind kind, int frame, bool live = false);
    // --- scene light editing (UiStudioLight.cpp)
    void StudioSelectLight(uint32_t uid);
    void StudioAddLight(studio::LightKind kind);
    void StudioDeleteLight(uint32_t uid);
    void StudioApplyLightPreset(int presetIndex);
    void StudioViewportLightHandles(bool hovered);
    bool StudioScriptLightGizmoPoint(const std::string& part, ImVec2& out) const;
    LightAnchors StudioBuildLightAnchors() const;
    void DrawStudioLightOutliner();
    void DrawStudioLightInspector(float w);
    void DrawStudioLightPresetConfirm();
    void StudioBeginLightEdit(uint32_t uid, bool keyEdit);
    void StudioEndLightEdit(uint32_t uid);

    void StudioUpdateCameraPath();                   // samples the motion camera (cached per cameraVersion)
    void DrawStudioCameraPath(float x0, float y0, float x1, float y1);
    bool StudioPickCameraKey(ImVec2 mouse);          // click on a key dot of the path: select it and seek
    void StudioCameraPathWindow(int& lo, int& hi) const;  // frames of the path drawn: the range, else now +-3 s
    // --- pose editing (UiStudioPose.cpp): bone overlay, picking, gizmo, pose layer, morph panel, VPD, mirror
    studio::StudioModel* StudioPoseModel();          // the selected model if it is a character (pose editable)
    void StudioSelectBone(int bone, bool toggle);    // viewport pick / bone row click: selection sync (-1 clears)
    std::vector<studio::PoseBone> StudioCurrentPose(const studio::StudioModel& m) const;  // effective anim values
    void StudioSetPose(const char* undoName, const studio::PoseLayer& before);  // push the selected model's layer edit
    // keys for the edited bones/morphs (all: every listed bone). layerBefore: the layer an undo restores (auto-key)
    void StudioRegisterPose(bool allBones, const studio::PoseLayer* layerBefore = nullptr, const char* undoName = nullptr);
    void StudioResetPose();                          // drop the unregistered edits
    void StudioMirrorPose();                         // left/right mirror (scope: whole model or selected bones)
    void StudioImportVpd();
    void StudioImportVpdFrom(const std::filesystem::path& path);
    void StudioExportVpd();
    bool StudioExportVpdTo(const std::filesystem::path& path);
    void StudioApplyPose(studio::StudioModel& m);    // writes the pose layer over the evaluated motion (UpdateStudioScene)
    bool StudioGizmoFrameOf(const studio::StudioModel& m, int bone, studio::GizmoFrame& f, studio::GizmoMode& mode) const;
    void StudioViewportPose(float x0, float y0, float x1, float y1, bool hovered, bool& consumed);  // overlay + input
    void StudioViewportToolbar(float x, float y);
    void DrawStudioBoneTab(float w);
    void DrawStudioMorphTab(float w);
    bool StudioScriptGizmoPoint(int part, ImVec2& out) const;  // ui-script: a screen point on a gizmo part

    // --- projects, adding/removing models, props, audio, autosave (UiStudioProject.cpp)
    enum class StudioAction { None, Leave, New, Open, OpenFile };  // what the unsaved-changes prompt guards
    void StartStudioEmpty();                          // new empty project: camera/light tracks only (no loading)
    void StartStudioProjectLoad(const std::filesystem::path& file, bool recovery);  // -> Loading -> Studio
    void StudioRequest(StudioAction a, const std::filesystem::path& file = {});  // prompts first when dirty
    void StudioRunAction(StudioAction a, const std::filesystem::path& file);
    bool StudioSave(bool saveAs);                     // dialog when untitled or saveAs; false: cancelled / failed
    bool StudioSaveTo(const std::filesystem::path& file);
    studio::ProjectData StudioProjectData() const;    // snapshot of the document (copies the motions)
    std::filesystem::path StudioRecoveryFile() const; // <exe>/recovery/autosave.mmdxproj
    void StudioAutosave(bool force, bool wait);       // timer: writes the recovery file on a worker thread
    void StudioDiscardRecovery();                     // waits for a running autosave, deletes the recovery files
    void StudioAddModelFile(studio::ModelKind kind, const std::filesystem::path& file);  // async
    void StudioAddModelDialog(studio::ModelKind kind);
    void StudioAddLibraryCharacter(int index);
    void StudioAddLibraryStage(int index);
    void StudioApplyLibrarySong(int index);           // dance -> selected character, camera, audio when none
    bool StudioSetAudio(const std::filesystem::path& file);  // empty: remove
    void StudioAudioDialog();
    void StudioPollJobs();                            // finished loads: GPU upload + append (main thread)
    void StudioRemoveModel(int index);                // waits for the GPU; clears the undo history
    void StudioSelectModel(int index);                // -1 camera
    DirectX::XMFLOAT4X4 StudioPropRoot(const studio::StudioModel& m) const;  // world matrix of a prop's origin
    void StudioSeekAudio();                           // audio cursor <- timeline time (audio offset)
    void DrawStudioAddMenu();                         // the outliner's "+" popup (library / file)
    void DrawStudioModelMenu(int index);              // outliner row context menu
    void DrawStudioPropPanel(float w);                // inspector: prop parent / bone / offset
    void DrawStudioUnsavedPrompt();
    void DrawStudioProjectMenu();                     // top bar file menu (new / open / recent / save as)
    void DrawRecoveryPrompt();                        // select screen: offer the autosave of a crashed session

    // --- rendering the project, shortcut help (UiStudioRender.cpp)
    // Video (settings_.video through the render dialog) or GI still of the current frame. The video covers
    // StudioRenderRange through the motion camera with the project's audio; the studio comes back as it was.
    void StartStudioRender(bool video);
    void StudioRenderRange(double& startSeconds, double& endSeconds) const;  // CLI range, else timeline range / whole
    void StudioPoseForRender(float frame);            // offline frames: every model at `frame` (physics steps forward)
    void StudioRestoreAfterRender();                  // FinishOffline: time, camera mode, physics back to the editor's
    DirectX::XMFLOAT3 StudioPerformerCenter() const;  // center bone of the first character (teleport detection)
    void DrawStudioRenderMenu();                      // top bar popup: video / still
    void DrawStudioEffectsMenu();                     // top bar popup: screen effects + effect stack
    void DrawStudioHelp();                            // shortcut overlay (? key / top bar button)
    bool StudioModal() const { return videoDialogOpen_ || studioHelpOpen_ || studioLeaveConfirm_ || studioLightPresetPending_ >= 0; }

    // --- scripted UI input for tests (UiScript.cpp) and MCP
    void PumpUiScript();  // before ImGui::NewFrame: feeds the events due at framesInScene_
    void ScheduleUiScriptStep(int frame, std::string cmd, std::vector<std::string> args);
    using ScheduleFunc = std::function<void(int frame, std::string cmd, std::vector<std::string> args)>;
    bool ExecuteUiCommand(const std::string& cmd, const std::vector<std::string>& args,
                          const ScheduleFunc& schedule = nullptr, std::string* outError = nullptr);

    // --- MCP server and control (AppMcp.cpp)
    void InitMcp();              // at startup: --mcp, --no-mcp, else the setting (not in headless runs)
    void StartMcpServer();       // the settings switch
    void ShutdownMcp();          // also fails the commands still waiting for a later frame
    void PumpMcp(bool minimized);  // main thread, before ImGui::NewFrame (and while minimized)
    void ExecuteMcp(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Leaves the current screen for a navigation tool like the UI's back buttons do. False (promise rejected) while
    // loading / rendering / benchmarking, or with unsaved studio changes unless args.discard_unsaved.
    bool McpLeaveScreen(const nlohmann::json& args, McpPromise& promise);
    void McpScheduleInput(int frame, std::string cmd, std::vector<std::string> args);  // frame in mcpFrame_ units
    const CharacterAsset* FindCharacter(const std::string& needle) const;
    const StageAsset* FindStage(const std::string& needle) const;
    const SongAsset* FindSong(const std::string& needle) const;
    const char* ScreenName(Screen s) const;

    // --- auto-update (UiUpdate.cpp): background check, notice, staged install across a restart
    void InitUpdater();          // once after the settings load: feed URL + current version
    void StartUpdateCheck();     // background check when the run is not headless/scripted
    void UpdateUpdate();         // every frame (any screen with the app bar): drains the workers
    void DrawUpdateNotice();     // the select screen's notice (available / progress / failed)
    void DrawUpdateProgressDialog();
    void StartUpdateInstall();   // "Update": stages and spawns the applier, then quits
    void UpdateCheckCommand();   // ui-script "updatecheck": synchronous check, logs the result
    void UpdateInstallCommand(); // ui-script "updateinstall": full flow, exits when staged
    bool HeadlessRun() const;    // --frames / --ui-script / benchmarks / offline renders

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
    // shader packs (UiShaderPack.cpp)
    void ApplyShaderChoice(GpuModel& gpu, const ShaderChoice& choice);   // every frame, before UpdateMaterials
    ShaderChoice PlayShaderChoice() const;                               // play character: settings + --shader-pack
    // A row showing the chosen pack; click opens a searchable pack list (+ "셰이더 관리"). True = changed.
    bool DrawShaderSelector(const char* id, ShaderChoice& choice, float width);
    bool DrawShaderPackParams(ShaderChoice& choice);                     // the chosen pack's sliders + reset
    uint64_t PackThumb(const ShaderPack& pack);                          // preview texture, 0 = none / loading
    uint64_t RemotePackThumb(const RemotePack& pack);
    void DrawPackImage(ImDrawList* dl, uint64_t tex, const std::string& key, ImVec2 a, ImVec2 b, float rounding,
                       ImDrawFlags flags = 0);                           // cover-fit, placeholder when tex == 0
    // shader pack manager (UiShaders.cpp)
    void DrawShaders();
    void DrawShaderPackDetail(float x0, float y0, float x1, float y1);
    void DrawRemotePackDetail(float x0, float y0, float x1, float y1);
    void DrawNewPackDialog();
    void InstallPackPath(const std::filesystem::path& path);             // .zip or a pack folder (drop / dialog)
    void PollShaderPacks();                                              // every frame: hot reload, store, drops
    // The effect stack editor (UiShaders.cpp): add / remove / reorder (up/down) / enable + per-effect
    // sliders. Edits `stack` in place; true = changed. `widths` = the content width for layout.
    bool DrawEffectStackEditor(std::vector<EffectStackEntry>& stack, bool header = true);
    bool shaderParamsOpen_ = false;                                      // select screen: pack settings disclosure
    bool effectStackOpen_ = false;                                       // select screen: effect stack disclosure
    int shaderDetailTab_ = 0;                                            // shader detail: 0 info, 1 settings, 2 manage
    ShaderPackStore shaderStore_;
    int shaderTab_ = 0;                                                  // 0 installed, 1 online
    char shaderFilter_[128] = {};
    std::string shaderSelInstalled_, shaderSelRemote_;
    bool newPackOpen_ = false, newPackEffect_ = false;
    char newPackId_[64] = {}, newPackName_[96] = {}, newPackAuthor_[96] = {};
    std::vector<std::filesystem::path> droppedFiles_;
    void DrawStudioShaderRow(float w);                                   // inspector: the selected character's pack
    void CheckShaderPackErrors();                                        // compile failures -> error toast
    uint32_t shaderPackErrorsSeen_ = 0;
    std::filesystem::path OfflineOutputDir(bool video) const;  // Pictures\MMDX12 or Videos\MMDX12
    DirectX::XMFLOAT3 CharacterCenter() const;  // world position of the character's center bone (teleport detection)
    void OfflinePose(float frame);              // UpdateScene, or StudioPoseForRender for a studio job
    void OfflineView(float frame, FrameView& view);  // BuildFrameView, or BuildStudioFrameView for a studio job
    void SaveOfflinePose();                     // current scene pose -> offline_.prevPose (video motion blur)
    void UploadOfflinePrevPose(uint64_t slot);  // offline_.prevPose -> the models' bone/morph ring entry `slot`

    AppOptions options_;
    int shadingOverride_ = -1;  // --shading (hidden): run-only raster ViewShading override, -1 = off
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
    int lobbyTab_ = 0;                                                   // select screen settings tab: 0 screen, 1 shader, 2 detail
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
    studio::CameraKf studioCamKeyBase_;              // ... and its key (interpolation curves)
    FreeCamera studioCamBase_;                       // the camera when a handle drag started
    POINT lastMouse_{};
    double timeSeconds_ = 0;      // wall clock since start (QPC)
    std::atomic<bool> loading_{false};  // startup renderer init in flight: splash paint, resize deferred
    int framesInScene_ = 0;       // frames rendered in Play/BenchRun (for --frames)
    double gpuMsSum_ = 0.0;       // --frames: GPU time of the second half, logged at quit
    int gpuMsCount_ = 0;
    LARGE_INTEGER startupRunBegan_{};  // Run() entry (STARTUP first-frame timing)
    LARGE_INTEGER startupFreq_{};      // QPC frequency for the STARTUP timings

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
        bool studio = false;           // the Studio's project is rendered (studio_ instead of scene_); back to the studio
        double studioTime = 0;         // studio: the editor's time and camera mode before the render
        bool studioMotionCamera = false;
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

    // ui script
    struct UiScriptStep { int frame = 0; std::string cmd; std::vector<std::string> args; };
    std::vector<UiScriptStep> uiScript_;
    size_t uiScriptNext_ = 0;
    bool uiScriptLoaded_ = false;
    float uiScriptMouse_[2] = {-1e30f, -1e30f};  // last scripted mouse position (re-sent every frame)

    // auto-update (Updater.h): the controller owns its worker threads; the UI polls
    updater::Controller update_;
    bool updateCheckQueued_ = false;   // the check starts once the window is up
    bool updateInstallingFromCli_ = false;  // ui-script "updateinstall": exit when staged/failed
    std::string updateRelaunchArgs_;   // argv for the restarted app (cli-triggered updates)

    // studio
    std::unique_ptr<studio::StudioDoc> studio_;
    std::unique_ptr<studio::StudioPackage> studioPackage_;
    double studioLastBind_ = 0;       // timeSeconds_ of the last motion re-bind (throttled while dragging)
    bool studioLeaveConfirm_ = false; // unsaved-changes prompt is open (guards studioPending_)
    StudioAction studioPending_ = StudioAction::None;
    std::filesystem::path studioPendingFile_;
    // async model / song loads started inside the studio
    struct StudioJob {
        std::string label;
        bool song = false;                         // library song: dance/camera/audio; else models
        bool audioOnly = false;                    // just an audio file (the add menu's audio entry)
        bool audioPreloaded = false;               // the worker decoded `audio` (AudioPlayer::Preload): release it
        std::vector<studio::StudioPackageModel> models;
        studio::MotionData dance, camera;
        std::filesystem::path audio;
        uint32_t targetUid = 0;                    // song: the character it was started for (0: none)
        std::shared_ptr<McpPromise> mcp;           // studio_add_model: answered when the job ends
        LoadProgress progress;
        std::string error;
        std::future<bool> future;                  // last member: destroyed first, waits for the worker
    };
    std::vector<std::unique_ptr<StudioJob>> studioJobs_;
    std::future<bool> studioAutosave_;             // recovery file writer
    double studioAutosaveAt_ = 0;                  // timeSeconds_ of the last autosave check
    bool recoveryChecked_ = false;                 // the select screen asked about a recovery file this session
    bool recoveryPrompt_ = false;
    int studioRenameModel_ = -1;                   // outliner rename popup target
    int studioRemoveModel_ = -1;                   // outliner remove confirmation target
    std::string studioLoadTitle_;                  // Loading screen title while a project opens (empty: library scene)
    char studioRenameBuf_[128] = {};
    char studioAddFilter_[64] = {};
    int studioAddPage_ = 0;                        // "+" popup page: 0 menu, 1 characters, 2 stages, 3 songs
    bool studioHelpOpen_ = false;                  // shortcut overlay
    bool studioEffectsOpen_ = false;               // screen effects popup
    bool studioRenderWhole_ = false;               // render dialog: whole project instead of the timeline range
    // viewport pose editing (cached from the last drawn frame: overlay, picking, scripts)
    studio::ViewProj studioVp_;
    bool studioGizmoShown_ = false;
    studio::GizmoFrame studioGizmoFrame_;
    studio::GizmoMode studioGizmoMode_ = studio::GizmoMode::Rotate;
    studio::GizmoPart studioGizmoHot_ = studio::GizmoPart::None;
    int studioViewDrag_ = 0;          // 0 none, 1 camera (orbit/pan), 2 gizmo, 3 press consumed by a bone pick
    ImVec2 studioPressPos_{};
    bool studioPressMoved_ = false;
    studio::GizmoDrag studioGizmoDrag_;
    studio::PoseLayer studioPoseBefore_;  // the layer when a gizmo drag / numeric edit started (one undo step)
    studio::PoseBone studioDragBase_;     // the bone's value when the drag started
    DirectX::XMFLOAT4 studioDragParentRot_{0, 0, 0, 1};
    float studioDragScale_ = 1.0f;
    int studioDragBone_ = -1;
    bool studioPoseFieldEdit_ = false;    // a numeric pose field / morph slider is being edited
    int studioHoverBone_ = -1;
    // camera inspector / path
    bool studioKeyEdit_ = false;                     // a camera/light/shadow key field is being edited
    bool studioKeyChanged_ = false;                  // ... and its value changed (else no undo step)
    bool studioKeyLive_ = false;                     // ... through the playhead fields (independent of the key selection)
    std::vector<studio::TrackState> studioKeyBefore_;
    std::vector<studio::CameraPathPoint> studioCamPath_;  // motion camera samples (per frame, strided when long)
    std::vector<DirectX::XMFLOAT3> studioCamKeys_;   // eye at each camera key
    uint64_t studioCamPathVersion_ = 0;
    int studioCamPathStride_ = 1;
    // scene light editing
    int studioLightPresetPending_ = -1;
    bool studioLightEdit_ = false;
    bool studioLightChanged_ = false;
    bool studioLightIsKeyEdit_ = false;
    uint32_t studioLightEditingUid_ = 0;
    std::vector<studio::TrackState> studioLightKeyBefore_;
    std::vector<studio::SceneLight> studioLightBaseBefore_;

    enum class LightDragPart { None = 0, PosGizmo, AimGizmo, SunRotate, ConeHandle, RangeHandle };
    LightDragPart studioLightDragPart_ = LightDragPart::None;
    studio::GizmoFrame studioLightPosFrame_;
    studio::GizmoFrame studioLightAimFrame_;
    studio::GizmoFrame studioLightSunFrame_;
    studio::GizmoPart studioLightHotGizmo_ = studio::GizmoPart::None;
    studio::GizmoDrag studioLightGizmoDrag_;
    studio::LightValues studioLightValuesBase_{};
    DirectX::XMFLOAT3 studioLightResolvedAim_{};
    ImVec2 studioLightConeHandlePos_{};
    ImVec2 studioLightRangeHandlePos_{};
    bool studioLightConeHot_ = false;
    bool studioLightRangeHot_ = false;
    bool studioLightGizmoShown_ = false;


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

    // MCP (AppMcp.cpp)
    std::unique_ptr<McpServer> mcpServer_;
    int mcpFrame_ = 0;                        // frames since start (framesInScene_ does not count on every screen)
    std::vector<UiScriptStep> mcpInput_;      // ui_input steps, frame = mcpFrame_ they run at (sorted)
    bool mcpInputActive_ = false;             // real mouse / keyboard input is ignored while steps are queued
    std::filesystem::path mcpOfflineOutput_;  // render_still / render_video output for the job about to start
    std::unique_ptr<AppOptions> mcpOptionsRestore_;  // the run's options before an MCP render's overrides

    struct PendingMcpWait {
        int waitFrames = 0;
        std::shared_ptr<McpPromise> promise;
    };
    std::vector<PendingMcpWait> pendingMcpWaits_;

    struct PendingMcpLoad {
        std::chrono::steady_clock::time_point startTime;
        std::shared_ptr<McpPromise> promise;
    };
    std::vector<PendingMcpLoad> pendingMcpLoads_;

    struct PendingMcpInput {
        int targetFrame = 0;
        std::shared_ptr<McpPromise> promise;
    };
    std::vector<PendingMcpInput> pendingMcpInputs_;
};

} // namespace mmdx
