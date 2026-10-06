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
#include <set>
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
// Self-shadow key: mode 0 off, 1 mode1, 2 mode2; distance as stored in VMD (0.1 - MMD's 0..9999 UI value * 1e-5,
// MMD default 8875 -> 0.01125).
struct ShadowKf { int frame = 0; uint8_t mode = 1; float distance = 0.01125f; };

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
    auto it = std::lower_bound(keys.begin(), keys.end(), frame, [](const K& k, int f) { return k.frame < f; });
    return it != keys.end() && it->frame == frame ? &*it : nullptr;
}
template <class K> const K* FindKey(const std::vector<K>& keys, int frame) {
    auto it = std::lower_bound(keys.begin(), keys.end(), frame, [](const K& k, int f) { return k.frame < f; });
    return it != keys.end() && it->frame == frame ? &*it : nullptr;
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
    auto it = std::lower_bound(keys.begin(), keys.end(), frame, [](const K& k, int f) { return k.frame < f; });
    if (it == keys.end() || it->frame != frame) return false;
    if (erased) *erased = *it;
    keys.erase(it);
    return true;
}

// Moves the keys whose frame is in `frames` by `delta` (results clamped to >= 0). A moved key replaces an unmoved key on
// its target frame; if several moved keys land on one frame (only possible through the clamp), the one that came last
// (highest original frame) wins. O(n log n): safe for tracks with many thousands of keys.
template <class K> void MoveKeyFrames(std::vector<K>& keys, const std::set<int>& frames, int delta) {
    if (frames.empty() || delta == 0) return;
    std::vector<K> moved, kept;
    moved.reserve(frames.size());
    kept.reserve(keys.size());
    for (const K& k : keys) (frames.count(k.frame) ? moved : kept).push_back(k);
    if (moved.empty()) return;
    for (K& k : moved) k.frame = std::max(0, k.frame + delta);
    // `moved` is still in original-frame order; after the clamp equal frames are adjacent: keep the last of each run.
    std::vector<K> uniq;
    uniq.reserve(moved.size());
    for (size_t i = 0; i < moved.size(); ++i)
        if (i + 1 == moved.size() || moved[i + 1].frame != moved[i].frame) uniq.push_back(moved[i]);
    // unmoved keys on a target frame are replaced
    size_t j = 0;
    std::vector<K> out;
    out.reserve(kept.size() + uniq.size());
    for (const K& k : kept) {
        while (j < uniq.size() && uniq[j].frame < k.frame) out.push_back(uniq[j++]);
        if (j < uniq.size() && uniq[j].frame == k.frame) continue;  // replaced by the moved key
        out.push_back(k);
    }
    while (j < uniq.size()) out.push_back(uniq[j++]);
    keys.swap(out);
}
// Erases the keys whose frame is in `frames`.
template <class K> void EraseKeyFrames(std::vector<K>& keys, const std::set<int>& frames) {
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&frames](const K& k) { return frames.count(k.frame) != 0; }),
               keys.end());
}
// MMD "frame insert": every key at frame >= `at` moves `count` frames later. Returns true if any key moved.
template <class K> bool InsertFrameSpan(std::vector<K>& keys, int at, int count) {
    if (count <= 0 || at < 0) return false;
    auto it = std::lower_bound(keys.begin(), keys.end(), at, [](const K& k, int f) { return k.frame < f; });
    if (it == keys.end()) return false;
    for (; it != keys.end(); ++it) it->frame += count;
    return true;
}
// MMD "frame delete": keys in [at, at + count) are erased, keys at >= at + count move `count` frames earlier.
// Returns true if anything changed.
template <class K> bool DeleteFrameSpan(std::vector<K>& keys, int at, int count) {
    if (count <= 0 || at < 0) return false;
    auto first = std::lower_bound(keys.begin(), keys.end(), at, [](const K& k, int f) { return k.frame < f; });
    auto last = std::lower_bound(keys.begin(), keys.end(), at + count, [](const K& k, int f) { return k.frame < f; });
    bool erased = first != last;
    bool shifted = last != keys.end();
    if (!erased && !shifted) return false;
    auto tail = keys.erase(first, last);  // keys after the span move `count` frames earlier
    for (; tail != keys.end(); ++tail) tail->frame -= count;
    return true;
}

struct MotionData {
    std::string modelName;  // VMD header model name
    std::map<std::string, std::vector<BoneKf>> bones;
    std::map<std::string, std::vector<MorphKf>> morphs;
    std::map<std::string, std::vector<IkKf>> ik;  // IK bone name -> enable keys
    std::vector<CameraKf> camera;                 // camera tracks live in the project's camera, not in model motions
    std::vector<LightKf> light;
    std::vector<ShadowKf> shadow;

    bool Empty() const { return bones.empty() && morphs.empty() && ik.empty() && camera.empty() && light.empty() && shadow.empty(); }
    int EndFrame() const;  // last key frame over everything (0 when empty)

    // Frame insert/delete over every track (bones, morphs, IK, camera, light, shadow); see InsertFrameSpan/DeleteFrameSpan.
    // Tracks that become empty are removed from the maps. Return true if anything changed.
    bool InsertFrames(int at, int count);
    bool DeleteFrames(int at, int count);
    size_t ApproxBytes() const;  // heap memory estimate (undo budget)

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
// Light: linear interpolation of colour and direction between keys (MMD interpolates lights linearly; the direction
// is lerped component-wise, not normalised). Before the first key: the first; after the last: the last.
// `frame` may be fractional (smooth playback). The returned key's frame is (int)floor(frame). `keys` non-empty.
LightKf SampleLight(const std::vector<LightKf>& keys, float frame);
// Self-shadow: no interpolation (MMD holds each key until the next): the last key at or before `frame`, the first key
// before it. The returned key's frame is (int)floor(frame). `keys` non-empty.
ShadowKf SampleShadow(const std::vector<ShadowKf>& keys, float frame);
// MMD's self-shadow distance UI value (0..9999) <-> the VMD value.
inline float ShadowUiFromVmd(float d) { return (0.1f - d) * 100000.0f; }
// The VMD self-shadow distance -> the renderer's cascade range in MMD units. MMD's default 8875 maps to the
// renderer's default range (160); a lower UI value covers more of the scene, as in MMD.
float ShadowRangeFromVmd(float vmd);
// A camera VMD's light / self-shadow keys that only repeat MMD's defaults (light colour 154/255, direction
// (-0.5, -1, 0.5); shadow mode 1 at distance 8875): most camera files carry them, and they say nothing.
bool IsDefaultLightTrack(const std::vector<LightKf>& keys);
bool IsDefaultShadowTrack(const std::vector<ShadowKf>& keys);
inline float ShadowVmdFromUi(float ui) { return 0.1f - ui * 0.00001f; }

} // namespace mmdx::studio
