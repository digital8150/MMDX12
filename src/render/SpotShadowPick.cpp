#include "render/SpotShadowPick.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace mmdx {

namespace {

constexpr float kSpotPickHysteresis = 1.5f;
constexpr float kOffscreenCasterWeight = 0.15f;

// CPU copy of PunctualFalloff (shaders/common.hlsli).
float Falloff(float dist, float range, LightFalloffType type) {
    const float d = dist / std::max(range, 0.01f);
    if (d >= 1.0f) return 0.0f;
    const float d2 = d * d;
    const float x = std::clamp(1.0f - d2 * d2, 0.0f, 1.0f);
    if (type == LightFalloffType::InverseSquare) {
        const float r = 25.0f / std::max(dist, 12.5f);
        return x * x * r * r;
    }
    if (type == LightFalloffType::Linear) return std::clamp(1.0f - d, 0.0f, 1.0f);
    return x * x / (1.0f + dist * dist * 0.0004f);
}

XMFLOAT3 Normalized(const XMFLOAT3& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return {0.0f, -1.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}

}  // namespace

bool LightReachesSphere(const PunctualLight& light, const XMFLOAT3& center, float radius) {
    const float vx = center.x - light.position.x, vy = center.y - light.position.y, vz = center.z - light.position.z;
    const float dist = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (dist - radius > std::max(light.range, 1.0f)) return false;
    if (light.spotCosOuter <= -1.0f || dist <= radius) return true;
    // the shadow frustum's half angle (Renderer::FillSceneConstants), capped at its 85 degrees
    const float half = std::min(std::acos(std::clamp(light.spotCosOuter, -0.99f, 1.0f)) * 1.08f + 0.01f, 1.4835f);
    const XMFLOAT3 axis = Normalized(light.direction);
    const float cosTheta = std::clamp((vx * axis.x + vy * axis.y + vz * axis.z) / dist, -1.0f, 1.0f);
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    const float cosHalf = std::cos(half), sinHalf = std::sin(half);
    if (cosTheta >= cosHalf) return true;                                   // the centre is inside the cone
    if (cosTheta * cosHalf + sinTheta * sinHalf <= 0.0f) return false;      // beyond the apex: only dist <= radius touches
    return dist * (sinTheta * cosHalf - cosTheta * sinHalf) <= radius;      // distance to the cone's surface
}

float CasterViewWeight(const CameraParams& camera, float aspect, const XMFLOAT3& center, float radius) {
    const XMVECTOR c = XMVector3TransformCoord(XMLoadFloat3(&center), XMLoadFloat4x4(&camera.view));
    const float x = XMVectorGetX(c), y = XMVectorGetY(c), z = XMVectorGetZ(c);
    const float tanY = std::tan(camera.fovYRadians * 0.5f), tanX = tanY * aspect;
    const bool visible = z + radius > 0.0f && z - radius < camera.farZ &&
                         std::fabs(x) - z * tanX <= radius * std::sqrt(1.0f + tanX * tanX) &&
                         std::fabs(y) - z * tanY <= radius * std::sqrt(1.0f + tanY * tanY);
    if (!visible) return kOffscreenCasterWeight;
    const float share = z > 1e-3f ? std::min(radius / (z * std::max(tanY, 1e-3f)), 1.0f) : 1.0f;
    return 1.0f + 2.0f * share;
}

float SpotShadowScore(const PunctualLight& light, const std::vector<SpotCaster>& casters, bool volumetricOn) {
    if (light.spotCosOuter <= -1.0f || light.shadow == LightShadowType::NoCast || light.shadowDensity <= 0.0f) return 0.0f;
    // the map is read by the surface terms this light has and by its haze beam
    if (!light.affectDiffuse && !light.affectSpecular && !(volumetricOn && light.volumetric > 0.0f)) return 0.0f;
    // A saturated light throws as visible a shadow as a white one of less luminance: a blue spot's luminance alone
    // (0.07) ranked it below everything else on the stage.
    const float lum = 0.2126f * light.color.x + 0.7152f * light.color.y + 0.0722f * light.color.z;
    const float peak = std::max({light.color.x, light.color.y, light.color.z});
    const float power = light.intensity * 0.5f * (lum + peak) * std::min(light.shadowDensity, 1.0f);
    if (power <= 0.0f) return 0.0f;
    const XMFLOAT3 axis = Normalized(light.direction);
    const float cosOuter = light.spotCosOuter;
    const float cosInner = std::max(light.spotCosInner, cosOuter + 1e-3f);
    float score = 0.0f;
    for (const SpotCaster& c : casters) {
        if (!LightReachesSphere(light, c.center, c.radius)) continue;
        const float vx = c.center.x - light.position.x, vy = c.center.y - light.position.y, vz = c.center.z - light.position.z;
        const float dist = std::sqrt(vx * vx + vy * vy + vz * vz);
        const float cosTheta = dist > 1e-4f ? (vx * axis.x + vy * axis.y + vz * axis.z) / dist : 1.0f;
        // a caster grazed by the cone's rim still counts: only part of it is lit
        const float inCone = 0.25f + 0.75f * std::clamp((cosTheta - cosOuter) / (cosInner - cosOuter), 0.0f, 1.0f);
        // at the caster's nearest point, so a large caster at the end of the range never scores 0
        const float falloff = Falloff(std::max(dist - c.radius, 0.0f), light.range, light.falloff);
        score += power * falloff * inCone * c.viewWeight;
    }
    return score;
}

std::vector<int> PickSpotSlices(const std::vector<float>& score, const std::vector<int>& previous, uint32_t sliceCount) {
    const size_t count = score.size();
    std::vector<int> pick(count, -1);
    std::vector<bool> sliceUsed(sliceCount, false);
    // 1. last frame's choice stays (same slice) while the light still qualifies. The indices only mean the same
    // lights while the list keeps its length.
    if (previous.size() == count) {
        for (size_t i = 0; i < count; ++i) {
            const int s = previous[i];
            if (s < 0 || s >= (int)sliceCount || score[i] <= 0.0f || sliceUsed[(size_t)s]) continue;
            pick[i] = s;
            sliceUsed[(size_t)s] = true;
        }
    }
    // 2. newcomers in score order fill the free slices, or replace the weakest pick when they score clearly higher
    std::vector<size_t> newcomers;
    for (size_t i = 0; i < count; ++i)
        if (score[i] > 0.0f && pick[i] < 0) newcomers.push_back(i);
    std::stable_sort(newcomers.begin(), newcomers.end(), [&](size_t a, size_t b) { return score[a] > score[b]; });
    uint32_t nextFree = 0;
    for (size_t i : newcomers) {
        while (nextFree < sliceCount && sliceUsed[nextFree]) ++nextFree;
        if (nextFree < sliceCount) {
            pick[i] = (int)nextFree;
            sliceUsed[nextFree] = true;
            continue;
        }
        int weakest = -1;
        for (size_t j = 0; j < count; ++j)
            if (pick[j] >= 0 && (weakest < 0 || score[j] < score[(size_t)weakest])) weakest = (int)j;
        if (weakest < 0) break;
        // newcomers are sorted: if this one cannot replace the weakest, neither can the rest
        if (score[i] < score[(size_t)weakest] * kSpotPickHysteresis) break;
        pick[i] = pick[(size_t)weakest];   // the newcomer takes the replaced light's slice
        pick[(size_t)weakest] = -1;
    }
    return pick;
}

} // namespace mmdx
