// Studio screen: a keyframe editor for a multi-model scene (outliner, viewport, inspector, timeline).
// Document model and undo live in src/studio; this file wires them to the App (loading, renderer, audio, input).
#include "app/App.h"
#include "ui_probe/UiProbe.h"

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
#include "asset/RestPose.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "render/MmeSemantics.h"
#include "studio/FileDialog.h"
#include "studio/RestPoseRetarget.h"
#include "studio/StudioPose.h"
#include "studio/UiBezier.h"

namespace mmdx {

using namespace studio;

namespace {
constexpr float kOutlinerW = 236.0f, kInspectorW = 312.0f, kBottomH = 300.0f, kTransportH = 46.0f;
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
    k.fovDeg = std::clamp(fov, 1.0f, 125.0f);
    k.perspective = true;
}

// S3: stage param track helpers (defined with the other row-name helpers further down; both unnamed namespace blocks
// are the same namespace, so these declarations are enough for the inspector panel above them).
EffectParamTrack* FindStageParamTrack(studio::StudioStage& stage, const std::string& param);
const EffectParamTrack* FindStageParamTrack(const studio::StudioStage& stage, const std::string& param);
std::string StageParamName(const StageParam& sp, int comp);
bool StageParamTrackStepped(const studio::StudioStage& stage, const std::string& param);
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
    const SongAsset so = hasSong ? SongAsChosen(*song) : SongAsset{};
    loadFuture_ = std::async(std::launch::async, [=, pkg = studioPackage_.get(), this] {
        const std::string variant =
            hasStage ? ResolveStageVariant(std::string(), settings_.StageVariant(s.id), options_.stageVariant)
                     : std::string();
        return LoadStudioPackage(c, hasStage ? &s : nullptr, hasSong ? &so : nullptr, *pkg, &loadProgress_,
                                 &loadError_, MaterialChoiceResolver(), variant);
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
        sm->defaultPackTextureFolder = m.defaultPackTextureFolder;
        sm->kind = m.kind;
        sm->visible = m.visible;
        sm->physicsOff = m.physicsOff;
        sm->attach = m.attach;
        sm->place = sm->placeApplied = m.place;
        sm->shader = m.shader;
        sm->material = m.material;
        sm->mme = m.mme;
        sm->stageManifest = m.stageManifest;
        sm->stagePart = m.stagePart;
        sm->stageUid = m.stageUid;
        if (m.stageManifest && m.pmx)
            sm->stageBinding = MatchStageMaterialRules(m.stageManifest->materials, *m.pmx);
        sm->uid = doc->nextUid++;
        sm->fx = m.fx;   // M5c: effect objects carry their `.fx`
        if (m.pmx) {
            sm->path = m.pmx->sourcePath;
            sm->pmx = m.pmx;
            sm->inst = std::make_unique<ModelInstance>(m.pmx);
            // props follow bones every frame: they need the per-frame (character) RT path, not the static stage BLAS
            sm->gpu = renderer_.CreateModel(batch, *m.pmx, m.textures,
                                            m.kind == ModelKind::Stage ? ModelRole::Stage : ModelRole::Character, &m.pbr,
                                            &m.compressedTextures);
            if (!sm->gpu) {
                if (m.kind != ModelKind::Character || pkg.fromProject) {
                    LOG_WARN("studio: GPU upload failed: %s", m.name.c_str());
                    continue;
                }
                return false;
            }
            sm->gpu->SetDefaultPackTextureFolder(m.defaultPackTextureFolder);
            sm->gpu->SetMaterialBlendHints(m.materialBlend, m.materialBlendDefault);
            // S6: the part's bundled surface pack (resolved once; the parameters follow the timeline every frame)
            if (m.stageManifest) sm->stageSurfacePack = ResolveStageSurfacePack(*m.stageManifest, *sm->gpu);
            if (m.kind == ModelKind::Prop) sm->gpu->SetProp(true);   // the world's ground map includes the props
            if (!pkg.fromProject) StudioSuggestShader(*sm);   // added character with no shader choice: maybe suggest a pack
            sm->inst->UpdatePose();
        }
        sm->motion = std::move(m.motion);
        sm->BuildRowGroups();
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
        // accessory keys are saved as 1 + the package model index (0 = the world): back to the model's uid
        for (AccessoryKf& k : m->motion.accessory) {
            if (k.parentUid == 0) continue;
            const size_t pi = k.parentUid - 1;
            const int ki = pi < remap.size() ? remap[pi] : -1;
            k.parentUid = ki >= 0 ? doc->models[(size_t)ki]->uid : 0;
        }
    }

    // S3: the project's MMDX stage entities (their uids are saved: part models / lights refer to them). Assigned before
    // the consolidation, so a stage's own parts join it instead of becoming a second entity.
    doc->stages = pkg.stages;
    doc->nextStageUid = 1;
    for (auto& stage : doc->stages) doc->nextStageUid = std::max(doc->nextStageUid, stage.uid + 1);
    // S3: one stage entity per MMDX stage (a library scene / a version <= 3 project: its parts share a manifest, the
    // variant came per part)
    std::map<uint32_t, std::string> legacyVariants;
    for (size_t i = 0; i < pkg.models.size() && i < remap.size(); ++i) {
        const StudioPackageModel& m = pkg.models[i];
        if (remap[i] < 0 || !m.stageManifest || m.stageVariant.empty()) continue;
        legacyVariants[(uint32_t)doc->models[(size_t)remap[i]]->uid] = m.stageVariant;
    }
    StudioConsolidateStages(*doc, legacyVariants);
    // S6: the stages' bundled packs join the registry ("stage:<id>/<folder>"); a broken one is one [W]. A project's
    // entities may hold their manifest without a model part, so every entity is asked.
    for (const studio::StudioStage& stage : doc->stages)
        if (stage.manifest) RegisterStagePacks(*stage.manifest);
    doc->camera = std::move(pkg.camera);
    // S4: the extra cameras (a version <= 4 project / a library scene: none). They feed the stage screens; the main
    // camera alone is the output (the loader already flattened an old project's switch track into it).
    doc->cameras = std::move(pkg.cameras);
    doc->nextCameraUid = 1;
    for (const studio::StudioCamera& c : doc->cameras) doc->nextCameraUid = std::max(doc->nextCameraUid, c.uid + 1);
    doc->selectedCameraUid = 0;
    // a light track that only repeats MMD's defaults says nothing (and would override the lighting): dropped, as in play mode
    if (IsDefaultLightTrack(doc->camera.light)) doc->camera.light.clear();
    doc->audioOffset = pkg.audioOffset;
    doc->audioLocked = pkg.audioLocked;
    doc->bgVideoPath = pkg.bgVideoPath;
    doc->bgVideoOffset = pkg.bgVideoOffset;
    doc->bgVideoAsBackground = pkg.bgVideoAsBackground;
    doc->physicsEnv = pkg.physicsEnv;  // a project's gravity / wind (a new scene: the defaults)
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
        doc->useMotionCamera = e.useMotionCamera && doc->HasCameraKeys();  // S4: an extra camera's keys count too
        doc->lights = e.lights;
        for (const SceneLight& l : doc->lights) doc->nextLightUid = std::max(doc->nextLightUid, l.uid + 1);
        // spot targets are saved as 1 + the project's model index (package index here): back to the model's uid
        for (SceneLight& l : doc->lights) {
            if (l.targetUid == 0) continue;
            const size_t pi = l.targetUid - 1;
            const int di = pi < remap.size() ? remap[pi] : -1;
            l.targetUid = di >= 0 ? doc->models[(size_t)di]->uid : 0;
        }
        for (FocusKf& k : doc->camera.focus) {  // focus targets: 1 + the project's model index -> the model's uid
            if (k.target == 0) continue;
            const size_t pi = k.target - 1;
            const int di = pi < remap.size() ? remap[pi] : -1;
            k.target = di >= 0 ? doc->models[(size_t)di]->uid : 0;
        }
        doc->useShadowTrack = e.useShadowTrack;
        doc->aspectW = e.aspectW;
        doc->aspectH = e.aspectH;
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
        doc->mmePost = pkg.mmePost;          // M6b: the project's own MME post stack
        doc->effects = pkg.effects;
        doc->nextEffectUid = 1;
        for (auto& eff : doc->effects) eff.uid = doc->nextEffectUid++;
        doc->worlds = pkg.worlds;
        doc->nextWorldUid = 1;
        for (auto& world : doc->worlds) world.uid = doc->nextWorldUid++;
    } else if (pkg.fromPmm) {
        // PMM import: a NEW unsaved project. Camera motion when the PMM had camera keys; the PMM has
        // no scene lights, so the Studio preset is the default (a PMM light track overrides it).
        doc->useMotionCamera = doc->HasCameraKeys() && !options_.freeCamera;
        doc->lights = PresetLights(0, DirectX::XMFLOAT3{0.0f, 10.0f, 0.0f}, doc->nextLightUid);
        doc->selectedModel = -1;
        for (int i = 0; i < (int)doc->models.size(); ++i)
            if (doc->models[(size_t)i]->kind == ModelKind::Character) { doc->selectedModel = i; break; }
        doc->time = options_.seekSeconds > 0 ? options_.seekSeconds
                                             : (float)std::max(0, pkg.editor.rangeStart) / (double)kMmdFps;
        doc->view.rangeStart = pkg.editor.rangeStart;
        doc->view.rangeEnd = pkg.editor.rangeEnd;
        // Frame aspect from the PMM output size when it maps to a preset (else keep 16:9).
        if (pkg.pmmWidth > 0 && pkg.pmmHeight > 0 && AspectIndexFromWH(pkg.pmmWidth, pkg.pmmHeight) != 0)
            doc->SetAspect(pkg.pmmWidth, pkg.pmmHeight);
        // MME effect controllers -> native render settings (bloom / DoF / SSAO / AutoLuminous / ...).
        const PmmEffectSettings& fx = pkg.pmmEffects;
        if (fx.autoLuminous) settings_.autoLuminous = true;
        if (fx.bloom) settings_.bloom = true;
        if (fx.ssao) settings_.ssao = true;
        if (fx.dof) settings_.dof = true;
        if (fx.dofAperture >= 0.0f) settings_.dofAperture = std::clamp(fx.dofAperture, 0.2f, 3.0f);
        if (fx.lutIntensity >= 0.0f) settings_.lutIntensity = std::clamp(fx.lutIntensity, 0.0f, 1.0f);
        ApplyRenderSettings();
        if (!fx.recognised.empty()) {
            LOG_INFO("studio: pmm MME effects: %d recognised, %d unsupported",
                     (int)fx.recognised.size(), (int)fx.unsupported.size());
            for (const std::string& r : fx.recognised) LOG_INFO("studio: pmm MME: %s", r.c_str());
        }
        toast_ = {Tr("이 .pmm 파일은 MMD 프로젝트(읽기 전용)예요. 모델·카메라·음원을 불러와 새 스튜디오 프로젝트를 만들어요."),
                  studioLoadTitle_, {}, false, timeSeconds_ + 6.0};
    } else {
        doc->useMotionCamera = doc->HasCameraKeys() && !options_.freeCamera;
        doc->lights = PresetLights(0, DirectX::XMFLOAT3{0.0f, 10.0f, 0.0f}, doc->nextLightUid);  // the Studio preset
        // A library scene with an MMDX stage starts with the stage's light rig, exactly as adding the stage inside the
        // studio does (StudioApplyStageLightRig), but as the new project's starting state (no undo step). Before this
        // the rig only came with "add stage", so a stage chosen in the lobby had no spots (no beams in the haze).
        for (const studio::StudioStage& stage : doc->stages) {
            std::vector<SceneLight> rig;
            if (!StudioStageRigLights(*doc, stage.uid, rig)) continue;
            doc->lights = std::move(rig);
            if (stage.manifest && stage.manifest->environment.fog.present && !settings_.volumetric) {
                settings_.volumetric = true;   // as StudioApplyStageLightRig: a fog stage turns the haze on
                ApplyRenderSettings();
            }
            break;   // one rig per project (the first stage that has one), as an added stage replaces the lights
        }
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
    // A project opened from disk joins the recent list; a .pmm import (a new unsaved project) remembers
    // the source so "recent files" can reopen it, but never overwrites .mmdxproj entries.
    if (!pkg.recovered && !pkg.projectPath.empty()) {
        settings_.AddRecentProject(PathToUtf8(pkg.projectPath));
        settings_.Save(settingsPath_);
    } else if (pkg.fromPmm && !pkg.pmmSourcePath.empty()) {
        settings_.AddRecentProject(PathToUtf8(pkg.pmmSourcePath));
        settings_.Save(settingsPath_);
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
    if (pkg.fromPmm && !pkg.pmmEffects.unsupported.empty()) {
        std::string list;
        for (size_t i = 0; i < pkg.pmmEffects.unsupported.size(); ++i)
            list += (i ? ", " : "") + pkg.pmmEffects.unsupported[i];
        char title[64];
        std::snprintf(title, sizeof(title), Tr("MME 효과 %d개는 지원하지 않습니다"),
                      (int)pkg.pmmEffects.unsupported.size());
        toast_ = {title, list, {}, true, timeSeconds_ + 8.0};
    }
    StudioEnter(std::move(doc));
    return true;
}

void App::StudioEnter(std::unique_ptr<StudioDoc> doc) {
    studio_ = std::move(doc);
    studioCamPath_.clear();  // cached per camera evaluator version, which restarts with the document
    studioCamKeys_.clear();
    studioKeyEdit_ = false;
    studioStageDragging_ = false;   // a stage panel gesture does not outlive its document
    studioStageBefore_.clear();
    studioStageOpen_.clear();
    studioLastBind_ = 0;
    studioLeaveConfirm_ = false;
    studioPending_ = StudioAction::None;
    studioViewDrag_ = 0;
    studioAutosaveAt_ = timeSeconds_;
    lastRenderedTime_ = -1;
    // The ribbon's tab and fold come from the ini; a scripted / --frames run always starts on 홈, expanded (so captures
    // are reproducible whatever the last interactive session left behind).
    studioRibbonTab_ = settings_.studioRibbonTab;
    studioRibbonCollapsed_ = settings_.studioRibbonCollapsed;
    // Auto-key is remembered too (default off, MMD's feel); scripted runs start with it off (`studioautokey 0|1`).
    studio_->autoKey = settings_.studioAutoKey;
    if (HeadlessRun()) {
        studioRibbonCollapsed_ = false;
        studioRibbonTab_ = 0;
        studio_->autoKey = false;
    }
    if (uiScriptNext_ == 0) framesInScene_ = 0;  // a running ui script keeps its frame clock across project switches
    if (studio_->hasAudio) StudioSeekAudio();
    screen_ = Screen::Studio;
    // M6b: while the studio is open the project's own MME post stack replaces the play-mode stack.
    ApplyRenderSettings();
}

void App::LeaveStudio() {
    for (auto& job : studioJobs_)
        if (job->mcp) job->mcp->Reject("The studio was closed before the model finished loading");
    studioJobs_.clear();  // waits for running loads
    StudioDiscardRecovery();
    ctx_.WaitForGpu();
    renderer_.ReleaseVideoLayers();   // stop the studio's background AVI decode (and any song layers)
    // S6: the project's stage-bundled packs leave the registry with the studio (a reopened project re-reads them)
    if (studio_)
        for (const studio::StudioStage& stage : studio_->stages)
            if (stage.manifest) ShaderPacks().UnregisterStagePacks(stage.manifest->id);
    studio_.reset();
    audio_.Unload();
    RenderSettings rs = renderer_.Settings();
    rs.viewportX = rs.viewportY = rs.viewportW = rs.viewportH = 0;
    rs.shading = ViewShading::Lit;  // the studio shading mode is an editing view only
    renderer_.SetSettings(rs);
    studioLeaveConfirm_ = false;
    studioPending_ = StudioAction::None;
    ResetStudioGi();   // a viewport GI job does not outlive the studio
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
    // Ctrl+F1 folds / unfolds the ribbon's tool row (the header row stays)
    if (!io.WantTextInput && !StudioModal() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F1, false))
        studioRibbonCollapsed_ = !studioRibbonCollapsed_;
    // holding the right button flies the viewport camera with WASDQE: the shortcuts stay out of the way
    if (!io.WantTextInput && !StudioModal() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
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
        const auto kbd2 = [&](const char* a) { return (ImGuiKey)settings_.KeyFor(a); };
        if (pressed(kbd2("togglePlay"), false)) StudioSetPlaying(!d.playing);
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
        if (ctrl && pressed(ImGuiKey_A, false)) {
            // Ctrl+A follows the focused panel: the viewport / camera window selects every bone of the active model,
            // the timeline selects all its keys; anywhere else, bones when a pose model exists, else the timeline rows.
            if (studioFocusPanel_ == 1) StudioSelectAll();
            else if (StudioPoseModel()) StudioSelectAllBones();
            else StudioSelectAll();
        }
        const auto kbd = [&](const char* a) { return (ImGuiKey)settings_.KeyFor(a); };
        if (pressed(kbd("registerPose"), false)) StudioRegisterPose(ctrl);  // pose edits, or the picked rows (Ctrl: all bones)
        if (!ctrl && pressed(ImGuiKey_T, false)) d.modelGizmo = !d.modelGizmo;
        if (!ctrl && pressed(kbd("toolRotate"), false)) d.gizmoTool = 0;
        if (!ctrl && pressed(kbd("toolMove"), false)) d.gizmoTool = 1;
        if (!ctrl && pressed(ImGuiKey_L, false)) d.gizmoLocal = !d.gizmoLocal;
        if (!ctrl && pressed(kbd("toolAutoIk"), false)) d.autoIk = !d.autoIk;
        if (!ctrl && pressed(kbd("toggleBones"), false)) d.showBones = !d.showBones;
        if (pressed(ImGuiKey_Escape, false) && d.possessCamera) StudioPossess(false);
        if (pressed(ImGuiKey_Escape, false) && studioViewDrag_ != 2 && d.activeBone >= 0) StudioSelectBone(-1, false);
        if (ctrl && pressed(ImGuiKey_S, false)) StudioSave(shift);
        if (ctrl && pressed(ImGuiKey_O, false)) StudioRequest(StudioAction::Open);
        if (ctrl && pressed(ImGuiKey_N, false)) StudioRequest(StudioAction::New);
        if (studio_.get() != &d) return;  // the project was closed or replaced (New / Open)
    }
    StudioPollJobs();
    StudioPollBake();
    StudioAutosave(false, false);
    StudioPumpColliders();
    // any offset change (the lane drag, the menu, undo / redo, MCP) re-aligns a playing track under the playhead
    if (d.audioOffset != studioSeekedAudioOffset_) {
        studioSeekedAudioOffset_ = d.audioOffset;
        if (d.playing && d.hasAudio) StudioSeekAudio();
    }
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

// World entities (docs/effect_ecosystem.md §2 item 7): the studio cast's collider sources. Characters only.
std::vector<CharacterSource> App::StudioColliderCast() const {
    std::vector<CharacterSource> cast;
    if (!studio_) return cast;
    const studio::StudioDoc& d = *studio_;
    for (const auto& m : d.models) {
        if (!m->pmx || m->kind != studio::ModelKind::Character || !m->inst) continue;
        CharacterSource src;
        src.pmx = m->pmx;
        if (m->boundVersion != m->motionVersion) {
            const VmdMotion vmd = m->motion.ToVmd();
            src.motion = BoundMotion::Bind(*m->pmx, {&vmd});
        } else {
            src.motion = m->bound;
        }
        src.motionVersion = m->motionVersion;
        src.id = std::to_string(m->uid) + "|" + m->name;
        const float scale = (m->libraryId.empty() ? 1.0f : settings_.CharacterScale(m->libraryId)) *
                            std::max(0.01f, m->place.scale);
        src.scale = scale;
        const float kRad = 0.01745329252f;
        DirectX::XMFLOAT4X4 root;
        DirectX::XMStoreFloat4x4(&root,
            DirectX::XMMatrixMultiply(
                DirectX::XMMatrixRotationRollPitchYaw(m->place.rotationDeg.x * kRad, m->place.rotationDeg.y * kRad,
                                                      m->place.rotationDeg.z * kRad),
                DirectX::XMMatrixTranslation(m->place.translation.x / scale, m->place.translation.y / scale,
                                            m->place.translation.z / scale)));
        src.root = root;
        cast.push_back(std::move(src));
    }
    return cast;
}

// Debounced background rebuild of the studio collider track (only while the project has world entities).
void App::StudioPumpColliders() {
    if (!studio_) return;
    studio::StudioDoc& d = *studio_;
    if (studioColliderFuture_.valid() &&
        studioColliderFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        studioColliders_ = studioColliderFuture_.get();
    }
    if (d.worlds.empty()) {
        studioColliders_.reset();
        studioColliderEditAt_ = -1.0;
        studioColliderPendingVersion_ = 0;
        return;
    }
    const std::vector<CharacterSource> cast = StudioColliderCast();
    uint64_t version = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) { version = (version ^ v) * 1099511628211ull; };
    for (const CharacterSource& c : cast) {
        mix((uint64_t)(uintptr_t)c.pmx.get());
        mix(c.motionVersion);
        mix((uint64_t)(uintptr_t)c.motion.get());
        const float* f = &c.root._11;
        for (int i = 0; i < 16; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &f[i], sizeof(bits));
            mix(bits);
        }
        uint32_t sbits;
        std::memcpy(&sbits, &c.scale, sizeof(sbits));
        mix(sbits);
    }
    mix(d.history.Version());
    // A model list / motion / cast change (history or the cast hash): start the 0.5 s debounce again.
    if (version != studioColliderPendingVersion_) {
        studioColliderPendingVersion_ = version;
        studioColliderEditAt_ = timeSeconds_;
    }
    if (studioColliderFuture_.valid()) return;
    if (version == studioColliderVersion_) return;
    if (studioColliderEditAt_ >= 0.0 && timeSeconds_ - studioColliderEditAt_ < 0.5) return;
    studioColliderVersion_ = version;
    const double end = (double)d.EndFrame() / kMmdFps;
    studioColliderFuture_ = std::async(std::launch::async, [cast, end] {
        return BuildColliderTrack(cast, 0.0, end, 30.0f);
    });
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
    // While playing, a slow frame (the motion advanced about as much as the wall clock) keeps stepping instead of
    // resetting: a reset settles every body for a second and could keep every following frame over 0.25 s too.
    const auto now = std::chrono::steady_clock::now();
    const float wallDt = std::chrono::duration<float>(now - d.physicsClock).count();
    d.physicsClock = now;
    float physicsDt = 0.0f;
    bool resetPhysics = d.physicsFrame < 0.0f;
    if (!resetPhysics) {
        const float df = frame - d.physicsFrame;
        if (d.playing) resetPhysics = df < 0.0f || (df > 0.25f * kMmdFps && df > (wallDt * 1.5f + 0.1f) * kMmdFps);
        else if (std::fabs(df) > 1e-3f) resetPhysics = df < 0.0f || df > 1.0f + 1e-3f;
        if (!resetPhysics) physicsDt = std::max(0.0f, df) / kMmdFps;
        // paused with pose edits: keep simulating so hair and skirts follow the edited pose
        if (!resetPhysics && !d.playing && (posing || d.physicsAlways)) physicsDt = 1.0f / 60.0f;
    }
    d.physicsFrame = frame;
    // S1b: the MMDX stage material targets at the timeline time, BEFORE UpdateMaterials (a changed shadow flag rebuilds
    // the constants in this frame).
    StudioApplyStageTargets();

    // props last: their root follows a bone of a model posed in the first pass
    for (int pass = 0; pass < 2; ++pass)
        for (auto& mp : d.models)
            if (mp->IsProp() == (pass == 1)) StudioUpdateModel(*mp, slot, frame, physicsDt, resetPhysics);

    d.RefreshCameraEvals();  // S4: the main camera + one evaluator per extra camera
    if (!StudioShownCameraEval()) d.useMotionCamera = false;  // the main camera's keys (or a possessed screen camera's)
}

void App::StudioUpdateModel(StudioModel& m, uint64_t slot, float frame, float physicsDt, bool resetPhysics) {
    StudioDoc& d = *studio_;
    if (!m.inst || !m.gpu) return;   // M5c: an `.fx`-only effect object has no mesh / instance
    // Re-bind after edits (throttled while a curve is being dragged: binding a long dance takes a while).
    if (m.boundVersion != m.motionVersion && (!d.curveEditing || timeSeconds_ - studioLastBind_ > 0.15)) {
        OpTimer timer{"bind motion"};
        if (m.motion.bones.empty() && m.motion.morphs.empty() && m.motion.ik.empty() && m.motion.visibility.empty()) {
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
    const bool posed = StudioApplyPose(m);
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
        inst.EnablePhysics(d.physics && !m.physicsOff);
        // Always sync gravity and wind (including zero values) so turning the wind off explicitly clears the
        // physics-world state. The same values reach the viewport, renders and the bake (BakeOptions::env).
        if (d.physics && !m.physicsOff) {
            const PhysicsEnv& env = d.physicsEnv;
            inst.SetGravity(env.gravity, env.gravityDir);
            inst.SetWind(env.windDir, env.windStrength, env.windGust, env.windGustFreq, env.windJitter);
        }
        // Interactive playback caps Bullet substeps (drops simulated time on a slow frame); offline renders keep full.
        inst.SetPhysicsMaxSubsteps(offline_.mode == OfflineMode::None ? 4 : 10);
        if (!(m.placeApplied == m.place)) {  // moved by hand: the bodies restart at the new place
            m.placeApplied = m.place;
            resetPhysics = true;
        }
        if (resetPhysics) inst.ResetPhysics(frame / kMmdFps);  // the wind's gusts follow the timeline
    }
    if (m.IsEffect()) {
        // M5c: an effect object's mesh keeps model-space vertices in MMD (the effect's WORLD places it), so the
        // root transform is the identity and only the controller panel (AccessoryPlacement) drives `(self)`.
        inst.SetRootTransform(DirectX::XMFLOAT4X4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    } else if (m.IsProp()) {
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
        // The stage BLAS is built once: rebuild it when what the stage shows changes (placement, keys, the frame of
        // a keyed stage, viewport edits). Keyless, unedited stages are posed once.
        const PoseLayer layer = posed ? m.pose : PoseLayer{};
        const bool changed = !(m.placeApplied == m.place) || m.stageMotionApplied != m.motionVersion ||
                             (m.bound && m.stageFrameApplied != frame) || !SamePoseLayer(m.stagePoseApplied, layer);
        if (changed) {
            m.placeApplied = m.place;
            m.stageMotionApplied = m.motionVersion;
            m.stageFrameApplied = frame;
            m.stagePoseApplied = layer;
            m.gpu->Rt().blasBuilt = false;
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
    // M6a: the model's MME choice (project field; characters default None, stages Auto), resolved through the global
    // switch / workflow and the run-wide CLI overrides. A project choice still shows in the inspector, but the
    // renderer uses ResolveMmeChoice.
    {
        const bool isStage = m.kind == ModelKind::Stage;
        MmeChoice mme = ResolveMmeChoiceFor(m.mme, isStage, m.path);
        if (MmeGlobalEnabled() && m.IsEffect() && !m.fx.empty()) mme = {MmeChoice::File, m.fx};  // M5c: the effect object's own .fx
        ApplyMmeEffect(*m.gpu, mme, m.path, MmeAutoSkipApplies(mme, isStage));
    }
    m.gpu->UpdateMaterials(slot, inst.MaterialMul(), inst.MaterialAdd(), inst.MaterialVersion(),
                           m.IsProp() ? AccessoryOpacity(m, frame) : 1.0f);
}

void App::StudioCamera(CameraParams& camera) const {
    const StudioDoc& d = *studio_;
    const float frame = (float)(d.time * kMmdFps);
    // The main camera (the output), except while possessing a stage-screen camera: the viewport then shows the camera
    // being edited (renders drop possession first, so they always get the main camera).
    const std::shared_ptr<CameraMotion>& eval = StudioShownCameraEval();
    if (d.useMotionCamera && eval) {
        CameraPose pose = eval->Evaluate(frame);
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

LightAnchors App::StudioBuildLightAnchors() const {
    LightAnchors anchors;
    if (!studio_) return anchors;
    const StudioDoc& d = *studio_;
    const auto anchorsOf = [](const StudioModel& m, DirectX::XMFLOAT3& focus, DirectX::XMFLOAT3& head) {
        if (!m.pmx || !m.inst) return;
        const int center = m.pmx->FindBone(kCenterBone);
        if (center >= 0) focus = m.inst->BoneWorldPosition(center);
        const int hb = m.pmx->FindBone(kHeadBone);
        head = focus;
        if (hb >= 0) head = m.inst->BoneWorldPosition(hb);
        else head.y += 8.0f;
    };
    const StudioModel* performer = nullptr;
    for (const auto& m : d.models) {
        if (m->visible && m->kind == ModelKind::Character) {
            performer = m.get();
            break;
        }
    }
    if (performer) anchorsOf(*performer, anchors.focus, anchors.head);
    for (const auto& m : d.models) {
        if (m->kind != ModelKind::Character) continue;
        LightAnchors::Character c;
        c.uid = m->uid;
        c.centre = {0, 10, 0};
        anchorsOf(*m, c.centre, c.head);
        anchors.characters.push_back(c);
    }
    return anchors;
}

void App::StudioApplyStageTargets() {
    // S3: every MMDX stage entity is evaluated once per frame at the timeline time: its param overrides (base values with
    // the keyed values at this frame) go through the pure evaluator, its material targets into the parts' GPU models and
    // its light targets onto the scene lights that came from it (tagged: SceneLight::stageUid / stageLightName).
    StudioDoc& d = *studio_;
    if (d.stages.empty()) return;
    const uint64_t frame = ctx_.FrameNumber();
    const float timelineFrame = (float)(d.time * kMmdFps);
    for (StudioStage& stage : d.stages) {
        if (!stage.manifest) continue;
        const StudioStageOverrides ov = StageOverridesAt(stage, timelineFrame);
        const StageEvalResult eval = EvaluateStage(*stage.manifest, stage.variant, ov.values, d.time, ov.skipCues);
        for (const std::string& w : eval.warnings) LOG_WARN("studio: stage %s: %s", stage.stageId.c_str(), w.c_str());
        // materials: one rule list per stage per frame, applied to every part of this stage in the document
        if (!stage.manifest->materials.empty()) {
            const std::vector<StageMaterialRule> rules = ResolvedStageMaterialRules(*stage.manifest, eval);
            for (const auto& mp : d.models) {
                const StudioModel& m = *mp;
                if (m.stageUid != stage.uid || !m.gpu) continue;
                ApplyStageMaterialList(*m.gpu, m.stagePart, m.stageBinding, rules, frame);
            }
        }
        // S6: the stage's bundled surface packs (their `packs.<index>.params.<key>` targets at this frame)
        if (!stage.manifest->packs.empty()) {
            const std::vector<StagePackState> packs = ResolveStagePacks(*stage.manifest, eval);
            for (const auto& mp : d.models) {
                const StudioModel& m = *mp;
                if (m.stageUid != stage.uid || !m.gpu) continue;
                StageRuntimePart part;
                part.part = m.stagePart;
                part.gpu = m.gpu.get();
                part.surfacePack = m.stageSurfacePack;
                ApplyStageSurfacePack(packs, part);
            }
        }
        // lights: reset the fields a binding drives to the manifest's value, then replay the frame's assignments
        for (SceneLight& l : d.lights) {
            if (l.stageUid != stage.uid || l.stageLightName.empty()) continue;
            const std::set<std::string> driven = StageDrivenLightFields(*stage.manifest, l.stageLightName);
            if (driven.empty()) continue;
            if (const SceneLight* src = stage.manifest->FindLight(l.stageLightName)) {
                if (driven.count("enabled")) l.enabled = src->enabled;
                if (driven.count("intensity")) l.v.intensity = src->v.intensity;
                if (driven.count("range")) l.v.range = src->v.range;
                if (driven.count("coneOuter")) l.v.coneOuter = src->v.coneOuter;
                if (driven.count("coneInner")) l.v.coneInner = src->v.coneInner;
                if (driven.count("shadowSoftness")) l.shadowSoftness = src->shadowSoftness;
                if (driven.count("shadowDensity")) l.shadowDensity = src->shadowDensity;
                if (driven.count("rimStrength")) l.rimStrength = src->rimStrength;
                if (driven.count("swayPhase")) l.swayPhase = src->swayPhase;
                if (driven.count("color")) l.v.color = src->v.color;
                if (driven.count("rimColor")) l.rimColor = src->rimColor;
                if (driven.count("shadowColor")) l.shadowColor = src->shadowColor;
                if (driven.count("position")) l.v.position = src->v.position;
                if (driven.count("aim")) l.v.aim = src->v.aim;
                if (driven.count("direction")) l.v.direction = src->v.direction;
                if (driven.count("size")) l.v.size = src->v.size;
                if (driven.count("vmdLink")) l.vmdLink = src->vmdLink;
                // S7b: the gobo block (the file keeps its relative form; the stage runtime / renderer resolves it)
                if (driven.count("gobo.rotation")) l.goboRotation = src->goboRotation;
                if (driven.count("gobo.scale")) l.goboScale = src->goboScale;
                if (driven.count("gobo.spin")) l.goboSpin = src->goboSpin;
                if (driven.count("gobo.focus")) l.goboFocus = src->goboFocus;
            }
            for (const StageAssignment& a : eval.targets) {
                const StageTargetSpec spec = ResolveStageTarget(*stage.manifest, a.target);
                if (!spec.ok || spec.kind != StageTargetSpec::Kind::Lights || spec.owner != l.stageLightName) continue;
                ApplyStageLightField(l, spec.field, a.value);
            }
        }
    }
}

// S5: one screen override edit (one StagesCommand step). An override equal to the manifest's value is still saved as
// given: the panel writes what the user picked, and "없음" means the project turns the source off.
void App::StudioSetScreenOverride(uint32_t stageUid, const std::string& screenId,
                                  const studio::StageScreenOverride& ov, const char* name) {
    StudioDoc& d = *studio_;
    StudioStage* stage = d.FindStage(stageUid);
    if (!stage || screenId.empty()) return;
    std::vector<studio::StudioStage> before = d.stages;
    stage->screens[screenId] = ov;
    d.history.Push(std::make_unique<studio::StagesCommand>(d, name, std::move(before), d.stages));
    ++d.projectVersion;
    d.rowsKey = ~0ull;
}

// S5: a stage entity's screens at the timeline time: the manifest's values, the evaluator's screen targets, then the
// project's per-screen overrides (the stage panel, `studioscreen` and `studiostate` all read this).
std::vector<StageScreenState> App::StudioStageScreenStates(const studio::StudioStage& stage,
                                                           const StageEvalResult& eval) const {
    if (!stage.manifest) return {};
    return ResolveStageScreens(*stage.manifest, eval, [&](StageScreenState& st) {
        const auto it = stage.screens.find(st.id);
        if (it == stage.screens.end()) return;
        const studio::StageScreenOverride& so = it->second;
        if (so.source) {
            StageScreenSource src = StageScreenSource::None;
            if (ParseStageScreenSource(*so.source, src)) st.source = src;
        }
        if (so.file) st.file = *so.file;
        if (so.camera) st.camera = *so.camera;
        if (so.brightness) st.brightness = *so.brightness;
    });
}

// S5: the studio's stage screens. Each stage entity's screens are evaluated at the timeline time (its param overrides
// and cue replacement as the material targets do), the project's per-screen overrides are applied on top, then the
// ScreenViews are built from the stage's parts.
void App::StudioStageScreens(FrameView& view) {
    StudioDoc& d = *studio_;
    if (d.stages.empty()) return;
    // the studio's own cameras (S4) are targets of "uid:<n>"
    const auto resolveCamera = [&](const std::string& name, CameraParams& out) {
        if (name.rfind("uid:", 0) == 0) {
            const uint32_t uid = (uint32_t)std::max(0, std::atoi(name.c_str() + 4));
            if (uid == 0) return false;
            const float frame = (float)(d.time * kMmdFps);
            // the camera's evaluator (StudioDoc::RefreshCameraEvals built one per camera with keys)
            for (size_t i = 0; i < d.cameras.size(); ++i) {
                if (d.cameras[i].uid != uid) continue;
                if (i >= d.cameraEvals.size() || !d.cameraEvals[i]) return false;
                const CameraPose pose = d.cameraEvals[i]->Evaluate(frame);
                CameraMotion::ToView(pose, &out.view, &out.eye);
                out.fovYRadians = DirectX::XMConvertToRadians(pose.fovDeg);
                return true;
            }
            return false;
        }
        return false;
    };
    for (const studio::StudioStage& stage : d.stages) {
        if (!stage.manifest) continue;
        const StudioStageOverrides ov = StageOverridesAt(stage, (float)(d.time * kMmdFps));
        const StageEvalResult eval = EvaluateStage(*stage.manifest, stage.variant, ov.values, d.time, ov.skipCues);
        const std::vector<StageScreenState> screens = StudioStageScreenStates(stage, eval);
        if (screens.empty()) continue;
        // the stage's parts in the document (its material targets use the same set)
        std::vector<StageRuntimePart> parts;
        for (const auto& mp : d.models) {
            const studio::StudioModel& m = *mp;
            if (m.stageUid != stage.uid || !m.gpu) continue;
            StageRuntimePart p;
            p.part = m.stagePart;
            p.gpu = m.gpu.get();
            parts.push_back(std::move(p));
        }
        if (parts.empty()) continue;
        const std::filesystem::path dir = stage.manifest->dir;
        std::vector<ScreenView> list = BuildStageScreenViews(
            *stage.manifest, screens, parts,
            [&](const std::string& file) {
                if (file.empty()) return std::filesystem::path{};
                std::filesystem::path p = Utf8ToPath(file);
                return p.is_absolute() ? p : dir / p;
            },
            [&](const std::string& name, CameraParams& out) {
                if (resolveCamera(name, out)) return true;
                for (const StageCamera& sc : stage.manifest->cameras)
                    if (sc.name == name) {
                        out = StageCameraView(sc);
                        return true;
                    }
                return false;
            });
        for (ScreenView& sv : list) {
            sv.key = std::to_string(stage.uid) + ":" + sv.key;   // two stages may have the same screen id
            view.screens.push_back(std::move(sv));
        }
    }
}

void App::StudioConsolidateStages(StudioDoc& d, const std::map<uint32_t, std::string>& legacyVariants) {
    // S3: the parts that share a manifest belong to one stage entity. A project brings its entities (and the parts'
    // stageUid); a library scene / a specifically added stage gets one here, keeping the variant the package carried
    // (S1b's per-part "stageVariant", also what a version <= 3 project stored).
    std::map<const StageManifest*, uint32_t> byManifest;
    for (const StudioStage& s : d.stages)
        if (s.manifest) byManifest.emplace(s.manifest.get(), s.uid);
    // a project entity whose stage.json could not be read adopts a part's manifest (the loader finds one from the file)
    for (const auto& mp : d.models) {
        StudioModel& m = *mp;
        if (!m.stageManifest || m.stageUid == 0) continue;
        if (StudioStage* s = d.FindStage(m.stageUid); s && !s->manifest) s->manifest = m.stageManifest;
    }
    for (const auto& mp : d.models) {
        StudioModel& m = *mp;
        if (!m.stageManifest) {
            m.stageUid = 0;
            continue;
        }
        const auto known = byManifest.find(m.stageManifest.get());
        if (m.stageUid != 0 && d.FindStage(m.stageUid)) {
            if (known == byManifest.end()) byManifest.emplace(m.stageManifest.get(), m.stageUid);
            continue;
        }
        if (known != byManifest.end()) {
            m.stageUid = known->second;
            continue;
        }
        StudioStage stage;
        stage.uid = d.nextStageUid++;
        stage.manifest = m.stageManifest;
        stage.manifestPath = m.stageManifest->manifestPath;
        stage.stageId = !m.libraryId.empty() ? m.libraryId : m.stageManifest->id;
        const auto lv = legacyVariants.find(m.uid);
        if (lv != legacyVariants.end()) stage.variant = lv->second;
        d.stages.push_back(std::move(stage));
        byManifest[m.stageManifest.get()] = d.stages.back().uid;
        m.stageUid = d.stages.back().uid;
    }
    // A stage entity is only created here for parts that are in the document, so a created one never dangles. A project's
    // entity is kept even when all its parts failed to load: its overrides survive a round trip.
}

void App::StudioSetStageVariant(int model, const std::string& variantId) {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    if (model < 0 || model >= (int)d.models.size()) return;
    StudioModel& m = *d.models[model];
    StudioStage* stage = d.StageOfModel(m);
    if (!stage) return;
    if (stage->variant == variantId) return;
    std::vector<StudioStage> before = d.stages;
    stage->variant = variantId;
    // the parts' records return to the manifest's rule values for the new variant on the next frame
    d.history.Push(std::make_unique<StagesCommand>(d, Tr("스테이지 배리언트"), std::move(before), d.stages));
    ++d.projectVersion;
}

// ---------------------------------------------------------------------------
// S3: stage entity inspector (variant, params, cue import, per-param reset)
// ---------------------------------------------------------------------------

// One param edit: the base value when the param is not keyed and auto-key is off, else a key at the playhead (the
// timeline's rule for effect params). `comp` = a colour component (-1: the whole value). The caller brackets a gesture
// with StudioBeginStageEdit / StudioEndStageEdit (one undo step per gesture) — the list may be replaced by the push,
// so the stage is looked up again here.
void App::StudioStageParamEdit(uint32_t stageUid, const StageParam& param, int comp, float value, bool keyIt) {
    StudioDoc& d = *studio_;
    StudioStage* stage = d.FindStage(stageUid);
    if (!stage) return;
    const std::string key = param.key;
    const std::string trackName = StageParamName(param, comp);
    const int frame = d.Frame();
    const StageValue clamped = param.type == StageParamType::Color
                                   ? ClampStageParamValue(param, StageValue::Color([&] {
                                         auto it = stage->base.find(key);
                                         DirectX::XMFLOAT4 c = it != stage->base.end() ? it->second.AsColor() : param.defColor;
                                         switch (std::clamp(comp, 0, 3)) {
                                         case 0: c.x = value; break;
                                         case 1: c.y = value; break;
                                         case 2: c.z = value; break;
                                         default: c.w = value; break;
                                         }
                                         return c;
                                     }()))
                                   : ClampStageParamValue(param, StageValue::Number(value));
    const bool hasTrack = FindStageParamTrack(*stage, trackName) != nullptr;
    if (keyIt || d.autoKey || hasTrack) {
        // auto-key on / the param is already keyed: the edit becomes a key at the playhead
        if (!hasTrack) stage->tracks.push_back({trackName, {}});
        EffectParamTrack* tr = FindStageParamTrack(*stage, trackName);
        if (!tr) return;
        float v = value;
        if (param.type != StageParamType::Color) v = clamped.AsNumber();
        else if (comp >= 0) {
            const DirectX::XMFLOAT4 c = clamped.AsColor();
            v = comp == 0 ? c.x : comp == 1 ? c.y : comp == 2 ? c.z : c.w;
        }
        UpsertParamKey(*tr, EffectParamKey{frame, v, StageParamTrackStepped(*stage, trackName)});
        // a keyed param must not also take the manifest cue over it
        stage->cuesOff.insert(key);
        d.rowsKey = ~0ull;
        return;
    }
    stage->base[key] = clamped;
    d.rowsKey = ~0ull;
}

// One undo step per stage param gesture (a slider drag, a colour pick, a combo pick).
void App::StudioBeginStageEdit() {
    if (studioStageDragging_ || !studio_) return;
    studioStageBefore_ = studio_->stages;
    studioStageDragging_ = true;
}

void App::StudioEndStageEdit(const char* name) {
    if (!studioStageDragging_ || !studio_) return;
    studioStageDragging_ = false;
    StudioDoc& d = *studio_;
    if (d.stages == studioStageBefore_) {
        studioStageBefore_.clear();
        return;
    }
    d.history.Push(std::make_unique<StagesCommand>(d, name, std::move(studioStageBefore_), d.stages));
    studioStageBefore_.clear();
    ++d.projectVersion;
}

// S5: the stage panel's screen rows. Per screen: a source combo, a file picker (image / video), a camera combo (the
// stage's cameras + the studio's own) and a brightness slider. Every change is one undoable step and lands in the
// project's stage entry ("screens"), never in the installed stage.
void App::DrawStudioStageScreens(float w, uint32_t stageUid) {
    using namespace ui;
    const Palette& p = P();
    StudioDoc& d = *studio_;
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const StudioStage* stage = d.FindStage(stageUid);
    if (!stage || !stage->manifest) return;
    const std::shared_ptr<const StageManifest> manifest = stage->manifest;
    const StageManifest& man = *manifest;
    if (man.screens.empty()) return;
    ImGui::Dummy(ImVec2(w, Dp(8.0f)));
    const ImVec2 c = ImGui::GetCursorScreenPos();
    Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("스크린"));
    ImGui::Dummy(ImVec2(w, Dp(20.0f)));

    // The screen's values now: the manifest's with the project's override on top (an edit re-reads this every call, as
    // a StagesCommand replaces the stage list).
    for (const StageScreen& man_screen : man.screens) {
        const std::string id = man_screen.id;
        ImGui::PushID(id.c_str());
        const auto current = [&]() {
            studio::StageScreenOverride ov;
            if (const StudioStage* st = d.FindStage(stageUid)) {
                const auto it = st->screens.find(id);
                if (it != st->screens.end()) ov = it->second;
            }
            return ov;
        };
        const studio::StageScreenOverride ov = current();
        StageScreenSource src = man_screen.source;
        if (ov.source && !ParseStageScreenSource(*ov.source, src)) src = StageScreenSource::None;
        const std::string file = ov.file ? *ov.file : man_screen.file;
        const std::string camera = ov.camera ? *ov.camera : man_screen.camera;
        const float brightness = ov.brightness ? *ov.brightness : man_screen.brightness;

        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        TextEllipsis(cdl, Font::Regular, size::Small, rowTop, rowTop.x + w, p.ink, id.c_str());
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
        // source combo: 없음 / 이미지 / 영상 / 카메라 (program stays unimplemented = 없음)
        static const char* const kSourceLabels[] = {TrKey("없음"), TrKey("이미지"), TrKey("영상"), TrKey("카메라")};
        static const StageScreenSource kSources[] = {StageScreenSource::None, StageScreenSource::Image,
                                                     StageScreenSource::Video, StageScreenSource::Camera};
        int sel = 0;
        for (int i = 0; i < 4; ++i)
            if (kSources[i] == src) sel = i;
        const char* srcLabel = Tr(kSourceLabels[sel]);
        const std::string srcId = "##stagescreensource";
        const ImVec2 spos = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(spos.x, spos.y + Dp(4.0f)), spos.x + rowW * 0.4f, p.ink2,
                     Tr("소스"));
        ImGui::SetCursorScreenPos(ImVec2(spos.x + rowW * 0.4f, spos.y));
        ImGui::SetNextItemWidth(rowW - rowW * 0.4f);
        if (ImGui::BeginCombo(srcId.c_str(), srcLabel)) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(Tr(kSourceLabels[i]), i == sel)) {
                    studio::StageScreenOverride next = ov;
                    next.source = std::string(StageScreenSourceName(kSources[i]));
                    if (kSources[i] != StageScreenSource::None && file.empty()) next.file = std::string();
                    StudioSetScreenOverride(stageUid, id, next, Tr("스크린 소스"));
                    ImGui::EndCombo();
                    ImGui::PopID();
                    return;
                }
            }
            ImGui::EndCombo();
        }
        // file (image / video) with a picker
        if (src == StageScreenSource::Image || src == StageScreenSource::Video) {
            const std::string shown = file.empty() ? Tr("(파일 없음)") : PathToUtf8(std::filesystem::path(file).filename());
            if (Button("##stagescreenfile", shown.c_str(), icon::FolderOpen, ButtonKind::Secondary, ImVec2(w / Dpi(), 30.0f))) {
                const wchar_t* pattern = src == StageScreenSource::Image ? L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif"
                                                                         : L"*.avi;*.mp4;*.mkv;*.mov;*.wmv";
                const std::vector<studio::FileFilter> filters = {
                    {src == StageScreenSource::Image ? L"이미지" : L"영상", pattern}};
                const std::filesystem::path picked = studio::OpenFileDialog(GetActiveWindow(), filters, man.dir);
                if (!picked.empty()) {
                    studio::StageScreenOverride next = ov;
                    next.file = PathToUtf8(picked);
                    StudioSetScreenOverride(stageUid, id, next, Tr("스크린 파일"));
                    ImGui::PopID();
                    return;
                }
            }
        }
        // camera (stage cameras + the studio's own cameras, addressed as "uid:<n>")
        if (src == StageScreenSource::Camera) {
            std::vector<std::string> names;
            for (const StageCamera& sc : man.cameras) names.push_back(sc.name);
            for (const studio::StudioCamera& sc : d.cameras) names.push_back("uid:" + std::to_string(sc.uid));
            std::string label = camera.empty() ? Tr("(카메라 없음)") : camera;
            if (camera.rfind("uid:", 0) == 0)   // a studio camera: its name, not the "uid:<n>" address
                for (const studio::StudioCamera& sc : d.cameras)
                    if ("uid:" + std::to_string(sc.uid) == camera) label = sc.name + "  · " + Tr("스튜디오");
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(cpos.x, cpos.y + Dp(4.0f)), cpos.x + rowW * 0.4f, p.ink2,
                         Tr("카메라"));
            ImGui::SetCursorScreenPos(ImVec2(cpos.x + rowW * 0.4f, cpos.y));
            ImGui::SetNextItemWidth(rowW - rowW * 0.4f);
            if (ImGui::BeginCombo("##stagescreencam", label.c_str())) {
                for (size_t ni = 0; ni < names.size(); ++ni) {
                    const std::string& n = names[ni];
                    std::string shown2 = n;
                    if (n.rfind("uid:", 0) == 0) {
                        // a studio camera (often imported from this very stage camera, so the names repeat): marked,
                        // and every row gets its own ID (two equal labels were an ImGui ID conflict)
                        for (const studio::StudioCamera& sc : d.cameras)
                            if ("uid:" + std::to_string(sc.uid) == n) shown2 = sc.name + "  · " + Tr("스튜디오");
                    }
                    ImGui::PushID((int)ni);
                    const bool picked = ImGui::Selectable(shown2.c_str(), n == camera);
                    ImGui::PopID();
                    if (picked) {
                        studio::StageScreenOverride next = ov;
                        next.camera = n;
                        StudioSetScreenOverride(stageUid, id, next, Tr("스크린 카메라"));
                        ImGui::EndCombo();
                        ImGui::PopID();
                        return;
                    }
                }
                ImGui::EndCombo();
            }
        }
        // brightness
        float b = brightness;
        const bool changed = SliderRow("##stagescreenbright", Tr("밝기"), &b, 0.0f, 8.0f, "%.2f");
        if (ImGui::IsItemActivated()) StudioBeginStageEdit();
        if (changed) {
            studio::StageScreenOverride next = ov;
            next.brightness = b;
            StudioSetScreenOverride(stageUid, id, next, Tr("스크린 밝기"));
        }
        if (ImGui::IsItemDeactivated()) StudioEndStageEdit(Tr("스크린 밝기"));
        ImGui::PopID();
    }
}

// The stage's param surface: one row per param (float slider / int / bool / colour / enum combo), grouped by the
// manifest's `groups`, with the param's reset button and the "큐를 키로 가져오기" import.
void App::DrawStudioStageParams(float w, uint32_t stageUid) {
    using namespace ui;
    const Palette& p = P();
    StudioDoc& d = *studio_;
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const StudioStage* stage = d.FindStage(stageUid);
    if (!stage || !stage->manifest) return;
    // the manifest is held here: an edit pushes a StagesCommand which replaces the stages list
    const std::shared_ptr<const StageManifest> manifest = stage->manifest;
    const StageManifest& man = *manifest;
    if (man.params.empty()) return;

    // A value at the playhead: the keyed track's value, else the base value (else the param default). The stage is
    // looked up again every call: an edit pushes a StagesCommand, which replaces the list (a held pointer would dangle).
    const auto currentOf = [&](const std::string& trackName) {
        const StudioStage* st = d.FindStage(stageUid);
        if (!st) return 0.0f;
        const float staticVal = StageParamStaticValue(*st, trackName);
        if (const EffectParamTrack* tr = FindStageParamTrack(*st, trackName))
            return EvaluateParamTrack(*tr, (float)d.Frame(), staticVal);
        return staticVal;
    };
    const auto resetRow = [&](const StageParam& sp) {
        ImGui::PushID(sp.key.c_str());
        if (IconButton("##stagereset", icon::ArrowCcw, Tr("이 파라미터 초기화 (배리언트 / 기본값)"), false, 24.0f)) {
            StudioStage* target = d.FindStage(stageUid);
            if (target) {
                std::vector<StudioStage> before = d.stages;
                if (ResetStageParam(*target, sp.key)) {
                    d.history.Push(std::make_unique<StagesCommand>(d, Tr("스테이지 파라미터 초기화"), std::move(before),
                                                                   d.stages));
                    ++d.projectVersion;
                    d.rowsKey = ~0ull;
                }
            }
        }
        ImGui::PopID();
    };
    const auto drawParam = [&](const StageParam& sp) {
        const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(StageLanguage());
        const std::string sid = "##stageparam_" + sp.key;
        const auto edit = [&](int comp, float value) {
            StudioBeginStageEdit();
            StudioStageParamEdit(stageUid, sp, comp, value, false);
        };
        ImGui::PushID(sp.key.c_str());
        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        // the reset button sits in a narrow gutter left of the row (so it never collides with the widget or its value)
        ImGui::Indent(Dp(24.0f));
        switch (sp.type) {
        case StageParamType::Float: {
            const std::string tn = StageParamName(sp, -1);
            float v = currentOf(tn);
            const bool changed = SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.3f");
            if (ImGui::IsItemActivated()) StudioBeginStageEdit();   // the gesture's "before" is this frame's list
            if (changed) edit(-1, v);
            if (ImGui::IsItemDeactivated()) StudioEndStageEdit(Tr("스테이지 파라미터"));
            break;
        }
        case StageParamType::Int: {
            const std::string tn = StageParamName(sp, -1);
            float v = std::round(currentOf(tn));
            const bool changed = SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.0f");
            if (ImGui::IsItemActivated()) StudioBeginStageEdit();
            if (changed) edit(-1, std::round(v));
            if (ImGui::IsItemDeactivated()) StudioEndStageEdit(Tr("스테이지 파라미터"));
            break;
        }
        case StageParamType::Bool: {
            const std::string tn = StageParamName(sp, -1);
            bool b = currentOf(tn) > 0.5f;
            const bool changed = Switch(sid.c_str(), label.c_str(), &b);
            if (ImGui::IsItemActivated()) StudioBeginStageEdit();
            if (changed) edit(-1, b ? 1.0f : 0.0f);
            if (ImGui::IsItemDeactivated()) StudioEndStageEdit(Tr("스테이지 파라미터"));
            break;
        }
        case StageParamType::Color: {
            float rgb[3] = {currentOf(StageParamName(sp, 0)), currentOf(StageParamName(sp, 1)),
                            currentOf(StageParamName(sp, 2))};
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(cpos.x, cpos.y + Dp(4.0f)), cpos.x + rowW * 0.4f,
                         p.ink2, label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(cpos.x + rowW * 0.4f, cpos.y));
            ImGui::SetNextItemWidth(rowW - rowW * 0.4f);
            bool activated = false, deactivated = false;
            const bool changed = ColorField(sid.c_str(), rgb, &activated, &deactivated);
            if (activated) StudioBeginStageEdit();
            if (changed) {
                for (int c = 0; c < 3; ++c) edit(c, rgb[c]);
            }
            if (deactivated) StudioEndStageEdit(Tr("스테이지 파라미터"));
            break;
        }
        case StageParamType::Enum: {
            const std::string tn = StageParamName(sp, -1);
            const int cur = (int)std::lround(currentOf(tn));
            const int idx = sp.options.empty() ? -1 : std::clamp(cur, 0, (int)sp.options.size() - 1);
            const std::string item = idx >= 0 && !sp.options[(size_t)idx].label.Empty()
                                         ? sp.options[(size_t)idx].label.Get(StageLanguage())
                                         : (idx >= 0 ? sp.options[(size_t)idx].value : label);
            const ImVec2 cpos = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(cpos.x, cpos.y + Dp(4.0f)), cpos.x + rowW * 0.4f,
                         p.ink2, label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(cpos.x + rowW * 0.4f, cpos.y));
            ImGui::SetNextItemWidth(rowW - rowW * 0.4f);
            StudioBeginStageEdit();
            if (ImGui::BeginCombo(sid.c_str(), item.c_str())) {
                for (size_t i = 0; i < sp.options.size(); ++i) {
                    const StageParamOption& o = sp.options[i];
                    const std::string ol = o.label.Empty() ? o.value : o.label.Get(StageLanguage());
                    if (ImGui::Selectable(ol.c_str(), (int)i == idx)) {
                        StudioStageParamEdit(stageUid, sp, -1, (float)i, false);
                        StudioEndStageEdit(Tr("스테이지 파라미터"));
                    }
                }
                ImGui::EndCombo();
            }
            StudioEndStageEdit(Tr("스테이지 파라미터"));
            break;
        }
        }
        ImGui::Unindent(Dp(24.0f));
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + Dp(1.0f)));
        resetRow(sp);
        ImGui::SetCursorScreenPos(after);
        ImGui::PopID();
    };

    std::vector<const StageParam*> ungrouped;
    std::map<std::string, std::vector<const StageParam*>> grouped;
    for (const StageParam& sp : man.params) {
        const bool known = std::any_of(man.groups.begin(), man.groups.end(),
                                       [&](const StageParamGroup& g) { return g.id == sp.group; });
        if (sp.group.empty() || !known) ungrouped.push_back(&sp);
        else grouped[sp.group].push_back(&sp);
    }
    for (const StageParam* sp : ungrouped) drawParam(*sp);
    for (const StageParamGroup& g : man.groups) {
        const auto it = grouped.find(g.id);
        if (it == grouped.end() || it->second.empty()) continue;
        const std::string gl = g.label.Empty() ? g.id : g.label.Get(StageLanguage());
        const float gw = ImGui::GetContentRegionAvail().x;
        const ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::PushID(g.id.c_str());
        ImGui::InvisibleButton("##stagegroup", ImVec2(gw, Dp(28.0f)));
        const bool hovered = ImGui::IsItemHovered();
        static std::map<ImGuiID, bool> open;   // UI-only open state (the manifest's "collapsed" seeds it)
        const ImGuiID gid = ImGui::GetItemID();
        const bool isOpen = open.emplace(gid, !g.collapsed).first->second;
        if (ImGui::IsItemClicked()) open[gid] = !isOpen;
        if (hovered) cdl->AddRectFilled(gp, ImVec2(gp.x + gw, gp.y + Dp(28.0f)), WithAlpha(p.ink, 0.05f), Dp(6.0f));
        Icon(cdl, isOpen ? icon::CaretDown : icon::CaretRight, 12.0f, ImVec2(gp.x + Dp(6.0f), gp.y + Dp(14.0f)), p.ink3);
        TextEllipsis(cdl, Font::Semibold, size::Caption, ImVec2(gp.x + Dp(20.0f), gp.y + Dp(7.0f)), gp.x + gw, p.ink2,
                     gl.c_str());
        ImGui::SetCursorScreenPos(ImVec2(gp.x, gp.y + Dp(28.0f)));
        if (isOpen) {
            ImGui::Indent(Dp(10.0f));
            for (const StageParam* sp : it->second) drawParam(*sp);
            ImGui::Unindent(Dp(10.0f));
        }
        ImGui::PopID();
    }
}

void App::DrawStudioStagePanel(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    StudioModel* m = d.Selected();
    StudioStage* stage = m ? d.StageOfModel(*m) : nullptr;
    if (!stage) return;
    const uint32_t stageUid = stage->uid;
    ImGui::PushID((int)stageUid);
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    // the manifest is held here: an edit pushes a StagesCommand which replaces the stages list (the old `stage` dangles)
    const std::shared_ptr<const StageManifest> manifest = stage->manifest;
    const StageManifest* man = manifest.get();
    const std::string name = man ? man->DisplayName(StageLanguage()) : stage->stageId;
    // the inspector's label row (its `line` lambda lives in DrawStudioInspector)
    const auto labelRow = [&](const char* label, const std::string& value) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, c, p.ink3, label);
        TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(c.x + Dp(96.0f), c.y), c.x + w, p.ink, value.c_str());
        ImGui::Dummy(ImVec2(w, Dp(22.0f)));
    };
    labelRow(Tr("스테이지"), name);
    if (!man) {
        ImGui::PopID();
        return;
    }
    // S9: the stage's participating media, shown as the project's overrides (the fog the stage sets while it is loaded).
    if (man->environment.fog.present) {
        const StageFog& fog = man->environment.fog;
        char value[160];
        std::snprintf(value, sizeof(value), "%s  ·  %.4f  ·  %.2f  ·  %.3f", Tr("환경 안개"), fog.density, fog.anisotropy,
                      fog.heightFalloff);
        labelRow(Tr("안개"), value);
        char c[96];
        std::snprintf(c, sizeof(c), "%.2f, %.2f, %.2f", fog.color.x, fog.color.y, fog.color.z);
        labelRow(Tr("안개 색"), c);
        if (!man->environment.fogVolumes.empty()) {
            char n[64];
            std::snprintf(n, sizeof(n), "%d", (int)man->environment.fogVolumes.size());
            labelRow(Tr("안개 볼륨"), n);
        }
    }

    // S5: the stage's LED / video surfaces (source, file, camera, brightness; one undo step per change).
    if (!man->screens.empty()) DrawStudioStageScreens(w, stageUid);

    // variant combo (the stage's own; the parts share it). Applying one replaces the stages list: leave this frame.
    if (man->variants.size() >= 2) {
        std::string current = stage->variant;
        if (current.empty() && man->DefaultVariant()) current = man->DefaultVariant()->id;
        int selected = -1;
        for (size_t i = 0; i < man->variants.size(); ++i)
            if (man->variants[i].id == current) selected = (int)i;
        if (selected < 0) selected = 0;
        std::string label = man->variants[(size_t)selected].label.Get(StageLanguage());
        if (label.empty()) label = man->variants[(size_t)selected].id;
        std::string picked;   // the variant clicked this frame (applied after the combo closes)
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        labelRow(Tr("스테이지 배리언트"), label);
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##stagevariant", label.c_str())) {
            for (size_t i = 0; i < man->variants.size(); ++i) {
                const StageVariant& v = man->variants[i];
                std::string item = v.label.Get(StageLanguage());
                if (item.empty()) item = v.id;
                if (ImGui::Selectable(item.c_str(), (int)i == selected)) picked = v.id;
            }
            ImGui::EndCombo();
        }
        if (!picked.empty()) {
            StudioSetStageVariant(d.selectedModel, picked);
            ImGui::PopID();
            return;
        }
    }

    if (!man->params.empty()) {
        ImGui::Dummy(ImVec2(w, Dp(8.0f)));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("파라미터"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        // cue import: the manifest's own light show becomes keyable project tracks (one undo step)
        if (!man->cues.empty()) {
            size_t cueKeys = 0;
            for (const StageCue& cue : man->cues) cueKeys += cue.keys.size();
            char cueLabel[96];
            std::snprintf(cueLabel, sizeof(cueLabel), "%s (%d)", Tr("큐를 키로 가져오기"), (int)cueKeys);
            if (Button("##stageimportcues", cueLabel, icon::DownloadSimple, ButtonKind::Secondary, ImVec2(w / Dpi(), 32.0f))) {
                std::vector<StudioStage> before = d.stages;
                if (StudioStage* target = d.FindStage(stageUid); target && ImportStageCues(*target)) {
                    d.history.Push(std::make_unique<StagesCommand>(d, Tr("큐를 키로 가져오기"), std::move(before), d.stages));
                    ++d.projectVersion;
                    d.rowsKey = ~0ull;
                    toast_ = {Tr("큐를 키로 가져왔어요"), name, {}, false, timeSeconds_ + 4.0};
                    ImGui::PopID();
                    return;
                }
            }
            ImGui::Dummy(ImVec2(w, Dp(6.0f)));
        }
        DrawStudioStageParams(w, stageUid);
    }
    ImGui::PopID();
}

void App::BuildStudioFrameView(FrameView& view) {
    StudioDoc& d = *studio_;
    StudioCamera(view.camera);

    // stage parts first (the renderer's draw order), then characters and props
    bool anyStage = false;
    const float viewFrame = (float)(d.time * kMmdFps);
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& m : d.models) {
            if (!m->visible || m->IsStage() != (pass == 0)) continue;
            if (m->IsProp() && !AccessoryVisible(*m, viewFrame)) continue;  // a hidden accessory key hides the prop
            if (!m->IsProp() && !ModelVisibleAt(m->motion, viewFrame)) continue;  // VMD show/IK keys hide the model
            view.models.push_back(m->gpu.get());
            AppendMmeObject(view, m->gpu.get(), m->inst.get(), true,
                            !m->pmx->name.empty() ? m->pmx->name : m->gpu->Name());
            if (m->IsStage()) anyStage = true;
        }
    }
    view.studioFloor = !anyStage;

    // M5c: MME effect objects (an accessory `.x` + its same-stem `.fx`). The effect's post/scene part runs in the
    // post chain with the accessory key's panel as `(self)`; its object part draws the mesh (drawn here through the
    // MME object path). The mesh is added to `view.models` (via AppendMmeObject) so every draw path covers it.
    for (const auto& m : d.models) {
        if (!MmeGlobalEnabled()) break;   // global MME off: no effect objects drawn
        if (!m->visible || !m->IsEffect() || m->fx.empty() || !m->gpu) continue;
        if (!AccessoryVisible(*m, viewFrame)) continue;
        const PropAttach pl = AccessoryPlacement(*m, viewFrame);
        MmeControllerPanel panel;
        panel.x = pl.translation.x;
        panel.y = pl.translation.y;
        panel.z = pl.translation.z;
        panel.rx = DirectX::XMConvertToRadians(pl.rotationDeg.x);
        panel.ry = DirectX::XMConvertToRadians(pl.rotationDeg.y);
        panel.rz = DirectX::XMConvertToRadians(pl.rotationDeg.z);
        panel.si = pl.scale;
        panel.tr = AccessoryOpacity(*m, viewFrame);
        panel.visible = true;
        MmeEffectObject eo;
        eo.fx = m->fx;
        eo.meshFile = m->path;
        eo.mesh = m->gpu.get();
        eo.fileName = PathToUtf8(m->path.filename());
        for (char& c : eo.fileName) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        eo.displayName = m->pmx && !m->pmx->name.empty() ? m->pmx->name : m->name;
        eo.panel = panel;
        m->gpu->SetMmeEffectMesh(true);
        view.models.push_back(m->gpu.get());
        AppendMmeObject(view, m->gpu.get(), m->inst.get(), true, eo.displayName);
        if (!view.mmeObjects.empty()) {
            view.mmeObjects.back().accessory = true;
            view.mmeObjects.back().panel = panel;
        }
        view.mmeEffectObjects.push_back(std::move(eo));
    }

    LightAnchors anchors = StudioBuildLightAnchors();
    BuildSceneLighting(d.lights, d.camera.light, d.time, anchors, view.light);
    ApplyHazeGains(view.light, settings_.volumetricLightGain, settings_.volumetricAmbientGain);
    // S7b: a gobo path that came from a stage is relative to that stage folder: resolve it for the renderer, which keys
    // its texture cache by absolute path (a studio light's own pick is already absolute).
    for (PunctualLight& p : view.light.punctual) {
        if (p.gobo.empty()) continue;
        const std::filesystem::path path = Utf8ToPath(p.gobo);
        if (path.is_absolute()) continue;
        for (const StudioStage& s : d.stages) {
            if (!s.manifest) continue;
            std::error_code ec;
            const std::filesystem::path candidate = s.manifest->dir / path;
            if (std::filesystem::is_regular_file(candidate, ec)) { p.gobo = PathToUtf8(candidate); break; }
        }
    }
    // S9b-3: the same for a stage sky light's image (`skyMap` is stage-relative in a manifest; a studio pick is absolute).
    // Unresolved, the renderer could not load it and the studio showed the procedural sky instead of the stage's image.
    if (!view.light.skyMap.empty() && !Utf8ToPath(view.light.skyMap).is_absolute()) {
        const std::filesystem::path path = Utf8ToPath(view.light.skyMap);
        for (const StudioStage& s : d.stages) {
            if (!s.manifest) continue;
            std::error_code ec;
            const std::filesystem::path candidate = s.manifest->dir / path;
            if (std::filesystem::is_regular_file(candidate, ec)) { view.light.skyMap = PathToUtf8(candidate); break; }
        }
    }
    // S9: a stage's environment.fog is the project's medium while that stage is in the scene (the stage panel shows the
    // same values; a later stage in the list wins, as its rig would).
    for (const StudioStage& s : d.stages) {
        if (!s.manifest) continue;
        const StageFog& fog = s.manifest->environment.fog;
        if (!fog.present) continue;
        view.light.volumetricSigma0 = std::max(fog.density, 0.0f);
        view.light.volumetricColor = fog.color;
        view.light.volumetricFalloff = std::max(fog.heightFalloff, 0.0f);
        view.light.volumetricAnisotropy = std::clamp(fog.anisotropy, -0.9f, 0.95f);
        view.light.volumetricRadius = fog.radius;
    }
    if (settings_.volumetricRange >= 50.0f) view.light.volumetricRadius = settings_.volumetricRange;   // "안개 범위"
    StudioApplyLightTracks(view);
    // DoF: the focus track over the automatic focus (UiStudioFocus.cpp)
    StudioComputeFocus(view);
    // Background AVI (背景AVI): the video feeds screen.bmp materials; drawn as background only when the project
    // asks for it. The video time is the timeline time minus the offset (loop handled by the pass / ScreenVideo).
    if (!d.bgVideoPath.empty()) {
        VideoLayerDesc bg;
        bg.path = d.bgVideoPath;
        bg.kind = VideoLayerKind::Background;
        bg.screen = true;
        bg.drawBackground = d.bgVideoAsBackground;
        bg.time = d.time - d.bgVideoOffset;
        view.videoLayers.push_back(std::move(bg));
    }
    // M8a: the studio is edit mode for MME TIME (wall clock without SyncInEditMode); the scene clock follows the
    // timeline (paused = constant).
    view.sceneTime = (float)d.time;
    view.sceneElapsed = lastRenderedTime_ < 0 ? 0.0f : (float)std::fabs(d.time - lastRenderedTime_);
    view.timelinePlaying = d.playing;
    view.mmeEditMode = true;
    // S5: the MMDX stages' screens (image / video / camera feed sources; the project's per-screen overrides applied).
    StudioStageScreens(view);
    view.cameraCut = lastRenderedTime_ < 0 || std::fabs(d.time - lastRenderedTime_) > 0.25;
    lastRenderedTime_ = d.time;
    if (options_.quitAfterFrames > 0 || !options_.uiScript.empty() || options_.mcp == 1 || HeadlessRun()) {
        view.effectDt = 1.0f / 60.0f;
    }
    // shader pack scene / audio inputs: the performer = the first visible character; audio time = timeline - offset
    const ModelInstance* performer = nullptr;
    for (const auto& m : d.models)
        if (m->visible && m->kind == ModelKind::Character && m->inst) { performer = m->inst.get(); break; }
    FillPackInputs(view, performer, d.time - d.audioOffset);

    // World entities: one instance per enabled world, placed by `place`, params evaluated at the timeline frame.
    const float worldFrame = (float)(d.time * kMmdFps);
    for (const studio::StudioWorld& w : d.worlds) {
        if (!w.fx.entry.enabled) continue;
        WorldInstance inst;
        inst.uid = w.uid;
        inst.pack = w.fx.entry.pack;
        DirectX::XMStoreFloat4x4(&inst.transform, studio::PropOffsetMatrix(w.place));
        inst.params = studio::EvaluateEffect(w.fx, worldFrame).params;
        inst.time = d.time;
        inst.exact = !d.playing;
        inst.colliders = studioColliders_;
        view.worlds.push_back(std::move(inst));
    }
    // S6: each stage's bundled world packs, after the project's own worlds and at their sockets (the socket's node bone
    // world matrix x its explicit offset; the parts were posed by UpdateStudioScene). Not editable outliner entities.
    for (const studio::StudioStage& stage : d.stages) {
        if (!stage.manifest || stage.manifest->packs.empty()) continue;
        const studio::StudioStageOverrides ov = studio::StageOverridesAt(stage, worldFrame);
        const StageEvalResult eval = EvaluateStage(*stage.manifest, stage.variant, ov.values, d.time, ov.skipCues);
        const std::vector<StagePackState> packs = ResolveStagePacks(*stage.manifest, eval);
        const auto resolveNode = [&](const std::string& node, DirectX::XMFLOAT4X4& out) {
            for (const auto& mp : d.models) {
                const studio::StudioModel& m = *mp;
                if (m.stageUid != stage.uid || !m.pmx || !m.inst) continue;
                const int bone = m.pmx->FindBone(node);
                if (bone < 0) continue;
                out = m.inst->BoneWorld(bone);
                return true;
            }
            return false;
        };
        AppendStageWorldPacks(*stage.manifest, packs, d.time, !d.playing, studioColliders_,
                              kStageWorldUidBase + stage.uid * 256u, resolveNode, view.worlds);
    }
}

namespace {
// The runtime row index scheme of RowKind::EffectParam / RowKind::WorldParam (never saved): every pack param component
// has a stable index whether or not it has a track, so a row exists for it before it is keyed.
//   pack param i, scalar (Float / Int / Bool): i * 4, track name "key"
//   pack param i, Vec3: i * 4 + c (c 0..2), track "key#c"; Color: i * 4 + c (c 0..2 = R,G,B, 3 = A if `alpha`)
//   a track whose name matches no pack param (pack missing / param removed): 0x100000 + n (n = its order among such
//   tracks), name = the track name.
constexpr uint32_t kOrphanParamIndexBase = 0x100000;
// Is `name` the base key of pack param `i`, or one of its components ("key#c")? `comp` receives c (-1 for the base key).
bool MatchPackParamName(const ShaderPackParam& sp, const std::string& name, int& comp) {
    if (sp.key == name) { comp = -1; return true; }
    if (name.size() > sp.key.size() + 1 && name.compare(0, sp.key.size(), sp.key) == 0 &&
        name[sp.key.size()] == '#') {
        comp = name[sp.key.size() + 1] - '0';
        return comp >= 0 && comp <= 3;
    }
    return false;
}
// The components a pack param offers: 1 for a scalar, 3 for a Vec3, 3 (or 4 with alpha) for a Color.
int EffectiveParamComponents(const ShaderPackParam& sp) {
    if (sp.type == ShaderPackParamType::Vec3) return 3;
    if (sp.type == ShaderPackParamType::Color) return sp.alpha ? 4 : 3;
    return 1;
}
// The row index of the track `name` in the pack scheme; false when the name matches no pack param component (the pack
// is missing, or the param was removed). A Vec3 / Color param is addressed through its components only.
bool PackParamIndexOfTrack(const ShaderPack* pk, const std::string& name, uint32_t& index) {
    if (!pk) return false;
    for (uint32_t i = 0; i < (uint32_t)pk->params.size(); ++i) {
        const ShaderPackParam& sp = pk->params[i];
        int comp = -1;
        if (!MatchPackParamName(sp, name, comp)) continue;
        const int comps = EffectiveParamComponents(sp);
        if (comps == 1) {
            if (comp < 0) { index = i * 4; return true; }
            continue;
        }
        if (comp < 0 || comp >= comps) continue;
        index = i * 4 + (uint32_t)comp;
        return true;
    }
    return false;
}
// The track name of pack param `i` component `comp` (comp -1 = the param's own scalar track).
std::string PackParamTrackName(const ShaderPackParam& sp, int comp) {
    if (comp < 0) return sp.key;
    return sp.key + "#" + std::to_string(comp);
}
// The track name of a row index. `eff.tracks` only lists keyed params, but the name (and so the track) of a component
// is known from the pack; the index is stable whether or not the track exists.
std::string EffectParamNameOfIndex(const StudioEffect& eff, uint32_t pidx) {
    const ShaderPack* pk = ShaderPacks().Find(eff.entry.pack);
    if (pk) {
        for (uint32_t i = 0; i < (uint32_t)pk->params.size(); ++i) {
            const ShaderPackParam& sp = pk->params[i];
            const uint32_t base = i * 4;
            const int comps = EffectiveParamComponents(sp);
            if (pidx < base || pidx >= base + (uint32_t)comps) continue;
            return PackParamTrackName(sp, comps == 1 ? -1 : (int)(pidx - base));
        }
    }
    uint32_t n = 0;
    for (const auto& t : eff.tracks) {
        bool inPack = false;
        if (pk) {
            for (const ShaderPackParam& sp : pk->params) {
                int comp = -1;
                if (MatchPackParamName(sp, t.param, comp)) { inPack = true; break; }
            }
        }
        if (inPack) continue;
        if (kOrphanParamIndexBase + n == pidx) return t.param;
        ++n;
    }
    return {};
}

uint32_t EffectParamIndexOfName(const StudioEffect& eff, const std::string& paramName) {
    const ShaderPack* pk = ShaderPacks().Find(eff.entry.pack);
    uint32_t index = 0;
    if (PackParamIndexOfTrack(pk, paramName, index)) return index;
    uint32_t n = 0;
    for (const auto& t : eff.tracks) {
        bool inPack = false;
        if (pk) {
            for (const ShaderPackParam& sp : pk->params) {
                int comp = -1;
                if (MatchPackParamName(sp, t.param, comp)) { inPack = true; break; }
            }
        }
        if (inPack) continue;
        if (t.param == paramName) return kOrphanParamIndexBase + n;
        ++n;
    }
    return UINT32_MAX;
}

// The pack param a track name belongs to ("key" or "key#c"), or nullptr when it matches no pack param.
const ShaderPackParam* PackParamOfTrack(const ShaderPack* pk, const std::string& param) {
    if (!pk) return nullptr;
    for (const ShaderPackParam& sp : pk->params) {
        int comp = -1;
        if (MatchPackParamName(sp, param, comp)) return &sp;
    }
    return nullptr;
}
// A param track's value without keys: the effect's static value, else the pack default (alpha: defVec[3]).
float ParamStaticValue(const StudioEffect& fx, const std::string& param) {
    float def = 0.0f;
    int comp = -1;
    if (const size_t hash = param.find('#'); hash != std::string::npos && hash + 1 < param.size())
        comp = param[hash + 1] - '0';
    if (const ShaderPackParam* sp = PackParamOfTrack(ShaderPacks().Find(fx.entry.pack), param)) {
        if (comp >= 0) def = sp->defVec[comp];
        else if (sp->type == ShaderPackParamType::Bool) def = sp->defBool ? 1.0f : 0.0f;
        else def = sp->def;
    }
    if (const auto it = fx.entry.params.find(param); it != fx.entry.params.end()) def = it->second;
    return def;
}
// A key of this param track holds its value until the next one (Int / Bool components, as the inspector keyed them).
bool ParamTrackStepped(const StudioEffect& fx, const std::string& param) {
    if (param.find('#') != std::string::npos) return false;
    const ShaderPackParam* sp = PackParamOfTrack(ShaderPacks().Find(fx.entry.pack), param);
    return sp && (sp->type == ShaderPackParamType::Int || sp->type == ShaderPackParamType::Bool);
}
// A param track's keys, or nullptr when it has no track yet.
EffectParamTrack* FindParamTrack(StudioEffect& fx, const std::string& param) {
    for (EffectParamTrack& t : fx.tracks)
        if (t.param == param) return &t;
    return nullptr;
}
// S3: the same for a stage's keyed params.
EffectParamTrack* FindStageParamTrack(StudioStage& stage, const std::string& param) {
    for (EffectParamTrack& t : stage.tracks)
        if (t.param == param) return &t;
    return nullptr;
}
const EffectParamTrack* FindStageParamTrack(const StudioStage& stage, const std::string& param) {
    for (const EffectParamTrack& t : stage.tracks)
        if (t.param == param) return &t;
    return nullptr;
}
// The confirm modal / row label of a param: "<pack or entity name> · <param label>" like the timeline rows.
std::string ParamRowLabel(const StudioEffect& fx, const std::string& entityName, const std::string& param) {
    const ShaderPack* pk = ShaderPacks().Find(fx.entry.pack);
    std::string label = param;
    std::string baseKey = param;
    std::string compSuffix;
    if (const size_t hash = param.find('#'); hash != std::string::npos) {
        baseKey = param.substr(0, hash);
        compSuffix = param.substr(hash);
    }
    if (pk) {
        for (const ShaderPackParam& sp : pk->params) {
            if (sp.key != baseKey) continue;
            if (!sp.label.Empty()) label = sp.label.Get(PackLanguage());
            if (compSuffix.size() > 1) {
                const int c = compSuffix[1] - '0';
                if (sp.type == ShaderPackParamType::Color) {
                    static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
                    if (c >= 0 && c < 4) label += kCol[c];
                } else {
                    static const char* kVec[3] = {" · X", " · Y", " · Z"};
                    if (c >= 0 && c < 3) label += kVec[c];
                }
            }
            break;
        }
    }
    const std::string prefix = entityName.empty() ? (pk ? pk->name.Get(PackLanguage()) : fx.entry.pack) : entityName;
    return prefix + " · " + label;
}

// ---- S3: stage param rows (RowKind::StageParam) ----
// The manifest params take the pack scheme: param i scalar -> i * 4, a colour -> i * 4 + c (c 0..3), so a row exists
// for every component before it is keyed. A track that matches no manifest param (the stage changed): 0x100000 + n.
int StageParamComponents(const StageParam& sp) { return sp.type == StageParamType::Color ? 4 : 1; }
// Is `name` stage param `sp` itself, or one of its colour components ("key#c")? `comp` receives c (-1 for the key).
bool MatchStageParamName(const StageParam& sp, const std::string& name, int& comp) {
    if (sp.key == name) { comp = -1; return true; }
    if (sp.type != StageParamType::Color) return false;
    if (name.size() != sp.key.size() + 2 || name.compare(0, sp.key.size(), sp.key) != 0 || name[sp.key.size()] != '#')
        return false;
    comp = name[sp.key.size() + 1] - '0';
    return comp >= 0 && comp <= 3;
}
std::string StageParamName(const StageParam& sp, int comp) {
    if (comp < 0) return sp.key;
    return sp.key + "#" + std::to_string(comp);
}
// The row index of the track `name` in the stage scheme (see above); false when it matches no manifest param.
bool StageParamIndexOfTrack(const StudioStage& stage, const std::string& name, uint32_t& index) {
    if (!stage.manifest) return false;
    for (uint32_t i = 0; i < (uint32_t)stage.manifest->params.size(); ++i) {
        const StageParam& sp = stage.manifest->params[i];
        int comp = -1;
        if (!MatchStageParamName(sp, name, comp)) continue;
        if (comp >= 0 && comp >= StageParamComponents(sp)) continue;
        index = i * 4 + (uint32_t)std::max(0, comp);
        return true;
    }
    return false;
}
std::string StageParamNameOfIndex(const StudioStage& stage, uint32_t pidx) {
    if (stage.manifest) {
        for (uint32_t i = 0; i < (uint32_t)stage.manifest->params.size(); ++i) {
            const StageParam& sp = stage.manifest->params[i];
            const uint32_t base = i * 4;
            if (pidx < base || pidx >= base + (uint32_t)StageParamComponents(sp)) continue;
            return StageParamName(sp, StageParamComponents(sp) == 1 ? -1 : (int)(pidx - base));
        }
    }
    uint32_t n = 0;
    for (const EffectParamTrack& t : stage.tracks) {
        uint32_t known = 0;
        if (StageParamIndexOfTrack(stage, t.param, known)) continue;   // a manifest param: its rows are its own
        if (kOrphanParamIndexBase + n == pidx) return t.param;
        ++n;
    }
    return {};
}
uint32_t StageParamIndexOfName(const StudioStage& stage, const std::string& paramName) {
    uint32_t index = 0;
    if (StageParamIndexOfTrack(stage, paramName, index)) return index;
    uint32_t n = 0;
    for (const EffectParamTrack& t : stage.tracks) {
        uint32_t known = 0;
        if (StageParamIndexOfTrack(stage, t.param, known)) continue;
        if (t.param == paramName) return kOrphanParamIndexBase + n;
        ++n;
    }
    return UINT32_MAX;
}
// A key of a stage param track holds its value until the next one (int / bool / enum, and colours no).
bool StageParamTrackStepped(const StudioStage& stage, const std::string& param) {
    int comp = -1;
    const std::string key = StageParamBaseKey(param, comp);
    if (!stage.manifest) return false;
    const StageParam* sp = stage.manifest->FindParam(key);
    return sp && sp->type != StageParamType::Float && sp->type != StageParamType::Color;
}
// The label of a stage param track ("· R" for a colour component), for a row / confirm modal.
std::string StageParamLabel(const StudioStage& stage, const std::string& param) {
    int comp = -1;
    const std::string key = StageParamBaseKey(param, comp);
    std::string label = key;
    if (stage.manifest) {
        if (const StageParam* sp = stage.manifest->FindParam(key)) {
            if (!sp->label.Empty()) label = sp->label.Get(StageLanguage());
        }
    }
    if (comp >= 0) {
        static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
        label += kCol[comp];
    }
    return label;
}
} // namespace

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool App::StudioTrackOfRow(uint64_t row, RowKind& kind, std::string& name) const {
    const StudioDoc& d = *studio_;
    kind = RowKindOf(row);
    name.clear();
    if (kind == RowKind::GraphLane || kind == RowKind::GraphChannel) return false;  // graph rows carry no track / keys
    if (IsCameraKind(kind)) return d.selectedModel < 0;
    if (kind == RowKind::SceneLight) {
        // a light row is only offered through the camera target (the lights live next to the camera)
        const uint32_t uid = RowIndexOf(row);
        if (d.selectedModel >= 0 || !d.FindLight(uid)) return false;
        name = LightTrackName(uid);
        return true;
    }
    if (kind == RowKind::EffectParam) {
        if (d.selectedModel >= 0) return false;
        const uint32_t uid = RowGroupOf(row);
        const uint32_t pidx = RowIndexOf(row);
        const StudioEffect* eff = d.FindEffect(uid);
        if (!eff) return false;
        const std::string param = EffectParamNameOfIndex(*eff, pidx);
        if (param.empty()) return false;
        name = EffectParamTrackName(uid, param);
        return true;
    }
    if (kind == RowKind::WorldParam) {
        if (d.selectedModel >= 0) return false;
        const uint32_t uid = RowGroupOf(row);
        const uint32_t pidx = RowIndexOf(row);
        const StudioWorld* world = d.FindWorld(uid);
        if (!world) return false;
        const std::string param = EffectParamNameOfIndex(world->fx, pidx);
        if (param.empty()) return false;
        name = WorldParamTrackName(uid, param);
        return true;
    }
    if (kind == RowKind::StageParam) {
        // the stage rows sit under the camera target, and under the stage itself while one of its parts is selected
        const uint32_t uid = RowGroupOf(row);
        if (d.selectedModel >= 0 && (!d.Selected() || d.Selected()->stageUid != uid)) return false;
        const uint32_t pidx = RowIndexOf(row);
        const StudioStage* stage = d.FindStage(uid);
        if (!stage) return false;
        const std::string param = StageParamNameOfIndex(*stage, pidx);
        if (param.empty()) return false;
        name = StageParamTrackName(uid, param);
        return true;
    }
    if (d.selectedModel < 0 || d.selectedModel >= (int)d.models.size()) return false;
    const PmxModel& pmx = *d.models[d.selectedModel]->pmx;
    const uint32_t idx = RowIndexOf(row);
    if (kind == RowKind::Accessory) return d.models[d.selectedModel]->IsProp();
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
    for (const TrackState& s : before) after.push_back(CaptureTrack(d, s));
    d.history.Push(std::make_unique<TrackEditCommand>(d, name, before, std::move(after)));
}

// Viewport navigation starts from the motion camera's current view, so the free camera begins where it was.
void App::StudioTakeFreeCamera() {
    StudioDoc& d = *studio_;
    const std::shared_ptr<CameraMotion>& eval = StudioShownCameraEval();  // the camera the viewport shows
    if (!d.useMotionCamera || !eval) return;
    freeCam_ = FreeFromPose(eval->Evaluate((float)(d.time * kMmdFps)));
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
    // S4: the keys the camera rows edit (the selected camera); an extra camera without keys falls back to the view
    const std::vector<CameraKf>& keys = d.SelectedCameraTrack().camera;
    if (!d.useMotionCamera || keys.empty()) {
        // the view the user is looking at
        FillLinearCameraInterp(k.interp);
        PoseFromFree(freeCam_.target, freeCam_.yaw, freeCam_.pitch, freeCam_.distance, freeCam_.fovDeg, k);
        k.frame = frame;
    } else {
        k = SampleCamera(keys, frame);
    }
    return k;
}

const std::shared_ptr<CameraMotion>& App::StudioShownCameraEval() const {
    const StudioDoc& d = *studio_;
    if (d.possessCamera && d.selectedCameraUid != 0 && d.SelectedCameraEval()) return d.SelectedCameraEval();
    return d.cameraEval;
}

CameraKf App::StudioViewportCameraKey(int frame) const {
    const StudioDoc& d = *studio_;
    CameraKf k;
    const std::vector<CameraKf>& shown = d.possessCamera ? d.SelectedCameraTrack().camera : d.camera.camera;
    if (d.useMotionCamera && !shown.empty()) {
        k = SampleCamera(shown, frame);   // looking through the main camera (or the possessed one)
    } else {
        FillLinearCameraInterp(k.interp);
        PoseFromFree(freeCam_.target, freeCam_.yaw, freeCam_.pitch, freeCam_.distance, freeCam_.fovDeg, k);
    }
    k.frame = frame;
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
            // S4: the selected camera's row; the key keeps what the view shows now
            UpsertKey(d.SelectedCameraTrack().camera, StudioViewedCamera(frame));
        } else if (kind == RowKind::Light) {
            // a new key keeps the current interpolated value (empty track: the sun's light)
            LightKf k = d.camera.light.empty() ? StudioSunLight((float)frame) : SampleLight(d.camera.light, (float)frame);
            k.frame = frame;
            UpsertKey(d.camera.light, k);
        } else if (kind == RowKind::Shadow) {
            ShadowKf k = d.camera.shadow.empty() ? ShadowKf{frame, 1, 0.01125f} : SampleShadow(d.camera.shadow, (float)frame);
            k.frame = frame;
            UpsertKey(d.camera.shadow, k);
        } else if (kind == RowKind::Focus) {
            UpsertKey(d.camera.focus, StudioNewFocusKey(frame));
        } else if (kind == RowKind::Accessory) {
            StudioModel* m = d.Selected();
            if (m && m->IsProp()) {
                AccessoryKf k;
                if (m->motion.accessory.empty()) k = StudioNewAccessoryKey(*m, frame);
                else k = SampleAccessory(m->motion.accessory, (float)frame);
                k.frame = frame;
                UpsertKey(m->motion.accessory, k);
            }
        } else if (kind == RowKind::SceneLight) {
            // a new light key keeps the light's current value (sampled where keys exist)
            if (SceneLight* light = d.FindLight(LightUidOfTrack(name)))
                UpsertKey(light->keys, LightKey{frame, SampleLightValues(*light, frame)});
        } else if (kind == RowKind::EffectParam) {
            uint32_t effectUid = 0;
            std::string param;
            if (ParseEffectParamTrackName(name, effectUid, param)) {
                if (StudioEffect* eff = d.FindEffect(effectUid)) {
                    EffectParamTrack* tr = nullptr;
                    for (auto& t : eff->tracks) {
                        if (t.param == param) { tr = &t; break; }
                    }
                    if (!tr) {
                        eff->tracks.push_back({param, {}});
                        tr = &eff->tracks.back();
                    }
                    float fallback = 0.0f;
                    if (auto it = eff->entry.params.find(param); it != eff->entry.params.end())
                        fallback = it->second;
                    else {
                        const ShaderPack* pk = ShaderPacks().Find(eff->entry.pack);
                        if (pk) {
                            for (const auto& sp : pk->params) {
                                if (sp.key == param) {
                                    fallback = (sp.type == ShaderPackParamType::Bool ? (sp.defBool ? 1.0f : 0.0f) : sp.def);
                                    break;
                                }
                                for (int c = 0; c < 4; ++c) {
                                    if (param == sp.key + "#" + std::to_string(c)) {
                                        fallback = sp.defVec[c];
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    float val = EvaluateParamTrack(*tr, (float)frame, fallback);
                    UpsertParamKey(*tr, EffectParamKey{frame, val, false});
                }
            }
        } else if (kind == RowKind::WorldParam) {
            uint32_t worldUid = 0;
            std::string param;
            if (ParseWorldParamTrackName(name, worldUid, param)) {
                if (StudioWorld* world = d.FindWorld(worldUid)) {
                    EffectParamTrack* tr = nullptr;
                    for (auto& t : world->fx.tracks) {
                        if (t.param == param) { tr = &t; break; }
                    }
                    if (!tr) {
                        world->fx.tracks.push_back({param, {}});
                        tr = &world->fx.tracks.back();
                    }
                    float fallback = 0.0f;
                    if (auto it = world->fx.entry.params.find(param); it != world->fx.entry.params.end())
                        fallback = it->second;
                    else {
                        const ShaderPack* pk = ShaderPacks().Find(world->fx.entry.pack);
                        if (pk) {
                            for (const auto& sp : pk->params) {
                                if (sp.key == param) {
                                    fallback = (sp.type == ShaderPackParamType::Bool ? (sp.defBool ? 1.0f : 0.0f) : sp.def);
                                    break;
                                }
                                for (int c = 0; c < 4; ++c) {
                                    if (param == sp.key + "#" + std::to_string(c)) {
                                        fallback = sp.defVec[c];
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    float val = EvaluateParamTrack(*tr, (float)frame, fallback);
                    UpsertParamKey(*tr, EffectParamKey{frame, val, false});
                }
            }
        } else if (kind == RowKind::StageParam) {
            uint32_t stageUid = 0;
            std::string param;
            if (ParseStageParamTrackName(name, stageUid, param)) {
                if (StudioStage* stage = d.FindStage(stageUid)) {
                    EffectParamTrack* tr = FindStageParamTrack(*stage, param);
                    if (!tr) {
                        stage->tracks.push_back({param, {}});
                        tr = &stage->tracks.back();
                    }
                    const float val = EvaluateParamTrack(*tr, (float)frame, StageParamStaticValue(*stage, param));
                    UpsertParamKey(*tr, EffectParamKey{frame, val, StageParamTrackStepped(*stage, param)});
                }
            }
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
        add(d.SelectedCameraTrack().camera);
    } else if (kind == RowKind::Light) {
        add(d.camera.light);
    } else if (kind == RowKind::Shadow) {
        add(d.camera.shadow);
    } else if (kind == RowKind::Focus) {
        add(d.camera.focus);
    } else if (kind == RowKind::Accessory) {
        if (d.models[d.selectedModel]->IsProp()) add(d.models[d.selectedModel]->motion.accessory);
    } else if (kind == RowKind::SceneLight) {
        if (const SceneLight* light = d.FindLight(LightUidOfTrack(name))) add(light->keys);
    } else if (kind == RowKind::EffectParam) {
        uint32_t effectUid = 0;
        std::string param;
        if (ParseEffectParamTrackName(name, effectUid, param)) {
            if (const StudioEffect* eff = d.FindEffect(effectUid)) {
                for (const auto& t : eff->tracks) {
                    if (t.param == param) { add(t.keys); break; }
                }
            }
        }
    } else if (kind == RowKind::WorldParam) {
        uint32_t worldUid = 0;
        std::string param;
        if (ParseWorldParamTrackName(name, worldUid, param)) {
            if (const StudioWorld* world = d.FindWorld(worldUid)) {
                for (const auto& t : world->fx.tracks) {
                    if (t.param == param) { add(t.keys); break; }
                }
            }
        }
    } else if (kind == RowKind::StageParam) {
        uint32_t stageUid = 0;
        std::string param;
        if (ParseStageParamTrackName(name, stageUid, param)) {
            if (const StudioStage* stage = d.FindStage(stageUid))
                if (const EffectParamTrack* t = FindStageParamTrack(*stage, param)) add(t->keys);
        }
    } else if (d.selectedModel < d.models.size()) {
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
        all.insert(MakeRowId(RowKind::Focus, 0, 0));
        for (const SceneLight& l : d.lights)
            if (l.kind != LightKind::Ambient) all.insert(MakeRowId(RowKind::SceneLight, 0, l.uid));
        for (const auto& eff : d.effects) {
            for (const auto& tr : eff.tracks) {
                if (!tr.keys.empty()) {
                    uint32_t pidx = EffectParamIndexOfName(eff, tr.param);
                    if (pidx != UINT32_MAX) all.insert(MakeRowId(RowKind::EffectParam, eff.uid, pidx));
                }
            }
        }
        for (const auto& world : d.worlds) {
            for (const auto& tr : world.fx.tracks) {
                if (!tr.keys.empty()) {
                    uint32_t pidx = EffectParamIndexOfName(world.fx, tr.param);
                    if (pidx != UINT32_MAX) all.insert(MakeRowId(RowKind::WorldParam, world.uid, pidx));
                }
            }
        }
        for (const auto& stage : d.stages) {
            for (const auto& tr : stage.tracks) {
                if (!tr.keys.empty()) {
                    const uint32_t pidx = StageParamIndexOfName(stage, tr.param);
                    if (pidx != UINT32_MAX) all.insert(MakeRowId(RowKind::StageParam, stage.uid, pidx));
                }
            }
        }
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
            else if (kind == RowKind::Focus) EraseKeyFrames(d.camera.focus, fs);
            else EraseKeyFrames(d.SelectedCameraTrack().camera, fs);
        }
        else if (kind == RowKind::SceneLight) {
            if (SceneLight* light = d.FindLight(LightUidOfTrack(track.second))) EraseKeyFrames(light->keys, fs);
        }
        else if (kind == RowKind::EffectParam) {
            uint32_t effectUid = 0;
            std::string param;
            if (ParseEffectParamTrackName(track.second, effectUid, param)) {
                if (StudioEffect* eff = d.FindEffect(effectUid)) {
                    for (auto& t : eff->tracks) {
                        if (t.param == param) {
                            std::vector<int32_t> fvec(fs.begin(), fs.end());
                            EraseParamKeys(t, fvec);
                            break;
                        }
                    }
                }
            }
        }
        else if (kind == RowKind::WorldParam) {
            uint32_t worldUid = 0;
            std::string param;
            if (ParseWorldParamTrackName(track.second, worldUid, param)) {
                if (StudioWorld* world = d.FindWorld(worldUid)) {
                    for (auto& t : world->fx.tracks) {
                        if (t.param == param) {
                            std::vector<int32_t> fvec(fs.begin(), fs.end());
                            EraseParamKeys(t, fvec);
                            break;
                        }
                    }
                }
            }
        }
        else if (kind == RowKind::StageParam) {
            uint32_t stageUid = 0;
            std::string param;
            if (ParseStageParamTrackName(track.second, stageUid, param)) {
                if (StudioStage* stage = d.FindStage(stageUid)) {
                    if (EffectParamTrack* t = FindStageParamTrack(*stage, param)) {
                        std::vector<int32_t> fvec(fs.begin(), fs.end());
                        EraseParamKeys(*t, fvec);
                    }
                }
            }
        }
        else if (kind == RowKind::Bone) EraseKeyFrames(d.models[d.selectedModel]->motion.bones[track.second], fs);
        else if (kind == RowKind::Morph) EraseKeyFrames(d.models[d.selectedModel]->motion.morphs[track.second], fs);
        else if (kind == RowKind::Accessory) EraseKeyFrames(d.models[d.selectedModel]->motion.accessory, fs);
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
            } else if (kind == RowKind::Focus) {
                if (const FocusKf* p = FindKey(d.camera.focus, k.second)) { c.focus = *p; found = true; }
            } else if (const CameraKf* p = FindKey(d.SelectedCameraTrack().camera, k.second)) { c.camera = *p; found = true; }
        } else if (kind == RowKind::SceneLight) {
            if (const SceneLight* light = d.FindLight(LightUidOfTrack(name)))
                if (const LightKey* p = FindKey(light->keys, k.second)) { c.sceneLight = *p; found = true; }
        } else if (kind == RowKind::EffectParam) {
            uint32_t effectUid = 0;
            std::string param;
            if (ParseEffectParamTrackName(name, effectUid, param)) {
                if (const StudioEffect* eff = d.FindEffect(effectUid)) {
                    for (const auto& t : eff->tracks) {
                        if (t.param == param) {
                            for (const auto& pk : t.keys) {
                                if (pk.frame == k.second) {
                                    c.effectParam = pk;
                                    found = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        } else if (kind == RowKind::WorldParam) {
            uint32_t worldUid = 0;
            std::string param;
            if (ParseWorldParamTrackName(name, worldUid, param)) {
                if (const StudioWorld* world = d.FindWorld(worldUid)) {
                    for (const auto& t : world->fx.tracks) {
                        if (t.param == param) {
                            for (const auto& pk : t.keys) {
                                if (pk.frame == k.second) {
                                    c.worldParam = pk;
                                    found = true;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        } else if (kind == RowKind::StageParam) {
            uint32_t stageUid = 0;
            std::string param;
            if (ParseStageParamTrackName(name, stageUid, param)) {
                if (const StudioStage* stage = d.FindStage(stageUid)) {
                    if (const EffectParamTrack* t = FindStageParamTrack(*stage, param)) {
                        for (const auto& pk : t->keys) {
                            if (pk.frame == k.second) {
                                c.stageParam = pk;
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
        } else if (kind == RowKind::Bone) {
            const auto& t = d.models[d.selectedModel]->motion.bones;
            if (auto it = t.find(name); it != t.end())
                if (const BoneKf* p = FindKey(it->second, k.second)) { c.bone = *p; found = true; }
        } else if (kind == RowKind::Accessory) {
            const auto& t = d.models[d.selectedModel]->motion.accessory;
            if (const AccessoryKf* p = FindKey(t, k.second)) { c.accessory = *p; found = true; }
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

void App::StudioPaste(bool curvesOnly, bool mirrored) {
    OpTimer timer{"StudioPaste"};
    StudioDoc& d = *studio_;
    if (d.clipboard.empty() || d.clipboardModel != d.selectedModel) return;
    const int at = d.Frame();
    // Mirrored: a bone key lands on the other side's bone (左 <-> 右) with its pose reflected (MirrorPose); centre
    // bones are reflected in place. Morph and camera keys paste unchanged.
    const auto target = [&](const ClipboardKey& c, RowKind& kind, std::string& name, uint64_t& row) {
        row = c.row;
        if (!StudioTrackOfRow(c.row, kind, name)) return false;
        if (mirrored && kind == RowKind::Bone) {
            const StudioModel& m = *d.models[d.selectedModel];
            const int other = MirrorBoneIndex(*m.pmx, (int)RowIndexOf(c.row));
            name = m.pmx->bones[other].name;
            row = CanonicalRow(m, RowKind::Bone, (uint32_t)other);
        }
        return true;
    };
    std::vector<TrackState> before;
    std::set<std::pair<int, std::string>> seen;
    for (const ClipboardKey& c : d.clipboard) {
        RowKind kind;
        std::string name;
        uint64_t row;
        if (target(c, kind, name, row) && seen.insert({(int)kind, name}).second)
            before.push_back(CaptureTrack(d, d.selectedModel, kind, name));
    }
    std::set<KeyId> pasted;
    for (const ClipboardKey& c0 : d.clipboard) {
        RowKind kind;
        std::string name;
        uint64_t row;
        if (!target(c0, kind, name, row)) continue;
        ClipboardKey c = c0;
        if (mirrored && kind == RowKind::Bone) {
            const PoseBone mp = MirrorPose(PoseBone{c.bone.t, c.bone.r});
            c.bone.t = mp.t;
            c.bone.r = mp.r;
        }
        const int f = at + c.offset;
        if (curvesOnly) {
            // interpolation only, onto keys that already exist at the target frames (morph keys have none)
            if (kind == RowKind::Camera) {
                CameraKf* k = FindKey(d.SelectedCameraTrack().camera, f);
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
        } else if (kind == RowKind::Camera) { CameraKf k = c.camera; k.frame = f; UpsertKey(d.SelectedCameraTrack().camera, k); }
        else if (kind == RowKind::Light) { LightKf k = c.light; k.frame = f; UpsertKey(d.camera.light, k); }
        else if (kind == RowKind::Shadow) { ShadowKf k = c.shadow; k.frame = f; UpsertKey(d.camera.shadow, k); }
        else if (kind == RowKind::Focus) { FocusKf k = c.focus; k.frame = f; UpsertKey(d.camera.focus, k); }
        else if (kind == RowKind::Accessory) { AccessoryKf k = c.accessory; k.frame = f; UpsertKey(d.models[d.selectedModel]->motion.accessory, k); }
        else if (kind == RowKind::SceneLight) {
            if (SceneLight* light = d.FindLight(LightUidOfTrack(name))) { LightKey k = c.sceneLight; k.frame = f; UpsertKey(light->keys, k); }
        }
        else if (kind == RowKind::EffectParam) {
            uint32_t effectUid = 0;
            std::string param;
            if (ParseEffectParamTrackName(name, effectUid, param)) {
                if (StudioEffect* eff = d.FindEffect(effectUid)) {
                    EffectParamTrack* tr = nullptr;
                    for (auto& t : eff->tracks) {
                        if (t.param == param) { tr = &t; break; }
                    }
                    if (!tr) {
                        eff->tracks.push_back({param, {}});
                        tr = &eff->tracks.back();
                    }
                    EffectParamKey pk = c.effectParam;
                    pk.frame = f;
                    UpsertParamKey(*tr, pk);
                }
            }
        }
        else if (kind == RowKind::WorldParam) {
            uint32_t worldUid = 0;
            std::string param;
            if (ParseWorldParamTrackName(name, worldUid, param)) {
                if (StudioWorld* world = d.FindWorld(worldUid)) {
                    EffectParamTrack* tr = nullptr;
                    for (auto& t : world->fx.tracks) {
                        if (t.param == param) { tr = &t; break; }
                    }
                    if (!tr) {
                        world->fx.tracks.push_back({param, {}});
                        tr = &world->fx.tracks.back();
                    }
                    EffectParamKey pk = c.worldParam;
                    pk.frame = f;
                    UpsertParamKey(*tr, pk);
                }
            }
        }
        else if (kind == RowKind::StageParam) {
            uint32_t stageUid = 0;
            std::string param;
            if (ParseStageParamTrackName(name, stageUid, param)) {
                if (StudioStage* stage = d.FindStage(stageUid)) {
                    EffectParamTrack* tr = FindStageParamTrack(*stage, param);
                    if (!tr) {
                        stage->tracks.push_back({param, {}});
                        tr = &stage->tracks.back();
                    }
                    EffectParamKey pk = c.stageParam;
                    pk.frame = f;
                    UpsertParamKey(*tr, pk);
                }
            }
        }
        else if (kind == RowKind::Bone) { BoneKf k = c.bone; k.frame = f; UpsertKey(d.models[d.selectedModel]->motion.bones[name], k); }
        else { MorphKf k = c.morph; k.frame = f; UpsertKey(d.models[d.selectedModel]->motion.morphs[name], k); }
        pasted.insert({row, f});
    }
    if (pasted.empty()) return;  // curve paste with no key at any target frame: nothing changed
    StudioPushTrackEdit(curvesOnly ? Tr("곡선만 붙여넣기") : (mirrored ? Tr("좌우 반전 붙여넣기") : Tr("키 붙여넣기")), before);
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
                apply(d.SelectedCameraTrack().camera);
            } else if (kind == RowKind::Light) {
                apply(d.camera.light);
            } else if (kind == RowKind::Shadow) {
                apply(d.camera.shadow);
            } else if (kind == RowKind::Focus) {
                apply(d.camera.focus);
            } else if (kind == RowKind::Accessory) {
                if (d.models[d.selectedModel]->IsProp()) apply(d.models[d.selectedModel]->motion.accessory);
            } else if (kind == RowKind::SceneLight) {
                if (SceneLight* light = d.FindLight(LightUidOfTrack(name))) apply(light->keys);
            } else if (kind == RowKind::EffectParam) {
                uint32_t effectUid = 0;
                std::string param;
                if (ParseEffectParamTrackName(name, effectUid, param)) {
                    if (StudioEffect* eff = d.FindEffect(effectUid)) {
                        for (auto& t : eff->tracks) {
                            if (t.param == param) { apply(t.keys); break; }
                        }
                    }
                }
            } else if (kind == RowKind::WorldParam) {
                uint32_t worldUid = 0;
                std::string param;
                if (ParseWorldParamTrackName(name, worldUid, param)) {
                    if (StudioWorld* world = d.FindWorld(worldUid)) {
                        for (auto& t : world->fx.tracks) {
                            if (t.param == param) { apply(t.keys); break; }
                        }
                    }
                }
            } else if (kind == RowKind::StageParam) {
                uint32_t stageUid = 0;
                std::string param;
                if (ParseStageParamTrackName(name, stageUid, param)) {
                    if (StudioStage* stage = d.FindStage(stageUid))
                        if (EffectParamTrack* t = FindStageParamTrack(*stage, param)) apply(t->keys);
                }
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
            if (const CameraKf* c = FindKey(d.SelectedCameraTrack().camera, k.second)) {
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
            if (CameraKf* c = FindKey(d.SelectedCameraTrack().camera, k.second)) std::memcpy(c->interp, d.curveClip, sizeof(c->interp));
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

void App::StudioRowStopwatch(uint64_t rowId) {
    StudioDoc& d = *studio_;
    RowKind kind;
    std::string name;
    if (!StudioTrackOfRow(rowId, kind, name)) return;
    const int frame = d.Frame();

    if (kind == RowKind::Bone) {
        StudioModel* m = StudioPoseModel();
        if (!m) return;
        const uint32_t bone = RowIndexOf(rowId);
        if (bone >= m->pmx->bones.size()) return;
        const auto it = m->motion.bones.find(name);
        const bool animated = it != m->motion.bones.end() && !it->second.empty() &&
                              !IsStaticBoneTrack(m->motion, name);
        std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Bone, name)};
        if (!animated) {
            const PoseBone cur0 = StudioCurrentPose(*m)[(size_t)bone];
            BoneKf k;
            FillLinearInterp(k.interp);
            k.frame = frame;
            k.t = cur0.t;
            k.r = cur0.r;
            UpsertKey(m->motion.bones[name], k);
            m->motion.staticBones.erase(name);
            StudioPushTrackEdit(Tr("애니메이션 켜기"), before);
        } else {
            studioStopwatchLabel_ = name;
            studioStopwatchOff_ = [this, &d, bi = bone, bname = name] {
                StudioModel* mm = d.Selected();
                if (!mm) return;
                std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Bone, bname)};
                const PoseBone cur1 = StudioCurrentPose(*mm)[(size_t)bi];
                BoneKf k;
                FillLinearInterp(k.interp);
                k.frame = 0;
                k.t = cur1.t;
                k.r = cur1.r;
                mm->motion.bones[bname] = {k};
                mm->motion.staticBones.insert(bname);
                StudioPushTrackEdit(Tr("애니메이션 끄기"), before);
                ++mm->motionVersion;
                d.rowsKey = ~0ull;
            };
        }
        ++m->motionVersion;
        d.rowsKey = ~0ull;
    } else if (kind == RowKind::Morph) {
        StudioModel* m = StudioPoseModel();
        if (!m) return;
        const uint32_t morphIdx = RowIndexOf(rowId);
        if (morphIdx >= m->pmx->morphs.size()) return;
        StudioSelectMorph((int)morphIdx);
        const auto mt = m->motion.morphs.find(name);
        const bool animated = mt != m->motion.morphs.end() && !mt->second.empty() &&
                              !IsStaticMorphTrack(m->motion, name);
        const bool staticTrack = IsStaticMorphTrack(m->motion, name);
        const float v = m->inst->MorphWeight((int)morphIdx);
        std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Morph, name)};
        if (!animated) {
            UpsertKey(m->motion.morphs[name], MorphKf{frame, v});
            if (staticTrack) m->motion.staticMorphs.erase(name);
            StudioPushTrackEdit(Tr("애니메이션 켜기"), before);
        } else {
            studioStopwatchLabel_ = name;
            studioStopwatchOff_ = [this, &d, mi = (int)morphIdx, mname = name] {
                StudioModel* mm = d.Selected();
                if (!mm) return;
                std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Morph, mname)};
                const auto kt = mm->motion.morphs.find(mname);
                float val = kt != mm->motion.morphs.end() && !kt->second.empty()
                                ? SampleMorph(kt->second, d.Frame()) : mm->inst->MorphWeight(mi);
                mm->motion.morphs[mname] = {MorphKf{0, val}};
                mm->motion.staticMorphs.insert(mname);
                StudioPushTrackEdit(Tr("애니메이션 끄기"), before);
                ++mm->motionVersion;
                d.rowsKey = ~0ull;
            };
        }
        ++m->motionVersion;
        d.rowsKey = ~0ull;
    } else if (kind == RowKind::SceneLight) {
        const uint32_t uid = RowIndexOf(rowId);
        SceneLight* light = d.FindLight(uid);
        if (!light) return;
        const bool animated = !light->keys.empty();
        if (!animated) {
            StudioBeginLightEdit(light->uid, true);
            LightKey k{d.Frame(), SampleLightValues(*light, d.Frame())};
            UpsertKey(light->keys, k);
            studioLightChanged_ = true;
            StudioEndLightEdit(light->uid);
            d.rowsKey = ~0ull;
        } else {
            studioStopwatchLabel_ = LightDisplayName(light->name);
            studioStopwatchOff_ = [this, &d, uid = light->uid] {
                if (SceneLight* l = d.FindLight(uid)) {
                    std::vector<SceneLight> before = d.lights;
                    l->v = SampleLightValues(*l, d.Frame());
                    l->keys.clear();
                    d.history.Push(std::make_unique<LightsCommand>(d, Tr("애니메이션 끄기"), before, d.lights));
                    d.rowsKey = ~0ull;
                }
            };
        }
    } else if (kind == RowKind::EffectParam || kind == RowKind::WorldParam) {
        const uint32_t uid = RowGroupOf(rowId);
        StudioEffect* fx = kind == RowKind::WorldParam
                                ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr)
                                : d.FindEffect(uid);
        if (!fx) return;
        std::string param;   // `name` is the "uid:param" track name
        { uint32_t parsedUid = 0; ParseEffectParamTrackName(name, parsedUid, param); }
        EffectParamTrack* tr = FindParamTrack(*fx, param);
        const bool animated = tr && !tr->keys.empty();
        if (!animated) {
            std::vector<TrackState> before = {CaptureTrack(d, -1, kind, name)};
            if (!tr) {
                fx->tracks.push_back({param, {}});
                tr = &fx->tracks.back();
            }
            const float val = EvaluateParamTrack(*tr, (float)frame, ParamStaticValue(*fx, param));
            UpsertParamKey(*tr, EffectParamKey{frame, val, ParamTrackStepped(*fx, param)});
            StudioPushTrackEdit(Tr("애니메이션 켜기"), before);
            d.rowsKey = ~0ull;
        } else {
            const std::string label =
                kind == RowKind::WorldParam
                    ? ParamRowLabel(*fx, d.FindWorld(uid) ? d.FindWorld(uid)->name : std::string(), param)
                    : ParamRowLabel(*fx, std::string(), param);
            studioStopwatchLabel_ = label;
            studioStopwatchOff_ = [this, &d, kind, uid, param] {
                // the entity is looked up again (the list may have changed since the modal opened)
                StudioEffect* target = kind == RowKind::WorldParam
                                           ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr)
                                           : d.FindEffect(uid);
                if (!target) return;
                const float value = ParamStaticValue(*target, param);
                float current = value;
                if (const EffectParamTrack* tr = FindParamTrack(*target, param))
                    current = EvaluateParamTrack(*tr, (float)d.Frame(), value);
                if (kind == RowKind::WorldParam) {
                    std::vector<StudioWorld> before = d.worlds;
                    StudioWorld* world = d.FindWorld(uid);
                    if (!world) return;
                    world->fx.entry.params[param] = current;
                    auto& tracks = world->fx.tracks;
                    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                                [&](const EffectParamTrack& t) { return t.param == param; }),
                                 tracks.end());
                    d.history.Push(std::make_unique<WorldsCommand>(d, Tr("애니메이션 끄기"), before, d.worlds));
                } else {
                    std::vector<StudioEffect> before = d.effects;
                    StudioEffect* eff = d.FindEffect(uid);
                    if (!eff) return;
                    eff->entry.params[param] = current;
                    auto& tracks = eff->tracks;
                    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                                [&](const EffectParamTrack& t) { return t.param == param; }),
                                 tracks.end());
                    d.history.Push(std::make_unique<EffectsCommand>(d, Tr("애니메이션 끄기"), before, d.effects));
                }
                d.rowsKey = ~0ull;
            };
        }
    } else if (kind == RowKind::StageParam) {
        // S3: a stage param's stopwatch (its row's keys are the project's override of the stage's own cue track)
        const uint32_t uid = RowGroupOf(rowId);
        StudioStage* stage = d.FindStage(uid);
        if (!stage) return;
        std::string param;   // `name` is the "uid:param" track name
        { uint32_t parsedUid = 0; ParseStageParamTrackName(name, parsedUid, param); }
        EffectParamTrack* tr = FindStageParamTrack(*stage, param);
        const bool animated = tr && !tr->keys.empty();
        if (!animated) {
            std::vector<TrackState> before = {CaptureTrack(d, -1, kind, name)};
            if (!tr) {
                stage->tracks.push_back({param, {}});
                tr = &stage->tracks.back();
            }
            const float val = EvaluateParamTrack(*tr, (float)frame, StageParamStaticValue(*stage, param));
            UpsertParamKey(*tr, EffectParamKey{frame, val, StageParamTrackStepped(*stage, param)});
            StudioPushTrackEdit(Tr("애니메이션 켜기"), before);
            d.rowsKey = ~0ull;
        } else {
            studioStopwatchLabel_ = StageParamLabel(*stage, param);
            studioStopwatchOff_ = [this, &d, uid, param] {
                // the stage is looked up again (the list may have changed since the modal opened); its last value
                // becomes the base value so the pose at the playhead is kept
                StudioStage* target = d.FindStage(uid);
                if (!target) return;
                const EffectParamTrack* tr = FindStageParamTrack(*target, param);
                if (!tr) return;
                const float current = EvaluateParamTrack(*tr, (float)d.Frame(), StageParamStaticValue(*target, param));
                std::vector<StudioStage> before = d.stages;
                int comp = -1;
                const std::string key = StageParamBaseKey(param, comp);
                if (const StageParam* p = target->manifest ? target->manifest->FindParam(key) : nullptr) {
                    StageValue v = p->DefaultValue();
                    const auto base = target->base.find(key);
                    if (base != target->base.end()) v = base->second;
                    if (comp >= 0) {
                        DirectX::XMFLOAT4 c = v.AsColor();
                        switch (comp) {
                        case 0: c.x = current; break;
                        case 1: c.y = current; break;
                        case 2: c.z = current; break;
                        default: c.w = current; break;
                        }
                        v = StageValue::Color(c);
                    } else {
                        v = StageValue::Number(current);
                    }
                    target->base[key] = ClampStageParamValue(*p, v);
                }
                auto& tracks = target->tracks;
                tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                            [&](const EffectParamTrack& t) { return t.param == param; }),
                             tracks.end());
                d.history.Push(std::make_unique<StagesCommand>(d, Tr("애니메이션 끄기"), std::move(before), d.stages));
                d.rowsKey = ~0ull;
            };
        }
    }
}

void App::StudioRowKeyNav(uint64_t rowId, int dir) {
    StudioDoc& d = *studio_;
    RowKind kind;
    std::string name;
    if (!StudioTrackOfRow(rowId, kind, name)) return;
    std::string param;   // an effect / world / stage param row: `name` is the "uid:param" track name
    if (kind == RowKind::EffectParam || kind == RowKind::WorldParam || kind == RowKind::StageParam) {
        uint32_t parsedUid = 0;
        ParseEffectParamTrackName(name, parsedUid, param);
    }
    const int frame = d.Frame();

    if (dir == -1 || dir == 1) {
        int target = -1;
        if (kind == RowKind::Bone) {
            StudioModel* m = StudioPoseModel();
            if (m) {
                const auto it = m->motion.bones.find(name);
                if (it != m->motion.bones.end()) {
                    if (dir < 0) {
                        for (const BoneKf& kf : it->second) { if (kf.frame < frame) target = kf.frame; else break; }
                    } else {
                        for (const BoneKf& kf : it->second) { if (kf.frame > frame) { target = kf.frame; break; } }
                    }
                }
            }
        } else if (kind == RowKind::Morph) {
            StudioModel* m = StudioPoseModel();
            if (m) {
                const auto it = m->motion.morphs.find(name);
                if (it != m->motion.morphs.end()) {
                    if (dir < 0) {
                        for (const MorphKf& kf : it->second) { if (kf.frame < frame) target = kf.frame; else break; }
                    } else {
                        for (const MorphKf& kf : it->second) { if (kf.frame > frame) { target = kf.frame; break; } }
                    }
                }
            }
        } else if (kind == RowKind::SceneLight) {
            SceneLight* light = d.FindLight(RowIndexOf(rowId));
            if (light) {
                if (dir < 0) {
                    for (const LightKey& k : light->keys) { if (k.frame < frame) target = k.frame; else break; }
                } else {
                    for (const LightKey& k : light->keys) { if (k.frame > frame) { target = k.frame; break; } }
                }
            }
        } else if (kind == RowKind::EffectParam || kind == RowKind::WorldParam) {
            const uint32_t uid = RowGroupOf(rowId);
            const StudioEffect* fx = kind == RowKind::WorldParam
                                         ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr)
                                         : d.FindEffect(uid);
            if (fx) {
                for (const auto& t : fx->tracks) {
                    if (t.param != param) continue;
                    if (dir < 0) {
                        for (const EffectParamKey& k : t.keys) { if (k.frame < frame) target = k.frame; else break; }
                    } else {
                        for (const EffectParamKey& k : t.keys) { if (k.frame > frame) { target = k.frame; break; } }
                    }
                    break;
                }
            }
        } else if (kind == RowKind::StageParam) {
            const StudioStage* stage = d.FindStage(RowGroupOf(rowId));
            if (stage) {
                const EffectParamTrack* t = FindStageParamTrack(*stage, param);
                if (t) {
                    if (dir < 0) {
                        for (const EffectParamKey& k : t->keys) { if (k.frame < frame) target = k.frame; else break; }
                    } else {
                        for (const EffectParamKey& k : t->keys) { if (k.frame > frame) { target = k.frame; break; } }
                    }
                }
            }
        }
        if (target >= 0) {
            StudioSetPlaying(false);
            StudioSeek(target / (double)kMmdFps);
        }
        return;
    }

    if (dir == 2) {
        if (kind == RowKind::Bone) {
            StudioModel* m = StudioPoseModel();
            if (!m) return;
            const uint32_t bone = RowIndexOf(rowId);
            if (bone >= m->pmx->bones.size()) return;
            const auto it = m->motion.bones.find(name);
            const bool keyed = it != m->motion.bones.end() && FindKey(it->second, frame) != nullptr;
            std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Bone, name)};
            if (keyed) {
                EraseKey(m->motion.bones[name], frame);
                StudioPushTrackEdit(Tr("본 키 삭제"), before);
            } else {
                const PoseBone cur0 = StudioCurrentPose(*m)[(size_t)bone];
                BoneKf k;
                if (it != m->motion.bones.end()) {
                    if (const BoneKf* existing = FindKey(it->second, frame)) k = *existing;
                    else FillLinearInterp(k.interp);
                } else {
                    FillLinearInterp(k.interp);
                }
                k.frame = frame;
                k.t = cur0.t;
                k.r = cur0.r;
                UpsertKey(m->motion.bones[name], k);
                m->motion.staticBones.erase(name);
                StudioPushTrackEdit(Tr("본 키 등록"), before);
            }
            ++m->motionVersion;
            d.rowsKey = ~0ull;
        } else if (kind == RowKind::Morph) {
            StudioModel* m = StudioPoseModel();
            if (!m) return;
            const uint32_t morphIdx = RowIndexOf(rowId);
            if (morphIdx >= m->pmx->morphs.size()) return;
            StudioSelectMorph((int)morphIdx);
            const auto mt = m->motion.morphs.find(name);
            const bool keyedHere = mt != m->motion.morphs.end() && FindKey(mt->second, frame) != nullptr;
            const float v = m->inst->MorphWeight((int)morphIdx);
            std::vector<TrackState> before = {CaptureTrack(d, d.selectedModel, RowKind::Morph, name)};
            if (keyedHere) {
                EraseKey(m->motion.morphs[name], frame);
                StudioPushTrackEdit(Tr("모프 키 삭제"), before);
            } else {
                UpsertKey(m->motion.morphs[name], MorphKf{frame, v});
                StudioPushTrackEdit(Tr("모프 키 등록"), before);
            }
            ++m->motionVersion;
            d.rowsKey = ~0ull;
        } else if (kind == RowKind::SceneLight) {
            SceneLight* light = d.FindLight(RowIndexOf(rowId));
            if (!light) return;
            StudioBeginLightEdit(light->uid, true);
            if (FindKey(light->keys, d.Frame())) {
                EraseKey(light->keys, d.Frame());
            } else {
                UpsertKey(light->keys, LightKey{d.Frame(), SampleLightValues(*light, d.Frame())});
            }
            studioLightChanged_ = true;
            StudioEndLightEdit(light->uid);
            d.rowsKey = ~0ull;
        } else if (kind == RowKind::EffectParam || kind == RowKind::WorldParam) {
            const uint32_t uid = RowGroupOf(rowId);
            StudioEffect* fx = kind == RowKind::WorldParam
                                   ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr)
                                   : d.FindEffect(uid);
            if (!fx) return;
            const char* addName = kind == RowKind::WorldParam ? Tr("월드 키 등록") : Tr("효과 키 등록");
            const char* delName = kind == RowKind::WorldParam ? Tr("월드 키 삭제") : Tr("효과 키 삭제");
            EffectParamTrack* tr = FindParamTrack(*fx, param);
            std::vector<TrackState> before = {CaptureTrack(d, -1, kind, name)};
            if (tr && FindKey(tr->keys, frame)) {
                EraseParamKeys(*tr, {(int32_t)frame});
                StudioPushTrackEdit(delName, before);
            } else {
                if (!tr) {
                    fx->tracks.push_back({param, {}});
                    tr = &fx->tracks.back();
                }
                const float val = EvaluateParamTrack(*tr, (float)frame, ParamStaticValue(*fx, param));
                UpsertParamKey(*tr, EffectParamKey{frame, val, ParamTrackStepped(*fx, param)});
                StudioPushTrackEdit(addName, before);
            }
            d.rowsKey = ~0ull;
        } else if (kind == RowKind::StageParam) {
            const uint32_t uid = RowGroupOf(rowId);
            StudioStage* stage = d.FindStage(uid);
            if (!stage) return;
            EffectParamTrack* tr = FindStageParamTrack(*stage, param);
            std::vector<TrackState> before = {CaptureTrack(d, -1, kind, name)};
            if (tr && FindKey(tr->keys, frame)) {
                EraseParamKeys(*tr, {(int32_t)frame});
                StudioPushTrackEdit(Tr("스테이지 키 삭제"), before);
            } else {
                if (!tr) {
                    stage->tracks.push_back({param, {}});
                    tr = &stage->tracks.back();
                }
                const float val = EvaluateParamTrack(*tr, (float)frame, StageParamStaticValue(*stage, param));
                UpsertParamKey(*tr, EffectParamKey{frame, val, StageParamTrackStepped(*stage, param)});
                StudioPushTrackEdit(Tr("스테이지 키 등록"), before);
            }
            d.rowsKey = ~0ull;
        }
    }
}

void App::StudioHandleTimeline(const TimelineEvents& ev) {
    OpTimer timer{"StudioHandleTimeline"};
    StudioDoc& d = *studio_;
    if (ev.stopwatch) {
        StudioRowStopwatch(ev.stopwatchRow);
    }
    if (ev.keyNav) {
        StudioRowKeyNav(ev.keyNavRow, ev.keyNavDir);
    }
    if (ev.seek) {
        StudioSetPlaying(false);
        StudioSeek(ev.seekFrame / (double)kMmdFps);
    }
    if (ev.toggleGroup) {
        // entity groups (default collapsed) and pack groups whose pack.json group is "collapsed": the open state lives in
        // d.expanded; every other group keeps d.collapsed ("collapsed"). Decided by the row id (see MakeRowId).
        const auto defaultCollapsed = [&](uint64_t id) {
            if (RowKindOf(id) != RowKind::Group) return false;
            const uint32_t groupWord = RowGroupOf(id);
            const uint32_t index = RowIndexOf(id);
            const uint32_t flag = groupWord & 0xC00000u;
            if (flag != 0x400000u && flag != 0x800000u && flag != 0xC00000u) return false;
            if (index == 0) return true;  // the entity group itself
            const uint32_t uid = groupWord & 0x3FFFFFu;
            if (flag == 0xC00000u) {   // S3: a stage entity's manifest `groups` entry ("collapsed")
                const StudioStage* stage = d.FindStage(uid);
                if (!stage || !stage->manifest || index - 1 >= stage->manifest->groups.size()) return false;
                return stage->manifest->groups[index - 1].collapsed;
            }
            const StudioEffect* fx = flag == 0x400000u ? d.FindEffect(uid)
                                     : (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr);
            if (!fx) return false;
            const ShaderPack* pk = ShaderPacks().Find(fx->entry.pack);
            if (!pk || index - 1 >= pk->groups.size()) return false;
            return pk->groups[index - 1].collapsed;
        };
        if (defaultCollapsed(ev.toggledRow)) {
            if (!d.expanded.erase(ev.toggledRow)) d.expanded.insert(ev.toggledRow);
        } else {
            if (!d.collapsed.erase(ev.toggledRow)) d.collapsed.insert(ev.toggledRow);
        }
        d.rowsKey = ~0ull;
    }
    if (ev.toggleGraph) {
        // the graph lane rows are inserted by StudioRebuildRows: rebuild them
        if (!d.graphOpen.erase(ev.toggleGraphRow)) d.graphOpen.insert(ev.toggleGraphRow);
        d.rowsKey = ~0ull;
    }
    if (ev.graphChannel) {
        // a channel row toggles ITS OWN lane (several can be open at once); a single channel row also picks the
        // inspector's channel, so its segmented control follows. d.curveAllChannels is no longer driven from here.
        const uint64_t laneRow = MakeRowId(RowKind::GraphChannel, (uint32_t)(ev.graphChannelRow & 0xFFFFFFu),
                                          (uint32_t)ev.graphChannelIndex);
        if (!d.graphLaneOpen.erase(laneRow)) d.graphLaneOpen.insert(laneRow);
        if (ev.graphChannelIndex != kTimelineGraphAllChannels) {
            int count = 6;
            for (const TimelineRow& r : d.rows)
                if (r.id == ev.graphChannelRow && !r.graphChannels.empty()) {
                    count = (int)r.graphChannels.size();
                    break;
                }
            d.curveChannel = std::clamp(ev.graphChannelIndex, 0, count - 1);
        }
        d.rowsKey = ~0ull;  // the lane rows sit under their channel row
    }
    if (ev.graphCurve) {
        // a lane handle drag: write the bytes into the block's channel (every channel for the "all channels" lane) of the
        // key that owns the segment. A curve edit moves no key: no d.rowsKey.
        RowKind kind;
        std::string name;
        if (StudioTrackOfRow(ev.graphCurveRow, kind, name)) {
            StudioModel* m = kind == RowKind::Bone ? d.Selected() : nullptr;
            BoneKf* boneKey = nullptr;
            CameraKf* cameraKey = nullptr;
            if (kind == RowKind::Bone && m) {
                const auto it = m->motion.bones.find(name);
                if (it != m->motion.bones.end()) boneKey = FindKey(it->second, ev.graphCurveFrame);
            } else if (kind == RowKind::Camera && d.selectedModel < 0) {
                cameraKey = FindKey(d.SelectedCameraTrack().camera, ev.graphCurveFrame);
            }
            if (boneKey || cameraKey) {
                if (!studioLaneCurveEditing_) {
                    studioLaneCurveBefore_.clear();
                    studioLaneCurveBefore_.push_back(CaptureTrack(d, d.selectedModel, kind, name));
                    studioLaneCurveEditing_ = true;
                    studioLaneCurveChanged_ = false;
                }
                const int channels = kind == RowKind::Bone ? 4 : 6;
                const int c0 = ev.graphCurveChannel == kTimelineGraphAllChannels ? 0
                                                                                : std::clamp(ev.graphCurveChannel, 0, channels - 1);
                const int c1 = ev.graphCurveChannel == kTimelineGraphAllChannels ? channels : c0 + 1;
                bool differs = false;
                for (int ch = c0; ch < c1; ++ch) {
                    uint8_t cur[4];
                    if (boneKey) GetBoneCurve(boneKey->interp, ch, cur);
                    else GetCameraCurve(cameraKey->interp, ch, cur);
                    if (std::memcmp(cur, ev.graphCurveBytes, 4) != 0) differs = true;
                }
                if (differs) {
                    if (boneKey)
                        for (int ch = c0; ch < c1; ++ch) SetBoneCurve(boneKey->interp, ch, ev.graphCurveBytes);
                    else
                        for (int ch = c0; ch < c1; ++ch) SetCameraCurve(cameraKey->interp, ch, ev.graphCurveBytes);
                    if (boneKey) ++m->motionVersion;
                    else ++d.cameraVersion;
                    studioLaneCurveChanged_ = true;
                }
            }
        }
        if (ev.graphCurveEnd) {
            if (studioLaneCurveEditing_ && studioLaneCurveChanged_)
                StudioPushTrackEdit(Tr("보간 곡선 편집"), studioLaneCurveBefore_);
            studioLaneCurveBefore_.clear();
            studioLaneCurveEditing_ = false;
            studioLaneCurveChanged_ = false;
        }
    }
    if (ev.graphCurveEnd && !ev.graphCurve) {
        // the segment vanished mid-gesture: still close the undo step (if anything was written)
        if (studioLaneCurveEditing_ && studioLaneCurveChanged_)
            StudioPushTrackEdit(Tr("보간 곡선 편집"), studioLaneCurveBefore_);
        studioLaneCurveBefore_.clear();
        studioLaneCurveEditing_ = false;
        studioLaneCurveChanged_ = false;
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
                if (d.rows[i].graphOf != 0) continue;  // graph lane rows are not selectable
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
            else if (IsCameraKind(RowKindOf(id)) || RowKindOf(id) == RowKind::EffectParam ||
                     RowKindOf(id) == RowKind::WorldParam || RowKindOf(id) == RowKind::StageParam)
                d.selectedLightUid = 0;
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
        // a plain click on one key seeks the playhead to it (Premiere / AE): the live panels then show and edit that key's values
        if (ev.selectMode == SelectMode::Replace && ev.selectKeys.size() == 1)
            StudioSeek(ev.selectKeys[0].frame / (double)kMmdFps);
        // a key click brings its properties up in the inspector's "키" tab
        if (!ev.selectKeys.empty() && StudioPoseModel()) d.inspectorTab = 0;
        if (!ev.selectKeys.empty()) studioInspectorToKey_ = true;  // scroll the inspector to the key's fields
        // a plain click on exactly one bone key also makes that bone the active bone (the gizmo follows)
        if (ev.selectMode == SelectMode::Replace && ev.selectKeys.size() == 1
            && RowKindOf(ev.selectKeys[0].row) == RowKind::Bone) {
            d.activeBone = (int)RowIndexOf(ev.selectKeys[0].row);
            d.selectedBones = {d.activeBone};
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
            if (kind == RowKind::Camera) MoveKeyFrames(d.SelectedCameraTrack().camera, fs, ev.moveDelta);
            else if (kind == RowKind::Light) MoveKeyFrames(d.camera.light, fs, ev.moveDelta);
            else if (kind == RowKind::Shadow) MoveKeyFrames(d.camera.shadow, fs, ev.moveDelta);
            else if (kind == RowKind::Focus) MoveKeyFrames(d.camera.focus, fs, ev.moveDelta);
            else if (kind == RowKind::Accessory) {
                if (d.models[d.selectedModel]->IsProp()) MoveKeyFrames(d.models[d.selectedModel]->motion.accessory, fs, ev.moveDelta);
            }
            else if (kind == RowKind::SceneLight) {
                if (SceneLight* light = d.FindLight(LightUidOfTrack(track.second)))
                    MoveKeyFrames(light->keys, fs, ev.moveDelta);
            }
            else if (kind == RowKind::EffectParam) {
                uint32_t effectUid = 0;
                std::string param;
                if (ParseEffectParamTrackName(track.second, effectUid, param)) {
                    if (StudioEffect* eff = d.FindEffect(effectUid)) {
                        for (auto& t : eff->tracks) {
                            if (t.param == param) {
                                MoveKeyFrames(t.keys, fs, ev.moveDelta);
                                break;
                            }
                        }
                    }
                }
            }
            else if (kind == RowKind::WorldParam) {
                uint32_t worldUid = 0;
                std::string param;
                if (ParseWorldParamTrackName(track.second, worldUid, param)) {
                    if (StudioWorld* world = d.FindWorld(worldUid)) {
                        for (auto& t : world->fx.tracks) {
                            if (t.param == param) {
                                MoveKeyFrames(t.keys, fs, ev.moveDelta);
                                break;
                            }
                        }
                    }
                }
            }
            else if (kind == RowKind::StageParam) {
                uint32_t stageUid = 0;
                std::string param;
                if (ParseStageParamTrackName(track.second, stageUid, param)) {
                    if (StudioStage* stage = d.FindStage(stageUid))
                        if (EffectParamTrack* t = FindStageParamTrack(*stage, param)) MoveKeyFrames(t->keys, fs, ev.moveDelta);
                }
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
    // Value / speed graph lane: a row with at least 2 keys offers one channel per `labels` entry. Returns true when
    // the lane is open (the caller then inserts kTimelineGraphRows lane rows after the row).
    const auto fillGraph = [&](TimelineRow& r, const char* const* labels, int count) {
        r.graphChannels.clear();
        if (r.keys.size() < 2) return false;
        for (int i = 0; i < count; ++i) r.graphChannels.push_back({labels[i], BezierChannelColor(i)});
        r.graphOpen = d.graphOpen.count(r.id) != 0;
        return r.graphOpen;
    };
    // Inserts the channel sub-rows of an open graph, and under every channel whose own lane is open (d.graphLaneOpen)
    // its kTimelineGraphRows lane rows, right where they belong (call after pushing the parent row; `parent` is passed by
    // value: the caller's vector may reallocate). Neither is a track: RowKind::GraphChannel rows carry the parent id in
    // `graphOf` and the channel in `graphChannelIndex`; RowKind::GraphLane rows carry the parent id in `graphOf`, the
    // block's channel in `graphLaneChannel` and the lane index in `graphIndex`. The lane rows' id group is the parent row
    // id's low 24 bits, their index (channel << 8) | lane index. The first channel row is "모든 채널"
    // (kTimelineGraphAllChannels: one easing curve for every channel).
    const auto pushGraphRows = [&](std::vector<TimelineRow>& out, TimelineRow parent) {
        const int channelCount = (int)parent.graphChannels.size();
        const auto pushChannel = [&](int ch, const std::string& label, ImU32 dot) {
            TimelineRow chan;
            chan.id = MakeRowId(RowKind::GraphChannel, (uint32_t)(parent.id & 0xFFFFFFu), (uint32_t)ch);
            chan.label = label;
            chan.depth = parent.depth + 1;
            chan.keysEditable = false;
            chan.keys = parent.keys;
            chan.graphOf = parent.id;
            chan.graphChannelIndex = ch;
            chan.dotColor = dot;
            chan.graphOpen = d.graphLaneOpen.count(chan.id) != 0;
            const bool laneOpen = chan.graphOpen;
            out.push_back(std::move(chan));
            if (!laneOpen) return;
            for (int i = 0; i < kTimelineGraphRows; ++i) {
                TimelineRow lane;
                lane.id = MakeRowId(RowKind::GraphLane, (uint32_t)(parent.id & 0xFFFFFFu), (uint32_t)((ch << 8) | i));
                lane.keysEditable = false;
                lane.graphOf = parent.id;
                lane.graphIndex = i;
                lane.graphLaneChannel = ch;
                out.push_back(std::move(lane));
            }
        };
        pushChannel(kTimelineGraphAllChannels, Tr("모든 채널"), 0);
        for (int ch = 0; ch < channelCount; ++ch)
            pushChannel(ch, parent.graphChannels[(size_t)ch].label, parent.graphChannels[(size_t)ch].color);
    };
    // Selecting the stage entity selects its part model: that stage's param rows ("스테이지") join the part's own rows,
    // so a stage's keys show where the user clicked (they used to appear only with the camera selected).
    const StudioModel* selStagePart =
        d.selectedModel >= 0 && d.Selected() && d.Selected()->stageUid != 0 ? d.Selected() : nullptr;
    if (d.selectedModel < 0 || selStagePart) {
      if (!selStagePart) {   // the scene rows (camera, effects, worlds) only with the camera selected
        TimelineRow r;
        r.id = MakeRowId(RowKind::Camera, 0, 0);
        // S4: the row name says which camera's keys it shows (the selected one)
        r.label = d.selectedCameraUid == 0 ? std::string(Tr("카메라")) : d.CameraLabel(d.selectedCameraUid);
        r.selected = d.selectedRows.count(r.id) != 0;
        keysOf(r.id, d.SelectedCameraTrack().camera, r.keys);
        const char* const cameraChannels[] = {"X", "Y", "Z", Tr("회전"), Tr("거리"), Tr("시야")};
        const bool cameraGraph = fillGraph(r, cameraChannels, 6);
        std::vector<TimelineRow> cameraGraphRows;
        if (cameraGraph) pushGraphRows(cameraGraphRows, r);
        d.rows.push_back(std::move(r));
        for (auto& c : cameraGraphRows) d.rows.push_back(std::move(c));
        // the light / self-shadow / focus tracks are the main camera's: an extra (stage-screen) camera has its keys only
        if (d.selectedCameraUid == 0) {
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
            TimelineRow fo;
            fo.id = MakeRowId(RowKind::Focus, 0, 0);
            fo.label = Tr("초점");
            fo.selected = d.selectedRows.count(fo.id) != 0;
            keysOf(fo.id, d.camera.focus, fo.keys);
            d.rows.push_back(std::move(fo));
        }
        // the scene lights: one row per keyable light (sun, point, spot); the ambient light has no keys
        for (const SceneLight& light : d.lights) {
            if (light.kind == LightKind::Ambient) continue;
            TimelineRow r;
            r.id = MakeRowId(RowKind::SceneLight, 0, light.uid);
            r.label = LightDisplayName(light.name);
            r.selected = d.selectedRows.count(r.id) != 0;
            const bool vmdLinkedSun = (light.kind == LightKind::Sun && light.vmdLink && !d.camera.light.empty());
            if (!vmdLinkedSun) {
                r.hasStopwatch = true;
                r.animated = !light.keys.empty();
            }
            keysOf(r.id, light.keys, r.keys);
            d.rows.push_back(std::move(r));
        }

        // effect parameters: a row tree "화면 효과" under the camera target: one default-collapsed group per effect
        // (label = pack name), one group per pack.json group that has params (label = group label), then the params
        // (label = param label; a component "색 · R"). Params without a group and orphan keyed tracks sit directly
        // under the effect, after its groups.
        if (!d.effects.empty()) {
            TimelineRow group;
            group.id = MakeRowId(RowKind::Group, 0, 0);
            group.label = Tr("화면 효과");
            group.isGroup = true;
            group.keysEditable = false;
            group.expanded = !d.collapsed.count(group.id);
            group.selected = d.selectedRows.count(group.id) != 0;
            std::vector<uint64_t>& childIds = d.groupChildren[group.id];
            std::set<int> summary;
            std::vector<TimelineRow> children;
            // one entry per param component; `packGroup` = the pack.json group it belongs to (-1 = none / unknown)
            struct ParamRow {
                std::string param, label;
                const EffectParamTrack* tr = nullptr;
                int packGroup = -1;
            };
            for (const auto& eff : d.effects) {
                const ShaderPack* pk = ShaderPacks().Find(eff.entry.pack);
                const std::string packName = pk ? pk->name.Get(PackLanguage()) : eff.entry.pack;
                const uint32_t entityGroup = 0x400000u | eff.uid;
                const auto trackOf = [&](const std::string& param) -> const EffectParamTrack* {
                    for (const auto& t : eff.tracks)
                        if (t.param == param) return &t;
                    return nullptr;
                };
                static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
                static const char* kVec[3] = {" · X", " · Y", " · Z"};
                std::vector<ParamRow> params;
                const auto packGroupOf = [&](const std::string& groupId) {
                    if (!pk) return -1;
                    for (int gi = 0; gi < (int)pk->groups.size(); ++gi)
                        if (pk->groups[(size_t)gi].id == groupId) return gi;
                    return -1;
                };
                std::set<std::string> listed;  // pack params that have their rows (orphan tracks follow)
                if (pk) {
                    for (const ShaderPackParam& sp : pk->params) {
                        listed.insert(sp.key);
                        const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(PackLanguage());
                        const int packGroup = packGroupOf(sp.group);
                        const int comps = EffectiveParamComponents(sp);
                        if (comps == 1) {
                            params.push_back({sp.key, label, trackOf(sp.key), packGroup});
                        } else {
                            for (int c = 0; c < comps; ++c) {
                                const std::string param = sp.key + "#" + std::to_string(c);
                                params.push_back({param, label + (sp.type == ShaderPackParamType::Color ? kCol[c] : kVec[c]),
                                                  trackOf(param), packGroup});
                            }
                        }
                    }
                }
                // keyed tracks whose param matches no pack param (pack missing / param removed)
                for (const auto& tr : eff.tracks) {
                    if (listed.count(tr.param)) continue;
                    bool inPack = false;
                    if (pk) {
                        for (const ShaderPackParam& sp : pk->params) {
                            int comp = -1;
                            if (MatchPackParamName(sp, tr.param, comp)) { inPack = true; break; }
                        }
                    }
                    if (inPack) continue;
                    params.push_back({tr.param, tr.param, &tr, -1});
                }
                const auto makeParamRow = [&](const ParamRow& pr, int depth) {
                    TimelineRow r;
                    r.depth = depth;
                    r.id = MakeRowId(RowKind::EffectParam, eff.uid, EffectParamIndexOfName(eff, pr.param));
                    r.label = pr.label;
                    r.selected = d.selectedRows.count(r.id) != 0;
                    r.tint = IM_COL32(100, 210, 210, 255);
                    r.hasStopwatch = true;
                    r.animated = pr.tr && !pr.tr->keys.empty();
                    if (pr.tr) keysOf(r.id, pr.tr->keys, r.keys);
                    return r;
                };
                // the effect's own group row: default collapsed (d.expanded holds the ones the user opened)
                TimelineRow ent;
                ent.id = MakeRowId(RowKind::Group, entityGroup, 0);
                ent.label = packName;
                ent.depth = 1;
                ent.isGroup = true;
                ent.keysEditable = false;
                ent.expanded = d.expanded.count(ent.id) != 0;
                ent.selected = d.selectedRows.count(ent.id) != 0;
                std::vector<uint64_t>& entChildren = d.groupChildren[ent.id];
                std::set<int> entSummary;
                std::vector<TimelineRow> entRows;
                const bool entShow = group.expanded && ent.expanded;
                // one group per pack.json group that has params (default collapsed = the group's own "collapsed")
                for (int gi = 0; pk && gi < (int)pk->groups.size(); ++gi) {
                    bool hasParam = false;
                    for (const ParamRow& pr : params)
                        if (pr.packGroup == gi) { hasParam = true; break; }
                    if (!hasParam) continue;
                    const ShaderPackGroup& pg = pk->groups[(size_t)gi];
                    TimelineRow g;
                    g.id = MakeRowId(RowKind::Group, entityGroup, 1 + (uint32_t)gi);
                    g.label = pg.label.Empty() ? pg.id : pg.label.Get(PackLanguage());
                    g.depth = 2;
                    g.isGroup = true;
                    g.keysEditable = false;
                    g.expanded = pg.collapsed ? d.expanded.count(g.id) != 0 : !d.collapsed.count(g.id);
                    g.selected = d.selectedRows.count(g.id) != 0;
                    std::vector<uint64_t>& groupChildIds = d.groupChildren[g.id];
                    std::set<int> groupSummary;
                    std::vector<TimelineRow> groupRows;
                    for (const ParamRow& pr : params) {
                        if (pr.packGroup != gi) continue;
                        TimelineRow r = makeParamRow(pr, 3);
                        for (const TimelineKey& k : r.keys) { groupSummary.insert(k.frame); entSummary.insert(k.frame); }
                        groupChildIds.push_back(r.id);
                        entChildren.push_back(r.id);
                        if (entShow && g.expanded) groupRows.push_back(std::move(r));
                    }
                    for (int f : groupSummary) g.keys.push_back({f, false});
                    if (entShow) entRows.push_back(std::move(g));
                    for (auto& r : groupRows) entRows.push_back(std::move(r));
                }
                // params without a group / with an unknown group, then the orphan keyed tracks: under the effect
                for (const ParamRow& pr : params) {
                    if (pr.packGroup >= 0) continue;
                    TimelineRow r = makeParamRow(pr, 2);
                    for (const TimelineKey& k : r.keys) entSummary.insert(k.frame);
                    entChildren.push_back(r.id);
                    if (entShow) entRows.push_back(std::move(r));
                }
                for (int f : entSummary) {
                    ent.keys.push_back({f, false});
                    summary.insert(f);
                }
                for (uint64_t c : entChildren) childIds.push_back(c);
                if (group.expanded) children.push_back(std::move(ent));
                for (auto& r : entRows) children.push_back(std::move(r));
            }
            for (int f : summary) group.keys.push_back({f, false});
            d.rows.push_back(std::move(group));
            for (auto& c : children) d.rows.push_back(std::move(c));
        }

        // world parameters: a row tree "월드" under the camera target (same structure as the effects): one
        // default-collapsed group per world entity (label = its name, else the pack name), one group per pack.json
        // group that has params, then the params
        if (!d.worlds.empty()) {
            TimelineRow group;
            group.id = MakeRowId(RowKind::Group, 0, 1);
            group.label = Tr("월드");
            group.isGroup = true;
            group.keysEditable = false;
            group.expanded = !d.collapsed.count(group.id);
            group.selected = d.selectedRows.count(group.id) != 0;
            std::vector<uint64_t>& childIds = d.groupChildren[group.id];
            std::set<int> summary;
            std::vector<TimelineRow> children;
            struct ParamRow {
                std::string param, label;
                const EffectParamTrack* tr = nullptr;
                int packGroup = -1;
            };
            for (const auto& world : d.worlds) {
                const ShaderPack* pk = ShaderPacks().Find(world.fx.entry.pack);
                const std::string packName = pk ? pk->name.Get(PackLanguage()) : world.fx.entry.pack;
                const std::string entityLabel = world.name.empty() ? packName : world.name;
                const uint32_t entityGroup = 0x800000u | world.uid;
                const auto trackOf = [&](const std::string& param) -> const EffectParamTrack* {
                    for (const auto& t : world.fx.tracks)
                        if (t.param == param) return &t;
                    return nullptr;
                };
                static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
                static const char* kVec[3] = {" · X", " · Y", " · Z"};
                std::vector<ParamRow> params;
                const auto packGroupOf = [&](const std::string& groupId) {
                    if (!pk) return -1;
                    for (int gi = 0; gi < (int)pk->groups.size(); ++gi)
                        if (pk->groups[(size_t)gi].id == groupId) return gi;
                    return -1;
                };
                std::set<std::string> listed;
                if (pk) {
                    for (const ShaderPackParam& sp : pk->params) {
                        listed.insert(sp.key);
                        const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(PackLanguage());
                        const int packGroup = packGroupOf(sp.group);
                        const int comps = EffectiveParamComponents(sp);
                        if (comps == 1) {
                            params.push_back({sp.key, label, trackOf(sp.key), packGroup});
                        } else {
                            for (int c = 0; c < comps; ++c) {
                                const std::string param = sp.key + "#" + std::to_string(c);
                                params.push_back({param, label + (sp.type == ShaderPackParamType::Color ? kCol[c] : kVec[c]),
                                                  trackOf(param), packGroup});
                            }
                        }
                    }
                }
                for (const auto& tr : world.fx.tracks) {
                    if (listed.count(tr.param)) continue;
                    bool inPack = false;
                    if (pk) {
                        for (const ShaderPackParam& sp : pk->params) {
                            int comp = -1;
                            if (MatchPackParamName(sp, tr.param, comp)) { inPack = true; break; }
                        }
                    }
                    if (inPack) continue;
                    params.push_back({tr.param, tr.param, &tr, -1});
                }
                const auto makeParamRow = [&](const ParamRow& pr, int depth) {
                    TimelineRow r;
                    r.depth = depth;
                    r.id = MakeRowId(RowKind::WorldParam, world.uid, EffectParamIndexOfName(world.fx, pr.param));
                    r.label = pr.label;
                    r.selected = d.selectedRows.count(r.id) != 0;
                    r.tint = IM_COL32(150, 190, 240, 255);
                    r.hasStopwatch = true;
                    r.animated = pr.tr && !pr.tr->keys.empty();
                    if (pr.tr) keysOf(r.id, pr.tr->keys, r.keys);
                    return r;
                };
                TimelineRow ent;
                ent.id = MakeRowId(RowKind::Group, entityGroup, 0);
                ent.label = entityLabel;
                ent.depth = 1;
                ent.isGroup = true;
                ent.keysEditable = false;
                ent.expanded = d.expanded.count(ent.id) != 0;
                ent.selected = d.selectedRows.count(ent.id) != 0;
                std::vector<uint64_t>& entChildren = d.groupChildren[ent.id];
                std::set<int> entSummary;
                std::vector<TimelineRow> entRows;
                const bool entShow = group.expanded && ent.expanded;
                for (int gi = 0; pk && gi < (int)pk->groups.size(); ++gi) {
                    bool hasParam = false;
                    for (const ParamRow& pr : params)
                        if (pr.packGroup == gi) { hasParam = true; break; }
                    if (!hasParam) continue;
                    const ShaderPackGroup& pg = pk->groups[(size_t)gi];
                    TimelineRow g;
                    g.id = MakeRowId(RowKind::Group, entityGroup, 1 + (uint32_t)gi);
                    g.label = pg.label.Empty() ? pg.id : pg.label.Get(PackLanguage());
                    g.depth = 2;
                    g.isGroup = true;
                    g.keysEditable = false;
                    g.expanded = pg.collapsed ? d.expanded.count(g.id) != 0 : !d.collapsed.count(g.id);
                    g.selected = d.selectedRows.count(g.id) != 0;
                    std::vector<uint64_t>& groupChildIds = d.groupChildren[g.id];
                    std::set<int> groupSummary;
                    std::vector<TimelineRow> groupRows;
                    for (const ParamRow& pr : params) {
                        if (pr.packGroup != gi) continue;
                        TimelineRow r = makeParamRow(pr, 3);
                        for (const TimelineKey& k : r.keys) { groupSummary.insert(k.frame); entSummary.insert(k.frame); }
                        groupChildIds.push_back(r.id);
                        entChildren.push_back(r.id);
                        if (entShow && g.expanded) groupRows.push_back(std::move(r));
                    }
                    for (int f : groupSummary) g.keys.push_back({f, false});
                    if (entShow) entRows.push_back(std::move(g));
                    for (auto& r : groupRows) entRows.push_back(std::move(r));
                }
                for (const ParamRow& pr : params) {
                    if (pr.packGroup >= 0) continue;
                    TimelineRow r = makeParamRow(pr, 2);
                    for (const TimelineKey& k : r.keys) entSummary.insert(k.frame);
                    entChildren.push_back(r.id);
                    if (entShow) entRows.push_back(std::move(r));
                }
                for (int f : entSummary) {
                    ent.keys.push_back({f, false});
                    summary.insert(f);
                }
                for (uint64_t c : entChildren) childIds.push_back(c);
                if (group.expanded) children.push_back(std::move(ent));
                for (auto& r : entRows) children.push_back(std::move(r));
            }
            for (int f : summary) group.keys.push_back({f, false});
            d.rows.push_back(std::move(group));
            for (auto& c : children) d.rows.push_back(std::move(c));
        }

      }   // !selStagePart
        // S3: stage parameters: a row tree "스테이지" (group word 0, index 2) with one default-collapsed group per stage
        // entity, one group per manifest `groups` entry that has params, then the params (colours per component).
        if (!d.stages.empty()) {
            TimelineRow group;
            group.id = MakeRowId(RowKind::Group, 0, 2);
            group.label = Tr("스테이지");
            group.isGroup = true;
            group.keysEditable = false;
            group.expanded = !d.collapsed.count(group.id);
            group.selected = d.selectedRows.count(group.id) != 0;
            std::vector<uint64_t>& childIds = d.groupChildren[group.id];
            std::set<int> summary;
            std::vector<TimelineRow> children;
            struct StageRow {
                std::string param, label;
                const EffectParamTrack* tr = nullptr;
                int stageGroup = -1;
            };
            for (const StudioStage& stage : d.stages) {
                if (!stage.manifest) continue;
                if (selStagePart && stage.uid != selStagePart->stageUid) continue;   // only the selected stage
                const StageManifest& man = *stage.manifest;
                const std::string entityLabel = man.DisplayName(StageLanguage());
                const uint32_t entityGroup = 0xC00000u | stage.uid;
                const auto trackOf = [&](const std::string& param) -> const EffectParamTrack* {
                    return FindStageParamTrack(stage, param);
                };
                static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
                std::vector<StageRow> params;
                const auto stageGroupOf = [&](const std::string& groupId) {
                    for (int gi = 0; gi < (int)man.groups.size(); ++gi)
                        if (man.groups[(size_t)gi].id == groupId) return gi;
                    return -1;
                };
                std::set<std::string> listed;
                for (const StageParam& sp : man.params) {
                    listed.insert(sp.key);
                    const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(StageLanguage());
                    const int stageGroup = stageGroupOf(sp.group);
                    const int comps = StageParamComponents(sp);
                    if (comps == 1) {
                        params.push_back({sp.key, label, trackOf(sp.key), stageGroup});
                    } else {
                        for (int c = 0; c < comps; ++c) {
                            const std::string param = StageParamName(sp, c);
                            params.push_back({param, label + kCol[c], trackOf(param), stageGroup});
                        }
                    }
                }
                for (const EffectParamTrack& tr : stage.tracks) {
                    if (listed.count(tr.param)) continue;
                    uint32_t known = 0;
                    if (StageParamIndexOfTrack(stage, tr.param, known)) continue;
                    params.push_back({tr.param, tr.param, &tr, -1});
                }
                const auto makeParamRow = [&](const StageRow& pr, int depth) {
                    TimelineRow r;
                    r.depth = depth;
                    r.id = MakeRowId(RowKind::StageParam, stage.uid, StageParamIndexOfName(stage, pr.param));
                    r.label = pr.label;
                    r.selected = d.selectedRows.count(r.id) != 0;
                    r.tint = IM_COL32(190, 160, 240, 255);
                    r.hasStopwatch = true;
                    r.animated = pr.tr && !pr.tr->keys.empty();
                    if (pr.tr) keysOf(r.id, pr.tr->keys, r.keys);
                    return r;
                };
                TimelineRow ent;
                ent.id = MakeRowId(RowKind::Group, entityGroup, 0);
                ent.label = entityLabel;
                ent.depth = 1;
                ent.isGroup = true;
                ent.keysEditable = false;
                ent.expanded = d.expanded.count(ent.id) != 0 || selStagePart != nullptr;   // open when the stage is selected
                ent.selected = d.selectedRows.count(ent.id) != 0;
                std::vector<uint64_t>& entChildren = d.groupChildren[ent.id];
                std::set<int> entSummary;
                std::vector<TimelineRow> entRows;
                const bool entShow = group.expanded && ent.expanded;
                for (int gi = 0; gi < (int)man.groups.size(); ++gi) {
                    bool hasParam = false;
                    for (const StageRow& pr : params)
                        if (pr.stageGroup == gi) { hasParam = true; break; }
                    if (!hasParam) continue;
                    const StageParamGroup& sg = man.groups[(size_t)gi];
                    TimelineRow g;
                    g.id = MakeRowId(RowKind::Group, entityGroup, 1 + (uint32_t)gi);
                    g.label = sg.label.Empty() ? sg.id : sg.label.Get(StageLanguage());
                    g.depth = 2;
                    g.isGroup = true;
                    g.keysEditable = false;
                    g.expanded = sg.collapsed ? d.expanded.count(g.id) != 0 : !d.collapsed.count(g.id);
                    g.selected = d.selectedRows.count(g.id) != 0;
                    std::vector<uint64_t>& groupChildIds = d.groupChildren[g.id];
                    std::set<int> groupSummary;
                    std::vector<TimelineRow> groupRows;
                    for (const StageRow& pr : params) {
                        if (pr.stageGroup != gi) continue;
                        TimelineRow r = makeParamRow(pr, 3);
                        for (const TimelineKey& k : r.keys) { groupSummary.insert(k.frame); entSummary.insert(k.frame); }
                        groupChildIds.push_back(r.id);
                        entChildren.push_back(r.id);
                        if (entShow && g.expanded) groupRows.push_back(std::move(r));
                    }
                    for (int f : groupSummary) g.keys.push_back({f, false});
                    if (entShow) entRows.push_back(std::move(g));
                    for (auto& r : groupRows) entRows.push_back(std::move(r));
                }
                for (const StageRow& pr : params) {
                    if (pr.stageGroup >= 0) continue;
                    TimelineRow r = makeParamRow(pr, 2);
                    for (const TimelineKey& k : r.keys) entSummary.insert(k.frame);
                    entChildren.push_back(r.id);
                    if (entShow) entRows.push_back(std::move(r));
                }
                for (int f : entSummary) {
                    ent.keys.push_back({f, false});
                    summary.insert(f);
                }
                for (uint64_t c : entChildren) childIds.push_back(c);
                if (group.expanded) children.push_back(std::move(ent));
                for (auto& r : entRows) children.push_back(std::move(r));
            }
            for (int f : summary) group.keys.push_back({f, false});
            d.rows.push_back(std::move(group));
            for (auto& c : children) d.rows.push_back(std::move(c));
        }
        if (!selStagePart) return;
    }
    StudioModel* m = d.Selected();
    if (!m) return;
    const PmxModel& pmx = *m->pmx;
    const MotionData& mo = m->motion;
    // a prop's accessory (placement) track: one row, like the camera's Focus row
    if (m->IsProp()) {
        TimelineRow acc;
        acc.id = MakeRowId(RowKind::Accessory, 0, 0);
        acc.label = Tr("소품");
        acc.selected = d.selectedRows.count(acc.id) != 0;
        keysOf(acc.id, mo.accessory, acc.keys);
        d.rows.push_back(std::move(acc));
    }
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
        const char* const boneChannels[] = {"X", "Y", "Z", Tr("회전")};
        for (const PmxDisplayFrame::Item& it : items) {
            if (!listed.insert({it.morph, it.index}).second) continue;
            TimelineRow r;
            r.depth = 1;
            if (it.morph) {
                r.id = MakeRowId(RowKind::Morph, g, (uint32_t)it.index);
                r.label = pmx.morphs[(size_t)it.index].name;  // internal track key (VMD/VPD-safe)
                r.displayLabel = DisplayName(pmx.morphs[(size_t)it.index].name, pmx.morphs[(size_t)it.index].nameEn);
                r.hasStopwatch = true;
                const auto t = mo.morphs.find(r.label);
                r.animated = (t != mo.morphs.end() && !t->second.empty() && !IsStaticMorphTrack(mo, r.label));
                if (t != mo.morphs.end()) {
                    keysOfModel(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            } else {
                r.id = MakeRowId(RowKind::Bone, g, (uint32_t)it.index);
                r.label = pmx.bones[(size_t)it.index].name;  // internal track key (VMD/VPD-safe)
                r.displayLabel = DisplayName(pmx.bones[(size_t)it.index].name, pmx.bones[(size_t)it.index].nameEn);
                r.hasStopwatch = true;
                const auto t = mo.bones.find(r.label);
                r.animated = (t != mo.bones.end() && !t->second.empty() && !IsStaticBoneTrack(mo, r.label));
                if (t != mo.bones.end()) {
                    keysOfModel(r.id, t->second, r.keys);
                    for (const auto& k : t->second) summary.insert(k.frame);
                }
            }
            r.selected = d.selectedRows.count(r.id) != 0 || (!it.morph && d.selectedBones.count(it.index));
            const bool graph = !it.morph && fillGraph(r, boneChannels, 4);
            childIds.push_back(r.id);
            if (group.expanded) children.push_back(std::move(r));
            if (graph && group.expanded) pushGraphRows(children, children.back());
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
    const std::filesystem::path path = OpenFileDialog(hwnd_, {{L"VMD / MVD", L"*.vmd;*.mvd"}});
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
        // S4: camera keys land in the SELECTED camera (uid 0 = the main one); light / shadow stay the main camera's
        if (d.selectedCameraUid == 0) {
            MotionData after = d.camera;
            for (const CameraKf& k : in.camera) UpsertKey(after.camera, k);
            for (const LightKf& k : in.light) UpsertKey(after.light, k);
            for (const ShadowKf& k : in.shadow) UpsertKey(after.shadow, k);
            d.history.Push(std::make_unique<MotionSwapCommand>(d, -1, Tr("카메라 VMD 불러오기"), d.camera, std::move(after)));
        } else {
            const std::vector<TrackState> before{CaptureTrack(d, -1, RowKind::Camera, "")};
            std::vector<CameraKf>& keys = d.SelectedCameraTrack().camera;
            for (const CameraKf& k : in.camera) UpsertKey(keys, k);
            StudioPushTrackEdit(Tr("카메라 VMD 불러오기"), before);
            if (!in.light.empty() || !in.shadow.empty()) {
                MotionData after = d.camera;   // the main camera keeps the light / self-shadow tracks
                for (const LightKf& k : in.light) UpsertKey(after.light, k);
                for (const ShadowKf& k : in.shadow) UpsertKey(after.shadow, k);
                d.history.Push(std::make_unique<MotionSwapCommand>(d, -1, Tr("카메라 VMD 불러오기"), d.camera,
                                                                   std::move(after)));
            }
        }
        d.selectedModel = -1;
        if (!in.camera.empty()) d.useMotionCamera = true;
    } else {
        StudioModel* m = d.Selected();
        if (!m) return;
        in.camera.clear();
        in.light.clear();
        in.shadow.clear();
        in.CanonicalizeNames(*m->pmx);
        // Arm rest-pose correction: a T-pose-authored motion onto an A-pose model asks before the merge.
        if (m->kind == ModelKind::Character) {
            const ArmRestGuess guess = GuessTPoseMotion(in);
            const ArmRest rest = MeasureArmRest(*m->pmx);
            if (guess.likely && rest.valid && rest.deg[0] >= 25.0f && rest.deg[1] >= 25.0f) {
                studioRestPosePending_ = std::move(in);
                studioRestPosePendingPath_ = path;
                studioRestPoseGuess_ = guess;
                studioRestPoseModel_ = (int)m->uid;
                studioRestPoseSourceDeg_ = 0.0f;
                studioRestPoseSide_[0] = studioRestPoseSide_[1] = true;
                studioRestPoseAuto_ = true;
                studioRestPoseOpen_ = true;
                LOG_INFO("studio: VMD looks T-pose authored (median %.1f deg, %d keys): asking before the merge",
                         guess.medianDeg, guess.samples);
                return;
            }
        }
        MotionData after = m->motion;
        after.Merge(in);
        d.history.Push(std::make_unique<MotionSwapCommand>(d, d.selectedModel, Tr("모션 VMD 불러오기"), m->motion, std::move(after)));
    }
    StudioFinishVmdImport(path);
}

void App::StudioFinishVmdImport(const std::filesystem::path& path) {
    StudioDoc& d = *studio_;
    d.selection.clear();
    d.rowsKey = ~0ull;
    toast_ = {Tr("VMD를 불러왔어요"), PathToUtf8(path.filename()), {}, false, timeSeconds_ + 4.0};
}

void App::StudioExportVmd() {
    StudioDoc& d = *studio_;
    const bool camera = d.selectedModel < 0;
    StudioModel* m = d.Selected();
    if (!camera && !m) return;
    std::wstring suggested = !camera                     ? Utf8ToWide(m->name) + L".vmd"
                             : d.selectedCameraUid == 0 ? std::wstring(L"camera.vmd")
                                                        : Utf8ToWide(d.CameraLabel(d.selectedCameraUid)) + L".vmd";
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
        // S4: the SELECTED camera's keys are exported (an extra camera's own track, else the main one). Light /
        // self-shadow stay the main camera's.
        MotionData c;
        c.camera = d.SelectedCameraTrack().camera;
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
    ImGui::DockBuilderDockWindow("###studio_effects", right);
    ImGui::DockBuilderDockWindow("###studio_inspector", right);
    ImGui::DockBuilderDockWindow("###studio_timeline", bottom);
    ImGui::DockBuilderDockWindow("###studio_viewport", center);
    ImGui::DockBuilderFinish(dock);
}
} // namespace

// Viewport renderer choice (the transport combo, the ribbon's 렌더 tab): saved so the next run keeps it.
void App::StudioSetViewportRenderer(int choice) {
    settings_.studioRenderer = std::clamp(choice, 0, 3);
    settings_.Save(settingsPath_);
}

// Physics switch (the transport's ##physics, the ribbon's 애니메이션 tab): the next frame starts from the motion's pose.
void App::StudioTogglePhysics() {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    d.physics = !d.physics;
    d.physicsFrame = -1.0f;
}

void App::DrawStudio() {
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const Palette& pal = P();
    const float top = Dp(App::kRibbonHeaderH + (studioRibbonCollapsed_ ? 0.0f : App::kRibbonToolsH));
    // The auto-key switch lives in several places (ribbon, transport, camera / light inspectors): remember whichever
    // flipped it last frame. Scripted runs never write the ini.
    if (d.autoKey != settings_.studioAutoKey && !HeadlessRun()) {
        settings_.studioAutoKey = d.autoKey;
        settings_.Save(settingsPath_);
    }
    // A field edit lasts while its value is typed. A field that went away before it saw its own end (a press
    // elsewhere changed the panel first, e.g. a click in the outliner; Escape deselecting the bone mid-drag) leaves its
    // edit open, and the next edit would join it with a stale "before". Nothing is active between gestures: such an
    // edit ends here, its change so far one undo step of the model it was made on. Camera possession keeps its undo
    // step open over idle frames on purpose (StudioNavEditTick ends it).
    if (!ImGui::IsAnyItemActive()) {
        if (studioKeyEdit_ && !(studioKeyLive_ && d.possessCamera)) StudioEndKeyEdit();
        if (studioPoseFieldEdit_) {
            studioPoseFieldEdit_ = false;
            // StudioSetPose registers the selected model's layer: never another model's
            if (d.Selected() && d.Selected()->uid == studioPoseFieldModel_)
                StudioSetPose(Tr("본 값 편집"), studioPoseBefore_);
        }
        studioFixedAxisActive_ = false;
        if (studioPlaceDragging_ && studioViewDrag_ != 6) {  // 6 = the gizmo's placement drag (commits itself)
            studioPlaceDragging_ = false;
            if (const int i = d.IndexOfUid(studioPlaceModel_); i >= 0)
                StudioCommitPlace(studioPlaceModel_, studioPlaceBefore_, d.models[(size_t)i]->place);
        }
    }

    // --- ribbon: a fixed window above the dock space
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(ds.x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##studiotop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    DrawStudioRibbon(0, 0, ds.x, top);
    ImGui::End();
    if (studio_.get() != &d) return;  // left the studio (back button) or replaced the project (recent menu)

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
        studioResetLayoutDone_ = true;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0), (ImGuiDockNodeFlags)ImGuiDockNodeFlags_NoWindowMenuButton);
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
    panel(Tr("하이어라키"), "studio_outliner", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioOutliner(x0, y0, x1, y1); });
    panel(Tr("속성"), "studio_inspector", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioInspector(x0, y0, x1, y1); });
    // effects: a tab next to the properties (layouts saved before it existed get it docked there too)
    if (ImGuiWindow* insp = ImGui::FindWindowByName("###studio_inspector"); insp && insp->DockId)
        ImGui::SetNextWindowDockID(insp->DockId, ImGuiCond_FirstUseEver);
    if (studioFocusEffects_) {
        ImGui::SetNextWindowFocus();
        studioFocusEffects_ = false;
    }
    panel(Tr("효과"), "studio_effects", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioEffectsPanel(x0, y0, x1, y1); });
    if (studioResetLayoutDone_) {   // a fresh layout opens on the properties tab
        if (ImGuiWindow* insp = ImGui::FindWindowByName("###studio_inspector")) ImGui::FocusWindow(insp);
        studioResetLayoutDone_ = false;
    }
    panel(Tr("타임라인"), "studio_timeline", true, true, [&](float x0, float y0, float x1, float y1) { DrawStudioTimeline(x0, y0, x1, y1); });

    DrawStudioUnsavedPrompt();
    if (studio_.get() != &d) return;  // left (or replaced) from the prompt
    DrawStudioHelp();
    DrawStudioLightPresetConfirm();
    DrawStudioStopwatchConfirm();   // stopwatch off: "키프레임을 모두 지웁니다"
    DrawStudioBakeDialog();    // P2.2: the physics bake options / progress modal
    DrawStudioRestPoseDialog();  // T-pose-authored VMDs: the arm rest-pose correction modal
    DrawVideoRenderDialog();   // the render dialog (top bar render menu); starting it leaves for Screen::Offline
    if (screen_ != Screen::Studio) return;
    DrawToast();

    // The 3D view fills the viewport panel (back buffer pixels = ImGui display pixels).
    // The chosen studio renderer draws the lit single view; Unlit / Wireframe shading and the quad view are
    // editing views and always render with the raster path. GI preview (choice 3) draws through its own
    // offline path in RecordStudioGiViewport; Raster stays the fallback there. LeaveStudio restores the
    // user's path via ApplyRenderSettings; offline / video / still renders keep their own chosen renderer.
    {
        const ViewShading shading = (ViewShading)std::clamp(d.shading, 0, 2);
        const bool lit = shading == ViewShading::Lit && d.viewLayout == 0;
        const int choice = std::clamp(settings_.studioRenderer, 0, 3);
        RenderPath want = RenderPath::Raster;
        if (lit && renderer_.RayTracingSupported()) {
            if (choice == 1) want = RenderPath::RayTraced;
            else if (choice == 2) want = RenderPath::PathTraced;
        }
        RenderSettings rs = renderer_.Settings();
        if (rs.renderPath != want) {
            rs.renderPath = want;
            renderer_.SetSettings(rs);
        }
        if (!StudioGiActive() && (studioGi_.valid || studioGi_.active)) ResetStudioGi();
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

void App::DrawStudioOutliner(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x1 - 0.5f, y0), ImVec2(x1 - 0.5f, y1), p.line);
    // "+": add a scene item / light / world effect (one drill-down popup)
    ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(12.0f + 64.0f), y0 + Dp(5.0f)));
    const bool addClicked = Button("##addmodel", Tr("추가"), icon::Plus, ButtonKind::Ghost, ImVec2(64.0f, 30.0f));
    Tooltip(Tr("장면, 조명, 월드 이펙트 추가"));
    if (addClicked) {
        studioAddPage_ = 0;
        studioReplaceUid_ = 0;
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
        const bool selected = d.selectedModel == index;
        ImGui::InvisibleButton("##row", ImVec2(w - Dp(index >= 0 ? 72.0f : 40.0f), rowH));
        uiprobe::Info(ImGui::GetItemID(), "##row", label);   // MCP ui_click by name
        const bool hovered = ImGui::IsItemHovered();
        if (index >= 0 && hovered) Tooltip(PathToUtf8(d.models[index]->path).c_str());
        if (ImGui::IsItemClicked() && index > -1) StudioSelectModel(index);   // the camera rows have their own section
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

    // Collapsible group header: caret + label + count, full-width clickable (the fold lives in studioOutlinerOpen_)
    int groupCount[3] = {0, 0, 0};
    groupCount[0] = (int)d.models.size() + 1 + (int)d.cameras.size() + (d.hasAudio ? 1 : 0);  // models + cameras + audio
    groupCount[1] = (int)d.lights.size();
    groupCount[2] = (int)d.worlds.size();
    const auto group = [&](int gi, const char* id, const char* label) {
        char count[32];
        std::snprintf(count, sizeof(count), "%d", groupCount[gi]);
        const float gw = ImGui::GetContentRegionAvail().x;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float headH = Dp(32.0f);
        const ImVec2 b(a.x + gw, a.y + headH);
        ImGui::InvisibleButton(id, ImVec2(gw, headH));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::GetWindowDrawList()->AddRectFilled(a, b, WithAlpha(p.ink, 0.05f), Dp(8.0f));
        if (ImGui::IsItemClicked()) studioOutlinerOpen_[gi] = !studioOutlinerOpen_[gi];
        Icon(ImGui::GetWindowDrawList(), studioOutlinerOpen_[gi] ? icon::CaretDown : icon::CaretRight, 14.0f,
             ImVec2(a.x + Dp(20.0f), a.y + headH * 0.5f), p.ink3);
        Text(ImGui::GetWindowDrawList(), Font::Semibold, size::Caption, ImVec2(a.x + Dp(36.0f), a.y + Dp(8.0f)), p.ink3, label);
        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption,
             ImVec2(a.x + Dp(42.0f) + TextSize(Font::Semibold, size::Caption, label).x, a.y + Dp(8.0f)), WithAlpha(p.ink3, 0.7f), count);
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(gw, 0.0f));
    };

    // --- 장면 group: camera, characters, props, stages, audio, loads, empty hint ---
    group(0, "##grp_scene", Tr("장면"));
    if (studioOutlinerOpen_[0]) {
        DrawStudioCameraOutliner();   // S4: the main camera + every extra camera, and their two actions
        // characters first (they are what gets animated), then props
        for (int pass = 0; pass < 2; ++pass) {
            const ModelKind kind = pass == 0 ? ModelKind::Character : ModelKind::Prop;
            for (int i = 0; i < (int)d.models.size(); ++i) {
                StudioModel& m = *d.models[i];
                if (m.kind != kind) continue;
                size_t keys = 0;
                for (const auto& [n, k] : m.motion.bones) keys += k.size();
                for (const auto& [n, k] : m.motion.morphs) keys += k.size();
                if (m.IsProp()) {
                    const int pi = m.attach.parent >= 0 ? d.IndexOfUid((uint32_t)m.attach.parent) : -1;
                    if (pi >= 0)
                        std::snprintf(sub, sizeof(sub), "%s · %s%s%s", Tr("소품"), d.models[pi]->name.c_str(),
                                      m.attach.bone.empty() ? "" : " / ", m.attach.bone.c_str());
                    else
                        std::snprintf(sub, sizeof(sub), "%s", Tr("소품 · 월드"));
                } else {
                    std::snprintf(sub, sizeof(sub), Tr("키 %d개"), (int)keys);
                }
                const char* glyph = m.IsProp() ? icon::Cube : icon::PersonSimple;
                // English-name preference when the label has not been custom-renamed (rename keeps the custom label)
                const std::string modelLabel = (m.pmx && m.name == m.pmx->name)
                                                   ? std::string(DisplayName(m.pmx->name, m.pmx->nameEn))
                                                   : m.name;
                row(i, glyph, modelLabel.c_str(), sub, &m.visible);
            }
        }
        // S3: an MMDX stage is ONE row (its entity's name) with its parts as children, collapsed by default. Selecting
        // it selects its first part (so the inspector shows the stage panel); the eye acts on the whole stage.
        {
            std::set<uint32_t> drawn;
            for (int i = 0; i < (int)d.models.size(); ++i) {
                StudioModel& m0 = *d.models[i];
                if (!m0.IsStage()) continue;
                const StudioStage* st = d.StageOfModel(m0);
                const uint32_t key = st ? st->uid : m0.uid;   // an ungrouped part is its own row
                if (!drawn.insert(key).second) continue;
                int first = -1;
                size_t parts = 0, keys = 0;
                bool visible = false;
                for (int k = 0; k < (int)d.models.size(); ++k) {
                    StudioModel& mk = *d.models[k];
                    if (!mk.IsStage()) continue;
                    const StudioStage* sk = d.StageOfModel(mk);
                    const uint32_t kw = sk ? sk->uid : mk.uid;
                    if (kw != key) continue;
                    if (first < 0) first = k;
                    ++parts;
                    visible = visible || mk.visible;
                    for (const auto& [n, b] : mk.motion.bones) keys += b.size();
                    for (const auto& [n, b] : mk.motion.morphs) keys += b.size();
                }
                if (first < 0) continue;
                StudioModel& m = *d.models[(size_t)first];
                const std::string name = st && st->manifest ? st->manifest->DisplayName(StageLanguage()) : m.name;
                const bool open = studioStageOpen_.count(key) != 0;
                ImGui::PushID("stage");
                ImGui::PushID((int)key);
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                const ImVec2 a = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const ImVec2 b(a.x + w, a.y + rowH);
                const bool selected = d.selectedModel >= 0 && d.Selected() &&
                                      d.Selected()->IsStage() &&
                                      ((d.StageOfModel(*d.Selected()) ? d.StageOfModel(*d.Selected())->uid : d.Selected()->uid) == key);
                ImGui::InvisibleButton("##row", ImVec2(w - Dp(76.0f), rowH));
                uiprobe::Info(ImGui::GetItemID(), "##row", name.c_str());   // MCP ui_click by name
                const bool hovered = ImGui::IsItemHovered();
                // the caret sits in the row's left edge (drawn after the row background, which fills over it)
                const bool onCaret = hovered && ImGui::GetIO().MousePos.x < a.x + Dp(26.0f);
                if (ImGui::IsItemClicked() && onCaret) {
                    if (!studioStageOpen_.erase(key)) studioStageOpen_.insert(key);
                } else if (ImGui::IsItemClicked()) {
                    StudioSelectModel(first);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) menuFor = first;
                const bool rowHot = hovered || ImGui::IsMouseHoveringRect(a, b);
                const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
                if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
                else if (rowHot) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));
                const ImU32 fg = selected ? p.accentInk : p.ink;
                Icon(cdl, open ? icon::CaretDown : icon::CaretRight, 12.0f, ImVec2(a.x + Dp(20.0f), a.y + rowH * 0.5f),
                     selected ? p.accentInk : p.ink3);
                Icon(cdl, icon::Mountains, 16.0f, ImVec2(a.x + Dp(44.0f), a.y + rowH * 0.5f), selected ? p.accentInk : p.ink2);
                char stageSub[96];
                std::snprintf(stageSub, sizeof(stageSub), "%s · %d%s", Tr("스테이지"), (int)parts, Tr("부품"));
                const float tx = a.x + Dp(62.0f), maxX = b.x - Dp(78.0f);
                TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, name.c_str());
                TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, stageSub);
                if (rowHot || selected) {
                    ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(76.0f), a.y + Dp(6.0f)));
                    if (IconButton("##more", icon::DotsThree, Tr("모델 메뉴"), false, 28.0f)) menuFor = first;
                }
                ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
                if (IconButton("##vis", visible ? icon::Eye : icon::EyeSlash, visible ? Tr("숨기기") : Tr("보이기"), false, 28.0f)) {
                    for (auto& mk : d.models) {
                        if (!mk->IsStage()) continue;
                        const StudioStage* sk = d.StageOfModel(*mk);
                        const uint32_t kw = sk ? sk->uid : mk->uid;
                        if (kw != key) continue;
                        mk->visible = !visible;
                    }
                    ++d.projectVersion;
                }
                ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
                ImGui::Dummy(ImVec2(w, 0.0f));
                // the parts, indented (nothing when collapsed)
                if (open) {
                    for (int k = first; k < (int)d.models.size(); ++k) {
                        StudioModel& part = *d.models[k];
                        if (!part.IsStage()) continue;
                        const StudioStage* sk = d.StageOfModel(part);
                        if ((sk ? sk->uid : part.uid) != key) continue;
                        const int pi = k;
                        ImGui::PushID(pi);
                        ImDrawList* pdl = ImGui::GetWindowDrawList();
                        const ImVec2 pa = ImGui::GetCursorScreenPos();
                        const float pw = ImGui::GetContentRegionAvail().x;
                        const ImVec2 pb(pa.x + pw, pa.y + Dp(32.0f));
                        const bool psel = d.selectedModel == pi;
                        ImGui::SetCursorScreenPos(ImVec2(pa.x + Dp(28.0f), pa.y));
                        ImGui::InvisibleButton("##partrow", ImVec2(pw - Dp(28.0f + 76.0f), Dp(32.0f)));
                        uiprobe::Info(ImGui::GetItemID(), "##partrow", part.name.c_str());
                        const bool phover = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked()) StudioSelectModel(pi);
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) menuFor = pi;
                        const ImVec2 pra(pa.x + Dp(24.0f), pa.y + Dp(1.0f)), prb(pb.x - Dp(8.0f), pb.y - Dp(1.0f));
                        if (psel) pdl->AddRectFilled(pra, prb, p.accentSoft, Dp(6.0f));
                        else if (phover) pdl->AddRectFilled(pra, prb, WithAlpha(p.ink, 0.05f), Dp(6.0f));
                        Icon(pdl, icon::Cube, 12.0f, ImVec2(pa.x + Dp(42.0f), pa.y + Dp(16.0f)), p.ink3);
                        TextEllipsis(pdl, Font::Regular, size::Small, ImVec2(pa.x + Dp(56.0f), pa.y + Dp(8.0f)),
                                     pb.x - Dp(50.0f), psel ? p.accentInk : p.ink2, part.name.c_str());
                        ImGui::SetCursorScreenPos(ImVec2(pb.x - Dp(40.0f), pa.y + Dp(2.0f)));
                        if (IconButton("##pvis", part.visible ? icon::Eye : icon::EyeSlash,
                                       part.visible ? Tr("숨기기") : Tr("보이기"), false, 24.0f)) {
                            part.visible = !part.visible;
                            ++d.projectVersion;
                        }
                        ImGui::SetCursorScreenPos(ImVec2(pa.x, pb.y));
                        ImGui::Dummy(ImVec2(pw, 0.0f));
                        ImGui::PopID();
                    }
                }
                ImGui::PopID();
                ImGui::PopID();
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
            ImGui::TextWrapped("%s", Tr("캐릭터, 스테이지, 소품, 음원은 추가 › 장면에서 라이브러리나 파일로 넣어요."));
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(a.x, ImGui::GetCursorScreenPos().y + Dp(10.0f)));
            if (Button("##emptyadd", Tr("캐릭터 추가"), icon::Plus, ButtonKind::Secondary, ImVec2((w - Dp(32.0f)) / Dpi(), 36.0f))) {
                openAdd = true;
                studioAddPage_ = 1;
                studioReplaceUid_ = 0;
            }
        }
    }

    // --- 조명 group: the light rows (the add menu's 조명 page adds / applies presets) ---
    group(1, "##grp_light", Tr("조명"));
    if (studioOutlinerOpen_[1]) DrawStudioLightOutliner();

    // --- 월드 이펙트 group: world entity rows (installed world packs, keyable like effects) ---
    group(2, "##grp_world", Tr("월드 이펙트"));
    if (studioOutlinerOpen_[2]) {
        const float w = ImGui::GetContentRegionAvail().x;
        if (d.worlds.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(16.0f), ImGui::GetCursorScreenPos().y + Dp(2.0f)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - Dp(32.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            PushFont(Font::Regular, size::Caption);
            ImGui::TextWrapped("%s", Tr("월드 이펙트가 없어요. 추가 › 월드 이펙트에서 넣으세요."));
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(w - Dp(16.0f), Dp(6.0f)));
        }
        uint32_t removeWorld = 0;
        for (size_t i = 0; i < d.worlds.size(); ++i) {
            StudioWorld& wd = d.worlds[i];
            const ShaderPack* pk = ShaderPacks().Find(wd.fx.entry.pack);
            const std::string name = pk ? pk->name.Get(PackLanguage()) : (wd.name.empty() ? wd.fx.entry.pack : wd.name);
            ImGui::PushID("world");
            ImGui::PushID((int)wd.uid);
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b(a.x + w, a.y + rowH);
            const bool selected = d.selectedModel == -1 && d.selectedLightUid == 0 && studioSelectedWorld_ == wd.uid;
            ImGui::InvisibleButton("##row", ImVec2(w - Dp(76.0f), rowH));
            uiprobe::Info(ImGui::GetItemID(), "##row", name.c_str());
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) StudioSelectWorld(wd.uid);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##worldmenu");
            const bool rowHot = hovered || ImGui::IsMouseHoveringRect(a, b);
            const ImVec2 ra(a.x + Dp(8.0f), a.y + Dp(2.0f)), rb(b.x - Dp(8.0f), b.y - Dp(2.0f));
            if (selected) cdl->AddRectFilled(ra, rb, p.accentSoft, Dp(8.0f));
            else if (rowHot) cdl->AddRectFilled(ra, rb, WithAlpha(p.ink, 0.05f), Dp(8.0f));
            const ImU32 fg = selected ? p.accentInk : p.ink;
            Icon(cdl, icon::Sparkle, 16.0f, ImVec2(a.x + Dp(26.0f), a.y + rowH * 0.5f), selected ? p.accentInk : p.ink2);
            const float tx = a.x + Dp(44.0f), maxX = b.x - Dp(78.0f);
            TextEllipsis(cdl, Font::Semibold, size::Small, ImVec2(tx, a.y + Dp(4.0f)), maxX, fg, name.c_str());
            TextEllipsis(cdl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(21.0f)), maxX, p.ink3, Tr("월드 이펙트"));
            if (rowHot || selected) {
                ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(76.0f), a.y + Dp(6.0f)));
                if (IconButton("##worldmenu", icon::DotsThree, Tr("월드 메뉴"), false, 28.0f))
                    ImGui::OpenPopup("##worldmenu");
                if (ImGui::BeginPopup("##worldmenu")) {
                    if (MenuItem("##worldremove", Tr("삭제"), icon::Trash)) removeWorld = wd.uid;
                    ImGui::EndPopup();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(44.0f), a.y + Dp(6.0f)));
            bool on = wd.fx.entry.enabled;
            if (IconButton("##vis", on ? icon::Eye : icon::EyeSlash, on ? Tr("끄기") : Tr("켜기"), false, 28.0f)) {
                std::vector<StudioWorld> before = d.worlds;
                wd.fx.entry.enabled = !on;
                d.history.Push(std::make_unique<WorldsCommand>(d, !on ? Tr("월드 켜기") : Tr("월드 끄기"), before, d.worlds));
                ++d.projectVersion;
                d.rowsKey = ~0ull;
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
            ImGui::Dummy(ImVec2(w, 0.0f));
            ImGui::PopID();
            ImGui::PopID();
        }
        if (removeWorld != 0) {
            std::vector<StudioWorld> before = d.worlds;
            d.worlds.erase(std::remove_if(d.worlds.begin(), d.worlds.end(),
                                          [&](const StudioWorld& x) { return x.uid == removeWorld; }),
                           d.worlds.end());
            d.history.Push(std::make_unique<WorldsCommand>(d, Tr("월드 삭제"), before, d.worlds));
            if (studioSelectedWorld_ == removeWorld) studioSelectedWorld_ = 0;
            ++d.projectVersion;
            d.rowsKey = ~0ull;
        }
    }
    if (menuFor != -100) ImGui::OpenPopup("##modelmenu");
    static int menuModel = -1;  // the row whose menu is open (one popup at a time)
    if (menuFor != -100) menuModel = menuFor;
    DrawStudioModelMenu(menuModel);
    DrawStudioCameraRename();   // S4: the camera rename modal (its own popup)
    ImGui::EndChild();
    if (openAdd || studioOpenAdd_) {  // studioOpenAdd_: the model menu's replace entry
        studioOpenAdd_ = false;
        studioAddFilter_[0] = 0;
        ImGui::OpenPopup("##studioadd");
    }
    ImGui::SetNextWindowPos(ImVec2(x1 - Dp(12.0f + 28.0f), y0 + Dp(38.0f)), ImGuiCond_Appearing);
    DrawStudioAddMenu();
}

void App::StudioAddWorld(const std::string& packId) {
    StudioDoc& d = *studio_;
    const ShaderPack* pk = ShaderPacks().Find(packId);
    if (!pk) return;
    const std::string lang = PackLanguage();
    std::vector<StudioWorld> before = d.worlds;
    StudioWorld nw;
    nw.uid = d.nextWorldUid++;
    nw.name = pk->name.Get(lang);
    nw.fx.entry.pack = pk->id;
    nw.fx.entry.enabled = true;
    d.worlds.push_back(std::move(nw));
    d.history.Push(std::make_unique<WorldsCommand>(d, Tr("월드 추가"), before, d.worlds));
    ++d.projectVersion;
    d.rowsKey = ~0ull;
    studioSelectedWorld_ = d.worlds.back().uid;
    StudioSelectWorld(studioSelectedWorld_);
}

void App::StudioSelectWorld(uint32_t uid) {
    StudioDoc& d = *studio_;
    if (d.selectedModel == -1 && d.selectedLightUid == 0 && studioSelectedWorld_ == uid) return;
    StudioPossess(false);
    d.selectedModel = -1;
    d.selectedLightUid = 0;
    studioSelectedWorld_ = uid;
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
    d.expanded.clear();
    d.rowsKey = ~0ull;
}

// World inspector: placement (translation / rotation / scale; one undo step per drag) and the pack's keyable params.
void App::DrawStudioWorldPanel(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioWorld* world = d.FindWorld(studioSelectedWorld_);
    if (!world) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const ShaderPack* pk = ShaderPacks().Find(world->fx.entry.pack);

    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("트랜스폼"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    PropAttach place = world->place;
    bool dragEnded = false;
    const auto dragRow = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(64.0f));
        PushFont(Font::Regular, size::Small);
        widget();
        PopFont();
        if (ImGui::IsItemActivated()) { studioWorldBefore_ = d.worlds; studioWorldPlaceDragging_ = true; }
        if (ImGui::IsItemDeactivatedAfterEdit() && studioWorldPlaceDragging_) dragEnded = true;
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };
    dragRow(Tr("위치"), [&] { ImGui::DragFloat3("##worldplacet", &place.translation.x, 0.05f, 0.0f, 0.0f, "%.2f"); });
    dragRow(Tr("회전"), [&] { ImGui::DragFloat3("##worldplacer", &place.rotationDeg.x, 0.5f, 0.0f, 0.0f, "%.1f°"); });
    dragRow(Tr("크기"), [&] { ImGui::DragFloat("##worldplaces", &place.scale, 0.005f, 0.01f, 100.0f, "%.3f"); });
    place.scale = std::clamp(place.scale, 0.01f, 100.0f);
    if (!(place == world->place)) world->place = place;
    if (dragEnded) {
        studioWorldPlaceDragging_ = false;
        d.history.Push(std::make_unique<WorldsCommand>(d, Tr("월드 배치"), studioWorldBefore_, d.worlds));
        ++d.projectVersion;
    }

    if (pk && PackUsable(pk) && !pk->params.empty()) {
        ImGui::Dummy(ImVec2(w, Dp(8.0f)));
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("파라미터"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        if (DrawKeyableParams(*pk, world->fx, world->uid, RowKind::WorldParam, "##studioworld")) {
            ++d.projectVersion;
        }
    }
}

// Bone-key Euler degrees, same convention as the read-only display in DrawStudioInspector (pitch = asin(-m._32),
// yaw = atan2(m._31, m._33), roll = atan2(m._12, m._22) of XMMatrixRotationQuaternion); verified round trip with
// XMQuaternionRotationRollPitchYaw (max error ~2e-5 over random quaternions).
static DirectX::XMFLOAT3 StudioKeyEulerDeg(const DirectX::XMFLOAT4& q) {
    const DirectX::XMMATRIX r = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&q));
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, r);
    return {DirectX::XMConvertToDegrees(std::asin(std::clamp(-m._32, -1.0f, 1.0f))),
            DirectX::XMConvertToDegrees(std::atan2(m._31, m._33)),
            DirectX::XMConvertToDegrees(std::atan2(m._12, m._22)) + 0.0f};
}
static DirectX::XMFLOAT4 StudioKeyQuatFromEulerDeg(const DirectX::XMFLOAT3& e) {
    const DirectX::XMVECTOR q = DirectX::XMQuaternionRotationRollPitchYaw(DirectX::XMConvertToRadians(e.x),
        DirectX::XMConvertToRadians(e.y), DirectX::XMConvertToRadians(e.z));
    DirectX::XMFLOAT4 out;
    DirectX::XMStoreFloat4(&out, DirectX::XMQuaternionNormalize(q));
    return out;
}

void App::DrawStudioInspector(float x0, float y0, float x1, float y1) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);
    dl->AddLine(ImVec2(x0 + 0.5f, y0), ImVec2(x0 + 0.5f, y1), p.line);
    const float pad = Dp(18.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + pad, y0 + Dp(14.0f)));
    ImGui::BeginChild("##inspector", ImVec2(x1 - x0 - pad * 2.0f, y1 - y0 - Dp(14.0f)), ImGuiChildFlags_None,
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
        if (studioSelectedWorld_ != 0 && d.selectedModel == -1 && d.selection.empty()) {
            const StudioWorld* world = d.FindWorld(studioSelectedWorld_);
            if (world) {
                const ShaderPack* wpk = ShaderPacks().Find(world->fx.entry.pack);
                const ImVec2 wc = ImGui::GetCursorScreenPos();
                const std::string wname = wpk ? wpk->name.Get(PackLanguage())
                                             : (world->name.empty() ? world->fx.entry.pack : world->name);
                TextEllipsis(cdl, Font::Semibold, size::Body, wc, wc.x + w, p.ink, wname.c_str());
                ImGui::Dummy(ImVec2(w, Dp(28.0f)));
                DrawStudioWorldPanel(w);
                ImGui::EndChild();
                return;
            }
            studioSelectedWorld_ = 0;
        }
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const StudioModel* m = d.Selected();
        // S4: the camera target names the camera being edited (uid 0 = the main camera)
        const std::string camTitle = d.selectedCameraUid == 0 ? std::string(Tr("메인 카메라"))
                                                              : d.CameraLabel(d.selectedCameraUid);
        TextEllipsis(cdl, Font::Semibold, size::Body, c, c.x + w, p.ink, m ? m->name.c_str() : camTitle.c_str());
        ImGui::Dummy(ImVec2(w, Dp(28.0f)));
        if (m) {
            line(Tr("본 / 모프"), std::to_string(m->pmx->bones.size()) + " / " + std::to_string(m->pmx->morphs.size()));
            // S3: an MMDX stage part shows its stage entity's panel (variant, params, cues imported as keys, resets).
            if (d.StageOfModel(*m)) DrawStudioStagePanel(w);
            if (m->IsProp()) DrawStudioPropPanel(w);
            else DrawStudioPlacePanel(w);
            // P2.2: per-model physics off (saved in the project; baked keys play instead of physics)
            if (m->kind == ModelKind::Character && !PhysicsDrivenBones(*m->pmx).empty()) {
                ImGui::Dummy(ImVec2(w, Dp(6.0f)));
                bool on = !m->physicsOff;
                if (Switch("##modelphysics", Tr("물리 시뮬레이션"), &on, Tr("끄면 구운 키로 재생해요 (프로젝트에 저장)")) &&
                    on == m->physicsOff)
                    d.history.Push(std::make_unique<PhysicsOffCommand>(d, d.selectedModel, m->physicsOff, !on));
            }
            if (m->kind == ModelKind::Character) DrawStudioShaderRow(w);
            DrawStudioMaterialRow(w);
            DrawStudioMmeRow(w);
        } else {
            char counts[96];
            if (d.selectedCameraUid != 0)
                std::snprintf(counts, sizeof(counts), Tr("카메라 %d"), (int)d.SelectedCameraTrack().camera.size());
            else
                std::snprintf(counts, sizeof(counts), Tr("카메라 %d · 조명 %d · 섀도 %d · 초점 %d"),
                              (int)d.SelectedCameraTrack().camera.size(),
                              (int)d.camera.light.size(), (int)d.camera.shadow.size(), (int)d.camera.focus.size());
            line(Tr("키"), counts);
            DrawStudioCameraPanel(w);
        }
    }
    // models: keys / bone (pose) / morph tabs
    if (StudioPoseModel()) {
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        const char* tabs[] = {Tr("키"), Tr("본"), Tr("모프")};
        const char* tabIcons[] = {icon::Diamond, icon::Bone, icon::Sliders};
        d.inspectorTab = std::clamp(d.inspectorTab, 0, 2);
        if (studioInspectorToBones_) {   // a bone was just picked in the viewport
            ImGui::SetScrollHereY(0.0f);
            studioInspectorToBones_ = false;
        }
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
        if (studioInspectorToKey_) {  // a timeline key click: bring its fields up (the model panels sit above them)
            ImGui::SetScrollHereY(0.0f);
            studioInspectorToKey_ = false;
        }
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Small, c, p.ink2, buf);
        ImGui::Dummy(ImVec2(w, Dp(26.0f)));
    }
    // After Effects model: the inspector's VALUE fields always mean the playhead. A selected key that sits elsewhere
    // shows only its track, frame and interpolation (its value fields stay hidden): never an off-screen key edit.
    const bool keyAwayFromPlayhead = first->second != d.Frame();
    // the key's fields disappear (playback moved off the key): close a dangling key-section edit. A live field drag
    // (camera values, effect params) shares studioKeyEdit_ and keeps an item active: leave that one alone.
    if (keyAwayFromPlayhead && studioKeyEdit_ && !studioKeyLive_ && !ImGui::IsAnyItemActive()) StudioEndKeyEdit();
    const SceneLight* sceneLight = kind == RowKind::SceneLight ? d.FindLight(LightUidOfTrack(name)) : nullptr;
    // S4: the camera row's key belongs to the SELECTED camera: say which one it is
    const std::string trackName = kind == RowKind::Camera
                                      ? (d.selectedCameraUid == 0 ? std::string(Tr("카메라"))
                                                                  : d.CameraLabel(d.selectedCameraUid))
                                  : kind == RowKind::Light ? std::string(Tr("조명"))
                                  : kind == RowKind::Focus ? std::string(Tr("초점"))
                                                           : std::string(Tr("셀프 섀도"));
    line(IsCameraKind(kind) || kind == RowKind::SceneLight
             ? Tr("트랙")
             : (kind == RowKind::Bone ? Tr("본") : Tr("모프")),
         kind == RowKind::SceneLight ? (sceneLight ? LightDisplayName(sceneLight->name) : std::string())
                                     : IsCameraKind(kind) ? trackName : name);
    line(Tr("프레임"), std::to_string(first->second));
    if (IsCameraKind(kind) && !keyAwayFromPlayhead) {
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        // a camera key at the playhead: the camera panel left its "camera values" section to these fields
        if (DrawStudioCameraKeyFields(w, kind, first->second)) {  // light / shadow: no curves
            ImGui::EndChild();
            return;
        }
    }
    if (IsCameraKind(kind) && keyAwayFromPlayhead && kind != RowKind::Camera) {
        // light / shadow / focus keys have no interpolation controls: only their track and frame show here
        ImGui::EndChild();
        return;
    }
    if (kind == RowKind::Accessory) {  // accessory keys have no curve editor (linear / stepped only)
        if (keyAwayFromPlayhead) {  // the key's values are not the playhead's: only its track and frame show
            ImGui::EndChild();
            return;
        }
        DrawStudioAccessoryKeyFields(w, first->second);
        ImGui::EndChild();
        return;
    }
    if (kind == RowKind::SceneLight) {  // light keys: their values are editable here (one key at a time)
        SceneLight* light = sceneLight ? const_cast<SceneLight*>(sceneLight) : nullptr;
        LightKey* k = keyAwayFromPlayhead ? nullptr : (light ? FindKey(light->keys, first->second) : nullptr);
        if (k) {
            LightValues& v = k->v;
            const auto beginLightKeyEdit = [&] {
                if (studioKeyEdit_) return;
                studioKeyBefore_ = {CaptureTrack(d, -1, RowKind::SceneLight, LightTrackName(light->uid))};
                studioKeyEdit_ = true;
                studioKeyChanged_ = false;
            };
            bool colorRow = false;  // the color field reports its own activation (its popup outlives the swatch item)
            // the edit ends after this frame's change is written: a typed value arrives in the frame its field is left
            bool ended = false;
            const auto row = [&](const char* lbl, auto&& widget) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, lbl);
                ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
                ImGui::SetNextItemWidth(w - Dp(64.0f));
                PushFont(Font::Regular, size::Small);
                bool ch = widget();
                PopFont();
                if (!colorRow) {  // one undo step per drag / typed value
                    if (ImGui::IsItemActivated()) beginLightKeyEdit();
                    ended |= ItemEditEnded();
                }
                ImGui::Dummy(ImVec2(w, Dp(4.0f)));
                return ch;
            };
            bool changed = false, colorEnded = false;
            if (sceneLight->kind == LightKind::Sun) {
                float dir[3] = {v.direction.x, v.direction.y, v.direction.z};
                if (row(Tr("방향"), [&] { return ImGui::DragFloat3("##skeydir", dir, 0.01f, -1.0f, 1.0f, "%.2f"); })) {
                    beginLightKeyEdit();
                    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    if (len > 1e-6f) { v.direction = {dir[0] / len, dir[1] / len, dir[2] / len}; changed = true; }
                }
            } else {
                float pos[3] = {v.position.x, v.position.y, v.position.z};
                if (row(Tr("위치"), [&] { return ImGui::DragFloat3("##skeypos", pos, 0.02f, 0.0f, 0.0f, "%.2f"); })) {
                    beginLightKeyEdit();
                    v.position = {pos[0], pos[1], pos[2]};
                    changed = true;
                }
            }
            {
                float col[3] = {v.color.x, v.color.y, v.color.z};
                bool activated = false, deactivated = false;
                colorRow = true;
                const bool colChanged = row(Tr("색"), [&] { return ui::ColorField("##skeycol", col, &activated, &deactivated); });
                colorRow = false;
                if (colChanged) {
                    beginLightKeyEdit();
                    v.color = {col[0], col[1], col[2]};
                    changed = true;
                }
                if (activated) beginLightKeyEdit();
                colorEnded = deactivated;
            }
            {
                float inten = v.intensity;
                if (row(Tr("강도"), [&] { return ImGui::DragFloat("##skeyinten", &inten, 0.02f, 0.0f, FLT_MAX, "%.2f"); })) {
                    beginLightKeyEdit();
                    v.intensity = std::max(0.0f, inten);
                    changed = true;
                }
            }
            if (sceneLight->kind == LightKind::Spot) {
                float aim[3] = {v.aim.x, v.aim.y, v.aim.z};
                if (row(Tr("조준"), [&] { return ImGui::DragFloat3("##skeyaim", aim, 0.02f, 0.0f, 0.0f, "%.2f"); })) {
                    beginLightKeyEdit();
                    v.aim = {aim[0], aim[1], aim[2]};
                    changed = true;
                }
                float cone = DirectX::XMConvertToDegrees(v.coneOuter);
                if (row(Tr("원뿔 각도"), [&] { return ImGui::DragFloat("##skeycone", &cone, 0.25f, 1.0f, 89.0f, "%.1f°"); })) {
                    beginLightKeyEdit();
                    v.coneOuter = DirectX::XMConvertToRadians(std::clamp(cone, 1.0f, 89.0f));
                    if (v.coneInner > v.coneOuter) v.coneInner = v.coneOuter;
                    changed = true;
                }
            }
            if (changed) studioKeyChanged_ = true;
            if (ended || colorEnded) StudioEndKeyEdit();
        }
        ImGui::EndChild();
        return;
    }
    if (kind == RowKind::EffectParam || kind == RowKind::WorldParam) {
        uint32_t effectUid = 0;
        std::string param;
        if (kind == RowKind::EffectParam) ParseEffectParamTrackName(name, effectUid, param);
        else ParseWorldParamTrackName(name, effectUid, param);
        StudioWorld* world = kind == RowKind::WorldParam ? d.FindWorld(effectUid) : nullptr;
        StudioEffect* eff = kind == RowKind::WorldParam ? (world ? &world->fx : nullptr) : d.FindEffect(effectUid);
        const ShaderPack* pk = eff ? ShaderPacks().Find(eff->entry.pack) : nullptr;
        const std::string packName = pk ? pk->name.Get(PackLanguage()) : (eff ? eff->entry.pack : std::string());
        std::string paramLabel = param;
        float pmin = 0.0f, pmax = 1.0f;
        float pstep = 0.0f;
        ShaderPackParamType ptype = ShaderPackParamType::Float;
        if (pk) {
            std::string baseKey = param;
            std::string compSuffix;
            size_t hashPos = param.find('#');
            if (hashPos != std::string::npos) {
                baseKey = param.substr(0, hashPos);
                compSuffix = param.substr(hashPos);
            }
            for (const auto& sp : pk->params) {
                if (sp.key == baseKey) {
                    ptype = sp.type;
                    if (!sp.label.Empty()) paramLabel = sp.label.Get(PackLanguage());
                    if (compSuffix.size() > 1) {
                        int c = compSuffix[1] - '0';
                        if (sp.type == ShaderPackParamType::Color) {
                            static const char* kCol[4] = {" · R", " · G", " · B", " · A"};
                            if (c >= 0 && c < 4) paramLabel += kCol[c];
                        } else {
                            static const char* kVec[3] = {" · X", " · Y", " · Z"};
                            if (c >= 0 && c < 3) paramLabel += kVec[c];
                        }
                    }
                    pmin = sp.min;
                    pmax = sp.max;
                    pstep = sp.step;
                    break;
                }
            }
        }
        line(Tr("효과"), packName);
        line(Tr("파라미터"), paramLabel);
        EffectParamKey* pkKey = nullptr;        if (eff) {
            for (auto& t : eff->tracks) {
                if (t.param == param) {
                    for (auto& k : t.keys) {
                        if (k.frame == first->second) { pkKey = &k; break; }
                    }
                    break;
                }
            }
        }
        if (pkKey) {
            float val = pkKey->value;
            if (!keyAwayFromPlayhead) {
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
            const auto row = [&](const char* lbl, auto&& widget) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, lbl);
                ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
                ImGui::SetNextItemWidth(w - Dp(64.0f));
                PushFont(Font::Regular, size::Small);
                bool ch = widget();
                PopFont();
                if (ImGui::IsItemActivated()) {
                    studioKeyBefore_ = {CaptureTrack(d, -1, kind,
                        kind == RowKind::WorldParam ? WorldParamTrackName(effectUid, param)
                                                    : EffectParamTrackName(effectUid, param))};
                    studioKeyEdit_ = true;
                    studioKeyChanged_ = false;
                }
                if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
                ImGui::Dummy(ImVec2(w, Dp(4.0f)));
                return ch;
            };
            bool valChanged = false;
            if (ptype == ShaderPackParamType::Bool) {
                bool b = (val > 0.5f);
                if (row(Tr("값"), [&] { return Switch("##effparambool", Tr("켬"), &b); })) {
                    val = b ? 1.0f : 0.0f;
                    valChanged = true;
                }
            } else if (ptype == ShaderPackParamType::Int) {
                if (row(Tr("값"), [&] { return ImGui::SliderFloat("##effparamval", &val, pmin, pmax, "%.0f"); })) {
                    val = std::round(val);
                    valChanged = true;
                }
            } else {
                const char* fmt = pstep > 0.0f ? (pstep >= 1.0f ? "%.0f" : "%.2f") : "%.2f";
                if (row(Tr("값"), [&] { return ImGui::SliderFloat("##effparamval", &val, pmin, pmax, fmt); })) {
                    if (pstep > 0.0f) val = pmin + std::round((val - pmin) / pstep) * pstep;
                    valChanged = true;
                }
            }
            if (valChanged) {
                pkKey->value = val;
                studioKeyChanged_ = true;
                d.rowsKey = ~0ull;
            }
            }
            bool stepped = pkKey->stepped;
            if (Switch("##effparamstepped", Tr("계단식 보간"), &stepped, Tr("다음 키까지 현재 값을 유지합니다"))) {
                std::vector<TrackState> before = {CaptureTrack(d, -1, kind,
                    kind == RowKind::WorldParam ? WorldParamTrackName(effectUid, param)
                                                : EffectParamTrackName(effectUid, param))};
                pkKey->stepped = stepped;
                StudioPushTrackEdit(Tr("보간 변경"), before);
                d.rowsKey = ~0ull;
            }
        }
        ImGui::EndChild();
        return;
    }
    if (kind == RowKind::StageParam) {
        // S3: the selected stage param key's value (one undoable TrackEditCommand per gesture, like an effect param)
        uint32_t stageUid = 0;
        std::string param;
        ParseStageParamTrackName(name, stageUid, param);
        StudioStage* stage = d.FindStage(stageUid);
        if (!stage) {
            ImGui::EndChild();
            return;
        }
        int comp = -1;
        const std::string key = StageParamBaseKey(param, comp);
        const StageParam* sp = stage->manifest ? stage->manifest->FindParam(key) : nullptr;
        std::string paramLabel = StageParamLabel(*stage, param);
        if (sp && !sp->group.empty() && stage->manifest) {
            for (const StageParamGroup& g : stage->manifest->groups)
                if (g.id == sp->group) {
                    const std::string gl = g.label.Empty() ? g.id : g.label.Get(StageLanguage());
                    paramLabel = gl + " · " + paramLabel;
                    break;
                }
        }
        line(Tr("스테이지"), stage->manifest ? stage->manifest->DisplayName(StageLanguage()) : stage->stageId);
        line(Tr("파라미터"), paramLabel);
        EffectParamKey* skKey = nullptr;
        if (EffectParamTrack* tr = FindStageParamTrack(*stage, param))
            for (EffectParamKey& k : tr->keys)
                if (k.frame == first->second) { skKey = &k; break; }
        if (skKey) {
            float val = skKey->value;
            const StageParamType type = sp ? sp->type : StageParamType::Float;
            float pmin = 0.0f, pmax = 1.0f;
            if (sp && type != StageParamType::Color) { pmin = sp->min; pmax = sp->max; }
            const auto row = [&](const char* lbl, auto&& widget) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, lbl);
                ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
                ImGui::SetNextItemWidth(w - Dp(64.0f));
                PushFont(Font::Regular, size::Small);
                bool ch = widget();
                PopFont();
                if (ImGui::IsItemActivated()) {
                    studioKeyBefore_ = {CaptureTrack(d, -1, kind, name)};
                    studioKeyEdit_ = true;
                    studioKeyChanged_ = false;
                }
                if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
                ImGui::Dummy(ImVec2(w, Dp(4.0f)));
                return ch;
            };
            bool valChanged = false;
            if (type == StageParamType::Bool) {
                bool b = (val > 0.5f);
                if (row(Tr("값"), [&] { return Switch("##stageparambool", Tr("켬"), &b); })) {
                    val = b ? 1.0f : 0.0f;
                    valChanged = true;
                }
            } else if (type == StageParamType::Int || type == StageParamType::Enum) {
                if (row(Tr("값"), [&] { return ImGui::SliderFloat("##stageparamval", &val, pmin, pmax, "%.0f"); })) {
                    val = std::round(val);
                    valChanged = true;
                }
            } else {
                if (row(Tr("값"), [&] { return ImGui::SliderFloat("##stageparamval", &val, pmin, pmax, "%.3f"); })) {
                    valChanged = true;
                }
            }
            if (valChanged && !keyAwayFromPlayhead) {
                skKey->value = val;
                studioKeyChanged_ = true;
                d.rowsKey = ~0ull;
            }
            bool stepped = skKey->stepped;
            if (Switch("##stageparamstepped", Tr("계단식 보간"), &stepped, Tr("다음 키까지 현재 값을 유지합니다"))) {
                std::vector<TrackState> before = {CaptureTrack(d, -1, kind, name)};
                skKey->stepped = stepped;
                StudioPushTrackEdit(Tr("보간 변경"), before);
                d.rowsKey = ~0ull;
            }
        }
        ImGui::EndChild();
        return;
    }

    StudioModel* m = d.Selected();
    if (kind == RowKind::Morph) {
        const MorphKf* k = m ? FindKey(m->motion.morphs[name], first->second) : nullptr;
        float weight = k ? k->weight : 0.0f;
        const auto row = [&](const char* lbl, auto&& widget) {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, lbl);
            ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
            ImGui::SetNextItemWidth(w - Dp(64.0f));
            PushFont(Font::Regular, size::Small);
            bool ch = widget();
            PopFont();
            if (ImGui::IsItemActivated()) {
                studioKeyBefore_.clear();
                // every selected key that is a morph row of this model: one undo step covers all of them
                for (const KeyId& s : d.selection) {
                    RowKind kk;
                    std::string nn;
                    if (!StudioTrackOfRow(s.first, kk, nn) || kk != RowKind::Morph) continue;
                    studioKeyBefore_.push_back(CaptureTrack(d, d.selectedModel, kk, nn));
                }
                studioKeyEdit_ = true;
                studioKeyChanged_ = false;
            }
            if (ImGui::IsItemDeactivated()) StudioEndKeyEdit();
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
            return ch;
        };
        if (row(Tr("값"), [&] { return ImGui::SliderFloat("##morphkey", &weight, 0.0f, 1.0f, "%.3f"); })) {
            for (const KeyId& s : d.selection) {
                RowKind kk;
                std::string nn;
                if (!StudioTrackOfRow(s.first, kk, nn) || kk != RowKind::Morph) continue;
                if (MorphKf* mk = FindKey(m->motion.morphs[nn], s.second)) mk->weight = std::clamp(weight, 0.0f, 1.0f);
            }
            studioKeyChanged_ = true;
            ++m->motionVersion;
        }
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
        BoneKf* k = m ? FindKey(m->motion.bones[name], first->second) : nullptr;
        if (k) {
            // One undo step per drag / typed value. An activation joins an open edit, a change opens one when none is
            // open (a field activated as another field's edit ends only joins it), and the edit ends after this
            // frame's change is written (a typed value arrives in the frame its field is left).
            const auto begin = [&] {
                if (studioKeyEdit_) return;
                studioKeyBefore_ = {CaptureTrack(d, d.selectedModel, kind, name)};
                studioKeyEdit_ = true;
                studioKeyChanged_ = false;
            };
            bool ended = false;
            const auto row = [&](const char* lbl, auto&& widget) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, lbl);
                ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
                ImGui::SetNextItemWidth(w - Dp(64.0f));
                PushFont(Font::Regular, size::Small);
                bool ch = widget();
                PopFont();
                if (ImGui::IsItemActivated()) begin();
                ended |= ItemEditEnded();
                ImGui::Dummy(ImVec2(w, Dp(4.0f)));
                return ch;
            };
            {
                float pos[3] = {k->t.x, k->t.y, k->t.z};
                if (row(Tr("위치"), [&] { return ImGui::DragFloat3("##bonekeypos", pos, 0.02f, 0.0f, 0.0f, "%.2f"); })) {
                    begin();
                    k->t = {pos[0], pos[1], pos[2]};
                    studioKeyChanged_ = true;
                    ++m->motionVersion;
                }
            }
            {
                // MMD shows bone rotations as Euler degrees
                const DirectX::XMFLOAT3 e = StudioKeyEulerDeg(k->r);
                float rot[3] = {e.x, e.y, e.z};
                if (row(Tr("회전"), [&] { return ImGui::DragFloat3("##bonekeyrot", rot, 0.25f, 0.0f, 0.0f, "%.1f°"); })) {
                    begin();
                    k->r = StudioKeyQuatFromEulerDeg({rot[0], rot[1], rot[2]});
                    studioKeyChanged_ = true;
                    ++m->motionVersion;
                }
            }
            if (ended) StudioEndKeyEdit();
            d.curveChannel = std::clamp(d.curveChannel, 0, 3);
            GetBoneCurve(k->interp, d.curveChannel, curve);
            for (int ch = 0; ch < 4; ++ch) GetBoneCurve(k->interp, ch, channelCurves[ch]);
        }
    } else {
        const CameraKf* k = FindKey(d.SelectedCameraTrack().camera, first->second);
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

    // the curve grows with the panel (widen or float the inspector for a large editor), capped so it stays a square
    const float plot = std::clamp(w, Dp(168.0f), Dp(420.0f));
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
                if (CameraKf* c = FindKey(d.SelectedCameraTrack().camera, k.second))
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
    if (ImGui::IsWindowFocused())
        studioFocusPanel_ = 1;
    else if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
             ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1)))
        studioFocusPanel_ = 1;  // a click in the timeline body / transport focuses it
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
    ImGui::SameLine(0, Dp(2.0f));
    // volume: one icon, the slider lives in its popover (the transport stays one quiet row)
    {
        const float vol = settings_.volume;
        const char* glyph = vol <= 0.001f ? icon::SpeakerX : (vol < 0.5f ? icon::SpeakerLow : icon::SpeakerHigh);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!d.hasAudio);
        if (IconButton("##volume", glyph, d.hasAudio ? Tr("음량") : Tr("음량  (음원 없음)"), false, 34.0f))
            ImGui::OpenPopup("##studiovolume");
        ImGui::EndDisabled();
        ImGui::SetNextWindowPos(ImVec2(at.x, at.y - Dp(6.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(240.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(14.0f), Dp(12.0f)));
        if (ImGui::BeginPopup("##studiovolume")) {
            float v = settings_.volume * 100.0f;
            if (SliderRow("##studiovol", Tr("음량"), &v, 0.0f, 100.0f, "%.0f%%")) {
                settings_.volume = std::clamp(v / 100.0f, 0.0f, 1.0f);
                audio_.SetVolume(settings_.volume);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::SameLine(0, Dp(12.0f));
    }
    // frame field with one-frame steppers (also ← / →): applied on Enter (typing must not seek on every digit)
    const auto stepFrame = [&](int delta) {
        StudioSetPlaying(false);
        StudioSeek(std::max(0, d.Frame() + delta) / (double)kMmdFps);
    };
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cy - Dp(13.0f)));
    if (IconButton("##prevframe", icon::CaretLeft, Tr("이전 프레임  (←)"), false, 26.0f)) stepFrame(-1);
    ImGui::SameLine(0, Dp(2.0f));
    int frame = d.Frame();
    ImGui::SetNextItemWidth(Dp(72.0f));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cy - ImGui::GetFrameHeight() * 0.5f));
    if (ImGui::InputInt("##frame", &frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        StudioSetPlaying(false);
        StudioSeek(std::max(0, frame) / (double)kMmdFps);
    }
    Tooltip(Tr("현재 프레임 (Enter로 이동)"));
    ImGui::SameLine(0, Dp(2.0f));
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cy - Dp(13.0f)));
    if (IconButton("##nextframe", icon::CaretRight, Tr("다음 프레임  (→)"), false, 26.0f)) stepFrame(1);
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
        if (IconButton("##clearrange", icon::X, Tr("구간 해제"), false, 28.0f)) d.view.rangeStart = d.view.rangeEnd = -1;
    }

    // right side: frame insert/delete | physics | view
    float rx = x1 - Dp(12.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    ImGui::BeginDisabled(!d.HasCameraKeys());
    if (IconButton("##motioncam", icon::VideoCamera,
                   d.useMotionCamera ? Tr("카메라 모션으로 보는 중 (클릭: 자유 카메라)") : Tr("자유 카메라 (클릭: 카메라 모션)"),
                   d.useMotionCamera && d.HasCameraKeys(), 34.0f))
        d.useMotionCamera = !d.useMotionCamera;
    ImGui::EndDisabled();
    separator(rx - Dp(8.0f));
    rx -= Dp(16.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    if (IconButton("##physreset", icon::ArrowCcw, Tr("물리 초기화"), false, 34.0f)) d.physicsFrame = -1.0f;
    rx -= Dp(2.0f + 34.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(17.0f)));
    if (IconButton("##physics", icon::Atom, d.physics ? Tr("물리 켜짐 (클릭: 끄기)") : Tr("물리 꺼짐 (클릭: 켜기)"), d.physics, 34.0f))
        StudioTogglePhysics();
    rx -= Dp(28.0f);
    const ImVec2 physAt(rx, cy - Dp(17.0f));
    ImGui::SetCursorScreenPos(physAt);
    if (IconButton("##physmenu", icon::CaretDown, Tr("물리/중력/바람 설정"), false, 24.0f)) ImGui::OpenPopup("##studiophysics");
    ImGui::SetNextWindowPos(ImVec2(physAt.x + Dp(24.0f), physAt.y - Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
    // as tall as its rows, but never past the window's top edge: in a short window the popup scrolls instead
    ImGui::SetNextWindowSizeConstraints(ImVec2(Dp(300.0f), 0.0f),
                                        ImVec2(Dp(300.0f), std::max(Dp(160.0f), physAt.y - Dp(6.0f + 8.0f))));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(14.0f), Dp(12.0f)));
    // SliderRow's last item is the slider itself, so a drag shows up as IsItemActive() on the row. The document tracks
    // the gesture (StudioDoc::physicsEnvEditing): it becomes one undo step when it ends (PhysicsEnvCommand, which also
    // re-seeds the bodies), not one per slider tick, so sliding doesn't freeze or thrash physics.
    const auto pushEnv = [&](const PhysicsEnv& before, const PhysicsEnv& after) {
        if (after == before) return;
        const char* name = after.SameGravity(before) ? Tr("바람 변경") : Tr("중력 변경");
        d.history.Push(std::make_unique<PhysicsEnvCommand>(d, name, before, after));
    };
    if (ImGui::BeginPopup("##studiophysics")) {
        PhysicsEnv& env = d.physicsEnv;
        const PhysicsEnv frameStart = env;  // a gesture's first frame already moves the value
        bool anyActive = false;
        // The values right after the gesture's own control was drawn this frame: its step ends there when another
        // control takes over in the same frame (a typed value commits as the click lands on a slider).
        bool ownerSeen = false;
        PhysicsEnv ownerAfter;
        const auto noteItem = [&] {
            anyActive |= ImGui::IsItemActive();
            if (d.physicsEnvEditing && ImGui::GetItemID() == d.physicsEnvControl) {
                ownerSeen = true;
                ownerAfter = env;
            }
        };
        {
            const auto windRow = [&](const char* id, const char* label, float* v, float lo, float hi) {
                SliderRow(id, label, v, lo, hi, "%.2f");
                noteItem();
            };
            // label + field rows, as the inspector's key fields (a plain click types a value); the label column fits
            // the longer label (English "Acceleration")
            const char* accelLabel = Tr("가속도");
            const char* dirLabel = Tr("방향");
            const float labelW = std::max(Dp(64.0f), std::max(TextSize(Font::Regular, size::Small, accelLabel).x,
                                                              TextSize(Font::Regular, size::Small, dirLabel).x) + Dp(10.0f));
            const auto fieldRow = [&](const char* label, auto&& widgets) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
                ImGui::SetCursorScreenPos(ImVec2(c.x + labelW, c.y));
                PushFont(Font::Regular, size::Small);
                widgets(w - labelW);
                PopFont();
                ImGui::Dummy(ImVec2(w, Dp(2.0f)));
            };
            const auto field = [&](const char* id, float* v, float speed, float lo, float hi, float width, const char* tip) {
                ImGui::SetNextItemWidth(width);
                ImGui::DragFloat(id, v, speed, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                noteItem();
                ui::Tooltip(tip);
            };
            Switch("##physalways", Tr("물리 항상 계산"), &d.physicsAlways);
            ui::Tooltip(Tr("재생 중이 아니어도 같은 프레임에 멈춰 있을 때 물리를 계속 계산해 (머리카락/치마가 계속 흔들림)"));
            Switch("##physbones", Tr("물리 꺼도 본 표시"), &d.showPhysicsBones);
            ui::Tooltip(Tr("물리를 꺼도 물리 뼈(머리카락/치마)를 뷰포트에 표시해 직접 키를 넣을 수 있게"));
            // gravity: MMD's gravity settings (acceleration + direction) without their noise and keys
            SectionLabel(Tr("중력"));
            {   // MMD's default on the heading line's right end
                const ImVec2 back = ImGui::GetCursorScreenPos();
                const float headY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(back.x + ImGui::GetContentRegionAvail().x - Dp(24.0f), headY - Dp(12.0f)));
                ImGui::BeginDisabled(env.DefaultGravity());
                if (IconButton("##gravityreset", icon::ArrowCcw, Tr("중력을 기본값으로 (9.8, 아래쪽)"), false, 24.0f)) {
                    PhysicsEnv after = env;
                    after.gravity = PhysicsEnv::kDefaultGravity;
                    after.gravityDir = {0, -1, 0};
                    d.history.Push(std::make_unique<PhysicsEnvCommand>(d, Tr("중력 변경"), env, after));
                }
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(back);
            }
            fieldRow(accelLabel, [&](float width) {
                field("##gravity", &env.gravity, 0.05f, 0.0f, 200.0f, width, Tr("MMD와 같은 값이에요 (기본 9.8). 0이면 무중력"));
            });
            fieldRow(dirLabel, [&](float width) {  // X / Y / Z laid out as DragFloat3, one control (undo step) each
                const char* tip = Tr("중력이 향하는 방향 (기본 0, -1, 0 = 아래쪽). 크기는 가속도가 정하고, 모두 0이면 무중력");
                const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
                const float each = std::floor((width - gap * 2.0f) / 3.0f);
                const char* ids[3] = {"##gravityx", "##gravityy", "##gravityz"};
                float* axes[3] = {&env.gravityDir.x, &env.gravityDir.y, &env.gravityDir.z};
                for (int i = 0; i < 3; ++i) {
                    if (i) ImGui::SameLine(0, gap);
                    field(ids[i], axes[i], 0.01f, -1.0f, 1.0f, i < 2 ? each : width - (each + gap) * 2.0f, tip);
                }
            });
            SectionLabel(Tr("바람"));
            Switch("##windvec", Tr("뷰포트에 바람 벡터 표시"), &d.showWindVector);
            const bool hasWind = env.HasWind();
            // Direction visualizer (top-down: +X right, +Z up) with the two X/Z slider rows to its right. The pad is
            // drawn after the rows so it takes exactly their height and both columns line up.
            const float padW = Dp(84.0f);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(padW, 1.0f));
            ImGui::SameLine(0, Dp(10.0f));
            ImGui::BeginDisabled(!hasWind);  // no wind: direction controls are greyed out
            ImGui::BeginGroup();
            windRow("##windx", Tr("바람 방향 X"), &env.windDir.x, -1.0f, 1.0f);
            windRow("##windz", Tr("바람 방향 Z"), &env.windDir.z, -1.0f, 1.0f);
            ImGui::EndGroup();
            ImGui::EndDisabled();
            const float padH = std::max(ImGui::GetItemRectMax().y - rowStart.y, Dp(60.0f));
            {
                const float side = std::min(padW, padH);  // square pad, as tall as the two slider rows allow
                const ImVec2 c0(rowStart.x, rowStart.y + (padH - side) * 0.5f);
                ImDrawList* wdl = ImGui::GetWindowDrawList();
                wdl->AddRectFilled(c0, ImVec2(c0.x + side, c0.y + side), p.sunken, Dp(8.0f));
                wdl->AddRect(c0, ImVec2(c0.x + side, c0.y + side), p.line, Dp(8.0f));
                const ImVec2 cc(c0.x + side * 0.5f, c0.y + side * 0.5f);
                wdl->AddLine(cc, ImVec2(c0.x + side - Dp(6.0f), cc.y), IM_COL32(235, 75, 75, 90));
                wdl->AddLine(cc, ImVec2(cc.x, c0.y + Dp(6.0f)), IM_COL32(70, 130, 245, 90));
                float nx = env.windDir.x, nz = env.windDir.z;
                const float len = std::sqrt(nx * nx + nz * nz);
                if (len > 1e-5f) { nx /= len; nz /= len; }
                if (hasWind && len > 1e-5f) {
                    const float r = side * 0.34f;
                    const ImVec2 tip(cc.x + nx * r, cc.y - nz * r);
                    const ImU32 arrowCol = IM_COL32(140, 230, 150, 235);
                    wdl->AddLine(cc, tip, arrowCol, Dp(2.0f));
                    const float ah = Dp(8.0f), hw = Dp(4.5f);
                    const float dx = nx, dy = -nz;
                    const float bx = tip.x - dx * ah, by = tip.y - dy * ah;
                    const float px = -nz * hw, py = nx * hw;
                    wdl->AddTriangleFilled(tip, ImVec2(bx + px, by + py), ImVec2(bx - px, by - py), arrowCol);
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + padH));
            windRow("##wstrength", Tr("바람 세기"), &env.windStrength, 0.0f, 1.0f);
            windRow("##wgust", Tr("돌풍 폭"), &env.windGust, 0.0f, 1.0f);
            windRow("##wgustfreq", Tr("돌풍 주파수"), &env.windGustFreq, 0.1f, 20.0f);
            windRow("##wjitter", Tr("바람 무작위성"), &env.windJitter, 0.0f, 1.0f);
        }
        // One step per control: a gesture ends when its control lets go, or when another control takes over in the
        // same frame; the next step then starts where the old control left the values.
        const uint32_t control = anyActive ? ImGui::GetActiveID() : 0;
        PhysicsEnv start = frameStart;
        if (d.physicsEnvEditing && control != d.physicsEnvControl) {
            const PhysicsEnv now = env;
            const PhysicsEnv end = control == 0 ? now : ownerSeen ? ownerAfter : frameStart;
            pushEnv(d.physicsEnvBefore, end);
            env = now;  // the next control's change of this frame stays
            start = end;
            d.physicsEnvEditing = false;
        }
        if (control && !d.physicsEnvEditing) {
            d.physicsEnvEditing = true;
            d.physicsEnvControl = control;
            d.physicsEnvBefore = start;
        }
        ImGui::EndPopup();
    } else if (d.physicsEnvEditing) {  // closed in the middle of a gesture: still one step
        pushEnv(d.physicsEnvBefore, d.physicsEnv);
        d.physicsEnvEditing = false;
    }
    ImGui::PopStyleVar();
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
        // track-type markers on the camera target's rows (bone / morph rows stay plain: they are the bulk)
        for (TimelineRow& r : d.rows) {
            switch (RowKindOf(r.id)) {
                case RowKind::Camera: r.tint = IM_COL32(64, 170, 230, 255); break;
                case RowKind::Light: case RowKind::SceneLight: r.tint = IM_COL32(236, 170, 50, 255); break;
                case RowKind::Shadow: r.tint = IM_COL32(150, 110, 220, 255); break;
                case RowKind::Focus: r.tint = IM_COL32(232, 96, 150, 255); break;
                case RowKind::Accessory: r.tint = IM_COL32(120, 200, 160, 255); break;
                case RowKind::EffectParam: r.tint = IM_COL32(100, 210, 210, 255); break;
                case RowKind::WorldParam: r.tint = IM_COL32(150, 190, 240, 255); break;
                case RowKind::StageParam: r.tint = IM_COL32(190, 160, 240, 255); break;
                default: break;
            }
        }
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
    // The lane's editable segment, filled from live key data every frame (no row rebuild needed): the key selected on
    // this row (when exactly one is selected and it is not the row's first key) or, else, the segment [a, b) holding the
    // playhead (at or after the last key: the last segment; before the first key: none).
    std::set<uint64_t> graphRows;  // rows that offer a lane (the only ones the segment search needs)
    for (const TimelineRow& r : d.rows)
        if (!r.graphChannels.empty()) graphRows.insert(r.id);
    std::map<uint64_t, std::pair<int, int>> rowSel;  // graph row -> (selected key count, first selected frame)
    for (const KeyId& k : d.selection) {
        if (!graphRows.count(k.first)) continue;
        auto& e = rowSel[k.first];
        if (e.first == 0) e.second = k.second;
        ++e.first;
    }
    for (TimelineRow& r : d.rows) {
        r.graphSegFrom = r.graphSegTo = -1;
        r.graphCurvesDiffer = false;
        if (r.graphChannels.empty()) continue;
        const RowKind kind = RowKindOf(r.id);
        const std::vector<CameraKf>* cameraKeys = nullptr;
        const std::vector<BoneKf>* boneKeys = nullptr;
        if (kind == RowKind::Bone) {
            RowKind k;
            std::string name;
            if (!StudioTrackOfRow(r.id, k, name) || k != RowKind::Bone) continue;
            const StudioModel* m = d.Selected();
            if (!m) continue;
            const auto it = m->motion.bones.find(name);
            if (it == m->motion.bones.end()) continue;
            boneKeys = &it->second;
        } else if (kind == RowKind::Camera && d.selectedModel < 0) {
            cameraKeys = &d.SelectedCameraTrack().camera;
        } else {
            continue;
        }
        const size_t keyCount = boneKeys ? boneKeys->size() : cameraKeys->size();
        if (keyCount < 2) continue;
        const auto frameAt = [&](size_t i) { return boneKeys ? (*boneKeys)[i].frame : (*cameraKeys)[i].frame; };
        size_t from = 0, to = 0;
        bool have = false;
        const auto sel = rowSel.find(r.id);
        const int selectedFrame = sel != rowSel.end() && sel->second.first == 1 ? sel->second.second : -1;
        if (selectedFrame >= 0 && selectedFrame != frameAt(0)) {
            for (size_t i = 1; i < keyCount; ++i) {
                if (frameAt(i) == selectedFrame) { from = i - 1; to = i; have = true; break; }
            }
        }
        if (!have) {
            const int frame = d.Frame();
            if (frame >= frameAt(0)) {
                for (size_t i = 1; i < keyCount; ++i) {
                    if (frame < frameAt(i)) { from = i - 1; to = i; have = true; break; }
                }
                if (!have) { from = keyCount - 2; to = keyCount - 1; have = true; }  // at or after the last key
            }
        }
        if (!have) continue;
        const int a = frameAt(from), b = frameAt(to);
        if (b <= a) continue;
        if (cameraKeys && b - a <= 1) continue;  // a one-frame camera gap is a cut: nothing to edit
        // every channel's bytes of the segment (each graph lane block shows / edits its own channel's curve)
        r.graphCurves.assign(r.graphChannels.size(), {});
        for (int ch = 0; ch < (int)r.graphChannels.size(); ++ch) {
            uint8_t c[4];
            if (boneKeys) GetBoneCurve((*boneKeys)[to].interp, ch, c);
            else GetCameraCurve((*cameraKeys)[to].interp, ch, c);
            r.graphCurves[(size_t)ch] = {c[0], c[1], c[2], c[3]};
        }
        r.graphCurvesDiffer = false;  // "모든 채널" shows channel 0's curve; a drag writes it to every channel
        for (size_t ch = 1; ch < r.graphCurves.size(); ++ch)
            if (r.graphCurves[ch] != r.graphCurves[0]) r.graphCurvesDiffer = true;
        r.graphSegFrom = a;
        r.graphSegTo = b;
    }
    // Graph lane sampler: reads the live motion data by row id on every call (never caches pointers into d.rows or the
    // tracks), so an inspector curve edit shows in the lane in the same frame. Bone: 0..2 translation X/Y/Z, 3 rotation
    // (degrees of the eased slerp). Camera: 0..2 target, 3 rotation (degrees), 4 distance, 5 fov degrees.
    d.view.graphSample = [this](uint64_t row, int channel, float frame) -> float {
        const StudioDoc& d = *studio_;
        const RowKind kind = RowKindOf(row);
        std::string name;
        if (kind == RowKind::Bone) {
            RowKind k;
            std::string n;
            if (!StudioTrackOfRow(row, k, n) || k != RowKind::Bone) return 0.0f;
            name = n;
        } else if (kind != RowKind::Camera || d.selectedModel >= 0) {
            return 0.0f;
        }
        if (kind == RowKind::Bone) {
            const StudioModel* m = d.Selected();
            if (!m) return 0.0f;
            const auto it = m->motion.bones.find(name);
            if (it == m->motion.bones.end() || it->second.empty()) return 0.0f;
            const std::vector<BoneKf>& keys = it->second;
            if (frame <= (float)keys.front().frame) {
                const BoneKf& k0 = keys.front();
                if (channel == 0) return k0.t.x;
                if (channel == 1) return k0.t.y;
                if (channel == 2) return k0.t.z;
                return DirectX::XMConvertToDegrees(2.0f * std::acos(std::min(1.0f, std::abs(k0.r.w))));
            }
            if (frame >= (float)keys.back().frame || keys.size() == 1) {
                const BoneKf& k0 = keys.back();
                if (channel == 0) return k0.t.x;
                if (channel == 1) return k0.t.y;
                if (channel == 2) return k0.t.z;
                return DirectX::XMConvertToDegrees(2.0f * std::acos(std::min(1.0f, std::abs(k0.r.w))));
            }
            auto seg = std::upper_bound(keys.begin(), keys.end(), frame,
                                        [](float f, const BoneKf& k) { return f < (float)k.frame; });
            const BoneKf& a = *(seg - 1);
            const BoneKf& b = *seg;
            const float span = (float)(b.frame - a.frame);
            const float t = span > 0.0f ? ((frame - (float)a.frame) / span) : 0.0f;
            if (channel >= 0 && channel <= 2) {
                uint8_t c[4];
                GetBoneCurve(b.interp, channel, c);
                const float e = Bezier::FromBytes(c[0], c[1], c[2], c[3]).Evaluate(t);
                const float av = channel == 0 ? a.t.x : (channel == 1 ? a.t.y : a.t.z);
                const float bv = channel == 0 ? b.t.x : (channel == 1 ? b.t.y : b.t.z);
                return av + (bv - av) * e;
            }
            uint8_t c[4];
            GetBoneCurve(b.interp, 3, c);
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q, DirectX::XMQuaternionSlerp(DirectX::XMLoadFloat4(&a.r), DirectX::XMLoadFloat4(&b.r),
                                                                 Bezier::FromBytes(c[0], c[1], c[2], c[3]).Evaluate(t)));
            return DirectX::XMConvertToDegrees(2.0f * std::acos(std::min(1.0f, std::abs(q.w))));
        }
        const std::vector<CameraKf>& keys = d.SelectedCameraTrack().camera;
        if (keys.empty()) return 0.0f;
        const auto pick = [&](const CameraKf& k) -> float {
            if (channel == 0) return k.target.x;
            if (channel == 1) return k.target.y;
            if (channel == 2) return k.target.z;
            if (channel == 3) return DirectX::XMConvertToDegrees(std::sqrt(k.rotation.x * k.rotation.x +
                                                                         k.rotation.y * k.rotation.y +
                                                                         k.rotation.z * k.rotation.z));
            if (channel == 4) return k.distance;
            return (float)k.fovDeg;
        };
        if (frame <= (float)keys.front().frame) return pick(keys.front());
        if (frame >= (float)keys.back().frame || keys.size() == 1) return pick(keys.back());
        auto seg = std::upper_bound(keys.begin(), keys.end(), frame,
                                    [](float f, const CameraKf& k) { return f < (float)k.frame; });
        const CameraKf& a = *(seg - 1);
        const CameraKf& b = *seg;
        if (b.frame - a.frame <= 1) return pick(a);  // a one-frame gap is a cut: MMD holds the first key
        const int param = std::clamp(channel, 0, 5);
        const float t = (frame - (float)a.frame) / (float)(b.frame - a.frame);
        uint8_t c[4];
        GetCameraCurve(b.interp, param, c);
        const float e = Bezier::FromBytes(c[0], c[1], c[2], c[3]).Evaluate(t);
        const float av = pick(a), bv = pick(b);
        return av + (bv - av) * e;
    };

    // Audio-waveform cache, decoded off the UI thread: a track change launches a background decode, the frame only
    // polls it and draws no waveform until it is ready (a decode error leaves empty peaks and is not retried).
    {
        const double dur = d.hasAudio ? audio_.DurationSeconds() : 0.0;
        const std::string curPath = d.hasAudio ? PathToUtf8(d.audioPath) : std::string();

        // 1) pick up a finished decode (never .get() an unfinished future: that would block the frame)
        if (audioPeaksRunning_ && audioPeaksFuture_.valid() &&
            audioPeaksFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::vector<float> peaks = audioPeaksFuture_.get();
            audioPeaksRunning_ = false;
            const bool current = !curPath.empty() && audioPeaksPendingPath_ == curPath &&
                                 std::fabs(audioPeaksPendingDur_ - dur) <= 0.01;
            if (current) {
                audioPeaks_ = std::move(peaks);
                audioPeaksPath_ = audioPeaksPendingPath_;
                audioPeaksDur_ = audioPeaksPendingDur_;
                if (audioPeaks_.empty()) audioPeaksFailedPath_ = audioPeaksPath_;  // decode error: no retry loop
            }
        }

        // 2) forget a waveform that no longer belongs to the current track / its length
        if (curPath.empty() || dur <= 0.0 || audioPeaksPath_ != curPath || std::fabs(audioPeaksDur_ - dur) > 0.01) {
            audioPeaks_.clear();
            audioPeaksPath_.clear();
            audioPeaksDur_ = 0.0;
        }
        if (curPath.empty() || dur <= 0.0) audioPeaksFailedPath_.clear();

        // 3) launch a decode between decodes only: a running one finishes first, the new track follows
        if (!curPath.empty() && dur > 0.0 && audioPeaks_.empty() && !audioPeaksRunning_ &&
            audioPeaksFailedPath_ != curPath) {
            audioPeaksPendingPath_ = curPath;
            audioPeaksPendingDur_ = dur;
            const std::filesystem::path path = d.audioPath;
            audioPeaksFuture_ = std::async(std::launch::async, [path]() {
                std::vector<float> peaks;
                AudioPlayer::BuildWaveform(path, kStudioWaveRate, peaks);
                return peaks;
            });
            audioPeaksRunning_ = true;
        }
    }
    const std::string audioName = d.hasAudio ? PathToUtf8(d.audioPath.filename()) : std::string();
    d.view.audioLane = d.hasAudio && d.showWaveform && audio_.DurationSeconds() > 0.0;
    d.view.audioLabel = audioName.c_str();
    d.view.audioWave = audioPeaks_.size() >= 2 ? audioPeaks_.data() : nullptr;
    d.view.audioWaveBuckets = (int)(audioPeaks_.size() / 2);
    d.view.audioWaveRate = (float)kStudioWaveRate;
    d.view.audioDurationSeconds = (float)audio_.DurationSeconds();
    d.view.audioOffsetSeconds = (float)d.audioOffset;
    d.view.audioFps = (float)kMmdFps;
    d.view.audioLocked = d.audioLocked;

    TimelineEvents ev;
    Timeline("##tl", ImVec2(0, 0), d.rows, d.Frame(), d.EndFrame(), d.view, ev);
    ImGui::EndChild();
    StudioHandleTimeline(ev);
    d.view.audioLabel = nullptr;  // `audioName` ends here
    // the audio clip drag: the offset follows live (a playing track re-seeks so it stays under the playhead), the
    // release is one undo step
    if (ev.audioOffset) {
        d.audioOffset = ev.audioOffsetSeconds;
        d.audioEndFrame = (float)((audio_.DurationSeconds() + d.audioOffset) * kMmdFps);
    }
    if (ev.audioOffsetEnd) StudioCommitAudioOffset(ev.audioOffsetFrom, ev.audioOffsetSeconds);
    if (ev.audioMenu) ImGui::OpenPopup("##audiolanemenu");
    if (ev.audioLockToggle) StudioSetAudioLocked(!d.audioLocked);
    ImGui::SetNextWindowSize(ImVec2(Dp(260.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##audiolanemenu")) {
        if (d.hasAudio) DrawStudioAudioMenuItems();
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (ev.contextMenu) {
        studioKeyMenuFrame_ = ev.contextFrame;
        studioKeyMenuRow_ = ev.contextRow;
        studioKeyMenuRuler_ = ev.contextOnRuler;
        ImGui::OpenPopup("##keymenu");
    }
    DrawStudioKeyMenu();
}

// Timeline right click: the key actions that otherwise live in shortcuts only, at the clicked frame / row.
void App::DrawStudioKeyMenu() {
    using namespace ui;
    ImGui::SetNextWindowSize(ImVec2(Dp(248.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(6.0f), Dp(6.0f)));
    if (ImGui::BeginPopup("##keymenu")) {
        StudioDoc& d = *studio_;
        const int f = studioKeyMenuFrame_;
        const bool keys = !d.selection.empty();
        const bool clip = !d.clipboard.empty() && d.clipboardModel == d.selectedModel;
        const bool bones = clip && std::any_of(d.clipboard.begin(), d.clipboard.end(),
                                               [](const ClipboardKey& c) { return RowKindOf(c.row) == RowKind::Bone; });
        const auto seekHere = [&] {
            StudioSetPlaying(false);
            StudioSeek(f / (double)kMmdFps);
        };
        char title[48];
        std::snprintf(title, sizeof(title), Tr("프레임 %d"), f);
        Text(ImGui::GetWindowDrawList(), Font::Semibold, size::Caption,
             ImVec2(ImGui::GetCursorScreenPos().x + Dp(10.0f), ImGui::GetCursorScreenPos().y + Dp(4.0f)), P().ink3, title);
        ImGui::Dummy(ImVec2(1, Dp(24.0f)));
        bool close = false;
        // frame range (also Shift+drag on the ruler): start / end at the clicked frame, clear
        const auto rangeItems = [&] {
            TimelineView& v = d.view;
            if (MenuItem("##km_rangestart", Tr("여기부터 구간 시작"), icon::CaretLineRight, nullptr)) {
                v.rangeStart = f;
                if (v.rangeEnd < f) v.rangeEnd = std::max(f, d.EndFrame());
                close = true;
            }
            if (MenuItem("##km_rangeend", Tr("여기까지 구간 끝"), icon::CaretLineLeft, nullptr)) {
                v.rangeEnd = f;
                if (v.rangeStart < 0 || v.rangeStart > f) v.rangeStart = v.rangeStart < 0 ? 0 : f;
                close = true;
            }
            ImGui::BeginDisabled(!d.HasRange());
            if (MenuItem("##km_rangeclear", Tr("구간 해제"), icon::X, nullptr)) { v.rangeStart = v.rangeEnd = -1; close = true; }
            ImGui::EndDisabled();
        };
        if (studioKeyMenuRuler_) {  // the ruler: the range only
            rangeItems();
            if (close) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar();
            return;
        }
        if (studioKeyMenuRow_ && RowKindOf(studioKeyMenuRow_) != RowKind::Group &&
            MenuItem("##km_add", Tr("여기에 키 추가"), icon::Plus, Tr("더블클릭"))) {
            TimelineEvents add;
            add.addKeyAt = true;
            add.addKeyRow = studioKeyMenuRow_;
            add.addKeyFrame = f;
            StudioHandleTimeline(add);
            close = true;
        }
        ImGui::BeginDisabled(!keys);
        if (MenuItem("##km_copy", Tr("복사"), icon::Copy, "Ctrl+C")) { StudioCopySelected(); close = true; }
        if (MenuItem("##km_cut", Tr("잘라내기"), icon::Scissors, "Ctrl+X")) {
            StudioCopySelected();
            StudioDeleteSelected(Tr("키 잘라내기"));
            close = true;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!clip);
        if (MenuItem("##km_paste", Tr("여기에 붙여넣기"), icon::ClipboardText, "Ctrl+V")) { seekHere(); StudioPaste(false); close = true; }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!bones);
        if (MenuItem("##km_mirror", Tr("좌우 반전 붙여넣기"), icon::FlipHorizontal, nullptr)) { seekHere(); StudioPaste(false, true); close = true; }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!clip);
        if (MenuItem("##km_curve", Tr("곡선만 붙여넣기"), icon::Bezier, "Ctrl+Shift+V")) { seekHere(); StudioPaste(true); close = true; }
        ImGui::EndDisabled();
        bool hasEffectKeys = false;
        bool allStepped = true;
        for (const KeyId& k : d.selection) {
            const RowKind rk = RowKindOf(k.first);
            if (rk != RowKind::EffectParam && rk != RowKind::WorldParam && rk != RowKind::StageParam) continue;
            hasEffectKeys = true;
            uint32_t uid = RowGroupOf(k.first);
            uint32_t pidx = RowIndexOf(k.first);
            std::string param;
            const EffectParamTrack* tr = nullptr;
            if (rk == RowKind::StageParam) {
                const StudioStage* stage = d.FindStage(uid);
                if (!stage) continue;
                param = StageParamNameOfIndex(*stage, pidx);
                tr = FindStageParamTrack(*stage, param);
            } else {
                const StudioEffect* eff = rk == RowKind::WorldParam
                    ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr) : d.FindEffect(uid);
                if (!eff) continue;
                param = EffectParamNameOfIndex(*eff, pidx);
                for (const auto& t : eff->tracks)
                    if (t.param == param) { tr = &t; break; }
            }
            if (!tr) continue;
            for (const auto& pk : tr->keys) {
                if (pk.frame == k.second) {
                    if (!pk.stepped) allStepped = false;
                    break;
                }
            }
        }
        if (hasEffectKeys) {
            const char* interpLabel = allStepped ? Tr("선형 보간으로 변경") : Tr("계단식 보간으로 변경");
            if (MenuItem("##km_stepped", interpLabel, allStepped ? icon::Bezier : icon::Pause, nullptr)) {
                std::vector<TrackState> before = StudioCaptureSelectedTracks();
                const bool newStepped = !allStepped;
                for (const KeyId& k : d.selection) {
                    const RowKind rk = RowKindOf(k.first);
                    if (rk != RowKind::EffectParam && rk != RowKind::WorldParam && rk != RowKind::StageParam)
                        continue;
                    uint32_t uid = RowGroupOf(k.first);
                    uint32_t pidx = RowIndexOf(k.first);
                    EffectParamTrack* tr = nullptr;
                    if (rk == RowKind::StageParam) {
                        StudioStage* stage = d.FindStage(uid);
                        if (!stage) continue;
                        tr = FindStageParamTrack(*stage, StageParamNameOfIndex(*stage, pidx));
                    } else {
                        StudioEffect* eff = rk == RowKind::WorldParam
                            ? (d.FindWorld(uid) ? &d.FindWorld(uid)->fx : nullptr) : d.FindEffect(uid);
                        if (!eff) continue;
                        for (auto& t : eff->tracks)
                            if (t.param == EffectParamNameOfIndex(*eff, pidx)) { tr = &t; break; }
                    }
                    if (!tr) continue;
                    for (auto& pk : tr->keys) {
                        if (pk.frame == k.second) {
                            pk.stepped = newStepped;
                            break;
                        }
                    }
                }
                StudioPushTrackEdit(Tr("보간 변경"), before);
                d.rowsKey = ~0ull;
                close = true;
            }
        }
        ImGui::BeginDisabled(!keys);
        if (MenuItem("##km_del", Tr("삭제"), icon::Trash, "Del")) { StudioDeleteSelected(Tr("키 삭제")); close = true; }
        ImGui::EndDisabled();
        {   // separator before the range items
            ImGui::Dummy(ImVec2(1, Dp(3.0f)));
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(c, ImVec2(c.x + ImGui::GetContentRegionAvail().x, c.y), P().line);
            ImGui::Dummy(ImVec2(1, Dp(4.0f)));
        }
        rangeItems();
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Viewport navigation (Unity / Unreal style): RMB look + WASDQE fly (wheel = speed), Alt+LMB or LMB orbit, MMB pan,
// Alt+RMB dolly, wheel zoom. The fly keys only count while the right button is held on the viewport.
// Possessing the camera (C4D style) sends the same gestures to the motion camera's key at the playhead instead of the
// free camera: orbit / pan / fly move the real camera, one undo step per gesture.
void App::StudioViewportNavigate(bool hovered, bool active) {
    StudioDoc& d = *studio_;
    ImGuiIO& io = ImGui::GetIO();
    const bool possessed = d.possessCamera;
    const AppSettings& st = settings_;
    const float rotX = (st.cameraInvertX ? -1.0f : 1.0f) * st.cameraRotateSpeed;
    const float rotY = (st.cameraInvertY ? -1.0f : 1.0f) * st.cameraRotateSpeed;
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
            cam.yaw += delta.x * 0.006f * rotX;
            cam.pitch = std::clamp(cam.pitch + delta.y * 0.006f * rotY, -1.45f, 1.45f);
        } else if (rmb && io.KeyAlt) {  // dolly
            if (possessed) {
                const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
                const float step = reach * (delta.x - delta.y) * 0.006f * st.cameraZoomSpeed;
                cam.target.x += -sy * cp * step;
                cam.target.y += -sp * step;
                cam.target.z += cy * cp * step;
            } else {
                cam.distance = std::clamp(cam.distance * std::exp((delta.x - delta.y) * 0.006f * st.cameraZoomSpeed), minDist, 600.0f);
            }
        } else if (rmb) {  // look around the eye: the target follows
            const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
            const DirectX::XMFLOAT3 eye{cam.target.x + sy * cp * cam.distance, cam.target.y + sp * cam.distance,
                                        cam.target.z - cy * cp * cam.distance};
            cam.yaw -= delta.x * 0.004f * rotX;
            cam.pitch = std::clamp(cam.pitch + delta.y * 0.004f * rotY, -1.45f, 1.45f);
            const float sy2 = std::sin(cam.yaw), cy2 = std::cos(cam.yaw), sp2 = std::sin(cam.pitch), cp2 = std::cos(cam.pitch);
            cam.target = {eye.x - sy2 * cp2 * cam.distance, eye.y - sp2 * cam.distance, eye.z + cy2 * cp2 * cam.distance};
        } else {  // pan
            const float rx = std::cos(cam.yaw), rz = std::sin(cam.yaw);
            const float scale = reach * 0.0015f * st.cameraPanSpeed;
            cam.target.x += (-delta.x * rx) * scale;
            cam.target.y += delta.y * scale;
            cam.target.z += (-delta.x * rz) * scale;
        }
    }
    if (flying) {
        const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
        const float fx = -sy * cp, fy = -sp, fz = cy * cp;  // eye -> target
        const float speed = 30.0f * st.cameraFlySpeed * studioFlyMul_ * (io.KeyShift ? 3.0f : 1.0f) * io.DeltaTime;
        cam.target.x += (fx * fwdIn + cy * rightIn) * speed;
        cam.target.y += (fy * fwdIn + upIn) * speed;
        cam.target.z += (fz * fwdIn + sy * rightIn) * speed;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        if (rmb) studioFlyMul_ = std::clamp(studioFlyMul_ * std::pow(1.2f, io.MouseWheel), 0.1f, 20.0f);  // fly speed
        else if (!io.KeyCtrl) {  // the lens: field of view (a possessed camera keys it like any other camera edit)
            cam.fovDeg = std::clamp(cam.fovDeg * std::pow(0.92f, io.MouseWheel * st.cameraFovSpeed), possessed ? 1.0f : 5.0f, possessed ? 125.0f : 120.0f);
            if (!possessed) cam.fovDeg = std::round(cam.fovDeg * 10.0f) * 0.1f;
        } else if (possessed) {  // dolly the camera itself: eye and target move together, the lens distance stays
            const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw), sp = std::sin(cam.pitch), cp = std::cos(cam.pitch);
            const float step = reach * 0.12f * io.MouseWheel * st.cameraZoomSpeed;
            cam.target.x += -sy * cp * step;
            cam.target.y += -sp * step;
            cam.target.z += cy * cp * step;
        } else cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel * st.cameraZoomSpeed), minDist, 600.0f);
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
    if (hovered && io.MouseWheel != 0.0f)
        d.quadHeight[v] = std::clamp(d.quadHeight[v] * std::pow(0.88f, io.MouseWheel * settings_.cameraZoomSpeed), 2.0f, 4000.0f);
}

void App::DrawStudioWindOverlay(const float rr[4]) {
    StudioDoc& d = *studio_;
    (void)rr;
    if (!d.showWindVector) return;
    if (!d.physicsEnv.HasWind()) return;
    float nx = d.physicsEnv.windDir.x, nz = d.physicsEnv.windDir.z;
    const float len = std::sqrt(nx * nx + nz * nz);
    if (len < 1e-5f) return;
    nx /= len; nz /= len;

    // 3D arrow in the scene: from an anchor on the floor along the (horizontal) wind direction, projected to screen.
    // The shaft ends at `tail`; the arrowhead occupies the last `headLen` world units so the line never pokes past it.
    const DirectX::XMFLOAT3 anchor(0, 1.5f, 0);
    const float L = 8.0f, headLen = 1.2f;
    const DirectX::XMFLOAT3 tailW(anchor.x + nx * L, anchor.y, anchor.z + nz * L);
    const DirectX::XMFLOAT3 tipW(anchor.x + nx * (L + headLen), anchor.y, anchor.z + nz * (L + headLen));
    const DirectX::XMFLOAT3 aX(anchor.x + 8.0f, anchor.y, anchor.z), aZ(anchor.x, anchor.y, anchor.z + 8.0f);

    ImVec2 a, tail, tip, ax, az;
    if (!studioVp_.Project(anchor, a) || !studioVp_.Project(tailW, tail) || !studioVp_.Project(tipW, tip)) return;
    const bool canX = studioVp_.Project(aX, ax);
    const bool canZ = studioVp_.Project(aZ, az);

    const ui::Palette& p = ui::P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ui::Dpi();

    // faint floor guide: X (red) and Z (blue) ground lines from the anchor
    if (canX) dl->AddLine(a, ax, ui::WithAlpha(IM_COL32(235, 75, 75, 255), 0.35f), 1.0f * dpi);
    if (canZ) dl->AddLine(a, az, ui::WithAlpha(IM_COL32(70, 130, 245, 255), 0.35f), 1.0f * dpi);

    // the wind shaft (outline + fill) ends at `tail`, under the arrowhead
    dl->AddLine(a, tail, ui::WithAlpha(IM_COL32(10, 14, 20, 255), 0.85f), 5.0f * dpi);
    dl->AddLine(a, tail, IM_COL32(140, 230, 150, 235), 2.4f * dpi);

    // arrowhead spans `tail` -> `tip` (the shaft never exceeds it)
    ImVec2 ds(tip.x - tail.x, tip.y - tail.y);
    const float dl2 = std::sqrt(ds.x * ds.x + ds.y * ds.y);
    if (dl2 > 1e-3f) {
        ds.x /= dl2; ds.y /= dl2;
        const float hw = 4.5f * dpi;
        const ImVec2 perp(-ds.y, ds.x);
        dl->AddTriangleFilled(tip, ImVec2(tail.x + perp.x * hw, tail.y + perp.y * hw),
                              ImVec2(tail.x - perp.x * hw, tail.y - perp.y * hw), IM_COL32(140, 230, 150, 235));
        dl->AddTriangle(ImVec2(tail.x + perp.x * hw, tail.y + perp.y * hw),
                        ImVec2(tail.x - perp.x * hw, tail.y - perp.y * hw), tip, ui::WithAlpha(IM_COL32(10, 14, 20, 255), 0.6f));
    }
    // anchor dot
    dl->AddCircleFilled(a, 3.0f * dpi, IM_COL32(120, 130, 140, 255), 12);
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
    if (ImGui::IsWindowFocused())
        studioFocusPanel_ = 0;
    else if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) &&
             ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1)))
        studioFocusPanel_ = 0;  // a click in the viewport focuses it
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
    StudioViewportLightHandles(hovered || active);
    {
        // the camera path / frustum in the perspective view, the frame guides when there is one view
        const ViewProj saved = studioVp_;
        studioVp_ = views[0].vp;
        DrawStudioCameraPath(views[0].x0, views[0].y0, views[0].x1, views[0].y1);
        DrawStudioCameraGizmos(views[0].x0, views[0].y0, views[0].x1, views[0].y1);   // S4: the other cameras
        studioVp_ = saved;
    }
    if (!quad) DrawStudioFrameMask(x0, y0, x1, y1);
    if (active && (std::fabs(io.MousePos.x - studioPressPos_.x) > Dp(3.0f) || std::fabs(io.MousePos.y - studioPressPos_.y) > Dp(3.0f)))
        studioPressMoved_ = true;
    if (deactivated) {
        // a click (no drag) on empty space clears the bone selection
        if (studioViewDrag_ == 1 && !studioPressMoved_ && io.MouseReleased[0] && StudioPoseModel() && !d.selectedBones.empty())
            StudioSelectBone(-1, false);
        if (studioViewDrag_ == 8) {
            StudioEndLightEdit(studioLightEditingUid_);
        }
        studioViewDrag_ = 0;
    }

    if (av == 0) {
        StudioViewportNavigate(hovered, active);
        DrawStudioViewportAxisControls(rr[0], rr[1], rr[0] + rr[2], rr[1] + rr[3]);
        DrawStudioWindOverlay(rr);
    } else StudioOrthoNavigate(act.vp, hovered, active);
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
    std::string possessLabel;   // possessing a stage-screen camera: say which
    if (d.possessCamera && d.selectedCameraUid != 0)
        possessLabel = d.CameraLabel(d.selectedCameraUid) + " " + Tr("빙의 중");
    const char* label = !possessLabel.empty() ? possessLabel.c_str()
                      : d.possessCamera ? Tr("카메라 빙의 중")
                      : d.useMotionCamera && d.cameraEval ? Tr("카메라 모션")
                                                                          : Tr("자유 카메라");
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
        // the renderer of the lit single view: a pill left of the layout switch, opening the four choices
        const bool rtOk = renderer_.RayTracingSupported(), giOk = renderer_.OfflineSupported();
        int cur = std::clamp(settings_.studioRenderer, 0, 3);
        if ((cur == 1 || cur == 2) && !rtOk) cur = 0;   // an unsupported saved choice runs as raster
        if (cur == 3 && !giOk) cur = 0;
        const char* names[4] = {Tr("래스터"), Tr("레이 트레이싱"), Tr("패스 트레이싱"), Tr("GI 미리보기")};
        const ImVec2 ts = TextSize(Font::Semibold, size::Small, names[cur]);
        const float rw = ts.x + Dp(40.0f), rh = Dp(30.0f);
        const ImVec2 ra(x1 - Dp(segW) - Dp(12.0f) - Dp(layW) - Dp(16.0f) - rw, y0 + Dp(8.0f)), rb(ra.x + rw, ra.y + rh);
        ImGui::SetCursorScreenPos(ra);
        const bool rClick = ImGui::InvisibleButton("##vprenderer", ImVec2(rw, rh));
        uiprobe::Info(ImGui::GetItemID(), "##vprenderer", names[cur]);
        const bool rHover = ImGui::IsItemHovered();
        dl->AddRectFilled(ra, rb, rHover ? p.surface : p.sunken, rh * 0.5f);
        const bool liveView = (ViewShading)std::clamp(d.shading, 0, 2) == ViewShading::Lit && d.viewLayout == 0;
        Text(dl, Font::Semibold, size::Small, ImVec2(ra.x + Dp(14.0f), ra.y + (rh - ts.y) * 0.5f),
             liveView ? p.ink : p.ink3, names[cur]);
        Icon(dl, icon::CaretDown, 11.0f, ImVec2(rb.x - Dp(14.0f), ra.y + rh * 0.5f), p.ink2);
        Tooltip(liveView ? Tr("뷰포트 렌더러") : Tr("뷰포트 렌더러 (솔리드 단일 뷰에서만 적용)"));
        if (rClick) ImGui::OpenPopup("##vprendererpop");
        ImGui::SetNextWindowPos(ImVec2(ra.x, rb.y + Dp(4.0f)));
        ImGui::SetNextWindowSize(ImVec2(Dp(220.0f), 0));
        if (ImGui::BeginPopup("##vprendererpop")) {
            static const char* const kIds[4] = {"##vpr0", "##vpr1", "##vpr2", "##vpr3"};
            for (int i = 0; i < 4; ++i) {
                const bool off = (i == 1 || i == 2) ? !rtOk : (i == 3 ? !giOk : false);
                ImGui::BeginDisabled(off);
                if (MenuItem(kIds[i], names[i], i == cur ? icon::Check : nullptr)) {
                    StudioSetViewportRenderer(i);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (off)
                    Tooltip(i == 3 ? Tr("이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다")
                                   : Tr("이 그래픽 카드에서는 레이 트레이싱을 사용할 수 없습니다"));
            }
            ImGui::EndPopup();
        }
    }
    if (StudioGiActive()) {
        // GI preview progress, bottom-left of the viewport
        const OfflineProgress& gi = renderer_.OfflineStatus();
        std::string text;
        if (gi.phase == OfflinePhase::Prepass) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), Tr("GI · 프리패스 %u/%u"), std::min(gi.prepassStep + 1, gi.prepassSteps), gi.prepassSteps);
            text = buf;
        } else if (gi.phase == OfflinePhase::Render) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "GI · %u spp", gi.samples);
            text = buf;
        } else if (gi.phase == OfflinePhase::Done) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "GI · %u spp · %s", gi.samples, Tr("완료"));
            text = buf;
        }
        if (!text.empty()) {
            ImVec2 gb;
            Badge(dl, ImVec2(x0 + Dp(12.0f), y1 - Dp(34.0f)), text.c_str(), p.accentSoft, p.accentInk, &gb);
        }
    }
    if (d.models.empty() && studioJobs_.empty()) {
        const char* t1 = Tr("빈 프로젝트");
        const char* t2 = Tr("추가 › 장면으로 캐릭터와 스테이지를 넣으세요");
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
