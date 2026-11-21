#include "asset/ImageLoader.h"
#include "asset/BinaryReader.h"
#include "asset/DdsLoader.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <Windows.h>

#include <fstream>
#include <ole2.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace mmdx {

namespace {

struct Tap {
    uint32_t index;
    float weight;
};

// The source texels output texel i covers when an axis shrinks from `src` to `dst` texels (dst < src): the span
// [i * s, (i + 1) * s), s = src / dst, the partly covered texels at its ends weighted by their overlap (sum 1).
std::vector<std::vector<Tap>> BoxTaps(uint32_t src, uint32_t dst) {
    std::vector<std::vector<Tap>> taps(dst);
    const double s = (double)src / (double)dst;
    for (uint32_t i = 0; i < dst; ++i) {
        const double a = i * s, b = std::min((double)src, (i + 1) * s);
        for (uint32_t j = (uint32_t)a; j < src && (double)j < b; ++j) {
            const double overlap = std::min(b, j + 1.0) - std::max(a, (double)j);
            if (overlap > 0.0) taps[i].push_back({j, (float)(overlap / s)});
        }
    }
    return taps;
}

// D3D12 creates no 2D texture wider or taller than kMaxImageDimension: CreateCommittedResource fails with
// E_INVALIDARG, and a model texture took its whole model with it (a 40000 x 30 strip of a stage). MMD's D3DX loader
// clamps the width and the height to the device's maximum separately and resamples the file; this box-filters the
// oversized axis (or both) of level 0 down to the limit the same way. Returns true when it scaled.
bool FitToTextureLimit(ImageRGBA8::Level& level) {
    const uint32_t w = level.width, h = level.height;
    const uint32_t nw = std::min(w, kMaxImageDimension), nh = std::min(h, kMaxImageDimension);
    if (nw == w && nh == h) return false;
    const auto store = [](uint8_t* dst, const float* sum) {
        for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)std::clamp(std::lround(sum[c]), 0L, 255L);
    };
    if (nw < w) {
        const std::vector<std::vector<Tap>> taps = BoxTaps(w, nw);
        std::vector<uint8_t> out((size_t)nw * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = level.pixels.data() + (size_t)y * w * 4;
            for (uint32_t x = 0; x < nw; ++x) {
                float sum[4] = {};
                for (const Tap& t : taps[x])
                    for (int c = 0; c < 4; ++c) sum[c] += row[(size_t)t.index * 4 + c] * t.weight;
                store(&out[((size_t)y * nw + x) * 4], sum);
            }
        }
        level.pixels = std::move(out);
        level.width = nw;
    }
    if (nh < h) {
        const std::vector<std::vector<Tap>> taps = BoxTaps(h, nh);
        const uint32_t rw = level.width;
        std::vector<uint8_t> out((size_t)rw * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < rw; ++x) {
                float sum[4] = {};
                for (const Tap& t : taps[y]) {
                    const uint8_t* p = &level.pixels[((size_t)t.index * rw + x) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c] * t.weight;
                }
                store(&out[((size_t)y * rw + x) * 4], sum);
            }
        }
        level.pixels = std::move(out);
        level.height = nh;
    }
    return true;
}

bool LoadWithWic(const uint8_t* bytes, size_t size, std::vector<uint8_t>& pixels,
                 int& w, int& h) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mustUninit = SUCCEEDED(hr);  // S_OK or S_FALSE
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    bool ok = false;
    do {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
        if (FAILED(hr)) break;
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) break;
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size));
        if (FAILED(hr)) break;
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) break;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) break;
        hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted);
        if (FAILED(hr)) break;
        UINT width = 0, height = 0;
        hr = converted->GetSize(&width, &height);
        if (FAILED(hr)) break;
        w = static_cast<int>(width);
        h = static_cast<int>(height);
        if (w <= 0 || h <= 0) break;
        pixels.resize(static_cast<size_t>(w) * h * 4);
        hr = converted->CopyPixels(nullptr, static_cast<UINT>(w) * 4,
                                   static_cast<UINT>(pixels.size()), pixels.data());
        ok = SUCCEEDED(hr);
    } while (false);
    if (converted) converted->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (mustUninit) CoUninitialize();
    return ok;
}

} // namespace

bool LoadImageRGBA8(const std::filesystem::path& path, ImageRGBA8& out, std::string* error) {
    out = ImageRGBA8{};
    std::vector<uint8_t> fileData;
    std::string readError;
    if (!ReadWholeFile(path, fileData, &readError)) {
        if (error) *error = readError;
        return false;
    }
    return LoadImageRGBA8FromMemory(fileData.data(), fileData.size(), out, error);
}

bool LoadImageRGBA8FromMemory(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error) {
    try {
        out = ImageRGBA8{};

        // M5a: a DDS file is decoded by our own loader (stb/WIC cannot read DDS); the mip chain is built below.
        if (IsDdsImage(bytes, size)) {
            if (!LoadDdsLevel0(bytes, size, out, error)) return false;
            uint32_t sw = out.mips[0].width, sh = out.mips[0].height;
            while (sw > 1 || sh > 1) {
                uint32_t nw = std::max(1u, sw / 2), nh = std::max(1u, sh / 2);
                ImageRGBA8::Level dst;
                dst.width = nw;
                dst.height = nh;
                dst.pixels.resize((size_t)nw * nh * 4);
                const ImageRGBA8::Level& srcLevel = out.mips.back();
                const uint8_t* src = srcLevel.pixels.data();
                for (uint32_t y = 0; y < nh; ++y) {
                    for (uint32_t x = 0; x < nw; ++x) {
                        const uint32_t sx0 = std::min(2 * x, sw - 1), sy0 = std::min(2 * y, sh - 1);
                        const uint32_t sx1 = std::min(2 * x + 1, sw - 1), sy1 = std::min(2 * y + 1, sh - 1);
                        uint8_t* dstPix = &dst.pixels[((size_t)y * nw + x) * 4];
                        for (int c = 0; c < 4; ++c) {
                            const int sum = src[((size_t)sy0 * sw + sx0) * 4 + c] + src[((size_t)sy0 * sw + sx1) * 4 + c] +
                                            src[((size_t)sy1 * sw + sx0) * 4 + c] + src[((size_t)sy1 * sw + sx1) * 4 + c];
                            dstPix[c] = (uint8_t)((sum + 2) / 4);
                        }
                    }
                }
                out.mips.push_back(std::move(dst));
                sw = nw;
                sh = nh;
            }
            return true;
        }

        const std::string readError = "empty image data";
        struct View {
            const uint8_t* ptr;
            size_t n;
            const uint8_t* data() const { return ptr; }
            size_t size() const { return n; }
            bool empty() const { return n == 0; }
        } fileData{bytes, bytes ? size : 0};

        std::vector<uint8_t> pixels;
        int w = 0, h = 0, n = 0;
        int ok = 0;
        if (!fileData.empty()) {
            ok = stbi_info_from_memory(fileData.data(), static_cast<int>(fileData.size()),
                                       nullptr, nullptr, nullptr);
        }
        bool stbFailed = false;
        const char* stbReason = nullptr;
        if (ok) {
            stbi_uc* data = stbi_load_from_memory(fileData.data(),
                                                  static_cast<int>(fileData.size()),
                                                  &w, &h, &n, 4);
            if (data) {
                pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
                stbi_image_free(data);
            } else {
                stbFailed = true;
                stbReason = stbi_failure_reason();
            }
        } else {
            stbFailed = true;
            stbReason = stbi_failure_reason();
        }

        if (!ok || pixels.empty()) {
            // WIC fallback.
            std::vector<uint8_t> wicPixels;
            int ww = 0, wh = 0;
            if (!fileData.empty() && LoadWithWic(fileData.data(), fileData.size(), wicPixels, ww, wh)) {
                w = ww;
                h = wh;
                pixels = std::move(wicPixels);
            } else {
                if (error) {
                    std::string reason = stbReason ? stbReason : "stb failed";
                    if (!stbFailed) reason = readError;
                    *error = reason + " / WIC failed";
                }
                return false;
            }
        }

        // ---- level 0 ----
        ImageRGBA8::Level level0;
        level0.width = static_cast<uint32_t>(w);
        level0.height = static_cast<uint32_t>(h);
        level0.pixels = std::move(pixels);
        if (FitToTextureLimit(level0)) {
            out.sourceWidth = static_cast<uint32_t>(w);
            out.sourceHeight = static_cast<uint32_t>(h);
        }

        // hasAlpha + mips
        bool hasAlpha = false;
        {
            const uint8_t* src = level0.pixels.data();
            const size_t count = level0.pixels.size();
            for (size_t i = 3; i < count; i += 4) {
                if (src[i] < 255) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        uint32_t sw = level0.width;
        uint32_t sh = level0.height;
        out.mips.push_back(std::move(level0));
        out.hasAlpha = hasAlpha;

        while (sw > 1 || sh > 1) {
            uint32_t nw = std::max(1u, sw / 2);
            uint32_t nh = std::max(1u, sh / 2);
            ImageRGBA8::Level dst;
            dst.width = nw;
            dst.height = nh;
            dst.pixels.resize(static_cast<size_t>(nw) * nh * 4);
            const ImageRGBA8::Level& srcLevel = out.mips.back();
            const uint8_t* src = srcLevel.pixels.data();
            for (uint32_t y = 0; y < nh; ++y) {
                for (uint32_t x = 0; x < nw; ++x) {
                    const uint32_t sx0 = std::min(2 * x, sw - 1);
                    const uint32_t sy0 = std::min(2 * y, sh - 1);
                    const uint32_t sx1 = std::min(2 * x + 1, sw - 1);
                    const uint32_t sy1 = std::min(2 * y + 1, sh - 1);
                    uint8_t* dstPix = &dst.pixels[(static_cast<size_t>(y) * nw + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        const int sum = src[(static_cast<size_t>(sy0) * sw + sx0) * 4 + c] +
                                        src[(static_cast<size_t>(sy0) * sw + sx1) * 4 + c] +
                                        src[(static_cast<size_t>(sy1) * sw + sx0) * 4 + c] +
                                        src[(static_cast<size_t>(sy1) * sw + sx1) * 4 + c];
                        dstPix[c] = static_cast<uint8_t>((sum + 2) / 4);
                    }
                }
            }
            out.mips.push_back(std::move(dst));
            sw = nw;
            sh = nh;
        }
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

bool LoadImageWithCompressedFromMemory(const uint8_t* bytes, size_t size, ImageRGBA8& image,
                                      CompressedImage& compressed, std::string* error) {
    image = ImageRGBA8{};
    compressed = CompressedImage{};
    // The compressed form is only kept for a plain 2D block-compressed DDS; a cube map keeps the RGBA8 path (nothing
    // binds a cube as a material texture) and every other format decodes to RGBA8 as before.
    bool ok = LoadImageRGBA8FromMemory(bytes, size, image, error);
    CompressedImage candidate;
    std::string compressedError;
    if (LoadDdsCompressed(bytes, size, candidate, &compressedError)) compressed = std::move(candidate);
    // BC7 / BC4 / BC5 have no RGBA8 decoder here, so for such a file the CPU copy is genuinely absent: the blocks
    // are the only form. The load still succeeds (the GPU takes them); callers that need pixels fall back as if the
    // decode had failed.
    if (!ok && !compressed.Empty()) {
        if (error) error->clear();
        return true;
    }
    // LoadDdsLevel0's message is only meaningful when nothing usable came out of the file.
    return ok;
}

bool LoadImageWithCompressed(const std::filesystem::path& path, ImageRGBA8& image, CompressedImage& compressed,
                             std::string* error) {
    image = ImageRGBA8{};
    compressed = CompressedImage{};
    std::vector<uint8_t> fileData;
    std::string readError;
    if (!ReadWholeFile(path, fileData, &readError)) {
        if (error) *error = readError;
        return false;
    }
    return LoadImageWithCompressedFromMemory(fileData.data(), fileData.size(), image, compressed, error);
}

bool EncodePngRGBA8(uint32_t width, uint32_t height, const uint8_t* rgba,
                    uint32_t rowPitchBytes, std::vector<uint8_t>& outPng) {
    try {
        if (!rgba || width == 0 || height == 0) return false;
        outPng.clear();
        auto writeFunc = [](void* context, void* data, int size) {
            static_cast<std::vector<uint8_t>*>(context)->insert(
                static_cast<std::vector<uint8_t>*>(context)->end(),
                static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + size);
        };
        const int ok = stbi_write_png_to_func(writeFunc, &outPng, static_cast<int>(width),
                                              static_cast<int>(height), 4, rgba,
                                              static_cast<int>(rowPitchBytes));
        return (ok != 0 && !outPng.empty());
    } catch (...) {
        return false;
    }
}

bool SavePngRGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const uint8_t* rgba, uint32_t rowPitchBytes) {
    try {
        std::vector<uint8_t> png;
        if (!EncodePngRGBA8(width, height, rgba, rowPitchBytes, png)) return false;
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(png.data()),
                static_cast<std::streamsize>(png.size()));
        return static_cast<bool>(f);
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> DownscaleRgba8(uint32_t srcW, uint32_t srcH, const uint8_t* srcRgba,
                                    uint32_t maxWidth, uint32_t& outW, uint32_t& outH) {
    if (!srcRgba || srcW == 0 || srcH == 0) {
        outW = outH = 0;
        return {};
    }
    if (maxWidth == 0 || srcW <= maxWidth) {
        outW = srcW;
        outH = srcH;
        return std::vector<uint8_t>(srcRgba, srcRgba + (size_t)srcW * srcH * 4);
    }
    const float scale = static_cast<float>(maxWidth) / static_cast<float>(srcW);
    outW = maxWidth;
    outH = std::max(1u, static_cast<uint32_t>(std::round(srcH * scale)));
    std::vector<uint8_t> dst(static_cast<size_t>(outW) * outH * 4);
    for (uint32_t y = 0; y < outH; ++y) {
        const float v = ((static_cast<float>(y) + 0.5f) / static_cast<float>(outH)) * static_cast<float>(srcH) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(v)), 0, static_cast<int>(srcH) - 1);
        const int y1 = std::clamp(y0 + 1, 0, static_cast<int>(srcH) - 1);
        const float fy = v - std::floor(v);
        for (uint32_t x = 0; x < outW; ++x) {
            const float u = ((static_cast<float>(x) + 0.5f) / static_cast<float>(outW)) * static_cast<float>(srcW) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(u)), 0, static_cast<int>(srcW) - 1);
            const int x1 = std::clamp(x0 + 1, 0, static_cast<int>(srcW) - 1);
            const float fx = u - std::floor(u);

            const uint8_t* p00 = srcRgba + ((size_t)y0 * srcW + x0) * 4;
            const uint8_t* p10 = srcRgba + ((size_t)y0 * srcW + x1) * 4;
            const uint8_t* p01 = srcRgba + ((size_t)y1 * srcW + x0) * 4;
            const uint8_t* p11 = srcRgba + ((size_t)y1 * srcW + x1) * 4;

            uint8_t* d = dst.data() + ((size_t)y * outW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = p00[c] * (1.0f - fx) + p10[c] * fx;
                const float bot = p01[c] * (1.0f - fx) + p11[c] * fx;
                const float val = top * (1.0f - fy) + bot * fy;
                d[c] = static_cast<uint8_t>(std::clamp(std::round(val), 0.0f, 255.0f));
            }
        }
    }
    return dst;
}

} // namespace mmdx
