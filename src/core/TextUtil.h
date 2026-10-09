#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace mmdx {

std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view s);

// Shift-JIS (code page 932) -> UTF-8. Input is truncated at the first '\0'.
// Invalid sequences are replaced by the converter's default behaviour (no throw).
std::string SjisToUtf8(std::string_view s);

// UTF-8 -> Shift-JIS (cp932). Unmappable characters become '?'.
std::string Utf8ToSjis(std::string_view s);

// UTF-16LE bytes -> UTF-8. `bytes` may be odd; the trailing byte is ignored.
std::string Utf16LeToUtf8(const void* data, size_t bytes);

std::string PathToUtf8(const std::filesystem::path& p);
std::filesystem::path Utf8ToPath(std::string_view s);

// ASCII-only lowercase (multibyte UTF-8 bytes untouched).
std::string ToLowerAscii(std::string s);

// Directory containing the running executable.
std::filesystem::path ExecutableDir();

// Starting at `start` and walking up through its parents (at most 6 levels), returns the
// first directory D such that D / relative exists. Returns empty path if not found.
std::filesystem::path FindUpward(const std::filesystem::path& start, const std::filesystem::path& relative);

// Base64 encoding / decoding
std::string Base64Encode(const void* data, size_t bytes);
std::vector<uint8_t> Base64Decode(std::string_view s);

} // namespace mmdx
