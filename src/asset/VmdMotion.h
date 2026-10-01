#pragma once
// VMD motion / camera file. Values are stored exactly as in the file (MMD native space).
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

struct VmdBoneKey {
    std::string boneName;     // UTF-8 (converted from Shift-JIS)
    uint32_t frame = 0;
    XMFLOAT3 translation{};
    XMFLOAT4 rotation{0, 0, 0, 1};  // quaternion xyzw
    // Raw 64-byte interpolation block. Channel c (0=X,1=Y,2=Z,3=Rotation) bezier control
    // points: x1 = interp[c], y1 = interp[c+4], x2 = interp[c+8], y2 = interp[c+12]  (0..127)
    uint8_t interp[64] = {};
};

struct VmdMorphKey {
    std::string morphName;
    uint32_t frame = 0;
    float weight = 0;
};

struct VmdCameraKey {
    uint32_t frame = 0;
    float distance = 0;      // usually negative (e.g. -45)
    XMFLOAT3 target{};       // look-at point
    XMFLOAT3 rotation{};     // radians (x pitch, y yaw, z roll)
    // Raw 24-byte block. Parameter p (0=X,1=Y,2=Z,3=Rotation,4=Distance,5=FOV):
    // x1 = interp[p*4+0], x2 = interp[p*4+1], y1 = interp[p*4+2], y2 = interp[p*4+3]
    uint8_t interp[24] = {};
    uint32_t fovDeg = 30;
    bool perspective = true;  // file stores 0 = perspective ON
};

struct VmdLightKey {
    uint32_t frame = 0;
    XMFLOAT3 color{};
    XMFLOAT3 direction{};
};

struct VmdIkKey {
    uint32_t frame = 0;
    bool visible = true;
    std::vector<std::pair<std::string, bool>> ikStates;  // (IK bone name, enabled)
};

struct VmdMotion {
    std::filesystem::path sourcePath;
    std::string modelName;  // "カメラ・照明" for camera files
    std::vector<VmdBoneKey> boneKeys;
    std::vector<VmdMorphKey> morphKeys;
    std::vector<VmdCameraKey> cameraKeys;
    std::vector<VmdLightKey> lightKeys;
    std::vector<VmdIkKey> ikKeys;
    uint32_t maxFrame = 0;  // largest frame number over all key types
};

bool LoadVmd(const std::filesystem::path& path, VmdMotion& out, std::string* error = nullptr);

struct VmdProbe {
    std::string modelName;
    uint32_t boneKeyCount = 0, morphKeyCount = 0, cameraKeyCount = 0;
    uint32_t maxFrame = 0;  // over bone, morph and camera keys
};
bool ProbeVmd(const std::filesystem::path& path, VmdProbe& out, std::string* error = nullptr);

} // namespace mmdx
