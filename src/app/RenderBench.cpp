#include "app/RenderBench.h"
#include <algorithm>
#include <cmath>

namespace mmdx {

using namespace DirectX;

namespace {

XMFLOAT3 Srgb(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {f(r), f(g), f(b)};
}

// Points per million path samples per second: an RTX 3060 Laptop GPU (~800 Msamples/s on this
// scene) scores about 5000 (tier A); the tier thresholds are the real-time benchmark's.
constexpr double kScorePerMsps = 6.25;

} // namespace

void BuildRenderBenchView(const XMFLOAT3& centerHead, FrameView& view) {
    // ---- camera: low three-quarter view over the glass cube toward the three performers
    const XMVECTOR eye = XMVectorSet(8.0f, 8.6f, -49.0f, 0.0f);
    const XMVECTOR target = XMVectorSet(0.0f, 10.4f, 0.0f, 0.0f);
    XMStoreFloat4x4(&view.camera.view, XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat3(&view.camera.eye, eye);
    view.camera.fovYRadians = XMConvertToRadians(27.0f);
    view.studioFloor = true;
    view.cameraCut = true;
    view.motionBlur = false;
    view.prevCamera = view.camera;
    const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&centerHead), XMLoadFloat4x4(&view.camera.view)));
    view.focusDistance = z > view.camera.nearZ ? z : 0.0f;

    // ---- lighting: warm key from the front left, dark studio, teal / pink rims from behind
    LightParams& l = view.light;
    l = LightParams{};
    l.direction = {0.42f, -0.78f, 0.62f};
    l.color = {0.64f, 0.61f, 0.57f};
    l.sunIntensity = 1.0f;
    l.skyZenith = Srgb(0.05f, 0.06f, 0.09f);
    l.skyHorizon = Srgb(0.13f, 0.14f, 0.19f);
    l.groundColor = Srgb(0.05f, 0.05f, 0.07f);
    l.hemiStrength = 0.14f;
    l.rimStrength = 0.55f;
    l.rimColor = Srgb(0.62f, 0.95f, 0.92f);
    const auto spot = [&](XMFLOAT3 pos, XMFLOAT3 aim, XMFLOAT3 color, float intensity, float outer, float inner) {
        PunctualLight s;
        s.position = pos;
        s.direction = {aim.x - pos.x, aim.y - pos.y, aim.z - pos.z};
        s.range = 120.0f;
        s.color = color;
        s.intensity = intensity;
        s.spotCosOuter = std::cos(outer);
        s.spotCosInner = std::cos(inner);
        l.punctual.push_back(s);
    };
    spot({-26.0f, 34.0f, 30.0f}, {-4.0f, 10.0f, 0.0f}, Srgb(0.25f, 0.85f, 0.80f), 2.4f, 0.42f, 0.22f);
    spot({26.0f, 30.0f, 28.0f}, {4.0f, 10.0f, 0.0f}, Srgb(0.98f, 0.40f, 0.66f), 2.2f, 0.42f, 0.22f);
    spot({-6.0f, 40.0f, -30.0f}, {0.0f, 3.0f, -9.0f}, Srgb(1.0f, 0.95f, 0.88f), 1.2f, 0.30f, 0.12f);  // cube top light

    // ---- props
    OfflineSceneProps& p = view.offlineProps;
    p = OfflineSceneProps{};
    p.glass.enabled = true;
    p.glass.center = {0.0f, 4.82f, -10.5f};   // just above the floor (no coplanar faces)
    p.glass.halfExtents = {4.8f, 4.8f, 4.8f};
    p.glass.yawRadians = XMConvertToRadians(32.0f);
    p.glass.cornerRadius = 0.35f;
    p.glass.ior = 1.52f;
    p.glass.dispersion = 0.035f;
    p.glass.absorption = {0.022f, 0.006f, 0.009f};   // faint Miku-teal tint through the body
    // overhead softbox above and behind the performers, facing down
    p.softboxes[0].enabled = true;
    p.softboxes[0].center = {0.0f, 46.0f, 10.0f};
    p.softboxes[0].halfU = {18.0f, 0.0f, 0.0f};
    p.softboxes[0].halfV = {0.0f, -2.5f, 8.0f};
    p.softboxes[0].radiance = {2.2f, 2.35f, 2.6f};
    // tall strip light on the right, facing the set
    p.softboxes[1].enabled = true;
    p.softboxes[1].center = {34.0f, 15.0f, -6.0f};
    p.softboxes[1].halfU = {0.0f, 13.0f, 0.0f};
    p.softboxes[1].halfV = {-2.0f, 0.0f, -4.5f};
    p.softboxes[1].radiance = {2.6f, 1.5f, 2.0f};
    p.customFloor = true;
    p.floorAlbedo = Srgb(0.62f, 0.64f, 0.68f);
    p.floorReflectivity = 0.28f;
}

BenchmarkResult ComputeRenderBenchResult(double seconds, uint32_t width, uint32_t height, uint32_t spp) {
    BenchmarkResult r;
    r.category = kBenchCategories[kBenchGiRender].id;
    r.width = width;
    r.height = height;
    r.totalFrames = (int)spp;
    r.durationSec = (float)seconds;
    r.tier = "D";
    if (seconds <= 0.0) {
        AssignBenchmarkTier(r);
        return r;
    }
    const double msps = (double)width * (double)height * (double)spp / seconds / 1e6;
    r.avgFps = (float)(std::round(msps * 100.0) / 100.0);
    r.score = (int)std::clamp(std::round(msps * kScorePerMsps), 0.0, 100000.0);
    AssignBenchmarkTier(r);
    return r;
}

} // namespace mmdx
