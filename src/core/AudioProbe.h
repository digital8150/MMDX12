#pragma once
#include <filesystem>

namespace mmdx {

// Length of an audio file (wav/mp3/flac/ogg) in seconds, 0 when it cannot be decoded.
double AudioDurationSec(const std::filesystem::path& path);

} // namespace mmdx
