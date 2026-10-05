// Studio pose editing: the viewport bone overlay, bone picking, the translate/rotate gizmo, the pose layer
// (unregistered edits, register / reset), the bone and morph inspector tabs, VPD pose files and the mirror pose.
// The pose layer overrides the evaluated motion at its frame (StudioApplyPose); dragging never re-binds the motion.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "anim/ModelInstance.h"
#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/FileDialog.h"

namespace mmdx {

using namespace studio;
using namespace DirectX;

namespace {

// Rotation of a rigid bone world matrix (rows normalised, so a display scale would not leak in).
XMVECTOR RotationOf(const XMFLOAT4X4& m) {
    XMMATRIX r = XMLoadFloat4x4(&m);
    r.r[0] = XMVector3Normalize(r.r[0]);
    r.r[1] = XMVector3Normalize(r.r[1]);
    r.r[2] = XMVector3Normalize(r.r[2]);
    r.r[3] = XMVectorSet(0, 0, 0, 1);
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(r));
}

// MMD shows bone rotations as Euler degrees (same convention as the key inspector): q = RollPitchYaw(x, y, z).
XMFLOAT3 EulerDeg(const XMFLOAT4& q) {
    XMFLOAT4X4 rm;
    XMStoreFloat4x4(&rm, XMMatrixRotationQuaternion(XMLoadFloat4(&q)));
    const float pitch = std::asin(std::clamp(-rm._32, -1.0f, 1.0f));
    const float yaw = std::atan2(rm._31, rm._33);
    const float roll = std::atan2(rm._12, rm._22);
    return {XMConvertToDegrees(pitch) + 0.0f, XMConvertToDegrees(yaw) + 0.0f, XMConvertToDegrees(roll) + 0.0f};
}
XMFLOAT4 QuatFromEulerDeg(const XMFLOAT3& e) {
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(XMConvertToRadians(e.x), XMConvertToRadians(e.y),
                                                       XMConvertToRadians(e.z)));
    return q;
}

// Two unit vectors perpendicular to `a` (and to each other).
void Perpendiculars(XMVECTOR a, XMFLOAT3& u, XMFLOAT3& v) {
    XMVECTOR ref = std::fabs(XMVectorGetY(a)) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
    const XMVECTOR pu = XMVector3Normalize(XMVector3Cross(ref, a));
    const XMVECTOR pv = XMVector3Normalize(XMVector3Cross(a, pu));
    XMStoreFloat3(&u, pu);
    XMStoreFloat3(&v, pv);
}

const char* const kMorphPanelNames[4] = {"눈", "눈썹", "입", "기타"};  // panels 2, 1, 3, 4 (0 = other)
int MorphPanelSlot(uint8_t panel) { return panel == 2 ? 0 : panel == 1 ? 1 : panel == 3 ? 2 : 3; }

} // namespace

// ---------------------------------------------------------------------------
// Pose layer
// ---------------------------------------------------------------------------

StudioModel* App::StudioPoseModel() {
    if (!studio_) return nullptr;
    StudioModel* m = studio_->Selected();
    return m && !m->IsStage() ? m : nullptr;
}

std::vector<PoseBone> App::StudioCurrentPose(const StudioModel& m) const {
    const size_t n = m.pmx->bones.size();
    std::vector<PoseBone> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = {m.inst->BoneAnimTranslation((int)i), m.inst->BoneAnimRotation((int)i)};
    // the layer may have changed since the last evaluation (this frame's edits): it wins
    if (m.pose.frame == studio_->Frame())
        for (const auto& [b, v] : m.pose.bones)
            if (b >= 0 && (size_t)b < n) out[(size_t)b] = v;
    return out;
}

void App::StudioApplyPose(StudioModel& m) {
    if (m.pose.Empty() || m.pose.frame != studio_->Frame()) return;
    for (const auto& [b, v] : m.pose.bones) m.inst->SetBoneAnim(b, v.t, v.r);
    for (const auto& [i, w] : m.pose.morphs) m.inst->SetMorphWeight(i, w);
}

void App::StudioSetPose(const char* undoName, const PoseLayer& before) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m) return;
    PoseLayer b = before, a = m->pose;
    if (b.Empty()) b.frame = -1;
    if (a.Empty()) a.frame = -1;
    m->pose = a;
    if (SamePoseLayer(b, a)) return;
    d.history.Push(std::make_unique<PoseEditCommand>(d, d.selectedModel, undoName, std::move(b), std::move(a)));
}

void App::StudioSelectBone(int bone, bool toggle) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    if (bone < 0 || bone >= (int)m->pmx->bones.size()) {
        d.selectedBones.clear();
        d.activeBone = -1;
    } else if (toggle) {
        if (d.selectedBones.erase(bone)) {
            if (d.activeBone == bone) d.activeBone = d.selectedBones.empty() ? -1 : *d.selectedBones.rbegin();
        } else {
            d.selectedBones.insert(bone);
            d.activeBone = bone;
        }
    } else {
        d.selectedBones = {bone};
        d.activeBone = bone;
    }
    // timeline: the bones' rows are picked, their keys at the current frame selected
    const int frame = d.Frame();
    d.selectedRows.clear();
    d.selection.clear();
    for (int b : d.selectedBones) {
        const uint64_t row = CanonicalRow(*m, RowKind::Bone, (uint32_t)b);
        d.selectedRows.insert(row);
        const auto it = m->motion.bones.find(m->pmx->bones[(size_t)b].name);
        if (it != m->motion.bones.end() && FindKey(it->second, frame)) d.selection.insert({row, frame});
    }
    if (d.activeBone >= 0) {
        const uint64_t row = CanonicalRow(*m, RowKind::Bone, (uint32_t)d.activeBone);
        d.collapsed.erase(MakeRowId(RowKind::Group, RowGroupOf(row), 0));  // reveal the row
        d.scrollRowId = row;
        d.scrollToRow = 3;
        d.rowAnchor = row;
        d.inspectorTab = 1;
    }
    d.rowsKey = ~0ull;
}

void App::StudioRegisterPose(bool allBones) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) {
        StudioRegisterKeys();
        return;
    }
    const PmxModel& pmx = *m->pmx;
    const int frame = d.Frame();
    const bool hasPose = !m->pose.Empty() && m->pose.frame == frame;
    std::set<int> bones, morphs;
    if (hasPose) {
        for (const auto& [b, v] : m->pose.bones) bones.insert(b);
        for (const auto& [i, w] : m->pose.morphs) morphs.insert(i);
    }
    if (allBones) {
        // every bone the model lists for keying (display frames; all operable bones without them)
        for (const PmxDisplayFrame& f : pmx.displayFrames)
            for (const auto& it : f.items)
                if (!it.morph && it.index >= 0 && (size_t)it.index < pmx.bones.size()) bones.insert(it.index);
        if (pmx.displayFrames.empty())
            for (size_t i = 0; i < pmx.bones.size(); ++i)
                if (pmx.bones[i].flags & (PmxBone_Rotatable | PmxBone_Movable)) bones.insert((int)i);
    }
    if (bones.empty() && morphs.empty()) {
        StudioRegisterKeys();  // nothing edited: key the picked rows (M2 behaviour)
        return;
    }

    const std::vector<PoseBone> current = StudioCurrentPose(*m);
    std::vector<TrackState> before;
    for (int b : bones) before.push_back(CaptureTrack(d, d.selectedModel, RowKind::Bone, pmx.bones[(size_t)b].name));
    for (int i : morphs) before.push_back(CaptureTrack(d, d.selectedModel, RowKind::Morph, pmx.morphs[(size_t)i].name));
    std::set<KeyId> keyed;
    for (int b : bones) {
        auto& track = m->motion.bones[pmx.bones[(size_t)b].name];
        BoneKf k;
        if (const BoneKf* existing = FindKey(track, frame)) k = *existing;  // keep its interpolation
        else FillLinearInterp(k.interp);
        k.frame = frame;
        k.t = current[(size_t)b].t;
        k.r = current[(size_t)b].r;
        UpsertKey(track, k);
        keyed.insert({CanonicalRow(*m, RowKind::Bone, (uint32_t)b), frame});
    }
    for (int i : morphs) {
        UpsertKey(m->motion.morphs[pmx.morphs[(size_t)i].name], MorphKf{frame, m->pose.morphs[i]});
        keyed.insert({CanonicalRow(*m, RowKind::Morph, (uint32_t)i), frame});
    }
    std::vector<TrackState> after;
    after.reserve(before.size());
    for (const TrackState& s : before) after.push_back(CaptureTrack(d, s.model, s.kind, s.name));

    PoseLayer poseAfter = m->pose;
    for (int b : bones) poseAfter.bones.erase(b);
    for (int i : morphs) poseAfter.morphs.erase(i);
    if (poseAfter.Empty()) poseAfter.frame = -1;
    const char* name = allBones ? Tr("모든 본 등록") : Tr("포즈 등록");
    std::vector<std::unique_ptr<Command>> parts;
    parts.push_back(std::make_unique<TrackEditCommand>(d, name, std::move(before), std::move(after)));
    parts.push_back(std::make_unique<PoseEditCommand>(d, d.selectedModel, name, m->pose, std::move(poseAfter)));
    d.history.Push(std::make_unique<CompositeCommand>(name, std::move(parts)));
    d.selection = std::move(keyed);
    d.rowsKey = ~0ull;
}

void App::StudioResetPose() {
    StudioModel* m = StudioPoseModel();
    if (!m || m->pose.Empty()) return;
    const PoseLayer before = m->pose;
    m->pose = PoseLayer{};
    StudioSetPose(Tr("포즈 초기화"), before);
}

namespace {
// The layer to add edits to at `frame`: the current one, or a fresh one when it belongs to another frame.
PoseLayer LayerAt(const PoseLayer& pose, int frame) {
    PoseLayer l = pose.frame == frame ? pose : PoseLayer{};
    l.frame = frame;
    return l;
}
} // namespace

void App::StudioMirrorPose() {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    if (d.poseScope == 1 && d.selectedBones.empty()) {
        toast_ = {Tr("본을 먼저 선택하세요"), Tr("뷰포트에서 본을 클릭하거나 타임라인의 본 행을 선택하세요."), {}, false, timeSeconds_ + 3.0};
        return;
    }
    const PoseLayer before = m->pose;
    PoseLayer after = LayerAt(m->pose, d.Frame());
    const int n = MirrorPoseInto(*m->pmx, StudioCurrentPose(*m), d.poseScope == 1 ? &d.selectedBones : nullptr, after);
    if (n == 0) {
        toast_ = {Tr("좌우 반전할 포즈가 없어요"), Tr("좌우가 이미 대칭이거나 반전할 수 있는 본이 없어요."), {}, false, timeSeconds_ + 3.0};
        return;
    }
    m->pose = std::move(after);
    StudioSetPose(Tr("좌우 반전"), before);
}

void App::StudioImportVpd() {
    if (!StudioPoseModel()) return;
    const std::filesystem::path path = OpenFileDialog(hwnd_, {{L"VPD", L"*.vpd"}});
    if (!path.empty()) StudioImportVpdFrom(path);
}

void App::StudioImportVpdFrom(const std::filesystem::path& path) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    if (d.poseScope == 1 && d.selectedBones.empty()) {
        toast_ = {Tr("본을 먼저 선택하세요"), Tr("뷰포트에서 본을 클릭하거나 타임라인의 본 행을 선택하세요."), {}, false, timeSeconds_ + 3.0};
        return;
    }
    VpdPose vpd;
    std::string err;
    if (!LoadVpd(path, vpd, &err)) {
        toast_ = {Tr("VPD를 불러오지 못했습니다"), err, {}, true, timeSeconds_ + 5.0};
        return;
    }
    const PoseLayer before = m->pose;
    PoseLayer after = LayerAt(m->pose, d.Frame());
    std::vector<std::string> missing;
    const int n = ApplyVpdPose(*m->pmx, vpd, d.poseScope == 1 ? &d.selectedBones : nullptr, after, &missing);
    if (n == 0) {
        toast_ = {Tr("적용할 본이 없는 VPD입니다"), PathToUtf8(path.filename()), {}, true, timeSeconds_ + 5.0};
        return;
    }
    m->pose = std::move(after);
    StudioSetPose(Tr("VPD 불러오기"), before);
    char detail[160];
    std::snprintf(detail, sizeof(detail), Tr("%d개 적용 · 모델에 없는 이름 %d개"), n, (int)missing.size());
    toast_ = {Tr("VPD를 불러왔어요"), std::string(PathToUtf8(path.filename())) + "  ·  " + detail, {}, false, timeSeconds_ + 4.0};
    if (!missing.empty()) LOG_INFO("studio: VPD %s: %d names not in the model (first: %s)", PathToUtf8(path).c_str(),
                                   (int)missing.size(), missing.front().c_str());
}

void App::StudioExportVpd() {
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    std::wstring suggested = Utf8ToWide(m->name) + L".vpd";
    for (wchar_t& c : suggested)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const std::filesystem::path path = SaveFileDialog(hwnd_, {{L"VPD", L"*.vpd"}}, suggested, L"vpd");
    if (!path.empty()) StudioExportVpdTo(path);
}

bool App::StudioExportVpdTo(const std::filesystem::path& path) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return false;
    if (d.poseScope == 1 && d.selectedBones.empty()) {
        toast_ = {Tr("본을 먼저 선택하세요"), Tr("뷰포트에서 본을 클릭하거나 타임라인의 본 행을 선택하세요."), {}, false, timeSeconds_ + 3.0};
        return false;
    }
    std::vector<float> weights(m->pmx->morphs.size());
    for (size_t i = 0; i < weights.size(); ++i) weights[i] = m->inst->MorphWeight((int)i);
    if (m->pose.frame == d.Frame())
        for (const auto& [i, w] : m->pose.morphs)
            if (i >= 0 && (size_t)i < weights.size()) weights[(size_t)i] = w;
    const VpdPose vpd = MakeVpdPose(*m->pmx, StudioCurrentPose(*m), weights, d.poseScope == 1 ? &d.selectedBones : nullptr);
    std::string err;
    if (!SaveVpd(path, vpd, &err)) {
        toast_ = {Tr("VPD를 저장하지 못했습니다"), err, {}, true, timeSeconds_ + 5.0};
        return false;
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), Tr("본 %d개 · 모프 %d개"), (int)vpd.bones.size(), (int)vpd.morphs.size());
    toast_ = {Tr("VPD로 내보냈어요"), std::string(PathToUtf8(path.filename())) + "  ·  " + detail, path, false, timeSeconds_ + 5.0};
    return true;
}

// ---------------------------------------------------------------------------
// Viewport: overlay, picking, gizmo
// ---------------------------------------------------------------------------

namespace {
GizmoStyle MakeGizmoStyle() {
    GizmoStyle s;
    const float k = ui::Dpi();
    s.ringRadius *= k;
    s.arrowLength *= k;
    s.planeOffset *= k;
    s.planeSize *= k;
    s.hitDistance *= k;
    s.thickness *= k;
    return s;
}
BoneOverlayStyle MakeOverlayStyle() {
    BoneOverlayStyle s;
    const float k = ui::Dpi();
    s.jointRadius *= k;
    s.linkWidth *= k;
    s.pickRadius *= k;
    return s;
}
} // namespace

bool App::StudioGizmoFrameOf(const StudioModel& m, int bone, GizmoFrame& f, GizmoMode& mode) const {
    const StudioDoc& d = *studio_;
    const PmxModel& pmx = *m.pmx;
    if (bone < 0 || bone >= (int)pmx.bones.size()) return false;
    const PmxBone& b = pmx.bones[(size_t)bone];
    const bool movable = b.flags & PmxBone_Movable, rotatable = b.flags & PmxBone_Rotatable;
    if (!movable && !rotatable) return false;
    // the chosen tool if the bone allows it, else the other one
    mode = d.gizmoTool == 1 ? (movable ? GizmoMode::Translate : GizmoMode::Rotate)
                            : (rotatable ? GizmoMode::Rotate : GizmoMode::Translate);
    f = GizmoFrame{};
    f.center = BoneJointWorld(pmx, *m.inst, bone);
    const XMVECTOR world = RotationOf(m.inst->BoneWorld(bone));
    if (mode == GizmoMode::Rotate && (b.flags & PmxBone_FixedAxis)) {
        // fixed-axis bones (twist bones) turn about their axis only
        XMVECTOR a = XMLoadFloat3(&b.fixedAxis);
        if (XMVectorGetX(XMVector3LengthSq(a)) < 1e-12f) a = XMVectorSet(1, 0, 0, 0);
        a = XMVector3Normalize(XMVector3Rotate(XMVector3Normalize(a), world));
        XMStoreFloat3(&f.axes[0], a);
        Perpendiculars(a, f.axes[1], f.axes[2]);
        f.enabled[1] = f.enabled[2] = false;
        return true;
    }
    if (!d.gizmoLocal) return true;
    XMVECTOR x = XMVectorSet(1, 0, 0, 0), y = XMVectorSet(0, 1, 0, 0), z = XMVectorSet(0, 0, 1, 0);
    if (b.flags & PmxBone_LocalAxis) {
        const XMVECTOR lx = XMLoadFloat3(&b.localAxisX), lz = XMLoadFloat3(&b.localAxisZ);
        if (XMVectorGetX(XMVector3LengthSq(lx)) > 1e-12f && XMVectorGetX(XMVector3LengthSq(lz)) > 1e-12f) {
            x = XMVector3Normalize(lx);
            y = XMVector3Normalize(XMVector3Cross(lz, x));
            z = XMVector3Cross(x, y);
        }
    }
    XMStoreFloat3(&f.axes[0], XMVector3Normalize(XMVector3Rotate(x, world)));
    XMStoreFloat3(&f.axes[1], XMVector3Normalize(XMVector3Rotate(y, world)));
    XMStoreFloat3(&f.axes[2], XMVector3Normalize(XMVector3Rotate(z, world)));
    return true;
}

void App::StudioViewportPose(float x0, float y0, float x1, float y1, bool hovered, bool& consumed) {
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    StudioModel* m = StudioPoseModel();
    studioGizmoShown_ = false;
    if (!m || !m->visible || d.playing) {
        studioHoverBone_ = -1;
        studioGizmoHot_ = GizmoPart::None;
        if (studioViewDrag_ == 2) {  // playback started mid-drag: keep the edit made so far
            StudioSetPose(Tr("본 편집"), studioPoseBefore_);
            studioViewDrag_ = 0;
        }
        return;
    }
    const GizmoStyle gs = MakeGizmoStyle();
    const BoneOverlayStyle os = MakeOverlayStyle();
    if (d.activeBone >= (int)m->pmx->bones.size()) d.activeBone = -1;
    studioGizmoShown_ = d.activeBone >= 0 && StudioGizmoFrameOf(*m, d.activeBone, studioGizmoFrame_, studioGizmoMode_);
    const ImVec2 mouse = io.MousePos;

    // hover (not while dragging): the gizmo first, then the bones
    if (studioViewDrag_ != 2) {
        studioGizmoHot_ = hovered && studioGizmoShown_ ? GizmoHitTest(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, mouse)
                                                       : GizmoPart::None;
        studioHoverBone_ = hovered && d.showBones && studioGizmoHot_ == GizmoPart::None &&
                                       (studioViewDrag_ == 0 || (studioViewDrag_ == 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                               ? PickBone(studioVp_, *m->pmx, *m->inst, mouse, os)
                               : -1;
    }

    // press: start a gizmo drag or pick a bone (the camera keeps right/middle and empty-space drags)
    if (studioViewDrag_ == 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered) {
        if (studioGizmoHot_ != GizmoPart::None) {
            studioGizmoDrag_ = BeginGizmoDrag(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, studioGizmoHot_, mouse);
            if (studioGizmoDrag_.part != GizmoPart::None) {
                const int bone = d.activeBone;
                studioViewDrag_ = 2;
                studioDragBone_ = bone;
                studioPoseBefore_ = m->pose;
                const auto it = m->pose.bones.find(bone);
                studioDragBase_ = it != m->pose.bones.end() && m->pose.frame == d.Frame()
                                      ? it->second
                                      : PoseBone{m->inst->BoneAnimTranslation(bone), m->inst->BoneAnimRotation(bone)};
                const int parent = m->pmx->bones[(size_t)bone].parentIndex;
                XMStoreFloat4(&studioDragParentRot_, parent >= 0 && parent < (int)m->pmx->bones.size()
                                                          ? RotationOf(m->inst->BoneWorld(parent))
                                                          : XMQuaternionIdentity());
                studioDragScale_ = m->inst->Scale();
            }
        } else if (studioHoverBone_ >= 0) {
            StudioSelectBone(studioHoverBone_, io.KeyCtrl);
            studioViewDrag_ = 3;
        }
    }

    // gizmo drag: the bone's value = start value composed with the drag (live, through the pose layer)
    if (studioViewDrag_ == 2) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // cancel
            m->pose = studioPoseBefore_;
            studioViewDrag_ = 4;  // swallow the rest of this press
        } else {
            PoseBone v = studioDragBase_;
            const XMVECTOR parent = XMLoadFloat4(&studioDragParentRot_);
            if (studioGizmoMode_ == GizmoMode::Translate) {
                XMFLOAT3 w = GizmoDragTranslation(studioGizmoDrag_, mouse);
                XMVECTOR dt = XMVectorScale(XMLoadFloat3(&w), 1.0f / std::max(studioDragScale_, 1e-4f));
                dt = XMVector3Rotate(dt, XMQuaternionInverse(parent));  // world -> parent frame (the key's space)
                XMStoreFloat3(&v.t, XMVectorAdd(XMLoadFloat3(&studioDragBase_.t), dt));
            } else {
                XMFLOAT3 axis;
                const float angle = GizmoDragAngle(studioGizmoDrag_, mouse, &axis);
                if (angle != 0.0f) {
                    // world rotation q about `axis` after the bone's rotation: local' = local * (P q P^-1)
                    const XMVECTOR q = XMQuaternionRotationAxis(XMVector3Normalize(XMLoadFloat3(&axis)), angle);
                    const XMVECTOR dq = XMQuaternionMultiply(XMQuaternionMultiply(parent, q), XMQuaternionInverse(parent));
                    XMStoreFloat4(&v.r, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&studioDragBase_.r), dq)));
                }
            }
            PoseLayer& pose = m->pose;
            if (pose.frame != d.Frame()) pose = PoseLayer{};
            pose.frame = d.Frame();
            pose.bones[studioDragBone_] = v;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                StudioSetPose(studioGizmoMode_ == GizmoMode::Translate ? Tr("본 이동") : Tr("본 회전"), studioPoseBefore_);
                studioViewDrag_ = 0;
            }
        }
    }
    if (studioViewDrag_ >= 3) consumed = true;
    if ((studioViewDrag_ == 3 || studioViewDrag_ == 4) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) studioViewDrag_ = 0;

    // overlay
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    if (d.showBones)
        DrawBoneOverlay(dl, studioVp_, *m->pmx, *m->inst, d.selectedBones, d.activeBone, studioHoverBone_, os);
    if (studioGizmoShown_)
        DrawGizmo(dl, studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, studioViewDrag_ == 2 ? GizmoPart::None : studioGizmoHot_,
                  studioViewDrag_ == 2 ? studioGizmoDrag_.part : GizmoPart::None);
    dl->PopClipRect();
    if (studioHoverBone_ >= 0 && studioViewDrag_ == 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ui::Tooltip(m->pmx->bones[(size_t)studioHoverBone_].name.c_str());
    }
}

void App::StudioViewportToolbar(float x, float cy) {
    using namespace ui;
    StudioDoc& d = *studio_;
    if (!StudioPoseModel()) return;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b = 30.0f, gap = 2.0f;
    const float w = Dp(4 * b + 3 * gap + 2 * 9.0f + 8.0f);
    dl->AddRectFilled(ImVec2(x, cy - Dp(b * 0.5f + 4.0f)), ImVec2(x + w, cy + Dp(b * 0.5f + 4.0f)), WithAlpha(p.surface, 0.85f),
                      Dp(10.0f));
    ImGui::SetCursorScreenPos(ImVec2(x + Dp(4.0f), cy - Dp(b * 0.5f)));
    if (IconButton("##vpbones", icon::Bone, d.showBones ? Tr("본 숨기기") : Tr("본 표시"), d.showBones, b)) d.showBones = !d.showBones;
    ImGui::SameLine(0, Dp(9.0f));
    if (IconButton("##vprot", icon::Refresh, Tr("회전  (E)"), d.gizmoTool == 0, b)) d.gizmoTool = 0;
    ImGui::SameLine(0, Dp(gap));
    if (IconButton("##vpmove", icon::ArrowsOutCardinal, Tr("이동  (W)"), d.gizmoTool == 1, b)) d.gizmoTool = 1;
    ImGui::SameLine(0, Dp(9.0f));
    if (IconButton("##vplocal", d.gizmoLocal ? icon::Cube : icon::Globe,
                   d.gizmoLocal ? Tr("로컬 축 (클릭: 전역 축)  (L)") : Tr("전역 축 (클릭: 로컬 축)  (L)"), false, b))
        d.gizmoLocal = !d.gizmoLocal;
}

bool App::StudioScriptGizmoPoint(int part, ImVec2& out) const {
    if (!studioGizmoShown_) return false;
    const GizmoStyle gs = MakeGizmoStyle();
    ImVec2 c;
    if (!studioVp_.Project(studioGizmoFrame_.center, c)) return false;
    // the hit point of that part farthest from the centre (arrow tips, ring rims)
    float best = -1.0f;
    const float r = gs.ringRadius + gs.arrowLength;
    for (float y = -r; y <= r; y += 2.0f)
        for (float x = -r; x <= r; x += 2.0f) {
            const ImVec2 q(c.x + x, c.y + y);
            if ((int)GizmoHitTest(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, q) != part) continue;
            const float dist = x * x + y * y;
            if (dist > best) {
                best = dist;
                out = q;
            }
        }
    if (best < 0.0f) return false;
    // stay a little inside the far end so the press lands on the part (keep the far point if that misses)
    const ImVec2 in(c.x + (out.x - c.x) * 0.92f, c.y + (out.y - c.y) * 0.92f);
    if ((int)GizmoHitTest(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, in) == part) out = in;
    return true;
}

// ---------------------------------------------------------------------------
// Inspector tabs
// ---------------------------------------------------------------------------

void App::DrawStudioBoneTab(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const PmxModel& pmx = *m->pmx;
    const int frame = d.Frame();
    const auto caption = [&](const char* text) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, text);
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
    };
    const auto wrapped = [&](const char* text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped("%s", text);
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    };

    // --- the active bone
    const int bone = d.activeBone >= 0 && d.activeBone < (int)pmx.bones.size() ? d.activeBone : -1;
    if (bone < 0) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, Tr("선택한 본 없음"));
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
        wrapped(Tr("뷰포트에서 본을 클릭하거나 타임라인의 본 행을 선택하세요. 기즈모를 드래그하면 포즈가 바뀌고, I로 키를 등록합니다."));
    } else {
        const PmxBone& b = pmx.bones[(size_t)bone];
        {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            TextEllipsis(cdl, Font::Semibold, size::Body, c, c.x + w, p.ink, b.name.c_str());
            ImGui::Dummy(ImVec2(w, Dp(26.0f)));
        }
        {
            // capability badges
            ImVec2 c = ImGui::GetCursorScreenPos(), bs;
            const auto badge = [&](const char* t, bool accent) {
                Badge(cdl, c, t, accent ? p.accentSoft : WithAlpha(p.ink, 0.06f), accent ? p.accentInk : p.ink2, &bs);
                c.x += bs.x + Dp(6.0f);
            };
            if (b.flags & PmxBone_Rotatable) badge(Tr("회전"), false);
            if (b.flags & PmxBone_Movable) badge(Tr("이동"), false);
            if (b.flags & PmxBone_IK) badge("IK", true);
            if (b.flags & PmxBone_FixedAxis) badge(Tr("축 고정"), false);
            if (d.selectedBones.size() > 1) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), Tr("본 %d개 선택"), (int)d.selectedBones.size());
                badge(buf, false);
            }
            ImGui::Dummy(ImVec2(w, Dp(30.0f)));
        }
        const PoseBone cur = StudioCurrentPose(*m)[(size_t)bone];
        const bool edited = m->pose.frame == frame && m->pose.bones.count(bone);
        // numeric fields: one undo step per field edit
        const auto field = [&](const char* id, const char* label, float v[3], bool enabled, float speed, const char* fmt) {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), enabled ? p.ink3 : WithAlpha(p.ink3, 0.5f), label);
            ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(52.0f), c.y));
            ImGui::SetNextItemWidth(w - Dp(52.0f));
            ImGui::BeginDisabled(!enabled);
            PushFont(Font::Regular, size::Small);
            const bool changed = ImGui::DragFloat3(id, v, speed, 0.0f, 0.0f, fmt);
            PopFont();
            const bool started = ImGui::IsItemActivated(), ended = ImGui::IsItemDeactivated();
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
            if (started && !studioPoseFieldEdit_) {
                studioPoseBefore_ = m->pose;
                studioPoseFieldEdit_ = true;
            }
            return std::pair<bool, bool>{changed, ended};
        };
        float t[3] = {cur.t.x, cur.t.y, cur.t.z};
        const XMFLOAT3 e = EulerDeg(cur.r);
        float r[3] = {e.x, e.y, e.z};
        const auto [tChanged, tEnded] = field("##bonet", Tr("위치"), t, b.flags & PmxBone_Movable, 0.02f, "%.2f");
        const auto [rChanged, rEnded] = field("##boner", Tr("회전"), r, b.flags & PmxBone_Rotatable, 0.25f, "%.1f°");
        if (tChanged || rChanged) {
            PoseLayer& pose = m->pose;
            if (pose.frame != frame) pose = PoseLayer{};
            pose.frame = frame;
            PoseBone v = cur;
            if (tChanged) v.t = {t[0], t[1], t[2]};
            if (rChanged) v.r = QuatFromEulerDeg({r[0], r[1], r[2]});
            pose.bones[bone] = v;
        }
        if ((tEnded || rEnded) && studioPoseFieldEdit_) {
            StudioSetPose(Tr("본 값 편집"), studioPoseBefore_);
            studioPoseFieldEdit_ = false;
        }
        {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            const auto it = m->motion.bones.find(b.name);
            const bool keyed = it != m->motion.bones.end() && FindKey(it->second, frame);
            Text(cdl, Font::Regular, size::Caption, c, edited ? p.warn : p.ink3,
                 edited ? Tr("편집됨 · 등록하지 않음") : keyed ? Tr("이 프레임에 키 있음") : Tr("모션 값 (보간)"));
            ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        }
    }

    // --- unregistered edits: register / reset
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    cdl->AddLine(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y), p.line);
    ImGui::Dummy(ImVec2(w, Dp(12.0f)));
    caption(Tr("포즈"));
    {
        const bool has = !m->pose.Empty() && m->pose.frame == frame;
        char buf[128];
        if (has) std::snprintf(buf, sizeof(buf), Tr("등록하지 않은 편집: 본 %d개 · 모프 %d개"), (int)m->pose.bones.size(),
                               (int)m->pose.morphs.size());
        else std::snprintf(buf, sizeof(buf), "%s", Tr("등록하지 않은 편집 없음"));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, c, has ? p.ink : p.ink3, buf);
        ImGui::Dummy(ImVec2(w, Dp(26.0f)));
        const float bw = (w / Dpi() - 8.0f) * 0.5f;
        if (Button("##poseregister", Tr("키 등록"), icon::Diamond, ButtonKind::Primary, ImVec2(bw, 34.0f)))
            StudioRegisterPose(ImGui::GetIO().KeyCtrl);
        Tooltip(Tr("편집한 본과 모프를 현재 프레임에 키로 등록  (I)\nCtrl+클릭: 모든 본 등록  (Ctrl+I)"));
        ImGui::SameLine(0, Dp(8.0f));
        ImGui::BeginDisabled(!has);
        if (Button("##posereset", Tr("편집 취소"), icon::ArrowCcw, ButtonKind::Secondary, ImVec2(bw, 34.0f))) StudioResetPose();
        Tooltip(Tr("등록하지 않은 편집을 버리고 모션 값으로 돌아가기"));
        ImGui::EndDisabled();
    }

    // --- pose files and mirror
    ImGui::Dummy(ImVec2(w, Dp(14.0f)));
    caption(Tr("포즈 파일 · 좌우 반전"));
    {
        const char* scopes[] = {Tr("모델 전체"), Tr("선택한 본")};
        Segmented("##posescope", scopes, 2, &d.poseScope, w / Dpi(), 30.0f);
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        const float bw = (w / Dpi() - 16.0f) / 3.0f;
        if (Button("##vpdimport", Tr("VPD 열기"), icon::FolderOpen, ButtonKind::Secondary, ImVec2(bw, 34.0f))) StudioImportVpd();
        Tooltip(Tr("VPD 포즈 파일을 등록하지 않은 편집으로 불러오기"));
        ImGui::SameLine(0, Dp(8.0f));
        if (Button("##vpdexport", Tr("VPD 저장"), icon::Export, ButtonKind::Secondary, ImVec2(bw, 34.0f))) StudioExportVpd();
        Tooltip(Tr("지금 포즈를 VPD 파일로 저장"));
        ImGui::SameLine(0, Dp(8.0f));
        if (Button("##mirror", Tr("좌우 반전"), icon::CircleHalf, ButtonKind::Secondary, ImVec2(bw, 34.0f))) StudioMirrorPose();
        Tooltip(Tr("왼쪽(左)과 오른쪽(右) 본의 포즈를 맞바꿔 반전"));
    }
    ImGui::Dummy(ImVec2(w, Dp(16.0f)));
}

void App::DrawStudioMorphTab(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    const Palette& p = P();
    const PmxModel& pmx = *m->pmx;
    const int frame = d.Frame();

    SearchField("##morphfilter", d.morphFilter, sizeof(d.morphFilter), Tr("모프 검색"), w / Dpi());
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    {
        const bool has = m->pose.frame == frame && !m->pose.morphs.empty();
        const float bw = (w / Dpi() - 8.0f) * 0.5f;
        if (Button("##morphregister", Tr("키 등록"), icon::Diamond, ButtonKind::Primary, ImVec2(bw, 32.0f))) StudioRegisterPose(false);
        Tooltip(Tr("편집한 본과 모프를 현재 프레임에 키로 등록  (I)"));
        ImGui::SameLine(0, Dp(8.0f));
        ImGui::BeginDisabled(!has);
        if (Button("##morphreset", Tr("편집 취소"), icon::ArrowCcw, ButtonKind::Secondary, ImVec2(bw, 32.0f))) {
            const PoseLayer before = m->pose;
            m->pose.morphs.clear();
            StudioSetPose(Tr("모프 편집 취소"), before);
        }
        ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));

    std::vector<int> slots[4];
    for (size_t i = 0; i < pmx.morphs.size(); ++i) {
        if (d.morphFilter[0] && pmx.morphs[i].name.find(d.morphFilter) == std::string::npos &&
            pmx.morphs[i].nameEn.find(d.morphFilter) == std::string::npos)
            continue;
        slots[MorphPanelSlot(pmx.morphs[i].panel)].push_back((int)i);
    }
    const float rowH = Dp(28.0f);
    for (int s = 0; s < 4; ++s) {
        if (slots[s].empty()) continue;
        ImGui::PushID(s);
        // group header (click to fold)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        bool& open = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##open"), true);
        if (ImGui::InvisibleButton("##hdr", ImVec2(w, Dp(26.0f)))) open = !open;
        const bool hh = ImGui::IsItemHovered();
        if (hh) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + Dp(26.0f)), WithAlpha(p.ink, 0.05f), Dp(6.0f));
        Icon(dl, open ? icon::CaretDown : icon::CaretRight, 12.0f, ImVec2(a.x + Dp(9.0f), a.y + Dp(13.0f)), p.ink3);
        char head[64];
        std::snprintf(head, sizeof(head), "%s  %d", Tr(kMorphPanelNames[s]), (int)slots[s].size());
        Text(dl, Font::Semibold, size::Caption, ImVec2(a.x + Dp(20.0f), a.y + Dp(5.0f)), p.ink2, head);
        if (open) {
            for (int i : slots[s]) {
                ImGui::PushID(i);
                const auto ov = m->pose.frame == frame ? m->pose.morphs.find(i) : m->pose.morphs.end();
                const bool edited = m->pose.frame == frame && ov != m->pose.morphs.end();
                float v = edited ? ov->second : m->inst->MorphWeight(i);
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const ImGuiID id = ImGui::GetID("##slider");
                const ImRect bb(c, ImVec2(c.x + w, c.y + rowH));
                ImGui::ItemSize(bb);
                if (ImGui::ItemAdd(bb, id)) {
                    bool hovered = false, held = false;
                    ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
                    const float labelW = w * 0.42f, valueW = Dp(40.0f);
                    const float sx0 = c.x + labelW + Dp(6.0f), sx1 = c.x + w - valueW - Dp(4.0f);
                    if (ImGui::IsItemActivated() && !studioPoseFieldEdit_) {
                        studioPoseBefore_ = m->pose;
                        studioPoseFieldEdit_ = true;
                    }
                    if (held) {
                        const float nv = std::clamp((ImGui::GetIO().MousePos.x - sx0) / std::max(sx1 - sx0, 1.0f), 0.0f, 1.0f);
                        const float snapped = std::round(nv * 100.0f) / 100.0f;
                        if (!edited || snapped != v) {
                            PoseLayer& pose = m->pose;
                            if (pose.frame != frame) pose = PoseLayer{};
                            pose.frame = frame;
                            pose.morphs[i] = snapped;
                            v = snapped;
                        }
                    }
                    if (ImGui::IsItemDeactivated() && studioPoseFieldEdit_) {
                        StudioSetPose(Tr("모프 편집"), studioPoseBefore_);
                        studioPoseFieldEdit_ = false;
                    }
                    if (hovered || held) dl->AddRectFilled(c, bb.Max, WithAlpha(p.ink, 0.04f), Dp(6.0f));
                    const auto it = m->motion.morphs.find(pmx.morphs[(size_t)i].name);
                    const bool keyed = it != m->motion.morphs.end() && FindKey(it->second, frame);
                    if (keyed) Icon(dl, icon::Diamond, 9.0f, ImVec2(c.x + Dp(6.0f), c.y + rowH * 0.5f), p.accent);
                    TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x + Dp(14.0f), c.y + Dp(5.0f)), c.x + labelW,
                                 edited ? p.warn : p.ink, pmx.morphs[(size_t)i].name.c_str());
                    const float cy = c.y + rowH * 0.5f, th = Dp(4.0f);
                    dl->AddRectFilled(ImVec2(sx0, cy - th * 0.5f), ImVec2(sx1, cy + th * 0.5f), p.sunken, th);
                    dl->AddRectFilled(ImVec2(sx0, cy - th * 0.5f), ImVec2(sx0 + (sx1 - sx0) * std::clamp(v, 0.0f, 1.0f), cy + th * 0.5f),
                                      edited ? p.warn : p.accent, th);
                    dl->AddCircleFilled(ImVec2(sx0 + (sx1 - sx0) * std::clamp(v, 0.0f, 1.0f), cy), Dp(hovered || held ? 6.5f : 5.5f),
                                        IM_COL32(255, 255, 255, 255), 16);
                    dl->AddCircle(ImVec2(sx0 + (sx1 - sx0) * std::clamp(v, 0.0f, 1.0f), cy), Dp(hovered || held ? 6.5f : 5.5f),
                                  WithAlpha(p.ink, 0.25f), 16);
                    char val[16];
                    std::snprintf(val, sizeof(val), "%.2f", v);
                    const ImVec2 vs = TextSize(Font::Semibold, size::Caption, val);
                    Text(dl, Font::Semibold, size::Caption, ImVec2(c.x + w - vs.x, c.y + Dp(6.0f)), p.ink2, val);
                    if (hovered) Tooltip(pmx.morphs[(size_t)i].name.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }
    if (pmx.morphs.empty()) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, c, p.ink3, Tr("이 모델에는 모프가 없어요"));
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
    }
    ImGui::Dummy(ImVec2(w, Dp(12.0f)));
}

} // namespace mmdx
