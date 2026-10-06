// Shared networking / file helpers (updater, shader pack store): HTTP GET, downloads, SHA-256, zip extraction,
// version compare. Moved out of app/Updater.cpp unchanged.
#include "core/NetUtil.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/Log.h"
#include "core/TextUtil.h"

#pragma comment(lib, "winhttp.lib")

namespace mmdx::net {

namespace fs = std::filesystem;

namespace {

std::string Trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool FileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::string Hex(const uint8_t* data, size_t bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string s(bytes * 2, '0');
    for (size_t i = 0; i < bytes; ++i) {
        s[i * 2] = kDigits[data[i] >> 4];
        s[i * 2 + 1] = kDigits[data[i] & 0xF];
    }
    return s;
}


struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64] = {};
    size_t bufLen = 0;
    uint64_t total = 0;

    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 |
                   (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* p, size_t n) {
        total += n;
        if (bufLen) {
            const size_t take = std::min(n, (size_t)64 - bufLen);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take;
            p += take;
            n -= take;
            if (bufLen == 64) {
                Block(buf);
                bufLen = 0;
            }
        }
        while (n >= 64) {
            Block(p);
            p += 64;
            n -= 64;
        }
        if (n) {
            std::memcpy(buf, p, n);
            bufLen = n;
        }
    }

    std::string Finish() {
        const uint64_t bits = total * 8;
        const uint8_t pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (bufLen != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - i * 8));
        Update(len, 8);
        uint8_t out[32];
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
        return Hex(out, 32);
    }
};

// SHA-256 of a file, streamed (progress in bytes for the verify strip). Empty string on failure.
} // namespace

std::string Sha256OfFile(const fs::path& file, std::atomic<uint64_t>* progress) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha256 s;
    char chunk[1 << 16];
    while (in) {
        in.read(chunk, sizeof(chunk));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        s.Update((const uint8_t*)chunk, (size_t)got);
        if (progress) progress->fetch_add((uint64_t)got, std::memory_order_relaxed);
    }
    return s.Finish();
}


// --- version compare -----------------------------------------------------------------------

int CompareVersions(const std::string& a, const std::string& b) {
    const auto parts = [](const std::string& v) {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : v) {
            if (c == '.' || c == '-') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        out.push_back(cur);
        return out;
    };
    const auto pa = parts(!a.empty() && (a[0] == 'v' || a[0] == 'V') ? a.substr(1) : a);
    const auto pb = parts(!b.empty() && (b[0] == 'v' || b[0] == 'V') ? b.substr(1) : b);
    const size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& sa = i < pa.size() ? pa[i] : std::string();
        const std::string& sb = i < pb.size() ? pb[i] : std::string();
        if (sa == sb) continue;
        const bool numa = !sa.empty() && sa.find_first_not_of("0123456789") == std::string::npos;
        const bool numb = !sb.empty() && sb.find_first_not_of("0123456789") == std::string::npos;
        if (numa && numb) {
            const unsigned long long va = std::stoull(sa);
            const unsigned long long vb = std::stoull(sb);
            if (va != vb) return va < vb ? -1 : 1;
        } else {
            const int c = sa.compare(sb);
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

// --- feed / download over WinHTTP ----------------------------------------------------------

// https/http GET with a size cap; 200 with the body, false + error otherwise.
bool HttpGet(const std::string& url, std::string& body, std::string& error, uint64_t maxBytes) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    const std::wstring wide = Utf8ToWide(url);
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &uc)) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool https = _wcsicmp(std::wstring(uc.lpszScheme, uc.dwSchemeLength).c_str(), L"https") == 0;

    struct H {
        HINTERNET h = nullptr;
        ~H() { if (h) WinHttpCloseHandle(h); }
        explicit operator bool() const { return h != nullptr; }
    };
    H session(WinHttpOpen(L"MMDX12", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 30000);
    H connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    H request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
        if ((uint64_t)body.size() + available > maxBytes) {
            error = "response too large";
            return false;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + offset, available, &read) || read == 0) break;
        body.resize(offset + read);
    }
    return true;
}

// A feed/zip source the app accepts for testing: file: URLs or plain local paths.
bool IsLocalSource(const std::string& url) {
    if (url.rfind("file:", 0) == 0) return true;
    return !(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0);
}

fs::path LocalPathOf(const std::string& source) {
    std::string path = source;
    if (path.rfind("file:", 0) == 0) {
        path = path.substr(5);
        // file:///C:/dir -> C:\dir ; file://host/share -> \\host\share ; file://C:/dir -> C:\dir
        if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
            path = path.substr(2);
            if (path.size() >= 2 && path[1] == ':') {
                // file://C:/... (two slashes, drive letter): drop nothing more
            } else if (!path.empty() && path[0] == '/') {
                path = path.substr(1);  // file:///C:/... (three slashes)
            } else {
                path = "\\\\" + path;   // file://host/... (UNC)
            }
        }
        for (char& c : path)
            if (c == '/') c = '\\';
    }
    return Utf8ToPath(path);
}

// Download a url to a file (https/http), or copy a local/file: source. Reports bytes done and
// honours the cancel flag between chunks.
bool DownloadToFile(const std::string& url, const fs::path& dest, std::atomic<uint64_t>* done,
                    std::atomic<bool>* cancel, std::string& error) {
    if (IsLocalSource(url)) {
        const fs::path src = LocalPathOf(url);
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy " + PathToUtf8(src) + " (" + ec.message() + ")";
            return false;
        }
        if (done) done->store(fs::file_size(dest, ec), std::memory_order_relaxed);
        return true;
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    const std::wstring wide = Utf8ToWide(url);
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &uc)) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool https = _wcsicmp(std::wstring(uc.lpszScheme, uc.dwSchemeLength).c_str(), L"https") == 0;

    struct H {
        HINTERNET h = nullptr;
        ~H() { if (h) WinHttpCloseHandle(h); }
        explicit operator bool() const { return h != nullptr; }
    };
    H session(WinHttpOpen(L"MMDX12", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 60000);
    H connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    H request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + PathToUtf8(dest);
        return false;
    }
    char chunk[1 << 16];
    bool failed = false;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            failed = true;
            break;
        }
        if (available == 0) break;
        // Read in <= chunk-sized pieces so the cancel flag is checked often.
        while (available > 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                error = "cancelled";
                return false;
            }
            const DWORD want = (DWORD)std::min<uint64_t>(available, sizeof(chunk));
            DWORD got = 0;
            if (!WinHttpReadData(request.h, chunk, want, &got) || got == 0) {
                available = 0;
                failed = true;
                break;
            }
            out.write(chunk, (std::streamsize)got);
            if (!out) {
                error = "cannot write " + PathToUtf8(dest);
                return false;
            }
            if (done) done->fetch_add(got, std::memory_order_relaxed);
            available -= got;
        }
        if (failed) break;
    }
    out.close();
    if (failed && !out) {
        error = "network error (download cut short)";
        return false;
    }
    return true;
}

// Extract a zip with the Windows built-in tar.exe (bsdtar, System32, ships with Windows 10
// 1803+; it reads zip archives natively). Hidden: CREATE_NO_WINDOW, output piped away.
bool ExtractZip(const fs::path& zip, const fs::path& destDir, std::string& error) {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const std::wstring tar = std::wstring(sysDir) + L"\\tar.exe";
    if (!FileExists(tar)) {
        error = "tar.exe not found (Windows 10 1803+ required)";
        return false;
    }

    std::error_code ec;
    fs::remove_all(destDir, ec);  // tar refuses to overwrite: extract into a fresh directory
    fs::create_directories(destDir, ec);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        error = "pipe creation failed";
        return false;
    }
    SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0);  // keep the write end out of the child

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + destDir.wstring() + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                   destDir.wstring().c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        error = "tar.exe could not be started (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    // Drain the pipe so tar cannot block on a full buffer, then wait for the result.
    std::string output;
    char buf[512];
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) || read == 0) break;
        output.append(buf, read);
    }
    CloseHandle(readPipe);
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        error = "tar.exe failed (" + std::to_string(exitCode) + ")" +
                (output.empty() ? std::string() : ": " + Trimmed(output).substr(0, 200));
        return false;
    }
    return true;
}


} // namespace mmdx::net
