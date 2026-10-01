#include "core/Log.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace mmdx {

namespace {

constexpr size_t kMaxRingLines = 512;

std::mutex g_logMutex;
std::deque<std::string> g_ring;
FILE* g_logFile = nullptr;

void WriteFileLocked(const std::string& line) {
    if (!g_logFile) {
        g_logFile = _wfopen((ExecutableDir() / L"mmdx12.log").c_str(), L"wb");
        if (!g_logFile) return;
    }
    fputs(line.c_str(), g_logFile);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

} // namespace

void LogWrite(LogLevel level, const char* fmt, ...) {
    const char* prefix = "[I] ";
    if (level == LogLevel::Warn) prefix = "[W] ";
    else if (level == LogLevel::Error) prefix = "[E] ";

    // Two-pass vsnprintf sizing.
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    if (n < 0) return;

    std::string line(prefix);
    line.resize(line.size() + static_cast<size_t>(n));
    va_start(args, fmt);
    vsnprintf(line.data() + 4, static_cast<size_t>(n) + 1, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_ring.push_back(line);
    while (g_ring.size() > kMaxRingLines) g_ring.pop_front();

    WriteFileLocked(line);
    OutputDebugStringW(Utf8ToWide(line + "\n").c_str());
    fputs((line + "\n").c_str(), stdout);
    fflush(stdout);
}

std::vector<std::string> LogRecentLines(size_t maxLines) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::vector<std::string> out;
    if (maxLines == 0 || g_ring.empty()) return out;
    size_t start = g_ring.size() > maxLines ? g_ring.size() - maxLines : 0;
    for (size_t i = start; i < g_ring.size(); ++i) out.push_back(g_ring[i]);
    return out;
}

} // namespace mmdx
