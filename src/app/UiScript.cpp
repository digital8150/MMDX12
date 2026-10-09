// Scripted ImGui input for unattended UI tests (--ui-script). Events are queued right before
// ImGui::NewFrame, so widgets see them exactly like real mouse/keyboard input.
#include "app/App.h"

#include <fstream>
#include <sstream>

#include "anim/ModelInstance.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

namespace {
int ButtonOf(const std::vector<std::string>& args, size_t i) {
    if (args.size() <= i) return 0;
    return args[i] == "r" ? 1 : args[i] == "m" ? 2 : 0;
}

ImGuiKey KeyByName(const std::string& name) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (name == ImGui::GetKeyName((ImGuiKey)k)) return (ImGuiKey)k;
    return ImGuiKey_None;
}
} // namespace

void App::PumpUiScript() {
    if (options_.uiScript.empty()) return;
    if (!uiScriptLoaded_) {
        uiScriptLoaded_ = true;
        std::ifstream f(options_.uiScript);
        if (!f) {
            LOG_ERROR("ui script not found: %s", PathToUtf8(options_.uiScript).c_str());
            return;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            UiScriptStep s;
            if (!(ss >> s.frame >> s.cmd)) continue;
            if (s.cmd == "log") {
                std::string rest;
                std::getline(ss, rest);
                s.args.push_back(rest);
            } else {
                for (std::string a; ss >> a;) s.args.push_back(a);
            }
            uiScript_.push_back(std::move(s));
        }
        std::stable_sort(uiScript_.begin(), uiScript_.end(),
                         [](const UiScriptStep& a, const UiScriptStep& b) { return a.frame < b.frame; });
        LOG_INFO("ui script: %d steps", (int)uiScript_.size());
    }
    // Follow-up events (button/key releases) are inserted after the current position, in frame order.
    auto schedule = [&](int frame, std::string cmd, std::vector<std::string> args) {
        size_t i = uiScriptNext_;
        while (i < uiScript_.size() && uiScript_[i].frame <= frame) ++i;
        uiScript_.insert(uiScript_.begin() + (ptrdiff_t)i, UiScriptStep{frame, std::move(cmd), std::move(args)});
    };

    ImGuiIO& io = ImGui::GetIO();
    const int now = framesInScene_;
    // The Win32 backend feeds the real cursor position when it is outside the focused window: keep ours.
    if (uiScriptMouse_[0] > -1e29f) io.AddMousePosEvent(uiScriptMouse_[0], uiScriptMouse_[1]);
    while (uiScriptNext_ < uiScript_.size() && uiScript_[uiScriptNext_].frame <= now) {
        const UiScriptStep s = uiScript_[uiScriptNext_++];
        const auto num = [&](size_t i) { return i < s.args.size() ? (float)std::atof(s.args[i].c_str()) : 0.0f; };
        if (s.cmd == "move") {
            { uiScriptMouse_[0] = num(0); uiScriptMouse_[1] = num(1); io.AddMousePosEvent(num(0), num(1)); }
        } else if (s.cmd == "down") {
            io.AddMouseButtonEvent(ButtonOf(s.args, 0), true);
        } else if (s.cmd == "up") {
            io.AddMouseButtonEvent(ButtonOf(s.args, 0), false);
        } else if (s.cmd == "click" || s.cmd == "dblclick") {
            // Move first and press on the next frame: a press in the frame the mouse arrives is seen with the previous
            // frame's hovered window, so release-activated buttons in child windows would miss the click.
            { uiScriptMouse_[0] = num(0); uiScriptMouse_[1] = num(1); io.AddMousePosEvent(num(0), num(1)); }
            schedule(now + 1, "down", {"l"});
            schedule(now + 2, "up", {"l"});
            if (s.cmd == "dblclick") {
                schedule(now + 3, "down", {"l"});
                schedule(now + 4, "up", {"l"});
            }
        } else if (s.cmd == "text") {
            for (size_t i = 0; i < s.args.size(); ++i) io.AddInputCharactersUTF8((i ? " " + s.args[i] : s.args[i]).c_str());
        } else if (s.cmd == "wheel") {
            io.AddMouseWheelEvent(0.0f, num(0));
        } else if (s.cmd == "key" || s.cmd == "keyup") {
            const bool down = s.cmd == "key";
            const ImGuiKey key = s.args.empty() ? ImGuiKey_None : KeyByName(s.args[0]);
            if (key == ImGuiKey_None) {
                LOG_WARN("ui script: unknown key '%s'", s.args.empty() ? "" : s.args[0].c_str());
                continue;
            }
            bool ctrl = false, shift = false;
            for (size_t i = 1; i < s.args.size(); ++i) {
                ctrl |= s.args[i] == "ctrl";
                shift |= s.args[i] == "shift";
            }
            if (down) {
                if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
                if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
                io.AddKeyEvent(key, true);
                schedule(now + 1, "keyup", s.args);
            } else {
                io.AddKeyEvent(key, false);
                if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
                if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
            }
        } else if (s.cmd == "mod") {  // mod <ctrl|shift> <down|up>: hold a modifier across mouse steps
            if (s.args.size() >= 2)
                io.AddKeyEvent(s.args[0] == "ctrl" ? ImGuiMod_Ctrl : ImGuiMod_Shift, s.args[1] == "down");
        } else if (s.cmd == "capture") {
            if (!s.args.empty()) ctx_.RequestCapture(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studiostate") {
            if (studio_) {
                const studio::StudioDoc& d = *studio_;
                size_t bones = 0, morphs = 0;
                if (d.selectedModel >= 0 && d.selectedModel < (int)d.models.size()) {
                    for (const auto& [n, k] : d.models[d.selectedModel]->motion.bones) bones += k.size();
                    for (const auto& [n, k] : d.models[d.selectedModel]->motion.morphs) morphs += k.size();
                }
                std::string sel;  // the first 16 selected frames
                int listed = 0;
                for (const auto& k : d.selection) {
                    if (++listed > 16) { sel += " ..."; break; }
                    sel += " " + std::to_string(k.second);
                }
                LOG_INFO("STUDIOSTATE frame=%d model=%d boneKeys=%zu morphKeys=%zu cameraKeys=%zu "
                         "lightKeys=%zu shadowKeys=%zu motionCam=%d shadowTrack=%d sceneLights=%zu "
                         "selected=%zu selection=[%s ] "
                         "rows=%zu range=%d..%d loop=%d playing=%d physics=%d undo='%s' redo='%s' undoCount=%zu undoMB=%.1f "
                         "scroll=%.1f zoom=%.2f hoveredWindow=%s activeId=%08x",
                         d.Frame(), d.selectedModel, bones, morphs, d.camera.camera.size(), d.camera.light.size(),
                         d.camera.shadow.size(), (int)d.useMotionCamera, (int)d.useShadowTrack, d.lights.size(),
                         d.selection.size(), sel.c_str(),
                         d.selectedRows.size(), d.view.rangeStart, d.view.rangeEnd, (int)d.loop, (int)d.playing,
                         (int)d.physics, d.history.UndoName().c_str(), d.history.RedoName().c_str(), d.history.Count(),
                         d.history.Bytes() / 1048576.0, d.view.scrollFrame, d.view.pxPerFrame,
                         GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "-", (unsigned)GImGui->ActiveId);
            }
            if (studio_) {
                const studio::StudioDoc& d = *studio_;
                const studio::StudioModel* m = d.Selected();
                std::string bone = "-", value;
                size_t poseBones = 0, poseMorphs = 0;
                int poseFrame = -1;
                if (m && !m->IsStage()) {
                    poseBones = m->pose.bones.size();
                    poseMorphs = m->pose.morphs.size();
                    poseFrame = m->pose.frame;
                    if (d.activeBone >= 0 && d.activeBone < (int)m->pmx->bones.size()) {
                        bone = m->pmx->bones[(size_t)d.activeBone].name;
                        const auto& t = m->inst->BoneAnimTranslation(d.activeBone);
                        const auto& r = m->inst->BoneAnimRotation(d.activeBone);
                        char buf[160];
                        std::snprintf(buf, sizeof(buf), "t=(%.3f,%.3f,%.3f) r=(%.4f,%.4f,%.4f,%.4f)", t.x, t.y, t.z, r.x, r.y, r.z, r.w);
                        value = buf;
                    }
                }
                LOG_INFO("STUDIOPOSE bone=%s %s selBones=%zu pose=%zu/%zu@%d tool=%d local=%d tab=%d gizmo=%d viewDrag=%d",
                         bone.c_str(), value.c_str(), d.selectedBones.size(), poseBones, poseMorphs, poseFrame, d.gizmoTool,
                         (int)d.gizmoLocal, d.inspectorTab, (int)studioGizmoShown_, studioViewDrag_);
                // project + every model (save / reopen comparisons, prop following)
                LOG_INFO("STUDIOPROJ path=%s dirty=%d models=%zu jobs=%zu audio=%s offset=%.3f end=%d camKeys=%zu "
                         "lightKeys=%zu shadowKeys=%zu",
                         PathToUtf8(d.projectPath.filename()).c_str(), (int)d.Dirty(), d.models.size(), studioJobs_.size(),
                         d.hasAudio ? PathToUtf8(d.audioPath.filename()).c_str() : "-", d.audioOffset, d.EndFrame(),
                         d.camera.camera.size(), d.camera.light.size(), d.camera.shadow.size());
                for (size_t i = 0; i < d.models.size(); ++i) {
                    const studio::StudioModel& mm = *d.models[i];
                    size_t keys = 0;
                    for (const auto& [n, k] : mm.motion.bones) keys += k.size();
                    for (const auto& [n, k] : mm.motion.morphs) keys += k.size();
                    // a probe point: the prop's origin, else the center bone (or bone 0)
                    DirectX::XMFLOAT3 pos{};
                    if (mm.IsProp()) {
                        const DirectX::XMFLOAT4X4 r = StudioPropRoot(mm);
                        pos = {r._41, r._42, r._43};
                    } else if (!mm.pmx->bones.empty()) {
                        const int c = mm.pmx->FindBone("ã»ã³ã¿ã¼");
                        pos = mm.inst->BoneWorldPosition(c >= 0 ? c : 0);
                    }
                    const int parent = mm.attach.parent >= 0 ? d.IndexOfUid((uint32_t)mm.attach.parent) : -1;
                    LOG_INFO("STUDIOMODEL %zu kind=%s name=%s visible=%d keys=%zu parent=%d bone=%s t=(%.3f,%.3f,%.3f) "
                             "r=(%.2f,%.2f,%.2f) s=%.3f pos=(%.3f,%.3f,%.3f) file=%s",
                             i, studio::ModelKindName(mm.kind), mm.name.c_str(), (int)mm.visible, keys, parent,
                             mm.attach.bone.empty() ? "-" : mm.attach.bone.c_str(), mm.attach.translation.x,
                             mm.attach.translation.y, mm.attach.translation.z, mm.attach.rotationDeg.x,
                             mm.attach.rotationDeg.y, mm.attach.rotationDeg.z, mm.attach.scale, pos.x, pos.y, pos.z,
                             PathToUtf8(mm.path.filename()).c_str());
                }
            }
        } else if (s.cmd == "studiobone") {  // studiobone <name>: click the bone's joint in the viewport
            const studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
            const int bone = m && !s.args.empty() ? m->pmx->FindBone(s.args[0]) : -1;
            ImVec2 pt;
            if (bone < 0 || !studioVp_.Project(studio::BoneJointWorld(*m->pmx, *m->inst, bone), pt)) {
                LOG_WARN("ui script: bone '%s' not found or not visible", s.args.empty() ? "" : s.args[0].c_str());
            } else {
                uiScriptMouse_[0] = pt.x;
                uiScriptMouse_[1] = pt.y;
                io.AddMousePosEvent(pt.x, pt.y);
                schedule(now + 1, "down", {"l"});
                schedule(now + 2, "up", {"l"});
            }
        } else if (s.cmd == "studiogizmo") {  // studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [frames]: drag a gizmo part
            static const char* const kParts[] = {"", "x", "y", "z", "yz", "zx", "xy", "rx", "ry", "rz"};
            int part = 0;
            for (int i = 1; i < 10; ++i)
                if (!s.args.empty() && s.args[0] == kParts[i]) part = i;
            ImVec2 pt;
            if (!studio_ || part == 0 || !StudioScriptGizmoPoint(part, pt)) {
                LOG_WARN("ui script: gizmo part '%s' not shown", s.args.empty() ? "" : s.args[0].c_str());
            } else {
                const int steps = std::max(1, s.args.size() > 3 ? std::atoi(s.args[3].c_str()) : 8);
                LOG_INFO("UISCRIPT gizmo %s at %.0f,%.0f", s.args[0].c_str(), pt.x, pt.y);
                uiScriptMouse_[0] = pt.x;
                uiScriptMouse_[1] = pt.y;
                io.AddMousePosEvent(pt.x, pt.y);
                schedule(now + 1, "down", {"l"});
                for (int k = 1; k <= steps; ++k) {
                    const float f = (float)k / steps;
                    schedule(now + 1 + k, "move", {std::to_string(pt.x + num(1) * f), std::to_string(pt.y + num(2) * f)});
                }
                schedule(now + steps + 2, "up", {"l"});
            }
        } else if (s.cmd == "studiovpdimport") {
            if (studio_ && !s.args.empty()) StudioImportVpdFrom(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studiovpdexport") {
            if (studio_ && !s.args.empty()) StudioExportVpdTo(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studioimport") {
            if (studio_ && !s.args.empty()) StudioImportVmdFrom(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studioexport") {
            if (studio_ && !s.args.empty()) StudioExportVmdTo(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studionew") {
            StudioRunAction(StudioAction::New, {});
        } else if (s.cmd == "studiosave") {
            if (studio_ && !s.args.empty()) StudioSaveTo(Utf8ToPath(s.args[0]));
        } else if (s.cmd == "studioopen") {
            if (!s.args.empty()) StudioRunAction(StudioAction::OpenFile, std::filesystem::absolute(Utf8ToPath(s.args[0])));
        } else if (s.cmd == "studioautosave") {
            if (studio_) StudioAutosave(true, true);
        } else if (s.cmd == "studioadd") {  // studioadd <character|stage|prop> <file>
            studio::ModelKind kind;
            if (studio_ && s.args.size() >= 2 && studio::ParseModelKind(s.args[0], kind))
                StudioAddModelFile(kind, std::filesystem::absolute(Utf8ToPath(s.args[1])));
        } else if (s.cmd == "studioaddlib" || s.cmd == "studiosong") {  // studioaddlib <character|stage> <substr>
            const bool song = s.cmd == "studiosong";
            const std::string kind = song ? "song" : (s.args.empty() ? "" : s.args[0]);
            const std::string needle = ToLowerAscii(s.args.size() > (song ? 0u : 1u) ? s.args[song ? 0 : 1] : "");
            const auto find = [&](const auto& list) {
                for (size_t i = 0; i < list.size(); ++i)
                    if (ToLowerAscii(list[i].id).find(needle) != std::string::npos ||
                        ToLowerAscii(list[i].displayName).find(needle) != std::string::npos)
                        return (int)i;
                return -1;
            };
            if (studio_ && !needle.empty()) {
                if (kind == "character") StudioAddLibraryCharacter(find(library_.characters));
                else if (kind == "stage") StudioAddLibraryStage(find(library_.stages));
                else if (kind == "song") StudioApplyLibrarySong(find(library_.songs));
            }
        } else if (s.cmd == "studioaudio") {  // studioaudio <file|none> [offset seconds]
            if (studio_ && !s.args.empty()) {
                if (s.args.size() > 1) studio_->audioOffset = num(1);
                StudioSetAudio(s.args[0] == "none" ? std::filesystem::path() : std::filesystem::absolute(Utf8ToPath(s.args[0])));
            }
        } else if (s.cmd == "studioselect") {
            if (studio_ && !s.args.empty()) StudioSelectModel(std::atoi(s.args[0].c_str()));
        } else if (s.cmd == "studioremove") {
            if (studio_ && !s.args.empty()) StudioRemoveModel(std::atoi(s.args[0].c_str()));
        } else if (s.cmd == "studiorename") {
            if (studio_ && studio_->Selected() && !s.args.empty()) {
                studio_->Selected()->name = s.args[0];
                ++studio_->projectVersion;
            }
        } else if (s.cmd == "studioattach") {  // studioattach <parent index|-1> <bone|-> tx ty tz rx ry rz s
            studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
            if (m && m->IsProp() && s.args.size() >= 9) {
                const int pi = std::atoi(s.args[0].c_str());
                m->attach.parent = pi >= 0 && pi < (int)studio_->models.size() ? (int)studio_->models[(size_t)pi]->uid : -1;
                m->attach.bone = s.args[1] == "-" ? std::string() : s.args[1];
                m->attach.translation = {num(2), num(3), num(4)};
                m->attach.rotationDeg = {num(5), num(6), num(7)};
                m->attach.scale = num(8);
                ++studio_->projectVersion;
            }
        } else if (s.cmd == "log") {
            LOG_INFO("UISCRIPT %s", s.args.empty() ? "" : s.args[0].c_str());
        } else if (s.cmd == "updatecheck") {  // synchronous update-feed check, logs UPDATECHECK
            UpdateCheckCommand();
        } else if (s.cmd == "updateinstall") {  // stage the feed's update; the app exits when staged
            UpdateInstallCommand();
        } else {
            LOG_WARN("ui script: unknown command '%s'", s.cmd.c_str());
        }
    }
}

} // namespace mmdx
