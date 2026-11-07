// Scripted ImGui input for unattended UI tests (--ui-script) and MCP dispatcher.
// Events are queued right before ImGui::NewFrame, so widgets see them exactly like real mouse/keyboard input.
#include "app/App.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "anim/ModelInstance.h"
#include "app/Lighting.h"
#include "asset/RayMaterial.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "core/Timecode.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/SceneLight.h"
#include "studio/RestPoseRetarget.h"
#include "studio/StudioDoc.h"

namespace mmdx {

namespace {
// Studio ribbon tab ids, in header order (UiStudioRibbon.cpp kRibbonTabs): studioribbon / STUDIORIBBON use these.
const char* const kRibbonTabIds[] = {"home", "anim", "camera", "light", "render", "view"};

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
            // S1b: the selected model's MMDX stage variant ("-" = not an MMDX stage part, "<default>" = the manifest's
            // default variant; S3: the variant is the stage entity's).
            std::string stageVariant = "-";
            if (const studio::StudioStage* selStage = d.Selected() ? d.StageOfModel(*d.Selected()) : nullptr)
                stageVariant = selStage->variant.empty() ? "<default>" : selStage->variant;
            LOG_INFO("STUDIOSTATE frame=%d model=%d boneKeys=%zu morphKeys=%zu cameraKeys=%zu "
                     "lightKeys=%zu shadowKeys=%zu motionCam=%d shadowTrack=%d sceneLights=%zu "
                     "selected=%zu selection=[%s ] stageVariant=%s "
                     "rows=%zu range=%d..%d loop=%d playing=%d physics=%d undo='%s' redo='%s' undoCount=%zu undoMB=%.1f "
                     "scroll=%.1f zoom=%.2f hoveredWindow=%s activeId=%08x",
                     d.Frame(), d.selectedModel, bones, morphs, d.camera.camera.size(), d.camera.light.size(),
                     d.camera.shadow.size(), (int)d.useMotionCamera, (int)d.useShadowTrack, d.lights.size(),
                     d.selection.size(), sel.c_str(), stageVariant.c_str(),
                     d.selectedRows.size(), d.view.rangeStart, d.view.rangeEnd, (int)d.loop, (int)d.playing,
                     (int)d.physics, d.history.UndoName().c_str(), d.history.RedoName().c_str(), d.history.Count(),
                     d.history.Bytes() / 1048576.0, d.view.scrollFrame, d.view.pxPerFrame,
                     GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "-", (unsigned)GImGui->ActiveId);
            LOG_INFO("STUDIORIBBON tab=%s collapsed=%d", kRibbonTabIds[std::clamp(studioRibbonTab_, 0, 5)],
                      (int)studioRibbonCollapsed_);
            LOG_INFO("STUDIORESTPOSE open=%d auto=%d model=%d median=%.1f", (int)studioRestPoseOpen_,
                     (int)studioRestPoseAuto_,
                     studioRestPoseModel_ < 0 ? -1 : d.IndexOfUid((uint32_t)studioRestPoseModel_),
                     studioRestPoseGuess_.medianDeg);
            LOG_INFO("STUDIOFOCUS keys=%zu distance=%.2f aperture=%.2f subject=%u subjectName=%s dof=%d", d.camera.focus.size(),
                     studioFocusShown_, studioFocusAperture_, studioFocusSubject_,
                     StudioModelLabel(studioFocusSubject_).c_str(), (int)settings_.dof);
            {
                const OfflineProgress& gi = renderer_.OfflineStatus();
                LOG_INFO("STUDIOGI renderer=%d active=%d phase=%d samples=%u w=%u h=%u",
                         std::clamp(settings_.studioRenderer, 0, 3), (int)StudioGiActive(), (int)gi.phase,
                         gi.samples, gi.width, gi.height);
            }
            // S5: one STUDIOSCREEN line per stage screen (stage uid, id, source, file / camera, w, h, brightness)
            for (const studio::StudioStage& st : d.stages) {
                if (!st.manifest) continue;
                const studio::StudioStageOverrides ov = studio::StageOverridesAt(st, (float)d.Frame());
                const StageEvalResult eval =
                    EvaluateStage(*st.manifest, st.variant, ov.values, d.time, ov.skipCues);
                for (const StageScreenState& s : StudioStageScreenStates(st, eval)) {
                    const char* detail = s.source == StageScreenSource::Camera ? s.camera.c_str() : s.file.c_str();
                    LOG_INFO("STUDIOSCREEN stage=%u id=%s source=%s detail=%s w=%u h=%u brightness=%.3f",
                             st.uid, s.id.c_str(), StageScreenSourceName(s.source), detail, s.width, s.height,
                             s.brightness);
                }
            }
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
            LOG_INFO("STUDIOPOSE bone=%s %s selBones=%zu pose=%zu/%zu@%d tool=%d local=%d autoIk=%d tab=%d gizmo=%d viewDrag=%d",
                     bone.c_str(), value.c_str(), d.selectedBones.size(), poseBones, poseMorphs, poseFrame, d.gizmoTool,
                     (int)d.gizmoLocal, (int)d.autoIk, d.inspectorTab, (int)studioGizmoShown_, studioViewDrag_);
            // the bones the pose layer currently overrides (auto-IK writes 2-3 of them per drag)
            if (m && !m->pose.bones.empty()) {
                std::string changed;
                for (const auto& [b, v] : m->pose.bones) {
                    (void)v;
                    if (b >= 0 && b < (int)m->pmx->bones.size()) {
                        changed += changed.empty() ? "" : ",";
                        changed += m->pmx->bones[(size_t)b].name;
                    }
                }
                LOG_INFO("STUDIOPOSEBONES %s", changed.c_str());
            }
            // S1b: the selected model's MMDX stage variant (not an MMDX stage part = "-"; the stage entity's variant,
            // "<default>" when the manifest's default drives it)
            if (m) {
                const studio::StudioStage* selStage = d.StageOfModel(*m);
                const std::string variant = selStage
                                                ? (selStage->variant.empty() ? std::string("<default>") : selStage->variant)
                                                : std::string("-");
                // S2a: the rig size is the manifest's lights plus the part's glTF ones ("gltfLights": true).
                const int gltfLights = m->stageManifest && m->stageManifest->gltfLights && m->pmx
                                           ? (int)m->pmx->importedLights.size()
                                           : 0;
                LOG_INFO("STUDIOSTAGE stage=%s part=%d uid=%u variant=%s lights=%d gltfLights=%d",
                         m->stageManifest ? m->stageManifest->id.c_str() : "-", m->stagePart, m->stageUid,
                         variant.c_str(),
                         (m->stageManifest ? (int)m->stageManifest->lights.size() : 0) + gltfLights, gltfLights);
            }
            // S3: one STUDIOSTAGE line per stage entity (uid, library id, variant, base / track / cuesOff counts)
            for (const studio::StudioStage& st : d.stages)
                LOG_INFO("STUDIOSTAGE uid=%u id=%s variant=%s base=%zu tracks=%zu cuesOff=%zu",
                         st.uid, st.stageId.c_str(),
                         st.variant.empty() ? std::string("<default>").c_str() : st.variant.c_str(),
                         st.base.size(), st.tracks.size(), st.cuesOff.size());
            // S4: one STUDIOCAM line per camera (uid 0 = the main camera)
            LOG_INFO("STUDIOCAM uid=%u name=%s keys=%zu visible=%d stageUid=%u%s", 0u, "main",
                     d.camera.camera.size(), 1, 0u, d.selectedCameraUid == 0 ? " selected" : "");
            for (const studio::StudioCamera& cam : d.cameras)
                LOG_INFO("STUDIOCAM uid=%u name=%s keys=%zu visible=%d stageUid=%u%s", cam.uid, cam.name.c_str(),
                         cam.track.camera.size(), (int)cam.visible, cam.stageUid,
                         d.selectedCameraUid == cam.uid ? " selected" : "");
            {   // the project's gravity / wind (+ the undo step on top, for edit checks)
                const studio::PhysicsEnv& pe = d.physicsEnv;
                LOG_INFO("STUDIOPHYSICS physics=%d gravity=%.3f dir=(%.3f,%.3f,%.3f) wind=%.3f gust=%.3f freq=%.3f "
                         "jitter=%.3f windDir=(%.3f,%.3f) undo='%s'",
                         (int)d.physics, pe.gravity, pe.gravityDir.x, pe.gravityDir.y, pe.gravityDir.z, pe.windStrength,
                         pe.windGust, pe.windGustFreq, pe.windJitter, pe.windDir.x, pe.windDir.z,
                         d.history.UndoName().c_str());
            }
            // P2.2: the selected model's physics bake state (driven bones + baked key count + the off flag)
            if (m) {
                size_t bakedKeys = 0;
                const std::vector<int> driven = studio::PhysicsDrivenBones(*m->pmx);
                for (int b : driven) {
                    if (b < 0 || b >= (int)m->pmx->bones.size()) continue;
                    auto it = m->motion.bones.find(m->pmx->bones[(size_t)b].name);
                    if (it != m->motion.bones.end()) bakedKeys += it->second.size();
                }
                LOG_INFO("STUDIOBAKE bones=%zu keys=%zu physicsOff=%d", driven.size(), bakedKeys, (int)m->physicsOff);
            }
            // project + every model (save / reopen comparisons, prop following)
            LOG_INFO("STUDIOPROJ path=%s dirty=%d aspect=%s models=%zu jobs=%zu audio=%s offset=%.3f audioLocked=%d end=%d camKeys=%zu "
                     "lightKeys=%zu shadowKeys=%zu bgvideo=%s bgoffset=%.3f bgasbg=%d",
                     PathToUtf8(d.projectPath.filename()).c_str(), (int)d.Dirty(), d.AspectLabel(), d.models.size(), studioJobs_.size(),
                     d.hasAudio ? PathToUtf8(d.audioPath.filename()).c_str() : "-", d.audioOffset, (int)d.audioLocked, d.EndFrame(),
                     d.camera.camera.size(), d.camera.light.size(), d.camera.shadow.size(),
                     d.bgVideoPath.empty() ? "-" : PathToUtf8(d.bgVideoPath.filename()).c_str(), d.bgVideoOffset,
                     (int)d.bgVideoAsBackground);
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
                         "r=(%.2f,%.2f,%.2f) s=%.3f pos=(%.3f,%.3f,%.3f) material=%s emd=%s file=%s armGuess=%.1f",
                         i, studio::ModelKindName(mm.kind), mm.name.c_str(), (int)mm.visible, keys, parent,
                         mm.attach.bone.empty() ? "-" : mm.attach.bone.c_str(), mm.attach.translation.x,
                         mm.attach.translation.y, mm.attach.translation.z, mm.attach.rotationDeg.x,
                         mm.attach.rotationDeg.y, mm.attach.rotationDeg.z, mm.attach.scale, pos.x, pos.y, pos.z,
                         mm.material.ray ? "ray" : "default", mm.material.emd.empty() ? "-" : mm.material.emd.c_str(),
                         PathToUtf8(mm.path.filename()).c_str(), studio::GuessTPoseMotion(mm.motion).medianDeg);
                if (mm.IsProp()) {  // the accessory key track: the evaluated placement at the playhead
                    const float af = (float)(d.time * kMmdFps);
                    const studio::AccessoryKf ak =
                        mm.motion.accessory.empty() ? studio::AccessoryKf{} : studio::SampleAccessory(mm.motion.accessory, af);
                    const studio::PropAttach ap = studio::AccessoryPlacement(mm, af);
                    LOG_INFO("STUDIOACC %zu name=%s keys=%zu frame=%d t=(%.3f,%.3f,%.3f) r=(%.2f,%.2f,%.2f) s=%.3f "
                             "opacity=%.3f visible=%d parent=%d bone=%s",
                             i, mm.name.c_str(), mm.motion.accessory.size(), d.Frame(), ap.translation.x, ap.translation.y,
                             ap.translation.z, ap.rotationDeg.x, ap.rotationDeg.y, ap.rotationDeg.z, ap.scale,
                             studio::AccessoryOpacity(mm, af), (int)studio::AccessoryVisible(mm, af),
                             ap.parent, ap.bone.empty() ? "-" : ap.bone.c_str());
                }
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
                             "intensity=%.3f range=%.1f size=(%.1f,%.1f) volumetric=%.2f volumetricG=%.2f keys=%zu visible=%d",
                             i, l.uid, studio::LightKindName(l.kind), l.name.c_str(), (int)l.enabled,
                             cur.position.x, cur.position.y, cur.position.z,
                             aim.x, aim.y, aim.z,
                             cur.direction.x, cur.direction.y, cur.direction.z,
                             cur.color.x, cur.color.y, cur.color.z,
                             cur.intensity, cur.range,
                             cur.size.x, cur.size.y,
                             cur.volumetric, l.volumetricG,
                             l.keys.size(), (int)l.viewportVisible);
                } else if (l.kind == studio::LightKind::FogVolume) {
                    // S9: a local medium volume (position / extent / density / tint / shape / edge; it lights nothing).
                    LOG_INFO("STUDIOLIGHT %zu uid=%u kind=%s name='%s' enabled=%d pos=(%.3f,%.3f,%.3f) "
                             "extent=(%.1f,%.1f,%.1f) density=%.3f col=(%.3f,%.3f,%.3f) shape=%s edge=%.2f "
                             "volumetricG=%.2f keys=%zu visible=%d",
                             i, l.uid, studio::LightKindName(l.kind), l.name.c_str(), (int)l.enabled,
                             cur.position.x, cur.position.y, cur.position.z,
                             cur.extent.x, cur.extent.y, cur.extent.z,
                             cur.intensity, cur.color.x, cur.color.y, cur.color.z,
                             studio::FogShapeName(l.fogShape), l.fogEdge, l.volumetricG,
                             l.keys.size(), (int)l.viewportVisible);
                } else {
                    LOG_INFO("STUDIOLIGHT %zu uid=%u kind=%s name='%s' enabled=%d pos=(%.3f,%.3f,%.3f) "
                             "aim=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f) col=(%.3f,%.3f,%.3f) "
                             "intensity=%.3f range=%.1f coneOuter=%.4f (%.1f deg) coneInner=%.4f (%.1f deg) "
                             "volumetric=%.2f volumetricG=%.2f keys=%zu visible=%d",
                             i, l.uid, studio::LightKindName(l.kind), l.name.c_str(), (int)l.enabled,
                             cur.position.x, cur.position.y, cur.position.z,
                             aim.x, aim.y, aim.z,
                             cur.direction.x, cur.direction.y, cur.direction.z,
                             cur.color.x, cur.color.y, cur.color.z,
                             cur.intensity, cur.range,
                             cur.coneOuter, DirectX::XMConvertToDegrees(cur.coneOuter),
                             cur.coneInner, DirectX::XMConvertToDegrees(cur.coneInner),
                             cur.volumetric, l.volumetricG,
                             l.keys.size(), (int)l.viewportVisible);
                }
            }
            // M6b: the per-model MME choice + the project's MME post stack (studio E2E comparisons).
            for (size_t i = 0; i < d.models.size(); ++i) {
                const studio::StudioModel& mm = *d.models[i];
                const char* mode = mm.mme.mode == MmeChoice::Auto  ? "auto"
                                   : mm.mme.mode == MmeChoice::File ? "file"
                                                                    : "none";
                LOG_INFO("STUDIOMME %zu mode=%s file=%s", i, mode,
                         mm.mme.file.empty() ? "-" : PathToUtf8(mm.mme.file).c_str());
            }
            for (size_t i = 0; i < d.mmePost.size(); ++i)
                LOG_INFO("STUDIOMMEPOST %zu enabled=%d path=%s", i, (int)d.mmePost[i].enabled,
                         PathToUtf8(d.mmePost[i].path).c_str());
            for (size_t i = 0; i < d.effects.size(); ++i) {
                const studio::StudioEffect& e = d.effects[i];
                size_t keyCount = 0;
                for (const auto& t : e.tracks) keyCount += t.keys.size();
                EffectStackEntry evaluated = studio::EvaluateEffect(e, (float)d.Frame());
                std::string vals;
                bool firstVal = true;
                for (const auto& [k, v] : evaluated.params) {
                    if (!firstVal) vals += ",";
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%s=%.2f", k.c_str(), v);
                    vals += buf;
                    firstVal = false;
                }
                LOG_INFO("STUDIOEFFECT %zu pack=%s enabled=%d keys=%zu values=%s",
                         i, e.entry.pack.c_str(), (int)e.entry.enabled, keyCount, vals.c_str());
            }
            for (size_t i = 0; i < d.worlds.size(); ++i) {
                const studio::StudioWorld& w = d.worlds[i];
                size_t keyCount = 0;
                for (const auto& t : w.fx.tracks) keyCount += t.keys.size();
                LOG_INFO("STUDIOWORLD %zu pack=%s enabled=%d keys=%zu place=(%.3f,%.3f,%.3f) (%.2f,%.2f,%.2f) %.3f",
                         i, w.fx.entry.pack.c_str(), (int)w.fx.entry.enabled, keyCount,
                         w.place.translation.x, w.place.translation.y, w.place.translation.z,
                         w.place.rotationDeg.x, w.place.rotationDeg.y, w.place.rotationDeg.z, w.place.scale);
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
    } else if (cmd == "studioautoik") {  // studioautoik <0|1>: auto-IK posing toggle
        if (studio_ && !args.empty()) studio_->autoIk = std::atoi(args[0].c_str()) != 0;
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
    } else if (cmd == "studioaspect") {  // studioaspect <w:h>
        if (studio_ && !args.empty()) {
            int idx = studio::AspectIndexFromLabel(args[0]);
            if (idx >= 0) {
                studio_->SetAspectPreset(idx);
            } else {
                size_t colon = args[0].find(':');
                if (colon != std::string::npos) {
                    int w = std::atoi(args[0].substr(0, colon).c_str());
                    int h = std::atoi(args[0].substr(colon + 1).c_str());
                    studio_->SetAspect(w, h);
                } else {
                    LOG_WARN("ui script: unknown aspect ratio '%s'", args[0].c_str());
                    if (outError) *outError = "Unknown aspect ratio: " + args[0];
                    return false;
                }
            }
        }
    } else if (cmd == "studiorenderer") {  // studiorenderer <raster|rt|pt|gi>
        if (!args.empty()) {
            const std::string v = ToLowerAscii(args[0]);
            int r = -1;
            if (v == "raster") r = 0;
            else if (v == "rt") r = 1;
            else if (v == "pt") r = 2;
            else if (v == "gi") r = 3;
            if (r >= 0) {
                StudioSetViewportRenderer(r);
            } else {
                LOG_WARN("ui script: unknown studio renderer '%s'", args[0].c_str());
                if (outError) *outError = "Unknown studio renderer: " + args[0];
                return false;
            }
        }
    } else if (cmd == "studioribbon") {  // studioribbon <home|anim|camera|light|render|view> | collapse <0|1>
        if (!args.empty() && ToLowerAscii(args[0]) == "collapse") {
            studioRibbonCollapsed_ = args.size() > 1 && std::atoi(args[1].c_str()) != 0;
        } else if (!args.empty()) {
            const std::string v = ToLowerAscii(args[0]);
            int t = -1;
            for (int i = 0; i < 6; ++i)
                if (v == kRibbonTabIds[i]) t = i;
            if (t >= 0) {
                studioRibbonTab_ = t;
                studioRibbonCollapsed_ = false;
            } else {
                LOG_WARN("ui script: unknown ribbon tab '%s'", args[0].c_str());
                if (outError) *outError = "Unknown ribbon tab: " + args[0];
                return false;
            }
        }
    } else if (cmd == "studiolightpreset") {  // studiolightpreset <0-3>
        if (studio_ && !args.empty()) {
            const int p = std::clamp(std::atoi(args[0].c_str()), 0, 3);
            StudioApplyLightPreset(p);
        }
    } else if (cmd == "studiolightadd") {  // studiolightadd <sun|point|spot|area|ambient|fog_volume|sky>
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
    } else if (cmd == "studioscreen") {  // studioscreen <stage index> <screen id> <none|image|video|camera> [file|camera]
        const studio::StudioStage* st = nullptr;
        if (studio_ && args.size() >= 3) {
            const int idx = std::atoi(args[0].c_str());
            if (idx >= 0 && idx < (int)studio_->stages.size()) st = &studio_->stages[(size_t)idx];
        }
        StageScreenSource src = StageScreenSource::None;
        if (!st || !ParseStageScreenSource(ToLowerAscii(args[2]), src)) {
            LOG_WARN("ui script: studioscreen needs a stage index and a known source");
            if (outError) *outError = "studioscreen: unknown stage or source";
            return false;
        }
        studio::StageScreenOverride ov;
        ov.source = std::string(StageScreenSourceName(src));
        if (args.size() >= 4 && !args[3].empty()) {
            if (src == StageScreenSource::Camera) ov.camera = args[3];
            else ov.file = PathToUtf8(std::filesystem::path(Utf8ToPath(args[3])).is_absolute()
                                          ? Utf8ToPath(args[3])
                                          : (st->manifest ? st->manifest->dir / Utf8ToPath(args[3]) : Utf8ToPath(args[3])));
        }
        StudioSetScreenOverride(st->uid, args[1], ov, Tr("스크린 소스"));
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
                                        f == "range" || f == "coneouter" || f == "coneinner" || f == "size" ||
                                        f == "extent" || f == "volumetric");

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
                    else if (f == "extent") target.extent = {num(2), num(3), num(4)};
                    else if (f == "volumetric") target.volumetric = std::max(0.0f, num(2));
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
                    else if (f == "volumetricg" || f == "anisotropy") light->volumetricG = std::clamp(num(2), -0.9f, 0.95f);
                    else if (f == "fogshape" || f == "shape") studio::ParseFogShape(ToLowerAscii(args[2]), light->fogShape);
                    else if (f == "fogedge" || f == "edge") light->fogEdge = std::clamp(num(2), 0.0f, 1.0f);
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
                    // S7b: the spot gobo (file + its four values)
                    else if (f == "gobo") light->gobo = args[2];
                    else if (f == "goborotation") light->goboRotation = num(2);
                    else if (f == "goboscale") light->goboScale = std::max(0.01f, num(2));
                    else if (f == "gobospin") light->goboSpin = num(2);
                    else if (f == "gobofocus") light->goboFocus = std::clamp(num(2), 0.0f, 1.0f);
                    // S9b-3: the sky light's image (the file + its two values; encoding / mirror stay base values)
                    else if (f == "skymap") light->skyMap = args[2];
                    else if (f == "skymaprot") light->v.skyMapRotationDeg = num(2);
                    else if (f == "skymapint") light->v.skyMapIntensity = std::max(0.0f, num(2));
                    // S9b-4: the cloud layer on the physical sky (cloudwind takes direction and speed in one command)
                    else if (f == "cloudcov") light->v.cloudCoverage = std::clamp(num(2), 0.0f, 1.0f);
                    else if (f == "cloudwind") {
                        light->v.cloudWindDeg = std::clamp(num(2), -360.0f, 360.0f);
                        light->v.cloudWindSpeed = std::clamp(num(3), 0.0f, 100.0f);
                    }

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
    } else if (cmd == "drop") {  // drop <file>[|<file>...]: inject a drag-and-drop path list
        std::vector<std::filesystem::path> files;
        for (const std::string& a : args)
            if (!a.empty()) files.push_back(std::filesystem::absolute(Utf8ToPath(a)));
        if (!files.empty()) ApplyStudioDrop(std::move(files));
        else LOG_WARN("ui script: drop with no files");
    } else if (cmd == "studioimport") {
        if (studio_ && !args.empty()) {  // the path may contain spaces
            std::string path = args[0];
            for (size_t i = 1; i < args.size(); ++i) path += " " + args[i];
            StudioImportVmdFrom(Utf8ToPath(path));
        }
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
    } else if (cmd == "studiommeobject") {  // studiommeobject <path.x|path.fx> [label...]
        // M5c: adds an MME effect object (an accessory `.x` + its same-stem `.fx`, or a fee `.fx`).
        if (studio_ && !args.empty()) {
            std::string path = args[0];
            for (size_t i = 1; i < args.size(); ++i) path += " " + args[i];
            const std::filesystem::path abs = std::filesystem::absolute(Utf8ToPath(path));
            // scripts cannot answer a modal: a Ray-MMD main effect is added with a warning
            if (IsRayMmdMainEffect(abs))
                LOG_WARN("Ray-MMD main effect added as MME (forced by script)");
            StudioAddMmeObject(abs);
        }
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
    } else if (cmd == "studioaudiolock") {  // studioaudiolock 0|1
        if (!args.empty() && (args[0] == "0" || args[0] == "1")) { if (studio_) StudioSetAudioLocked(args[0] == "1"); }
        else LOG_WARN("ui script: studioaudiolock expects 0 or 1, got '%s'", args.empty() ? "" : args[0].c_str());
    } else if (cmd == "studiobgvideo") {  // studiobgvideo <file|none>: the WHOLE rest of the line is the path
        if (studio_) {
            std::string path;
            for (size_t i = 0; i < args.size(); ++i) { if (i) path += " "; path += args[i]; }
            StudioSetBgVideo(path.empty() || path == "none" ? std::filesystem::path()
                                                            : std::filesystem::absolute(Utf8ToPath(path)));
        }
    } else if (cmd == "studiobgvideoopt") {  // studiobgvideoopt <offsetSeconds> <asBackground 0|1>
        if (studio_ && args.size() >= 2) {
            studio_->bgVideoOffset = num(0);
            studio_->bgVideoAsBackground = args[1] != "0" && args[1] != "false" && args[1] != "off";
            ++studio_->projectVersion;
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
    } else if (cmd == "studioattachkeep") {  // studioattachkeep <parent index|-1> <bone|->: re-parent, stay in place
        studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
        if (m && m->IsProp() && args.size() >= 2) {
            const int pi = std::atoi(args[0].c_str());
            studio::PropAttach to = m->attach;
            to.parent = pi >= 0 && pi < (int)studio_->models.size() ? (int)studio_->models[(size_t)pi]->uid : -1;
            to.bone = args[1] == "-" || to.parent < 0 ? std::string() : args[1];
            m->attach = StudioKeepWorldAttach(*m, m->attach, to);
            ++studio_->projectVersion;
        }
    } else if (cmd == "studioacckey") {  // studioacckey <frame> tx ty tz rx ry rz s [opacity] [visible 0|1]
        studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
        if (m && m->IsProp() && args.size() >= 8) {
            studio::AccessoryKf k;
            k.frame = std::atoi(args[0].c_str());
            k.translation = {num(1), num(2), num(3)};
            k.rotationDeg = {num(4), num(5), num(6)};
            k.scale = num(7);
            if (args.size() > 8) k.opacity = num(8);
            if (args.size() > 9) k.visible = (args[9] == "1" || args[9] == "true" || args[9] == "on");
            // keep the current parent / bone (editable through the inspector) unless a key already set one
            if (const studio::AccessoryKf* cur = studio::FindKey(m->motion.accessory, k.frame)) {
                k.parentUid = cur->parentUid;
                k.bone = cur->bone;
            } else {
                const studio::PropAttach place = studio::AccessoryPlacement(*m, (float)k.frame);
                k.parentUid = place.parent < 0 ? 0 : (uint32_t)place.parent;
                k.bone = place.bone;
            }
            std::vector<studio::TrackState> before{studio::CaptureTrack(*studio_, studio_->selectedModel, studio::RowKind::Accessory, "")};
            studio::UpsertKey(m->motion.accessory, k);
            StudioPushTrackEdit(Tr("소품 키"), before);
            studio_->selection = {{studio::MakeRowId(studio::RowKind::Accessory, 0, 0), k.frame}};
            studio_->rowsKey = ~0ull;
        }
    } else if (cmd == "studiobake") {  // studiobake <startFrame> <endFrame> [step]: bake the selected model's physics
        if (studio_ && args.size() >= 2) {
            const int a = std::max(0, std::atoi(args[0].c_str()));
            const int b = std::max(a, std::atoi(args[1].c_str()));
            const int step = args.size() > 2 ? std::max(1, std::atoi(args[2].c_str())) : 1;
            if (StudioStartBake(a, b, step, false, true)) {
                while (studioBakeRunning_) {  // scripts may block until the bake is done
                    StudioPollBake();
                    if (studioBakeRunning_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } else {
                LOG_WARN("ui script: studiobake could not start");
                if (outError) *outError = "Bake could not start";
                return false;
            }
        }
    } else if (cmd == "studiogravity" || cmd == "studiowind") {
        // studiogravity <acceleration> [x y z] / studiowind <strength> [gust frequency jitter [x z]]: the physics
        // popup's undoable edit (studiostate logs STUDIOPHYSICS)
        if (studio_ && !args.empty()) {
            studio::PhysicsEnv after = studio_->physicsEnv;
            if (cmd == "studiogravity") {
                after.gravity = num(0);
                if (args.size() >= 4) after.gravityDir = {num(1), num(2), num(3)};
            } else {
                after.windStrength = num(0);
                if (args.size() >= 4) { after.windGust = num(1); after.windGustFreq = num(2); after.windJitter = num(3); }
                if (args.size() >= 6) after.windDir = {num(4), 0.0f, num(5)};
            }
            after.Sanitize();
            if (!(after == studio_->physicsEnv))
                studio_->history.Push(std::make_unique<studio::PhysicsEnvCommand>(
                    *studio_, cmd == "studiogravity" ? Tr("중력 변경") : Tr("바람 변경"), studio_->physicsEnv, after));
        }
    } else if (cmd == "studiorestpose") {  // studiorestpose <deg> [both|left|right]: selected model's arm keys
        if (studio_) {
            const float deg = args.empty() ? 0.0f : (float)std::atof(args[0].c_str());
            const std::string side = args.size() > 1 ? ToLowerAscii(args[1]) : "both";
            if (!StudioApplyRestPose(deg, side != "right", side != "left")) {
                LOG_WARN("ui script: studiorestpose applied nothing");
                if (outError) *outError = "Nothing to correct";
                return false;
            }
        }
    } else if (cmd == "studiorestposeanswer") {  // studiorestposeanswer <accept|keep> [deg]: the auto modal's button
        if (studio_ && studioRestPoseOpen_ && studioRestPoseAuto_) {
            if (!args.empty() && args[0] != "accept" && args[0] != "keep") {
                LOG_WARN("ui script: studiorestposeanswer expects accept|keep");
            } else {
                if (args.size() > 1) studioRestPoseSourceDeg_ = (float)std::atof(args[1].c_str());
                studioRestPoseAnswer_ = args.empty() || args[0] == "accept" ? 1 : 2;
            }
        } else {
            LOG_WARN("ui script: studiorestposeanswer without an open auto modal");
            if (outError) *outError = "No rest-pose modal is open";
            return false;
        }
    } else if (cmd == "studiomaterial") {  // studiomaterial <default|ray> [emd path] (selected studio model)
        studio::StudioModel* m = studio_ ? studio_->Selected() : nullptr;
        if (m && !args.empty()) {
            MaterialChoice c;
            c.ray = args[0] != "default";
            if (c.ray && args.size() > 1 && !args[1].empty() && args[1] != "-")
                c.emd = PathToUtf8(std::filesystem::absolute(Utf8ToPath(args[1])));
            m->material = c;
            if (!m->libraryId.empty()) {
                settings_.SetCharacterMaterial(m->libraryId, c);
                settings_.Save(settingsPath_);
            }
            StudioAddModelFile(m->kind, m->path, m->uid, &m->material);   // reload this model's PBR data
            ++studio_->projectVersion;
        }
    } else if (cmd == "studiomme") {  // studiomme <model index> <none|auto|file> [path]
        if (studio_ && args.size() >= 2) {
            const int mi = std::atoi(args[0].c_str());
            if (mi >= 0 && mi < (int)studio_->models.size()) {
                studio::StudioModel* m = studio_->models[(size_t)mi].get();
                MmeChoice c;
                if (args[1] == "auto") c.mode = MmeChoice::Auto;
                else if (args[1] == "file") {
                    c.mode = MmeChoice::File;
                    if (args.size() > 2 && !args[2].empty() && args[2] != "-")
                        c.file = std::filesystem::absolute(Utf8ToPath(args[2]));
                }
                m->mme = c;
                ++studio_->projectVersion;
            }
        }
    } else if (cmd == "studiommepost") {  // studiommepost <add|remove> <path>
        if (studio_ && args.size() >= 2) {
            const std::string action = ToLowerAscii(args[0]);
            if (action == "add") {
                const std::filesystem::path p = std::filesystem::absolute(Utf8ToPath(args[1]));
                bool dup = false;
                for (const studio::MmePostEntry& e : studio_->mmePost) dup |= e.path == p;
                if (!dup) studio_->mmePost.push_back(studio::MmePostEntry{p, true});
                StudioMmeChanged();
            } else if (action == "remove") {
                const std::filesystem::path p = std::filesystem::absolute(Utf8ToPath(args[1]));
                auto& v = studio_->mmePost;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const studio::MmePostEntry& e) { return e.path == p; }),
                        v.end());
                StudioMmeChanged();
            }
        }
    } else if (cmd == "studioeffect") {  // studioeffect <add <pack id> | remove <index> | enable <index> <0|1>>
        if (studio_ && !args.empty()) {
            const std::string action = ToLowerAscii(args[0]);
            if (action == "add" && args.size() >= 2) {
                std::vector<studio::StudioEffect> before = studio_->effects;
                studio::StudioEffect eff;
                eff.uid = studio_->nextEffectUid++;
                eff.entry.pack = args[1];
                eff.entry.enabled = true;
                studio_->effects.push_back(std::move(eff));
                studio_->history.Push(std::make_unique<studio::EffectsCommand>(*studio_, Tr("효과 추가"), before, studio_->effects));
                ++studio_->projectVersion;
                studio_->rowsKey = ~0ull;
            } else if (action == "remove" && args.size() >= 2) {
                size_t idx = (size_t)std::atoi(args[1].c_str());
                if (idx < studio_->effects.size()) {
                    std::vector<studio::StudioEffect> before = studio_->effects;
                    studio_->effects.erase(studio_->effects.begin() + (long)idx);
                    studio_->history.Push(std::make_unique<studio::EffectsCommand>(*studio_, Tr("효과 삭제"), before, studio_->effects));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            } else if (action == "enable" && args.size() >= 3) {
                size_t idx = (size_t)std::atoi(args[1].c_str());
                if (idx < studio_->effects.size()) {
                    std::vector<studio::StudioEffect> before = studio_->effects;
                    studio_->effects[idx].entry.enabled = std::atoi(args[2].c_str()) != 0;
                    studio_->history.Push(std::make_unique<studio::EffectsCommand>(*studio_,
                        studio_->effects[idx].entry.enabled ? Tr("효과 켜기") : Tr("효과 끄기"), before, studio_->effects));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            }
        }
    } else if (cmd == "studioeffectkey") {  // studioeffectkey <index> <param key> <frame> <value> [stepped]
        if (studio_ && args.size() >= 4) {
            size_t idx = (size_t)std::atoi(args[0].c_str());
            if (idx < studio_->effects.size()) {
                studio::StudioEffect& eff = studio_->effects[idx];
                const std::string& param = args[1];
                int frame = std::atoi(args[2].c_str());
                float val = (float)std::atof(args[3].c_str());
                bool stepped = args.size() > 4 && (args[4] == "1" || ToLowerAscii(args[4]) == "stepped" || ToLowerAscii(args[4]) == "true");
                std::vector<studio::TrackState> before = {studio::CaptureTrack(*studio_, -1, studio::RowKind::EffectParam,
                                                                               studio::EffectParamTrackName(eff.uid, param))};
                studio::EffectParamTrack* tr = nullptr;
                for (auto& t : eff.tracks) {
                    if (t.param == param) { tr = &t; break; }
                }
                if (!tr) {
                    eff.tracks.push_back({param, {}});
                    tr = &eff.tracks.back();
                }
                studio::UpsertParamKey(*tr, studio::EffectParamKey{frame, val, stepped});
                StudioPushTrackEdit(Tr("효과 키 등록"), before);
                studio_->rowsKey = ~0ull;
            }
        }
    } else if (cmd == "studioworld") {  // studioworld <add <pack id> | remove <index> | enable <index> <0|1> | place <index> tx ty tz rx ry rz s>
        if (studio_ && !args.empty()) {
            const std::string action = ToLowerAscii(args[0]);
            if (action == "add" && args.size() >= 2) {
                std::vector<studio::StudioWorld> before = studio_->worlds;
                studio::StudioWorld w;
                w.uid = studio_->nextWorldUid++;
                w.name = args[1];
                w.fx.entry.pack = args[1];
                w.fx.entry.enabled = true;
                studio_->worlds.push_back(std::move(w));
                studio_->history.Push(std::make_unique<studio::WorldsCommand>(*studio_, Tr("월드 추가"), before, studio_->worlds));
                ++studio_->projectVersion;
                studio_->rowsKey = ~0ull;
            } else if (action == "remove" && args.size() >= 2) {
                size_t idx = (size_t)std::atoi(args[1].c_str());
                if (idx < studio_->worlds.size()) {
                    std::vector<studio::StudioWorld> before = studio_->worlds;
                    studio_->worlds.erase(studio_->worlds.begin() + (long)idx);
                    studio_->history.Push(std::make_unique<studio::WorldsCommand>(*studio_, Tr("월드 삭제"), before, studio_->worlds));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            } else if (action == "enable" && args.size() >= 3) {
                size_t idx = (size_t)std::atoi(args[1].c_str());
                if (idx < studio_->worlds.size()) {
                    std::vector<studio::StudioWorld> before = studio_->worlds;
                    studio_->worlds[idx].fx.entry.enabled = std::atoi(args[2].c_str()) != 0;
                    studio_->history.Push(std::make_unique<studio::WorldsCommand>(*studio_,
                        studio_->worlds[idx].fx.entry.enabled ? Tr("월드 켜기") : Tr("월드 끄기"), before, studio_->worlds));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            } else if (action == "place" && args.size() >= 9) {
                size_t idx = (size_t)std::atoi(args[1].c_str());
                if (idx < studio_->worlds.size()) {
                    std::vector<studio::StudioWorld> before = studio_->worlds;
                    studio::PropAttach& p = studio_->worlds[idx].place;
                    p.translation = {(float)std::atof(args[2].c_str()), (float)std::atof(args[3].c_str()),
                                     (float)std::atof(args[4].c_str())};
                    p.rotationDeg = {(float)std::atof(args[5].c_str()), (float)std::atof(args[6].c_str()),
                                     (float)std::atof(args[7].c_str())};
                    p.scale = (float)std::atof(args[8].c_str());
                    studio_->history.Push(std::make_unique<studio::WorldsCommand>(*studio_, Tr("월드 배치"), before, studio_->worlds));
                    ++studio_->projectVersion;
                }
            }
        }
    } else if (cmd == "studioworldkey") {  // studioworldkey <index> <param key> <frame> <value> [stepped]
        if (studio_ && args.size() >= 4) {
            size_t idx = (size_t)std::atoi(args[0].c_str());
            if (idx < studio_->worlds.size()) {
                studio::StudioWorld& world = studio_->worlds[idx];
                const std::string& param = args[1];
                int frame = std::atoi(args[2].c_str());
                float val = (float)std::atof(args[3].c_str());
                bool stepped = args.size() > 4 && (args[4] == "1" || ToLowerAscii(args[4]) == "stepped" || ToLowerAscii(args[4]) == "true");
                std::vector<studio::TrackState> before = {studio::CaptureTrack(*studio_, -1, studio::RowKind::WorldParam,
                                                                               studio::WorldParamTrackName(world.uid, param))};
                studio::EffectParamTrack* tr = nullptr;
                for (auto& t : world.fx.tracks) {
                    if (t.param == param) { tr = &t; break; }
                }
                if (!tr) {
                    world.fx.tracks.push_back({param, {}});
                    tr = &world.fx.tracks.back();
                }
                studio::UpsertParamKey(*tr, studio::EffectParamKey{frame, val, stepped});
                StudioPushTrackEdit(Tr("월드 키 등록"), before);
                studio_->rowsKey = ~0ull;
            }
        }
    } else if (cmd == "studiostageparam") {  // studiostageparam <stage index> <param key>[#c] <value>
        if (studio_ && args.size() >= 3) {
            size_t idx = (size_t)std::atoi(args[0].c_str());
            if (idx < studio_->stages.size()) {
                studio::StudioStage& stage = studio_->stages[idx];
                const std::string name = args[1];
                int comp = -1;
                const std::string key = studio::StageParamBaseKey(name, comp);
                const StageParam* param = stage.manifest ? stage.manifest->FindParam(key) : nullptr;
                if (!param) {
                    LOG_WARN("studio: studiostageparam: unknown param \"%s\"", key.c_str());
                } else {
                    std::vector<studio::StudioStage> before = studio_->stages;
                    studio::StudioStage* st = studio_->FindStage(stage.uid);
                    if (!st) return true;
                    auto value = param->DefaultValue();
                    const auto base = st->base.find(key);
                    if (base != st->base.end()) value = base->second;
                    if (param->type == StageParamType::Color) {
                        if (comp < 0) comp = 0;   // a colour is set per component (the manifest's rgba shape)
                        DirectX::XMFLOAT4 c = value.AsColor();
                        const int comps = (int)args.size() - 2;
                        for (int i = 0; i < 4 && i < comps; ++i) {
                            const float v = (float)std::atof(args[(size_t)(2 + i)].c_str());
                            switch (i) {
                            case 0: c.x = v; break;
                            case 1: c.y = v; break;
                            case 2: c.z = v; break;
                            default: c.w = v; break;
                            }
                        }
                        value = StageValue::Color(c);
                    } else if (param->type == StageParamType::Enum && !param->options.empty()) {
                        // an enum takes the option's index or its "value" string
                        const std::string& a = args[2];
                        bool numeric = !a.empty() && std::all_of(a.begin(), a.end(), [](char ch) { return std::isdigit((unsigned char)ch) || ch == '-' || ch == '+'; });
                        float v = numeric ? (float)std::atof(a.c_str()) : 0.0f;
                        if (!numeric) {
                            for (size_t i = 0; i < param->options.size(); ++i)
                                if (param->options[i].value == a) { v = (float)i; break; }
                        }
                        value = StageValue::Number(v);
                    } else {
                        value = StageValue::Number((float)std::atof(args[2].c_str()));
                    }
                    st->base[key] = ClampStageParamValue(*param, value);
                    studio_->history.Push(std::make_unique<studio::StagesCommand>(*studio_, Tr("스테이지 파라미터"),
                                                                                  std::move(before), studio_->stages));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            }
        }
    } else if (cmd == "studiostagekey") {  // studiostagekey <stage index> <param key[#c]> <frame> <value> [stepped]
        if (studio_ && args.size() >= 4) {
            size_t idx = (size_t)std::atoi(args[0].c_str());
            if (idx < studio_->stages.size()) {
                studio::StudioStage& stage = studio_->stages[idx];
                const std::string param = args[1];
                int frame = std::atoi(args[2].c_str());
                float val = (float)std::atof(args[3].c_str());
                bool stepped = args.size() > 4 &&
                               (args[4] == "1" || ToLowerAscii(args[4]) == "stepped" || ToLowerAscii(args[4]) == "true");
                std::vector<studio::TrackState> before = {studio::CaptureTrack(*studio_, -1, studio::RowKind::StageParam,
                                                                               studio::StageParamTrackName(stage.uid, param))};
                studio::StudioStage* st = studio_->FindStage(stage.uid);
                studio::EffectParamTrack* tr = nullptr;
                for (auto& t : st->tracks) {
                    if (t.param == param) { tr = &t; break; }
                }
                if (!tr) {
                    st->tracks.push_back({param, {}});
                    tr = &st->tracks.back();
                }
                studio::UpsertParamKey(*tr, studio::EffectParamKey{frame, val, stepped});
                StudioPushTrackEdit(Tr("스테이지 키 등록"), before);
                studio_->rowsKey = ~0ull;
            }
        }
    } else if (cmd == "studiostagecues") {  // studiostagecues <stage index>: the manifest's cue tracks become keys
        if (studio_ && !args.empty()) {
            const size_t idx = (size_t)std::atoi(args[0].c_str());
            if (idx < studio_->stages.size()) {
                std::vector<studio::StudioStage> before = studio_->stages;
                studio::StudioStage* st = studio_->FindStage(studio_->stages[idx].uid);
                if (st && studio::ImportStageCues(*st)) {
                    studio_->history.Push(std::make_unique<studio::StagesCommand>(*studio_, Tr("큐를 키로 가져오기"),
                                                                                  std::move(before), studio_->stages));
                    ++studio_->projectVersion;
                    studio_->rowsKey = ~0ull;
                }
            }
        }
    } else if (cmd == "studiocamadd") {  // studiocamadd <name>: an extra camera from the current view
        if (studio_) {
            std::string name;
            for (const std::string& a : args) name += (name.empty() ? "" : " ") + a;
            StudioAddCameraFromView();
            if (!name.empty()) {
                studio::StudioCamera* c = studio_->FindCamera(studio_->selectedCameraUid);
                if (c) c->name = name;   // the script names it (no undo step of its own: the add already is one)
            }
        }
    } else if (cmd == "studiocamselect") {  // studiocamselect <uid>
        if (studio_ && !args.empty()) StudioSelectCamera((uint32_t)std::max(0, std::atoi(args[0].c_str())));
    } else if (cmd == "studiocamimport") {
        if (studio_) StudioImportStageCameras();
    } else if (cmd == "studiocamview") {  // studiocamview <0|1>: look through the camera (0 = the free camera)
        if (studio_ && !args.empty()) {
            studio_->useMotionCamera = std::atoi(args[0].c_str()) != 0 && studio_->HasCameraKeys();
            if (studio_->useMotionCamera) StudioPossess(false);
        }
    } else if (cmd == "studiopossess") {  // studiopossess <0|1>: possess the selected camera (viewport edits its keys)
        if (studio_ && !args.empty()) StudioPossess(std::atoi(args[0].c_str()) != 0);
    } else if (cmd == "look") {  // look <id>: applies a look pack and saves the settings like a UI change
        if (!args.empty()) ApplyLookById(args[0], true);
    } else if (cmd == "lookexport") {  // lookexport <id> <name...>
        if (!args.empty()) {
            std::string name;
            for (size_t i = 1; i < args.size(); ++i) name += (i > 1 ? " " : "") + args[i];
            ExportLook(args[0], name);
        }
    } else if (cmd == "mmeglobal") {  // mmeglobal <0|1>: the saved global MME switch
        if (!args.empty()) {
            settings_.mmeEnabled = std::atoi(args[0].c_str()) != 0;
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
    } else if (cmd == "rayglobal") {  // rayglobal <0|1>: the saved global Ray-MMD material-source switch
        if (!args.empty()) {
            settings_.rayMmdEnabled = std::atoi(args[0].c_str()) != 0;
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
    } else if (cmd == "mmeworkflow") {  // mmeworkflow <auto|manual>: the saved MME workflow
        if (!args.empty()) {
            const std::string v = ToLowerAscii(args[0]);
            if (v == "auto") settings_.mmeWorkflow = 1;
            else if (v == "manual") settings_.mmeWorkflow = 0;
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
    } else if (cmd == "rendershutdown") {  // rendershutdown 0|1: the "완료 후 시스템 종료" switch (scripted runs: dry run)
        shutdownAfterRender_ = !args.empty() && args[0] != "0";
        LOG_INFO("SYSTEM SHUTDOWN switch %d", shutdownAfterRender_ ? 1 : 0);
    } else if (cmd == "rendershutdownanswer") {  // rendershutdownanswer <cancel|now>: presses the countdown's button
        if (!args.empty() && args[0] == "now") shutdownDeadline_ = timeSeconds_;
        else CancelShutdownCountdown();
    } else if (cmd == "videorange") {  // videorange <all|start end>: the lobby video render dialog's range
        if (!args.empty() && ToLowerAscii(args[0]) == "all") {
            videoRangeCustom_ = false;
        } else if (args.size() >= 2) {
            double a = 0.0, b = 0.0;
            if (!ParseTimecode(args[0], a) || !ParseTimecode(args[1], b)) {
                LOG_WARN("ui script: videorange wants timecodes or seconds (got '%s', '%s')",
                         args[0].c_str(), args[1].c_str());
                if (outError) *outError = "Invalid videorange range";
                return false;
            }
            videoRangeCustom_ = true;
            videoRangeSec_[0] = a;
            videoRangeSec_[1] = b;
            std::snprintf(videoRangeText_[0], sizeof(videoRangeText_[0]), "%s", FormatTimecode(a).c_str());
            std::snprintf(videoRangeText_[1], sizeof(videoRangeText_[1]), "%s", FormatTimecode(b).c_str());
        }
    } else if (cmd == "fxpanel") {  // fxpanel <open|close> [effects|shader|mme]: the play-mode 화면 효과 panel
        if (!args.empty()) playPanelOpen_ = args[0] != "close";
        if (args.size() > 1) {
            const std::string tab = ToLowerAscii(args[1]);
            if (tab == "effects") settings_.playPanelTab = 0;
            else if (tab == "shader") settings_.playPanelTab = 1;
            else if (tab == "mme") settings_.playPanelTab = 2;
        }
    } else if (cmd == "mmestate") {  // mmestate: logs an MMESTATE line (global switch + workflow + per loaded model)
        std::string models;
        const auto addModel = [&](const std::string& id, const std::filesystem::path& path, bool isStage) {
            const MmeChoice mme = ResolveMmeChoice(id, isStage, path);
            const MaterialChoice mc = ResolveMaterialChoice(id, path);
            const char* mode = mme.mode == MmeChoice::Auto ? "auto" : mme.mode == MmeChoice::File ? "file" : "none";
            std::string entry = id + ":" + mode + ":ray" + (mc.ray ? "1" : "0");
            if (MmeAutoSkipApplies(mme, isStage)) {
                const std::string skip = MmeControllerSkip(path);
                if (!skip.empty()) entry += ":skip=" + skip;
            }
            models += (models.empty() ? "" : ",") + entry;
        };
        if (scene_) {
            if (scene_->character) addModel(scene_->characterId, scene_->character->Model().sourcePath, false);
            for (const auto& e : scene_->extras)
                if (e.instance) addModel(e.characterId, e.instance->Model().sourcePath, false);
            for (const auto& st : scene_->stages) addModel(scene_->stageId, st->Model().sourcePath, true);
        }
        LOG_INFO("MMESTATE enabled=%d ray=%d workflow=%s models=%s", (int)MmeGlobalEnabled(),
                 (int)RayMmdGlobalEnabled(), MmeAutoWorkflow() ? "auto" : "manual",
                 models.empty() ? "-" : models.c_str());
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
            } else if (s.cmd == "drop") {
                // drop <file>[|<file>...]: paths may contain spaces, so `|` separates them (paths relative to the cwd)
                std::string rest;
                std::getline(ss, rest);
                size_t start = rest.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    rest = rest.substr(start);
                    for (size_t bar = 0; bar <= rest.size();) {
                        const size_t next = rest.find('|', bar);
                        std::string part = rest.substr(bar, next == std::string::npos ? std::string::npos : next - bar);
                        const size_t a = part.find_first_not_of(" \t");
                        const size_t b = part.find_last_not_of(" \t");
                        if (a != std::string::npos) s.args.push_back(part.substr(a, b - a + 1));
                        if (next == std::string::npos) break;
                        bar = next + 1;
                    }
                }
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
