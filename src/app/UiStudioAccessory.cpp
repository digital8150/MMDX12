// Studio accessory (prop) keys: MMD-style accessory placement animation. The keys live in the prop model's own
// MotionData (never in a VMD), are evaluated by studio::AccessoryPlacement / AccessoryOpacity / AccessoryVisible and
// drive StudioPropRoot (placement) and the per-model opacity multiplier (UiStudio.cpp StudioUpdateModel). This file
// holds the inspector fields of an accessory key and the undoable insert / edit helpers, sharing the same
// StudioBegin/EndKeyEdit undo path the focus and camera key fields use.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui.h"

namespace mmdx {

using namespace studio;

// The prop's placement at `frame` as a key: the evaluated accessory values (or the static attach without keys).
AccessoryKf App::StudioNewAccessoryKey(const StudioModel& m, int frame) const {
    const float f = (float)frame;
    const PropAttach p = AccessoryPlacement(m, f);
    AccessoryKf k;
    k.frame = frame;
    k.translation = p.translation;
    k.rotationDeg = p.rotationDeg;
    k.scale = p.scale;
    k.parentUid = p.parent < 0 ? 0 : (uint32_t)p.parent;
    k.bone = p.bone;
    k.opacity = AccessoryOpacity(m, f);
    k.visible = AccessoryVisible(m, f);
    return k;
}

void App::StudioSetAccessoryAtPlayhead(const AccessoryKf& key, const char* undoName) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || !m->IsProp()) return;
    std::vector<TrackState> before{CaptureTrack(d, d.selectedModel, RowKind::Accessory, "")};
    AccessoryKf k = key;
    k.frame = d.Frame();
    if (const AccessoryKf* cur = FindKey(m->motion.accessory, k.frame); cur && *cur == k) return;
    UpsertKey(m->motion.accessory, k);
    StudioPushTrackEdit(undoName, before);
    d.selection = {{MakeRowId(RowKind::Accessory, 0, 0), k.frame}};
    d.rowsKey = ~0ull;
}

bool App::DrawStudioAccessoryKeyFields(float w, int frame) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || !m->IsProp()) { StudioEndKeyEdit(); return true; }
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    AccessoryKf* k = FindKey(m->motion.accessory, frame);
    if (!k) { StudioEndKeyEdit(); return true; }
    // a key edit captures the prop's accessory track (the model-specific twin of StudioBeginKeyEdit)
    const auto begin = [&] {
        if (studioKeyEdit_) return;
        studioKeyBefore_ = {CaptureTrack(d, d.selectedModel, RowKind::Accessory, "")};
        studioKeyEdit_ = true;
        studioKeyChanged_ = false;
    };
    bool ended = false;
    const auto row = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(76.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(76.0f));
        PushFont(Font::Regular, size::Small);
        const bool changed = widget();
        PopFont();
        if (ImGui::IsItemActivated()) begin();
        ended |= ItemEditEnded();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        return changed;
    };

    // parent combo (stepped): world or another model + its bone
    {
        const int parentIndex = k->parentUid != 0 ? d.IndexOfUid(k->parentUid) : -1;
        const StudioModel* parent = parentIndex >= 0 ? d.models[(size_t)parentIndex].get() : nullptr;
        row(Tr("부모"), [&] {
            bool changed = false;
            if (ImGui::BeginCombo("##accparent", parent ? parent->name.c_str() : Tr("월드 (없음)"))) {
                if (ImGui::Selectable(Tr("월드 (없음)"), !parent) && parent) {
                    k->parentUid = 0;
                    k->bone.clear();
                    changed = true;
                }
                for (int i = 0; i < (int)d.models.size(); ++i) {
                    const StudioModel& o = *d.models[(size_t)i];
                    if (&o == m || o.IsProp()) continue;  // one level: props follow characters / stages
                    ImGui::PushID(i);
                    if (ImGui::Selectable(o.name.c_str(), parent == &o) && parent != &o) {
                        k->parentUid = o.uid;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return changed;
        });
        if (parent) {
            row(Tr("본"), [&] {
                bool changed = false;
                if (ImGui::BeginCombo("##accbone", k->bone.empty() ? Tr("원점") : k->bone.c_str(), ImGuiComboFlags_HeightLarge)) {
                    if (ImGui::Selectable(Tr("원점"), k->bone.empty()) && !k->bone.empty()) { k->bone.clear(); changed = true; }
                    for (size_t b = 0; b < parent->pmx->bones.size(); ++b) {
                        const std::string& bn = parent->pmx->bones[b].name;
                        ImGui::PushID((int)b);
                        if (ImGui::Selectable(bn.c_str(), bn == k->bone) && bn != k->bone) { k->bone = bn; changed = true; }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                return changed;
            });
        }
    }

    // a change opens an edit when none is open (begin() joins an open one): a field activated as another field's edit
    // ends only joins it
    float t[3] = {k->translation.x, k->translation.y, k->translation.z};
    if (row(Tr("위치"), [&] { return ImGui::DragFloat3("##acct", t, 0.02f, 0.0f, 0.0f, "%.2f"); })) {
        begin();
        k->translation = {t[0], t[1], t[2]};
        studioKeyChanged_ = true;
    }
    float r[3] = {k->rotationDeg.x, k->rotationDeg.y, k->rotationDeg.z};
    if (row(Tr("회전"), [&] { return ImGui::DragFloat3("##accr", r, 0.25f, 0.0f, 0.0f, "%.1f°"); })) {
        begin();
        k->rotationDeg = {r[0], r[1], r[2]};
        studioKeyChanged_ = true;
    }
    float s = k->scale;
    if (row(Tr("크기"), [&] { return ImGui::DragFloat("##accs", &s, 0.005f, 0.01f, 100.0f, "%.3f"); })) {
        begin();
        k->scale = std::clamp(s, 0.01f, 100.0f);
        studioKeyChanged_ = true;
    }
    float op = k->opacity;
    if (row(Tr("불투명도"), [&] { return ImGui::SliderFloat("##accop", &op, 0.0f, 1.0f, "%.2f"); })) {
        begin();
        k->opacity = std::clamp(op, 0.0f, 1.0f);
        studioKeyChanged_ = true;
    }
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(76.0f), c.y));
        bool vis = k->visible;
        const bool before = vis;
        Switch("##accvis", Tr("보이기"), &vis, Tr("끄면 이 구간에서 소품이 숨어요"));
        if (vis != before) {
            begin();
            k->visible = vis;
            studioKeyChanged_ = true;
        }
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }
    if (ended) StudioEndKeyEdit();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    Text(cdl, Font::Regular, size::Caption, c, p.ink3, Tr("소품 키는 다음 키까지 유지돼요. 보이기와 부모는 계단식, 나머지는 선형이에요."));
    ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    return true;
}

// The prop inspector's accessory section: the key track summary, the evaluated values at the playhead and the
// "key at playhead" button (the same insertion StudioInsertKeys does, so the timeline and the undo history agree).
void App::DrawStudioAccessorySection(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || !m->IsProp()) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));
    cdl->AddLine(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y), p.line);
    ImGui::Dummy(ImVec2(w, Dp(10.0f)));

    const float frame = (float)d.Frame();
    const bool keyed = !m->motion.accessory.empty();
    char buf[160];
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const char* title = Tr("소품 키 (배치 애니메이션)");
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, title);
        std::snprintf(buf, sizeof(buf), Tr("키 %d개"), (int)m->motion.accessory.size());
        const float tw = TextSize(Font::Semibold, size::Caption, title).x;
        Text(cdl, Font::Regular, size::Caption, ImVec2(c.x + tw + Dp(8.0f), c.y + Dp(1.0f)), p.ink3, buf);
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    if (keyed) {
        std::snprintf(buf, sizeof(buf), Tr("불투명도 %.2f · %s"), AccessoryOpacity(*m, frame),
                      AccessoryVisible(*m, frame) ? Tr("보임") : Tr("숨김"));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(cdl, Font::Regular, size::Small, c, c.x + w, p.ink2, buf);
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    } else {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        (void)c;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::TextWrapped("%s", Tr("키가 없으면 위의 배치가 그대로 쓰여요. 키를 등록하면 프레임마다 애니메이션돼요."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }
    if (Button("##acckey", Tr("키 등록"), icon::Plus, ButtonKind::Ghost, ImVec2(w / Dpi(), 30.0f)))
        StudioInsertKeys({MakeRowId(RowKind::Accessory, 0, 0)}, d.Frame());
    Tooltip(Tr("지금 배치를 재생 헤드에 소품 키로 등록 (다음 키까지 유지)"));
    ImGui::Dummy(ImVec2(w, Dp(4.0f)));
}

} // namespace mmdx
