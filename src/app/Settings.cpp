#include "app/Settings.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace mmdx {

namespace {

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool ParseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "true") { out = true; return true; }
    if (v == "0" || v == "false") { out = false; return true; }
    return false;
}

bool ParseInt(const std::string& v, int& out) {
    int value = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), value);
    if (ec != std::errc() || p != v.data() + v.size()) {
        char* end = nullptr;
        long l = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0') return false;
        value = (int)l;
    }
    out = value;
    return true;
}

bool ParseFloat(const std::string& v, float& out) {
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return false;
    out = f;
    return true;
}

} // namespace

bool AppSettings::Load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));

        bool b;
        int i;
        float f;
        if (key == "libraryPath") libraryPath = value;
        else if (key == "nickname") nickname = value;
        else if (key == "vsync" && ParseBool(value, b)) vsync = b;
        else if (key == "msaa" && ParseInt(value, i)) msaa = i;
        else if (key == "renderScale" && ParseFloat(value, f)) renderScale = f;
        else if (key == "volume" && ParseFloat(value, f)) volume = f;
        else if (key == "drawEdges" && ParseBool(value, b)) drawEdges = b;
        else if (key == "leaderboardUrl") leaderboardUrl = value;
        else if (key == "lastCharacter") lastCharacter = value;
        else if (key == "lastStage") lastStage = value;
        else if (key == "lastSong") lastSong = value;
        else if (key == "windowWidth" && ParseInt(value, i)) windowWidth = i;
        else if (key == "windowHeight" && ParseInt(value, i)) windowHeight = i;
    }

    // Clamp msaa to {1,2,4,8}, else 4.
    if (msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) msaa = 4;
    renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    volume = std::clamp(volume, 0.0f, 1.0f);
    windowWidth = std::clamp(windowWidth, 640, 7680);
    windowHeight = std::clamp(windowHeight, 360, 4320);
    return true;
}

bool AppSettings::Save(const std::filesystem::path& file) const {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"w") != 0 || !f) return false;

    std::fprintf(f, "libraryPath=%s\n", libraryPath.c_str());
    std::fprintf(f, "nickname=%s\n", nickname.c_str());
    std::fprintf(f, "vsync=%d\n", vsync ? 1 : 0);
    std::fprintf(f, "msaa=%d\n", msaa);
    std::fprintf(f, "renderScale=%.3f\n", renderScale);
    std::fprintf(f, "volume=%.3f\n", volume);
    std::fprintf(f, "drawEdges=%d\n", drawEdges ? 1 : 0);
    std::fprintf(f, "leaderboardUrl=%s\n", leaderboardUrl.c_str());
    std::fprintf(f, "lastCharacter=%s\n", lastCharacter.c_str());
    std::fprintf(f, "lastStage=%s\n", lastStage.c_str());
    std::fprintf(f, "lastSong=%s\n", lastSong.c_str());
    std::fprintf(f, "windowWidth=%d\n", windowWidth);
    std::fprintf(f, "windowHeight=%d\n", windowHeight);
    std::fclose(f);
    return true;
}

} // namespace mmdx
