#include "studio/StudioFocus.h"

#include <algorithm>
#include <cmath>

namespace mmdx::studio {

using namespace DirectX;

namespace {

XMFLOAT3 ToView(const XMFLOAT3& p, const FocusView& v) {
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3TransformCoord(XMLoadFloat3(&p), XMLoadFloat4x4(&v.view)));
    return out;
}

float Smoothstep(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

float Inv(float z) { return z > 0.0f ? 1.0f / z : 0.0f; }

// 1 / z blend (the blur radius is linear in 1 / z); a missing side (<= 0) takes the other.
float BlendFocus(float za, float zb, float t) {
    if (za <= 0.0f) return zb;
    if (zb <= 0.0f) return za;
    const float inv = Inv(za) + (Inv(zb) - Inv(za)) * t;
    return inv > 0.0f ? 1.0f / inv : zb;
}

} // namespace

float FocusViewZ(const XMFLOAT3& p, const FocusView& v) { return ToView(p, v).z; }

float FocusScore(const FocusSubject& s, const FocusView& v) {
    // the body as a line from the feet (as far below the centre bone as the head is above it) to the top of the head
    const XMVECTOR head = XMLoadFloat3(&s.head), center = XMLoadFloat3(&s.center);
    const XMVECTOR up = XMVectorSubtract(head, center);
    const XMVECTOR feet = XMVectorSubtract(center, up);
    const XMVECTOR top = XMVectorAdd(head, XMVectorScale(up, 0.15f));
    const float height = XMVectorGetX(XMVector3Length(XMVectorSubtract(top, feet)));
    if (height <= 1e-3f) return 0.0f;
    const float sx = 1.0f / (v.tanHalfFovY * v.aspect), sy = 1.0f / v.tanHalfFovY;
    constexpr int kSamples = 7;
    int inside = 0;
    for (int i = 0; i < kSamples; ++i) {
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVectorLerp(feet, top, i / (float)(kSamples - 1)));
        const XMFLOAT3 q = ToView(p, v);
        if (q.z <= v.nearZ) continue;
        const float x = q.x * sx / q.z, y = q.y * sy / q.z;
        if (std::fabs(x) <= 1.0f && std::fabs(y) <= 1.0f) ++inside;
    }
    if (inside == 0) return 0.0f;
    XMFLOAT3 mid;
    XMStoreFloat3(&mid, XMVectorLerp(feet, top, 0.5f));
    const float zMid = std::max(v.nearZ, ToView(mid, v).z);
    const float size = std::min(height / (zMid * 2.0f * v.tanHalfFovY), 1.5f);  // share of the frame height
    // centrality of the head (the centre bone when the head is behind the camera)
    XMFLOAT3 h = ToView(s.head, v);
    if (h.z <= v.nearZ) h = ToView(s.center, v);
    float cent = 0.0f;
    if (h.z > v.nearZ) {
        const float x = std::clamp(h.x * sx / h.z, -2.0f, 2.0f), y = std::clamp(h.y * sy / h.z, -2.0f, 2.0f);
        cent = std::exp(-(x * x + 0.5f * y * y) / 0.6f);
    }
    return std::pow(size, 1.5f) * (inside / (float)kSamples) * (0.3f + 0.7f * cent);
}

float AutoFocus::Update(double time, const std::vector<FocusSubject>& subjects, const FocusView& v) {
    const XMFLOAT3 fwd{v.view._13, v.view._23, v.view._33};
    const double dt = time - lastTime_;
    const float moved = XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&v.eye), XMLoadFloat3(&lastEye_))));
    const float turned = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&fwd), XMLoadFloat3(&lastFwd_)));
    const float shownZ = shownInv_ > 0.0f ? 1.0f / shownInv_ : 0.0f;
    // a time jump, or the camera jumping within one step (a VMD camera cut): no history, no rack
    const bool cut = dt < 0.0 || dt > 0.25 || (dt > 0.0 && (moved > std::max(3.0f, 0.3f * shownZ) || turned < 0.94f));
    lastTime_ = time;
    lastEye_ = v.eye;
    lastFwd_ = fwd;

    float best = 0.0f, current = 0.0f;
    const FocusSubject* bestS = nullptr;
    const FocusSubject* currentS = nullptr;
    for (const FocusSubject& s : subjects) {
        const float score = FocusScore(s, v);
        if (score > best) { best = score; bestS = &s; }
        if (s.uid == uid_) { current = score; currentS = &s; }
    }
    const FocusSubject* pick = bestS;
    if (!cut && currentS && current > 0.0f && current >= kKeepRatio * best) pick = currentS;
    if (!pick) {
        uid_ = 0;
        shownInv_ = 0.0f;
        switchTime_ = -1e9;
        return 0.0f;
    }
    const float targetZ = FocusViewZ(pick->head, v);
    const float targetInv = Inv(std::max(targetZ, v.nearZ));
    if (cut || uid_ == 0 || shownInv_ <= 0.0f) {
        switchTime_ = -1e9;  // snap
    } else if (pick->uid != uid_) {
        fromInv_ = shownInv_;  // rack from what is shown now
        switchTime_ = time;
    }
    uid_ = pick->uid;
    const float u = Smoothstep((float)((time - switchTime_) / kRackSeconds));
    shownInv_ = fromInv_ + (targetInv - fromInv_) * u;
    if (u >= 1.0f) fromInv_ = targetInv;
    return shownInv_ > 0.0f ? 1.0f / shownInv_ : 0.0f;
}

FocusResult EvaluateFocus(const std::vector<FocusKf>& keys, float frame,
                          const std::function<bool(uint32_t, FocusBone, float&)>& targetZ, float autoZ) {
    FocusResult r;
    r.distance = autoZ;
    if (keys.empty()) return r;
    // the segment: the last key at or before `frame` (the first key before it)
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](float f, const FocusKf& k) { return f < (float)k.frame; });
    const size_t k = it == keys.begin() ? 0 : (size_t)(it - keys.begin()) - 1;
    const auto segment = [&](size_t i) -> float {
        const FocusKf& key = keys[i];
        switch (key.mode) {
        case FocusMode::Target: {
            float z = 0.0f;
            if (key.target != 0 && targetZ && targetZ(key.target, key.bone, z) && z > 0.0f) return z;
            return autoZ;
        }
        case FocusMode::Manual:
            if (i + 1 < keys.size() && keys[i + 1].mode == FocusMode::Manual && keys[i + 1].frame > key.frame) {
                const float t = std::clamp((frame - key.frame) / (float)(keys[i + 1].frame - key.frame), 0.0f, 1.0f);
                return key.distance + (keys[i + 1].distance - key.distance) * t;
            }
            return key.distance;
        default:
            return autoZ;
        }
    };
    r.distance = segment(k);
    const FocusKf& key = keys[k];
    if (k > 0 && key.transition > 0 && frame >= (float)key.frame && frame < (float)(key.frame + key.transition) &&
        !(key.mode == FocusMode::Manual && keys[k - 1].mode == FocusMode::Manual)) {
        const float u = Smoothstep((frame - key.frame) / (float)key.transition);
        r.distance = BlendFocus(segment(k - 1), r.distance, u);
    }
    // aperture: linear between keys (held before the first / after the last)
    if (frame <= (float)keys.front().frame || k + 1 >= keys.size()) {
        r.aperture = key.aperture;
    } else {
        const FocusKf& next = keys[k + 1];
        const float t = std::clamp((frame - key.frame) / (float)std::max(1, next.frame - key.frame), 0.0f, 1.0f);
        r.aperture = key.aperture + (next.aperture - key.aperture) * t;
    }
    r.aperture = std::clamp(r.aperture, 0.0f, 3.0f);
    return r;
}

} // namespace mmdx::studio
