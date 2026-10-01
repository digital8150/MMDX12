#include "anim/Motion.h"
#include "anim/ModelInstance.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

using namespace DirectX;

namespace mmdx {

Bezier Bezier::FromBytes(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
    Bezier b;
    b.x1 = x1 / 127.0f; b.y1 = y1 / 127.0f;
    b.x2 = x2 / 127.0f; b.y2 = y2 / 127.0f;
    b.linear = (x1 == y1 && x2 == y2);
    return b;
}

float Bezier::Evaluate(float x) const {
    if (linear) return x;
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    // The x(t) curve is monotonic for control points in [0,1]: bisect for t, then return y(t).
    float lo = 0.0f, hi = 1.0f, t = x;
    for (int i = 0; i < 24; ++i) {
        t = 0.5f * (lo + hi);
        const float it = 1.0f - t;
        const float bx = 3.0f * it * it * t * x1 + 3.0f * it * t * t * x2 + t * t * t;
        if (bx < x) lo = t; else hi = t;
    }
    const float it = 1.0f - t;
    return 3.0f * it * it * t * y1 + 3.0f * it * t * t * y2 + t * t * t;
}

namespace {

// Name lookup that also matches VMD names truncated to 15 Shift-JIS bytes.
class NameIndex {
public:
    template <class GetName>
    NameIndex(size_t count, GetName getName, size_t vmdNameBytes) {
        for (size_t i = 0; i < count; ++i) {
            const std::string& name = getName(i);
            exact_.emplace(name, (int)i);
            std::string sjis = Utf8ToSjis(name);
            if (sjis.size() > vmdNameBytes) truncated_.emplace(SjisToUtf8(sjis.substr(0, vmdNameBytes)), (int)i);
        }
    }
    int Find(const std::string& name) const {
        if (auto it = exact_.find(name); it != exact_.end()) return it->second;
        if (auto it = truncated_.find(name); it != truncated_.end()) return it->second;
        return -1;
    }

private:
    std::unordered_map<std::string, int> exact_, truncated_;  // first occurrence wins
};

// Finds k such that keys[k].frame <= f < keys[k+1].frame. Assumes keys.size() >= 2 and
// keys.front().frame <= f < keys.back().frame.
template <class Key>
size_t FindSegment(const std::vector<Key>& keys, float f) {
    auto it = std::upper_bound(keys.begin(), keys.end(), f, [](float v, const Key& k) { return v < k.frame; });
    return (size_t)(it - keys.begin()) - 1;
}

template <class Key>
void SortAndDedupe(std::vector<Key>& keys) {
    // Stable so that for equal frames the later layer (appended last) wins.
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
    std::vector<Key> out;
    out.reserve(keys.size());
    for (auto& k : keys) {
        if (!out.empty() && out.back().frame == k.frame) out.back() = k;
        else out.push_back(k);
    }
    keys.swap(out);
}

}  // namespace

std::shared_ptr<BoundMotion> BoundMotion::Bind(const PmxModel& model, const std::vector<const VmdMotion*>& layers) {
    auto m = std::make_shared<BoundMotion>();
    NameIndex bones(model.bones.size(), [&](size_t i) -> const std::string& { return model.bones[i].name; }, 15);
    NameIndex morphs(model.morphs.size(), [&](size_t i) -> const std::string& { return model.morphs[i].name; }, 15);
    NameIndex ikBones(model.bones.size(), [&](size_t i) -> const std::string& { return model.bones[i].name; }, 20);

    std::unordered_map<int, size_t> boneTrack, morphTrack, ikTrack;
    int unboundBoneKeys = 0, unboundMorphKeys = 0;
    for (const VmdMotion* vmd : layers) {
        if (!vmd) continue;
        for (const VmdBoneKey& k : vmd->boneKeys) {
            const int bone = bones.Find(k.boneName);
            if (bone < 0) { ++unboundBoneKeys; continue; }
            auto [it, inserted] = boneTrack.emplace(bone, m->boneTracks_.size());
            if (inserted) m->boneTracks_.push_back({bone, {}});
            BoneKey key;
            key.frame = (float)k.frame;
            key.t = k.translation;
            key.r = k.rotation;
            const uint8_t* ip = k.interp;
            key.bx = Bezier::FromBytes(ip[0], ip[4], ip[8], ip[12]);
            key.by = Bezier::FromBytes(ip[1], ip[5], ip[9], ip[13]);
            key.bz = Bezier::FromBytes(ip[2], ip[6], ip[10], ip[14]);
            key.br = Bezier::FromBytes(ip[3], ip[7], ip[11], ip[15]);
            m->boneTracks_[it->second].keys.push_back(key);
        }
        for (const VmdMorphKey& k : vmd->morphKeys) {
            const int morph = morphs.Find(k.morphName);
            if (morph < 0) { ++unboundMorphKeys; continue; }
            auto [it, inserted] = morphTrack.emplace(morph, m->morphTracks_.size());
            if (inserted) m->morphTracks_.push_back({morph, {}});
            m->morphTracks_[it->second].keys.push_back({(float)k.frame, k.weight});
        }
        for (const VmdIkKey& k : vmd->ikKeys) {
            for (const auto& [name, enabled] : k.ikStates) {
                const int bone = ikBones.Find(name);
                if (bone < 0 || !(model.bones[bone].flags & PmxBone_IK)) continue;
                auto [it, inserted] = ikTrack.emplace(bone, m->ikTracks_.size());
                if (inserted) m->ikTracks_.push_back({bone, {}});
                m->ikTracks_[it->second].keys.push_back({(float)k.frame, enabled});
            }
        }
    }

    float endFrame = 0;
    for (auto& t : m->boneTracks_) { SortAndDedupe(t.keys); endFrame = std::max(endFrame, t.keys.back().frame); }
    for (auto& t : m->morphTracks_) { SortAndDedupe(t.keys); endFrame = std::max(endFrame, t.keys.back().frame); }
    for (auto& t : m->ikTracks_) SortAndDedupe(t.keys);
    m->endFrame_ = endFrame;
    if (unboundBoneKeys || unboundMorphKeys)
        LOG_INFO("motion bind: %d bone keys and %d morph keys have no matching bone/morph in '%s'",
                 unboundBoneKeys, unboundMorphKeys, model.name.c_str());
    return m;
}

void BoundMotion::Evaluate(float frame, ModelInstance& inst) const {
    inst.ResetPose();
    for (const BoneTrack& track : boneTracks_) {
        const auto& keys = track.keys;
        if (frame <= keys.front().frame || keys.size() == 1) {
            inst.SetBoneAnim(track.bone, keys.front().t, keys.front().r);
            continue;
        }
        if (frame >= keys.back().frame) {
            inst.SetBoneAnim(track.bone, keys.back().t, keys.back().r);
            continue;
        }
        const size_t i = FindSegment(keys, frame);
        const BoneKey& a = keys[i];
        const BoneKey& b = keys[i + 1];
        const float t = (frame - a.frame) / (b.frame - a.frame);
        XMFLOAT3 pos;
        pos.x = a.t.x + (b.t.x - a.t.x) * b.bx.Evaluate(t);
        pos.y = a.t.y + (b.t.y - a.t.y) * b.by.Evaluate(t);
        pos.z = a.t.z + (b.t.z - a.t.z) * b.bz.Evaluate(t);
        XMFLOAT4 rot;
        XMStoreFloat4(&rot, XMQuaternionSlerp(XMLoadFloat4(&a.r), XMLoadFloat4(&b.r), b.br.Evaluate(t)));
        inst.SetBoneAnim(track.bone, pos, rot);
    }
    for (const MorphTrack& track : morphTracks_) {
        const auto& keys = track.keys;
        float w;
        if (frame <= keys.front().frame || keys.size() == 1) w = keys.front().weight;
        else if (frame >= keys.back().frame) w = keys.back().weight;
        else {
            const size_t i = FindSegment(keys, frame);
            const float t = (frame - keys[i].frame) / (keys[i + 1].frame - keys[i].frame);
            w = keys[i].weight + (keys[i + 1].weight - keys[i].weight) * t;
        }
        inst.SetMorphWeight(track.morph, w);
    }
    for (const IkTrack& track : ikTracks_) {
        const auto& keys = track.keys;
        bool enabled = keys.front().enabled;
        if (frame > keys.front().frame && keys.size() > 1) {
            enabled = frame >= keys.back().frame ? keys.back().enabled : keys[FindSegment(keys, frame)].enabled;
        }
        inst.SetIkEnabled(track.bone, enabled);
    }
}

std::shared_ptr<CameraMotion> CameraMotion::Create(const VmdMotion& vmd) {
    if (vmd.cameraKeys.empty()) return nullptr;
    auto c = std::make_shared<CameraMotion>();
    c->keys_.reserve(vmd.cameraKeys.size());
    for (const VmdCameraKey& k : vmd.cameraKeys) {
        Key key;
        key.frame = (float)k.frame;
        key.pose.target = k.target;
        key.pose.rotation = k.rotation;
        key.pose.distance = k.distance;
        key.pose.fovDeg = (float)k.fovDeg;
        const uint8_t* ip = k.interp;  // per parameter: x1, x2, y1, y2
        auto bz = [&](int p) { return Bezier::FromBytes(ip[p * 4 + 0], ip[p * 4 + 2], ip[p * 4 + 1], ip[p * 4 + 3]); };
        key.bx = bz(0); key.by = bz(1); key.bz = bz(2); key.br = bz(3); key.bd = bz(4); key.bf = bz(5);
        c->keys_.push_back(key);
    }
    SortAndDedupe(c->keys_);
    c->endFrame_ = c->keys_.back().frame;
    return c;
}

CameraPose CameraMotion::Evaluate(float frame) const {
    if (frame <= keys_.front().frame || keys_.size() == 1) return keys_.front().pose;
    if (frame >= keys_.back().frame) return keys_.back().pose;
    const size_t i = FindSegment(keys_, frame);
    const Key& a = keys_[i];
    const Key& b = keys_[i + 1];
    // Keys on adjacent frames are a camera cut: never blend across them.
    if (b.frame - a.frame <= 1.0f) return a.pose;
    const float t = (frame - a.frame) / (b.frame - a.frame);
    auto lerp = [](float x, float y, float s) { return x + (y - x) * s; };
    CameraPose p;
    p.target.x = lerp(a.pose.target.x, b.pose.target.x, b.bx.Evaluate(t));
    p.target.y = lerp(a.pose.target.y, b.pose.target.y, b.by.Evaluate(t));
    p.target.z = lerp(a.pose.target.z, b.pose.target.z, b.bz.Evaluate(t));
    const float tr = b.br.Evaluate(t);
    p.rotation.x = lerp(a.pose.rotation.x, b.pose.rotation.x, tr);
    p.rotation.y = lerp(a.pose.rotation.y, b.pose.rotation.y, tr);
    p.rotation.z = lerp(a.pose.rotation.z, b.pose.rotation.z, tr);
    p.distance = lerp(a.pose.distance, b.pose.distance, b.bd.Evaluate(t));
    p.fovDeg = lerp(a.pose.fovDeg, b.pose.fovDeg, b.bf.Evaluate(t));
    return p;
}

void CameraMotion::ToView(const CameraPose& pose, XMFLOAT4X4* view, XMFLOAT3* eye) {
    // MMD camera: eye = target + R * (0, 0, distance), R = yaw/pitch/roll of the negated angles
    // (same as babylon-mmd's MmdCamera; both MMD and D3D are left-handed).
    const XMMATRIX r = XMMatrixRotationRollPitchYaw(-pose.rotation.x, -pose.rotation.y, -pose.rotation.z);
    const XMVECTOR target = XMLoadFloat3(&pose.target);
    const XMVECTOR eyePos = XMVectorAdd(XMVector3TransformCoord(XMVectorSet(0, 0, pose.distance, 0), r), target);
    const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), r);
    XMVECTOR at = target;
    if (std::fabs(pose.distance) < 1e-4f)  // degenerate: look along the camera's forward axis
        at = XMVectorAdd(eyePos, XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), r));
    if (view) XMStoreFloat4x4(view, XMMatrixLookAtLH(eyePos, at, up));
    if (eye) XMStoreFloat3(eye, eyePos);
}

}  // namespace mmdx
