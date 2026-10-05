#pragma once
// PMX 2.0 / 2.1 model data. All coordinates are kept in native MMD space
// (left-handed, +Y up, model faces -Z), which matches Direct3D conventions, so no
// axis flipping is ever done anywhere in the engine.
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

enum class PmxDeform : uint8_t { BDEF1 = 0, BDEF2 = 1, BDEF4 = 2, SDEF = 3, QDEF = 4 };

struct PmxVertex {
    XMFLOAT3 position{};
    XMFLOAT3 normal{};
    XMFLOAT2 uv{};
    PmxDeform deform = PmxDeform::BDEF1;
    // Unused slots: index -1, weight 0. For BDEF2/SDEF: weight[1] = 1 - weight[0].
    int32_t boneIndex[4] = {-1, -1, -1, -1};
    float boneWeight[4] = {0, 0, 0, 0};
    XMFLOAT3 sdefC{}, sdefR0{}, sdefR1{};  // only meaningful for SDEF
    float edgeScale = 1.0f;
};

enum PmxMaterialFlag : uint8_t {
    PmxMat_DoubleSided  = 0x01,
    PmxMat_GroundShadow = 0x02,
    PmxMat_CastShadow   = 0x04,  // draws into self-shadow map
    PmxMat_ReceiveShadow= 0x08,
    PmxMat_Edge         = 0x10,
    PmxMat_VertexColor  = 0x20,  // 2.1
    PmxMat_PointDraw    = 0x40,  // 2.1
    PmxMat_LineDraw     = 0x80,  // 2.1
};

enum class PmxSphereMode : uint8_t { None = 0, Multiply = 1, Add = 2, SubTexture = 3 };

struct PmxMaterial {
    std::string name, nameEn;
    XMFLOAT4 diffuse{1, 1, 1, 1};
    XMFLOAT3 specular{};
    float specularPower = 0;
    XMFLOAT3 ambient{};
    uint8_t flags = 0;  // PmxMaterialFlag bits
    XMFLOAT4 edgeColor{0, 0, 0, 1};
    float edgeSize = 1;
    int32_t textureIndex = -1;        // into PmxModel::textures, -1 = none
    int32_t sphereTextureIndex = -1;  // into PmxModel::textures, -1 = none
    PmxSphereMode sphereMode = PmxSphereMode::None;
    bool sharedToon = false;          // true: toonIndex is 0..9 => built-in toon01..toon10
    int32_t toonIndex = -1;           // sharedToon ? 0..9 : texture index (-1 = none)
    std::string memo;
    uint32_t indexCount = 0;          // number of indices (multiple of 3) this material draws
};

enum PmxBoneFlag : uint16_t {
    PmxBone_TailIsBone      = 0x0001,
    PmxBone_Rotatable       = 0x0002,
    PmxBone_Movable         = 0x0004,
    PmxBone_Visible         = 0x0008,
    PmxBone_Operable        = 0x0010,
    PmxBone_IK              = 0x0020,
    PmxBone_AppendLocal     = 0x0080,
    PmxBone_AppendRotate    = 0x0100,
    PmxBone_AppendTranslate = 0x0200,
    PmxBone_FixedAxis       = 0x0400,
    PmxBone_LocalAxis       = 0x0800,
    PmxBone_AfterPhysics    = 0x1000,
    PmxBone_ExternalParent  = 0x2000,
};

struct PmxIkLink {
    int32_t boneIndex = -1;
    bool hasLimit = false;
    XMFLOAT3 limitMin{}, limitMax{};  // radians, Euler X/Y/Z
};

struct PmxBone {
    std::string name, nameEn;
    XMFLOAT3 position{};          // model-space rest position
    int32_t parentIndex = -1;
    int32_t deformLayer = 0;
    uint16_t flags = 0;           // PmxBoneFlag bits
    XMFLOAT3 tailOffset{};        // valid if !(flags & TailIsBone)
    int32_t tailBoneIndex = -1;   // valid if  (flags & TailIsBone)
    int32_t appendParentIndex = -1;  // valid if AppendRotate|AppendTranslate
    float appendRatio = 0;
    XMFLOAT3 fixedAxis{};
    XMFLOAT3 localAxisX{1, 0, 0}, localAxisZ{0, 0, 1};
    int32_t externalKey = 0;
    // IK (valid if flags & IK)
    int32_t ikTargetIndex = -1;
    int32_t ikLoopCount = 0;
    float ikLimitAngle = 0;       // radians, per-iteration limit
    std::vector<PmxIkLink> ikLinks;
};

enum class PmxMorphType : uint8_t {
    Group = 0, Vertex = 1, Bone = 2, UV = 3, UV1 = 4, UV2 = 5, UV3 = 6, UV4 = 7,
    Material = 8, Flip = 9, Impulse = 10
};

struct PmxMorph {
    struct VertexOffset { int32_t vertex; XMFLOAT3 offset; };
    struct GroupOffset { int32_t morph; float weight; };
    struct BoneOffset { int32_t bone; XMFLOAT3 translation; XMFLOAT4 rotation; };  // rotation = quaternion xyzw
    struct MaterialOffset {
        int32_t material;   // -1 = all materials
        uint8_t operation;  // 0 = multiply, 1 = add
        XMFLOAT4 diffuse; XMFLOAT3 specular; float specularPower; XMFLOAT3 ambient;
        XMFLOAT4 edgeColor; float edgeSize;
        XMFLOAT4 textureFactor, sphereFactor, toonFactor;
    };
    std::string name, nameEn;
    uint8_t panel = 0;  // 1 brow, 2 eye, 3 mouth, 4 other
    PmxMorphType type = PmxMorphType::Vertex;
    std::vector<VertexOffset> vertexOffsets;      // type Vertex
    std::vector<GroupOffset> groupOffsets;        // type Group and Flip
    std::vector<BoneOffset> boneOffsets;          // type Bone
    std::vector<MaterialOffset> materialOffsets;  // type Material
    // UV and Impulse morph data are parsed but discarded.
};

struct PmxRigidBody {
    std::string name, nameEn;
    int32_t boneIndex = -1;
    uint8_t group = 0;
    uint16_t collisionMask = 0;  // "non-collision group" bitmask as stored in file
    uint8_t shape = 0;           // 0 sphere, 1 box, 2 capsule
    XMFLOAT3 size{}, position{}, rotation{};
    float mass = 0, linearDamping = 0, angularDamping = 0, restitution = 0, friction = 0;
    uint8_t physicsMode = 0;     // 0 follow bone (static), 1 physics, 2 physics + bone position
};

struct PmxJoint {
    std::string name, nameEn;
    uint8_t type = 0;  // 0 = spring 6DOF
    int32_t rigidBodyA = -1, rigidBodyB = -1;
    XMFLOAT3 position{}, rotation{};
    XMFLOAT3 linearMin{}, linearMax{}, angularMin{}, angularMax{};
    XMFLOAT3 springLinear{}, springAngular{};
};

// Display frame (表示枠): MMD's grouping of bones and morphs in the timeline.
struct PmxDisplayFrame {
    std::string name, nameEn;
    bool special = false;  // "Root" / "表情" frames
    struct Item { bool morph = false; int32_t index = -1; };
    std::vector<Item> items;
};

struct PmxModel {
    std::filesystem::path sourcePath;  // absolute path of the loaded .pmx
    float version = 2.0f;
    std::string name, nameEn, comment, commentEn;
    std::vector<PmxVertex> vertices;
    std::vector<uint32_t> indices;     // triangle list, already widened to 32-bit
    // Texture paths exactly as stored in the file (UTF-8, may contain '\\').
    std::vector<std::string> textures;
    // Encoded image bytes for textures that live inside the source file (glTF/GLB/VRM/FBX
    // imports). Empty, or parallel to `textures` with empty entries for file textures.
    std::vector<std::vector<uint8_t>> embeddedTextures;
    std::vector<PmxMaterial> materials;
    std::vector<PmxBone> bones;
    std::vector<PmxMorph> morphs;
    std::vector<PmxRigidBody> rigidBodies;
    std::vector<PmxJoint> joints;
    std::vector<PmxDisplayFrame> displayFrames;  // empty for imported (non-PMX) models

    // Resolves textures[index] against the model directory. Handles '\\' separators,
    // tolerates a leading ".\\", and when the exact file does not exist tries, in the same
    // directory, a case-insensitive filename match, then the same stem with extensions
    // .png .bmp .jpg .jpeg .tga .dds .spa .sph. Returns empty path if nothing found.
    std::filesystem::path ResolveTexturePath(int32_t index) const;

    int32_t FindBone(std::string_view name) const;   // -1 if absent
    int32_t FindMorph(std::string_view name) const;  // -1 if absent
};

// Full load. Returns false and fills *error (if non-null) on failure; never throws.
bool LoadPmx(const std::filesystem::path& path, PmxModel& out, std::string* error = nullptr);

// Cheap probe used by the library scanner: reads header + names + bone names only.
// (Vertices etc. must still be walked to reach the bone section, but are not stored.)
struct PmxProbe {
    std::string name, nameEn, comment;
    uint32_t vertexCount = 0, faceCount = 0, textureCount = 0, materialCount = 0, boneCount = 0;
    std::vector<std::string> boneNames;
};
bool ProbePmx(const std::filesystem::path& path, PmxProbe& out, std::string* error = nullptr);

} // namespace mmdx
