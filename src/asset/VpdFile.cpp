#include "asset/VpdFile.h"
#include "asset/BinaryReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace mmdx {

namespace {

// Strips NUL bytes (SjisToUtf8 truncates at the first one).
void StripNuls(std::string& s) {
    size_t write = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\0') s[write++] = s[i];
    }
    s.resize(write);
}

// Removes trailing '\r' characters.
void RTrimCR(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

// Removes the "//" comment part of a VPD line, if present.
void StripComment(std::string& s) {
    const size_t pos = s.find("//");
    if (pos != std::string::npos) {
        s.resize(pos);
    }
    // Trim trailing whitespace too.
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
}

// Trims leading/trailing whitespace (space, tab).
std::string Trim(std::string s) {
    size_t from = 0;
    while (from < s.size() && (s[from] == ' ' || s[from] == '\t')) ++from;
    size_t to = s.size();
    while (to > from && (s[to - 1] == ' ' || s[to - 1] == '\t')) --to;
    return s.substr(from, to - from);
}

// True if `s` starts with "Bone" or "Morph" followed by only digits, then '{'.
bool MatchBlockHeader(const std::string& s, const char* kind, std::string& name) {
    const size_t kindLen = strlen(kind);
    if (s.compare(0, kindLen, kind) != 0) return false;
    size_t i = kindLen;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i >= s.size() || s[i] != '{') return false;
    name = Trim(s.substr(i + 1));
    return true;
}

// Splits "a,b,c" (with an optional trailing ';') into float values.
// Returns the number of values parsed (up to maxValues).
int ParseFloats(const std::string& line, float* values, int maxValues) {
    std::string body = line;
    StripComment(body);
    if (!body.empty() && body.back() == ';') body.pop_back();
    int count = 0;
    size_t from = 0;
    while (count < maxValues) {
        size_t comma = body.find(',', from);
        std::string token = body.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
        token = Trim(token);
        if (token.empty()) break;
        const char* begin = token.c_str();
        char* end = nullptr;
        const float v = std::strtof(begin, &end);
        if (end == begin) break;
        values[count++] = v;
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    return count;
}

} // namespace

bool LoadVpd(const std::filesystem::path& path, VpdPose& out, std::string* error) {
    try {
        out = VpdPose{};
        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData, error)) return false;

        std::string text(reinterpret_cast<const char*>(fileData.data()), fileData.size());
        StripNuls(text);
        text = SjisToUtf8(text);

        // Normalise CRLF / lone LF into a single vector of lines.
        std::vector<std::string> lines;
        size_t from = 0;
        while (from <= text.size()) {
            size_t nl = text.find('\n', from);
            std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            RTrimCR(line);
            lines.push_back(std::move(line));
            if (nl == std::string::npos) break;
            from = nl + 1;
        }

        if (lines.empty() ||
            lines[0].rfind("Vocaloid Pose Data file", 0) != 0) {
            if (error) *error = "not a VPD file";
            return false;
        }

        // First line ending with ';' after comment stripping, containing ".osm" -> modelName.
        for (size_t i = 1; i < lines.size(); ++i) {
            std::string line = lines[i];
            StripComment(line);
            if (!line.empty() && line.back() == ';') {
                const size_t osm = line.find(".osm");
                if (osm != std::string::npos) {
                    out.modelName = Trim(line.substr(0, osm));
                }
                break;
            }
        }

        enum class Block { None, Bone, Morph };
        Block block = Block::None;
        VpdBone bone;
        VpdMorph morph;
        int boneValuesSeen = 0;
        bool morphWeightSeen = false;
        for (size_t i = 1; i < lines.size(); ++i) {
            std::string line = lines[i];
            std::string name;
            if (MatchBlockHeader(line, "Bone", name)) {
                if (block == Block::Bone) {
                    // Previous bone block was never closed; keep what we have.
                    out.bones.push_back(std::move(bone));
                    bone = VpdBone{};
                }
                block = Block::Bone;
                bone = VpdBone{};
                bone.name = name;
                boneValuesSeen = 0;
            } else if (MatchBlockHeader(line, "Morph", name)) {
                if (block == Block::Bone) {
                    out.bones.push_back(std::move(bone));
                    bone = VpdBone{};
                }
                block = Block::Morph;
                morph = VpdMorph{};
                morph.name = name;
                morphWeightSeen = false;
            } else if (line == "}") {
                if (block == Block::Bone) {
                    out.bones.push_back(std::move(bone));
                    bone = VpdBone{};
                } else if (block == Block::Morph) {
                    out.morphs.push_back(std::move(morph));
                    morph = VpdMorph{};
                }
                block = Block::None;
            } else if (line.find_first_not_of(" \t\r") != std::string::npos) {
                // Non-empty value line.
                std::string body = line;
                StripComment(body);
                if (body.empty() || body == ";") continue;
                if (block == Block::Bone) {
                    float v[4] = {0, 0, 0, 0};
                    const int n = ParseFloats(body, v, 4);
                    if (boneValuesSeen == 0) {
                        if (n >= 3) {
                            bone.translation = DirectX::XMFLOAT3{v[0], v[1], v[2]};
                            boneValuesSeen = 1;
                        } else {
                            LOG_WARN("VPD %s: malformed translation in bone block %s",
                                     out.modelName.c_str(), bone.name.c_str());
                        }
                    } else if (boneValuesSeen == 1) {
                        if (n >= 4) {
                            bone.rotation = DirectX::XMFLOAT4{v[0], v[1], v[2], v[3]};
                            boneValuesSeen = 2;
                        } else {
                            LOG_WARN("VPD %s: malformed rotation in bone block %s",
                                     out.modelName.c_str(), bone.name.c_str());
                        }
                    }
                    // Extra value lines inside a bone block are ignored.
                } else if (block == Block::Morph) {
                    if (!morphWeightSeen) {
                        float v[4] = {0, 0, 0, 0};
                        const int n = ParseFloats(body, v, 4);
                        if (n >= 1) {
                            morph.weight = v[0];
                            morphWeightSeen = true;
                        } else {
                            LOG_WARN("VPD %s: malformed morph block for %s",
                                     out.modelName.c_str(), morph.name.c_str());
                        }
                    }
                }
            }
        }
        if (block == Block::Bone) {
            out.bones.push_back(std::move(bone));
        }
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

bool SaveVpd(const std::filesystem::path& path, const VpdPose& pose, std::string* error) {
    try {
        std::string outUtf8 = "Vocaloid Pose Data file\r\n";
        outUtf8 += "\r\n";
        outUtf8 += pose.modelName + ".osm;\t\t" + "// 親ファイル名\r\n";
        outUtf8 += std::to_string(pose.bones.size()) + ";\t\t\t// 総ポーズ数\r\n";
        outUtf8 += "\r\n";

        char line[512];
        for (size_t i = 0; i < pose.bones.size(); ++i) {
            const VpdBone& b = pose.bones[i];
            outUtf8 += "Bone" + std::to_string(i) + "{" + b.name + "\r\n";
            snprintf(line, sizeof(line), "  %.6f,%.6f,%.6f;\t\t\t\t// trans x,y,z\r\n",
                     b.translation.x, b.translation.y, b.translation.z);
            outUtf8 += line;
            snprintf(line, sizeof(line), "  %.6f,%.6f,%.6f,%.6f;\t\t// Quaternion x,y,z,w\r\n",
                     b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w);
            outUtf8 += line;
            outUtf8 += "}\r\n";
            outUtf8 += "\r\n";
        }
        for (size_t i = 0; i < pose.morphs.size(); ++i) {
            const VpdMorph& m = pose.morphs[i];
            outUtf8 += "Morph" + std::to_string(i) + "{" + m.name + "\r\n";
            snprintf(line, sizeof(line), "  %.6f;\r\n", m.weight);
            outUtf8 += line;
            outUtf8 += "}\r\n";
            outUtf8 += "\r\n";
        }

        const std::string outSjis = Utf8ToSjis(outUtf8);
        const std::filesystem::path tmp = path.wstring() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) {
                if (error) *error = "cannot open temporary file for writing";
                return false;
            }
            if (!outSjis.empty()) {
                f.write(outSjis.data(), static_cast<std::streamsize>(outSjis.size()));
                if (!f) {
                    if (error) *error = "short write";
                    return false;
                }
            }
        }
        std::filesystem::rename(tmp, path);
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

} // namespace mmdx
