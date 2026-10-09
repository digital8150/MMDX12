#include "anim/Motion.h"
#include "core/I18n.h"
#include "app/Lighting.h"
#include "render/Renderer.h"
#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

DirectX::XMFLOAT3 Srgb(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {f(r), f(g), f(b)};
}

} // namespace

const char* LightingPresetName(LightingPreset p) {
    switch (p) {
    case LightingPreset::Studio: return Tr("스튜디오");
    case LightingPreset::Sunset: return Tr("노을");
    case LightingPreset::Concert: return Tr("콘서트");
    case LightingPreset::Night: return Tr("밤");
    }
    return "";
}

namespace {

// The concert spots' animated sway (shared with the scene lights' Sway aim mode): a truss above the
// stage, the beams sweep with the song time. `phase` staggers the spots. The offset from the sway centre.
void SwayOffset(double t, float phase, float& dx, float& dz) {
    const float sway = (float)std::sin(t * 0.9 + phase) * 16.0f;
    const float swayZ = (float)std::cos(t * 0.7 + phase * 0.6) * 10.0f;
    dx = sway * 0.8f;
    dz = swayZ;
}

DirectX::XMFLOAT3 SwayTarget(double t, float phase, const DirectX::XMFLOAT3& focus) {
    float dx, dz;
    SwayOffset(t, phase, dx, dz);
    return {focus.x + dx, 0.0f, focus.z + dz};
}

} // namespace

void BuildLighting(LightingPreset preset, double t, const DirectX::XMFLOAT3& focus, LightParams& out) {
    out = LightParams{};
    switch (preset) {
    case LightingPreset::Studio:
        out.direction = {-0.45f, -1.0f, 0.62f};
        out.color = {0.6f, 0.6f, 0.6f};
        out.sunIntensity = 1.0f;
        out.skyZenith = Srgb(0.62f, 0.78f, 0.93f);
        out.skyHorizon = Srgb(0.90f, 0.94f, 0.97f);
        out.groundColor = Srgb(0.70f, 0.72f, 0.75f);
        out.hemiStrength = 0.16f;
        out.rimStrength = 0.32f;
        out.rimColor = {1.0f, 0.98f, 0.95f};
        break;
    case LightingPreset::Sunset:
        out.direction = {0.85f, -0.38f, 0.42f};
        out.color = {0.68f, 0.56f, 0.46f};
        out.sunIntensity = 1.0f;
        out.skyZenith = Srgb(0.38f, 0.42f, 0.66f);
        out.skyHorizon = Srgb(0.98f, 0.70f, 0.52f);
        out.groundColor = Srgb(0.42f, 0.34f, 0.36f);
        out.hemiStrength = 0.20f;
        out.rimStrength = 0.7f;
        out.rimColor = Srgb(1.0f, 0.78f, 0.55f);
        break;
    case LightingPreset::Night:
        out.direction = {0.3f, -0.85f, 0.55f};
        out.color = {0.44f, 0.48f, 0.56f};
        out.sunIntensity = 0.85f;
        out.skyZenith = Srgb(0.05f, 0.07f, 0.16f);
        out.skyHorizon = Srgb(0.16f, 0.21f, 0.36f);
        out.groundColor = Srgb(0.08f, 0.09f, 0.12f);
        out.hemiStrength = 0.22f;
        out.rimStrength = 0.55f;
        out.rimColor = Srgb(0.62f, 0.78f, 1.0f);
        break;
    case LightingPreset::Concert: {
        out.direction = {-0.2f, -1.0f, 0.35f};
        out.color = {0.46f, 0.47f, 0.52f};
        out.sunIntensity = 0.8f;
        out.skyZenith = Srgb(0.03f, 0.03f, 0.08f);
        out.skyHorizon = Srgb(0.10f, 0.08f, 0.20f);
        out.groundColor = Srgb(0.05f, 0.05f, 0.08f);
        out.hemiStrength = 0.12f;
        out.rimStrength = 0.45f;
        out.rimColor = Srgb(0.55f, 0.95f, 0.92f);
        // Six moving spots from a truss above the stage, Miku teal / magenta / white.
        const DirectX::XMFLOAT3 colors[3] = {Srgb(0.22f, 0.77f, 0.73f), Srgb(0.95f, 0.35f, 0.62f), Srgb(1.0f, 0.95f, 0.88f)};
        for (int i = 0; i < 6; ++i) {
            const float side = (i % 2 == 0) ? -1.0f : 1.0f;
            const float px = side * (14.0f + 9.0f * (float)(i / 2));
            const float phase = (float)i * 1.3f;
            PunctualLight s;
            s.position = {focus.x + px, 58.0f, focus.z - 18.0f + 6.0f * (float)(i / 2)};
            DirectX::XMFLOAT3 target = SwayTarget(t, phase, focus);
            target.x -= px * 0.25f;
            s.direction = {target.x - s.position.x, target.y - s.position.y, target.z - s.position.z};
            s.range = 140.0f;
            s.color = colors[i % 3];
            s.intensity = 2.6f;
            s.spotCosOuter = std::cos(0.24f);
            s.spotCosInner = std::cos(0.15f);
            s.shadow = LightShadowType::Soft;   // stage spots: soft-edged shadows (default softness)
            out.punctual.push_back(s);
        }
        // a warm front fill so faces never go dark between spot passes
        PunctualLight fill;
        fill.position = {focus.x, focus.y + 22.0f, focus.z - 40.0f};
        fill.range = 120.0f;
        fill.color = Srgb(1.0f, 0.92f, 0.86f);
        fill.intensity = 0.55f;
        out.punctual.push_back(fill);
        break;
    }
    }
}

void BuildSceneLighting(const std::vector<studio::SceneLight>& lights, const std::vector<studio::LightKf>& cameraLight,
                        double t, const LightAnchors& anchors, LightParams& out) {
    using studio::AimMode;
    using studio::LightKind;
    out = LightParams{};
    // the frame as a float like the VMD light sample's: a whole-frame time (frame / 30.0) must not floor to the frame before
    const float frameF = (float)(t * kMmdFps);
    const int frame = (int)std::floor(frameF);

    const studio::SceneLight* sun = nullptr;
    const studio::SceneLight* ambient = nullptr;
    for (const studio::SceneLight& l : lights) {
        if (!l.enabled) continue;
        if (l.kind == LightKind::Sun && !sun) sun = &l;
        else if (l.kind == LightKind::Ambient && !ambient) ambient = &l;
    }

    if (sun) {
        const studio::LightValues v = studio::SampleLightValues(*sun, frame);
        out.color = v.color;
        out.direction = v.direction;
        out.sunIntensity = v.intensity;
        out.rimStrength = sun->rimStrength;
        out.rimColor = sun->rimColor;
        out.sunShadow = (LightShadowType)sun->shadow;
        out.sunShadowSoftness = sun->shadowSoftness;
        out.sunShadowDensity = sun->shadowDensity;
        out.sunShadowColor = sun->shadowColor;
        // linked to the camera VMD: its light track decides the colour and direction while it has keys
        if (sun->vmdLink && !cameraLight.empty()) {
            const studio::LightKf k = studio::SampleLight(cameraLight, frameF);
            out.color = k.color;
            // a zero direction (broken file) keeps the sun's
            if (std::fabs(k.direction.x) + std::fabs(k.direction.y) + std::fabs(k.direction.z) > 1e-4f)
                out.direction = k.direction;
        }
    } else {
        // no sun: a black key light. sunIntensity stays 1 (it also scales the sky backdrop, the floor and the fog).
        out.color = {0.0f, 0.0f, 0.0f};
        out.sunIntensity = 1.0f;
        out.rimStrength = 0.0f;
        out.sunShadow = LightShadowType::NoCast;
        out.sunShadowSoftness = 0.5f;
        out.sunShadowDensity = 0.0f;
        out.sunShadowColor = {0.0f, 0.0f, 0.0f};
    }

    if (ambient) {
        out.skyZenith = ambient->skyZenith;
        out.skyHorizon = ambient->skyHorizon;
        out.groundColor = ambient->groundColor;
        out.hemiStrength = ambient->v.intensity;
    } else {
        out.hemiStrength = 0.0f;  // the default sky colours stay
    }

    for (const studio::SceneLight& l : lights) {
        if (!l.enabled || (l.kind != LightKind::Point && l.kind != LightKind::Spot && l.kind != LightKind::Area)) continue;
        if (out.punctual.size() >= Renderer::kMaxPunctualLights) break;
        const studio::LightValues v = studio::SampleLightValues(l, frame);
        PunctualLight p;
        p.position = v.position;
        p.range = v.range;
        p.color = v.color;
        p.intensity = v.intensity;
        p.shadow = (LightShadowType)l.shadow;
        p.shadowSoftness = l.shadowSoftness;
        p.shadowDensity = l.shadowDensity;
        p.shadowColor = l.shadowColor;
        p.falloff = (LightFalloffType)l.falloff;
        p.affectDiffuse = l.affectDiffuse;
        p.affectSpecular = l.affectSpecular;
        if (l.kind == LightKind::Point) {
            p.castPointShadow = (l.shadow != studio::ShadowType::NoCast);
        } else if (l.kind == LightKind::Spot) {
            DirectX::XMFLOAT3 target = ResolveSpotAim(l, v, t, anchors);
            p.direction = {target.x - v.position.x, target.y - v.position.y, target.z - v.position.z};
            if (p.direction.x * p.direction.x + p.direction.y * p.direction.y + p.direction.z * p.direction.z < 1e-6f)
                p.direction = {0.0f, -1.0f, 0.0f};
            p.spotCosOuter = std::cos(std::clamp(v.coneOuter, 0.02f, 1.5f));
            p.spotCosInner = std::cos(std::clamp(v.coneInner, 0.01f, std::max(v.coneOuter, 0.01f)));
        } else if (l.kind == LightKind::Area) {
            p.castPointShadow = (l.shadow != studio::ShadowType::NoCast);
            p.areaSize = v.size;
            p.direction = {v.aim.x - v.position.x, v.aim.y - v.position.y, v.aim.z - v.position.z};
            float lenSq = p.direction.x * p.direction.x + p.direction.y * p.direction.y + p.direction.z * p.direction.z;
            if (lenSq < 1e-6f) {
                p.direction = {0.0f, -1.0f, 0.0f};
            } else {
                float invLen = 1.0f / std::sqrt(lenSq);
                p.direction.x *= invLen;
                p.direction.y *= invLen;
                p.direction.z *= invLen;
            }
        }
        out.punctual.push_back(p);
    }
}

DirectX::XMFLOAT3 ResolveSpotAim(const studio::SceneLight& l, const studio::LightValues& v,
                                 double songSeconds, const LightAnchors& anchors) {
    using studio::AimMode;
    DirectX::XMFLOAT3 target = v.aim;
    if (l.aimMode == AimMode::Target) {
        // the character (0 or an unknown uid: the performer)
        const bool head = l.targetPart == studio::TargetPart::Head;
        target = head ? anchors.head : anchors.focus;
        if (l.targetUid != 0)
            for (const LightAnchors::Character& c : anchors.characters)
                if (c.uid == l.targetUid) {
                    target = head ? c.head : c.centre;
                    break;
                }
    } else if (l.aimMode == AimMode::Sway) {
        float dx, dz;
        SwayOffset(songSeconds, l.swayPhase, dx, dz);
        target = {v.aim.x + dx, v.aim.y, v.aim.z + dz};
    }
    return target;
}

} // namespace mmdx

