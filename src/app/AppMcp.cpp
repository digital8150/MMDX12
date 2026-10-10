#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "asset/ImageLoader.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "render/ShaderPack.h"
#include "imgui.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"
#include "ui_probe/UiProbe.h"

namespace mmdx {

const char* App::ScreenName(Screen s) const {
    switch (s) {
    case Screen::Scanning: return "scanning";
    case Screen::Select: return "select";
    case Screen::Loading: return "loading";
    case Screen::Play: return "play";
    case Screen::Offline: return "offline";
    case Screen::BenchLobby: return "bench_lobby";
    case Screen::Shaders: return "shaders";
    case Screen::BenchRun: return "bench_run";
    case Screen::BenchResult: return "bench_result";
    case Screen::BenchRender: return "bench_render";
    case Screen::Studio: return "studio";
    }
    return "unknown";
}

void App::InitMcp() {
    // --mcp / --no-mcp decide; otherwise the setting, except in automated runs (tests, renders, benchmarks)
    const bool enable = options_.mcp >= 0 ? options_.mcp == 1 : settings_.mcpEnabled && !HeadlessRun();
    if (enable) StartMcpServer();
}

void App::StartMcpServer() {
    if (mcpServer_) return;
    mcpServer_ = std::make_unique<McpServer>();
    if (!mcpServer_->Start()) mcpServer_.reset();
    uiprobe::SetEnabled(mcpServer_ != nullptr);   // ui_items / ui_click read the widget registry
}

void App::ShutdownMcp() {
    if (mcpServer_) {
        mcpServer_->Stop();
        mcpServer_.reset();
    }
    uiprobe::SetEnabled(false);
    for (auto& w : pendingMcpWaits_) w.promise->Reject("MCP server stopped");
    for (auto& l : pendingMcpLoads_) l.promise->Reject("MCP server stopped");
    for (auto& i : pendingMcpInputs_) i.promise->Reject("MCP server stopped");
    pendingMcpWaits_.clear();
    pendingMcpLoads_.clear();
    pendingMcpInputs_.clear();
    mcpInput_.clear();
    mcpInputActive_ = false;
}

bool App::McpLeaveScreen(const nlohmann::json& args, McpPromise& promise) {
    if (screen_ == Screen::Loading && !loadError_.empty() && !loadFuture_.valid()) {
        screen_ = Screen::Select;  // a failed load waits on its error screen: "back"
        loadError_.clear();
    }
    if (offline_.mode != OfflineMode::None || screen_ == Screen::Scanning || screen_ == Screen::Loading ||
        screen_ == Screen::Offline || screen_ == Screen::BenchRun || screen_ == Screen::BenchRender) {
        promise.Reject(std::string("MMDX12 is busy (screen: ") + ScreenName(screen_) +
                       "); wait for it to finish or call render_cancel");
        return false;
    }
    if (screen_ == Screen::Studio && studio_) {
        if (studio_->Dirty() && !args.value("discard_unsaved", false)) {
            promise.Reject("The studio project has unsaved changes: call studio_save first, or pass discard_unsaved: true");
            return false;
        }
        LeaveStudio();  // waits for model loads and the GPU, like the studio's back button
    } else if (screen_ == Screen::Play) {
        UnloadScene();  // like the play bar's exit button
        screen_ = Screen::Select;
    }
    return true;
}

void App::McpScheduleInput(int frame, std::string cmd, std::vector<std::string> args) {
    auto it = mcpInput_.begin();
    while (it != mcpInput_.end() && it->frame <= frame) ++it;
    mcpInput_.insert(it, UiScriptStep{frame, std::move(cmd), std::move(args)});
}

const CharacterAsset* App::FindCharacter(const std::string& needle) const {
    if (needle.empty()) return nullptr;
    const std::string n = ToLowerAscii(needle);
    for (const auto& c : library_.characters) {
        if (ToLowerAscii(c.id).find(n) != std::string::npos ||
            ToLowerAscii(c.displayName).find(n) != std::string::npos)
            return &c;
    }
    return nullptr;
}

const StageAsset* App::FindStage(const std::string& needle) const {
    if (needle.empty() || ToLowerAscii(needle) == "none") return nullptr;
    const std::string n = ToLowerAscii(needle);
    for (const auto& s : library_.stages) {
        if (ToLowerAscii(s.id).find(n) != std::string::npos ||
            ToLowerAscii(s.displayName).find(n) != std::string::npos)
            return &s;
    }
    return nullptr;
}

const SongAsset* App::FindSong(const std::string& needle) const {
    if (needle.empty()) return nullptr;
    const std::string n = ToLowerAscii(needle);
    for (const auto& s : library_.songs) {
        if (ToLowerAscii(s.id).find(n) != std::string::npos ||
            ToLowerAscii(s.displayName).find(n) != std::string::npos)
            return &s;
    }
    return nullptr;
}

void App::PumpMcp(bool minimized) {
    if (!mcpServer_ || !mcpServer_->IsRunning()) return;

    // 1. Drain incoming requests from the server worker thread
    auto requests = mcpServer_->PopRequests();
    for (auto& req : requests) {
        static const char* const kQueries[] = {"get_state",       "get_recent_logs", "list_library",   "get_render_settings",
                                               "studio_get_state", "render_status",   "render_cancel", "quit_app"};
        if (minimized && std::none_of(std::begin(kQueries), std::end(kQueries), [&](const char* q) { return req.tool == q; })) {
            ShowWindow(hwnd_, SW_RESTORE);  // loads, renders and edits need rendered frames
            minimized_ = false;
        }
        try {
            ExecuteMcp(req.tool, req.args, req.promise);
        } catch (const std::exception& e) {  // a wrongly typed argument (nlohmann type_error) fails the call only
            req.promise->Reject(std::string("Invalid arguments: ") + e.what());
        }
    }

    // A render started by render_still / render_video has ended: its run overrides go back.
    if (mcpOptionsRestore_ && offline_.mode == OfflineMode::None) {
        const AppOptions& o = *mcpOptionsRestore_;
        options_.offlineSpp = o.offlineSpp;
        options_.offlineSize[0] = o.offlineSize[0];
        options_.offlineSize[1] = o.offlineSize[1];
        options_.offlineFps = o.offlineFps;
        options_.offlineBitrate = o.offlineBitrate;
        options_.offlineQuality = o.offlineQuality;
        options_.offlineRenderer = o.offlineRenderer;
        mcpOptionsRestore_.reset();
    }

    // 2. Multi-frame command: load_scene / open_studio / studio_open
    for (auto it = pendingMcpLoads_.begin(); it != pendingMcpLoads_.end();) {
        if (screen_ == Screen::Play || screen_ == Screen::Studio) {
            it->promise->Resolve({{"status", "loaded"}, {"screen", ScreenName(screen_)}});
            it = pendingMcpLoads_.erase(it);
        } else if (screen_ == Screen::Loading && !loadError_.empty()) {
            it->promise->Reject(loadError_);
            it = pendingMcpLoads_.erase(it);
        } else if (screen_ != Screen::Loading) {
            it->promise->Reject(std::string("Loading ended on screen ") + ScreenName(screen_));
            it = pendingMcpLoads_.erase(it);
        } else {
            ++it;
        }
    }
    if (minimized) return;  // the rest counts rendered frames

    ++mcpFrame_;

    // 3. Multi-frame command: wait_frames
    for (auto it = pendingMcpWaits_.begin(); it != pendingMcpWaits_.end();) {
        if (--it->waitFrames <= 0) {
            it->promise->Resolve({{"waited_frames", true}});
            it = pendingMcpWaits_.erase(it);
        } else {
            ++it;
        }
    }

    // 4. ui_input steps due this frame (the same commands as --ui-script; follow-up steps such as a click's
    // press / release are scheduled through McpScheduleInput)
    if (mcpInputActive_) {
        ImGuiIO& io = ImGui::GetIO();
        if (uiScriptMouse_[0] > -1e29f) io.AddMousePosEvent(uiScriptMouse_[0], uiScriptMouse_[1]);
        const auto sched = [this](int frame, std::string cmd, std::vector<std::string> args) {
            McpScheduleInput(mcpFrame_ + (frame - framesInScene_), std::move(cmd), std::move(args));
        };
        while (!mcpInput_.empty() && mcpInput_.front().frame <= mcpFrame_) {
            const UiScriptStep step = mcpInput_.front();
            mcpInput_.erase(mcpInput_.begin());
            ExecuteUiCommand(step.cmd, step.args, sched);
        }
    }
    for (auto it = pendingMcpInputs_.begin(); it != pendingMcpInputs_.end();) {
        if (mcpFrame_ >= it->targetFrame) {
            it->promise->Resolve(it->result.is_null() ? nlohmann::json{{"status", "completed"}} : it->result);
            it = pendingMcpInputs_.erase(it);
        } else {
            ++it;
        }
    }
    if (mcpInputActive_ && mcpInput_.empty() && pendingMcpInputs_.empty()) {
        mcpInputActive_ = false;  // real input again
        if (options_.uiScript.empty()) uiScriptMouse_[0] = uiScriptMouse_[1] = -1e30f;
    }
}

void App::ExecuteMcp(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise) {
    if (!promise) return;

    if (tool == "get_state") {
        nlohmann::json s;
        s["screen"] = ScreenName(screen_);
        if (screen_ == Screen::Loading && !loadError_.empty() && !loadFuture_.valid())
            s["load_error"] = loadError_;  // the error screen: any navigation tool leaves it
        s["playing"] = (screen_ == Screen::Play ? playing_ : (studio_ ? studio_->playing : false));
        s["play_time"] = (screen_ == Screen::Play ? playTime_ : (studio_ ? studio_->time : 0.0));
        s["frame"] = (screen_ == Screen::Play ? framesInScene_ : (studio_ ? studio_->Frame() : framesInScene_));
        s["duration"] = (scene_ ? (double)scene_->endFrame / 30.0 : (studio_ ? (double)studio_->EndFrame() / 30.0 : 0.0));
        // the lobby selection = what play mode shows (MCP loads keep it in sync)
        s["character"] = selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size()
                             ? library_.characters[(size_t)selCharacter_].displayName
                             : std::string(scene_ ? scene_->characterId : "");
        s["stage"] = (selStage_ >= 0 && (size_t)selStage_ < library_.stages.size() ? library_.stages[(size_t)selStage_].displayName : "none");
        s["song"] = (selSong_ >= 0 && (size_t)selSong_ < library_.songs.size() ? library_.songs[(size_t)selSong_].displayName : "");
        s["render_path"] = (settings_.renderPath == 1 ? "rt" : settings_.renderPath == 2 ? "pt" : "raster");
        s["upscaler"] = (settings_.upscaler == 1 ? "dlss" : settings_.upscaler == 2 ? "fsr" : settings_.upscaler == 3 ? "xess" : "none");
        s["upscale_quality"] = (settings_.upscalerQuality == 0 ? "native" : settings_.upscalerQuality == 1 ? "quality" : settings_.upscalerQuality == 2 ? "balanced" : settings_.upscalerQuality == 3 ? "performance" : "ultra");
        s["fps"] = ImGui::GetIO().Framerate;
        s["gpu_ms"] = renderer_.Stats().gpuFrameMs;
        s["window_width"] = ctx_.Width();
        s["window_height"] = ctx_.Height();
        if (studio_) {
            nlohmann::json st;
            st["project_path"] = PathToUtf8(studio_->projectPath);
            st["dirty"] = studio_->Dirty();
            st["models_count"] = studio_->models.size();
            st["lights_count"] = studio_->lights.size();
            st["selected_model"] = studio_->selectedModel;
            st["undo"] = studio_->history.UndoName();
            st["redo"] = studio_->history.RedoName();
            s["studio"] = st;
        }
        promise->Resolve(s);
        return;
    }

    if (tool == "get_recent_logs") {
        size_t lines = args.value("lines", 50);
        std::string level = ToLowerAscii(args.value("level", ""));
        std::vector<std::string> all = LogRecentLines(lines);
        std::vector<std::string> filtered;
        for (const auto& l : all) {
            if (level == "error" && !l.starts_with("[E]")) continue;
            if (level == "warn" && !l.starts_with("[W]") && !l.starts_with("[E]")) continue;
            filtered.push_back(l);
        }
        promise->Resolve({{"logs", filtered}});
        return;
    }

    if (tool == "list_library") {
        std::string kind = ToLowerAscii(args.value("kind", "all"));
        std::string query = ToLowerAscii(args.value("query", ""));
        if (kind != "all" && kind != "character" && kind != "stage" && kind != "song") {
            promise->Reject("Invalid kind: " + kind + " (all, character, stage, song)");
            return;
        }

        nlohmann::json res;
        if (kind == "all" || kind == "character") {
            nlohmann::json chList = nlohmann::json::array();
            for (const auto& c : library_.characters) {
                if (!query.empty() && ToLowerAscii(c.id).find(query) == std::string::npos &&
                    ToLowerAscii(c.displayName).find(query) == std::string::npos)
                    continue;
                chList.push_back({{"id", c.id}, {"name", c.displayName}, {"format", c.format}});
            }
            res["characters"] = chList;
        }
        if (kind == "all" || kind == "stage") {
            nlohmann::json stList = nlohmann::json::array();
            for (const auto& s : library_.stages) {
                if (!query.empty() && ToLowerAscii(s.id).find(query) == std::string::npos &&
                    ToLowerAscii(s.displayName).find(query) == std::string::npos)
                    continue;
                stList.push_back({{"id", s.id}, {"name", s.displayName}, {"format", s.format}});
            }
            res["stages"] = stList;
        }
        if (kind == "all" || kind == "song") {
            nlohmann::json soList = nlohmann::json::array();
            for (const auto& s : library_.songs) {
                if (!query.empty() && ToLowerAscii(s.id).find(query) == std::string::npos &&
                    ToLowerAscii(s.displayName).find(query) == std::string::npos)
                    continue;
                soList.push_back({{"id", s.id}, {"name", s.displayName}, {"duration", s.durationSec}});
            }
            res["songs"] = soList;
        }
        promise->Resolve(res);
        return;
    }

    if (tool == "screenshot") {
        const std::string path = args.value("path", "");
        const bool returnImage = path.empty() || args.value("return_image", false);
        // inline images default to 1280 wide; a saved file keeps the full size unless max_width says otherwise
        const uint32_t maxWidth = (uint32_t)std::max(0, args.value("max_width", path.empty() ? 1280 : 0));
        float region[4] = {0, 0, 0, 0};
        if (args.contains("region")) {
            if (!args["region"].is_array() || args["region"].size() != 4) {
                promise->Reject("region must be [x, y, width, height]");
                return;
            }
            for (int k = 0; k < 4; ++k) region[k] = args["region"][k].get<float>();
        }
        ctx_.RequestCaptureMemory([this, promise, maxWidth, path, returnImage, region](uint32_t w, uint32_t h,
                                                                                         std::vector<uint8_t> rgba) {
            if (region[2] > 0 && region[3] > 0) {   // crop (clamped to the image)
                const uint32_t x0 = (uint32_t)std::clamp(region[0], 0.0f, (float)w - 1);
                const uint32_t y0 = (uint32_t)std::clamp(region[1], 0.0f, (float)h - 1);
                const uint32_t cw = std::min((uint32_t)region[2], w - x0), ch = std::min((uint32_t)region[3], h - y0);
                std::vector<uint8_t> crop((size_t)cw * ch * 4);
                for (uint32_t y = 0; y < ch; ++y)
                    std::memcpy(&crop[(size_t)y * cw * 4], &rgba[((size_t)(y0 + y) * w + x0) * 4], (size_t)cw * 4);
                rgba.swap(crop);
                w = cw;
                h = ch;
            }
            uint32_t outW = w, outH = h;
            std::vector<uint8_t> scaled;
            const uint8_t* px = rgba.data();
            if (maxWidth > 0 && w > maxWidth) {
                scaled = DownscaleRgba8(w, h, rgba.data(), maxWidth, outW, outH);
                px = scaled.data();
            }
            std::vector<uint8_t> pngBytes;
            if (!EncodePngRGBA8(outW, outH, px, outW * 4, pngBytes)) {
                promise->Reject("Failed to encode PNG");
                return;
            }
            nlohmann::json res;
            if (!path.empty()) {
                const std::filesystem::path file = std::filesystem::absolute(Utf8ToPath(path));
                std::error_code ec;
                if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
                FILE* f = _wfopen(file.c_str(), L"wb");
                const bool ok = f && std::fwrite(pngBytes.data(), 1, pngBytes.size(), f) == pngBytes.size();
                if (f) std::fclose(f);
                if (!ok) {
                    promise->Reject("Could not write " + PathToUtf8(file));
                    return;
                }
                res["path"] = PathToUtf8(file);
            }
            if (returnImage) res["image_base64"] = Base64Encode(pngBytes.data(), pngBytes.size());
            res["width"] = outW;
            res["height"] = outH;
            res["screen"] = ScreenName(screen_);
            promise->Resolve(res);
        });
        return;
    }

    if (tool == "ui_items" || tool == "ui_click") {
        if (!uiprobe::Enabled()) {
            promise->Reject("The widget registry is off (the MCP server is not running)");
            return;
        }
        const bool click = tool == "ui_click";
        const std::string query = args.value(click ? "target" : "query", "");
        const std::string windowQ = ToLowerAscii(args.value("window", ""));
        const bool includeHidden = !click && args.value("include_hidden", false);
        if (click && query.empty()) {
            promise->Reject("ui_click needs a target");
            return;
        }
        // Korean source text also matches its translation in the current UI language
        const std::string q = ToLowerAscii(query), qTr = ToLowerAscii(Tr(query.c_str()));
        struct Hit {
            const uiprobe::Item* item;
            bool exact;
        };
        std::vector<Hit> hits;
        for (const uiprobe::Item& it : uiprobe::LastFrame()) {
            if (it.window.rfind("Debug##", 0) == 0) continue;   // imgui's implicit fallback window
            const bool covered = !it.isWindow && uiprobe::Covered(it);
            if (!includeHidden && (!it.visible || covered)) continue;
            if (click && it.isWindow) continue;
            if (it.max.x - it.min.x < 1.0f || it.max.y - it.min.y < 1.0f) continue;
            if (!windowQ.empty() && ToLowerAscii(it.window).find(windowQ) == std::string::npos) continue;
            const std::string label = ToLowerAscii(it.label), id = ToLowerAscii(it.idStr);
            bool exact = false, match = q.empty();
            for (const std::string* n : {&q, &qTr}) {
                if (n->empty()) continue;
                if (label == *n || id == *n || id == "##" + *n) exact = match = true;
                else if (label.find(*n) != std::string::npos || id.find(*n) != std::string::npos) match = true;
            }
            if (match) hits.push_back({&it, exact});
        }
        if (std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.exact; }))
            hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Hit& h) { return !h.exact; }), hits.end());
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if (std::abs(a.item->min.y - b.item->min.y) > 2.0f) return a.item->min.y < b.item->min.y;
            return a.item->min.x < b.item->min.x;
        });
        const auto toJson = [](const uiprobe::Item& it) {
            nlohmann::json j{{"label", it.label},
                             {"id", it.idStr},
                             {"window", it.window},
                             {"rect", {std::round(it.min.x), std::round(it.min.y), std::round(it.max.x - it.min.x),
                                       std::round(it.max.y - it.min.y)}}};
            if (it.disabled) j["disabled"] = true;
            if (!it.visible) j["hidden"] = true;
            else if (!it.isWindow && uiprobe::Covered(it)) j["covered"] = true;
            if (it.isWindow) j["kind"] = "window";
            return j;
        };
        if (!click) {
            const size_t limit = (size_t)std::max(1, args.value("limit", 200));
            nlohmann::json arr = nlohmann::json::array();
            for (const Hit& h : hits) {
                if (arr.size() >= limit) break;
                arr.push_back(toJson(*h.item));
            }
            promise->Resolve({{"items", arr}, {"total", hits.size()}, {"screen", ScreenName(screen_)}});
            return;
        }
        const int index = args.value("index", -1);
        if (hits.empty() || (hits.size() > 1 && index < 0) || index >= (int)hits.size()) {
            nlohmann::json cands = nlohmann::json::array();
            for (size_t k = 0; k < hits.size() && k < 12; ++k) cands.push_back(toJson(*hits[k].item));
            promise->Reject(hits.empty() ? "No visible widget matches '" + query + "' (list them with ui_items)"
                                         : std::to_string(hits.size()) + " widgets match '" + query +
                                               "': pass index (or window). Candidates: " + cands.dump());
            return;
        }
        const uiprobe::Item& it = *hits[hits.size() == 1 ? 0 : (size_t)index].item;
        float fx = 0.5f, fy = 0.5f;
        if (args.contains("offset") && args["offset"].is_array() && args["offset"].size() == 2) {
            fx = args["offset"][0].get<float>();
            fy = args["offset"][1].get<float>();
        }
        const std::string x = std::to_string(it.min.x + (it.max.x - it.min.x) * fx);
        const std::string y = std::to_string(it.min.y + (it.max.y - it.min.y) * fy);
        const std::string action = args.value("action", "click");
        const int f = mcpFrame_ + 1;
        int last = f + 1;
        if (action == "hover") {
            McpScheduleInput(f, "move", {x, y});
        } else if (action == "right") {
            McpScheduleInput(f, "move", {x, y});
            McpScheduleInput(f + 1, "down", {"r"});
            McpScheduleInput(f + 2, "up", {"r"});
            last = f + 3;
        } else if (action == "click" || action == "dblclick") {
            McpScheduleInput(f, action, {x, y});   // schedules its own press / release
            last = f + (action == "dblclick" ? 5 : 3);
        } else {
            promise->Reject("Unknown action: " + action + " (click, dblclick, right, hover)");
            return;
        }
        mcpInputActive_ = true;
        nlohmann::json res = toJson(it);
        res["action"] = action;
        pendingMcpInputs_.push_back({last, promise, res});
        return;
    }

    if (tool == "wait_frames") {
        int n = args.value("n", 1);
        if (n <= 0) {
            promise->Resolve({{"waited_frames", 0}});
            return;
        }
        pendingMcpWaits_.push_back({n, promise});
        return;
    }

    if (tool == "set_screen") {
        const std::string target = args.value("screen", "");
        if (target != "select" && target != "studio" && target != "shaders" && target != "bench") {
            promise->Reject("Unknown screen: " + target);
            return;
        }
        if (target == "studio" && screen_ == Screen::Studio) {
            promise->Resolve({{"screen", "studio"}});
            return;
        }
        if (!McpLeaveScreen(args, *promise)) return;
        if (target == "studio") StartStudioEmpty();  // a new empty project
        else screen_ = target == "select" ? Screen::Select : target == "shaders" ? Screen::Shaders : Screen::BenchLobby;
        promise->Resolve({{"screen", ScreenName(screen_)}});
        return;
    }

    if (tool == "load_scene" || tool == "open_studio") {
        const bool studio = tool == "open_studio";
        if (studio && args.contains("project")) {
            const std::filesystem::path file = std::filesystem::absolute(Utf8ToPath(args["project"].get<std::string>()));
            if (!std::filesystem::exists(file)) {
                promise->Reject("Project file not found: " + PathToUtf8(file));
                return;
            }
            if (!McpLeaveScreen(args, *promise)) return;
            StartStudioProjectLoad(file, false);
            pendingMcpLoads_.push_back({std::chrono::steady_clock::now(), promise});
            return;
        }
        const std::string chNeedle = args.value("character", "");
        const std::string stNeedle = args.value("stage", "");
        const std::string soNeedle = args.value("song", "");
        if (studio && chNeedle.empty()) {  // no character: a new empty project
            if (!McpLeaveScreen(args, *promise)) return;
            StartStudioEmpty();
            promise->Resolve({{"status", "loaded"}, {"screen", ScreenName(screen_)}});
            return;
        }
        const CharacterAsset* ch = FindCharacter(chNeedle);
        if (!ch) {
            promise->Reject("Character not found: " + chNeedle + " (see list_library)");
            return;
        }
        const StageAsset* st = FindStage(stNeedle);
        if (!stNeedle.empty() && ToLowerAscii(stNeedle) != "none" && !st) {
            promise->Reject("Stage not found: " + stNeedle);
            return;
        }
        const SongAsset* so = soNeedle.empty() ? nullptr : FindSong(soNeedle);
        if (!soNeedle.empty() && !so) {
            promise->Reject("Song not found: " + soNeedle);
            return;
        }
        if (!studio && !so) {  // play mode needs a song (the motion)
            if (library_.songs.empty()) {
                promise->Reject("The library has no songs");
                return;
            }
            so = &library_.songs[0];
        }
        // the asset pointers stay valid: leaving a screen does not rescan the library
        if (!McpLeaveScreen(args, *promise)) return;
        // the lobby selection follows (the play overlay's title, get_state and the lobby after leaving read it)
        selCharacter_ = (int)(ch - library_.characters.data());
        selStage_ = st ? (int)(st - library_.stages.data()) : -1;
        selSong_ = so ? (int)(so - library_.songs.data()) : -1;
        if (studio) StartStudioLoad(ch, st, so);
        else StartLoad(LoadTarget::Play, ch, st, so);
        pendingMcpLoads_.push_back({std::chrono::steady_clock::now(), promise});
        return;
    }

    if (tool == "play" || tool == "pause") {
        const bool play = tool == "play";
        if (screen_ == Screen::Play && scene_) SetPlaying(play);
        else if (screen_ == Screen::Studio && studio_) StudioSetPlaying(play);
        else {
            promise->Reject(tool + " needs the play or studio screen");
            return;
        }
        promise->Resolve({{"playing", screen_ == Screen::Play ? playing_ : studio_->playing}});
        return;
    }

    if (tool == "seek") {
        double sec = args.value("seconds", 0.0);
        if (screen_ == Screen::Play && scene_) {
            playTime_ = std::clamp(sec, 0.0, (double)(scene_->endFrame / kMmdFps));  // like the play bar
            if (scene_->hasAudio) audio_.Seek(playTime_);
            sec = playTime_;
        } else if (screen_ == Screen::Studio && studio_) {
            StudioSeek(sec);
            sec = studio_->time;
        } else {
            promise->Reject("seek needs the play or studio screen");
            return;
        }
        promise->Resolve({{"seconds", sec}});
        return;
    }

    if (tool == "get_render_settings") {
        nlohmann::json r;
        r["render_path"] = (settings_.renderPath == 1 ? "rt" : settings_.renderPath == 2 ? "pt" : "raster");
        r["upscaler"] = (settings_.upscaler == 1 ? "dlss" : settings_.upscaler == 2 ? "fsr" : settings_.upscaler == 3 ? "xess" : "none");
        r["upscale_quality"] = (settings_.upscalerQuality == 0 ? "native" : settings_.upscalerQuality == 1 ? "quality" : settings_.upscalerQuality == 2 ? "balanced" : settings_.upscalerQuality == 3 ? "performance" : "ultra");
        r["lighting_preset"] = settings_.lighting;
        r["graphics_preset"] = settings_.graphicsPreset;
        r["shadows"] = settings_.shadows;
        r["ssao"] = settings_.ssao;
        r["ssr"] = settings_.ssr;
        r["bloom"] = settings_.bloom;
        r["taa"] = settings_.taa;
        r["draw_edges"] = settings_.drawEdges;
        r["dof"] = settings_.dof;
        r["volumetric"] = settings_.volumetric;
        r["bloom_convolution"] = settings_.bloomConvolution;
        nlohmann::json fx = nlohmann::json::array();
        for (const EffectStackEntry& e : settings_.effectStack) {
            nlohmann::json params = nlohmann::json::object();
            for (const auto& [k, v] : e.params) params[k] = v;
            nlohmann::json entry = {{"id", e.pack}, {"enabled", e.enabled}, {"params", params}};
            if (!e.textureFolder.empty()) entry["texture_folder"] = e.textureFolder;
            fx.push_back(entry);
        }
        r["effects"] = fx;
        if (screen_ == Screen::Play && scene_) r["shader_pack"] = settings_.CharacterShader(scene_->characterId).pack;
        promise->Resolve(r);
        return;
    }

    if (tool == "set_render_settings") {
        // validate everything first: a bad value fails the call and changes nothing
        const auto pick = [&](const char* key, std::initializer_list<const char*> names, int& out) -> bool {
            if (!args.contains(key)) return true;
            const std::string v = args[key].get<std::string>();
            int i = 0;
            for (const char* n : names) {
                if (v == n) { out = i; return true; }
                ++i;
            }
            promise->Reject(std::string("Invalid ") + key + ": " + v);
            return false;
        };
        int renderPath = settings_.renderPath, upscaler = settings_.upscaler, upscaleQuality = settings_.upscalerQuality;
        if (!pick("render_path", {"raster", "rt", "pt"}, renderPath) ||
            !pick("upscaler", {"none", "dlss", "fsr", "xess"}, upscaler) ||
            !pick("upscale_quality", {"native", "quality", "balanced", "performance", "ultra"}, upscaleQuality))
            return;
        const auto intIn = [&](const char* key, int lo, int hi) -> bool {
            if (!args.contains(key)) return true;
            const int v = args[key].get<int>();
            if (v >= lo && v <= hi) return true;
            promise->Reject(std::string("Invalid ") + key + ": " + std::to_string(v) + " (" + std::to_string(lo) + ".." +
                            std::to_string(hi) + ")");
            return false;
        };
        if (!intIn("lighting_preset", 0, kLightingPresetCount - 1) || !intIn("quality", 0, 3)) return;
        const ShaderPackRegistry& reg = ShaderPacks();
        std::string shaderPack;
        if (args.contains("shader_pack")) {
            if (!(screen_ == Screen::Play && scene_)) {
                promise->Reject("shader_pack applies to the character in play mode (load_scene first)");
                return;
            }
            shaderPack = args["shader_pack"].get<std::string>();
            if (shaderPack == "default" || shaderPack == "none") shaderPack.clear();
            const ShaderPack* sp = shaderPack.empty() ? nullptr : reg.Find(shaderPack);
            if (!shaderPack.empty() && (!sp || sp->type != PackType::Surface)) {
                promise->Reject("Unknown surface shader pack: " + shaderPack);
                return;
            }
        }
        std::vector<EffectStackEntry> effects;
        if (args.contains("effects")) {  // the whole stack, in order ("none" / "" clears it)
            const std::string list = args["effects"].get<std::string>();
            if (list != "none") {
                size_t pos = 0;
                while (pos <= list.size()) {
                    const size_t comma = std::min(list.find(',', pos), list.size());
                    std::string id = list.substr(pos, comma - pos);
                    id.erase(0, id.find_first_not_of(' '));
                    id.erase(id.find_last_not_of(' ') + 1);
                    pos = comma + 1;
                    if (id.empty()) continue;
                    const ShaderPack* ep = reg.Find(id);
                    if (!ep || ep->type != PackType::Effect) {
                        promise->Reject("Unknown effect pack: " + id);
                        return;
                    }
                    // an effect already in the stack keeps its parameters and texture folder
                    EffectStackEntry entry{id, true, {}, {}};
                    for (const EffectStackEntry& cur : settings_.effectStack)
                        if (cur.pack == id) {
                            entry.params = cur.params;
                            entry.textureFolder = cur.textureFolder;
                            break;
                        }
                    effects.push_back(std::move(entry));
                }
            }
        }
        if (args.contains("effect_stack")) {  // the whole stack with parameters: [{id, enabled, params, texture_folder}]
            if (args.contains("effects")) {
                promise->Reject("Pass either effects or effect_stack, not both");
                return;
            }
            const nlohmann::json& list = args["effect_stack"];
            if (!list.is_array()) {
                promise->Reject("effect_stack must be an array of {id, enabled, params, texture_folder}");
                return;
            }
            for (const nlohmann::json& item : list) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
                    promise->Reject("effect_stack entries need a string id");
                    return;
                }
                const std::string id = item["id"].get<std::string>();
                const ShaderPack* ep = reg.Find(id);
                if (!ep || ep->type != PackType::Effect) {
                    promise->Reject("Unknown effect pack: " + id);
                    return;
                }
                EffectStackEntry entry{id, true, {}, {}};
                if (item.contains("enabled")) {
                    if (!item["enabled"].is_boolean()) {
                        promise->Reject("effect_stack " + id + ": enabled must be a boolean");
                        return;
                    }
                    entry.enabled = item["enabled"].get<bool>();
                }
                if (item.contains("params")) {
                    if (!item["params"].is_object()) {
                        promise->Reject("effect_stack " + id + ": params must be an object {key: number}");
                        return;
                    }
                    for (const auto& [key, value] : item["params"].items()) {
                        const auto it = std::find_if(ep->params.begin(), ep->params.end(),
                                                     [&](const ShaderPackParam& sp) { return sp.key == key; });
                        if (it == ep->params.end() || !value.is_number()) {
                            std::string known;
                            for (const ShaderPackParam& sp : ep->params) known += (known.empty() ? "" : ", ") + sp.key;
                            promise->Reject("effect_stack " + id + ": unknown or non-numeric param '" + key +
                                            "' (known: " + known + ")");
                            return;
                        }
                        entry.params[key] = std::clamp(value.get<float>(), it->min, it->max);
                    }
                }
                if (item.contains("texture_folder")) {
                    if (!item["texture_folder"].is_string()) {
                        promise->Reject("effect_stack " + id + ": texture_folder must be a string");
                        return;
                    }
                    entry.textureFolder = item["texture_folder"].get<std::string>();
                }
                effects.push_back(std::move(entry));
            }
        }

        settings_.renderPath = renderPath;
        settings_.upscaler = upscaler;
        settings_.upscalerQuality = upscaleQuality;
        if (args.contains("lighting_preset")) settings_.lighting = args["lighting_preset"].get<int>();
        if (args.contains("quality")) ApplyGraphicsPreset(args["quality"].get<int>());
        if (args.contains("dof")) settings_.dof = args["dof"].get<bool>();
        if (args.contains("volumetric")) settings_.volumetric = args["volumetric"].get<bool>();
        if (args.contains("bloom_conv")) settings_.bloomConvolution = args["bloom_conv"].get<bool>();
        if (args.contains("shader_pack")) {
            ShaderChoice sc = settings_.CharacterShader(scene_->characterId);  // keeps the pack's other choices
            if (sc.pack != shaderPack) sc = ShaderChoice{};
            sc.pack = shaderPack;
            settings_.SetCharacterShader(scene_->characterId, sc);
        }
        if (args.contains("effects") || args.contains("effect_stack")) settings_.effectStack = std::move(effects);
        ApplyRenderSettings();
        settings_.Save(settingsPath_);
        ExecuteMcp("get_render_settings", {}, promise);  // answers with the settings now in effect
        return;
    }

    if (tool == "studio_get_state") {
        if (!studio_) {
            promise->Reject("Studio is not active");
            return;
        }
        const studio::StudioDoc& d = *studio_;
        nlohmann::json sj;
        sj["frame"] = d.Frame();
        sj["selected_model"] = d.selectedModel;
        sj["active_bone"] = d.activeBone;
        sj["use_motion_camera"] = d.useMotionCamera;
        sj["use_shadow_track"] = d.useShadowTrack;
        sj["range_start"] = d.view.rangeStart;
        sj["range_end"] = d.view.rangeEnd;
        sj["loop"] = d.loop;
        sj["playing"] = d.playing;
        sj["physics"] = d.physics;
        sj["undo"] = d.history.UndoName();
        sj["redo"] = d.history.RedoName();
        sj["undo_count"] = d.history.Count();
        sj["project_path"] = PathToUtf8(d.projectPath);
        sj["dirty"] = d.Dirty();

        nlohmann::json modelsJson = nlohmann::json::array();
        for (size_t i = 0; i < d.models.size(); ++i) {
            const auto& m = *d.models[i];
            size_t boneKeys = 0, morphKeys = 0;
            for (const auto& [n, k] : m.motion.bones) boneKeys += k.size();
            for (const auto& [n, k] : m.motion.morphs) morphKeys += k.size();
            modelsJson.push_back({
                {"index", i},
                {"kind", studio::ModelKindName(m.kind)},
                {"name", m.name},
                {"visible", m.visible},
                {"bone_keys", boneKeys},
                {"morph_keys", morphKeys},
                {"file", PathToUtf8(m.path.filename())}
            });
        }
        sj["models"] = modelsJson;

        nlohmann::json lightsJson = nlohmann::json::array();
        for (size_t i = 0; i < d.lights.size(); ++i) {
            const auto& l = d.lights[i];
            auto cur = studio::SampleLightValues(l, d.Frame());
            lightsJson.push_back({
                {"index", i},
                {"uid", l.uid},
                {"kind", studio::LightKindName(l.kind)},
                {"name", l.name},
                {"enabled", l.enabled},
                {"intensity", cur.intensity},
                {"range", cur.range},
                {"keys", l.keys.size()}
            });
        }
        sj["lights"] = lightsJson;
        promise->Resolve(sj);
        return;
    }

    if (tool == "studio_command") {
        std::string cmd = args.value("cmd", "");
        std::vector<std::string> cmdArgs;
        if (args.contains("args") && args["args"].is_array()) {
            for (const auto& a : args["args"]) cmdArgs.push_back(a.get<std::string>());
        }
        const bool discard = args.value("discard_unsaved", false);
        if (cmd == "studionew") {
            ExecuteMcp("open_studio", {{"discard_unsaved", discard}}, promise);
            return;
        }
        if (cmd == "studioopen") {
            if (cmdArgs.empty()) { promise->Reject("studioopen needs a file"); return; }
            ExecuteMcp("studio_open", {{"file", cmdArgs[0]}, {"discard_unsaved", discard}}, promise);
            return;
        }
        if (cmd == "studiosave") {
            if (cmdArgs.empty()) { promise->Reject("studiosave needs a file"); return; }
            ExecuteMcp("studio_save", {{"file", cmdArgs[0]}}, promise);
            return;
        }
        if (!studio_ && cmd.rfind("studio", 0) == 0 && cmd != "studiostate") {
            promise->Reject("Studio is not active");
            return;
        }
        std::string err;
        // multi-frame commands (gizmo drags, clicks) continue through the ui_input queue
        const auto sched = [this](int frame, std::string c, std::vector<std::string> a) {
            McpScheduleInput(mcpFrame_ + (frame - framesInScene_), std::move(c), std::move(a));
            mcpInputActive_ = true;
        };
        bool ok = ExecuteUiCommand(cmd, cmdArgs, sched, &err);
        if (ok) promise->Resolve({{"ok", true}, {"cmd", cmd}});
        else promise->Reject(err.empty() ? ("Failed studio command: " + cmd) : err);
        return;
    }

    if (tool == "studio_save") {
        std::string file = args.value("file", "");
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        if (file.empty()) { promise->Reject("File path cannot be empty"); return; }
        const std::filesystem::path path = std::filesystem::absolute(Utf8ToPath(file));
        if (!StudioSaveTo(path)) {
            promise->Reject("Saving failed: " + PathToUtf8(path) + " (see get_recent_logs)");
            return;
        }
        promise->Resolve({{"saved", PathToUtf8(path)}});
        return;
    }

    if (tool == "studio_open") {
        const std::string file = args.value("file", "");
        if (file.empty()) { promise->Reject("File path cannot be empty"); return; }
        ExecuteMcp("open_studio", {{"project", file}, {"discard_unsaved", args.value("discard_unsaved", false)}}, promise);
        return;
    }

    if (tool == "studio_add_model") {
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        std::string kindStr = args.value("kind", "");
        studio::ModelKind kind = studio::ModelKind::Character;
        if (!studio::ParseModelKind(kindStr, kind)) {
            promise->Reject("Unknown model kind: " + kindStr + " (character, stage, prop)");
            return;
        }
        const size_t jobsBefore = studioJobs_.size();
        if (args.contains("file")) {
            const std::filesystem::path file = std::filesystem::absolute(Utf8ToPath(args["file"].get<std::string>()));
            if (!std::filesystem::exists(file)) { promise->Reject("File not found: " + PathToUtf8(file)); return; }
            StudioAddModelFile(kind, file);
        } else if (args.contains("library")) {
            const std::string needle = args["library"].get<std::string>();
            if (kind == studio::ModelKind::Character) {
                const CharacterAsset* c = FindCharacter(needle);
                if (!c) { promise->Reject("Character not found: " + needle); return; }
                StudioAddLibraryCharacter((int)(c - library_.characters.data()));
            } else if (kind == studio::ModelKind::Stage) {
                const StageAsset* st = FindStage(needle);
                if (!st) { promise->Reject("Stage not found: " + needle); return; }
                StudioAddLibraryStage((int)(st - library_.stages.data()));
            } else {
                promise->Reject("Props are added by 'file'");
                return;
            }
        } else {
            promise->Reject("studio_add_model requires 'file' or 'library'");
            return;
        }
        if (studioJobs_.size() == jobsBefore) {
            promise->Reject("The model could not be added (see get_recent_logs)");
            return;
        }
        studioJobs_.back()->mcp = promise;  // StudioPollJobs answers when the load is done
        return;
    }

    if (tool == "studio_select") {
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        int idx = args.value("index", -1);
        if (idx < -1 || idx >= (int)studio_->models.size()) {
            promise->Reject("Model index out of range: " + std::to_string(idx) + " (-1 = camera, 0.." +
                            std::to_string((int)studio_->models.size() - 1) + ")");
            return;
        }
        StudioSelectModel(idx);
        promise->Resolve({{"selected", idx}});
        return;
    }

    if (tool == "studio_set_camera") {
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        studio::StudioDoc& d = *studio_;
        const bool key = args.value("key", false);
        FreeCamera cam = freeCam_;
        if (d.useMotionCamera && !d.camera.camera.empty()) {  // start from what the viewport shows
            const studio::CameraKf k = StudioViewedCamera(d.Frame());
            cam.target = k.target;
            cam.pitch = -k.rotation.x;
            cam.yaw = k.rotation.y;
            cam.distance = -k.distance;
            cam.fovDeg = (float)k.fovDeg;
        }
        if (args.contains("tx")) cam.target.x = args["tx"].get<float>();
        if (args.contains("ty")) cam.target.y = args["ty"].get<float>();
        if (args.contains("tz")) cam.target.z = args["tz"].get<float>();
        if (args.contains("yaw_deg")) cam.yaw = DirectX::XMConvertToRadians(args["yaw_deg"].get<float>());
        if (args.contains("pitch_deg")) cam.pitch = DirectX::XMConvertToRadians(args["pitch_deg"].get<float>());
        if (args.contains("dist")) cam.distance = args["dist"].get<float>();
        if (args.contains("fov_deg")) cam.fovDeg = args["fov_deg"].get<float>();
        if (key) {
            // the motion camera's key at the playhead (inserted first when there is none, one undo step each)
            if (!studio::FindKey(d.camera.camera, d.Frame()) && !d.autoKey) {
                const int sel = d.selectedModel;
                d.selectedModel = -1;  // camera rows belong to the camera selection
                StudioInsertKeys({studio::MakeRowId(studio::RowKind::Camera, 0, 0)}, d.Frame());
                d.selectedModel = sel;
            }
            StudioWriteCamera(cam, nullptr, true);
            StudioEndKeyEdit();
            d.useMotionCamera = true;
        } else {
            freeCam_ = cam;
            d.useMotionCamera = false;  // the free view (the motion camera's keys stay)
        }
        promise->Resolve({{"view", d.useMotionCamera ? "motion_camera" : "free"}, {"keyed", key}, {"frame", d.Frame()},
                          {"camera_keys", d.camera.camera.size()}});
        return;
    }

    if (tool == "studio_set_bone") {
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        studio::StudioModel* m = StudioPoseModel();
        if (!m) { promise->Reject("The selected model is not a character (studio_select one)"); return; }
        const std::string boneName = args.value("bone", "");
        const int bone = m->pmx->FindBone(boneName);
        if (bone < 0) { promise->Reject("Bone not found: " + boneName); return; }

        // the same edit as the inspector's bone fields: values are VMD-local, an untouched channel keeps the
        // motion's value at this frame, the edit is one undo step (and keys itself with auto-key)
        StudioSelectBone(bone, false);
        const int frame = studio_->Frame();
        const studio::PoseLayer before = m->pose;
        studio::PoseLayer& pose = m->pose;
        if (pose.frame != frame) pose = studio::PoseLayer{};
        pose.frame = frame;
        const auto it = pose.bones.find(bone);
        studio::PoseBone v = it != pose.bones.end()
                                 ? it->second
                                 : studio::PoseBone{m->inst->BoneAnimTranslation(bone), m->inst->BoneAnimRotation(bone)};
        if (args.contains("translate")) {
            const auto& t = args["translate"];
            v.t = {t.at(0).get<float>(), t.at(1).get<float>(), t.at(2).get<float>()};
        }
        if (args.contains("rotate_deg")) {
            const auto& r = args["rotate_deg"];
            DirectX::XMStoreFloat4(&v.r, DirectX::XMQuaternionRotationRollPitchYaw(
                                             DirectX::XMConvertToRadians(r.at(0).get<float>()),
                                             DirectX::XMConvertToRadians(r.at(1).get<float>()),
                                             DirectX::XMConvertToRadians(r.at(2).get<float>())));
        }
        pose.bones[bone] = v;
        StudioSetPose(Tr("본 값 편집"), before);
        const bool keyed = studio_->autoKey || args.value("key", false);
        if (args.value("key", false) && !studio_->autoKey) StudioRegisterPose(false);
        promise->Resolve({{"bone", boneName}, {"frame", frame}, {"keyed", keyed}});
        return;
    }

    if (tool == "studio_key") {
        if (!studio_) { promise->Reject("Studio is not active"); return; }
        studio::StudioDoc& d = *studio_;
        const std::string what = ToLowerAscii(args.value("what", "selected"));
        const size_t undoBefore = d.history.Count();
        if (what == "camera" || what == "lights") {
            std::vector<uint64_t> rows;
            if (what == "camera") rows.push_back(studio::MakeRowId(studio::RowKind::Camera, 0, 0));
            else
                for (const auto& l : d.lights) rows.push_back(studio::MakeRowId(studio::RowKind::SceneLight, 0, l.uid));
            const int sel = d.selectedModel;
            d.selectedModel = -1;  // camera / light rows belong to the camera selection (StudioTrackOfRow)
            if (!rows.empty()) StudioInsertKeys(rows, d.Frame());
            d.selectedModel = sel;
        } else if (what == "all" || what == "selected") {
            if (!StudioPoseModel()) { promise->Reject("Select a character first (studio_select)"); return; }
            if (what == "all") StudioRegisterPose(true);
            else StudioRegisterKeys();
        } else {
            promise->Reject("Invalid what: " + what + " (selected, all, camera, lights)");
            return;
        }
        promise->Resolve({{"what", what}, {"frame", d.Frame()}, {"changed", d.history.Count() != undoBefore},
                          {"undo", d.history.UndoName()}});
        return;
    }

    if (tool == "undo") {
        if (studio_ && studio_->history.CanUndo()) {
            std::string name = studio_->history.UndoName();
            studio_->history.Undo();
            studio_->selection.clear();
            studio_->rowsKey = ~0ull;
            promise->Resolve({{"undo", name}});
        } else {
            promise->Resolve({{"undo", "none"}});
        }
        return;
    }

    if (tool == "redo") {
        if (studio_ && studio_->history.CanRedo()) {
            std::string name = studio_->history.RedoName();
            studio_->history.Redo();
            studio_->selection.clear();
            studio_->rowsKey = ~0ull;
            promise->Resolve({{"redo", name}});
        } else {
            promise->Resolve({{"redo", "none"}});
        }
        return;
    }

    if (tool == "render_still" || tool == "render_video") {
        const bool video = tool == "render_video";
        if (offline_.mode != OfflineMode::None) { promise->Reject("A render is already running"); return; }
        const bool studio = screen_ == Screen::Studio && studio_;
        if (!studio && !(screen_ == Screen::Play && scene_)) {
            promise->Reject("Rendering needs a loaded scene (load_scene) or the studio");
            return;
        }
        if (args.contains("width") != args.contains("height")) {
            promise->Reject("Give both width and height");
            return;
        }
        if (args.contains("width") && (args["width"].get<int>() < 16 || args["height"].get<int>() < 16 ||
                                       args["width"].get<int>() > 7680 || args["height"].get<int>() > 4320)) {
            promise->Reject("width / height must be within 16x16 .. 7680x4320");
            return;
        }
        if (args.contains("renderer")) {
            const std::string r = args["renderer"];
            if (r != "raster" && r != "rt" && r != "pt" && r != "gi") { promise->Reject("Invalid renderer: " + r); return; }
        }
        if (args.contains("fps")) {
            const int fps = args["fps"].get<int>();
            if (fps != 24 && fps != 30 && fps != 60) { promise->Reject("fps must be 24, 30 or 60"); return; }
        }
        if (video) {
            const double duration = studio ? studio_->EndFrame() / (double)kMmdFps : scene_->endFrame / kMmdFps;
            const double a = args.value("start", 0.0), b = args.value("end", duration);
            if (a < 0.0 || b <= a || a >= duration) {
                promise->Reject("Invalid range: start " + std::to_string(a) + ", end " + std::to_string(b) + " (duration " +
                                std::to_string(duration) + " s)");
                return;
            }
        }
        // The run's offline overrides are borrowed for this job and restored when it ends (PumpMcp). The output
        // path does not go through options_.offlineStill/Video: those mark a CLI job, which quits the app.
        if (!mcpOptionsRestore_) mcpOptionsRestore_ = std::make_unique<AppOptions>(options_);
        if (args.contains("spp")) options_.offlineSpp = args["spp"].get<int>();
        if (args.contains("width") && args.contains("height")) {
            options_.offlineSize[0] = args["width"].get<int>();
            options_.offlineSize[1] = args["height"].get<int>();
        }
        if (args.contains("fps")) options_.offlineFps = args["fps"].get<int>();
        if (args.contains("bitrate")) options_.offlineBitrate = args["bitrate"].get<int>();
        if (args.contains("quality")) options_.offlineQuality = args["quality"].get<int>();
        if (args.contains("renderer")) {
            const std::string r = args["renderer"];
            options_.offlineRenderer = r == "raster" ? 0 : r == "rt" ? 1 : r == "pt" ? 2 : r == "gi" ? 3 : options_.offlineRenderer;
        }
        mcpOfflineOutput_.clear();
        if (args.contains("output")) {
            mcpOfflineOutput_ = std::filesystem::absolute(Utf8ToPath(args["output"].get<std::string>()));
            std::error_code ec;
            std::filesystem::create_directories(mcpOfflineOutput_.parent_path(), ec);
        }
        toast_ = {};
        if (!video) {
            if (studio) StartStudioRender(false);
            else StartOfflineStill();
        } else {
            const double duration = studio ? studio_->EndFrame() / (double)kMmdFps : scene_->endFrame / kMmdFps;
            StartOfflineVideo(args.value("start", 0.0), args.value("end", duration));
        }
        mcpOfflineOutput_.clear();
        if (offline_.mode == OfflineMode::None) {
            promise->Reject(toast_.title.empty() ? std::string("The render could not start") : toast_.title + ": " + toast_.detail);
            return;
        }
        promise->Resolve({{"status", "started"}, {"mode", video ? "video" : "still"}, {"output", PathToUtf8(offline_.output)},
                          {"frame_count", offline_.frameCount}});
        return;
    }

    if (tool == "render_status") {
        nlohmann::json st;
        st["active"] = (offline_.mode != OfflineMode::None);
        st["mode"] = (offline_.mode == OfflineMode::Still ? "still" : offline_.mode == OfflineMode::Video ? "video" : offline_.mode == OfflineMode::Probe ? "probe" : "none");
        st["frame"] = offline_.frame;
        st["frame_count"] = offline_.frameCount;
        st["progress"] = offline_.frameCount > 0 ? ((double)offline_.frame / (double)offline_.frameCount) : 0.0;
        st["output"] = PathToUtf8(offline_.output);
        promise->Resolve(st);
        return;
    }

    if (tool == "render_cancel") {
        LOG_INFO("offline render: cancelled over MCP");
        offline_.cancelRequested = true;
        promise->Resolve({{"status", "cancelling"}});
        return;
    }

    if (tool == "ui_input") {
        if (!args.contains("events") || !args["events"].is_array()) {
            promise->Reject("ui_input requires an 'events' array");
            return;
        }
        static const char* const kCmds[] = {"move", "down", "up", "click", "dblclick", "wheel", "key", "keyup", "text", "mod"};
        for (const auto& ev : args["events"]) {
            const std::string cmd = ev.value("cmd", "");
            if (std::none_of(std::begin(kCmds), std::end(kCmds), [&](const char* c) { return cmd == c; })) {
                promise->Reject("Unknown input command: '" + cmd + "' (move, down, up, click, dblclick, wheel, key, keyup, text, mod)");
                return;
            }
            const size_t n = ev.contains("args") && ev["args"].is_array() ? ev["args"].size() : 0;
            const size_t need = cmd == "move" || cmd == "click" || cmd == "dblclick" || cmd == "mod" ? 2
                                : cmd == "wheel" || cmd == "key" || cmd == "keyup" || cmd == "text" ? 1 : 0;
            if (n < need) {
                promise->Reject("'" + cmd + "' needs " + std::to_string(need) + " argument(s)");
                return;
            }
        }
        // events run one per frame unless they give "frame" (offset from now); clicks add their own press / release
        int frame = mcpFrame_, last = mcpFrame_;
        for (const auto& ev : args["events"]) {
            frame = ev.contains("frame") ? mcpFrame_ + 1 + ev["frame"].get<int>() : frame + 1;
            std::vector<std::string> evArgs;
            if (ev.contains("args") && ev["args"].is_array())
                for (const auto& a : ev["args"]) evArgs.push_back(a.is_string() ? a.get<std::string>() : a.dump());
            McpScheduleInput(frame, ev.value("cmd", ""), std::move(evArgs));
            last = std::max(last, frame + 5);  // room for a double click's follow-up steps
        }
        mcpInputActive_ = true;
        pendingMcpInputs_.push_back({last, promise});
        return;
    }

    if (tool == "quit_app") {
        running_ = false;
        PostQuitMessage(0);
        promise->Resolve({{"status", "quitting"}});
        return;
    }

    promise->Reject("Unknown tool: " + tool);
}

} // namespace mmdx
