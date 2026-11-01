#pragma once
#include <atomic>
#include <functional>
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "app/Benchmark.h"
#include "app/Lighting.h"
#include "app/StageRuntime.h"
#include "app/McpServer.h"
#include "app/RenderBench.h"
#include "app/LeaderboardClient.h"
#include "app/SceneLoader.h"
#include "app/Settings.h"
#include "app/ShaderPackStore.h"
#include "app/StageStore.h"
#include "app/ThumbnailCache.h"
#include "app/Updater.h"
#include "app/VideoEncoder.h"
#include "app/WorldColliders.h"
#include "core/TextUtil.h"
#include "asset/AssetLibrary.h"
#include "asset/MaterialChoice.h"
#include "asset/RayMaterial.h"
#include "asset/VmdMotion.h"
#include "audio/AudioPlayer.h"
#include "audio/BeatAnalysis.h"
#include "render/ColorLut.h"
#include "render/Dx12Context.h"
#include "render/MmeDiagnostics.h"
#include "render/LookPreset.h"
#include "render/PackCrashGuard.h"
#include "render/Renderer.h"
#include "studio/AutoIk.h"
#include "studio/Gizmo.h"
#include "studio/PhysicsBake.h"
#include "studio/RestPoseRetarget.h"
#include "studio/StudioDoc.h"
#include "studio/StudioFocus.h"
#include <Windows.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmdx {

// Command line (all optional). Used for normal launches and for automated verification.
//   --library <dir>        override library folder for this run
//   --character <substr>   preselect: first character whose id or displayName contains substr (case-insensitive)
//   --stage <substr>       preselect stage ("none" = no stage)
//   --stage-variant <id>   the MMDX stage's variant for this run (wins over the saved stageVariant= ini choice)
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
//                          studiobgvideo <file|none> (the whole rest of the line is the path) |
//                          studiobgvideoopt <offsetSeconds> <asBackground 0|1> |
//                          studioreplace <index> <file> | studioreplacelib <index> <substr> (swap a model's file) |
//                          studioselect <model index|-1> | studioremove <model index> | studiorename <text> |
//                          studiomaterial <default|ray> [emd path] (selected studio model's PBR material source) |
//                          studioattach <parent index|-1> <bone|-> <tx ty tz rx ry rz s> (selected prop).
//                          drop <file>[|<file>...] injects a drag-and-drop path list (models, .vmd/.vpd, audio,
//                          .mmdxproj) into the Studio; '|' separates paths so they may contain spaces.
//                          Coordinates in window pixels.
//   --width <w> --height <h>  initial window client size
//   --debug                enable the D3D12 debug layer
//   --free-camera          start with the free orbit camera
//   --screen <select|stages|songs|settings|video|bench|bench-gi|studio|shaders|shaders-online|shaders-mme|mme> open
//                          that screen after the scan (UI testing; shaders-mme: the shader manager's MME tab; mme: the
//                          MME · Ray-MMD settings screen);
//                          --frames counts all frames; studio: opens --character/--stage/--song in the Studio at
//                          --seek and counts frames from the Studio on; without --character: a new empty project)
//   --project <file.mmdxproj>  open that Studio project after the scan (implies --screen studio)
//   --lighting <0..3>      lighting preset for this run (Studio, Sunset, Concert, Night)
//   --quality <0..3>       graphics preset for this run (low, medium, high, ultra)
//   --render <raster|rt|pt>            render path for this run (override settings)
//   --studio-renderer <raster|rt|pt|gi>  studio viewport renderer for this run (lit single view; override settings)
//   --upscaler <none|dlss|fsr|xess>    upscaler for this run (override settings)
//   --upscale-quality <native|quality|balanced|performance|ultra>  upscaler quality for this run
//   --frame-gen <off|2|3|4>            DLSS Frame Generation multiplier for this run (2x / 3x / 4x)
//   --dlss-model <auto|cnn|transformer|transformer2>  DLSS Super Resolution model (preset hint) for this run
//   --dlss-rr <0|1>        DLSS Ray Reconstruction (PathTraced + DLSS only) for this run
//   --no-physics           disable rigid-body physics for this run
//   --dof <0|1>  --volumetric <0|1>  --bloom-conv <0|1>   post effects for this run
//   --bloom-kernel <cinema|starburst|glow|anamorphic>  convolution bloom kernel preset for this run
//   --bloom-blend <add|energy>  --bloom-depth-mask <0..1>  --bloom-intensity <v>  --bloom-threshold <v>
//   --bloom-radius <0..1>      bloom look for this run (not saved)
//   --auto-luminous <0|1>  そぼろ AutoLuminous glow convention for this run
//   --volumetric-density <0.25..4>  volumetric medium density for this run
//   --volumetric-mode <froxel|march>  S9d real-time haze: the froxel grid (default) or the per-pixel march (A/B)
//   --rtao-rays <n>       RTAO rays per half-res pixel (1..64)
//   --volumetric-ms <0|1>  S9d froxel multiple-scatter approximation (0 = single scattering only)
//   --lut <substr|none>    colour LUT for this run (first LUT whose id or name contains substr)
//   --offline-still <file.png>   with --autoplay: render the --seek frame with the offline GI renderer,
//                                save it to the file and quit
//   --offline-video <file.mp4>   with --autoplay: offline video render, then quit
//   --offline-range <a> <b>      video range; each of a / b is seconds or a timecode (HH:MM:SS, MM:SS, SS;
//                                default: the whole song)
//   --offline-spp <n>            testing: cap samples per pixel (min samples = min(min, n))
//   --offline-size <w> <h>       testing: output size override for stills and videos
//   --offline-fps <n>            video frame rate for this run (24, 30, 60; default: the lobby dialog's)
//   --offline-bitrate <mbps>     video bit rate for this run
//   --offline-quality <0..3>     video quality preset for this run (draft, standard, high, best)
//   --offline-renderer <r>       video renderer for this run: raster, rt, pt or gi (default: the lobby dialog's)
//   --offline-alpha 1            transparent-background export: --offline-video writes a straight-alpha RGBA8 PNG
//                                sequence (<output stem>_frames/<stem>_00000.png), --offline-still one RGBA PNG
//   --offline-probe              with --autoplay: sample render from the middle of the song (the lobby dialog's
//                                time measurement) with the dialog's settings, log "VIDEO PROBE", then quit
//   --offline-volumetric <m>     offline GI haze for this run: single (the single-scatter pass) or full (the paths
//                                compute the medium); default: the saved volumetricFull / videoVolumetricFull
//                                Studio (--project / --screen studio): --offline-video renders the project once it is
//                                open (range: --offline-range in timeline seconds, else the timeline range, else the
//                                whole project; motion camera; project audio) and quits; --offline-still renders the
//                                --seek frame (GI).
//   --theme <auto|light|dark>    colour theme for this run instead of the saved one (auto = follow Windows)
//   --mme-global <0|1>          global MME switch for this run (0 = off everywhere; default: settings.mmeEnabled)
//   --ray-global <0|1>          global Ray-MMD material-source switch for this run (0 = Default everywhere;
//                                default: settings.rayMmdEnabled)
//   --mme-workflow <auto|manual>  MME workflow for this run (auto = every model gets the automatic choices)
//   --material-source <default|ray>  per-material PBR data source (work unit P2); default renders as before and
//                                never reads Ray .md/.fx data
//   --pbr-debug <ch>             with --material-source ray: output one PBR channel (none|albedo|normal|roughness|
//                                metalness|ao|emissive|model) in the lit raster view
//   --classic                    vanilla-MMD look for this run: SSAO, SSR, bloom, volumetric, DoF, LUT, effects and
//                                soft shadows off (the plain toon key light stays); never saved to mmdx12.ini
//   --ui-scale <f>              UI scale for this run instead of the monitor's DPI scale (1.5 = 144 dpi; captures)
//   --update-feed <url-or-file>  auto-update feed override: an https URL, a file: URL or a local
//                                latest.json path (testing); the zip url may also be file:/local
//   --update-relaunch-frames <n> with --update-feed: relaunch the updated app with --frames <n>
//                                (headless E2E; the restarted version quits on its own)
//   --apply-update               internal: apply a staged update with no window, then exit
//   --apply-wait <pid>           internal: process id the applier waits for before the swap
//   --component <id>             optional component (app/Components.h) to act on
//   --download-component         with --component: fetch it now with no window, then exit (headless E2E)
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
    bool captureSceneOnly = false;   // --capture-scene 1: the capture is the rendered scene without UI
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
    int studioRenderer = -1;   // --studio-renderer <raster|rt|pt|gi>: override settings for this run
    int shading = -1;          // --shading <lit|unlit|wire>: raster ViewShading override for this run (hidden)
    int upscaler = -1;         // --upscaler <none|dlss|fsr|xess>: override settings for this run
    int upscalerQuality = -1;  // --upscale-quality <native|quality|balanced|performance|ultra>: override settings for this run
    int frameGen = -1;         // --frame-gen <off|2|3|4>: override settings for this run
    int dlssModel = -1;        // --dlss-model <auto|cnn|transformer|transformer2>: override settings for this run
    int dlssRayReconstruction = -1;  // --dlss-rr <0|1>: null default; override settings for this run

    bool noPhysics = false;    // --no-physics: override settings for this run
    std::string shaderPack;    // --shader-pack <id|none>: the character's shader pack for this run (play and studio)
    bool shaderPackSet = false;
    std::string look;          // --look <id>: applies a look pack to this run's settings (not persisted)
    bool lookSet = false;
    int materialSource = 0;    // --material-source <default|ray>: per-material PBR source for this run (0 default, 1 ray)
    bool materialSourceSet = false;   // --material-source was given: it wins over the saved per-character choices
    int mmeGlobal = -1;        // --mme-global <0|1>: global MME switch for this run (-1 = settings.mmeEnabled)
    int rayGlobal = -1;        // --ray-global <0|1>: global Ray-MMD material-source switch (-1 = settings.rayMmdEnabled)
    int mmeWorkflow = -1;      // --mme-workflow <auto|manual>: workflow for this run (-1 = settings.mmeWorkflow)
    int pbrDebug = 0;          // --pbr-debug <none|albedo|...>: PBR debug channel for this run (0 none .. 7 model)
    std::string packIndex;     // --pack-index <url|path>: the online shader pack gallery index (testing)
    std::vector<EffectStackEntry> effectStack;  // --effect <id>[,<id>...] / none: the effect stack, this run
    bool effectSet = false;
    std::vector<EffectStackEntry> worldStack;   // --world <id>[,<id>...] / none: the world stack, this run
    bool worldSet = false;
    std::vector<MmeEffectEntry> mmeEffects;  // --mme <file>[,<file>...]: MME post effects, this run
    bool mmeSet = false;                     // --mme was given: it overrides the saved play-mode stack for one run
    bool mmePanelSet = false;                // --mme-panel <si>,<tr>
    float mmePanelSi = 1.0f, mmePanelTr = 1.0f;
    // M5c: --mme-object <path>[,<path>...] (an accessory `.x` or its `.fx`). Pairs each with its same-stem partner
    // in the same folder (an `.x` -> its `.fx`, an `.fx` -> its `.x` when present). The `.fx` runs as a post/scene
    // effect (panel default) whose object techniques draw the `.x`.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> mmeObjects;  // (fx, meshFile)
    bool mmeObjectSet = false;
    std::filesystem::path mmeModel;          // --mme-model <fx>: an MME object effect on the play-mode character
    bool mmeModelSet = false;                // --mme-model was given (File mode for the character, this run)
    bool mmeModelAll = false;                // --mme-model-all: apply --mme-model to every performer
    int mmeAuto = -1;                        // --mme-auto <0|1>: force None(0)/Auto(1) for every model, this run
    bool mmeAutoSet = false;
    int stageMme = -1;                       // --stage-mme <0|1>: force None(0)/Auto(1) for stages only, this run
    bool stageMmeSet = false;
    int dof = -1, volumetric = -1, bloomConv = -1;  // --dof/--volumetric/--bloom-conv <0|1>: override for this run
    int bloomKernel = -1;      // --bloom-kernel <preset 0..3>: override for this run (-1 = settings)
    int bloomBlend = -1;       // --bloom-blend <add|energy>: override for this run (-1 = settings)
    float bloomDepthMask = -1.0f;  // --bloom-depth-mask (0..1; < 0 = settings)
    float bloomIntensity = -1.0f;  // --bloom-intensity (0..1.5; < 0 = settings)
    float bloomThreshold = -1.0f;  // --bloom-threshold (0.2..4; < 0 = settings)
    float bloomRadius = -1.0f;     // --bloom-radius (0..1; < 0 = settings)
    int autoLuminous = -1;     // --auto-luminous <0|1>: override for this run (-1 = settings)
    int screenLights = -1;     // --screen-lights <0|1>: S8b stage screens as area lights, this run (-1 = settings)
    bool classic = false;      // --classic: vanilla-MMD look for this run (SSAO/SSR/bloom/volumetric/DoF/LUT/effects/soft shadows off)
    int motionLighting = -1;   // --motion-lighting <0|1>: camera VMD light/self-shadow tracks in play mode, this run
    float volumetricDensity = -1.0f;                 // --volumetric-density (< 0 = settings)
    int volumetricMode = -1;                         // --volumetric-mode froxel|march (-1 = settings)
    int rtaoRays = -1;                               // --rtao-rays (-1 = settings)
    int volumetricMs = -1;                           // --volumetric-ms 0|1 (-1 = settings)
    int offlineVolumetric = -1;                      // --offline-volumetric single|full (-1 = settings)
    std::string lut;           // --lut <substr|none>: override for this run (resolved against the LUT list)
    std::filesystem::path offlineStill, offlineVideo;  // --offline-still / --offline-video
    double offlineRange[2] = {-1.0, -1.0};             // --offline-range (seconds; < 0 = unset)
    int offlineSpp = 0;                                // --offline-spp (0 = default)
    int offlinePrepass = -1;                           // --offline-prepass 0|1: GI irradiance cache prepass (-1 = default on)
    int offlineSize[2] = {0, 0};                       // --offline-size (0 = default)
    int offlineFps = 0, offlineBitrate = 0;            // --offline-fps / --offline-bitrate (0 = settings)
    int offlineQuality = -1;                           // --offline-quality (-1 = settings)
    int offlineRenderer = -1;                          // --offline-renderer <raster|rt|pt|gi> (-1 = settings)
    int offlineAlpha = -1;                             // --offline-alpha <0|1>: transparent PNG sequence for --offline-video/still
    bool offlineProbe = false;                         // --offline-probe: sample render for the time estimate, then quit
    int theme = -1;     // --theme auto|light|dark for this run only (-1: keep the saved setting; 0 auto, 1 light, 2 dark)
    int language = -1;  // --lang auto|ko|en|ja|zh for this run only (-1: keep the saved setting)
    std::filesystem::path project;  // --project
    float uiScale = 0.0f;           // --ui-scale (0 = the window's DPI scale)
    std::string formation;          // --formation <spec>
    std::string songAudio;          // --song-audio <substring>: pick an audio version of the song for this run
    std::string stageVariant;       // --stage-variant <id>: the MMDX stage's variant for this run (wins over the saved)
    std::string startScreen;  // --screen select|bench: open that screen after the scan; --frames then counts every frame  // --free-camera: start in the orbit camera instead of the VMD camera
    std::string galleryIndex;  // --gallery-index <url|path>: the stage gallery's index for this run (default: the online one)
    std::string updateFeed;   // --update-feed: feed URL / file override (empty: the default feed)
    int updateRelaunchFrames = 0;  // --update-relaunch-frames: relaunch the updated app with --frames (E2E)
    bool applyUpdate = false; // --apply-update (internal): apply a staged update, then exit
    uint32_t applyWaitPid = 0;        // --apply-wait (internal): pid the applier waits for
    std::string componentId;   // --component <id>: optional component (Components.h) to act on
    bool componentDownload = false;  // --download-component: fetch --component now, then exit (headless E2E)
    int mcp = -1;                     // --mcp (1), --no-mcp (0), auto (-1)
};
AppOptions ParseCommandLine(int argc, wchar_t** argv);  // unknown args are logged and ignored

// M5c: pairs an MME effect object path (an accessory `.x` or its `.fx`) with its same-stem partner in the same
// folder (case-insensitive; `.x` -> `.fx`, `.fx` -> `.x` when present). Either output may be empty when the partner
// is absent. Defined in App.cpp.
void ResolveMmeObjectPair(const std::filesystem::path& path, std::filesystem::path& fx,
                          std::filesystem::path& meshFile);


// Folder picker for a pack's user texture folder (FOS_PICKFOLDERS): runs on its own STA thread,
// keeping the owner's messages pumped. Defined in UiShaderPack.cpp, also used by UiShaders.cpp.
std::filesystem::path PickPackTextureFolder(HWND owner);

// Popup width for the pack parameter grid (columns from the pack's param count), clamped to the viewport.
// Defined in UiShaderPack.cpp, used by the studio shader popup.
float PackParamsPopupWidth(const ShaderChoice& choice);

class App {
public:
    // The studio ribbon's row heights (UiStudioRibbon.cpp draws them; UiStudio.cpp sizes the top window with them).
    static constexpr float kRibbonHeaderH = 44.0f, kRibbonToolsH = 76.0f;
    int Run(HINSTANCE instance, const AppOptions& options);  // returns process exit code

    // Window procedure trampoline (public so the static WndProc can call it).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Screen { Scanning, Select, Loading, Play, BenchLobby, BenchRun, BenchResult, Offline, BenchRender, Studio, Shaders, Mme, Gallery };
    enum class LoadTarget { Play, Benchmark, RenderBench, OfflineVideo, Studio };

    struct SceneRuntime {
        std::unique_ptr<ModelInstance> character;
        std::string characterId;  // library id, key of the saved display scale
        std::string characterDisplayName;
        std::unique_ptr<GpuModel> characterGpu;
        std::vector<std::unique_ptr<ModelInstance>> stages;
        std::vector<std::unique_ptr<GpuModel>> stageGpu;
        std::string stageId;   // M6a: the stage library id (key of the saved MME choice / display scale)
        // S1b: an MMDX stage's runtime (manifest, variant, light rig, material targets); invalid for a classic stage.
        std::shared_ptr<StageRuntime> stage;
        // The cast a formation (unit song) scene was loaded with; empty for a single-performer scene. Kept so a
        // material-source change can reload the same formation (App::ReloadPlaySceneForMaterial).
        std::vector<PerformerCast> formation;
        std::shared_ptr<BoundMotion> motion;
        std::shared_ptr<CameraMotion> camera;
        // M8a: the bundled stage's motions (may be empty), each bound to one stage part.
        std::vector<StageMotionBinding> stageMotions;
        // M8b: the song's resolved video layers (background + overlay), from the song / the ini override.
        std::vector<VideoLayerDesc> videos;
        std::filesystem::path audioPath;
        // M5c: MME effect objects (accessory `.x` + same-stem `.fx`). Each `mesh` is appended to `view.models` (so
        // the renderer draws it through the MME object path) and to `mmeEffectObjects`.
        struct EffectObject {
            std::filesystem::path fx, meshFile;
            std::unique_ptr<ModelInstance> inst;  // accessory transform carrier (identity world vertices are skinned by this)
            std::unique_ptr<GpuModel> mesh;
            MmeControllerPanel panel;
            std::string fileName, displayName;
        };
        std::vector<EffectObject> effectObjects;
        // Auto workflow (완전 자동): the MME controllers a play scene auto-loaded (a model's effect needed a
        // CONTROLOBJECT object and `FindMmeControllerFile` located its file). They are CONTROLOBJECT-visible
        // (MmeSceneNames / view.mmeObjects) but never drawn: not in view.models, not in BLAS, not in shadows.
        // Freed with the scene.
        struct Controller {
            std::filesystem::path path;
            std::unique_ptr<ModelInstance> inst;   // identity root transform, posed once at rest
            std::unique_ptr<GpuModel> gpu;
            std::string fileName;    // lower case (the CONTROLOBJECT match key)
            std::string displayName; // the PMX name
        };
        std::vector<Controller> controllers;
        // The camera VMD's light / self-shadow tracks (empty when absent or only MMD's defaults); used when
        // AppSettings::motionLighting is on.
        std::vector<studio::LightKf> lightTrack;
        std::vector<studio::ShadowKf> shadowTrack;
        float endFrame = 0;
        float motionFps = kMmdFps;  // play scene's motion time base (60 for 60 fps VMDs)
        bool hasAudio = false;
        float physicsFrame = -1;  // motion frame of the last physics step (-1: reset on next update)
        std::chrono::steady_clock::time_point physicsClock;  // wall time of the last physics step
        // Render benchmark: performers after `character` (posed once by PoseRenderBench, never
        // updated per frame). Empty for normal playback.
        struct Performer {
            std::unique_ptr<ModelInstance> instance;
            std::unique_ptr<GpuModel> gpu;
            std::shared_ptr<BoundMotion> motion;
            std::string characterId;
            std::string displayName;
        };
        std::vector<Performer> extras;
        // World entities (docs/effect_ecosystem.md §2 item 7): the collider track of the cast, built after the scene
        // loads (whole song, 30 Hz) only while the world stack is not empty. The previous track stays in use until
        // the worker's result arrives.
        std::shared_ptr<const WorldColliderTrack> colliders;
        std::future<std::shared_ptr<const WorldColliderTrack>> colliderFuture;
        uint64_t colliderVersion = 0;
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
    void StartRevalidation();
    void ApplyRevalidatedLibrary(LibraryScanResult fresh);
    void RestoreSelectionsFromSettings();
    void ApplyStartScreen();
    void ApplyCommandLinePreselection();
    void ApplyRenderSettings();
    std::vector<EffectStackEntry> UsableEffectStack() const;
    // Theme: settings_.theme (or --theme) resolved to light / dark, applied to the UI kit palette, the ImGui style and
    // the title bar. Requests go through themeDirty_ and run before the next ImGui frame (never mid-frame).
    void ApplyTheme();
    void DrawThemeButton(ImVec2 pos, float popupRight, float popupY);  // icon button + system / light / dark popup
    void SetThemeSetting(int theme);  // 0 auto, 1 light, 2 dark: saves and applies
    void ApplyGraphicsPreset(int preset);  // sets the effect toggles of AppSettings
    void RefreshColorLuts();            // rescans luts_, resolves options_.lut
    void ApplyColorLut();               // uploads settings_.colorLut when it differs from appliedLut_

    // --- scene (App.cpp)
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song) {
        StartLoad(target, ch, st, song, nullptr);
    }
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song,
                   const std::vector<PerformerCast>* formation);
    // P5: a play-mode material-source change reloads the scene at the current time (single character; a unit-song
    // formation scene applies it at the next load instead, and the UI says so).
    void ReloadPlaySceneForMaterial();
    void PollLoad();
    bool BuildSceneRuntime(ScenePackage& pkg);  // GPU upload on the main thread
    void UnloadScene();                          // WaitForGpu, release runtime + audio
    void UpdateScene(float frame);               // evaluate motion, update poses, upload to GPU
    void BuildFrameView(float frame, FrameView& view);
    // M1a: typed scene roles and audio-reactive inputs for shader packs (PackInputs.cpp)
    void FillPackInputs(FrameView& view, const ModelInstance* performer, double audioSeconds);
    bool NeedsAudioInputs() const;
    void EnsureAudioAnalysis(const std::filesystem::path& audioPath);
    // M5a: appends one MME scene object (CONTROLOBJECT / DefaultEffect matching) parallel to view.models. `inst`
    // may be null (no per-model data); `displayName` is the model's display name.
    void AppendMmeObject(FrameView& view, const GpuModel* gpu, const ModelInstance* inst, bool visible,
                         const std::string& displayName);
    // M5c: appends the play scene's MME effect objects' meshes to `view.models` (before the characters so the
    // character draws over them) and fills `view.mmeEffectObjects`, each mesh with its own scene object (the
    // accessory world matrix). Shared by the play and studio views.
    void AppendMmeEffectObjects(FrameView& view, const std::vector<SceneRuntime::EffectObject>& objects);

    // --- screens (UiSelect.cpp / UiPlay.cpp / UiBenchmark.cpp)
    void DrawScanning();
    void DrawSelect();
    void DrawLoading();
    void UpdatePlay(double dt);
    void DrawPlayOverlay();
    // Play mode: the performer the per-character popups edit (0 = center, i+1 = extras[i]); reset per scene.
    int playEditPerformer_ = 0;
    // Play-mode "화면 효과" side panel (effect / shader / MME tabs): slides in from the right edge while a play
    // scene runs. Its MME tab is a draft copy of the MME-relevant settings taken when it opens; nothing is applied
    // until 적용 (되돌리기 discards).
    struct MmeDraft {
        bool valid = false;
        bool enabled = true;
        bool rayEnabled = true;
        int workflow = 0;
        std::vector<MmeEffectEntry> mmeEffects;
        std::map<std::string, MmeChoice> characterMmes;
        std::map<std::string, MmeChoice> stageMmes;
        std::map<std::string, MaterialChoice> characterMaterials;
    };
    bool playPanelOpen_ = false;
    float playPanelT_ = 0.0f;   // slide progress 0 (closed) .. 1 (open), stepped with the frame time
    bool playPanelReopen_ = false;   // an MME apply reloaded the scene: show the panel again after it
    bool mmeScreenShowStages_ = false;  // MME screen list: show library stages instead of characters
    char mmeFilter_[128] = {};          // MME screen list: search filter
    // The panel's draft: SyncPlayMmeDraft captures the live settings into it when the panel opens (and again after an
    // apply-triggered reload); ApplyPlayMmeDraft writes a draft back. The MME screen reads/writes the settings directly.
    MmeDraft playMmeDraft_;
    void SyncPlayMmeDraft();
    void ApplyPlayMmeDraft(const MmeDraft& draft, bool reloadNeeded);
    MmeDraft CaptureMmeDraft() const;
    void WriteMmeDraft(const MmeDraft& d);
    // The play-mode side panel (UiPlayPanel.cpp): the sliding window + tabs, and one method per tab body.
    void DrawPlayFxPanel();
    void DrawScreenEffectControls(float width);   // shared by the panel's 효과 tab and the lobby 세부 tab
    void DrawBloomControls(float width, bool* changed);  // bloom detail rows shared by the play / studio effect panels
    void DrawPlayShaderTab(float width);          // the old ##shader popup body
    void DrawPlayMmeTab(float width);             // the old play MME panel body (draft logic unchanged)
    void DrawPlayMmeFooter(float width);          // 되돌리기 / 적용
    bool PlayMmeDraftDirty() const;               // draft differs from the live settings
    struct PlayPerformerRef {
        std::string id;              // library id (settings CharacterScale / CharacterShader / CharacterMaterial)
        GpuModel* gpu = nullptr;     // traits for pack suggestions / shader button state (null when out of range)
        ModelInstance* instance = nullptr;  // material list, HasMaterialOptions
    };
    PlayPerformerRef PlayPerformerAt(int index);
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
    // at the current frame; curvesOnly: interpolation onto existing keys; mirrored: bone keys onto the other side, reflected
    void StudioPaste(bool curvesOnly, bool mirrored = false);
    void DrawStudioKeyMenu();                        // timeline right-click menu (key actions at the clicked frame)
    void StudioRegisterKeys();                       // I: key the target rows at the current frame
    void StudioShiftFrames(bool remove);             // MMD frame insert/delete (range length or 1 frame)
    void StudioCopyCurve();                          // inspector: interpolation of the first selected key
    void StudioPasteCurve();                         // onto every selected key of the same kind
    void StudioJumpKey(int dir);                     // previous/next key of the shown rows
    void StudioRowStopwatch(uint64_t rowId);         // stopwatch clicked in row
    void StudioRowKeyNav(uint64_t rowId, int dir);   // keynav clicked in row (-1, 1, 2)
    void StudioImportVmd();                                   // open dialog, then StudioImportVmdFrom
    void StudioImportVmdFrom(const std::filesystem::path& path);  // merge into the selected model (camera VMDs: camera)
    // The VMD import's tail (clear selection, refresh rows, "VMD를 불러왔어요" toast): shared with the arm
    // rest-pose dialog, which merges the pending motion when the import asked for a correction first.
    void StudioFinishVmdImport(const std::filesystem::path& path);
    void StudioExportVmd();                                   // save dialog, then StudioExportVmdTo
    bool StudioExportVmdTo(const std::filesystem::path& path);  // selected model's motion (or the camera)
    bool StudioGiActive() const;   // the GI preview draws the studio viewport this frame
    void RecordStudioGiViewport(ID3D12GraphicsCommandList* cmd, FrameView& view);  // instead of Renderer::Render
    void ResetStudioGi();          // drops the viewport GI job (a real render / leaving the studio)
    struct StudioGiState {
        uint64_t key = 0;          // restart key of the running / last job
        bool valid = false;        // key is meaningful
        bool active = false;       // a viewport job is running (BeginOffline succeeded)
        float scale = 0.5f;        // resolution scale of the running job
        double keySince = 0;       // timeSeconds_ when `key` was first seen
        bool warned = false;       // the BeginOffline failure was logged for this entry into GI mode
    } studioGi_;
    // The studio ribbon (UiStudioRibbon.cpp): a header row of tabs over a tool row of grouped buttons. It replaces the
    // old flat top bar and calls the same functions the rest of the studio does.
    void DrawStudioRibbon(float x0, float y0, float x1, float y1);
    int studioRibbonTab_ = 0;              // selected ribbon tab: 0 home, 1 anim, 2 camera, 3 light, 4 render, 5 view
    bool studioRibbonCollapsed_ = false;
    bool studioInspectorToBones_ = false;   // a viewport bone pick: the inspector scrolls to its 본 tab   // only the header row shows
    // Helpers shared by the ribbon and the controls it duplicates (Settings: studioRenderer / physics / lighting source)
    void StudioSetViewportRenderer(int choice);        // settings_.studioRenderer + save (the transport combo's setter)
    void StudioTogglePhysics();                        // the transport's ##physics body
    void StudioSetLightSource(int source);             // 0 VMD link on, 1 preset lights, 2 custom (VMD link off)
    std::vector<std::filesystem::path> StudioRecentProjects() const;  // the 홈 tab's recent list (current excluded)
    void StudioRenderVideo();                          // render menu: 영상 렌더…
    void StudioRenderStill();                          // render menu: 고품질 스틸
    void DrawStudioOutliner(float x0, float y0, float x1, float y1);
    void StudioSelectWorld(uint32_t uid);              // 0 = no world selected
    void StudioAddWorld(const std::string& packId);    // add an installed world pack as a world entity (undoable)
    void DrawStudioWorldPanel(float w);                // world inspector: placement + keyable params
    uint32_t studioSelectedWorld_ = 0;                 // selected world entity (0 = none)
    bool studioOutlinerOpen_[3] = {true, true, true};  // outliner group folds: 장면 / 조명 / 월드 이펙트
    bool studioKeyMenuRuler_ = false;                  // the timeline menu was opened on the ruler (range items only)
    bool studioInspectorToKey_ = false;                // a timeline key click scrolls the inspector to the key fields
    std::vector<studio::StudioWorld> studioWorldBefore_;  // placement drag start (one undo step)
    bool studioWorldPlaceDragging_ = false;
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
    static FreeCamera FreeFromPose(const CameraPose& pose);    // orbit camera with the pose's eye and view axis (roll dropped)
    FreeCamera StudioViewedFree(int frame) const;    // the camera as shown now, in orbit form (target, yaw, pitch, distance)
    // possession: store `cam` as the camera key at the playhead. base: the key to start from when none exists there
    void StudioWriteCamera(const FreeCamera& cam, const studio::CameraKf* base = nullptr, bool takeView = true);
    void StudioNavEditTick();                        // closes the undo step of a possession gesture when it went quiet
    void StudioPossess(bool on);
    void StudioViewportCameraHandles(bool hovered);  // eye / target handles of the camera (free view, camera selected)
    std::string iniPath_;                            // ImGui ini (panel layout); must outlive the context
    bool studioFocusEffects_ = false;               // top bar effects button: bring the effects tab forward
    bool studioResetLayoutDone_ = false;            // default layout just built: show the properties tab
    int studioKeyMenuFrame_ = 0;                     // frame / row the timeline key menu was opened on
    uint64_t studioKeyMenuRow_ = 0;
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
    // The viewport's view now as a camera key at `frame`: the free view, or the main camera while looking through it
    // (what "새 카메라" / an extra camera's "현재 뷰로 키 등록" take).
    studio::CameraKf StudioViewportCameraKey(int frame) const;
    // The camera view the viewport shows: the main camera (the output), or a possessed stage-screen camera.
    const std::shared_ptr<CameraMotion>& StudioShownCameraEval() const;
    void StudioKeyExtraCameraFromView();             // the selected extra camera gets the viewport's view as a key
    void StudioFocusSelection();                     // F: frame the picked bone / selected model
    float studioFlyMul_ = 1.0f;                      // RMB + WASD fly speed multiplier (wheel while flying)
    // UI display name preference: when settings_.showEnglishNames is on and a PMX English name exists,
    // it wins, else the original name. Never used for internal ids (VMD / VPD track keys).
    const char* DisplayName(const std::string& name, const std::string& nameEn) const {
        return settings_.showEnglishNames && !nameEn.empty() ? nameEn.c_str() : name.c_str();
    }
    // Gizmo precision drag (Ctrl dampens pointer motion). The solved gizmo values are recomputed from the
    // drag-start base every frame, so keeping a running effective-mouse lets the modifier switch mid-drag
    // without a jump and keeps one undo step per gesture.
    ImVec2 studioGizmoEffMouse_{0, 0};
    ImVec2 studioGizmoLastRaw_{0, 0};
    bool studioGizmoCtrl_ = false;
    bool studioGizmoInit_ = false;
    // When on, clicking a bone in the viewport also centers the free camera on it.
    bool studioCenterOnBone_ = false;
    void StudioSelectAllBones();  // Ctrl+A: pick every bone of the active model
    // Overlapping-bone cycling: last click spot + the bone chosen there (see StudioViewportPose).
    ImVec2 studioCyclePos_{-1e9f, -1e9f};
    int studioCycleBone_ = -1;
    // Fixed-axis bone rotation panel: base rotation + accumulated angle captured at the gesture start.
    DirectX::XMFLOAT4 studioFixedAxisBaseR_{0, 0, 0, 1};
    float studioFixedAxisDelta_ = 0.0f;
    bool studioFixedAxisActive_ = false;
    // Last focused studio panel (updated while panels draw): 0 viewport / camera, 1 timeline, 2 other. Ctrl+A picks
    // bones when the viewport is focused and keys when the timeline is focused.
    int studioFocusPanel_ = 2;
    studio::CameraKf StudioViewedCamera(int frame) const;  // the camera as shown now (free view or motion camera) as a key at `frame`
    void StudioBeginKeyEdit(studio::RowKind kind);   // inspector key fields: one undo step per edit
    void StudioEndKeyEdit();
    void DrawStudioCameraPanel(float w);             // view / light / shadow sections of the camera inspector
    // true: no curve editor follows. live (camera only): the playhead's fields, shown with or without a key
    bool DrawStudioCameraKeyFields(float w, studio::RowKind kind, int frame, bool live = false);
    // --- depth-of-field focus (UiStudioFocus.cpp): the focus track + automatic focus -> FrameView focus / aperture
    void StudioComputeFocus(FrameView& view);        // BuildStudioFrameView: view.camera must be set
    bool StudioFocusTargetZ(uint32_t uid, studio::FocusBone bone, const CameraParams& cam, float& z) const;
    studio::FocusKf StudioNewFocusKey(int frame) const;  // a key inserted at `frame`: the segment in effect, or the shown subject
    void StudioSetFocusAtPlayhead(const studio::FocusKf& k, const char* undoName);  // upsert (undoable), keeps the key's fields
    void DrawStudioFocusSection(float w);            // camera panel: what is in focus + the segment picker at the playhead
    bool DrawStudioFocusKeyFields(float w, int frame);  // inspector fields of a selected focus key
    std::string StudioModelLabel(uint32_t uid) const;   // outliner name of a model uid ("" when gone)
    // --- accessory (prop) key track (UiStudioAccessory.cpp)
    studio::AccessoryKf StudioNewAccessoryKey(const studio::StudioModel& m, int frame) const;  // current placement as a key
    void StudioSetAccessoryAtPlayhead(const studio::AccessoryKf& k, const char* undoName);      // upsert (undoable)
    bool DrawStudioAccessoryKeyFields(float w, int frame);  // inspector fields of the accessory key at `frame`
    void DrawStudioAccessorySection(float w);               // prop inspector: accessory key summary + key button
    studio::AutoFocus studioAutoFocus_;
    studio::AutoFocus playAutoFocus_;   // play mode: picks the performer the shot is about (reset per scene)
    float studioFocusShown_ = 0.0f;                  // the last frame's focus distance / aperture scale / auto subject
    float studioFocusAperture_ = 1.0f;
    uint32_t studioFocusSubject_ = 0;                // the model the focus follows now (0: a distance / nothing)
    // --- scene light editing (UiStudioLight.cpp)
    void StudioSelectLight(uint32_t uid);
    void StudioAddLight(studio::LightKind kind);
    void StudioDeleteLight(uint32_t uid);
    void StudioApplyLightPreset(int presetIndex);
    // S1b: an MMDX stage with a light rig replaces the project's lights with it (one LightsCommand step + a toast).
    // S3: the rig lights are tagged with the stage (SceneLight::stageUid / stageLightName) and the stage's light targets
    // are replayed onto them every frame.
    void StudioApplyStageLightRig(uint32_t stageUid);
    // The rig lights of `stageUid` in `d` (tagged, with fresh uids from d.nextLightUid); false when the stage has none.
    // Shared by StudioApplyStageLightRig (undoable add) and a new project opened with a stage (no undo step).
    bool StudioStageRigLights(studio::StudioDoc& d, uint32_t stageUid, std::vector<studio::SceneLight>& out);
    // S1b: the studio's per-frame MMDX stage targets (material values + light values on the stage's own rig lights).
    void StudioApplyStageTargets();
    // S5: BuildStudioFrameView's stage screens (image / video / camera feed sources), the project's overrides applied.
    void StudioStageScreens(FrameView& view);
    // S5: the stage panel's screen rows (source combo, file picker, camera combo, brightness slider).
    void DrawStudioStageScreens(float w, uint32_t stageUid);
    // S5: one screen override edit (one StagesCommand step).
    void StudioSetScreenOverride(uint32_t stageUid, const std::string& screenId, const studio::StageScreenOverride& ov,
                                 const char* name);
    // S5: one screen's state at the timeline time: the manifest's values, the evaluator's screen targets, then the
    // project's override for that id (the studio panel, `studioscreen` and `studiostate` share it).
    std::vector<StageScreenState> StudioStageScreenStates(const studio::StudioStage& stage,
                                                          const StageEvalResult& eval) const;
    // S3: one StudioStage per stage entity for the parts that have none yet (`legacyVariants`: model uid -> the S1b
    // per-part variant a version <= 3 project / the library carried). Project stages already come as entities.
    void StudioConsolidateStages(studio::StudioDoc& doc, const std::map<uint32_t, std::string>& legacyVariants);
    // S3: inspector panel of the selected model's stage (variant combo, params, "큐를 키로 가져오기", per-param reset).
    void DrawStudioStagePanel(float w);
    void DrawStudioStageParams(float w, uint32_t stageUid);
    // S3: one stage param edit (base value, or a key at the playhead when `keyIt`); one StagesCommand per gesture.
    void StudioStageParamEdit(uint32_t stageUid, const StageParam& param, int comp, float value, bool keyIt);
    void StudioBeginStageEdit();
    void StudioEndStageEdit(const char* name);
    std::vector<studio::StudioStage> studioStageBefore_;   // a stage panel edit / drag started from this list
    bool studioStageDragging_ = false;
    // S1b: the variant of the selected / given MMDX stage model (inspector combo); pushes one undoable step.
    void StudioSetStageVariant(int model, const std::string& variantId);
    std::set<uint32_t> studioStageOpen_;               // outliner stage rows the user opened (parts shown; not saved)
    void StudioViewportLightHandles(bool hovered);
    bool StudioScriptLightGizmoPoint(const std::string& part, ImVec2& out) const;
    LightAnchors StudioBuildLightAnchors() const;
    void DrawStudioLightOutliner();
    void DrawStudioLightInspector(float w);
    void DrawStudioLightPresetConfirm();
    void DrawStudioStopwatchConfirm();  // "애니메이션 끄기" modal (pending while studioStopwatchOff_ is set)
    void StudioBeginLightEdit(uint32_t uid, bool keyEdit);
    void StudioEndLightEdit(uint32_t uid);

    void StudioUpdateCameraPath();                   // samples the motion camera (cached per cameraVersion)
    void DrawStudioCameraPath(float x0, float y0, float x1, float y1);
    bool StudioPickCameraKey(ImVec2 mouse);          // click on a key dot of the path: select it and seek
    void StudioCameraPathWindow(int& lo, int& hi) const;  // frames of the path drawn: the range, else now +-3 s
    // --- pose editing (UiStudioPose.cpp): bone overlay, picking, gizmo, pose layer, morph panel, VPD, mirror
    studio::StudioModel* StudioPoseModel();          // the selected model (pose editable: any kind)
    void StudioSelectBone(int bone, bool toggle);    // viewport pick / bone row click: selection sync (-1 clears)
    void StudioSelectMorph(int morph);               // morph slider / stopwatch: select the morph's timeline row and reveal it
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
    bool StudioApplyPose(studio::StudioModel& m);    // writes the pose layer over the evaluated motion (UpdateStudioScene); true = applied
    bool StudioGizmoFrameOf(const studio::StudioModel& m, int bone, studio::GizmoFrame& f, studio::GizmoMode& mode) const;
    void StudioViewportPose(float x0, float y0, float x1, float y1, bool hovered, bool& consumed);  // overlay + input
    void StudioViewportToolbar(float x, float y);
    void DrawStudioViewportAxisControls(float x0, float y0, float x1, float y1);  // bottom-corner move/rotate XYZ for the active bone
    void DrawStudioWindOverlay(const float rr[4]);  // viewport wind-direction arrow (camera-aligned)
    void DrawStudioBoneTab(float w);
    void DrawStudioMorphTab(float w);
    bool StudioScriptGizmoPoint(int part, ImVec2& out) const;  // ui-script: a screen point on a gizmo part

    // --- projects, adding/removing models, props, audio, autosave (UiStudioProject.cpp)
    enum class StudioAction { None, Leave, New, Open, OpenFile };  // what the unsaved-changes prompt guards
    void StartStudioEmpty();                          // new empty project: camera/light tracks only (no loading)
    void StartStudioProjectLoad(const std::filesystem::path& file, bool recovery);  // -> Loading -> Studio
    void StartStudioPmmImport(const std::filesystem::path& file);  // .pmm import -> a NEW unsaved project
    void StudioRequest(StudioAction a, const std::filesystem::path& file = {});  // prompts first when dirty
    void StudioRunAction(StudioAction a, const std::filesystem::path& file);
    bool StudioSave(bool saveAs);                     // dialog when untitled or saveAs; false: cancelled / failed
    bool StudioSaveTo(const std::filesystem::path& file);
    studio::ProjectData StudioProjectData() const;    // snapshot of the document (copies the motions)
    std::filesystem::path StudioRecoveryFile() const; // <exe>/recovery/autosave.mmdxproj
    void StudioAutosave(bool force, bool wait);       // timer: writes the recovery file on a worker thread
    void StudioDiscardRecovery();                     // waits for a running autosave, deletes the recovery files
    // replaceUid != 0: the loaded model replaces that model's file (StudioReplaceModel) instead of being added.
    // `material` (non-null) overrides the resolved material choice for the loaded model (P5: a material-source change
    // reloads the model's PBR data without touching the saved choice again).
    // `queuedDrops` (drag and drop): VMD/VPD files meant for this model, applied once it finishes loading.
    // `retryQueuedDrops`: when the load fails, these are handed to the next job added, so the motion still lands.
    void StudioAddModelFile(studio::ModelKind kind, const std::filesystem::path& file, uint32_t replaceUid = 0,
                            const MaterialChoice* material = nullptr,
                            std::vector<std::filesystem::path> queuedDrops = {}, size_t retryQueuedDrops = 0);  // async
    // M5c: an MME effect object (an accessory `.x` + its same-stem `.fx`, or a bare `.fx`): a prop row with the
    // accessory key track, its effect file saved as the project model's "fx". The dialog filters `.x` / `.fx`.
    void StudioAddMmeObjectDialog();
    void StudioAddMmeObject(const std::filesystem::path& path);
    void StudioAddModelDialog(studio::ModelKind kind, uint32_t replaceUid = 0);    void StudioAddLibraryCharacter(int index, uint32_t replaceUid = 0);
    void StudioAddLibraryStage(int index, uint32_t replaceUid = 0);  // replace: the first part replaces, the rest are added
    void StudioApplyLibrarySong(int index);           // dance -> selected character, camera, audio when none
    bool StudioSetAudio(const std::filesystem::path& file);  // empty: remove
    // Background AVI (背景AVI): set / clear the studio's background video (empty = none). Bumps projectVersion.
    bool StudioSetBgVideo(const std::filesystem::path& file);
    void StudioAudioDialog();
    void StudioPollJobs();                            // finished loads: GPU upload + append (main thread)
    // --- drag and drop (AppDrop.cpp): WM_DROPFILES / the ui-script "drop" command -> the Studio
    void PollDroppedFiles();                          // every frame: consume droppedFiles_ (shader packs keep their screens)
    // The studio model kind a dropped model file becomes: `.x` -> prop; a PMX/PMD the library classified as a stage
    // part -> stage; everything else -> character.
    studio::ModelKind DropModelKind(const std::filesystem::path& file, const std::string& ext) const;
    void ApplyStudioDrop(std::vector<std::filesystem::path> files);  // route a drop into the open studio project
    void ApplyQueuedDrops(int modelIndex, const std::vector<std::filesystem::path>& files);  // VMD/VPD behind a model load
    void StudioRemoveModel(int index);                // waits for the GPU; clears the undo history
    // Swaps model `index`'s file for `pm` and keeps everything else (motion keys by name, placement, prop parent, shader,
    // visibility, uid). Waits for the GPU; clears the undo history (pose edits hold bone indices). False: GPU upload failed.
    bool StudioReplaceModel(int index, studio::StudioPackageModel& pm, std::string* note);
    void StudioClearEditState();                      // undo history, selection, clipboard, drags (model list changed)
    void StudioSelectModel(int index);                // -1 camera
    // --- S4: camera entities (uitudio camera outliner / project / script)
    void StudioSelectCamera(uint32_t uid);            // 0 = the main camera; also selects the camera target
    void DrawStudioCameraOutliner();                  // the camera rows under the outliner's 장면 group
    void DrawStudioCameraRename();                    // the camera rename modal (outliner double click / menu)
    void DrawStudioCameraGizmos(float x0, float y0, float x1, float y1);  // frustums of the other cameras
    void StudioAddCameraFromView();                   // new extra camera from the current view (undoable)
    void StudioImportStageCameras();                  // one static-key camera per stage.json camera (undoable)
    // A freshly added / opened character with no shader choice: SuggestShaderPack for its library id (never applies).
    void StudioSuggestShader(studio::StudioModel& m);
    DirectX::XMFLOAT4X4 StudioPropRoot(const studio::StudioModel& m) const;  // world matrix of a prop's origin
    // The world matrix a prop placement `a` hangs from: the parent bone's world (x the parent's display scale), else
    // the identity (world / missing parent / the prop itself).
    DirectX::XMMATRIX StudioPropParentWorld(const studio::StudioModel& m, const studio::PropAttach& a) const;
    // `to` with its offset recomputed so the prop stays where `from` put it in the world at the current frame
    // (re-parenting keeps the prop in place instead of jumping by the new bone's position).
    studio::PropAttach StudioKeepWorldAttach(const studio::StudioModel& m, const studio::PropAttach& from,
                                             studio::PropAttach to) const;
    void StudioSeekAudio();                           // audio cursor <- timeline time (audio offset)
    void DrawStudioAddMenu();                         // the outliner's "+" popup (library / file)
    void DrawStudioModelMenu(int index);              // outliner row context menu
    void DrawStudioAudioMenuItems();                  // the audio menu's items (outliner audio row, timeline audio lane)
    void StudioCommitAudioOffset(double from, double to);  // one undo step (the offset is already `to`), re-seeks
    // --- physics bake (P2.2, UiStudioBake.cpp): turn a character's live physics into bone keys
    void StudioOpenBakeDialog();                      // model menu: open the options modal (range / step / bones / off)
    bool StudioStartBake(int start, int end, int step, bool selectedOnly, bool turnOff);  // start the worker / script
    void StudioPollBake();                            // every frame: finish the worker, apply one undo step
    void StudioBakeApply(const studio::BakeResult& result, int start, int end, bool turnOff);  // main thread apply
    void DrawStudioBakeDialog();                      // the options / progress modal
    std::future<bool> studioBakeFuture_;
    studio::BakeResult studioBakeResult_;             // worker result (read on the main thread after the future is ready)
    std::atomic<bool> studioBakeCancel_{false};       // cancel request (worker + progress modal)
    std::atomic<float> studioBakeProgress_{0.0f};
    bool studioBakeRunning_ = false;
    int studioBakeModel_ = -1;                        // model index the running bake belongs to
    int studioBakeStart_ = 0, studioBakeEnd_ = 0, studioBakeStep_ = 1;
    bool studioBakeTurnOff_ = true;                   // "physics off after baking" of the running / pending bake
    double studioBakeStartWall_ = 0;                  // for "is it taking a moment" (progress modal)
    // options modal fields
    bool studioBakeDialogOpen_ = false;
    int studioBakeOptStart_ = 0, studioBakeOptEnd_ = 0, studioBakeOptStep_ = 1;
    bool studioBakeOptSelectedOnly_ = false, studioBakeOptTurnOff_ = true;
    // --- arm rest-pose correction (UiStudioRestPose.cpp): T-pose-authored VMDs onto an A-pose model
    void StudioOpenRestPoseDialog();          // model menu / ribbon: manual correction of the selected character's motion
    void DrawStudioRestPoseDialog();
    bool StudioApplyRestPose(float sourceArmDeg, bool left, bool right);  // manual path, one undo step; script too
    // The modal's primary / secondary button: auto mode merges the pending motion (retargeted when `accept`),
    // manual mode applies the current settings when `accept`. Closes the modal; false when it was not open.
    bool StudioRestPoseAnswer(bool accept);
    bool studioRestPoseOpen_ = false;
    bool studioRestPoseAuto_ = false;         // opened by the VMD import detection (pending motion below)
    float studioRestPoseSourceDeg_ = 0.0f;
    bool studioRestPoseSide_[2] = {true, true};
    studio::ArmRestGuess studioRestPoseGuess_;
    studio::MotionData studioRestPosePending_; // auto mode: the imported (canonicalised) motion not merged yet
    std::filesystem::path studioRestPosePendingPath_;
    int studioRestPoseModel_ = -1;
    int studioRestPoseAnswer_ = 0;            // ui-script answer pending: 1 = primary, 2 = secondary
    void DrawStudioPropPanel(float w);                // inspector: prop parent / bone / offset
    void DrawStudioUnsavedPrompt();
    void DrawRecoveryPrompt();                        // select screen: offer the autosave of a crashed session

    // --- rendering the project, shortcut help (UiStudioRender.cpp)
    // Video (settings_.video through the render dialog) or GI still of the current frame. The video covers
    // StudioRenderRange through the motion camera with the project's audio; the studio comes back as it was.
    void StartStudioRender(bool video);
    void StudioRenderRange(double& startSeconds, double& endSeconds) const;  // CLI range, else timeline range / whole
    void StudioPoseForRender(float frame);            // offline frames: every model at `frame` (physics steps forward)
    void StudioRestoreAfterRender();                  // FinishOffline: time, camera mode, physics back to the editor's
    DirectX::XMFLOAT3 StudioPerformerCenter() const;  // center bone of the first character (teleport detection)
    void DrawStudioEffectsPanel(float x0, float y0, float x1, float y1);  // the "효과" panel: screen effects + effect stack
    // Shared keyable typed params of a studio effect / world (key diamonds at the playhead; `kind` = EffectParam /
    // WorldParam). True = a static value or a key changed this frame.
    bool DrawKeyableParams(const ShaderPack& pk, studio::StudioEffect& fx, uint32_t uid, studio::RowKind kind,
                           const char* idPrefix);
    void DrawStudioHelp();                            // shortcut overlay (? key / top bar button)
    bool StudioModal() const { return videoDialogOpen_ || studioHelpOpen_ || studioLeaveConfirm_ || studioLightPresetPending_ >= 0 ||
                                       studioStopwatchOff_ || studioBakeDialogOpen_ || studioBakeRunning_ ||
                                       studioRestPoseOpen_; }

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
    // Bulk key tools (AppMcpKeys.cpp); true when it handled the tool.
    bool ExecuteMcpKeys(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Layered motion-editing tool (AppMcpMotion.cpp); true when it handled the tool.
    bool ExecuteMcpMotion(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Stiff-motion diagnosis (AppMcpMotionQuality.cpp); true when it handled the tool.
    bool ExecuteMcpMotionQuality(const std::string& tool, const nlohmann::json& args,
                                 std::shared_ptr<McpPromise> promise);
    // Read-only analysis tools (AppMcpAnalysis.cpp); true when it handled the tool.
    bool ExecuteMcpAnalysis(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Camera-direction tools (AppMcpCamera.cpp); true when it handled the tool.
    bool ExecuteMcpCamera(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Writes a camera key list (studio_set_camera_keys' behaviour). False + error when it rejects the input.
    bool McpWriteCameraKeys(const nlohmann::json& keys, const std::string& mode, size_t& changed, std::string& error,
                            bool pushUndo = true);
    // Framing evaluation of one frame/pose (one element of studio_frame_check's frames array).
    nlohmann::json McpFrameCheck(int frame, const CameraPose& pose);
    // AI lighting tools (AppMcpLight.cpp); true when it handled the tool.
    bool ExecuteMcpLight(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Perception tools (AppMcpPerception.cpp): contact sheet + singing analysis.
    bool ExecuteMcpPerception(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    void AdvanceMcpContactSheet();  // one rendered frame of a running contact sheet (PumpMcp)
    // Cached beat analysis of the project's audio (AppMcpAnalysis.cpp); nullptr with `error` set when unavailable.
    const BeatAnalysis* StudioBeats(std::string& error);
    // Poses `m` at each frame (bound motion, placement, no pose layer, no physics) and reads the bones, then restores
    // the current frame. Rows are per frame; columns follow `bones`.
    std::vector<std::vector<DirectX::XMFLOAT3>> StudioSampleBones(studio::StudioModel& m, const std::vector<int>& frames,
                                                                 const std::vector<int>& bones,
                                                                 std::vector<float>* facingYawDeg = nullptr);
    // Like StudioSampleBones, but the rows hold each bone's world matrix (BoneWorld) instead of its position.
    std::vector<std::vector<DirectX::XMFLOAT4X4>> StudioSampleBoneWorlds(studio::StudioModel& m,
                                                                       const std::vector<int>& frames,
                                                                       const std::vector<int>& bones);
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
    int CliComponentE2e();       // Run(): headless component fetch for tools/components_e2e.py
    void UpdateComponentUi();    // every frame: drains component_
    void DrawFontComponentRow(float width);              // lobby settings: offer the CJK fonts
    void DrawUpscalerComponentRow(UpscalerKind kind, float width);  // upscaler picker: download hint
    bool HeadlessRun() const;    // --frames / --ui-script / benchmarks / offline renders

    // --- thumbnails (AppThumbnails.cpp)
    std::filesystem::path ThumbnailCacheDir() const;
    bool RenderThumbnail(ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out);
    uint64_t CharacterThumb(int index);  // ImTextureID, 0 while not ready
    uint64_t StageThumb(int index);

    // --- free camera helpers (UiPlay.cpp)
    void UpdateFreeCamera();
    void SetPlayMotionCamera(bool on);   // C / the play bar: leaving the motion camera keeps its current view

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
    // Hands one finished video image to the async sink (rendered order), updates the timings and the preview.
    void EncodeOfflineImage(ImageRGBA8&& img, double renderSeconds);
    void FinishOffline(bool cancelled);  // Offline -> Play (paused); closes the encoder; toast
    void DrawOfflineOverlay();  // progress panel over the preview (screen_ == Offline)
    void DrawVideoRenderDialog();  // video render settings + start (Select, UiVideoDialog.cpp)
    void ResetVideoRange();        // whole song + empty text fields (called where the dialog / song changes)
    // formation modal (UiFormation.cpp)
    struct FormationSlot {
        std::string characterId;
    };
    struct FormationState {
        bool open = false;
        LoadTarget target = LoadTarget::Play;
        int songIndex = -1;
        int centerIndex = 0;
        std::vector<FormationSlot> slots;
        int pickerSlot = -1;
        char pickerSearch[64] = {};
        bool justOpened = false;   // set by OpenFormationModal: keys of that frame belong to the lobby
    };
    FormationState formationModal_;

    void StartLobbyLoad(LoadTarget target, bool allowModal = true);
    SongAsset SongAsChosen(const SongAsset& song) const;   // with the camera / audio / stage picked in the lobby (songs with several)
    // M8b: the song's video layers (docs/mme/mme_design.md §5b), applying the songVideo ini override (kind + blend
    // + a chosen file) over the scan's classification. Empty when the song has no videos / the override is "off".
    std::vector<VideoLayerDesc> ResolveSongVideos(const SongAsset& song) const;
    // M8b: the videos a song would play (classification + override), for the lobby song header line.
    std::vector<VideoLayerDesc> SongVideosForDisplay(const SongAsset& song) const { return ResolveSongVideos(song); }
    // M8a: the stage to load for a song. Precedence: the run's --stage (already in selStage_) > the song's songStage
    // ini choice > the song's bundled stage (when the user has not chosen one) > the library selection.
    const StageAsset* ResolveSongStage(const SongAsset& song) const;
    void OpenFormationModal(LoadTarget target);
    void DrawFormationModal();
    void AutoFormation();
    void StartFormationLoad();
    std::vector<PerformerCast> ParseFormationSpec(const std::string& spec, const SongAsset& song);

    void DrawToast();           // completion / error toast (Select, Play)
    // shader packs (UiShaderPack.cpp)
    void ApplyShaderChoice(GpuModel& gpu, const ShaderChoice& choice);   // every frame, before UpdateMaterials
    // M6b: the per-model MME object effect choice editor (없음 / 모델 동봉 효과 / 파일 선택…). Shared by the lobby
    // character panel, the play bar's shader popup and the studio inspector. True = the choice changed.
    bool DrawMmeRow(const char* id, MmeChoice& choice, const std::string& modelId,
                    const std::filesystem::path& modelPath, float width);
    // M6b: one-time suggestion when a character with no saved MME choice ships an .emd / same-name .fx (a toast
    // with an apply button; remembered per character id). Never applies it automatically.
    void SuggestMme(const std::string& characterId, const std::filesystem::path& modelPath);
    // M6a: the per-model MME object effect choice (player + studio). Resolves the global switch / workflow, the saved
    // choice (or the run-wide CLI overrides: --mme-auto for every model, --stage-mme for stages) and calls
    // gpu.SetMmeEffects. `isStage` picks the stage default / store. `modelPath` drives the Auto workflow (it decides
    // whether a character can take an MME object effect at all). Applied right before UpdateMaterials.
    MmeChoice ResolveMmeChoice(const std::string& id, bool isStage, const std::filesystem::path& modelPath = {}) const;
    // The same resolution for an explicit saved choice (a studio project model's own MmeChoice): the global switch /
    // workflow / CLI overrides layered on top. Nothing here reads settings_, so a project choice stays authoritative in
    // Manual mode.
    MmeChoice ResolveMmeChoiceFor(const MmeChoice& saved, bool isStage,
                                  const std::filesystem::path& modelPath = {}) const;
    // Applies a resolved choice: None = native, Auto = the model's own .emd / same-name .fx ([fx]), File = one effect
    // for every material. `modelPath` drives Auto (FindModelEffects). `autoWorkflow` = the Auto choice came from the
    // Auto workflow (완전 자동) and not from a run-wide CLI override: then an Auto assignment whose effect needs an
    // external controller object that is not in the scene resolves to native shading for the whole model. Never throws.
    void ApplyMmeEffect(GpuModel& gpu, const MmeChoice& choice, const std::filesystem::path& modelPath,
                        bool autoWorkflow = false);
    // True when a resolved Auto choice comes from the Auto workflow itself (완전 자동) and not from a run-wide CLI
    // override (--mme-auto / --stage-mme / --mme-model): only then does the required-controller skip apply.
    bool MmeAutoSkipApplies(const MmeChoice& mme, bool isStage) const;
    // The lower-case file / display names the CONTROLOBJECT lookup (render/PassMmeOffscreen.cpp MmeSceneLookup) would
    // match against: the loaded play scene (character, extras, stages, effect objects) or the studio project's models.
    // Cached per scene / studio model set (count + pointers), never re-read per frame.
    const std::vector<std::string>& MmeSceneNames();
    // The required controller a model's Auto assignment needs but the scene lacks (lower case), cached by
    // (model path, scene-names key); "" when nothing is missing / the model ships no such effect. Shared by the Auto
    // skip and the UI caption (both the Auto-skipped row and the Manual "모델 동봉 효과" warning).
    std::string MmeControllerSkip(const std::filesystem::path& modelPath);
    // The controller an Auto workflow assignment skipped for this model path (ui: the "자동 제외" caption); empty when
    // the model was not skipped. Keyed by (model path, scene-names key).
    std::map<std::string, std::string> mmeAutoSkip_;
    // Auto workflow (완전 자동), play scenes only: when a required controller is missing but its file exists in the
    // library (FindMmeControllerFile, non-Ray), queue its load for this scene (RequestMmeController); the effect
    // applies from the frame after PollMmeControllers loaded it. Never queued twice (key = lower-case file name).
    void RequestMmeController(const std::filesystem::path& modelPath, const std::string& skip);
    // Loads queued controller models synchronously, once per play-scene frame before the models' MME choices are
    // applied. A failed load is LOG_WARN'd once and marked failed (never retried).
    void PollMmeControllers();
    // The queued (not yet loaded) controller files + the requester that needed them, and the failed ones, this scene.
    std::vector<std::pair<std::filesystem::path, std::string>> mmeControllerQueue_;   // (file, requester path utf8)
    std::set<std::string> mmeControllerQueued_;    // queued / loaded file names (lower case), this scene
    std::set<std::string> mmeControllerFailed_;    // failed file names (lower case), never retried
    // The Auto resolution ApplyMmeEffect builds from the model's files on disk (per-material paths + hidden +
    // ray-skipped), keyed by (model path, material count) and reused for up to 1.0 s so an unchanged scene does no
    // filesystem work per frame (a .emd edit still applies while running).
    struct MmeAutoResolved {
        std::vector<std::filesystem::path> perMaterial;
        bool hidden = false;
        size_t raySkipped = 0;
        std::chrono::steady_clock::time_point at;
    };
    std::unordered_map<std::string, MmeAutoResolved> mmeAutoResolved_;
    std::set<std::string> mmeSkipLogged_;   // (model path utf8 | controller) reasoned once
    std::vector<std::string> mmeSceneNames_;        // MmeSceneNames cache
    std::string mmeSceneNamesKey_;                  // inputs the cache was built from
    std::set<std::string> mmeHiddenWarned_;   // models whose .emd asked to hide (one [W] each)
    std::string mmeSuggestApply_;             // the character id the MME suggestion toast's apply button would set
    // M6b: the studio project's MME post stack as RenderSettings entries (empty outside the studio / without a project).
    std::vector<MmeEffectEntry> StudioMmeEntries() const;
    // Studio project effect stack: evaluated entries for the frame (empty outside the studio).
    std::vector<EffectStackEntry> StudioEvaluatedEffects(float frame) const;
    // World entities: the cast's collider sources and the background track build (play + studio).
    std::vector<CharacterSource> PlayColliderCast() const;
    std::vector<CharacterSource> StudioColliderCast() const;
    void PumpColliderTrack();
    void StudioPumpColliders();   // debounced rebuild of the studio track while the project has worlds
    // M6b: applies a studio MME stack change (add / remove / toggle) to the project and the renderer.
    void StudioMmeChanged();
    // P5: the per-character PBR material source. The global Ray-MMD switch off forces Default; the Auto workflow picks
    // Ray for a model with a Ray .emd candidate (Default otherwise); --material-source wins over every mode.
    MaterialChoice ResolveMaterialChoice(const std::string& characterId, const std::filesystem::path& modelPath = {}) const;
    // The same resolution over an explicit saved map / switch / workflow (the MME panel's draft comparison). `enabled`
    // is the Ray-MMD switch (--ray-global 0 wins over everything).
    MaterialChoice ResolveMaterialChoiceFrom(const std::map<std::string, MaterialChoice>& materials, bool enabled,
                                             bool autoWorkflow, const std::string& id,
                                             const std::filesystem::path& modelPath) const;
    MaterialChoiceFn MaterialChoiceResolver() const {
        return [this](const std::string& id, const std::filesystem::path& path) {
            return ResolveMaterialChoice(id, path);
        };
    }
    // The model's .emd candidates (asset/RayMaterial.h), cached per key (character id / library id / "<path>") for
    // the session so the detail panel never repeats the ~10 ms scan. Empty = none found.
    const std::vector<RayEmdCandidate>& MaterialCandidates(const std::string& key,
                                                           const std::filesystem::path& modelPath, int pmxMaterialCount) const;
    // P6: does the imported model carry PBR data (asset/ModelImport.h ModelHasImportedPbr)? Cached per key like
    // MaterialCandidates; false for PMX/PMD.
    bool ImportedPbrCandidates(const std::string& key, const std::filesystem::path& modelPath);
    // The shared "재질" widget (Default / Ray-MMD + candidate combo + file picker). Shown only when the model has a
    // Ray candidate or an explicit .emd is saved. Writes `choice`; true = changed. `modelPath`/`count` come from the
    // model (PMX or a loaded-only copy); `cacheKey` names the candidate cache entry.
    bool HasMaterialOptions(const std::string& cacheKey, const std::filesystem::path& modelPath, int pmxMaterialCount,
                            const MaterialChoice& choice);
    bool DrawMaterialSelector(const char* id, MaterialChoice& choice, const std::string& cacheKey,
                              const std::filesystem::path& modelPath, int pmxMaterialCount, float width);
    // P5: the material count of a model file, read once and cached per key (the lobby stage selector needs it; a
    // stage carries none). A cheap PMX/PMD header probe; an imported model with PBR data answers non-zero.
    int ModelMaterialCount(const std::string& key, const std::filesystem::path& modelPath);
    // One toast per character id when a character with no saved material choice has a Ray .emd candidate (never
    // applies). Imitates SuggestShaderPack; skipped in headless / scripted runs and with --material-source.
    void SuggestRayMaterial(const std::string& characterId, const std::filesystem::path& modelPath, int pmxMaterialCount);
    mutable std::map<std::string, std::vector<RayEmdCandidate>> materialCandidates_;   // session cache (see MaterialCandidates)
    mutable std::mutex materialCandidatesMutex_;        // MaterialCandidates is reachable from loader worker threads (Auto)
    std::map<std::string, bool> importedPbrCandidates_;                         // P6 session cache (ImportedPbrCandidates)
    std::map<std::string, int> materialCountCache_;                            // session cache (ModelMaterialCount)
    std::map<std::string, bool> materialSuggestChecked_;                        // ids checked this session;
    // Play character: the saved settings choice or the --shader-pack override. A character with no saved choice also
    // gets SuggestShaderPack (a toast only; the choice is never set by the app).
    ShaderChoice PlayShaderChoice(const GpuModel& gpu, const std::string& characterId);
    // Applies the look pack `id` to settings_ (and the centre character when a scene is loaded). `persist` saves the
    // ini like a UI change (ui-script); the CLI one-shot never persists and is reverted on exit. False = no such look.
    bool ApplyLookById(const std::string& id, bool persist);
    // Writes a look pack from the current settings into ShaderPacks().UserRoot() / id.
    bool ExportLook(const std::string& id, const std::string& displayName);
    bool cliLookActive_ = false;      // --look: applied to this run only, reverted before the ini is saved
    LookData cliLook_;                // the CLI look
    LookData cliLookPrev_;            // the settings' groups before it
    std::string cliLookCharacterId_;  // the scene character its surface group touched
    ShaderChoice cliLookPrevSurface_; // that character's choice before it
    // Shows ONE toast per (installed pack that Suggests the model's traits, character id), remembered in
    // AppSettings::characterShaderSuggested. Skipped in headless / scripted runs and with --shader-pack. The first
    // suggesting pack in registry order wins. Cheap to call every frame (checked ids are cached per session).
    void SuggestShaderPack(const GpuModel& gpu, const std::string& characterId);
    std::set<std::string> suggestChecked_;   // character ids SuggestShaderPack already evaluated this session
    // A row showing the chosen pack; click opens a searchable pack list (+ "셰이더 관리"). True = changed.
    // `traits` = the shown model's data traits (null = unknown, e.g. the lobby): packs that Suggest them get a "추천" tag.
    bool DrawShaderSelector(const char* id, ShaderChoice& choice, float width, const ModelTraits* traits = nullptr);
    bool DrawShaderPackParams(ShaderChoice& choice);                     // the chosen pack's sliders + reset
    // typed param widgets (float / int / bool / color / vec3; color + vec3 as key#0..2 entries) under collapsible
    // pack.json groups (open state in AppSettings::paramGroupOpen); `grid` = up to 3 columns in wide panels
    bool DrawParamGroupHeader(const ShaderPack& pack, const ShaderPackGroup& g, const char* idPrefix);
    bool DrawShaderPackParameters(const ShaderPack& pack, std::map<std::string, float>& params,
                                  const char* idPrefix = "##spp", bool grid = false);
    // per-material class / default-shading list of a pack model (`maxHeight` caps the scrolling list)
    bool DrawMaterialShaderList(const char* id, ShaderChoice& choice, const PmxModel& pmx, float width, float maxHeight);
    static int MaterialOverrideCount(const ShaderChoice& choice);
    std::set<int> materialSel_;                    // DrawMaterialShaderList: selected rows (Ctrl / Shift + click)
    std::string materialSelOwner_;                 // the list the selection belongs to
    int materialSelAnchor_ = -1;
    bool playMaterialsOpen_ = false;               // play bar shader popup: the material list is expanded
    bool lobbyMaterialsOpen_ = false;              // library shader tab: the material list is expanded
    std::string lobbyMatId_;                       // character whose model the list below was loaded for
    std::string lobbyStageManifestId_;              // MMDX stage whose variant deck the lobby panel read
    std::shared_ptr<const StageManifest> lobbyStageManifest_;   // the selected MMDX stage's manifest (null: classic)
    std::future<std::shared_ptr<PmxModel>> lobbyMatFuture_;
    std::shared_ptr<PmxModel> lobbyMatPmx_;
    enum class PackCompareMode { Drag, Hover };
    uint64_t PackThumb(const ShaderPack& pack);                          // preview texture, 0 = none / loading
    uint64_t PackThumbBefore(const ShaderPack& pack);
    uint64_t RemotePackThumb(const RemotePack& pack);
    uint64_t RemotePackThumbBefore(const RemotePack& pack);
    void DrawPackImage(ImDrawList* dl, uint64_t tex, const std::string& key, ImVec2 a, ImVec2 b, float rounding,
                       ImDrawFlags flags = 0);                           // cover-fit, placeholder when tex == 0
    void DrawPackCompare(const char* id, ImDrawList* dl, uint64_t before, const std::string& beforeKey, uint64_t after,
                         const std::string& afterKey, ImVec2 a, ImVec2 b, float rounding, ImDrawFlags flags,
                         PackCompareMode mode);
    // shader pack manager (UiShaders.cpp)
    void DrawShaders();
    // MME / Ray-MMD settings screen (UiMme.cpp, Screen::Mme, top tab "MME"): the global switch, the workflow, the
    // MME post stack and the per-model list. The play-mode panel is the MME tab of DrawPlayFxPanel (UiPlayPanel.cpp).
    void DrawMme();
    // The resolved global MME switch / workflow (the run's CLI override wins over the settings). With the switch off
    // every MME object / post / effect-object feature falls back to its off state.
    bool MmeGlobalEnabled() const { return options_.mmeGlobal >= 0 ? options_.mmeGlobal != 0 : settings_.mmeEnabled; }
    // The resolved global Ray-MMD material-source switch (CLI --ray-global wins). Off = every model's material source
    // resolves to Default.
    bool RayMmdGlobalEnabled() const { return options_.rayGlobal >= 0 ? options_.rayGlobal != 0 : settings_.rayMmdEnabled; }
    bool MmeAutoWorkflow() const { return options_.mmeWorkflow >= 0 ? options_.mmeWorkflow == 1 : settings_.mmeWorkflow == 1; }
    // True when a run-wide CLI override makes the saved MME switch / workflow not what this run uses (the UI shows a
    // "명령줄 설정이 우선합니다" hint).
    bool MmeCliOverrideActive() const {
        return options_.mmeGlobal >= 0 || options_.rayGlobal >= 0 || options_.mmeWorkflow >= 0;
    }
    // The material source the renderers use for a model: the saved / auto choice, forced to Default while the Ray-MMD
    // switch is globally off.
    MaterialChoice EffectiveMaterialChoice(const std::string& id) const {
        if (!RayMmdGlobalEnabled()) return MaterialChoice{};
        return ResolveMaterialChoice(id);
    }
    void DrawShaderPackDetail(float x0, float y0, float x1, float y1);    // M6b: the shader manager's MME tab detail pane (compile status per pass, unsupported features, missing textures).
    void DrawMmeDetail(float x0, float y0, float x1, float y1);
    void DrawRemotePackDetail(float x0, float y0, float x1, float y1);
    void DrawNewPackDialog();
    void DrawGallerySourcesDialog();
    void DrawUnverifiedInstallDialog();
    void RefreshGallery();
    std::vector<std::string> GalleryIndexUrls() const;
    bool PackUsable(const ShaderPack* pack) const;   // Selectable() and not crash-guard quarantined
    void InstallPackPath(const std::filesystem::path& path);             // .zip or a pack folder (drop / dialog)
    void PollShaderPacks();                                              // every frame: hot reload, store, drops
    // The effect stack editor (UiShaders.cpp): add / remove / reorder (up/down) / enable + per-effect
    // sliders. Edits `stack` in place; true = changed. `widths` = the content width for layout.
    bool DrawEffectStackEditor(std::vector<EffectStackEntry>& stack, bool header = true);
    // M6b: the ordered MME post stack editor (play-mode settings_.mmeEffects in the play bar / shader manager; the
    // studio calls DrawStudioMmeStackEditor for its project stack). True = the stack changed.
    bool DrawMmePostStackEditor(std::vector<MmeEffectEntry>& stack, bool header = true);
    bool DrawStudioMmeStackEditor();                                     // the studio project's own MME post stack
    // Ray-MMD main effect (ray.x / ray.fx) guard: an MME add site calls GuardRayMme before touching its stack; when it
    // returns true the path is queued and the confirm modal (DrawRayMmeConfirm) offers "add anyway", which runs the
    // stored continuation. False = not a Ray-MMD main effect, add as before.
    bool GuardRayMme(const std::filesystem::path& p, std::function<void(const std::filesystem::path&)> add);
    void DrawRayMmeConfirm();                                            // the shared confirm modal
    std::filesystem::path rayConfirmPath_;   // pending Ray-MMD main effect (empty = modal closed)
    std::function<void(const std::filesystem::path&)> rayConfirmAdd_;   // the site's "add" continuation
    bool shaderParamsOpen_ = false;                                      // select screen: pack settings disclosure
    bool effectStackOpen_ = false;                                       // select screen: effect stack disclosure
    int shaderDetailTab_ = 0;                                            // shader detail: 0 info, 1 settings, 2 manage
    ShaderPackStore shaderStore_;
    bool gallerySourcesOpen_ = false;
    char galleryUrl_[512] = {};
    std::string galleryUrlError_;
    std::string pendingUnverifiedInstall_;   // remote pack id waiting in the unverified install modal
    PackCrashGuard packGuard_{ExecutableDir() / L"pack_guard.json"};
    std::set<std::string> frameActivePacks_;   // non-BuiltIn packs active in the previous frame
    std::vector<std::string> frameBeganPacks_; // Begin(id) issued this frame (Clear after Present)
    void CollectFrameActivePacks(std::set<std::string>& out) const;
    std::string pendingGuardToast_;   // startup crash-guard report, shown once the clock is running

    int shaderTab_ = 0;                                                  // 0 installed, 1 online, 2 MME effects
    char shaderFilter_[128] = {};
    uint32_t shaderPackWriteTimesGen_ = 0;
    std::unordered_map<std::string, std::filesystem::file_time_type> shaderPackWriteTimes_;
    std::string shaderSelInstalled_, shaderSelRemote_;
    // Stage gallery (UiGallery.cpp, Screen::Gallery, top tab "갤러리"): the online stage store and its selection.
    StageStore stageStore_;
    char galleryFilter_[128] = {};
    std::string gallerySel_;
    bool galleryIndexApplied_ = false;   // the --gallery-index / default index was started this run
    void DrawGallery();
    void DrawGalleryDetail(float x0, float y0, float x1, float y1);
    void RefreshStageGallery();
    uint64_t RemoteStageThumb(const RemoteStage& stage);
    void PollStageStore();                     // every frame: index fetch, previews, installs -> toasts
    // True while the stage folder of `id` (a "stages:" library id or a bare folder name) is used by a scene / studio.
    bool StageInUse(const std::string& id) const;
    // The background library rescan that makes a freshly installed stage appear in the lobby's stage list.
    void RefreshLibraryForStage();
    // M6b: shader manager MME tab (docs/mme/mme_design.md §2.13). The selected library effect and its async
    // diagnostics compile (spinner while running).
    std::string shaderSelMme_;
    MmeDiagJob mmeDiagJob_;
    MmeEffectDiag mmeDiag_;
    std::string mmeDiagFile_;   // the file mmeDiag_ describes
    bool newPackOpen_ = false, newPackEffect_ = false;
    char newPackId_[64] = {}, newPackName_[96] = {}, newPackAuthor_[96] = {};
    std::vector<std::filesystem::path> droppedFiles_;
    std::vector<std::filesystem::path> pendingDrop_;  // a drop that arrived while scanning / loading: applied at the lobby
    void DrawStudioShaderRow(float w);                                   // inspector: the selected character's pack
    void DrawStudioMaterialRow(float w);                                 // inspector: the selected model's material source
    void DrawStudioMmeRow(float w);                                      // M6b inspector: the selected model's MME choice
    void CheckShaderPackErrors();                                        // compile failures -> error toast
    uint32_t shaderPackErrorsSeen_ = 0;
    std::filesystem::path OfflineOutputDir(bool video) const;  // Pictures\MMDX12 or Videos\MMDX12
    // Transparent export: the PNG-sequence folder for a chosen output path ("<stem>_frames" next to it).
    static std::filesystem::path AlphaSequenceDir(const std::filesystem::path& output);
    DirectX::XMFLOAT3 CharacterCenter() const;  // world position of the character's center bone (teleport detection)
    void OfflinePose(float frame);              // UpdateScene, or StudioPoseForRender for a studio job
    void OfflineView(float frame, FrameView& view);  // BuildFrameView, or BuildStudioFrameView for a studio job
    void SaveOfflinePose();                     // current scene pose -> offline_.prevPose (video motion blur)
    void UploadOfflinePrevPose(uint64_t slot);  // offline_.prevPose -> the models' bone/morph ring entry `slot`

    AppOptions options_;
    int shadingOverride_ = -1;  // --shading (hidden): run-only raster ViewShading override, -1 = off
    bool classicLighting_ = false;  // --classic: hard sun shadow, no rim light (vanilla-MMD look), never saved
    AppSettings settings_;
    std::filesystem::path settingsPath_;
    HWND hwnd_ = nullptr;
    bool running_ = true;
    bool minimized_ = false;
    bool themeDirty_ = false;   // re-resolve and apply the theme before the next frame (user choice / Windows theme changed)
    Screen screen_ = Screen::Scanning;

    Dx12Context ctx_;
    Renderer renderer_;
    AudioPlayer audio_;
    static constexpr int kStudioWaveRate = 200;   // timeline waveform buckets per second of audio
    double studioSeekedAudioOffset_ = 0.0;        // the offset the playing sound was last aligned to (UpdateStudio)
    std::vector<float> audioPeaks_;   // timeline waveform: (peak, rms) pairs at kStudioWaveRate, decoded off the UI thread
    double studioAudioOffsetFrom_ = 0.0;  // the audio menu's offset field: the value when its edit started
    double audioPeaksDur_ = 0.0;      // audio duration the cached envelope was decoded for
    std::string audioPeaksPath_;      // audio path the cached envelope was decoded for
    // Background waveform decode: the future plus the track it was launched for. The result is picked up when it
    // is ready; a track change never blocks the UI on an unfinished decode.
    std::future<std::vector<float>> audioPeaksFuture_;
    bool audioPeaksRunning_ = false;
    std::string audioPeaksPendingPath_;   // track the running decode was launched for
    double audioPeaksPendingDur_ = 0.0;
    std::string audioPeaksFailedPath_;    // a decode that failed is not retried while the track stays the same

    // library
    LibraryScanResult library_;
    std::future<LibraryScanResult> scanFuture_;
    ScanProgress scanProgress_;
    bool revalidating_ = false;
    std::future<LibraryScanResult> revalidateFuture_;
    ScanProgress revalidateProgress_;
    std::unique_ptr<LibraryScanResult> pendingLibraryResult_;
    int selCharacter_ = -1, selStage_ = -1, selSong_ = -1;  // indices into library_ vectors; stage -1 = none
    char filterCharacter_[128] = {}, filterStage_[128] = {}, filterSong_[128] = {};
    char libraryPathEdit_[512] = {};
    ThumbnailCache thumbs_;
    bool thumbsClearPending_ = false;
    int libraryTab_ = 0;          // 0 characters, 1 stages, 2 songs
    // The lighting preset in effect this frame: the play scene's own choice ("스테이지 조명" for a loaded MMDX stage
    // with lights, else the ordinary setting).
    LightingPreset PlayLighting() const {
        if (playLighting_ >= 0) return (LightingPreset)playLighting_;
        if (scene_ && scene_->stage && scene_->stage->HasLights()) return LightingPreset::Stage;
        return (LightingPreset)settings_.lighting;
    }
    bool stageFogVolumetric_ = false;   // S9: the play scene's stage has environment.fog: haze on for this scene only
    int playLighting_ = -1;   // S1b: this scene's lighting choice (-1 = the default: the stage rig when it has lights,
                              // else settings_.lighting); "스테이지 조명" is never written to the ini
    // Sets this scene's lighting choice; an ordinary preset also becomes the saved default for other stages.
    void PlaySetLighting(int preset) {
        playLighting_ = preset;
        if (preset >= 0 && preset < kLightingPresetCount) settings_.lighting = preset;
    }

    bool libraryShowHidden_ = false;  // library grid: show the "숨긴 항목" (hidden) list instead of the active tab
    LibraryOverrides libraryOverridesCache_;  // hidden items: the override file, read when the view opens and after an
                                              // unhide / hide (never once per frame)
    bool libraryListView_ = false;    // library grid: show rows instead of thumbnail cards
    int libraryCols_ = 4;             // library card grid: number of columns (2..7), stepped
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
    int freeCamDrag_ = 0;                            // mouse buttons (bit per ImGuiMouseButton) whose drag began over the scene
    studio::CameraKf studioCamKeyBase_;              // ... and its key (interpolation curves)
    FreeCamera studioCamBase_;                       // the camera when a handle drag started
    POINT lastMouse_{};
    double timeSeconds_ = 0;      // wall clock since start (QPC)
    std::atomic<bool> loading_{false};  // startup renderer init in flight: splash paint, resize deferred
    RenderInitProgress* renderInitProgress_ = nullptr;
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
        int renderedFrame = 0;         // real-time renderers: the next image to render (readbacks/encoding lag behind)
        bool awaitingReadback = false; // real-time renderers: this image is recorded, its readback is pending
        int previewFrame = -1;         // last frame uploaded as the window preview (every 5th)
        uint32_t previewW = 0, previewH = 0;  // its size (letterbox in the progress overlay)
        bool readbackQueued = false;   // real-time renderers: the finished frame's readback copy is in the queue
        bool readbackFallback = false; // ring was full: read the final image synchronously before the next render
        bool finishing = false;        // FinishOffline is draining: a sink failure must not re-enter it
        std::deque<double> pendingRenderSeconds;  // render time of each recorded, not yet collected image
        double avgImageSeconds = 0;    // wall time per finished image (ETA)
        std::unique_ptr<AsyncVideoSink> sink;  // video / alpha sequence encoding on a worker thread
        bool alpha = false;                // transparent export: write a PNG sequence instead of an encoder
        std::filesystem::path alphaDir;    // sequence folder (AlphaSequenceDir of the output path)
        std::string alphaBase;             // frame file stem: "<alphaBase>_00000.png"
        bool background = false;       // a probe behind the select screen: screen_ stays Select, nothing is presented
        std::chrono::steady_clock::time_point imageStartClock;  // precise start of the current image
        double probeSum = 0;           // probe: seconds per image of the measured images (GI: render + encode,
                                       // real-time: interval between accepted images, at least the encode time)
        int probeN = 0;
        uint64_t probeKey = 0;         // probe: VideoProbeKey of the measured combination
        double avgEncodeSeconds = 0;   // wall time of one encoded frame on the sink's worker thread (ETA)
        // real-time renderers: when the last images were accepted by the sink (frame period over a window: ETA, probe)
        std::deque<std::chrono::steady_clock::time_point> doneClock;
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
    // Video render range (select screen dialog; not persisted). Whole song by default.
    bool videoRangeCustom_ = false;     // false: whole song, true: the custom [videoRangeSec_]
    double videoRangeSec_[2] = {0.0, 0.0};
    char videoRangeText_[2][16] = {};   // "시작" / "끝" text fields (HH:MM:SS)
    double lobbyVideoRange_[2] = {-1.0, -1.0};  // range handed to the load (-1 = unset)
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
    updater::ComponentFetcher component_;  // optional components (Components.h): on-demand fetch

    // studio
    std::unique_ptr<studio::StudioDoc> studio_;
    std::unique_ptr<studio::StudioPackage> studioPackage_;
    double studioLastBind_ = 0;       // timeSeconds_ of the last motion re-bind (throttled while dragging)
    bool studioLeaveConfirm_ = false; // unsaved-changes prompt is open (guards studioPending_)
    // MCP analysis caches (AppMcpAnalysis.cpp): static stage/prop AABBs by model uid, and the audio analysis by
    // path + write time.
    std::map<uint32_t, std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>> studioAabbCache_;
    std::shared_ptr<BeatAnalysis> studioBeats_;
    std::string studioBeatsKey_;
    std::future<std::pair<std::string, BeatAnalysis>> audioFuture_;
    std::string audioAnalysisPendingKey_;
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
        uint32_t replaceUid = 0;                   // model load: replaces this model (0: adds)
        bool materialReload = false;               // replace = the same file re-read for a material-source change
        std::vector<std::filesystem::path> queuedDrops;  // drop VMD/VPD applied to this model once it is in the doc
        size_t retryQueuedDrops = 0;                     // drop: on load failure, re-queue the first N onto the next job
        std::shared_ptr<McpPromise> mcp;           // studio_add_model: answered when the job ends
        LoadProgress progress;
        std::string error;
        std::future<bool> future;                  // last member: destroyed first, waits for the worker
    };
    std::vector<std::unique_ptr<StudioJob>> studioJobs_;
    std::future<bool> studioAutosave_;             // recovery file writer
    // World colliders (WorldColliders.h): the studio track is rebuilt in the background after motion / cast edits
    // (debounced 0.5 s); the previous track stays in use until the new one is ready.
    std::shared_ptr<const WorldColliderTrack> studioColliders_;
    std::future<std::shared_ptr<const WorldColliderTrack>> studioColliderFuture_;
    uint64_t studioColliderVersion_ = ~0ull;       // inputs the building / built track were made from
    uint64_t studioColliderPendingVersion_ = 0;
    double studioColliderEditAt_ = -1.0;           // timeSeconds_ of the last noticed edit (-1 = none)
    uint64_t seenHistoryVersion_ = 0;
    double studioAutosaveAt_ = 0;                  // timeSeconds_ of the last autosave check
    bool recoveryChecked_ = false;                 // the select screen asked about a recovery file this session
    bool recoveryPrompt_ = false;
    int studioRenameModel_ = -1;                   // outliner rename popup target
    int studioRemoveModel_ = -1;                   // outliner remove confirmation target
    int studioRenameCamera_ = -1;                  // S4: outliner camera rename popup target (-1 = none); value = uid + 1
    std::string studioLoadTitle_;                  // Loading screen title while a project opens (empty: library scene)
    char studioRenameBuf_[128] = {};
    char studioAddFilter_[64] = {};
    bool studioThumbsShown_ = false;               // the add popup's thumbnail grid was drawn (pump thumbnails)
    int studioAddPage_ = 0;   // "+" popup page: 0 root (장면 › / 조명 › / 월드 이펙트 ›), 1 characters, 2 stages, 3 songs,
                              // 4 장면 (캐릭터/스테이지/소품/MME/곡/음원/VMD), 5 조명, 6 조명 프리셋, 7 월드 이펙트
    int studioAddGridCols_ = 3;  // the "+" popup thumbnail grid: columns (2..6); higher = smaller tiles
    uint32_t studioReplaceUid_ = 0;                // "+" popup in replace mode: the model whose file is swapped (0: add)
    bool studioOpenAdd_ = false;                   // the outliner opens the "+" popup next frame (model menu's replace)
    bool studioHelpOpen_ = false;                  // shortcut overlay
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
    studio::AutoIkDrag studioAutoIkDrag_;   // auto-IK chain state captured when a wrist drag started
    bool studioAutoIkActive_ = false;       // the current gizmo drag is an auto-IK solve
    bool studioPoseFieldEdit_ = false;    // a numeric pose field / morph slider is being edited
    int studioHoverBone_ = -1;
    // camera inspector / path
    bool studioKeyEdit_ = false;                     // a camera/light/shadow key field is being edited
    bool studioKeyChanged_ = false;                  // ... and its value changed (else no undo step)
    bool studioKeyLive_ = false;                     // ... through the playhead fields (independent of the key selection)
    std::vector<studio::TrackState> studioKeyBefore_;
    // timeline lane handle drag: the tracks before the gesture (one undo step on release; not d.curveBefore, which the
    // inspector clears when no key item is active)
    bool studioLaneCurveEditing_ = false;
    bool studioLaneCurveChanged_ = false;            // ... and the gesture actually changed bytes (else no undo step)
    std::vector<studio::TrackState> studioLaneCurveBefore_;
    std::vector<studio::CameraPathPoint> studioCamPath_;  // motion camera samples (per frame, strided when long)
    std::vector<DirectX::XMFLOAT3> studioCamKeys_;   // eye at each camera key
    uint64_t studioCamPathVersion_ = 0;
    uint32_t studioCamPathUid_ = 0;                  // S4: the camera the samples were built for (selectedCameraUid)
    int studioCamPathStride_ = 1;
    // scene light editing
    int studioLightPresetPending_ = -1;
    bool studioLightEdit_ = false;
    bool studioLightChanged_ = false;
    bool studioLightIsKeyEdit_ = false;
    uint32_t studioLightEditingUid_ = 0;
    std::vector<studio::TrackState> studioLightKeyBefore_;
    std::vector<studio::SceneLight> studioLightBaseBefore_;
    // stopwatch-off confirm modal (pending while the callback is set)
    std::string studioStopwatchLabel_;
    std::function<void()> studioStopwatchOff_;

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
    double mcpRange_[2] = {0.0, 0.0};          // render_video's parsed range for the job about to start
    bool mcpRangeSet_ = false;
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
        nlohmann::json result;   // resolved with this (null: {"status": "completed"})
    };
    std::vector<PendingMcpInput> pendingMcpInputs_;

    struct PendingMcpContactSheet {
        bool active = false;
        std::shared_ptr<McpPromise> promise;
        std::vector<int> frames;
        size_t next = 0;
        int columns = 4;
        int tileWidth = 320;
        int settleFrames = 4;
        int waitFrames = 0;
        std::string path;
        bool returnImage = true;
        bool savedPlaying = false;
        int savedFrame = 0;
        bool savedUseMotionCamera = true;
        bool savedPhysics = true;
        uint32_t tileW = 0, tileH = 0;
        uint32_t gridW = 0, gridH = 0;
        std::vector<uint8_t> pixels;
        nlohmann::json tiles = nlohmann::json::array();
    };
    std::optional<PendingMcpContactSheet> pendingMcpContactSheet_;
};

} // namespace mmdx
