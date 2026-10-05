#include "studio/StudioMotion.h"
#include <algorithm>
#include <cstring>
#include <set>

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
    return end;
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
    d.shadow = vmd.shadowKeys;
    std::sort(d.shadow.begin(), d.shadow.end(), [](const VmdShadowKey& a, const VmdShadowKey& b) { return a.frame < b.frame; });
    return d;
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
    v.shadowKeys = shadow;

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
    for (const auto& k : v.ikKeys) maxFrame = std::max(maxFrame, k.frame);
    v.maxFrame = maxFrame;
    return v;
}

} // namespace mmdx::studio
