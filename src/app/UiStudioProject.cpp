// Studio projects (.mmdxproj save / open / recent / autosave + recovery), adding and removing models inside the
// studio (library or file, loaded on worker threads), props (accessories following a bone of another model) and audio.
#include "app/App.h"

#include <algorithm>
#include <chrono>
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

namespace {

constexpr double kAutosaveSeconds = 60.0;

// Undoable prop placement edit. Refers to the prop by uid: removing another model shifts indices.
class PropAttachCommand : public Command {
public:
    // place: the character's world placement (StudioModel::place) instead of the prop's attach
    PropAttachCommand(StudioDoc& doc, uint32_t uid, PropAttach before, PropAttach after, bool place = false)
        : doc_(doc), uid_(uid), before_(std::move(before)), after_(std::move(after)), place_(place) {}
    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }
    std::string Name() const override { return place_ ? Tr("모델 배치") : Tr("소품 배치"); }

private:
    void Apply(const PropAttach& a) {
        const int i = doc_.IndexOfUid(uid_);
        if (i >= 0) (place_ ? doc_.models[(size_t)i]->place : doc_.models[(size_t)i]->attach) = a;
    }
    StudioDoc& doc_;
    uint32_t uid_;
    PropAttach before_, after_;
    bool place_;
};

std::string StemUtf8(const std::filesystem::path& p) { return PathToUtf8(p.stem()); }

const std::vector<FileFilter>& ModelFilters() {
    static const std::vector<FileFilter> f = {
        {L"3D", L"*.pmx;*.pmd;*.x;*.glb;*.gltf;*.vrm;*.fbx;*.obj"}, {L"PMX / PMD", L"*.pmx;*.pmd"}, {L"X", L"*.x"},
        {L"glTF / VRM", L"*.glb;*.gltf;*.vrm"},
        {L"FBX / OBJ", L"*.fbx;*.obj"}};
    return f;
}
const std::vector<FileFilter>& ProjectFilters() {
    static const std::vector<FileFilter> f = {{L"MMDX12 Studio", L"*.mmdxproj"}};
    return f;
}

bool MenuItemChip(const char* id, const char* label, const char* glyph, const char* hint = nullptr) {
    return ui::MenuItem(id, label, glyph, hint);
}

void MenuSeparator() {
    using namespace ui;
    ui::Gap(4.0f);
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(c, ImVec2(c.x + ImGui::GetContentRegionAvail().x, c.y), P().line);
    ui::Gap(6.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

void App::StartStudioEmpty() {
    StopVideoProbe(false);
    UnloadScene();
    studioPackage_ = std::make_unique<StudioPackage>();
    FinishStudioLoad();  // no models: the camera / light tracks, a floor and the default camera
    studioPackage_.reset();
    LOG_INFO("studio: new empty project");
}

void App::StartStudioProjectLoad(const std::filesystem::path& file, bool recovery) {
    StopVideoProbe(false);
    UnloadScene();
    screen_ = Screen::Loading;
    loadTarget_ = LoadTarget::Studio;
    loadError_.clear();
    loadProgress_.fraction.store(0.0f, std::memory_order_relaxed);
    loadProgress_.SetStatus("");
    studioLoadTitle_ = recovery ? std::string(Tr("자동 저장에서 복구")) : StemUtf8(file);
    studioPackage_ = std::make_unique<StudioPackage>();
    loadFuture_ = std::async(std::launch::async, [=, pkg = studioPackage_.get(), this] {
        return LoadStudioProjectPackage(file, recovery, *pkg, &loadProgress_, &loadError_);
    });
}

void App::StudioRequest(StudioAction a, const std::filesystem::path& file) {
    if (a == StudioAction::Open && file.empty()) {
        // pick the file first; the unsaved-changes prompt comes after (cancelling the dialog changes nothing)
        const std::filesystem::path picked = OpenFileDialog(hwnd_, ProjectFilters(),
                                                            studio_ && !studio_->projectPath.empty()
                                                                ? studio_->projectPath.parent_path()
                                                                : std::filesystem::path());
        if (picked.empty()) return;
        StudioRequest(StudioAction::OpenFile, picked);
        return;
    }
    if (studio_ && studio_->Dirty()) {
        studioPending_ = a;
        studioPendingFile_ = file;
        studioLeaveConfirm_ = true;
        return;
    }
    StudioRunAction(a, file);
}

void App::StudioRunAction(StudioAction a, const std::filesystem::path& file) {
    studioLeaveConfirm_ = false;
    studioPending_ = StudioAction::None;
    if (a == StudioAction::None) return;
    if (studio_) LeaveStudio();  // waits for loads and the GPU, deletes the recovery file
    if (a == StudioAction::New) StartStudioEmpty();
    else if (a == StudioAction::OpenFile && !file.empty()) StartStudioProjectLoad(file, false);
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

ProjectData App::StudioProjectData() const {
    const StudioDoc& d = *studio_;
    ProjectData pd;
    pd.models.reserve(d.models.size());
    for (const auto& mp : d.models) {
        const StudioModel& m = *mp;
        ProjectModel pm;
        pm.name = m.name;
        pm.kind = m.kind;
        pm.path = m.path;
        pm.libraryId = m.libraryId;
        pm.visible = m.visible;
        pm.place = m.place;
        pm.shader = m.shader;
        if (m.IsProp()) {
            pm.attach = m.attach;
            pm.attach.parent = m.attach.parent >= 0 ? d.IndexOfUid((uint32_t)m.attach.parent) : -1;  // uid -> index
        }
        pm.motion = m.motion;
        pm.motion.modelName = m.pmx->name;  // MMD checks it against the model the motion is loaded onto
        pd.models.push_back(std::move(pm));
    }
    pd.camera.camera = d.camera.camera;
    pd.camera.light = d.camera.light;
    pd.camera.shadow = d.camera.shadow;
    if (d.hasAudio) pd.audioPath = d.audioPath;
    pd.audioOffset = d.audioOffset;
    ProjectEditor& e = pd.editor;
    e.frame = d.Frame();
    e.selectedModel = d.selectedModel;
    e.useMotionCamera = d.useMotionCamera;
    e.useShadowTrack = d.useShadowTrack;
    e.lights = d.lights;
    for (SceneLight& l : e.lights) {  // model uids do not survive a reload: spot targets are saved as 1 + the model's index
        if (l.targetUid == 0) continue;
        const int i = d.IndexOfUid(l.targetUid);
        l.targetUid = i >= 0 ? (uint32_t)i + 1 : 0;
    }
    e.showCameraPath = d.showCameraPath;
    e.loop = d.loop;
    e.physics = d.physics;
    e.rangeStart = d.view.rangeStart;
    e.rangeEnd = d.view.rangeEnd;
    e.pxPerFrame = d.view.pxPerFrame;
    e.camTarget = freeCam_.target;
    e.camYaw = freeCam_.yaw;
    e.camPitch = freeCam_.pitch;
    e.camDistance = freeCam_.distance;
    e.camFovDeg = freeCam_.fovDeg;
    return pd;
}

bool App::StudioSave(bool saveAs) {
    if (!studio_) return false;
    StudioDoc& d = *studio_;
    if (!saveAs && !d.projectPath.empty()) return StudioSaveTo(d.projectPath);
    std::wstring suggested = d.projectPath.empty() ? std::wstring(L"project") : d.projectPath.stem().wstring();
    const std::filesystem::path picked = SaveFileDialog(hwnd_, ProjectFilters(), suggested + kProjectExtension, L"mmdxproj",
                                                        d.projectPath.empty() ? std::filesystem::path()
                                                                              : d.projectPath.parent_path());
    if (picked.empty()) return false;
    return StudioSaveTo(picked);
}

bool App::StudioSaveTo(const std::filesystem::path& file) {
    StudioDoc& d = *studio_;
    std::filesystem::path target = std::filesystem::absolute(file);
    if (target.extension().empty()) target += kProjectExtension;
    const auto t0 = std::chrono::steady_clock::now();
    const ProjectData data = StudioProjectData();
    std::string err;
    if (!SaveProject(target, data, &err)) {
        toast_ = {Tr("프로젝트를 저장하지 못했습니다"), err, {}, true, timeSeconds_ + 6.0};
        LOG_ERROR("studio: save failed: %s", err.c_str());
        return false;
    }
    LOG_INFO("studio: saved %s (%.0f ms)", PathToUtf8(target).c_str(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    d.projectPath = target;
    d.MarkSaved();
    d.autosavedStamp = d.ChangeStamp();
    StudioDiscardRecovery();  // the work is safe on disk now
    settings_.AddRecentProject(PathToUtf8(target));
    settings_.Save(settingsPath_);
    toast_ = {Tr("프로젝트를 저장했어요"), PathToUtf8(target.filename()), target, false, timeSeconds_ + 4.0};
    return true;
}

// ---------------------------------------------------------------------------
// Autosave / recovery
// ---------------------------------------------------------------------------

std::filesystem::path App::StudioRecoveryFile() const {
    return ExecutableDir() / L"recovery" / L"autosave.mmdxproj";
}

void App::StudioAutosave(bool force, bool wait) {
    if (studioAutosave_.valid() && studioAutosave_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        studioAutosave_.get();
    if (!studio_) return;
    StudioDoc& d = *studio_;
    if (!force) {
        if (timeSeconds_ - studioAutosaveAt_ < kAutosaveSeconds) return;
        studioAutosaveAt_ = timeSeconds_;
        if (!d.Dirty() || d.ChangeStamp() == d.autosavedStamp) return;
    }
    if (studioAutosave_.valid()) {
        if (!wait) return;  // the previous one is still writing: try again at the next tick
        studioAutosave_.get();
    }
    // The snapshot (a copy of the motions) is taken here; serialising and writing happen on a worker thread so a
    // long dance does not stall the UI.
    ProjectData data = StudioProjectData();
    data.recoveryOf = d.projectPath;
    d.autosavedStamp = d.ChangeStamp();
    const std::filesystem::path file = StudioRecoveryFile();
    studioAutosave_ = std::async(std::launch::async, [data = std::move(data), file]() {
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        const bool ok = SaveProject(file, data, &err);
        if (ok)
            LOG_INFO("studio: autosaved (%.0f ms)",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        else
            LOG_WARN("studio: autosave failed: %s", err.c_str());
        return ok;
    });
    if (wait) studioAutosave_.get();
}

void App::StudioDiscardRecovery() {
    if (studioAutosave_.valid()) studioAutosave_.get();
    std::error_code ec;
    const std::filesystem::path dir = StudioRecoveryFile().parent_path();
    if (std::filesystem::exists(dir, ec)) {
        std::filesystem::remove_all(dir, ec);
        if (!ec) LOG_INFO("studio: recovery files removed");
    }
}

void App::DrawRecoveryPrompt() {
    using namespace ui;
    if (!recoveryChecked_) {
        recoveryChecked_ = true;
        std::error_code ec;
        // automated runs that open another screen are not interrupted by a leftover autosave
        recoveryPrompt_ = std::filesystem::exists(StudioRecoveryFile(), ec) &&
                          (options_.startScreen.empty() || options_.startScreen == "select") &&
                          !options_.autoplay && options_.benchmarkCategory.empty();
    }
    if (!recoveryPrompt_) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::OpenPopup("##recovery");
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(440.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(P().surface));
    if (ImGui::BeginPopupModal("##recovery", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const Palette& p = P();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Icon(dl, icon::ClockCounterClockwise, 22.0f, ImVec2(c.x + Dp(11.0f), c.y + Dp(12.0f)), p.accentInk);
        Text(dl, Font::Semibold, size::Title, ImVec2(c.x + Dp(32.0f), c.y), p.ink, Tr("복구할 스튜디오 작업이 있어요"));
        ImGui::Dummy(ImVec2(0, Dp(34.0f)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Dp(392.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped("%s", Tr("지난번에 저장하지 않고 끝난 작업이 자동 저장되어 있어요. 복구하면 스튜디오에서 이어서 편집할 수 있어요."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        {
            std::error_code ec;
            const auto when = std::filesystem::last_write_time(StudioRecoveryFile(), ec);
            if (!ec) {
                const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(when);
                const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
                std::tm lt{};
                localtime_s(&lt, &tt);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
                Gap(6.0f);
                PushFont(Font::Regular, size::Caption);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
                ImGui::Text("%s %s", Tr("자동 저장:"), buf);
                ImGui::PopStyleColor();
                PopFont();
            }
        }
        Gap(16.0f);
        bool recover = false;
        if (Button("##recdiscard", Tr("버리기"), icon::Trash, ButtonKind::Ghost, ImVec2(190.0f, 40.0f))) {
            StudioDiscardRecovery();
            recoveryPrompt_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##recover", Tr("복구"), icon::ClockCounterClockwise, ButtonKind::Primary, ImVec2(190.0f, 40.0f))) {
            recover = true;
            recoveryPrompt_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (recover) StartStudioProjectLoad(StudioRecoveryFile(), true);
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Adding models / songs / audio
// ---------------------------------------------------------------------------

void App::StudioAddModelFile(ModelKind kind, const std::filesystem::path& file) {
    auto job = std::make_unique<StudioJob>();
    job->label = StemUtf8(file);
    job->models.resize(1);
    StudioJob* j = job.get();
    const std::string label = job->label;
    job->future = std::async(std::launch::async, [j, kind, file, label] {
        return LoadStudioModel(file, kind, label, j->models[0], &j->progress, &j->error);
    });
    studioJobs_.push_back(std::move(job));
}

void App::StudioAddModelDialog(ModelKind kind) {
    const std::filesystem::path f = OpenFileDialog(hwnd_, ModelFilters());
    if (!f.empty()) StudioAddModelFile(kind, f);
}

void App::StudioAddLibraryCharacter(int index) {
    if (index < 0 || index >= (int)library_.characters.size()) return;
    const CharacterAsset c = library_.characters[(size_t)index];
    auto job = std::make_unique<StudioJob>();
    job->label = c.displayName;
    job->models.resize(1);
    StudioJob* j = job.get();
    job->future = std::async(std::launch::async, [j, c] {
        const bool ok = LoadStudioModel(c.modelPath, ModelKind::Character, c.displayName, j->models[0], &j->progress, &j->error);
        j->models[0].libraryId = c.id;
        return ok;
    });
    studioJobs_.push_back(std::move(job));
}

void App::StudioAddLibraryStage(int index) {
    if (index < 0 || index >= (int)library_.stages.size()) return;
    const StageAsset s = library_.stages[(size_t)index];
    auto job = std::make_unique<StudioJob>();
    job->label = s.displayName;
    StudioJob* j = job.get();
    job->future = std::async(std::launch::async, [j, s] { return LoadStudioStage(s, j->models, &j->progress, &j->error); });
    studioJobs_.push_back(std::move(job));
}

void App::StudioApplyLibrarySong(int index) {
    if (index < 0 || index >= (int)library_.songs.size() || !studio_) return;
    const SongAsset s = library_.songs[(size_t)index];
    auto job = std::make_unique<StudioJob>();
    job->label = s.displayName;
    job->song = true;
    job->audio = s.audioPath;
    const StudioModel* m = studio_->Selected();
    job->targetUid = m && m->kind == ModelKind::Character ? m->uid : 0;
    StudioJob* j = job.get();
    job->future = std::async(std::launch::async, [this, j, s] {
        const bool ok = LoadStudioSong(s, j->dance, j->camera, &j->error);
        // decode the audio here too: the main thread's Load then takes no time (no hitch when the song arrives)
        if (ok && !j->audio.empty()) j->audioPreloaded = audio_.Preload(j->audio);
        return ok;
    });
    studioJobs_.push_back(std::move(job));
}

bool App::StudioSetAudio(const std::filesystem::path& file) {
    StudioDoc& d = *studio_;
    if (file.empty()) {
        audio_.Unload();
        d.hasAudio = false;
        d.audioPath.clear();
        d.audioEndFrame = 0;
        ++d.projectVersion;
        return true;
    }
    if (!audio_.Load(file)) {
        toast_ = {Tr("음원을 열 수 없습니다"), PathToUtf8(file.filename()), {}, true, timeSeconds_ + 5.0};
        // Load dropped the previous audio: bring it back
        if (d.hasAudio && !audio_.Load(d.audioPath)) d.hasAudio = false;
        if (d.hasAudio) StudioSeekAudio();
        return false;
    }
    d.hasAudio = true;
    d.audioPath = file;
    d.audioEndFrame = (float)((audio_.DurationSeconds() + d.audioOffset) * kMmdFps);
    audio_.SetMuted(false);
    StudioSeekAudio();
    ++d.projectVersion;
    return true;
}

void App::StudioAudioDialog() {
    const std::filesystem::path f =
        OpenFileDialog(hwnd_, {{L"Audio", L"*.wav;*.mp3;*.ogg;*.flac"}, {L"WAV", L"*.wav"}, {L"MP3", L"*.mp3"}});
    if (f.empty()) return;
    // decoded on a worker (a long song takes a while), applied by StudioPollJobs
    auto job = std::make_unique<StudioJob>();
    job->label = PathToUtf8(f.filename());
    job->song = true;
    job->audioOnly = true;
    job->audio = f;
    StudioJob* j = job.get();
    job->future = std::async(std::launch::async, [this, j] {
        j->audioPreloaded = audio_.Preload(j->audio);
        return true;   // a file that cannot be decoded fails in StudioSetAudio (with its toast)
    });
    studioJobs_.push_back(std::move(job));
}

void App::StudioPollJobs() {
    if (!studio_) return;
    StudioDoc& d = *studio_;
    for (size_t ji = 0; ji < studioJobs_.size();) {
        StudioJob& job = *studioJobs_[ji];
        if (job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++ji;
            continue;
        }
        const bool ok = job.future.get();
        if (!ok) {
            toast_ = {Tr("불러오지 못했습니다"), job.label + (job.error.empty() ? "" : ": " + job.error), {}, true,
                      timeSeconds_ + 6.0};
            LOG_ERROR("studio: load failed: %s: %s", job.label.c_str(), job.error.c_str());
        } else if (job.audioOnly) {
            if (StudioSetAudio(job.audio)) toast_ = {Tr("음원을 넣었어요"), job.label, {}, false, timeSeconds_ + 4.0};
        } else if (job.song) {
            // a library song: dance onto the character it was started for, camera tracks, audio
            std::vector<std::unique_ptr<Command>> parts;
            const int target = job.targetUid ? d.IndexOfUid(job.targetUid) : -1;
            if (target >= 0 && !job.dance.Empty()) {
                StudioModel& m = *d.models[(size_t)target];
                MotionData dance = std::move(job.dance);
                dance.CanonicalizeNames(*m.pmx);
                dance.modelName = m.pmx->name;
                parts.push_back(std::make_unique<MotionSwapCommand>(d, target, Tr("곡 적용"), m.motion, std::move(dance)));
            }
            if (!job.camera.Empty()) {
                MotionData cam = d.camera;
                if (!job.camera.camera.empty()) cam.camera = job.camera.camera;
                if (!job.camera.light.empty()) cam.light = job.camera.light;
                if (!job.camera.shadow.empty()) cam.shadow = job.camera.shadow;
                parts.push_back(std::make_unique<MotionSwapCommand>(d, -1, Tr("곡 적용"), d.camera, std::move(cam)));
                if (!job.camera.camera.empty() && !options_.freeCamera) d.useMotionCamera = true;
            }
            if (!parts.empty()) d.history.Push(std::make_unique<CompositeCommand>(Tr("곡 적용"), std::move(parts)));
            if (!job.audio.empty()) StudioSetAudio(job.audio);
            d.selection.clear();
            d.rowsKey = ~0ull;
            d.physicsFrame = -1.0f;
            const bool danceSkipped = target < 0 && !job.dance.Empty();
            toast_ = {Tr("곡을 적용했어요"),
                      danceSkipped ? std::string(Tr("댄스 모션은 캐릭터를 선택하고 다시 적용하세요")) : job.label, {}, false,
                      timeSeconds_ + 5.0};
        } else {
            UploadBatch batch(ctx_);
            int first = -1;
            for (StudioPackageModel& pm : job.models) {
                if (!pm.pmx) continue;
                auto sm = std::make_unique<StudioModel>();
                sm->name = pm.name;
                sm->libraryId = pm.libraryId;
                sm->kind = pm.kind;
                sm->visible = pm.visible;
                sm->uid = d.nextUid++;
                sm->path = pm.pmx->sourcePath;
                sm->pmx = pm.pmx;
                sm->inst = std::make_unique<ModelInstance>(pm.pmx);
                sm->gpu = renderer_.CreateModel(batch, *pm.pmx, pm.textures,
                                                pm.kind == ModelKind::Stage ? ModelRole::Stage : ModelRole::Character);
                if (!sm->gpu) {
                    LOG_WARN("studio: GPU upload failed: %s", pm.name.c_str());
                    continue;
                }
                sm->BuildRowGroups();
                sm->inst->UpdatePose();
                if (first < 0) first = (int)d.models.size();
                d.models.push_back(std::move(sm));
            }
            batch.Submit();
            if (first >= 0) {
                ++d.projectVersion;
                d.physicsFrame = -1.0f;
                if (d.models[(size_t)first]->kind != ModelKind::Stage) StudioSelectModel(first);
                toast_ = {Tr("모델을 추가했어요"), job.label, {}, false, timeSeconds_ + 3.0};
                LOG_INFO("studio: added %s (%d models)", job.label.c_str(), (int)d.models.size());
            }
        }
        if (job.mcp) {  // an MCP studio_add_model waits for this
            if (!ok) job.mcp->Reject("Loading failed: " + job.label + (job.error.empty() ? "" : ": " + job.error));
            else job.mcp->Resolve({{"status", "added"}, {"label", job.label}, {"models_count", (int)d.models.size()},
                                   {"selected_model", d.selectedModel}});
        }
        if (job.audioPreloaded) audio_.ReleasePreload(job.audio);   // the loaded sound holds its own reference
        studioJobs_.erase(studioJobs_.begin() + (ptrdiff_t)ji);
    }
}

// ---------------------------------------------------------------------------
// Removing / selecting / props
// ---------------------------------------------------------------------------

void App::StudioRemoveModel(int index) {
    StudioDoc& d = *studio_;
    if (index < 0 || index >= (int)d.models.size()) return;
    // the GPU may still read this model's buffers from frames in flight
    ctx_.WaitForGpu();
    const uint32_t uid = d.models[(size_t)index]->uid;
    const std::string name = d.models[(size_t)index]->name;
    d.models.erase(d.models.begin() + index);
    for (auto& m : d.models)
        if (m->IsProp() && m->attach.parent == (int)uid) m->attach.parent = -1;  // stays where it is, in the world
    for (auto& job : studioJobs_)
        if (job->targetUid == uid) job->targetUid = 0;
    // undo steps refer to models by index: they cannot survive a removal
    d.history.Clear();
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
    d.clipboard.clear();
    d.clipboardModel = -2;
    if (d.selectedModel == index) d.selectedModel = -1;
    else if (d.selectedModel > index) --d.selectedModel;
    d.rowsKey = ~0ull;
    d.physicsFrame = -1.0f;
    d.curveEditing = false;
    d.curveBefore.clear();
    ++d.projectVersion;
    studioViewDrag_ = 0;
    studioHoverBone_ = -1;
    studioGizmoShown_ = false;
    studioKeyEdit_ = false;
    studioPoseFieldEdit_ = false;
    toast_ = {Tr("모델을 제거했어요"), name + "  ·  " + Tr("실행 취소 기록을 비웠어요"), {}, false, timeSeconds_ + 4.0};
    LOG_INFO("studio: removed %s (%d models left)", name.c_str(), (int)d.models.size());
}

void App::StudioSelectModel(int index) {
    StudioDoc& d = *studio_;
    if (index < -1 || index >= (int)d.models.size()) return;
    if (d.selectedModel == index && (index >= 0 || d.selectedLightUid == 0)) return;
    if (index >= 0) StudioPossess(false);  // possession belongs to the camera row
    d.selectedModel = index;
    d.selectedLightUid = 0;
    d.selection.clear();
    d.selectedRows.clear();
    d.selectedBones.clear();
    d.activeBone = -1;
    d.collapsed.clear();
    d.rowsKey = ~0ull;
}

DirectX::XMFLOAT4X4 App::StudioPropRoot(const StudioModel& m) const {
    using namespace DirectX;
    const StudioDoc& d = *studio_;
    XMMATRIX parent = XMMatrixIdentity();
    const int pi = m.attach.parent >= 0 ? d.IndexOfUid((uint32_t)m.attach.parent) : -1;
    if (pi >= 0 && d.models[(size_t)pi].get() != &m) {
        const StudioModel& p = *d.models[(size_t)pi];
        const int bone = m.attach.bone.empty() ? -1 : p.pmx->FindBone(m.attach.bone);
        if (bone >= 0) parent = XMLoadFloat4x4(&p.inst->BoneWorld(bone));
        // the parent's display scale is applied after its bone worlds (ModelInstance::SetScale)
        const float s = p.inst->Scale();
        if (s != 1.0f) parent = XMMatrixMultiply(parent, XMMatrixScaling(s, s, s));
    }
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(PropOffsetMatrix(m.attach), parent));
    return out;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void App::DrawStudioAddMenu() {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##studioadd")) {
        const auto header = [&](const char* title) {
            if (IconButton("##addback", icon::CaretLeft, Tr("뒤로"), false, 28.0f)) studioAddPage_ = 0;
            const ImVec2 c = ImGui::GetItemRectMin();
            Text(ImGui::GetWindowDrawList(), Font::Semibold, size::Small, ImVec2(c.x + Dp(36.0f), c.y + Dp(5.0f)), p.ink, title);
            Gap(6.0f);
            SearchField("##addfilter", studioAddFilter_, sizeof(studioAddFilter_), Tr("검색"), 276.0f);
            Gap(6.0f);
        };
        const std::string needle = ToLowerAscii(studioAddFilter_);
        const auto match = [&](const std::string& a, const std::string& b) {
            return needle.empty() || ToLowerAscii(a).find(needle) != std::string::npos ||
                   ToLowerAscii(b).find(needle) != std::string::npos;
        };
        if (studioAddPage_ == 0) {
            SectionLabel(Tr("모델"));
            if (MenuItemChip("##addch", Tr("캐릭터"), icon::PersonSimple, "›")) { studioAddPage_ = 1; studioAddFilter_[0] = 0; }
            if (MenuItemChip("##addst", Tr("스테이지"), icon::Mountains, "›")) { studioAddPage_ = 2; studioAddFilter_[0] = 0; }
            if (MenuItemChip("##addprop", Tr("소품 (파일에서)"), icon::Cube)) {
                ImGui::CloseCurrentPopup();
                StudioAddModelDialog(ModelKind::Prop);
            }
            MenuSeparator();
            SectionLabel(Tr("모션 · 음원"));
            if (MenuItemChip("##addsong", Tr("라이브러리 곡 적용"), icon::Music, "›")) { studioAddPage_ = 3; studioAddFilter_[0] = 0; }
            if (MenuItemChip("##addaudio", d.hasAudio ? Tr("음원 바꾸기 (파일)") : Tr("음원 (파일)"), icon::MusicNotes)) {
                ImGui::CloseCurrentPopup();
                StudioAudioDialog();
            }
            const char* target = d.selectedModel < 0 ? Tr("카메라") : (d.Selected() ? d.Selected()->name.c_str() : "");
            const std::string vmd = std::string(Tr("모션 VMD → ")) + target;
            if (MenuItemChip("##addvmd", vmd.c_str(), icon::FilmStrip)) {
                ImGui::CloseCurrentPopup();
                StudioImportVmd();
            }
        } else if (studioAddPage_ == 1 || studioAddPage_ == 2) {
            const bool chars = studioAddPage_ == 1;
            header(chars ? Tr("캐릭터 추가") : Tr("스테이지 추가"));
            ImGui::BeginChild("##addlist", ImVec2(0, Dp(300.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            int shown = 0;
            const int n = chars ? (int)library_.characters.size() : (int)library_.stages.size();
            for (int i = 0; i < n; ++i) {
                const std::string& name = chars ? library_.characters[(size_t)i].displayName : library_.stages[(size_t)i].displayName;
                const std::string& id = chars ? library_.characters[(size_t)i].id : library_.stages[(size_t)i].id;
                if (!match(name, id)) continue;
                ++shown;
                ImGui::PushID(i);
                if (MenuItemChip("##libitem", name.c_str(), chars ? icon::PersonSimple : icon::Mountains)) {
                    ImGui::CloseCurrentPopup();
                    if (chars) StudioAddLibraryCharacter(i);
                    else StudioAddLibraryStage(i);
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                PushFont(Font::Regular, size::Small);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
                ImGui::TextUnformatted(n == 0 ? Tr("라이브러리에 없어요") : Tr("검색 결과가 없습니다"));
                ImGui::PopStyleColor();
                PopFont();
            }
            ImGui::EndChild();
            MenuSeparator();
            if (Button("##addfile", Tr("파일에서 열기…"), icon::FolderOpen, ButtonKind::Secondary, ImVec2(276.0f, 36.0f))) {
                ImGui::CloseCurrentPopup();
                StudioAddModelDialog(chars ? ModelKind::Character : ModelKind::Stage);
            }
        } else {
            header(Tr("라이브러리 곡 적용"));
            const StudioModel* sel = d.Selected();
            const std::string hint = sel && sel->kind == ModelKind::Character
                                         ? std::string(Tr("댄스 → ")) + sel->name + Tr(", 카메라와 음원도 함께")
                                         : std::string(Tr("카메라와 음원만 (캐릭터를 선택하면 댄스도)"));
            PushFont(Font::Regular, size::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Dp(276.0f));
            ImGui::TextWrapped("%s", hint.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            PopFont();
            Gap(4.0f);
            ImGui::BeginChild("##songlist", ImVec2(0, Dp(300.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            int shown = 0;
            for (int i = 0; i < (int)library_.songs.size(); ++i) {
                const SongAsset& s = library_.songs[(size_t)i];
                if (!match(s.displayName, s.id)) continue;
                ++shown;
                ImGui::PushID(i);
                if (MenuItemChip("##song", s.displayName.c_str(), icon::Music)) {
                    ImGui::CloseCurrentPopup();
                    StudioApplyLibrarySong(i);
                }
                ImGui::PopID();
            }
            if (shown == 0) {
                PushFont(Font::Regular, size::Small);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
                ImGui::TextUnformatted(library_.songs.empty() ? Tr("라이브러리에 없어요") : Tr("검색 결과가 없습니다"));
                ImGui::PopStyleColor();
                PopFont();
            }
            ImGui::EndChild();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void App::DrawStudioModelMenu(int index) {
    using namespace ui;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImGui::SetNextWindowSize(ImVec2(Dp(index == -2 ? 280.0f : 240.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##modelmenu")) {
        if (index == -2) {
            // audio row: offset, replace, remove
            SectionLabel(Tr("음원 오프셋 (초)"));
            float off = (float)d.audioOffset;
            ImGui::SetNextItemWidth(-1.0f);
            PushFont(Font::Regular, size::Small);
            if (ImGui::DragFloat("##audiooffset", &off, 0.01f, -600.0f, 600.0f, "%+.2f")) {
                d.audioOffset = off;
                d.audioEndFrame = (float)((audio_.DurationSeconds() + d.audioOffset) * kMmdFps);
                StudioSeekAudio();
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) ++d.projectVersion;
            PopFont();
            Tooltip(Tr("음원이 타임라인의 이 시간에 시작해요 (양수: 늦게, 음수: 앞부분을 건너뜀)"));
            Gap(8.0f);
            if (MenuItemChip("##audioreplace", Tr("음원 바꾸기…"), icon::FolderOpen)) {
                ImGui::CloseCurrentPopup();
                StudioAudioDialog();
            }
            if (MenuItemChip("##audioremove", Tr("음원 제거"), icon::Trash)) {
                ImGui::CloseCurrentPopup();
                StudioSetAudio({});
            }
        } else if (index >= 0 && index < (int)d.models.size()) {
            StudioModel& m = *d.models[(size_t)index];
            TextEllipsis(ImGui::GetWindowDrawList(), Font::Semibold, size::Small, ImGui::GetCursorScreenPos(),
                         ImGui::GetCursorScreenPos().x + Dp(216.0f), p.ink2, m.name.c_str());
            Gap(24.0f);
            if (MenuItemChip("##rename", Tr("이름 바꾸기"), icon::PencilSimple)) {
                studioRenameModel_ = index;
                std::snprintf(studioRenameBuf_, sizeof(studioRenameBuf_), "%s", m.name.c_str());
                ImGui::CloseCurrentPopup();
            }
            if (!m.IsStage()) {
                if (MenuItemChip("##mimport", Tr("모션 VMD 불러오기…"), icon::DownloadSimple)) {
                    ImGui::CloseCurrentPopup();
                    StudioSelectModel(index);
                    StudioImportVmd();
                }
                if (MenuItemChip("##mexport", Tr("모션 VMD 내보내기…"), icon::Export)) {
                    ImGui::CloseCurrentPopup();
                    StudioSelectModel(index);
                    StudioExportVmd();
                }
            }
            MenuSeparator();
            if (MenuItemChip("##remove", Tr("제거…"), icon::Trash)) {
                studioRemoveModel_ = index;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // rename
    if (studioRenameModel_ >= 0) {
        if (studioRenameModel_ >= (int)d.models.size()) studioRenameModel_ = -1;
        else if (!ImGui::IsPopupOpen("##renamemodel")) ImGui::OpenPopup("##renamemodel");
    }
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(380.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(22.0f), Dp(20.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));
    if (ImGui::BeginPopupModal("##renamemodel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = TextField("##renamefield", Tr("모델 이름"), studioRenameBuf_, sizeof(studioRenameBuf_), 336.0f) &&
                           ImGui::IsKeyPressed(ImGuiKey_Enter);
        Gap(12.0f);
        bool close = false;
        if (Button("##renamecancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(162.0f, 38.0f))) close = true;
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##renameok", Tr("확인"), icon::Check, ButtonKind::Primary, ImVec2(162.0f, 38.0f)) || enter ||
            ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (studioRenameModel_ >= 0 && studioRenameModel_ < (int)d.models.size() && studioRenameBuf_[0] &&
                d.models[(size_t)studioRenameModel_]->name != studioRenameBuf_) {
                d.models[(size_t)studioRenameModel_]->name = studioRenameBuf_;
                ++d.projectVersion;
            }
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        if (close) {
            studioRenameModel_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // remove confirmation (removal cannot be undone: the history refers to models by index)
    if (studioRemoveModel_ >= 0) {
        if (studioRemoveModel_ >= (int)d.models.size()) studioRemoveModel_ = -1;
        else if (!ImGui::IsPopupOpen("##removemodel")) ImGui::OpenPopup("##removemodel");
    }
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(400.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));
    if (ImGui::BeginPopupModal("##removemodel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const std::string name = studioRemoveModel_ >= 0 && studioRemoveModel_ < (int)d.models.size()
                                     ? d.models[(size_t)studioRemoveModel_]->name : std::string();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(352.0f), p.ink,
                     (std::string(Tr("모델 제거: ")) + name).c_str());
        Text(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(30.0f)), p.ink2, Tr("모션도 함께 사라지고, 실행 취소 기록이 지워집니다."));
        ImGui::Dummy(ImVec2(0, Dp(64.0f)));
        bool close = false;
        if (Button("##removecancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(170.0f, 40.0f))) close = true;
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##removeok", Tr("제거"), icon::Trash, ButtonKind::Danger, ImVec2(170.0f, 40.0f))) {
            StudioRemoveModel(studioRemoveModel_);
            close = true;
        }
        if (close) {
            studioRemoveModel_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::DrawStudioPropPanel(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || !m->IsProp()) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("배치 (부모 본에 붙이기)"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    const PropAttach before = m->attach;
    PropAttach a = m->attach;
    const int parentIndex = a.parent >= 0 ? d.IndexOfUid((uint32_t)a.parent) : -1;
    const StudioModel* parent = parentIndex >= 0 ? d.models[(size_t)parentIndex].get() : nullptr;
    bool commit = false;  // push one undo step now (combo picks, chips, the end of a drag)
    static bool dragging = false;
    static PropAttach dragBefore;
    const auto row = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(64.0f));
        PushFont(Font::Regular, size::Small);
        // combo arrow boxes take the button colours: keep them in the field's tone
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(p.sunken));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Mix(p.sunken, p.lineStrong, 0.35f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink));
        widget();
        ImGui::PopStyleColor(3);
        PopFont();
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };
    row(Tr("부모"), [&] {
        if (ImGui::BeginCombo("##propparent", parent ? parent->name.c_str() : Tr("월드 (없음)"))) {
            if (ImGui::Selectable(Tr("월드 (없음)"), !parent)) { a.parent = -1; a.bone.clear(); commit = true; }
            for (int i = 0; i < (int)d.models.size(); ++i) {
                const StudioModel& o = *d.models[(size_t)i];
                if (o.IsProp()) continue;  // one level: props follow characters / stages
                ImGui::PushID(i);
                if (ImGui::Selectable(o.name.c_str(), parent == &o) && parent != &o) {
                    a.parent = (int)o.uid;
                    // a character's right wrist is the usual place for a hand-held prop
                    a.bone = o.kind == ModelKind::Character && o.pmx->FindBone("\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96") >= 0
                                 ? "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96" : "";
                    commit = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    });
    if (parent) {
        row(Tr("본"), [&] {
            static char filter[64] = {};
            if (ImGui::BeginCombo("##propbone", a.bone.empty() ? Tr("원점") : a.bone.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::IsWindowAppearing()) {
                    filter[0] = 0;
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##bonefilter", Tr("본 검색"), filter, sizeof(filter));
                const std::string needle = ToLowerAscii(filter);
                if (needle.empty() && ImGui::Selectable(Tr("원점"), a.bone.empty())) { a.bone.clear(); commit = true; }
                for (size_t b = 0; b < parent->pmx->bones.size(); ++b) {
                    const std::string& bn = parent->pmx->bones[b].name;
                    if (!needle.empty() && ToLowerAscii(bn).find(needle) == std::string::npos) continue;
                    ImGui::PushID((int)b);
                    if (ImGui::Selectable(bn.c_str(), bn == a.bone)) { a.bone = bn; commit = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        });
        // quick picks for characters (MMD's usual accessory parents)
        if (parent->kind == ModelKind::Character) {
            struct Pick { const char* label; const char* bone; };
            const Pick picks[] = {{Tr("오른손"), "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96"},
                                  {Tr("왼손"), "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96"},
                                  {Tr("머리"), "\xE9\xA0\xAD"}};
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(64.0f), ImGui::GetCursorScreenPos().y));
            const float chipW = ((w - Dp(64.0f)) / Dpi() - 8.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (parent->pmx->FindBone(picks[i].bone) < 0) continue;
                if (i) ImGui::SameLine(0, Dp(4.0f));
                ImGui::PushID(i);
                if (Chip("##bonepick", picks[i].label, nullptr, a.bone == picks[i].bone, chipW)) { a.bone = picks[i].bone; commit = true; }
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(w, Dp(4.0f)));
        }
    }
    bool dragEnded = false;
    const auto dragRow = [&](const char* label, auto&& widget) {
        row(label, [&] {
            widget();
            if (ImGui::IsItemActivated() && !dragging) { dragging = true; dragBefore = before; }
            if (ImGui::IsItemDeactivated()) dragEnded = true;
        });
    };
    dragRow(Tr("위치"), [&] { ImGui::DragFloat3("##propt", &a.translation.x, 0.02f, 0.0f, 0.0f, "%.2f"); });
    dragRow(Tr("회전"), [&] { ImGui::DragFloat3("##propr", &a.rotationDeg.x, 0.25f, 0.0f, 0.0f, "%.1f°"); });
    dragRow(Tr("크기"), [&] { ImGui::DragFloat("##props", &a.scale, 0.005f, 0.01f, 100.0f, "%.3f"); });
    a.scale = std::clamp(a.scale, 0.01f, 100.0f);
    if (!(a == m->attach)) m->attach = a;  // live while dragging; the undo step is pushed below
    if (commit && !(a == before)) {
        d.history.Push(std::make_unique<PropAttachCommand>(d, m->uid, before, a));
    } else if (dragEnded && dragging) {
        dragging = false;
        if (!(m->attach == dragBefore)) d.history.Push(std::make_unique<PropAttachCommand>(d, m->uid, dragBefore, m->attach));
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(64.0f), ImGui::GetCursorScreenPos().y));
    if (Button("##propreset", Tr("배치 초기화"), icon::ArrowCcw, ButtonKind::Ghost, ImVec2((w - Dp(64.0f)) / Dpi(), 32.0f))) {
        PropAttach reset;
        reset.parent = m->attach.parent;
        reset.bone = m->attach.bone;
        if (!(reset == m->attach)) d.history.Push(std::make_unique<PropAttachCommand>(d, m->uid, m->attach, reset));
    }
    ImGui::Dummy(ImVec2(w, Dp(4.0f)));
}

void App::StudioCommitPlace(uint32_t uid, const PropAttach& before, const PropAttach& after) {
    if (before == after) return;
    studio_->history.Push(std::make_unique<PropAttachCommand>(*studio_, uid, before, after, true));
}

// A character's or stage's place in the world (Unity-style transform): position, rotation, scale. Live while
// dragging a field, one undo step per drag / typed value. The viewport gizmo (StudioViewportPose) edits the same data.
void App::DrawStudioPlacePanel(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->IsProp()) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("트랜스폼"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    PropAttach a = m->place;
    bool dragEnded = false;
    const auto dragRow = [&](const char* label, auto&& widget) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(5.0f)), p.ink3, label);
        ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(64.0f), c.y));
        ImGui::SetNextItemWidth(w - Dp(64.0f));
        PushFont(Font::Regular, size::Small);
        widget();
        PopFont();
        if (ImGui::IsItemActivated() && !studioPlaceDragging_) {
            studioPlaceDragging_ = true;
            studioPlaceBefore_ = m->place;
        }
        if (ImGui::IsItemDeactivated()) dragEnded = true;
        ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    };
    dragRow(Tr("위치"), [&] { ImGui::DragFloat3("##placet", &a.translation.x, 0.05f, 0.0f, 0.0f, "%.2f"); });
    dragRow(Tr("회전"), [&] { ImGui::DragFloat3("##placer", &a.rotationDeg.x, 0.5f, 0.0f, 0.0f, "%.1f°"); });
    dragRow(Tr("크기"), [&] { ImGui::DragFloat("##places", &a.scale, 0.005f, 0.01f, 100.0f, "%.3f"); });
    a.scale = std::clamp(a.scale, 0.01f, 100.0f);
    if (!(a == m->place)) m->place = a;
    if (dragEnded && studioPlaceDragging_ && studioViewDrag_ != 6) {
        studioPlaceDragging_ = false;
        StudioCommitPlace(m->uid, studioPlaceBefore_, m->place);
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + Dp(64.0f), ImGui::GetCursorScreenPos().y));
    if (Button("##placereset", Tr("트랜스폼 초기화"), icon::ArrowCcw, ButtonKind::Ghost, ImVec2((w - Dp(64.0f)) / Dpi(), 32.0f))) {
        const PropAttach before = m->place;
        m->place = PropAttach{};
        StudioCommitPlace(m->uid, before, m->place);
    }
    ImGui::Dummy(ImVec2(w, Dp(4.0f)));
}

void App::DrawStudioUnsavedPrompt() {
    using namespace ui;
    if (!studioLeaveConfirm_ || !studio_) return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::OpenPopup("##leavestudio");
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(460.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(P().surface));
    StudioAction run = StudioAction::None;
    bool save = false;
    if (ImGui::BeginPopupModal("##leavestudio", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(dl, Font::Semibold, size::Title, c, P().ink, Tr("저장하지 않은 편집이 있어요"));
        Text(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(30.0f)), P().ink2,
             Tr("저장하지 않으면 마지막 저장 이후의 변경 사항이 사라집니다."));
        ImGui::Dummy(ImVec2(0, Dp(64.0f)));
        if (Button("##stay", Tr("계속 편집"), nullptr, ButtonKind::Secondary, ImVec2(128.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            studioLeaveConfirm_ = false;
            studioPending_ = StudioAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, Dp(10.0f));
        if (Button("##discard", Tr("저장 안 함"), nullptr, ButtonKind::Danger, ImVec2(128.0f, 40.0f))) {
            run = studioPending_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, Dp(10.0f));
        if (Button("##saveleave", Tr("저장"), icon::FloppyDisk, ButtonKind::Primary, ImVec2(128.0f, 40.0f))) {
            save = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (save) {
        studioLeaveConfirm_ = false;
        if (StudioSave(false)) run = studioPending_;  // cancelled save dialog: stay in the studio
        else studioPending_ = StudioAction::None;
    }
    if (run != StudioAction::None) StudioRunAction(run, studioPendingFile_);
}

void App::DrawStudioProjectMenu() {
    using namespace ui;
    ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##studioproj")) {
        StudioAction act = StudioAction::None;
        std::filesystem::path file;
        if (MenuItemChip("##pnew", Tr("새 프로젝트"), icon::FilePlus, "Ctrl+N")) act = StudioAction::New;
        if (MenuItemChip("##popen", Tr("열기…"), icon::FolderOpen, "Ctrl+O")) act = StudioAction::Open;
        MenuSeparator();
        bool save = false, saveAs = false;
        if (MenuItemChip("##psave", Tr("저장"), icon::FloppyDisk, "Ctrl+S")) save = true;
        if (MenuItemChip("##psaveas", Tr("다른 이름으로 저장…"), icon::FloppyDisk, "Ctrl+Shift+S")) saveAs = true;
        const StudioDoc& d = *studio_;
        bool anyRecent = false;
        for (size_t i = 0; i < settings_.recentProjects.size(); ++i) {
            const std::filesystem::path rp = Utf8ToPath(settings_.recentProjects[i]);
            if (!d.projectPath.empty() && rp.lexically_normal() == d.projectPath.lexically_normal()) continue;
            if (!anyRecent) {
                MenuSeparator();
                SectionLabel(Tr("최근 프로젝트"));
                anyRecent = true;
            }
            ImGui::PushID((int)i);
            if (MenuItemChip("##recent", StemUtf8(rp).c_str(), icon::FolderSimple)) {
                act = StudioAction::OpenFile;
                file = rp;
            }
            Tooltip(settings_.recentProjects[i].c_str());
            ImGui::PopID();
        }
        if (act != StudioAction::None || save || saveAs) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (save || saveAs) StudioSave(saveAs);
        if (act != StudioAction::None) StudioRequest(act, file);
    }
    ImGui::PopStyleVar();
}

} // namespace mmdx
