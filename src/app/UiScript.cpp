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
                LOG_INFO("STUDIOSTATE frame=%d model=%d boneKeys=%zu morphKeys=%zu cameraKeys=%zu selected=%zu selection=[%s ] "
                         "rows=%zu range=%d..%d loop=%d playing=%d physics=%d undo='%s' redo='%s' undoCount=%zu undoMB=%.1f "
                         "scroll=%.1f zoom=%.2f hoveredWindow=%s activeId=%08x",
                         d.Frame(), d.selectedModel, bones, morphs, d.camera.camera.size(), d.selection.size(), sel.c_str(),
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
                if (m && !m->isStage) {
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
        } else if (s.cmd == "log") {
            LOG_INFO("UISCRIPT %s", s.args.empty() ? "" : s.args[0].c_str());
        } else {
            LOG_WARN("ui script: unknown command '%s'", s.cmd.c_str());
        }
    }
}

} // namespace mmdx
