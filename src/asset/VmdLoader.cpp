#include "asset/VmdMotion.h"
#include "asset/BinaryReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <cstring>

namespace mmdx {

namespace {

// Reads a fixed-size Shift-JIS char field as UTF-8.
std::string ReadSjisField(BinaryReader& r, size_t bytes) {
    std::string raw(bytes, '\0');
    r.Read(raw.data(), bytes);
    size_t len = raw.find('\0');
    if (len != std::string::npos) raw.resize(len);
    return SjisToUtf8(raw);
}

uint32_t UpdateMaxFrame(uint32_t cur, uint32_t frame) {
    return frame > cur ? frame : cur;
}

} // namespace

bool LoadVmd(const std::filesystem::path& path, VmdMotion& out, std::string* error) {
    try {
        out = VmdMotion{};
        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData, error)) return false;
        if (fileData.empty()) {
            if (error) *error = "empty file";
            return false;
        }

        BinaryReader r(fileData.data(), fileData.size());

        // ---- header ----
        char header[30] = {};
        if (!r.Read(header, 30)) {
            if (error) *error = "not a VMD file";
            return false;
        }
        if (strncmp(header, "Vocaloid Motion Data 0002", 26) == 0) {
            out.modelName = ReadSjisField(r, 20);
        } else if (strncmp(header, "Vocaloid Motion Data file", 25) == 0) {
            out.modelName = ReadSjisField(r, 10);
        } else {
            if (error) *error = "not a VMD file";
            return false;
        }

        // ---- bone keys ----
        uint32_t boneCount = 0;
        if (!r.Read(boneCount) || static_cast<uint64_t>(boneCount) * 111 > r.Remaining()) {
            if (error) *error = "corrupt bone section";
            return false;
        }
        out.boneKeys.resize(boneCount);
        for (uint32_t i = 0; i < boneCount; ++i) {
            VmdBoneKey& key = out.boneKeys[i];
            key.boneName = ReadSjisField(r, 15);
            if (!r.Read(key.frame) || !r.Read(key.translation) || !r.Read(key.rotation) ||
                !r.Read(key.interp, 64)) {
                if (error) *error = "corrupt bone section";
                return false;
            }
            out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
        }

        // ---- morph keys ----
        uint32_t morphCount = 0;
        if (!r.Read(morphCount) || static_cast<uint64_t>(morphCount) * 23 > r.Remaining()) {
            if (error) *error = "corrupt morph section";
            return false;
        }
        out.morphKeys.resize(morphCount);
        for (uint32_t i = 0; i < morphCount; ++i) {
            VmdMorphKey& key = out.morphKeys[i];
            key.morphName = ReadSjisField(r, 15);
            if (!r.Read(key.frame) || !r.Read(key.weight)) {
                if (error) *error = "corrupt morph section";
                return false;
            }
            out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
        }

        // ---- camera keys (optional section) ----
        if (r.Remaining() > 0) {
            uint32_t cameraCount = 0;
            if (!r.Read(cameraCount) ||
                static_cast<uint64_t>(cameraCount) * 61 > r.Remaining()) {
                LOG_WARN("VMD %s: corrupt camera section, ignoring the rest",
                         out.modelName.c_str());
                out.sourcePath = std::filesystem::absolute(path);
                return true;
            }
            out.cameraKeys.resize(cameraCount);
            for (uint32_t i = 0; i < cameraCount; ++i) {
                VmdCameraKey& key = out.cameraKeys[i];
                uint8_t perspectiveFlag = 0;
                if (!r.Read(key.frame) || !r.Read(key.distance) || !r.Read(key.target) ||
                    !r.Read(key.rotation) || !r.Read(key.interp, 24) || !r.Read(key.fovDeg) ||
                    !r.Read(perspectiveFlag)) {
                    LOG_WARN("VMD %s: corrupt camera section, ignoring the rest",
                             out.modelName.c_str());
                    out.cameraKeys.resize(i);
                    out.sourcePath = std::filesystem::absolute(path);
                    return true;
                }
                key.perspective = (perspectiveFlag == 0);
                out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
            }
        }

        // ---- light keys (optional section) ----
        if (r.Remaining() > 0) {
            uint32_t lightCount = 0;
            if (!r.Read(lightCount) ||
                static_cast<uint64_t>(lightCount) * 28 > r.Remaining()) {
                LOG_WARN("VMD %s: corrupt light section, ignoring the rest",
                         out.modelName.c_str());
                out.sourcePath = std::filesystem::absolute(path);
                return true;
            }
            out.lightKeys.resize(lightCount);
            for (uint32_t i = 0; i < lightCount; ++i) {
                VmdLightKey& key = out.lightKeys[i];
                if (!r.Read(key.frame) || !r.Read(key.color) || !r.Read(key.direction)) {
                    LOG_WARN("VMD %s: corrupt light section, ignoring the rest",
                             out.modelName.c_str());
                    out.lightKeys.resize(i);
                    out.sourcePath = std::filesystem::absolute(path);
                    return true;
                }
                out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
            }
        }

        // ---- self shadow keys (optional section) ----
        if (r.Remaining() > 0) {
            uint32_t shadowCount = 0;
            if (!r.Read(shadowCount) ||
                static_cast<uint64_t>(shadowCount) * 9 > r.Remaining()) {
                LOG_WARN("VMD %s: corrupt shadow section, ignoring the rest",
                         out.modelName.c_str());
                out.sourcePath = std::filesystem::absolute(path);
                return true;
            }
            out.shadowKeys.resize(shadowCount);
            for (uint32_t i = 0; i < shadowCount; ++i) {
                VmdShadowKey& key = out.shadowKeys[i];
                if (!r.Read(key.frame) || !r.Read(key.mode) || !r.Read(key.distance)) {
                    LOG_WARN("VMD %s: corrupt shadow section, ignoring the rest",
                             out.modelName.c_str());
                    out.shadowKeys.resize(i);
                    out.sourcePath = std::filesystem::absolute(path);
                    return true;
                }
                out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
            }
        }

        // ---- IK visibility keys (optional section) ----
        if (r.Remaining() > 0) {
            uint32_t ikCount = 0;
            if (!r.Read(ikCount) || static_cast<uint64_t>(ikCount) * 25 > r.Remaining()) {
                LOG_WARN("VMD %s: corrupt ik section, ignoring the rest",
                         out.modelName.c_str());
                out.sourcePath = std::filesystem::absolute(path);
                return true;
            }
            out.ikKeys.resize(ikCount);
            for (uint32_t i = 0; i < ikCount; ++i) {
                VmdIkKey& key = out.ikKeys[i];
                uint8_t visible = 0;
                uint32_t n = 0;
                if (!r.Read(key.frame) || !r.Read(visible) || !r.Read(n)) {
                    LOG_WARN("VMD %s: corrupt ik section, ignoring the rest",
                             out.modelName.c_str());
                    out.ikKeys.resize(i);
                    break;
                }
                key.visible = (visible != 0);
                bool bad = false;
                for (uint32_t j = 0; j < n; ++j) {
                    std::string name = ReadSjisField(r, 20);
                    uint8_t enabled = 0;
                    if (!r.Read(enabled)) { bad = true; break; }
                    key.ikStates.emplace_back(std::move(name), enabled != 0);
                }
                if (bad) {
                    LOG_WARN("VMD %s: corrupt ik section, ignoring the rest",
                             out.modelName.c_str());
                    out.ikKeys.resize(i);
                    break;
                }
                out.maxFrame = UpdateMaxFrame(out.maxFrame, key.frame);
            }
        }

        out.sourcePath = std::filesystem::absolute(path);
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

bool ProbeVmd(const std::filesystem::path& path, VmdProbe& out, std::string* error) {
    try {
        out = VmdProbe{};
        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData, error)) return false;
        if (fileData.empty()) {
            if (error) *error = "empty file";
            return false;
        }

        BinaryReader r(fileData.data(), fileData.size());

        char header[30] = {};
        if (!r.Read(header, 30)) {
            if (error) *error = "not a VMD file";
            return false;
        }
        if (strncmp(header, "Vocaloid Motion Data 0002", 26) == 0) {
            out.modelName = ReadSjisField(r, 20);
        } else if (strncmp(header, "Vocaloid Motion Data file", 25) == 0) {
            out.modelName = ReadSjisField(r, 10);
        } else {
            if (error) *error = "not a VMD file";
            return false;
        }

        // ---- bone keys: read the frame at offset 15 of each 111-byte record ----
        uint32_t boneCount = 0;
        if (!r.Read(boneCount) || static_cast<uint64_t>(boneCount) * 111 > r.Remaining()) {
            if (error) *error = "corrupt bone section";
            return false;
        }
        out.boneKeyCount = boneCount;
        for (uint32_t i = 0; i < boneCount; ++i) {
            uint32_t frame = 0;
            if (!r.Skip(15) || !r.Read(frame) || !r.Skip(92)) {
                if (error) *error = "corrupt bone section";
                return false;
            }
            out.maxFrame = UpdateMaxFrame(out.maxFrame, frame);
        }

        // ---- morph keys: read the frame at offset 15 of each 23-byte record ----
        if (r.Remaining() == 0) return true;
        uint32_t morphCount = 0;
        if (!r.Read(morphCount) || static_cast<uint64_t>(morphCount) * 23 > r.Remaining()) {
            if (error) *error = "corrupt morph section";
            return false;
        }
        out.morphKeyCount = morphCount;
        for (uint32_t i = 0; i < morphCount; ++i) {
            uint32_t frame = 0;
            if (!r.Skip(15) || !r.Read(frame) || !r.Skip(4)) {
                if (error) *error = "corrupt morph section";
                return false;
            }
            out.maxFrame = UpdateMaxFrame(out.maxFrame, frame);
        }

        // ---- camera keys: read the frame at offset 0 of each 61-byte record ----
        if (r.Remaining() == 0) return true;
        uint32_t cameraCount = 0;
        if (!r.Read(cameraCount) || static_cast<uint64_t>(cameraCount) * 61 > r.Remaining()) {
            LOG_WARN("VMD %s: corrupt camera section, ignoring the rest",
                     out.modelName.c_str());
            return true;
        }
        out.cameraKeyCount = cameraCount;
        for (uint32_t i = 0; i < cameraCount; ++i) {
            uint32_t frame = 0;
            if (!r.Read(frame) || !r.Skip(57)) {
                LOG_WARN("VMD %s: corrupt camera section, ignoring the rest",
                         out.modelName.c_str());
                out.cameraKeyCount = i;
                return true;
            }
            out.maxFrame = UpdateMaxFrame(out.maxFrame, frame);
        }
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

} // namespace mmdx
