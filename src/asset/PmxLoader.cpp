#include "asset/PmxModel.h"
#include "asset/BinaryReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cstring>

namespace mmdx {

namespace {

constexpr const char* kTexExts[] = {
    ".png", ".bmp", ".jpg", ".jpeg", ".tga", ".dds", ".spa", ".sph"};

bool CheckCount(int64_t count, size_t remaining, const char* section, std::string* error) {
    if (count < 0 || static_cast<uint64_t>(count) > remaining) {
        if (error) *error = std::string("corrupt ") + section + " count";
        return false;
    }
    return true;
}

int32_t ClampVertexIndex(int32_t idx, int32_t vertexCount) {
    if (idx < 0 || idx >= vertexCount) return 0;
    return idx;
}

} // namespace

// ---------------------------------------------------------------- LoadPmx

bool LoadPmx(const std::filesystem::path& path, PmxModel& out, std::string* error) {
    try {
        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData, error)) return false;
        if (fileData.empty()) {
            if (error) *error = "empty file";
            return false;
        }

        BinaryReader r(fileData.data(), fileData.size());

        // ---- header ----
        char magic[4] = {};
        if (!r.Read(magic, 4) || memcmp(magic, "PMX", 3) != 0) {
            if (error) *error = "not a PMX file";
            return false;
        }
        float version = 0;
        if (!r.Read(version)) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }
        uint8_t globalsCount = 0;
        if (!r.Read(globalsCount) || globalsCount < 8) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }
        uint8_t globals[8] = {};
        for (int i = 0; i < 8; ++i) {
            if (!r.Read(globals[i])) {
                if (error) *error = "header: unexpected end of file";
                return false;
            }
        }
        if (globalsCount > 8) {
            if (!r.Skip(globalsCount - 8)) {
                if (error) *error = "header: unexpected end of file";
                return false;
            }
        }
        const uint8_t addUv = globals[1];
        const uint8_t vIdxSize = globals[2];
        const uint8_t tIdxSize = globals[3];
        const uint8_t mIdxSize = globals[4];
        const uint8_t bIdxSize = globals[5];
        const uint8_t morphIdxSize = globals[6];
        const uint8_t rigidIdxSize = globals[7];
        out.version = version;

        auto readText = [&](std::string* dst) -> bool {
            int32_t len = 0;
            if (!r.Read(len)) return false;
            if (len < 0 || static_cast<size_t>(len) > r.Remaining()) return false;
            std::vector<uint8_t> bytes(static_cast<size_t>(len));
            if (len > 0 && !r.Read(bytes.data(), static_cast<size_t>(len))) return false;
            if (dst) {
                if (globals[0] == 0) *dst = Utf16LeToUtf8(bytes.data(), bytes.size());
                else dst->assign(bytes.begin(), bytes.end());
            }
            return true;
        };

        if (!readText(&out.name) || !readText(&out.nameEn) ||
            !readText(&out.comment) || !readText(&out.commentEn)) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }

        // ---- vertices ----
        int32_t vertexCount = 0;
        if (!r.Read(vertexCount) || !CheckCount(vertexCount, r.Remaining(), "vertices", error)) {
            if (r.Failed() && error) *error = "vertices: unexpected end of file";
            return false;
        }
        const int32_t vCount = vertexCount;
        out.vertices.resize(static_cast<size_t>(vCount));
        for (int32_t i = 0; i < vCount; ++i) {
            PmxVertex& v = out.vertices[static_cast<size_t>(i)];
            if (!r.Read(v.position) || !r.Read(v.normal) || !r.Read(v.uv)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
            if (addUv > 0) {
                if (!r.Skip(static_cast<size_t>(addUv) * 16)) {
                    if (error) *error = "vertices: unexpected end of file";
                    return false;
                }
            }
            uint8_t deform = 0;
            if (!r.Read(deform)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
            float w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            int32_t i0 = -1, i1 = -1, i2 = -1, i3 = -1;
            switch (deform) {
                case 0:  // BDEF1
                    i0 = r.ReadIndex(bIdxSize);
                    w0 = 1;
                    break;
                case 1:  // BDEF2
                    i0 = r.ReadIndex(bIdxSize);
                    i1 = r.ReadIndex(bIdxSize);
                    if (!r.Read(w0)) {
                        if (error) *error = "vertices: unexpected end of file";
                        return false;
                    }
                    w1 = 1.0f - w0;
                    break;
                case 2:  // BDEF4
                case 4:  // QDEF
                    i0 = r.ReadIndex(bIdxSize);
                    i1 = r.ReadIndex(bIdxSize);
                    i2 = r.ReadIndex(bIdxSize);
                    i3 = r.ReadIndex(bIdxSize);
                    if (!r.Read(w0) || !r.Read(w1) || !r.Read(w2) || !r.Read(w3)) {
                        if (error) *error = "vertices: unexpected end of file";
                        return false;
                    }
                    break;
                case 3:  // SDEF
                    i0 = r.ReadIndex(bIdxSize);
                    i1 = r.ReadIndex(bIdxSize);
                    {
                        float w = 0;
                        if (!r.Read(w) || !r.Read(v.sdefC) ||
                            !r.Read(v.sdefR0) || !r.Read(v.sdefR1)) {
                            if (error) *error = "vertices: unexpected end of file";
                            return false;
                        }
                        w0 = w;
                        w1 = 1.0f - w0;
                    }
                    break;
                default:
                    if (error) *error = "vertices: unknown deform type " + std::to_string(deform);
                    return false;
            }
            const bool bdef4 = (deform == 2 || deform == 4);
            if (bdef4) {
                float sum = w0 + w1 + w2 + w3;
                if (sum <= 0.0f) {
                    w0 = 1; w1 = 0; w2 = 0; w3 = 0;
                } else {
                    w0 /= sum; w1 /= sum; w2 /= sum; w3 /= sum;
                }
            }
            v.deform = static_cast<PmxDeform>(deform);
            v.boneIndex[0] = i0;
            v.boneIndex[1] = i1;
            v.boneIndex[2] = i2;
            v.boneIndex[3] = i3;
            v.boneWeight[0] = w0;
            v.boneWeight[1] = w1;
            v.boneWeight[2] = w2;
            v.boneWeight[3] = w3;
            if (!r.Read(v.edgeScale)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
        }

        // ---- faces ----
        int32_t indexCount = 0;
        if (!r.Read(indexCount) || !CheckCount(indexCount, r.Remaining(), "faces", error)) {
            if (r.Failed() && error) *error = "faces: unexpected end of file";
            return false;
        }
        out.indices.resize(static_cast<size_t>(indexCount));
        for (int32_t i = 0; i < indexCount; ++i) {
            out.indices[static_cast<size_t>(i)] =
                static_cast<uint32_t>(r.ReadVertexIndex(vIdxSize));
        }
        if (r.Failed()) {
            if (error) *error = "faces: unexpected end of file";
            return false;
        }

        // ---- textures ----
        int32_t textureCount = 0;
        if (!r.Read(textureCount) || !CheckCount(textureCount, r.Remaining(), "textures", error)) {
            if (r.Failed() && error) *error = "textures: unexpected end of file";
            return false;
        }
        out.textures.resize(static_cast<size_t>(textureCount));
        for (int32_t i = 0; i < textureCount; ++i) {
            if (!readText(&out.textures[static_cast<size_t>(i)])) {
                if (error) *error = "textures: unexpected end of file";
                return false;
            }
        }

        // ---- materials ----
        int32_t materialCount = 0;
        if (!r.Read(materialCount) || !CheckCount(materialCount, r.Remaining(), "materials", error)) {
            if (r.Failed() && error) *error = "materials: unexpected end of file";
            return false;
        }
        out.materials.resize(static_cast<size_t>(materialCount));
        uint64_t indexSum = 0;
        for (int32_t i = 0; i < materialCount; ++i) {
            PmxMaterial& m = out.materials[static_cast<size_t>(i)];
            if (!readText(&m.name) || !readText(&m.nameEn) || !r.Read(m.diffuse) ||
                !r.Read(m.specular) || !r.Read(m.specularPower) || !r.Read(m.ambient) ||
                !r.Read(m.flags) || !r.Read(m.edgeColor) || !r.Read(m.edgeSize)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            m.textureIndex = r.ReadIndex(tIdxSize);
            m.sphereTextureIndex = r.ReadIndex(tIdxSize);
            uint8_t sphereMode = 0;
            if (!r.Read(sphereMode)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            m.sphereMode = static_cast<PmxSphereMode>(sphereMode);
            uint8_t sharedToon = 0;
            if (!r.Read(sharedToon)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            m.sharedToon = (sharedToon == 1);
            if (sharedToon == 1) {
                uint8_t toon = 0;
                if (!r.Read(toon)) {
                    if (error) *error = "materials: unexpected end of file";
                    return false;
                }
                m.toonIndex = toon;
            } else {
                m.toonIndex = r.ReadIndex(tIdxSize);
            }
            if (!readText(&m.memo)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            int32_t count = 0;
            if (!r.Read(count)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            // Clamp so the running sum never exceeds indices.size().
            int64_t clamped = count;
            if (clamped < 0) clamped = 0;
            if (indexSum + clamped > out.indices.size()) {
                clamped = static_cast<int64_t>(out.indices.size()) - static_cast<int64_t>(indexSum);
                if (clamped < 0) clamped = 0;
            }
            indexSum += static_cast<uint64_t>(clamped);
            m.indexCount = static_cast<uint32_t>(clamped);
        }

        // ---- bones ----
        int32_t boneCount = 0;
        if (!r.Read(boneCount) || !CheckCount(boneCount, r.Remaining(), "bones", error)) {
            if (r.Failed() && error) *error = "bones: unexpected end of file";
            return false;
        }
        out.bones.resize(static_cast<size_t>(boneCount));
        for (int32_t i = 0; i < boneCount; ++i) {
            PmxBone& b = out.bones[static_cast<size_t>(i)];
            if (!readText(&b.name) || !readText(&b.nameEn) || !r.Read(b.position)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            b.parentIndex = r.ReadIndex(bIdxSize);
            if (!r.Read(b.deformLayer)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            uint16_t flags = 0;
            if (!r.Read(flags)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            b.flags = flags;
            if (flags & PmxBone_TailIsBone) b.tailBoneIndex = r.ReadIndex(bIdxSize);
            else if (!r.Read(b.tailOffset)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            if (flags & (PmxBone_AppendRotate | PmxBone_AppendTranslate)) {
                b.appendParentIndex = r.ReadIndex(bIdxSize);
                if (!r.Read(b.appendRatio)) {
                    if (error) *error = "bones: unexpected end of file";
                    return false;
                }
            }
            if (flags & PmxBone_FixedAxis) {
                if (!r.Read(b.fixedAxis)) {
                    if (error) *error = "bones: unexpected end of file";
                    return false;
                }
            }
            if (flags & PmxBone_LocalAxis) {
                if (!r.Read(b.localAxisX) || !r.Read(b.localAxisZ)) {
                    if (error) *error = "bones: unexpected end of file";
                    return false;
                }
            }
            if (flags & PmxBone_ExternalParent) {
                if (!r.Read(b.externalKey)) {
                    if (error) *error = "bones: unexpected end of file";
                    return false;
                }
            }
            if (flags & PmxBone_IK) {
                b.ikTargetIndex = r.ReadIndex(bIdxSize);
                if (!r.Read(b.ikLoopCount) || !r.Read(b.ikLimitAngle)) {
                    if (error) *error = "bones: unexpected end of file";
                    return false;
                }
                int32_t linkCount = 0;
                if (!r.Read(linkCount) || !CheckCount(linkCount, r.Remaining(), "bones", error)) {
                    if (r.Failed() && error) *error = "bones: unexpected end of file";
                    return false;
                }
                b.ikLinks.resize(static_cast<size_t>(linkCount));
                for (int32_t j = 0; j < linkCount; ++j) {
                    PmxIkLink& link = b.ikLinks[static_cast<size_t>(j)];
                    link.boneIndex = r.ReadIndex(bIdxSize);
                    uint8_t hasLimit = 0;
                    if (!r.Read(hasLimit)) {
                        if (error) *error = "bones: unexpected end of file";
                        return false;
                    }
                    link.hasLimit = (hasLimit != 0);
                    if (hasLimit) {
                        if (!r.Read(link.limitMin) || !r.Read(link.limitMax)) {
                            if (error) *error = "bones: unexpected end of file";
                            return false;
                        }
                    }
                }
            }
        }

        // ---- morphs ----
        int32_t morphCount = 0;
        if (!r.Read(morphCount) || !CheckCount(morphCount, r.Remaining(), "morphs", error)) {
            if (r.Failed() && error) *error = "morphs: unexpected end of file";
            return false;
        }
        out.morphs.resize(static_cast<size_t>(morphCount));
        for (int32_t i = 0; i < morphCount; ++i) {
            PmxMorph& m = out.morphs[static_cast<size_t>(i)];
            if (!readText(&m.name) || !readText(&m.nameEn) || !r.Read(m.panel)) {
                if (error) *error = "morphs: unexpected end of file";
                return false;
            }
            uint8_t type = 0;
            if (!r.Read(type)) {
                if (error) *error = "morphs: unexpected end of file";
                return false;
            }
            int32_t offsetCount = 0;
            if (!r.Read(offsetCount) ||
                !CheckCount(offsetCount, r.Remaining(), "morphs", error)) {
                if (r.Failed() && error) *error = "morphs: unexpected end of file";
                return false;
            }
            switch (type) {
                case 0:  // group
                case 9:  // flip
                    m.type = (type == 0) ? PmxMorphType::Group : PmxMorphType::Flip;
                    m.groupOffsets.resize(static_cast<size_t>(offsetCount));
                    for (auto& g : m.groupOffsets) {
                        g.morph = r.ReadIndex(morphIdxSize);
                        if (!r.Read(g.weight)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                case 1: {  // vertex
                    m.type = PmxMorphType::Vertex;
                    m.vertexOffsets.resize(static_cast<size_t>(offsetCount));
                    for (auto& vo : m.vertexOffsets) {
                        vo.vertex = r.ReadVertexIndex(vIdxSize);
                        if (!r.Read(vo.offset)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                }
                case 2:  // bone
                    m.type = PmxMorphType::Bone;
                    m.boneOffsets.resize(static_cast<size_t>(offsetCount));
                    for (auto& bo : m.boneOffsets) {
                        bo.bone = r.ReadIndex(bIdxSize);
                        if (!r.Read(bo.translation) || !r.Read(bo.rotation)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                case 3: case 4: case 5: case 6: case 7: {  // uv variants
                    m.type = static_cast<PmxMorphType>(type);
                    for (int32_t j = 0; j < offsetCount; ++j) {
                        // vertex index + float4: parsed but discarded.
                        r.ReadVertexIndex(vIdxSize);
                        if (!r.Skip(16)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                }
                case 8: {  // material
                    m.type = PmxMorphType::Material;
                    m.materialOffsets.resize(static_cast<size_t>(offsetCount));
                    for (auto& mo : m.materialOffsets) {
                        mo.material = r.ReadIndex(mIdxSize);
                        if (!r.Read(mo.operation) || !r.Read(mo.diffuse) ||
                            !r.Read(mo.specular) || !r.Read(mo.specularPower) ||
                            !r.Read(mo.ambient) || !r.Read(mo.edgeColor) ||
                            !r.Read(mo.edgeSize) || !r.Read(mo.textureFactor) ||
                            !r.Read(mo.sphereFactor) || !r.Read(mo.toonFactor)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                }
                case 10: {  // impulse
                    m.type = PmxMorphType::Impulse;
                    for (int32_t j = 0; j < offsetCount; ++j) {
                        // rigid index + uint8 + float3 + float3: parsed but discarded.
                        r.ReadIndex(rigidIdxSize);
                        if (!r.Skip(1 + 12 + 12)) {
                            if (error) *error = "morphs: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                }
                default:
                    if (error) *error = "morphs: unknown morph type " + std::to_string(type);
                    return false;
            }
            if (r.Failed()) {
                if (error) *error = "morphs: unexpected end of file";
                return false;
            }
        }

        // ---- display frames (tolerated truncation) ----
        bool truncated = false;
        auto readTextTolerant = [&](std::string* dst) -> bool {
            int32_t len = 0;
            if (!r.Read(len) || len < 0 || static_cast<size_t>(len) > r.Remaining()) {
                truncated = true;
                return false;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(len));
            if (len > 0 && !r.Read(bytes.data(), static_cast<size_t>(len))) {
                truncated = true;
                return false;
            }
            if (dst) {
                if (globals[0] == 0) *dst = Utf16LeToUtf8(bytes.data(), bytes.size());
                else dst->assign(bytes.begin(), bytes.end());
            }
            return true;
        };
        if (!truncated) {
            int32_t displayFrameCount = 0;
            if (!r.Read(displayFrameCount) || displayFrameCount < 0 ||
                static_cast<size_t>(displayFrameCount) * 4 > r.Remaining()) {
                truncated = true;
            }
            for (int32_t i = 0; i < displayFrameCount && !truncated; ++i) {
                PmxDisplayFrame frame;
                uint8_t special = 0;
                int32_t n = 0;
                if (!readTextTolerant(&frame.name) || !readTextTolerant(&frame.nameEn) ||
                    !r.Read(special) || !r.Read(n) || n < 0) {
                    truncated = true;
                    break;
                }
                frame.special = special != 0;
                for (int32_t j = 0; j < n && !truncated; ++j) {
                    uint8_t kind = 0;
                    if (!r.Read(kind)) { truncated = true; break; }
                    PmxDisplayFrame::Item item;
                    item.morph = kind != 0;
                    item.index = item.morph ? r.ReadIndex(morphIdxSize) : r.ReadIndex(bIdxSize);
                    if (r.Failed()) { truncated = true; break; }
                    const size_t limit = item.morph ? out.morphs.size() : out.bones.size();
                    if (item.index >= 0 && (size_t)item.index < limit) frame.items.push_back(item);
                }
                if (!truncated) out.displayFrames.push_back(std::move(frame));
            }
        }

        // ---- rigid bodies (tolerated truncation) ----
        if (!truncated) {
            int32_t rigidCount = 0;
            if (!r.Read(rigidCount) || rigidCount < 0 || rigidCount * 100 > static_cast<int32_t>(r.Remaining())) {
                truncated = true;
            }
            for (int32_t i = 0; i < rigidCount && !truncated; ++i) {
                PmxRigidBody body;
                if (!readTextTolerant(&body.name) || !readTextTolerant(&body.nameEn)) break;
                body.boneIndex = r.ReadIndex(bIdxSize);
                uint8_t group = 0;
                if (!r.Read(group)) { truncated = true; break; }
                body.group = group;
                if (!r.Read(body.collisionMask)) { truncated = true; break; }
                uint8_t shape = 0;
                if (!r.Read(shape)) { truncated = true; break; }
                body.shape = shape;
                if (!r.Read(body.size) || !r.Read(body.position) || !r.Read(body.rotation) ||
                    !r.Read(body.mass) || !r.Read(body.linearDamping) ||
                    !r.Read(body.angularDamping) || !r.Read(body.restitution) ||
                    !r.Read(body.friction)) {
                    truncated = true;
                    break;
                }
                uint8_t physicsMode = 0;
                if (!r.Read(physicsMode)) { truncated = true; break; }
                body.physicsMode = physicsMode;
                out.rigidBodies.push_back(body);
            }
        }

        // ---- joints (tolerated truncation) ----
        if (!truncated) {
            int32_t jointCount = 0;
            if (!r.Read(jointCount) || jointCount < 0 || jointCount * 100 > static_cast<int32_t>(r.Remaining())) {
                truncated = true;
            }
            for (int32_t i = 0; i < jointCount && !truncated; ++i) {
                PmxJoint joint;
                if (!readTextTolerant(&joint.name) || !readTextTolerant(&joint.nameEn)) break;
                if (!r.Read(joint.type)) { truncated = true; break; }
                joint.rigidBodyA = r.ReadIndex(rigidIdxSize);
                joint.rigidBodyB = r.ReadIndex(rigidIdxSize);
                if (!r.Read(joint.position) || !r.Read(joint.rotation) ||
                    !r.Read(joint.linearMin) || !r.Read(joint.linearMax) ||
                    !r.Read(joint.angularMin) || !r.Read(joint.angularMax) ||
                    !r.Read(joint.springLinear) || !r.Read(joint.springAngular)) {
                    truncated = true;
                    break;
                }
                out.joints.push_back(joint);
            }
        }
        if (truncated) {
            LOG_WARN("PMX %s: truncated in display frames / rigid bodies / joints, keeping what was parsed",
                     out.name.c_str());
        }

        // ---- post-parse fixes ----
        out.sourcePath = std::filesystem::absolute(path);

        const int32_t vtxCount = static_cast<int32_t>(out.vertices.size());
        const bool noBones = out.bones.empty();
        for (auto& v : out.vertices) {
            for (int k = 0; k < 4; ++k) {
                int32_t bi = v.boneIndex[k];
                // The signed sizes use negative values for "none"; treat anything
                // outside [0, boneCount) as out of range.
                if (bi < 0 || bi >= static_cast<int32_t>(out.bones.size())) {
                    if (noBones) {
                        v.boneIndex[k] = -1;
                        v.boneWeight[k] = 0;
                    } else {
                        v.boneIndex[k] = 0;
                        // Negative weights are ignored.
                        if (k != 0 && v.boneWeight[k] > 0 && v.boneWeight[0] > 0)
                            v.boneWeight[0] += v.boneWeight[k];
                        v.boneWeight[k] = 0;
                    }
                }
            }
        }
        for (auto& vo_i : out.indices) {
            if (static_cast<int32_t>(vo_i) >= vtxCount) vo_i = 0;
        }
        for (auto& v : out.vertices) {
            for (int k = 0; k < 4; ++k) {
                if (v.boneIndex[k] >= vtxCount) v.boneIndex[k] = 0;
            }
        }
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

// ---------------------------------------------------------------- ProbePmx

bool ProbePmx(const std::filesystem::path& path, PmxProbe& out, std::string* error) {
    try {
        out = PmxProbe{};
        std::vector<uint8_t> fileData;
        if (!ReadWholeFile(path, fileData, error)) return false;
        if (fileData.empty()) {
            if (error) *error = "empty file";
            return false;
        }

        BinaryReader r(fileData.data(), fileData.size());

        char magic[4] = {};
        if (!r.Read(magic, 4) || memcmp(magic, "PMX", 3) != 0) {
            if (error) *error = "not a PMX file";
            return false;
        }
        float version = 0;
        if (!r.Read(version)) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }
        uint8_t globalsCount = 0;
        if (!r.Read(globalsCount) || globalsCount < 8) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }
        uint8_t globals[8] = {};
        for (int i = 0; i < 8; ++i) {
            if (!r.Read(globals[i])) {
                if (error) *error = "header: unexpected end of file";
                return false;
            }
        }
        if (globalsCount > 8) {
            if (!r.Skip(globalsCount - 8)) {
                if (error) *error = "header: unexpected end of file";
                return false;
            }
        }
        const uint8_t vIdxSize = globals[2];
        const uint8_t bIdxSize = globals[5];

        auto readText = [&](std::string* dst) -> bool {
            int32_t len = 0;
            if (!r.Read(len)) return false;
            if (len < 0 || static_cast<size_t>(len) > r.Remaining()) return false;
            std::vector<uint8_t> bytes(static_cast<size_t>(len));
            if (len > 0 && !r.Read(bytes.data(), static_cast<size_t>(len))) return false;
            if (dst) {
                if (globals[0] == 0) *dst = Utf16LeToUtf8(bytes.data(), bytes.size());
                else dst->assign(bytes.begin(), bytes.end());
            }
            return true;
        };

        if (!readText(&out.name) || !readText(&out.nameEn) || !readText(&out.comment)) {
            if (error) *error = "header: unexpected end of file";
            return false;
        }
        if (!readText(nullptr)) {  // commentEn
            if (error) *error = "header: unexpected end of file";
            return false;
        }

        // ---- walk vertices ----
        int32_t vertexCount = 0;
        if (!r.Read(vertexCount) || !CheckCount(vertexCount, r.Remaining(), "vertices", error))
            return false;
        out.vertexCount = static_cast<uint32_t>(vertexCount);
        const int32_t vCount = vertexCount;
        for (int32_t i = 0; i < vCount; ++i) {
            if (!r.Skip(12 + 12 + 8)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
            if (globals[1] > 0 && !r.Skip(static_cast<size_t>(globals[1]) * 16)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
            uint8_t deform = 0;
            if (!r.Read(deform)) {
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
            switch (deform) {
                case 0:
                    r.ReadIndex(bIdxSize);
                    break;
                case 1:
                case 3:
                    r.ReadIndex(bIdxSize);
                    r.ReadIndex(bIdxSize);
                    if (!r.Skip(4)) {
                        if (error) *error = "vertices: unexpected end of file";
                        return false;
                    }
                    if (deform == 3) {
                        if (!r.Skip(12 + 12 + 12)) {
                            if (error) *error = "vertices: unexpected end of file";
                            return false;
                        }
                    }
                    break;
                case 2:
                case 4:
                    if (!r.Skip(static_cast<size_t>(bIdxSize) * 4 + 16)) {
                        if (error) *error = "vertices: unexpected end of file";
                        return false;
                    }
                    break;
                default:
                    if (error) *error = "vertices: unknown deform type " + std::to_string(deform);
                    return false;
            }
            if (!r.Skip(4)) {  // edgeScale
                if (error) *error = "vertices: unexpected end of file";
                return false;
            }
        }
        if (r.Failed()) {
            if (error) *error = "vertices: unexpected end of file";
            return false;
        }

        // ---- walk faces ----
        int32_t indexCount = 0;
        if (!r.Read(indexCount) ||
            !CheckCount(static_cast<int64_t>(indexCount) * vIdxSize, r.Remaining(), "faces", error)) {
            return false;
        }
        if (!r.Skip(static_cast<size_t>(indexCount) * vIdxSize)) {
            if (error) *error = "faces: unexpected end of file";
            return false;
        }
        out.faceCount = static_cast<uint32_t>(indexCount / 3);

        // ---- count textures ----
        int32_t textureCount = 0;
        if (!r.Read(textureCount) || !CheckCount(textureCount, r.Remaining(), "textures", error)) {
            return false;
        }
        out.textureCount = static_cast<uint32_t>(textureCount);
        for (int32_t i = 0; i < textureCount; ++i) {
            if (!readText(nullptr)) {
                if (error) *error = "textures: unexpected end of file";
                return false;
            }
        }

        // ---- count materials ----
        int32_t materialCount = 0;
        if (!r.Read(materialCount) || !CheckCount(materialCount, r.Remaining(), "materials", error)) {
            return false;
        }
        out.materialCount = static_cast<uint32_t>(materialCount);
        const int32_t tIdxSize = globals[3];
        for (int32_t i = 0; i < materialCount; ++i) {
            if (!readText(nullptr) || !readText(nullptr)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            if (!r.Skip(16 + 12 + 4 + 12 + 1 + 16 + 4)) {  // diffuse..edgeSize
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            if (!r.Skip(static_cast<size_t>(tIdxSize) * 2 + 1)) {  // tex+sphere idx, sphereMode
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            uint8_t sharedToon = 0;
            if (!r.Read(sharedToon)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            if (sharedToon == 1) {
                if (!r.Skip(1)) {
                    if (error) *error = "materials: unexpected end of file";
                    return false;
                }
            } else {
                if (!r.Skip(tIdxSize)) {
                    if (error) *error = "materials: unexpected end of file";
                    return false;
                }
            }
            if (!readText(nullptr)) {  // memo
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
            int32_t cnt = 0;
            if (!r.Read(cnt)) {
                if (error) *error = "materials: unexpected end of file";
                return false;
            }
        }
        if (r.Failed()) {
            if (error) *error = "materials: unexpected end of file";
            return false;
        }

        // ---- read bones (names only) ----
        int32_t boneCount = 0;
        if (!r.Read(boneCount) || !CheckCount(boneCount, r.Remaining(), "bones", error)) {
            return false;
        }
        out.boneCount = static_cast<uint32_t>(boneCount);
        out.boneNames.resize(static_cast<size_t>(boneCount));
        for (int32_t i = 0; i < boneCount; ++i) {
            if (!readText(&out.boneNames[static_cast<size_t>(i)])) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            // nameEn only (skip); stop after the bone section — no need to walk
            // the rest of each bone record.
            if (!readText(nullptr)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            r.Skip(12 + bIdxSize + 4);  // position + parent + deformLayer
            uint16_t flags = 0;
            if (!r.Read(flags)) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
            if (flags & PmxBone_TailIsBone) r.Skip(bIdxSize);
            else r.Skip(12);
            if (flags & (PmxBone_AppendRotate | PmxBone_AppendTranslate)) r.Skip(bIdxSize + 4);
            if (flags & PmxBone_FixedAxis) r.Skip(12);
            if (flags & PmxBone_LocalAxis) r.Skip(24);
            if (flags & PmxBone_ExternalParent) r.Skip(4);
            if (flags & PmxBone_IK) {
                r.Skip(bIdxSize + 4 + 4);  // target, loopCount, limitAngle
                int32_t linkCount = 0;
                if (!r.Read(linkCount) || !CheckCount(linkCount, r.Remaining(), "bones", error)) {
                    return false;
                }
                for (int32_t j = 0; j < linkCount; ++j) {
                    r.Skip(bIdxSize);  // link bone index
                    uint8_t hasLimit = 0;
                    if (!r.Read(hasLimit)) {
                        if (error) *error = "bones: unexpected end of file";
                        return false;
                    }
                    if (hasLimit) r.Skip(24);
                }
            }
            if (r.Failed()) {
                if (error) *error = "bones: unexpected end of file";
                return false;
            }
        }
        return true;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

// ---------------------------------------------------------------- ResolveTexturePath

std::filesystem::path PmxModel::ResolveTexturePath(int32_t index) const {
    if (index < 0 || index >= static_cast<int32_t>(textures.size())) return {};
    std::string rel = textures[static_cast<size_t>(index)];
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    while (rel.rfind("./", 0) == 0) rel = rel.substr(2);
    size_t begin = rel.find_first_not_of(" \t\r\n");
    size_t end = rel.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    rel = rel.substr(begin, end - begin + 1);
    if (rel.empty()) return {};

    std::filesystem::path dir = sourcePath.parent_path();
    std::filesystem::path p = dir / Utf8ToPath(rel);
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) return p;

    // Case-insensitive filename match, then same stem with other extensions.
    std::filesystem::path targetName = p.filename();
    std::filesystem::path targetStem = p.stem();
    const std::string lowerTargetName = ToLowerAscii(PathToUtf8(targetName));
    const std::string lowerTargetStem = ToLowerAscii(PathToUtf8(targetStem));

    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator it(p.parent_path(), ec), endIt;
         it != endIt; it.increment(ec)) {
        if (ec) break;
        entries.push_back(it->path());
    }

    for (const auto& entry : entries) {
        std::error_code ec2;
        if (!std::filesystem::is_regular_file(entry, ec2)) continue;
        if (ToLowerAscii(PathToUtf8(entry.filename())) == lowerTargetName) return entry;
    }
    for (const char* ext : kTexExts) {
        for (const auto& entry : entries) {
            std::error_code ec2;
            if (!std::filesystem::is_regular_file(entry, ec2)) continue;
            if (ToLowerAscii(PathToUtf8(entry.stem())) == lowerTargetStem &&
                ToLowerAscii(PathToUtf8(entry.extension())) == ext) {
                return entry;
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------- FindBone / FindMorph

int32_t PmxModel::FindBone(std::string_view name) const {
    for (size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t PmxModel::FindMorph(std::string_view name) const {
    for (size_t i = 0; i < morphs.size(); ++i) {
        if (morphs[i].name == name) return static_cast<int32_t>(i);
    }
    return -1;
}

} // namespace mmdx
