#include "asset/VmdMotion.h"
#include "asset/BinaryReader.h"
#include "core/TextUtil.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace mmdx {
namespace {
// Layouts and conversions compared with hkrn/MMDAI libvpvl2 (BSD).
bool ReadText(BinaryReader& r, uint8_t encoding, std::string& out) {
    int32_t n;
    if (!r.Read(n) || n < 0 || size_t(n) > r.Remaining() || (!encoding && (n & 1)))
        return false;
    std::vector<char> b(size_t(n), 0);
    if (!r.Read(b.data(), b.size()))
        return false;
    out = encoding ? std::string(b.begin(), b.end()) : Utf16LeToUtf8(b.data(), b.size());
    return true;
}
void NormalizeCurve(uint8_t* c) {
    if (c[0] == c[1] && c[2] == c[3]) {
        c[0] = c[1] = 20;
        c[2] = c[3] = 107;
    } else {
        for (int i = 0; i < 4; ++i)
            c[i] = std::min<uint8_t>(c[i], 127);
    }
}
// UNVERIFIED: libvpvl2 stores the header FPS but does not apply it; use the requested 30 Hz conversion.
uint32_t Frame(uint64_t t, float fps) {
    double v = double(t) * 30.0 / fps;
    return v >= double(UINT32_MAX) ? UINT32_MAX : uint32_t(std::floor(v + .5));
}

} // namespace
bool LoadMvd(const std::filesystem::path& path, VmdMotion& out, std::string* error) {
    try {
        VmdMotion m;
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data, error))
            return false;
        BinaryReader r(data.data(), data.size());
        auto bad = [&]() {
            if (error)
                *error = "corrupt MVD";
            return false;
        };
        char magic[30];
        float version, fps;
        uint8_t encoding;
        std::string ignored;
        if (!r.Read(magic, 30) || std::memcmp(magic, "Motion Vector Data file", 23) || !r.Read(version) ||
            version != 1.f || !r.Read(encoding) || encoding > 1 || !ReadText(r, encoding, m.modelName) ||
            !ReadText(r, encoding, ignored) || !r.Read(fps) || !std::isfinite(fps) || fps <= 0 ||
            !ReadText(r, encoding, ignored))
            return bad();
        std::map<int32_t, std::string> names;
        size_t ignoredBones = 0, ignoredCameras = 0;
        std::map<std::pair<int32_t, uint32_t>, size_t> boneIndex, morphIndex;
        std::map<uint32_t, size_t> cameraIndex;
        std::vector<int32_t> boneIds, morphIds;
        std::vector<std::vector<int32_t>> modelIds;
        std::vector<bool> cameraHasProperties;
        std::vector<std::pair<uint32_t, bool>> cameraProperties;
        while (r.Remaining()) {
            uint8_t type, minor;
            if (!r.Read(type) || !r.Read(minor))
                return bad();
            if (type == 0xff)
                break;
            int32_t id, size, count, extra;
            if (!r.Read(id) || !r.Read(size) || !r.Read(count) || !r.Read(extra) || count < 0 || extra < 0)
                return bad();
            if (type == 0) {
                if (size_t(extra) > r.Remaining() || !r.Skip(extra) || size_t(count) > r.Remaining() / 8)
                    return bad();
                for (int32_t i = 0; i < count; ++i) {
                    int32_t key;
                    std::string name;
                    if (!r.Read(key) || !ReadText(r, encoding, name))
                        return bad();
                    names[key] = std::move(name);
                }
                continue;
            }
            std::vector<int32_t> ikNames;
            if (type == 0x40) {
                int32_t n;
                if (!r.Read(n) || n < 0 || extra < 4 || uint64_t(n) * 4 > uint64_t(extra - 4) ||
                    size_t(extra - 4) > r.Remaining())
                    return bad();
                for (int i = 0; i < n; ++i) {
                    int32_t k;
                    if (!r.Read(k))
                        return bad();
                    ikNames.push_back(k);
                }
                if (!r.Skip(size_t(extra - 4) - size_t(n) * 4))
                    return bad();
            } else if (type == 0x58) {
                // UNVERIFIED: follow libvpvl2 EffectSection::preparse, whose parameter sizing is asymmetric.
                int32_t n;
                if (!r.Read(n) || n < 0 || uint64_t(n) * 8 + 4 > uint64_t(extra))
                    return bad();
                size_t bytes = size_t(extra) - 4 - size_t(n) * 4;
                if (bytes > r.Remaining() || !r.Skip(bytes))
                    return bad();
            } else if (size_t(extra) > r.Remaining() || !r.Skip(extra))
                return bad();
            // Model minor 1 adds four alignment bytes per key in libvpvl2.
            if (type == 0x40 && minor == 1) {
                if (size > INT32_MAX - 4)
                    return bad();
                size += 4;
            }
            if (size < 0 || uint64_t(size) * uint64_t(count) > r.Remaining())
                return bad();
            if (type != 0x10 && type != 0x20 && type != 0x40 && type != 0x60 && type != 0x68 && type != 0x70) {
                // UNVERIFIED: unknown section types are assumed to use the common sized header.
                if (!r.Skip(size_t(size) * size_t(count)))
                    return bad();
                continue;
            }
            for (int i = 0; i < count; ++i) {
                size_t start = r.Pos();
                uint64_t time = 0;
                int32_t layer = 0;
                if ((type == 0x10 || type == 0x60) && (!r.Read(layer)))
                    return bad();
                if (!r.Read(time))
                    return bad();
                uint32_t frame = Frame(time, fps);
                if (type == 0x10) {
                    if (size < 56)
                        return bad();
                    VmdBoneKey k;
                    k.frame = frame;
                    uint8_t curves[16];
                    // Both VMD and MVD use libvpvl2 setPosition/setRotation2: retain native bytes.
                    if (!r.Read(k.translation) || !r.Read(k.rotation) || !r.Read(curves, 16))
                        return bad();
                    for (int c = 0; c < 4; ++c)
                        NormalizeCurve(curves + 4 * c);
                    for (int c = 0; c < 4; ++c)
                        for (int j = 0; j < 4; ++j)
                            k.interp[c + 4 * j] = curves[4 * c + j];
                    for (int row = 1; row < 4; ++row)
                        for (int j = 0; j < 16 - row; ++j)
                            k.interp[row * 16 + j] = k.interp[j + row];
                    if (layer == 0) {
                        auto [it, inserted] = boneIndex.emplace(std::make_pair(id, frame), m.boneKeys.size());
                        if (inserted) {
                            boneIds.push_back(id);
                            m.boneKeys.push_back(std::move(k));
                        } else
                            m.boneKeys[it->second] = std::move(k);
                    } else if (k.translation.x || k.translation.y || k.translation.z || k.rotation.x || k.rotation.y ||
                               k.rotation.z || k.rotation.w != 1)
                        ++ignoredBones;
                } else if (type == 0x20) {
                    if (size < 16)
                        return bad();
                    VmdMorphKey k;
                    k.frame = frame;
                    if (!r.Read(k.weight) || !r.Skip(4))
                        return bad();
                    auto [it, inserted] = morphIndex.emplace(std::make_pair(id, frame), m.morphKeys.size());
                    if (inserted) {
                        morphIds.push_back(id);
                        m.morphKeys.push_back(std::move(k));
                    } else
                        m.morphKeys[it->second] = std::move(k);
                } else if (type == 0x60) {
                    if (size < 61)
                        return bad();
                    VmdCameraKey k;
                    k.frame = frame;
                    float fov;
                    uint8_t perspective, curves[16];
                    DirectX::XMFLOAT3 eye, angle;
                    float distance;
                    if (!r.Read(distance) || !r.Read(eye) || !r.Read(angle) || !r.Read(fov) || !r.Read(perspective))
                        return bad();
                    if (minor >= 3 && (size < 64 || !r.Skip(3)))
                        return bad();
                    if (!r.Read(curves, 16) || !std::isfinite(fov))
                        return bad();
                    k.rotation = {-angle.x, angle.y - DirectX::XM_PI, angle.z};
                    const auto rotation =
                        DirectX::XMMatrixRotationRollPitchYaw(-k.rotation.x, -k.rotation.y, -k.rotation.z);
                    const auto forward = DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 0, 1, 0), rotation);
                    DirectX::XMStoreFloat3(&k.target,
                                           DirectX::XMVectorMultiplyAdd(DirectX::XMVectorReplicate(distance), forward,
                                                                        DirectX::XMLoadFloat3(&eye)));
                    k.distance = -distance;
                    k.fovExact = fov * (180.0f / DirectX::XM_PI);
                    k.fovDeg = static_cast<uint32_t>(std::clamp(std::round(k.fovExact), 1.0f, 125.0f));
                    k.perspective = minor >= 3 ? true : perspective != 0;
                    for (int c = 0; c < 4; ++c)
                        NormalizeCurve(curves + 4 * c);
                    for (int p = 0; p < 6; ++p) {
                        int c = p < 3 ? 0 : p - 2;
                        auto* d = k.interp + p * 4;
                        auto* curve = curves + c * 4;
                        d[0] = curve[0];
                        d[1] = curve[2];
                        d[2] = curve[1];
                        d[3] = curve[3];
                    }
                    if (layer == 0) {
                        auto [it, inserted] = cameraIndex.emplace(frame, m.cameraKeys.size());
                        if (inserted) {
                            m.cameraKeys.push_back(std::move(k));
                            cameraHasProperties.push_back(minor >= 3);
                        } else {
                            m.cameraKeys[it->second] = std::move(k);
                            cameraHasProperties[it->second] = minor >= 3;
                        }
                    } else
                        ++ignoredCameras;
                } else if (type == 0x68) {
                    if (size < 10)
                        return bad();
                    uint8_t enabled, perspective;
                    if (!r.Read(enabled) || !r.Read(perspective))
                        return bad();
                    cameraProperties.emplace_back(frame, perspective != 0);
                } else if (type == 0x70) {
                    if (size < 33)
                        return bad();
                    VmdLightKey k;
                    k.frame = frame;
                    uint8_t enabled;
                    // MVD and VMD light readers both apply setPosition to direction and raw color.
                    // UNVERIFIED: VMD has no light enable flag, so its state is discarded.
                    if (!r.Read(k.direction) || !r.Read(k.color) || !r.Read(enabled))
                        return bad();
                    m.lightKeys.push_back(k);
                } else {
                    if (size < 24 || size_t(size) < 24 + ikNames.size())
                        return bad();
                    VmdIkKey k;
                    k.frame = frame;
                    uint8_t visible;
                    if (!r.Read(visible) || !r.Skip(15))
                        return bad();
                    k.visible = visible != 0;
                    for (auto id2 : ikNames) {
                        uint8_t enabled;
                        if (!r.Read(enabled))
                            return bad();
                        k.ikStates.emplace_back("", enabled != 0);
                    }
                    modelIds.push_back(ikNames);
                    m.ikKeys.push_back(std::move(k));
                }
                if (r.Pos() - start > size_t(size) || !r.Skip(size_t(size) - (r.Pos() - start)))
                    return bad();
            }
        }
        std::stable_sort(cameraProperties.begin(), cameraProperties.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < m.cameraKeys.size(); ++i) {
            if (!cameraHasProperties[i])
                continue;
            const auto it = std::upper_bound(cameraProperties.begin(), cameraProperties.end(), m.cameraKeys[i].frame,
                                             [](uint32_t f, const auto& p) { return f < p.first; });
            if (it != cameraProperties.begin())
                m.cameraKeys[i].perspective = (it - 1)->second;
        }
        // Name lists may appear after tracks; resolve IDs only after all sections have been read.
        for (size_t i = 0; i < boneIds.size(); ++i) {
            auto it = names.find(boneIds[i]);
            if (it == names.end())
                return bad();
            m.boneKeys[i].boneName = it->second;
        }
        for (size_t i = 0; i < morphIds.size(); ++i) {
            auto it = names.find(morphIds[i]);
            if (it == names.end())
                return bad();
            m.morphKeys[i].morphName = it->second;
        }
        for (size_t i = 0; i < modelIds.size(); ++i)
            for (size_t j = 0; j < modelIds[i].size(); ++j) {
                auto it = names.find(modelIds[i][j]);
                if (it == names.end())
                    return bad();
                m.ikKeys[i].ikStates[j].first = it->second;
            }
        if (ignoredBones)
            LOG_WARN("MVD bone layers > 0 ignored: %zu", ignoredBones);
        if (ignoredCameras)
            LOG_WARN("MVD camera layers > 0 ignored: %zu", ignoredCameras);
        for (const auto& k : m.boneKeys)
            m.maxFrame = std::max(m.maxFrame, k.frame);
        for (const auto& k : m.morphKeys)
            m.maxFrame = std::max(m.maxFrame, k.frame);
        for (const auto& k : m.cameraKeys)
            m.maxFrame = std::max(m.maxFrame, k.frame);
        for (const auto& k : m.lightKeys)
            m.maxFrame = std::max(m.maxFrame, k.frame);
        for (const auto& k : m.ikKeys)
            m.maxFrame = std::max(m.maxFrame, k.frame);
        m.sourcePath = std::filesystem::absolute(path);
        out = std::move(m);
        return true;
    } catch (...) {
        if (error)
            *error = "unexpected exception";
        return false;
    }
}

bool ProbeMvd(const std::filesystem::path& path, VmdProbe& out, std::string* error) {
    VmdMotion m;
    if (!LoadMvd(path, m, error))
        return false;
    out = {};
    out.modelName = m.modelName;
    out.boneKeyCount = (uint32_t)m.boneKeys.size();
    out.morphKeyCount = (uint32_t)m.morphKeys.size();
    out.cameraKeyCount = (uint32_t)m.cameraKeys.size();
    std::set<std::string> names;
    std::vector<std::pair<uint32_t, float>> center, root;
    for (const auto& k : m.boneKeys) {
        names.insert(k.boneName);
        out.maxFrame = std::max(out.maxFrame, k.frame);
        if (k.boneName == "センター")
            center.emplace_back(k.frame, k.translation.x);
        else if (k.boneName == "全ての親")
            root.emplace_back(k.frame, k.translation.x);
    }
    const uint32_t stageMaxFrame = out.maxFrame;
    for (const auto& k : m.morphKeys)
        out.maxFrame = std::max(out.maxFrame, k.frame);
    for (const auto& k : m.cameraKeys)
        out.maxFrame = std::max(out.maxFrame, k.frame);
    out.boneNames.assign(names.begin(), names.end());
    if (!center.empty() || !root.empty()) {
        auto prep = [](auto& v) { std::sort(v.begin(), v.end()); };
        prep(center);
        prep(root);
        auto at = [](const auto& v, uint32_t f) {
            if (v.empty())
                return 0.f;
            auto it = std::upper_bound(v.begin(), v.end(), f, [](uint32_t n, const auto& p) { return n < p.first; });
            if (it == v.begin())
                return it->second;
            if (it == v.end())
                return v.back().second;
            auto a = it - 1;
            return a->second + (it->second - a->second) * float(f - a->first) / float(it->first - a->first);
        };
        out.stageStep = std::max(10u, stageMaxFrame / 1000u + 1u);
        for (uint64_t f = 0; f <= stageMaxFrame; f += out.stageStep)
            out.stageX.push_back(at(center, (uint32_t)f) + at(root, (uint32_t)f));
    }
    return true;
}
} // namespace mmdx
