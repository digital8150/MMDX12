// Scene lights: project-file names, exact equality, key sampling and the preset lists. See SceneLight.h.
#include "studio/SceneLight.h"
#include <algorithm>
#include <cmath>

namespace mmdx::studio {

namespace {

bool Same(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

DirectX::XMFLOAT3 Lerp3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t) {
    return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
}

// sRGB -> linear, the same helper as app/Lighting.cpp (the presets are specified in sRGB)
DirectX::XMFLOAT3 Srgb(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {f(r), f(g), f(b)};
}

} // namespace

const char* LightKindName(LightKind k) {
    switch (k) {
    case LightKind::Point: return "point";
    case LightKind::Spot: return "spot";
    case LightKind::Ambient: return "ambient";
    case LightKind::Sun: break;
    }
    return "sun";
}

bool ParseLightKind(const std::string& s, LightKind& out) {
    if (s == "sun") { out = LightKind::Sun; return true; }
    if (s == "point") { out = LightKind::Point; return true; }
    if (s == "spot") { out = LightKind::Spot; return true; }
    if (s == "ambient") { out = LightKind::Ambient; return true; }
    return false;
}

const char* AimModeName(AimMode m) {
    switch (m) {
    case AimMode::Target: return "target";
    case AimMode::Sway: return "sway";
    case AimMode::Manual: break;
    }
    return "manual";
}

bool ParseAimMode(const std::string& s, AimMode& out) {
    if (s == "manual") { out = AimMode::Manual; return true; }
    if (s == "target") { out = AimMode::Target; return true; }
    if (s == "sway") { out = AimMode::Sway; return true; }
    return false;
}

const char* TargetPartName(TargetPart p) { return p == TargetPart::Head ? "head" : "centre"; }

bool ParseTargetPart(const std::string& s, TargetPart& out) {
    if (s == "centre") { out = TargetPart::Centre; return true; }
    if (s == "head") { out = TargetPart::Head; return true; }
    return false;
}

const char* ShadowTypeName(ShadowType t) {
    switch (t) {
    case ShadowType::NoCast: return "nocast";
    case ShadowType::Soft: return "soft";
    case ShadowType::Hard: break;
    }
    return "hard";
}

bool ParseShadowType(const std::string& s, ShadowType& out) {
    if (s == "nocast") { out = ShadowType::NoCast; return true; }
    if (s == "hard") { out = ShadowType::Hard; return true; }
    if (s == "soft") { out = ShadowType::Soft; return true; }
    return false;
}

const char* FalloffTypeName(FalloffType f) {
    switch (f) {
    case FalloffType::Linear: return "linear";
    case FalloffType::InverseSquare: return "inverse_square";
    case FalloffType::None: break;
    }
    return "none";
}

bool ParseFalloffType(const std::string& s, FalloffType& out) {
    if (s == "none") { out = FalloffType::None; return true; }
    if (s == "linear") { out = FalloffType::Linear; return true; }
    if (s == "inverse_square") { out = FalloffType::InverseSquare; return true; }
    return false;
}

bool LightValues::operator==(const LightValues& o) const {
    return Same(position, o.position) && Same(aim, o.aim) && Same(direction, o.direction) && Same(color, o.color) &&
           intensity == o.intensity && range == o.range && coneOuter == o.coneOuter && coneInner == o.coneInner;
}

bool LightKey::operator==(const LightKey& o) const { return frame == o.frame && v == o.v; }

bool SceneLight::operator==(const SceneLight& o) const {
    return uid == o.uid && name == o.name && kind == o.kind && enabled == o.enabled && v == o.v && keys == o.keys &&
           vmdLink == o.vmdLink && rimStrength == o.rimStrength && Same(rimColor, o.rimColor) &&
           aimMode == o.aimMode && targetUid == o.targetUid && targetPart == o.targetPart && swayPhase == o.swayPhase &&
           Same(skyZenith, o.skyZenith) && Same(skyHorizon, o.skyHorizon) && Same(groundColor, o.groundColor) &&
           shadow == o.shadow && shadowSoftness == o.shadowSoftness && shadowDensity == o.shadowDensity &&
           Same(shadowColor, o.shadowColor) && falloff == o.falloff && affectDiffuse == o.affectDiffuse &&
           affectSpecular == o.affectSpecular && viewportVisible == o.viewportVisible;
}

LightValues SampleLightValues(const SceneLight& light, int frame) {
    const std::vector<LightKey>& keys = light.keys;
    if (light.kind == LightKind::Ambient || keys.empty()) return light.v;
    if (frame <= keys.front().frame) return keys.front().v;
    if (frame >= keys.back().frame) return keys.back().v;
    // the last key at or before `frame` (strictly inside the key range here, so both neighbours exist)
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame, [](int f, const LightKey& k) { return f < k.frame; });
    const LightKey& a = *(next - 1);
    const LightKey& b = *next;
    if (a.frame == frame) return a.v;
    const float t = (float)(frame - a.frame) / (float)(b.frame - a.frame);
    LightValues out;
    out.position = Lerp3(a.v.position, b.v.position, t);
    out.aim = Lerp3(a.v.aim, b.v.aim, t);
    out.direction = Lerp3(a.v.direction, b.v.direction, t);
    out.color = Lerp3(a.v.color, b.v.color, t);
    out.intensity = Lerp(a.v.intensity, b.v.intensity, t);
    out.range = Lerp(a.v.range, b.v.range, t);
    out.coneOuter = Lerp(a.v.coneOuter, b.v.coneOuter, t);
    out.coneInner = Lerp(a.v.coneInner, b.v.coneInner, t);
    return out;
}

std::vector<SceneLight> PresetLights(int presetIndex, DirectX::XMFLOAT3 focus, uint32_t& nextLightUid) {
    const int preset = std::clamp(presetIndex, 0, 3);
    std::vector<SceneLight> out;

    // The key light and the environment of the preset (the values of BuildLighting in app/Lighting.cpp).
    SceneLight sun;
    sun.name = "메인 조명";
    sun.kind = LightKind::Sun;
    sun.vmdLink = true;
    SceneLight ambient;
    ambient.name = "환경광";
    ambient.kind = LightKind::Ambient;
    switch (preset) {
    case 0:  // Studio
        sun.v.direction = {-0.45f, -1.0f, 0.62f};
        sun.v.color = {0.6f, 0.6f, 0.6f};
        sun.v.intensity = 1.0f;
        sun.rimStrength = 0.32f;
        sun.rimColor = {1.0f, 0.98f, 0.95f};
        ambient.skyZenith = Srgb(0.62f, 0.78f, 0.93f);
        ambient.skyHorizon = Srgb(0.90f, 0.94f, 0.97f);
        ambient.groundColor = Srgb(0.70f, 0.72f, 0.75f);
        ambient.v.intensity = 0.16f;
        break;
    case 1:  // Sunset
        sun.v.direction = {0.85f, -0.38f, 0.42f};
        sun.v.color = {0.68f, 0.56f, 0.46f};
        sun.v.intensity = 1.0f;
        sun.rimStrength = 0.7f;
        sun.rimColor = Srgb(1.0f, 0.78f, 0.55f);
        ambient.skyZenith = Srgb(0.38f, 0.42f, 0.66f);
        ambient.skyHorizon = Srgb(0.98f, 0.70f, 0.52f);
        ambient.groundColor = Srgb(0.42f, 0.34f, 0.36f);
        ambient.v.intensity = 0.20f;
        break;
    case 2:  // Concert
        sun.v.direction = {-0.2f, -1.0f, 0.35f};
        sun.v.color = {0.46f, 0.47f, 0.52f};
        sun.v.intensity = 0.8f;
        sun.rimStrength = 0.45f;
        sun.rimColor = Srgb(0.55f, 0.95f, 0.92f);
        ambient.skyZenith = Srgb(0.03f, 0.03f, 0.08f);
        ambient.skyHorizon = Srgb(0.10f, 0.08f, 0.20f);
        ambient.groundColor = Srgb(0.05f, 0.05f, 0.08f);
        ambient.v.intensity = 0.12f;
        break;
    default:  // 3: Night
        sun.v.direction = {0.3f, -0.85f, 0.55f};
        sun.v.color = {0.44f, 0.48f, 0.56f};
        sun.v.intensity = 0.85f;
        sun.rimStrength = 0.55f;
        sun.rimColor = Srgb(0.62f, 0.78f, 1.0f);
        ambient.skyZenith = Srgb(0.05f, 0.07f, 0.16f);
        ambient.skyHorizon = Srgb(0.16f, 0.21f, 0.36f);
        ambient.groundColor = Srgb(0.08f, 0.09f, 0.12f);
        ambient.v.intensity = 0.22f;
        break;
    }
    sun.uid = nextLightUid++;
    out.push_back(std::move(sun));
    ambient.uid = nextLightUid++;
    out.push_back(std::move(ambient));

    if (preset == 2) {
        // Six moving spots from a truss above the stage, Miku teal / magenta / white, and a warm front fill so faces
        // never go dark between spot passes.
        const DirectX::XMFLOAT3 colors[3] = {Srgb(0.22f, 0.77f, 0.73f), Srgb(0.95f, 0.35f, 0.62f), Srgb(1.0f, 0.95f, 0.88f)};
        for (int i = 0; i < 6; ++i) {
            const float side = (i % 2 == 0) ? -1.0f : 1.0f;
            const float px = side * (14.0f + 9.0f * (float)(i / 2));
            SceneLight s;
            s.uid = nextLightUid++;
            s.name = "스팟 " + std::to_string(i + 1);
            s.kind = LightKind::Spot;
            s.v.position = {focus.x + px, 58.0f, focus.z - 18.0f + 6.0f * (float)(i / 2)};
            s.v.aim = {focus.x - px * 0.25f, 0.0f, focus.z};
            s.v.color = colors[i % 3];
            s.v.intensity = 2.6f;
            s.v.range = 140.0f;
            s.v.coneOuter = 0.24f;
            s.v.coneInner = 0.15f;
            s.aimMode = AimMode::Sway;
            s.swayPhase = (float)i * 1.3f;
            out.push_back(std::move(s));
        }
        SceneLight fill;
        fill.uid = nextLightUid++;
        fill.name = "채움광";
        fill.kind = LightKind::Point;
        fill.v.position = {focus.x, focus.y + 22.0f, focus.z - 40.0f};
        fill.v.color = Srgb(1.0f, 0.92f, 0.86f);
        fill.v.intensity = 0.55f;
        fill.v.range = 120.0f;
        fill.shadow = ShadowType::NoCast; // a fill lifts the key light's shadows, it casts none itself
        out.push_back(std::move(fill));
    }
    return out;
}

} // namespace mmdx::studio
