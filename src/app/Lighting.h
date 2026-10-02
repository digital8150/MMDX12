#pragma once
// Lighting presets: the key light, sky and (for the concert preset) animated stage
// spotlights. Pure functions of the preset and the song time, so playback is deterministic
// (the benchmark sees the same lighting every run).
#include "render/RenderTypes.h"
#include <cstdint>

namespace mmdx {

enum class LightingPreset : int { Studio = 0, Sunset = 1, Concert = 2, Night = 3 };
inline constexpr int kLightingPresetCount = 4;

const char* LightingPresetName(LightingPreset p);  // Korean UI label

// `songSeconds` drives the animated lights; `focus` is where spotlights aim (performer).
void BuildLighting(LightingPreset preset, double songSeconds, const DirectX::XMFLOAT3& focus, LightParams& out);

} // namespace mmdx
