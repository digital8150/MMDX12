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

// 32-bit FNV-1a.
uint32_t Fnv1a(uint32_t h, const char* s) {
    for (const char* p = s; *p; ++p) {
        h ^= (uint32_t)(uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

// SplitMix-style finalizer.
uint32_t Mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

// Points per million path samples per second: an RTX 3060 Laptop GPU (~800 Msamples/s on this
// scene) scores about 5000 (tier A); the tier thresholds are the real-time benchmark's.
constexpr double kScorePerMsps = 6.25;

} // namespace

// The three characters with the most vertices (fewer characters repeat), each with a dance from
// the library; the seed mixes the chosen characters' ids and every candidate song's id.
bool PickRenderBenchCast(const LibraryScanResult& library, RenderBenchCast& out) {
    if (library.characters.empty()) return false;

    // characters: indices sorted by vertexCount descending (stable, ties keep the lower index).
    std::vector<int> sorted;
    sorted.reserve(library.characters.size());
    for (size_t i = 0; i < library.characters.size(); ++i) sorted.push_back((int)i);
    std::stable_sort(sorted.begin(), sorted.end(), [&library](int a, int b) {
        return library.characters[(size_t)a].vertexCount > library.characters[(size_t)b].vertexCount;
    });
    for (int k = 0; k < kRenderBenchPerformerCount; ++k)
        out.characters[k] = sorted[(size_t)(k % (int)sorted.size())];

    // song candidates: dances of at least 10 s, else any dance with a duration.
    std::vector<int> candidates;
    for (size_t i = 0; i < library.songs.size(); ++i)
        if (library.songs[i].durationSec >= 10.0f) candidates.push_back((int)i);
    if (candidates.empty())
        for (size_t i = 0; i < library.songs.size(); ++i)
            if (library.songs[i].durationSec > 0.0f) candidates.push_back((int)i);
    if (candidates.empty()) return false;

    // seed: the chosen characters' id strings, then all candidate songs' id strings.
    uint32_t seed = 2166136261u;
    for (int k = 0; k < kRenderBenchPerformerCount; ++k) {
        const std::string& id = library.characters[(size_t)out.characters[k]].id;
        seed = Fnv1a(seed, id.c_str());
        seed = Fnv1a(seed, "\n");
    }
    for (int i : candidates) {
        seed = Fnv1a(seed, library.songs[(size_t)i].id.c_str());
        seed = Fnv1a(seed, "\n");
    }
    out.seed = seed;

    // songs: three distinct candidates when possible, else with repetition.
    if (candidates.size() >= 3) {
        std::vector<int> remaining = candidates;
        for (int k = 0; k < kRenderBenchPerformerCount; ++k) {
            const size_t idx = (size_t)(Mix32(seed + 101u * (uint32_t)(k + 1)) % remaining.size());
            out.songs[k] = remaining[idx];
            remaining.erase(remaining.begin() + (long)idx);
        }
    } else {
        for (int k = 0; k < kRenderBenchPerformerCount; ++k)
            out.songs[k] = candidates[Mix32(seed + 101u * (uint32_t)(k + 1)) % candidates.size()];
    }
    return true;
}

float RenderBenchPoseFrame(uint32_t seed, int slot, int attempt, float danceSeconds) {
    const uint32_t u =
        Mix32(seed ^ Mix32((uint32_t)(slot * 7919 + attempt * 104729 + 1))) & 0xFFFFFFu;
    const float f = (0.25f + 0.5f * ((float)u / 16777216.0f)) * danceSeconds * 30.0f;
    return std::floor(f);
}

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
