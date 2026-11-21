#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

struct CompressedImage;

// The largest width / height of a 2D texture in D3D12 (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION). The decoders below fit a
// larger file into it, so every image they return can be uploaded.
inline constexpr uint32_t kMaxImageDimension = 16384;

// Tightly packed RGBA8 image with a full mip chain (level 0 first).
struct ImageRGBA8 {
    struct Level { uint32_t width = 0, height = 0; std::vector<uint8_t> pixels; };  // pixels.size() == w*h*4
    std::vector<Level> mips;
    bool hasAlpha = false;  // true if any texel in level 0 has alpha < 255
    // The decoded file's size when it exceeded kMaxImageDimension and level 0 was scaled down to fit (0 = not scaled).
    uint32_t sourceWidth = 0, sourceHeight = 0;

    bool Empty() const { return mips.empty(); }
    uint32_t Width() const { return mips.empty() ? 0 : mips[0].width; }
    uint32_t Height() const { return mips.empty() ? 0 : mips[0].height; }
    bool ScaledDown() const { return sourceWidth != 0; }
};

// Decodes PNG/JPG/BMP/TGA/GIF/PSD (stb_image), and treats .spa/.sph as BMP (stb sniffs
// the content, so the extension is irrelevant). Builds mips with a 2x2 box filter down
// to 1x1 (odd sizes: clamp the source coordinate). A side longer than kMaxImageDimension is
// box-filtered down to it first, that axis alone, as MMD's D3DX loader fits a file to the
// device (UVs are normalised, so the mapping stays). Returns false on failure.
bool LoadImageRGBA8(const std::filesystem::path& path, ImageRGBA8& out, std::string* error = nullptr);
// Same decode from an in-memory file image (textures embedded in glTF/GLB/VRM/FBX). WIC covers
// formats stb lacks (e.g. WebP when the system codec is installed).
bool LoadImageRGBA8FromMemory(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error = nullptr);

// S11: loads `path` for a material texture. `image` receives the RGBA8 decode (level 0 + the built mip chain, exactly
// like LoadImageRGBA8: the CPU copies the material code needs) and `compressed` the blocks of a block-compressed DDS
// file (BC1..BC7) when the file is one, so the GPU can take it compressed; `compressed` stays empty for every other
// file. The file is read once. BC4/BC5/BC6H/BC7 have no RGBA8 decoder: for such a file `image` stays empty, `compressed`
// carries the blocks and the call reports success (the GPU takes the compressed form). Failing with a message means
// neither form is usable.
bool LoadImageWithCompressed(const std::filesystem::path& path, ImageRGBA8& image, CompressedImage& compressed,
                             std::string* error = nullptr);
bool LoadImageWithCompressedFromMemory(const uint8_t* bytes, size_t size, ImageRGBA8& image,
                                       CompressedImage& compressed, std::string* error = nullptr);

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
