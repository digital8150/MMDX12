// Studio depth-of-field focus: the per-frame focus (the focus track over the automatic focus, studio/StudioFocus.h)
// written into FrameView::focusDistance / apertureScale for the viewport and the offline renders, the camera panel's
// focus section (what is in focus, the segment picker at the playhead) and the inspector fields of a focus key.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui.h"
#include "render/ShaderPack.h"

namespace mmdx {

using namespace studio;

namespace {
constexpr float kRenderAspect = 16.0f / 9.0f;  // every video size of the render dialog is 16:9
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";      // センター
const char* const kHeadBone = "\xE9\xA0\xAD";                                            // 頭
const char* const kUpperBody2 = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB" "2";              // 上半身2
const char* const kUpperBody = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";                   // 上半身

uint64_t FocusRow() { return MakeRowId(RowKind::Focus, 0, 0); }

bool BonePos(const StudioModel& m, const char* name, DirectX::XMFLOAT3& out) {
    const int b = m.pmx->FindBone(name);
    if (b < 0) return false;
    out = m.inst->BoneWorldPosition(b);
    return true;
}

// World position of a focus bone; models without it fall back along head -> upper body -> centre (+ offsets).
DirectX::XMFLOAT3 FocusBonePos(const StudioModel& m, FocusBone bone) {
    DirectX::XMFLOAT3 c{0, 10, 0}, p;
    if (!BonePos(m, kCenterBone, c) && !m.pmx->bones.empty()) {
        c = m.inst->BoneWorldPosition(0);
        c.y += 10.0f;
    }
    switch (bone) {
    case FocusBone::Head:
        if (BonePos(m, kHeadBone, p)) return p;
        return {c.x, c.y + 8.0f, c.z};
    case FocusBone::UpperBody:
        if (BonePos(m, kUpperBody2, p) || BonePos(m, kUpperBody, p)) return p;
        return {c.x, c.y + 4.0f, c.z};
    default:
        return c;
    }
}

// The key whose segment contains `frame` (the first key before it); nullptr without keys.
const FocusKf* SegmentKey(const std::vector<FocusKf>& keys, int frame) {
    if (keys.empty()) return nullptr;
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](int f, const FocusKf& k) { return f < k.frame; });
    return it == keys.begin() ? &keys.front() : &*(it - 1);
}
} // namespace

std::string App::StudioModelLabel(uint32_t uid) const {
    const int i = studio_ ? studio_->IndexOfUid(uid) : -1;
    return i >= 0 ? studio_->models[(size_t)i]->name : std::string();
}

bool App::StudioFocusTargetZ(uint32_t uid, FocusBone bone, const CameraParams& cam, float& z) const {
    const int i = studio_->IndexOfUid(uid);
    if (i < 0) return false;
    const StudioModel& m = *studio_->models[(size_t)i];
    if (!m.pmx || !m.inst) return false;
    const DirectX::XMFLOAT3 p = FocusBonePos(m, bone);
    z = DirectX::XMVectorGetZ(
        DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&p), DirectX::XMLoadFloat4x4(&cam.view)));
    return z > cam.nearZ;
}

void App::StudioComputeFocus(FrameView& view) {
    StudioDoc& d = *studio_;
    std::vector<FocusSubject> subjects;
    const StudioModel* performer = nullptr;
    for (const auto& m : d.models) {
        if (!m->visible || m->kind != ModelKind::Character || !m->pmx || !m->inst) continue;
        if (!performer) performer = m.get();
        subjects.push_back({m->uid, FocusBonePos(*m, FocusBone::Head), FocusBonePos(*m, FocusBone::UpperBody),
                            FocusBonePos(*m, FocusBone::Center)});
    }
    FocusView fv;
    fv.view = view.camera.view;
    fv.eye = view.camera.eye;
    fv.tanHalfFovY = std::tan(view.camera.fovYRadians * 0.5f);
    fv.aspect = kRenderAspect;
    fv.nearZ = view.camera.nearZ;
    float autoZ = studioAutoFocus_.Update(d.time, subjects, fv);
    uint32_t autoSubject = studioAutoFocus_.Subject();
    float z = 0.0f;
    if (autoZ <= 0.0f && performer && StudioFocusTargetZ(performer->uid, FocusBone::Head, view.camera, z)) {
        autoZ = z;  // nobody in frame: the first character's head, as before the automatic focus
        autoSubject = performer->uid;
    }
    const float frame = (float)(d.time * kMmdFps);
    const FocusResult r = EvaluateFocus(
        d.camera.focus, frame,
        [&](uint32_t uid, FocusBone bone, float& out) { return StudioFocusTargetZ(uid, bone, view.camera, out); }, autoZ);
    view.focusDistance = r.distance > view.camera.nearZ ? r.distance : 0.0f;
    view.apertureScale = r.aperture;
    studioFocusShown_ = view.focusDistance;
    studioFocusAperture_ = r.aperture;
    studioFocusSubject_ = autoSubject;
    if (const FocusKf* k = SegmentKey(d.camera.focus, (int)std::floor(frame + 1e-4f))) {
        if (k->mode == FocusMode::Manual) studioFocusSubject_ = 0;
        else if (k->mode == FocusMode::Target && k->target != 0 && StudioFocusTargetZ(k->target, k->bone, view.camera, z))
            studioFocusSubject_ = k->target;
    }
}

FocusKf App::StudioNewFocusKey(int frame) const {
    const StudioDoc& d = *studio_;
    FocusKf k;
    if (const FocusKf* seg = SegmentKey(d.camera.focus, frame)) {
        k = *seg;  // the segment in effect continues (a key is a cut point to edit from)
    } else if (studioFocusSubject_ != 0) {
        k.mode = FocusMode::Target;  // the first key locks what is in focus now
        k.target = studioFocusSubject_;
    } else if (studioFocusShown_ > 0.0f) {
        k.mode = FocusMode::Manual;
        k.distance = studioFocusShown_;
    }
    k.frame = frame;
    return k;
}

void App::StudioSetFocusAtPlayhead(const FocusKf& key, const char* undoName) {
    StudioDoc& d = *studio_;
    std::vector<TrackState> before{CaptureTrack(d, -1, RowKind::Focus, "")};
    FocusKf k = key;
    k.frame = d.Frame();
    if (const FocusKf* cur = FindKey(d.camera.focus, k.frame); cur && *cur == k) return;
    UpsertKey(d.camera.focus, k);
    StudioPushTrackEdit(undoName, before);
    d.selection = {{FocusRow(), k.frame}};
    d.rowsKey = ~0ull;
}

void App::DrawStudioFocusSection(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float keyW = 84.0f;
    // an effect pack that replaces the depth of field (bokeh_dof ...) follows the focus track as well
    const bool dofOn = settings_.dof || EffectStackReplacesDof(settings_.effectStack);
    char buf[200];

    const ImVec2 t0 = ImGui::GetCursorScreenPos();
    Text(cdl, Font::Semibold, size::Small, t0, p.ink2, Tr("초점"));
    std::snprintf(buf, sizeof(buf), Tr("키 %d개"), (int)d.camera.focus.size());
    const float tw = TextSize(Font::Semibold, size::Small, Tr("초점")).x;
    Text(cdl, Font::Regular, size::Caption, ImVec2(t0.x + tw + Dp(8.0f), t0.y + Dp(2.0f)), p.ink3, buf);
    ImGui::Dummy(ImVec2(w - Dp(keyW + 8.0f), Dp(24.0f)));
    Tooltip(Tr("피사계 심도의 초점이에요. 키가 없으면 화면의 주인공 캐릭터를 자동으로 따라가요."));
    ImGui::SetCursorScreenPos(ImVec2(t0.x + w - Dp(keyW), t0.y - Dp(5.0f)));
    if (Button("##focuskey", Tr("키 등록"), icon::Plus, ButtonKind::Ghost, ImVec2(keyW, 30.0f)))
        StudioInsertKeys({FocusRow()}, d.Frame());
    Tooltip(Tr("지금 초점 설정을 재생 헤드에 키로 등록 (다음 키까지 유지)"));
    ImGui::SetCursorScreenPos(ImVec2(t0.x, t0.y + Dp(26.0f)));

    // what is in focus now
    const FocusKf* seg = SegmentKey(d.camera.focus, d.Frame());
    const FocusMode mode = seg ? seg->mode : FocusMode::Auto;
    const std::string who = studioFocusSubject_ ? StudioModelLabel(studioFocusSubject_) : std::string();
    const char* modeName = mode == FocusMode::Target ? Tr("캐릭터") : mode == FocusMode::Manual ? Tr("수동 거리") : Tr("자동");
    if (studioFocusShown_ <= 0.0f)
        std::snprintf(buf, sizeof(buf), "%s · %s", modeName, Tr("초점 없음"));
    else if (!who.empty())
        std::snprintf(buf, sizeof(buf), "%s · %s · %.1f", modeName, who.c_str(), studioFocusShown_);
    else
        std::snprintf(buf, sizeof(buf), "%s · %.1f", modeName, studioFocusShown_);
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(cdl, Font::Regular, size::Small, c, c.x + w, dofOn ? p.ink2 : p.ink3, buf);
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        if (studioFocusAperture_ < 0.999f || studioFocusAperture_ > 1.001f) {
            std::snprintf(buf, sizeof(buf), Tr("조리개 배율 %.2f"), studioFocusAperture_);
            const ImVec2 c2 = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Caption, c2, p.ink3, buf);
            ImGui::Dummy(ImVec2(w, Dp(18.0f)));
        }
    }

    // the segment picker: from the playhead on, focus on ... (writes / updates the key under the playhead)
    std::string current = mode == FocusMode::Target && seg ? StudioModelLabel(seg->target) : std::string();
    if (mode == FocusMode::Target && current.empty()) current = Tr("(없는 캐릭터) · 자동");
    if (mode == FocusMode::Auto) current = Tr("자동 (주인공 캐릭터)");
    if (mode == FocusMode::Manual) current = Tr("수동 거리");
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, Tr("여기부터"));
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(64.0f));
        PushFont(Font::Regular, size::Small);
        if (ImGui::BeginCombo("##focuspick", current.c_str())) {
            FocusKf base = StudioNewFocusKey(d.Frame());
            if (const FocusKf* here = FindKey(d.camera.focus, d.Frame())) base = *here;
            if (ImGui::Selectable(Tr("자동 (주인공 캐릭터)"), mode == FocusMode::Auto && seg)) {
                FocusKf k = base;
                k.mode = FocusMode::Auto;
                StudioSetFocusAtPlayhead(k, Tr("초점 변경"));
            }
            for (const auto& m : d.models) {
                if (m->kind != ModelKind::Character) continue;
                ImGui::PushID((int)m->uid);
                if (ImGui::Selectable(m->name.c_str(), mode == FocusMode::Target && seg && seg->target == m->uid)) {
                    FocusKf k = base;
                    k.mode = FocusMode::Target;
                    k.target = m->uid;
                    StudioSetFocusAtPlayhead(k, Tr("초점 변경"));
                }
                ImGui::PopID();
            }
            if (ImGui::Selectable(Tr("수동 거리 (지금 거리로 고정)"), mode == FocusMode::Manual && seg)) {
                FocusKf k = base;
                k.mode = FocusMode::Manual;
                if (studioFocusShown_ > 0.0f) k.distance = studioFocusShown_;
                StudioSetFocusAtPlayhead(k, Tr("초점 변경"));
            }
            ImGui::EndCombo();
        }
        PopFont();
        Tooltip(Tr("재생 헤드에 초점 키를 만들어 이 지점부터 초점을 맞출 대상을 정해요 (다음 초점 키까지)."));
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }
    if (!dofOn) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::TextWrapped("%s", Tr("피사계 심도가 꺼져 있어요. 상단 렌더 효과 메뉴에서 켜면 뷰포트와 GI 스틸에 적용돼요."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        (void)c;
    }
}

bool App::DrawStudioFocusKeyFields(float w, int frame) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    FocusKf* k = FindKey(d.camera.focus, frame);
    if (!k) { StudioEndKeyEdit(); return true; }
    bool ended = false;
    // a one-shot change (segmented / combo / button): one undo step
    const auto commit = [&](auto&& apply) {
        StudioBeginKeyEdit(RowKind::Focus);
        apply();
        studioKeyChanged_ = true;
        StudioEndKeyEdit();
        d.rowsKey = ~0ull;
    };
    // label + full-width drag field; one undo step per drag / typed value
    const auto row = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(76.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(76.0f));
        PushFont(Font::Regular, size::Small);
        const bool changed = widget();
        PopFont();
        if (ImGui::IsItemActivated()) StudioBeginKeyEdit(RowKind::Focus);
        ended |= ImGui::IsItemDeactivated();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        return changed;
    };

    {
        const char* modes[] = {Tr("자동"), Tr("캐릭터"), Tr("거리")};
        int mode = std::clamp((int)k->mode, 0, 2);
        if (Segmented("##focusmode", modes, 3, &mode, w / Dpi(), 32.0f)) {
            commit([&] {
                k->mode = (FocusMode)mode;
                if (k->mode == FocusMode::Target && k->target == 0) k->target = studioFocusSubject_;
                if (k->mode == FocusMode::Manual && studioFocusShown_ > 0.0f && FindKey(d.camera.focus, d.Frame()) == k)
                    k->distance = studioFocusShown_;
            });
            k = FindKey(d.camera.focus, frame);
            if (!k) return true;
        }
        Tooltip(Tr("자동: 화면에서 가장 크고 가운데 있는 캐릭터 · 캐릭터: 정한 캐릭터를 따라감 · 거리: 초점 거리를 직접 애니메이션"));
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    }

    if (k->mode == FocusMode::Target) {
        std::string name = StudioModelLabel(k->target);
        if (name.empty()) name = Tr("(선택 안 됨)");
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, Tr("대상"));
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(76.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(76.0f));
        PushFont(Font::Regular, size::Small);
        if (ImGui::BeginCombo("##focustarget", name.c_str())) {
            for (const auto& m : d.models) {
                if (m->kind != ModelKind::Character) continue;
                ImGui::PushID((int)m->uid);
                if (ImGui::Selectable(m->name.c_str(), k->target == m->uid) && k->target != m->uid) {
                    const uint32_t uid = m->uid;
                    commit([&] { k->target = uid; });
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        PopFont();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        k = FindKey(d.camera.focus, frame);
        if (!k) return true;
        const char* bones[] = {Tr("머리"), Tr("상체"), Tr("센터")};
        int bone = std::clamp((int)k->bone, 0, 2);
        if (Segmented("##focusbone", bones, 3, &bone, w / Dpi(), 30.0f)) commit([&] { k->bone = (FocusBone)bone; });
        Tooltip(Tr("대상 캐릭터의 어느 부분에 초점을 맞출지"));
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    } else if (k->mode == FocusMode::Manual) {
        float dist = k->distance;
        if (row(Tr("거리"), [&] { return ImGui::DragFloat("##focusdist", &dist, 0.1f, 0.5f, 3000.0f, "%.1f"); })) {
            k->distance = std::clamp(dist, 0.5f, 3000.0f);
            studioKeyChanged_ = true;
        }
        if (Button("##focusgrab", Tr("지금 초점 거리로"), icon::Crosshair, ButtonKind::Ghost, ImVec2(0.0f, 28.0f)) &&
            studioFocusShown_ > 0.0f) {
            const float shown = studioFocusShown_;
            commit([&] { k->distance = shown; });
        }
        Tooltip(Tr("재생 헤드에서 보이는 초점 거리를 이 키의 거리로 써요. 거리 키끼리는 선형으로 이어져요."));
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }

    k = FindKey(d.camera.focus, frame);
    if (!k) return true;
    float aperture = k->aperture;
    if (row(Tr("조리개 배율"), [&] { return ImGui::DragFloat("##focusap", &aperture, 0.01f, 0.0f, 3.0f, "%.2f"); })) {
        k->aperture = std::clamp(aperture, 0.0f, 3.0f);
        studioKeyChanged_ = true;
    }
    Tooltip(Tr("렌더 설정의 조리개에 곱해요. 0이면 이 구간은 모두 선명해요. 키 사이에서 선형으로 변해요."));
    int transition = k->transition;
    if (row(Tr("전환 프레임"), [&] { return ImGui::DragInt("##focustrans", &transition, 0.25f, 0, 600, "%d f"); })) {
        k->transition = std::clamp(transition, 0, 600);
        studioKeyChanged_ = true;
    }
    Tooltip(Tr("이전 구간의 초점에서 이 키의 초점으로 옮겨 가는 길이 (0 = 바로 전환, 포커스 풀)"));
    if (ended) StudioEndKeyEdit();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    Text(cdl, Font::Regular, size::Caption, c, p.ink3, Tr("초점 키는 다음 키까지 유지돼요. 프로젝트에만 저장돼요."));
    ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    return true;
}

} // namespace mmdx
