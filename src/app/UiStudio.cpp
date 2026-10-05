// Studio screen: a keyframe editor for a multi-model scene (outliner, viewport, inspector, timeline).
// Document model and undo live in src/studio; this file wires them to the App (loading, renderer, audio, input).
#include "app/App.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <cmath>
#include <map>

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
#include "studio/UiBezier.h"

namespace mmdx {

using namespace studio;

namespace {
constexpr float kTopBarH = 56.0f, kOutlinerW = 236.0f, kInspectorW = 312.0f, kBottomH = 300.0f, kTransportH = 46.0f;
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
const char* const kHeadBone = "\xE9\xA0\xAD";                                        // 頭
const char* const kCameraModelName = "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x83\xBB\xE7\x85\xA7\xE6\x98\x8E";  // カメラ・照明

std::string FormatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    const int total = (int)(seconds * 100.0 + 0.5);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d.%02d", total / 6000, (total / 100) % 60, total % 100);
    return buf;
}

// Logs editing operations that take long enough to be felt (large selections).
struct OpTimer {
    const char* name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~OpTimer() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 15.0) LOG_INFO("studio: %s took %.1f ms", name, ms);
    }
};

// Free orbit camera <-> MMD camera pose (rotation = (-pitch, yaw, 0), distance = -orbit distance).
void PoseFromFree(const DirectX::XMFLOAT3& target, float yaw, float pitch, float distance, float fov, CameraKf& k) {
    k.target = target;
    k.rotation = {-pitch, yaw, 0.0f};
    k.distance = -distance;
    k.fovDeg = (uint32_t)std::lround(fov);
    k.perspective = true;
}
} // namespace

// ---------------------------------------------------------------------------
// Loading / leaving
// ---------------------------------------------------------------------------

void App::StartStudioLoad(const CharacterAsset* ch, const StageAsset* st, const SongAsset* song) {
    if (!ch) return;
    StopVideoProbe(false);
    UnloadScene();
    screen_ = Screen::Loading;
    loadTarget_ = LoadTarget::Studio;
    loadError_.clear();
    loadProgress_.fraction.store(0.0f, std::memory_order_relaxed);
    loadProgress_.SetStatus("");
    studioPackage_ = std::make_unique<StudioPackage>();
    const CharacterAsset c = *ch;
    const bool hasStage = st != nullptr, hasSong = song != nullptr;
    const StageAsset s = hasStage ? *st : StageAsset{};
    const SongAsset so = hasSong ? *song : SongAsset{};
    loadFuture_ = std::async(std::launch::async, [=, pkg = studioPackage_.get(), this] {
        return LoadStudioPackage(c, hasStage ? &s : nullptr, hasSong ? &so : nullptr, *pkg, &loadProgress_, &loadError_);
    });
}

bool App::FinishStudioLoad() {
    auto doc = std::make_unique<StudioDoc>();
    UploadBatch batch(ctx_);
    for (StudioPackageModel& m : studioPackage_->models) {
        auto sm = std::make_unique<StudioModel>();
        sm->name = m.name;
        sm->libraryId = m.libraryId;
        sm->isStage = m.isStage;
        sm->path = m.pmx->sourcePath;
        sm->pmx = m.pmx;
        sm->inst = std::make_unique<ModelInstance>(m.pmx);
        sm->gpu = renderer_.CreateModel(batch, *m.pmx, m.textures, m.isStage ? ModelRole::Stage : ModelRole::Character);
        if (!sm->gpu) {
            if (m.isStage) {
                LOG_WARN("studio: stage part GPU upload failed: %s", m.name.c_str());
                continue;
            }
            return false;
        }
        sm->motion = std::move(m.motion);
        sm->inst->UpdatePose();
        doc->models.push_back(std::move(sm));
    }
    batch.Submit();

    doc->camera = std::move(studioPackage_->camera);
    doc->audioPath = studioPackage_->audioPath;
    doc->hasAudio = !doc->audioPath.empty() && audio_.Load(doc->audioPath);
    if (doc->hasAudio) {
        doc->audioEndFrame = (float)(audio_.DurationSeconds() * kMmdFps);
        audio_.SetMuted(false);
    }
    doc->useMotionCamera = !doc->camera.camera.empty() && !options_.freeCamera;
    doc->physics = settings_.physics && !options_.noPhysics;
    doc->selectedModel = (int)doc->models.size() - 1;  // the character
    doc->time = std::max(0.0, options_.seekSeconds);
    options_.seekSeconds = 0;
    freeCam_ = FreeCamera{};
    if (options_.hasCamera) {
        const float* c = options_.camera;
        freeCam_.target = {c[0], c[1], c[2]};
        freeCam_.yaw = DirectX::XMConvertToRadians(c[3]);
        freeCam_.pitch = DirectX::XMConvertToRadians(c[4]);
        freeCam_.distance = c[5];
        doc->useMotionCamera = false;
    }
    studio_ = std::move(doc);
    studioLastBind_ = 0;
    lastRenderedTime_ = -1;
    framesInScene_ = 0;
    screen_ = Screen::Studio;
    return true;
}

void App::LeaveStudio() {
    ctx_.WaitForGpu();
    studio_.reset();
    audio_.Unload();
    RenderSettings rs = renderer_.Settings();
    rs.viewportX = rs.viewportY = rs.viewportW = rs.viewportH = 0;
    renderer_.SetSettings(rs);
    studioLeaveConfirm_ = false;
    screen_ = Screen::Select;
}

// ---------------------------------------------------------------------------
// Playback and evaluation
// ---------------------------------------------------------------------------

void App::StudioSeek(double seconds) {
    StudioDoc& d = *studio_;
    d.time = std::clamp(seconds, 0.0, d.EndFrame() / (double)kMmdFps);
    if (d.hasAudio) audio_.Seek(d.time);
}

void App::StudioSetPlaying(bool play) {
    StudioDoc& d = *studio_;
    if (play) {
        // with a frame range, playback runs inside it; otherwise from the start once the end is reached
        if (d.HasRange()) {
            if (d.Frame() < d.view.rangeStart || d.Frame() >= d.view.rangeEnd) d.time = d.view.rangeStart / (double)kMmdFps;
        } else if (d.Frame() >= d.EndFrame()) {
            d.time = 0;
        }
    }
    d.playing = play;
    if (d.hasAudio) {
        if (play) {
            audio_.Seek(d.time);
            audio_.Play();
        } else {
            audio_.Pause();
        }
    }
}

void App::UpdateStudio(double dt) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    // The studio is one big ImGui window, so WantCaptureKeyboard is always set: only text input blocks shortcuts.
    if (!io.WantTextInput && !studioLeaveConfirm_) {
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
        const auto pressed = [](ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); };
        if (pressed(ImGuiKey_Space, false)) StudioSetPlaying(!d.playing);
        const bool redo = ctrl && (pressed(ImGuiKey_Y) || (shift && pressed(ImGuiKey_Z)));
        // the selection refers to key frames that an undo/redo may have moved: drop it
        if (redo && d.history.CanRedo()) { d.history.Redo(); d.selection.clear(); d.rowsKey = ~0ull; }
        else if (!redo && ctrl && pressed(ImGuiKey_Z) && d.history.CanUndo()) {
            d.history.Undo();
            d.selection.clear();
            d.rowsKey = ~0ull;
        }
        if (pressed(ImGuiKey_LeftArrow)) {
            if (ctrl) StudioJumpKey(-1);
            else { StudioSetPlaying(false); StudioSeek((d.Frame() - 1) / (double)kMmdFps); }
        }
        if (pressed(ImGuiKey_RightArrow)) {
            if (ctrl) StudioJumpKey(1);
            else { StudioSetPlaying(false); StudioSeek((d.Frame() + 1) / (double)kMmdFps); }
        }
        if (pressed(ImGuiKey_Home, false)) StudioSeek(0.0);
        if (pressed(ImGuiKey_End, false)) StudioSeek(d.EndFrame() / (double)kMmdFps);
        if (pressed(ImGuiKey_Delete, false) || pressed(ImGuiKey_Backspace, false)) StudioDeleteSelected(Tr("키 삭제"));
        if (ctrl && pressed(ImGuiKey_C, false)) StudioCopySelected();
        if (ctrl && pressed(ImGuiKey_X, false)) { StudioCopySelected(); StudioDeleteSelected(Tr("키 잘라내기")); }
        if (ctrl && pressed(ImGuiKey_V, false)) StudioPaste(shift);
        if (ctrl && pressed(ImGuiKey_A, false)) StudioSelectAll();
        if (!ctrl && pressed(ImGuiKey_I, false)) StudioRegisterKeys();
    }
    if (d.playing) {
        if (d.hasAudio && audio_.IsPlaying()) {
            const double a = audio_.PositionSeconds();
            const double predicted = d.time + dt;
            d.time = std::fabs(predicted - a) > 0.05 ? a : predicted;
        } else {
            d.time += dt;
        }
        // the range plays through its last frame; without a range the timeline ends at EndFrame
        const int start = d.HasRange() ? d.view.rangeStart : 0;
        const int end = d.HasRange() ? d.view.rangeEnd + 1 : d.EndFrame();
        if (d.time * kMmdFps >= end) {
            if (d.loop && end > start) {
                StudioSeek(start / (double)kMmdFps);  // also moves the audio; the backwards jump resets physics
            } else {
                d.time = (d.HasRange() ? d.view.rangeEnd : d.EndFrame()) / (double)kMmdFps;
                StudioSetPlaying(false);
            }
        }
    }
}

void App::UpdateStudioScene() {
    StudioDoc& d = *studio_;
    const uint64_t slot = ctx_.FrameNumber();
    const float frame = (float)(d.time * kMmdFps);

    // Physics runs while playing and when stepping one frame forward; any other jump (seek, scrub, loop) resets it
    // like play mode does. Paused on the same frame: no simulation (dt 0).
    float physicsDt = 0.0f;
    bool resetPhysics = d.physicsFrame < 0.0f;
    if (!resetPhysics) {
        const float df = frame - d.physicsFrame;
        if (d.playing) resetPhysics = df < 0.0f || df > 0.25f * kMmdFps;
        else if (std::fabs(df) > 1e-3f) resetPhysics = df < 0.0f || df > 1.0f + 1e-3f;
        if (!resetPhysics) physicsDt = std::max(0.0f, df) / kMmdFps;
    }
    d.physicsFrame = frame;

    for (auto& mp : d.models) {
        StudioModel& m = *mp;
        // Re-bind after edits (throttled while a curve is being dragged: binding a long dance takes a while).
        if (m.boundVersion != m.motionVersion && (!d.curveEditing || timeSeconds_ - studioLastBind_ > 0.15)) {
            OpTimer timer{"bind motion"};
            if (m.motion.bones.empty() && m.motion.morphs.empty() && m.motion.ik.empty()) {
                m.bound.reset();
            } else {
                const VmdMotion vmd = m.motion.ToVmd();
                m.bound = BoundMotion::Bind(*m.pmx, {&vmd});
            }
            m.boundVersion = m.motionVersion;
            studioLastBind_ = timeSeconds_;
        }
        ModelInstance& inst = *m.inst;
        if (m.bound) m.bound->Evaluate(frame, inst);
        else inst.ResetPose();
        if (!m.isStage) {
            inst.SetScale(m.libraryId.empty() ? 1.0f : settings_.CharacterScale(m.libraryId));
            inst.EnablePhysics(d.physics);
            if (resetPhysics) inst.ResetPhysics();
        }
        if (!m.isStage || m.bound) inst.UpdatePose(physicsDt);
        m.gpu->UpdateSkinning(slot, inst.SkinMatrices());
        m.gpu->UpdateMorphs(slot, inst.VertexMorphDeltas(), inst.MorphVersion());
    }

    if (d.cameraEvalVersion != d.cameraVersion) {
        d.cameraEval = d.camera.camera.empty() ? nullptr : CameraMotion::Create(d.camera.ToVmd());
        d.cameraEvalVersion = d.cameraVersion;
        if (!d.cameraEval) d.useMotionCamera = false;
    }
}

void App::BuildStudioFrameView(FrameView& view) {
    StudioDoc& d = *studio_;
    const float frame = (float)(d.time * kMmdFps);
    if (d.useMotionCamera && d.cameraEval) {
        const CameraPose pose = d.cameraEval->Evaluate(frame);
        CameraMotion::ToView(pose, &view.camera.view, &view.camera.eye);
        view.camera.fovYRadians = DirectX::XMConvertToRadians(pose.fovDeg);
    } else {
        const FreeCamera& cam = freeCam_;
        const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
        const DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
        const DirectX::XMVECTOR eye =
            DirectX::XMVectorAdd(target, DirectX::XMVectorScale(DirectX::XMVectorSet(sy * cp, sp, -cy * cp, 0), cam.distance));
        DirectX::XMStoreFloat4x4(&view.camera.view, DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0, 1, 0, 0)));
        DirectX::XMStoreFloat3(&view.camera.eye, eye);
        view.camera.fovYRadians = DirectX::XMConvertToRadians(cam.fovDeg);
    }

    bool anyStage = false;
    const StudioModel* performer = nullptr;
    for (const auto& m : d.models) {
        if (!m->visible) continue;
        view.models.push_back(m->gpu.get());
        if (m->isStage) anyStage = true;
        else if (!performer) performer = m.get();
    }
    view.studioFloor = !anyStage;

    DirectX::XMFLOAT3 focus{0, 10, 0};
    if (performer) {
        const int center = performer->pmx->FindBone(kCenterBone);
        if (center >= 0) focus = performer->inst->BoneWorldPosition(center);
    }
    BuildLighting((LightingPreset)settings_.lighting, d.time, focus, view.light);
    if (performer) {
        const int head = performer->pmx->FindBone(kHeadBone);
        DirectX::XMFLOAT3 target = focus;
        if (head >= 0) target = performer->inst->BoneWorldPosition(head);
        else target.y += 8.0f;
        const float z = DirectX::XMVectorGetZ(DirectX::XMVector3TransformCoord(
            DirectX::XMLoadFloat3(&target), DirectX::XMLoadFloat4x4(&view.camera.view)));
        view.focusDistance = z > view.camera.nearZ ? z : 0.0f;
    }
    view.cameraCut = lastRenderedTime_ < 0 || std::fabs(d.time - lastRenderedTime_) > 0.25;
    lastRenderedTime_ = d.time;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool App::StudioTrackOfRow(uint64_t row, RowKind& kind, std::string& name) const {
    const StudioDoc& d = *studio_;
    kind = RowKindOf(row);
    name.clear();
    if (kind == RowKind::Camera) return d.selectedModel < 0;
    if (d.selectedModel < 0 || d.selectedModel >= (int)d.models.size()) return false;
    const PmxModel& pmx = *d.models[d.selectedModel]->pmx;
    const uint32_t idx = RowIndexOf(row);
    if (kind == RowKind::Bone && idx < pmx.bones.size()) { name = pmx.bones[idx].name; return true; }
    if (kind == RowKind::Morph && idx < pmx.morphs.size()) { name = pmx.morphs[idx].name; return true; }
    return false;
}

std::vector<TrackState> App::StudioCaptureSelectedTracks() {
    StudioDoc& d = *studio_;
    std::vector<TrackState> out;
    std::set<std::pair<int, std::string>> seen;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(k.first, kind, name)) continue;
        if (!seen.insert({(int)kind, name}).second) continue;
        out.push_back(CaptureTrack(d, d.selectedModel, kind, name));
    }
    return out;
}

void App::StudioPushTrackEdit(const char* name, const std::vector<TrackState>& before) {
    StudioDoc& d = *studio_;
    std::vector<TrackState> after;
    after.reserve(before.size());
    for (const TrackState& s : before) after.push_back(CaptureTrack(d, s.model, s.kind, s.name));
    d.history.Push(std::make_unique<TrackEditCommand>(d, name, before, std::move(after)));
}

void App::StudioInsertKeys(const std::vector<uint64_t>& rows, int frame) {
    OpTimer timer{"StudioInsertKeys"};
    StudioDoc& d = *studio_;
    std::vector<TrackState> before;
    std::set<KeyId> inserted;
    std::set<std::pair<int, std::string>> seen;  // a bone listed in two display frames has two rows
    for (uint64_t row : rows) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(row, kind, name)) continue;
        if (seen.insert({(int)kind, name}).second) before.push_back(CaptureTrack(d, d.selectedModel, kind, name));
    }
    if (before.empty()) return;
    seen.clear();
    for (uint64_t row : rows) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(row, kind, name)) continue;
        inserted.insert({row, frame});
        if (!seen.insert({(int)kind, name}).second) continue;
        if (kind == RowKind::Camera) {
            CameraKf k;
            if (!d.useMotionCamera || d.camera.camera.empty()) {
                // keys the view the user is looking at
                FillLinearCameraInterp(k.interp);
                PoseFromFree(freeCam_.target, freeCam_.yaw, freeCam_.pitch, freeCam_.distance, freeCam_.fovDeg, k);
                k.frame = frame;
            } else {
                k = SampleCamera(d.camera.camera, frame);
            }
            UpsertKey(d.camera.camera, k);
        } else if (kind == RowKind::Bone) {
            auto& track = d.models[d.selectedModel]->motion.bones[name];
            BoneKf k;
            if (track.empty()) {
                FillLinearInterp(k.interp);
                k.frame = frame;
            } else {
                k = SampleBone(track, frame);
            }
            UpsertKey(track, k);
        } else {
            auto& track = d.models[d.selectedModel]->motion.morphs[name];
            UpsertKey(track, MorphKf{frame, track.empty() ? 0.0f : SampleMorph(track, frame)});
        }
    }
    StudioPushTrackEdit(Tr("키 추가"), before);
    d.selection = std::move(inserted);
    d.rowsKey = ~0ull;
}

std::vector<int> App::StudioRowFrames(uint64_t row) const {
    std::vector<int> out;
    RowKind kind;
    std::string name;
    if (!StudioTrackOfRow(row, kind, name)) return out;
    const StudioDoc& d = *studio_;
    const auto add = [&](const auto& keys) {
        out.reserve(keys.size());
        for (const auto& k : keys) out.push_back(k.frame);
    };
    if (kind == RowKind::Camera) {
        add(d.camera.camera);
    } else {
        const MotionData& m = d.models[d.selectedModel]->motion;
        if (kind == RowKind::Bone) {
            if (auto it = m.bones.find(name); it != m.bones.end()) add(it->second);
        } else if (auto it = m.morphs.find(name); it != m.morphs.end()) {
            add(it->second);
        }
    }
    return out;
}

std::vector<uint64_t> App::StudioExpandRows(const std::set<uint64_t>& rows) const {
    const StudioDoc& d = *studio_;
    std::vector<uint64_t> out;
    std::set<uint64_t> seen;
    for (uint64_t r : rows) {
        if (RowKindOf(r) == RowKind::Group) {
            if (auto it = d.groupChildren.find(r); it != d.groupChildren.end())
                for (uint64_t c : it->second)
                    if (seen.insert(c).second) out.push_back(c);
        } else if (seen.insert(r).second) {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<uint64_t> App::StudioTargetRows() const {
    const StudioDoc& d = *studio_;
    std::set<uint64_t> rows = d.selectedRows;
    for (const KeyId& k : d.selection) rows.insert(k.first);
    return StudioExpandRows(rows);
}

void App::StudioSelectAll() {
    OpTimer timer{"StudioSelectAll"};
    StudioDoc& d = *studio_;
    d.selection.clear();
    d.selectedRows.clear();
    std::set<uint64_t> all;
    if (d.selectedModel < 0) all.insert(MakeRowId(RowKind::Camera, 0, 0));
    for (const auto& [group, children] : d.groupChildren) all.insert(group);
    for (uint64_t row : StudioExpandRows(all))
        for (int f : StudioRowFrames(row)) d.selection.emplace_hint(d.selection.end(), row, f);
    d.rowsKey = ~0ull;
}

void App::StudioDeleteSelected(const char* undoName) {
    OpTimer timer{"StudioDeleteSelected"};
    StudioDoc& d = *studio_;
    if (d.selection.empty()) return;
    const std::vector<TrackState> before = StudioCaptureSelectedTracks();
    std::map<std::pair<int, std::string>, std::set<int>> frames;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (StudioTrackOfRow(k.first, kind, name)) frames[{(int)kind, name}].insert(k.second);
    }
    for (const auto& [track, fs] : frames) {
        const RowKind kind = (RowKind)track.first;
        if (kind == RowKind::Camera) EraseKeyFrames(d.camera.camera, fs);
        else if (kind == RowKind::Bone) EraseKeyFrames(d.models[d.selectedModel]->motion.bones[track.second], fs);
        else EraseKeyFrames(d.models[d.selectedModel]->motion.morphs[track.second], fs);
    }
    StudioPushTrackEdit(undoName, before);
    d.selection.clear();
    d.rowsKey = ~0ull;
}

void App::StudioCopySelected() {
    OpTimer timer{"StudioCopySelected"};
    StudioDoc& d = *studio_;
    if (d.selection.empty()) return;
    d.clipboard.clear();
    const int base = std::min_element(d.selection.begin(), d.selection.end(),
                                      [](const KeyId& a, const KeyId& b) { return a.second < b.second; })->second;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(k.first, kind, name)) continue;
        ClipboardKey c;
        c.row = k.first;
        c.offset = k.second - base;
        bool found = false;
        if (kind == RowKind::Camera) {
            if (const CameraKf* p = FindKey(d.camera.camera, k.second)) { c.camera = *p; found = true; }
        } else if (kind == RowKind::Bone) {
            const auto& t = d.models[d.selectedModel]->motion.bones;
            if (auto it = t.find(name); it != t.end())
                if (const BoneKf* p = FindKey(it->second, k.second)) { c.bone = *p; found = true; }
        } else {
            const auto& t = d.models[d.selectedModel]->motion.morphs;
            if (auto it = t.find(name); it != t.end())
                if (const MorphKf* p = FindKey(it->second, k.second)) { c.morph = *p; found = true; }
        }
        if (found) d.clipboard.push_back(c);
    }
    d.clipboardModel = d.clipboard.empty() ? -2 : d.selectedModel;
}

namespace {
// Copies the interpolation curves of `src` onto `dst`, keeping dst's MMD physics flags (bytes 2/3).
void CopyBoneCurves(const uint8_t src[64], uint8_t dst[64]) {
    uint8_t c[4];
    for (int ch = 0; ch < 4; ++ch) {
        GetBoneCurve(src, ch, c);
        SetBoneCurve(dst, ch, c);
    }
}
} // namespace

void App::StudioPaste(bool curvesOnly) {
    OpTimer timer{"StudioPaste"};
    StudioDoc& d = *studio_;
    if (d.clipboard.empty() || d.clipboardModel != d.selectedModel) return;
    const int at = d.Frame();
    std::vector<TrackState> before;
    std::set<std::pair<int, std::string>> seen;
    for (const ClipboardKey& c : d.clipboard) {
        RowKind kind;
        std::string name;
        if (StudioTrackOfRow(c.row, kind, name) && seen.insert({(int)kind, name}).second)
            before.push_back(CaptureTrack(d, d.selectedModel, kind, name));
    }
    std::set<KeyId> pasted;
    for (const ClipboardKey& c : d.clipboard) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(c.row, kind, name)) continue;
        const int f = at + c.offset;
        if (curvesOnly) {
            // interpolation only, onto keys that already exist at the target frames (morph keys have none)
            if (kind == RowKind::Camera) {
                CameraKf* k = FindKey(d.camera.camera, f);
                if (!k) continue;
                std::memcpy(k->interp, c.camera.interp, sizeof(k->interp));
            } else if (kind == RowKind::Bone) {
                auto& t = d.models[d.selectedModel]->motion.bones;
                auto it = t.find(name);
                BoneKf* k = it != t.end() ? FindKey(it->second, f) : nullptr;
                if (!k) continue;
                CopyBoneCurves(c.bone.interp, k->interp);
            } else {
                continue;
            }
        } else if (kind == RowKind::Camera) { CameraKf k = c.camera; k.frame = f; UpsertKey(d.camera.camera, k); }
        else if (kind == RowKind::Bone) { BoneKf k = c.bone; k.frame = f; UpsertKey(d.models[d.selectedModel]->motion.bones[name], k); }
        else { MorphKf k = c.morph; k.frame = f; UpsertKey(d.models[d.selectedModel]->motion.morphs[name], k); }
        pasted.insert({c.row, f});
    }
    if (pasted.empty()) return;  // curve paste with no key at any target frame: nothing changed
    StudioPushTrackEdit(curvesOnly ? Tr("곡선만 붙여넣기") : Tr("키 붙여넣기"), before);
    d.selection = std::move(pasted);
    d.rowsKey = ~0ull;
}

void App::StudioRegisterKeys() {
    StudioDoc& d = *studio_;
    const std::vector<uint64_t> rows = StudioTargetRows();
    if (rows.empty()) {
        toast_ = {Tr("행이나 키를 먼저 선택하세요"), Tr("행 이름을 클릭하면 그 행이 선택돼요."), {}, false, timeSeconds_ + 3.0};
        return;
    }
    StudioInsertKeys(rows, d.Frame());
}

void App::StudioShiftFrames(bool remove) {
    OpTimer timer{"StudioShiftFrames"};
    StudioDoc& d = *studio_;
    const int at = d.HasRange() ? d.view.rangeStart : d.Frame();
    const int count = d.HasRange() ? d.view.rangeEnd - d.view.rangeStart + 1 : 1;
    const char* undoName = remove ? Tr("프레임 삭제") : Tr("프레임 삽입");
    const std::vector<uint64_t> rows = StudioTargetRows();
    if (rows.empty()) {
        // every track of the shown target, IK/light/shadow included: swap the whole motion
        if (d.selectedModel >= (int)d.models.size()) return;
        MotionData& current = d.selectedModel < 0 ? d.camera : d.models[d.selectedModel]->motion;
        MotionData after = current;
        if (!(remove ? after.DeleteFrames(at, count) : after.InsertFrames(at, count))) return;
        d.history.Push(std::make_unique<MotionSwapCommand>(d, d.selectedModel, undoName, current, std::move(after)));
    } else {
        std::vector<TrackState> before;
        std::set<std::pair<int, std::string>> seen;
        bool changed = false;
        for (uint64_t row : rows) {
            RowKind kind;
            std::string name;
            if (!StudioTrackOfRow(row, kind, name) || !seen.insert({(int)kind, name}).second) continue;
            before.push_back(CaptureTrack(d, d.selectedModel, kind, name));
            const auto apply = [&](auto& keys) {
                changed |= remove ? DeleteFrameSpan(keys, at, count) : InsertFrameSpan(keys, at, count);
            };
            if (kind == RowKind::Camera) {
                apply(d.camera.camera);
            } else {
                MotionData& m = d.models[d.selectedModel]->motion;
                if (kind == RowKind::Bone) {
                    if (auto it = m.bones.find(name); it != m.bones.end()) apply(it->second);
                } else if (auto it = m.morphs.find(name); it != m.morphs.end()) {
                    apply(it->second);
                }
            }
        }
        if (!changed) return;
        StudioPushTrackEdit(undoName, before);  // Push re-applies the after state: emptied tracks are dropped
    }
    d.selection.clear();
    d.rowsKey = ~0ull;
}

void App::StudioCopyCurve() {
    StudioDoc& d = *studio_;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(k.first, kind, name)) continue;
        if (kind == RowKind::Camera) {
            if (const CameraKf* c = FindKey(d.camera.camera, k.second)) {
                std::memcpy(d.curveClip, c->interp, sizeof(c->interp));
                d.curveClipKind = kind;
            }
        } else if (kind == RowKind::Bone) {
            const auto& t = d.models[d.selectedModel]->motion.bones;
            if (auto it = t.find(name); it != t.end())
                if (const BoneKf* b = FindKey(it->second, k.second)) {
                    std::memcpy(d.curveClip, b->interp, sizeof(b->interp));
                    d.curveClipKind = kind;
                }
        }
        return;  // the first selected key decides (as in the inspector)
    }
}

void App::StudioPasteCurve() {
    StudioDoc& d = *studio_;
    if (d.curveClipKind != RowKind::Bone && d.curveClipKind != RowKind::Camera) return;
    std::vector<TrackState> before;
    std::set<std::pair<int, std::string>> seen;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (StudioTrackOfRow(k.first, kind, name) && kind == d.curveClipKind && seen.insert({(int)kind, name}).second)
            before.push_back(CaptureTrack(d, d.selectedModel, kind, name));
    }
    if (before.empty()) return;
    for (const KeyId& k : d.selection) {
        RowKind kind;
        std::string name;
        if (!StudioTrackOfRow(k.first, kind, name) || kind != d.curveClipKind) continue;
        if (kind == RowKind::Camera) {
            if (CameraKf* c = FindKey(d.camera.camera, k.second)) std::memcpy(c->interp, d.curveClip, sizeof(c->interp));
        } else if (auto& t = d.models[d.selectedModel]->motion.bones; t.count(name)) {
            if (BoneKf* b = FindKey(t[name], k.second)) CopyBoneCurves(d.curveClip, b->interp);
        }
    }
    StudioPushTrackEdit(Tr("곡선 붙여넣기"), before);
}

void App::StudioJumpKey(int dir) {
    StudioDoc& d = *studio_;
    const int f = d.Frame();
    int best = dir < 0 ? -1 : INT_MAX;
    for (const TimelineRow& r : d.rows) {
        // rows are sorted by frame: binary search for the neighbours of f
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), f, [](const TimelineKey& k, int v) { return k.frame < v; });
        if (dir < 0) {
            if (it != r.keys.begin()) best = std::max(best, std::prev(it)->frame);
        } else {
            if (it != r.keys.end() && it->frame == f) ++it;
            if (it != r.keys.end()) best = std::min(best, it->frame);
        }
    }
    if (best >= 0 && best != INT_MAX) {
        StudioSetPlaying(false);
        StudioSeek(best / (double)kMmdFps);
    }
}

void App::StudioHandleTimeline(const TimelineEvents& ev) {
    OpTimer timer{"StudioHandleTimeline"};
    StudioDoc& d = *studio_;
    if (ev.seek) {
        StudioSetPlaying(false);
        StudioSeek(ev.seekFrame / (double)kMmdFps);
    }
    if (ev.toggleGroup) {
        if (!d.collapsed.erase(ev.toggledRow)) d.collapsed.insert(ev.toggledRow);
        d.rowsKey = ~0ull;
    }
    if (ev.rowClick) {
        // a row label selects the row and all of its keys (a group: every row in it, also when collapsed)
        const auto selectRowKeys = [&](uint64_t row, bool add) {
            for (uint64_t r : StudioExpandRows({row}))
                for (int f : StudioRowFrames(r)) {
                    if (add) d.selection.insert({r, f});
                    else d.selection.erase({r, f});
                }
        };
        const uint64_t id = ev.rowClickId;
        int anchorIndex = -1, index = -1;
        for (int i = 0; i < (int)d.rows.size(); ++i) {
            if (d.rows[i].id == d.rowAnchor) anchorIndex = i;
            if (d.rows[i].id == id) index = i;
        }
        if (ev.rowClickMode == RowClickMode::Range && anchorIndex >= 0 && index >= 0) {
            d.selectedRows.clear();
            d.selection.clear();
            for (int i = std::min(anchorIndex, index); i <= std::max(anchorIndex, index); ++i) {
                d.selectedRows.insert(d.rows[i].id);
                selectRowKeys(d.rows[i].id, true);
            }
        } else if (ev.rowClickMode == RowClickMode::Toggle) {
            if (d.selectedRows.erase(id)) selectRowKeys(id, false);
            else { d.selectedRows.insert(id); selectRowKeys(id, true); }
            d.rowAnchor = id;
        } else {
            d.selectedRows = {id};
            d.selection.clear();
            selectRowKeys(id, true);
            d.rowAnchor = id;
        }
        d.rowsKey = ~0ull;
    }
    if (ev.select) {
        if (ev.selectMode == SelectMode::Replace) {
            d.selection.clear();
            d.selectedRows.clear();
        }
        for (const TimelineKeyRef& k : ev.selectKeys) {
            const KeyId id{k.row, k.frame};
            if (ev.selectMode == SelectMode::Toggle && d.selection.count(id)) d.selection.erase(id);
            else d.selection.insert(id);
        }
        d.rowsKey = ~0ull;
    }
    if (ev.moveKeys && !d.selection.empty()) {
        const std::vector<TrackState> before = StudioCaptureSelectedTracks();
        std::map<std::pair<int, std::string>, std::set<int>> frames;  // (kind, name) -> selected frames
        std::map<std::pair<int, std::string>, std::set<uint64_t>> rowsOf;  // a bone can be listed in two groups
        for (const KeyId& k : d.selection) {
            RowKind kind;
            std::string name;
            if (!StudioTrackOfRow(k.first, kind, name)) continue;
            frames[{(int)kind, name}].insert(k.second);
            rowsOf[{(int)kind, name}].insert(k.first);
        }
        std::set<KeyId> moved;
        for (const auto& [track, fs] : frames) {
            const RowKind kind = (RowKind)track.first;
            if (kind == RowKind::Camera) MoveKeyFrames(d.camera.camera, fs, ev.moveDelta);
            else if (kind == RowKind::Bone) MoveKeyFrames(d.models[d.selectedModel]->motion.bones[track.second], fs, ev.moveDelta);
            else MoveKeyFrames(d.models[d.selectedModel]->motion.morphs[track.second], fs, ev.moveDelta);
            for (uint64_t row : rowsOf[track])
                for (int f : fs) moved.insert({row, std::max(0, f + ev.moveDelta)});
        }
        StudioPushTrackEdit(Tr("키 이동"), before);
        d.selection = std::move(moved);
        d.rowsKey = ~0ull;
    }
    if (ev.addKeyAt) StudioInsertKeys({ev.addKeyRow}, ev.addKeyFrame);
}

void App::StudioRebuildRows() {
    OpTimer timer{"StudioRebuildRows"};
    StudioDoc& d = *studio_;
    d.rows.clear();
    d.groupChildren.clear();
    auto keysOf = [&](uint64_t rowId, const auto& keys, std::vector<TimelineKey>& out) {
        out.reserve(keys.size());
        for (const auto& k : keys) out.push_back({k.frame, d.selection.count({rowId, k.frame}) != 0});
    };
    if (d.selectedModel < 0) {
        TimelineRow r;
        r.id = MakeRowId(RowKind::Camera, 0, 0);
        r.label = Tr("카메라");
        r.selected = d.selectedRows.count(r.id) != 0;
        keysOf(r.id, d.camera.camera, r.keys);
        d.rows.push_back(std::move(r));
        return;
    }
    StudioModel* m = d.Selected();
    if (!m) return;
    const PmxModel& pmx = *m->pmx;
    const MotionData& mo = m->motion;

    auto addGroup = [&](uint32_t g, const std::string& label, const std::vector<PmxDisplayFrame::Item>& items) {
        if (items.empty()) return;
        TimelineRow group;
        group.id = MakeRowId(RowKind::Group, g, 0);
        group.label = label;
        group.isGroup = true;
        group.keysEditable = false;
        group.expanded = !d.collapsed.count(group.id);
        group.selected = d.selectedRows.count(group.id) != 0;
        std::vector<uint64_t>& childIds = d.groupChildren[group.id];
        std::set<int> summary;
        std::vector<TimelineRow> children;
        std::set<std::pair<bool, int32_t>> listed;  // some models list a bone twice in one frame
        for (const PmxDisplayFrame::Item& it : items) {
            if (!listed.insert({it.morph, it.index}).second) continue;
            TimelineRow r;
            r.depth = 1;
            if (it.morph) {
                r.id = MakeRowId(RowKind::Morph, g, (uint32_t)it.index);
                r.label = pmx.morphs[(size_t)it.index].name;
                if (auto t = mo.morphs.find(r.label); t != mo.morphs.end()) {
                    keysOf(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            } else {
                r.id = MakeRowId(RowKind::Bone, g, (uint32_t)it.index);
                r.label = pmx.bones[(size_t)it.index].name;
                if (auto t = mo.bones.find(r.label); t != mo.bones.end()) {
                    keysOf(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            }
            r.selected = d.selectedRows.count(r.id) != 0;
            childIds.push_back(r.id);
            if (group.expanded) children.push_back(std::move(r));
        }
        for (int f : summary) group.keys.push_back({f, false});
        d.rows.push_back(std::move(group));
        for (auto& c : children) d.rows.push_back(std::move(c));
    };

    std::vector<char> boneListed(pmx.bones.size(), 0), morphListed(pmx.morphs.size(), 0);
    uint32_t g = 0;
    for (const PmxDisplayFrame& f : pmx.displayFrames) {
        for (const auto& it : f.items) (it.morph ? morphListed : boneListed)[(size_t)it.index] = 1;
        addGroup(g++, f.name, f.items);
    }
    // Animated (or, without display frames, all) bones and morphs that no display frame lists.
    std::vector<PmxDisplayFrame::Item> otherBones, otherMorphs;
    const bool noFrames = pmx.displayFrames.empty();
    for (size_t i = 0; i < pmx.bones.size(); ++i)
        if (!boneListed[i] && (noFrames || mo.bones.count(pmx.bones[i].name))) otherBones.push_back({false, (int32_t)i});
    for (size_t i = 0; i < pmx.morphs.size(); ++i)
        if (!morphListed[i] && (noFrames || mo.morphs.count(pmx.morphs[i].name))) otherMorphs.push_back({true, (int32_t)i});
    addGroup(g++, noFrames ? Tr("본") : Tr("기타 본"), otherBones);
    addGroup(g++, noFrames ? Tr("모프") : Tr("기타 모프"), otherMorphs);
}

void App::StudioImportVmd() {
    const std::filesystem::path path = OpenFileDialog(hwnd_, {{L"VMD", L"*.vmd"}});
    if (!path.empty()) StudioImportVmdFrom(path);
}

void App::StudioImportVmdFrom(const std::filesystem::path& path) {
    StudioDoc& d = *studio_;
    VmdMotion vmd;
    std::string err;
    if (!LoadVmd(path, vmd, &err)) {
        toast_ = {Tr("VMD를 불러오지 못했습니다"), err, {}, true, timeSeconds_ + 5.0};
        return;
    }
    MotionData in = MotionData::FromVmd(vmd);
    const bool cameraFile = !in.camera.empty() && vmd.boneKeys.empty() && vmd.morphKeys.empty();
    if (cameraFile || d.selectedModel < 0) {
        if (in.camera.empty()) {
            toast_ = {Tr("카메라 키가 없는 VMD입니다"), PathToUtf8(path.filename()), {}, true, timeSeconds_ + 5.0};
            return;
        }
        MotionData after = d.camera;
        for (const CameraKf& k : in.camera) UpsertKey(after.camera, k);
        for (const LightKf& k : in.light) UpsertKey(after.light, k);
        d.history.Push(std::make_unique<MotionSwapCommand>(d, -1, Tr("카메라 VMD 불러오기"), d.camera, std::move(after)));
        d.selectedModel = -1;
        d.useMotionCamera = true;
    } else {
        StudioModel* m = d.Selected();
        if (!m) return;
        in.camera.clear();
        in.light.clear();
        in.CanonicalizeNames(*m->pmx);
        MotionData after = m->motion;
        after.Merge(in);
        d.history.Push(std::make_unique<MotionSwapCommand>(d, d.selectedModel, Tr("모션 VMD 불러오기"), m->motion, std::move(after)));
    }
    d.selection.clear();
    d.rowsKey = ~0ull;
    toast_ = {Tr("VMD를 불러왔어요"), PathToUtf8(path.filename()), {}, false, timeSeconds_ + 4.0};
}

void App::StudioExportVmd() {
    StudioDoc& d = *studio_;
    const bool camera = d.selectedModel < 0;
    StudioModel* m = d.Selected();
    if (!camera && !m) return;
    std::wstring suggested = camera ? L"camera.vmd" : Utf8ToWide(m->name) + L".vmd";
    for (wchar_t& c : suggested)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const std::filesystem::path path = SaveFileDialog(hwnd_, {{L"VMD", L"*.vmd"}}, suggested, L"vmd");
    if (!path.empty()) StudioExportVmdTo(path);
}

bool App::StudioExportVmdTo(const std::filesystem::path& path) {
    StudioDoc& d = *studio_;
    const bool camera = d.selectedModel < 0;
    StudioModel* m = d.Selected();
    if (!camera && !m) return false;
    VmdMotion vmd;
    if (camera) {
        MotionData c;
        c.camera = d.camera.camera;
        c.light = d.camera.light;
        c.shadow = d.camera.shadow;
        vmd = c.ToVmd();
        vmd.modelName = kCameraModelName;
    } else {
        vmd = m->motion.ToVmd();
        vmd.modelName = m->pmx->name;  // MMD checks the motion's model name against the model it is loaded onto
    }
    std::string err;
    if (!SaveVmd(path, vmd, &err)) {
        toast_ = {Tr("VMD를 저장하지 못했습니다"), err, {}, true, timeSeconds_ + 5.0};
        return false;
    }
    d.savedVersion = d.history.Version();
    toast_ = {Tr("VMD로 내보냈어요"), PathToUtf8(path.filename()), path, false, timeSeconds_ + 5.0};
    return true;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void App::DrawStudio() {
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ds);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##studio", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    const float top = Dp(kTopBarH), left = Dp(kOutlinerW), right = ds.x - Dp(kInspectorW), bottom = ds.y - Dp(kBottomH);
    DrawStudioViewport(left, top, right, bottom);
    DrawStudioTopBar(0, 0, ds.x, top);
    if (!studio_) {  // left the studio from the back button
        ImGui::End();
        return;
    }
    DrawStudioOutliner(0, top, left, bottom);
    DrawStudioInspector(right, top, ds.x, bottom);
    DrawStudioTimeline(0, bottom, ds.x, ds.y);

    // Unsaved changes prompt
    if (studioLeaveConfirm_) {
        ImGui::OpenPopup("##leavestudio");
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(Dp(400.0f), 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(P().surface));
        if (ImGui::BeginPopupModal("##leavestudio", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(dl, Font::Semibold, size::Title, c, P().ink, Tr("내보내지 않은 편집이 있어요"));
            Text(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(30.0f)), P().ink2,
                 Tr("나가면 VMD로 내보내지 않은 변경 사항이 사라집니다."));
            ImGui::Dummy(ImVec2(0, Dp(64.0f)));
            if (Button("##stay", Tr("계속 편집"), nullptr, ButtonKind::Secondary, ImVec2(170.0f, 40.0f))) {
                studioLeaveConfirm_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, Dp(12.0f));
            bool leave = false;
            if (Button("##leave", Tr("나가기"), nullptr, ButtonKind::Danger, ImVec2(170.0f, 40.0f))) {
                leave = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            if (leave) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::End();
                LeaveStudio();
                return;
            }
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::End();
    DrawToast();

    // The 3D view fills the area between the panels (back buffer pixels = ImGui display pixels).
    RenderSettings rs = renderer_.Settings();
    const uint32_t vx = (uint32_t)left, vy = (uint32_t)top;
    const uint32_t vw = (uint32_t)std::max(16.0f, right - left), vh = (uint32_t)std::max(16.0f, bottom - top);
    if (rs.viewportX != vx || rs.viewportY != vy || rs.viewportW != vw || rs.viewportH != vh) {
        rs.viewportX = vx; rs.viewportY = vy; rs.viewportW = vw; rs.viewportH = vh;
        renderer_.SetSettings(rs);
    }
}

void App::DrawStudioTopBar(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x0, y1 - 0.5f), ImVec2(x1, y1 - 0.5f), p.line);

    const float cy = (y0 + y1) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(x0 + Dp(10.0f), cy - Dp(18.0f)));
    if (IconButton("##back", icon::ArrowLeft, Tr("라이브러리로 돌아가기"))) {
        if (d.history.Version() != d.savedVersion) studioLeaveConfirm_ = true;
        else {
            LeaveStudio();
            return;
        }
    }
    float x = x0 + Dp(58.0f);
    Text(dl, Font::Bold, size::Title, ImVec2(x, cy - Dp(11.0f)), p.ink, Tr("스튜디오"));
    x += TextSize(Font::Bold, size::Title, Tr("스튜디오")).x + Dp(14.0f);
    if (d.history.Version() != d.savedVersion) {
        ImVec2 bs;
        Badge(dl, ImVec2(x, cy - Dp(10.0f)), Tr("내보내지 않음"), p.warnSoft, p.warn, &bs);
        x += bs.x + Dp(14.0f);
    }

    // undo / redo
    ImGui::SetCursorScreenPos(ImVec2(x + Dp(6.0f), cy - Dp(18.0f)));
    ImGui::BeginDisabled(!d.history.CanUndo());
    const std::string undoTip = d.history.CanUndo() ? std::string(Tr("실행 취소: ")) + d.history.UndoName() + "  (Ctrl+Z)"
                                                    : std::string(Tr("실행 취소"));
    if (IconButton("##undo", icon::ArrowCcw, undoTip.c_str())) { d.history.Undo(); d.selection.clear(); d.rowsKey = ~0ull; }
    ImGui::EndDisabled();
    ImGui::SameLine(0, Dp(4.0f));
    ImGui::BeginDisabled(!d.history.CanRedo());
    const std::string redoTip = d.history.CanRedo() ? std::string(Tr("다시 실행: ")) + d.history.RedoName() + "  (Ctrl+Y)"
                                                    : std::string(Tr("다시 실행"));
    if (IconButton("##redo", icon::ArrowCw, redoTip.c_str())) { d.history.Redo(); d.selection.clear(); d.rowsKey = ~0ull; }
    ImGui::EndDisabled();

    // import / export (right)
    const char* target = d.selectedModel < 0 ? Tr("카메라") : (d.Selected() ? d.Selected()->name.c_str() : "");
    const float exportW = 150.0f, importW = 150.0f;
    ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(14.0f + exportW + 10.0f + importW), cy - Dp(19.0f)));
    if (Button("##import", Tr("VMD 불러오기"), icon::FolderOpen, ButtonKind::Secondary, ImVec2(importW, 38.0f)))
        StudioImportVmd();
    Tooltip((std::string(Tr("불러올 대상: ")) + target).c_str());
    ImGui::SameLine(0, Dp(10.0f));
    if (Button("##export", Tr("VMD 내보내기"), icon::Export, ButtonKind::Primary, ImVec2(exportW, 38.0f)))
        StudioExportVmd();
    Tooltip((std::string(Tr("내보낼 대상: ")) + target).c_str());
}

void App::DrawStudioOutliner(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x1 - 0.5f, y0), ImVec2(x1 - 0.5f, y1), p.line);
    Text(dl, Font::Semibold, size::Caption, ImVec2(x0 + Dp(16.0f), y0 + Dp(14.0f)), p.ink3, Tr("장면"));

    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + Dp(38.0f)));
    ImGui::BeginChild("##outliner", ImVec2(x1 - x0 - 1.0f, y1 - y0 - Dp(38.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float rowH = Dp(40.0f);
    auto row = [&](int index, const char* glyph, const char* label, const char* sub, bool* visible) {
        ImGui::PushID(index);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 b(a.x + w, a.y + rowH);
        const bool selected = d.selectedModel == index;
        ImGui::InvisibleButton("##row", ImVec2(w - Dp(40.0f), rowH));
        const bool hovered = ImGui::IsItemHovered();
        if (index >= 0 && hovered) Tooltip(PathToUtf8(d.models[index]->path).c_str());
        if (ImGui::IsItemClicked() && d.selectedModel != index) {
            d.selectedModel = index;
            d.selection.clear();
            d.collapsed.clear();
            d.rowsKey = ~0ull;
        }
        const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
        if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
        else if (hovered) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));
        const ImU32 fg = selected ? p.accentInk : p.ink;
        Icon(cdl, glyph, 16.0f, ImVec2(a.x + Dp(26.0f), a.y + rowH * 0.5f), selected ? p.accentInk : p.ink2);
        const float tx = a.x + Dp(44.0f), maxX = b.x - Dp(46.0f);
        if (sub && *sub) {
            TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, label);
            TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, sub);
        } else {
            TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(11.0f)), maxX, fg, label);
        }
        if (visible) {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
            if (IconButton("##vis", *visible ? icon::Eye : icon::EyeSlash, *visible ? Tr("숨기기") : Tr("보이기"), false, 28.0f))
                *visible = !*visible;
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::PopID();
    };

    char sub[64];
    std::snprintf(sub, sizeof(sub), Tr("키 %d개"), (int)d.camera.camera.size());
    row(-1, icon::VideoCamera, Tr("카메라"), sub, nullptr);
    // characters first (they are what gets animated), stage parts after
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < (int)d.models.size(); ++i) {
            StudioModel& m = *d.models[i];
            if (m.isStage != (pass == 1)) continue;
            size_t keys = 0;
            for (const auto& [n, k] : m.motion.bones) keys += k.size();
            for (const auto& [n, k] : m.motion.morphs) keys += k.size();
            if (m.isStage) std::snprintf(sub, sizeof(sub), "%s", Tr("스테이지"));
            else std::snprintf(sub, sizeof(sub), Tr("키 %d개"), (int)keys);
            row(i, m.isStage ? icon::Mountains : icon::PersonSimple, m.name.c_str(), sub, &m.visible);
        }
    }
    ImGui::EndChild();
}

void App::DrawStudioInspector(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x0 + 0.5f, y0), ImVec2(x0 + 0.5f, y1), p.line);
    Text(dl, Font::Semibold, size::Caption, ImVec2(x0 + Dp(18.0f), y0 + Dp(14.0f)), p.ink3, Tr("속성"));

    const float pad = Dp(18.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + pad, y0 + Dp(38.0f)));
    ImGui::BeginChild("##inspector", ImVec2(x1 - x0 - pad * 2.0f, y1 - y0 - Dp(38.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x - Dp(4.0f);

    auto line = [&](const char* label, const std::string& value) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, c, p.ink3, label);
        TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(c.x + Dp(96.0f), c.y), c.x + w, p.ink, value.c_str());
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
    };

    // target
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const StudioModel* m = d.Selected();
        TextEllipsis(cdl, Font::Semibold, size::Body, c, c.x + w, p.ink, m ? m->name.c_str() : Tr("카메라"));
        ImGui::Dummy(ImVec2(w, Dp(28.0f)));
        if (m) {
            line(Tr("본 / 모프"), std::to_string(m->pmx->bones.size()) + " / " + std::to_string(m->pmx->morphs.size()));
        } else {
            line(Tr("키"), std::to_string(d.camera.camera.size()));
            line(Tr("시점"), d.useMotionCamera ? Tr("카메라 모션") : Tr("자유 카메라"));
        }
    }
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));
    cdl->AddLine(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y), p.line);
    ImGui::Dummy(ImVec2(w, Dp(12.0f)));

    // the first selected key decides what is shown; curve edits apply to every selected key of that kind
    const KeyId* first = nullptr;
    RowKind kind = RowKind::Bone;
    std::string name;
    for (const KeyId& k : d.selection) {
        if (StudioTrackOfRow(k.first, kind, name)) { first = &k; break; }
    }
    if (!first) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, Tr("선택한 키 없음"));
        ImGui::Dummy(ImVec2(w, Dp(24.0f)));
        const ImVec2 c2 = ImGui::GetCursorScreenPos();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);  // local coordinates
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped("%s", Tr("타임라인에서 키를 클릭하거나 드래그해서 선택하세요. 행 이름을 클릭하면 그 행의 키가 모두 선택되고, 빈 칸을 더블클릭하면 그 프레임에 키를 추가합니다."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), Tr("선택한 키 %d개"), (int)d.selection.size());
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, buf);
        ImGui::Dummy(ImVec2(w, Dp(26.0f)));
    }
    line(kind == RowKind::Camera ? Tr("트랙") : (kind == RowKind::Bone ? Tr("본") : Tr("모프")),
         kind == RowKind::Camera ? std::string(Tr("카메라")) : name);
    line(Tr("프레임"), std::to_string(first->second));

    StudioModel* m = d.Selected();
    if (kind == RowKind::Morph) {
        const MorphKf* k = m ? FindKey(m->motion.morphs[name], first->second) : nullptr;
        std::snprintf(buf, sizeof(buf), "%.3f", k ? k->weight : 0.0f);
        line(Tr("값"), buf);
        ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Caption, c, p.ink3, Tr("모프 키는 항상 선형으로 보간돼요."));
        ImGui::EndChild();
        return;
    }

    uint8_t curve[4] = {20, 20, 107, 107};
    if (kind == RowKind::Bone) {
        const BoneKf* k = m ? FindKey(m->motion.bones[name], first->second) : nullptr;
        if (k) {
            std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", k->t.x, k->t.y, k->t.z);
            line(Tr("위치"), buf);
            // MMD shows bone rotations as Euler degrees
            const DirectX::XMVECTOR q = DirectX::XMLoadFloat4(&k->r);
            const DirectX::XMMATRIX r = DirectX::XMMatrixRotationQuaternion(q);
            DirectX::XMFLOAT4X4 rm;
            DirectX::XMStoreFloat4x4(&rm, r);
            const float pitch = std::asin(std::clamp(-rm._32, -1.0f, 1.0f));
            const float yaw = std::atan2(rm._31, rm._33);
            const float roll = std::atan2(rm._12, rm._22);
            // + 0.0f turns -0.0 into 0.0 for display
            std::snprintf(buf, sizeof(buf), "%.1f°, %.1f°, %.1f°", DirectX::XMConvertToDegrees(pitch) + 0.0f,
                          DirectX::XMConvertToDegrees(yaw) + 0.0f, DirectX::XMConvertToDegrees(roll) + 0.0f);
            line(Tr("회전"), buf);
            d.curveChannel = std::clamp(d.curveChannel, 0, 3);
            GetBoneCurve(k->interp, d.curveChannel, curve);
        }
    } else {
        const CameraKf* k = FindKey(d.camera.camera, first->second);
        if (k) {
            std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", k->target.x, k->target.y, k->target.z);
            line(Tr("중심"), buf);
            std::snprintf(buf, sizeof(buf), "%.1f°, %.1f°, %.1f°", DirectX::XMConvertToDegrees(k->rotation.x),
                          DirectX::XMConvertToDegrees(k->rotation.y), DirectX::XMConvertToDegrees(k->rotation.z));
            line(Tr("회전"), buf);
            std::snprintf(buf, sizeof(buf), "%.2f  ·  %u°", k->distance, k->fovDeg);
            line(Tr("거리 · 시야각"), buf);
            d.curveChannel = std::clamp(d.curveChannel, 0, 5);
            GetCameraCurve(k->interp, d.curveChannel, curve);
        }
    }

    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("보간 곡선"));
        // copy the first selected key's curves / paste them onto every selected key of the same kind
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(2.0f * 28.0f + 4.0f), c.y - Dp(6.0f)));
        if (IconButton("##curvecopy", icon::Copy, Tr("곡선 복사"), false, 28.0f)) StudioCopyCurve();
        ImGui::SameLine(0, Dp(4.0f));
        ImGui::BeginDisabled(d.curveClipKind != kind);
        if (IconButton("##curvepaste", icon::ClipboardText, Tr("곡선 붙여넣기 (선택한 키 전체)"), false, 28.0f)) StudioPasteCurve();
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y));
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
    }
    if (kind == RowKind::Bone) {
        const char* labels[] = {"X", "Y", "Z", Tr("회전")};
        Segmented("##ch", labels, 4, &d.curveChannel, w / Dpi(), 32.0f);
    } else {
        const char* labels[] = {"X", "Y", "Z", Tr("회전"), Tr("거리"), Tr("시야")};
        Segmented("##ch", labels, 6, &d.curveChannel, w / Dpi(), 32.0f);
    }
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));

    const float plot = std::min(w, Dp(200.0f));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (w - plot) * 0.5f, ImGui::GetCursorScreenPos().y));
    const bool changed = BezierCurveEditor("##curve", curve, plot / Dpi());
    if (changed) {
        if (!d.curveEditing) {
            d.curveBefore = StudioCaptureSelectedTracks();
            d.curveEditing = true;
        }
        // apply to every selected key of the same kind
        for (const KeyId& k : d.selection) {
            RowKind kk;
            std::string nn;
            if (!StudioTrackOfRow(k.first, kk, nn) || kk != kind) continue;
            if (kk == RowKind::Bone) {
                if (BoneKf* b = FindKey(m->motion.bones[nn], k.second)) SetBoneCurve(b->interp, d.curveChannel, curve);
            } else if (kk == RowKind::Camera) {
                if (CameraKf* c = FindKey(d.camera.camera, k.second)) SetCameraCurve(c->interp, d.curveChannel, curve);
            }
        }
        // re-evaluate, but keep the timeline rows: a curve edit moves no key
        if (kind == RowKind::Camera) ++d.cameraVersion;
        else ++m->motionVersion;
    }
    // one undo step per drag / click
    if (d.curveEditing && !ImGui::IsAnyItemActive()) {
        StudioPushTrackEdit(Tr("보간 곡선 편집"), d.curveBefore);
        d.curveBefore.clear();
        d.curveEditing = false;
    }
    ImGui::EndChild();
}

void App::DrawStudioTimeline(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x0, y0 + 0.5f), ImVec2(x1, y0 + 0.5f), p.lineStrong);

    // transport
    const float th = Dp(kTransportH), cy = y0 + th * 0.5f;
    const auto separator = [&](float x) { dl->AddLine(ImVec2(x, cy - Dp(11.0f)), ImVec2(x, cy + Dp(11.0f)), p.line); };
    ImGui::SetCursorScreenPos(ImVec2(x0 + Dp(12.0f), cy - Dp(17.0f)));
    if (IconButton("##start", icon::CaretLineLeft, Tr("처음으로  (Home)"), false, 34.0f)) StudioSeek(0.0);
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##prevkey", icon::SkipBack, Tr("이전 키  (Ctrl+←)"), false, 34.0f)) StudioJumpKey(-1);
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##play", d.playing ? icon::Pause : icon::Play, d.playing ? Tr("일시정지  (Space)") : Tr("재생  (Space)"),
                   d.playing, 34.0f))
        StudioSetPlaying(!d.playing);
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##nextkey", icon::SkipForward, Tr("다음 키  (Ctrl+→)"), false, 34.0f)) StudioJumpKey(1);
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##end", icon::CaretLineRight, Tr("끝으로  (End)"), false, 34.0f)) StudioSeek(d.EndFrame() / (double)kMmdFps);
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##loop", icon::Repeat,
                   d.HasRange() ? Tr("구간 반복") : Tr("반복 재생  (눈금자를 Shift+드래그하면 구간 지정)"), d.loop, 34.0f))
        d.loop = !d.loop;
    ImGui::SameLine(0, Dp(12.0f));

    // frame field: applied on Enter (typing must not seek on every digit)
    int frame = d.Frame();
    ImGui::SetNextItemWidth(Dp(84.0f));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cy - ImGui::GetFrameHeight() * 0.5f));
    if (ImGui::InputInt("##frame", &frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        StudioSetPlaying(false);
        StudioSeek(std::max(0, frame) / (double)kMmdFps);
    }
    Tooltip(Tr("현재 프레임 (Enter로 이동)"));
    ImGui::SameLine(0, Dp(12.0f));
    float tx = ImGui::GetCursorScreenPos().x;
    {
        const std::string t = FormatTime(d.time) + "  /  " + FormatTime(d.EndFrame() / (double)kMmdFps);
        Text(dl, Font::Regular, size::Small, ImVec2(tx, cy - Dp(9.0f)), p.ink2, t.c_str());
        tx += TextSize(Font::Regular, size::Small, t.c_str()).x + Dp(16.0f);
    }
    if (d.HasRange()) {
        // range chip: "Range a–b" + clear
        char buf[64];
        std::snprintf(buf, sizeof(buf), Tr("구간 %d–%d"), d.view.rangeStart, d.view.rangeEnd);
        ImVec2 bs;
        Badge(dl, ImVec2(tx, cy - Dp(10.0f)), buf, p.accentSoft, p.accentInk, &bs);
        ImGui::SetCursorScreenPos(ImVec2(tx + bs.x + Dp(2.0f), cy - Dp(14.0f)));
        if (IconButton("##clearrange", icon::X, Tr("구간 해제  (눈금자 우클릭)"), false, 28.0f)) d.view.rangeStart = d.view.rangeEnd = -1;
    }

    // right side: frame insert/delete | physics | view
    float rx = x1 - Dp(12.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    ImGui::BeginDisabled(!d.cameraEval);
    if (IconButton("##motioncam", icon::VideoCamera,
                   d.useMotionCamera ? Tr("카메라 모션으로 보는 중 (클릭: 자유 카메라)") : Tr("자유 카메라 (클릭: 카메라 모션)"),
                   d.useMotionCamera && d.cameraEval, 34.0f))
        d.useMotionCamera = !d.useMotionCamera;
    ImGui::EndDisabled();
    separator(rx - Dp(8.0f));
    rx -= Dp(16.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    if (IconButton("##physreset", icon::ArrowCcw, Tr("물리 초기화"), false, 34.0f)) d.physicsFrame = -1.0f;
    rx -= Dp(2.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    if (IconButton("##physics", icon::Atom, d.physics ? Tr("물리 켜짐 (클릭: 끄기)") : Tr("물리 꺼짐 (클릭: 켜기)"), d.physics, 34.0f)) {
        d.physics = !d.physics;
        d.physicsFrame = -1.0f;  // start from the motion's pose
    }
    separator(rx - Dp(8.0f));
    {
        const bool range = d.HasRange();
        const int n = range ? d.view.rangeEnd - d.view.rangeStart + 1 : 1;
        const bool rows = !d.selectedRows.empty() || !d.selection.empty();
        char tipDel[160], tipIns[160];
        std::snprintf(tipIns, sizeof(tipIns), range ? Tr("구간 앞에 %d프레임 삽입") : Tr("현재 프레임에 %d프레임 삽입"), n);
        std::snprintf(tipDel, sizeof(tipDel), range ? Tr("구간의 %d프레임 삭제") : Tr("현재 프레임에서 %d프레임 삭제"), n);
        const std::string scope = std::string("  (") + (rows ? Tr("선택한 행") : Tr("모든 행")) + ")";
        rx -= Dp(16.0f + 34.0f);
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
        if (IconButton("##delframes", icon::ArrowsInLineHorizontal, (tipDel + scope).c_str(), false, 34.0f)) StudioShiftFrames(true);
        rx -= Dp(2.0f + 34.0f);
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
        if (IconButton("##insframes", icon::ArrowsOutLineHorizontal, (tipIns + scope).c_str(), false, 34.0f)) StudioShiftFrames(false);
    }

    // timeline
    const uint64_t key = (uint64_t)(d.selectedModel + 2) * 1000003ull;
    if (d.rowsKey != key) {
        StudioRebuildRows();
        d.rowsKey = key;
    }
    const float ty = y0 + th;
    dl->AddLine(ImVec2(x0, ty - 0.5f), ImVec2(x1, ty - 0.5f), p.line);
    ImGui::SetCursorScreenPos(ImVec2(x0, ty));
    ImGui::BeginChild("##timeline", ImVec2(x1 - x0, y1 - ty), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    // Keep the playhead in view when the frame changes by playback or a seek (not while scrubbing the ruler).
    if (d.Frame() != d.followFrame && d.view.dragMode != 1) {
        const float visible = (x1 - x0 - Dp(220.0f + 24.0f)) / (d.view.pxPerFrame * Dpi());
        const float f = (float)d.Frame();
        if (f < d.view.scrollFrame || f > d.view.scrollFrame + visible * 0.92f)
            d.view.scrollFrame = std::max(0.0f, f - visible * (d.playing ? 0.08f : 0.4f));
    }
    d.followFrame = d.Frame();
    TimelineEvents ev;
    Timeline("##tl", ImVec2(0, 0), d.rows, d.Frame(), d.EndFrame(), d.view, ev);
    ImGui::EndChild();
    StudioHandleTimeline(ev);
}

void App::DrawStudioViewport(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##viewport", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool dragging = active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
    if ((dragging || (hovered && io.MouseWheel != 0)) && d.useMotionCamera && d.cameraEval) {
        // take over the motion camera's current view, so the free camera starts where it was
        const CameraPose pose = d.cameraEval->Evaluate((float)(d.time * kMmdFps));
        freeCam_.target = pose.target;
        freeCam_.yaw = pose.rotation.y;
        freeCam_.pitch = std::clamp(-pose.rotation.x, -1.45f, 1.45f);
        freeCam_.distance = std::clamp(std::fabs(pose.distance), 2.0f, 600.0f);
        freeCam_.fovDeg = pose.fovDeg;
        d.useMotionCamera = false;
    }
    FreeCamera& cam = freeCam_;
    if (dragging) {
        const ImVec2 delta = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            cam.yaw += delta.x * 0.006f;
            cam.pitch = std::clamp(cam.pitch + delta.y * 0.006f, -1.45f, 1.45f);
        } else {
            const float rx = std::cos(cam.yaw), rz = std::sin(cam.yaw);
            const float scale = cam.distance * 0.0015f;
            cam.target.x += (-delta.x * rx) * scale;
            cam.target.y += delta.y * scale;
            cam.target.z += (-delta.x * rz) * scale;
        }
    }
    if (hovered && io.MouseWheel != 0.0f) cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel), 2.0f, 600.0f);

    // view label (top left of the viewport)
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* label = d.useMotionCamera && d.cameraEval ? Tr("카메라 모션") : Tr("자유 카메라");
    ImVec2 bs;
    Badge(dl, ImVec2(x0 + Dp(12.0f), y0 + Dp(12.0f)), label, WithAlpha(p.surface, 0.85f), p.ink2, &bs);
}

} // namespace mmdx
