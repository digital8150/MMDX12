#pragma once
// Editable motion data for the Studio: keyframes keyed by bone / morph / IK name (so the same data
// round-trips through VMD files and can be bound to any model), sorted by frame, one key per frame.
// Evaluation stays with BoundMotion: the studio converts to VmdMotion (ToVmd) and binds.
#include "asset/VmdMotion.h"
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mmdx::studio {

struct BoneKf {
    int frame = 0;
    DirectX::XMFLOAT3 t{};
    DirectX::XMFLOAT4 r{0, 0, 0, 1};
    uint8_t interp[64] = {};  // VMD layout, see asset/VmdMotion.h
};
struct MorphKf { int frame = 0; float weight = 0; };
struct IkKf { int frame = 0; bool enabled = true; };
struct CameraKf {
    int frame = 0;
    float distance = -45.0f;
    DirectX::XMFLOAT3 target{0, 10, 0};
    DirectX::XMFLOAT3 rotation{};
    uint8_t interp[24] = {};
    uint32_t fovDeg = 30;
    bool perspective = true;
};
struct LightKf { int frame = 0; DirectX::XMFLOAT3 color{0.6f, 0.6f, 0.6f}; DirectX::XMFLOAT3 direction{-0.5f, -1.0f, 0.5f}; };

// Bone interpolation block (64 bytes). The true table T is 16 bytes: x1 of channels X,Y,Z,R, then y1, x2, y2 (4 each).
// MMD stores T in row 0 with bytes 2 and 3 overwritten by physics flags (0 = physics on), and rows 1..3 as T shifted
// left by 1..3 bytes (zero padded), so Z/R x1 live at bytes 17/18. Channel c: 0 X, 1 Y, 2 Z, 3 rotation.
// Curves are {x1, y1, x2, y2}, 0..127.
void GetBoneCurve(const uint8_t interp[64], int channel, uint8_t out[4]);
void SetBoneCurve(uint8_t interp[64], int channel, const uint8_t c[4]);  // rewrites all rows, keeps the physics flags
void FillLinearInterp(uint8_t interp[64]);
// Camera block (24 bytes): parameter p (0 X,1 Y,2 Z,3 rotation,4 distance,5 fov) stores x1,x2,y1,y2 at p*4.
void GetCameraCurve(const uint8_t interp[24], int param, uint8_t out[4]);
void SetCameraCurve(uint8_t interp[24], int param, const uint8_t c[4]);
void FillLinearCameraInterp(uint8_t interp[24]);

// Sorted-by-frame key container operations (K needs a `frame` member).
template <class K> K* FindKey(std::vector<K>& keys, int frame) {
    for (K& k : keys) if (k.frame == frame) return &k;
    return nullptr;
}
template <class K> const K* FindKey(const std::vector<K>& keys, int frame) {
    for (const K& k : keys) if (k.frame == frame) return &k;
    return nullptr;
}
// Inserts `key` at its frame, replacing an existing key of the same frame. Returns the replaced key if any.
template <class K> bool UpsertKey(std::vector<K>& keys, const K& key, K* replaced = nullptr) {
    auto it = std::lower_bound(keys.begin(), keys.end(), key.frame, [](const K& k, int f) { return k.frame < f; });
    if (it != keys.end() && it->frame == key.frame) {
        if (replaced) *replaced = *it;
        *it = key;
        return true;
    }
    keys.insert(it, key);
    return false;
}
template <class K> bool EraseKey(std::vector<K>& keys, int frame, K* erased = nullptr) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].frame != frame) continue;
        if (erased) *erased = keys[i];
        keys.erase(keys.begin() + (ptrdiff_t)i);
        return true;
    }
    return false;
}

struct MotionData {
    std::string modelName;  // VMD header model name
    std::map<std::string, std::vector<BoneKf>> bones;
    std::map<std::string, std::vector<MorphKf>> morphs;
    std::map<std::string, std::vector<IkKf>> ik;  // IK bone name -> enable keys
    std::vector<CameraKf> camera;                 // camera tracks live in the project's camera, not in model motions
    std::vector<LightKf> light;
    std::vector<VmdShadowKey> shadow;

    bool Empty() const { return bones.empty() && morphs.empty() && ik.empty() && camera.empty() && light.empty(); }
    int EndFrame() const;  // last key frame over everything (0 when empty)

    static MotionData FromVmd(const VmdMotion& vmd);
    // Adds `other`'s keys (same frame: other wins). Used to merge a dance with its facial VMDs.
    void Merge(const MotionData& other);
    // Renames tracks whose VMD name is a 15/20-byte truncation of a model bone/morph name to the full name,
    // so the timeline can find them by model name. Tracks without a match keep their name (and stay exported).
    void CanonicalizeNames(const PmxModel& model);
    // Sorted by frame then name. IK keys are regrouped: every frame that has an IK key in any track writes
    // one VMD IK record listing the state of ALL IK tracks at that frame (last key at or before it; enabled before the first).
    VmdMotion ToVmd() const;
};

// Interpolated values between keys, exactly as BoundMotion::Evaluate computes them (before the first key: the first
// key, after the last: the last). The returned key's interpolation block is the next key's (or linear past the end).
// `keys` must not be empty.
BoneKf SampleBone(const std::vector<BoneKf>& keys, int frame);
float SampleMorph(const std::vector<MorphKf>& keys, int frame);
CameraKf SampleCamera(const std::vector<CameraKf>& keys, int frame);

} // namespace mmdx::studio
