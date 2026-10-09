#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

// Tightly packed RGBA8 image with a full mip chain (level 0 first).
struct ImageRGBA8 {
    struct Level { uint32_t width = 0, height = 0; std::vector<uint8_t> pixels; };  // pixels.size() == w*h*4
    std::vector<Level> mips;
    bool hasAlpha = false;  // true if any texel in level 0 has alpha < 255

    bool Empty() const { return mips.empty(); }
    uint32_t Width() const { return mips.empty() ? 0 : mips[0].width; }
    uint32_t Height() const { return mips.empty() ? 0 : mips[0].height; }
};

// Decodes PNG/JPG/BMP/TGA/GIF/PSD (stb_image), and treats .spa/.sph as BMP (stb sniffs
// the content, so the extension is irrelevant). Builds mips with a 2x2 box filter down
// to 1x1 (odd sizes: clamp the source coordinate). Returns false on failure.
bool LoadImageRGBA8(const std::filesystem::path& path, ImageRGBA8& out, std::string* error = nullptr);
// Same decode from an in-memory file image (textures embedded in glTF/GLB/VRM/FBX). WIC covers
// formats stb lacks (e.g. WebP when the system codec is installed).
bool LoadImageRGBA8FromMemory(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error = nullptr);

// Writes level 0 of an RGBA8 buffer as PNG (stb_image_write).
bool SavePngRGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const uint8_t* rgba, uint32_t rowPitchBytes);

// Encodes an RGBA8 buffer to in-memory PNG bytes (stb_image_write).
bool EncodePngRGBA8(uint32_t width, uint32_t height, const uint8_t* rgba,
                    uint32_t rowPitchBytes, std::vector<uint8_t>& outPng);

// Downscales an RGBA8 buffer so width <= maxWidth (preserves aspect ratio).
// If width <= maxWidth, returns a copy.
std::vector<uint8_t> DownscaleRgba8(uint32_t srcW, uint32_t srcH, const uint8_t* srcRgba,
                                    uint32_t maxWidth, uint32_t& outW, uint32_t& outH);

} // namespace mmdx
