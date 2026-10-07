#pragma once
// Studio light rig: where the studio's lighting comes from and what the user can control there.
// Pure data (no renderer / imgui / App includes): StudioDoc holds one LightRig, projects persist it
// (StudioProject.cpp) and app/Lighting.cpp resolves it into LightParams with BuildStudioLighting.
//   - VmdTrack: today's behaviour. The camera VMD's light track (LightKf, linear) overrides the
//     preset's key light; an empty track falls back to the preset.
//   - Preset: the preset key light, with an optional explicit override (KeyOverride). The concert
//     preset's animated spots stay automatic (no rig editing).
//   - Custom: the rig owns the spotlight list (SpotLight, keyframeable through the timeline's
//     RowKind::Spot rows) and the optional warm front fill. The key override applies here too.
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx::studio {

inline constexpr int kMaxRigSpots = 16;  // Renderer::kMaxPunctualLights (the render side clamps again)

enum class LightSource : uint8_t { VmdTrack = 0, Preset = 1, Custom = 2 };
const char* LightSourceName(LightSource s);            // "vmd" | "preset" | "custom" (.mmdxproj)
bool ParseLightSource(const std::string& s, LightSource& out);
const char* SpotModeName(uint8_t mode);                // "auto" | "center" | "head" | "manual"
bool ParseSpotMode(const std::string& s, uint8_t& out);

// Manual control over the preset's key light (Preset and Custom modes): direction, colour,
// intensity, rim. Off = the preset decides everything.
struct KeyOverride {
    bool enabled = false;
    DirectX::XMFLOAT3 direction{-0.5f, -1.0f, 0.5f};
    DirectX::XMFLOAT3 color{0.6f, 0.6f, 0.6f};
    float intensity = 1.0f;              // LightParams::sunIntensity
    float rimStrength = 0.35f;
    DirectX::XMFLOAT3 rimColor{1.0f, 0.97f, 0.92f};
    bool operator==(const KeyOverride& o) const {
        return enabled == o.enabled && intensity == o.intensity && rimStrength == o.rimStrength &&
               direction.x == o.direction.x && direction.y == o.direction.y && direction.z == o.direction.z &&
               color.x == o.color.x && color.y == o.color.y && color.z == o.color.z &&
               rimColor.x == o.rimColor.x && rimColor.y == o.rimColor.y && rimColor.z == o.rimColor.z;
    }
};

// Spot keyframe: the numeric fields of one spot at `frame`. Aim is a world point (MMD space); the
// spot axis is aim - position. Cone outer in radians (the UI shows degrees). Keyed fields
// interpolate linearly between keys; before the first / after the last the nearest key holds.
struct SpotKf {
    int frame = 0;
    DirectX::XMFLOAT3 position{0, 45, -15};
    DirectX::XMFLOAT3 aim{0, 0, 0};
    float intensity = 2.6f;
    float coneOuter = 0.24f;
    DirectX::XMFLOAT3 color{1, 1, 1};
};

// One concert-style spot light. Mode decides the aim: the animated swing (like the concert
// preset's spots), the performer's centre or head, or a manual aim point. Numeric fields and the
// manual aim are keyframeable: `keys` is the timeline's RowKind::Spot track of this spot.
struct SpotLight {
    std::string name;                                    // "스팟 1"..., unique (undo snapshots match by name)
    uint8_t mode = 0;                                    // SpotMode: 0 auto swing, 1 centre, 2 head, 3 manual
    DirectX::XMFLOAT3 position{0, 45, -15};
    DirectX::XMFLOAT3 aim{0, 0, 0};
    DirectX::XMFLOAT3 color{1.0f, 0.98f, 0.92f};
    float intensity = 2.6f;
    float coneOuter = 0.24f;                             // radians
    float coneInner = 0.15f;                             // radians (kept at ~0.6 * coneOuter by the editor)
    bool enabled = true;
    float swingPhase = 0.0f;                             // auto swing: phase of the sway formula (per spot)
    std::vector<SpotKf> keys;                            // sorted by frame, one key per frame

    bool operator==(const SpotLight& o) const { return FullEqual(*this, o); }

    // full equality including identity fields (coneInner is derived: 0.6 * coneOuter, not persisted)
    static bool FullEqual(const SpotLight& a, const SpotLight& b) {
        return a.name == b.name && a.mode == b.mode && a.enabled == b.enabled && a.swingPhase == b.swingPhase &&
               KeysEqual(a, b);
    }
    // equality of the rendered result (mode / position / aim / colour / intensity / cone / keys)
    static bool KeysEqual(const SpotLight& a, const SpotLight& b) {
        return a.mode == b.mode && a.enabled == b.enabled && a.position.x == b.position.x &&
               a.position.y == b.position.y && a.position.z == b.position.z && a.aim.x == b.aim.x &&
               a.aim.y == b.aim.y && a.aim.z == b.aim.z && a.color.x == b.color.x && a.color.y == b.color.y &&
               a.color.z == b.color.z && a.intensity == b.intensity && a.coneOuter == b.coneOuter &&
               a.keys.size() == b.keys.size() &&
               std::equal(a.keys.begin(), a.keys.end(), b.keys.begin(), [](const SpotKf& x, const SpotKf& y) {
                   return x.frame == y.frame && x.intensity == y.intensity && x.coneOuter == y.coneOuter &&
                          x.position.x == y.position.x && x.position.y == y.position.y && x.position.z == y.position.z &&
                          x.aim.x == y.aim.x && x.aim.y == y.aim.y && x.aim.z == y.aim.z && x.color.x == y.color.x &&
                          x.color.y == y.color.y && x.color.z == y.color.z;
               });
    }
};

// Where the studio's light comes from: source, preset choice, key override, spots, front fill.
struct LightRig {
    LightSource source = LightSource::VmdTrack;
    int presetIndex = 0;                 // LightingPreset (app/Lighting.h): the base preset in every mode
    KeyOverride key;                     // Preset / Custom modes: manual key light control
    std::vector<SpotLight> spots;        // Custom mode: the rig's spots (up to kMaxRigSpots)
    bool frontFill = false;              // Custom mode: a warm front fill so faces never go dark

    SpotLight* Spot(const std::string& name) {
        for (SpotLight& s : spots)
            if (s.name == name) return &s;
        return nullptr;
    }
    const SpotLight* Spot(const std::string& name) const {
        return const_cast<LightRig*>(this)->Spot(name);
    }
    SpotLight* SpotAt(size_t index) { return index < spots.size() ? &spots[index] : nullptr; }
    // Appends "스팟 N" (first unused N) with the next swing phase; false when the rig is full.
    bool AddSpot();
    size_t ApproxBytes() const {
        size_t bytes = sizeof(LightRig);
        for (const SpotLight& s : spots) bytes += sizeof(SpotLight) + s.name.size() + s.keys.capacity() * sizeof(SpotKf);
        return bytes;
    }
    bool operator==(const LightRig& o) const {
        return source == o.source && presetIndex == o.presetIndex && frontFill == o.frontFill && key == o.key &&
               spots.size() == o.spots.size() &&
               std::equal(spots.begin(), spots.end(), o.spots.begin(), SpotLight::FullEqual);
    }
};

// Keyed spot values at `frame` (linear between keys, nearest key held past the ends; without keys
// the rig's own values). The returned frame is the input.
SpotKf SampleSpotKeys(const SpotLight& s, int frame);

} // namespace mmdx::studio
