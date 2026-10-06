// Format dispatch and the ImpScene -> PmxModel conversion (see ModelImport.h).
#include "asset/ModelImport.h"
#include "asset/ImportScene.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>

namespace mmdx {

using namespace DirectX;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kUnitsPerMeter = 12.5f;      // MMD: 1 unit = 8 cm
constexpr float kMmdArmAngleDeg = 38.0f;     // upper arm below horizontal: 36..42 deg in common MMD rigs (median 38)

// ---------------------------------------------------------------- humanoid name dictionary

struct SlotName {
    const char* slot;     // VRM 1.0 humanoid slot; "L"/"R" prefixes are added for sided slots
    bool sided;
    std::vector<const char*> cores;  // canonical name cores (see Canon)
};

const std::vector<SlotName>& SlotNames() {
    static const std::vector<SlotName> k = {
        {"hips", false, {"hips", "hip", "pelvis"}},
        {"spine", false, {"spine", "spine01", "spine0", "abdomen", "lowerspine"}},
        {"chest", false, {"spine1", "chest", "spine02", "torso", "midspine"}},
        {"upperChest", false, {"spine2", "upperchest", "spine03", "chest2", "upperspine"}},
        {"neck", false, {"neck", "neck01", "neck1"}},
        {"head", false, {"head"}},
        {"Eye", true, {"eye"}},
        {"Shoulder", true, {"shoulder", "clavicle", "collar", "collarbone"}},
        {"UpperArm", true, {"arm", "upperarm", "uparm", "armupper"}},
        {"LowerArm", true, {"forearm", "lowerarm", "elbow", "loarm", "armlower"}},
        {"Hand", true, {"hand", "wrist"}},
        {"UpperLeg", true, {"upleg", "upperleg", "thigh", "legupper"}},
        {"LowerLeg", true, {"leg", "lowerleg", "calf", "shin", "knee", "leglower"}},
        {"Foot", true, {"foot", "ankle"}},
        {"Toes", true, {"toebase", "toe", "toes", "ball"}},
    };
    return k;
}

// Splits a bone name into lower-case tokens (separators and camelCase), drops rig prefixes
// and reads the side. "mixamorig:LeftForeArm" -> side 'L', core "forearm";
// "J_Bip_R_UpperArm" -> 'R', "upperarm"; "DEF-thigh.L" -> 'L', "thigh".
struct Canonical {
    char side = 0;
    std::string core;
};

Canonical Canon(const std::string& raw) {
    std::string name = raw;
    if (const size_t c = name.find_last_of(":|"); c != std::string::npos) name = name.substr(c + 1);
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) tokens.push_back(ToLowerAscii(cur));
        cur.clear();
    };
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char ch = (unsigned char)name[i];
        if (!std::isalnum(ch)) {
            flush();
            continue;
        }
        // camelCase boundary: lower/digit -> upper
        if (std::isupper(ch) && !cur.empty() && (std::islower((unsigned char)cur.back()) || std::isdigit((unsigned char)cur.back())))
            flush();
        cur.push_back((char)ch);
    }
    flush();
    static const std::set<std::string> kFiller = {"mixamorig", "bip01", "bip001", "bip", "biped", "j", "c", "def", "b",
                                                  "jnt", "joint", "bone", "cc", "base", "rig", "sk", "drv", "org", "mch"};
    Canonical out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "l" || t == "left") {
            out.side = 'L';
            continue;
        }
        if (t == "r" || t == "right") {
            out.side = 'R';
            continue;
        }
        if (kFiller.count(t)) continue;
        out.core += t;
    }
    // Mixamo finger names: "LeftHandThumb1" -> "handthumb1"
    if (out.core.rfind("hand", 0) == 0 && out.core.size() > 4 && !std::isdigit((unsigned char)out.core[4])) out.core = out.core.substr(4);
    return out;
}

// Finger slots: thumb -> Metacarpal/Proximal/Distal, others -> Proximal/Intermediate/Distal.
std::string FingerSlot(const std::string& core) {
    static const std::pair<const char*, const char*> kFingers[] = {
        {"thumb", "Thumb"}, {"index", "Index"}, {"middle", "Middle"}, {"ring", "Ring"}, {"pinky", "Little"}, {"little", "Little"}};
    for (auto& [prefix, finger] : kFingers) {
        const size_t len = std::strlen(prefix);
        if (core.rfind(prefix, 0) != 0) continue;
        std::string rest = core.substr(len);
        if (!rest.empty() && rest[0] == '0') rest = rest.substr(1);
        if (rest.size() != 1 || rest[0] < '1' || rest[0] > '3') return {};
        static const char* kThumb[] = {"Metacarpal", "Proximal", "Distal"};
        static const char* kOther[] = {"Proximal", "Intermediate", "Distal"};
        const int n = rest[0] - '1';
        return std::string(finger) + (std::string(finger) == "Thumb" ? kThumb[n] : kOther[n]);
    }
    return {};
}

// VRM slot -> MMD bone name.
const std::map<std::string, std::string>& MmdNames() {
    static const std::map<std::string, std::string> k = [] {
        std::map<std::string, std::string> m = {
            {"neck", "首"}, {"head", "頭"},
            {"leftEye", "左目"}, {"rightEye", "右目"},
            {"leftShoulder", "左肩"}, {"rightShoulder", "右肩"},
            {"leftUpperArm", "左腕"}, {"rightUpperArm", "右腕"},
            {"leftLowerArm", "左ひじ"}, {"rightLowerArm", "右ひじ"},
            {"leftHand", "左手首"}, {"rightHand", "右手首"},
            {"leftUpperLeg", "左足"}, {"rightUpperLeg", "右足"},
            {"leftLowerLeg", "左ひざ"}, {"rightLowerLeg", "右ひざ"},
            {"leftFoot", "左足首"}, {"rightFoot", "右足首"},
            {"leftToes", "左つま先"}, {"rightToes", "右つま先"},
        };
        const char* fingers[][4] = {{"Thumb", "親指０", "親指１", "親指２"}, {"Index", "人指１", "人指２", "人指３"},
                                    {"Middle", "中指１", "中指２", "中指３"}, {"Ring", "薬指１", "薬指２", "薬指３"},
                                    {"Little", "小指１", "小指２", "小指３"}};
        for (auto& f : fingers) {
            const bool thumb = std::string(f[0]) == "Thumb";
            const char* seg[3] = {thumb ? "Metacarpal" : "Proximal", thumb ? "Proximal" : "Intermediate", "Distal"};
            for (int i = 0; i < 3; ++i) {
                m[std::string("left") + f[0] + seg[i]] = std::string("左") + f[i + 1];
                m[std::string("right") + f[0] + seg[i]] = std::string("右") + f[i + 1];
            }
        }
        return m;
    }();
    return k;
}

// ---------------------------------------------------------------- scene helpers

XMMATRIX Load(const XMFLOAT4X4& m) { return XMLoadFloat4x4(&m); }
XMFLOAT3 Translation(const XMFLOAT4X4& m) { return {m._41, m._42, m._43}; }

// Nodes that skins reference, plus mesh nodes' ancestors are looked up separately.
std::vector<char> JointSet(const ImpScene& s) {
    std::vector<char> isJoint(s.nodes.size(), 0);
    for (const ImpSkin& sk : s.skins)
        for (int j : sk.joints)
            if (j >= 0 && (size_t)j < isJoint.size()) isJoint[j] = 1;
    return isJoint;
}

// Rest-pose (current node transforms) skinning matrix of one vertex, or the node matrix.
XMMATRIX VertexMatrix(const ImpScene& s, const ImpMesh& m, size_t v) {
    if (m.skin < 0 || m.joints.empty()) return Load(s.nodes[m.node].world);
    const ImpSkin& sk = s.skins[m.skin];
    const float* w = &m.weights[v].x;
    float sum = 0;
    XMMATRIX acc = XMMatrixSet(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int k = 0; k < 4; ++k) {
        const int j = m.joints[v][k];
        if (j < 0 || (size_t)j >= sk.joints.size() || w[k] <= 0) continue;
        const XMMATRIX jm = Load(sk.inverseBind[j]) * Load(s.nodes[sk.joints[j]].world);
        acc += jm * w[k];
        sum += w[k];
    }
    if (sum <= 1e-6f) return Load(s.nodes[m.node].world);
    return acc * (1.0f / sum);
}

void BoundsOf(const ImpScene& s, XMFLOAT3& lo, XMFLOAT3& hi) {
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (const ImpMesh& m : s.meshes) {
        if (m.node < 0) continue;
        for (size_t v = 0; v < m.positions.size(); ++v) {
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&m.positions[v]), VertexMatrix(s, m, v)));
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
    if (lo.x > hi.x) lo = hi = {0, 0, 0};
}

// ---------------------------------------------------------------- humanoid mapping

struct Humanoid {
    std::map<std::string, int> slots;  // VRM slot -> node
    bool ok = false;
    std::string note;
    float facing = 1;                  // +1: model faces +Z (glTF convention), -1: faces -Z
    int Node(const std::string& slot) const {
        auto it = slots.find(slot);
        return it == slots.end() ? -1 : it->second;
    }
};

Humanoid MapHumanoid(const ImpScene& s) {
    Humanoid h;
    const std::vector<char> isJoint = JointSet(s);
    if (!s.humanBones.empty()) {
        for (auto& [slot, node] : s.humanBones)
            if (node >= 0 && (size_t)node < s.nodes.size()) h.slots[slot] = node;
    } else {
        // Shallowest match wins (depth order), so "Hips" beats a deeper "hip" helper.
        std::vector<int> depth(s.nodes.size(), 0);
        std::function<int(int)> depthOf = [&](int n) { return s.nodes[n].parent < 0 ? 0 : 1 + depthOf(s.nodes[n].parent); };
        std::vector<int> order;
        for (size_t i = 0; i < s.nodes.size(); ++i)
            if (isJoint[i]) {
                depth[i] = depthOf((int)i);
                order.push_back((int)i);
            }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[a] < depth[b]; });
        for (int n : order) {
            const Canonical c = Canon(s.nodes[n].name);
            std::string slot;
            for (const SlotName& sn : SlotNames()) {
                if (sn.sided != (c.side != 0)) continue;
                for (const char* core : sn.cores)
                    if (c.core == core) {
                        slot = sn.sided ? std::string(c.side == 'L' ? "left" : "right") + sn.slot : sn.slot;
                        break;
                    }
                if (!slot.empty()) break;
            }
            if (slot.empty() && c.side) {
                const std::string f = FingerSlot(c.core);
                if (!f.empty()) slot = std::string(c.side == 'L' ? "left" : "right") + f;
            }
            if (!slot.empty() && !h.slots.count(slot)) h.slots[slot] = n;
        }
    }

    static const char* kRequired[] = {"hips", "head", "leftUpperArm", "rightUpperArm", "leftLowerArm", "rightLowerArm",
                                      "leftUpperLeg", "rightUpperLeg", "leftLowerLeg",
                                      "rightLowerLeg", "leftFoot", "rightFoot"};
    for (const char* r : kRequired)
        if (h.Node(r) < 0) {
            h.note = std::string("no ") + r + " bone";
            return h;
        }
    if (h.Node("spine") < 0 && h.Node("chest") < 0) {
        h.note = "no spine bone";
        return h;
    }
    auto P = [&](const char* slot) { return Translation(s.nodes[h.Node(slot)].world); };
    const XMFLOAT3 hips = P("hips"), head = P("head"), lf = P("leftFoot"), rf = P("rightFoot");
    const XMFLOAT3 lul = P("leftUpperLeg"), lll = P("leftLowerLeg"), lua = P("leftUpperArm"), rua = P("rightUpperArm");
    const float height = head.y - std::min(lf.y, rf.y);
    if (!(height > 1e-4f)) {
        h.note = "head is not above the feet";
        return h;
    }
    if (!(hips.y > std::max(lf.y, rf.y) && head.y > hips.y && lul.y > lll.y && lll.y > lf.y)) {
        h.note = "bones are not stacked like a standing humanoid";
        return h;
    }
    // Upright torso: quadrupeds have the head in front of the hips, not above them.
    const float dx = head.x - hips.x, dz = head.z - hips.z, dy = head.y - hips.y;
    if (std::sqrt(dx * dx + dz * dz) > dy) {
        h.note = "torso is not upright";
        return h;
    }
    const float side = lua.x - rua.x;
    if (std::fabs(side) < 0.1f * height || std::fabs(side) < std::fabs(lua.z - rua.z)) {
        h.note = "arms are not left/right of the body";
        return h;
    }
    h.facing = side > 0 ? 1.0f : -1.0f;  // facing +Z puts the model's left hand at +X
    h.ok = true;
    return h;
}

// Rotates the upper arms so the rest pose matches MMD's A-pose (VMD rotations are relative
// to the model's rest pose, so a T-pose model would dance with raised arms).
void ToMmdArmPose(ImpScene& s, const Humanoid& h) {
    for (const char* side : {"left", "right"}) {
        const int arm = h.Node(std::string(side) + "UpperArm");
        const int elbow = h.Node(std::string(side) + "LowerArm");
        if (arm < 0 || elbow < 0) continue;
        const XMFLOAT3 a = Translation(s.nodes[arm].world), e = Translation(s.nodes[elbow].world);
        const XMVECTOR d = XMVector3Normalize(XMVectorSet(e.x - a.x, e.y - a.y, e.z - a.z, 0));
        const XMVECTOR horiz = XMVectorSet(XMVectorGetX(d), 0, XMVectorGetZ(d), 0);
        if (XMVectorGetX(XMVector3Length(horiz)) < 0.2f) continue;  // arms already hang down
        const float ang = kMmdArmAngleDeg * kPi / 180.0f;
        const XMVECTOR t = XMVector3Normalize(XMVector3Normalize(horiz) * std::cos(ang) + XMVectorSet(0, -std::sin(ang), 0, 0));
        const XMVECTOR axis = XMVector3Cross(d, t);
        const float len = XMVectorGetX(XMVector3Length(axis));
        if (len < 1e-5f) continue;
        const float angle = std::atan2(len, XMVectorGetX(XMVector3Dot(d, t)));
        const XMMATRIX rot = XMMatrixTranslation(-a.x, -a.y, -a.z) * XMMatrixRotationAxis(axis / len, angle) * XMMatrixTranslation(a.x, a.y, a.z);
        const XMMATRIX newWorld = Load(s.nodes[arm].world) * rot;
        const int parent = s.nodes[arm].parent;
        const XMMATRIX parentWorld = parent >= 0 ? Load(s.nodes[parent].world) : XMMatrixIdentity();
        XMStoreFloat4x4(&s.nodes[arm].local, newWorld * XMMatrixInverse(nullptr, parentWorld));
        s.UpdateWorld();
    }
}

// ---------------------------------------------------------------- PMX building

struct Builder {
    const ImpScene& s;
    PmxModel& out;
    float scale = kUnitsPerMeter;
    float fx = 1, fz = -1;  // source -> MMD axis signs (a mirror, so the winding flips)

    XMFLOAT3 Pos(XMFLOAT3 p) const { return {p.x * fx * scale, p.y * scale, p.z * fz * scale}; }
    XMFLOAT3 Dir(XMFLOAT3 n) const { return {n.x * fx, n.y, n.z * fz}; }
    XMFLOAT3 Off(XMFLOAT3 n) const { return {n.x * fx * scale, n.y * scale, n.z * fz * scale}; }
};

PmxBone MakeBone(const std::string& name, XMFLOAT3 pos, int parent, uint16_t extraFlags = 0) {
    PmxBone b;
    b.name = name;
    b.position = pos;
    b.parentIndex = parent;
    b.flags = PmxBone_Rotatable | PmxBone_Visible | PmxBone_Operable | extraFlags;
    return b;
}

void BuildTextures(const ImpScene& s, PmxModel& out) {
    bool anyEmbedded = false;
    for (const ImpTexture& t : s.textures) anyEmbedded |= !t.bytes.empty();
    for (const ImpTexture& t : s.textures) {
        out.textures.push_back(!t.file.empty() ? PathToUtf8(t.file) : t.label);
        if (anyEmbedded) out.embeddedTextures.push_back(t.bytes);
    }
}

void BuildMaterialsAndGeometry(const ImpScene& s, Builder& b, bool character, const std::vector<int>& boneOfNode,
                               const std::vector<int>& skinJointBone /*flattened per skin*/, const std::vector<size_t>& skinBase,
                               std::vector<uint32_t>& meshVertexBase) {
    PmxModel& out = b.out;
    meshVertexBase.assign(s.meshes.size(), 0);
    // Vertices, mesh by mesh.
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const ImpMesh& m = s.meshes[mi];
        meshVertexBase[mi] = (uint32_t)out.vertices.size();
        std::vector<XMFLOAT3> normals = m.normals;
        if (normals.size() != m.positions.size()) {
            normals.assign(m.positions.size(), {0, 0, 0});
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                const uint32_t a = m.indices[i], c = m.indices[i + 1], d = m.indices[i + 2];
                if (a >= normals.size() || c >= normals.size() || d >= normals.size()) continue;
                const XMVECTOR pa = XMLoadFloat3(&m.positions[a]);
                const XMVECTOR n = XMVector3Cross(XMLoadFloat3(&m.positions[c]) - pa, XMLoadFloat3(&m.positions[d]) - pa);
                for (uint32_t v : {a, c, d}) XMStoreFloat3(&normals[v], XMLoadFloat3(&normals[v]) + n);
            }
        }
        for (size_t v = 0; v < m.positions.size(); ++v) {
            const XMMATRIX M = VertexMatrix(s, m, v);
            PmxVertex pv;
            XMFLOAT3 p, n;
            XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&m.positions[v]), M));
            XMStoreFloat3(&n, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&normals[v]), M)));
            pv.position = b.Pos(p);
            pv.normal = b.Dir(n);
            pv.uv = m.uvs.size() == m.positions.size() ? m.uvs[v] : XMFLOAT2{0, 0};
            pv.deform = PmxDeform::BDEF1;
            pv.boneIndex[0] = 0;
            pv.boneWeight[0] = 1;
            if (character) {
                if (m.skin >= 0 && !m.joints.empty()) {
                    // Merge duplicate bones, keep the four largest, renormalise.
                    std::vector<std::pair<int, float>> bw;
                    const float* w = &m.weights[v].x;
                    for (int k = 0; k < 4; ++k) {
                        const int j = m.joints[v][k];
                        if (j < 0 || w[k] <= 0 || (size_t)j >= s.skins[m.skin].joints.size()) continue;
                        const int bone = skinJointBone[skinBase[m.skin] + j];
                        auto it = std::find_if(bw.begin(), bw.end(), [&](auto& e) { return e.first == bone; });
                        if (it != bw.end()) it->second += w[k];
                        else bw.push_back({bone, w[k]});
                    }
                    std::sort(bw.begin(), bw.end(), [](auto& x, auto& y) { return x.second > y.second; });
                    if (bw.size() > 4) bw.resize(4);
                    float sum = 0;
                    for (auto& e : bw) sum += e.second;
                    if (sum > 1e-6f) {
                        for (size_t k = 0; k < bw.size(); ++k) {
                            pv.boneIndex[k] = bw[k].first;
                            pv.boneWeight[k] = bw[k].second / sum;
                        }
                        pv.deform = bw.size() == 1 ? PmxDeform::BDEF1 : bw.size() == 2 ? PmxDeform::BDEF2 : PmxDeform::BDEF4;
                        if (pv.deform == PmxDeform::BDEF2) pv.boneWeight[1] = 1 - pv.boneWeight[0];
                    } else {
                        pv.boneIndex[0] = boneOfNode[m.node];
                    }
                } else {
                    pv.boneIndex[0] = boneOfNode[m.node];
                }
            }
            out.vertices.push_back(pv);
        }
    }

    // Indices grouped by material (PMX draws material ranges in order). The mirror flips the winding.
    const int matCount = (int)s.materials.size();
    std::vector<std::vector<uint32_t>> perMat(matCount + 1);
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const ImpMesh& m = s.meshes[mi];
        const int slot = m.material >= 0 && m.material < matCount ? m.material : matCount;
        auto& dst = perMat[slot];
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const uint32_t a = m.indices[i], c = m.indices[i + 1], d = m.indices[i + 2];
            if (a >= m.positions.size() || c >= m.positions.size() || d >= m.positions.size()) continue;
            dst.push_back(meshVertexBase[mi] + a);
            dst.push_back(meshVertexBase[mi] + d);
            dst.push_back(meshVertexBase[mi] + c);
        }
    }
    for (int slot = 0; slot <= matCount; ++slot) {
        if (perMat[slot].empty()) continue;
        PmxMaterial pm;
        XMFLOAT4 base{0.8f, 0.8f, 0.8f, 1.0f};
        if (slot < matCount) {
            const ImpMaterial& im = s.materials[slot];
            pm.name = im.name;
            base = im.baseColor;
            pm.textureIndex = im.texture;
            if (im.doubleSided) pm.flags |= PmxMat_DoubleSided;
        } else {
            pm.name = "default";
        }
        // MMD colour model: saturate(ambient + diffuse * light) * texture. d = base, a = base / 2
        // gives "fully lit = texture colour" like most PMX rigs.
        pm.diffuse = base;
        pm.ambient = {base.x * 0.5f, base.y * 0.5f, base.z * 0.5f};
        pm.specular = {0, 0, 0};
        pm.specularPower = 5;
        pm.flags |= PmxMat_GroundShadow | PmxMat_CastShadow | PmxMat_ReceiveShadow;
        if (character) {
            pm.sharedToon = true;
            pm.toonIndex = 1;  // toon02: soft ramp
        }
        pm.edgeSize = 0;
        pm.indexCount = (uint32_t)perMat[slot].size();
        out.indices.insert(out.indices.end(), perMat[slot].begin(), perMat[slot].end());
        out.materials.push_back(std::move(pm));
    }
}

// Vertex morphs from morph targets (merged by name across meshes), then MMD-named aliases.
void BuildMorphs(const ImpScene& s, Builder& b, const std::vector<uint32_t>& meshVertexBase) {
    PmxModel& out = b.out;
    std::map<std::string, std::map<uint32_t, XMFLOAT3>> byName;                       // name -> vertex -> offset
    std::map<std::pair<std::string, int>, std::map<uint32_t, XMFLOAT3>> byTarget;     // (group, index)
    std::vector<std::string> nameOrder;
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const ImpMesh& m = s.meshes[mi];
        for (size_t t = 0; t < m.morphs.size(); ++t) {
            const ImpMorphTarget& mt = m.morphs[t];
            if (mt.vertices.empty()) continue;
            if (!byName.count(mt.name)) nameOrder.push_back(mt.name);
            auto& named = byName[mt.name];
            auto& target = byTarget[{m.morphGroup, (int)t}];
            for (size_t k = 0; k < mt.vertices.size(); ++k) {
                const uint32_t v = mt.vertices[k];
                if (v >= m.positions.size()) continue;
                XMFLOAT3 o;
                XMStoreFloat3(&o, XMVector3TransformNormal(XMLoadFloat3(&mt.offsets[k]), VertexMatrix(s, m, v)));
                o = b.Off(o);
                named[meshVertexBase[mi] + v] = o;
                target[meshVertexBase[mi] + v] = o;
            }
        }
    }
    auto addVertexMorph = [&](const std::string& name, const std::map<uint32_t, XMFLOAT3>& offsets, uint8_t panel) {
        PmxMorph pm;
        pm.name = name;
        pm.panel = panel;
        pm.type = PmxMorphType::Vertex;
        for (auto& [v, o] : offsets) pm.vertexOffsets.push_back({(int32_t)v, o});
        out.morphs.push_back(std::move(pm));
    };
    for (const std::string& name : nameOrder) addVertexMorph(name, byName[name], 4);

    struct Alias {
        const char* mmd;
        uint8_t panel;
        const char* vrmPreset;
        std::vector<std::vector<const char*>> sources;  // alternatives; each is a set summed together
    };
    static const std::vector<Alias> kAliases = {
        {"あ", 3, "aa", {{"viseme_aa"}, {"Fcl_MTH_A"}, {"vrc.v_aa"}, {"mouthOpen"}, {"jawOpen"}, {"A"}, {"a"}}},
        {"い", 3, "ih", {{"viseme_I"}, {"Fcl_MTH_I"}, {"vrc.v_ih"}, {"I"}, {"i"}}},
        {"う", 3, "ou", {{"viseme_U"}, {"Fcl_MTH_U"}, {"vrc.v_ou"}, {"mouthFunnel"}, {"U"}, {"u"}}},
        {"え", 3, "ee", {{"viseme_E"}, {"Fcl_MTH_E"}, {"vrc.v_e"}, {"E"}, {"e"}}},
        {"お", 3, "oh", {{"viseme_O"}, {"Fcl_MTH_O"}, {"vrc.v_oh"}, {"mouthPucker"}, {"O"}, {"o"}}},
        {"まばたき", 2, "blink", {{"eyesClosed"}, {"Fcl_EYE_Close"}, {"Blink"}, {"blink"}, {"eyeBlinkLeft", "eyeBlinkRight"}, {"eyeBlink_L", "eyeBlink_R"}}},
        {"笑い", 2, "happy", {{"Fcl_EYE_Joy"}, {"eyeSquintLeft", "eyeSquintRight"}}},
        {"ウィンク", 2, "blinkLeft", {{"Fcl_EYE_Close_L"}, {"eyeBlinkLeft"}, {"eyeBlink_L"}}},
        {"ウィンク右", 2, "blinkRight", {{"Fcl_EYE_Close_R"}, {"eyeBlinkRight"}, {"eyeBlink_R"}}},
    };
    for (const Alias& a : kAliases) {
        if (out.FindMorph(a.mmd) >= 0) continue;
        // VRM expression preset: build the vertex morph from its weighted binds.
        auto ex = std::find_if(s.expressions.begin(), s.expressions.end(), [&](const ImpExpression& e) { return e.preset == a.vrmPreset; });
        if (ex != s.expressions.end()) {
            std::map<uint32_t, XMFLOAT3> sum;
            for (const auto& bind : ex->binds) {
                auto it = byTarget.find({bind.morphGroup, bind.target});
                if (it == byTarget.end()) continue;
                for (auto& [v, o] : it->second) {
                    XMFLOAT3& d = sum[v];
                    d = {d.x + o.x * bind.weight, d.y + o.y * bind.weight, d.z + o.z * bind.weight};
                }
            }
            if (!sum.empty()) {
                addVertexMorph(a.mmd, sum, a.panel);
                continue;
            }
        }
        // Name aliases: a group morph over existing morphs (case-insensitive match).
        for (const auto& set : a.sources) {
            std::vector<int> found;
            for (const char* src : set) {
                int idx = -1;
                for (size_t i = 0; i < out.morphs.size() && idx < 0; ++i)
                    if (out.morphs[i].type == PmxMorphType::Vertex && ToLowerAscii(out.morphs[i].name) == ToLowerAscii(src)) idx = (int)i;
                if (idx < 0) break;
                found.push_back(idx);
            }
            if (found.size() != set.size()) continue;
            PmxMorph pm;
            pm.name = a.mmd;
            pm.panel = a.panel;
            pm.type = PmxMorphType::Group;
            for (int idx : found) pm.groupOffsets.push_back({idx, 1.0f});
            out.morphs.push_back(std::move(pm));
            break;
        }
    }
}

// MMD stages are built around the dancer at the origin; generic scenes are not. When nothing
// walkable lies under the origin, move the scene so its main floor (the height with the most
// upward-facing area) is at y = 0, centred on a floor triangle near that floor's middle.
void PlaceStage(PmxModel& out) {
    struct Tri {
        XMFLOAT3 a, b, c, centroid;
        float area;
    };
    std::vector<Tri> floors;
    for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
        const XMFLOAT3 a = out.vertices[out.indices[i]].position, b = out.vertices[out.indices[i + 1]].position,
                       c = out.vertices[out.indices[i + 2]].position;
        XMFLOAT3 n;
        XMStoreFloat3(&n, XMVector3Cross(XMLoadFloat3(&b) - XMLoadFloat3(&a), XMLoadFloat3(&c) - XMLoadFloat3(&a)));
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        // Up-facing by the shading normals (the geometric winding is mirrored on import).
        const float ny = out.vertices[out.indices[i]].normal.y + out.vertices[out.indices[i + 1]].normal.y +
                         out.vertices[out.indices[i + 2]].normal.y;
        if (len < 1e-8f || ny < 3 * 0.85f) continue;
        floors.push_back({a, b, c, {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3}, 0.5f * len});
    }
    if (floors.empty()) return;
    auto containsOrigin = [](const Tri& t) {
        auto edge = [](XMFLOAT3 p, XMFLOAT3 q) { return (q.x - p.x) * (0 - p.z) - (q.z - p.z) * (0 - p.x); };
        const float e0 = edge(t.a, t.b), e1 = edge(t.b, t.c), e2 = edge(t.c, t.a);
        return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    };
    const float tol = 0.5f * kUnitsPerMeter;
    for (const Tri& t : floors)
        if (std::fabs(t.centroid.y) < tol && containsOrigin(t)) return;  // a floor at the origin: authored for it

    std::map<int, float> areaByHeight;  // 25 cm bins
    const float bin = 0.25f * kUnitsPerMeter;
    for (const Tri& t : floors) areaByHeight[(int)std::floor(t.centroid.y / bin)] += t.area;
    const int best = std::max_element(areaByHeight.begin(), areaByHeight.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
    double sx = 0, sz = 0, sa = 0;
    std::vector<const Tri*> level;
    for (const Tri& t : floors)
        if (std::abs((int)std::floor(t.centroid.y / bin) - best) <= 1) {
            level.push_back(&t);
            sx += t.centroid.x * t.area;
            sz += t.centroid.z * t.area;
            sa += t.area;
        }
    const float cx = (float)(sx / sa), cz = (float)(sz / sa);
    const Tri* pick = *std::min_element(level.begin(), level.end(), [&](const Tri* x, const Tri* y) {
        const float dx = (x->centroid.x - cx) * (x->centroid.x - cx) + (x->centroid.z - cz) * (x->centroid.z - cz);
        const float dy = (y->centroid.x - cx) * (y->centroid.x - cx) + (y->centroid.z - cz) * (y->centroid.z - cz);
        return dx < dy;
    });
    const XMFLOAT3 o = pick->centroid;
    for (PmxVertex& v : out.vertices) v.position = {v.position.x - o.x, v.position.y - o.y, v.position.z - o.z};
    LOG_INFO("stage placed on its main floor: moved by (%.1f, %.1f, %.1f)", -o.x, -o.y, -o.z);
}

bool ConvertStage(const ImpScene& s, PmxModel& out, bool place) {
    Builder b{s, out};
    out.bones.push_back(MakeBone("全ての親", {0, 0, 0}, -1, PmxBone_Movable));
    std::vector<int> boneOfNode(s.nodes.size(), 0);
    std::vector<uint32_t> base;
    BuildTextures(s, out);
    BuildMaterialsAndGeometry(s, b, false, boneOfNode, {}, {}, base);
    if (place) PlaceStage(out);
    return !out.vertices.empty();
}

bool ConvertCharacter(ImpScene& s, PmxModel& out, std::string* error) {
    Humanoid h = MapHumanoid(s);
    if (!h.ok) {
        if (error) *error = "not a humanoid rig: " + h.note;
        return false;
    }
    ToMmdArmPose(s, h);

    Builder b{s, out};
    b.fx = h.facing > 0 ? 1.0f : -1.0f;  // facing +Z: mirror z; facing -Z: turn around (mirror x)
    b.fz = h.facing > 0 ? -1.0f : 1.0f;
    XMFLOAT3 lo, hi;
    BoundsOf(s, lo, hi);
    const float height = hi.y - lo.y;
    // Dancing-doll scale: about 1.4..1.8 m tall in MMD units. Models outside that range (giants,
    // cm-scaled exports, chibis) are brought to its edge; the app's size slider does the rest.
    if (height > 1e-4f) {
        const float metres = (height > 0.5f && height < 3.0f) ? height : 1.7f;  // unit-less files: assume 1.7 m
        const float unit = (height > 0.5f && height < 3.0f) ? 1.0f : 1.7f / height;
        b.scale = kUnitsPerMeter * unit * std::clamp(metres, 1.4f, 1.8f) / metres;
    } else {
        b.scale = kUnitsPerMeter;
    }

    // Skeleton: every skin joint, parented to its nearest joint ancestor.
    const std::vector<char> isJoint = JointSet(s);
    std::map<int, std::string> nameOfNode;
    for (auto& [slot, node] : h.slots) {
        auto it = MmdNames().find(slot);
        if (it != MmdNames().end()) nameOfNode[node] = it->second;
    }
    const int hipsNode = h.Node("hips");
    std::vector<int> torso;
    for (const char* t : {"spine", "chest", "upperChest"})
        if (h.Node(t) >= 0) torso.push_back(h.Node(t));
    static const char* kTorsoNames[] = {"上半身", "上半身2", "上半身3"};
    for (size_t i = 0; i < torso.size(); ++i) nameOfNode[torso[i]] = kTorsoNames[i];
    nameOfNode[hipsNode] = "下半身";

    auto P = [&](int node) { return b.Pos(Translation(s.nodes[node].world)); };
    const XMFLOAT3 hipsPos = P(hipsNode);
    out.bones.push_back(MakeBone("全ての親", {0, 0, 0}, -1, PmxBone_Movable));
    out.bones.push_back(MakeBone("センター", {0, hipsPos.y * 0.65f, 0}, 0, PmxBone_Movable));
    out.bones.push_back(MakeBone("グルーブ", {0, hipsPos.y * 0.65f, 0}, 1, PmxBone_Movable));
    constexpr int kGroove = 2;

    std::vector<std::vector<int>> children(s.nodes.size());
    std::vector<int> roots;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].parent >= 0) children[s.nodes[i].parent].push_back((int)i);
        else roots.push_back((int)i);
    }
    std::vector<int> boneOfNode(s.nodes.size(), 1);  // non-joint nodes: rigidly on センター unless under a joint
    std::set<std::string> usedNames = {"全ての親", "センター", "グルーブ"};
    std::function<void(int, int)> visit = [&](int node, int parentBone) {
        int myBone = parentBone;
        if (isJoint[node]) {
            std::string name = nameOfNode.count(node) ? nameOfNode[node] : s.nodes[node].name;
            for (int k = 2; usedNames.count(name); ++k) name = (nameOfNode.count(node) ? nameOfNode[node] : s.nodes[node].name) + "_" + std::to_string(k);
            usedNames.insert(name);
            int parent = parentBone;
            if (node == hipsNode || (!torso.empty() && node == torso[0])) parent = kGroove;
            myBone = (int)out.bones.size();
            out.bones.push_back(MakeBone(name, P(node), parent));
        }
        boneOfNode[node] = myBone;
        for (int c : children[node]) visit(c, myBone);
    };
    for (int r : roots) visit(r, 1);

    // Leg IK (MMD dances drive the legs almost entirely through 足ＩＫ / つま先ＩＫ).
    for (const char* side : {"left", "right"}) {
        const std::string jp = std::string(side) == "left" ? "左" : "右";
        const int leg = out.FindBone(jp + "足"), knee = out.FindBone(jp + "ひざ"), ankle = out.FindBone(jp + "足首");
        if (leg < 0 || knee < 0 || ankle < 0) continue;
        PmxBone ik = MakeBone(jp + "足ＩＫ", out.bones[ankle].position, 0, PmxBone_Movable | PmxBone_IK);
        ik.ikTargetIndex = ankle;
        ik.ikLoopCount = 40;
        ik.ikLimitAngle = 2.0f;
        PmxIkLink kneeLink;
        kneeLink.boneIndex = knee;
        kneeLink.hasLimit = true;
        kneeLink.limitMin = {-kPi, 0, 0};
        kneeLink.limitMax = {-0.5f * kPi / 180.0f, 0, 0};
        PmxIkLink legLink;
        legLink.boneIndex = leg;
        ik.ikLinks = {kneeLink, legLink};
        const int ikIndex = (int)out.bones.size();
        out.bones.push_back(ik);
        const int toe = out.FindBone(jp + "つま先");
        if (toe >= 0) {
            PmxBone tik = MakeBone(jp + "つま先ＩＫ", out.bones[toe].position, ikIndex, PmxBone_Movable | PmxBone_IK);
            tik.ikTargetIndex = toe;
            tik.ikLoopCount = 3;
            tik.ikLimitAngle = 4.0f;
            PmxIkLink ankleLink;
            ankleLink.boneIndex = ankle;
            tik.ikLinks = {ankleLink};
            out.bones.push_back(tik);
        }
    }

    // Skin joint -> bone.
    std::vector<int> skinJointBone;
    std::vector<size_t> skinBase;
    for (const ImpSkin& sk : s.skins) {
        skinBase.push_back(skinJointBone.size());
        for (int j : sk.joints) skinJointBone.push_back(j >= 0 && (size_t)j < boneOfNode.size() ? boneOfNode[j] : 1);
    }
    std::vector<uint32_t> base;
    BuildTextures(s, out);
    BuildMaterialsAndGeometry(s, b, true, boneOfNode, skinJointBone, skinBase, base);
    BuildMorphs(s, b, base);
    return !out.vertices.empty();
}

bool ImportAny(const std::filesystem::path& path, ImpScene& scene, std::string* error) {
    switch (ModelFormatFromPath(path)) {
    case ModelFormat::Gltf:
    case ModelFormat::Vrm: return ImportGltf(path, scene, error);
    case ModelFormat::Fbx:
    case ModelFormat::Obj: return ImportUfbx(path, scene, error);
    default:
        if (error) *error = "unsupported format";
        return false;
    }
}

} // namespace

void ImpScene::UpdateWorld() {
    std::vector<char> state(nodes.size(), 0);  // 0 todo, 1 in progress, 2 done
    std::function<void(int)> eval = [&](int i) {
        if (state[i] == 2) return;
        if (state[i] == 1) {  // cycle: treat as root
            nodes[i].world = nodes[i].local;
            state[i] = 2;
            return;
        }
        state[i] = 1;
        const int p = nodes[i].parent;
        if (p >= 0 && (size_t)p < nodes.size()) {
            eval(p);
            XMStoreFloat4x4(&nodes[i].world, XMLoadFloat4x4(&nodes[i].local) * XMLoadFloat4x4(&nodes[p].world));
        } else {
            nodes[i].world = nodes[i].local;
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < nodes.size(); ++i) eval((int)i);
}

ModelFormat ModelFormatFromPath(const std::filesystem::path& p) {
    const std::string ext = ToLowerAscii(PathToUtf8(p.extension()));
    if (ext == ".pmx") return ModelFormat::Pmx;
    if (ext == ".gltf" || ext == ".glb") return ModelFormat::Gltf;
    if (ext == ".vrm") return ModelFormat::Vrm;
    if (ext == ".fbx") return ModelFormat::Fbx;
    if (ext == ".obj") return ModelFormat::Obj;
    if (ext == ".pmd") return ModelFormat::Pmd;
    if (ext == ".x") return ModelFormat::X;
    return ModelFormat::Unknown;
}

const char* ModelFormatName(ModelFormat f) {
    switch (f) {
    case ModelFormat::Pmx: return "PMX";
    case ModelFormat::Gltf: return "glTF";
    case ModelFormat::Vrm: return "VRM";
    case ModelFormat::Fbx: return "FBX";
    case ModelFormat::Obj: return "OBJ";
    case ModelFormat::Pmd: return "PMD";
    case ModelFormat::X: return "X";
    default: return "?";
    }
}

bool LoadModelFile(const std::filesystem::path& path, ModelRole role, PmxModel& out, std::string* error) {
    switch (ModelFormatFromPath(path)) {
    case ModelFormat::Pmx: return LoadPmx(path, out, error);
    case ModelFormat::Pmd: return LoadPmd(path, out, error);
    case ModelFormat::X:
        if (!LoadXFile(path, out, error)) return false;
        LOG_INFO("loaded %s: %zu vertices, %zu materials", PathToUtf8(path.filename()).c_str(), out.vertices.size(),
                 out.materials.size());
        return true;
    default: break;
    }
    try {
        out = PmxModel{};
        ImpScene scene;
        if (!ImportAny(path, scene, error)) return false;
        if (!scene.unsupported.empty()) {
            if (error) *error = scene.unsupported;
            return false;
        }
        for (const std::string& w : scene.warnings) LOG_WARN("%s: %s", PathToUtf8(path.filename()).c_str(), w.c_str());
        out.sourcePath = path;
        out.name = !scene.name.empty() ? scene.name : PathToUtf8(path.stem());
        out.comment = std::string("imported from ") + ModelFormatName(ModelFormatFromPath(path));
        const bool ok = role == ModelRole::Character ? ConvertCharacter(scene, out, error)
                                                        : ConvertStage(scene, out, role == ModelRole::Stage);
        if (!ok) {
            if (error && error->empty()) *error = "no geometry";
            return false;
        }
        LOG_INFO("imported %s as %s: %zu vertices, %zu materials, %zu bones, %zu morphs", PathToUtf8(path.filename()).c_str(),
                 role == ModelRole::Character ? "character" : role == ModelRole::Stage ? "stage" : "prop", out.vertices.size(), out.materials.size(),
                 out.bones.size(), out.morphs.size());
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

bool ProbeModelFile(const std::filesystem::path& path, ModelProbe& out, std::string* error) {
    const ModelFormat format = ModelFormatFromPath(path);
    if (format == ModelFormat::Pmx || format == ModelFormat::Pmd || format == ModelFormat::X) {
        // native formats: load the file (the library scanner reads PMX/PMD bone names with ProbePmx/ProbePmd)
        out = ModelProbe{};
        PmxModel m;
        if (!LoadModelFile(path, ModelRole::Prop, m, error)) return false;
        out.name = format == ModelFormat::X ? std::string() : m.name;
        out.vertexCount = (uint32_t)m.vertices.size();
        out.materialCount = (uint32_t)m.materials.size();
        out.boneCount = (uint32_t)m.bones.size();
        out.skinned = format != ModelFormat::X;
        out.morphed = !m.morphs.empty();
        XMFLOAT3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const PmxVertex& v : m.vertices) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
        }
        if (!m.vertices.empty()) {
            out.heightMeters = (hi.y - lo.y) / kUnitsPerMeter;
            out.extentMeters = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) / kUnitsPerMeter;
        }
        out.humanoidNote = format == ModelFormat::X ? "static accessory (.x)" : "MMD model";
        return true;
    }
    try {
        out = ModelProbe{};
        ImpScene scene;
        if (!ImportAny(path, scene, error)) return false;
        out.name = scene.name;
        out.unsupported = scene.unsupported;
        out.materialCount = (uint32_t)scene.materials.size();
        for (const ImpMesh& m : scene.meshes) {
            out.vertexCount += (uint32_t)m.positions.size();
            out.skinned |= m.skin >= 0;
            for (const ImpMorphTarget& t : m.morphs) out.morphed |= !t.vertices.empty();
        }
        const std::vector<char> joints = JointSet(scene);
        out.boneCount = (uint32_t)std::count(joints.begin(), joints.end(), 1);
        XMFLOAT3 lo, hi;
        BoundsOf(scene, lo, hi);
        out.heightMeters = hi.y - lo.y;
        out.extentMeters = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        if (out.skinned) {
            const Humanoid h = MapHumanoid(scene);
            out.humanoid = h.ok;
            out.humanoidNote = h.note;
        } else {
            out.humanoidNote = "no skinned mesh";
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "unexpected exception";
        return false;
    }
}

} // namespace mmdx
