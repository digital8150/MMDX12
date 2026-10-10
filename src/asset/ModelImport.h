#pragma once
// Loading of every supported model format into the engine's PmxModel.
// PMX is read natively; glTF/GLB/VRM (cgltf) and FBX/OBJ (ufbx) are converted on load:
// - Stage role: all meshes baked at their node transforms, one root bone, metres * 12.5.
// - Character role: the humanoid skeleton is renamed to the MMD standard bones (VRM humanoid
//   table or name dictionaries for Mixamo/VRoid/Unity/Blender/UE/Biped rigs), センター/グルーブ
//   and leg IK are synthesised, the rest pose is turned into MMD's A-pose, and lip/blink
//   morphs get MMD aliases, so ordinary VMD dances drive it. No physics.
#include "asset/PmxModel.h"

#include <filesystem>
#include <string>

namespace mmdx {

enum class ModelFormat { Unknown, Pmx, Gltf, Vrm, Fbx, Obj, Pmd, X };

ModelFormat ModelFormatFromPath(const std::filesystem::path& p);
const char* ModelFormatName(ModelFormat f);  // "PMX", "glTF", "VRM", "FBX", "OBJ", "PMD", "X"
// MMD's own model formats (PMX, PMD): full rigs read natively, classified by their bone names.
inline bool IsMmdModelFormat(ModelFormat f) { return f == ModelFormat::Pmx || f == ModelFormat::Pmd; }
inline bool IsModelFile(const std::filesystem::path& p) { return ModelFormatFromPath(p) != ModelFormat::Unknown; }

// Prop: baked like a stage but kept where the file puts it (no move onto a floor): accessories in the Studio.
enum class ModelRole { Character, Stage, Prop };

// Full load. PMX and PMD are read natively; as a Character they also get their T-pose arms turned to the A-pose
// (asset/RestPose.h), otherwise the role is ignored. DirectX .x (MMD accessories) loads as a static
// model with one root bone, placed where the file puts it, whatever the role.
bool LoadModelFile(const std::filesystem::path& path, ModelRole role, PmxModel& out, std::string* error = nullptr);

// What the library scanner needs to classify a non-PMX file. Loads the file (geometry
// included, textures not decoded).
struct ModelProbe {
    std::string name;             // title from the file (VRM meta, scene name) or empty
    uint32_t vertexCount = 0, materialCount = 0, boneCount = 0;
    bool skinned = false;
    bool morphed = false;         // has non-empty morph targets
    bool humanoid = false;        // the humanoid mapping succeeded and passed the shape checks
    std::string humanoidNote;     // why the mapping failed (for logs)
    float heightMeters = 0;       // bounding box of the rest pose
    float extentMeters = 0;       // largest bounding box side
    std::string unsupported;      // non-empty: cannot be displayed (reason)
};
bool ProbeModelFile(const std::filesystem::path& path, ModelProbe& out, std::string* error = nullptr);

} // namespace mmdx
