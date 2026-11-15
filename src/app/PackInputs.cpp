// Role resolution and audio-reactive inputs for shader packs (M1a-B).
// Fills FrameView::packInputs for every render path (raster / RT / PT / offline GI).
#include "app/App.h"
#include "audio/BeatAnalysis.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "render/ShaderPack.h"
#include "studio/StudioDoc.h"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace mmdx {

// Cache key of an audio file's analysis: path + modification time (the MCP beat tools use the same key).
static std::string AudioAnalysisKey(const std::filesystem::path& audioPath) {
    std::string key = PathToUtf8(audioPath);
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(audioPath, ec);
    key += "|";
    key += ec ? std::string("0") : std::to_string((long long)writeTime.time_since_epoch().count());
    return key;
}

// Continuous mark count at t: index of the mark at or before t + the phase toward the next one. Before the first
// mark it runs negative with `period`; after the last it keeps counting with the same period the phase inputs use.
static double ContinuousCount(const std::vector<double>& marks, double t, double period) {
    if (marks.empty() || !(period > 0.0)) return 0.0;
    if (t < marks.front()) return (t - marks.front()) / period;
    auto it = std::upper_bound(marks.begin(), marks.end(), t);
    const size_t idx = (size_t)(it - marks.begin()) - 1;
    const double start = marks[idx];
    if (idx + 1 < marks.size()) {
        const double next = marks[idx + 1];
        return next > start ? (double)idx + std::clamp((t - start) / (next - start), 0.0, 1.0) : (double)idx;
    }
    return (double)idx + (t - start) / period;
}

static double BeatPeriod(const BeatAnalysis& beats) { return beats.bpm > 0.0 ? 60.0 / beats.bpm : 0.5; }

static void SampleAudioInputs(const BeatAnalysis& beats, double audioSeconds, GpuPackInputs& out) {
    if (!beats.ok) {
        out.audio0 = {0.0f, 0.0f, 0.0f, 0.0f};
        out.audio1 = {0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }

    const double t = audioSeconds;
    float beatVal = 0.0f;
    float downbeatVal = 0.0f;
    float onsetVal = 0.0f;
    float energyVal = 0.0f;
    float sectionVal = 0.0f;
    float bpmVal = (float)beats.bpm;

    // 1. Beat phase (0..1) and current beat energy
    if (!beats.beats.empty() && t >= beats.beats.front()) {
        auto it = std::upper_bound(beats.beats.begin(), beats.beats.end(), t);
        size_t idx = (it - beats.beats.begin()) - 1;
        double bStart = beats.beats[idx];
        double bNext = (idx + 1 < beats.beats.size())
                           ? beats.beats[idx + 1]
                           : (bStart + (beats.bpm > 0.0 ? 60.0 / beats.bpm : 0.5));
        if (bNext > bStart) {
            beatVal = (float)std::clamp((t - bStart) / (bNext - bStart), 0.0, 1.0);
        }
        if (idx < beats.beatEnergy.size()) {
            energyVal = beats.beatEnergy[idx];
        }
    }

    // 2. Downbeat phase (0..1) within the bar
    if (!beats.downbeats.empty() && t >= beats.downbeats.front()) {
        auto it = std::upper_bound(beats.downbeats.begin(), beats.downbeats.end(), t);
        size_t idx = (it - beats.downbeats.begin()) - 1;
        double dbStart = beats.downbeats[idx];
        double dbNext = (idx + 1 < beats.downbeats.size())
                            ? beats.downbeats[idx + 1]
                            : (dbStart + (beats.bpm > 0.0 ? 240.0 / beats.bpm : 2.0));
        if (dbNext > dbStart) {
            downbeatVal = (float)std::clamp((t - dbStart) / (dbNext - dbStart), 0.0, 1.0);
        }
    }

    // 3. Onset: envelope of the latest onset decaying with ~0.25 s time constant
    if (!beats.onsets.empty()) {
        auto it = std::upper_bound(beats.onsets.begin(), beats.onsets.end(), t);
        if (it != beats.onsets.begin()) {
            size_t idx = (it - beats.onsets.begin()) - 1;
            double dt = t - beats.onsets[idx];
            if (dt >= 0.0 && dt < 2.0 && idx < beats.onsetStrength.size()) {
                onsetVal = beats.onsetStrength[idx] * (float)std::exp(-dt / 0.25);
            }
        }
    }

    // 4. Section: 0 low, 0.5 mid, 1 high
    for (const auto& sec : beats.sections) {
        if (t >= sec.start && t < sec.end) {
            if (sec.level == "high") sectionVal = 1.0f;
            else if (sec.level == "mid") sectionVal = 0.5f;
            else sectionVal = 0.0f;
            break;
        }
    }

    out.audio0 = {beatVal, downbeatVal, onsetVal, energyVal};
    const float beatCount = (float)ContinuousCount(beats.beats, t, BeatPeriod(beats));
    out.audio1 = {sectionVal, bpmVal, 1.0f, beatCount}; // valid = 1.0f
}

static bool PackNeedsAudio(const std::string& id) {
    const ShaderPack* p = ShaderPacks().Find(id);
    return p && p->HasCap("inputs.audio");
}

// A loaded MMDX stage needs the analysis for an audio.* driver or for one of its bundled packs (packs[] folders and
// the folders of "program" screens, registered as stage:<id>/<folder>) that requires inputs.audio.
static bool StageNeedsAudio(const StageManifest& m) {
    if (StageUsesAudio(m)) return true;
    for (const StagePackRef& ref : m.packs)
        if (PackNeedsAudio(StagePackId(m.id, ref.folder))) return true;
    for (const StageScreen& s : m.screens)
        if (s.source == StageScreenSource::Program && !s.file.empty() && PackNeedsAudio(StagePackId(m.id, s.file)))
            return true;
    return false;
}

bool App::NeedsAudioInputs() const {
    // World entries (play: the world stack; studio: the project's world entities) and the loaded stages
    if (studio_) {
        for (const studio::StudioWorld& w : studio_->worlds)
            if (w.fx.entry.enabled && PackNeedsAudio(w.fx.entry.pack)) return true;
        for (const studio::StudioStage& st : studio_->stages)
            if (st.manifest && StageNeedsAudio(*st.manifest)) return true;
    }
    if (scene_) {
        for (const EffectStackEntry& e : settings_.worldStack)
            if (e.enabled && PackNeedsAudio(e.pack)) return true;
        if (scene_->stage && scene_->stage->Manifest() && StageNeedsAudio(*scene_->stage->Manifest())) return true;
    }
    // Enabled effect stack entries (the studio project's own stack while the studio is open)
    if (studio_) {
        for (const auto& e : studio_->effects) {
            if (!e.entry.enabled) continue;
            const ShaderPack* p = ShaderPacks().Find(e.entry.pack);
            if (p && p->HasCap("inputs.audio")) return true;
        }
    }
    for (const auto& entry : settings_.effectStack) {
        if (!entry.enabled) continue;
        const ShaderPack* p = ShaderPacks().Find(entry.pack);
        if (p && p->HasCap("inputs.audio")) return true;
    }
    // Loaded character surface pack
    if (scene_) {
        if (scene_->characterGpu) {
            const ShaderPack* p = ShaderPacks().Find(scene_->characterGpu->ShaderPackId());
            if (p && p->HasCap("inputs.audio")) return true;
        }
        for (const auto& extra : scene_->extras) {
            if (extra.gpu) {
                const ShaderPack* p = ShaderPacks().Find(extra.gpu->ShaderPackId());
                if (p && p->HasCap("inputs.audio")) return true;
            }
        }
    }
    // Studio models
    if (studio_) {
        for (const auto& m : studio_->models) {
            if (m && !m->shader.pack.empty()) {
                const ShaderPack* p = ShaderPacks().Find(m->shader.pack);
                if (p && p->HasCap("inputs.audio")) return true;
            }
        }
    }
    return false;
}

void App::EnsureAudioAnalysis(const std::filesystem::path& audioPath) {
    const std::string key = AudioAnalysisKey(audioPath);
    if (studioBeats_ && studioBeatsKey_ == key) return;

    if (audioFuture_.valid()) {
        if (offline_.mode != OfflineMode::None) {
            // Deterministic offline renders: wait for audio analysis
            audioFuture_.wait();
            auto [resKey, resAnalysis] = audioFuture_.get();
            studioBeats_ = std::make_shared<BeatAnalysis>(std::move(resAnalysis));
            studioBeatsKey_ = resKey;
            return;
        }
        if (audioFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto [resKey, resAnalysis] = audioFuture_.get();
            studioBeats_ = std::make_shared<BeatAnalysis>(std::move(resAnalysis));
            studioBeatsKey_ = resKey;
            if (studioBeatsKey_ == key) return;
        } else if (audioAnalysisPendingKey_ == key) {
            return; // currently working on this key
        }
    }

    // Launch background worker thread
    audioAnalysisPendingKey_ = key;
    audioFuture_ = std::async(std::launch::async, [path = audioPath, key]() {
        return std::make_pair(key, AnalyzeBeats(path));
    });

    if (offline_.mode != OfflineMode::None) {
        audioFuture_.wait();
        auto [resKey, resAnalysis] = audioFuture_.get();
        studioBeats_ = std::make_shared<BeatAnalysis>(std::move(resAnalysis));
        studioBeatsKey_ = resKey;
    }
}

void App::FillPackInputs(FrameView& view, const ModelInstance* performer, double audioSeconds) {
    // Role 0..5: performer bones
    static const char* const kRoleBones[6] = {
        "\xE9\xA0\xAD",                                   // 0: PerformerHead (頭)
        "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", // 1: PerformerCenter (センター)
        "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96",         // 2: PerformerHandL (左手首)
        "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96",         // 3: PerformerHandR (右手首)
        "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96",         // 4: PerformerFootL (左足首)
        "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96",         // 5: PerformerFootR (右足首)
    };

    for (uint32_t i = 0; i < 6; ++i) {
        GpuPackInputRole& role = view.packInputs.roles[i];
        if (performer) {
            const int b = performer->Model().FindBone(kRoleBones[i]);
            if (b >= 0) {
                const DirectX::XMFLOAT3 pos = performer->BoneWorldPosition(b);
                role.posValid = {pos.x, pos.y, pos.z, 1.0f};
                const DirectX::XMFLOAT4X4& m = performer->BoneWorld(b);
                DirectX::XMVECTOR r = DirectX::XMVectorSet(m._11, m._12, m._13, 0.0f);
                DirectX::XMVECTOR u = DirectX::XMVectorSet(m._21, m._22, m._23, 0.0f);
                DirectX::XMVECTOR f = DirectX::XMVectorSet(m._31, m._32, m._33, 0.0f);
                r = DirectX::XMVector3Normalize(r);
                u = DirectX::XMVector3Normalize(u);
                f = DirectX::XMVector3Normalize(f);
                DirectX::XMStoreFloat4(&role.right, r);
                DirectX::XMStoreFloat4(&role.up, u);
                DirectX::XMStoreFloat4(&role.forward, f);
                continue;
            }
        }
        role.posValid = {0.0f, 0.0f, 0.0f, 0.0f};
        role.right = {1.0f, 0.0f, 0.0f, 0.0f};
        role.up = {0.0f, 1.0f, 0.0f, 0.0f};
        role.forward = {0.0f, 0.0f, -1.0f, 0.0f};
    }

    // Role 6: LightKey
    // Key light: its direction as the z axis
    {
        GpuPackInputRole& role = view.packInputs.roles[6];
        DirectX::XMVECTOR z = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&view.light.direction));
        DirectX::XMFLOAT3 zF;
        DirectX::XMStoreFloat3(&zF, z);
        DirectX::XMVECTOR tempUp =
            std::abs(zF.y) < 0.99f ? DirectX::XMVectorSet(0, 1, 0, 0) : DirectX::XMVectorSet(1, 0, 0, 0);
        DirectX::XMVECTOR x = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(tempUp, z));
        DirectX::XMVECTOR y = DirectX::XMVector3Cross(z, x);
        role.posValid = {0.0f, 0.0f, 0.0f, 1.0f};
        DirectX::XMStoreFloat4(&role.right, x);
        DirectX::XMStoreFloat4(&role.up, y);
        DirectX::XMStoreFloat4(&role.forward, z);
    }

    // Role 7: CameraFocus
    // Camera focus: focus point, camera axes
    {
        GpuPackInputRole& role = view.packInputs.roles[7];
        DirectX::XMMATRIX viewMat = DirectX::XMLoadFloat4x4(&view.camera.view);
        DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, viewMat);
        DirectX::XMFLOAT4X4 iv;
        DirectX::XMStoreFloat4x4(&iv, invView);
        DirectX::XMVECTOR cRight = DirectX::XMVector3Normalize(DirectX::XMVectorSet(iv._11, iv._12, iv._13, 0.0f));
        DirectX::XMVECTOR cUp = DirectX::XMVector3Normalize(DirectX::XMVectorSet(iv._21, iv._22, iv._23, 0.0f));
        DirectX::XMVECTOR cForward = DirectX::XMVector3Normalize(DirectX::XMVectorSet(iv._31, iv._32, iv._33, 0.0f));
        DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&view.camera.eye);
        float dist = view.focusDistance > 0.0f ? view.focusDistance : 20.0f;
        DirectX::XMVECTOR focusPt = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(cForward, dist));
        DirectX::XMFLOAT3 fp;
        DirectX::XMStoreFloat3(&fp, focusPt);
        role.posValid = {fp.x, fp.y, fp.z, 1.0f};
        DirectX::XMStoreFloat4(&role.right, cRight);
        DirectX::XMStoreFloat4(&role.up, cUp);
        DirectX::XMStoreFloat4(&role.forward, cForward);
    }

    // Role 8: Audio inputs
    if (const BeatAnalysis* beats = CurrentAudioAnalysis()) {
        SampleAudioInputs(*beats, audioSeconds, view.packInputs);
    } else {
        view.packInputs.audio0 = {0.0f, 0.0f, 0.0f, 0.0f};
        view.packInputs.audio1 = {0.0f, 0.0f, 0.0f, 0.0f};
    }
}

const BeatAnalysis* App::CurrentAudioAnalysis() {
    std::filesystem::path currentAudio;
    if (scene_ && !scene_->audioPath.empty()) {
        currentAudio = scene_->audioPath;
    } else if (studio_ && studio_->hasAudio && !studio_->audioPath.empty()) {
        currentAudio = studio_->audioPath;
    }
    // Only the analysis of the audio that plays now (the cache may still hold another song's).
    if (currentAudio.empty() || !NeedsAudioInputs()) return nullptr;
    EnsureAudioAnalysis(currentAudio);
    const bool ready = studioBeats_ && studioBeats_->ok && studioBeatsKey_ == AudioAnalysisKey(currentAudio);
    return ready ? studioBeats_.get() : nullptr;
}

StageSignals App::StageAudioSignals(double audioSeconds) {
    StageSignals s;
    const BeatAnalysis* beats = CurrentAudioAnalysis();
    if (!beats) return s;
    GpuPackInputs in{};
    SampleAudioInputs(*beats, audioSeconds, in);
    s.audioValid = true;
    s.beatCount = ContinuousCount(beats->beats, audioSeconds, BeatPeriod(*beats));
    s.barCount = ContinuousCount(beats->downbeats, audioSeconds, 4.0 * BeatPeriod(*beats));   // bar = 4 beats
    s.onset = in.audio0.z;
    s.energy = in.audio0.w;
    s.section = in.audio1.x;
    s.bpm = in.audio1.y;
    return s;
}

} // namespace mmdx
