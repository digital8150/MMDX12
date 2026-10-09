#include "studio/StudioMotion.h"
#include "anim/Motion.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

namespace mmdx::studio {

namespace {
// The 16-byte table T of a bone interpolation block (see GetBoneCurve).
void ReadTable(const uint8_t interp[64], uint8_t t[16]) {
    std::memcpy(t, interp, 16);
    t[2] = interp[17];
    t[3] = interp[18];
}
void WriteTable(uint8_t interp[64], const uint8_t t[16]) {
    const uint8_t flags[2] = {interp[2], interp[3]};
    std::memset(interp, 0, 64);
    for (int row = 0; row < 4; ++row)
        for (int j = 0; j + row < 16; ++j) interp[row * 16 + j] = t[j + row];
    interp[2] = flags[0];
    interp[3] = flags[1];
}
} // namespace

void GetBoneCurve(const uint8_t interp[64], int channel, uint8_t out[4]) {
    uint8_t t[16];
    ReadTable(interp, t);
    for (int i = 0; i < 4; ++i) out[i] = t[i * 4 + channel];
}

void SetBoneCurve(uint8_t interp[64], int channel, const uint8_t c[4]) {
    uint8_t t[16];
    ReadTable(interp, t);
    for (int i = 0; i < 4; ++i) t[i * 4 + channel] = c[i];
    WriteTable(interp, t);
}

void FillLinearInterp(uint8_t interp[64]) {
    std::memset(interp, 0, 64);
    const uint8_t linear[4] = {20, 20, 107, 107};
    for (int c = 0; c < 4; ++c) SetBoneCurve(interp, c, linear);
}

void GetCameraCurve(const uint8_t interp[24], int param, uint8_t out[4]) {
    const uint8_t* p = interp + param * 4;
    out[0] = p[0]; out[1] = p[2]; out[2] = p[1]; out[3] = p[3];
}

void SetCameraCurve(uint8_t interp[24], int param, const uint8_t c[4]) {
    uint8_t* p = interp + param * 4;
    p[0] = c[0]; p[2] = c[1]; p[1] = c[2]; p[3] = c[3];
}

void FillLinearCameraInterp(uint8_t interp[24]) {
    const uint8_t linear[4] = {20, 20, 107, 107};
    for (int p = 0; p < 6; ++p) SetCameraCurve(interp, p, linear);
}

int MotionData::EndFrame() const {
    int end = 0;
    for (const auto& [n, k] : bones) if (!k.empty()) end = std::max(end, k.back().frame);
    for (const auto& [n, k] : morphs) if (!k.empty()) end = std::max(end, k.back().frame);
    for (const auto& [n, k] : ik) if (!k.empty()) end = std::max(end, k.back().frame);
    if (!camera.empty()) end = std::max(end, camera.back().frame);
    if (!light.empty()) end = std::max(end, light.back().frame);
    if (!shadow.empty()) end = std::max(end, shadow.back().frame);
    if (!focus.empty()) end = std::max(end, focus.back().frame);
    return end;
}

bool MotionData::InsertFrames(int at, int count) {
    if (count <= 0 || at < 0) return false;
    bool changed = false;
    for (auto& [name, keys] : bones) changed |= InsertFrameSpan(keys, at, count);
    for (auto& [name, keys] : morphs) changed |= InsertFrameSpan(keys, at, count);
    for (auto& [name, keys] : ik) changed |= InsertFrameSpan(keys, at, count);
    changed |= InsertFrameSpan(camera, at, count);
    changed |= InsertFrameSpan(light, at, count);
    changed |= InsertFrameSpan(shadow, at, count);
    changed |= InsertFrameSpan(focus, at, count);
    return changed;
}

bool MotionData::DeleteFrames(int at, int count) {
    if (count <= 0 || at < 0) return false;
    bool changed = false;
    for (auto& [name, keys] : bones) changed |= DeleteFrameSpan(keys, at, count);
    for (auto& [name, keys] : morphs) changed |= DeleteFrameSpan(keys, at, count);
    for (auto& [name, keys] : ik) changed |= DeleteFrameSpan(keys, at, count);
    changed |= DeleteFrameSpan(camera, at, count);
    changed |= DeleteFrameSpan(light, at, count);
    changed |= DeleteFrameSpan(shadow, at, count);
    changed |= DeleteFrameSpan(focus, at, count);
    // Tracks that lost their last key are removed from the maps.
    for (auto it = bones.begin(); it != bones.end();) it = it->second.empty() ? bones.erase(it) : std::next(it);
    for (auto it = morphs.begin(); it != morphs.end();) it = it->second.empty() ? morphs.erase(it) : std::next(it);
    for (auto it = ik.begin(); it != ik.end();) it = it->second.empty() ? ik.erase(it) : std::next(it);
    return changed;
}

size_t MotionData::ApproxBytes() const {
    size_t bytes = sizeof(MotionData) + modelName.size();
    const auto mapBytes = [](const auto& tracks) {
        size_t n = 0;
        for (const auto& [name, keys] : tracks) n += 64 + name.size() + keys.capacity() * sizeof(typename std::decay_t<decltype(keys)>::value_type);
        return n;
    };
    bytes += mapBytes(bones);
    bytes += mapBytes(morphs);
    bytes += mapBytes(ik);
    bytes += camera.capacity() * sizeof(CameraKf);
    bytes += light.capacity() * sizeof(LightKf);
    bytes += shadow.capacity() * sizeof(ShadowKf);
    bytes += focus.capacity() * sizeof(FocusKf);
    return bytes;
}

MotionData MotionData::FromVmd(const VmdMotion& vmd) {
    MotionData d;
    d.modelName = vmd.modelName;
    for (const VmdBoneKey& k : vmd.boneKeys) {
        BoneKf kf;
        kf.frame = (int)k.frame; kf.t = k.translation; kf.r = k.rotation;
        std::memcpy(kf.interp, k.interp, 64);
        UpsertKey(d.bones[k.boneName], kf);
    }
    for (const VmdMorphKey& k : vmd.morphKeys) UpsertKey(d.morphs[k.morphName], MorphKf{(int)k.frame, k.weight});
    for (const VmdIkKey& k : vmd.ikKeys)
        for (const auto& [name, enabled] : k.ikStates) UpsertKey(d.ik[name], IkKf{(int)k.frame, enabled});
    for (const VmdCameraKey& k : vmd.cameraKeys) {
        CameraKf kf;
        kf.frame = (int)k.frame; kf.distance = k.distance; kf.target = k.target; kf.rotation = k.rotation;
        std::memcpy(kf.interp, k.interp, 24);
        kf.fovDeg = k.fovDeg; kf.perspective = k.perspective;
        UpsertKey(d.camera, kf);
    }
    for (const VmdLightKey& k : vmd.lightKeys) UpsertKey(d.light, LightKf{(int)k.frame, k.color, k.direction});
    for (const VmdShadowKey& k : vmd.shadowKeys) UpsertKey(d.shadow, ShadowKf{(int)k.frame, k.mode, k.distance});
    return d;
}

void MotionData::Merge(const MotionData& o) {
    for (const auto& [name, keys] : o.bones) for (const BoneKf& k : keys) UpsertKey(bones[name], k);
    for (const auto& [name, keys] : o.morphs) for (const MorphKf& k : keys) UpsertKey(morphs[name], k);
    for (const auto& [name, keys] : o.ik) for (const IkKf& k : keys) UpsertKey(ik[name], k);
    for (const CameraKf& k : o.camera) UpsertKey(camera, k);
    for (const LightKf& k : o.light) UpsertKey(light, k);
    for (const ShadowKf& k : o.shadow) UpsertKey(shadow, k);
    for (const FocusKf& k : o.focus) UpsertKey(focus, k);
}

namespace {
// VMD name of `full` as the loader decodes it after truncation to `bytes` Shift-JIS bytes (Motion.cpp NameIndex).
std::string TruncatedVmdName(const std::string& full, size_t bytes) {
    const std::string sjis = Utf8ToSjis(full);
    return sjis.size() > bytes ? SjisToUtf8(sjis.substr(0, bytes)) : full;
}

template <class K, class GetName>
void Canonicalize(std::map<std::string, std::vector<K>>& tracks, size_t count, GetName getName, size_t bytes) {
    std::unordered_map<std::string, std::string> exact, truncated;  // name -> full name, first occurrence wins
    for (size_t i = 0; i < count; ++i) {
        const std::string& full = getName(i);
        exact.emplace(full, full);
        const std::string t = TruncatedVmdName(full, bytes);
        if (t != full) truncated.emplace(t, full);
    }
    std::map<std::string, std::vector<K>> out;
    for (auto& [name, keys] : tracks) {
        std::string target = name;
        if (!exact.count(name)) {
            if (auto it = truncated.find(name); it != truncated.end()) target = it->second;
        }
        auto& dst = out[target];
        for (const K& k : keys) UpsertKey(dst, k);
    }
    tracks.swap(out);
}
} // namespace

void MotionData::CanonicalizeNames(const PmxModel& model) {
    Canonicalize(bones, model.bones.size(), [&](size_t i) -> const std::string& { return model.bones[i].name; }, 15);
    Canonicalize(morphs, model.morphs.size(), [&](size_t i) -> const std::string& { return model.morphs[i].name; }, 15);
    Canonicalize(ik, model.bones.size(), [&](size_t i) -> const std::string& { return model.bones[i].name; }, 20);
}

namespace {
// Index of the last key with frame <= f (keys non-empty, f strictly inside the key range).
template <class K> size_t Segment(const std::vector<K>& keys, int f) {
    auto it = std::upper_bound(keys.begin(), keys.end(), f, [](int v, const K& k) { return v < k.frame; });
    return (size_t)(it - keys.begin()) - 1;
}
float Curve(const uint8_t c[4], float t) { return Bezier::FromBytes(c[0], c[1], c[2], c[3]).Evaluate(t); }
} // namespace

BoneKf SampleBone(const std::vector<BoneKf>& keys, int frame) {
    if (frame <= keys.front().frame || keys.size() == 1) { BoneKf k = keys.front(); k.frame = frame; return k; }
    if (frame >= keys.back().frame) {
        BoneKf k = keys.back();
        k.frame = frame;
        FillLinearInterp(k.interp);
        return k;
    }
    const size_t i = Segment(keys, frame);
    if (keys[i].frame == frame) return keys[i];
    const BoneKf& a = keys[i];
    const BoneKf& b = keys[i + 1];
    const float t = (float)(frame - a.frame) / (float)(b.frame - a.frame);
    uint8_t c[4];
    BoneKf out = b;
    out.frame = frame;
    GetBoneCurve(b.interp, 0, c); out.t.x = a.t.x + (b.t.x - a.t.x) * Curve(c, t);
    GetBoneCurve(b.interp, 1, c); out.t.y = a.t.y + (b.t.y - a.t.y) * Curve(c, t);
    GetBoneCurve(b.interp, 2, c); out.t.z = a.t.z + (b.t.z - a.t.z) * Curve(c, t);
    GetBoneCurve(b.interp, 3, c);
    DirectX::XMStoreFloat4(&out.r, DirectX::XMQuaternionSlerp(DirectX::XMLoadFloat4(&a.r), DirectX::XMLoadFloat4(&b.r),
                                                              Curve(c, t)));
    return out;
}

float SampleMorph(const std::vector<MorphKf>& keys, int frame) {
    if (frame <= keys.front().frame || keys.size() == 1) return keys.front().weight;
    if (frame >= keys.back().frame) return keys.back().weight;
    const size_t i = Segment(keys, frame);
    const float t = (float)(frame - keys[i].frame) / (float)(keys[i + 1].frame - keys[i].frame);
    return keys[i].weight + (keys[i + 1].weight - keys[i].weight) * t;
}

CameraKf SampleCamera(const std::vector<CameraKf>& keys, int frame) {
    if (frame <= keys.front().frame || keys.size() == 1) { CameraKf k = keys.front(); k.frame = frame; return k; }
    if (frame >= keys.back().frame) {
        CameraKf k = keys.back();
        k.frame = frame;
        FillLinearCameraInterp(k.interp);
        return k;
    }
    const size_t i = Segment(keys, frame);
    if (keys[i].frame == frame) return keys[i];
    const CameraKf& a = keys[i];
    const CameraKf& b = keys[i + 1];
    CameraKf out = b;
    out.frame = frame;
    // A one-frame gap is a cut: MMD holds the first key.
    if (b.frame - a.frame <= 1) { out = a; out.frame = frame; return out; }
    const float t = (float)(frame - a.frame) / (float)(b.frame - a.frame);
    uint8_t c[4];
    auto lerp = [&](int p, float x, float y) { GetCameraCurve(b.interp, p, c); return x + (y - x) * Curve(c, t); };
    out.target.x = lerp(0, a.target.x, b.target.x);
    out.target.y = lerp(1, a.target.y, b.target.y);
    out.target.z = lerp(2, a.target.z, b.target.z);
    out.rotation.x = lerp(3, a.rotation.x, b.rotation.x);
    out.rotation.y = lerp(3, a.rotation.y, b.rotation.y);
    out.rotation.z = lerp(3, a.rotation.z, b.rotation.z);
    out.distance = lerp(4, a.distance, b.distance);
    out.fovDeg = (uint32_t)std::lround(lerp(5, (float)a.fovDeg, (float)b.fovDeg));
    return out;
}

LightKf SampleLight(const std::vector<LightKf>& keys, float frame) {
    const int f = (int)std::floor(frame);
    if (f <= keys.front().frame || keys.size() == 1) { LightKf k = keys.front(); k.frame = f; return k; }
    if (f >= keys.back().frame) { LightKf k = keys.back(); k.frame = f; return k; }
    // last key at or before `frame`
    auto it = std::upper_bound(keys.begin(), keys.end(), f, [](int v, const LightKf& k) { return v < k.frame; });
    const size_t i = (size_t)(it - keys.begin()) - 1;
    const LightKf& a = keys[i];
    const LightKf& b = keys[i + 1];
    const float t = (frame - (float)a.frame) / (float)(b.frame - a.frame);
    LightKf out;
    out.frame = f;
    out.color.x = a.color.x + (b.color.x - a.color.x) * t;
    out.color.y = a.color.y + (b.color.y - a.color.y) * t;
    out.color.z = a.color.z + (b.color.z - a.color.z) * t;
    out.direction.x = a.direction.x + (b.direction.x - a.direction.x) * t;
    out.direction.y = a.direction.y + (b.direction.y - a.direction.y) * t;
    out.direction.z = a.direction.z + (b.direction.z - a.direction.z) * t;
    return out;
}

ShadowKf SampleShadow(const std::vector<ShadowKf>& keys, float frame) {
    const int f = (int)std::floor(frame);
    // no interpolation: the last key at or before `frame` holds until the next
    auto it = std::upper_bound(keys.begin(), keys.end(), f, [](int v, const ShadowKf& k) { return v < k.frame; });
    const ShadowKf& k = it != keys.begin() ? *(it - 1) : keys.front();
    ShadowKf out = k;
    out.frame = f;
    return out;
}

VmdMotion MotionData::ToVmd() const {
    VmdMotion v;
    v.modelName = modelName;
    for (const auto& [name, keys] : bones)
        for (const BoneKf& k : keys) {
            VmdBoneKey o;
            o.boneName = name; o.frame = (uint32_t)k.frame; o.translation = k.t; o.rotation = k.r;
            std::memcpy(o.interp, k.interp, 64);
            v.boneKeys.push_back(std::move(o));
        }
    std::stable_sort(v.boneKeys.begin(), v.boneKeys.end(),
                     [](const VmdBoneKey& a, const VmdBoneKey& b) { return a.frame < b.frame; });
    for (const auto& [name, keys] : morphs)
        for (const MorphKf& k : keys) v.morphKeys.push_back({name, (uint32_t)k.frame, k.weight});
    std::stable_sort(v.morphKeys.begin(), v.morphKeys.end(),
                     [](const VmdMorphKey& a, const VmdMorphKey& b) { return a.frame < b.frame; });
    for (const CameraKf& k : camera) {
        VmdCameraKey o;
        o.frame = (uint32_t)k.frame; o.distance = k.distance; o.target = k.target; o.rotation = k.rotation;
        std::memcpy(o.interp, k.interp, 24);
        o.fovDeg = k.fovDeg; o.perspective = k.perspective;
        v.cameraKeys.push_back(o);
    }
    for (const LightKf& k : light) v.lightKeys.push_back({(uint32_t)k.frame, k.color, k.direction});
    for (const ShadowKf& k : shadow) v.shadowKeys.push_back({(uint32_t)k.frame, k.mode, k.distance});

    // IK: one record per frame that has a key in any track, with the state of every track at that frame.
    std::set<int> frames;
    for (const auto& [n, keys] : ik) for (const IkKf& k : keys) frames.insert(k.frame);
    for (int f : frames) {
        VmdIkKey rec;
        rec.frame = (uint32_t)f;
        rec.visible = true;
        for (const auto& [name, keys] : ik) {
            bool state = true;
            for (const IkKf& k : keys) { if (k.frame > f) break; state = k.enabled; }
            rec.ikStates.emplace_back(name, state);
        }
        v.ikKeys.push_back(std::move(rec));
    }
    uint32_t maxFrame = 0;
    for (const auto& k : v.boneKeys) maxFrame = std::max(maxFrame, k.frame);
    for (const auto& k : v.morphKeys) maxFrame = std::max(maxFrame, k.frame);
    for (const auto& k : v.cameraKeys) maxFrame = std::max(maxFrame, k.frame);
    for (const auto& k : v.lightKeys) maxFrame = std::max(maxFrame, k.frame);
    for (const auto& k : v.shadowKeys) maxFrame = std::max(maxFrame, k.frame);
    for (const auto& k : v.ikKeys) maxFrame = std::max(maxFrame, k.frame);
    v.maxFrame = maxFrame;
    return v;
}

float ShadowRangeFromVmd(float vmd) {
    const float ui = std::clamp(ShadowUiFromVmd(vmd), 0.0f, 9999.0f);
    return std::clamp((10000.0f - ui) * (160.0f / 1125.0f), 20.0f, 2000.0f);
}

bool IsDefaultLightTrack(const std::vector<LightKf>& keys) {
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    for (const LightKf& k : keys)
        if (!near(k.color.x, 154.0f / 255.0f) || !near(k.color.y, 154.0f / 255.0f) || !near(k.color.z, 154.0f / 255.0f) ||
            !near(k.direction.x, -0.5f) || !near(k.direction.y, -1.0f) || !near(k.direction.z, 0.5f))
            return false;
    return true;
}

bool IsDefaultShadowTrack(const std::vector<ShadowKf>& keys) {
    for (const ShadowKf& k : keys)
        if (k.mode == 0 || std::fabs(ShadowUiFromVmd(k.distance) - 8875.0f) > 1.0f) return false;
    return true;
}

} // namespace mmdx::studio
