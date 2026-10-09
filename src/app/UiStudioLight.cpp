// Studio scene light UI: outliner group and rows, preset confirmation modal, and the light inspector.
// Keyed edits follow the keyed edit rule (playhead key -> auto-key insert -> base values).
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/SceneLight.h"
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
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
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
    } else if (kind == LightKind::Sun) {
        l.name = "메인 조명";
    } else if (kind == LightKind::Ambient) {
        l.name = "환경광";
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


// ---------------------------------------------------------------------------
// Outliner
// ---------------------------------------------------------------------------

void App::DrawStudioLightOutliner() {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = Dp(40.0f);

    // Group header: "조명" with preset menu button and "+" add menu button
    {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float headH = Dp(32.0f);
        const ImVec2 b(a.x + w, a.y + headH);
        Text(dl, Font::Semibold, size::Caption, ImVec2(a.x + Dp(16.0f), a.y + Dp(8.0f)), p.ink3, Tr("조명"));

        // Preset menu button
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(56.0f), a.y + Dp(2.0f)));
        if (IconButton("##lightpresets", icon::Sliders, Tr("조명 프리셋"), false, 28.0f)) {
            ImGui::OpenPopup("##lightpresetspopup");
        }

        // Add menu button "+"
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(28.0f), a.y + Dp(2.0f)));
        if (IconButton("##addlight", icon::Plus, Tr("조명 추가"), false, 28.0f)) {
            ImGui::OpenPopup("##addlightpopup");
        }

        if (ImGui::BeginPopup("##lightpresetspopup")) {
            if (MenuItem("##pre_0", Tr("스튜디오"), icon::Sun)) {
                studioLightPresetPending_ = 0;
                ImGui::CloseCurrentPopup();
            }
            if (MenuItem("##pre_1", Tr("노을"), icon::Sun)) {
                studioLightPresetPending_ = 1;
                ImGui::CloseCurrentPopup();
            }
            if (MenuItem("##pre_2", Tr("콘서트"), icon::MicrophoneStage)) {
                studioLightPresetPending_ = 2;
                ImGui::CloseCurrentPopup();
            }
            if (MenuItem("##pre_3", Tr("밤"), icon::Moon)) {
                studioLightPresetPending_ = 3;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##addlightpopup")) {
            if (MenuItem("##add_pt", Tr("점광원"), icon::Lightbulb)) {
                StudioAddLight(LightKind::Point);
                ImGui::CloseCurrentPopup();
            }
            if (MenuItem("##add_sp", Tr("스팟"), icon::Aperture)) {
                StudioAddLight(LightKind::Spot);
                ImGui::CloseCurrentPopup();
            }
            bool hasSun = false, hasAmbient = false;
            for (const auto& l : d.lights) {
                if (l.kind == LightKind::Sun) hasSun = true;
                if (l.kind == LightKind::Ambient) hasAmbient = true;
            }
            if (!hasSun) {
                if (MenuItem("##add_sun", Tr("메인 조명"), icon::Sun)) {
                    StudioAddLight(LightKind::Sun);
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!hasAmbient) {
                if (MenuItem("##add_amb", Tr("환경광"), icon::Globe)) {
                    StudioAddLight(LightKind::Ambient);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(w, 0.0f));
    }

    // Light rows
    for (size_t i = 0; i < d.lights.size(); ++i) {
        SceneLight& l = d.lights[i];
        ImGui::PushID((int)l.uid);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + w, a.y + rowH);
        const bool selected = (d.selectedModel == -1 && d.selectedLightUid == l.uid);

        ImGui::InvisibleButton("##row", ImVec2(w - Dp(40.0f), rowH));
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
        else if (l.kind == LightKind::Ambient) { glyph = icon::Globe; sub = Tr("환경광"); }

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
        TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, l.name.c_str());
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

    // Helper for editing a keyable field according to Keyed Edit Rule
    auto keyedEdit = [&](const char* label, auto&& widget, auto&& apply) {
        const int frame = d.Frame();
        LightKey* k = FindKey(light->keys, frame);
        const bool isKeyed = (k != nullptr);
        const bool willKey = isKeyed || d.autoKey;

        bool changed = row(label, [&] { return widget(); });

        if (ImGui::IsItemActivated()) {
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
        if (ImGui::IsItemDeactivated()) {
            StudioEndLightEdit(light->uid);
        }
        return changed;
    };

    // Helper for editing a continuous base property
    auto basePropEdit = [&](const char* label, auto&& widget, auto&& apply) {
        bool changed = row(label, [&] { return widget(); });
        if (ImGui::IsItemActivated()) {
            StudioBeginLightEdit(light->uid, false);
        }
        if (changed) {
            studioLightChanged_ = true;
            apply(*light);
            d.rowsKey = ~0ull;
        }
        if (ImGui::IsItemDeactivated()) {
            StudioEndLightEdit(light->uid);
        }
        return changed;
    };

    // Header: Kind label
    {
        const char* kindLabel = light->kind == LightKind::Sun ? Tr("태양") :
                                light->kind == LightKind::Point ? Tr("점광원") :
                                light->kind == LightKind::Spot ? Tr("스팟") : Tr("환경광");
        Text(cdl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.ink3, kindLabel);
        ImGui::Dummy(ImVec2(w, Dp(16.0f)));
    }

    // Rename (undoable)
    {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", light->name.c_str());
        row(Tr("이름"), [&] {
            if (ImGui::InputText("##lightname", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit()) {
                if (nameBuf[0] != '\0' && light->name != nameBuf) {
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

    // Delete button (undoable)
    if (Button("##deletelight", Tr("조명 삭제"), icon::Trash, ButtonKind::Danger, ImVec2(w / Dpi(), btnH))) {
        StudioDeleteLight(light->uid);
        return;
    }


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
                ImGui::DragFloat3("##suncol", col, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::ColorEdit3("##suncolpick", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
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
                [&](LightValues& v) { v.direction = {dir[0], dir[1], dir[2]}; });

            float col[3] = {cur.color.x, cur.color.y, cur.color.z};
            keyedEdit(Tr("색"),
                [&] {
                    bool c = false;
                    c |= ImGui::DragFloat3("##suncol", col, 0.01f, 0.0f, 1.0f, "%.2f");
                    ImGui::SameLine();
                    c |= ImGui::ColorEdit3("##suncolpick", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                    return c;
                },
                [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; });
        }

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##sunintensity", &intensity, 0.02f, 0.0f, 100.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); });

        float rimStr = light->rimStrength;
        basePropEdit(Tr("림 강도"),
            [&] { return ImGui::DragFloat("##rimstr", &rimStr, 0.01f, 0.0f, 10.0f, "%.2f"); },
            [&](SceneLight& l) { l.rimStrength = std::max(0.0f, rimStr); });

        float rimCol[3] = {light->rimColor.x, light->rimColor.y, light->rimColor.z};
        basePropEdit(Tr("림 색"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##rimcol", rimCol, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##rimcolpick", rimCol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](SceneLight& l) { l.rimColor = {rimCol[0], rimCol[1], rimCol[2]}; });

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
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; });

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##pointcol", col, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##pointcolpick", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; });

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##pointintensity", &intensity, 0.05f, 0.0f, 1000.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); });

        float range = cur.range;
        keyedEdit(Tr("범위"),
            [&] { return ImGui::DragFloat("##pointrange", &range, 0.5f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.range = std::max(0.1f, range); });

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
            std::string currentTargetName = Tr("퍼포머");
            if (light->targetUid != 0) {
                const int idx = d.IndexOfUid(light->targetUid);
                if (idx >= 0) currentTargetName = d.models[(size_t)idx]->name;
            }
            row(Tr("타겟 캐릭터"), [&] {
                if (ImGui::BeginCombo("##targetchar", currentTargetName.c_str())) {
                    const bool selPerformer = (light->targetUid == 0);
                    if (ImGui::Selectable(Tr("퍼포머"), selPerformer)) {
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
                [&](LightValues& v) { v.aim = {aim[0], aim[1], aim[2]}; });
        }

        float pos[3] = {cur.position.x, cur.position.y, cur.position.z};
        keyedEdit(Tr("위치"),
            [&] { return ImGui::DragFloat3("##spotpos", pos, 0.1f, 0.0f, 0.0f, "%.1f"); },
            [&](LightValues& v) { v.position = {pos[0], pos[1], pos[2]}; });

        float col[3] = {cur.color.x, cur.color.y, cur.color.z};
        keyedEdit(Tr("색"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##spotcol", col, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##spotcolpick", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](LightValues& v) { v.color = {col[0], col[1], col[2]}; });

        float intensity = cur.intensity;
        keyedEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##spotintensity", &intensity, 0.05f, 0.0f, 1000.0f, "%.2f"); },
            [&](LightValues& v) { v.intensity = std::max(0.0f, intensity); });

        float range = cur.range;
        keyedEdit(Tr("범위"),
            [&] { return ImGui::DragFloat("##spotrange", &range, 0.5f, 0.1f, 10000.0f, "%.1f"); },
            [&](LightValues& v) { v.range = std::max(0.1f, range); });

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
            });
        keyedEdit(Tr("내부 각도"),
            [&] { return ImGui::DragFloat("##coneinner", &innerDeg, 0.2f, 0.5f, outerDeg, "%.1f°"); },
            [&](LightValues& v) {
                float curOut = DirectX::XMConvertToDegrees(v.coneOuter);
                innerDeg = std::clamp(innerDeg, 0.5f, curOut);
                v.coneInner = DirectX::XMConvertToRadians(innerDeg);
            });

    } else if (light->kind == LightKind::Ambient) {
        float zenith[3] = {light->skyZenith.x, light->skyZenith.y, light->skyZenith.z};
        basePropEdit(Tr("하늘 천정"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##zenith", zenith, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##zenithpick", zenith, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](SceneLight& l) { l.skyZenith = {zenith[0], zenith[1], zenith[2]}; });

        float horizon[3] = {light->skyHorizon.x, light->skyHorizon.y, light->skyHorizon.z};
        basePropEdit(Tr("하늘 지평선"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##horizon", horizon, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##horizonpick", horizon, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](SceneLight& l) { l.skyHorizon = {horizon[0], horizon[1], horizon[2]}; });

        float ground[3] = {light->groundColor.x, light->groundColor.y, light->groundColor.z};
        basePropEdit(Tr("지면"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##ground", ground, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##groundpick", ground, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](SceneLight& l) { l.groundColor = {ground[0], ground[1], ground[2]}; });

        float strength = light->v.intensity;
        basePropEdit(Tr("강도"),
            [&] { return ImGui::DragFloat("##ambstrength", &strength, 0.02f, 0.0f, 10.0f, "%.2f"); },
            [&](SceneLight& l) { l.v.intensity = std::max(0.0f, strength); });
    }

    // Shadow section (sun, point, spot)
    if (light->kind != LightKind::Ambient) {
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
                [&](SceneLight& l) { l.shadowSoftness = std::clamp(softness, 0.0f, 1.0f); });
        }

        float density = light->shadowDensity * 100.0f;
        basePropEdit(Tr("농도"),
            [&] { return ImGui::SliderFloat("##density", &density, 0.0f, 100.0f, "%.0f %%"); },
            [&](SceneLight& l) { l.shadowDensity = std::clamp(density / 100.0f, 0.0f, 1.0f); });

        float shadowCol[3] = {light->shadowColor.x, light->shadowColor.y, light->shadowColor.z};
        basePropEdit(Tr("그림자 색"),
            [&] {
                bool c = false;
                c |= ImGui::DragFloat3("##shadowcol", shadowCol, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                c |= ImGui::ColorEdit3("##shadowcolpick", shadowCol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                return c;
            },
            [&](SceneLight& l) { l.shadowColor = {shadowCol[0], shadowCol[1], shadowCol[2]}; });

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

    // Falloff (point and spot)
    if (light->kind == LightKind::Point || light->kind == LightKind::Spot) {
        separator(8.0f);
        SectionLabel(Tr("감쇠"));

        const char* falloffs[] = {Tr("없음"), Tr("선형"), Tr("역제곱")};
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

    // Switches (point, spot, sun)
    if (light->kind != LightKind::Ambient) {
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

    ImGui::Dummy(ImVec2(w, Dp(16.0f)));
}

} // namespace mmdx
