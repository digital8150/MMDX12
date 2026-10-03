#include "app/Benchmark.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mmdx {

namespace {

struct Tier {
    int threshold;
    const char* tier;
    const char* title;
};

// Tier titles identical to the web version (src/benchmark/benchmarkCalculator.ts).
constexpr Tier kTiers[] = {
    {18000, "SSS", "DIVA GOD · 정점"},
    {12000, "SS",  "VOCALOID MASTER · 최상위"},
    {8000,  "S",   "PRO PERFORMER · 하이엔드"},
    {5000,  "A",   "CONCERT READY · 쾌적"},
    {3000,  "B",   "SMOOTH DANCER · 준수"},
    {1500,  "C",   "TRAINEE · 한계 사양"},
    {0,     "D",   "NEEDS BOOST · 구동 어려움"},
};

} // namespace

BenchmarkResult ComputeBenchmarkResult(const std::vector<float>& frameTimesMs, double durationSec,
                                       const BenchmarkCategory& category) {
    BenchmarkResult out;
    out.category = category.id;
    out.tier = "D";
    out.tierTitle = kTiers[6].title;
    out.width = category.width;
    out.height = category.height;
    out.durationSec = (float)durationSec;
    out.totalFrames = (int)frameTimesMs.size();

    if (frameTimesMs.empty() || durationSec <= 0) return out;

    const size_t n = frameTimesMs.size();
    double sum = 0;
    for (float ft : frameTimesMs) sum += ft;
    const double mean = sum / (double)n;

    std::vector<float> sorted = frameTimesMs;
    std::sort(sorted.begin(), sorted.end());

    const double low1Ft = sorted[std::min(n - 1, (size_t)std::floor((double)n * 0.99))];
    const double low01Ft = sorted[std::min(n - 1, (size_t)std::floor((double)n * 0.999))];

    double varSum = 0;
    for (float ft : frameTimesMs) {
        const double d = ft - mean;
        varSum += d * d;
    }
    const double stdDev = std::sqrt(varSum / (double)n); // population std

    const double avgFps = 1000.0 / mean;
    const double low1 = 1000.0 / low1Ft;
    const double minFps = 1000.0 / (double)sorted.back();
    const double maxFps = 1000.0 / (double)sorted.front();

    double stability = low1 / std::max(1.0, avgFps);
    stability = std::clamp(stability, 0.1, 1.0);
    const double stabilityFactor = 0.8 + 0.2 * stability;

    const double resolutionFactor =
        std::pow((double)category.width * (double)category.height / (1920.0 * 1080.0), 0.45);

    const double score = std::round((avgFps * 0.6 + low1 * 0.4) * resolutionFactor * stabilityFactor * 100.0);

    // Store rounded values (0.1 fps / 0.01 ms).
    out.avgFps = (float)std::round(avgFps * 10.0) / 10.0f;
    out.low1Fps = (float)std::round(low1 * 10.0) / 10.0f;
    out.low01Fps = (float)std::round((1000.0 / low01Ft) * 10.0) / 10.0f;
    out.minFps = (float)std::round(minFps * 10.0) / 10.0f;
    out.maxFps = (float)std::round(maxFps * 10.0) / 10.0f;
    out.frametimeMeanMs = (float)std::round(mean * 100.0) / 100.0f;
    out.frametimeStdMs = (float)std::round(stdDev * 100.0) / 100.0f;
    out.stabilityPct = (int)std::round(stability * 100.0);
    out.score = (int)score;

    AssignBenchmarkTier(out);
    return out;
}

void AssignBenchmarkTier(BenchmarkResult& r) {
    for (const Tier& t : kTiers) {
        if (r.score >= t.threshold) {
            r.tier = t.tier;
            r.tierTitle = t.title;
            return;
        }
    }
}

} // namespace mmdx
