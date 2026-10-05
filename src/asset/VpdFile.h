#pragma once
#include <DirectXMath.h>
#include <filesystem>
#include <string>
#include <vector>
namespace mmdx {
struct VpdBone { std::string name; DirectX::XMFLOAT3 translation{}; DirectX::XMFLOAT4 rotation{0, 0, 0, 1}; };
struct VpdMorph { std::string name; float weight = 0; };
struct VpdPose {
    std::string modelName;   // UTF-8, the "parent file" line without the trailing ".osm"
    std::vector<VpdBone> bones;
    std::vector<VpdMorph> morphs;
};
bool LoadVpd(const std::filesystem::path& path, VpdPose& out, std::string* error = nullptr);
bool SaveVpd(const std::filesystem::path& path, const VpdPose& pose, std::string* error = nullptr);
}
