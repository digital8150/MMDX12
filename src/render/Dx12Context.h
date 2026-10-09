#pragma once
// D3D12 device, queue, swap chain, frame pacing, descriptor heaps and resource upload.
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

using Microsoft::WRL::ComPtr;
struct ImageRGBA8;

// Fixed-capacity descriptor heap with a first-fit free-list allocator of contiguous ranges.
class DescriptorHeap {
public:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible);
    uint32_t Allocate(uint32_t count = 1);          // kInvalid when full
    void Free(uint32_t index, uint32_t count = 1);  // merges adjacent free ranges
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const;  // only for shader-visible heaps
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }
    uint32_t Capacity() const { return capacity_; }
    uint32_t Used() const { return used_; }
    // Inverse of Cpu(): index of a CPU handle inside this heap.
    uint32_t IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE h) const;

private:
    struct Range { uint32_t start, count; };
    ComPtr<ID3D12DescriptorHeap> heap_;
    std::vector<Range> free_;  // sorted by start
    uint32_t capacity_ = 0, used_ = 0, increment_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart_{};
};

struct DeviceCaps {
    std::string adapterName;           // UTF-8, from DXGI_ADAPTER_DESC1::Description
    uint64_t dedicatedVideoMemory = 0; // bytes
    uint32_t vendorId = 0;             // PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
    D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    D3D12_VARIABLE_SHADING_RATE_TIER vrsTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    bool tearingSupported = false;
};

class Dx12Context {
public:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kBackBufferCount = 3;
    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    Dx12Context() = default;
    ~Dx12Context();
    Dx12Context(const Dx12Context&) = delete;
    Dx12Context& operator=(const Dx12Context&) = delete;

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, bool enableDebugLayer);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);  // no-op for 0 or unchanged sizes

    // Waits until the GPU is done with this frame slot, resets the slot's allocator and the
    // command list, transitions the current back buffer PRESENT -> RENDER_TARGET and returns
    // the open command list. Also releases deferred resources whose frame has completed.
    ID3D12GraphicsCommandList* BeginFrame();
    // Handles a pending capture, transitions back buffer -> PRESENT, closes, executes,
    // presents (vsync ? 1 : 0, ALLOW_TEARING when !vsync and supported), signals the fence.
    void EndFrame(bool vsync);
    void WaitForGpu();

    // Writes the back buffer content of the next EndFrame (including UI) to a PNG file.
    void RequestCapture(const std::filesystem::path& pngPath);
    bool CapturePending() const { return !capturePath_.empty() || captureInFlight_; }

    // Keeps `res` alive until the GPU has finished the frame currently being recorded.
    void DeferRelease(ComPtr<ID3D12Resource> res);

    ID3D12Device* Device() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    ID3D12GraphicsCommandList* CommandList() const { return cmdList_.Get(); }
    uint32_t FrameSlot() const { return frameSlot_; }       // 0..kFramesInFlight-1
    uint64_t FrameNumber() const { return frameNumber_; }   // increments every BeginFrame
    ID3D12Resource* BackBuffer() const { return backBuffers_[backBufferIndex_].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv() const { return rtvHeap_.Cpu(backBufferRtv_[backBufferIndex_]); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    const DeviceCaps& Caps() const { return caps_; }

    DescriptorHeap& SrvHeap() { return srvHeap_; }  // shader-visible CBV/SRV/UAV, capacity 16384
    DescriptorHeap& RtvHeap() { return rtvHeap_; }  // capacity 64
    DescriptorHeap& DsvHeap() { return dsvHeap_; }  // capacity 192 (16 point lights x 6 shadow faces fit)

private:
    void CreateBackBuffers();
    void ReleaseBackBuffers();
    void ProcessCapture();

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    uint64_t slotFence_[kFramesInFlight] = {};
    ComPtr<ID3D12Resource> backBuffers_[kBackBufferCount];
    uint32_t backBufferRtv_[kBackBufferCount] = {};
    uint32_t backBufferIndex_ = 0;
    uint32_t frameSlot_ = 0;
    uint64_t frameNumber_ = 0;
    uint32_t width_ = 0, height_ = 0;
    HWND hwnd_ = nullptr;
    DeviceCaps caps_;
    DescriptorHeap srvHeap_, rtvHeap_, dsvHeap_;
    struct Deferred { uint64_t fence; ComPtr<ID3D12Resource> res; };
    std::vector<Deferred> deferred_;
    // capture
    std::filesystem::path capturePath_, captureInFlightPath_;
    bool captureInFlight_ = false;
    uint64_t captureFence_ = 0;
    ComPtr<ID3D12Resource> captureReadback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint_{};
};

// Batches resource creation + initial data upload into one command list and one GPU wait.
// Usage: UploadBatch b(ctx); auto vb = b.CreateBuffer(...); ...; b.Submit();
class UploadBatch {
public:
    explicit UploadBatch(Dx12Context& ctx);
    ~UploadBatch();  // calls Submit() if not yet submitted

    // DEFAULT-heap buffer initialised with `data` (may be null => zero-filled not required).
    ComPtr<ID3D12Resource> CreateBuffer(const void* data, size_t bytes, D3D12_RESOURCE_STATES finalState,
                                        const wchar_t* debugName = nullptr);
    // DEFAULT-heap R8G8B8A8_UNORM texture with all mips of `image`; final state
    // PIXEL_SHADER_RESOURCE. `image` must not be empty.
    ComPtr<ID3D12Resource> CreateTexture(const ImageRGBA8& image, const wchar_t* debugName = nullptr);
    // Same with another resource format of the same layout (R8G8B8A8_TYPELESS: the caller creates
    // sRGB and UNORM SRVs on one texture).
    ComPtr<ID3D12Resource> CreateTextureTyped(const ImageRGBA8& image, DXGI_FORMAT format,
                                              const wchar_t* debugName = nullptr);
    // Closes, executes on ctx.Queue(), waits for completion, frees staging buffers.
    void Submit();

private:
    Dx12Context& ctx_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    std::vector<ComPtr<ID3D12Resource>> staging_;
    bool submitted_ = false;
};

// Throws nothing; logs with LOG_ERROR and returns false. Helper for HRESULT checks.
bool CheckHr(HRESULT hr, const char* what);

} // namespace mmdx
