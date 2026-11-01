// Drag and drop onto the window (WM_DROPFILES) and the ui-script "drop" command: model / motion / pose / audio /
// project files are routed into the Studio (UiShaders.cpp keeps consuming shader-pack zips and folders).
#include "app/App.h"

#include <algorithm>
#include <cstdio>
#include <future>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "asset/MmeEffect.h"
#include "studio/StudioDoc.h"

namespace mmdx {

namespace {

std::string LowerExt(const std::filesystem::path& p) { return ToLowerAscii(PathToUtf8(p.extension())); }

bool IsModelExt(const std::string& e) {
    return e == ".pmx" || e == ".pmd" || e == ".x" || e == ".vrm" || e == ".glb" || e == ".gltf" || e == ".fbx" ||
           e == ".obj";
}
bool IsAudioExt(const std::string& e) { return e == ".wav" || e == ".mp3" || e == ".ogg" || e == ".flac"; }
bool IsVideoExt(const std::string& e) {
    return e == ".avi" || e == ".mp4" || e == ".mov" || e == ".wmv" || e == ".mkv";
}

// A shader-pack zip / folder is left for PollShaderPacks (UiShaders.cpp), not routed into the studio.
bool IsPackLike(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) return true;
    return LowerExt(p) == ".zip";
}

std::string UnsupportedList(const std::vector<std::filesystem::path>& files) {
    std::string list;
    const size_t shown = std::min<size_t>(files.size(), 5);
    for (size_t i = 0; i < shown; ++i) {
        if (i) list += ", ";
        list += PathToUtf8(files[i].filename());
    }
    if (files.size() > shown) list += ", ...";
    return list;
}

}  // namespace

// The studio model kind a dropped model file becomes. `.x` (an MMD accessory with no humanoid skeleton) is a prop.
// A PMX/PMD the library's own classification put in a stage (a stage part) becomes a stage; a humanoid is always a
// character. Anything else (imported formats, unclassified accessory-less PMX) becomes a character.
studio::ModelKind App::DropModelKind(const std::filesystem::path& file, const std::string& ext) const {
    if (ext == ".x") return studio::ModelKind::Prop;
    if (ext == ".pmx" || ext == ".pmd") {
        const std::filesystem::path abs = std::filesystem::absolute(file);
        for (const CharacterAsset& c : library_.characters)
            if (c.modelPath == abs) return studio::ModelKind::Character;   // humanoid: the library kept it as a character
        for (const StageAsset& s : library_.stages)
            if (std::find(s.parts.begin(), s.parts.end(), abs) != s.parts.end()) return studio::ModelKind::Stage;
    }
    return studio::ModelKind::Character;
}

// Peel off the files this route handles, leaving shader-pack zips / folders for PollShaderPacks. Runs before
// PollShaderPacks every frame; only the select (lobby) and studio screens route model files into the studio.
void App::PollDroppedFiles() {
    // A drop that arrives while the library still scans (or a scene loads) has no studio to enter yet: hold it and
    // apply it once the lobby is up, so nothing is silently lost.
    if (!pendingDrop_.empty()) {
        if (screen_ != Screen::Select) return;
        std::vector<std::filesystem::path> files = std::move(pendingDrop_);
        pendingDrop_.clear();
        ApplyStudioDrop(std::move(files));
    }
    if (droppedFiles_.empty()) return;
    if (screen_ == Screen::Scanning || screen_ == Screen::Loading) {
        // Defer real files (apply at the lobby); leave shader packs in droppedFiles_ for PollShaderPacks' screens.
        std::vector<std::filesystem::path> packs;
        for (auto& f : droppedFiles_) {
            if (IsPackLike(f)) packs.push_back(std::move(f));
            else pendingDrop_.push_back(std::move(f));
        }
        droppedFiles_ = std::move(packs);
        return;
    }
    if (screen_ != Screen::Select && screen_ != Screen::Studio) return;
    bool onlyPacks = true;
    for (const auto& f : droppedFiles_) {
        if (!IsPackLike(f)) {
            onlyPacks = false;
            break;
        }
    }
    if (onlyPacks) return;   // shader packs: PollShaderPacks installs them on its screens
    std::vector<std::filesystem::path> files = std::move(droppedFiles_);
    droppedFiles_.clear();
    ApplyStudioDrop(std::move(files));
}

void App::ApplyStudioDrop(std::vector<std::filesystem::path> files) {
    std::vector<std::filesystem::path> models, vmds, vpds, audios, projects, effects, videos, unsupported;
    for (auto& f : files) {
        const std::string e = LowerExt(f);
        if (e == ".mmdxproj" || e == ".pmm") projects.push_back(std::move(f));
        else if (e == ".x" && [&] {   // M5c: a `.x` with a same-stem `.fx` is an effect object, not a prop
                     std::filesystem::path fx, mesh;
                     ResolveMmeObjectPair(std::filesystem::absolute(f), fx, mesh);
                     return !fx.empty() && !mesh.empty();
                 }())
            effects.push_back(std::move(f));
        else if (IsModelExt(e)) models.push_back(std::move(f));
        else if ((e == ".vmd" || e == ".mvd")) vmds.push_back(std::move(f));
        else if (e == ".vpd") vpds.push_back(std::move(f));
        else if (e == ".fx") effects.push_back(std::move(f));
        else if (IsAudioExt(e)) audios.push_back(std::move(f));
        else if (IsVideoExt(e)) videos.push_back(std::move(f));
        else unsupported.push_back(std::move(f));
    }

    // A project dropped with anything else: the project wins (open it, respecting the unsaved-changes flow).
    if (!projects.empty()) {
        StudioRequest(StudioAction::OpenFile, std::filesystem::absolute(projects[0]));
        if (!unsupported.empty()) {
            toast_ = {Tr("지원하지 않는 파일"), UnsupportedList(unsupported), {}, true, timeSeconds_ + 6.0};
        }
        return;
    }

    const size_t accepted = models.size() + vmds.size() + vpds.size() + audios.size() + effects.size() + videos.size();
    if (accepted == 0) {
        if (!unsupported.empty()) {
            toast_ = {Tr("지원하지 않는 파일"), UnsupportedList(unsupported), {}, true, timeSeconds_ + 6.0};
            LOG_INFO("drop: no supported files (%d unsupported)", (int)unsupported.size());
        }
        return;
    }

    // The lobby: start a new empty project to hold the drop.
    if (screen_ == Screen::Select) StartStudioEmpty();
    if (!studio_) {
        if (!unsupported.empty())
            toast_ = {Tr("지원하지 않는 파일"), UnsupportedList(unsupported), {}, true, timeSeconds_ + 6.0};
        return;
    }

    // Models first, then motions / pose / audio / effects. A dropped VMD/VPD needs a model to land on and models load on
    // worker threads, so they are queued onto the first new model (models load in drop order, so it is in the
    // document first). If that model fails to load, StudioPollJobs re-queues the VMD/VPD onto the next new model.
    const bool oneModel = models.size() == 1;
    for (size_t i = 0; i < models.size(); ++i) {
        const std::filesystem::path abs = std::filesystem::absolute(models[i]);
        const bool first = i == 0;
        std::vector<std::filesystem::path> queued;
        if (first) {
            queued = vmds;
            queued.insert(queued.end(), vpds.begin(), vpds.end());
        }
        StudioAddModelFile(DropModelKind(models[i], LowerExt(models[i])), abs, 0, nullptr, std::move(queued),
                           first && !oneModel ? vmds.size() + vpds.size() : 0);
        LOG_INFO("drop: model %s", PathToUtf8(abs).c_str());
    }
    if (models.empty()) {
        // No new model: motions / poses go onto the selected model now (a camera VMD lands on the camera, and
        // StudioImportVmdFrom / StudioImportVpdFrom report a missing model themselves).
        for (const auto& v : vmds) StudioImportVmdFrom(std::filesystem::absolute(v));
        for (const auto& v : vpds) StudioImportVpdFrom(std::filesystem::absolute(v));
        if (audios.empty() && effects.empty() && videos.empty()) {   // the imports posted their own result toast
            if (!unsupported.empty())
                toast_ = {Tr("지원하지 않는 파일"), UnsupportedList(unsupported), {}, true, timeSeconds_ + 6.0};
            return;
        }
    } else {
        for (const auto& v : vmds) LOG_INFO("drop: motion %s (queued onto a new model)", PathToUtf8(v).c_str());
        for (const auto& v : vpds) LOG_INFO("drop: pose %s (queued onto a new model)", PathToUtf8(v).c_str());
    }
    for (const auto& a : audios) {
        // Decode on a worker: the main thread's Load then takes no time (same as the add menu's audio entry).
        const std::filesystem::path abs = std::filesystem::absolute(a);
        auto job = std::make_unique<StudioJob>();
        job->label = PathToUtf8(abs.filename());
        job->song = true;
        job->audioOnly = true;
        job->audio = abs;
        StudioJob* j = job.get();
        job->future = std::async(std::launch::async, [this, j] {
            j->audioPreloaded = audio_.Preload(j->audio);
            return true;   // a file that cannot be decoded fails in StudioSetAudio (with its toast)
        });
        studioJobs_.push_back(std::move(job));
        LOG_INFO("drop: audio %s", PathToUtf8(abs).c_str());
    }
    // Background AVI (背景AVI): the last dropped video wins; if it cannot be opened the current one is kept.
    for (const auto& v : videos) {
        const std::filesystem::path abs = std::filesystem::absolute(v);
        if (StudioSetBgVideo(abs)) LOG_INFO("drop: background video %s", PathToUtf8(abs).c_str());
    }

    // M6b: dropped .fx effects. M5c: a `.x` dropped with a same-stem `.fx` becomes an effect object; a `.fx` with
    // no `.x` next to it keeps the M6b behaviour (object -> the selected model's choice, post -> the project stack).
    // A Ray-MMD main effect (ray.x / ray.fx) opens the shared confirm modal first; a plain add keeps the flow below.
    for (const auto& f : effects) {
        const std::filesystem::path abs = std::filesystem::absolute(f);
        std::filesystem::path fx, mesh;
        ResolveMmeObjectPair(abs, fx, mesh);
        if (!mesh.empty()) {
            // A Ray-MMD main effect pair: queue the confirm modal, the add (StudioAddMmeObject) runs on "그래도".
            if (GuardRayMme(abs, [this](const std::filesystem::path& file) {
                    if (!studio_) return;
                    StudioAddMmeObject(file);
                    LOG_INFO("drop: MME effect object %s", PathToUtf8(file).c_str());
                })) {
                continue;
            }
            StudioAddMmeObject(abs);
            LOG_INFO("drop: MME effect object %s", PathToUtf8(abs).c_str());
            continue;
        }
        const std::string kind = LoadMmeEffect(abs, MmeLoadOptions{}).kind();
        if (kind == "post" || kind == "pre") {
            if (GuardRayMme(abs, [this](const std::filesystem::path& file) {
                    if (!studio_) return;
                    bool dup = false;
                    for (const studio::MmePostEntry& e : studio_->mmePost) dup |= e.path == file;
                    if (!dup) studio_->mmePost.push_back(studio::MmePostEntry{file, true});
                    StudioMmeChanged();
                })) {
                continue;
            }
            bool dup = false;
            for (const studio::MmePostEntry& e : studio_->mmePost) dup |= e.path == abs;
            if (!dup) studio_->mmePost.push_back(studio::MmePostEntry{abs, true});
            StudioMmeChanged();
            LOG_INFO("drop: MME post effect %s", PathToUtf8(abs).c_str());
        } else {
            studio::StudioModel* target = studio_->Selected();
            if (!target && studio_->models.size() == 1) target = studio_->models[0].get();
            if (target) {
                target->mme = MmeChoice{MmeChoice::File, abs};
                ++studio_->projectVersion;
                LOG_INFO("drop: MME object effect %s on %s", PathToUtf8(abs).c_str(), target->name.c_str());
            } else {
                unsupported.push_back(abs);
                LOG_INFO("drop: MME object effect %s (no model selected)", PathToUtf8(abs).c_str());
            }
        }
    }

    char title[96];
    std::snprintf(title, sizeof(title), Tr("파일 %d개를 추가했어요"), (int)accepted);
    std::string detail;
    if (!unsupported.empty()) detail = std::string(Tr("지원하지 않는 파일: ")) + UnsupportedList(unsupported);
    toast_ = {title, detail, {}, false, timeSeconds_ + 4.0};
    LOG_INFO("drop: %d files accepted into the studio (%d unsupported)", (int)accepted, (int)unsupported.size());
}

// Called from StudioPollJobs once a dropped model is in the document: import its queued VMD / VPD onto that model
// (a camera VMD still lands on the camera; StudioImportVmdFrom decides).
void App::ApplyQueuedDrops(int modelIndex, const std::vector<std::filesystem::path>& files) {
    if (!studio_ || files.empty()) return;
    studio::StudioDoc& d = *studio_;
    if (modelIndex < 0 || modelIndex >= (int)d.models.size()) return;
    d.selectedModel = modelIndex;
    for (const auto& f : files) {
        const std::string e = LowerExt(f);
        if ((e == ".vmd" || e == ".mvd")) StudioImportVmdFrom(f);
        else if (e == ".vpd") StudioImportVpdFrom(f);
    }
}

}  // namespace mmdx
