// S7a: which spot lights get one of the kSpotShadowSlices shadow slices. A slice holds the characters and props only
// (ShadowPass), so a spot that reaches no caster would render an empty map: its image is the same without one. The
// score therefore looks at the casters, not at the distance between the light and the eye.
#pragma once
#include "render/RenderTypes.h"
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace mmdx {

// One shadow caster of the frame. viewWeight: how much a shadow from it can show (CasterViewWeight).
struct SpotCaster {
    DirectX::XMFLOAT3 center{};
    float radius = 0.0f;
    float viewWeight = 1.0f;
};

// True when a punctual light's lit volume can touch the sphere. A spot's volume is its cone (widened to the shadow
// frustum Renderer::FillSceneConstants builds) cut at the range, a point or area light's the sphere of its range. A
// caster outside it shadows nothing lit: the segment from the light to any lit receiver stays inside the volume.
bool LightReachesSphere(const PunctualLight& light, const DirectX::XMFLOAT3& center, float radius);

// Sphere against the camera frustum: on screen = 1 + 2 x its height share of the screen (a close caster's shadow
// matters most), off screen = a small weight (its shadow may still fall into the view).
float CasterViewWeight(const CameraParams& camera, float aspect, const DirectX::XMFLOAT3& center, float radius);

// A spot's importance: the sum over the casters it reaches of power x falloff at the caster x cone position x view
// weight. 0 = no slice: not a casting spot, a shadow that cannot show, or no caster in reach.
float SpotShadowScore(const PunctualLight& light, const std::vector<SpotCaster>& casters, bool volumetricOn);

// The slice per light (-1 = none) from this frame's scores and last frame's choice. A light keeps its slice while its
// score is positive unless a newcomer scores kSpotPickHysteresis times more than the weakest pick, so the choice does
// not flicker between similar lights.
std::vector<int> PickSpotSlices(const std::vector<float>& score, const std::vector<int>& previous, uint32_t sliceCount);

} // namespace mmdx
