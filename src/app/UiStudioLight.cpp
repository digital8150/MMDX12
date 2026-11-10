// Studio scene light UI: outliner group and rows, preset confirmation modal, and the light inspector.
// Keyed edits follow the keyed edit rule (playhead key -> auto-key insert -> base values).
#include "app/App.h"
#include "ui_probe/UiProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiKit.h"
#include "asset/StageLights.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/SceneLight.h"
#include "studio/FileDialog.h"
#include "studio/StudioDoc.h"

namespace mmdx {

using namespace studio;

namespace {

const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
const char* const kHeadBone = "\xE9\xA0\xAD";                                        // 頭

// The performer centre as BuildStudioFrameView computes it, else {0, 10, 0}.
DirectX::XMFLOAT3 StudioPerformerFocus(const StudioDoc& d) {
    for (const auto& m : d.models) {
        if (m->kind == ModelKind::Character && m->visible && m->pmx && m->inst) {
            const int c = m->pmx->FindBone(kCenterBone);
            if (c >= 0) return m->inst->BoneWorldPosition(c);
        }
    }
    return {0.0f, 10.0f, 0.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// Selection and Light addition
// ---------------------------------------------------------------------------

void App::StudioSelectLight(uint32_t uid) {
    StudioDoc& d = *studio_;
    if (d.selectedModel == -1 && d.selectedLightUid == uid) return;
    StudioPossess(false);  // camera possession does not belong to lights
    d.selectedModel = -1;
    d.selectedLightUid = uid;
    studioSelectedWorld_ = 0;
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
    d.expanded.clear();
    d.rowsKey = ~0ull;
}

void App::StudioAddLight(LightKind kind) {
    StudioDoc& d = *studio_;
    SceneLight l;
    l.uid = d.nextLightUid++;
    l.kind = kind;
    l.enabled = true;
    const DirectX::XMFLOAT3 focus = StudioPerformerFocus(d);

    if (kind == LightKind::Point) {
        l.v.position = {focus.x + 0.0f, focus.y + 40.0f, focus.z - 20.0f};
        l.v.range = 140.0f;
        l.v.color = {1.0f, 1.0f, 1.0f};
        l.v.intensity = 1.0f;
        int n = 1;
        while (true) {
            std::string candidate = "점광원 " + std::to_string(n);
            bool taken = false;
            for (const auto& ex : d.lights) {
                if (ex.name == candidate) { taken = true; break; }
            }
            if (!taken) { l.name = candidate; break; }
            ++n;
        }
    } else if (kind == LightKind::Spot) {
        l.v.position = {focus.x + 0.0f, focus.y + 58.0f, focus.z - 18.0f};
        l.v.aim = focus;
        l.aimMode = AimMode::Manual;
        l.v.coneOuter = 0.24f;
        l.v.coneInner = 0.15f;
        l.v.intensity = 2.6f;
        l.v.range = 140.0f;
        l.v.color = {1.0f, 1.0f, 1.0f};
        int n = 1;
        while (true) {
            std::string candidate = "스팟 " + std::to_string(n);
            bool taken = false;
            for (const auto& ex : d.lights) {
                if (ex.name == candidate) { taken = true; break; }
            }
            if (!taken) { l.name = candidate; break; }
            ++n;
        }
    } else if (kind == LightKind::Area) {
        l.v.position = {focus.x + 0.0f, focus.y + 40.0f, focus.z - 20.0f};
        l.v.aim = focus;
        l.aimMode = AimMode::Manual;
        l.v.size = {20.0f, 20.0f};
        l.v.intensity = 1.0f;
        l.v.range = 140.0f;
        l.v.color = {1.0f, 1.0f, 1.0f};
        int n = 1;
        while (true) {
            std::string candidate = "면광원 " + std::to_string(n);
            bool taken = false;
            for (const auto& ex : d.lights) {
                if (ex.name == candidate) { taken = true; break; }
            }
            if (!taken) { l.name = candidate; break; }
            ++n;
        }
    } else if (kind == LightKind::Sun) {
        l.name = "메인 조명";
    } else if (kind == LightKind::Ambient) {
        l.name = "환경광";
    } else if (kind == LightKind::Sky) {
        // S9b-1: the Physical Sky light: one per scene, the sun at noon by default
        l.name = "하늘";
        l.shadow = ShadowType::NoCast;
    } else if (kind == LightKind::FogVolume) {
        // S9: a box of local medium around the performer. intensity = the density it adds to the global haze.
        l.v.position = {focus.x + 0.0f, focus.y + 6.0f, focus.z};
        l.v.extent = {30.0f, 14.0f, 30.0f};
        l.v.intensity = 1.0f;
        l.v.color = {1.0f, 1.0f, 1.0f};
        l.fogShape = FogShape::Box;
        l.fogEdge = 0.35f;
        l.shadow = ShadowType::NoCast;
        int n = 1;
        while (true) {
            std::string candidate = "안개 " + std::to_string(n);
            bool taken = false;
            for (const auto& ex : d.lights) {
                if (ex.name == candidate) { taken = true; break; }
            }
            if (!taken) { l.name = candidate; break; }
            ++n;
        }
    }

    std::vector<SceneLight> before = d.lights;
    std::vector<SceneLight> after = before;
    after.push_back(l);
    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 추가"), before, after));
    StudioSelectLight(l.uid);
    ++d.projectVersion;
    d.rowsKey = ~0ull;
}

void App::StudioDeleteLight(uint32_t uid) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    std::vector<SceneLight> before = d.lights;
    std::vector<SceneLight> after = before;
    std::erase_if(after, [uid](const SceneLight& l) { return l.uid == uid; });
    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 삭제"), before, after));
    if (d.selectedLightUid == uid) d.selectedLightUid = 0;
    ++d.projectVersion;
    d.rowsKey = ~0ull;
}

// The lighting source of the scene lights (the ribbon's 조명 tab): 0 VMD (the sun follows the camera VMD's light track),
// 1 preset (the preset's own sun), 2 custom (hand-edited lights, the sun no longer linked). The studio has no stored
// source field: the state is read off the sun, and every choice is one undo step like the inspector's VMD 연동 switch.
void App::StudioSetLightSource(int source) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    if (source == 1) {
        // the inspector's own path: the confirmation modal, then one LightsCommand step
        studioLightPresetPending_ = std::clamp(settings_.lighting, 0, 3);
        return;
    }
    const bool link = source == 0;
    SceneLight* sun = nullptr;
    for (SceneLight& l : d.lights)
        if (l.kind == studio::LightKind::Sun) { sun = &l; break; }
    if (!sun || sun->vmdLink == link) return;
    std::vector<SceneLight> before = d.lights;
    sun->vmdLink = link;
    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, d.lights));
    ++d.projectVersion;
    d.rowsKey = ~0ull;
}

void App::StudioApplyLightPreset(int presetIndex) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    const int idx = std::clamp(presetIndex, 0, 3);
    const DirectX::XMFLOAT3 focus = StudioPerformerFocus(d);
    std::vector<SceneLight> before = d.lights;
    std::vector<SceneLight> after = PresetLights(idx, focus, d.nextLightUid);
    d.history.Push(std::make_unique<LightsCommand>(d, Tr("프리셋 적용"), before, after));
    d.selectedLightUid = 0;
    ++d.projectVersion;
    d.rowsKey = ~0ull;
}

// S1b: an MMDX stage that ships a light rig replaces the project's lights with it, in ONE undoable step. S3: the lights
// are ordinary scene lights tagged with the stage they came from (SceneLight::stageUid) and the manifest light name their
// bindings address (stageLightName), so the per-frame stage targets can drive them. S2a: when the manifest says
// "gltfLights", the parts' KHR_lights_punctual lights are appended first, named after their glTF nodes.
bool App::StudioStageRigLights(StudioDoc& d, uint32_t stageUid, std::vector<SceneLight>& after) {
    after.clear();
    StudioStage* stage = d.FindStage(stageUid);
    if (!stage || !stage->manifest) return false;
    const std::shared_ptr<const StageManifest> manifest = stage->manifest;
    if (manifest->lights.empty() && manifest->environment.fogVolumes.empty() && !manifest->gltfLights) return false;
    after = manifest->lights;
    if (manifest->gltfLights) {
        std::vector<std::string> taken;
        for (const SceneLight& l : after) taken.push_back(l.name);
        for (const auto& mp : d.models) {
            const StudioModel& m = *mp;
            if (m.stageUid != stageUid || !m.pmx) continue;
            std::vector<SceneLight> added = PartGltfLights(*manifest, *m.pmx, taken);
            for (const SceneLight& l : added) taken.push_back(l.name);
            after.insert(after.end(), added.begin(), added.end());
        }
    }
    // S9: the stage's local fog volumes become FogVolume lights of the rig (placement only, never cue driven).
    for (size_t i = 0; i < manifest->environment.fogVolumes.size(); ++i)
        after.push_back(StageFogVolumeLight(manifest->environment.fogVolumes[i], i));
    if (after.empty()) return false;
    for (SceneLight& l : after) {
        l.uid = d.nextLightUid++;   // the stage's order is its identity; uids are per project
        if (l.name.empty()) l.name = LightKindName(l.kind);
        l.viewportVisible = true;
        l.stageUid = stageUid;
        l.stageLightName = l.name;
    }
    return true;
}

void App::StudioApplyStageLightRig(uint32_t stageUid) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    std::vector<SceneLight> before = d.lights;
    std::vector<SceneLight> after;
    if (!StudioStageRigLights(d, stageUid, after)) return;
    const std::shared_ptr<const StageManifest> manifest = d.FindStage(stageUid)->manifest;
    d.history.Push(std::make_unique<LightsCommand>(d, Tr("스테이지 조명"), before, after));
    d.selectedLightUid = 0;
    ++d.projectVersion;
    d.rowsKey = ~0ull;
    // S9: a stage with an environment.fog turns the volumetric haze on (the user's switch can still turn it off).
    if (manifest->environment.fog.present && !settings_.volumetric) {
        settings_.volumetric = true;
        ApplyRenderSettings();
    }
    toast_ = {Tr("스테이지 조명을 불러왔어요"),
              manifest->DisplayName(StageLanguage()) + "  ·  " + std::to_string(after.size()) + Tr("개 조명"),
              {}, false, timeSeconds_ + 4.0};
    LOG_INFO("studio: stage light rig '%s' (%d lights)", manifest->id.c_str(), (int)after.size());
}


// ---------------------------------------------------------------------------
// Outliner
// ---------------------------------------------------------------------------

void App::DrawStudioLightOutliner() {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = Dp(40.0f);

    if (d.lights.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(16.0f), ImGui::GetCursorScreenPos().y + Dp(2.0f)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - Dp(32.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::TextWrapped("%s", Tr("조명이 없어요. 추가 › 조명에서 넣으세요."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(w - Dp(16.0f), Dp(6.0f)));
    }

    // Light rows
    for (size_t i = 0; i < d.lights.size(); ++i) {
        SceneLight& l = d.lights[i];
        ImGui::PushID("light");  // model rows push their index in the same scope
        ImGui::PushID((int)l.uid);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + w, a.y + rowH);
        const bool selected = (d.selectedModel == -1 && d.selectedLightUid == l.uid && studioSelectedWorld_ == 0);

        ImGui::InvisibleButton("##row", ImVec2(w - Dp(40.0f), rowH));
        const std::string shownName = LightDisplayName(l.name);
        uiprobe::Info(ImGui::GetItemID(), "##row", shownName.c_str());   // MCP ui_click by name
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            StudioSelectLight(l.uid);
        }

        const bool rowHot = hovered || ImGui::IsMouseHoveringRect(a, b);
        const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
        if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
        else if (rowHot) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));

        const char* glyph = icon::Lightbulb;
        const char* sub = Tr("점광원");
        if (l.kind == LightKind::Sun) { glyph = icon::Sun; sub = Tr("태양"); }
        else if (l.kind == LightKind::Spot) { glyph = icon::Aperture; sub = Tr("스팟"); }
        else if (l.kind == LightKind::Ambient) { glyph = icon::CircleHalf; sub = Tr("환경광"); }
        else if (l.kind == LightKind::Area) { glyph = icon::Image; sub = Tr("면광원"); }
        else if (l.kind == LightKind::FogVolume) { glyph = icon::Drop; sub = Tr("안개 볼륨"); }
        else if (l.kind == LightKind::Sky) { glyph = icon::Mountains; sub = Tr("하늘"); }

        char subBuf[64];
        if (!l.keys.empty()) {
            char kBuf[32];
            std::snprintf(kBuf, sizeof(kBuf), Tr("키 %d개"), (int)l.keys.size());
            std::snprintf(subBuf, sizeof(subBuf), "%s · %s", sub, kBuf);
            sub = subBuf;
        }

        const ImU32 fg = selected ? p.accentInk : p.ink;
        Icon(cdl, glyph, 16.0f, ImVec2(a.x + Dp(26.0f), a.y + rowH * 0.5f), selected ? p.accentInk : p.ink2);
        const float tx = a.x + Dp(44.0f), maxX = b.x - Dp(46.0f);
        TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, shownName.c_str());
        TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, sub);

        // Enable eye toggle
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
        if (IconButton("##vis", l.enabled ? icon::Eye : icon::EyeSlash, l.enabled ? Tr("끄기") : Tr("켜기"), false, 28.0f)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& light : after) {
                if (light.uid == l.uid) { light.enabled = !light.enabled; break; }
            }
            const bool nowOn = !l.enabled;
            d.history.Push(std::make_unique<LightsCommand>(d, nowOn ? Tr("조명 켜기") : Tr("조명 끄기"), before, after));
            ++d.projectVersion;
        }

        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::PopID();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Preset confirmation modal
// ---------------------------------------------------------------------------

void App::DrawStudioLightPresetConfirm() {
    using namespace ui;
    if (studioLightPresetPending_ < 0) return;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(420.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##lightpresetconfirm")) ImGui::OpenPopup("##lightpresetconfirm");
    if (ImGui::BeginPopupModal("##lightpresetconfirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(372.0f), p.ink, Tr("조명 프리셋 적용"));
        Text(dl, Font::Semibold, size::Body, ImVec2(c.x, c.y + Dp(28.0f)), p.ink, Tr("프리셋으로 재설정됩니다. 계속하시겠어요?"));

        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(54.0f)));
        ImGui::PushTextWrapPos(c.x + Dp(372.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped("%s", Tr("현재 조명 목록이 프리셋의 조명으로 교체됩니다. 실행 취소(Ctrl+Z)로 되돌릴 수 있습니다."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        ImGui::Dummy(ImVec2(0, Dp(80.0f)));
        bool close = false;
        if (Button("##presetcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close = true;
        }
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##presetok", Tr("적용"), nullptr, ButtonKind::Primary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            StudioApplyLightPreset(studioLightPresetPending_);
            close = true;
        }
        if (close) {
            studioLightPresetPending_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Inspector for the selected light
// ---------------------------------------------------------------------------

void App::StudioBeginLightEdit(uint32_t uid, bool keyEdit) {
    if (studioLightEdit_) return;
    StudioDoc& d = *studio_;
    studioLightEditingUid_ = uid;
    studioLightIsKeyEdit_ = keyEdit;
    studioLightChanged_ = false;
    if (keyEdit) {
        studioLightKeyBefore_ = {CaptureTrack(d, -1, RowKind::SceneLight, LightTrackName(uid))};
    } else {
        studioLightBaseBefore_ = d.lights;
    }
    studioLightEdit_ = true;
}

void App::StudioEndLightEdit(uint32_t uid) {
    if (!studioLightEdit_) return;
    StudioDoc& d = *studio_;
    if (studioLightChanged_) {
        if (studioLightIsKeyEdit_) {
            StudioPushTrackEdit(Tr("조명 편집"), studioLightKeyBefore_);
        } else {
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), studioLightBaseBefore_, d.lights));
        }
    }
    studioLightKeyBefore_.clear();
    studioLightBaseBefore_.clear();
    studioLightEdit_ = false;
    studioLightChanged_ = false;
    studioLightEditingUid_ = 0;
}

void App::DrawStudioLightInspector(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();

    SceneLight* light = d.FindLight(d.selectedLightUid);
    if (!light) {
        d.selectedLightUid = 0;
        return;
    }

    const float btnH = 32.0f;
    char buf[160];

    const auto separator = [&](float gap) {
        ImGui::Dummy(ImVec2(w, Dp(gap)));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        cdl->AddLine(c, ImVec2(c.x + w, c.y), p.line);
        ImGui::Dummy(ImVec2(w, Dp(8.0f)));
    };

    const auto row = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(72.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(72.0f));
        PushFont(Font::Regular, size::Small);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, Dp(4.0f)));
        bool changed = widget();
        ImGui::PopStyleVar();
        PopFont();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        return changed;
    };

    // S3: the fields of this light a stage binding drives: read-only here (the stage's params own them)
    std::set<std::string> stageDriven;
    if (light->stageUid != 0 && !light->stageLightName.empty())
        if (const StudioStage* st = d.FindStage(light->stageUid); st && st->manifest)
            stageDriven = StageDrivenLightFields(*st->manifest, light->stageLightName);
    const auto stageOwns = [&](const char* field) { return stageDriven.count(field) != 0; };
    const auto stageNote = [&] {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::TextWrapped("%s", Tr("스테이지 파라미터가 제어"));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };

    // Helper for editing a keyable field according to Keyed Edit Rule
    auto keyedEdit = [&](const char* label, auto&& widget, auto&& apply, const char* field = nullptr) {
        const int frame = d.Frame();
        LightKey* k = FindKey(light->keys, frame);
        const bool isKeyed = (k != nullptr);
        const bool willKey = isKeyed || !light->keys.empty() || d.autoKey;
        const bool locked = field && stageOwns(field);
        if (locked) ImGui::BeginDisabled(true);
        bool changed = row(label, [&] { return widget(); });
        if (locked) ImGui::EndDisabled();

        // One undo step per drag / typed value. A change opens an edit when none is open (a field activated as another
        // field's edit ends only joins it), and the edit ends after this frame's value is written.
        if (ImGui::IsItemActivated() || changed) {
            StudioBeginLightEdit(light->uid, willKey);
        }
        if (changed) {
            studioLightChanged_ = true;
            if (studioLightIsKeyEdit_) {
                LightKey* curKey = FindKey(light->keys, frame);
                if (curKey) {
                    apply(curKey->v);
                } else {
                    LightValues val = SampleLightValues(*light, frame);
                    apply(val);
                    UpsertKey(light->keys, LightKey{frame, val});
                }
            } else {
                apply(light->v);
            }
            d.rowsKey = ~0ull;
        }
        if (ItemEditEnded()) {
            StudioEndLightEdit(light->uid);
        }
        return changed;
    };

    // Helper for editing a continuous base property
    auto basePropEdit = [&](const char* label, auto&& widget, auto&& apply, const char* field = nullptr) {
        const bool locked = field && stageOwns(field);
        if (locked) ImGui::BeginDisabled(true);
        bool changed = row(label, [&] { return widget(); });
        if (locked) ImGui::EndDisabled();
        if (ImGui::IsItemActivated() || changed) {  // as keyedEdit: a change opens an edit when none is open
            StudioBeginLightEdit(light->uid, false);
        }
        if (changed) {
            studioLightChanged_ = true;
            apply(*light);
            d.rowsKey = ~0ull;
        }
        if (ItemEditEnded()) {
            StudioEndLightEdit(light->uid);
        }
        return changed;
    };

    // Header: Kind label + the light's stopwatch (its keys hold all values) with the key diamond and ◀ ▶
    {
        const char* kindLabel = light->kind == LightKind::Sun ? Tr("태양") :
                                light->kind == LightKind::Point ? Tr("점광원") :
                                light->kind == LightKind::Spot ? Tr("스팟") :
                                light->kind == LightKind::Area ? Tr("면광원") :
                                light->kind == LightKind::FogVolume ? Tr("안개 볼륨") :
                                light->kind == LightKind::Sky ? Tr("하늘") : Tr("환경광");
        Text(cdl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.ink3, kindLabel);
        ImGui::Dummy(ImVec2(w, Dp(16.0f)));

        // A VMD-linked sun's colour / direction follow the camera VMD: read-only exactly as today (no stopwatch).
        const bool vmdLinkedSun = light->kind == LightKind::Sun && light->vmdLink && !d.camera.light.empty();
        if (!vmdLinkedSun) {
            const bool animated = !light->keys.empty();
            const ImVec2 lc = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Caption, ImVec2(lc.x, lc.y + Dp(4.0f)), animated ? p.ink2 : p.ink3,
                 animated ? Tr("애니메이션 (키 사용)") : Tr("장면 전체에 같은 값"));
            ImGui::Dummy(ImVec2(1.0f, Dp(22.0f)));
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        }
        // S3: a light that came from an MMDX stage rig: the fields its bindings drive belong to the stage's params
        if (!stageDriven.empty()) stageNote();
    }

    // Rename (undoable). The field shows the display name; an untouched default name keeps its saved form.
    {
        const std::string shownName = LightDisplayName(light->name);
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", shownName.c_str());
        row(Tr("이름"), [&] {
            if (ImGui::InputText("##lightname", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit()) {
                if (nameBuf[0] != '\0' && shownName != nameBuf) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.name = nameBuf; break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 이름 바꾸기"), before, after));
                    ++d.projectVersion;
                    d.rowsKey = ~0ull;
                }
            }
            return false;
        });
    }

    // Enable switch
    {
        bool enabled = light->enabled;
        if (Switch("##lightenable", Tr("활성화"), &enabled)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.enabled = enabled; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, enabled ? Tr("조명 켜기") : Tr("조명 끄기"), before, after));
            ++d.projectVersion;
        }
    }
    ImGui::Dummy(ImVec2(w, Dp(4.0f)));

    separator(6.0f);

    // Current values at playhead
    const int frame = d.Frame();
    const LightValues cur = SampleLightValues(*light, frame);

    // Kind-specific section
    if (light->kind == LightKind::Sun) {
        const bool vmdLinked = light->vmdLink && !d.camera.light.empty();
        if (vmdLinked) {
            float dir[3] = {cur.direction.x, cur.direction.y, cur.direction.z};
            ImGui::BeginDisabled(true);
            row(Tr("방향"), [&] { return ImGui::DragFloat3("##sundir", dir, 0.01f, -1.0f, 1.0f, "%.2f"); });
            float col[3] = {cur.color.x, cur.color.y, cur.color.z};
            row(Tr("색"), [&] {
                ui::ColorField("##suncol", col);
                return false;
            });
            ImGui::EndDisabled();

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            PushFont(Font::Regular, size::Caption);
            ImGui::TextWrapped("%s", Tr("카메라 VMD의 조명 트랙과 연동되어 있어 색과 방향이 읽기 전용입니다."));
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        } else {
            float dir[3] = {cur.direction.x, cur.direction.y, cur.direction.z};
            keyedEdit(Tr("방향"),
                [&] { return ImGui::DragFloat3("##sundir", dir, 0.01f, -1.0f, 1.0f, "%.2f"); },
                [&](LightValues& v) { v.direction = {dir[0], dir[1], dir[2]}; }, "direction");

            float col[3] = {cur.color.x, cur.color.y, cur.color.z};
            keyedEdit(Tr("색"),
                [&] { return ui::ColorField("##suncol", col); },
                [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; }, "color");
        }

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##sunintensity", &intensity, 0.02f, 0.0f, 100.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); }, "intensity");

        float rimStr = light->rimStrength;
        basePropEdit(Tr("림 강도"),
            [&] { return ImGui::DragFloat("##rimstr", &rimStr, 0.01f, 0.0f, 10.0f, "%.2f"); },
            [&](SceneLight& l) { l.rimStrength = std::max(0.0f, rimStr); }, "rimStrength");

        float rimCol[3] = {light->rimColor.x, light->rimColor.y, light->rimColor.z};
        basePropEdit(Tr("림 색"),
            [&] { return ui::ColorField("##rimcol", rimCol); },
            [&](SceneLight& l) { l.rimColor = {rimCol[0], rimCol[1], rimCol[2]}; }, "rimColor");

        bool vmdLink = light->vmdLink;
        if (Switch("##vmdlink", Tr("VMD 연동"), &vmdLink, Tr("카메라 VMD의 조명 키를 따릅니다"))) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.vmdLink = vmdLink; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }

    } else if (light->kind == LightKind::Point) {
        float pos[3] = {cur.position.x, cur.position.y, cur.position.z};
        keyedEdit(Tr("위치"),
            [&] { return ImGui::DragFloat3("##pointpos", pos, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; }, "position");

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] { return ui::ColorField("##pointcol", col); },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; }, "color");

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##pointintensity", &intensity, 0.05f, 0.0f, 1000.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); }, "intensity");

        float range = cur.range;
        keyedEdit(Tr("범위"),
            [&] { return ImGui::DragFloat("##pointrange", &range, 0.5f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.range = std::max(0.1f, range); }, "range");

    } else if (light->kind == LightKind::Spot) {
        // Aim mode: 수동 / 타겟 / 자동 스윙
        const char* aimModes[] = {Tr("수동"), Tr("타겟"), Tr("자동 스윙")};
        int aimMode = (int)light->aimMode;
        if (Segmented("##spotaimmode", aimModes, 3, &aimMode, w / Dpi(), 32.0f)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.aimMode = (AimMode)aimMode; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));

        if (light->aimMode == AimMode::Target) {
            std::string currentTargetName = Tr("메인 캐릭터 (자동)");
            if (light->targetUid != 0) {
                const int idx = d.IndexOfUid(light->targetUid);
                if (idx >= 0) currentTargetName = d.models[(size_t)idx]->name;
            }
            row(Tr("타겟 캐릭터"), [&] {
                if (ImGui::BeginCombo("##targetchar", currentTargetName.c_str())) {
                    const bool selPerformer = (light->targetUid == 0);
                    if (ImGui::Selectable(Tr("메인 캐릭터 (자동)"), selPerformer)) {
                        if (light->targetUid != 0) {
                            std::vector<SceneLight> before = d.lights;
                            std::vector<SceneLight> after = before;
                            for (SceneLight& l : after) {
                                if (l.uid == light->uid) { l.targetUid = 0; break; }
                            }
                            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                            ++d.projectVersion;
                        }
                    }
                    for (const auto& m : d.models) {
                        if (m->kind != ModelKind::Character) continue;
                        const bool isSel = (light->targetUid == m->uid);
                        if (ImGui::Selectable(m->name.c_str(), isSel)) {
                            if (light->targetUid != m->uid) {
                                std::vector<SceneLight> before = d.lights;
                                std::vector<SceneLight> after = before;
                                for (SceneLight& l : after) {
                                    if (l.uid == light->uid) { l.targetUid = m->uid; break; }
                                }
                                d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                                ++d.projectVersion;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                return false;
            });

            const char* parts[] = {Tr("중심"), Tr("머리")};
            int targetPart = (int)light->targetPart;
            row(Tr("타겟 부위"), [&] {
                bool changed = Segmented("##targetpart", parts, 2, &targetPart, (w - Dp(72.0f)) / Dpi(), 30.0f);
                if (changed) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.targetPart = (TargetPart)targetPart; break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                    ++d.projectVersion;
                }
                return changed;
            });
        }

        // Aim point shown only for 수동
        if (light->aimMode == AimMode::Manual) {
            float aim[3] = {cur.aim.x, cur.aim.y, cur.aim.z};
            keyedEdit(Tr("조준점"),
                [&] { return ImGui::DragFloat3("##spotaim", aim, 0.1f, 0.0f, 0.0f, "%.1f"); },
                [&](LightValues& v) { v.aim = {aim[0], aim[1], aim[2]}; }, "aim");
        }

        float pos[3] = {cur.position.x, cur.position.y, cur.position.z};
        keyedEdit(Tr("위치"),
            [&] { return ImGui::DragFloat3("##spotpos", pos, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; }, "position");

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] { return ui::ColorField("##spotcol", col); },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; }, "color");

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##spotintensity", &intensity, 0.05f, 0.0f, 1000.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); }, "intensity");

        float range = cur.range;
        keyedEdit(Tr("범위"),
            [&] { return ImGui::DragFloat("##spotrange", &range, 0.5f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.range = std::max(0.1f, range); }, "range");

        float outerDeg = DirectX::XMConvertToDegrees(cur.coneOuter);
        float innerDeg = DirectX::XMConvertToDegrees(cur.coneInner);
        keyedEdit(Tr("외부 각도"),
            [&] { return ImGui::DragFloat("##coneouter", &outerDeg, 0.2f, 1.0f, 89.0f, "%.1f°"); },
            [&](LightValues& v) {
                outerDeg = std::clamp(outerDeg, 1.0f, 89.0f);
                float curIn = DirectX::XMConvertToDegrees(v.coneInner);
                if (curIn > outerDeg) curIn = outerDeg;
                v.coneOuter = DirectX::XMConvertToRadians(outerDeg);
                v.coneInner = DirectX::XMConvertToRadians(curIn);
            }, "coneOuter");
        keyedEdit(Tr("내부 각도"),
            [&] { return ImGui::DragFloat("##coneinner", &innerDeg, 0.2f, 0.5f, outerDeg, "%.1f°"); },
            [&](LightValues& v) {
                float curOut = DirectX::XMConvertToDegrees(v.coneOuter);
                innerDeg = std::clamp(innerDeg, 0.5f, curOut);
                v.coneInner = DirectX::XMConvertToRadians(innerDeg);
            }, "coneInner");

        // S7b: the gobo (a stencil projected through the cone). The base values are bound fields of a stage light, so
        // they are read-only while a stage owns them, exactly like the shadow properties.
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        SectionLabel(Tr("고보"));
        {
            const std::string shown = light->gobo.empty() ? std::string(Tr("(없음)"))
                                                          : PathToUtf8(std::filesystem::path(light->gobo).filename());
            if (Button("##gobofile", shown.c_str(), icon::FolderOpen, ButtonKind::Secondary, ImVec2(w / Dpi(), 30.0f))) {
                const std::vector<studio::FileFilter> filters = {{L"이미지", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif"}};
                const std::filesystem::path picked = studio::OpenFileDialog(GetActiveWindow(), filters);
                if (!picked.empty()) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.gobo = PathToUtf8(picked); break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                    ++d.projectVersion;
                }
            }
            if (!light->gobo.empty()) {
                if (Button("##goboclear", Tr("고보 지우기"), nullptr, ButtonKind::Ghost, ImVec2(w / Dpi(), 26.0f))) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.gobo.clear(); break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                    ++d.projectVersion;
                }
            }
        }
        if (!light->gobo.empty()) {
            float rot = light->goboRotation;
            basePropEdit(Tr("회전"),
                [&] { return ImGui::DragFloat("##goborot", &rot, 1.0f, -360.0f, 360.0f, "%.0f°"); },
                [&](SceneLight& l) { l.goboRotation = rot; }, "gobo.rotation");

            float scale = light->goboScale;
            basePropEdit(Tr("크기"),
                [&] { return ImGui::DragFloat("##goboscale", &scale, 0.01f, 0.05f, 20.0f, "%.2f"); },
                [&](SceneLight& l) { l.goboScale = std::max(0.05f, scale); }, "gobo.scale");

            float spin = light->goboSpin;
            basePropEdit(Tr("회전 속도"),
                [&] { return ImGui::DragFloat("##gobospin", &spin, 1.0f, -360.0f, 360.0f, "%.0f°/s"); },
                [&](SceneLight& l) { l.goboSpin = spin; }, "gobo.spin");

            float focus = light->goboFocus;
            basePropEdit(Tr("초점"),
                [&] { return ImGui::SliderFloat("##gobofocus", &focus, 0.0f, 1.0f, "%.2f"); },
                [&](SceneLight& l) { l.goboFocus = std::clamp(focus, 0.0f, 1.0f); }, "gobo.focus");
        }

    } else if (light->kind == LightKind::Area) {
        float pos[3] = {cur.position.x, cur.position.y, cur.position.z};
        keyedEdit(Tr("위치"),
            [&] { return ImGui::DragFloat3("##areapos", pos, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; }, "position");

        float aim[3] = {cur.aim.x, cur.aim.y, cur.aim.z};
        keyedEdit(Tr("조준점"),
            [&] { return ImGui::DragFloat3("##areaaim", aim, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.aim = {aim[0], aim[1], aim[2]}; }, "aim");

        float sz[2] = {cur.size.x, cur.size.y};
        keyedEdit(Tr("크기"),
            [&] { return ImGui::DragFloat2("##areasize", sz, 0.2f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.size = {std::max(0.01f, sz[0]), std::max(0.01f, sz[1])}; }, "size");

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] { return ui::ColorField("##areacol", col); },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; }, "color");

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##areaintensity", &intensity, 0.05f, 0.0f, 1000.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); }, "intensity");

        float range = cur.range;
        keyedEdit(Tr("범위"),
            [&] { return ImGui::DragFloat("##arearange", &range, 0.5f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.range = std::max(0.1f, range); }, "range");

    } else if (light->kind == LightKind::Ambient) {
        float zenith[3] = {light->skyZenith.x, light->skyZenith.y, light->skyZenith.z};
        basePropEdit(Tr("하늘 천정"),
            [&] { return ui::ColorField("##zenith", zenith); },
            [&](SceneLight& l) { l.skyZenith = {zenith[0], zenith[1], zenith[2]}; });

        float horizon[3] = {light->skyHorizon.x, light->skyHorizon.y, light->skyHorizon.z};
        basePropEdit(Tr("하늘 지평선"),
            [&] { return ui::ColorField("##horizon", horizon); },
            [&](SceneLight& l) { l.skyHorizon = {horizon[0], horizon[1], horizon[2]}; });

        float ground[3] = {light->groundColor.x, light->groundColor.y, light->groundColor.z};
        basePropEdit(Tr("지면"),
            [&] { return ui::ColorField("##ground", ground); },
            [&](SceneLight& l) { l.groundColor = {ground[0], ground[1], ground[2]}; });

        float strength = light->v.intensity;
        basePropEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##ambstrength", &strength, 0.02f, 0.0f, 10.0f, "%.2f"); },
            [&](SceneLight& l) { l.v.intensity = std::max(0.0f, strength); });

    } else if (light->kind == LightKind::Sky) {
        // S9b-1: the Physical Sky light (render/PhysicalSky.h): a Preetham clear sky from the time of day. The sun's
        // direction follows the time unless the scene has a Sun light; the sun's colour always comes from the model.
        float tod = cur.skyTimeOfDay;
        keyedEdit(Tr("시간"),
            [&] { return ImGui::SliderFloat("##skytod", &tod, 0.0f, 24.0f, "%.2f h"); },
            [&](LightValues& v) { v.skyTimeOfDay = std::clamp(tod, 0.0f, 24.0f); }, "sky.timeOfDay");

        float turbidity = cur.skyTurbidity;
        keyedEdit(Tr("탁도"),
            [&] { return ImGui::SliderFloat("##skyturbidity", &turbidity, 1.7f, 10.0f, "%.2f"); },
            [&](LightValues& v) { v.skyTurbidity = std::clamp(turbidity, 1.7f, 10.0f); }, "sky.turbidity");

        float haze = cur.skyHaze;
        keyedEdit(Tr("지평선 안개"),
            [&] { return ImGui::SliderFloat("##skyhaze", &haze, 0.0f, 1.0f, "%.2f"); },
            [&](LightValues& v) { v.skyHaze = std::clamp(haze, 0.0f, 1.0f); }, "sky.haze");

        float north = cur.skyNorthDeg;
        keyedEdit(Tr("방위"),
            [&] { return ImGui::DragFloat("##skynorth", &north, 1.0f, -360.0f, 360.0f, "%.0f°"); },
            [&](LightValues& v) { v.skyNorthDeg = north; }, "sky.northDeg");

        float ground[3] = {cur.skyGround.x, cur.skyGround.y, cur.skyGround.z};
        keyedEdit(Tr("지면 색"),
            [&] { return ui::ColorField("##skyground", ground); },
            [&](LightValues& v) { v.skyGround = {ground[0], ground[1], ground[2]}; }, "sky.ground");

        float skyInt = cur.skyIntensity;
        keyedEdit(Tr("하늘 밝기"),
            [&] { return ImGui::DragFloat("##skyintensity", &skyInt, 0.02f, 0.0f, 20.0f, "%.2f"); },
            [&](LightValues& v) { v.skyIntensity = std::max(0.0f, skyInt); }, "sky.intensity");

        float sunInt = cur.skySunIntensity;
        keyedEdit(Tr("태양 밝기"),
            [&] { return ImGui::DragFloat("##skysunintensity", &sunInt, 0.02f, 0.0f, 20.0f, "%.2f"); },
            [&](LightValues& v) { v.skySunIntensity = std::max(0.0f, sunInt); }, "sky.sunIntensity");

        float maxElev = cur.skyMaxElevationDeg;
        keyedEdit(Tr("최대 고도"),
            [&] { return ImGui::SliderFloat("##skymaxelev", &maxElev, 1.0f, 89.0f, "%.0f°"); },
            [&](LightValues& v) { v.skyMaxElevationDeg = std::clamp(maxElev, 1.0f, 89.0f); }, "sky.maxElevationDeg");

        // S9b-3: the sky's IMAGE (an equirect HDRI / LDR image / Ray-MMD RGBT dome / DDS cubemap): it becomes the
        // background and, through the sky environment buffer, the ambient light. The file / encoding / mirror are base
        // values (read-only while a stage owns the field); rotation and radiance scale are keyable.
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        SectionLabel(Tr("하늘 이미지"));
        {
            const std::string shown = light->skyMap.empty()
                                          ? std::string(Tr("(없음)"))
                                          : PathToUtf8(std::filesystem::path(light->skyMap).filename());
            if (Button("##skymapfile", shown.c_str(), icon::FolderOpen, ButtonKind::Secondary, ImVec2(w / Dpi(), 30.0f))) {
                const std::vector<studio::FileFilter> filters = {
                    {L"하늘 이미지", L"*.hdr;*.png;*.jpg;*.jpeg;*.dds"},
                    {L"HDR", L"*.hdr"}, {L"이미지", L"*.png;*.jpg;*.jpeg"}, {L"DDS", L"*.dds"}};
                const std::filesystem::path picked = studio::OpenFileDialog(GetActiveWindow(), filters);
                if (!picked.empty()) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.skyMap = PathToUtf8(picked); break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                    ++d.projectVersion;
                }
            }
            if (!light->skyMap.empty()) {
                if (Button("##skymapclear", Tr("하늘 이미지 지우기"), nullptr, ButtonKind::Ghost, ImVec2(w / Dpi(), 26.0f))) {
                    std::vector<SceneLight> before = d.lights;
                    std::vector<SceneLight> after = before;
                    for (SceneLight& l : after) {
                        if (l.uid == light->uid) { l.skyMap.clear(); break; }
                    }
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                    ++d.projectVersion;
                }
            }
        }
        if (!light->skyMap.empty()) {
            float rot = cur.skyMapRotationDeg;
            keyedEdit(Tr("회전"),
                [&] { return ImGui::SliderFloat("##skymaprot", &rot, -180.0f, 180.0f, "%.0f°"); },
                [&](LightValues& v) { v.skyMapRotationDeg = rot; }, "sky.mapRotation");

            float mapInt = cur.skyMapIntensity;
            keyedEdit(Tr("세기"),
                [&] { return ImGui::SliderFloat("##skymapint", &mapInt, 0.0f, 8.0f, "%.2f"); },
                [&](LightValues& v) { v.skyMapIntensity = std::max(0.0f, mapInt); }, "sky.mapIntensity");

            bool mirror = light->skyMapMirror;
            if (Switch("##skymapmirror", Tr("좌우 반전"), &mirror, Tr("파노라마를 좌우로 뒤집습니다"))) {
                std::vector<SceneLight> before = d.lights;
                std::vector<SceneLight> after = before;
                for (SceneLight& l : after) {
                    if (l.uid == light->uid) { l.skyMapMirror = mirror; break; }
                }
                d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
                ++d.projectVersion;
            }

            const char* encodings[] = {Tr("자동"), Tr("선형"), Tr("sRGB"), Tr("RGBT")};
            int enc = 0;
            studio::SkyMapEncoding parsed = studio::SkyMapEncoding::Auto;
            if (studio::ParseSkyMapEncoding(light->skyMapEncoding, parsed)) enc = (int)parsed;
            basePropEdit(Tr("인코딩"),
                [&] {
                    bool changed = Segmented("##skymapenc", encodings, 4, &enc, (w - Dp(72.0f)) / Dpi(), 30.0f);
                    return changed;
                },
                [&](SceneLight& l) { l.skyMapEncoding = studio::SkyMapEncodingName((studio::SkyMapEncoding)enc); });
        }

        // S9b-4: the cloud layer on the procedural sky (an image sky draws its own clouds, so these are only used while
        // no image is loaded). Every value is keyable: coverage / opacity / softness, the layer's height and noise size
        // in km, and the wind that drifts it (direction in degrees, speed in m/s over the scene time).
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        SectionLabel(Tr("구름"));
        {
            float coverage = cur.cloudCoverage;
            keyedEdit(Tr("피복"),
                [&] { return ImGui::SliderFloat("##cloudcov", &coverage, 0.0f, 1.0f, "%.2f"); },
                [&](LightValues& v) { v.cloudCoverage = std::clamp(coverage, 0.0f, 1.0f); }, "sky.cloudCoverage");

            float opacity = cur.cloudOpacity;
            keyedEdit(Tr("불투명도"),
                [&] { return ImGui::SliderFloat("##cloudopacity", &opacity, 0.0f, 1.0f, "%.2f"); },
                [&](LightValues& v) { v.cloudOpacity = std::clamp(opacity, 0.0f, 1.0f); }, "sky.cloudOpacity");

            float softness = cur.cloudSoftness;
            keyedEdit(Tr("부드러움"),
                [&] { return ImGui::SliderFloat("##cloudsoft", &softness, 0.02f, 1.0f, "%.2f"); },
                [&](LightValues& v) { v.cloudSoftness = std::clamp(softness, 0.02f, 1.0f); }, "sky.cloudSoftness");

            float height = cur.cloudHeightKm;
            keyedEdit(Tr("높이 (km)"),
                [&] { return ImGui::SliderFloat("##cloudheight", &height, 0.3f, 12.0f, "%.2f km"); },
                [&](LightValues& v) { v.cloudHeightKm = std::clamp(height, 0.3f, 12.0f); }, "sky.cloudHeight");

            float scale = cur.cloudScaleKm;
            keyedEdit(Tr("크기 (km)"),
                [&] { return ImGui::SliderFloat("##cloudscale", &scale, 0.2f, 20.0f, "%.2f km"); },
                [&](LightValues& v) { v.cloudScaleKm = std::clamp(scale, 0.2f, 20.0f); }, "sky.cloudScale");

            float windDeg = cur.cloudWindDeg;
            keyedEdit(Tr("바람 방향"),
                [&] { return ImGui::DragFloat("##cloudwinddeg", &windDeg, 1.0f, -360.0f, 360.0f, "%.0f°"); },
                [&](LightValues& v) { v.cloudWindDeg = std::clamp(windDeg, -360.0f, 360.0f); }, "sky.cloudWindDeg");

            float windSpeed = cur.cloudWindSpeed;
            keyedEdit(Tr("바람 속도"),
                [&] { return ImGui::SliderFloat("##cloudwindspeed", &windSpeed, 0.0f, 100.0f, "%.1f m/s"); },
                [&](LightValues& v) { v.cloudWindSpeed = std::clamp(windSpeed, 0.0f, 100.0f); }, "sky.cloudWindSpeed");
        }

        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextWrapped("%s", Tr("하늘 조명은 배경, 안개 색과 태양 색을 함께 정합니다. 태양 조명이 있으면 그 방향을 씁니다."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    } else if (light->kind == LightKind::FogVolume) {
        // S9: a local medium volume. It lights nothing; its density adds to the global haze inside the shape.
        float pos[3] = {cur.position.x, cur.position.y, cur.position.z};
        keyedEdit(Tr("위치"),
            [&] { return ImGui::DragFloat3("##fogpos", pos, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; }, "position");

        float ext[3] = {cur.extent.x, cur.extent.y, cur.extent.z};
        keyedEdit(Tr("크기"),
            [&] { return ImGui::DragFloat3("##fogextent", ext, 0.5f, 0.5f, 5000.0f, "%.1f"); },
            [&](LightValues& v) {
                v.extent = {std::max(0.5f, ext[0]), std::max(0.5f, ext[1]), std::max(0.5f, ext[2])};
            }, "extent");

        float density = cur.intensity;
        keyedEdit(Tr("밀도"),
            [&] { return ImGui::DragFloat("##fogdensity", &density, 0.02f, 0.0f, 20.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, density); }, "intensity");

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] { return ui::ColorField("##fogcol", col); },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; }, "color");

        const char* shapes[] = {Tr("상자"), Tr("구")};
        int shape = (int)light->fogShape;
        basePropEdit(Tr("모양"), [&] {
            bool changed = Segmented("##fogshape", shapes, 2, &shape, (w - Dp(72.0f)) / Dpi(), 30.0f);
            return changed;
        }, [&](SceneLight& l) { l.fogShape = (FogShape)shape; });

        float edge = light->fogEdge;
        basePropEdit(Tr("가장자리"),
            [&] { return ImGui::SliderFloat("##fogedge", &edge, 0.0f, 1.0f, "%.2f"); },
            [&](SceneLight& l) { l.fogEdge = std::clamp(edge, 0.0f, 1.0f); });
    }

    // Shadow section (sun, point, spot)
    if (light->kind != LightKind::Ambient && light->kind != LightKind::FogVolume &&
        light->kind != LightKind::Sky) {
        separator(8.0f);
        SectionLabel(Tr("그림자"));

        const char* shadowModes[] = {Tr("없음"), Tr("하드"), Tr("소프트")};
        int shadowType = (int)light->shadow;
        if (Segmented("##shadowtype", shadowModes, 3, &shadowType, w / Dpi(), 32.0f)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.shadow = (ShadowType)shadowType; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));

        if (light->shadow == ShadowType::Soft) {
            float softness = light->shadowSoftness;
            basePropEdit(Tr("부드러움"),
                [&] { return ImGui::SliderFloat("##softness", &softness, 0.0f, 1.0f, "%.2f"); },
                [&](SceneLight& l) { l.shadowSoftness = std::clamp(softness, 0.0f, 1.0f); }, "shadowSoftness");
        }

        float density = light->shadowDensity * 100.0f;
        basePropEdit(Tr("농도"),
            [&] { return ImGui::SliderFloat("##density", &density, 0.0f, 100.0f, "%.0f %%"); },
            [&](SceneLight& l) { l.shadowDensity = std::clamp(density / 100.0f, 0.0f, 1.0f); }, "shadowDensity");

        float shadowCol[3] = {light->shadowColor.x, light->shadowColor.y, light->shadowColor.z};
        basePropEdit(Tr("그림자 색"),
            [&] { return ui::ColorField("##shadowcol", shadowCol); },
            [&](SceneLight& l) { l.shadowColor = {shadowCol[0], shadowCol[1], shadowCol[2]}; }, "shadowColor");

        if (light->kind == LightKind::Point) {
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            PushFont(Font::Regular, size::Caption);
            ImGui::TextWrapped("%s", Tr("래스터에서는 그림자를 켠 점광원이 많아지면 점광원 그림자 해상도가 낮아집니다."));
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
    }

    // Falloff (point, spot, area)
    if (light->kind == LightKind::Point || light->kind == LightKind::Spot || light->kind == LightKind::Area) {
        separator(8.0f);
        SectionLabel(Tr("감쇠"));

        // "None" is the engine's default curve (a range window x 1/(1 + 0.0004 d^2)), not a constant: label it so.
        const char* falloffs[] = {Tr("기본"), Tr("선형"), Tr("역제곱")};
        int falloff = (int)light->falloff;
        if (Segmented("##falloff", falloffs, 3, &falloff, w / Dpi(), 32.0f)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.falloff = (FalloffType)falloff; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    }

    // S9: haze (every light that shows in the medium: sun, point, spot, area)
    if (light->kind == LightKind::Sun || light->kind == LightKind::Point || light->kind == LightKind::Spot ||
        light->kind == LightKind::Area) {
        separator(8.0f);
        SectionLabel(Tr("안개"));

        float scattering = cur.volumetric;
        keyedEdit(Tr("안개 산란"),
            [&] { return ImGui::SliderFloat("##volumetric", &scattering, 0.0f, 20.0f, "%.2f"); },
            [&](LightValues& v) { v.volumetric = std::max(0.0f, scattering); }, "volumetric");
        // unchecked = the fog's own anisotropy (-1); checked = this light's g (negative values scatter backwards)
        bool ownG = light->volumetricG > -0.99f;
        float aniso = ownG ? light->volumetricG : 0.6f;
        basePropEdit(Tr("산란 방향"),
            [&] {
                bool changed = ImGui::Checkbox("##volumetricgown", &ownG);
                ImGui::SameLine();
                if (ownG) changed |= ImGui::SliderFloat("##volumetricg", &aniso, -0.9f, 0.95f, "%.2f");
                else ImGui::TextDisabled("%s", Tr("안개 따름"));
                return changed;
            },
            [&](SceneLight& l) { l.volumetricG = ownG ? std::clamp(aniso, -0.9f, 0.95f) : -1.0f; });
    }

    if (light->kind != LightKind::Ambient && light->kind != LightKind::FogVolume && light->kind != LightKind::Sky) {
        separator(8.0f);
        bool diff = light->affectDiffuse;
        if (Switch("##diffuse", Tr("디퓨즈 영향"), &diff)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.affectDiffuse = diff; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
        bool spec = light->affectSpecular;
        if (Switch("##specular", Tr("스펙큘러 영향"), &spec)) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.affectSpecular = spec; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
        bool vis = light->viewportVisible;
        if (Switch("##visible", Tr("뷰포트 표시"), &vis, Tr("에디터 전용이며 렌더에는 영향이 없습니다"))) {
            std::vector<SceneLight> before = d.lights;
            std::vector<SceneLight> after = before;
            for (SceneLight& l : after) {
                if (l.uid == light->uid) { l.viewportVisible = vis; break; }
            }
            d.history.Push(std::make_unique<LightsCommand>(d, Tr("조명 편집"), before, after));
            ++d.projectVersion;
        }
    }

    // Keys section (sun, point, spot)
    if (light->kind != LightKind::Ambient) {
        separator(8.0f);
        const bool keyed = FindKey(light->keys, d.Frame()) != nullptr;
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, Tr("키"));

        std::snprintf(buf, sizeof(buf), Tr("키 %d개"), (int)light->keys.size());
        const ImVec2 ks = TextSize(Font::Regular, size::Caption, buf);
        Text(cdl, Font::Regular, size::Caption, ImVec2(c.x + w - ks.x - Dp(20.0f), c.y + Dp(2.0f)), p.ink3, buf);

        // diamond indicator
        const ImVec2 dc(c.x + w - Dp(8.0f), c.y + Dp(8.0f));
        const float dr = Dp(4.5f);
        const ImU32 kc = keyed ? p.accent : p.ink3;
        const ImVec2 pts[4] = {{dc.x, dc.y - dr}, {dc.x + dr, dc.y}, {dc.x, dc.y + dr}, {dc.x - dr, dc.y}};
        if (keyed) cdl->AddConvexPolyFilled(pts, 4, kc);
        else cdl->AddPolyline(pts, 4, kc, ImDrawFlags_Closed, 1.5f);

        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));

        Switch("##autokey", Tr("자동 키"), &d.autoKey, Tr("값을 고치면 재생 헤드에 키가 생겨요"));
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));

        if (Button("##regkey", Tr("현재 프레임에 키 등록"), icon::Plus, ButtonKind::Secondary, ImVec2(w / Dpi(), 34.0f))) {
            StudioInsertKeys({MakeRowId(RowKind::SceneLight, 0, light->uid)}, d.Frame());
        }
    }

    // the destructive action sits last, below everything it would throw away (undoable)
    separator(8.0f);
    if (Button("##deletelight", Tr("조명 삭제"), icon::Trash, ButtonKind::Danger, ImVec2(w / Dpi(), btnH))) {
        StudioDeleteLight(light->uid);
        return;
    }
    ImGui::Dummy(ImVec2(w, Dp(16.0f)));
}

} // namespace mmdx
