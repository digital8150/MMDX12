#pragma once
#include <filesystem>
#include <string>

namespace mmdx {

// Persisted as UTF-8 "key=value" lines in mmdx12.ini next to the executable.
// Unknown keys are ignored; malformed values keep the default.
struct AppSettings {
    std::string libraryPath;   // empty => auto: first existing of <exe>/library, then FindUpward(exe, "library")
    std::string nickname;
    bool vsync = true;
    int msaa = 4;              // 1, 2, 4, 8
    float renderScale = 1.0f;  // 0.5 .. 2.0
    float volume = 0.8f;       // 0 .. 1
    bool drawEdges = true;
    std::string leaderboardUrl = "https://home.codingbot.kr/api/benchmark";
    std::string lastCharacter, lastStage, lastSong;  // asset ids
    int windowWidth = 1600, windowHeight = 900;

    bool Load(const std::filesystem::path& file);   // false if missing/unreadable (defaults kept)
    bool Save(const std::filesystem::path& file) const;
};

} // namespace mmdx
