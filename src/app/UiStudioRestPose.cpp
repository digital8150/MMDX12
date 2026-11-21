// Studio arm rest-pose correction UI: the modal for a VMD that looks T-pose-authored (asked by the VMD import
// before the keys are merged) and for the selected character's whole motion (model menu). The math lives in
// src/studio/RestPoseRetarget.cpp (App-independent).
#include "app/App.h"
#include "app/Icons.h"
#include "app/UiKit.h"
#include "asset/RestPose.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "studio/RestPoseRetarget.h"
#include <algorithm>
#include <cstdio>

namespace mmdx {

using namespace studio;

namespace {
// The model the dialog is for (studioRestPoseModel_ holds its uid: indices shift when a model is removed).
StudioModel* RestPoseModel(StudioDoc& d, int uid) {
    const int i = d.IndexOfUid((uint32_t)uid);
    if (i < 0 || i >= (int)d.models.size()) return nullptr;
    StudioModel* m = d.models[(size_t)i].get();
    return m && m->kind == ModelKind::Character && m->pmx ? m : nullptr;
}
} // namespace

void App::StudioOpenRestPoseDialog() {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character) return;
    studioRestPoseAuto_ = false;
    studioRestPoseReplace_ = false;
    studioRestPosePending_ = {};
    studioRestPosePendingPath_.clear();
    studioRestPoseGuess_ = {};
    studioRestPoseModel_ = (int)m->uid;
    studioRestPoseSourceDeg_ = 0.0f;
    studioRestPoseSide_[0] = studioRestPoseSide_[1] = true;
    studioRestPoseOpen_ = true;
}

bool App::StudioApplyRestPose(float sourceArmDeg, bool left, bool right) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character || !m->pmx) return false;
    MotionData after = m->motion;
    ArmRetargetOptions opt;
    opt.sourceArmDeg = std::clamp(sourceArmDeg, 0.0f, 45.0f);
    opt.side[0] = left;
    opt.side[1] = right;
    const ArmRetargetResult res = RetargetArmRest(*m->pmx, after, opt);
    if (!res.applied) {
        toast_ = {Tr("보정할 팔 키가 없어요"), {}, {}, false, timeSeconds_ + 3.0};
        return false;
    }
    d.history.Push(std::make_unique<MotionSwapCommand>(d, d.selectedModel, Tr("팔 레스트 포즈 보정"), m->motion,
                                                       std::move(after)));
    char detail[64];
    std::snprintf(detail, sizeof(detail), Tr("키 %d개"), res.keysChanged);
    toast_ = {Tr("팔 레스트 포즈를 보정했어요"), detail, {}, false, timeSeconds_ + 4.0};
    LOG_INFO("studio: arm rest-pose correction: model %d, angle %.1f / %.1f, bones %d, keys %d", d.selectedModel,
             res.rotationDeg[0], res.rotationDeg[1], res.bonesChanged, res.keysChanged);
    return true;
}

bool App::StudioRestPoseAnswer(bool accept) {
    if (!studio_ || !studioRestPoseOpen_) return false;
    StudioDoc& d = *studio_;
    StudioModel* target = RestPoseModel(d, studioRestPoseModel_);
    if (!target) {  // the model went away while the modal was open
        studioRestPoseOpen_ = false;
        studioRestPoseAuto_ = false;
        studioRestPosePending_ = {};
        studioRestPosePendingPath_.clear();
        studioRestPoseModel_ = -1;
        return true;
    }
    if (studioRestPoseAuto_) {
        MotionData in = studioRestPosePending_;
        ArmRetargetOptions opt;
        opt.sourceArmDeg = std::clamp(studioRestPoseSourceDeg_, 0.0f, 45.0f);
        opt.side[0] = studioRestPoseSide_[0];
        opt.side[1] = studioRestPoseSide_[1];
        ArmRetargetResult res;
        if (accept) res = RetargetArmRest(*target->pmx, in, opt);
        MotionData after = target->motion;
        if (studioRestPoseReplace_) after.ClearModelKeys();  // the import-mode modal chose "clear and import"
        after.Merge(in);
        const char* undoName = accept ? (studioRestPoseReplace_ ? Tr("모션 VMD 불러오기 (팔 보정, 기존 키 비움)")
                                                                : Tr("모션 VMD 불러오기 (팔 보정)"))
                                      : (studioRestPoseReplace_ ? Tr("모션 VMD 불러오기 (기존 키 비움)")
                                                                : Tr("모션 VMD 불러오기"));
        d.history.Push(std::make_unique<MotionSwapCommand>(d, d.IndexOfUid(target->uid), undoName, target->motion,
                                                           std::move(after)));
        if (accept)
            LOG_INFO("studio: arm rest-pose correction on import: angle %.1f / %.1f, bones %d, keys %d",
                     res.rotationDeg[0], res.rotationDeg[1], res.bonesChanged, res.keysChanged);
        StudioFinishVmdImport(studioRestPosePendingPath_);
    } else if (accept) {
        StudioApplyRestPose(studioRestPoseSourceDeg_, studioRestPoseSide_[0], studioRestPoseSide_[1]);
    }
    studioRestPoseOpen_ = false;
    studioRestPoseAuto_ = false;
    studioRestPoseReplace_ = false;
    studioRestPosePending_ = {};
    studioRestPosePendingPath_.clear();
    studioRestPoseModel_ = -1;
    return true;
}

void App::DrawStudioRestPoseDialog() {
    using namespace ui;
    if (!studioRestPoseOpen_) return;
    if (!studio_) {
        studioRestPoseOpen_ = false;
        return;
    }
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(420.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##restposedlg")) ImGui::OpenPopup("##restposedlg");
    if (ImGui::BeginPopupModal("##restposedlg", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(372.0f), p.ink, Tr("팔 레스트 포즈 보정"));

        // The drag row (the bake dialog's dragInt layout, for a float).
        const auto dragFloat = [&](const char* label, float* v, float lo, float hi, const char* id, const char* tip) {
            const ImVec2 r = ImGui::GetCursorScreenPos();
            Text(dl, Font::Regular, size::Small, ImVec2(r.x, r.y + Dp(5.0f)), p.ink3, label);
            ImGui::SetCursorScreenPos(ImVec2(r.x + Dp(120.0f), r.y));
            ImGui::SetNextItemWidth(Dp(252.0f));
            PushFont(Font::Regular, size::Small);
            ImGui::DragFloat(id, v, 0.5f, lo, hi, "%.0f°");
            PopFont();
            if (tip) Tooltip(tip);
            ImGui::Dummy(ImVec2(0.0f, Dp(6.0f)));
        };

        StudioModel* m = RestPoseModel(d, studioRestPoseModel_);
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
        if (studioRestPoseAuto_) {
            PushFont(Font::Semibold, size::Body);
            ImGui::TextUnformatted(Tr("T 포즈 기준으로 만든 모션 같아요"));
            PopFont();
            ImGui::Dummy(ImVec2(0.0f, Dp(4.0f)));
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Dp(372.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        PushFont(Font::Regular, size::Small);
        if (studioRestPoseAuto_) {
            ImGui::TextWrapped(Tr("팔이 몸 안쪽으로 들어가는 키가 많아요 (중앙값 %.0f°). A 포즈 모델에 맞게 팔 회전을 보정할 수 있어요."),
                               studioRestPoseGuess_.medianDeg);
        } else {
            ImGui::TextWrapped(Tr("T 포즈 모델용 모션을 이 모델(A 포즈)에 맞게 팔 회전 키를 고쳐요. 실행 취소할 수 있어요."));
        }
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, Dp(10.0f)));

        if (m) {
            const ArmRest rest = MeasureArmRest(*m->pmx);
            PushFont(Font::Regular, size::Small);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            ImGui::Text(Tr("이 모델의 팔 각도: 왼쪽 %.0f° · 오른쪽 %.0f°"), rest.deg[0], rest.deg[1]);
            ImGui::PopStyleColor();
            PopFont();
            ImGui::Dummy(ImVec2(0.0f, Dp(10.0f)));
        }

        dragFloat(Tr("모션 기준 팔 각도"), &studioRestPoseSourceDeg_, 0.0f, 45.0f, "##rpsrc",
                  Tr("T 포즈 = 0°, 원래 모션이 가정한 팔 각도"));
        Switch("##rpsideL", Tr("왼팔"), &studioRestPoseSide_[0]);
        Switch("##rpsideR", Tr("오른팔"), &studioRestPoseSide_[1]);

        ImGui::Dummy(ImVec2(0.0f, Dp(8.0f)));
        const char* secondary = studioRestPoseAuto_ ? Tr("그대로 불러오기") : Tr("취소");
        const char* primary = studioRestPoseAuto_ ? Tr("보정하고 불러오기") : Tr("보정");
        bool answer = false, accept = false;
        if (Button("##rpcancel", secondary, nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            answer = true;
        }
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##rpok", primary, icon::Sparkle, ButtonKind::Primary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            answer = true;
            accept = true;
        }
        // A script `studiorestposeanswer` presses the same buttons.
        if (studioRestPoseAnswer_) {
            answer = true;
            accept = studioRestPoseAnswer_ == 1;
            studioRestPoseAnswer_ = 0;
        }
        if (answer) {
            StudioRestPoseAnswer(accept);  // also closes the modal (or just closes when the model went away)
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace mmdx
