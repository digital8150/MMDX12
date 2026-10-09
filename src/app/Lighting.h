#pragma once
// Lighting presets: the key light, sky and (for the concert preset) animated stage
// spotlights. Pure functions of the preset and the song time, so playback is deterministic
// (the benchmark sees the same lighting every run).
// BuildSceneLighting resolves the studio's scene lights (studio/SceneLight.h) the same way: a pure function of its inputs.
#include "render/RenderTypes.h"
#include "studio/SceneLight.h"
#include "studio/StudioMotion.h"
#include <cstdint>
#include <vector>

namespace mmdx {

enum class LightingPreset : int { Studio = 0, Sunset = 1, Concert = 2, Night = 3 };
inline constexpr int kLightingPresetCount = 4;

const char* LightingPresetName(LightingPreset p);  // Korean UI label

// `songSeconds` drives the animated lights; `focus` is where spotlights aim (performer).
void BuildLighting(LightingPreset preset, double songSeconds, const DirectX::XMFLOAT3& focus, LightParams& out);

// Where the characters are, for the spots that aim at one (world space).
struct LightAnchors {
    DirectX::XMFLOAT3 focus{0, 10, 0};   // the performer's centre bone, else {0, 10, 0}
    DirectX::XMFLOAT3 head{0, 16, 0};    // the performer's head bone, else focus + 8 up
    struct Character {
        uint32_t uid = 0;                // StudioModel::uid
        DirectX::XMFLOAT3 centre{}, head{};
    };
    std::vector<Character> characters;   // every character model with its centre and head positions
};

// The studio lighting from the scene lights at `songSeconds`:
//   - sun: the first enabled sun (the camera VMD's light track `cameraLight` overrides its colour and direction while
//     the sun is vmdLink-ed and the track has keys). Without a sun the key light is black with sunIntensity 1, so the
//     environment stays lit (sunIntensity also scales the sky).
//   - environment: the first enabled ambient light (sky colours, hemisphere strength); without one the hemisphere
//     strength is 0 and the default sky colours stay.
//   - punctual: the enabled point and spot lights in list order, at most Renderer::kMaxPunctualLights.
// With the lights of PresetLights(p, focus) the result equals BuildLighting(p, songSeconds, focus). Pure function.
void BuildSceneLighting(const std::vector<studio::SceneLight>& lights, const std::vector<studio::LightKf>& cameraLight,
                        double songSeconds, const LightAnchors& anchors, LightParams& out);

} // namespace mmdx
