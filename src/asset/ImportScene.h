#pragma once
// Format-neutral scene description shared by the glTF (cgltf) and FBX/OBJ (ufbx) front ends.
// It stays in the source convention: right-handed, +Y up, metres, row-vector matrices
// (v * M, DirectXMath). ImportConvert turns it into a PmxModel in MMD space.
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

struct ImpNode {
    std::string name;
    int parent = -1;
    DirectX::XMFLOAT4X4 local{};  // relative to the parent
    DirectX::XMFLOAT4X4 world{};  // filled by ImpScene::UpdateWorld
};

struct ImpTexture {
    std::filesystem::path file;   // resolved file on disk (empty when embedded or missing)
    std::vector<uint8_t> bytes;   // embedded encoded image (PNG/JPG/WebP...)
    std::string label;            // name for logs
};

struct ImpMaterial {
    std::string name;
    DirectX::XMFLOAT4 baseColor{1, 1, 1, 1};
    DirectX::XMFLOAT3 emissive{0, 0, 0};
    int texture = -1;             // into ImpScene::textures
    bool doubleSided = false;
    bool blend = false;           // alpha blend or mask: keep texture alpha
};

struct ImpMorphTarget {
    std::string name;
    std::vector<uint32_t> vertices;              // mesh-local vertex indices
    std::vector<DirectX::XMFLOAT3> offsets;      // position deltas in mesh space
};

// One drawable: a triangle list with a single material, instanced at `node`.
struct ImpMesh {
    std::string name;
    int node = -1;
    int skin = -1;
    int material = -1;
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;      // may be empty (generated later)
    std::vector<DirectX::XMFLOAT2> uvs;          // may be empty
    std::vector<std::array<int, 4>> joints;      // indices into ImpSkin::joints, -1 unused
    std::vector<DirectX::XMFLOAT4> weights;
    std::vector<uint32_t> indices;
    std::vector<ImpMorphTarget> morphs;
    std::string morphGroup;                      // key shared by primitives of the same source mesh
};

struct ImpSkin {
    std::vector<int> joints;                          // node indices
    std::vector<DirectX::XMFLOAT4X4> inverseBind;     // mesh space -> joint space at bind
};

// A named expression assembled from morph targets (VRM presets).
struct ImpExpression {
    std::string preset;           // normalised VRM preset: aa ih ou ee oh blink blinkLeft blinkRight happy ...
    struct Bind { std::string morphGroup; int target = 0; float weight = 1; };
    std::vector<Bind> binds;
};

struct ImpScene {
    std::string name;             // model / scene title (may be empty)
    std::vector<ImpNode> nodes;
    std::vector<ImpMesh> meshes;
    std::vector<ImpSkin> skins;
    std::vector<ImpMaterial> materials;
    std::vector<ImpTexture> textures;
    std::map<std::string, int> humanBones;   // VRM humanoid slot (VRM 1.0 names) -> node
    std::vector<ImpExpression> expressions;
    std::vector<std::string> warnings;
    std::string unsupported;      // non-empty: the file cannot be shown (e.g. Draco compression)

    void UpdateWorld();           // any node order
};

// Front ends. Return false only when the file cannot be parsed; `scene.unsupported` reports
// files that parse but cannot be shown.
bool ImportGltf(const std::filesystem::path& path, ImpScene& scene, std::string* error);
bool ImportUfbx(const std::filesystem::path& path, ImpScene& scene, std::string* error);

} // namespace mmdx
