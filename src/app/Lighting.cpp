#include "app/Lighting.h"
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
    case LightingPreset::Studio: return "스튜디오";
    case LightingPreset::Sunset: return "노을";
    case LightingPreset::Concert: return "콘서트";
    case LightingPreset::Night: return "밤";
    }
    return "";
}

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
            const float sway = (float)std::sin(t * 0.9 + phase) * 16.0f;
            const float swayZ = (float)std::cos(t * 0.7 + phase * 0.6) * 10.0f;
            DirectX::XMFLOAT3 target{focus.x + sway * 0.8f - px * 0.25f, 0.0f, focus.z + swayZ};
            s.direction = {target.x - s.position.x, target.y - s.position.y, target.z - s.position.z};
            s.range = 140.0f;
            s.color = colors[i % 3];
            s.intensity = 2.6f;
            s.spotCosOuter = std::cos(0.24f);
            s.spotCosInner = std::cos(0.15f);
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

} // namespace mmdx
