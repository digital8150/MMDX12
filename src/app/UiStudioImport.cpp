// Studio VMD import onto existing keys: the modal that asks whether the imported motion is merged with the target's
// keys (the old behaviour: same frame = the file wins) or replaces them. The import itself lives in
// App::StudioImportVmdFrom (UiStudio.cpp); several dropped VMDs queue behind an open modal and follow one by one.
#include "app/App.h"
#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include <cstdio>

namespace mmdx {

using namespace studio;

namespace {
// Selects the import's target again (model by uid, else the camera); false when the model went away.
bool SelectImportTarget(StudioDoc& d, int modelUid, uint32_t cameraUid) {
    if (modelUid >= 0) {
        const int i = d.IndexOfUid((uint32_t)modelUid);
        if (i < 0) return false;
        d.selectedModel = i;
    } else {
        d.selectedModel = -1;
    }
    d.selectedCameraUid = cameraUid != 0 && d.FindCamera(cameraUid) ? cameraUid : 0;
    return true;
}
} // namespace

bool App::StudioImportModeAnswer(int answer) {
    if (!studio_ || !studioImportModeOpen_) return false;
    StudioDoc& d = *studio_;
    const QueuedVmdImport q = studioImportPending_;
    studioImportModeOpen_ = false;
    studioImportPending_ = {};
    studioImportModeExisting_ = 0;
    if (answer != 1 && answer != 2) {
        LOG_INFO("studio: VMD import cancelled: %s", PathToUtf8(q.path.filename()).c_str());
        return true;
    }
    if (!SelectImportTarget(d, q.modelUid, q.cameraUid)) {  // the model went away while the modal was open
        LOG_WARN("studio: VMD import target is gone: %s", PathToUtf8(q.path.filename()).c_str());
        return true;
    }
    LOG_INFO("studio: VMD import %s: %s", answer == 2 ? "replaces the existing keys" : "merges",
             PathToUtf8(q.path.filename()).c_str());
    StudioImportVmdFrom(q.path, answer == 2 ? VmdImportMode::Replace : VmdImportMode::Merge);
    return true;
}

void App::DrawStudioImportModeDialog() {
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    // The next queued import starts once no import question is open (it may open the modal again).
    if (!studioImportModeOpen_ && !studioRestPoseOpen_ && !studioImportQueue_.empty()) {
        const QueuedVmdImport q = studioImportQueue_.front();
        studioImportQueue_.erase(studioImportQueue_.begin());
        if (SelectImportTarget(d, q.modelUid, q.cameraUid)) StudioImportVmdFrom(q.path);
        else LOG_WARN("studio: queued VMD import target is gone: %s", PathToUtf8(q.path.filename()).c_str());
    }
    if (!studioImportModeOpen_) return;

    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(480.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##importmodedlg")) ImGui::OpenPopup("##importmodedlg");
    if (ImGui::BeginPopupModal("##importmodedlg", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float w = Dp(432.0f);
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + w, p.ink, Tr("기존 키가 있어요"));
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));

        std::string target;
        if (studioImportPending_.modelUid >= 0) {
            const int i = d.IndexOfUid((uint32_t)studioImportPending_.modelUid);
            if (i >= 0) target = d.models[(size_t)i]->name;
        } else {
            target = d.CameraLabel(studioImportPending_.cameraUid);
        }
        PushFont(Font::Semibold, size::Body);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextWrapped("%s", PathToUtf8(studioImportPending_.path.filename()).c_str());
        PopFont();
        ImGui::Dummy(ImVec2(0.0f, Dp(4.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped(Tr("'%s'에 키가 %zu개 있어요. 불러온 모션을 기존 키와 병합하거나, 기존 키를 비우고 새로 넣을 수 있어요."),
                           target.c_str(), studioImportModeExisting_);
        if (!studioImportQueue_.empty()) {
            ImGui::Dummy(ImVec2(0.0f, Dp(2.0f)));
            ImGui::Text(Tr("이어서 불러올 파일 %zu개"), studioImportQueue_.size());
        }
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, Dp(14.0f)));

        int answer = -1;
        if (Button("##imcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(100.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            answer = 0;
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##imreplace", Tr("비우고 불러오기"), icon::Trash, ButtonKind::Secondary, ImVec2(160.0f, 40.0f)))
            answer = 2;
        Tooltip(Tr("기존 키를 모두 지우고 파일의 키만 넣어요. 실행 취소할 수 있어요."));
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##immerge", Tr("병합"), nullptr, ButtonKind::Primary, ImVec2(148.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            answer = 1;
        Tooltip(Tr("기존 키를 두고 파일의 키를 더해요. 같은 프레임은 파일의 키로 바뀌어요."));
        // A script `studioimportanswer` presses the same buttons.
        if (studioImportModeAnswer_ >= 0) {
            answer = studioImportModeAnswer_;
            studioImportModeAnswer_ = -1;
        }
        if (answer >= 0) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            StudioImportModeAnswer(answer);  // may open the rest-pose modal (drawn next frame)
            return;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace mmdx
