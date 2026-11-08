// Studio pose editing: the viewport bone overlay, bone picking, the translate/rotate gizmo, the pose layer
// (unregistered edits, register / reset), the bone and morph inspector tabs, VPD pose files and the mirror pose.
// The pose layer overrides the evaluated motion at its frame (StudioApplyPose); dragging never re-binds the motion.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "anim/ModelInstance.h"
#include "app/Icons.h"
#include "app/Lighting.h"
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

const char* const kMorphPanelNames[4] = {TrKey("눈"), TrKey("눈썹"), TrKey("입"), TrKey("기타")};  // panels 2, 1, 3, 4 (0 = other)
int MorphPanelSlot(uint8_t panel) { return panel == 2 ? 0 : panel == 1 ? 1 : panel == 3 ? 2 : 3; }

} // namespace

// ---------------------------------------------------------------------------
// Pose layer
// ---------------------------------------------------------------------------

StudioModel* App::StudioPoseModel() {
    if (!studio_) return nullptr;
    StudioModel* m = studio_->Selected();
    return m;  // characters, props and stages (stage motions key bones too)
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

bool App::StudioApplyPose(StudioModel& m) {
    if (m.pose.Empty() || m.pose.frame != studio_->Frame()) return false;
    // a video renders the registered motion only (MMD); a still shows the edits like the viewport
    if (offline_.studio && offline_.mode != OfflineMode::Still) return false;
    for (const auto& [b, v] : m.pose.bones) m.inst->SetBoneAnim(b, v.t, v.r);
    for (const auto& [i, w] : m.pose.morphs) m.inst->SetMorphWeight(i, w);
    return true;
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
    // auto-key: the finished edit is keyed under the playhead right away (one undo step for edit + keys)
    if (d.autoKey && !a.Empty() && a.frame == d.Frame()) {
        StudioRegisterPose(false, &b, undoName);
        return;
    }
    d.history.Push(std::make_unique<PoseEditCommand>(d, d.selectedModel, undoName, std::move(b), std::move(a)));
}

// The layer an undo of the gesture restores, and the model it is made on: a gesture whose field goes away before it
// sees its end is registered there (DrawStudio).
void App::StudioBeginPoseFieldEdit(const StudioModel& m) {
    if (studioPoseFieldEdit_) return;
    studioPoseBefore_ = m.pose;
    studioPoseFieldModel_ = m.uid;
    studioPoseFieldEdit_ = true;
}

void App::StudioSelectAllBones() {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) return;
    d.selectedBones.clear();
    for (size_t i = 0; i < m->pmx->bones.size(); ++i) d.selectedBones.insert((int)i);
    d.activeBone = d.selectedBones.empty() ? -1 : *d.selectedBones.rbegin();
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
    d.rowsKey = ~0ull;
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
        // a fresh pick shows the bone's values: the inspector switches to 본 (and scrolls there once, on the switch)
        if (d.inspectorTab != 1) {
            d.inspectorTab = 1;
            studioInspectorToBones_ = true;
        }
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

// Inspector → timeline sync for a morph (like StudioSelectBone for bones): the morph's row is picked, its group
// un-collapsed and scrolled into view, and its key at the playhead selected if there is one. No tab switch.
void App::StudioSelectMorph(int morph) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m || morph < 0 || morph >= (int)m->pmx->morphs.size()) return;
    const int frame = d.Frame();
    const uint64_t row = CanonicalRow(*m, RowKind::Morph, (uint32_t)morph);
    d.selectedRows.clear();
    d.selection.clear();
    d.selectedRows.insert(row);
    d.rowAnchor = row;
    const auto it = m->motion.morphs.find(m->pmx->morphs[(size_t)morph].name);
    if (it != m->motion.morphs.end() && FindKey(it->second, frame)) d.selection.insert({row, frame});
    d.collapsed.erase(MakeRowId(RowKind::Group, RowGroupOf(row), 0));  // reveal the row
    d.scrollRowId = row;
    d.scrollToRow = 3;
    d.rowsKey = ~0ull;
}

void App::StudioRegisterPose(bool allBones, const PoseLayer* layerBefore, const char* undoName) {
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
    for (const TrackState& s : before) after.push_back(CaptureTrack(d, s));

    PoseLayer poseAfter = m->pose;
    for (int b : bones) poseAfter.bones.erase(b);
    for (int i : morphs) poseAfter.morphs.erase(i);
    if (poseAfter.Empty()) poseAfter.frame = -1;
    const char* name = undoName ? undoName : allBones ? Tr("모든 본 등록") : Tr("포즈 등록");
    std::vector<std::unique_ptr<Command>> parts;
    parts.push_back(std::make_unique<TrackEditCommand>(d, name, std::move(before), std::move(after)));
    // undo returns to the layer as it was before the edit (auto-key passes it), not to the half-keyed one
    parts.push_back(std::make_unique<PoseEditCommand>(d, d.selectedModel, name, layerBefore ? *layerBefore : m->pose, std::move(poseAfter)));
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
BoneOverlayStyle MakeOverlayStyle(float scale = 1.0f) {
    BoneOverlayStyle s;
    const float k = ui::Dpi() * std::clamp(scale, 0.5f, 2.5f);
    s.jointRadius *= k;
    s.linkWidth *= k;
    s.pickRadius *= k;
    s.minPixelRadius *= k;
    s.maxPixelRadius *= k;
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
    // auto-IK: a wrist / any bone whose parent chain has >= 2 rotatable bones gets a translate gizmo
    int ikRoot = -1, ikMid = -1;
    if (d.autoIk && FindAutoIkChain(pmx, bone, ikRoot, ikMid)) {
        mode = GizmoMode::Translate;
    } else {
        // the chosen tool if the bone allows it, else the other one
        mode = d.gizmoTool == 1 ? (movable ? GizmoMode::Translate : GizmoMode::Rotate)
                                : (rotatable ? GizmoMode::Rotate : GizmoMode::Translate);
    }
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
            studioAutoIkActive_ = false;
        }
        if (studioViewDrag_ == 6 && d.Selected()) {
            StudioCommitPlace(d.Selected()->uid, studioPlaceBefore_, d.Selected()->place);
            studioPlaceDragging_ = false;
            studioViewDrag_ = 0;
        }
        // a light drag (studioViewDrag_ 8) belongs to StudioViewportLightHandles: no model is selected while a light is
        return;

    }
    const GizmoStyle gs = MakeGizmoStyle();
    BoneOverlayStyle os = MakeOverlayStyle(settings_.boneOverlayScale);
    os.showPhysicsBones = studio_->showPhysicsBones;
    if (m && d.activeBone >= (int)m->pmx->bones.size()) d.activeBone = -1;
    studioGizmoShown_ = m && d.activeBone >= 0 && StudioGizmoFrameOf(*m, d.activeBone, studioGizmoFrame_, studioGizmoMode_);
    // no bone picked: the toolbar's model tool moves / rotates the whole character / stage (world axes at its origin)
    StudioModel* const mm = m;
    const bool modelTool = d.modelGizmo && d.activeBone < 0 && mm;
    if (modelTool) {
        studioGizmoFrame_ = GizmoFrame{};
        studioGizmoFrame_.center = mm->place.translation;
        studioGizmoMode_ = d.gizmoTool == 1 ? GizmoMode::Translate : GizmoMode::Rotate;
        studioGizmoShown_ = true;
    }
    const ImVec2 mouse = io.MousePos;

    // Precision modifier (Ctrl) while a gizmo drag is live: the pointer motion is dampened through a running
    // effective-mouse. Because the gizmo solvers recompute the value from the drag-start base every frame, this
    // re-anchors cleanly when Ctrl toggles mid-drag (no jump) and the single undo step is unchanged.
    ImVec2 gizmoMouse = mouse;
    if (studioViewDrag_ == 2) {
        const bool ctrl = io.KeyCtrl;
        if (!studioGizmoInit_ || ctrl != studioGizmoCtrl_) {
            studioGizmoEffMouse_ = mouse;
            studioGizmoLastRaw_ = mouse;
            studioGizmoCtrl_ = ctrl;
            studioGizmoInit_ = true;
        }
        const float factor = ctrl ? std::clamp(settings_.editorPrecisionFactor, 0.01f, 1.0f) : 1.0f;
        studioGizmoEffMouse_.x += (mouse.x - studioGizmoLastRaw_.x) * factor;
        studioGizmoEffMouse_.y += (mouse.y - studioGizmoLastRaw_.y) * factor;
        studioGizmoLastRaw_ = mouse;
        gizmoMouse = studioGizmoEffMouse_;
    } else if (studioGizmoInit_) {
        studioGizmoInit_ = false;
    }

    // hover (not while dragging): the gizmo first, then the bones
    if (studioViewDrag_ != 2) {
        studioGizmoHot_ = hovered && studioGizmoShown_ ? GizmoHitTest(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, mouse)
                                                       : GizmoPart::None;
        studioHoverBone_ = hovered && m && d.showBones && studioGizmoHot_ == GizmoPart::None &&
                                       (studioViewDrag_ == 0 || (studioViewDrag_ == 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                               ? PickBone(studioVp_, *m->pmx, *m->inst, mouse, os)
                               : -1;
    }

    // press: start a gizmo drag or pick a bone (the camera keeps right/middle and empty-space drags)
    if (studioViewDrag_ == 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered && !io.KeyAlt) {  // Alt+LMB orbits
        if (studioGizmoHot_ != GizmoPart::None) {
            studioGizmoDrag_ = BeginGizmoDrag(studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, studioGizmoHot_, mouse);
            if (studioGizmoDrag_.part != GizmoPart::None && modelTool) {
                studioViewDrag_ = 6;  // model placement drag
                studioPlaceBefore_ = mm->place;
                studioPlaceModel_ = mm->uid;
                studioPlaceDragging_ = true;
            } else if (studioGizmoDrag_.part != GizmoPart::None) {
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
                // auto-IK: capture the chain's world state so the drag can solve it
                studioAutoIkActive_ = false;
                int ikRoot = -1, ikMid = -1;
                if (d.autoIk && studioGizmoMode_ == GizmoMode::Translate && FindAutoIkChain(*m->pmx, bone, ikRoot, ikMid)) {
                    const PmxModel& pmx = *m->pmx;
                    const int endParent = pmx.bones[(size_t)bone].parentIndex;
                    const int midParent = pmx.bones[(size_t)ikMid].parentIndex;
                    const int rootParent = pmx.bones[(size_t)ikRoot].parentIndex;
                    studioAutoIkDrag_ = AutoIkDrag{};
                    studioAutoIkDrag_.root = ikRoot;
                    studioAutoIkDrag_.mid = ikMid;
                    studioAutoIkDrag_.end = bone;
                    studioAutoIkDrag_.rootPos = BoneJointWorld(pmx, *m->inst, ikRoot);
                    studioAutoIkDrag_.midPos = BoneJointWorld(pmx, *m->inst, ikMid);
                    studioAutoIkDrag_.endPos = BoneJointWorld(pmx, *m->inst, bone);
                    XMStoreFloat4(&studioAutoIkDrag_.rootWorld, RotationOf(m->inst->BoneWorld(ikRoot)));
                    XMStoreFloat4(&studioAutoIkDrag_.midWorld, RotationOf(m->inst->BoneWorld(ikMid)));
                    XMStoreFloat4(&studioAutoIkDrag_.endWorld, RotationOf(m->inst->BoneWorld(bone)));
                    XMStoreFloat4(&studioAutoIkDrag_.rootParentWorld,
                                  rootParent >= 0 ? RotationOf(m->inst->BoneWorld(rootParent)) : XMQuaternionIdentity());
                    XMStoreFloat4(&studioAutoIkDrag_.midParentWorld,
                                  midParent >= 0 ? RotationOf(m->inst->BoneWorld(midParent)) : XMQuaternionIdentity());
                    XMStoreFloat4(&studioAutoIkDrag_.endParentWorld,
                                  endParent >= 0 ? RotationOf(m->inst->BoneWorld(endParent)) : XMQuaternionIdentity());
                    studioAutoIkDrag_.bendNormal =
                        AutoIkBendNormal(pmx, ikRoot, ikMid, bone, studioAutoIkDrag_.rootPos, studioAutoIkDrag_.midPos,
                                         studioAutoIkDrag_.endPos);
                    studioAutoIkDrag_.valid = true;
                    studioAutoIkActive_ = true;
                }
            }
        } else if (studioHoverBone_ >= 0) {
            // Overlapping joints (e.g. an IK helper on a rotatable bone): repeated clicks at the same spot cycle
            // through the candidates instead of always picking the same one.
            int pick = studioHoverBone_;
            if (!io.KeyCtrl && !io.KeyShift) {
                const std::vector<int> cands = PickBoneCandidates(studioVp_, *m->pmx, *m->inst, mouse, os);
                if (cands.size() > 1) {
                    const bool sameSpot = std::hypot(mouse.x - studioCyclePos_.x, mouse.y - studioCyclePos_.y) <= ui::Dp(4.0f);
                    if (sameSpot) {
                        auto it = std::find(cands.begin(), cands.end(), studioCycleBone_);
                        if (it != cands.end()) {
                            ++it;
                            if (it == cands.end()) it = cands.begin();
                            pick = *it;
                        } else pick = cands.front();
                    } else pick = cands.front();
                }
                studioCyclePos_ = mouse;
                studioCycleBone_ = pick;
            }
            StudioSelectBone(pick, io.KeyCtrl || io.KeyShift);
            if (studioCenterOnBone_ && m && m->inst) {
                StudioTakeFreeCamera();
                // centre on the bone without zooming: re-aim the target, keep the distance
                freeCam_.target = m->inst->BoneWorldPosition(studioHoverBone_);
            }
            studioViewDrag_ = 3;
        }
    }

    // gizmo drag: the bone's value = start value composed with the drag (live, through the pose layer)
    if (studioViewDrag_ == 2) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // cancel
            m->pose = studioPoseBefore_;
            studioViewDrag_ = 4;  // swallow the rest of this press
            studioAutoIkActive_ = false;
        } else {
            PoseBone v = studioDragBase_;
            const XMVECTOR parent = XMLoadFloat4(&studioDragParentRot_);
            if (studioAutoIkActive_) {
                // auto-IK: solve the two parent bones so the end bone follows the gizmo (world), keep its orientation
                const XMFLOAT3 w = GizmoDragTranslation(studioGizmoDrag_, gizmoMouse);
                const XMFLOAT3 target{w.x + studioAutoIkDrag_.endPos.x, w.y + studioAutoIkDrag_.endPos.y,
                                      w.z + studioAutoIkDrag_.endPos.z};
                AutoIkResult r;
                if (SolveAutoIkDrag(studioAutoIkDrag_, target, &r)) {
                    PoseLayer& pose = m->pose;
                    if (pose.frame != d.Frame()) pose = PoseLayer{};
                    pose.frame = d.Frame();
                    // start values (layer overrides or the evaluated anim) keep their translations; only r changes
                    const auto startBone = [&](int b) {
                        const auto it = studioPoseBefore_.bones.find(b);
                        return it != studioPoseBefore_.bones.end() && studioPoseBefore_.frame == d.Frame()
                                   ? it->second
                                   : PoseBone{m->inst->BoneAnimTranslation(b), m->inst->BoneAnimRotation(b)};
                    };
                    PoseBone rb = startBone(studioAutoIkDrag_.root);
                    PoseBone mb = startBone(studioAutoIkDrag_.mid);
                    PoseBone eb = startBone(studioAutoIkDrag_.end);
                    rb.r = r.rootLocal;
                    mb.r = r.midLocal;
                    eb.r = r.endLocal;
                    pose.bones[studioAutoIkDrag_.root] = rb;
                    pose.bones[studioAutoIkDrag_.mid] = mb;
                    pose.bones[studioAutoIkDrag_.end] = eb;
                }
            } else if (studioGizmoMode_ == GizmoMode::Translate) {
                XMFLOAT3 w = GizmoDragTranslation(studioGizmoDrag_, gizmoMouse);
                XMVECTOR dt = XMVectorScale(XMLoadFloat3(&w), 1.0f / std::max(studioDragScale_, 1e-4f));
                dt = XMVector3Rotate(dt, XMQuaternionInverse(parent));  // world -> parent frame (the key's space)
                XMStoreFloat3(&v.t, XMVectorAdd(XMLoadFloat3(&studioDragBase_.t), dt));
            } else {
                XMFLOAT3 axis;
                const float angle = GizmoDragAngle(studioGizmoDrag_, gizmoMouse, &axis);
                if (angle != 0.0f) {
                    // world rotation q about `axis` after the bone's rotation: local' = local * (P q P^-1)
                    const XMVECTOR q = XMQuaternionRotationAxis(XMVector3Normalize(XMLoadFloat3(&axis)), angle);
                    const XMVECTOR dq = XMQuaternionMultiply(XMQuaternionMultiply(parent, q), XMQuaternionInverse(parent));
                    XMStoreFloat4(&v.r, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&studioDragBase_.r), dq)));
                }
            }
            if (!studioAutoIkActive_) {
                PoseLayer& pose = m->pose;
                if (pose.frame != d.Frame()) pose = PoseLayer{};
                pose.frame = d.Frame();
                pose.bones[studioDragBone_] = v;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                StudioSetPose(studioAutoIkActive_ ? Tr("자동 IK")
                                                  : studioGizmoMode_ == GizmoMode::Translate ? Tr("본 이동") : Tr("본 회전"),
                              studioPoseBefore_);
                studioViewDrag_ = 0;
                studioAutoIkActive_ = false;
            }
        }
    }
    // model placement drag: world translation / rotation about a world axis, live; one undo step on release
    if (studioViewDrag_ == 6) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // cancel
            mm->place = studioPlaceBefore_;
            studioPlaceDragging_ = false;
            studioViewDrag_ = 4;
        } else {
            PropAttach pl = studioPlaceBefore_;
            if (studioGizmoMode_ == GizmoMode::Translate) {
                const XMFLOAT3 w = GizmoDragTranslation(studioGizmoDrag_, mouse);
                pl.translation = {pl.translation.x + w.x, pl.translation.y + w.y, pl.translation.z + w.z};
            } else {
                XMFLOAT3 axis;
                const float angle = GizmoDragAngle(studioGizmoDrag_, mouse, &axis);
                if (angle != 0.0f) {
                    const XMFLOAT4 q0f = QuatFromEulerDeg(pl.rotationDeg);
                    const XMVECTOR dq = XMQuaternionRotationAxis(XMVector3Normalize(XMLoadFloat3(&axis)), angle);
                    XMFLOAT4 q;
                    XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&q0f), dq)));  // q0, then the world turn
                    pl.rotationDeg = EulerDeg(q);
                }
            }
            mm->place = pl;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                StudioCommitPlace(mm->uid, studioPlaceBefore_, mm->place);
                studioPlaceDragging_ = false;
                studioViewDrag_ = 0;
            }
        }
    }
    if (studioViewDrag_ >= 3) consumed = true;
    if ((studioViewDrag_ == 3 || studioViewDrag_ == 4) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) studioViewDrag_ = 0;

    // overlay
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    if (m && d.showBones)
        DrawBoneOverlay(dl, studioVp_, *m->pmx, *m->inst, d.selectedBones, d.activeBone, studioHoverBone_, os);
    if (studioGizmoShown_)
        DrawGizmo(dl, studioVp_, studioGizmoFrame_, studioGizmoMode_, gs, studioViewDrag_ == 2 ? GizmoPart::None : studioGizmoHot_,
                  studioViewDrag_ == 2 ? studioGizmoDrag_.part : GizmoPart::None);
    dl->PopClipRect();
    if (studioHoverBone_ >= 0 && studioViewDrag_ == 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const PmxBone& hb = m->pmx->bones[(size_t)studioHoverBone_];
        ui::Tooltip(DisplayName(hb.name, hb.nameEn));
    }
}

void App::StudioViewportToolbar(float x, float cy) {
    using namespace ui;
    StudioDoc& d = *studio_;
    if (!d.Selected()) return;  // characters and stages (the pose model or the model gizmo's target)
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b = 30.0f, gap = 2.0f;
    const float w = Dp(7 * b + 6 * gap + 3 * 9.0f + 8.0f);
    dl->AddRectFilled(ImVec2(x, cy - Dp(b * 0.5f + 4.0f)), ImVec2(x + w, cy + Dp(b * 0.5f + 4.0f)), WithAlpha(p.surface, 0.85f),
                      Dp(10.0f));
    ImGui::SetCursorScreenPos(ImVec2(x + Dp(4.0f), cy - Dp(b * 0.5f)));
    if (IconButton("##vpbones", icon::Bone, d.showBones ? Tr("본 숨기기") : Tr("본 표시"), d.showBones, b)) d.showBones = !d.showBones;
    ImGui::SameLine(0, Dp(9.0f));
    if (IconButton("##vprot", icon::Refresh, Tr("회전  (E)"), d.gizmoTool == 0, b)) d.gizmoTool = 0;
    ImGui::SameLine(0, Dp(gap));
    if (IconButton("##vpmove", icon::ArrowsOutCardinal, Tr("이동  (W)"), d.gizmoTool == 1, b)) d.gizmoTool = 1;
    ImGui::SameLine(0, Dp(gap));
    if (IconButton("##vpautoik", icon::Magnet, Tr("자동 IK: 손목을 끌면 팔이 따라옵니다  (K)"), d.autoIk, b)) d.autoIk = !d.autoIk;
    ImGui::SameLine(0, Dp(gap));
    if (IconButton("##vpmodel", icon::PersonSimple, d.modelGizmo ? Tr("모델 이동/회전 끄기  (T)") : Tr("모델 이동/회전  (T)"), d.modelGizmo, b))
        d.modelGizmo = !d.modelGizmo;
    ImGui::SameLine(0, Dp(9.0f));
    if (IconButton("##vplocal", d.gizmoLocal ? icon::Cube : icon::Globe,
                   d.gizmoLocal ? Tr("로컬 축 (클릭: 전역 축)  (L)") : Tr("전역 축 (클릭: 로컬 축)  (L)"), false, b))
        d.gizmoLocal = !d.gizmoLocal;
    ImGui::SameLine(0, Dp(gap));
    if (IconButton("##vpcenter", icon::Crosshair,
                   studioCenterOnBone_ ? Tr("카메라 본 중심 잠금 해제") : Tr("본 클릭 시 카메라 중심 이동"),
                   studioCenterOnBone_, b))
        studioCenterOnBone_ = !studioCenterOnBone_;
}

// Bottom-corner XYZ controllers for the active bone (the original MMD's axis sliders): move / rotate the bone by
// typing or dragging X/Y/Z directly, without touching the gizmo. Writes through the pose layer (like the gizmo), one
// undo step per field gesture.
void App::DrawStudioViewportAxisControls(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    (void)y0;
    if (!m || d.activeBone < 0 || d.activeBone >= (int)m->pmx->bones.size()) return;
    const int frame = d.Frame();
    const PmxBone& b = m->pmx->bones[(size_t)d.activeBone];
    const bool movable = (b.flags & PmxBone_Movable) != 0;
    const bool rotatable = (b.flags & PmxBone_Rotatable) != 0;
    if (!movable && !rotatable) return;
    const Palette& p = P();

    // Axis colours (match the gizmo: X red, Y green, Z blue).
    static const ImU32 kAxisCol[3] = {IM_COL32(235, 75, 75, 255), IM_COL32(110, 205, 70, 255), IM_COL32(70, 130, 245, 255)};
    static const char* kAxisName[3] = {"X", "Y", "Z"};

    const bool fixedAxis = (b.flags & PmxBone_FixedAxis) != 0;
    // parent world rotation (the frame the bone's local keys live in)
    XMFLOAT4 parentRotF;
    {
        const int parent = b.parentIndex;
        const XMVECTOR pq = parent >= 0 && parent < (int)m->pmx->bones.size()
                               ? RotationOf(m->inst->BoneWorld(parent)) : XMQuaternionIdentity();
        XMStoreFloat4(&parentRotF, pq);
    }
    // Effective tool mode (mirrors the gizmo): auto-IK / move tool => translate, else rotate.
    int ikRoot = -1, ikMid = -1;
    const bool autoIk = d.autoIk && FindAutoIkChain(*m->pmx, d.activeBone, ikRoot, ikMid);
    const bool rotateMode = !autoIk && (d.gizmoTool == 1 ? !movable : rotatable);
    const bool transActive = !rotateMode;

    char title[96];
    std::snprintf(title, sizeof(title), "%s · %s", DisplayName(b.name, b.nameEn), Tr("본 값 편집"));

    // Two rows (Move / Rotate) of three side-by-side slider boxes, laid out with explicit coordinates.
    const float pad = Dp(6.0f);
    const float panelW = Dp(252.0f);
    const float innerW = panelW - pad * 2.0f;
    const float rowLabelW = Dp(34.0f);
    const float cGap = Dp(6.0f);
    const float cellsAvail = innerW - rowLabelW - cGap;
    const float cW = (cellsAvail - cGap * 2.0f) / 3.0f;
    const float labelH = Dp(16.0f);
    const float fieldH = Dp(24.0f);
    const float rowH = labelH + fieldH + Dp(6.0f);
    const int rowsN = (movable ? 1 : 0) + (rotatable ? 1 : 0);
    const float panelH = pad * 2.0f + Dp(18.0f) + rowsN * rowH + (rowsN > 1 ? Dp(10.0f) : 0.0f) + Dp(6.0f);
    const ImVec2 pos(x0 + Dp(4.0f), y1 - panelH - Dp(4.0f));

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##vpaxis", nullptr, wf)) {
        const PoseBone cur = StudioCurrentPose(*m)[(size_t)d.activeBone];
        ImDrawList* dl = ImGui::GetWindowDrawList();
        TextEllipsis(dl, Font::Semibold, size::Caption, ImGui::GetCursorScreenPos(),
                     ImGui::GetCursorScreenPos().x + panelW - pad * 2, p.ink2, title);
        ImGui::Dummy(ImVec2(0, Dp(4.0f)));

        // Draw one row of three cells. Each cell: axis label above a slider box with < > chevrons.
        const auto drawRow = [&](const char* groupLabel, float vals[3], bool enabled, float speed, const char* fmt,
                                 const char* unit, int fixedAxisOnly) {
            const ImVec2 row0 = ImGui::GetCursorScreenPos();
            Text(dl, Font::Regular, size::Caption, ImVec2(row0.x, row0.y + labelH * 0.5f), p.ink3, groupLabel);
            bool rowCh = false, rowEnd = false;
            for (int ax = 0; ax < 3; ++ax) {
                if (fixedAxisOnly >= 0 && ax != fixedAxisOnly) continue;
                const float x = row0.x + rowLabelW + cGap + ax * (cW + cGap);
                const ImU32 axisCol = enabled ? kAxisCol[ax] : IM_COL32(90, 98, 106, 180);
                const std::string an = kAxisName[ax];
                const float lw = TextSize(Font::Semibold, size::Caption, an.c_str()).x;
                Text(dl, Font::Semibold, size::Caption, ImVec2(x + cW * 0.5f - lw * 0.5f, row0.y), axisCol, an.c_str());
                const float yf = row0.y + labelH;
                const float bx0 = x + Dp(9.0f), bx1 = x + cW - Dp(9.0f);
                const ImU32 chevron = enabled ? kAxisCol[ax] : IM_COL32(90, 98, 106, 140);
                dl->AddTriangleFilled(ImVec2(x + Dp(2.0f), yf + fieldH * 0.5f),
                                      ImVec2(x + Dp(8.0f), yf + fieldH * 0.5f - Dp(4.0f)),
                                      ImVec2(x + Dp(8.0f), yf + fieldH * 0.5f + Dp(4.0f)), chevron);
                ImGui::BeginDisabled(!enabled);
                ImGui::SetCursorScreenPos(ImVec2(bx0, yf));
                ImGui::SetNextItemWidth(std::max(bx1 - bx0, Dp(30.0f)));
                const bool ch = ImGui::DragFloat(("##ax" + std::to_string(ax) + unit).c_str(), &vals[ax], speed,
                                                 0.0f, 0.0f, fmt);
                const bool started = ImGui::IsItemActivated(), ended = ItemEditEnded();
                ImGui::EndDisabled();
                dl->AddTriangleFilled(ImVec2(bx1 + Dp(6.0f), yf + fieldH * 0.5f),
                                      ImVec2(bx1 + Dp(0.0f), yf + fieldH * 0.5f - Dp(4.0f)),
                                      ImVec2(bx1 + Dp(0.0f), yf + fieldH * 0.5f + Dp(4.0f)), chevron);
                if (started) StudioBeginPoseFieldEdit(*m);
                rowCh |= ch; rowEnd |= ended;
            }
            return std::pair<bool, bool>{rowCh, rowEnd};
        };
        // helper to apply the changed flag of the whole row
        PoseBone v = cur;
        bool anyChanged = false, anyEnded = false;

        if (movable) {
            float t[3] = {cur.t.x, cur.t.y, cur.t.z};
            const auto apply = [&]() {
                const auto r = drawRow(Tr("이동"), t, transActive, 0.02f, "%.2f", "t", -1);
                if (r.first) v.t = {t[0], t[1], t[2]};
                anyChanged |= r.first; anyEnded |= r.second;
            };
            apply();
            ImGui::Dummy(ImVec2(0, Dp(6.0f)));
        }

        if (rotatable) {
            if (fixedAxis) {
                // Fixed-axis: a single full-width angle control. The angle is an accumulated offset from the pose
                // captured at the gesture start, so dragging never re-applies to an already-changed rotation each frame.
                XMVECTOR axW = XMLoadFloat3(&b.fixedAxis);
                if (XMVectorGetX(XMVector3LengthSq(axW)) < 1e-12f) axW = XMVectorSet(1, 0, 0, 0);
                axW = XMVector3Normalize(XMVector3Rotate(XMVector3Normalize(axW), XMLoadFloat4(&parentRotF)));
                XMFLOAT3 axWf; XMStoreFloat3(&axWf, axW);
                const ImVec2 row0 = ImGui::GetCursorScreenPos();
                Text(dl, Font::Regular, size::Caption, ImVec2(row0.x, row0.y + labelH * 0.5f), p.ink3, Tr("회전"));
                const float yf = row0.y + labelH;
                const float bx0 = row0.x + rowLabelW + cGap;
                const float bx1 = row0.x + innerW - pad;
                const ImU32 axisCol = rotateMode ? kAxisCol[0] : IM_COL32(90, 98, 106, 180);
                Text(dl, Font::Semibold, size::Caption, ImVec2(bx0, row0.y), axisCol, "A");
                ImGui::BeginDisabled(!rotateMode);
                ImGui::SetCursorScreenPos(ImVec2(bx0, yf));
                ImGui::SetNextItemWidth(std::max(bx1 - bx0, Dp(40.0f)));
                const bool ch = ImGui::DragFloat("##vpfixeda", &studioFixedAxisDelta_, 0.5f, 0.0f, 0.0f, "%.1f°");
                const bool started = ImGui::IsItemActivated(), ended = ItemEditEnded();
                ImGui::EndDisabled();
                if (started && !studioFixedAxisActive_) {
                    studioFixedAxisBaseR_ = cur.r;
                    studioFixedAxisDelta_ = 0.0f;
                    studioFixedAxisActive_ = true;
                    StudioBeginPoseFieldEdit(*m);
                }
                if (ch) {
                    const XMVECTOR q = XMQuaternionRotationAxis(XMVector3Normalize(XMLoadFloat3(&axWf)),
                                                               XMConvertToRadians(studioFixedAxisDelta_));
                    const XMVECTOR parentQ = XMLoadFloat4(&parentRotF);
                    const XMVECTOR dq = XMQuaternionMultiply(XMQuaternionMultiply(parentQ, q), XMQuaternionInverse(parentQ));
                    XMStoreFloat4(&v.r, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&studioFixedAxisBaseR_), dq)));
                    anyChanged = true;
                }
                if (ended) studioFixedAxisActive_ = false;
                anyEnded |= ended;  // registered below, once this frame's value is in the pose layer
            } else {
                const XMFLOAT3 e0 = EulerDeg(cur.r);
                float r[3] = {e0.x, e0.y, e0.z};
                const auto apply = [&]() {
                    const auto rr = drawRow(Tr("회전"), r, rotateMode, 0.25f, "%.1f°", "r", -1);
                    if (rr.first) v.r = QuatFromEulerDeg({r[0], r[1], r[2]});
                    anyChanged |= rr.first; anyEnded |= rr.second;
                };
                apply();
            }
        }

        if (anyChanged) {
            // a change opens the edit when none is open: a field activated as another field's edit ends only joins it
            StudioBeginPoseFieldEdit(*m);
            PoseLayer& pose = m->pose;
            if (pose.frame != frame) pose = PoseLayer{};
            pose.frame = frame;
            pose.bones[d.activeBone] = v;
        }
        if (anyEnded && studioPoseFieldEdit_) {
            studioPoseFieldEdit_ = false;
            StudioSetPose(Tr("본 값 편집"), studioPoseBefore_);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
}

// Eye and target handles of the motion camera, drawn with the translate gizmo in the free view (the camera row is
// selected): dragging the eye swings the camera around its target, dragging the target turns it about its eye. Both go
// through StudioWriteCamera (auto-key rules, one undo step per drag).
void App::StudioViewportCameraHandles(bool hovered) {
    using namespace DirectX;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    // the handles move the SELECTED camera (an extra one too), in the free view (the camera view is the main camera)
    const std::shared_ptr<CameraMotion>& eval = d.SelectedCameraEval();
    const bool free = !(d.useMotionCamera && d.cameraEval);
    if (d.selectedModel >= 0 || d.selectedLightUid != 0 || d.possessCamera || !free || !eval ||
        d.SelectedCameraTrack().camera.empty() || d.playing) {
        if (studioViewDrag_ == 7) studioViewDrag_ = 0;
        studioCamHandle_ = 0;
        return;
    }
    const int frame = d.Frame();
    const CameraPose pose = eval->Evaluate((float)(d.time * kMmdFps));
    CameraPathPoint cur;
    CameraMotion::ToView(pose, &cur.view, &cur.eye);
    const XMFLOAT3 handles[2] = {cur.eye, pose.target};
    const GizmoStyle gs = MakeGizmoStyle();
    const ImVec2 mouse = io.MousePos;

    // pick a handle: a click near one of the dots, not on a gizmo part
    if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !(studioCamHandle_ && studioCamHot_ != GizmoPart::None)) {
        float best = ui::Dp(12.0f);
        int pick = 0;
        for (int i = 0; i < 2; ++i) {
            ImVec2 pt;
            if (!studioVp_.Project(handles[i], pt)) continue;
            const float dist = std::hypot(mouse.x - pt.x, mouse.y - pt.y);
            if (dist <= best) { best = dist; pick = i + 1; }
        }
        if (pick) {
            studioCamHandle_ = pick;
            studioViewDrag_ = 4;  // the click selects the handle; no orbit
        }
    }
    if (!studioCamHandle_) return;
    const XMFLOAT3 pos = handles[studioCamHandle_ - 1];
    if (studioViewDrag_ != 7) {
        studioCamFrame_ = GizmoFrame{};
        studioCamFrame_.center = pos;
        studioCamHot_ = hovered ? GizmoHitTest(studioVp_, studioCamFrame_, GizmoMode::Translate, gs, mouse) : GizmoPart::None;
    }
    if (studioViewDrag_ == 1 && hovered && studioCamHot_ != GizmoPart::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        studioCamDrag_ = BeginGizmoDrag(studioVp_, studioCamFrame_, GizmoMode::Translate, gs, studioCamHot_, mouse);
        if (studioCamDrag_.part != GizmoPart::None) {
            studioViewDrag_ = 7;
            studioCamKeyBase_ = SampleCamera(d.SelectedCameraTrack().camera, frame);  // S4: the selected camera's key
            studioCamBase_ = FreeFromKey(studioCamKeyBase_);
        }
    }
    if (studioViewDrag_ == 7) {
        const XMFLOAT3 w = GizmoDragTranslation(studioCamDrag_, mouse);
        FreeCamera c = studioCamBase_;
        const float sy = std::sin(c.yaw), cy = std::cos(c.yaw), sp = std::sin(c.pitch), cp = std::cos(c.pitch);
        XMFLOAT3 eye{c.target.x + sy * cp * c.distance, c.target.y + sp * c.distance, c.target.z - cy * cp * c.distance};
        XMFLOAT3 target = c.target;
        if (studioCamHandle_ == 1) { eye.x += w.x; eye.y += w.y; eye.z += w.z; }
        else { target.x += w.x; target.y += w.y; target.z += w.z; }
        const XMFLOAT3 dir{eye.x - target.x, eye.y - target.y, eye.z - target.z};
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len > 0.05f) {
            c.target = target;
            c.distance = len;
            c.pitch = std::clamp(std::asin(std::clamp(dir.y / len, -1.0f, 1.0f)), -1.45f, 1.45f);
            c.yaw = std::atan2(dir.x, -dir.z);
            StudioWriteCamera(c, &studioCamKeyBase_, false);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            StudioEndKeyEdit();
            studioViewDrag_ = 0;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawGizmo(dl, studioVp_, studioCamFrame_, GizmoMode::Translate, gs, studioCamHot_,
              studioViewDrag_ == 7 ? studioCamDrag_.part : GizmoPart::None);
}

void App::StudioViewportLightHandles(bool hovered) {
    using namespace DirectX;
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const GizmoStyle gs = MakeGizmoStyle();
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (d.playing) {
        if (studioViewDrag_ == 8) {
            StudioEndLightEdit(studioLightEditingUid_);
            studioViewDrag_ = 0;
        }
        studioLightGizmoShown_ = false;
        return;
    }

    const int frame = d.Frame();
    const LightAnchors anchors = StudioBuildLightAnchors();
    const XMFLOAT3 focus = anchors.focus;

    // Draw icons for all lights with viewportVisible
    uint32_t iconHoveredUid = 0;
    for (const SceneLight& l : d.lights) {
        if (!l.viewportVisible) continue;
        const LightValues cur = SampleLightValues(l, frame);
        XMFLOAT3 anchor = cur.position;
        if (l.kind == LightKind::Sun) {
            anchor = focus;
        } else if (l.kind == LightKind::Ambient || l.kind == LightKind::Sky) {
            continue;   // no position of its own (the S9b-1 sky light has none either)
        }

        ImVec2 screenPos;
        if (!studioVp_.Project(anchor, screenPos)) continue;

        const bool isSelected = (d.selectedModel < 0 && d.selectedLightUid == l.uid);
        const float dIcon = std::hypot(mouse.x - screenPos.x, mouse.y - screenPos.y);
        const bool iconHot = hovered && (dIcon <= Dp(12.0f)) && (studioViewDrag_ == 0 || studioViewDrag_ == 1);
        if (iconHot && !iconHoveredUid) iconHoveredUid = l.uid;

        const ImU32 bgCol = isSelected ? p.accentSoft : WithAlpha(p.surface, 0.90f);
        const ImU32 borderCol = (isSelected || iconHot) ? p.accentInk : p.line;
        const ImU32 glyphCol = (isSelected || iconHot) ? p.accentInk : p.ink2;
        const float iconR = Dp(10.0f);
        dl->AddCircleFilled(screenPos, iconR, bgCol);
        dl->AddCircle(screenPos, iconR, borderCol, 0, 1.5f);
        const char* glyph = icon::Lightbulb;
        if (l.kind == LightKind::Sun) glyph = icon::Sun;
        else if (l.kind == LightKind::Spot) glyph = icon::Aperture;
        else if (l.kind == LightKind::Area) glyph = icon::Image;
        else if (l.kind == LightKind::FogVolume) glyph = icon::Drop;
        ui::Icon(dl, glyph, 12.0f, screenPos, glyphCol);
    }

    if (iconHoveredUid != 0 && studioViewDrag_ == 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (const SceneLight* hl = d.FindLight(iconHoveredUid)) ui::Tooltip(LightDisplayName(hl->name).c_str());
    }

    // Clicking a light icon selects that light
    if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && iconHoveredUid != 0) {
        StudioSelectLight(iconHoveredUid);
        studioViewDrag_ = 4;
        return;
    }

    SceneLight* light = (d.selectedModel < 0 && d.selectedLightUid != 0) ? d.FindLight(d.selectedLightUid) : nullptr;
    if (!light || !light->viewportVisible || light->kind == LightKind::Ambient || light->kind == LightKind::Sky) {
        studioLightGizmoShown_ = false;
        if (studioViewDrag_ == 8) {
            StudioEndLightEdit(studioLightEditingUid_);
            studioViewDrag_ = 0;
        }
        return;
    }

    studioLightGizmoShown_ = true;
    LightValues cur = SampleLightValues(*light, frame);

    // Setup gizmo frames and handles for the selected light
    if (light->kind == LightKind::Point) {
        studioLightPosFrame_ = GizmoFrame{};
        studioLightPosFrame_.center = cur.position;

        // Camera-facing range circle
        const float R = std::max(0.1f, cur.range);
        const XMFLOAT3 camRight{studioVp_.view._11, studioVp_.view._21, studioVp_.view._31};
        const XMFLOAT3 camUp{studioVp_.view._12, studioVp_.view._22, studioVp_.view._32};
        const XMFLOAT3 rangeHandleWorld = {cur.position.x + camRight.x * R, cur.position.y + camRight.y * R, cur.position.z + camRight.z * R};
        studioVp_.Project(rangeHandleWorld, studioLightRangeHandlePos_);

        // Draw range circle
        constexpr int kSamples = 36;
        for (int k = 0; k < kSamples; ++k) {
            const float a0 = 2.0f * 3.14159265f * (float)k / kSamples;
            const float a1 = 2.0f * 3.14159265f * (float)(k + 1) / kSamples;
            const XMFLOAT3 p0{cur.position.x + (camRight.x * std::cos(a0) + camUp.x * std::sin(a0)) * R,
                              cur.position.y + (camRight.y * std::cos(a0) + camUp.y * std::sin(a0)) * R,
                              cur.position.z + (camRight.z * std::cos(a0) + camUp.z * std::sin(a0)) * R};
            const XMFLOAT3 p1{cur.position.x + (camRight.x * std::cos(a1) + camUp.x * std::sin(a1)) * R,
                              cur.position.y + (camRight.y * std::cos(a1) + camUp.y * std::sin(a1)) * R,
                              cur.position.z + (camRight.z * std::cos(a1) + camUp.z * std::sin(a1)) * R};
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, p0, p1, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 160), 3.0f);
                dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 180), 1.5f);
            }
        }

        // Hit testing
        if (studioViewDrag_ != 8) {
            const float dRange = std::hypot(mouse.x - studioLightRangeHandlePos_.x, mouse.y - studioLightRangeHandlePos_.y);
            studioLightRangeHot_ = hovered && (dRange <= Dp(8.0f));
            studioLightHotGizmo_ = (hovered && !studioLightRangeHot_) ? GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, mouse) : GizmoPart::None;
        }

        // Draw range handle dot
        const bool rangeActive = (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::RangeHandle);
        dl->AddCircleFilled(studioLightRangeHandlePos_, Dp(6.0f), IM_COL32(20, 20, 20, 220));
        dl->AddCircleFilled(studioLightRangeHandlePos_, Dp(4.5f), (studioLightRangeHot_ || rangeActive) ? gs.hotColor : IM_COL32(255, 215, 60, 255));

        // Draw position gizmo
        DrawGizmo(dl, studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs,
                  studioViewDrag_ == 8 ? GizmoPart::None : studioLightHotGizmo_,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::PosGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);

        // Press start
        if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (studioLightRangeHot_) {
                studioLightDragPart_ = LightDragPart::RangeHandle;
                studioLightValuesBase_ = cur;
                const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                StudioBeginLightEdit(light->uid, willKey);
                studioViewDrag_ = 8;
            } else if (studioLightHotGizmo_ != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, studioLightHotGizmo_, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::PosGizmo;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            }
        }
    } else if (light->kind == LightKind::Spot) {
        studioLightResolvedAim_ = ResolveSpotAim(*light, cur, d.time, anchors);
        const XMFLOAT3 P = cur.position;
        const XMFLOAT3 A = studioLightResolvedAim_;

        XMVECTOR axisV = XMVectorSubtract(XMLoadFloat3(&A), XMLoadFloat3(&P));
        float aimDist = XMVectorGetX(XMVector3Length(axisV));
        if (aimDist < 1e-4f) {
            axisV = XMVectorSet(0, -1, 0, 0);
            aimDist = 10.0f;
        } else {
            axisV = XMVectorScale(axisV, 1.0f / aimDist);
        }
        XMFLOAT3 axis;
        XMStoreFloat3(&axis, axisV);

        studioLightPosFrame_ = GizmoFrame{};
        studioLightPosFrame_.center = P;
        studioLightAimFrame_ = GizmoFrame{};
        studioLightAimFrame_.center = A;

        // Orthonormal basis for cone rim
        XMVECTOR upV = std::fabs(axis.y) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
        XMVECTOR uV = XMVector3Normalize(XMVector3Cross(axisV, upV));
        XMVECTOR vV = XMVector3Normalize(XMVector3Cross(axisV, uV));
        const float rimR = aimDist * std::tan(cur.coneOuter);

        // Cone handle on rim
        XMFLOAT3 coneHandleWorld;
        XMStoreFloat3(&coneHandleWorld, XMVectorAdd(XMLoadFloat3(&A), XMVectorScale(uV, rimR)));
        studioVp_.Project(coneHandleWorld, studioLightConeHandlePos_);

        // Range handle along spot axis
        XMFLOAT3 rangeHandleWorld;
        XMStoreFloat3(&rangeHandleWorld, XMVectorAdd(XMLoadFloat3(&P), XMVectorScale(axisV, cur.range)));
        studioVp_.Project(rangeHandleWorld, studioLightRangeHandlePos_);

        // Visualisation: spot cone outline
        constexpr int kRimSamples = 32;
        for (int k = 0; k < kRimSamples; ++k) {
            const float a0 = 2.0f * 3.14159265f * (float)k / kRimSamples;
            const float a1 = 2.0f * 3.14159265f * (float)(k + 1) / kRimSamples;
            XMFLOAT3 p0, p1;
            XMStoreFloat3(&p0, XMVectorAdd(XMLoadFloat3(&A), XMVectorAdd(XMVectorScale(uV, rimR * std::cos(a0)), XMVectorScale(vV, rimR * std::sin(a0)))));
            XMStoreFloat3(&p1, XMVectorAdd(XMLoadFloat3(&A), XMVectorAdd(XMVectorScale(uV, rimR * std::cos(a1)), XMVectorScale(vV, rimR * std::sin(a1)))));
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, p0, p1, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 160), 3.0f);
                dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 200), 1.5f);
            }
        }
        // 4 cone generators from position to rim
        for (int k = 0; k < 4; ++k) {
            const float a = k * 3.14159265f * 0.5f;
            XMFLOAT3 rimPt;
            XMStoreFloat3(&rimPt, XMVectorAdd(XMLoadFloat3(&A), XMVectorAdd(XMVectorScale(uV, rimR * std::cos(a)), XMVectorScale(vV, rimR * std::sin(a)))));
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, P, rimPt, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 160), 3.0f);
                dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 160), 1.5f);
            }
        }

        // Line along spot axis to range
        {
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, P, rangeHandleWorld, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 140), 2.5f);
                dl->AddLine(sa, sb, IM_COL32(255, 255, 255, 140), 1.0f);
            }
        }

        // If Manual aim mode: line from P to A
        if (light->aimMode == AimMode::Manual) {
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, P, A, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 180), 3.0f);
                dl->AddLine(sa, sb, IM_COL32(100, 200, 255, 220), 1.5f);
            }
        }

        // Hit testing
        GizmoPart hotAim = GizmoPart::None;
        if (studioViewDrag_ != 8) {
            const float dCone = std::hypot(mouse.x - studioLightConeHandlePos_.x, mouse.y - studioLightConeHandlePos_.y);
            const float dRange = std::hypot(mouse.x - studioLightRangeHandlePos_.x, mouse.y - studioLightRangeHandlePos_.y);
            studioLightConeHot_ = hovered && (dCone <= Dp(8.0f));
            studioLightRangeHot_ = hovered && !studioLightConeHot_ && (dRange <= Dp(8.0f));
            hotAim = (hovered && !studioLightConeHot_ && !studioLightRangeHot_ && light->aimMode == AimMode::Manual)
                         ? GizmoHitTest(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, mouse)
                         : GizmoPart::None;
            studioLightHotGizmo_ = (hovered && !studioLightConeHot_ && !studioLightRangeHot_ && hotAim == GizmoPart::None)
                                       ? GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, mouse)
                                       : GizmoPart::None;
        }

        // Draw handles
        const bool coneActive = (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::ConeHandle);
        dl->AddCircleFilled(studioLightConeHandlePos_, Dp(6.0f), IM_COL32(20, 20, 20, 220));
        dl->AddCircleFilled(studioLightConeHandlePos_, Dp(4.5f), (studioLightConeHot_ || coneActive) ? gs.hotColor : IM_COL32(255, 215, 60, 255));

        const bool rangeActive = (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::RangeHandle);
        dl->AddCircleFilled(studioLightRangeHandlePos_, Dp(6.0f), IM_COL32(20, 20, 20, 220));
        dl->AddCircleFilled(studioLightRangeHandlePos_, Dp(4.5f), (studioLightRangeHot_ || rangeActive) ? gs.hotColor : IM_COL32(255, 255, 255, 255));

        // Draw position gizmo
        DrawGizmo(dl, studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs,
                  studioViewDrag_ == 8 ? GizmoPart::None : studioLightHotGizmo_,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::PosGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);

        // Draw aim gizmo if Manual
        if (light->aimMode == AimMode::Manual) {
            DrawGizmo(dl, studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs,
                      studioViewDrag_ == 8 ? GizmoPart::None : hotAim,
                      (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::AimGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);
        }

        // Press start
        if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (studioLightConeHot_) {
                studioLightDragPart_ = LightDragPart::ConeHandle;
                studioLightValuesBase_ = cur;
                const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                StudioBeginLightEdit(light->uid, willKey);
                studioViewDrag_ = 8;
            } else if (studioLightRangeHot_) {
                studioLightDragPart_ = LightDragPart::RangeHandle;
                studioLightValuesBase_ = cur;
                const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                StudioBeginLightEdit(light->uid, willKey);
                studioViewDrag_ = 8;
            } else if (hotAim != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, hotAim, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::AimGizmo;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            } else if (studioLightHotGizmo_ != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, studioLightHotGizmo_, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::PosGizmo;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            }
        }
    } else if (light->kind == LightKind::Area) {
        const XMFLOAT3 P = cur.position;
        const XMFLOAT3 A = cur.aim;
        studioLightResolvedAim_ = A;

        XMVECTOR axisV = XMVectorSubtract(XMLoadFloat3(&A), XMLoadFloat3(&P));
        float aimDist = XMVectorGetX(XMVector3Length(axisV));
        if (aimDist < 1e-4f) {
            axisV = XMVectorSet(0, -1, 0, 0);
            aimDist = 10.0f;
        } else {
            axisV = XMVectorScale(axisV, 1.0f / aimDist);
        }

        studioLightPosFrame_ = GizmoFrame{};
        studioLightPosFrame_.center = P;
        studioLightAimFrame_ = GizmoFrame{};
        studioLightAimFrame_.center = A;

        const XMVECTOR normalV = axisV;
        const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        const XMVECTOR crossV = XMVector3Cross(worldUp, normalV);
        const float crossLenSq = XMVectorGetX(XMVector3LengthSq(crossV));
        const XMVECTOR rightV = crossLenSq > 1e-6f ? XMVector3Normalize(crossV) : XMVectorSet(1, 0, 0, 0);
        const XMVECTOR upV = XMVector3Cross(normalV, rightV);

        const float hw = cur.size.x * 0.5f;
        const float hh = cur.size.y * 0.5f;
        const XMVECTOR c0V = XMVectorSubtract(XMVectorSubtract(XMLoadFloat3(&P), XMVectorScale(rightV, hw)), XMVectorScale(upV, hh));
        const XMVECTOR c1V = XMVectorSubtract(XMVectorAdd(XMLoadFloat3(&P), XMVectorScale(rightV, hw)), XMVectorScale(upV, hh));
        const XMVECTOR c2V = XMVectorAdd(XMVectorAdd(XMLoadFloat3(&P), XMVectorScale(rightV, hw)), XMVectorScale(upV, hh));
        const XMVECTOR c3V = XMVectorAdd(XMVectorSubtract(XMLoadFloat3(&P), XMVectorScale(rightV, hw)), XMVectorScale(upV, hh));

        XMFLOAT3 c0, c1, c2, c3;
        XMStoreFloat3(&c0, c0V);
        XMStoreFloat3(&c1, c1V);
        XMStoreFloat3(&c2, c2V);
        XMStoreFloat3(&c3, c3V);

        auto drawSeg = [&](const XMFLOAT3& p0, const XMFLOAT3& p1) {
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, p0, p1, sa, sb)) {
                dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 160), 3.0f);
                dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 200), 1.5f);
            }
        };
        drawSeg(c0, c1);
        drawSeg(c1, c2);
        drawSeg(c2, c3);
        drawSeg(c3, c0);

        XMFLOAT3 arrowTip;
        XMStoreFloat3(&arrowTip, XMVectorAdd(XMLoadFloat3(&P), XMVectorScale(normalV, 10.0f)));
        ImVec2 sa, sb;
        if (ProjectSegment(studioVp_, P, arrowTip, sa, sb)) {
            dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 200), 3.0f);
            dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 255), 1.5f);
            const ImVec2 sd(sb.x - sa.x, sb.y - sa.y);
            const float len = std::hypot(sd.x, sd.y);
            if (len > 4.0f) {
                const ImVec2 u(sd.x / len, sd.y / len);
                const ImVec2 n(-u.y, u.x);
                const ImVec2 tip(sb.x + u.x * 6.0f, sb.y + u.y * 6.0f);
                const ImVec2 left(sb.x - u.x * 4.0f + n.x * 4.0f, sb.y - u.y * 4.0f + n.y * 4.0f);
                const ImVec2 right(sb.x - u.x * 4.0f - n.x * 4.0f, sb.y - u.y * 4.0f - n.y * 4.0f);
                dl->AddTriangleFilled(tip, left, right, IM_COL32(255, 215, 60, 255));
                dl->AddTriangle(tip, left, right, IM_COL32(20, 20, 20, 200), 1.0f);
            }
        }

        if (ProjectSegment(studioVp_, P, A, sa, sb)) {
            dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 180), 3.0f);
            dl->AddLine(sa, sb, IM_COL32(100, 200, 255, 220), 1.5f);
        }

        GizmoPart hotAim = GizmoPart::None;
        if (studioViewDrag_ != 8) {
            hotAim = hovered ? GizmoHitTest(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, mouse) : GizmoPart::None;
            studioLightHotGizmo_ = (hovered && hotAim == GizmoPart::None)
                                       ? GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, mouse)
                                       : GizmoPart::None;
        }

        DrawGizmo(dl, studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs,
                  studioViewDrag_ == 8 ? GizmoPart::None : studioLightHotGizmo_,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::PosGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);
        DrawGizmo(dl, studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs,
                  studioViewDrag_ == 8 ? GizmoPart::None : hotAim,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::AimGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);

        if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hotAim != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, hotAim, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::AimGizmo;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            } else if (studioLightHotGizmo_ != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, studioLightHotGizmo_, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::PosGizmo;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            }
        }
    } else if (light->kind == LightKind::FogVolume) {
        // S9: the local medium's wireframe: a box (12 edges) or an ellipsoid (three great circles), plus the position
        // gizmo. Drawn thin and cyan-dim so it reads as a volume, not a solid.
        const XMFLOAT3 c = cur.position;
        const XMFLOAT3 e{std::max(0.5f, cur.extent.x), std::max(0.5f, cur.extent.y), std::max(0.5f, cur.extent.z)};
        studioLightPosFrame_ = GizmoFrame{};
        studioLightPosFrame_.center = c;
        const ImU32 wire = IM_COL32(120, 210, 255, 170);
        const ImU32 wireDark = IM_COL32(20, 20, 20, 140);
        auto line = [&](const XMFLOAT3& p0, const XMFLOAT3& p1) {
            ImVec2 sa, sb;
            if (ProjectSegment(studioVp_, p0, p1, sa, sb)) {
                dl->AddLine(sa, sb, wireDark, 2.5f);
                dl->AddLine(sa, sb, wire, 1.2f);
            }
        };
        if (light->fogShape == FogShape::Sphere) {
            constexpr int kSamples = 40;
            for (int axis = 0; axis < 3; ++axis) {
                for (int k = 0; k < kSamples; ++k) {
                    const float a0 = 2.0f * 3.14159265f * (float)k / kSamples;
                    const float a1 = 2.0f * 3.14159265f * (float)(k + 1) / kSamples;
                    auto at = [&](float a) {
                        XMFLOAT3 p = c;
                        if (axis == 0) { p.y += e.y * std::sin(a); p.z += e.z * std::cos(a); }
                        else if (axis == 1) { p.x += e.x * std::sin(a); p.z += e.z * std::cos(a); }
                        else { p.x += e.x * std::sin(a); p.y += e.y * std::cos(a); }
                        return p;
                    };
                    line(at(a0), at(a1));
                }
            }
        } else {
            XMFLOAT3 corner[8];
            for (int i = 0; i < 8; ++i) {
                corner[i] = {c.x + ((i & 1) ? e.x : -e.x), c.y + ((i & 2) ? e.y : -e.y), c.z + ((i & 4) ? e.z : -e.z)};
            }
            static const int kEdges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
            for (const auto& ed : kEdges) line(corner[ed[0]], corner[ed[1]]);
        }

        if (studioViewDrag_ != 8)
            studioLightHotGizmo_ = hovered ? GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, mouse)
                                           : GizmoPart::None;
        DrawGizmo(dl, studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs,
                  studioViewDrag_ == 8 ? GizmoPart::None : studioLightHotGizmo_,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::PosGizmo) ? studioLightGizmoDrag_.part : GizmoPart::None);
        if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            studioLightHotGizmo_ != GizmoPart::None) {
            studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, studioLightHotGizmo_, mouse);
            if (studioLightGizmoDrag_.part != GizmoPart::None) {
                studioLightDragPart_ = LightDragPart::PosGizmo;
                studioLightValuesBase_ = cur;
                const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                StudioBeginLightEdit(light->uid, willKey);
                studioViewDrag_ = 8;
            }
        }
    } else if (light->kind == LightKind::Sun) {
        studioLightSunFrame_ = GizmoFrame{};
        studioLightSunFrame_.center = focus;
        const bool readOnly = (light->vmdLink && !d.camera.light.empty());

        // Direction arrow
        XMVECTOR dirV = XMLoadFloat3(&cur.direction);
        if (XMVectorGetX(XMVector3LengthSq(dirV)) < 1e-4f) dirV = XMVectorSet(0, -1, 0, 0);
        else dirV = XMVector3Normalize(dirV);
        XMFLOAT3 arrowTip;
        XMStoreFloat3(&arrowTip, XMVectorAdd(XMLoadFloat3(&focus), XMVectorScale(dirV, 35.0f)));

        ImVec2 sa, sb;
        if (ProjectSegment(studioVp_, focus, arrowTip, sa, sb)) {
            dl->AddLine(sa, sb, IM_COL32(20, 20, 20, 200), 4.5f);
            dl->AddLine(sa, sb, IM_COL32(255, 215, 60, 255), 2.5f);
            const ImVec2 sd(sb.x - sa.x, sb.y - sa.y);
            const float len = std::hypot(sd.x, sd.y);
            if (len > 4.0f) {
                const ImVec2 u(sd.x / len, sd.y / len);
                const ImVec2 n(-u.y, u.x);
                const ImVec2 tip(sb.x + u.x * 10.0f, sb.y + u.y * 10.0f);
                const ImVec2 left(sb.x - u.x * 6.0f + n.x * 6.0f, sb.y - u.y * 6.0f + n.y * 6.0f);
                const ImVec2 right(sb.x - u.x * 6.0f - n.x * 6.0f, sb.y - u.y * 6.0f - n.y * 6.0f);
                dl->AddTriangleFilled(tip, left, right, IM_COL32(255, 215, 60, 255));
                dl->AddTriangle(tip, left, right, IM_COL32(20, 20, 20, 200), 1.5f);
            }
        }

        // Rotate gizmo
        if (studioViewDrag_ != 8) {
            studioLightHotGizmo_ = (!readOnly && hovered) ? GizmoHitTest(studioVp_, studioLightSunFrame_, GizmoMode::Rotate, gs, mouse) : GizmoPart::None;
        }

        GizmoStyle sunGs = gs;
        if (readOnly) {
            sunGs.axisColor[0] = WithAlpha(sunGs.axisColor[0], 0.35f);
            sunGs.axisColor[1] = WithAlpha(sunGs.axisColor[1], 0.35f);
            sunGs.axisColor[2] = WithAlpha(sunGs.axisColor[2], 0.35f);
        }
        DrawGizmo(dl, studioVp_, studioLightSunFrame_, GizmoMode::Rotate, sunGs,
                  studioViewDrag_ == 8 ? GizmoPart::None : studioLightHotGizmo_,
                  (studioViewDrag_ == 8 && studioLightDragPart_ == LightDragPart::SunRotate) ? studioLightGizmoDrag_.part : GizmoPart::None);

        if (!readOnly && studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (studioLightHotGizmo_ != GizmoPart::None) {
                studioLightGizmoDrag_ = BeginGizmoDrag(studioVp_, studioLightSunFrame_, GizmoMode::Rotate, gs, studioLightHotGizmo_, mouse);
                if (studioLightGizmoDrag_.part != GizmoPart::None) {
                    studioLightDragPart_ = LightDragPart::SunRotate;
                    studioLightValuesBase_ = cur;
                    const bool willKey = (FindKey(light->keys, frame) != nullptr) || d.autoKey;
                    StudioBeginLightEdit(light->uid, willKey);
                    studioViewDrag_ = 8;
                }
            }
        }
    }

    // Active Drag (studioViewDrag_ == 8)
    if (studioViewDrag_ == 8) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // Cancel drag
            if (studioLightIsKeyEdit_) {
                for (const TrackState& s : studioLightKeyBefore_) RestoreTrack(d, s);
            } else {
                d.lights = studioLightBaseBefore_;
            }
            studioLightKeyBefore_.clear();
            studioLightBaseBefore_.clear();
            studioLightEdit_ = false;
            studioLightChanged_ = false;
            studioLightEditingUid_ = 0;
            studioViewDrag_ = 4;
            return;
        }

        LightValues val = studioLightValuesBase_;
        if (studioLightDragPart_ == LightDragPart::PosGizmo) {
            const XMFLOAT3 w = GizmoDragTranslation(studioLightGizmoDrag_, mouse);
            val.position.x += w.x;
            val.position.y += w.y;
            val.position.z += w.z;
        } else if (studioLightDragPart_ == LightDragPart::AimGizmo) {
            const XMFLOAT3 w = GizmoDragTranslation(studioLightGizmoDrag_, mouse);
            val.aim.x += w.x;
            val.aim.y += w.y;
            val.aim.z += w.z;
        } else if (studioLightDragPart_ == LightDragPart::SunRotate) {
            XMFLOAT3 axis;
            const float angle = GizmoDragAngle(studioLightGizmoDrag_, mouse, &axis);
            if (angle != 0.0f) {
                const XMVECTOR dirV = XMLoadFloat3(&studioLightValuesBase_.direction);
                const XMVECTOR axisV = XMVector3Normalize(XMLoadFloat3(&axis));
                const XMVECTOR q = XMQuaternionRotationAxis(axisV, angle);
                const XMVECTOR newDir = XMVector3Normalize(XMVector3Rotate(dirV, q));
                XMStoreFloat3(&val.direction, newDir);
            }
        } else if (studioLightDragPart_ == LightDragPart::ConeHandle) {
            float newAngle = 0.0f;
            if (SolveConeAngle(studioVp_, val.position, studioLightResolvedAim_, mouse, newAngle)) {
                val.coneOuter = newAngle;
                if (val.coneInner > val.coneOuter) val.coneInner = val.coneOuter;
            }
        } else if (studioLightDragPart_ == LightDragPart::RangeHandle) {
            if (light->kind == LightKind::Point) {
                float newRange = 0.0f;
                if (SolveRangeDistance(studioVp_, val.position, mouse, newRange)) {
                    val.range = newRange;
                }
            } else if (light->kind == LightKind::Spot) {
                float newRange = 0.0f;
                XMVECTOR axisV = XMVectorSubtract(XMLoadFloat3(&studioLightResolvedAim_), XMLoadFloat3(&val.position));
                if (XMVectorGetX(XMVector3LengthSq(axisV)) > 1e-4f) {
                    axisV = XMVector3Normalize(axisV);
                    XMFLOAT3 axis;
                    XMStoreFloat3(&axis, axisV);
                    if (SolveSpotRange(studioVp_, val.position, axis, mouse, newRange)) {
                        val.range = newRange;
                    }
                }
            }
        }

        // Apply updated values
        if (studioLightIsKeyEdit_) {
            LightKey* curKey = FindKey(light->keys, frame);
            if (curKey) curKey->v = val;
            else UpsertKey(light->keys, LightKey{frame, val});
        } else {
            light->v = val;
        }
        studioLightChanged_ = true;
        d.rowsKey = ~0ull;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            StudioEndLightEdit(light->uid);
            studioViewDrag_ = 0;
        }
    }
}


// Bones and the gizmo of the current selection in a view that has no interaction this frame (quad view).
void App::StudioDrawPoseOverlay(const ViewProj& vp, float x0, float y0, float x1, float y1) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m || !m->visible || d.playing) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    if (m && d.showBones) {
        BoneOverlayStyle os2 = MakeOverlayStyle(settings_.boneOverlayScale);
        os2.showPhysicsBones = studio_->showPhysicsBones;
        DrawBoneOverlay(dl, vp, *m->pmx, *m->inst, d.selectedBones, d.activeBone, -1, os2);
    }
    if (studioGizmoShown_) DrawGizmo(dl, vp, studioGizmoFrame_, studioGizmoMode_, MakeGizmoStyle(), GizmoPart::None, GizmoPart::None);
    dl->PopClipRect();
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

bool App::StudioScriptLightGizmoPoint(const std::string& part, ImVec2& out) const {
    if (!studio_) return false;
    const StudioDoc& d = *studio_;
    const SceneLight* light = d.FindLight(d.selectedLightUid);
    if (!light || !light->viewportVisible) return false;

    const GizmoStyle gs = MakeGizmoStyle();
    const std::string p = ToLowerAscii(part);

    if (p == "cone" || p == "coneouter") {
        if (light->kind != LightKind::Spot) return false;
        out = studioLightConeHandlePos_;
        return true;
    }
    if (p == "range") {
        if (light->kind != LightKind::Point && light->kind != LightKind::Spot) return false;
        out = studioLightRangeHandlePos_;
        return true;
    }

    // Aim gizmo parts: "aim", "aim_x", "aim_y", "aim_z", "aim_yz", "aim_zx", "aim_xy"
    if (p.rfind("aim", 0) == 0) {
        if (light->kind != LightKind::Spot || light->aimMode != AimMode::Manual) return false;
        std::string sub = p.size() > 4 ? p.substr(4) : (p == "aim" ? "x" : p.substr(3));
        if (sub.empty() || sub == "_") sub = "x";
        static const char* const kParts[] = {"", "x", "y", "z", "yz", "zx", "xy"};
        int partIdx = 1;
        for (int i = 1; i <= 6; ++i) if (sub == kParts[i]) partIdx = i;

        ImVec2 c;
        if (!studioVp_.Project(studioLightAimFrame_.center, c)) return false;
        float best = -1.0f;
        const float r = gs.ringRadius + gs.arrowLength;
        for (float y = -r; y <= r; y += 2.0f) {
            for (float x = -r; x <= r; x += 2.0f) {
                const ImVec2 q(c.x + x, c.y + y);
                if ((int)GizmoHitTest(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, q) != partIdx) continue;
                const float dist = x * x + y * y;
                if (dist > best) { best = dist; out = q; }
            }
        }
        if (best >= 0.0f) {
            const ImVec2 in(c.x + (out.x - c.x) * 0.92f, c.y + (out.y - c.y) * 0.92f);
            if ((int)GizmoHitTest(studioVp_, studioLightAimFrame_, GizmoMode::Translate, gs, in) == partIdx) out = in;
            return true;
        }
        return false;
    }

    // Sun rotate rings: "rx", "ry", "rz"
    if (p == "rx" || p == "ry" || p == "rz") {
        if (light->kind != LightKind::Sun) return false;
        int ringIdx = (p == "rx") ? 7 : (p == "ry") ? 8 : 9;
        ImVec2 c;
        if (!studioVp_.Project(studioLightSunFrame_.center, c)) return false;
        float best = -1.0f;
        const float r = gs.ringRadius + 20.0f;
        for (float y = -r; y <= r; y += 2.0f) {
            for (float x = -r; x <= r; x += 2.0f) {
                const ImVec2 q(c.x + x, c.y + y);
                if ((int)GizmoHitTest(studioVp_, studioLightSunFrame_, GizmoMode::Rotate, gs, q) != ringIdx) continue;
                const float dist = x * x + y * y;
                if (dist > best) { best = dist; out = q; }
            }
        }
        if (best >= 0.0f) return true;
        return false;
    }

    // Position gizmo parts: "x", "y", "z", "yz", "zx", "xy"
    static const char* const kParts[] = {"", "x", "y", "z", "yz", "zx", "xy"};
    int partIdx = 0;
    for (int i = 1; i <= 6; ++i) if (p == kParts[i]) partIdx = i;
    if (partIdx > 0) {
        ImVec2 c;
        if (!studioVp_.Project(studioLightPosFrame_.center, c)) return false;
        float best = -1.0f;
        const float r = gs.ringRadius + gs.arrowLength;
        for (float y = -r; y <= r; y += 2.0f) {
            for (float x = -r; x <= r; x += 2.0f) {
                const ImVec2 q(c.x + x, c.y + y);
                if ((int)GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, q) != partIdx) continue;
                const float dist = x * x + y * y;
                if (dist > best) { best = dist; out = q; }
            }
        }
        if (best >= 0.0f) {
            const ImVec2 in(c.x + (out.x - c.x) * 0.92f, c.y + (out.y - c.y) * 0.92f);
            if ((int)GizmoHitTest(studioVp_, studioLightPosFrame_, GizmoMode::Translate, gs, in) == partIdx) out = in;
            return true;
        }
    }
    return false;
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

    // --- search: narrows the timeline rows to the matching bones / morphs (Enter picks the first matching bone)
    if (SearchField("##bonefilter", d.boneFilter, sizeof(d.boneFilter), Tr("본 검색 (타임라인 필터)"), w / Dpi())) d.rowsKey = ~0ull;
    if (d.boneFilter[0] && ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        for (size_t i = 0; i < pmx.bones.size(); ++i)
            if (pmx.bones[i].name.find(d.boneFilter) != std::string::npos || pmx.bones[i].nameEn.find(d.boneFilter) != std::string::npos) {
                StudioSelectBone((int)i, false);
                break;
            }
    }
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));

    // --- the active bone
    const int bone =d.activeBone >= 0 && d.activeBone < (int)pmx.bones.size() ? d.activeBone : -1;
    if (bone < 0) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, Tr("선택한 본 없음"));
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
        wrapped(Tr("뷰포트에서 본을 클릭하거나 타임라인의 본 행을 선택하세요. 기즈모를 드래그하면 포즈가 바뀌고, I로 키를 등록합니다."));
    } else {
        const PmxBone& b = pmx.bones[(size_t)bone];
        {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            TextEllipsis(cdl, Font::Semibold, size::Body, c, c.x + w, p.ink, DisplayName(b.name, b.nameEn));
            ImGui::Dummy(ImVec2(w, Dp(26.0f)));
        }
        {
            const auto it = m->motion.bones.find(b.name);
            const bool animated = it != m->motion.bones.end() && !it->second.empty() &&
                                  !IsStaticBoneTrack(m->motion, b.name);
            const ImVec2 lc = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Caption, ImVec2(lc.x, lc.y + Dp(4.0f)), animated ? p.ink2 : p.ink3,
                 animated ? Tr("애니메이션 (키 사용)") : Tr("장면 전체에 같은 값"));
            ImGui::Dummy(ImVec2(1.0f, Dp(22.0f)));
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
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
            const bool started = ImGui::IsItemActivated(), ended = ItemEditEnded();
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
            if (started) StudioBeginPoseFieldEdit(*m);
            return std::pair<bool, bool>{changed, ended};
        };
        float t[3] = {cur.t.x, cur.t.y, cur.t.z};
        const XMFLOAT3 e = EulerDeg(cur.r);
        float r[3] = {e.x, e.y, e.z};
        const auto [tChanged, tEnded] = field("##bonet", Tr("위치"), t, b.flags & PmxBone_Movable, 0.02f, "%.2f");
        const auto [rChanged, rEnded] = field("##boner", Tr("회전"), r, b.flags & PmxBone_Rotatable, 0.25f, "%.1f°");
        // a change opens the field edit when none is open: a field activated as another field's edit ends only joins it
        if (tChanged || rChanged) StudioBeginPoseFieldEdit(*m);
        // stopwatch rule: an animated bone (or auto-key) ends the edit as a key at the playhead; a bone that is not
        // animated edits its constant (a single frame-0 key flagged static) directly, without the pose layer. A bone
        // already edited in the pose layer (gizmo / VPD) keeps that path.
        const auto bit = m->motion.bones.find(b.name);
        const bool boneAnimated = bit != m->motion.bones.end() && !bit->second.empty() && !IsStaticBoneTrack(m->motion, b.name);
        if ((tChanged || rChanged) && !edited && !boneAnimated && !d.autoKey && !studioKeyEdit_) {
            studioKeyBefore_ = {CaptureTrack(d, d.selectedModel, RowKind::Bone, b.name)};
            studioKeyEdit_ = true;
            studioKeyChanged_ = false;
        }
        if ((tChanged || rChanged) && studioKeyEdit_) {
            std::vector<BoneKf>& track = m->motion.bones[b.name];
            BoneKf k;
            if (!track.empty() && track.front().frame == 0) k = track.front();  // keep its interpolation
            else FillLinearInterp(k.interp);
            k.frame = 0;
            k.t = cur.t;
            k.r = cur.r;
            if (tChanged) k.t = {t[0], t[1], t[2]};
            if (rChanged) k.r = QuatFromEulerDeg({r[0], r[1], r[2]});
            track = {k};
            m->motion.staticBones.insert(b.name);
            ++m->motionVersion;
            studioKeyChanged_ = true;
            d.rowsKey = ~0ull;
        } else if (tChanged || rChanged) {
            PoseLayer& pose = m->pose;
            if (pose.frame != frame) pose = PoseLayer{};
            pose.frame = frame;
            PoseBone v = cur;
            if (tChanged) v.t = {t[0], t[1], t[2]};
            if (rChanged) v.r = QuatFromEulerDeg({r[0], r[1], r[2]});
            pose.bones[bone] = v;
        }
        if ((tEnded || rEnded) && studioPoseFieldEdit_) {
            studioPoseFieldEdit_ = false;
            if (studioKeyEdit_) StudioEndKeyEdit();
            else if (boneAnimated && !d.autoKey && !m->pose.Empty() && m->pose.frame == frame)
                StudioRegisterPose(false, &studioPoseBefore_, Tr("본 값 편집"));
            else StudioSetPose(Tr("본 값 편집"), studioPoseBefore_);
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

    // --- IK on/off: the state at the playhead (VMD IK keys: enabled before the first key); a toggle keys it here
    {
        std::vector<int> ikBones;
        for (size_t i = 0; i < pmx.bones.size(); ++i)
            if (pmx.bones[i].flags & PmxBone_IK) ikBones.push_back((int)i);
        if (!ikBones.empty()) {
            ImGui::Dummy(ImVec2(w, Dp(14.0f)));
            caption(Tr("IK 켜기 / 끄기"));
            for (int ib : ikBones) {
                const std::string& nm = pmx.bones[(size_t)ib].name;
                const auto it = m->motion.ik.find(nm);
                bool on = true;
                if (it != m->motion.ik.end())
                    for (const IkKf& k : it->second) {
                        if (k.frame > frame) break;
                        on = k.enabled;
                    }
                bool v = on;
                char id[32];
                std::snprintf(id, sizeof(id), "##ikon%d", ib);
                Switch(id, nm.c_str(), &v);
                if (v != on) {
                    std::vector<IkKf> before = it != m->motion.ik.end() ? it->second : std::vector<IkKf>{};
                    std::vector<IkKf> after = before;
                    UpsertKey(after, IkKf{frame, v});
                    d.history.Push(std::make_unique<IkEditCommand>(d, d.selectedModel, v ? Tr("IK 켜기") : Tr("IK 끄기"), nm,
                                                                   std::move(before), std::move(after)));
                    break;  // the motion changed under the iterators
                }
            }
        }
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
    bool changedMorphs = false;

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
                const std::string& mname = pmx.morphs[(size_t)i].name;
                const auto mt = m->motion.morphs.find(mname);
                const bool animated = mt != m->motion.morphs.end() && !mt->second.empty() &&
                                      !IsStaticMorphTrack(m->motion, mname);
                const bool staticTrack = IsStaticMorphTrack(m->motion, mname);
                float v = edited ? ov->second : m->inst->MorphWeight(i);
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const ImGuiID id = ImGui::GetID("##slider");
                const ImRect bb(c, ImVec2(c.x + w, c.y + rowH));
                const ImRect hit(ImVec2(c.x + Dp(28.0f), c.y), bb.Max);  // the stopwatch / key diamond own the left edge
                ImGui::ItemSize(bb);
                const ImVec2 rowEnd = ImGui::GetCursorScreenPos();
                if (ImGui::ItemAdd(hit, id)) {
                    bool hovered = false, held = false;
                    ImGui::ButtonBehavior(hit, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
                    const float labelW = w * 0.42f, valueW = Dp(40.0f);
                    const float sx0 = c.x + labelW + Dp(6.0f) + Dp(16.0f), sx1 = c.x + w - valueW - Dp(4.0f);
                    // A morph edited in the pose layer (VPD import) keeps that path; otherwise the slider writes the
                    // motion directly (stopwatch rule): animated or auto-key = the key at the playhead, else the
                    // constant (a single frame-0 key flagged static). One undo step per drag.
                    if (ImGui::IsItemActivated()) {
                        StudioSelectMorph(i);
                        if (edited) {
                            StudioBeginPoseFieldEdit(*m);
                        } else if (!studioKeyEdit_) {
                            studioKeyBefore_ = {CaptureTrack(d, d.selectedModel, RowKind::Morph, mname)};
                            studioKeyEdit_ = true;
                            studioKeyChanged_ = false;
                        }
                    }
                    if (held) {
                        const float nv = std::clamp((ImGui::GetIO().MousePos.x - sx0) / std::max(sx1 - sx0, 1.0f), 0.0f, 1.0f);
                        const float snapped = std::round(nv * 100.0f) / 100.0f;
                        if (snapped != v) {
                            if (edited) {
                                m->pose.morphs[i] = snapped;
                            } else if (studioKeyEdit_) {
                                std::vector<MorphKf>& track = m->motion.morphs[mname];
                                if (animated || d.autoKey) {
                                    UpsertKey(track, MorphKf{frame, snapped});
                                    if (staticTrack) m->motion.staticMorphs.erase(mname);
                                } else {
                                    track = {MorphKf{0, snapped}};
                                    m->motion.staticMorphs.insert(mname);
                                }
                                ++m->motionVersion;
                                studioKeyChanged_ = true;
                                d.rowsKey = ~0ull;
                            }
                            v = snapped;
                        }
                    }
                    if (ImGui::IsItemDeactivated()) {
                        if (studioPoseFieldEdit_) {
                            StudioSetPose(Tr("모프 편집"), studioPoseBefore_);
                            studioPoseFieldEdit_ = false;
                        }
                        StudioEndKeyEdit();
                    }
                    if (hovered || held) dl->AddRectFilled(c, bb.Max, WithAlpha(p.ink, 0.04f), Dp(6.0f));
                    ImGui::SetCursorScreenPos(rowEnd);
                    TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x + Dp(4.0f), c.y + Dp(5.0f)), c.x + labelW + Dp(16.0f),
                                 edited ? p.warn : p.ink, DisplayName(pmx.morphs[(size_t)i].name, pmx.morphs[(size_t)i].nameEn));
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
                    if (hovered) Tooltip(DisplayName(pmx.morphs[(size_t)i].name, pmx.morphs[(size_t)i].nameEn));
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    }
    if (pmx.morphs.empty() || (slots[0].empty() && slots[1].empty() && slots[2].empty() && slots[3].empty())) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, c, p.ink3,
             pmx.morphs.empty() ? Tr("이 모델에는 모프가 없어요") : Tr("검색 결과가 없습니다"));
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
    }
    ImGui::Dummy(ImVec2(w, Dp(12.0f)));
    if (changedMorphs) {
        m->BuildRowGroups();
        d.rowsKey = ~0ull;
    }
}

} // namespace mmdx
