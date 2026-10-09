// Scripted ImGui input for unattended UI tests (--ui-script) and MCP dispatcher.
// Events are queued right before ImGui::NewFrame, so widgets see them exactly like real mouse/keyboard input.
#include "app/App.h"

#include <fstream>
#include <sstream>

#include "anim/ModelInstance.h"
#include "app/Lighting.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"

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

void App::ScheduleUiScriptStep(int frame, std::string cmd, std::vector<std::string> args) {
    size_t i = uiScriptNext_;
    while (i < uiScript_.size() && uiScript_[i].frame <= frame) ++i;
    uiScript_.insert(uiScript_.begin() + (ptrdiff_t)i, UiScriptStep{frame, std::move(cmd), std::move(args)});
}

bool App::ExecuteUiCommand(const std::string& cmd, const std::vector<std::string>& args,
                           const ScheduleFunc& schedule, std::string* outError) {
    auto defaultSchedule = [this](int f, std::string c, std::vector<std::string> a) {
        ScheduleUiScriptStep(f, std::move(c), std::move(a));
    };
    const auto& sched = schedule ? schedule : defaultSchedule;
    const int now = framesInScene_;
    ImGuiIO& io = ImGui::GetIO();
    const auto num = [&](size_t i) { return i < args.size() ? (float)std::atof(args[i].c_str()) : 0.0f; };

    if (cmd == "move") {
        uiScriptMouse_[0] = num(0); uiScriptMouse_[1] = num(1); io.AddMousePosEvent(num(0), num(1));
    } else if (cmd == "down") {
        io.AddMouseButtonEvent(ButtonOf(args, 0), true);
    } else if (cmd == "up") {
        io.AddMouseButtonEvent(ButtonOf(args, 0), false);
    } else if (cmd == "click" || cmd == "dblclick") {
        // Move first and press on the next frame: a press in the frame the mouse arrives is seen with the previous
        // frame's hovered window, so release-activated buttons in child windows would miss the click.
        uiScriptMouse_[0] = num(0); uiScriptMouse_[1] = num(1); io.AddMousePosEvent(num(0), num(1));
        sched(now + 1, "down", {"l"});
        sched(now + 2, "up", {"l"});
        if (cmd == "dblclick") {
            sched(now + 3, "down", {"l"});
            sched(now + 4, "up", {"l"});
        }
    } else if (cmd == "text") {
        for (size_t i = 0; i < args.size(); ++i) io.AddInputCharactersUTF8((i ? " " + args[i] : args[i]).c_str());
    } else if (cmd == "wheel") {
        io.AddMouseWheelEvent(0.0f, num(0));
    } else if (cmd == "key" || cmd == "keyup") {
        const bool down = cmd == "key";
        const ImGuiKey key = args.empty() ? ImGuiKey_None : KeyByName(args[0]);
        if (key == ImGuiKey_None) {
            LOG_WARN("ui script: unknown key '%s'", args.empty() ? "" : args[0].c_str());
            if (outError) *outError = "Unknown key: " + (args.empty() ? "" : args[0]);
            return false;
        }
        bool ctrl = false, shift = false;
        for (size_t i = 1; i < args.size(); ++i) {
            ctrl |= args[i] == "ctrl";
            shift |= args[i] == "shift";
        }
        if (down) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
            io.AddKeyEvent(key, true);
            sched(now + 1, "keyup", args);
        } else {
            io.AddKeyEvent(key, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        }
    } else if (cmd == "mod") {  // mod <ctrl|shift> <down|up>: hold a modifier across mouse steps
        if (args.size() >= 2)
            io.AddKeyEvent(args[0] == "ctrl" ? ImGuiMod_Ctrl : ImGuiMod_Shift, args[1] == "down");
    } else if (cmd == "capture") {
        if (!args.empty()) ctx_.RequestCapture(Utf8ToPath(args[0]));
    } else if (cmd == "studiostate") {
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
            LOG_INFO("STUDIOFOCUS keys=%zu distance=%.2f aperture=%.2f subject=%u subjectName=%s dof=%d", d.camera.focus.size(),
                     studioFocusShown_, studioFocusAperture_, studioFocusSubject_,
                     StudioModelLabel(studioFocusSubject_).c_str(), (int)settings_.dof);
        }
        if (studio_) {
            const studio::StudioDoc& d = *studio_;
            const studio::StudioModel* m = d.Selected();
            std::string bone = "-", value;
            size_t poseBones = 0, poseMorphs = 0;
            int poseFrame = -1;
            if (m) {
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
                DirectX::XMFLOAT3 pos{};
                if (mm.IsProp()) {
                    const DirectX::XMFLOAT4X4 r = StudioPropRoot(mm);
                    pos = {r._41, r._42, r._43};
                } else if (!mm.pmx->bones.empty()) {
                    const int c = mm.pmx->FindBone("ã‚»ãƒ³ã‚¿ãƒ¼");
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
            const LightAnchors anchors = StudioBuildLightAnchors();
            const int lightFrame = d.Frame();
            for (size_t i = 0; i < d.lights.size(); ++i) {
                const studio::SceneLight& l = d.lights[i];
                const studio::LightValues cur = studio::SampleLightValues(l, lightFrame);
                DirectX::XMFLOAT3 aim = cur.aim;
                if (l.kind == studio::LightKind::Spot) {
                    aim = ResolveSpotAim(l, cur, d.time, anchors);
                }
                if (l.kind == studio::LightKind::Area) {
                    LOG_INFO("STUDIOLIGHT %zu uid=%u kind=%s name='%s' enabled=%d pos=(%.3f,%.3f,%.3f) "
                             "aim=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f) col=(%.3f,%.3f,%.3f) "
                             "intensity=%.3f range=%.1f size=(%.1f,%.1f) keys=%zu visible=%d",
                             i, l.uid, studio::LightKindName(l.kind), l.name.c_str(), (int)l.enabled,
                             cur.position.x, cur.position.y, cur.position.z,
                             aim.x, aim.y, aim.z,
                             cur.direction.x, cur.direction.y, cur.direction.z,
                             cur.color.x, cur.color.y, cur.color.z,
                             cur.intensity, cur.range,
                             cur.size.x, cur.size.y,
                             l.keys.size(), (int)l.viewportVisible);
                } else {
                    LOG_INFO("STUDIOLIGHT %zu uid=%u kind=%s name='%s' enabled=%d pos=(%.3f,%.3f,%.3f) "
                             "aim=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f) col=(%.3f,%.3f,%.3f) "
                             "intensity=%.3f range=%.1f coneOuter=%.4f (%.1f deg) coneInner=%.4f (%.1f deg) keys=%zu visible=%d",
                             i, l.uid, studio::LightKindName(l.kind), l.name.c_str(), (int)l.enabled,
                             cur.position.x, cur.position.y, cur.position.z,
                             aim.x, aim.y, aim.z,
                             cur.direction.x, cur.direction.y, cur.direction.z,
                             cur.color.x, cur.color.y, cur.color.z,
                             cur.intensity, cur.range,
                             cur.coneOuter, DirectX::XMConvertToDegrees(cur.coneOuter),
                             cur.coneInner, DirectX::XMConvertToDegrees(cur.coneInner),
                             l.keys.size(), (int)l.viewportVisible);
                }
            }
        }
    } else if (cmd == "studiobone") {  // studiobone <name>: click the bone's joint in the viewport
        const studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
        const int bone = m && !args.empty() ? m->pmx->FindBone(args[0]) : -1;
        ImVec2 pt;
        if (bone < 0 || !studioVp_.Project(studio::BoneJointWorld(*m->pmx, *m->inst, bone), pt)) {
            LOG_WARN("ui script: bone '%s' not found or not visible", args.empty() ? "" : args[0].c_str());
            if (outError) *outError = "Bone not found or not visible: " + (args.empty() ? "" : args[0]);
            return false;
        } else {
            uiScriptMouse_[0] = pt.x;
            uiScriptMouse_[1] = pt.y;
            io.AddMousePosEvent(pt.x, pt.y);
            sched(now + 1, "down", {"l"});
            sched(now + 2, "up", {"l"});
        }
    } else if (cmd == "studiogizmo") {  // studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [frames]: drag a gizmo part
        static const char* const kParts[] = {"", "x", "y", "z", "yz", "zx", "xy", "rx", "ry", "rz"};
        int part = 0;
        for (int i = 1; i < 10; ++i)
            if (!args.empty() && args[0] == kParts[i]) part = i;
        ImVec2 pt;
        if (!studio_ || part == 0 || !StudioScriptGizmoPoint(part, pt)) {
            LOG_WARN("ui script: gizmo part '%s' not shown", args.empty() ? "" : args[0].c_str());
            if (outError) *outError = "Gizmo part not shown: " + (args.empty() ? "" : args[0]);
            return false;
        } else {
            const int steps = std::max(1, args.size() > 3 ? std::atoi(args[3].c_str()) : 8);
            LOG_INFO("UISCRIPT gizmo %s at %.0f,%.0f", args[0].c_str(), pt.x, pt.y);
            uiScriptMouse_[0] = pt.x;
            uiScriptMouse_[1] = pt.y;
            io.AddMousePosEvent(pt.x, pt.y);
            sched(now + 1, "down", {"l"});
            for (int k = 1; k <= steps; ++k) {
                const float f = (float)k / steps;
                sched(now + 1 + k, "move", {std::to_string(pt.x + num(1) * f), std::to_string(pt.y + num(2) * f)});
            }
            sched(now + steps + 2, "up", {"l"});
        }
    } else if (cmd == "studiolightpreset") {  // studiolightpreset <0-3>
        if (studio_ && !args.empty()) {
            const int p = std::clamp(std::atoi(args[0].c_str()), 0, 3);
            StudioApplyLightPreset(p);
        }
    } else if (cmd == "studiolightadd") {  // studiolightadd <sun|point|spot|ambient>
        if (studio_ && !args.empty()) {
            studio::LightKind kind = studio::LightKind::Point;
            if (studio::ParseLightKind(ToLowerAscii(args[0]), kind)) {
                StudioAddLight(kind);
            } else {
                LOG_WARN("ui script: unknown light kind '%s'", args[0].c_str());
                if (outError) *outError = "Unknown light kind: " + args[0];
                return false;
            }
        }
    } else if (cmd == "studiolightdel") {  // studiolightdel <index>
        if (studio_ && !args.empty()) {
            const int idx = std::atoi(args[0].c_str());
            if (idx >= 0 && idx < (int)studio_->lights.size()) {
                StudioDeleteLight(studio_->lights[(size_t)idx].uid);
            }
        }
    } else if (cmd == "studiolightsel") {  // studiolightsel <index>
        if (studio_ && !args.empty()) {
            const int idx = std::atoi(args[0].c_str());
            if (idx >= 0 && idx < (int)studio_->lights.size()) {
                StudioSelectLight(studio_->lights[(size_t)idx].uid);
            } else if (idx < 0) {
                StudioSelectLight(0);
            }
        }
    } else if (cmd == "studiofocus") {
        // studiofocus <frame> <auto|target|manual> [model index | distance] [aperture] [transition] [head|upper|center]
        if (studio_ && args.size() >= 2) {
            studio::StudioDoc& d = *studio_;
            studio::FocusKf k;
            k.frame = std::max(0, std::atoi(args[0].c_str()));
            k.mode = args[1] == "target" ? studio::FocusMode::Target
                     : args[1] == "manual" ? studio::FocusMode::Manual
                                           : studio::FocusMode::Auto;
            if (args.size() > 2) {
                if (k.mode == studio::FocusMode::Target) {
                    const int mi = std::atoi(args[2].c_str());
                    k.target = mi >= 0 && mi < (int)d.models.size() ? d.models[(size_t)mi]->uid : 0;
                } else if (k.mode == studio::FocusMode::Manual) {
                    k.distance = std::clamp((float)std::atof(args[2].c_str()), 0.5f, 3000.0f);
                }
            }
            if (args.size() > 3) k.aperture = std::clamp((float)std::atof(args[3].c_str()), 0.0f, 3.0f);
            if (args.size() > 4) k.transition = std::clamp(std::atoi(args[4].c_str()), 0, 600);
            if (args.size() > 5)
                k.bone = args[5] == "upper" ? studio::FocusBone::UpperBody
                         : args[5] == "center" ? studio::FocusBone::Center
                                               : studio::FocusBone::Head;
            std::vector<studio::TrackState> before{studio::CaptureTrack(d, -1, studio::RowKind::Focus, "")};
            studio::UpsertKey(d.camera.focus, k);
            StudioPushTrackEdit("focus key", before);
            d.rowsKey = ~0ull;
        }
    } else if (cmd == "studiolightkey") {  // studiolightkey <index> [frame]
        if (studio_ && !args.empty()) {
            const int idx = std::atoi(args[0].c_str());
            if (idx >= 0 && idx < (int)studio_->lights.size()) {
                const int frame = args.size() > 1 ? std::atoi(args[1].c_str()) : studio_->Frame();
                StudioInsertKeys({studio::MakeRowId(studio::RowKind::SceneLight, 0, studio_->lights[(size_t)idx].uid)}, frame);
            }
        }
    } else if (cmd == "studiolightset") {  // studiolightset <index> <field> <args...>
        if (studio_ && args.size() >= 3) {
            const int idx = std::atoi(args[0].c_str());
            if (idx >= 0 && idx < (int)studio_->lights.size()) {
                studio::SceneLight* light = &studio_->lights[(size_t)idx];
                const std::string f = ToLowerAscii(args[1]);
                const int frame = studio_->Frame();

                const bool isKeyable = (f == "pos" || f == "position" || f == "aim" || f == "dir" ||
                                        f == "direction" || f == "col" || f == "color" || f == "intensity" ||
                                        f == "range" || f == "coneouter" || f == "coneinner" || f == "size");

                if (isKeyable) {
                    studio::LightKey* k = studio::FindKey(light->keys, frame);
                    const bool willKey = (k != nullptr) || studio_->autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studio::LightValues& target = willKey ? (k ? k->v : [&]() -> studio::LightValues& {
                        studio::LightValues val = studio::SampleLightValues(*light, frame);
                        studio::UpsertKey(light->keys, studio::LightKey{frame, val});
                        return studio::FindKey(light->keys, frame)->v;
                    }()) : light->v;

                    if (f == "pos" || f == "position") target.position = {num(2), num(3), num(4)};
                    else if (f == "aim") target.aim = {num(2), num(3), num(4)};
                    else if (f == "dir" || f == "direction") target.direction = {num(2), num(3), num(4)};
                    else if (f == "col" || f == "color") target.color = {num(2), num(3), num(4)};
                    else if (f == "intensity") target.intensity = num(2);
                    else if (f == "range") target.range = num(2);
                    else if (f == "size") target.size = {num(2), num(3)};
                    else if (f == "coneouter") {
                        float v = num(2);
                        if (v > 1.6f) v = DirectX::XMConvertToRadians(v);
                        target.coneOuter = v;
                        if (target.coneInner > target.coneOuter) target.coneInner = target.coneOuter;
                    } else if (f == "coneinner") {
                        float v = num(2);
                        if (v > 1.6f) v = DirectX::XMConvertToRadians(v);
                        target.coneInner = v;
                        if (target.coneInner > target.coneOuter) target.coneOuter = target.coneInner;
                    }

                    studioLightChanged_ = true;
                    studio_->rowsKey = ~0ull;
                    ++studio_->projectVersion;
                    StudioEndLightEdit(light->uid);
                } else {
                    StudioBeginLightEdit(light->uid, false);
                    if (f == "name") light->name = args[2];
                    else if (f == "enabled") light->enabled = (args[2] == "1" || args[2] == "true" || args[2] == "on");
                    else if (f == "vmdlink") light->vmdLink = (args[2] == "1" || args[2] == "true" || args[2] == "on");
                    else if (f == "rimstrength") light->rimStrength = num(2);
                    else if (f == "rimcolor") light->rimColor = {num(2), num(3), num(4)};
                    else if (f == "aimmode") studio::ParseAimMode(args[2], light->aimMode);
                    else if (f == "target") {
                        const int ti = std::atoi(args[2].c_str());
                        light->targetUid = (ti >= 0 && ti < (int)studio_->models.size()) ? studio_->models[(size_t)ti]->uid : 0;
                    }
                    else if (f == "targetpart") studio::ParseTargetPart(args[2], light->targetPart);
                    else if (f == "swayphase") light->swayPhase = num(2);
                    else if (f == "skyzenith") light->skyZenith = {num(2), num(3), num(4)};
                    else if (f == "skyhorizon") light->skyHorizon = {num(2), num(3), num(4)};
                    else if (f == "groundcolor") light->groundColor = {num(2), num(3), num(4)};
                    else if (f == "shadow" || f == "shadowtype") studio::ParseShadowType(args[2], light->shadow);
                    else if (f == "shadowsoftness" || f == "softness") light->shadowSoftness = num(2);
                    else if (f == "shadowdensity" || f == "density") light->shadowDensity = num(2);
                    else if (f == "shadowcolor" || f == "shadowcolour") light->shadowColor = {num(2), num(3), num(4)};
                    else if (f == "falloff") studio::ParseFalloffType(args[2], light->falloff);
                    else if (f == "diffuse" || f == "affectdiffuse") light->affectDiffuse = (args[2] == "1" || args[2] == "true" || args[2] == "on");
                    else if (f == "specular" || f == "affectspecular") light->affectSpecular = (args[2] == "1" || args[2] == "true" || args[2] == "on");
                    else if (f == "visible" || f == "viewportvisible") light->viewportVisible = (args[2] == "1" || args[2] == "true" || args[2] == "on");

                    studioLightChanged_ = true;
                    studio_->rowsKey = ~0ull;
                    ++studio_->projectVersion;
                    StudioEndLightEdit(light->uid);
                }
            }
        }
    } else if (cmd == "studiolightgizmo") {  // studiolightgizmo <part> <dx> <dy> [steps]
        ImVec2 pt;
        if (!studio_ || args.empty() || !StudioScriptLightGizmoPoint(args[0], pt)) {
            LOG_WARN("ui script: light gizmo part '%s' not shown", args.empty() ? "" : args[0].c_str());
            if (outError) *outError = "Light gizmo part not shown: " + (args.empty() ? "" : args[0]);
            return false;
        } else {
            const int steps = std::max(1, args.size() > 3 ? std::atoi(args[3].c_str()) : 8);
            LOG_INFO("UISCRIPT lightgizmo %s at %.0f,%.0f", args[0].c_str(), pt.x, pt.y);
            uiScriptMouse_[0] = pt.x;
            uiScriptMouse_[1] = pt.y;
            io.AddMousePosEvent(pt.x, pt.y);
            sched(now + 1, "down", {"l"});
            for (int k = 1; k <= steps; ++k) {
                const float f = (float)k / steps;
                sched(now + 1 + k, "move", {std::to_string(pt.x + num(1) * f), std::to_string(pt.y + num(2) * f)});
            }
            sched(now + steps + 2, "up", {"l"});
        }
    } else if (cmd == "studiovpdimport") {
        if (studio_ && !args.empty()) StudioImportVpdFrom(Utf8ToPath(args[0]));
    } else if (cmd == "studiovpdexport") {
        if (studio_ && !args.empty()) StudioExportVpdTo(Utf8ToPath(args[0]));
    } else if (cmd == "studioimport") {
        if (studio_ && !args.empty()) StudioImportVmdFrom(Utf8ToPath(args[0]));
    } else if (cmd == "studioexport") {
        if (studio_ && !args.empty()) StudioExportVmdTo(Utf8ToPath(args[0]));
    } else if (cmd == "studionew") {
        StudioRunAction(StudioAction::New, {});
    } else if (cmd == "studiosave") {
        if (studio_ && !args.empty()) StudioSaveTo(Utf8ToPath(args[0]));
    } else if (cmd == "studioopen") {
        if (!args.empty()) StudioRunAction(StudioAction::OpenFile, std::filesystem::absolute(Utf8ToPath(args[0])));
    } else if (cmd == "studioautosave") {
        if (studio_) StudioAutosave(true, true);
    } else if (cmd == "studioadd") {  // studioadd <character|stage|prop> <file>
        studio::ModelKind kind;
        if (studio_ && args.size() >= 2 && studio::ParseModelKind(args[0], kind))
            StudioAddModelFile(kind, std::filesystem::absolute(Utf8ToPath(args[1])));
    } else if (cmd == "studioaddlib" || cmd == "studiosong") {  // studioaddlib <character|stage> <substr>
        const bool song = cmd == "studiosong";
        const std::string kind = song ? "song" : (args.empty() ? "" : args[0]);
        const std::string needle = ToLowerAscii(args.size() > (song ? 0u : 1u) ? args[song ? 0 : 1] : "");
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
    } else if (cmd == "studioaudio") {  // studioaudio <file|none> [offset seconds]
        if (studio_ && !args.empty()) {
            if (args.size() > 1) studio_->audioOffset = num(1);
            StudioSetAudio(args[0] == "none" ? std::filesystem::path() : std::filesystem::absolute(Utf8ToPath(args[0])));
        }
    } else if (cmd == "studioselect") {
        if (studio_ && !args.empty()) StudioSelectModel(std::atoi(args[0].c_str()));
    } else if (cmd == "studioreplace") {  // studioreplace <model index> <file>
        const int i = args.size() >= 2 ? std::atoi(args[0].c_str()) : -1;
        if (studio_ && i >= 0 && i < (int)studio_->models.size()) {
            const studio::StudioModel& m = *studio_->models[(size_t)i];
            StudioAddModelFile(m.kind, std::filesystem::absolute(Utf8ToPath(args[1])), m.uid);
        }
    } else if (cmd == "studioreplacelib") {  // studioreplacelib <model index> <substr> (library character / stage)
        const int i = args.size() >= 2 ? std::atoi(args[0].c_str()) : -1;
        const std::string needle = ToLowerAscii(args.size() >= 2 ? args[1] : "");
        if (studio_ && i >= 0 && i < (int)studio_->models.size() && !needle.empty()) {
            const studio::StudioModel& m = *studio_->models[(size_t)i];
            const auto find = [&](const auto& list) {
                for (size_t k = 0; k < list.size(); ++k)
                    if (ToLowerAscii(list[k].id).find(needle) != std::string::npos ||
                        ToLowerAscii(list[k].displayName).find(needle) != std::string::npos)
                        return (int)k;
                return -1;
            };
            if (m.IsStage()) StudioAddLibraryStage(find(library_.stages), m.uid);
            else if (!m.IsProp()) StudioAddLibraryCharacter(find(library_.characters), m.uid);
        }
    } else if (cmd == "studioremove") {
        if (studio_ && !args.empty()) StudioRemoveModel(std::atoi(args[0].c_str()));
    } else if (cmd == "studiorename") {
        if (studio_ && studio_->Selected() && !args.empty()) {
            studio_->Selected()->name = args[0];
            ++studio_->projectVersion;
        }
    } else if (cmd == "studioattach") {  // studioattach <parent index|-1> <bone|-> tx ty tz rx ry rz s
        studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
        if (m && m->IsProp() && args.size() >= 9) {
            const int pi = std::atoi(args[0].c_str());
            m->attach.parent = pi >= 0 && pi < (int)studio_->models.size() ? (int)studio_->models[(size_t)pi]->uid : -1;
            m->attach.bone = args[1] == "-" ? std::string() : args[1];
            m->attach.translation = {num(2), num(3), num(4)};
            m->attach.rotationDeg = {num(5), num(6), num(7)};
            m->attach.scale = num(8);
            ++studio_->projectVersion;
        }
    } else if (cmd == "log") {
        LOG_INFO("UISCRIPT %s", args.empty() ? "" : args[0].c_str());
    } else if (cmd == "updatecheck") {  // synchronous update-feed check, logs UPDATECHECK
        UpdateCheckCommand();
    } else if (cmd == "updateinstall") {  // stage the feed's update; the app exits when staged
        UpdateInstallCommand();
    } else {
        LOG_WARN("ui script: unknown command '%s'", cmd.c_str());
        if (outError) *outError = "Unknown command: " + cmd;
        return false;
    }
    return true;
}

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

    ImGuiIO& io = ImGui::GetIO();
    const int now = framesInScene_;
    // The Win32 backend feeds the real cursor position when it is outside the focused window: keep ours.
    if (uiScriptMouse_[0] > -1e29f) io.AddMousePosEvent(uiScriptMouse_[0], uiScriptMouse_[1]);
    auto sched = [this](int frame, std::string cmd, std::vector<std::string> args) {
        ScheduleUiScriptStep(frame, std::move(cmd), std::move(args));
    };
    while (uiScriptNext_ < uiScript_.size() && uiScript_[uiScriptNext_].frame <= now) {
        const UiScriptStep s = uiScript_[uiScriptNext_++];
        ExecuteUiCommand(s.cmd, s.args, sched);
    }
}

} // namespace mmdx
