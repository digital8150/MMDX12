// S4: camera entities in the studio. The main camera (uid 0) stays StudioDoc::camera; `StudioDoc::cameras` holds the
// extra ones, each with its own camera keys and VMD. The main camera alone is the output and the viewport's camera view
// (MMD compatible: a cut is a 1-frame key pair); the extra cameras feed the stage screens. This file owns their outliner
// rows (select / rename / delete / eye / possess / "새 카메라" / "스테이지 카메라 가져오기") and the small viewport
// gizmos of the extra cameras.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"
#include "studio/Gizmo.h"
#include "ui_probe/UiProbe.h"

namespace mmdx {

using namespace studio;

namespace {
// A stage camera's static pose as a camera key: MMD's camera looks at `target` from `position`, its distance is the
// eye -> target length (negative: MMD's convention for an eye in front of the target along its own axis) and its
// rotation the yaw / pitch of that direction.
CameraKf KeyFromStageCamera(const StageCamera& c) {
    CameraKf k;
    FillLinearCameraInterp(k.interp);
    k.frame = 0;
    k.target = c.target;
    const float dx = c.position.x - c.target.x, dy = c.position.y - c.target.y, dz = c.position.z - c.target.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    k.distance = -std::max(0.1f, len);
    // the eye sits at target + R * (0, 0, distance) with R = rot(-x, -y, -z): solve the yaw / pitch of -d
    const float yaw = std::atan2(-dx, -dz);
    const float pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    k.rotation = {-pitch, yaw, 0.0f};
    k.fovDeg = std::clamp(c.fov, 1.0f, 125.0f);
    k.perspective = true;
    return k;
}
} // namespace

// ---------------------------------------------------------------------------
// Selection / camera list edits
// ---------------------------------------------------------------------------

void App::StudioKeyExtraCameraFromView() {
    StudioDoc& d = *studio_;
    if (d.selectedCameraUid == 0) return;
    const std::vector<TrackState> before = {CaptureTrack(d, -1, RowKind::Camera, "")};
    UpsertKey(d.SelectedCameraTrack().camera, StudioViewportCameraKey(d.Frame()));
    d.TouchCamera(d.selectedCameraUid);
    StudioPushTrackEdit(Tr("카메라 키"), before);
}

void App::StudioSelectCamera(uint32_t uid) {
    StudioDoc& d = *studio_;
    if (uid != 0 && !d.FindCamera(uid)) return;
    if (d.selectedCameraUid == uid && d.selectedModel == -1 && d.selectedLightUid == 0 && studioSelectedWorld_ == 0) return;
    StudioPossess(false);   // possession belongs to the camera that was selected
    d.selectedCameraUid = uid;
    d.selectedModel = -1;
    d.selectedLightUid = 0;
    studioSelectedWorld_ = 0;
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
    d.expanded.clear();
    d.rowsKey = ~0ull;
}

void App::StudioAddCameraFromView() {
    StudioDoc& d = *studio_;
    std::vector<studio::StudioCamera> before = d.cameras;
    studio::StudioCamera c;
    c.uid = d.nextCameraUid++;
    for (int n = 1;; ++n) {   // the first free "카메라 <n>" (the list may hold imported stage cameras)
        const std::string name = std::string(Tr("카메라")) + " " + std::to_string(n);
        if (std::none_of(d.cameras.begin(), d.cameras.end(), [&](const studio::StudioCamera& x) { return x.name == name; })) {
            c.name = name;
            break;
        }
    }
    // the viewport's view becomes a key at the playhead (the free view, or the main camera while looking through it)
    c.track.camera = {StudioViewportCameraKey(d.Frame())};
    d.cameras.push_back(std::move(c));
    const uint32_t uid = d.cameras.back().uid;
    d.history.Push(std::make_unique<CamerasCommand>(d, Tr("카메라 추가"), before, d.cameras));
    ++d.projectVersion;
    d.TouchCamera(0);
    StudioSelectCamera(uid);
    toast_ = {Tr("카메라를 추가했어요"), d.CameraLabel(uid), {}, false, timeSeconds_ + 3.0};
}

void App::StudioImportStageCameras() {
    StudioDoc& d = *studio_;
    // every loaded MMDX stage entity with declared cameras, one extra camera per stage.json camera
    std::vector<studio::StudioStage*> stages;
    for (studio::StudioStage& st : d.stages)
        if (st.manifest && !st.manifest->cameras.empty()) stages.push_back(&st);
    if (stages.empty()) {
        toast_ = {Tr("이 스테이지에는 카메라가 없어요"), {}, {}, true, timeSeconds_ + 4.0};
        return;
    }
    std::vector<studio::StudioCamera> before = d.cameras;
    const auto taken = [&](const std::string& name) {
        return std::any_of(d.cameras.begin(), d.cameras.end(), [&](const studio::StudioCamera& c) { return c.name == name; });
    };
    size_t added = 0;
    for (const studio::StudioStage* st : stages) {
        if (!st->manifest) continue;
        for (const StageCamera& sc : st->manifest->cameras) {
            studio::StudioCamera c;
            c.uid = d.nextCameraUid++;
            c.name = sc.name;
            if (taken(c.name)) {   // the same stage camera imported twice: a numbered copy
                int n = 2;
                while (taken(c.name + " " + std::to_string(n))) ++n;
                c.name += " " + std::to_string(n);
            }
            c.stageUid = st->uid;
            c.track.camera = {KeyFromStageCamera(sc)};
            d.cameras.push_back(std::move(c));
            ++added;
        }
    }
    if (added == 0) return;
    d.history.Push(std::make_unique<CamerasCommand>(d, Tr("스테이지 카메라 가져오기"), before, d.cameras));
    ++d.projectVersion;
    d.TouchCamera(0);
    StudioSelectCamera(d.cameras[before.size()].uid);
    toast_ = {Tr("스테이지 카메라를 가져왔어요"), std::to_string(added) + Tr("개"), {}, false, timeSeconds_ + 3.0};
}

// ---------------------------------------------------------------------------
// Outliner
// ---------------------------------------------------------------------------

// One camera row: the same layout as the outliner's model rows (glyph, name + sub line, eye, menu).
// `main`: the main camera (uid 0), which has no name / delete of its own.
void App::DrawStudioCameraOutliner() {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = Dp(40.0f);
    int renameUid = -1;   // the camera whose rename was asked for this frame (0 = the main camera)
    uint32_t removeUid = 0;
    bool haveRemove = false;

    for (int i = -1; i < (int)d.cameras.size(); ++i) {
        const studio::StudioCamera* cam = i < 0 ? nullptr : &d.cameras[(size_t)i];
        const uint32_t uid = cam ? cam->uid : 0;
        ImGui::PushID("camera");
        ImGui::PushID((int)uid + 1);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + w, a.y + rowH);
        const bool selected = d.selectedModel == -1 && d.selectedLightUid == 0 && studioSelectedWorld_ == 0 &&
                              d.selectedCameraUid == uid;
        ImGui::InvisibleButton("##row", ImVec2(w - Dp(104.0f), rowH));
        uiprobe::Info(ImGui::GetItemID(), "##row", cam ? cam->name.c_str() : Tr("메인 카메라"));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) StudioSelectCamera(uid);
        if (cam && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            studioRenameCamera_ = (int)uid;
            std::snprintf(studioRenameBuf_, sizeof(studioRenameBuf_), "%s", cam->name.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##cammenu");   // the main camera too
        const bool rowHot = hovered || ImGui::IsMouseHoveringRect(a, b);
        const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
        if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
        else if (rowHot) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));
        const ImU32 fg = selected ? p.accentInk : p.ink;
        Icon(cdl, uid == 0 ? icon::VideoCamera : icon::Camera, 16.0f, ImVec2(a.x + Dp(26.0f), a.y + rowH * 0.5f),
             selected ? p.accentInk : p.ink2);
        const float tx = a.x + Dp(44.0f), maxX = b.x - Dp(108.0f);
        char sub[96];
        const size_t keys = d.CameraKeys(uid).size();
        std::string caption = cam ? cam->name : std::string(Tr("메인 카메라"));
        // the main camera is the output; the extra ones are the stage screens' (say how many screens show one)
        if (!cam) {
            std::snprintf(sub, sizeof(sub), Tr("키 %d개 · 출력 카메라"), (int)keys);
        } else {
            const std::string addr = "uid:" + std::to_string(uid);
            int screens = 0;
            for (const studio::StudioStage& st : d.stages)
                for (const auto& [id, o] : st.screens) screens += o.camera && *o.camera == addr ? 1 : 0;
            if (screens > 0) std::snprintf(sub, sizeof(sub), Tr("키 %d개 · 스크린 %d곳"), (int)keys, screens);
            else std::snprintf(sub, sizeof(sub), Tr("키 %d개 · 스크린용"), (int)keys);
        }
        TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, caption.c_str());
        TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, sub);
        // Right side, every camera: possess (the viewport shows and moves this camera; renders keep the main one); an
        // extra camera also has its eye (the viewport overlay of its frustum); the menu button shows on hover.
        const bool possessedHere = d.possessCamera && d.selectedCameraUid == uid;
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(40.0f), a.y + Dp(6.0f)));
        if (IconButton("##possess", icon::Crosshair,
                       possessedHere ? Tr("카메라 빙의 해제  (Esc)") : Tr("카메라 빙의: 뷰포트 조작이 이 카메라를 움직여요"),
                       possessedHere, 28.0f)) {
            if (possessedHere) {
                StudioPossess(false);
            } else {
                StudioSelectCamera(uid);   // possession edits the selected camera (selecting drops the old possession)
                StudioPossess(true);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(72.0f), a.y + Dp(6.0f)));
        if (cam && IconButton("##vis", cam->visible ? icon::Eye : icon::EyeSlash,
                              cam->visible ? Tr("뷰포트에서 숨기기") : Tr("뷰포트에 보이기"), false, 28.0f)) {
            std::vector<studio::StudioCamera> before = d.cameras;
            if (studio::StudioCamera* c = d.FindCamera(uid)) {
                c->visible = !c->visible;
                d.history.Push(std::make_unique<CamerasCommand>(d, Tr("카메라 표시"), before, d.cameras));
                ++d.projectVersion;
                d.rowsKey = ~0ull;
            }
        }
        if (rowHot || selected) {
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(cam ? 104.0f : 72.0f), a.y + Dp(6.0f)));
            if (IconButton("##more", icon::DotsThree, Tr("카메라 메뉴"), false, 28.0f)) ImGui::OpenPopup("##cammenu");
        }
        if (ImGui::BeginPopup("##cammenu")) {
            if (!cam) {
                if (MenuItem("##campossessm", possessedHere ? Tr("카메라 빙의 해제") : Tr("카메라 빙의"), icon::Crosshair)) {
                    StudioSelectCamera(0);
                    StudioPossess(!possessedHere);
                    ImGui::CloseCurrentPopup();
                }
                if (MenuItem("##camkeym", Tr("현재 시점을 키로 등록"), icon::Plus)) {
                    StudioSelectCamera(0);
                    StudioKeyCameraFromView();
                    ImGui::CloseCurrentPopup();
                }
            } else {
                if (MenuItem("##campossessx", possessedHere ? Tr("카메라 빙의 해제") : Tr("카메라 빙의"), icon::Crosshair)) {
                    if (possessedHere) {
                        StudioPossess(false);
                    } else {
                        StudioSelectCamera(uid);
                        StudioPossess(true);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (MenuItem("##camkeyx", Tr("현재 뷰를 키로 등록"), icon::Plus)) {
                    StudioSelectCamera(uid);
                    StudioKeyExtraCameraFromView();
                    ImGui::CloseCurrentPopup();
                }
                Tooltip(Tr("뷰포트에 보이는 시점을 재생 헤드에 이 카메라의 키로 넣어요"));
                if (MenuItem("##camename", Tr("이름 바꾸기"), icon::PencilSimple)) {
                    studioRenameCamera_ = (int)uid;
                    std::snprintf(studioRenameBuf_, sizeof(studioRenameBuf_), "%s", cam->name.c_str());
                    ImGui::CloseCurrentPopup();
                }
                if (MenuItem("##camremove", Tr("카메라 삭제"), icon::Trash)) removeUid = uid, haveRemove = true;
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::PopID();
        ImGui::PopID();
    }
    if (renameUid >= 0) studioRenameCamera_ = renameUid;

    // the two camera actions sit under the rows as quiet text buttons
    {
        const float bw = (w - Dp(16.0f)) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(8.0f), ImGui::GetCursorScreenPos().y + Dp(2.0f)));
        if (Button("##camadd", Tr("새 카메라"), icon::Plus, ButtonKind::Ghost, ImVec2(bw / Dpi(), 30.0f)))
            StudioAddCameraFromView();
        Tooltip(Tr("무대 스크린에 띄울 카메라를 지금 뷰포트 시점으로 만들어요"));
        ImGui::SameLine(0, Dp(4.0f));
        if (Button("##camimport", Tr("스테이지 카메라"), icon::DownloadSimple, ButtonKind::Ghost, ImVec2(bw / Dpi(), 30.0f)))
            StudioImportStageCameras();
        Tooltip(Tr("스테이지가 정의한 카메라를 하나씩 가져와요"));
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }

    if (haveRemove) {
        std::vector<studio::StudioCamera> before = d.cameras;
        d.cameras.erase(std::remove_if(d.cameras.begin(), d.cameras.end(),
                                       [&](const studio::StudioCamera& c) { return c.uid == removeUid; }),
                        d.cameras.end());
        if (d.selectedCameraUid == removeUid) d.selectedCameraUid = 0;
        d.history.Push(std::make_unique<CamerasCommand>(d, Tr("카메라 삭제"), before, d.cameras));
        ++d.projectVersion;
        d.TouchCamera(0);
    }
}

// The rename modal of a camera (the model one is a separate popup: one dialog at a time is enough).
void App::DrawStudioCameraRename() {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    studio::StudioCamera* cam = studioRenameCamera_ >= 0 ? d.FindCamera((uint32_t)studioRenameCamera_) : nullptr;
    if (studioRenameCamera_ >= 0 && !cam) studioRenameCamera_ = -1;   // its camera is gone
    if (studioRenameCamera_ >= 0 && !ImGui::IsPopupOpen("##renamecamera")) ImGui::OpenPopup("##renamecamera");
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(380.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(22.0f), Dp(20.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));
    if (ImGui::BeginPopupModal("##renamecamera", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = TextField("##renamecamfield", Tr("카메라 이름"), studioRenameBuf_, sizeof(studioRenameBuf_), 336.0f) &&
                           ImGui::IsKeyPressed(ImGuiKey_Enter);
        Gap(12.0f);
        bool close = false;
        if (Button("##renamecamcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(162.0f, 38.0f))) close = true;
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##renamecamok", Tr("확인"), icon::Check, ButtonKind::Primary, ImVec2(162.0f, 38.0f)) || enter ||
            ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (studio::StudioCamera* c = studioRenameCamera_ >= 0 ? d.FindCamera((uint32_t)studioRenameCamera_) : nullptr) {
                if (studioRenameBuf_[0] && c->name != studioRenameBuf_) {
                    std::vector<studio::StudioCamera> before = d.cameras;
                    c->name = studioRenameBuf_;
                    d.history.Push(std::make_unique<CamerasCommand>(d, Tr("카메라 이름"), before, d.cameras));
                    ++d.projectVersion;
                }
            }
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        if (close) {
            studioRenameCamera_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Viewport: the other cameras' frustum gizmos
// ---------------------------------------------------------------------------

// Draws a small frustum for every visible extra camera that is not the one being looked through, in the camera path
// overlay's style (dimmed outline + the camera's colour). Selecting one of them is left to the outliner: the gizmo is
// a scene reference, not a handle.
void App::DrawStudioCameraGizmos(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    if (d.cameras.empty()) return;
    if (d.useMotionCamera && d.cameraEval) return;   // looking through the main camera: no overlay clutter
    const float frame = (float)(d.time * kMmdFps);
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    CameraPathStyle style;
    style.lineWidth = Dp(1.5f);
    style.keyRadius = Dp(3.0f);
    style.currentRadius = Dp(4.0f);
    style.currentFrame = frame;
    for (const studio::StudioCamera& cam : d.cameras) {
        // the selected camera already has the path overlay (DrawStudioCameraPath): a gizmo too would double it
        if (!cam.visible || cam.track.camera.empty() || cam.uid == d.selectedCameraUid) continue;
        const CameraKf k = SampleCamera(cam.track.camera, frame);
        CameraPose pose;
        pose.target = k.target;
        pose.rotation = k.rotation;
        pose.distance = k.distance;
        pose.fovDeg = (float)k.fovDeg;
        DirectX::XMFLOAT3 eye;
        CameraMotion::ToView(pose, nullptr, &eye);
        // an eye outside the viewport (a stage camera far behind the free view) is not drawn: its frustum edges would
        // cross the whole image instead of reading as a marker
        ImVec2 pe;
        if (!studioVp_.Project(eye, pe)) continue;
        const float marginX = (x1 - x0) * 0.15f, marginY = (y1 - y0) * 0.15f;
        if (pe.x < x0 - marginX || pe.x > x1 + marginX || pe.y < y0 - marginY || pe.y > y1 + marginY) continue;
        const float toViewer = std::sqrt((eye.x - studioVp_.eye.x) * (eye.x - studioVp_.eye.x) +
                                         (eye.y - studioVp_.eye.y) * (eye.y - studioVp_.eye.y) +
                                         (eye.z - studioVp_.eye.z) * (eye.z - studioVp_.eye.z));
        // a gizmo, not the camera's real coverage: a constant screen size (a 190 unit stage camera at the far end of the
        // stage would otherwise cover the viewport), at least a couple of world units so a near eye still reads
        const float frustum = std::max(studioVp_.PixelWorldSize(eye) * Dp(56.0f), std::max(1.0f, toViewer * 0.02f));
        CameraPathPoint cur;
        CameraMotion::ToView(pose, &cur.view, &cur.eye);
        // the target marker sits `frustum` ahead (the real target of a stage camera is far away: the eye -> target
        // line would cross the whole viewport)
        const float tx = pose.target.x - cur.eye.x, ty = pose.target.y - cur.eye.y, tz = pose.target.z - cur.eye.z;
        const float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
        cur.target = tl > 1e-3f ? DirectX::XMFLOAT3{cur.eye.x + tx / tl * frustum, cur.eye.y + ty / tl * frustum,
                                                   cur.eye.z + tz / tl * frustum}
                                : pose.target;
        cur.fovY = DirectX::XMConvertToRadians(pose.fovDeg);
        cur.frame = frame;
        style.current = cam.uid == d.selectedCameraUid ? p.accent : WithAlpha(p.ink2, 0.8f);
        style.path = style.current;
        style.outline = IM_COL32(12, 14, 18, 170);
        style.target = WithAlpha(style.current, 0.7f);
        style.key = style.current;
        style.selectedKey = p.accent;
        style.currentFrame = frame;
        style.windowHalfWidth = 0.0f;
        DrawCameraPath(dl, studioVp_, nullptr, 0, nullptr, 0, {}, &cur, d.AspectRatio(), frustum, 0.0f, style);
        // the name, above the eye
        Text(dl, Font::Semibold, size::Caption, ImVec2(pe.x + Dp(6.0f), pe.y - Dp(16.0f)), style.current,
             cam.name.c_str());
    }
    dl->PopClipRect();
}

} // namespace mmdx
