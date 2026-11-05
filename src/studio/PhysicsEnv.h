#pragma once
// A studio project's physics environment: the gravity (MMD's gravity settings without their noise and keys) and the
// studio wind. Saved in .mmdxproj as "physicsWorld"; StudioUpdateModel applies it to every simulated model (viewport
// and renders) and the physics bake takes it through BakeOptions::env.
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace mmdx::studio {

struct PhysicsEnv {
    static constexpr float kDefaultGravity = 9.8f;   // MMD's default acceleration (the world runs x 10: MMD units)
    // Gravity: `gravity` (m/s^2, MMD's acceleration) along `gravityDir` (normalised when applied, so only the
    // direction counts; a zero vector = no gravity).
    float gravity = kDefaultGravity;
    DirectX::XMFLOAT3 gravityDir{0, -1, 0};
    // Wind: horizontal direction (X, Z), constant strength, gust amplitude / frequency (Hz), jitter (0..1).
    DirectX::XMFLOAT3 windDir{0, 0, 1};
    float windStrength = 0.0f;
    float windGust = 0.0f;
    float windGustFreq = 2.0f;
    float windJitter = 0.0f;

    bool HasWind() const { return windStrength > 0.0f || windGust > 0.0f || windJitter > 0.0f; }
    bool DefaultGravity() const {
        return gravity == kDefaultGravity && gravityDir.x == 0.0f && gravityDir.y == -1.0f && gravityDir.z == 0.0f;
    }
    bool SameGravity(const PhysicsEnv& o) const {
        return gravity == o.gravity && gravityDir.x == o.gravityDir.x && gravityDir.y == o.gravityDir.y &&
               gravityDir.z == o.gravityDir.z;
    }
    bool operator==(const PhysicsEnv& o) const {
        return SameGravity(o) && windDir.x == o.windDir.x && windDir.y == o.windDir.y && windDir.z == o.windDir.z &&
               windStrength == o.windStrength && windGust == o.windGust && windGustFreq == o.windGustFreq &&
               windJitter == o.windJitter;
    }

    // Non-finite values fall back to the defaults; amounts are clamped (wider than the popup's sliders, so a value
    // set by a script stays); a direction keeps its heading and is scaled into the -1..1 box of its fields.
    void Sanitize() {
        const auto finite = [](float v, float fallback, float lo, float hi) {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
        };
        const auto direction = [](DirectX::XMFLOAT3 v, DirectX::XMFLOAT3 fallback) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return fallback;
            const float m = std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)});
            if (m > 1.0f) v = {v.x / m, v.y / m, v.z / m};
            return v;
        };
        gravity = finite(gravity, kDefaultGravity, 0.0f, 200.0f);
        gravityDir = direction(gravityDir, {0, -1, 0});
        windDir = direction(windDir, {0, 0, 1});
        windStrength = finite(windStrength, 0.0f, 0.0f, 10.0f);
        windGust = finite(windGust, 0.0f, 0.0f, 10.0f);
        windGustFreq = finite(windGustFreq, 2.0f, 0.05f, 50.0f);
        windJitter = finite(windJitter, 0.0f, 0.0f, 1.0f);
    }
};

} // namespace mmdx::studio
