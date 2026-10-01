#pragma once
// VMD evaluation: bone/morph motion bound to a specific model, and camera motion.
// Time unit is the MMD frame (30 per second), fractional values allowed.
#include "asset/VmdMotion.h"
#include <DirectXMath.h>
#include <memory>
#include <vector>

namespace mmdx {

struct PmxModel;
class ModelInstance;

constexpr float kMmdFps = 30.0f;

struct Bezier {
    float x1 = 20.f / 127.f, y1 = 20.f / 127.f, x2 = 107.f / 127.f, y2 = 107.f / 127.f;
    bool linear = true;
    static Bezier FromBytes(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
    float Evaluate(float t) const;  // t in [0,1] -> eased [0,1]
};

class BoundMotion {
public:
    // Merges all layers (e.g. dance + separate facial vmd) and binds names to `model`.
    static std::shared_ptr<BoundMotion> Bind(const PmxModel& model, const std::vector<const VmdMotion*>& layers);

    // Writes bone anim, morph weights and IK enable states for `frame` into `inst`
    // (calls ResetPose first). Does not call UpdatePose.
    void Evaluate(float frame, ModelInstance& inst) const;

    float EndFrame() const { return endFrame_; }
    int BoundBoneCount() const { return (int)boneTracks_.size(); }
    int BoundMorphCount() const { return (int)morphTracks_.size(); }

private:
    struct BoneKey { float frame; DirectX::XMFLOAT3 t; DirectX::XMFLOAT4 r; Bezier bx, by, bz, br; };
    struct MorphKey { float frame; float weight; };
    struct IkKey { float frame; bool enabled; };
    struct BoneTrack { int bone; std::vector<BoneKey> keys; };
    struct MorphTrack { int morph; std::vector<MorphKey> keys; };
    struct IkTrack { int bone; std::vector<IkKey> keys; };
    std::vector<BoneTrack> boneTracks_;
    std::vector<MorphTrack> morphTracks_;
    std::vector<IkTrack> ikTracks_;
    float endFrame_ = 0;
};

struct CameraPose {
    DirectX::XMFLOAT3 target{0, 10, 0};
    DirectX::XMFLOAT3 rotation{};  // radians, VMD convention
    float distance = -45.0f;
    float fovDeg = 30.0f;
};

class CameraMotion {
public:
    static std::shared_ptr<CameraMotion> Create(const VmdMotion& vmd);  // null if no camera keys
    CameraPose Evaluate(float frame) const;
    float EndFrame() const { return endFrame_; }

    // MMD camera -> view matrix (LH) and eye position.
    static void ToView(const CameraPose& pose, DirectX::XMFLOAT4X4* view, DirectX::XMFLOAT3* eye);

private:
    struct Key { float frame; CameraPose pose; Bezier bx, by, bz, br, bd, bf; };
    std::vector<Key> keys_;
    float endFrame_ = 0;
};

} // namespace mmdx
