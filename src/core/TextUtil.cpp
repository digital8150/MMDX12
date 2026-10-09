#include "core/TextUtil.h"

#include <Windows.h>

namespace mmdx {

std::string WideToUtf8(std::wstring_view w) {
    std::string out;
    if (w.empty()) return out;
    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return out;
    out.resize(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view s) {
    std::wstring out;
    if (s.empty()) return out;
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                   nullptr, 0);
    if (size <= 0) return out;
    out.resize(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), size);
    return out;
}

std::string SjisToUtf8(std::string_view s) {
    // Truncate at the first '\0'.
    size_t len = s.find('\0');
    if (len != std::string_view::npos) s = s.substr(0, len);
    std::string out;
    if (s.empty()) return out;
    int wlen = MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen <= 0) return out;
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(932, 0, s.data(), static_cast<int>(s.size()), wide.data(), wlen);
    return WideToUtf8(wide);
}

std::string Utf8ToSjis(std::string_view s) {
    std::wstring wide = Utf8ToWide(s);
    std::string out;
    if (wide.empty()) return out;
    int size = WideCharToMultiByte(932, 0, wide.data(), static_cast<int>(wide.size()),
                                   nullptr, 0, "?", nullptr);
    if (size <= 0) return out;
    out.resize(static_cast<size_t>(size));
    WideCharToMultiByte(932, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), size, "?", nullptr);
    return out;
}

std::string Utf16LeToUtf8(const void* data, size_t bytes) {
    std::string out;
    if (!data || bytes < 2) return out;
    const wchar_t* units = reinterpret_cast<const wchar_t*>(data);
    size_t count = bytes / 2;
    if (count == 0) return out;
    int size = WideCharToMultiByte(CP_UTF8, 0, units, static_cast<int>(count),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return out;
    out.resize(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, units, static_cast<int>(count),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::string PathToUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.wstring());
}

std::filesystem::path Utf8ToPath(std::string_view s) {
    return std::filesystem::path(Utf8ToWide(s));
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::filesystem::path ExecutableDir() {
    std::wstring buf;
    buf.resize(1024);
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size() - 1 || buf.size() >= 32768) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

std::filesystem::path FindUpward(const std::filesystem::path& start,
                                 const std::filesystem::path& relative) {
    std::filesystem::path d = start;
    std::error_code ec;
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(d / relative, ec)) return d;
        if (d == d.parent_path()) break;
        d = d.parent_path();
    }
    return {};
}

static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const void* data, size_t bytes) {
    if (!data || bytes == 0) return {};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((bytes + 2) / 3) * 4);
    for (size_t i = 0; i < bytes; i += 3) {
        uint32_t b0 = p[i];
        uint32_t b1 = (i + 1 < bytes) ? p[i + 1] : 0;
        uint32_t b2 = (i + 2 < bytes) ? p[i + 2] : 0;
        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kBase64Chars[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Chars[(triple >> 12) & 0x3f]);
        out.push_back((i + 1 < bytes) ? kBase64Chars[(triple >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < bytes) ? kBase64Chars[triple & 0x3f] : '=');
    }
    return out;
}

std::vector<uint8_t> Base64Decode(std::string_view s) {
    auto b64val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() * 3) / 4);
    uint32_t val = 0;
    int bits = -8;
    for (char c : s) {
        if (c == '=') break;
        int d = b64val(c);
        if (d < 0) continue;
        val = (val << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace mmdx
