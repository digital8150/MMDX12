#include "asset/ImageLoader.h"
#include "asset/BinaryReader.h"

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
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace mmdx {

namespace {

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

        out.mips.push_back(std::move(level0));
        out.hasAlpha = hasAlpha;

        uint32_t sw = static_cast<uint32_t>(w);
        uint32_t sh = static_cast<uint32_t>(h);
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

bool SavePngRGBA8(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const uint8_t* rgba, uint32_t rowPitchBytes) {
    try {
        if (!rgba || width == 0 || height == 0) return false;
        auto writeFunc = [](void* context, void* data, int size) {
            static_cast<std::vector<uint8_t>*>(context)->insert(
                static_cast<std::vector<uint8_t>*>(context)->end(),
                static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + size);
        };
        std::vector<uint8_t> png;
        const int ok = stbi_write_png_to_func(writeFunc, &png, static_cast<int>(width),
                                              static_cast<int>(height), 4, rgba,
                                              static_cast<int>(rowPitchBytes));
        if (!ok || png.empty()) return false;
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(png.data()),
                static_cast<std::streamsize>(png.size()));
        return static_cast<bool>(f);
    } catch (...) {
        return false;
    }
}

} // namespace mmdx
