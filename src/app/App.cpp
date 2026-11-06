#include <future>
#include <chrono>
#include "core/I18n.h"
#include "app/App.h"
#include "asset/ImageLoader.h"
#include "asset/MmeAssign.h"
#include "asset/RayMaterial.h"
#include "asset/StageLights.h"

#include <ShlObj.h>
#include <shellapi.h>
#include <directx/d3dx12.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <json.hpp>

#include "app/Lighting.h"
#include "app/LookApply.h"
#include "app/UiKit.h"
#include "render/MmeSemantics.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "core/Timecode.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "ui_probe/UiProbe.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mmdx {

// M5c: case-insensitive ASCII lowercase of a wide string (path stems only; avoids a dependency on Passes.cpp).
static std::wstring LowerAsciiWideLocal(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return out;
}

// M5c: pairs an MME effect object path (an accessory `.x` or its `.fx`) with its same-stem partner in the same
// folder (case-insensitive; `.x` -> `.fx`, `.fx` -> `.x` when present). Sets `fx` and `meshFile` (either may be
// empty when the partner is absent). Shared by --mme-object and the studio / project load.
void ResolveMmeObjectPair(const std::filesystem::path& path, std::filesystem::path& fx,
                          std::filesystem::path& meshFile) {
    fx.clear();
    meshFile.clear();
    if (path.empty()) return;
    const std::filesystem::path dir = path.parent_path();
    const std::wstring wantLower = LowerAsciiWideLocal(path.stem().wstring());
    auto hasExt = [&](const std::filesystem::path& p, const wchar_t* ext) {
        std::wstring e = p.extension().wstring();
        for (wchar_t& c : e) c = (wchar_t)towlower(c);
        return e == ext;
    };
    if (hasExt(path, L".x")) {
        meshFile = path;
        // same-stem .fx in the same folder
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path p = entry.path();
            if (!hasExt(p, L".fx")) continue;
            if (LowerAsciiWideLocal(p.stem().wstring()) == wantLower) { fx = p; break; }
        }
    } else {
        fx = path;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path p = entry.path();
            if (!hasExt(p, L".x")) continue;
            if (LowerAsciiWideLocal(p.stem().wstring()) == wantLower) { meshFile = p; break; }
        }
    }
}

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
        } else if (arg == L"--formation") {
            opt.formation = WideToUtf8(next());
        } else if (arg == L"--song-audio") {
            opt.songAudio = WideToUtf8(next());
        } else if (arg == L"--stage-variant") {
            opt.stageVariant = WideToUtf8(next());
        } else if (arg == L"--autoplay") {
            opt.autoplay = true;
        } else if (arg == L"--seek") {
            opt.seekSeconds = _wtof(next().c_str());
        } else if (arg == L"--benchmark") {
            opt.benchmarkCategory = WideToUtf8(next());
        } else if (arg == L"--bench-frames") {
            opt.benchFrames = _wtoi(next().c_str());
        } else if (arg == L"--bench-spp") {
            opt.benchSpp = _wtoi(next().c_str());
        } else if (arg == L"--frames") {
            opt.quitAfterFrames = _wtoi(next().c_str());
        } else if (arg == L"--capture") {
            opt.capturePath = next();
        } else if (arg == L"--capture-scene") {   // --capture saves the rendered scene only (no UI / overlays)
            opt.captureSceneOnly = _wtoi(next().c_str()) != 0;
        } else if (arg == L"--ui-script") {
            opt.uiScript = next();
        } else if (arg == L"--width") {
            opt.width = _wtoi(next().c_str());
        } else if (arg == L"--height") {
            opt.height = _wtoi(next().c_str());
        } else if (arg == L"--debug") {
            opt.debugLayer = true;
        } else if (arg == L"--screen") {
            opt.startScreen = WideToUtf8(next());
        } else if (arg == L"--gallery-index") {
            opt.galleryIndex = WideToUtf8(next());
        } else if (arg == L"--ui-scale") {
            opt.uiScale = (float)_wtof(next().c_str());
        } else if (arg == L"--project") {
            opt.project = next();
            opt.startScreen = "studio";
        } else if (arg == L"--theme") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "auto" || v == "system") opt.theme = 0;
            else if (v == "light") opt.theme = 1;
            else if (v == "dark") opt.theme = 2;
            else LOG_WARN("unknown --theme value: %s (want auto|light|dark)", v.c_str());
        } else if (arg == L"--lang") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            opt.language = v == "ko" ? 1 : v == "en" ? 2 : v == "ja" ? 3 : v == "zh" ? 4 : 0;
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
        } else if (arg == L"--shading") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "lit") opt.shading = 0;
            else if (v == "unlit") opt.shading = 1;
            else if (v == "wire") opt.shading = 2;
            else LOG_WARN("unknown --shading value: %s (want lit|unlit|wire)", v.c_str());
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
        } else if (arg == L"--dlss-model") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "auto") opt.dlssModel = 0;
            else if (v == "cnn") opt.dlssModel = 1;
            else if (v == "transformer") opt.dlssModel = 2;
            else if (v == "transformer2") opt.dlssModel = 3;
            else LOG_WARN("unknown --dlss-model value: %s (want auto|cnn|transformer|transformer2)", v.c_str());
        } else if (arg == L"--dlss-rr") {
            opt.dlssRayReconstruction = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--frame-gen") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "off" || v == "0" || v == "none") opt.frameGen = 0;
            else if (v == "2" || v == "2x") opt.frameGen = 2;
            else if (v == "3" || v == "3x") opt.frameGen = 3;
            else if (v == "4" || v == "4x") opt.frameGen = 4;
            else LOG_WARN("unknown --frame-gen value: %s (want off|2|3|4)", v.c_str());
        } else if (arg == L"--dof") {
            opt.dof = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--volumetric") {
            opt.volumetric = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--volumetric-density") {
            opt.volumetricDensity = (float)_wtof(next().c_str());
        } else if (arg == L"--volumetric-mode") {
            // S9d: froxel (the default) or march (the S9 per-pixel march, kept for A/B)
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "froxel" || v == "0") opt.volumetricMode = 0;
            else if (v == "march" || v == "1") opt.volumetricMode = 1;
            else LOG_WARN("unknown --volumetric-mode value: %s (want froxel|march)", v.c_str());
        } else if (arg == L"--rtao-rays") {
            opt.rtaoRays = _wtoi(next().c_str());
        } else if (arg == L"--volumetric-ms") {
            opt.volumetricMs = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--bloom-conv") {
            opt.bloomConv = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--bloom-kernel") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "cinema") opt.bloomKernel = 0;
            else if (v == "starburst") opt.bloomKernel = 1;
            else if (v == "glow") opt.bloomKernel = 2;
            else if (v == "anamorphic") opt.bloomKernel = 3;
            else LOG_WARN("unknown --bloom-kernel value: %s (want cinema|starburst|glow|anamorphic)", v.c_str());
        } else if (arg == L"--bloom-blend") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "add") opt.bloomBlend = 0;
            else if (v == "energy") opt.bloomBlend = 1;
            else LOG_WARN("unknown --bloom-blend value: %s (want add|energy)", v.c_str());
        } else if (arg == L"--bloom-depth-mask") {
            opt.bloomDepthMask = (float)_wtof(next().c_str());
        } else if (arg == L"--bloom-intensity") {
            opt.bloomIntensity = (float)_wtof(next().c_str());
        } else if (arg == L"--bloom-threshold") {
            opt.bloomThreshold = (float)_wtof(next().c_str());
        } else if (arg == L"--bloom-radius") {
            opt.bloomRadius = (float)_wtof(next().c_str());
        } else if (arg == L"--auto-luminous") {
            opt.autoLuminous = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--screen-lights") {
            opt.screenLights = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--lut") {
            opt.lut = WideToUtf8(next());
        } else if (arg == L"--classic") {
            opt.classic = true;   // vanilla-MMD look, this run only (never saved to the ini)
        } else if (arg == L"--motion-lighting") {
            opt.motionLighting = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--no-physics") {
            opt.noPhysics = true;
        } else if (arg == L"--pack-index") {
            opt.packIndex = WideToUtf8(next());
        } else if (arg == L"--shader-pack") {
            opt.shaderPack = WideToUtf8(next());
            if (opt.shaderPack == "none") opt.shaderPack.clear();
            opt.shaderPackSet = true;
        } else if (arg == L"--material-source") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            opt.materialSourceSet = true;
            if (v == "default") opt.materialSource = 0;
            else if (v == "ray") opt.materialSource = 1;
            else { opt.materialSourceSet = false; LOG_WARN("unknown --material-source value: %s (want default|ray)", v.c_str()); }
        } else if (arg == L"--pbr-debug") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "none") opt.pbrDebug = 0;
            else if (v == "albedo") opt.pbrDebug = 1;
            else if (v == "normal") opt.pbrDebug = 2;
            else if (v == "roughness") opt.pbrDebug = 3;
            else if (v == "metalness") opt.pbrDebug = 4;
            else if (v == "ao") opt.pbrDebug = 5;
            else if (v == "emissive") opt.pbrDebug = 6;
            else if (v == "model") opt.pbrDebug = 7;
            else LOG_WARN("unknown --pbr-debug value: %s (want none|albedo|normal|roughness|metalness|ao|emissive|model)", v.c_str());
        } else if (arg == L"--effect") {
            // --effect <id>[,<id>...] / --effect none: the effect stack for this run (overrides the settings)
            const std::string v = WideToUtf8(next());
            opt.effectSet = true;
            opt.effectStack.clear();
            if (v != "none" && !v.empty()) {
                size_t pos = 0;
                while (pos <= v.size()) {
                    size_t end = v.find(',', pos);
                    if (end == std::string::npos) end = v.size();
                    const std::string id = v.substr(pos, end - pos);
                    if (!id.empty()) opt.effectStack.push_back({id, true, {}, {}});
                    pos = end + 1;
                }
            }
        } else if (arg == L"--world") {
            // --world <id>[,<id>...] / --world none: the world stack for this run (overrides the settings)
            const std::string v = WideToUtf8(next());
            opt.worldSet = true;
            opt.worldStack.clear();
            if (v != "none" && !v.empty()) {
                size_t pos = 0;
                while (pos <= v.size()) {
                    size_t end = v.find(',', pos);
                    if (end == std::string::npos) end = v.size();
                    const std::string id = v.substr(pos, end - pos);
                    if (!id.empty()) opt.worldStack.push_back({id, true, {}, {}});
                    pos = end + 1;
                }
            }
        } else if (arg == L"--look") {
            // --look <id>: applies a look pack to the settings for this run (not persisted), like --effect
            opt.look = WideToUtf8(next());
            opt.lookSet = true;
        } else if (arg == L"--mme") {
            // --mme <file>[,<file>...]: MMDX12 MME post effects for this run, in list order
            const std::string v = WideToUtf8(next());
            opt.mmeEffects.clear();
            opt.mmeSet = true;
            size_t pos = 0;
            while (pos <= v.size()) {
                size_t end = v.find(',', pos);
                if (end == std::string::npos) end = v.size();
                const std::string p = v.substr(pos, end - pos);
                if (!p.empty()) {
                    MmeEffectEntry e;
                    e.file = Utf8ToPath(p);
                    std::error_code ec;
                    if (!std::filesystem::exists(e.file, ec))
                        LOG_WARN("--mme: file not found: %s", p.c_str());
                    // CLI cannot answer a modal: a Ray-MMD main effect is added with a warning
                    if (IsRayMmdMainEffect(e.file))
                        LOG_WARN("Ray-MMD main effect added as MME (forced by script)");
                    opt.mmeEffects.push_back(std::move(e));
                }
                pos = end + 1;
            }
        } else if (arg == L"--mme-model") {
            // --mme-model <fx>: an MME object effect applied to the play-mode character (M4b)
            opt.mmeModel = Utf8ToPath(WideToUtf8(next()));
            opt.mmeModelSet = true;
            std::error_code ec;
            if (!std::filesystem::exists(opt.mmeModel, ec))
                LOG_WARN("--mme-model: file not found: %s", PathToUtf8(opt.mmeModel).c_str());
        } else if (arg == L"--mme-object") {
            // --mme-object <path>[,<path>...]: MME effect objects (an accessory `.x` + its same-stem `.fx`), this run
            const std::string v = WideToUtf8(next());
            opt.mmeObjects.clear();
            opt.mmeObjectSet = true;
            size_t pos = 0;
            while (pos <= v.size()) {
                size_t end = v.find(',', pos);
                if (end == std::string::npos) end = v.size();
                const std::string p = v.substr(pos, end - pos);
                if (!p.empty()) {
                    const std::filesystem::path path = Utf8ToPath(p);
                    std::error_code ec;
                    if (!std::filesystem::exists(path, ec)) {
                        LOG_WARN("--mme-object: file not found: %s", p.c_str());
                    } else {
                        std::filesystem::path fx, mesh;
                        ResolveMmeObjectPair(path, fx, mesh);
                        if (fx.empty())
                            LOG_WARN("--mme-object: no same-stem .fx next to %s; skipped", p.c_str());
                        else {
                            // CLI cannot answer a modal: a Ray-MMD main effect is added with a warning (once)
                            if (IsRayMmdMainEffect(path))
                                LOG_WARN("Ray-MMD main effect added as MME (forced by script)");
                            opt.mmeObjects.emplace_back(fx, mesh);
                        }
                    }
                }
                pos = end + 1;
            }
        } else if (arg == L"--mme-model-all") {
            opt.mmeModelAll = _wtoi(next().c_str()) != 0;
        } else if (arg == L"--mme-auto") {
            // --mme-auto <0|1>: force None(0) / Auto(1) for every model of this run (not saved)
            opt.mmeAuto = _wtoi(next().c_str()) != 0 ? 1 : 0;
            opt.mmeAutoSet = true;
        } else if (arg == L"--mme-global") {
            // --mme-global <0|1>: global MME switch for this run (not saved). 0 wins over everything.
            opt.mmeGlobal = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--ray-global") {
            // --ray-global <0|1>: global Ray-MMD material-source switch for this run (not saved). 0 wins over everything.
            opt.rayGlobal = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--mme-workflow") {
            // --mme-workflow <auto|manual>: MME workflow for this run (not saved)
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "auto") opt.mmeWorkflow = 1;
            else if (v == "manual") opt.mmeWorkflow = 0;
            else LOG_WARN("unknown --mme-workflow value: %s (want auto|manual)", v.c_str());
        } else if (arg == L"--stage-mme") {
            // --stage-mme <0|1>: force None(0) / Auto(1) for the stage(s) of this run only
            opt.stageMme = _wtoi(next().c_str()) != 0 ? 1 : 0;
            opt.stageMmeSet = true;
        } else if (arg == L"--mme-panel") {
            // --mme-panel <si>,<tr>: sets si / tr of every --mme entry (for tests)
            const std::string v = WideToUtf8(next());
            float si = 1.0f, tr = 1.0f;
            if (sscanf_s(v.c_str(), "%f,%f", &si, &tr) == 2) {
                opt.mmePanelSet = true;
                opt.mmePanelSi = si;
                opt.mmePanelTr = tr;
            } else {
                LOG_WARN("--mme-panel wants <si>,<tr>; got: %s", v.c_str());
            }
        } else if (arg == L"--offline-still") {
            opt.offlineStill = next();
        } else if (arg == L"--offline-video") {
            opt.offlineVideo = next();
        } else if (arg == L"--offline-range") {
            const std::wstring a = next(), b = next();
            double av = -1.0, bv = -1.0;
            if (!ParseTimecode(WideToUtf8(a), av))
                LOG_WARN("--offline-range: cannot parse start '%s' (seconds or HH:MM:SS)", WideToUtf8(a).c_str());
            if (!ParseTimecode(WideToUtf8(b), bv))
                LOG_WARN("--offline-range: cannot parse end '%s' (seconds or HH:MM:SS)", WideToUtf8(b).c_str());
            opt.offlineRange[0] = av;
            opt.offlineRange[1] = bv;
        } else if (arg == L"--offline-spp") {
            opt.offlineSpp = _wtoi(next().c_str());
        } else if (arg == L"--studio-renderer") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "raster") opt.studioRenderer = 0;
            else if (v == "rt") opt.studioRenderer = 1;
            else if (v == "pt") opt.studioRenderer = 2;
            else if (v == "gi") opt.studioRenderer = 3;
            else LOG_WARN("unknown --studio-renderer value: %s (want raster|rt|pt|gi)", v.c_str());
        } else if (arg == L"--offline-prepass") {
            opt.offlinePrepass = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--offline-volumetric") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "single" || v == "0") opt.offlineVolumetric = 0;
            else if (v == "full" || v == "1") opt.offlineVolumetric = 1;
            else LOG_WARN("unknown --offline-volumetric value: %s (want single|full)", v.c_str());
        } else if (arg == L"--offline-fps") {
            opt.offlineFps = _wtoi(next().c_str());
        } else if (arg == L"--offline-bitrate") {
            opt.offlineBitrate = _wtoi(next().c_str());
        } else if (arg == L"--offline-quality") {
            opt.offlineQuality = _wtoi(next().c_str());
        } else if (arg == L"--offline-renderer") {
            const std::string v = ToLowerAscii(WideToUtf8(next()));
            if (v == "raster") opt.offlineRenderer = 0;
            else if (v == "rt") opt.offlineRenderer = 1;
            else if (v == "pt") opt.offlineRenderer = 2;
            else if (v == "gi") opt.offlineRenderer = 3;
            else LOG_WARN("unknown --offline-renderer value: %s (want raster|rt|pt|gi)", v.c_str());
        } else if (arg == L"--offline-alpha") {
            opt.offlineAlpha = _wtoi(next().c_str()) != 0 ? 1 : 0;
        } else if (arg == L"--offline-probe") {
            opt.offlineProbe = true;
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
        } else if (arg == L"--update-feed") {
            opt.updateFeed = WideToUtf8(next());
        } else if (arg == L"--update-relaunch-frames") {
            opt.updateRelaunchFrames = _wtoi(next().c_str());
        } else if (arg == L"--apply-update") {
            opt.applyUpdate = true;
        } else if (arg == L"--apply-wait") {
            opt.applyWaitPid = (uint32_t)_wtoi(next().c_str());
        } else if (arg == L"--component") {
            opt.componentId = WideToUtf8(next());
        } else if (arg == L"--download-component") {
            opt.componentDownload = true;
        } else if (arg == L"--mcp") {
            opt.mcp = 1;
        } else if (arg == L"--no-mcp") {
            opt.mcp = 0;
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
    // Startup phase timings logged at the end of Init (STARTUP lines, milliseconds per phase).
    QueryPerformanceCounter(&startupRunBegan_);
    QueryPerformanceFrequency(&startupFreq_);
    LARGE_INTEGER startupPrev = startupRunBegan_;
    const auto startupPhase = [&](const char* name) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LOG_INFO("STARTUP %s: %.0f ms", name,
                 (double)(now.QuadPart - startupPrev.QuadPart) * 1000.0 / (double)startupFreq_.QuadPart);
        startupPrev = now;
    };

    // A leftover staged update (the applier never ran, e.g. the machine was switched off mid
    // swap) is completed before anything else opens; --apply-update runs the applier directly.
    if (options_.applyUpdate) return updater::ApplyPendingUpdateAndRelaunch(options_.applyWaitPid, true);
    // --download-component: headless optional-component fetch (tools/components_e2e.py); no window.
    if (options_.componentDownload) return CliComponentE2e();
    if (updater::HasPendingUpdate()) {
        LOG_INFO("update: applying a staged update left by the previous run");
        return updater::ApplyPendingUpdateAndRelaunch(0, false);
    }
    updater::CleanupUpdateLeftovers();

    LOG_INFO("MMDX12 %s", MMDX12_VERSION);
    settingsPath_ = ExecutableDir() / L"mmdx12.ini";
    settings_.Load(settingsPath_);
    // M5-U: crash guard suspects from the previous run. A marker left by a crashed run names the non-built-in packs
    // that were compiling / drawn; quarantine them and report once.
    for (const std::string& suspect : packGuard_.TakeSuspects()) {
        const ShaderPack* pk = ShaderPacks().Find(suspect);
        if (pk && pk->source == PackSource::BuiltIn) continue;
        packGuard_.Quarantine(suspect);
        LOG_WARN("pack crash guard: '%s' was active when the previous run exited; disabled", suspect.c_str());
        pendingGuardToast_ = std::string(Tr("이전 실행이 '")) + suspect + Tr("' 팩 사용 중 종료되어 비활성화했습니다");
    }
    for (const auto& [id, dir] : settings_.packTextureFolders)   // shader packs' user texture folders
        ShaderPacks().SetTextureFolder(id, Utf8ToPath(dir));
    SetLanguage((Language)(options_.language >= 0 ? options_.language : settings_.language));
    ApplyTheme();   // palette first: the splash and every screen read it
    const AppSettings persisted = settings_;  // CLI overrides below are for this run only
    if (options_.lighting >= 0) settings_.lighting = std::clamp(options_.lighting, 0, kLightingPresetCount - 1);
    if (options_.quality >= 0) ApplyGraphicsPreset(std::clamp(options_.quality, 0, 3));
    if (options_.renderPath >= 0) settings_.renderPath = std::clamp(options_.renderPath, 0, 2);
    if (options_.studioRenderer >= 0) settings_.studioRenderer = std::clamp(options_.studioRenderer, 0, 3);
    shadingOverride_ = options_.shading;
    if (options_.upscaler >= 0) settings_.upscaler = std::clamp(options_.upscaler, 0, 3);
    if (options_.upscalerQuality >= 0) settings_.upscalerQuality = std::clamp(options_.upscalerQuality, 0, 4);
    if (options_.dlssModel >= 0) settings_.dlssModel = std::clamp(options_.dlssModel, 0, 3);
    if (options_.dlssRayReconstruction >= 0) settings_.dlssRayReconstruction = options_.dlssRayReconstruction != 0;
    if (options_.frameGen >= 0) settings_.dlssFrameGen = options_.frameGen;
    if (options_.dof >= 0) settings_.dof = options_.dof != 0;
    if (options_.volumetric >= 0) settings_.volumetric = options_.volumetric != 0;
    if (options_.volumetricDensity > 0.0f) settings_.volumetricDensity = std::clamp(options_.volumetricDensity, 0.25f, 4.0f);
    if (options_.volumetricMode >= 0) settings_.volumetricMode = std::clamp(options_.volumetricMode, 0, 1);
    if (options_.rtaoRays >= 0) settings_.rtaoRays = std::clamp(options_.rtaoRays, 1, 64);
    if (options_.volumetricMs >= 0) settings_.volumetricMultiScatter = options_.volumetricMs != 0;
    if (options_.bloomConv >= 0) settings_.bloomConvolution = options_.bloomConv != 0;
    if (options_.bloomKernel >= 0) {
        settings_.bloomKernelPreset = options_.bloomKernel;
        settings_.bloomLook.kernel = BloomKernelPreset(options_.bloomKernel);
    }
    if (options_.bloomBlend >= 0) settings_.bloomLook.blend = options_.bloomBlend;
    if (options_.bloomDepthMask >= 0.0f) settings_.bloomLook.depthMask = std::clamp(options_.bloomDepthMask, 0.0f, 1.0f);
    if (options_.bloomIntensity >= 0.0f) settings_.bloomIntensity = std::clamp(options_.bloomIntensity, 0.0f, 1.5f);
    if (options_.bloomThreshold >= 0.0f) settings_.bloomThreshold = std::clamp(options_.bloomThreshold, 0.2f, 4.0f);
    if (options_.bloomRadius >= 0.0f) settings_.bloomLook.radius = std::clamp(options_.bloomRadius, 0.0f, 1.0f);
    if (options_.autoLuminous >= 0) settings_.autoLuminous = options_.autoLuminous != 0;
    if (options_.screenLights >= 0) settings_.screenLights = options_.screenLights != 0;
    if (options_.motionLighting >= 0) settings_.motionLighting = options_.motionLighting != 0;
    if (options_.effectSet) settings_.effectStack = options_.effectStack;   // --effect, this run only
    if (options_.effectSet)
        for (const EffectStackEntry& e : options_.effectStack) {
            const ShaderPack* pk = ShaderPacks().Find(e.pack);
            if (!pk || !PackUsable(pk))
                LOG_WARN("--effect: pack '%s' is not usable (missing, revoked or disabled)", e.pack.c_str());
        }
    if (options_.shaderPackSet && !options_.shaderPack.empty()) {
        const ShaderPack* pk = ShaderPacks().Find(options_.shaderPack);
        if (!pk || !PackUsable(pk))
            LOG_WARN("--shader-pack: pack '%s' is not usable (missing, revoked or disabled)",
                     options_.shaderPack.c_str());
    }
    if (options_.worldSet) settings_.worldStack = options_.worldStack;      // --world, this run only
    if (options_.lookSet && !options_.look.empty()) {   // --look: apply the look's groups for this run only
        const ShaderPack* pack = ShaderPacks().Find(options_.look);
        if (!pack || pack->type != PackType::Look || !pack->look || !PackUsable(pack)) {
            LOG_WARN("--look: no usable look pack '%s'", options_.look.c_str());
        } else {
            cliLook_ = *pack->look;
            cliLookPrev_ = CaptureLook(persisted, nullptr);
            cliLookActive_ = true;
            for (const LookPackRef& ref : cliLook_.packs)
                if (!ref.id.empty() && !ShaderPacks().Find(ref.id))
                    LOG_WARN("--look %s: referenced pack '%s' is not installed", options_.look.c_str(), ref.id.c_str());
            ApplyLook(cliLook_, settings_, nullptr);
        }
    }
    if (options_.classic) {   // --classic: vanilla-MMD look for this run only (never saved to the ini)
        settings_.ssao = false;
        settings_.ssr = false;
        settings_.bloom = false;
        settings_.bloomConvolution = false;
        settings_.autoLuminous = false;
        settings_.dof = false;
        settings_.volumetric = false;
        settings_.colorLut.clear();
        settings_.effectStack.clear();
        classicLighting_ = true;   // BuildFrameView: hard sun shadow, no rim light
        settings_.readOnly = true;  // a classic session never writes the ini (UI toggles save mid-run)
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    startupPhase("settle");

    int w = options_.width > 0 ? options_.width : settings_.windowWidth;
    int h = options_.height > 0 ? options_.height : settings_.windowHeight;
    loading_ = true;  // splash painting from the first message on
    if (!InitWindow(instance, w, h)) return 1;
    ApplyTheme();   // title bar (the window did not exist for the first call)
    UpdateWindow(hwnd_);
    startupPhase("window");

    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!ctx_.Initialize(hwnd_, (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top),
                         options_.debugLayer)) {
        MessageBoxW(hwnd_, Utf8ToWide(Tr("Direct3D 12 초기화에 실패했습니다.")).c_str(), L"MMDX12", MB_ICONERROR);
        return 1;
    }
    startupPhase("device");

    std::filesystem::path shaderDir = ExecutableDir() / L"shaders";
    if (!std::filesystem::exists(shaderDir / L"mmd.hlsl")) {
        std::filesystem::path found = FindUpward(ExecutableDir(), L"shaders/mmd.hlsl");
        if (!found.empty()) shaderDir = found / L"shaders";
    }
    // The renderer init (shader compile, upscaler SDKs) takes seconds: run it on a worker thread and keep pumping
    // window messages here, so the window paints the splash instead of going white / "not responding".
    loading_ = true;
    RenderInitProgress initProgress;
    renderInitProgress_ = &initProgress;
    auto rendererInit = std::async(std::launch::async, [&] { return renderer_.Initialize(ctx_, shaderDir, &initProgress); });
    while (rendererInit.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (renderInitProgress_) {
            uint32_t compiled = GetCompiledShaderCount();
            uint32_t expected = GetExpectedShaderCount();
            if (expected > 0) {
                float sFrac = std::clamp((float)compiled / (float)expected, 0.0f, 1.0f);
                float f = 0.15f + 0.75f * sFrac;
                if (f > renderInitProgress_->fraction.load()) {
                    renderInitProgress_->fraction.store(f);
                }
            }
            if (GetColdShaderCount() > 0) {
                renderInitProgress_->SetStatus(Tr("셰이더 컴파일 중 (첫 실행은 시간이 걸립니다)"));
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    const bool rendererOk = rendererInit.get();
    renderInitProgress_ = nullptr;
    loading_ = false;
    if (!hwnd_) return 0;  // closed while loading
    if (!rendererOk) {
        MessageBoxW(hwnd_, Utf8ToWide(Tr("렌더러 초기화에 실패했습니다.")).c_str(), L"MMDX12", MB_ICONERROR);
        return 1;
    }
    {  // a resize during loading was deferred
        RECT now;
        GetClientRect(hwnd_, &now);
        if (now.right > 0 && now.bottom > 0 && !IsIconic(hwnd_)) ctx_.Resize((uint32_t)now.right, (uint32_t)now.bottom);
    }
    startupPhase("shaders+upscalers");

    ApplyRenderSettings();
    // M6b: MME hot-reload failures report a toast (fires on the main thread during Render).
    renderer_.SetMmeMessage([this](const std::string& msg, bool error) {
        toast_ = Toast{Tr("MME 효과를 다시 컴파일하지 못했습니다"), msg, {}, error, timeSeconds_ + 8.0};
    });

    if (!audio_.Initialize()) LOG_WARN("audio disabled");
    audio_.SetVolume(settings_.volume);

    if (!InitImGui()) return 1;
    startupPhase("font atlas");
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

    InitUpdater();
    startupPhase("updater");
    InitMcp();
    startupPhase("mcp");
    LibraryScanResult cachedResult;
    if (LoadLibraryScanResult(libPath, cachedResult)) {
        LOG_INFO("library: loaded cached library scan result (%d characters, %d stages, %d songs)",
                 static_cast<int>(cachedResult.characters.size()),
                 static_cast<int>(cachedResult.stages.size()),
                 static_cast<int>(cachedResult.songs.size()));
        library_ = std::move(cachedResult);
        RestoreSelectionsFromSettings();
        ApplyCommandLinePreselection();
        ApplyStartScreen();
        StartRevalidation();
        startupPhase("scan start");
    } else {
        StartScan();
        startupPhase("scan start");
    }
    MainLoop();
    scanProgress_.cancel = true;   // a library scan still running (quit while scanning) stops instead of finishing
    revalidateProgress_.cancel = true;
    if (revalidateFuture_.valid()) revalidateFuture_.wait();

    // Studio work is never lost by closing the window: unsaved edits go to the recovery file, offered at the next
    // start (automated runs skip this, they would leave prompts behind for the next test).
    studioJobs_.clear();
    if (studio_ && studio_->Dirty() && options_.quitAfterFrames == 0 && options_.uiScript.empty()) StudioAutosave(true, true);
    if (studioAutosave_.valid()) studioAutosave_.get();
    ShutdownMcp();
    ctx_.WaitForGpu();
    ReleaseRenderBenchImage();
    UnloadScene();
    thumbs_.Shutdown();
    ShutdownImGui();
    // The renderer may still wait for the offline GI renderer's background creation (a compile in progress after a
    // cold start): hide the window first, so a quit never shows a frozen "not responding" window meanwhile.
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
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
    if (options_.studioRenderer >= 0) settings_.studioRenderer = persisted.studioRenderer;
    if (options_.upscaler >= 0) settings_.upscaler = persisted.upscaler;
    if (options_.upscalerQuality >= 0) settings_.upscalerQuality = persisted.upscalerQuality;
    if (options_.dlssModel >= 0) settings_.dlssModel = persisted.dlssModel;
    if (options_.dlssRayReconstruction >= 0) settings_.dlssRayReconstruction = persisted.dlssRayReconstruction;
    if (options_.frameGen >= 0) settings_.dlssFrameGen = persisted.dlssFrameGen;
    if (options_.dof >= 0) settings_.dof = persisted.dof;
    if (options_.volumetric >= 0) settings_.volumetric = persisted.volumetric;
    if (options_.volumetricDensity > 0.0f) settings_.volumetricDensity = persisted.volumetricDensity;
    if (options_.volumetricMode >= 0) settings_.volumetricMode = persisted.volumetricMode;
    if (options_.rtaoRays >= 0) settings_.rtaoRays = persisted.rtaoRays;
    if (options_.volumetricMs >= 0) settings_.volumetricMultiScatter = persisted.volumetricMultiScatter;
    if (options_.bloomConv >= 0) settings_.bloomConvolution = persisted.bloomConvolution;
    if (options_.bloomKernel >= 0) {
        settings_.bloomKernelPreset = persisted.bloomKernelPreset;
        settings_.bloomLook.kernel = persisted.bloomLook.kernel;
    }
    if (options_.bloomBlend >= 0) settings_.bloomLook.blend = persisted.bloomLook.blend;
    if (options_.bloomDepthMask >= 0.0f) settings_.bloomLook.depthMask = persisted.bloomLook.depthMask;
    if (options_.bloomIntensity >= 0.0f) settings_.bloomIntensity = persisted.bloomIntensity;
    if (options_.bloomThreshold >= 0.0f) settings_.bloomThreshold = persisted.bloomThreshold;
    if (options_.bloomRadius >= 0.0f) settings_.bloomLook.radius = persisted.bloomLook.radius;
    if (options_.autoLuminous >= 0) settings_.autoLuminous = persisted.autoLuminous;
    if (options_.screenLights >= 0) settings_.screenLights = persisted.screenLights;
    if (options_.motionLighting >= 0) settings_.motionLighting = persisted.motionLighting;
    if (!options_.lut.empty()) settings_.colorLut = persisted.colorLut;
    if (options_.effectSet) settings_.effectStack = persisted.effectStack;   // --effect, that run only
    if (options_.worldSet) settings_.worldStack = persisted.worldStack;      // --world, that run only
    if (cliLookActive_) {   // --look, that run only
        ApplyLook(cliLookPrev_, settings_, nullptr);
        if (!cliLookCharacterId_.empty()) settings_.SetCharacterShader(cliLookCharacterId_, cliLookPrevSurface_);
        cliLookActive_ = false;
    }
    settings_.Save(settingsPath_);

    ctx_.Shutdown();
    if (hwnd_) DestroyWindow(hwnd_);
    return 0;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

namespace {
// The GDI splash shown while the renderer initialises: the same layout, font and palette as the library scan
// screen (App::DrawScanning) that follows it, so the hand-over does not jump. Painted off-screen (no flicker).
void PaintSplash(HDC hdc, const RECT& r, float dpi, float fraction, const std::string& status, const std::string& count) {
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return;
    static const bool fontsAdded = [] {   // Pretendard for this process only (ImGui loads its own copy later)
        for (const wchar_t* f : {L"Pretendard-Bold.otf", L"Pretendard-SemiBold.otf", L"Pretendard-Regular.otf"})
            AddFontResourceExW((ExecutableDir() / L"assets" / L"fonts" / f).c_str(), FR_PRIVATE, nullptr);
        return true;
    }();
    (void)fontsAdded;
    const ui::Palette& p = ui::P();
    const auto rgb = [](ImU32 c) { return (COLORREF)(c & 0xFFFFFF); };   // ImU32 is 0xAABBGGRR, COLORREF 0x00BBGGRR
    const auto dp = [dpi](float v) { return (int)std::lround(v * dpi); };
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(rgb(p.bg));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const auto font = [&](float px, int weight) {
        return CreateFontW(-dp(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                           L"Pretendard");
    };
    const auto centred = [&](const std::string& text, HFONT f, COLORREF col, int y) {
        const std::wstring w = Utf8ToWide(text);
        HGDIOBJ old = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
        SetTextColor(dc, col);
        TextOutW(dc, (width - sz.cx) / 2, y, w.c_str(), (int)w.size());
        SelectObject(dc, old);
        return sz;
    };
    const int cx = width / 2, cy = height / 2;
    const int y = cy - dp(70.0f);
    HFONT logo = font(34.0f, FW_BOLD);
    HGDIOBJ old = SelectObject(dc, logo);
    SIZE a{}, b{};
    GetTextExtentPoint32W(dc, L"MMDX", 4, &a);
    GetTextExtentPoint32W(dc, L"12", 2, &b);
    const int lx = cx - (a.cx + b.cx) / 2;
    SetTextColor(dc, rgb(p.ink));
    TextOutW(dc, lx, y, L"MMDX", 4);
    SetTextColor(dc, rgb(p.accentInk));
    TextOutW(dc, lx + a.cx, y, L"12", 2);
    SelectObject(dc, old);
    DeleteObject(logo);
    const int logoH = a.cy;
    HFONT body = font(ui::size::Body, FW_SEMIBOLD);
    centred(status.empty() ? std::string(Tr("렌더러 준비 중")) : status, body, rgb(p.ink2), y + logoH + dp(22.0f));
    DeleteObject(body);
    // the bar: ui::ProgressBar's shape (sunken track, accent fill, fully rounded)
    const int bw = dp(360.0f), bh = dp(6.0f), by = y + logoH + dp(54.0f), bx = cx - bw / 2;
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH track = CreateSolidBrush(rgb(p.sunken));
    HGDIOBJ oldBrush = SelectObject(dc, track);
    RoundRect(dc, bx, by, bx + bw + 1, by + bh + 1, bh, bh);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    HBRUSH fill = CreateSolidBrush(rgb(p.accent));
    if (fraction > 0.0f) {
        SelectObject(dc, fill);
        const int fw = std::max((int)std::lround(bw * fraction), bh);
        RoundRect(dc, bx, by, bx + fw + 1, by + bh + 1, bh, bh);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(track);
    DeleteObject(fill);
    if (!count.empty()) {
        HFONT caption = font(ui::size::Caption, FW_NORMAL);
        centred(count, caption, rgb(p.ink3), by + dp(16.0f));
        DeleteObject(caption);
    }
    const std::string version = std::string("v") + MMDX12_VERSION;
    HFONT caption = font(ui::size::Caption, FW_NORMAL);
    centred(version, caption, rgb(p.ink3), height - dp(28.0f));
    DeleteObject(caption);
    BitBlt(hdc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}
}  // namespace

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // A --ui-script or MCP input step owns mouse and keyboard: real input (the user's cursor over or off the window) would race it.
    const bool scripted = (!options_.uiScript.empty() || mcpInputActive_) &&
                          ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE ||
                           msg == WM_NCMOUSELEAVE || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST));
    if (scripted) return msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE ? 0 : DefWindowProcW(hwnd, msg, wParam, lParam);
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            minimized_ = true;
        } else {
            minimized_ = false;
            if (ctx_.Device() && !loading_) ctx_.Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_SETTINGCHANGE:   // the user flipped Windows' light / dark mode ("ImmersiveColorSet")
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) themeDirty_ = true;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (loading_) {  // splash while the renderer initialises (GDI: nothing else is ready yet)
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            float fraction = 0.0f;
            std::string status;
            if (renderInitProgress_) {
                fraction = renderInitProgress_->fraction.load();
                status = renderInitProgress_->Status();
            }
            const uint32_t expected = GetExpectedShaderCount();
            const std::string count = expected > 0 ? std::to_string(std::min(GetCompiledShaderCount(), expected)) + " / " +
                                                         std::to_string(expected) + Tr(" 셰이더")
                                                   : std::string();
            const UINT winDpi = GetDpiForWindow(hwnd);
            const float dpi = options_.uiScale > 0.0f ? options_.uiScale : (winDpi > 0 ? (float)winDpi / 96.0f : 1.0f);
            PaintSplash(hdc, r, dpi, fraction, status, count);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_DROPFILES: {   // dragged files: AppDrop.cpp routes model/motion/pose/audio/project, shader packs stay in PollShaderPacks
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[MAX_PATH * 2] = {};
            if (DragQueryFileW(drop, i, buf, (UINT)std::size(buf))) droppedFiles_.emplace_back(buf);
        }
        DragFinish(drop);
        return 0;
    }
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

    const std::wstring title = L"MMDX12 v" + Utf8ToWide(MMDX12_VERSION);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    DragAcceptFiles(hwnd_, TRUE);   // shader pack zips (WM_DROPFILES)
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingTransparentPayload = true;
    // Drag fields (bone / camera / light values): a click without dragging types a number, as in DCC tools.
    io.ConfigDragClickToInputText = true;
    // The studio's panel arrangement persists next to the exe. Scripted / captured runs start from the default layout.
    if (options_.uiScript.empty() && options_.quitAfterFrames == 0) {
        iniPath_ = PathToUtf8(ExecutableDir() / L"mmdx12_layout.ini");
        io.IniFilename = iniPath_.c_str();
    } else {
        io.IniFilename = nullptr;
    }

    assetsDir_ = ExecutableDir() / L"assets";
    if (!std::filesystem::exists(assetsDir_ / L"fonts")) {
        std::filesystem::path found = FindUpward(ExecutableDir(), L"assets/fonts");
        if (!found.empty()) assetsDir_ = found / L"assets";
    }
    if (!ui::LoadFonts(assetsDir_)) LOG_WARN("UI fonts not found under %s, using system fonts", PathToUtf8(assetsDir_).c_str());
    // --ui-scale: a fixed scale instead of the monitor's (high-DPI captures on any monitor)
    ui::ApplyStyle(options_.uiScale > 0.25f ? std::min(options_.uiScale, 4.0f) : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_));

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
        // An offline render (video/still) keeps going while minimized.
        if (minimized_ && offline_.mode == OfflineMode::None && !shutdownPending_) {
            PumpMcp(true);
            Sleep(16);
            continue;
        }
        RenderFrame();
    }
}

void App::RenderFrame() {
    // Device loss (TDR / driver reset): the swap chain and every GPU resource are gone, so nothing may record or
    // present any more. Surface it once and quit; no device re-creation (spec fix C).
    if (ctx_.DeviceLost()) {
        audio_.Unload();   // stop audio before leaving
        if (!HeadlessRun()) {
            // Unattended runs (--frames / --ui-script / benchmarks / offline CLI jobs) must not block on a dialog.
            MessageBoxW(hwnd_, Utf8ToWide(Tr("GPU 드라이버가 재설정되어(디바이스 제거) 렌더링을 계속할 수 없습니다. "
                                             "프로그램을 종료합니다.\n자세한 내용은 mmdx12.log 를 확인하세요.")).c_str(),
                        L"MMDX12", MB_ICONERROR);
        }
        running_ = false;
        return;
    }

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double nowSec = (double)now.QuadPart / (double)freq.QuadPart;
    double dt = nowSec - timeSeconds_;
    dt = std::clamp(dt, 0.0, 0.25);
    timeSeconds_ = nowSec;
    if (!pendingGuardToast_.empty()) {
        toast_ = Toast{pendingGuardToast_, {}, {}, false, timeSeconds_ + 8.0};
        pendingGuardToast_.clear();
    }

    if (screen_ == Screen::BenchRun) UpdateBenchRun(); // frame timing first thing in the frame

    // Thumbnails: renders/uploads happen outside frame recording, only on menu screens (and in the studio while the
    // add popup's card grid is open: a render waits for the GPU and borrows the scene targets).
    if (thumbsClearPending_) {
        thumbs_.Clear();
        thumbsClearPending_ = false;
    }
    if (screen_ == Screen::Select || screen_ == Screen::BenchLobby || screen_ == Screen::Loading || screen_ == Screen::Shaders ||
        screen_ == Screen::Mme || screen_ == Screen::Gallery || (screen_ == Screen::Studio && studioThumbsShown_))
        thumbs_.Pump();
    else if (screen_ == Screen::Play && studioThumbsShown_)   // the play bar's pack picker: image previews only
        thumbs_.Pump(true);
    studioThumbsShown_ = false;

    ImGui_ImplDX12_NewFrame();
    if (themeDirty_) {
        themeDirty_ = false;
        ApplyTheme();
    }
    ImGui_ImplWin32_NewFrame();
    uiprobe::BeginFrame();   // the widgets of the frame just drawn become ui_items / ui_click targets
    PumpUiScript();
    PumpMcp(false);
    ImGui::NewFrame();
    ui::NewFrame((float)dt);

    PollScan();
    PollLoad();
    PollDroppedFiles();
    PollShaderPacks();
    PollStageStore();
    PollProbeScene();
    UpdateUpdate();
    UpdateComponentUi();

    switch (screen_) {
    case Screen::Scanning: DrawScanning(); break;
    case Screen::Select:
        UpdateVideoProbe();
        DrawSelect();
        DrawVideoRenderDialog();
        DrawFormationModal();
        DrawRecoveryPrompt();
        DrawUpdateNotice();
        DrawToast();
        DrawRayMmeConfirm();
        break;
    case Screen::Loading: DrawLoading(); break;
    case Screen::Play:
        UpdatePlay(dt);
        UpdateOffline();          // CLI trigger (may switch to Screen::Offline)
        if (screen_ == Screen::Play) {
            DrawPlayOverlay();
            DrawToast();
        }
        DrawRayMmeConfirm();
        break;
    case Screen::Offline:
        UpdateOffline();
        DrawOfflineOverlay();
        break;
    case Screen::BenchLobby: DrawBenchLobby(); break;
    case Screen::Shaders: DrawShaders(); DrawRayMmeConfirm(); break;
    case Screen::Mme: DrawMme(); DrawRayMmeConfirm(); break;
    case Screen::Gallery: DrawGallery(); DrawRayMmeConfirm(); break;
    case Screen::BenchRun: DrawBenchRunOverlay(); break;
    case Screen::BenchResult: DrawBenchResult(); break;
    case Screen::BenchRender: DrawRenderBenchOverlay(); break;
    case Screen::Studio:
        UpdateStudio(dt);
        if (screen_ == Screen::Studio) UpdateOffline();   // CLI render trigger (may switch to Screen::Offline)
        if (screen_ == Screen::Studio) DrawStudio();
        DrawRayMmeConfirm();
        break;
    }
    DrawShutdownCountdown();

    ImGui::Render();

    if (!running_) return;

    ApplyColorLut();
    ID3D12GraphicsCommandList* cmd = ctx_.BeginFrame();
    FrameView view;
    const bool inScene = scene_ && (screen_ == Screen::Play || screen_ == Screen::BenchRun);
    if (screen_ == Screen::Offline && (scene_ || (offline_.studio && studio_))) {
        RecordOfflineFrame(cmd);
    } else if (screen_ == Screen::BenchRender && scene_) {
        RecordRenderBenchFrame(cmd);
    } else if (offline_.background && scene_ && screen_ == Screen::Select) {
        RecordOfflineFrame(cmd);          // the sample render behind the dialog: nothing is presented
        renderer_.ClearBackBuffer(cmd);   // the UI draws on a clean surface as usual
    } else {
        bool studioGi = false;
        if (screen_ == Screen::Studio && studio_) {
            UpdateStudioScene();
            // keyed effect parameters follow the playhead and undo / timeline edits change the stack without a settings edit
            {
                RenderSettings rs = renderer_.Settings();
                rs.packEffects = UsableEffectStack();
                renderer_.SetSettings(rs);
            }
            BuildStudioFrameView(view);
            StudioAddQuadViews(view);
            studioGi = StudioGiActive();
        } else if (inScene) {
            const float frame = (float)(playTime_ * scene_->motionFps);
            UpdateScene(frame);
            BuildFrameView(frame, view);
            // Frame Generation only for the live play scene: never the benchmark run, captures (which must read the
            // real frame) or when the option is off.
            view.frameGen = screen_ == Screen::Play && options_.capturePath.empty() && !ctx_.CapturePending();
            lastLiveCamera_ = view.camera;
            haveLastLiveCamera_ = true;
        }
        if (studioGi) RecordStudioGiViewport(cmd, view);
        else renderer_.Render(cmd, view);
    }
    // M5-U crash guard: packs active in this frame that were not active in the previous one get a marker before the
    // frame is presented; the marker is cleared after the Present succeeded. A crash during the next frames leaves the
    // pack as a suspect for the next start.
    {
        std::set<std::string> active;
        CollectFrameActivePacks(active);
        frameBeganPacks_.clear();
        for (const std::string& id : active)
            if (!frameActivePacks_.count(id)) {
                packGuard_.Begin(id);
                frameBeganPacks_.push_back(id);
            }
        frameActivePacks_ = std::move(active);
    }
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

    // DLSS Frame Generation: copy the final (with UI) frame and generate the intermediate frames; EndFrame then
    // paces them (generated frames, then the held real frame). Captures always read the real frame.
    const bool frameGenPresented =
        inScene && screen_ == Screen::Play && options_.capturePath.empty() && !ctx_.CapturePending() &&
        renderer_.FrameGenEvaluate(cmd);

    bool quit = false;
    bool sceneCapture = false;   // --capture-scene: read the final image once this frame is submitted
    const bool countFrame = options_.startScreen == "studio"
                                ? screen_ != Screen::Scanning && screen_ != Screen::Loading
                                : inScene || screen_ == Screen::Offline || !options_.startScreen.empty();
    if (countFrame) {
        ++framesInScene_;
        // launch-to-first-frame: the first counted frame's time since Run started (STARTUP total)
        if (framesInScene_ == 1) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            LOG_INFO("STARTUP first frame: %.0f ms (launch to first frame)",
                     (double)(now.QuadPart - startupRunBegan_.QuadPart) * 1000.0 / (double)startupFreq_.QuadPart);
        }
        // --frames runs: average GPU time over the second half (after loading hitches settle)
        if (inScene && options_.quitAfterFrames > 0 && framesInScene_ > options_.quitAfterFrames / 2) {
            gpuMsSum_ += renderer_.Stats().gpuFrameMs;
            ++gpuMsCount_;
        }
        if (options_.quitAfterFrames > 0 && framesInScene_ >= options_.quitAfterFrames) {
            if (!options_.capturePath.empty()) {
                if (options_.captureSceneOnly && inScene) sceneCapture = true;   // read after EndFrame
                else {
                    if (options_.captureSceneOnly) LOG_WARN("--capture-scene needs a play scene: capturing the window");
                    ctx_.RequestCapture(options_.capturePath);
                }
            }
            quit = true;
        }
    }
    if (frameGenPresented && !ctx_.CapturePending()) {
        const int n = renderer_.FrameGenGeneratedCount();
        ID3D12Resource* generated[3];
        for (int i = 0; i < n && i < 3; ++i) generated[i] = renderer_.FrameGenGeneratedFrame(i);
        // Even spacing over one rendered frame's wall time (the previous frames' rate).
        const float frameMs = ImGui::GetIO().Framerate > 1.0f ? 1000.0f / ImGui::GetIO().Framerate : 16.7f;
        ctx_.EndFrameFrameGen(renderer_.Settings().vsync, n, generated, renderer_.FrameGenRealFrame(), frameMs);
    } else {
        ctx_.EndFrame(renderer_.Settings().vsync);
    }
    // The frame is on screen: its pack markers are no longer crash suspects. On a device loss the markers stay, so
    // the next start quarantines the pack (the guard's whole point).
    if (!ctx_.DeviceLost())
        for (const std::string& id : frameBeganPacks_) packGuard_.Clear(id);
    frameBeganPacks_.clear();
    if (sceneCapture) {
        ImageRGBA8 img;
        if (renderer_.ReadFinalImage(img) && !img.mips.empty() && !img.mips[0].pixels.empty() &&
            SavePngRGBA8(options_.capturePath, img.mips[0].width, img.mips[0].height, img.mips[0].pixels.data(),
                         img.mips[0].width * 4))
            LOG_INFO("captured %s (scene only)", PathToUtf8(options_.capturePath).c_str());
        else
            LOG_ERROR("capture: no scene image for %s", PathToUtf8(options_.capturePath).c_str());
    }
    if (screen_ == Screen::Offline || (offline_.background && screen_ == Screen::Select)) AfterOfflineFrame();
    else if (screen_ == Screen::BenchRender) AfterRenderBenchFrame();

    if (quit) {
        ctx_.WaitForGpu();
        if (gpuMsCount_ > 0)
            LOG_INFO("quit after %d frames (GPU %.2f ms average over the last %d)", framesInScene_,
                     gpuMsSum_ / gpuMsCount_, gpuMsCount_);
        else
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

namespace {

std::string LibraryRel(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string rel = PathToUtf8(p.lexically_normal().lexically_relative(root.lexically_normal()));
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
}

nlohmann::json ReadOverridesFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

const char* KindName(AssetKind k) {
    return k == AssetKind::Character ? "character" : k == AssetKind::Stage ? "stage" : k == AssetKind::Hidden ? "hidden" : "auto";
}

} // namespace

LibraryOverrides App::LoadLibraryOverrides() const {
    LibraryOverrides out;
    const nlohmann::json j = ReadOverridesFile(ExecutableDir() / L"library_overrides.json");
    const std::string key = PathToUtf8(ResolveLibraryPath().lexically_normal());
    if (!j.contains(key) || !j[key].is_object()) return out;
    for (auto& [rel, v] : j[key].items()) {
        const std::string s = v.is_string() ? v.get<std::string>() : std::string();
        const AssetKind k = s == "character" ? AssetKind::Character : s == "stage" ? AssetKind::Stage
                          : s == "hidden" ? AssetKind::Hidden : AssetKind::Auto;
        if (k != AssetKind::Auto) out.byPath[rel] = k;
    }
    return out;
}

void App::SetLibraryOverride(const std::vector<std::filesystem::path>& files, AssetKind kind) {
    const std::filesystem::path file = ExecutableDir() / L"library_overrides.json";
    nlohmann::json j = ReadOverridesFile(file);
    const std::filesystem::path root = ResolveLibraryPath();
    const std::string key = PathToUtf8(root.lexically_normal());
    if (!j.contains(key) || !j[key].is_object()) j[key] = nlohmann::json::object();
    for (const auto& f : files) {
        const std::string rel = LibraryRel(f, root);
        if (kind == AssetKind::Auto) j[key].erase(rel);
        else j[key][rel] = KindName(kind);
        LOG_INFO("library override: %s -> %s", rel.c_str(), KindName(kind));
    }
    std::ofstream(file, std::ios::binary) << j.dump(2);
    StartScan();
}

void App::ClearHiddenOverrides() {
    std::vector<std::filesystem::path> hidden;
    const std::filesystem::path root = ResolveLibraryPath();
    for (auto& [rel, k] : LoadLibraryOverrides().byPath)
        if (k == AssetKind::Hidden) hidden.push_back(root / Utf8ToPath(rel));
    if (!hidden.empty()) SetLibraryOverride(hidden, AssetKind::Auto);
}

void App::StartScan() {
    revalidateProgress_.cancel.store(true, std::memory_order_relaxed);
    revalidating_ = false;
    pendingLibraryResult_.reset();
    screen_ = Screen::Scanning;
    thumbsClearPending_ = true;
    scanProgress_.filesVisited.store(0, std::memory_order_relaxed);
    scanProgress_.filesTotal.store(0, std::memory_order_relaxed);
    scanProgress_.cancel.store(false, std::memory_order_relaxed);
    const std::filesystem::path path = ResolveLibraryPath();
    const std::filesystem::path templateDir = ExecutableDir() / L"assets" / L"library_template";
    const LibraryOverrides overrides = LoadLibraryOverrides();
    scanFuture_ = std::async(std::launch::async, [this, path, templateDir, overrides] {
        CreateLibrarySkeleton(path, templateDir);  // first run: suggest characters/ stages/ songs/
        return ScanLibrary(path, &scanProgress_, &overrides);
    });
}

void App::StartRevalidation() {
    revalidating_ = true;
    revalidateProgress_.cancel.store(false, std::memory_order_relaxed);
    revalidateProgress_.filesVisited.store(0, std::memory_order_relaxed);
    revalidateProgress_.filesTotal.store(0, std::memory_order_relaxed);
    const std::filesystem::path path = ResolveLibraryPath();
    const std::filesystem::path templateDir = ExecutableDir() / L"assets" / L"library_template";
    const LibraryOverrides overrides = LoadLibraryOverrides();
    revalidateFuture_ = std::async(std::launch::async, [this, path, templateDir, overrides] {
        CreateLibrarySkeleton(path, templateDir);
        return ScanLibrary(path, &revalidateProgress_, &overrides);
    });
}

void App::RestoreSelectionsFromSettings() {
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
}

void App::ApplyStartScreen() {
    screen_ = Screen::Select;
    if (options_.startScreen == "bench" || options_.startScreen == "bench-gi") {
        screen_ = Screen::BenchLobby;
        if (options_.startScreen == "bench-gi") benchCategory_ = kBenchGiRender;
        RefreshLeaderboard();
    } else if (options_.startScreen == "stages") {
        libraryTab_ = 1;
    } else if (options_.startScreen == "songs") {
        libraryTab_ = 2;
    } else if (options_.startScreen == "shaders" || options_.startScreen == "shaders-online" ||
               options_.startScreen == "shaders-mme") {
        screen_ = Screen::Shaders;
        shaderTab_ = options_.startScreen == "shaders-online" ? 1 : options_.startScreen == "shaders-mme" ? 2 : 0;
    } else if (options_.startScreen == "mme") {
        screen_ = Screen::Mme;
    } else if (options_.startScreen == "gallery") {
        screen_ = Screen::Gallery;
    } else if (options_.startScreen == "settings") {
        lobbyTab_ = 2;
    } else if (options_.startScreen == "studio") {
        if (!options_.project.empty()) {
            const std::filesystem::path p = std::filesystem::absolute(options_.project);
            if (ToLowerAscii(PathToUtf8(p.extension())) == ".pmm") StartStudioPmmImport(p);
            else StartStudioProjectLoad(p, false);
            return;
        }
        if (options_.character.empty()) {  // no preselection asked for: a new empty project
            StartStudioEmpty();
            return;
        }
        if (selCharacter_ >= 0) {
            StartStudioLoad(&library_.characters[(size_t)selCharacter_],
                            selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr,
                            selSong_ >= 0 ? &library_.songs[(size_t)selSong_] : nullptr);
            return;
        }
        LOG_ERROR("--screen studio needs --character");
    } else if (options_.startScreen == "video") {
        // needs --character and --song; with --offline-video <file> it starts the render without the dialog
        videoDialogOpen_ = true;
        ResetVideoRange();
        if (!options_.offlineVideo.empty() && selCharacter_ >= 0 && selSong_ >= 0) {
            if (!renderer_.OfflineSupported()) {
                LOG_ERROR("offline render unavailable (needs DXR)");
                running_ = false;
                return;
            }
            StartVideoRenderLoad();
            return;
        }
    }

    // One-shot command line auto-actions.
    if (options_.autoplay && selCharacter_ >= 0 && selSong_ >= 0) {
        StartLobbyLoad(LoadTarget::Play, false);
        options_.autoplay = false;
    } else if (!options_.benchmarkCategory.empty()) {
        int cat = 0;
        for (size_t i = 0; i < std::size(kBenchCategories); ++i)
            if (ToLowerAscii(kBenchCategories[i].id) == ToLowerAscii(options_.benchmarkCategory)) { cat = (int)i; break; }
        if (kBenchCategories[cat].offline) {
            benchCategory_ = cat;
            options_.benchmarkCategory.clear();
            if (!renderer_.OfflineSupported()) {
                LOG_ERROR("render benchmark needs DXR (offline renderer unavailable)");
                running_ = false;
                return;
            }
            renderBench_.fromCli = true;
            StartRenderBenchLoad();
            return;
        }
        if (kBenchCategories[cat].path != RenderPath::Raster && !renderer_.RayTracingSupported()) {
            LOG_ERROR("benchmark %s needs DXR 1.1", kBenchCategories[cat].id);
            options_.benchmarkCategory.clear();
            return;
        }
        benchCategory_ = cat;
        // Same library-picked scene as the lobby's "측정 시작" button.
        const BenchmarkScene sc = PickBenchmarkScene(library_);
        benchSubmittable_ = sc.Runnable();
        if (sc.Runnable()) {
            LOG_INFO("benchmark scene: %s / %s / %s", library_.characters[(size_t)sc.character].id.c_str(),
                     sc.stage >= 0 ? library_.stages[(size_t)sc.stage].id.c_str() : "studio",
                     library_.songs[(size_t)sc.song].id.c_str());
            StartLoad(LoadTarget::Benchmark, &library_.characters[(size_t)sc.character],
                      sc.stage >= 0 ? &library_.stages[(size_t)sc.stage] : nullptr, &library_.songs[(size_t)sc.song]);
        } else {
            LOG_ERROR("benchmark: the library needs at least one character and one song");
        }
        options_.benchmarkCategory.clear();
    }
}

void App::ApplyRevalidatedLibrary(LibraryScanResult fresh) {
    if (LibraryScanResultsEqual(library_, fresh)) {
        return;
    }
    LOG_INFO("library: background revalidation found changes; updating library");
    std::string charId = (selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size())
                             ? library_.characters[(size_t)selCharacter_].id : settings_.lastCharacter;
    std::string stageId = (selStage_ >= 0 && (size_t)selStage_ < library_.stages.size())
                            ? library_.stages[(size_t)selStage_].id : settings_.lastStage;
    std::string songId = (selSong_ >= 0 && (size_t)selSong_ < library_.songs.size())
                           ? library_.songs[(size_t)selSong_].id : settings_.lastSong;

    library_ = std::move(fresh);
    thumbsClearPending_ = true;

    selCharacter_ = -1;
    if (!charId.empty()) {
        const std::string needle = ToLowerAscii(charId);
        for (size_t i = 0; i < library_.characters.size(); ++i) {
            if (ToLowerAscii(library_.characters[i].id) == needle) { selCharacter_ = (int)i; break; }
        }
    }
    selStage_ = -1;
    if (!stageId.empty()) {
        const std::string needle = ToLowerAscii(stageId);
        for (size_t i = 0; i < library_.stages.size(); ++i) {
            if (ToLowerAscii(library_.stages[i].id) == needle) { selStage_ = (int)i; break; }
        }
    }
    selSong_ = -1;
    if (!songId.empty()) {
        const std::string needle = ToLowerAscii(songId);
        for (size_t i = 0; i < library_.songs.size(); ++i) {
            if (ToLowerAscii(library_.songs[i].id) == needle) { selSong_ = (int)i; break; }
        }
    }
    ApplyCommandLinePreselection();
}

void App::PollScan() {
    if (scanFuture_.valid()) {
        if (scanFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            library_ = scanFuture_.get();
            RestoreSelectionsFromSettings();
            ApplyCommandLinePreselection();
            ApplyStartScreen();
        }
        return;
    }

    if (revalidateFuture_.valid()) {
        if (revalidateFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            pendingLibraryResult_ = std::make_unique<LibraryScanResult>(revalidateFuture_.get());
            revalidating_ = false;
        }
    }

    if (pendingLibraryResult_) {
        // the gallery is as safe as the lobby: it owns no scene, and a freshly installed stage should already be
        // in gallery / library the moment the user looks at them
        const bool isSafe = (screen_ == Screen::Select || screen_ == Screen::BenchLobby || screen_ == Screen::Gallery) &&
                            !loadFuture_.valid();
        if (isSafe) {
            ApplyRevalidatedLibrary(std::move(*pendingLibraryResult_));
            pendingLibraryResult_.reset();
        }
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

namespace {
// Windows' "choose your default app mode" (HKCU ...\Themes\Personalize\AppsUseLightTheme); light when unreadable.
bool SystemPrefersDark() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}
} // namespace

void App::ApplyTheme() {
    const int mode = options_.theme >= 0 ? options_.theme : settings_.theme;
    const bool dark = mode == 2 || (mode == 0 && SystemPrefersDark());
    ui::SetTheme(dark ? ui::Theme::Dark : ui::Theme::Light);
    if (ImGui::GetCurrentContext()) ui::ApplyStyle(ui::Dpi());
    if (hwnd_) {
        // Dark title bar / caption buttons (Windows 10 20H1+; older builds ignore the attribute).
        const BOOL useDark = dark ? TRUE : FALSE;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
            using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            if (auto fn = reinterpret_cast<SetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
                fn(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof(useDark));
            FreeLibrary(dwm);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::SetThemeSetting(int theme) {
    settings_.theme = std::clamp(theme, 0, 2);
    settings_.Save(settingsPath_);
    themeDirty_ = true;
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

// The effect stack the renderer runs: the studio project's own stack evaluated at the playhead, else the play-mode one.
// M5-U: quarantined (crash guard) effect packs never run. The stack entry stays so the manager can offer "다시 사용".
std::vector<EffectStackEntry> App::UsableEffectStack() const {
    std::vector<EffectStackEntry> out = studio_ ? StudioEvaluatedEffects((float)(studio_->time * kMmdFps)) : settings_.effectStack;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [this](const EffectStackEntry& e) {
                                 const ShaderPack* pk = ShaderPacks().Find(e.pack);
                                 return !pk || !PackUsable(pk);
                             }),
              out.end());
    return out;
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
    rs.autoLuminous = settings_.autoLuminous;
    rs.screenLights = settings_.screenLights;
    rs.taa = settings_.taa;
    rs.exposure = settings_.exposure;
    rs.renderPath = (RenderPath)settings_.renderPath;
    rs.pbrDebug = (PbrDebug)std::clamp(options_.pbrDebug, 0, 7);
    if (shadingOverride_ >= 0) rs.shading = (ViewShading)std::clamp(shadingOverride_, 0, 2);
    rs.upscaler = (UpscalerKind)settings_.upscaler;
    rs.upscalerQuality = (UpscalerQuality)settings_.upscalerQuality;
    rs.frameGen = settings_.dlssFrameGen;
    rs.dlssModel = (DlssModel)settings_.dlssModel;
    rs.dlssRayReconstruction = settings_.dlssRayReconstruction;
    rs.rtaoRays = (uint32_t)settings_.rtaoRays;
    rs.ptSamples = (uint32_t)settings_.ptSamples;
    rs.ptBounces = (uint32_t)settings_.ptBounces;
    rs.dof = settings_.dof;
    rs.dofAperture = settings_.dofAperture;
    rs.volumetric = settings_.volumetric || stageFogVolumetric_;
    rs.volumetricDensity = settings_.volumetricDensity;
    rs.volumetricMode = settings_.volumetricMode;
    rs.volumetricMultiScatter = settings_.volumetricMultiScatter;
    rs.volumetricAnisotropy = settings_.volumetricAnisotropy;
    rs.bloomConvolution = settings_.bloomConvolution;
    rs.bloomIntensity = settings_.bloomIntensity;
    rs.bloomThreshold = settings_.bloomThreshold;
    rs.bloomLook = settings_.bloomLook;
    rs.lutIntensity = settings_.lutIntensity;
    // effect packs (the shader screens edit them); while the studio is open, the project's own stack evaluated at the
    // playhead (keyed parameters, docs/effect_ecosystem.md), as the MME post stack below
    rs.packEffects = UsableEffectStack();
    // M6b: the MME post stack. --mme overrides for one run; while the studio is open the project's own stack
    // replaces the play-mode one (docs/mme/mme_design.md §2.10). The global MME switch off disables it everywhere.
    rs.mmeEffects = !MmeGlobalEnabled() ? std::vector<MmeEffectEntry>{}
                   : options_.mmeSet ? options_.mmeEffects
                   : studio_        ? StudioMmeEntries()
                                    : settings_.mmeEffects;
    // M5c: a `.x` stack entry is an effect object (its `.fx` runs via FrameView::mmeEffectObjects, its mesh via the
    // model list), not a post effect: keep it out of the post chain.
    rs.mmeEffects.erase(std::remove_if(rs.mmeEffects.begin(), rs.mmeEffects.end(),
                                       [](const MmeEffectEntry& e) {
                                           std::wstring ext = e.file.extension().wstring();
                                           for (wchar_t& c : ext) c = (wchar_t)towlower(c);
                                           return ext == L".x";
                                       }),
                        rs.mmeEffects.end());
    if (options_.mmePanelSet)
        for (MmeEffectEntry& e : rs.mmeEffects) { e.si = options_.mmePanelSi; e.tr = options_.mmePanelTr; }
    rs.fixedResolution = false;
    rs.headless = false;
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
                    const SongAsset* song, const std::vector<PerformerCast>* formation) {
    StopVideoProbe(false);
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
    const SongAsset so = SongAsChosen(*song);
    // M8b: the song's video layers (classification + ini override), resolved here and re-applied to the package after
    // the worker runs (LoadScenePackage resets the package struct it fills).
    const std::vector<VideoLayerDesc> videos = ResolveSongVideos(so);
    const bool hasFormation = formation != nullptr && !formation->empty();
    const std::vector<PerformerCast> form = hasFormation ? *formation : std::vector<PerformerCast>{};
    // S1b: the MMDX stage variant: --stage-variant, else the ini choice for that stage (empty = the manifest's default).
    const std::string stageVariant =
        !hasStage ? std::string()
                  : (!options_.stageVariant.empty() ? options_.stageVariant : settings_.StageVariant(s.id));
    loadFuture_ = std::async(std::launch::async, [=, pkg = loadPackage_.get(), this] {
        const bool ok = LoadScenePackage(c, hasStage ? &s : nullptr, so, *pkg, &loadProgress_, &loadError_,
                                         hasFormation ? &form : nullptr, MaterialChoiceResolver(), stageVariant);
        pkg->videos = videos;   // M8b: LoadScenePackage resets the package; re-apply the resolved video layers
        pkg->formation = form;  // the cast a material-source reload reuses (unit songs)
        return ok;
    });
}

void App::PollLoad() {
    if (screen_ != Screen::Loading) return;
    if (!loadFuture_.valid()) return;
    if (loadFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    bool ok = loadFuture_.get();
    if (loadTarget_ == LoadTarget::Studio) {
        if (ok && !FinishStudioLoad()) {
            ok = false;
            loadError_ = Tr("GPU 리소스 생성 실패");
        }
        studioPackage_.reset();
        if (!ok) LOG_ERROR("%s", loadError_.c_str());
        return;
    }
    if (ok) {
        ok = BuildSceneRuntime(*loadPackage_);
        if (!ok) loadError_ = Tr("GPU 리소스 생성 실패");
    }
    loadPackage_.reset();

    if (!ok) {
        LOG_ERROR("%s", loadError_.c_str());
        return; // stay on Loading; DrawLoading shows the error + "돌아가기"
    }

    // S1b: the play scene defaults the lighting picker to "스테이지 조명" when the loaded MMDX stage has a light rig
    // (an ordinary preset chosen for other stages stays as it is).
    playLighting_ = -1;
    if (loadTarget_ == LoadTarget::Play) {
        playTime_ = options_.seekSeconds;
        options_.seekSeconds = 0;
        playing_ = true;
        // S1b: a loaded MMDX stage with a light rig defaults the lighting picker to "스테이지 조명"; an ordinary preset
        // chosen for other stages stays as it is.
        if (scene_->stage && scene_->stage->HasLights() && settings_.lighting < kLightingPresetCount)
            playLighting_ = (int)LightingPreset::Stage;
        // S9: a stage's environment.fog turns the volumetric haze on and sets its anisotropy while the stage is loaded
        // (the user's volumetric switch can still turn the pass off afterwards).
        // The switch is this scene's (stageFogVolumetric_), never written into the saved settings.
        settings_.volumetricAnisotropy = -1.0f;
        stageFogVolumetric_ = false;
        if (scene_->stage && scene_->stage->HasFog()) {
            const StageFog& fog = scene_->stage->Manifest()->environment.fog;
            stageFogVolumetric_ = true;
            settings_.volumetricAnisotropy = std::clamp(fog.anisotropy, -0.9f, 0.95f);
            ApplyRenderSettings();
        }
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
        playEditPerformer_ = 0;   // a freshly loaded scene edits the center performer
        screen_ = Screen::Play;
        // --offline-still renders exactly the --seek frame: hold playback there.
        if (!options_.offlineStill.empty() && !cliOfflineStarted_) options_.paused = true;
        if (options_.paused) {
            options_.paused = false;
            SetPlaying(false);
        }
        return;
    }

    if (loadTarget_ == LoadTarget::OfflineVideo) {
        // Rendered straight away from the select screen: paused scene, no audio playback (the encoder
        // decodes the song itself)
        playTime_ = 0;
        playing_ = false;
        useMotionCamera_ = scene_->camera != nullptr && !options_.freeCamera;
        framesInScene_ = 0;
        screen_ = Screen::Play;
        const double duration = scene_->endFrame / scene_->motionFps;
        // An explicit --offline-range wins; otherwise the dialog's chosen range (else the whole song).
        double ra = 0.0, rb = duration;
        if (options_.offlineRange[0] >= 0 || options_.offlineRange[1] >= 0) {
            ra = options_.offlineRange[0] >= 0 ? options_.offlineRange[0] : 0.0;
            rb = options_.offlineRange[1] >= 0 ? options_.offlineRange[1] : duration;
        } else if (lobbyVideoRange_[0] >= 0 || lobbyVideoRange_[1] >= 0) {
            ra = lobbyVideoRange_[0] >= 0 ? lobbyVideoRange_[0] : 0.0;
            rb = lobbyVideoRange_[1] >= 0 ? lobbyVideoRange_[1] : duration;
        }
        ra = std::clamp(ra, 0.0, duration);
        rb = std::clamp(rb, 0.0, duration);
        if (rb <= ra) { ra = 0.0; rb = duration; }   // empty range: fall back to the whole song
        lobbyVideoRange_[0] = -1.0;
        lobbyVideoRange_[1] = -1.0;
        StartOfflineVideo(ra, rb, true);
        return;
    }

    if (loadTarget_ == LoadTarget::RenderBench) {
        PoseRenderBench();
        framesInScene_ = 0;
        renderBench_.beginPending = true;
        renderBench_.cancelRequested = false;
        renderBench_.elapsed = 0;
        RenderSettings rs = renderer_.Settings();
        rs.vsync = false;               // presents must not throttle the GPU-bound render
        renderer_.SetSettings(rs);
        screen_ = Screen::BenchRender;
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
    rs.packEffects.clear();   // fixed workload: no effect packs
    rs.mmeEffects.clear();    // fixed workload: no MME post effects
    renderer_.SetSettings(rs);

    benchStartTime_ = timeSeconds_;
    framesInScene_ = 0;
    screen_ = Screen::BenchRun;
}

App::PlayPerformerRef App::PlayPerformerAt(int index) {
    PlayPerformerRef r;
    if (!scene_) return r;
    if (index < 0 || (size_t)index > scene_->extras.size()) {   // out of range: clamp back to the center
        index = 0;
        playEditPerformer_ = 0;
    }
    if (index == 0) {
        r.id = scene_->characterId;
        r.gpu = scene_->characterGpu.get();
        r.instance = scene_->character.get();
        return r;
    }
    SceneRuntime::Performer& e = scene_->extras[(size_t)(index - 1)];
    r.id = e.characterId;
    r.gpu = e.gpu.get();
    r.instance = e.instance.get();
    return r;
}

void App::ReloadPlaySceneForMaterial() {
    if (screen_ != Screen::Play || !scene_) return;
    const int ci = selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size() ? selCharacter_ : -1;
    const int si = selSong_ >= 0 && (size_t)selSong_ < library_.songs.size() ? selSong_ : -1;
    if (ci < 0 || si < 0) {   // no library scene to reload (benchmark / studio): apply at the next load
        toast_ = {Tr("재질을 저장했어요"), Tr("다음 로드 때 반영돼요"), {}, false, timeSeconds_ + 6.0};
        return;
    }
    const StageAsset* st = selStage_ >= 0 && (size_t)selStage_ < library_.stages.size() ? &library_.stages[(size_t)selStage_]
                                                                                      : nullptr;
    const bool wasPlaying = playing_;
    options_.seekSeconds = playTime_;   // the scene-load path consumes it: same time
    options_.paused = !wasPlaying;      // keep the play / pause state across the reload
    playing_ = false;
    // A unit-song formation scene reloads the same cast (the song's parts need every performer re-loaded).
    if (!scene_->formation.empty()) {
        // a copy: StartLoad unloads scene_ (and its formation) before it reads the cast
        const std::vector<PerformerCast> cast = scene_->formation;
        StartLoad(LoadTarget::Play, &cast[0].character, ResolveSongStage(library_.songs[(size_t)si]),
                  &library_.songs[(size_t)si], &cast);
        return;
    }
    StartLoad(LoadTarget::Play, &library_.characters[(size_t)ci], st, &library_.songs[(size_t)si]);
}

bool App::BuildSceneRuntime(ScenePackage& pkg) {
    auto s = std::make_unique<SceneRuntime>();
    UploadBatch batch(ctx_);

    s->character = std::make_unique<ModelInstance>(pkg.character.pmx);
    if (!pkg.characterId.empty()) {
        s->characterId = pkg.characterId;
    } else if (selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size()) {
        s->characterId = library_.characters[(size_t)selCharacter_].id;
    }
    s->characterDisplayName = !pkg.characterDisplayName.empty() ? pkg.characterDisplayName : (s->character ? s->character->Model().name : "");
    if (selStage_ >= 0 && (size_t)selStage_ < library_.stages.size()) s->stageId = library_.stages[(size_t)selStage_].id;
    s->characterGpu = renderer_.CreateModel(batch, *pkg.character.pmx, pkg.character.textures,
                                            ModelRole::Character, &pkg.character.pbr,
                                            &pkg.character.compressedTextures);
    if (!s->characterGpu) return false;
    if (cliLookActive_ && cliLook_.setsSurface && !s->characterId.empty()) {
        cliLookCharacterId_ = s->characterId;
        cliLookPrevSurface_ = settings_.CharacterShader(s->characterId);
        ShaderChoice lookChoice = cliLookPrevSurface_;
        ApplyLook(cliLook_, settings_, &lookChoice);
        settings_.SetCharacterShader(s->characterId, lookChoice);
    }
    s->characterGpu->SetDefaultPackTextureFolder(pkg.character.defaultPackTextureFolder);
    s->characterGpu->SetMaterialBlendHints(pkg.character.materialBlend, pkg.character.materialBlendDefault);

    for (size_t i = 0; i < pkg.extraCharacters.size(); ++i) {
        SceneRuntime::Performer perf;
        perf.instance = std::make_unique<ModelInstance>(pkg.extraCharacters[i].pmx);
        perf.gpu = renderer_.CreateModel(batch, *pkg.extraCharacters[i].pmx, pkg.extraCharacters[i].textures,
                                         ModelRole::Character, &pkg.extraCharacters[i].pbr,
                                         &pkg.extraCharacters[i].compressedTextures);
        if (!perf.gpu) return false;
        perf.gpu->SetDefaultPackTextureFolder(pkg.extraCharacters[i].defaultPackTextureFolder);
        perf.gpu->SetMaterialBlendHints(pkg.extraCharacters[i].materialBlend, pkg.extraCharacters[i].materialBlendDefault);
        perf.motion = i < pkg.extraMotions.size() ? pkg.extraMotions[i] : nullptr;
        if (i < pkg.extraCharacterIds.size()) perf.characterId = pkg.extraCharacterIds[i];
        if (i < pkg.extraCharacterDisplayNames.size()) perf.displayName = pkg.extraCharacterDisplayNames[i];
        s->extras.push_back(std::move(perf));
    }

    for (size_t i = 0; i < pkg.stageParts.size(); ++i) {
        LoadedModelCpu& part = pkg.stageParts[i];
        auto inst = std::make_unique<ModelInstance>(part.pmx);
        inst->UpdatePose();
        auto gpu = renderer_.CreateModel(batch, *part.pmx, part.textures, ModelRole::Stage, &part.pbr,
                                         &part.compressedTextures);
        if (!gpu) {
            LOG_WARN("stage part GPU upload failed: %s", part.pmx->name.c_str());
            continue;
        }
        gpu->SetMaterialBlendHints(part.materialBlend, part.materialBlendDefault);
        s->stages.push_back(std::move(inst));
        s->stageGpu.push_back(std::move(gpu));
    }
    // S1b: an MMDX stage's runtime: the manifest, its variant, and the rule -> material binding of every loaded part.
    if (pkg.stageManifest) {
        // S6: the stage's bundled packs join the registry ("stage:<id>/<folder>"); a broken one is one [W].
        RegisterStagePacks(*pkg.stageManifest);
        std::vector<StageRuntimePart> parts;
        for (size_t i = 0; i < s->stageGpu.size() && i < pkg.stagePartBindings.size(); ++i) {
            StageRuntimePart p;
            p.part = (int)i;
            p.gpu = s->stageGpu[i].get();
            p.binding = pkg.stagePartBindings[i];
            p.surfacePack = ResolveStageSurfacePack(*pkg.stageManifest, *p.gpu);   // S6: resolved once per part
            parts.push_back(std::move(p));
        }
        s->stage = std::make_shared<StageRuntime>();
        s->stage->Init(pkg.stageManifest, pkg.stageVariant, std::move(parts));
        // S2a: the parts' glTF punctual lights ("gltfLights": true) join the rig, named after their glTF nodes.
        std::vector<studio::SceneLight> gltfLights;
        if (pkg.stageManifest->gltfLights) {
            std::vector<std::string> taken;
            for (const studio::SceneLight& l : pkg.stageManifest->lights) taken.push_back(l.name);
            for (const LoadedModelCpu& part : pkg.stageParts)
                if (part.pmx) {
                    std::vector<studio::SceneLight> added = PartGltfLights(*pkg.stageManifest, *part.pmx, taken);
                    gltfLights.insert(gltfLights.end(), added.begin(), added.end());
                }
            s->stage->SetGltfLights(gltfLights);
        }
        const size_t gltfLightsCount = gltfLights.size();
        LOG_INFO("MMDX stage: %s variant '%s' parts %d lights %d rules %d", pkg.stageManifest->id.c_str(),
                 pkg.stageVariant.c_str(), (int)s->stage->Manifest()->models.size(),
                 (int)pkg.stageManifest->lights.size() + (int)gltfLightsCount,
                 (int)pkg.stageManifest->materials.size());
    }

    // M5c: MME effect objects (an accessory `.x` + its same-stem `.fx`). --mme-object, else the persisted play-mode
    // stack's `.x` entries (mmeEffect= lines). Each accessory becomes a prop-role mesh drawn through the MME object
    // path with its controller panel as the world transform.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> objectPairs;
    if (MmeGlobalEnabled()) {
        if (options_.mmeObjectSet) {
            objectPairs = options_.mmeObjects;
        }
        // Persisted / CLI stack `.x` entries are effect objects too (the dialog now offers `.x` files).
        const std::vector<MmeEffectEntry>& stackEntries =
            options_.mmeSet ? options_.mmeEffects : settings_.mmeEffects;
        for (const MmeEffectEntry& e : stackEntries) {
            std::wstring ext = e.file.extension().wstring();
            for (wchar_t& c : ext) c = (wchar_t)towlower(c);
            if (ext != L".x") continue;
            std::filesystem::path fx, mesh;
            ResolveMmeObjectPair(e.file, fx, mesh);
            if (!fx.empty()) objectPairs.emplace_back(fx, mesh);
        }
    }
    for (const auto& [fx, meshFile] : objectPairs) {
        SceneRuntime::EffectObject obj;
        obj.fx = fx;
        obj.meshFile = meshFile;
        obj.panel = MmeControllerPanel{};
        if (meshFile.empty()) {
            // .fx-only effect object: nothing to draw, but the post/scene part still runs.
            obj.fileName = PathToUtf8(fx.filename());
            s->effectObjects.push_back(std::move(obj));
            continue;
        }
        LoadedModelCpu cpu;
        studio::StudioPackageModel spm;
        std::string err;
        LoadProgress progress;
        if (!studio::LoadStudioModel(meshFile, studio::ModelKind::Prop, PathToUtf8(meshFile.stem()), spm, &progress,
                                     &err)) {
            LOG_WARN("MME effect object mesh load failed: %s: %s", PathToUtf8(meshFile).c_str(), err.c_str());
            obj.fileName = PathToUtf8(meshFile.filename());
            s->effectObjects.push_back(std::move(obj));
            continue;
        }
        cpu.pmx = spm.pmx;
        cpu.textures = std::move(spm.textures);
        cpu.compressedTextures = std::move(spm.compressedTextures);
        obj.inst = std::make_unique<ModelInstance>(cpu.pmx);
        obj.inst->SetRootTransform(DirectX::XMFLOAT4X4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
        obj.mesh = renderer_.CreateModel(batch, *cpu.pmx, cpu.textures, ModelRole::Stage, nullptr,
                                         &cpu.compressedTextures);
        obj.fileName = PathToUtf8(meshFile.filename());
        for (char& c : obj.fileName) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        obj.displayName = cpu.pmx->name;
        // the accessory draws only through the effect's object technique (kInvisible handled by the mesh's MME choice)
        if (obj.mesh) {
            obj.mesh->SetMmeEffect(fx);
        }
        s->effectObjects.push_back(std::move(obj));
    }

    batch.Submit();
    // M8a: the bundled stage's motions (a stage part with no physics). Their models take the dynamic (character) BLAS
    // path.
    for (const StageMotionBinding& b : pkg.stageMotions) {
        if (!b.motion || b.part < 0 || (size_t)b.part >= s->stageGpu.size()) continue;
        s->stageMotions.push_back(b);
        s->stageGpu[(size_t)b.part]->SetDynamic(true);
    }

    s->motion = pkg.motion;
    s->camera = pkg.camera;
    s->videos = std::move(pkg.videos);   // M8b: the song's video layers
    s->formation = std::move(pkg.formation);   // the cast of a unit-song formation scene (material-source reload)
    // M8a: the loaded stage's library id (a bundled stage loaded for a song), for the MME choice / display scale keys.
    if (!pkg.stageId.empty()) s->stageId = pkg.stageId;
    for (const VmdLightKey& k : pkg.lightKeys) studio::UpsertKey(s->lightTrack, studio::LightKf{(int)k.frame, k.color, k.direction});
    for (const VmdShadowKey& k : pkg.shadowKeys)
        studio::UpsertKey(s->shadowTrack, studio::ShadowKf{(int)k.frame, k.mode, k.distance});
    if (studio::IsDefaultLightTrack(s->lightTrack)) s->lightTrack.clear();
    if (studio::IsDefaultShadowTrack(s->shadowTrack)) s->shadowTrack.clear();
    if (!s->lightTrack.empty() || !s->shadowTrack.empty())
        LOG_INFO("camera vmd: light track %zu keys, self-shadow track %zu keys", s->lightTrack.size(), s->shadowTrack.size());
    s->endFrame = pkg.endFrame;
    s->motionFps = pkg.motionFps;
    s->audioPath = pkg.audioPath;
    s->hasAudio = !pkg.audioPath.empty() && audio_.Load(pkg.audioPath);
    if (s->hasAudio)
        LOG_INFO("audio: playing %s", PathToUtf8(pkg.audioPath.filename()).c_str());
    if (s->hasAudio) s->endFrame = std::max(s->endFrame, (float)(audio_.DurationSeconds() * s->motionFps));

    scene_ = std::move(s);
    return true;
}

void App::UnloadScene() {
    if (!scene_) return;
    if (offline_.background) CancelBackgroundProbe();
    bgProbe_.sceneKey = 0;
    if (offline_.mode != OfflineMode::None) {
        renderer_.CancelOffline();
        if (offline_.sink) {
            offline_.sink->Abort();
            offline_.sink.reset();
        }
        ImageRGBA8 pending;
        renderer_.EndFrameReadback();
        while (renderer_.PollFinalReadback(pending, true)) {}
        offline_ = OfflineJob{};
    }
    ctx_.WaitForGpu();
    renderer_.ReleaseVideoLayers();   // M8b: stop the song's video decode threads, free their buffers
    haveLastLiveCamera_ = false;
    if (scene_ && scene_->colliderFuture.valid()) scene_->colliderFuture.wait();
    // S6: the scene's stage-bundled packs leave the registry with it (a reloaded stage re-reads its folders)
    if (scene_ && scene_->stage && scene_->stage->Valid())
        ShaderPacks().UnregisterStagePacks(scene_->stage->Manifest()->id);
    scene_.reset();
    mmeControllerQueue_.clear();      // auto-loaded MME controllers are per scene (freed with it)
    mmeControllerQueued_.clear();
    mmeControllerFailed_.clear();
    playAutoFocus_.Reset();
    playEditPerformer_ = 0;
    playPanelOpen_ = playPanelReopen_;   // the 화면 효과 panel does not carry into the next scene, except
    playPanelReopen_ = false;            // after its own MME apply reloaded this one (the user sees the result)
    playPanelT_ = 0.0f;
    playMmeDraft_.valid = false;     // a fresh scene re-captures the MME draft when its tab opens
    audio_.Unload();
    playing_ = false;
    playLighting_ = -1;   // S1b: the next scene picks its own default (stage rig / the saved preset)
    if (stageFogVolumetric_) {   // S9: the stage fog's haze ends with its scene
        stageFogVolumetric_ = false;
        ApplyRenderSettings();
    }
}

// Auto workflow (완전 자동), play scenes only: a required CONTROLOBJECT controller was missing, but its file may
// exist next to the model / its effects (FindMmeControllerFile). Queue the load (deduped by lower-case file name;
// never a name already in the scene, already queued / loaded, or failed before). A Ray file is refused inside
// FindMmeControllerFile (treated as not found).
void App::RequestMmeController(const std::filesystem::path& modelPath, const std::string& skip) {
    if (!scene_ || studio_ || skip.empty()) return;
    const std::filesystem::path file = FindMmeControllerFile(modelPath, skip);
    if (file.empty()) return;
    const std::string key = ToLowerAscii(PathToUtf8(file.filename()));
    if (key.empty()) return;
    if (mmeControllerQueued_.count(key) || mmeControllerFailed_.count(key)) return;
    const std::vector<std::string>& names = MmeSceneNames();
    if (std::find(names.begin(), names.end(), key) != names.end()) return;   // already in the scene
    mmeControllerQueued_.insert(key);
    mmeControllerQueue_.push_back({file, PathToUtf8(modelPath)});
}

// Loads queued controller models synchronously (controller PMX files are small), before the models' MME choices
// are applied. A failed load is LOG_WARN'd once and the name is marked failed (never retried).
void App::PollMmeControllers() {
    if (!scene_ || mmeControllerQueue_.empty()) return;
    for (const auto& [file, requester] : mmeControllerQueue_) {
        SceneRuntime::Controller c;
        c.path = file;
        c.fileName = ToLowerAscii(PathToUtf8(file.filename()));
        LoadedModelCpu cpu;
        studio::StudioPackageModel spm;
        std::string err;
        LoadProgress progress;
        if (!studio::LoadStudioModel(file, studio::ModelKind::Prop, PathToUtf8(file.stem()), spm, &progress, &err)) {
            LOG_WARN("MME auto: controller load failed: %s: %s", PathToUtf8(file).c_str(), err.c_str());
            mmeControllerFailed_.insert(c.fileName);
            continue;
        }
        cpu.pmx = spm.pmx;
        cpu.textures = std::move(spm.textures);
        cpu.compressedTextures = std::move(spm.compressedTextures);
        c.inst = std::make_unique<ModelInstance>(cpu.pmx);
        c.inst->SetRootTransform(DirectX::XMFLOAT4X4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
        c.inst->UpdatePose();
        {
            UploadBatch batch(ctx_);
            c.gpu = renderer_.CreateModel(batch, *cpu.pmx, cpu.textures, ModelRole::Stage, nullptr,
                                          &cpu.compressedTextures);
            batch.Submit();
        }
        if (!c.gpu) {
            LOG_WARN("MME auto: controller GPU upload failed: %s", PathToUtf8(file).c_str());
            mmeControllerFailed_.insert(c.fileName);
            continue;
        }
        c.displayName = cpu.pmx->name;
        LOG_INFO("MME auto: loaded controller %s for %s", PathToUtf8(file).c_str(), requester.c_str());
        scene_->controllers.push_back(std::move(c));
        // the scene model set changed: the per-model skip cache no longer applies (the effect applies from here on)
        mmeAutoSkip_.clear();
    }
    mmeControllerQueue_.clear();
}

void App::UpdateScene(float frame) {
    // 1/30 s of physics per frame at most: a slow frame then drops simulated time instead of asking for more substeps
    // (five ~400-body performers spiralled to 6 fps with the 10-substep cap).
    constexpr int kInteractivePhysicsSubsteps = 4;
    // Auto workflow (완전 자동): load queued CONTROLOBJECT controller models before the models' MME choices are
    // applied below, so a controller loaded this frame is visible to the skip re-evaluation the same frame.
    PollMmeControllers();
    const uint64_t slot = ctx_.FrameNumber();
    // Physics follows the motion clock. Backward jumps (seek, loop) and forward jumps beyond a
    // normal frame step reset the bodies to the animated pose instead of simulating the jump.
    // The benchmark always simulates (fixed workload).
    ModelInstance& ch = *scene_->character;
    // Main-thread decisions for the parallel posing below: workers may only touch their own instance.
    const bool benchMode = screen_ == Screen::BenchRun || screen_ == Screen::BenchRender;
    const bool physicsOn = screen_ == Screen::BenchRun || (settings_.physics && !options_.noPhysics);
    // Interactive play caps Bullet's substeps (see kInteractivePhysicsSubsteps); offline renders and benchmarks
    // keep the full cap so the simulated time stays exact.
    const int physicsMaxSubsteps =
        (offline_.mode == OfflineMode::None && !benchMode) ? kInteractivePhysicsSubsteps : 10;
    // A slow frame is not a jump: when the motion advanced about as much as the wall clock, keep stepping (Bullet
    // caps the work at the substep limit). Resetting there settles every body for a second, which made a six-performer
    // frame slower than 0.25 s, so every following frame reset again (stuck at 2-5 fps until a pause).
    const auto now = std::chrono::steady_clock::now();
    const float wallDt = std::chrono::duration<float>(now - scene_->physicsClock).count();
    scene_->physicsClock = now;
    float physicsDt = 0.0f;
    if (scene_->physicsFrame >= 0.0f) physicsDt = (frame - scene_->physicsFrame) / scene_->motionFps;
    const bool resetPhysics = scene_->physicsFrame < 0.0f || physicsDt < 0.0f ||
                              (physicsDt > 0.25f && physicsDt > wallDt * 1.5f + 0.1f);
    if (resetPhysics) physicsDt = 0.0f;
    scene_->physicsFrame = frame;
    // The saved per-character display scale (never in the benchmarks: they measure a fixed scene).
    const float characterScale = benchMode || scene_->characterId.empty()
                                     ? 1.0f
                                     : settings_.CharacterScale(scene_->characterId);
    // Pose the center character and the extras in parallel: only the CPU posing runs off the main thread, the GPU
    // uploads below stay sequential. The center runs on this thread, each extra on the Windows thread pool.
    std::vector<std::future<void>> pendingPoses;
    if (screen_ != Screen::BenchRender) {
        for (auto& extra : scene_->extras) {
            if (!extra.instance || !extra.gpu) continue;
            ModelInstance* inst = extra.instance.get();
            const BoundMotion* motion = extra.motion.get();
            const float scale = screen_ == Screen::BenchRun || extra.characterId.empty()
                                    ? 1.0f
                                    : settings_.CharacterScale(extra.characterId);
            pendingPoses.push_back(std::async(std::launch::async, [=]() {
                if (motion) motion->Evaluate(frame, *inst);
                else inst->ResetPose();
                inst->SetScale(scale);
                inst->EnablePhysics(physicsOn);
                inst->SetPhysicsMaxSubsteps(physicsMaxSubsteps);
                if (resetPhysics) inst->ResetPhysics();
                inst->UpdatePose(physicsDt);
            }));
        }
    }
    if (scene_->motion) scene_->motion->Evaluate(frame, ch);
    else ch.ResetPose();
    ch.SetScale(characterScale);
    ch.EnablePhysics(physicsOn);
    ch.SetPhysicsMaxSubsteps(physicsMaxSubsteps);
    if (resetPhysics) ch.ResetPhysics();
    ch.UpdatePose(physicsDt);
    for (std::future<void>& f : pendingPoses) f.get();
    scene_->characterGpu->UpdateSkinning(slot, scene_->character->SkinMatrices());
    scene_->characterGpu->UpdateMorphs(slot, scene_->character->VertexMorphDeltas(),
                                       scene_->character->MorphVersion());
    ApplyShaderChoice(*scene_->characterGpu, PlayShaderChoice(*scene_->characterGpu, scene_->characterId));
    // M6a: the character's MME object effect: --mme-model (File) wins for this run, else the resolved choice.
    {
        const MmeChoice mme = !MmeGlobalEnabled() ? MmeChoice{}
                              : options_.mmeModelSet ? MmeChoice{MmeChoice::File, options_.mmeModel}
                                                     : ResolveMmeChoice(scene_->characterId, /*isStage=*/false,
                                                                        scene_->character->Model().sourcePath);
        ApplyMmeEffect(*scene_->characterGpu, mme, scene_->character->Model().sourcePath,
                       MmeAutoSkipApplies(mme, /*isStage=*/false));
    }
    scene_->characterGpu->UpdateMaterials(slot, ch.MaterialMul(), ch.MaterialAdd(), ch.MaterialVersion());
    // P5: suggest Ray-MMD material data once per character that has a Ray .emd candidate and no saved choice.
    if (scene_->character)
        SuggestRayMaterial(scene_->characterId, scene_->character->Model().sourcePath,
                           (int)scene_->character->Model().materials.size());
    for (auto& extra : scene_->extras) {   // P5: the same suggestion per extra performer
        if (extra.instance)
            SuggestRayMaterial(extra.characterId, extra.instance->Model().sourcePath,
                               (int)extra.instance->Model().materials.size());
    }
    // S1b: the MMDX stage's material targets at the song time, BEFORE UpdateMaterials so its rebuild (a
    // receiveShadow override) lands in this frame's constants ring.
    if (scene_->stage && scene_->stage->Valid() && screen_ != Screen::BenchRender)
        scene_->stage->Apply(scene_->motionFps > 0.0f ? (double)frame / scene_->motionFps : playTime_,
                             ctx_.FrameNumber());
    for (size_t i = 0; i < scene_->stages.size(); ++i) {
        ModelInstance& st = *scene_->stages[i];
        // M8a: the bundled stage's motion animates on the song clock (the normal pose path, no physics).
        for (const StageMotionBinding& b : scene_->stageMotions) {
            if (b.part != (int)i || !b.motion) continue;
            b.motion->Evaluate(frame, st);
            st.UpdatePose(0.0f);
            break;
        }
        scene_->stageGpu[i]->UpdateSkinning(slot, st.SkinMatrices());
        scene_->stageGpu[i]->UpdateMorphs(slot, st.VertexMorphDeltas(), st.MorphVersion());
        // M6a: the stage MME choice (default Auto: a stage package ships its .emd for exactly this).
        const MmeChoice stageMme = ResolveMmeChoice(scene_->stageId, /*isStage=*/true, st.Model().sourcePath);
        ApplyMmeEffect(*scene_->stageGpu[i], stageMme, st.Model().sourcePath,
                       MmeAutoSkipApplies(stageMme, /*isStage=*/true));
        scene_->stageGpu[i]->UpdateMaterials(slot, st.MaterialMul(), st.MaterialAdd(), st.MaterialVersion());
    }
    // M5c: MME effect objects. An accessory `.x` is drawn in MODEL space in MMD (unlike a CPU-skinned PMX model):
    // the effect's WORLD / `CONTROLOBJECT (self)` = the controller panel places it. Keep the root identity so the
    // skinned vertices stay model space, and hand the binder the panel as `self` (DrawMmeModel does).
    for (SceneRuntime::EffectObject& obj : scene_->effectObjects) {
        if (!obj.mesh || !obj.inst) continue;
        obj.inst->SetRootTransform(DirectX::XMFLOAT4X4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
        obj.inst->UpdatePose(0.0f);
        obj.mesh->UpdateSkinning(slot, obj.inst->SkinMatrices());
        obj.mesh->UpdateMorphs(slot, obj.inst->VertexMorphDeltas(), obj.inst->MorphVersion());
        obj.mesh->UpdateMaterials(slot, obj.inst->MaterialMul(), obj.inst->MaterialAdd(), obj.inst->MaterialVersion());
    }
    if (screen_ != Screen::BenchRender) {
        for (auto& extra : scene_->extras) {            if (!extra.instance || !extra.gpu) continue;
            extra.gpu->UpdateSkinning(slot, extra.instance->SkinMatrices());
            extra.gpu->UpdateMorphs(slot, extra.instance->VertexMorphDeltas(),
                                    extra.instance->MorphVersion());
            ShaderChoice choice;
            if (screen_ == Screen::BenchRun) {
                choice = {};
            } else if (options_.shaderPackSet) {
                choice = PlayShaderChoice(*extra.gpu, extra.characterId);
            } else if (!extra.characterId.empty()) {
                // render benchmark extras keep their saved choice (deterministic); play mode also suggests fitting packs
                choice = screen_ == Screen::BenchRender ? settings_.CharacterShader(extra.characterId)
                                                        : PlayShaderChoice(*extra.gpu, extra.characterId);
            }
            ApplyShaderChoice(*extra.gpu, choice);
            // M6a: the extra performer's MME choice (--mme-model-all makes them all use --mme-model).
            {
                const MmeChoice mme = !MmeGlobalEnabled() ? MmeChoice{}
                                      : (options_.mmeModelSet && options_.mmeModelAll)
                                          ? MmeChoice{MmeChoice::File, options_.mmeModel}
                                          : ResolveMmeChoice(extra.characterId, /*isStage=*/false,
                                                             extra.instance->Model().sourcePath);
                ApplyMmeEffect(*extra.gpu, mme, extra.instance->Model().sourcePath,
                               MmeAutoSkipApplies(mme, /*isStage=*/false));
            }
            extra.gpu->UpdateMaterials(slot, extra.instance->MaterialMul(), extra.instance->MaterialAdd(),
                                       extra.instance->MaterialVersion());
        }
    }
}

void App::AppendMmeObject(FrameView& view, const GpuModel* gpu, const ModelInstance* inst, bool visible,
                          const std::string& displayName) {
    MmeSceneObject obj;
    obj.gpu = gpu;
    obj.visible = visible;
    obj.displayName = displayName;
    obj.rootWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (inst) {
        const PmxModel& pmx = inst->Model();
        // The model's source file name (MME DefaultEffect / CONTROLOBJECT match by file name, no folder).
        if (!pmx.sourcePath.empty()) obj.fileName = PathToUtf8(pmx.sourcePath.filename());
        obj.rootWorld = inst->RootTransform();
        obj.boneNames.reserve(pmx.bones.size());
        obj.boneWorld.reserve(pmx.bones.size());
        for (size_t b = 0; b < pmx.bones.size(); ++b) {
            obj.boneNames.push_back(pmx.bones[b].name);
            obj.boneWorld.push_back(inst->BoneWorld((int)b));
        }
        obj.morphNames.reserve(pmx.morphs.size());
        obj.morphWeights.reserve(pmx.morphs.size());
        for (size_t mi = 0; mi < pmx.morphs.size(); ++mi) {
            obj.morphNames.push_back(pmx.morphs[mi].name);
            obj.morphWeights.push_back(inst->MorphWeight((int)mi));
        }
    }
    view.mmeObjects.push_back(std::move(obj));
}

void App::AppendMmeEffectObjects(FrameView& view, const std::vector<SceneRuntime::EffectObject>& objects) {
    for (const SceneRuntime::EffectObject& o : objects) {
        if (o.fx.empty()) continue;
        // The post/scene part runs from FrameView::mmeEffectObjects (its panel is `(self)`); the effect is also
        // CONTROLOBJECT-addressable by its `.x` file name (or, without a mesh, its `.fx` name).
        MmeEffectObject eo;
        eo.fx = o.fx;
        eo.meshFile = o.meshFile;
        eo.mesh = o.mesh.get();
        eo.fileName = o.fileName;
        eo.displayName = o.displayName;
        eo.panel = o.panel;
        if (o.mesh) {
            o.mesh->SetMmeEffectMesh(true);
            view.models.push_back(o.mesh.get());
            AppendMmeObject(view, o.mesh.get(), o.inst.get(), true,
                            !o.displayName.empty() ? o.displayName : o.mesh->Name());
            // Mark the scene object as an accessory (its vertices are model space; the controller panel is its world).
            if (!view.mmeObjects.empty()) {
                MmeSceneObject& so = view.mmeObjects.back();
                so.accessory = true;
                so.panel = o.panel;
            }
        }
        view.mmeEffectObjects.push_back(std::move(eo));
    }
}

// World entities (docs/effect_ecosystem.md §2 item 7): the cast's collider sources for the play-mode track.
std::vector<CharacterSource> App::PlayColliderCast() const {
    std::vector<CharacterSource> cast;
    if (!scene_) return cast;
    if (scene_->character) {
        CharacterSource src;
        src.pmx = scene_->character->ModelPtr();
        src.motion = scene_->motion;
        src.fps = scene_->motionFps > 0.0f ? scene_->motionFps : kMmdFps;
        src.id = scene_->characterId;
        cast.push_back(std::move(src));
    }
    for (size_t i = 0; i < scene_->extras.size(); ++i) {
        const SceneRuntime::Performer& p = scene_->extras[i];
        if (!p.instance) continue;
        CharacterSource src;
        src.pmx = p.instance->ModelPtr();
        src.motion = p.motion;
        src.fps = scene_->motionFps > 0.0f ? scene_->motionFps : kMmdFps;
        src.id = p.characterId;
        cast.push_back(std::move(src));
    }
    return cast;
}

// Starts a play-mode collider build (whole song, 30 Hz) when the world stack is not empty and none is running.
void App::PumpColliderTrack() {
    if (studio_) return;   // the studio's track is rebuilt by StudioPollJob path (StudioColliderCast)
    if (!scene_) return;
    if (scene_->colliderFuture.valid() &&
        scene_->colliderFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        scene_->colliders = scene_->colliderFuture.get();
        if (scene_->colliders)
            LOG_INFO("WORLD colliders ready: %u per sample, %u samples at %.0f Hz", scene_->colliders->perSample,
                     scene_->colliders->sampleCount, scene_->colliders->rate);
    }
    if (settings_.worldStack.empty()) {
        scene_->colliders.reset();
        return;
    }
    if (scene_->colliderFuture.valid() || scene_->colliders) return;   // one build per scene (play mode never edits motions)
    const double end = scene_->motionFps > 0.0f ? (double)scene_->endFrame / scene_->motionFps : 0.0;
    const std::vector<CharacterSource> cast = PlayColliderCast();
    scene_->colliderFuture = std::async(std::launch::async, [cast, end] {
        return BuildColliderTrack(cast, 0.0, end, 30.0f);
    });
}

void App::BuildFrameView(float frame, FrameView& view) {
    view.frame = frame;
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
    for (size_t i = 0; i < scene_->stageGpu.size(); ++i) {
        // M8a: a stage part with a stage motion honours its model visibility keys (stepped).
        const BoundMotion* sm = nullptr;
        for (const StageMotionBinding& b : scene_->stageMotions)
            if (b.part == (int)i) sm = b.motion.get();
        if (sm && sm->HasVisibilityKeys() && !sm->Visible(frame)) continue;
        view.models.push_back(scene_->stageGpu[i].get());
        AppendMmeObject(view, scene_->stageGpu[i].get(), nullptr, true, scene_->stageGpu[i]->Name());  // static
    }
    if (scene_->characterGpu) {
        const BoundMotion* cm = scene_->motion.get();
        const bool vis = !(cm && cm->HasVisibilityKeys() && !cm->Visible(frame));
        if (vis) {
            view.models.push_back(scene_->characterGpu.get());
            AppendMmeObject(view, scene_->characterGpu.get(), scene_->character.get(), true,
                            scene_->characterDisplayName);
        }
    }
    for (const auto& extra : scene_->extras) {
        if (extra.gpu) {
            const BoundMotion* em = extra.motion.get();
            if (em && em->HasVisibilityKeys() && !em->Visible(frame)) continue;
            view.models.push_back(extra.gpu.get());
            AppendMmeObject(view, extra.gpu.get(), extra.instance.get(), true,
                            !extra.displayName.empty() ? extra.displayName : extra.gpu->Name());
        }
    }
    view.studioFloor = scene_->stageGpu.empty();
    // M5c: the MME effect objects' accessory meshes (drawn through the MME object path; appended after the
    // characters so particle / floor accessories sit over the scene as in MMD).
    AppendMmeEffectObjects(view, scene_->effectObjects);
    // Auto workflow (완전 자동): the auto-loaded CONTROLOBJECT controllers are MME-visible (mmeObjects,
    // visible = false) but never drawn: not pushed into view.models (not in BLAS, not in shadows).
    for (const SceneRuntime::Controller& c : scene_->controllers) {
        if (!c.gpu) continue;
        AppendMmeObject(view, c.gpu.get(), c.inst.get(), /*visible=*/false, c.displayName);
    }

    // Spotlights follow the performer (center bone when present).
    DirectX::XMFLOAT3 focus{0, 10, 0};
    if (scene_->character) {
        const int center = scene_->character->Model().FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
        if (center >= 0) focus = scene_->character->BoneWorldPosition(center);
    }
    const LightingPreset preset = screen_ == Screen::BenchRun ? LightingPreset::Studio : PlayLighting();
    // S1b: an MMDX stage is evaluated once per frame here (song time): the material targets go to the parts' GPU
    // records (SetPbrOverrides), the light targets into the rig BuildSceneLighting resolves below.
    const double stageSeconds = scene_->motionFps > 0.0f ? (double)frame / scene_->motionFps : playTime_;
    const std::vector<studio::SceneLight>* stageRig = nullptr;
    if (scene_->stage && scene_->stage->Valid() && screen_ != Screen::BenchRun)
        stageRig = &scene_->stage->Apply(stageSeconds, ctx_.FrameNumber());
    const bool stageLights = stageRig && !stageRig->empty();    if (stageLights && preset == LightingPreset::Stage) {
        // the stage's own rig (cue / binding targets on its lights included)
        LightAnchors stageAnchors;
        stageAnchors.focus = focus;
        stageAnchors.head = focus;
        const auto boneOf = [](const ModelInstance& inst, const char* name) {
            const int b = inst.Model().FindBone(name);
            return b >= 0 ? inst.BoneWorldPosition(b) : DirectX::XMFLOAT3{0, 10, 0};
        };
        if (scene_->character) {
            stageAnchors.focus = boneOf(*scene_->character, "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
            stageAnchors.head = boneOf(*scene_->character, "\xE9\xA0\xAD");
        }
        for (size_t i = 0; i < scene_->extras.size(); ++i) {
            const SceneRuntime::Performer& p = scene_->extras[i];
            if (!p.instance) continue;
            LightAnchors::Character c;
            c.uid = (uint32_t)(2 + i);
            c.centre = boneOf(*p.instance, "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
            c.head = boneOf(*p.instance, "\xE9\xA0\xAD");
            stageAnchors.characters.push_back(c);
        }
        BuildSceneLighting(*stageRig, scene_->lightTrack, stageSeconds, stageAnchors, view.light);
        ApplyHazeGains(view.light, settings_.volumetricLightGain, settings_.volumetricAmbientGain);
        // S9: a stage's environment.fog drives the haze's tint, density and height falloff while the stage is loaded; the
        // user's volumetric switch still decides whether the pass runs at all (the anisotropy is a render setting, set
        // when the scene loads).
        if (const StageFog& fog = scene_->stage->Manifest()->environment.fog; fog.present) {
            view.light.volumetricSigma0 = std::max(fog.density, 0.0f);
            view.light.volumetricColor = fog.color;
            view.light.volumetricFalloff = std::max(fog.heightFalloff, 0.0f);
            view.light.volumetricAnisotropy = std::clamp(fog.anisotropy, -0.9f, 0.95f);
            view.light.volumetricRadius = fog.radius;
        }
    } else {
        // "스테이지 조명" without a stage rig (or a classic stage): the Studio preset, as before.
        BuildLighting(preset == LightingPreset::Stage ? LightingPreset::Studio : preset, playTime_, focus, view.light);
        ApplyHazeGains(view.light, settings_.volumetricLightGain, settings_.volumetricAmbientGain);
    }
    if (settings_.volumetricRange >= 50.0f) view.light.volumetricRadius = settings_.volumetricRange;   // "안개 범위"
    // --classic: vanilla-MMD shading (plain toon key light): a hard-edged sun shadow, no rim / specular rim light
    if (classicLighting_) {
        view.light.sunShadow = LightShadowType::Hard;
        view.light.rimStrength = 0.0f;
    }
    // the camera VMD's light / self-shadow tracks (MMD's own lighting of the song) replace the preset's key light
    if (settings_.motionLighting && screen_ != Screen::BenchRun)
        ApplyLightShadowTracks(scene_->lightTrack, scene_->shadowTrack, frame, view);

    // DoF focus plane: the studio's automatic focus over every performer (the one the shot is about: on-screen size,
    // visible share, head near the frame centre; hysteresis and a short rack between subjects), else the center
    // performer's head. The render benchmark keeps the fixed head focus (deterministic workload).
    if (scene_->character) {
        const auto bonePos = [](const ModelInstance& m, std::initializer_list<const char*> names, DirectX::XMFLOAT3 fallback) {
            for (const char* n : names) {
                const int b = m.Model().FindBone(n);
                if (b >= 0) return m.BoneWorldPosition(b);
            }
            return fallback;
        };
        static const char* const kHead = "\xE9\xA0\xAD";                                    // 頭
        static const char* const kUpper2 = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB\x32";  // 上半身2
        static const char* const kUpper = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";       // 上半身
        static const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
        const auto subjectOf = [&](const ModelInstance& m, uint32_t uid) {
            const DirectX::XMFLOAT3 c = bonePos(m, {kCenterBone}, DirectX::XMFLOAT3{0, 10, 0});
            studio::FocusSubject fs;
            fs.uid = uid;
            fs.center = c;
            fs.upperBody = bonePos(m, {kUpper2, kUpper}, DirectX::XMFLOAT3{c.x, c.y + 4.0f, c.z});
            fs.head = bonePos(m, {kHead}, DirectX::XMFLOAT3{c.x, c.y + 8.0f, c.z});
            return fs;
        };
        const DirectX::XMMATRIX viewMat = DirectX::XMLoadFloat4x4(&view.camera.view);
        const auto viewZ = [&](const DirectX::XMFLOAT3& pos) {
            return DirectX::XMVectorGetZ(DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&pos), viewMat));
        };
        float z = 0.0f;
        if (screen_ != Screen::BenchRun && screen_ != Screen::BenchRender) {
            std::vector<studio::FocusSubject> subjects;
            subjects.push_back(subjectOf(*scene_->character, 1));
            for (size_t i = 0; i < scene_->extras.size(); ++i)
                if (scene_->extras[i].instance) subjects.push_back(subjectOf(*scene_->extras[i].instance, (uint32_t)(2 + i)));
            studio::FocusView fv;
            fv.view = view.camera.view;
            fv.eye = view.camera.eye;
            fv.tanHalfFovY = std::tan(view.camera.fovYRadians * 0.5f);
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            fv.aspect = ds.y > 0.0f ? ds.x / ds.y : 16.0f / 9.0f;
            fv.nearZ = view.camera.nearZ;
            z = playAutoFocus_.Update(frame / scene_->motionFps, subjects, fv);
        }
        if (z <= view.camera.nearZ) z = viewZ(subjectOf(*scene_->character, 1).head);
        view.focusDistance = z > view.camera.nearZ ? z : 0.0f;
    }

    // Seeking (or a restart) breaks temporal history.
    const double step = std::fabs(playTime_ - lastRenderedTime_);
    view.cameraCut = lastRenderedTime_ < 0 || step > 0.25;
    // M8a: the MME scene clock = seconds since the song / motion start (paused = constant, seeks jump).
    view.sceneTime = (float)playTime_;
    view.sceneElapsed = lastRenderedTime_ < 0 ? 0.0f : (float)step;
    view.timelinePlaying = playing_;
    lastRenderedTime_ = playTime_;
    // M8b: the song's video layers, with the video time = the song time (the background pass loops it).
    if (scene_) {
        view.videoLayers = scene_->videos;
        for (VideoLayerDesc& v : view.videoLayers) v.time = playTime_;
        // S5: the MMDX stage's screens (image / video / camera feed sources). The camera names resolve against the
        // stage's own `cameras`; a screen's file is relative to the stage folder.
        if (scene_->stage && scene_->stage->Valid() && !scene_->stage->Screens().empty()) {
            const StageManifest& man = *scene_->stage->Manifest();
            std::vector<StageRuntimePart> parts;
            for (size_t i = 0; i < scene_->stageGpu.size(); ++i) {
                StageRuntimePart p;
                p.part = (int)i;
                p.gpu = scene_->stageGpu[i].get();
                parts.push_back(p);
            }
            const std::filesystem::path dir = man.dir;
            view.screens = BuildStageScreenViews(
                man, scene_->stage->Screens(), parts,
                [&](const std::string& file) {
                    if (file.empty()) return std::filesystem::path{};
                    std::filesystem::path p = Utf8ToPath(file);
                    return p.is_absolute() ? p : dir / p;
                },
                [&](const std::string& name, CameraParams& out) {
                    const StageCamera* c = nullptr;
                    for (const StageCamera& sc : man.cameras)
                        if (sc.name == name) c = &sc;
                    if (!c) return false;
                    out = StageCameraView(*c);
                    return true;
                });
        }
    }
    if (options_.quitAfterFrames > 0 || !options_.uiScript.empty() || options_.mcp == 1 || HeadlessRun()) {
        view.effectDt = 1.0f / 60.0f;
    }
    const double audioSec = (offline_.mode != OfflineMode::None && scene_ && scene_->motionFps > 0.0f)
                                ? (double)(frame / scene_->motionFps)
                                : playTime_;
    FillPackInputs(view, scene_ ? scene_->character.get() : nullptr, audioSec);

    // World entities: one instance per enabled stack entry (identity transform), on the song clock used for the frame.
    PumpColliderTrack();
    if (scene_) {
        const double worldTime = offline_.mode != OfflineMode::None && scene_->motionFps > 0.0f
                                     ? (double)frame / scene_->motionFps
                                     : playTime_;
        const bool worldExact = !playing_ || offline_.mode != OfflineMode::None;
        uint32_t uid = 0;
        for (const EffectStackEntry& e : settings_.worldStack) {
            ++uid;
            if (!e.enabled) continue;
            WorldInstance w;
            w.uid = uid;
            w.pack = e.pack;
            w.params = e.params;
            w.time = worldTime;
            w.exact = worldExact;
            w.colliders = scene_->colliders;
            view.worlds.push_back(std::move(w));
        }
        // S6: the stage's bundled world packs, appended after the user's stack, each at its socket (the socket's node
        // bone world matrix x its explicit offset; the parts are posed by now).
        if (scene_->stage && scene_->stage->Valid() && !scene_->stage->Manifest()->packs.empty()) {
            const StageManifest& man = *scene_->stage->Manifest();
            const auto resolveNode = [&](const std::string& node, DirectX::XMFLOAT4X4& out) {
                for (size_t i = 0; i < scene_->stages.size() && i < scene_->stageGpu.size(); ++i) {
                    const ModelInstance& inst = *scene_->stages[i];
                    const int bone = inst.Model().FindBone(node);
                    if (bone < 0) continue;
                    out = inst.BoneWorld(bone);
                    return true;
                }
                return false;
            };
            AppendStageWorldPacks(man, scene_->stage->Packs(), worldTime, worldExact, scene_->colliders,
                                  kStageWorldUidBase, resolveNode, view.worlds);
        }
    }
}

} // namespace mmdx
