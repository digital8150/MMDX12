#include "asset/VmdMotion.h"
#include "core/TextUtil.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace mmdx {

namespace {

template<class T> void Put(std::vector<uint8_t>& buf, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "Put requires a trivially copyable type");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

// Appends a zero-initialised fixed-width Shift-JIS field for `utf8`, truncating to
// `width` bytes if necessary.
//
// NOTE: the spec asked to drop a Shift-JIS lead byte that would be cut in half by
// truncation. That contradicts the loader: SjisToUtf8 maps a dangling lead byte to
// U+30FB ('・'), and real VMD files (e.g. rolling_girl/motion.vmd morph key 29) store
// names truncated mid-character. Dropping the lead byte changes the decoded name and
// breaks the LoadVmd -> SaveVmd -> LoadVmd round trip, so the truncated bytes are kept
// verbatim to make SaveVmd the exact inverse of LoadVmd.
void WriteSjisField(std::vector<uint8_t>& buf, const std::string& utf8, size_t width) {
    uint8_t field[64] = {};
    const std::string sjis = Utf8ToSjis(utf8);
    size_t len = sjis.size() < width ? sjis.size() : width;
    if (len > 0) memcpy(field, sjis.data(), len);
    buf.insert(buf.end(), field, field + width);
}

} // namespace

bool SaveVmd(const std::filesystem::path& path, const VmdMotion& motion, std::string* error) {
    try {
        std::vector<uint8_t> buf;
        buf.reserve(1024 * 1024);

        // ---- header ----
        static const char kHeader[] = "Vocaloid Motion Data 0002";
        for (size_t i = 0; i < 30; ++i) {
            buf.push_back(i < sizeof(kHeader) - 1 ? static_cast<uint8_t>(kHeader[i]) : 0);
        }
        WriteSjisField(buf, motion.modelName, 20);

        // ---- bone keys ----
        Put(buf, static_cast<uint32_t>(motion.boneKeys.size()));
        for (const VmdBoneKey& key : motion.boneKeys) {
            WriteSjisField(buf, key.boneName, 15);
            Put(buf, key.frame);
            Put(buf, key.translation);
            Put(buf, key.rotation);
            buf.insert(buf.end(), key.interp, key.interp + 64);
        }

        // ---- morph keys ----
        Put(buf, static_cast<uint32_t>(motion.morphKeys.size()));
        for (const VmdMorphKey& key : motion.morphKeys) {
            WriteSjisField(buf, key.morphName, 15);
            Put(buf, key.frame);
            Put(buf, key.weight);
        }

        // ---- camera keys ----
        Put(buf, static_cast<uint32_t>(motion.cameraKeys.size()));
        for (const VmdCameraKey& key : motion.cameraKeys) {
            Put(buf, key.frame);
            Put(buf, key.distance);
            Put(buf, key.target);
            Put(buf, key.rotation);
            buf.insert(buf.end(), key.interp, key.interp + 24);
            Put(buf, key.fovDeg);
            Put(buf, static_cast<uint8_t>(key.perspective ? 0 : 1));
        }

        // ---- light keys ----
        Put(buf, static_cast<uint32_t>(motion.lightKeys.size()));
        for (const VmdLightKey& key : motion.lightKeys) {
            Put(buf, key.frame);
            Put(buf, key.color);
            Put(buf, key.direction);
        }

        // ---- self shadow keys ----
        Put(buf, static_cast<uint32_t>(motion.shadowKeys.size()));
        for (const VmdShadowKey& key : motion.shadowKeys) {
            Put(buf, key.frame);
            Put(buf, key.mode);
            Put(buf, key.distance);
        }

        // ---- IK visibility keys ----
        Put(buf, static_cast<uint32_t>(motion.ikKeys.size()));
        for (const VmdIkKey& key : motion.ikKeys) {
            Put(buf, key.frame);
            Put(buf, static_cast<uint8_t>(key.visible ? 1 : 0));
            Put(buf, static_cast<uint32_t>(key.ikStates.size()));
            for (const auto& [name, enabled] : key.ikStates) {
                WriteSjisField(buf, name, 20);
                Put(buf, static_cast<uint8_t>(enabled ? 1 : 0));
            }
        }

        // ---- write atomically: temp file, then rename over the target ----
        const std::filesystem::path tmp = path.wstring() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) {
                if (error) *error = "cannot open temporary file for writing";
                return false;
            }
            if (!buf.empty()) {
                f.write(reinterpret_cast<const char*>(buf.data()),
                        static_cast<std::streamsize>(buf.size()));
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
