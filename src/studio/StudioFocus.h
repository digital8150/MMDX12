#pragma once
// Depth-of-field focus for the Studio: the focus track (FocusKf, StudioMotion.h) and the automatic focus that picks
// the character a shot is about. Pure math (no App / renderer state), shared by the viewport and the offline renders.
#include "studio/StudioMotion.h"
#include <DirectXMath.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace mmdx::studio {

// A character the focus can land on: world positions at the frame.
struct FocusSubject {
    uint32_t uid = 0;
    DirectX::XMFLOAT3 head{}, upperBody{}, center{};
};

// The camera the focus is computed for.
struct FocusView {
    DirectX::XMFLOAT4X4 view{};      // LH view matrix, row vectors
    DirectX::XMFLOAT3 eye{};
    float tanHalfFovY = 0.268f;
    float aspect = 16.0f / 9.0f;     // the render frame
    float nearZ = 0.5f;
};

float FocusViewZ(const DirectX::XMFLOAT3& p, const FocusView& v);  // view-space z of a world point
// How strongly `s` reads as the subject of the shot: on-screen height^1.5 x the visible share of its body x closeness of
// its head to the frame centre. 0 when it is behind the camera or entirely out of frame.
float FocusScore(const FocusSubject& s, const FocusView& v);

// The automatic focus, with memory: it keeps its subject while that one scores at least kKeepRatio of the best (no
// flicker between similar candidates), racks the focus over kRackSeconds when the subject changes, and snaps on camera
// cuts and time jumps (seek, scrubbing backwards). Calling it again at the same time changes nothing, so the viewport
// and a render of the same frame agree. A video rendered from a later start may pick differently until the first cut.
class AutoFocus {
public:
    static constexpr float kRackSeconds = 0.35f;
    static constexpr float kKeepRatio = 0.7f;
    // The focus distance (view z) at `time`; 0 when no subject is in front of the camera.
    float Update(double time, const std::vector<FocusSubject>& subjects, const FocusView& v);
    void Reset() { *this = AutoFocus{}; }
    uint32_t Subject() const { return uid_; }  // 0 = none

private:
    uint32_t uid_ = 0;
    float fromInv_ = 0.0f, shownInv_ = 0.0f;   // 1 / distance: rack start and the last value returned
    double switchTime_ = -1e9, lastTime_ = -1e9;
    DirectX::XMFLOAT3 lastEye_{}, lastFwd_{0, 0, 1};
};

struct FocusResult {
    float distance = 0.0f;  // view z; <= 0: none (the real-time pass autofocuses the screen centre, GI is a pinhole)
    float aperture = 1.0f;  // x the render aperture
};
// The focus track at `frame` (fractional). `targetZ(uid, bone, z)` gives a target bone's view z (false: the model is
// gone or the bone is behind the camera -> the auto focus); `autoZ` is the automatic focus at this frame.
// No keys: {autoZ, 1}. Segment k (keys[k].frame <= frame < keys[k + 1].frame; before the first key, the first key's)
// focuses on: Auto -> autoZ, Target -> the target bone, Manual -> its distance, linear toward a directly following
// Manual key. keys[k].transition > 0 blends from the previous segment's focus over that many frames (smoothstep in
// 1 / z, which is how the blur changes), except Manual -> Manual, which is already interpolated. The aperture is linear
// between keys.
FocusResult EvaluateFocus(const std::vector<FocusKf>& keys, float frame,
                          const std::function<bool(uint32_t, FocusBone, float&)>& targetZ, float autoZ);

} // namespace mmdx::studio
