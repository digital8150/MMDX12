#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx {

struct BenchmarkCategory {
    const char* id;      // leaderboard category id (server whitelist)
    const char* label;   // UI label
    uint32_t width, height;
};

inline constexpr BenchmarkCategory kBenchCategories[] = {
    {"dx12-raster-fhd", "DX12 Raster · FHD (1920x1080)", 1920, 1080},
    {"dx12-raster-4k",  "DX12 Raster · 4K (3840x2160)",  3840, 2160},
};

// Fixed workload: the animation advances exactly kBenchSimStep MMD-seconds per rendered
// frame (independent of wall time), audio muted, vsync off. The first kBenchWarmupFrames
// are not measured, then kBenchMeasuredFrames are timed.
inline constexpr int kBenchWarmupFrames = 120;
inline constexpr int kBenchMeasuredFrames = 3600;
inline constexpr double kBenchSimStep = 1.0 / 60.0;

// Official preset (results are only submittable when all three are found in the library):
// matched case-insensitively as substrings of the asset id (library-relative path).
inline constexpr const char* kBenchPresetCharacter = "miku/MikuProjectDIVAstyle_ver105.pmx";
inline constexpr const char* kBenchPresetStage = "stage/theater";
inline constexpr const char* kBenchPresetSong = "motion/worldismine";

struct BenchmarkResult {
    std::string category;
    int score = 0;
    std::string tier, tierTitle;
    float avgFps = 0, low1Fps = 0, low01Fps = 0, minFps = 0, maxFps = 0;
    float frametimeMeanMs = 0, frametimeStdMs = 0;
    int stabilityPct = 0;
    int totalFrames = 0;
    float durationSec = 0;
    uint32_t width = 0, height = 0;
};

// Same formula as the web MikuMark (src/benchmark/benchmarkCalculator.ts on the server):
//   avgFps = 1000 / mean(ft); low1 = 1000 / ft_sorted[min(n-1, floor(n*0.99))];
//   low01 likewise with 0.999; min/max from the extremes; std = population std of ft (ms)
//   stability = clamp(low1 / max(1, avg), 0.1, 1); stabilityFactor = 0.8 + 0.2 * stability
//   resolutionFactor = pow(w*h / (1920*1080), 0.45)
//   score = round((avg*0.6 + low1*0.4) * resolutionFactor * stabilityFactor * 100)
//   tier: >=18000 SSS, >=12000 SS, >=8000 S, >=5000 A, >=3000 B, >=1500 C, else D
//   (tierTitle strings identical to the web version). Empty input -> score 0, tier "D".
BenchmarkResult ComputeBenchmarkResult(const std::vector<float>& frameTimesMs, double durationSec,
                                       const BenchmarkCategory& category);

} // namespace mmdx
