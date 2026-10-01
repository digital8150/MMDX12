#pragma once
#include <string>
#include <vector>

namespace mmdx {

enum class LogLevel { Info, Warn, Error };

// printf-style, UTF-8. Thread-safe. Writes to OutputDebugStringW, stdout (if a console
// is attached), "mmdx12.log" next to the executable (truncated on first write of the
// process), and an in-memory ring buffer of the last 512 lines.
void LogWrite(LogLevel level, const char* fmt, ...);

// Last `maxLines` lines from the ring buffer, oldest first. Each line is prefixed
// "[I] ", "[W] " or "[E] ".
std::vector<std::string> LogRecentLines(size_t maxLines);

} // namespace mmdx

#define LOG_INFO(...)  ::mmdx::LogWrite(::mmdx::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  ::mmdx::LogWrite(::mmdx::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::mmdx::LogWrite(::mmdx::LogLevel::Error, __VA_ARGS__)
