#pragma once
// Shared networking / file helpers: the updater and the shader pack store use them.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mmdx::net {

// https/http GET with a size cap; true with the body on 200, false + error otherwise.
bool HttpGet(const std::string& url, std::string& body, std::string& error, uint64_t maxBytes);
// A source the app accepts for testing: file: URLs or plain local paths (not http/https).
bool IsLocalSource(const std::string& url);
std::filesystem::path LocalPathOf(const std::string& source);
// Downloads a url to a file (https/http), or copies a local / file: source. Reports bytes done and honours the
// cancel flag between chunks (both may be null).
bool DownloadToFile(const std::string& url, const std::filesystem::path& dest, std::atomic<uint64_t>* done,
                    std::atomic<bool>* cancel, std::string& error);
// SHA-256 of a file as lower-case hex (progress in bytes, may be null). Empty on failure.
std::string Sha256OfFile(const std::filesystem::path& file, std::atomic<uint64_t>* progress);
// Extracts a zip with Windows' tar.exe (bsdtar, Windows 10 1803+), hidden.
bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& destDir, std::string& error);
// Dotted version compare ("1.10.0" > "1.9", leading v ignored): -1, 0, 1.
int CompareVersions(const std::string& a, const std::string& b);

} // namespace mmdx::net
