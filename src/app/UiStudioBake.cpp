// Physics bake UI (P2.2): options / progress modal, the async run and the one undoable apply step.
// The simulation itself lives in src/studio/PhysicsBake.cpp (App-independent).
#include "app/App.h"
#include "anim/ModelInstance.h"
#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>

namespace mmdx {

using namespace studio;

void App::StudioOpenBakeDialog() {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character) return;
    if (d.HasRange()) {
        studioBakeOptStart_ = d.view.rangeStart;
        studioBakeOptEnd_ = d.view.rangeEnd;
    } else {
        studioBakeOptStart_ = 0;
        studioBakeOptEnd_ = std::max(0, (int)m->motion.EndFrame());
    }
    studioBakeOptStep_ = 1;
    studioBakeOptSelectedOnly_ = false;
    studioBakeOptTurnOff_ = true;
    studioBakeDialogOpen_ = true;
}

bool App::StudioStartBake(int start, int end, int step, bool selectedOnly, bool turnOff) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character || !m->inst) return false;
    if (!m->bound) {
        toast_ = {Tr("물리 굽기를 할 수 없어요"), Tr("모션을 먼저 불러오세요."), {}, true, timeSeconds_ + 4.0};
        return false;
    }
    BakeOptions o;
    o.start = std::max(0, start);
    o.end = std::max(o.start, end);
    o.step = std::max(1, step);
    o.env = d.physicsEnv;   // bake under the project's gravity / wind, as the viewport simulates
    if (selectedOnly) {
        for (int b : d.selectedBones)
            if (b >= 0 && b < (int)m->pmx->bones.size()) o.bones.push_back(b);
        std::sort(o.bones.begin(), o.bones.end());
        o.bones.erase(std::unique(o.bones.begin(), o.bones.end()), o.bones.end());
        if (o.bones.empty()) {
            toast_ = {Tr("물리 굽기를 할 수 없어요"), Tr("선택한 본이 없어요."), {}, true, timeSeconds_ + 4.0};
            return false;
        }
    } else if (PhysicsDrivenBones(*m->pmx).empty()) {
        toast_ = {Tr("물리 굽기를 할 수 없어요"), Tr("이 모델에는 구동되는 물리 본이 없어요."), {}, true, timeSeconds_ + 4.0};
        return false;
    }

    auto pmx = m->pmx;
    auto bound = m->bound;
    const XMFLOAT4X4 root = m->inst->RootTransform();
    studioBakeCancel_.store(false);
    studioBakeProgress_.store(0.0f);
    studioBakeResult_ = {};
    studioBakeModel_ = d.selectedModel;
    studioBakeStart_ = o.start;
    studioBakeEnd_ = o.end;
    studioBakeStep_ = o.step;
    studioBakeTurnOff_ = turnOff;
    studioBakeRunning_ = true;
    studioBakeDialogOpen_ = true;  // the modal switches to the progress view
    studioBakeStartWall_ = timeSeconds_;
    studioBakeFuture_ = std::async(std::launch::async, [this, pmx, bound, root, o] {
        BakeResult res;
        const bool ok = BakePhysics(pmx, *bound, root, o, res, &studioBakeCancel_, &studioBakeProgress_);
        if (ok) studioBakeResult_ = std::move(res);
        return ok;
    });
    LOG_INFO("studio: bake started (model %d, frames %d..%d step %d, %d bones)", d.selectedModel, o.start, o.end, o.step,
             (int)o.bones.size());
    return true;
}

void App::StudioPollBake() {
    if (!studioBakeRunning_ || !studioBakeFuture_.valid()) return;
    if (studioBakeFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const bool ok = studioBakeFuture_.get();
    studioBakeRunning_ = false;
    studioBakeDialogOpen_ = false;
    StudioDoc& d = *studio_;
    if (ok && studioBakeModel_ >= 0 && studioBakeModel_ == d.selectedModel && studioBakeModel_ < (int)d.models.size()) {
        StudioBakeApply(studioBakeResult_, studioBakeStart_, studioBakeEnd_, studioBakeTurnOff_);
    } else if (!ok) {
        toast_ = {Tr("물리 굽기를 취소했어요"), {}, {}, false, timeSeconds_ + 3.0};
    }
    studioBakeResult_.bones.clear();
    studioBakeModel_ = -1;
    studioBakeCancel_.store(false);
}

void App::StudioBakeApply(const BakeResult& result, int start, int end, bool turnOff) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || result.bones.empty()) {
        toast_ = {Tr("굽을 물리가 없어요"), {}, {}, false, timeSeconds_ + 3.0};
        return;
    }
    size_t keyCount = 0;
    // Snapshot the touched tracks, then replace their keys inside [start, end] with the baked ones.
    std::vector<TrackState> before;
    before.reserve(result.bones.size());
    for (const auto& [name, keys] : result.bones) {
        (void)keys;
        before.push_back(CaptureTrack(d, d.selectedModel, RowKind::Bone, name));
    }
    for (const auto& [name, keys] : result.bones) {
        auto& track = m->motion.bones[name];
        const int lo = std::min(start, end), hi = std::max(start, end);
        track.erase(std::remove_if(track.begin(), track.end(),
                                   [lo, hi](const BoneKf& k) { return k.frame >= lo && k.frame <= hi; }),
                    track.end());
        for (const BoneKf& k : keys) {
            UpsertKey(track, k);
            ++keyCount;
        }
    }
    // Drop tracks that had keys only inside the baked range (nothing left to keep).
    for (const auto& [name, keys] : result.bones) {
        (void)keys;
        auto it = m->motion.bones.find(name);
        if (it != m->motion.bones.end() && it->second.empty()) m->motion.bones.erase(it);
    }
    d.TouchModel(d.selectedModel);

    std::vector<TrackState> after;
    after.reserve(before.size());
    for (const TrackState& s : before) after.push_back(CaptureTrack(d, s));
    std::vector<std::unique_ptr<Command>> parts;
    parts.push_back(std::make_unique<TrackEditCommand>(d, Tr("물리 굽기"), before, std::move(after)));
    if (turnOff && !m->physicsOff)  // share the bake's undo step (PhysicsOffCommand resets physicsFrame / dirty)
        parts.push_back(std::make_unique<PhysicsOffCommand>(d, d.selectedModel, false, true));
    d.history.Push(std::make_unique<CompositeCommand>(Tr("물리 굽기"), std::move(parts)));
    d.selection.clear();
    d.rowsKey = ~0ull;

    char detail[128];
    std::snprintf(detail, sizeof(detail), Tr("본 %d개 · 키 %d개"),
                  (int)result.bones.size(), (int)keyCount);
    toast_ = {Tr("물리 굽기를 마쳤어요"), detail, {}, false, timeSeconds_ + 5.0};
    LOG_INFO("studio: bake applied: %d bones, %d keys", (int)result.bones.size(), (int)keyCount);
}

void App::DrawStudioBakeDialog() {
    using namespace ui;
    if (!studioBakeDialogOpen_ && !studioBakeRunning_) return;
    if (!studio_) {
        studioBakeDialogOpen_ = false;
        return;
    }
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(420.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##bakedlg")) ImGui::OpenPopup("##bakedlg");
    if (ImGui::BeginPopupModal("##bakedlg", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(372.0f), p.ink, Tr("물리 굽기"));

        const auto dragInt = [&](const char* label, int* v, int lo, int hi, const char* id) {
            const ImVec2 r = ImGui::GetCursorScreenPos();
            Text(dl, Font::Regular, size::Small, ImVec2(r.x, r.y + Dp(5.0f)), p.ink3, label);
            ImGui::SetCursorScreenPos(ImVec2(r.x + Dp(120.0f), r.y));
            ImGui::SetNextItemWidth(Dp(252.0f));
            PushFont(Font::Regular, size::Small);
            ImGui::DragInt(id, v, 0.5f, lo, hi);
            PopFont();
            ImGui::Dummy(ImVec2(0.0f, Dp(6.0f)));
        };

        if (studioBakeRunning_) {
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
            ImGui::PushTextWrapPos(c.x + Dp(372.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            PushFont(Font::Regular, size::Small);
            ImGui::TextWrapped("%s", Tr("물리 시뮬레이션을 구워 키프레임으로 저장하는 중입니다…"));
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();

            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(74.0f)));
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ProgressBar(dl, a, ImVec2(a.x + Dp(372.0f), a.y + Dp(8.0f)), studioBakeProgress_.load());
            ImGui::Dummy(ImVec2(0.0f, Dp(22.0f)));

            ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(96.0f), c.y + Dp(110.0f)));
            if (Button("##bakecancelrun", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f))) {
                studioBakeCancel_.store(true);
            }
        } else {
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(34.0f)));
            dragInt(Tr("시작 프레임"), &studioBakeOptStart_, 0, 1000000, "##bakestart");
            dragInt(Tr("끝 프레임"), &studioBakeOptEnd_, 0, 1000000, "##bakeend");
            dragInt(Tr("간격 (프레임)"), &studioBakeOptStep_, 1, 1000, "##bakestep");
            studioBakeOptStep_ = std::max(1, studioBakeOptStep_);
            if (studioBakeOptEnd_ < studioBakeOptStart_) studioBakeOptEnd_ = studioBakeOptStart_;

            Switch("##bakesel", Tr("선택한 본만"), &studioBakeOptSelectedOnly_,
                   Tr("끄면 물리로 움직이는 모든 본을 구워요"));
            Switch("##bakeoff", Tr("굽은 뒤 이 모델의 물리 끄기"), &studioBakeOptTurnOff_,
                   Tr("구운 키가 물리 대신 재생돼요"));

            ImGui::Dummy(ImVec2(0.0f, Dp(8.0f)));
            bool close = false;
            if (Button("##bakecancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                close = true;
            }
            ImGui::SameLine(0, Dp(12.0f));
            if (Button("##bakeok", Tr("굽기"), icon::Atom, ButtonKind::Primary, ImVec2(180.0f, 40.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                if (StudioStartBake(studioBakeOptStart_, studioBakeOptEnd_, studioBakeOptStep_,
                                    studioBakeOptSelectedOnly_, studioBakeOptTurnOff_)) {
                    close = false;  // the modal stays open as the progress view
                } else {
                    close = true;
                }
            }
            if (close) {
                studioBakeDialogOpen_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    (void)d;
}

} // namespace mmdx
