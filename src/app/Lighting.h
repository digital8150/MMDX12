#pragma once
// Lighting presets: the key light, sky and (for the concert preset) animated stage
// spotlights. Pure functions of the preset and the song time, so playback is deterministic
// (the benchmark sees the same lighting every run).
// BuildStudioLighting resolves the studio's LightRig (studio/LightRig.h): the preset base, then the
// VMD light track or the key override, then the custom spot rig. Also a pure function of its inputs.
#include "render/RenderTypes.h"
#include "studio/LightRig.h"
#include "studio/StudioMotion.h"
#include <cstdint>
#include <vector>

namespace mmdx {

enum class LightingPreset : int { Studio = 0, Sunset = 1, Concert = 2, Night = 3 };
inline constexpr int kLightingPresetCount = 4;

const char* LightingPresetName(LightingPreset p);  // Korean UI label

// `songSeconds` drives the animated lights; `focus` is where spotlights aim (performer).
void BuildLighting(LightingPreset preset, double songSeconds, const DirectX::XMFLOAT3& focus, LightParams& out);

// The studio lighting: `rig` decides the source (studio/LightRig.h), `vmdTrack` is the camera VMD's
// light track (VmdTrack mode), `focus`/`head` are the performer's centre / head bones (spot aims).
// Pure function of its inputs.
void BuildStudioLighting(const studio::LightRig& rig, const std::vector<studio::LightKf>& vmdTrack, double songSeconds,
                         const DirectX::XMFLOAT3& focus, const DirectX::XMFLOAT3& head, LightParams& out);

} // namespace mmdx
