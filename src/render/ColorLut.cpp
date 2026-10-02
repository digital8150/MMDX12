// Colour LUTs (see ColorLut.h): six procedural built-in looks and an Adobe/Resolve
// .cube loader resampled into a (N*N) x N RGBA8 strip.
#include "render/ColorLut.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace mmdx {

namespace {

struct Rgb { float r, g, b; };

float Luma(const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

Rgb Sat(const Rgb& c, float s) {
    const float l = Luma(c);
    return {l + (c.r - l) * s, l + (c.g - l) * s, l + (c.b - l) * s};
}

// S-curve: k > 0 increases contrast.
float S(float x, float k) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x + k * x * (1.0f - x) * (2.0f * x - 1.0f);
}

Rgb S(const Rgb& c, float k) { return {S(c.r, k), S(c.g, k), S(c.b, k)}; }

Rgb Fade(const Rgb& c, float lo, float hi) {
    return {lo + c.r * (hi - lo), lo + c.g * (hi - lo), lo + c.b * (hi - lo)};
}

float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

using LookFn = Rgb (*)(Rgb);

Rgb Cinematic(Rgb c) {
    c = S(c, 0.6f);
    const float l = Luma(c);
    const float t = Smoothstep(0.15f, 0.85f, l);
    // teal shadows, orange highlights
    const Rgb shadowTint = {-0.03f, 0.02f, 0.05f}, highlightTint = {0.06f, 0.015f, -0.05f};
    c.r += shadowTint.r + (highlightTint.r - shadowTint.r) * t;
    c.g += shadowTint.g + (highlightTint.g - shadowTint.g) * t;
    c.b += shadowTint.b + (highlightTint.b - shadowTint.b) * t;
    return Sat(c, 0.95f);
}

Rgb WarmFilm(Rgb c) {
    c.r *= 1.06f;
    c.b *= 0.9f;
    c = Fade(c, 0.04f, 0.98f);
    c = S(c, 0.3f);
    return Sat(c, 1.05f);
}

Rgb CoolNight(Rgb c) {
    c.r *= 0.9f;
    c.g *= 0.98f;
    c.b *= 1.1f;
    c = S(c, 0.25f);
    c = Sat(c, 0.85f);
    const float l = Luma(c);
    c.g += 0.01f * (1.0f - l);
    c.b += 0.035f * (1.0f - l);
    return c;
}

Rgb AnimeVivid(Rgb c) {
    c = Sat(c, 1.3f);
    c = S(c, 0.25f);
    c.r = std::pow(std::clamp(c.r, 0.0f, 1.0f), 0.95f);
    c.g = std::pow(std::clamp(c.g, 0.0f, 1.0f), 0.95f);
    c.b = std::pow(std::clamp(c.b, 0.0f, 1.0f), 0.95f);
    return c;
}

Rgb Vintage(Rgb c) {
    c = Sat(c, 0.7f);
    c = Fade(c, 0.06f, 0.94f);
    const float l = Luma(c);
    c.r += 0.04f * l - 0.01f * (1.0f - l);
    c.g += 0.03f * l + 0.015f * (1.0f - l);
    c.b += -0.03f * l + 0.01f * (1.0f - l);
    return c;
}

Rgb Mono(Rgb c) {
    const float l = Luma(S(c, 0.4f));
    return {l * 1.02f, l, l * 0.96f};
}

const LookFn kLooks[] = {Cinematic, WarmFilm, CoolNight, AnimeVivid, Vintage, Mono};

// F: (r, g, b) in [0,1] -> Rgb. One mip level, layout per ColorLut.h: texel (x, y) =
// LUT(r = (x % N)/(N-1), g = y/(N-1), b = (x / N)/(N-1)); loops b (slowest), g, r (fastest).
template <typename F> void FillStrip(ImageRGBA8& img, F f) {
    constexpr uint32_t n = kColorLutSize;
    img.mips.clear();
    ImageRGBA8::Level& lv = img.mips.emplace_back();
    lv.width = n * n;
    lv.height = n;
    lv.pixels.resize(size_t(lv.width) * lv.height * 4);
    for (uint32_t b = 0; b < n; ++b) {
        for (uint32_t g = 0; g < n; ++g) {
            for (uint32_t r = 0; r < n; ++r) {
                const Rgb c = f(r / float(n - 1), g / float(n - 1), b / float(n - 1));
                uint8_t* px = &lv.pixels[(size_t(g * lv.width) + b * n + r) * 4];
                px[0] = uint8_t(lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
                px[1] = uint8_t(lround(std::clamp(c.g, 0.0f, 1.0f) * 255.0f));
                px[2] = uint8_t(lround(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
                px[3] = 255;
            }
        }
    }
    img.hasAlpha = false;
}

} // namespace

std::vector<ColorLutEntry> ListColorLuts(const std::vector<std::filesystem::path>& dirs) {
    static const char* kSuffixes[] = {"cinematic", "warm_film", "cool_night", "anime_vivid", "vintage", "mono"};
    static const char* kNames[] = {"시네마틱", "따뜻한 필름", "차가운 밤", "애니 비비드", "빈티지", "흑백"};

    std::vector<ColorLutEntry> out;
    for (int i = 0; i < 6; ++i) {
        ColorLutEntry e;
        e.id = std::string("builtin:") + kSuffixes[i];
        e.displayName = kNames[i];
        e.builtin = i;
        out.push_back(std::move(e));
    }

    std::vector<std::filesystem::path> files;
    for (const std::filesystem::path& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            std::error_code fec;
            if (ec) break;
            if (!it->is_regular_file(fec) || fec) continue;
            if (ToLowerAscii(it->path().extension().string()) == ".cube") files.push_back(it->path());
        }
    }
    std::stable_sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return ToLowerAscii(PathToUtf8(a.filename())) < ToLowerAscii(PathToUtf8(b.filename()));
    });

    std::vector<std::string> seen;
    for (const std::filesystem::path& f : files) {
        const std::string name = ToLowerAscii(PathToUtf8(f.filename()));
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        ColorLutEntry e;
        e.id = "file:" + PathToUtf8(f.filename());
        e.displayName = PathToUtf8(f.stem());
        e.file = f;
        out.push_back(std::move(e));
    }
    return out;
}

bool BuildColorLut(const ColorLutEntry& entry, ImageRGBA8& strip) {
    strip = {};
    if (entry.builtin >= 0 && entry.builtin < (int)std::size(kLooks)) {
        FillStrip(strip, [&](float r, float g, float b) { return kLooks[entry.builtin]({r, g, b}); });
        return true;
    }
    if (entry.file.empty()) {
        LOG_WARN("color LUT: builtin index %d out of range and no .cube file given", entry.builtin);
        return false;
    }
    const std::string pathUtf8 = PathToUtf8(entry.file);
    const char* path = pathUtf8.c_str();

    std::ifstream in(entry.file, std::ios::binary);
    if (!in) {
        LOG_WARN("color LUT: cannot open %s", path);
        return false;
    }

    int n = 0;
    float dmin[3] = {0.0f, 0.0f, 0.0f}, dmax[3] = {1.0f, 1.0f, 1.0f};
    std::vector<float> data;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        const size_t e = line.find_last_not_of(" \t");
        const std::string s = line.substr(b, e - b + 1);
        if (s[0] == '#') continue;

        std::istringstream ss(s);
        std::string kw;
        ss >> kw;
        if (kw == "TITLE") {
            // ignore
        } else if (kw == "LUT_3D_SIZE") {
            int v = 0;
            ss >> v;
            if (v < 2 || v > 256) {
                LOG_WARN("color LUT: %s: invalid LUT_3D_SIZE %d", path, v);
                return false;
            }
            n = v;
        } else if (kw == "DOMAIN_MIN" || kw == "DOMAIN_MAX") {
            float v[3] = {0, 0, 0};
            ss >> v[0] >> v[1] >> v[2];
            float* d = kw == "DOMAIN_MIN" ? dmin : dmax;
            for (int k = 0; k < 3; ++k) d[k] = v[k];
        } else if (kw == "LUT_1D_SIZE") {
            LOG_WARN("color LUT: %s: 1D LUTs are not supported", path);
            return false;
        } else if (kw == "LUT_3D_INPUT_RANGE") {
            float lo = 0.0f, hi = 0.0f;
            ss >> lo >> hi;
            for (int k = 0; k < 3; ++k) {
                dmin[k] = lo;
                dmax[k] = hi;
            }
        } else if (s[0] >= 'A' && s[0] <= 'z') {
            // unknown keyword: ignore
        } else {
            // Data line (three floats, strtof; the C locale is the default in this app):
            // r varies fastest (index = r + g*n + b*n*n), which push order gives directly.
            const char* p = s.c_str();
            for (int k = 0; k < 3; ++k) {
                char* end = nullptr;
                data.push_back(std::strtof(p, &end));
                p = end;
            }
        }
    }

    if (n == 0 || data.size() != size_t(n) * n * n * 3) {
        LOG_WARN("color LUT: %s: expected %d data lines, got %zu", path, n * n * n, data.size() / 3);
        return false;
    }

    // Trilinear resample of the .cube data into the strip layout.
    FillStrip(strip, [&](float r, float g, float b) {
        const float v[3] = {r, g, b};
        int i0[3], i1[3];
        float ft[3];
        for (int k = 0; k < 3; ++k) {
            const float u = std::clamp((v[k] - dmin[k]) / (dmax[k] - dmin[k]), 0.0f, 1.0f) * float(n - 1);
            i0[k] = std::min(int(u), n - 1);
            i1[k] = std::min(i0[k] + 1, n - 1);
            ft[k] = u - float(i0[k]);
        }
        const auto value = [&](int cr, int cg, int cb, int ch) {
            const size_t idx = (size_t(cb) * n + cg) * n + cr;
            return data[idx * 3 + ch];
        };
        Rgb outC = {0.0f, 0.0f, 0.0f};
        for (int cb = 0; cb <= 1; ++cb) {
            for (int cg = 0; cg <= 1; ++cg) {
                for (int cr = 0; cr <= 1; ++cr) {
                    const float w = (cr ? ft[0] : 1.0f - ft[0]) * (cg ? ft[1] : 1.0f - ft[1]) *
                                    (cb ? ft[2] : 1.0f - ft[2]);
                    outC.r += w * value(cr ? i1[0] : i0[0], cg ? i1[1] : i0[1], cb ? i1[2] : i0[2], 0);
                    outC.g += w * value(cr ? i1[0] : i0[0], cg ? i1[1] : i0[1], cb ? i1[2] : i0[2], 1);
                    outC.b += w * value(cr ? i1[0] : i0[0], cg ? i1[1] : i0[1], cb ? i1[2] : i0[2], 2);
                }
            }
        }
        return outC;
    });
    LOG_INFO("color LUT: %s (%d^3)", PathToUtf8(entry.file.stem()).c_str(), n);
    return true;
}

} // namespace mmdx