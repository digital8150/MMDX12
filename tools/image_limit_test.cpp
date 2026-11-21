// Console tests for the decoder's texture size limit (asset/ImageLoader.h kMaxImageDimension). D3D12 creates no 2D
// texture wider or taller than 16384, so a larger file (a stage's 40000 x 30 strip) is box-filtered down to the limit on
// that axis alone; everything at or below the limit decodes exactly as before. No GPU or assets needed (CI).
#include "asset/ImageLoader.h"
#include "asset/DdsLoader.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace mmdx;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name, const char* fmt = "", ...) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

namespace {

// w x h RGBA8: red ramps along x, green along y, blue constant 77, alpha 255 except a translucent first row.
std::vector<uint8_t> Pattern(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)std::lround(255.0 * x / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)std::lround(255.0 * y / (h > 1 ? h - 1 : 1));
            p[2] = 77;
            p[3] = y == 0 ? 128 : 255;
        }
    return px;
}

std::vector<uint8_t> Png(uint32_t w, uint32_t h, const std::vector<uint8_t>& px) {
    std::vector<uint8_t> png;
    if (!EncodePngRGBA8(w, h, px.data(), w * 4, png)) std::printf("note: PNG encode of %ux%u failed\n", w, h);
    return png;
}

const uint8_t* At(const ImageRGBA8& img, uint32_t x, uint32_t y) {
    return &img.mips[0].pixels[((size_t)y * img.Width() + x) * 4];
}

bool ChainEndsAtOne(const ImageRGBA8& img) {
    for (size_t i = 1; i < img.mips.size(); ++i) {
        const auto& a = img.mips[i - 1];
        const auto& b = img.mips[i];
        if (b.width != std::max(1u, a.width / 2) || b.height != std::max(1u, a.height / 2)) return false;
        if (b.pixels.size() != (size_t)b.width * b.height * 4) return false;
    }
    return img.mips.back().width == 1 && img.mips.back().height == 1;
}

void WideStrip() {
    const uint32_t w = 40000, h = 30;   // the size of the stage texture that took its model down
    const std::vector<uint8_t> src = Pattern(w, h);
    const std::vector<uint8_t> png = Png(w, h, src);
    ImageRGBA8 img;
    std::string err;
    const bool ok = LoadImageRGBA8FromMemory(png.data(), png.size(), img, &err);
    Check(ok, "wide: decodes", "%s", err.c_str());
    if (!ok) return;
    Check(img.Width() == kMaxImageDimension && img.Height() == h, "wide: level 0 fits the limit, height kept",
          "%ux%u", img.Width(), img.Height());
    Check(img.ScaledDown() && img.sourceWidth == w && img.sourceHeight == h, "wide: reports the file's size",
          "%ux%u", img.sourceWidth, img.sourceHeight);
    Check(img.mips[0].pixels.size() == (size_t)img.Width() * img.Height() * 4, "wide: level 0 is tightly packed");
    Check(ChainEndsAtOne(img), "wide: mip chain halves down to 1x1", "%zu levels", img.mips.size());
    Check(img.hasAlpha, "wide: alpha survives (translucent first row)");
    bool blue = true, green = true, mono = true, alpha = true;
    for (uint32_t y = 0; y < img.Height(); ++y) {
        int prev = -1;
        for (uint32_t x = 0; x < img.Width(); ++x) {
            const uint8_t* p = At(img, x, y);
            blue &= p[2] == 77;                                   // a constant stays exactly constant
            green &= p[1] == src[((size_t)y * w) * 4 + 1];        // the untouched axis keeps its rows
            alpha &= p[3] == (y == 0 ? 128 : 255);
            mono &= (int)p[0] >= prev;
            prev = p[0];
        }
    }
    Check(blue, "wide: constant channel unchanged");
    Check(green, "wide: rows (the unscaled axis) unchanged");
    Check(alpha, "wide: alpha per row unchanged");
    Check(mono, "wide: the ramp stays monotonic");
    // Output texel i averages the source span [i s, (i + 1) s): its mean x is (i + 0.5) s - 0.5.
    const double s = (double)w / kMaxImageDimension;
    int worst = 0;
    for (uint32_t x = 0; x < img.Width(); x += 97) {
        const double expect = 255.0 * ((x + 0.5) * s - 0.5) / (w - 1);
        worst = std::max(worst, (int)std::lround(std::fabs(At(img, x, 5)[0] - expect)));
    }
    Check(worst <= 1, "wide: the ramp is the area average", "worst %d levels", worst);
    Check(At(img, 0, 5)[0] <= 1 && At(img, img.Width() - 1, 5)[0] >= 254, "wide: the ends stay at the ends", "%d %d",
          At(img, 0, 5)[0], At(img, img.Width() - 1, 5)[0]);

    // The model texture path (S11) decodes through the same function.
    ImageRGBA8 img2;
    CompressedImage blocks;
    const bool ok2 = LoadImageWithCompressedFromMemory(png.data(), png.size(), img2, blocks, &err);
    Check(ok2 && img2.Width() == kMaxImageDimension && img2.Height() == h && img2.ScaledDown() && blocks.Empty(),
          "wide: the model texture path gets the same image", "%ux%u", img2.Width(), img2.Height());
    Check(ok2 && img2.mips[0].pixels == img.mips[0].pixels, "wide: identical pixels on both paths");
}

void TallStrip() {
    const uint32_t w = 3, h = 20000;
    const std::vector<uint8_t> src = Pattern(w, h);
    const std::vector<uint8_t> png = Png(w, h, src);
    ImageRGBA8 img;
    std::string err;
    const bool ok = LoadImageRGBA8FromMemory(png.data(), png.size(), img, &err);
    Check(ok, "tall: decodes", "%s", err.c_str());
    if (!ok) return;
    Check(img.Width() == w && img.Height() == kMaxImageDimension, "tall: level 0 fits the limit, width kept", "%ux%u",
          img.Width(), img.Height());
    Check(img.sourceWidth == w && img.sourceHeight == h, "tall: reports the file's size");
    bool cols = true, mono = true;
    for (uint32_t x = 0; x < w; ++x) {
        int prev = -1;
        for (uint32_t y = 0; y < img.Height(); ++y) {
            const uint8_t* p = At(img, x, y);
            cols &= p[0] == src[(size_t)x * 4] && p[2] == 77;
            mono &= (int)p[1] >= prev;
            prev = p[1];
        }
    }
    Check(cols, "tall: columns (the unscaled axis) unchanged");
    Check(mono, "tall: the ramp stays monotonic");
    Check(ChainEndsAtOne(img), "tall: mip chain halves down to 1x1");
}

void AtAndJustOverTheLimit() {
    {
        const uint32_t w = kMaxImageDimension, h = 4;
        const std::vector<uint8_t> src = Pattern(w, h);
        const std::vector<uint8_t> png = Png(w, h, src);
        ImageRGBA8 img;
        const bool ok = LoadImageRGBA8FromMemory(png.data(), png.size(), img);
        Check(ok && img.Width() == w && img.Height() == h && !img.ScaledDown() && img.sourceWidth == 0,
              "limit: 16384 wide is not scaled");
        Check(ok && img.mips[0].pixels == src, "limit: pixels byte for byte");
    }
    {
        const uint32_t w = kMaxImageDimension + 1, h = 2;
        const std::vector<uint8_t> png = Png(w, h, Pattern(w, h));
        ImageRGBA8 img;
        const bool ok = LoadImageRGBA8FromMemory(png.data(), png.size(), img);
        Check(ok && img.Width() == kMaxImageDimension && img.Height() == h && img.sourceWidth == w,
              "limit: 16385 wide is scaled to 16384", "%ux%u", img.Width(), img.Height());
    }
    {
        const uint32_t w = 300, h = 200;
        const std::vector<uint8_t> src = Pattern(w, h);
        const std::vector<uint8_t> png = Png(w, h, src);
        ImageRGBA8 img;
        const bool ok = LoadImageRGBA8FromMemory(png.data(), png.size(), img);
        Check(ok && !img.ScaledDown() && img.mips[0].pixels == src && ChainEndsAtOne(img),
              "limit: an ordinary image is untouched");
    }
}

} // namespace

int main() {
    WideStrip();
    TallStrip();
    AtAndJustOverTheLimit();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
