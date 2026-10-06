// PMD (MikuMikuDance 1.x model) -> PmxModel, converted the way PMX Editor converts a PMD:
// BDEF2 vertices, the PMD bone types as PMX flags (append rotation for 回転影響下/回転連動, fixed
// axis for twist bones), IK chains with MMD's knee limit, the "base" skin resolved into vertex
// morphs, absolute rigid-body positions, display frames. Strings are Shift-JIS.
#include "asset/PmxModel.h"
#include "asset/BinaryReader.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace mmdx {

namespace {

constexpr float kPi = 3.14159265358979f;

std::string FixedSjis(BinaryReader& r, size_t n) {
    std::string raw(n, '\0');
    r.Read(raw.data(), n);
    const size_t end = raw.find('\0');
    if (end != std::string::npos) raw.resize(end);
    // a few tools pad with 0xFD after the terminator; trailing spaces are not part of names either
    while (!raw.empty() && (raw.back() == ' ' || (unsigned char)raw.back() == 0xFD)) raw.pop_back();
    return SjisToUtf8(raw);
}

bool Fail(std::string* error, const char* what) {
    if (error) *error = std::string("PMD: ") + what;
    return false;
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && ToLowerAscii(s.substr(s.size() - n)) == suffix;
}

int32_t AddTexture(PmxModel& out, const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < out.textures.size(); ++i)
        if (out.textures[i] == name) return (int32_t)i;
    out.textures.push_back(name);
    return (int32_t)out.textures.size() - 1;
}

}  // namespace

bool LoadPmd(const std::filesystem::path& path, PmxModel& out, std::string* error) {
    try {
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data, error)) return false;
        BinaryReader r(data.data(), data.size());
        out = PmxModel{};
        out.sourcePath = std::filesystem::absolute(path);
        out.version = 1.0f;

        // ---- header
        char magic[3] = {};
        float version = 0;
        if (!r.Read(magic, 3) || std::memcmp(magic, "Pmd", 3) != 0 || !r.Read(version)) return Fail(error, "not a PMD file");
        out.name = FixedSjis(r, 20);
        out.comment = FixedSjis(r, 256);

        // ---- vertices
        uint32_t vertexCount = 0;
        if (!r.Read(vertexCount) || (uint64_t)vertexCount * 38 > r.Remaining()) return Fail(error, "corrupt vertex count");
        out.vertices.resize(vertexCount);
        for (PmxVertex& v : out.vertices) {
            uint16_t bones[2];
            uint8_t weight = 0, noEdge = 0;
            r.Read(v.position);
            r.Read(v.normal);
            r.Read(v.uv);
            r.Read(bones);
            r.Read(weight);
            r.Read(noEdge);
            const float w0 = std::min(weight, (uint8_t)100) / 100.0f;
            v.deform = PmxDeform::BDEF2;
            v.boneIndex[0] = bones[0] == 0xFFFF ? -1 : bones[0];
            v.boneIndex[1] = bones[1] == 0xFFFF ? -1 : bones[1];
            v.boneWeight[0] = w0;
            v.boneWeight[1] = 1.0f - w0;
            if (v.boneIndex[0] == v.boneIndex[1] || v.boneIndex[1] < 0 || w0 >= 1.0f) {
                v.deform = PmxDeform::BDEF1;
                v.boneIndex[1] = -1;
                v.boneWeight[0] = 1.0f;
                v.boneWeight[1] = 0.0f;
            } else if (w0 <= 0.0f) {
                v.deform = PmxDeform::BDEF1;
                v.boneIndex[0] = v.boneIndex[1];
                v.boneIndex[1] = -1;
                v.boneWeight[0] = 1.0f;
                v.boneWeight[1] = 0.0f;
            }
            v.edgeScale = noEdge ? 0.0f : 1.0f;
        }

        // ---- faces
        uint32_t indexCount = 0;
        if (!r.Read(indexCount) || (uint64_t)indexCount * 2 > r.Remaining() || indexCount % 3) return Fail(error, "corrupt face count");
        out.indices.resize(indexCount);
        for (uint32_t& i : out.indices) {
            uint16_t v = 0;
            r.Read(v);
            i = v < vertexCount ? v : 0;
        }

        // ---- materials (textures are collected as they appear; toons are resolved after the toon table)
        uint32_t materialCount = 0;
        if (!r.Read(materialCount) || (uint64_t)materialCount * 70 > r.Remaining()) return Fail(error, "corrupt material count");
        std::vector<uint8_t> materialToon(materialCount);
        out.materials.resize(materialCount);
        for (uint32_t m = 0; m < materialCount; ++m) {
            PmxMaterial& mat = out.materials[m];
            uint8_t edge = 0;
            r.Read(mat.diffuse);  // rgb + alpha
            r.Read(mat.specularPower);
            r.Read(mat.specular);
            r.Read(mat.ambient);
            r.Read(materialToon[m]);
            r.Read(edge);
            r.Read(mat.indexCount);
            const std::string tex = FixedSjis(r, 20);
            mat.name = "材質" + std::to_string(m + 1);
            mat.nameEn = "Material" + std::to_string(m + 1);
            // PMX Editor's conversion: everything casts/receives shadows except MMD's "no self
            // shadow" alpha 0.98; translucent materials are drawn double sided.
            mat.flags = PmxMat_GroundShadow;
            if (std::fabs(mat.diffuse.w - 0.98f) > 1e-4f) mat.flags |= PmxMat_CastShadow | PmxMat_ReceiveShadow;
            if (mat.diffuse.w < 1.0f) mat.flags |= PmxMat_DoubleSided;
            if (edge) mat.flags |= PmxMat_Edge;
            mat.edgeColor = {0, 0, 0, 1};
            mat.edgeSize = 1.0f;
            // "texture*sphere", or a lone texture / sphere
            std::string main = tex, sphere;
            if (const size_t star = tex.find('*'); star != std::string::npos) {
                main = tex.substr(0, star);
                sphere = tex.substr(star + 1);
            } else if (EndsWithNoCase(tex, ".sph") || EndsWithNoCase(tex, ".spa")) {
                main.clear();
                sphere = tex;
            }
            mat.textureIndex = AddTexture(out, main);
            if (!sphere.empty()) {
                mat.sphereTextureIndex = AddTexture(out, sphere);
                mat.sphereMode = EndsWithNoCase(sphere, ".spa") ? PmxSphereMode::Add : PmxSphereMode::Multiply;
            }
        }
        if (r.Failed()) return Fail(error, "materials: unexpected end of file");

        // ---- bones
        uint16_t boneCount = 0;
        if (!r.Read(boneCount) || (uint64_t)boneCount * 39 > r.Remaining()) return Fail(error, "corrupt bone count");
        struct RawBone { uint16_t parent, tail, ikParent; uint8_t type; };
        std::vector<RawBone> raw(boneCount);
        out.bones.resize(boneCount);
        for (uint16_t b = 0; b < boneCount; ++b) {
            PmxBone& bone = out.bones[b];
            bone.name = FixedSjis(r, 20);
            r.Read(raw[b].parent);
            r.Read(raw[b].tail);
            r.Read(raw[b].type);
            r.Read(raw[b].ikParent);
            r.Read(bone.position);
        }

        // ---- IK
        uint16_t ikCount = 0;
        if (!r.Read(ikCount)) return Fail(error, "IK: unexpected end of file");
        for (uint16_t k = 0; k < ikCount; ++k) {
            uint16_t ikBone = 0, target = 0, iterations = 0;
            uint8_t chainLength = 0;
            float limit = 0;
            r.Read(ikBone);
            r.Read(target);
            r.Read(chainLength);
            r.Read(iterations);
            r.Read(limit);
            std::vector<uint16_t> chain(chainLength);
            for (uint16_t& c : chain) r.Read(c);
            if (r.Failed()) return Fail(error, "IK: unexpected end of file");
            if (ikBone >= boneCount) continue;
            PmxBone& bone = out.bones[ikBone];
            bone.flags |= PmxBone_IK;
            bone.ikTargetIndex = target < boneCount ? target : -1;
            bone.ikLoopCount = iterations;
            bone.ikLimitAngle = limit * 4.0f;  // PMD stores the per-iteration limit in units of 4 radians
            for (uint16_t c : chain) {
                if (c >= boneCount) continue;
                PmxIkLink link;
                link.boneIndex = c;
                // MMD bends knees only forward: PMX Editor gives ひざ links an X-only limit
                if (out.bones[c].name.find("\xE3\x81\xB2\xE3\x81\x96") != std::string::npos) {  // ひざ
                    link.hasLimit = true;
                    link.limitMin = {-kPi, 0, 0};
                    link.limitMax = {-0.5f * kPi / 180.0f, 0, 0};
                }
                bone.ikLinks.push_back(link);
            }
        }

        // PMD bone types -> PMX flags
        for (uint16_t b = 0; b < boneCount; ++b) {
            PmxBone& bone = out.bones[b];
            const RawBone& rb = raw[b];
            bone.parentIndex = rb.parent < boneCount ? rb.parent : -1;
            bone.flags |= PmxBone_Rotatable | PmxBone_Visible | PmxBone_Operable;
            if (rb.tail > 0 && rb.tail < boneCount && rb.type != 9) {
                bone.flags |= PmxBone_TailIsBone;
                bone.tailBoneIndex = rb.tail;
            } else {
                bone.tailBoneIndex = -1;
                bone.tailOffset = {0, 0, 0};
            }
            switch (rb.type) {
            case 1:  // rotate + move
            case 2:  // IK
                bone.flags |= PmxBone_Movable;
                break;
            case 4:  // under IK
                break;
            case 5:  // 回転影響下: follows the rotation of the bone in the IK field
                if (rb.ikParent < boneCount) {
                    bone.flags |= PmxBone_AppendRotate;
                    bone.appendParentIndex = rb.ikParent;
                    bone.appendRatio = 1.0f;
                }
                bone.flags &= ~(PmxBone_Visible | PmxBone_Operable);
                break;
            case 6:  // IK target
            case 7:  // invisible
                bone.flags &= ~(PmxBone_Visible | PmxBone_Operable);
                break;
            case 8:  // twist: rotates about the axis toward its tail
                if (rb.tail > 0 && rb.tail < boneCount) {
                    const XMFLOAT3 &a = bone.position, &t = out.bones[rb.tail].position;
                    XMFLOAT3 d{t.x - a.x, t.y - a.y, t.z - a.z};
                    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    if (len > 1e-6f) {
                        bone.flags |= PmxBone_FixedAxis;
                        bone.fixedAxis = {d.x / len, d.y / len, d.z / len};
                    }
                }
                break;
            case 9:  // 回転連動: rotation of the IK-field bone times tail / 100
                if (rb.ikParent < boneCount) {
                    bone.flags |= PmxBone_AppendRotate;
                    bone.appendParentIndex = rb.ikParent;
                    bone.appendRatio = rb.tail * 0.01f;
                }
                bone.flags &= ~(PmxBone_Visible | PmxBone_Operable);
                break;
            default:
                break;
            }
        }

        // ---- skins (morphs). Skin 0 "base" lists the vertices with absolute positions; the others
        // index into that list.
        uint16_t skinCount = 0;
        if (!r.Read(skinCount)) return Fail(error, "skins: unexpected end of file");
        std::vector<uint32_t> baseVertices;
        for (uint16_t s = 0; s < skinCount; ++s) {
            const std::string name = FixedSjis(r, 20);
            uint32_t count = 0;
            uint8_t type = 0;
            r.Read(count);
            r.Read(type);
            if (r.Failed() || (uint64_t)count * 16 > r.Remaining()) return Fail(error, "corrupt skin");
            if (type == 0) {
                baseVertices.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    r.Read(baseVertices[i]);
                    r.Skip(12);
                }
                continue;
            }
            PmxMorph m;
            m.name = name;
            m.type = PmxMorphType::Vertex;
            m.panel = type <= 4 ? type : 4;
            m.vertexOffsets.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t idx = 0;
                XMFLOAT3 offset;
                r.Read(idx);
                r.Read(offset);
                if (idx < baseVertices.size() && baseVertices[idx] < vertexCount)
                    m.vertexOffsets.push_back({(int32_t)baseVertices[idx], offset});
            }
            out.morphs.push_back(std::move(m));
        }
        const bool hadBase = !baseVertices.empty();

        // ---- display lists
        uint8_t skinDisplayCount = 0;
        r.Read(skinDisplayCount);
        std::vector<uint16_t> skinDisplay(skinDisplayCount);
        for (uint16_t& s : skinDisplay) r.Read(s);
        uint8_t boneFrameCount = 0;
        r.Read(boneFrameCount);
        std::vector<std::string> frameNames(boneFrameCount);
        for (std::string& n : frameNames) {
            n = FixedSjis(r, 50);
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        }
        uint32_t boneDisplayCount = 0;
        r.Read(boneDisplayCount);
        if (r.Failed() || (uint64_t)boneDisplayCount * 3 > r.Remaining()) return Fail(error, "corrupt display list");
        std::vector<std::pair<uint16_t, uint8_t>> boneDisplay(boneDisplayCount);
        for (auto& [bone, frame] : boneDisplay) {
            r.Read(bone);
            r.Read(frame);
        }
        {
            PmxDisplayFrame root;
            root.name = "Root";
            root.nameEn = "Root";
            root.special = true;
            if (boneCount > 0) root.items.push_back({false, 0});
            out.displayFrames.push_back(std::move(root));
            PmxDisplayFrame face;
            face.name = "\xE8\xA1\xA8\xE6\x83\x85";  // 表情
            face.nameEn = "Exp";
            face.special = true;
            for (uint16_t s : skinDisplay) {
                const int32_t idx = (int32_t)s - (hadBase ? 1 : 0);  // the base skin is not a morph
                if (idx >= 0 && idx < (int32_t)out.morphs.size()) face.items.push_back({true, idx});
            }
            out.displayFrames.push_back(std::move(face));
            for (size_t f = 0; f < frameNames.size(); ++f) {
                PmxDisplayFrame df;
                df.name = frameNames[f];
                for (const auto& [bone, frame] : boneDisplay)
                    if (frame == f + 1 && bone < boneCount && bone != 0) df.items.push_back({false, bone});
                out.displayFrames.push_back(std::move(df));
            }
        }

        // ---- optional extensions: English names, toon table, physics
        uint8_t hasEnglish = 0;
        if (r.Remaining() > 0 && r.Read(hasEnglish) && hasEnglish) {
            out.nameEn = FixedSjis(r, 20);
            out.commentEn = FixedSjis(r, 256);
            for (PmxBone& b : out.bones) b.nameEn = FixedSjis(r, 20);
            for (PmxMorph& m : out.morphs) m.nameEn = FixedSjis(r, 20);
            for (size_t f = 0; f < frameNames.size(); ++f) {
                std::string n = FixedSjis(r, 50);
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
                if (f + 2 < out.displayFrames.size()) out.displayFrames[f + 2].nameEn = n;
            }
        }
        std::string toonNames[10];
        for (int i = 0; i < 10; ++i) {
            char def[16];
            std::snprintf(def, sizeof(def), "toon%02d.bmp", i + 1);
            toonNames[i] = def;
        }
        if (!r.Failed() && r.Remaining() >= 1000)
            for (std::string& t : toonNames) t = FixedSjis(r, 100);
        for (uint32_t m = 0; m < materialCount; ++m) {
            PmxMaterial& mat = out.materials[m];
            const uint8_t t = materialToon[m];
            if (t >= 10) continue;  // 255: no toon
            const std::string& name = toonNames[t];
            int shared = -1;
            const std::string lower = ToLowerAscii(name);
            if (lower.size() == 10 && lower.rfind("toon", 0) == 0 && lower.substr(6) == ".bmp" &&
                std::isdigit((unsigned char)lower[4]) && std::isdigit((unsigned char)lower[5]))
                shared = (lower[4] - '0') * 10 + (lower[5] - '0') - 1;
            if (shared >= 0 && shared <= 9) {
                mat.sharedToon = true;
                mat.toonIndex = shared;
            } else if (!name.empty()) {
                mat.toonIndex = AddTexture(out, name);
            }
        }

        if (!r.Failed() && r.Remaining() >= 4) {
            uint32_t rigidCount = 0;
            r.Read(rigidCount);
            if ((uint64_t)rigidCount * 83 > r.Remaining()) return Fail(error, "corrupt rigid body count");
            const int32_t center = out.FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");  // センター
            out.rigidBodies.resize(rigidCount);
            for (PmxRigidBody& rb : out.rigidBodies) {
                uint16_t bone = 0;
                rb.name = FixedSjis(r, 20);
                r.Read(bone);
                r.Read(rb.group);
                r.Read(rb.collisionMask);
                r.Read(rb.shape);
                r.Read(rb.size);
                r.Read(rb.position);
                r.Read(rb.rotation);
                r.Read(rb.mass);
                r.Read(rb.linearDamping);
                r.Read(rb.angularDamping);
                r.Read(rb.restitution);
                r.Read(rb.friction);
                r.Read(rb.physicsMode);
                // PMD positions are relative to the bone (センター when the body has none)
                const int32_t ref = bone < boneCount ? (int32_t)bone : (center >= 0 ? center : (boneCount ? 0 : -1));
                rb.boneIndex = bone < boneCount ? (int32_t)bone : -1;
                if (ref >= 0) {
                    const XMFLOAT3& bp = out.bones[ref].position;
                    rb.position = {rb.position.x + bp.x, rb.position.y + bp.y, rb.position.z + bp.z};
                }
            }
            uint32_t jointCount = 0;
            if (r.Read(jointCount)) {
                if ((uint64_t)jointCount * 124 > r.Remaining()) return Fail(error, "corrupt joint count");
                out.joints.resize(jointCount);
                for (PmxJoint& j : out.joints) {
                    uint32_t a = 0, b = 0;
                    j.name = FixedSjis(r, 20);
                    r.Read(a);
                    r.Read(b);
                    j.rigidBodyA = a < rigidCount ? (int32_t)a : -1;
                    j.rigidBodyB = b < rigidCount ? (int32_t)b : -1;
                    r.Read(j.position);
                    r.Read(j.rotation);
                    r.Read(j.linearMin);
                    r.Read(j.linearMax);
                    r.Read(j.angularMin);
                    r.Read(j.angularMax);
                    r.Read(j.springLinear);
                    r.Read(j.springAngular);
                }
            }
        }
        if (r.Failed()) return Fail(error, "unexpected end of file");
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("PMD: ") + e.what();
        return false;
    }
}

bool ProbePmd(const std::filesystem::path& path, PmxProbe& out, std::string* error) {
    PmxModel m;
    if (!LoadPmd(path, m, error)) return false;
    out = PmxProbe{};
    out.name = m.name;
    out.nameEn = m.nameEn;
    out.comment = m.comment;
    out.vertexCount = (uint32_t)m.vertices.size();
    out.faceCount = (uint32_t)m.indices.size() / 3;
    out.textureCount = (uint32_t)m.textures.size();
    out.materialCount = (uint32_t)m.materials.size();
    out.boneCount = (uint32_t)m.bones.size();
    for (const PmxBone& b : m.bones) out.boneNames.push_back(b.name);
    return true;
}

}  // namespace mmdx
