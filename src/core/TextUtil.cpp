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

} // namespace mmdx
