#pragma once
// DXR acceleration structures for the frame's models, consumed by inline ray queries
// (RayQuery, raytracing tier 1.1, shader model 6.5). See shaders/rt_common.hlsli.
//
// Per frame (Build):
//   1. skin.hlsl skins every model into its RtVertex buffer (GpuModel::Rt().vertices):
//      current and previous pose, world space. Stage models are skinned once.
//   2. Up to two BLASes per model (single-sided / double-sided materials), one geometry per
//      drawable material (indexCount > 0 and diffuse.a > 0.001). Characters: full rebuild
//      every frame (PREFER_FAST_BUILD). Stages: built once (PREFER_FAST_TRACE).
//   3. TLAS over all BLASes (identity transforms; vertices are already in world space), rebuilt
//      every frame, plus the RtGeometry table for this frame slot. Double-sided instances set
//      TRIANGLE_CULL_DISABLE, so RAY_FLAG_CULL_BACK_FACING_TRIANGLES culls like the raster
//      pass. Instance masks: 0x01 stage, 0x02 character.
#include "render/Dx12Context.h"
#include <filesystem>
#include <vector>

namespace mmdx {

class GpuModel;

class RtScene {
public:
    RtScene() = default;
    ~RtScene();
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    // False (logged) when the device lacks raytracing tier 1.1 / shader model 6.5 /
    // ID3D12Device5, or skin.hlsl does not compile.
    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir);
    void Shutdown();  // the GPU must be idle

    // Records skinning, BLAS and TLAS builds into `cmd` (an ID3D12GraphicsCommandList4 via QI).
    // `frame` selects the bone/morph ring entry (GpuModel::BoneBuffer(frame)), `slot` the
    // upload ring entry for instance descs and the geometry table (0..kSlots-1). Ends with the
    // RtVertex buffers in NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE and a UAV barrier
    // on the TLAS. Returns Ready(). `time` < 1 skins characters inside the previous -> current
    // interval (offline motion blur); stage models keep their first build.
    static constexpr uint32_t kSlots = Dx12Context::kFramesInFlight + 1;
    bool Build(ID3D12GraphicsCommandList* cmd, const std::vector<GpuModel*>& models, uint64_t frame, uint32_t slot,
               float time = 1.0f);

    bool Ready() const { return ready_; }
    D3D12_GPU_VIRTUAL_ADDRESS Tlas() const;        // RaytracingAccelerationStructure (t0 space1)
    D3D12_GPU_VIRTUAL_ADDRESS Geometries() const;  // StructuredBuffer<RtGeometry> of the last Build (t1 space1)

private:
    bool EnsureModelResources(GpuModel& model);

    Dx12Context* ctx_ = nullptr;
    ComPtr<ID3D12Device5> device5_;
    ComPtr<ID3D12RootSignature> skinRootSig_;
    ComPtr<ID3D12PipelineState> skinPso_;
    // TLAS (grown on demand; old buffers go through ctx.DeferRelease)
    ComPtr<ID3D12Resource> tlas_, tlasScratch_;
    uint64_t tlasBytes_ = 0, tlasScratchBytes_ = 0;
    uint32_t tlasCapacity_ = 0;  // instances the TLAS buffers were sized for
    // Upload rings (persistently mapped), kSlots entries each, grown on demand
    ComPtr<ID3D12Resource> instances_, geometries_;
    uint8_t* instancesMapped_ = nullptr;
    uint8_t* geometriesMapped_ = nullptr;
    uint32_t instanceCapacity_ = 0, geometryCapacity_ = 0;  // per slot
    // PT pack records upload ring (persistently mapped), kSlots entries each, grown on demand
    ComPtr<ID3D12Resource> packRecords_;
    uint8_t* packRecordsMapped_ = nullptr;
    uint32_t packRecordCapacity_ = 0;                       // per slot
    uint32_t packSrvBase_ = DescriptorHeap::kInvalid;       // kSlots * packRecordCapacity_ descriptors
    uint32_t lastSlot_ = 0;
    bool ready_ = false;
};

} // namespace mmdx
