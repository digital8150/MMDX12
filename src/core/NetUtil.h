#pragma once
// Shared networking / file helpers: the updater and the shader pack store use them.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mmdx::net {

// WINHTTP_OPTION_DECOMPRESSION for the request. None: as before (no gzip); AcceptGzip: transparently
// decompresses a server response that is gzip-encoded (Content-Encoding: gzip). The delta updater
// asks for AcceptGzip; the zip download / shader pack store keep the default.
enum class HttpOption { None, AcceptGzip };

// https/http GET with a size cap; true with the body on 200, false + error otherwise.
bool HttpGet(const std::string& url, std::string& body, std::string& error, uint64_t maxBytes,
             HttpOption option = HttpOption::None);
// A source the app accepts for testing: file: URLs or plain local paths (not http/https).
bool IsLocalSource(const std::string& url);
std::filesystem::path LocalPathOf(const std::string& source);
// Downloads a url to a file (https/http), or copies a local / file: source. Reports bytes done and honours the
// cancel flag between chunks (both may be null).
bool DownloadToFile(const std::string& url, const std::filesystem::path& dest, std::atomic<uint64_t>* done,
                    std::atomic<bool>* cancel, std::string& error, HttpOption option = HttpOption::None);

// --- resumable download (stage gallery) ---------------------------------------------------------------
// A download that survives a cancel / crash: it writes "<dest>.part" and, when that file already holds bytes,
// asks the server for the rest ("Range: bytes=<size>-"). A 206 appends, a 200 means the server sent the whole
// file again and the .part restarts from 0. The .part is renamed onto `dest` only when the transfer completed, so
// a leftover "<dest>.part" is always safe to resume. `total` (may be null) receives the expected body size when
// the server reports one. Local / file: sources just copy (and drop a stale .part).
enum class ResumeAction { Start, Append, Fail };
// The .part path for `dest` ("<dest>.part").
std::filesystem::path PartPathFor(const std::filesystem::path& dest);
// The request header a .part of `partBytes` asks for ("" = no header: fetch from 0).
std::string ResumeRangeHeader(uint64_t partBytes);
// How to treat a response: 206 appends at the requested offset, 200 / 416 (the server ignored or refused the
// range) restart from 0, anything else fails. Exposed for tests.
ResumeAction ResumeDecision(int httpStatus, uint64_t requestOffset);
bool DownloadToFileResumable(const std::string& url, const std::filesystem::path& dest, std::atomic<uint64_t>* done,
                             std::atomic<uint64_t>* total, std::atomic<bool>* cancel, std::string& error);
// SHA-256 of a file as lower-case hex (progress in bytes, may be null). Empty on failure.
std::string Sha256OfFile(const std::filesystem::path& file, std::atomic<uint64_t>* progress);
std::string Sha256OfBytes(std::string_view bytes);
// Extracts a zip with Windows' tar.exe (bsdtar, Windows 10 1803+), hidden.
bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& destDir, std::string& error);
// Dotted version compare ("1.10.0" > "1.9", leading v ignored): -1, 0, 1.
int CompareVersions(const std::string& a, const std::string& b);

} // namespace mmdx::net
