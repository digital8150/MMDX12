#pragma once
// Colour look-up tables for the final grade (PostPass). A LUT maps display-referred sRGB
// (after tonemapping) to graded sRGB. It is stored as a 2D strip of kColorLutSize slices:
//   width = kColorLutSize * kColorLutSize, height = kColorLutSize, RGBA8 (alpha 255),
//   texel (x, y) = LUT(r = (x % N) / (N-1), g = y / (N-1), b = (x / N) / (N-1)), N = kColorLutSize.
// Built-in looks are generated procedurally; user looks are Adobe/Resolve .cube files.
#include "asset/ImageLoader.h"
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

inline constexpr uint32_t kColorLutSize = 32;

struct ColorLutEntry {
    std::string id;            // stable key persisted in settings: "builtin:<name>" or "file:<file name>"
    std::string displayName;   // UI label (Korean for built-ins, file stem for .cube files)
    std::filesystem::path file;  // .cube path; empty for built-ins
    int builtin = -1;          // index into the built-in looks, -1 for files
};

// Built-in looks first (fixed order), then every *.cube file found directly in `dirs`
// (non-recursive, missing dirs skipped, sorted by file name, duplicates by file name dropped).
std::vector<ColorLutEntry> ListColorLuts(const std::vector<std::filesystem::path>& dirs);

// Generates (built-in) or loads + resamples (.cube, trilinear, any LUT_3D_SIZE 2..256,
// DOMAIN_MIN/MAX honoured) the LUT into `strip` (one mip level, layout above).
// Returns false (and logs) on read/parse errors; `strip` is then left empty.
bool BuildColorLut(const ColorLutEntry& entry, ImageRGBA8& strip);

} // namespace mmdx
