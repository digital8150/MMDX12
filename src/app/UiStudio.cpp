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
    studioLoadTitle_.clear();
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
    StudioPackage& pkg = *studioPackage_;
    auto doc = std::make_unique<StudioDoc>();
    UploadBatch batch(ctx_);
    std::vector<int> remap(pkg.models.size(), -1);  // package index -> doc index (prop parents)
    for (size_t i = 0; i < pkg.models.size(); ++i) {
        StudioPackageModel& m = pkg.models[i];
        auto sm = std::make_unique<StudioModel>();
        sm->name = m.name;
        sm->libraryId = m.libraryId;
        sm->kind = m.kind;
        sm->visible = m.visible;
        sm->attach = m.attach;
        sm->place = sm->placeApplied = m.place;
        sm->shader = m.shader;
        sm->uid = doc->nextUid++;
        sm->path = m.pmx->sourcePath;
        sm->pmx = m.pmx;
        sm->inst = std::make_unique<ModelInstance>(m.pmx);
        // props follow bones every frame: they need the per-frame (character) RT path, not the static stage BLAS
        sm->gpu = renderer_.CreateModel(batch, *m.pmx, m.textures,
                                        m.kind == ModelKind::Stage ? ModelRole::Stage : ModelRole::Character);
        if (!sm->gpu) {
            if (m.kind != ModelKind::Character || pkg.fromProject) {
                LOG_WARN("studio: GPU upload failed: %s", m.name.c_str());
                continue;
            }
            return false;
        }
        sm->motion = std::move(m.motion);
        sm->BuildRowGroups();
        sm->inst->UpdatePose();
        remap[i] = (int)doc->models.size();
        doc->models.push_back(std::move(sm));
    }
    batch.Submit();
    // prop parents: package index -> uid
    for (auto& m : doc->models) {
        if (!m->IsProp()) { m->attach = PropAttach{}; continue; }
        const int p = m->attach.parent;
        const int di = p >= 0 && p < (int)remap.size() ? remap[(size_t)p] : -1;
        m->attach.parent = di >= 0 ? (int)doc->models[(size_t)di]->uid : -1;
    }

    doc->camera = std::move(pkg.camera);
    // a light track that only repeats MMD's defaults says nothing (and would override the lighting): dropped, as in play mode
    if (IsDefaultLightTrack(doc->camera.light)) doc->camera.light.clear();
    doc->audioOffset = pkg.audioOffset;
    doc->audioPath = pkg.audioPath;
    doc->hasAudio = !doc->audioPath.empty() && audio_.Load(doc->audioPath);
    if (doc->hasAudio) {
        doc->audioEndFrame = (float)((audio_.DurationSeconds() + doc->audioOffset) * kMmdFps);
        audio_.SetMuted(false);
    } else if (!doc->audioPath.empty()) {
        pkg.warnings.push_back(Tr("음원을 열 수 없습니다: ") + PathToUtf8(doc->audioPath));
        doc->audioPath.clear();
    }
    doc->physics = settings_.physics && !options_.noPhysics;
    freeCam_ = FreeCamera{};
    if (pkg.fromProject) {
        const ProjectEditor& e = pkg.editor;
        const int sel = e.selectedModel >= 0 && e.selectedModel < (int)remap.size() ? remap[(size_t)e.selectedModel] : -1;
        doc->selectedModel = sel;
        doc->time = options_.seekSeconds > 0 ? options_.seekSeconds : std::max(0, e.frame) / (double)kMmdFps;  // --seek wins
        doc->useMotionCamera = e.useMotionCamera && !doc->camera.camera.empty();
        doc->lights = e.lights;
        for (const SceneLight& l : doc->lights) doc->nextLightUid = std::max(doc->nextLightUid, l.uid + 1);
        // spot targets are saved as 1 + the project's model index (package index here): back to the model's uid
        for (SceneLight& l : doc->lights) {
            if (l.targetUid == 0) continue;
            const size_t pi = l.targetUid - 1;
            const int di = pi < remap.size() ? remap[pi] : -1;
            l.targetUid = di >= 0 ? doc->models[(size_t)di]->uid : 0;
        }
        doc->useShadowTrack = e.useShadowTrack;
        doc->showCameraPath = e.showCameraPath;
        doc->loop = e.loop;
        doc->physics = e.physics && !options_.noPhysics;
        doc->view.rangeStart = e.rangeStart;
        doc->view.rangeEnd = e.rangeEnd;
        doc->view.pxPerFrame = std::clamp(e.pxPerFrame, 0.2f, 40.0f);
        freeCam_.target = e.camTarget;
        freeCam_.yaw = e.camYaw;
        freeCam_.pitch = std::clamp(e.camPitch, -1.45f, 1.45f);
        freeCam_.distance = std::clamp(e.camDistance, 2.0f, 600.0f);
        freeCam_.fovDeg = std::clamp(e.camFovDeg, 5.0f, 120.0f);
        doc->projectPath = pkg.projectPath;  // recovery: the project the autosave belongs to (may be empty)
        if (!pkg.recovered && !pkg.projectPath.empty()) {
            settings_.AddRecentProject(PathToUtf8(pkg.projectPath));
            settings_.Save(settingsPath_);
        }
    } else {
        doc->useMotionCamera = !doc->camera.camera.empty() && !options_.freeCamera;
        doc->lights = PresetLights(0, DirectX::XMFLOAT3{0.0f, 10.0f, 0.0f}, doc->nextLightUid);  // the Studio preset
        // the character (the last model of a library scene); the camera in an empty project
        doc->selectedModel = -1;
        for (int i = (int)doc->models.size() - 1; i >= 0; --i)
            if (doc->models[(size_t)i]->kind == ModelKind::Character) { doc->selectedModel = i; break; }
        doc->time = std::max(0.0, options_.seekSeconds);
    }
    options_.seekSeconds = 0;
    if (options_.hasCamera) {
        const float* c = options_.camera;
        freeCam_.target = {c[0], c[1], c[2]};
        freeCam_.yaw = DirectX::XMConvertToRadians(c[3]);
        freeCam_.pitch = DirectX::XMConvertToRadians(c[4]);
        freeCam_.distance = c[5];
        doc->useMotionCamera = false;
    }
    // a recovered autosave is unsaved work; a project opened from disk or a fresh scene is clean
    doc->MarkSaved();
    if (pkg.recovered) ++doc->projectVersion;
    doc->autosavedStamp = doc->ChangeStamp();
    if (!pkg.warnings.empty()) {
        std::string detail;
        for (size_t i = 0; i < pkg.warnings.size() && i < 3; ++i) detail += (i ? "\n" : "") + pkg.warnings[i];
        if (pkg.warnings.size() > 3) detail += "\n...";
        toast_ = {Tr("일부 파일을 불러오지 못했어요"), detail, {}, true, timeSeconds_ + 8.0};
        for (const std::string& w : pkg.warnings) LOG_WARN("studio: %s", w.c_str());
    }
    StudioEnter(std::move(doc));
    return true;
}

void App::StudioEnter(std::unique_ptr<StudioDoc> doc) {
    studio_ = std::move(doc);
    studioCamPath_.clear();  // cached per camera evaluator version, which restarts with the document
    studioCamKeys_.clear();
    studioKeyEdit_ = false;
    studioLastBind_ = 0;
    studioLeaveConfirm_ = false;
    studioPending_ = StudioAction::None;
    studioViewDrag_ = 0;
    studioAutosaveAt_ = timeSeconds_;
    lastRenderedTime_ = -1;
    if (uiScriptNext_ == 0) framesInScene_ = 0;  // a running ui script keeps its frame clock across project switches
    if (studio_->hasAudio) StudioSeekAudio();
    screen_ = Screen::Studio;
}

void App::LeaveStudio() {
    studioJobs_.clear();  // waits for running loads
    StudioDiscardRecovery();
    ctx_.WaitForGpu();
    studio_.reset();
    audio_.Unload();
    RenderSettings rs = renderer_.Settings();
    rs.viewportX = rs.viewportY = rs.viewportW = rs.viewportH = 0;
    rs.shading = ViewShading::Lit;  // the studio shading mode is an editing view only
    renderer_.SetSettings(rs);
    studioLeaveConfirm_ = false;
    studioPending_ = StudioAction::None;
    screen_ = Screen::Select;
    ApplyRenderSettings();  // also restores the user's render path: the studio forces the raster path (editing view)
}

// ---------------------------------------------------------------------------
// Playback and evaluation
// ---------------------------------------------------------------------------

void App::StudioSeek(double seconds) {
    StudioDoc& d = *studio_;
    d.time = std::clamp(seconds, 0.0, d.EndFrame() / (double)kMmdFps);
    if (d.hasAudio) StudioSeekAudio();
}

void App::StudioSeekAudio() {
    // timeline time t plays audio position t - audioOffset; before the audio starts (negative position) it waits
    const StudioDoc& d = *studio_;
    if (!d.hasAudio) return;
    const double a = d.time - d.audioOffset;
    const bool inside = a >= 0.0 && a < audio_.DurationSeconds() - 0.02;
    audio_.Seek(std::max(0.0, a));
    if (d.playing && inside) audio_.Play();
    else audio_.Pause();
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
    if (d.hasAudio) StudioSeekAudio();
}

void App::UpdateStudio(double dt) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    // The studio is one big ImGui window, so WantCaptureKeyboard is always set: only text input blocks shortcuts.
    // ? (or F1) opens / closes the shortcut list; dialogs over the studio (render, help, unsaved prompt) take the keys.
    if (!io.WantTextInput && !videoDialogOpen_ && !studioLeaveConfirm_ &&
        ((ImGui::IsKeyPressed(ImGuiKey_Slash, false) && io.KeyShift) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
        studioHelpOpen_ = !studioHelpOpen_;
    // holding the right button flies the viewport camera with WASDQE: the shortcuts stay out of the way
    if (!io.WantTextInput && !StudioModal() && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
        if (!ctrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) StudioFocusSelection();
        {  // numpad views (the renderer is perspective-only): 1 front, 3 right, 7 top; Ctrl = the opposite side
            const auto view = [&](ImGuiKey k, float yaw, float pitch) {
                if (!ImGui::IsKeyPressed(k, false)) return;
                StudioTakeFreeCamera();
                freeCam_.yaw = yaw;
                freeCam_.pitch = pitch;
            };
            const float pi = 3.14159265f;
            view(ImGuiKey_Keypad1, ctrl ? pi : 0.0f, 0.0f);
            view(ImGuiKey_Keypad3, ctrl ? -pi * 0.5f : pi * 0.5f, 0.0f);
            view(ImGuiKey_Keypad7, 0.0f, ctrl ? -1.45f : 1.45f);
        }
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
        if (pressed(ImGuiKey_I, false)) StudioRegisterPose(ctrl);  // pose edits, or the picked rows (Ctrl: all bones)
        if (!ctrl && pressed(ImGuiKey_T, false)) d.modelGizmo = !d.modelGizmo;
        if (!ctrl && pressed(ImGuiKey_E, false)) d.gizmoTool = 0;
        if (!ctrl && pressed(ImGuiKey_W, false)) d.gizmoTool = 1;
        if (!ctrl && pressed(ImGuiKey_L, false)) d.gizmoLocal = !d.gizmoLocal;
        if (pressed(ImGuiKey_Escape, false) && d.possessCamera) StudioPossess(false);
        if (pressed(ImGuiKey_Escape, false) && studioViewDrag_ != 2 && d.activeBone >= 0) StudioSelectBone(-1, false);
        if (ctrl && pressed(ImGuiKey_S, false)) StudioSave(shift);
        if (ctrl && pressed(ImGuiKey_O, false)) StudioRequest(StudioAction::Open);
        if (ctrl && pressed(ImGuiKey_N, false)) StudioRequest(StudioAction::New);
        if (studio_.get() != &d) return;  // the project was closed or replaced (New / Open)
    }
    StudioPollJobs();
    StudioAutosave(false, false);
    if (d.playing) {
        if (d.hasAudio && audio_.IsPlaying()) {
            const double a = audio_.PositionSeconds() + d.audioOffset;
            const double predicted = d.time + dt;
            d.time = std::fabs(predicted - a) > 0.05 ? a : predicted;
        } else {
            d.time += dt;
            // the audio starts later on the timeline (positive offset): start it when the playhead reaches it
            const double a = d.time - d.audioOffset;
            if (d.hasAudio && a >= 0.0 && a < audio_.DurationSeconds() - 0.05) StudioSeekAudio();
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
    // a pose undo/redo restored the edits of another frame: go there (the pose only applies at its frame)
    if (d.pendingSeekFrame >= 0) {
        StudioSetPlaying(false);
        StudioSeek(d.pendingSeekFrame / (double)kMmdFps);
        d.pendingSeekFrame = -1;
    }
    // Unregistered pose edits belong to their frame (MMD): leaving it discards them, as an undoable step.
    bool posing = false;
    for (int i = 0; i < (int)d.models.size(); ++i) {
        StudioModel& m = *d.models[i];
        if (m.pose.Empty()) continue;
        if (m.pose.frame == d.Frame() || studioViewDrag_ == 2) {
            posing = true;
            continue;
        }
        d.history.Push(std::make_unique<PoseEditCommand>(d, i, Tr("포즈 버리기"), m.pose, PoseLayer{}));
        d.pendingSeekFrame = -1;  // Push re-applies the empty layer: nothing to seek to
        toast_ = {Tr("등록하지 않은 포즈를 버렸어요"), Tr("Ctrl+Z로 되돌릴 수 있어요."), {}, false, timeSeconds_ + 3.0};
    }
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
        // paused with pose edits: keep simulating so hair and skirts follow the edited pose
        if (!resetPhysics && !d.playing && posing) physicsDt = 1.0f / 60.0f;
    }
    d.physicsFrame = frame;

    // props last: their root follows a bone of a model posed in the first pass
    for (int pass = 0; pass < 2; ++pass)
        for (auto& mp : d.models)
            if (mp->IsProp() == (pass == 1)) StudioUpdateModel(*mp, slot, frame, physicsDt, resetPhysics);

    if (d.cameraEvalVersion != d.cameraVersion) {
        d.cameraEval = d.camera.camera.empty() ? nullptr : CameraMotion::Create(d.camera.ToVmd());
        d.cameraEvalVersion = d.cameraVersion;
        if (!d.cameraEval) d.useMotionCamera = false;
    }
}

void App::StudioUpdateModel(StudioModel& m, uint64_t slot, float frame, float physicsDt, bool resetPhysics) {
    StudioDoc& d = *studio_;
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
    if (!m.IsStage()) StudioApplyPose(m);
    if (m.kind == ModelKind::Character) {
        // placement: the root carries rotation + position; the display scale (library x placement) scales about the
        // origin afterwards, so the position is divided by it
        const PropAttach& pl = m.place;
        const float s = (m.libraryId.empty() ? 1.0f : settings_.CharacterScale(m.libraryId)) * std::max(0.01f, pl.scale);
        inst.SetScale(s);
        {
            constexpr float kRad = 0.01745329252f;
            DirectX::XMFLOAT4X4 root;
            DirectX::XMStoreFloat4x4(&root, DirectX::XMMatrixMultiply(
                DirectX::XMMatrixRotationRollPitchYaw(pl.rotationDeg.x * kRad, pl.rotationDeg.y * kRad, pl.rotationDeg.z * kRad),
                DirectX::XMMatrixTranslation(pl.translation.x / s, pl.translation.y / s, pl.translation.z / s)));
            inst.SetRootTransform(root);
        }
        inst.EnablePhysics(d.physics);
        if (!(m.placeApplied == m.place)) {  // moved by hand: the bodies restart at the new place
            m.placeApplied = m.place;
            resetPhysics = true;
        }
        if (resetPhysics) inst.ResetPhysics();
    }
    if (m.IsProp()) {
        inst.SetRootTransform(StudioPropRoot(m));
    } else if (m.IsStage()) {
        // placement of a stage: scale + rotation + position in the root (no library scale). Keyless stages are
        // posed once, so a moved one is re-posed here and its once-built static BLAS is rebuilt.
        const PropAttach& pl = m.place;
        const float s = std::max(0.01f, pl.scale);
        constexpr float kRad = 0.01745329252f;
        DirectX::XMFLOAT4X4 root;
        DirectX::XMStoreFloat4x4(&root, DirectX::XMMatrixMultiply(
            DirectX::XMMatrixMultiply(
                DirectX::XMMatrixScaling(s, s, s),
                DirectX::XMMatrixRotationRollPitchYaw(pl.rotationDeg.x * kRad, pl.rotationDeg.y * kRad,
                                                      pl.rotationDeg.z * kRad)),
            DirectX::XMMatrixTranslation(pl.translation.x, pl.translation.y, pl.translation.z)));
        inst.SetRootTransform(root);
        if (!(m.placeApplied == m.place)) {
            m.placeApplied = m.place;
            m.gpu->Rt().blasBuilt = false;  // the stage BLAS was built once, at the old placement
            if (!m.bound) inst.UpdatePose(0.0f);
        }
    }
    if (!m.IsStage() || m.bound) inst.UpdatePose(physicsDt);
    m.gpu->UpdateSkinning(slot, inst.SkinMatrices());
    m.gpu->UpdateMorphs(slot, inst.VertexMorphDeltas(), inst.MorphVersion());
    if (m.kind == ModelKind::Character) {
        ShaderChoice shader = m.shader;
        if (options_.shaderPackSet) {   // --shader-pack: this run only, the project keeps its choice
            if (shader.pack != options_.shaderPack) shader.params.clear();
            shader.pack = options_.shaderPack;
        }
        ApplyShaderChoice(*m.gpu, shader);
    }
    m.gpu->UpdateMaterials(slot, inst.MaterialMul(), inst.MaterialAdd(), inst.MaterialVersion());
}

void App::StudioCamera(CameraParams& camera) const {
    const StudioDoc& d = *studio_;
    const float frame = (float)(d.time * kMmdFps);
    if (d.useMotionCamera && d.cameraEval) {
        CameraPose pose = d.cameraEval->Evaluate(frame);
        if (!StudioCameraPerspective(frame)) {
            StudioOrthoCamera(pose, camera);
            return;
        }
        CameraMotion::ToView(pose, &camera.view, &camera.eye);
        camera.fovYRadians = DirectX::XMConvertToRadians(pose.fovDeg);
    } else {
        const FreeCamera& cam = freeCam_;
        const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
        const DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&cam.target);
        const DirectX::XMVECTOR eye =
            DirectX::XMVectorAdd(target, DirectX::XMVectorScale(DirectX::XMVectorSet(sy * cp, sp, -cy * cp, 0), cam.distance));
        DirectX::XMStoreFloat4x4(&camera.view, DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0, 1, 0, 0)));
        DirectX::XMStoreFloat3(&camera.eye, eye);
        camera.fovYRadians = DirectX::XMConvertToRadians(cam.fovDeg);
    }
}

void App::BuildStudioFrameView(FrameView& view) {
    StudioDoc& d = *studio_;
    StudioCamera(view.camera);

    // stage parts first (the renderer's draw order), then characters and props
    bool anyStage = false;
    const StudioModel* performer = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& m : d.models) {
            if (!m->visible || m->IsStage() != (pass == 0)) continue;
            view.models.push_back(m->gpu.get());
            if (m->IsStage()) anyStage = true;
            else if (!performer && m->kind == ModelKind::Character) performer = m.get();
        }
    }
    view.studioFloor = !anyStage;

    // where the spots that aim at a character look: its centre bone and head (the performer: the first visible character)
    const auto anchorsOf = [](const StudioModel& m, DirectX::XMFLOAT3& focus, DirectX::XMFLOAT3& head) {
        const int center = m.pmx->FindBone(kCenterBone);
        if (center >= 0) focus = m.inst->BoneWorldPosition(center);
        const int hb = m.pmx->FindBone(kHeadBone);
        head = focus;
        if (hb >= 0) head = m.inst->BoneWorldPosition(hb);
        else head.y += 8.0f;
    };
    LightAnchors anchors;
    if (performer) anchorsOf(*performer, anchors.focus, anchors.head);
    for (const auto& m : d.models) {
        if (m->kind != ModelKind::Character) continue;
        LightAnchors::Character c;
        c.uid = m->uid;
        c.centre = {0, 10, 0};
        anchorsOf(*m, c.centre, c.head);
        anchors.characters.push_back(c);
    }
    BuildSceneLighting(d.lights, d.camera.light, d.time, anchors, view.light);
    StudioApplyLightTracks(view);
    if (performer) {
        DirectX::XMFLOAT3 target = anchors.head;
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
    if (IsCameraKind(kind)) return d.selectedModel < 0;
    if (kind == RowKind::SceneLight) {
        // a light row is only offered through the camera target (the lights live next to the camera)
        const uint32_t uid = RowIndexOf(row);
        if (d.selectedModel >= 0 || !d.FindLight(uid)) return false;
        name = LightTrackName(uid);
        return true;
    }
    if (d.selectedModel < 0 || d.selectedModel >= (int)d.models.size()) return false;
    const PmxModel& pmx = *d.models[d.selectedModel]->pmx;
    const uint32_t idx = RowIndexOf(row);
    if (kind == RowKind::Bone && idx < pmx.bones.size()) { name = pmx.bones[idx].name; return true; }
    if (kind == RowKind::Morph && idx < pmx.morphs.size()) { name = pmx.morphs[idx].name; return true; }
    return false;
}

// The sun's light as the viewport shows it at `frame`: the camera VMD's light track while the sun is linked to it,
// else the sun's own (keyed) values; without a sun a black light with the default direction.
studio::LightKf App::StudioSunLight(float frame) const {
    LightParams lp;
    BuildSceneLighting(studio_->lights, studio_->camera.light, frame / (double)kMmdFps, LightAnchors{}, lp);
    return LightKf{(int)std::floor(frame), lp.color, lp.direction};
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

// Viewport navigation starts from the motion camera's current view, so the free camera begins where it was.
void App::StudioTakeFreeCamera() {
    StudioDoc& d = *studio_;
    if (!d.useMotionCamera || !d.cameraEval) return;
    const CameraPose pose = d.cameraEval->Evaluate((float)(d.time * kMmdFps));
    freeCam_.target = pose.target;
    freeCam_.yaw = pose.rotation.y;
    freeCam_.pitch = std::clamp(-pose.rotation.x, -1.45f, 1.45f);
    freeCam_.distance = std::clamp(std::fabs(pose.distance), 2.0f, 600.0f);
    freeCam_.fovDeg = pose.fovDeg;
    d.useMotionCamera = false;
}

// F: frame the picked bone, else the selected model (its centre bone), else the whole scene origin.
void App::StudioFocusSelection() {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m) m = d.Selected();
    StudioTakeFreeCamera();
    if (!m || !m->inst) {
        freeCam_.target = {0, 10, 0};
        freeCam_.distance = 45.0f;
        return;
    }
    const float scale = std::max(0.1f, m->inst->Scale());
    if (d.activeBone >= 0 && d.activeBone < (int)m->pmx->bones.size()) {
        freeCam_.target = m->inst->BoneWorldPosition(d.activeBone);
        freeCam_.distance = std::clamp(12.0f * scale, 3.0f, 600.0f);
        return;
    }
    const int center = m->pmx->FindBone(kCenterBone);
    DirectX::XMFLOAT3 t{0, 10, 0};
    if (center >= 0) {
        t = m->inst->BoneWorldPosition(center);
        t.y += 4.0f * scale;
    }
    freeCam_.target = t;
    freeCam_.distance = std::clamp((m->IsStage() ? 90.0f : 40.0f) * scale, 3.0f, 600.0f);
}

CameraKf App::StudioViewedCamera(int frame) const {
    const StudioDoc& d = *studio_;
    CameraKf k;
    if (!d.useMotionCamera || d.camera.camera.empty()) {
        // the view the user is looking at
        FillLinearCameraInterp(k.interp);
        PoseFromFree(freeCam_.target, freeCam_.yaw, freeCam_.pitch, freeCam_.distance, freeCam_.fovDeg, k);
        k.frame = frame;
    } else {
        k = SampleCamera(d.camera.camera, frame);
    }
    return k;
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
            UpsertKey(d.camera.camera, StudioViewedCamera(frame));
        } else if (kind == RowKind::Light) {
            // a new key keeps the current interpolated value (empty track: the sun's light)
            LightKf k = d.camera.light.empty() ? StudioSunLight((float)frame) : SampleLight(d.camera.light, (float)frame);
            k.frame = frame;
            UpsertKey(d.camera.light, k);
        } else if (kind == RowKind::Shadow) {
            ShadowKf k = d.camera.shadow.empty() ? ShadowKf{frame, 1, 0.01125f} : SampleShadow(d.camera.shadow, (float)frame);
            k.frame = frame;
            UpsertKey(d.camera.shadow, k);
        } else if (kind == RowKind::SceneLight) {
            // a new light key keeps the light's current value (sampled where keys exist)
            if (SceneLight* light = d.FindLight(LightUidOfTrack(name)))
                UpsertKey(light->keys, LightKey{frame, SampleLightValues(*light, frame)});
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
    } else if (kind == RowKind::Light) {
        add(d.camera.light);
    } else if (kind == RowKind::Shadow) {
        add(d.camera.shadow);
    } else if (kind == RowKind::SceneLight) {
        if (const SceneLight* light = d.FindLight(LightUidOfTrack(name))) add(light->keys);
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
    if (d.selectedModel < 0) {
        all.insert(MakeRowId(RowKind::Camera, 0, 0));
        all.insert(MakeRowId(RowKind::Light, 0, 0));
        all.insert(MakeRowId(RowKind::Shadow, 0, 0));
        for (const SceneLight& l : d.lights)
            if (l.kind != LightKind::Ambient) all.insert(MakeRowId(RowKind::SceneLight, 0, l.uid));
    }
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
        if (IsCameraKind(kind)) {
            if (kind == RowKind::Light) EraseKeyFrames(d.camera.light, fs);
            else if (kind == RowKind::Shadow) EraseKeyFrames(d.camera.shadow, fs);
            else EraseKeyFrames(d.camera.camera, fs);
        }
        else if (kind == RowKind::SceneLight) {
            if (SceneLight* light = d.FindLight(LightUidOfTrack(track.second))) EraseKeyFrames(light->keys, fs);
        }
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
        if (IsCameraKind(kind)) {
            if (kind == RowKind::Light) {
                if (const LightKf* p = FindKey(d.camera.light, k.second)) { c.light = *p; found = true; }
            } else if (kind == RowKind::Shadow) {
                if (const ShadowKf* p = FindKey(d.camera.shadow, k.second)) { c.shadow = *p; found = true; }
            } else if (const CameraKf* p = FindKey(d.camera.camera, k.second)) { c.camera = *p; found = true; }
        } else if (kind == RowKind::SceneLight) {
            if (const SceneLight* light = d.FindLight(LightUidOfTrack(name)))
                if (const LightKey* p = FindKey(light->keys, k.second)) { c.sceneLight = *p; found = true; }
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
                continue;  // morphs, lights and shadows have no interpolation
            }
        } else if (kind == RowKind::Camera) { CameraKf k = c.camera; k.frame = f; UpsertKey(d.camera.camera, k); }
        else if (kind == RowKind::Light) { LightKf k = c.light; k.frame = f; UpsertKey(d.camera.light, k); }
        else if (kind == RowKind::Shadow) { ShadowKf k = c.shadow; k.frame = f; UpsertKey(d.camera.shadow, k); }
        else if (kind == RowKind::SceneLight) {
            if (SceneLight* light = d.FindLight(LightUidOfTrack(name))) { LightKey k = c.sceneLight; k.frame = f; UpsertKey(light->keys, k); }
        }
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
            } else if (kind == RowKind::Light) {
                apply(d.camera.light);
            } else if (kind == RowKind::Shadow) {
                apply(d.camera.shadow);
            } else if (kind == RowKind::SceneLight) {
                if (SceneLight* light = d.FindLight(LightUidOfTrack(name))) apply(light->keys);
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
            if (RowKindOf(id) == RowKind::SceneLight) StudioSelectLight(RowIndexOf(id));
            else if (IsCameraKind(RowKindOf(id))) d.selectedLightUid = 0;
        }
        // bone rows also pick the bones for the viewport (the clicked one gets the gizmo)
        d.selectedBones.clear();
        for (uint64_t r : d.selectedRows)
            if (RowKindOf(r) == RowKind::Bone) d.selectedBones.insert((int)RowIndexOf(r));
        d.activeBone = RowKindOf(id) == RowKind::Bone && d.selectedBones.count((int)RowIndexOf(id)) ? (int)RowIndexOf(id)
                       : d.selectedBones.empty() ? -1 : *d.selectedBones.begin();
        d.rowsKey = ~0ull;
    }
    if (ev.select) {
        if (ev.selectMode == SelectMode::Replace) {
            d.selection.clear();
            d.selectedRows.clear();
        }
        if (ev.selectMode == SelectMode::Replace) {
            d.selectedBones.clear();
            d.activeBone = -1;
        }
        const StudioModel* sm = d.Selected();
        for (const TimelineKeyRef& k : ev.selectKeys) {
            uint64_t row = k.row;
            if (sm && (RowKindOf(row) == RowKind::Bone || RowKindOf(row) == RowKind::Morph))
                if (const uint64_t c = CanonicalRow(*sm, RowKindOf(row), RowIndexOf(row))) row = c;
            const KeyId id{row, k.frame};
            if (ev.selectMode == SelectMode::Toggle && d.selection.count(id)) d.selection.erase(id);
            else d.selection.insert(id);
            // keys of a bone row pick that bone for the viewport (the first key's bone gets the gizmo)
            if (RowKindOf(row) == RowKind::Bone && ev.selectKeys.size() <= 64) {
                d.selectedBones.insert((int)RowIndexOf(row));
                if (d.activeBone < 0) d.activeBone = (int)RowIndexOf(row);
            }
        }
        if (!ev.selectKeys.empty()) d.inspectorTab = 0;
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
            else if (kind == RowKind::Light) MoveKeyFrames(d.camera.light, fs, ev.moveDelta);
            else if (kind == RowKind::Shadow) MoveKeyFrames(d.camera.shadow, fs, ev.moveDelta);
            else if (kind == RowKind::SceneLight) {
                if (SceneLight* light = d.FindLight(LightUidOfTrack(track.second)))
                    MoveKeyFrames(light->keys, fs, ev.moveDelta);
            }
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
        TimelineRow l;
        l.id = MakeRowId(RowKind::Light, 0, 0);
        l.label = Tr("조명");
        l.selected = d.selectedRows.count(l.id) != 0;
        keysOf(l.id, d.camera.light, l.keys);
        d.rows.push_back(std::move(l));
        TimelineRow sh;
        sh.id = MakeRowId(RowKind::Shadow, 0, 0);
        sh.label = Tr("셀프 섀도");
        sh.selected = d.selectedRows.count(sh.id) != 0;
        keysOf(sh.id, d.camera.shadow, sh.keys);
        d.rows.push_back(std::move(sh));
        // the scene lights: one row per keyable light (sun, point, spot); the ambient light has no keys
        for (const SceneLight& light : d.lights) {
            if (light.kind == LightKind::Ambient) continue;
            TimelineRow r;
            r.id = MakeRowId(RowKind::SceneLight, 0, light.uid);
            r.label = light.name;
            r.selected = d.selectedRows.count(r.id) != 0;
            keysOf(r.id, light.keys, r.keys);
            d.rows.push_back(std::move(r));
        }
        return;
    }
    StudioModel* m = d.Selected();
    if (!m) return;
    const PmxModel& pmx = *m->pmx;
    const MotionData& mo = m->motion;
    // A bone listed in two display frames has two rows: keys are selected through its canonical row only, so each key
    // counts once and both rows show the same selection.
    const auto canon = [&](uint64_t row) {
        const RowKind k = RowKindOf(row);
        if (k != RowKind::Bone && k != RowKind::Morph) return row;
        const uint64_t c = CanonicalRow(*m, k, RowIndexOf(row));
        return c ? c : row;
    };
    if (std::any_of(d.selection.begin(), d.selection.end(), [&](const KeyId& k) { return canon(k.first) != k.first; })) {
        std::set<KeyId> canonical;
        for (const KeyId& k : d.selection) canonical.emplace_hint(canonical.end(), canon(k.first), k.second);
        d.selection.swap(canonical);
    }
    auto keysOfModel = [&](uint64_t rowId, const auto& keys, std::vector<TimelineKey>& out) { keysOf(canon(rowId), keys, out); };

    // the bone / morph search in the inspector narrows the timeline rows (groups without a match disappear)
    const auto matches = [&](const PmxDisplayFrame::Item& it) {
        if (!d.boneFilter[0]) return true;
        const std::string& n = it.morph ? pmx.morphs[(size_t)it.index].name : pmx.bones[(size_t)it.index].name;
        const std::string& en = it.morph ? pmx.morphs[(size_t)it.index].nameEn : pmx.bones[(size_t)it.index].nameEn;
        return n.find(d.boneFilter) != std::string::npos || en.find(d.boneFilter) != std::string::npos;
    };
    auto addGroup = [&](uint32_t g, const std::string& label, const std::vector<PmxDisplayFrame::Item>& allItems) {
        std::vector<PmxDisplayFrame::Item> filtered;
        if (d.boneFilter[0])
            for (const auto& it : allItems)
                if (matches(it)) filtered.push_back(it);
        const std::vector<PmxDisplayFrame::Item>& items = d.boneFilter[0] ? filtered : allItems;
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
                    keysOfModel(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            } else {
                r.id = MakeRowId(RowKind::Bone, g, (uint32_t)it.index);
                r.label = pmx.bones[(size_t)it.index].name;
                if (auto t = mo.bones.find(r.label); t != mo.bones.end()) {
                    keysOfModel(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            }
            r.selected = d.selectedRows.count(r.id) != 0 || (!it.morph && d.selectedBones.count(it.index));
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
    // a "camera file" is a VMD with no bone and no morph keys and at least one camera, light or shadow key
    const bool cameraFile = (!in.camera.empty() || !in.light.empty() || !in.shadow.empty()) && vmd.boneKeys.empty() &&
                            vmd.morphKeys.empty();
    if (cameraFile || d.selectedModel < 0) {
        // a light track that only repeats MMD's defaults says nothing (and would override the lighting): dropped
        if (IsDefaultLightTrack(in.light)) in.light.clear();
        // the error fires only when camera, light AND shadow are all empty
        if (in.camera.empty() && in.light.empty() && in.shadow.empty()) {
            toast_ = {Tr("카메라 키가 없는 VMD입니다"), PathToUtf8(path.filename()), {}, true, timeSeconds_ + 5.0};
            return;
        }
        MotionData after = d.camera;
        for (const CameraKf& k : in.camera) UpsertKey(after.camera, k);
        for (const LightKf& k : in.light) UpsertKey(after.light, k);
        for (const ShadowKf& k : in.shadow) UpsertKey(after.shadow, k);
        d.history.Push(std::make_unique<MotionSwapCommand>(d, -1, Tr("카메라 VMD 불러오기"), d.camera, std::move(after)));
        d.selectedModel = -1;
        if (!in.camera.empty()) d.useMotionCamera = true;
    } else {
        StudioModel* m = d.Selected();
        if (!m) return;
        in.camera.clear();
        in.light.clear();
        in.shadow.clear();
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
    toast_ = {Tr("VMD로 내보냈어요"), PathToUtf8(path.filename()), path, false, timeSeconds_ + 5.0};
    return true;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

namespace {
// Panel window titles: the ### part is the stable id (the text follows the UI language)
std::string PanelTitle(const char* text, const char* id) { return std::string(text) + "###" + id; }

// Default panel arrangement, built when no layout exists (first run, scripted runs, "reset layout"):
// scene list left, properties right, timeline below, the 3D view in the middle.
void BuildDefaultStudioDock(ImGuiID dock, ImVec2 size, float leftW, float rightW, float bottomH) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, size);
    ImGuiID center = dock, left = 0, right = 0, bottom = 0;
    bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomH / size.y, nullptr, &center);
    left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftW / size.x, nullptr, &center);
    right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightW / (size.x - leftW), nullptr, &center);
    ImGui::DockBuilderDockWindow("###studio_outliner", left);
    ImGui::DockBuilderDockWindow("###studio_inspector", right);
    ImGui::DockBuilderDockWindow("###studio_timeline", bottom);
    ImGui::DockBuilderDockWindow("###studio_viewport", center);
    ImGui::DockBuilderFinish(dock);
}
} // namespace

void App::DrawStudio() {
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const Palette& pal = P();
    const float top = Dp(kTopBarH);

    // --- top bar: a fixed window above the dock space
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(ds.x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##studiotop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    DrawStudioTopBar(0, 0, ds.x, top);
    ImGui::End();
    if (studio_.get() != &d) return;  // left the studio (back button) or replaced the project (project menu)

    // --- dock space host (transparent: the 3D view shows through the viewport panel)
    const ImVec2 hostSize(ds.x, std::max(1.0f, ds.y - top));
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(hostSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##studiohost", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const ImGuiID dockId = ImGui::GetID("##StudioDock");
    if (studioResetLayout_ || ImGui::DockBuilderGetNode(dockId) == nullptr) {
        // a short window (high DPI scale, small screens) gives the viewport a bigger share: the timeline shrinks to 30 %
        const float bottomH = std::clamp(ds.y / Dpi() * 0.3f, 220.0f, kBottomH);
        BuildDefaultStudioDock(dockId, hostSize, Dp(kOutlinerW), Dp(kInspectorW), Dp(bottomH));
        studioResetLayout_ = false;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    // One dockable window per panel. The panel code draws in screen coordinates, so each gets its content rectangle.
    const auto panel = [&](const char* title, const char* id, bool opaque, bool tabBarWhenAlone, auto&& draw) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (opaque) ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.surface));
        ImGuiWindowClass cls;
        cls.DockNodeFlagsOverrideSet = tabBarWhenAlone ? 0 : ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::SetNextWindowClass(&cls);
        const std::string t = PanelTitle(title, id);
        const bool open = ImGui::Begin(t.c_str(), nullptr,
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                           ImGuiWindowFlags_NoCollapse | (opaque ? 0 : ImGuiWindowFlags_NoBackground));
        ImGui::PopStyleVar();
        if (open) {
            const ImVec2 o = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
            if (sz.x > 4.0f && sz.y > 4.0f) draw(o.x, o.y, o.x + sz.x, o.y + sz.y);
        }
        ImGui::End();
        if (opaque) ImGui::PopStyleColor();
        return open;
    };
    float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
    bool viewportOpen = false;
    viewportOpen = panel(Tr("뷰포트"), "studio_viewport", false, false, [&](float x0, float y0, float x1, float y1) {
        vx0 = x0; vy0 = y0; vx1 = x1; vy1 = y1;
        DrawStudioViewport(x0, y0, x1, y1);
    });
    if (studio_.get() != &d) return;
    panel(Tr("장면"), "studio_outliner", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioOutliner(x0, y0, x1, y1); });
    panel(Tr("속성"), "studio_inspector", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioInspector(x0, y0, x1, y1); });
    panel(Tr("타임라인"), "studio_timeline", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioTimeline(x0, y0, x1, y1); });

    DrawStudioUnsavedPrompt();
    if (studio_.get() != &d) return;  // left (or replaced) from the prompt
    DrawStudioHelp();
    DrawStudioLightPresetConfirm();
    DrawVideoRenderDialog();   // the render dialog (top bar render menu); starting it leaves for Screen::Offline
    if (screen_ != Screen::Studio) return;
    DrawToast();

    // The 3D view fills the viewport panel (back buffer pixels = ImGui display pixels).
    // PT/RT cannot render the Unlit / Wireframe shading or the quad view: the studio viewport is
    // an editing view and always renders with the raster path (LeaveStudio restores the user's
    // path; offline / video / still renders keep their own chosen renderer).
    {
        RenderSettings rs = renderer_.Settings();
        if (rs.renderPath != RenderPath::Raster) {
            rs.renderPath = RenderPath::Raster;
            renderer_.SetSettings(rs);
        }
    }
    if (viewportOpen) {
        RenderSettings rs = renderer_.Settings();
        float rr[4];
        StudioRenderRect(vx0, vy0, vx1, vy1, rr);  // the 16:9 frame while looking through the motion camera
        const uint32_t vx = (uint32_t)rr[0], vy = (uint32_t)rr[1];
        const uint32_t vw = (uint32_t)std::max(16.0f, rr[2]), vh = (uint32_t)std::max(16.0f, rr[3]);
        const ViewShading shading = (ViewShading)std::clamp(d.shading, 0, 2);
        if (rs.viewportX != vx || rs.viewportY != vy || rs.viewportW != vw || rs.viewportH != vh || rs.shading != shading) {
            rs.viewportX = vx; rs.viewportY = vy; rs.viewportW = vw; rs.viewportH = vh;
            rs.shading = shading;
            renderer_.SetSettings(rs);
        }
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
        StudioRequest(StudioAction::Leave);
        if (!studio_) return;
    }
    float x = x0 + Dp(58.0f);
    Text(dl, Font::Bold, size::Title, ImVec2(x, cy - Dp(11.0f)), p.ink, Tr("스튜디오"));
    x += TextSize(Font::Bold, size::Title, Tr("스튜디오")).x + Dp(12.0f);
    // project name (the file's stem; untitled until the first save)
    {
        const std::string title = d.projectPath.empty() ? std::string(Tr("제목 없음")) : PathToUtf8(d.projectPath.stem());
        const float maxX = std::min(x + Dp(260.0f), x1 - Dp(700.0f));
        dl->AddLine(ImVec2(x - Dp(4.0f), cy - Dp(9.0f)), ImVec2(x - Dp(4.0f), cy + Dp(9.0f)), p.line);
        x += Dp(8.0f);
        TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(x, cy - Dp(10.0f)), maxX, p.ink2, title.c_str());
        x = std::min(maxX, x + TextSize(Font::Semibold, size::Body, title.c_str()).x) + Dp(12.0f);
        if (!d.projectPath.empty() && ImGui::IsMouseHoveringRect(ImVec2(x0 + Dp(58.0f), y0), ImVec2(x, y1)))
            Tooltip(PathToUtf8(d.projectPath).c_str());
    }
    if (d.Dirty()) {
        ImVec2 bs;
        Badge(dl, ImVec2(x, cy - Dp(10.0f)), Tr("저장 안 됨"), p.warnSoft, p.warn, &bs);
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

    // right: project menu | VMD import / export | save
    const char* target = d.selectedModel < 0 ? Tr("카메라") : (d.Selected() ? d.Selected()->name.c_str() : "");
    const float vmdW = 140.0f, saveW = 104.0f;
    float rx = x1 - Dp(14.0f + saveW);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(19.0f)));
    if (Button("##save", Tr("저장"), icon::FloppyDisk, ButtonKind::Primary, ImVec2(saveW, 38.0f))) StudioSave(false);
    Tooltip(Tr("프로젝트 저장  (Ctrl+S)"));
    rx -= Dp(10.0f + vmdW);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(19.0f)));
    if (Button("##export", Tr("VMD 내보내기"), icon::Export, ButtonKind::Secondary, ImVec2(vmdW, 38.0f))) StudioExportVmd();
    Tooltip((std::string(Tr("내보낼 대상: ")) + target).c_str());
    rx -= Dp(8.0f + vmdW);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(19.0f)));
    if (Button("##import", Tr("VMD 불러오기"), icon::FolderOpen, ButtonKind::Secondary, ImVec2(vmdW, 38.0f)))
        StudioImportVmd();
    Tooltip((std::string(Tr("불러올 대상: ")) + target).c_str());
    rx -= Dp(14.0f + 36.0f);
    dl->AddLine(ImVec2(rx + Dp(36.0f + 7.0f), cy - Dp(11.0f)), ImVec2(rx + Dp(36.0f + 7.0f), cy + Dp(11.0f)), p.line);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(18.0f)));
    if (IconButton("##projmenu", icon::List, Tr("프로젝트: 새로 만들기, 열기, 다른 이름으로 저장"))) ImGui::OpenPopup("##studioproj");
    ImGui::SetNextWindowPos(ImVec2(rx + Dp(36.0f), y1 + Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    DrawStudioProjectMenu();
    if (!studio_) return;
    // render (video / still) and the shortcut list
    rx -= Dp(4.0f + 36.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(18.0f)));
    if (IconButton("##rendermenu", icon::FilmStrip, Tr("렌더: 영상, 고품질 스틸"))) ImGui::OpenPopup("##studiorender");
    ImGui::SetNextWindowPos(ImVec2(rx + Dp(36.0f), y1 + Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    DrawStudioRenderMenu();
    rx -= Dp(4.0f + 36.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(18.0f)));
    if (IconButton("##layoutreset", icon::Stack, Tr("패널 배치 초기화 (패널은 탭을 끌어서 옮기고 크기를 바꿀 수 있어요)"))) studioResetLayout_ = true;
    rx -= Dp(4.0f + 36.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(18.0f)));
    if (IconButton("##help", icon::Keyboard, Tr("단축키  (?)"), studioHelpOpen_)) studioHelpOpen_ = !studioHelpOpen_;
}

void App::DrawStudioOutliner(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x1 - 0.5f, y0), ImVec2(x1 - 0.5f, y1), p.line);
    Text(dl, Font::Semibold, size::Caption, ImVec2(x0 + Dp(16.0f), y0 + Dp(14.0f)), p.ink3, Tr("장면"));
    // "+": add a character / stage / prop / audio / song (library or file)
    ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(12.0f + 28.0f), y0 + Dp(6.0f)));
    if (IconButton("##addmodel", icon::Plus, Tr("추가: 캐릭터, 스테이지, 소품, 음원, 곡"), false, 28.0f)) {
        studioAddPage_ = 0;
        studioAddFilter_[0] = 0;
        ImGui::OpenPopup("##studioadd");
    }
    bool openAdd = false;

    ImGui::SetCursorScreenPos(ImVec2(x0, y0 + Dp(38.0f)));
    ImGui::BeginChild("##outliner", ImVec2(x1 - x0 - 1.0f, y1 - y0 - Dp(38.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float rowH = Dp(40.0f);
    int menuFor = -100;  // row whose context menu opens this frame
    auto row = [&](int index, const char* glyph, const char* label, const char* sub, bool* visible) {
        ImGui::PushID(index);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 b(a.x + w, a.y + rowH);
        const bool selected = index == -1 ? (d.selectedModel == -1 && d.selectedLightUid == 0) : (d.selectedModel == index);
        ImGui::InvisibleButton("##row", ImVec2(w - Dp(index >= 0 ? 72.0f : 40.0f), rowH));
        const bool hovered = ImGui::IsItemHovered();
        if (index >= 0 && hovered) Tooltip(PathToUtf8(d.models[index]->path).c_str());
        if (ImGui::IsItemClicked() && index > -2) StudioSelectModel(index);
        if (index >= 0 && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            studioRenameModel_ = index;
            std::snprintf(studioRenameBuf_, sizeof(studioRenameBuf_), "%s", d.models[index]->name.c_str());
        }
        if (index != -1 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) menuFor = index;
        if (index == -2 && ImGui::IsItemClicked()) menuFor = index;
        const bool rowHot = hovered || ImGui::IsMouseHoveringRect(a, b);
        const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
        if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
        else if (rowHot) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));
        const ImU32 fg = selected ? p.accentInk : p.ink;
        Icon(cdl, glyph, 16.0f, ImVec2(a.x + Dp(26.0f), a.y + rowH * 0.5f), selected ? p.accentInk : p.ink2);
        const float tx = a.x + Dp(44.0f), maxX = b.x - Dp(index >= 0 ? 78.0f : 46.0f);
        if (sub && *sub) {
            TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, label);
            TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, sub);
        } else {
            TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(11.0f)), maxX, fg, label);
        }
        if (index >= 0 && (rowHot || selected)) {
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(76.0f), a.y + Dp(6.0f)));
            if (IconButton("##more", icon::DotsThree, Tr("모델 메뉴"), false, 28.0f)) menuFor = index;
        }
        if (index == -1) {  // possess the camera (C4D): navigating the viewport then edits this camera
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
            if (IconButton("##possess", icon::Crosshair,
                           d.possessCamera ? Tr("카메라 빙의 해제  (Esc)") : Tr("카메라 빙의: 뷰포트 조작이 이 카메라를 움직여요"),
                           d.possessCamera, 28.0f))
                StudioPossess(!d.possessCamera);
        }
        if (visible) {
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
            if (IconButton("##vis", *visible ? icon::Eye : icon::EyeSlash, *visible ? Tr("숨기기") : Tr("보이기"), false, 28.0f)) {
                *visible = !*visible;
                ++d.projectVersion;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::PopID();
    };

    char sub[160];
    std::snprintf(sub, sizeof(sub), Tr("키 %d개"),
                  (int)(d.camera.camera.size() + d.camera.light.size() + d.camera.shadow.size()));
    row(-1, icon::VideoCamera, Tr("카메라 VMD"), sub, nullptr);
    DrawStudioLightOutliner();
    // characters first (they are what gets animated), then props, then stage parts
    for (int pass = 0; pass < 3; ++pass) {
        const ModelKind kind = pass == 0 ? ModelKind::Character : pass == 1 ? ModelKind::Prop : ModelKind::Stage;
        for (int i = 0; i < (int)d.models.size(); ++i) {
            StudioModel& m = *d.models[i];
            if (m.kind != kind) continue;
            size_t keys = 0;
            for (const auto& [n, k] : m.motion.bones) keys += k.size();
            for (const auto& [n, k] : m.motion.morphs) keys += k.size();
            if (m.IsStage()) {
                std::snprintf(sub, sizeof(sub), "%s", Tr("스테이지"));
            } else if (m.IsProp()) {
                const int pi = m.attach.parent >= 0 ? d.IndexOfUid((uint32_t)m.attach.parent) : -1;
                if (pi >= 0)
                    std::snprintf(sub, sizeof(sub), "%s · %s%s%s", Tr("소품"), d.models[pi]->name.c_str(),
                                  m.attach.bone.empty() ? "" : " / ", m.attach.bone.c_str());
                else
                    std::snprintf(sub, sizeof(sub), "%s", Tr("소품 · 월드"));
            } else {
                std::snprintf(sub, sizeof(sub), Tr("키 %d개"), (int)keys);
            }
            const char* glyph = m.IsStage() ? icon::Mountains : m.IsProp() ? icon::Cube : icon::PersonSimple;
            row(i, glyph, m.name.c_str(), sub, &m.visible);
        }
    }
    // the audio track last (offset / replace / remove from its menu)
    if (d.hasAudio) {
        const std::string name = PathToUtf8(d.audioPath.filename());
        if (std::fabs(d.audioOffset) > 1e-4) std::snprintf(sub, sizeof(sub), Tr("음원 · 오프셋 %+.2f초"), d.audioOffset);
        else std::snprintf(sub, sizeof(sub), "%s", Tr("음원"));
        row(-2, icon::MusicNotes, name.c_str(), sub, nullptr);
    }
    // loads in progress
    for (const auto& job : studioJobs_) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float t = (float)ImGui::GetTime();
        // spinning notch
        const ImVec2 c(a.x + Dp(26.0f), a.y + rowH * 0.5f);
        cdl->PathArcTo(c, Dp(6.5f), t * 6.0f, t * 6.0f + 4.2f, 16);
        cdl->PathStroke(p.accent, 0, Dp(2.0f));
        TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(a.x + Dp(44.0f), a.y + Dp(4.0f)), a.x + w - Dp(14.0f), p.ink2,
                     job->label.c_str());
        ProgressBar(cdl, ImVec2(a.x + Dp(44.0f), a.y + Dp(26.0f)), ImVec2(a.x + w - Dp(16.0f), a.y + Dp(30.0f)),
                    job->progress.fraction.load(std::memory_order_relaxed));
        ImGui::Dummy(ImVec2(w, rowH));
    }
    // empty project: say what to do next
    if (d.models.empty() && studioJobs_.empty()) {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 a(ImGui::GetCursorScreenPos().x + Dp(16.0f), ImGui::GetCursorScreenPos().y + Dp(14.0f));
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        Text(cdl, Font::Semibold, size::Small, a, p.ink2, Tr("모델이 없어요"));
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + Dp(24.0f)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - Dp(32.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        PushFont(Font::Regular, size::Caption);
        ImGui::TextWrapped("%s", Tr("캐릭터, 스테이지, 소품, 음원은 ＋ 버튼으로 라이브러리나 파일에서 추가해요."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(a.x, ImGui::GetCursorScreenPos().y + Dp(10.0f)));
        if (Button("##emptyadd", Tr("캐릭터 추가"), icon::Plus, ButtonKind::Secondary, ImVec2((w - Dp(32.0f)) / Dpi(), 36.0f))) {
            openAdd = true;
            studioAddPage_ = 1;
        }
    }
    if (menuFor != -100) ImGui::OpenPopup("##modelmenu");
    static int menuModel = -1;  // the row whose menu is open (one popup at a time)
    if (menuFor != -100) menuModel = menuFor;
    DrawStudioModelMenu(menuModel);
    ImGui::EndChild();
    if (openAdd) {
        studioAddFilter_[0] = 0;
        ImGui::OpenPopup("##studioadd");
    }
    ImGui::SetNextWindowPos(ImVec2(x1 - Dp(12.0f + 28.0f), y0 + Dp(38.0f)), ImGuiCond_Appearing);
    DrawStudioAddMenu();
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
        if (d.selectedLightUid != 0) {
            DrawStudioLightInspector(w);
            ImGui::EndChild();
            return;
        }
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const StudioModel* m = d.Selected();
        TextEllipsis(cdl, Font::Semibold, size::Body, c, c.x + w, p.ink, m ? m->name.c_str() : Tr("카메라 VMD"));
        ImGui::Dummy(ImVec2(w, Dp(28.0f)));
        if (m) {
            line(Tr("본 / 모프"), std::to_string(m->pmx->bones.size()) + " / " + std::to_string(m->pmx->morphs.size()));
            if (m->IsProp()) DrawStudioPropPanel(w);
            else DrawStudioPlacePanel(w);
            if (m->kind == ModelKind::Character) DrawStudioShaderRow(w);
        } else {
            char counts[96];
            std::snprintf(counts, sizeof(counts), Tr("카메라 %d · 조명 %d · 섀도 %d"), (int)d.camera.camera.size(),
                          (int)d.camera.light.size(), (int)d.camera.shadow.size());
            line(Tr("키"), counts);
            DrawStudioCameraPanel(w);
        }
    }
    // characters: keys / bone (pose) / morph tabs
    if (StudioPoseModel()) {
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        const char* tabs[] = {Tr("키"), Tr("본"), Tr("모프")};
        const char* tabIcons[] = {icon::Diamond, icon::Bone, icon::Sliders};
        d.inspectorTab = std::clamp(d.inspectorTab, 0, 2);
        Segmented("##inspectortab", tabs, 3, &d.inspectorTab, w / Dpi(), 32.0f, tabIcons);
        ImGui::Dummy(ImVec2(w, Dp(10.0f)));
        if (d.inspectorTab == 1 || d.inspectorTab == 2) {
            if (d.inspectorTab == 1) DrawStudioBoneTab(w);
            else DrawStudioMorphTab(w);
            ImGui::EndChild();
            return;
        }
    } else if (d.selectedModel >= 0) {  // the camera panel ends with its own separator
        ImGui::Dummy(ImVec2(w, Dp(8.0f)));
        cdl->AddLine(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y), p.line);
        ImGui::Dummy(ImVec2(w, Dp(12.0f)));
    }

    // the first selected key decides what is shown; curve edits apply to every selected key of that kind
    const KeyId* first = nullptr;
    RowKind kind = RowKind::Bone;
    std::string name;
    for (const KeyId& k : d.selection) {
        if (StudioTrackOfRow(k.first, kind, name)) { first = &k; break; }
    }
    if (!first) {
        if (!studioKeyLive_) StudioEndKeyEdit();  // the edited key is gone (deleted / deselected mid-edit)
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
    const SceneLight* sceneLight = kind == RowKind::SceneLight ? d.FindLight(LightUidOfTrack(name)) : nullptr;
    const char* trackName = kind == RowKind::Camera ? Tr("카메라") : kind == RowKind::Light ? Tr("조명") : Tr("셀프 섀도");
    line(IsCameraKind(kind) || kind == RowKind::SceneLight ? Tr("트랙") : (kind == RowKind::Bone ? Tr("본") : Tr("모프")),
         kind == RowKind::SceneLight ? (sceneLight ? sceneLight->name : std::string())
                                     : IsCameraKind(kind) ? std::string(trackName) : name);
    line(Tr("프레임"), std::to_string(first->second));
    if (IsCameraKind(kind)) {
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        // the camera key under the playhead is edited by the "camera values" section above (no second copy)
        const bool shownAbove = kind == RowKind::Camera && first->second == d.Frame();
        if (!shownAbove && DrawStudioCameraKeyFields(w, kind, first->second)) {  // light / shadow: no curves
            ImGui::EndChild();
            return;
        }
    }
    if (kind == RowKind::SceneLight) {  // light keys have no curve editor; their values are shown, not edited, here
        const LightKey* k = sceneLight ? FindKey(sceneLight->keys, first->second) : nullptr;
        if (k) {
            const LightValues& v = k->v;
            if (sceneLight->kind == LightKind::Sun) {
                std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", v.direction.x, v.direction.y, v.direction.z);
                line(Tr("방향"), buf);
            } else {
                std::snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f", v.position.x, v.position.y, v.position.z);
                line(Tr("위치"), buf);
            }
            std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", v.color.x, v.color.y, v.color.z);
            line(Tr("색"), buf);
            std::snprintf(buf, sizeof(buf), "%.2f", v.intensity);
            line(Tr("강도"), buf);
            if (sceneLight->kind == LightKind::Spot) {
                std::snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f", v.aim.x, v.aim.y, v.aim.z);
                line(Tr("조준"), buf);
                std::snprintf(buf, sizeof(buf), "%.1f°", DirectX::XMConvertToDegrees(v.coneOuter));
                line(Tr("원뿔 각도"), buf);
            }
        }
        ImGui::EndChild();
        return;
    }

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
    uint8_t channelCurves[6][4] = {};  // every channel of the first key: the other ones are drawn behind the edited one
    for (auto& c : channelCurves) c[0] = c[1] = 20, c[2] = c[3] = 107;
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
            for (int ch = 0; ch < 4; ++ch) GetBoneCurve(k->interp, ch, channelCurves[ch]);
        }
    } else {
        const CameraKf* k = FindKey(d.camera.camera, first->second);
        if (k) {
            d.curveChannel = std::clamp(d.curveChannel, 0, 5);
            GetCameraCurve(k->interp, d.curveChannel, curve);
            for (int ch = 0; ch < 6; ++ch) GetCameraCurve(k->interp, ch, channelCurves[ch]);
        }
    }

    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("보간 곡선"));
        // copy the first selected key's curves / paste them onto every selected key of the same kind
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(3.0f * 28.0f + 8.0f), c.y - Dp(6.0f)));
        if (IconButton("##curveall", icon::Stack,
                       d.curveAllChannels ? Tr("모든 채널에 적용 중 (클릭: 선택한 채널만)") : Tr("선택한 채널에만 적용 (클릭: 모든 채널)"),
                       d.curveAllChannels, 28.0f))
            d.curveAllChannels = !d.curveAllChannels;
        ImGui::SameLine(0, Dp(4.0f));
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

    const float plot = std::min(w, Dp(168.0f));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (w - plot) * 0.5f, ImGui::GetCursorScreenPos().y));
    const int channels = kind == RowKind::Bone ? 4 : 6;
    uint8_t ghosts[5][4];
    ImU32 ghostColors[5];
    int ghostCount = 0;
    for (int ch = 0; ch < channels; ++ch) {
        if (ch == d.curveChannel) continue;
        std::memcpy(ghosts[ghostCount], channelCurves[ch], 4);
        ghostColors[ghostCount++] = WithAlpha(BezierChannelColor(ch), 0.55f);
    }
    const bool changed = BezierCurveEditor("##curve", curve, plot / Dpi(), ghosts, ghostCount, ghostColors);
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
            // the edited channel, or every channel when "all channels" is on (MMD's curve copy to all)
            const int c0 = d.curveAllChannels ? 0 : d.curveChannel, c1 = d.curveAllChannels ? channels : d.curveChannel + 1;
            if (kk == RowKind::Bone) {
                if (BoneKf* b = FindKey(m->motion.bones[nn], k.second))
                    for (int ch = c0; ch < c1; ++ch) SetBoneCurve(b->interp, ch, curve);
            } else if (kk == RowKind::Camera) {
                if (CameraKf* c = FindKey(d.camera.camera, k.second))
                    for (int ch = c0; ch < c1; ++ch) SetCameraCurve(c->interp, ch, curve);
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
    ImGui::Dummy(ImVec2(w, Dp(16.0f)));  // the panel scrolls: keep the last row clear of the bottom edge
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
    ImGui::SameLine(0, Dp(2.0f));
    if (IconButton("##autokey", icon::Diamond, d.autoKey ? Tr("자동 키 켬: 값을 고치면 재생 헤드에 키가 생겨요") : Tr("자동 키 끔: I 키로 직접 등록해요"),
                   d.autoKey, 34.0f))
        d.autoKey = !d.autoKey;
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
    if (d.scrollToRow > 0) {
        // bring a row picked elsewhere (viewport bone) into view
        --d.scrollToRow;
        for (size_t i = 0; i < d.rows.size(); ++i) {
            if (d.rows[i].id != d.scrollRowId) continue;
            const float rowH = Dp(24.0f), top = (float)i * rowH;
            const float visibleH = (y1 - ty) - Dp(28.0f) - Dp(10.0f);
            if (top < d.view.scrollY) d.view.scrollY = top;
            else if (top + rowH > d.view.scrollY + visibleH) d.view.scrollY = top + rowH - visibleH;
            d.scrollToRow = 0;
            break;
        }
    }
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

// Viewport navigation (Unity / Unreal style): RMB look + WASDQE fly (wheel = speed), Alt+LMB or LMB orbit, MMB pan,
// Alt+RMB dolly, wheel zoom. The fly keys only count while the right button is held on the viewport.
// Possessing the camera (C4D style) sends the same gestures to the motion camera's key at the playhead instead of the
// free camera: orbit / pan / fly move the real camera, one undo step per gesture.
void App::StudioViewportNavigate(bool hovered, bool active) {
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const bool possessed = d.possessCamera;
    const bool rmb = active && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const auto keyDown = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
    const float fwdIn = rmb && !io.KeyAlt ? (float)keyDown(ImGuiKey_W) - (float)keyDown(ImGuiKey_S) : 0.0f;
    const float rightIn = rmb && !io.KeyAlt ? (float)keyDown(ImGuiKey_D) - (float)keyDown(ImGuiKey_A) : 0.0f;
    const float upIn = rmb && !io.KeyAlt ? (float)(keyDown(ImGuiKey_E) || keyDown(ImGuiKey_Space)) - (float)keyDown(ImGuiKey_Q) : 0.0f;
    const bool flying = fwdIn != 0.0f || rightIn != 0.0f || upIn != 0.0f;
    const bool dragging = active && studioViewDrag_ == 1 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
    const bool input = dragging || flying || (hovered && io.MouseWheel != 0);
    FreeCamera possCam;
    if (input && possessed) {
        if (d.playing) StudioSetPlaying(false);  // the edit belongs to one frame
        possCam = StudioViewedFree(d.Frame());
    } else if (input) {
        StudioTakeFreeCamera();
    }
    FreeCamera& cam = possessed ? possCam : freeCam_;
    // a real camera often sits on its target (distance ~ 0): pan / dolly then follow a floor of 3 units
    const float minDist = possessed ? 0.1f : 2.0f;
    const float reach = possessed ? std::max(cam.distance, 3.0f) : cam.distance;
    if (dragging) {
        const ImVec2 delta = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            cam.yaw += delta.x * 0.006f;
            cam.pitch = std::clamp(cam.pitch + delta.y * 0.006f, -1.45f, 1.45f);
        } else if (rmb && io.KeyAlt) {  // dolly
            if (possessed) {
                const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
                const float step = reach * (delta.x - delta.y) * 0.006f;
                cam.target.x += -sy * cp * step;
                cam.target.y += -sp * step;
                cam.target.z += cy * cp * step;
            } else {
                cam.distance = std::clamp(cam.distance * std::exp((delta.x - delta.y) * 0.006f), minDist, 600.0f);
            }
        } else if (rmb) {  // look around the eye: the target follows
            const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
            const DirectX::XMFLOAT3 eye{cam.target.x + sy * cp * cam.distance, cam.target.y + sp * cam.distance,
                                        cam.target.z - cy * cp * cam.distance};
            cam.yaw -= delta.x * 0.004f;
            cam.pitch = std::clamp(cam.pitch + delta.y * 0.004f, -1.45f, 1.45f);
            const float sy2 = std::sin(cam.yaw), cy2 = std::cos(cam.yaw), sp2 = std::sin(cam.pitch), cp2 = std::cos(cam.pitch);
            cam.target = {eye.x - sy2 * cp2 * cam.distance, eye.y - sp2 * cam.distance, eye.z + cy2 * cp2 * cam.distance};
        } else {  // pan
            const float rx = std::cos(cam.yaw), rz = std::sin(cam.yaw);
            const float scale = reach * 0.0015f;
            cam.target.x += (-delta.x * rx) * scale;
            cam.target.y += delta.y * scale;
            cam.target.z += (-delta.x * rz) * scale;
        }
    }
    if (flying) {
        const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
        const float fx = -sy * cp, fy = -sp, fz = cy * cp;  // eye -> target
        const float speed = 30.0f * studioFlyMul_ * (io.KeyShift ? 3.0f : 1.0f) * io.DeltaTime;
        cam.target.x += (fx * fwdIn + cy * rightIn) * speed;
        cam.target.y += (fy * fwdIn + upIn) * speed;
        cam.target.z += (fz * fwdIn + sy * rightIn) * speed;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        if (rmb) studioFlyMul_ = std::clamp(studioFlyMul_ * std::pow(1.2f, io.MouseWheel), 0.1f, 20.0f);  // fly speed
        else if (possessed) {  // dolly the camera itself: eye and target move together, the lens distance stays
            const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
            const float step = reach * 0.12f * io.MouseWheel;
            cam.target.x += -sy * cp * step;
            cam.target.y += -sp * step;
            cam.target.z += cy * cp * step;
        } else cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel), minDist, 600.0f);
    }
    if (possessed && input) StudioWriteCamera(cam);
    StudioNavEditTick();
}

void App::StudioOrthoNavigate(const ViewProj& vp, bool hovered, bool active) {
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    // each ortho view has its own navigation state; the view under the mouse (studioActiveView_)
    // drives the one being edited (this runs only when an ortho view is active, av 1..3)
    const int v = std::clamp(studioActiveView_ - 1, 0, 2);
    // pan: right / middle (or an empty-space left) drag moves the view's own look-at point in its plane
    const bool dragging = active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) &&
                          (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                           (ImGui::IsMouseDown(ImGuiMouseButton_Left) && studioViewDrag_ == 1));
    if (dragging) {
        const float s = vp.orthoHeight / std::max(1.0f, vp.h);
        const XMFLOAT4X4& m = vp.view;
        const XMFLOAT3 right{m._11, m._21, m._31}, up{m._12, m._22, m._32};  // view axes in world space (row-vector view)
        d.quadCenter[v].x += (-io.MouseDelta.x * right.x + io.MouseDelta.y * up.x) * s;
        d.quadCenter[v].y += (-io.MouseDelta.x * right.y + io.MouseDelta.y * up.y) * s;
        d.quadCenter[v].z += (-io.MouseDelta.x * right.z + io.MouseDelta.y * up.z) * s;
    }
    if (hovered && io.MouseWheel != 0.0f) d.quadHeight[v] = std::clamp(d.quadHeight[v] * std::pow(0.88f, io.MouseWheel), 2.0f, 4000.0f);
}

void App::StudioAddQuadViews(FrameView& view) const {
    const StudioDoc& d = *studio_;
    if (d.viewLayout != 1) return;
    const float rects[3][4] = {{0.5f, 0.0f, 0.5f, 0.5f}, {0.0f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};  // top, front, left
    view.mainRect[0] = 0.0f; view.mainRect[1] = 0.0f; view.mainRect[2] = 0.5f; view.mainRect[3] = 0.5f;
    for (int k = 0; k < 3; ++k) {
        ExtraView ev;
        std::copy(rects[k], rects[k] + 4, ev.rect);
        OrthoViewMatrix(k, d.quadCenter[k], &ev.view, nullptr);
        ev.height = d.quadHeight[k];
        view.extraViews.push_back(ev);
    }
}

void App::DrawStudioViewport(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    // camera of this frame: overlays, picking and the gizmo project with it
    CameraParams cp;
    StudioCamera(cp);
    float rr[4];
    StudioRenderRect(x0, y0, x1, y1, rr);
    const bool quad = d.viewLayout == 1;
    // views: 0 perspective camera, then (quad) top / front / left. Each has its rectangle and its projection.
    struct QuadView { float x0, y0, x1, y1; ViewProj vp; };
    QuadView views[4];
    int viewCount = 1;
    if (!quad) {
        views[0] = {rr[0], rr[1], rr[0] + rr[2], rr[1] + rr[3],
                    MakeViewProj(cp.view, cp.eye, cp.fovYRadians, cp.nearZ, cp.farZ, rr[0], rr[1], std::max(1.0f, rr[2]), std::max(1.0f, rr[3]))};
    } else {
        viewCount = 4;
        const float mx = std::floor((x0 + x1) * 0.5f), my = std::floor((y0 + y1) * 0.5f);
        const float xs[3] = {x0, mx, x1}, ys[3] = {y0, my, y1};
        const int cells[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};  // perspective, top, front, left
        for (int k = 0; k < 4; ++k) {
            QuadView& q = views[k];
            q.x0 = xs[cells[k][0]]; q.x1 = xs[cells[k][0] + 1];
            q.y0 = ys[cells[k][1]]; q.y1 = ys[cells[k][1] + 1];
            const float w = std::max(1.0f, q.x1 - q.x0), h = std::max(1.0f, q.y1 - q.y0);
            if (k == 0) {
                q.vp = MakeViewProj(cp.view, cp.eye, cp.fovYRadians, cp.nearZ, cp.farZ, q.x0, q.y0, w, h);
            } else {
                XMFLOAT4X4 view;
                XMFLOAT3 eye;
                OrthoViewMatrix(k - 1, d.quadCenter[k - 1], &view, &eye);
                q.vp = MakeOrthoViewProj(view, eye, d.quadHeight[k - 1], 0.1f, 2.0f * kOrthoEyeDistance, q.x0, q.y0, w, h);
            }
        }
    }
    const auto viewAt = [&](ImVec2 m) {
        for (int k = 0; k < viewCount; ++k)
            if (m.x >= views[k].x0 && m.x < views[k].x1 && m.y >= views[k].y0 && m.y < views[k].y1) return k;
        return 0;
    };
    if (!quad) studioActiveView_ = 0;
    studioVp_ = views[std::clamp(studioActiveView_, 0, viewCount - 1)].vp;

    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::SetNextItemAllowOverlap();  // the toolbar buttons drawn on top take the hover
    ImGui::InvisibleButton("##viewport", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated(), deactivated = ImGui::IsItemDeactivated();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    // the view under the mouse is the active one for a gesture (locked while a press is held)
    if (quad && (activated || (studioViewDrag_ == 0 && !active))) {
        studioActiveView_ = viewAt(io.MousePos);
        studioVp_ = views[studioActiveView_].vp;
    }
    const int av = std::clamp(studioActiveView_, 0, viewCount - 1);
    const QuadView& act = views[av];

    // a left press goes to the gizmo or a bone first, else it orbits; right/middle always move the camera
    if (activated) {
        studioPressPos_ = io.MousePos;
        studioPressMoved_ = false;
        studioViewDrag_ = 1;
    }
    bool consumed = false;
    StudioViewportPose(act.x0, act.y0, act.x1, act.y1, hovered || active, consumed);
    if (av == 0) {
        StudioViewportCameraHandles(hovered || active);
        // camera target: a click on a key dot of the camera path selects that key (no orbit)
        if (studioViewDrag_ == 1 && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && StudioPickCameraKey(io.MousePos))
            studioViewDrag_ = 4;
    }
    {
        // the camera path / frustum in the perspective view, the frame guides when there is one view
        const ViewProj saved = studioVp_;
        studioVp_ = views[0].vp;
        DrawStudioCameraPath(views[0].x0, views[0].y0, views[0].x1, views[0].y1);
        studioVp_ = saved;
    }
    if (!quad) DrawStudioFrameMask(x0, y0, x1, y1);
    if (active &&(std::fabs(io.MousePos.x - studioPressPos_.x) > Dp(3.0f) || std::fabs(io.MousePos.y - studioPressPos_.y) > Dp(3.0f)))
        studioPressMoved_ = true;
    if (deactivated) {
        // a click (no drag) on empty space clears the bone selection
        if (studioViewDrag_ == 1 && !studioPressMoved_ && io.MouseReleased[0] && StudioPoseModel() && !d.selectedBones.empty())
            StudioSelectBone(-1, false);
        studioViewDrag_ = 0;
    }

    if (av == 0) StudioViewportNavigate(hovered, active);
    else StudioOrthoNavigate(act.vp, hovered, active);
    (void)consumed;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (quad) {
        // the other views show the bones and the gizmo too; labels and the separators
        for (int k = 0; k < viewCount; ++k)
            if (k != av) StudioDrawPoseOverlay(views[k].vp, views[k].x0, views[k].y0, views[k].x1, views[k].y1);
        const Palette& pp = P();
        const float gx = views[1].x0, gy = views[2].y0;
        dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), pp.lineStrong, 1.0f);
        dl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), pp.lineStrong, 1.0f);
        const char* names[4] = {Tr("원근"), Tr("상단"), Tr("정면"), Tr("좌측")};
        for (int k = 1; k < viewCount; ++k) {
            ImVec2 sz;
            Badge(dl, ImVec2(views[k].x0 + Dp(10.0f), views[k].y0 + Dp(10.0f)), names[k], WithAlpha(pp.surface, 0.85f), pp.ink2, &sz);
        }
    }

    // view label (top left of the viewport) and the pose toolbar next to it, over the bone overlay
    const Palette& p = P();
    const char* label = d.possessCamera ? Tr("카메라 빙의 중") : d.useMotionCamera && d.cameraEval ? Tr("카메라 모션") : Tr("자유 카메라");
    ImVec2 bs;
    Badge(dl, ImVec2(x0 + Dp(12.0f), y0 + Dp(12.0f)), label, d.possessCamera ? p.accent : WithAlpha(p.surface, 0.85f),
          d.possessCamera ? p.onAccent : p.ink2, &bs);
    StudioViewportToolbar(x0 + Dp(12.0f) + bs.x + Dp(10.0f), y0 + Dp(12.0f) + bs.y * 0.5f);
    {
        // viewport shading, top right: solid / unlit / wireframe (the ray-traced paths always show lit)
        const char* modes[] = {Tr("솔리드"), Tr("언릿"), Tr("와이어")};
        const float segW = 210.0f, layW = 128.0f;
        ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(segW) - Dp(12.0f), y0 + Dp(8.0f)));
        Segmented("##shading", modes, 3, &d.shading, segW, 30.0f);
        // one view or the four-way split (top / front / left are orthographic and always drawn flat)
        const char* layouts[] = {Tr("단일"), Tr("4분할")};
        ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(segW) - Dp(12.0f) - Dp(layW) - Dp(8.0f), y0 + Dp(8.0f)));
        Segmented("##viewlayout", layouts, 2, &d.viewLayout, layW, 30.0f);
    }
    if (d.models.empty() && studioJobs_.empty()) {
        const char* t1 = Tr("빈 프로젝트");
        const char* t2 = Tr("왼쪽 위의 ＋ 버튼으로 캐릭터와 스테이지를 추가하세요");
        const ImVec2 s1 = TextSize(Font::Semibold, size::Title, t1), s2 = TextSize(Font::Regular, size::Small, t2);
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        const float bw = std::max(s1.x, s2.x) + Dp(48.0f), bh = s1.y + s2.y + Dp(40.0f);
        dl->AddRectFilled(ImVec2(cx - bw * 0.5f, cy - bh * 0.5f), ImVec2(cx + bw * 0.5f, cy + bh * 0.5f),
                          WithAlpha(p.surface, 0.82f), Dp(14.0f));
        Text(dl, Font::Semibold, size::Title, ImVec2(cx - s1.x * 0.5f, cy - bh * 0.5f + Dp(16.0f)), p.ink, t1);
        Text(dl, Font::Regular, size::Small, ImVec2(cx - s2.x * 0.5f, cy - bh * 0.5f + Dp(24.0f) + s1.y), p.ink2, t2);
    }
}

} // namespace mmdx
