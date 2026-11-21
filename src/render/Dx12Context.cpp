// D3D12 device / swap chain / frame pacing, descriptor heaps and resource upload.
#include "render/Dx12Context.h"
#include "asset/DdsLoader.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>

namespace mmdx {

// Throws nothing; logs with LOG_ERROR and returns false.
bool CheckHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        LOG_ERROR("%s failed: 0x%08X", what, (unsigned)hr);
        return false;
    }
    return true;
}

// ---- DescriptorHeap ---------------------------------------------------------

DescriptorHeap& DescriptorHeap::operator=(DescriptorHeap&& other) noexcept {
    if (this != &other) {
        heap_ = std::move(other.heap_);
        free_ = std::move(other.free_);
        capacity_ = other.capacity_;
        used_ = other.used_;
        increment_ = other.increment_;
        cpuStart_ = other.cpuStart_;
        gpuStart_ = other.gpuStart_;
    }
    return *this;
}

bool DescriptorHeap::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible) {
    if (!device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    if (!CheckHr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)), "CreateDescriptorHeap")) return false;
    increment_ = device->GetDescriptorHandleIncrementSize(type);
    cpuStart_ = heap_->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) gpuStart_ = heap_->GetGPUDescriptorHandleForHeapStart();
    capacity_ = capacity;
    free_ = {{0, capacity}};
    return true;
}

uint32_t DescriptorHeap::Allocate(uint32_t count) {
    if (count == 0 || count > capacity_) return kInvalid;
    std::lock_guard<std::mutex> lock(mutex_);
    // First range with count <= range.count -> take from its front.
    for (size_t i = 0; i < free_.size(); ++i) {
        if (count <= free_[i].count) {
            uint32_t start = free_[i].start;
            free_[i].start += count;
            free_[i].count -= count;
            if (free_[i].count == 0) free_.erase(free_.begin() + i);
            used_ += count;
            return start;
        }
    }
    return kInvalid;
}

void DescriptorHeap::Free(uint32_t index, uint32_t count) {
    if (index == kInvalid || count == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    used_ = used_ >= count ? used_ - count : 0;
    Range r{index, count};
    auto it = std::lower_bound(free_.begin(), free_.end(), r,
                               [](const Range& a, const Range& b) { return a.start < b.start; });
    it = free_.insert(it, r);
    // Merge with the next neighbour.
    if (it + 1 != free_.end() && it->start + it->count == (it + 1)->start) {
        it->count += (it + 1)->count;
        free_.erase(it + 1);
    }
    // Merge with the previous neighbour.
    if (it != free_.begin() && (it - 1)->start + (it - 1)->count == it->start) {
        (it - 1)->count += it->count;
        free_.erase(it);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Cpu(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuStart_;
    h.ptr += (SIZE_T)increment_ * index;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::Gpu(uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = gpuStart_;
    h.ptr += (UINT64)increment_ * index;
    return h;
}

uint32_t DescriptorHeap::IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE h) const {
    if (increment_ == 0) return kInvalid;
    return (uint32_t)((h.ptr - cpuStart_.ptr) / increment_);
}

// ---- Dx12Context ------------------------------------------------------------

void Dx12Context::CreateBackBuffers() {
    for (uint32_t i = 0; i < kBackBufferCount; ++i) {
        if (!CheckHr(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])), "GetBuffer")) continue;
        backBufferRtv_[i] = rtvHeap_.Allocate(1);
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtvHeap_.Cpu(backBufferRtv_[i]));
    }
}

void Dx12Context::ReleaseBackBuffers() {
    for (uint32_t i = 0; i < kBackBufferCount; ++i) {
        if (backBuffers_[i]) {
            rtvHeap_.Free(backBufferRtv_[i], 1);
            backBufferRtv_[i] = DescriptorHeap::kInvalid;
            backBuffers_[i].Reset();
        }
    }
}

bool Dx12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height, bool enableDebugLayer) {
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> dbg;
        if (CheckHr(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)), "D3D12GetDebugInterface"))
            dbg->EnableDebugLayer();
        // MMDX12_GBV=1 (with --debug): GPU-based validation (descriptor / resource-state errors at execution time; slow)
        char gbv[8] = {};
        ComPtr<ID3D12Debug1> dbg1;
        if (GetEnvironmentVariableA("MMDX12_GBV", gbv, sizeof(gbv)) && gbv[0] == '1' && dbg && SUCCEEDED(dbg.As(&dbg1))) {
            dbg1->SetEnableGPUBasedValidation(TRUE);
            LOG_INFO("D3D12 GPU-based validation on");
        }
    }

    UINT factoryFlags = enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (!CheckHr(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2")) return false;

    // DRED settings must be applied before the device is created. Auto-breadcrumbs + page-fault reporting give the
    // command-list breadcrumb history and the allocations around a page fault after a device removal. Works without
    // the debug layer; a missing interface is silently ignored.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    // Adapter: first hardware adapter that creates a D3D12 device.
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (FAILED(factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&a))))
            break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // MMDX12_ADAPTER=<substring>: the first adapter whose name contains it (case-insensitive), e.g. "AMD" to test on
        // a hybrid laptop's integrated GPU instead of the high-performance one
        {
            wchar_t want[128] = {};
            if (GetEnvironmentVariableW(L"MMDX12_ADAPTER", want, 128) && want[0]) {
                std::wstring name = d.Description, needle = want;
                for (auto& c : name) c = (wchar_t)towlower(c);
                for (auto& c : needle) c = (wchar_t)towlower(c);
                if (name.find(needle) == std::wstring::npos) continue;
            }
        }
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            adapter_ = std::move(a);
            device_ = std::move(dev);
            break;
        }
    }
    if (!device_) {
        LOG_ERROR("no Direct3D 12 hardware adapter found");
        return false;
    }
    // --debug: the debug layer's messages go to the log (OutputDebugString is invisible without a debugger)
    if (enableDebugLayer) {
        ComPtr<ID3D12InfoQueue1> iq;
        if (SUCCEEDED(device_.As(&iq))) {
            DWORD cookie = 0;
            iq->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                    if (sev <= D3D12_MESSAGE_SEVERITY_ERROR) LOG_ERROR("D3D12 [%d] %s", (int)id, text);
                    else if (sev == D3D12_MESSAGE_SEVERITY_WARNING) LOG_WARN("D3D12 [%d] %s", (int)id, text);
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
    }

    // Caps.
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
        caps_.adapterName = WideToUtf8(desc.Description);
        caps_.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        caps_.vendorId = desc.VendorId;
    }
    {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
        fl.NumFeatureLevels = (UINT)std::size(levels);
        fl.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
            caps_.featureLevel = fl.MaxSupportedFeatureLevel;
    }
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        const D3D_SHADER_MODEL models[] = {
            D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
            D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
            D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1,
        };
        for (D3D_SHADER_MODEL m : models) {
            sm.HighestShaderModel = m;
            if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                caps_.shaderModel = sm.HighestShaderModel;
                break;
            }
        }
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            caps_.raytracingTier = o5.RaytracingTier;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
            caps_.meshShaderTier = o7.MeshShaderTier;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
            caps_.vrsTier = o6.VariableShadingRateTier;
    }
    {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory_.As(&f5))) {
            BOOL tearing = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                caps_.tearingSupported = tearing != FALSE;
        }
    }
    LOG_INFO("adapter: %s (%.0f MB VRAM)", caps_.adapterName.c_str(),
             (double)caps_.dedicatedVideoMemory / (1024.0 * 1024.0));
    LOG_INFO("feature level: 0x%04X", (unsigned)caps_.featureLevel);
    LOG_INFO("shader model: 0x%04X", (unsigned)caps_.shaderModel);
    LOG_INFO("raytracing tier: %d", (int)caps_.raytracingTier);
    LOG_INFO("mesh shader tier: %d", (int)caps_.meshShaderTier);
    LOG_INFO("variable shading rate tier: %d", (int)caps_.vrsTier);
    LOG_INFO("tearing supported: %d", caps_.tearingSupported ? 1 : 0);

    // Queue, allocators, command list, fence + event.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!CheckHr(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue")) return false;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!CheckHr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])),
                     "CreateCommandAllocator"))
            return false;
    }
    if (!CheckHr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                            IID_PPV_ARGS(&cmdList_)),
                 "CreateCommandList"))
        return false;
    cmdList_->Close();
    if (!CheckHr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence")) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        LOG_ERROR("CreateEvent failed");
        return false;
    }

    // Heaps.
    if (!srvHeap_.Create(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16384, true)) return false;
    // RTV/DSV heaps are CPU-only (cheap); MME effects declare many render targets each (AutoLuminous ~12).
    if (!rtvHeap_.Create(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false)) return false;
    if (!dsvHeap_.Create(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512, false)) return false;

    // Swap chain.
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = kBackBufferFormat;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    if (caps_.tearingSupported) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc;
    if (!CheckHr(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc),
                 "CreateSwapChainForHwnd"))
        return false;
    if (!CheckHr(sc.As(&swapChain_), "QueryInterface(IDXGISwapChain3)")) return false;
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateBackBuffers();
    width_ = width;
    height_ = height;
    hwnd_ = hwnd;
    return true;
}

ID3D12GraphicsCommandList* Dx12Context::BeginFrame() {
    if (deviceLost_) return nullptr;   // the device is gone: no fences, no D3D calls
    frameSlot_ = (uint32_t)(frameNumber_ % kFramesInFlight);
    if (fence_ && fenceEvent_ && fence_->GetCompletedValue() < slotFence_[frameSlot_]) {
        CheckHr(fence_->SetEventOnCompletion(slotFence_[frameSlot_], fenceEvent_), "SetEventOnCompletion");
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    ProcessCapture();
    // Release deferred resources whose fence has completed.
    UINT64 completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
    deferred_.erase(std::remove_if(deferred_.begin(), deferred_.end(),
                                   [&](const Deferred& d) { return d.fence <= completed; }),
                    deferred_.end());
    if (allocators_[frameSlot_]) allocators_[frameSlot_]->Reset();
    if (cmdList_) cmdList_->Reset(allocators_[frameSlot_].Get(), nullptr);
    if (swapChain_) backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (ID3D12Resource* bb = BackBuffer()) {
        D3D12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(
            bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
    }
    ++frameNumber_;
    recording_ = true;
    return cmdList_.Get();
}

void Dx12Context::EndFrame(bool vsync) {
    if (!cmdList_ || !queue_ || !swapChain_) return;
    if (deviceLost_) return;   // no waits / D3D calls after the device is gone
    bool captureIssued = false;
    ID3D12Resource* bb = BackBuffer();
    const bool wantsCapture = (!capturePath_.empty() || captureMemoryCallback_ != nullptr);
    if (wantsCapture && !captureInFlight_ && bb && device_) {
        D3D12_RESOURCE_DESC desc = bb->GetDesc();
        UINT64 total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &captureFootprint_, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        CD3DX12_RESOURCE_DESC rbDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
        if (CheckHr(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&captureReadback_)),
                     "CreateCommittedResource(capture readback)")) {
            D3D12_RESOURCE_BARRIER toCopy =
                CD3DX12_RESOURCE_BARRIER::Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmdList_->ResourceBarrier(1, &toCopy);
            CD3DX12_TEXTURE_COPY_LOCATION src(bb, 0);
            CD3DX12_TEXTURE_COPY_LOCATION dst(captureReadback_.Get(), captureFootprint_);
            cmdList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER toPresent =
                CD3DX12_RESOURCE_BARRIER::Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                     D3D12_RESOURCE_STATE_PRESENT);
            cmdList_->ResourceBarrier(1, &toPresent);
            captureIssued = true;
        } else {
            D3D12_RESOURCE_BARRIER toPresent =
                CD3DX12_RESOURCE_BARRIER::Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     D3D12_RESOURCE_STATE_PRESENT);
            cmdList_->ResourceBarrier(1, &toPresent);
        }
    } else {
        if (bb) {
            D3D12_RESOURCE_BARRIER toPresent =
                CD3DX12_RESOURCE_BARRIER::Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     D3D12_RESOURCE_STATE_PRESENT);
            cmdList_->ResourceBarrier(1, &toPresent);
        }
    }
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const HRESULT presentHr =
        swapChain_->Present(vsync ? 1 : 0, (!vsync && caps_.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    ++fenceValue_;
    queue_->Signal(fence_.Get(), fenceValue_);
    slotFence_[frameSlot_] = fenceValue_;
    // Resources released while this frame was recorded live until this frame's fence.
    for (Deferred& d : deferred_)
        if (d.fence == kPendingFrame) d.fence = fenceValue_;
    recording_ = false;
    if (captureIssued) {
        captureFence_ = fenceValue_;
        captureInFlightPath_ = capturePath_;
        captureInFlightMemoryCallback_ = std::move(captureMemoryCallback_);
        capturePath_.clear();
        captureMemoryCallback_ = nullptr;
        captureInFlight_ = true;
    }
    CheckDeviceLost(presentHr);
}

void Dx12Context::CheckDeviceLost(HRESULT presentHr) {
    if (deviceLost_) return;
    const bool presentLost = presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET;
    HRESULT reason = S_OK;
    if (device_) reason = device_->GetDeviceRemovedReason();
    if (!presentLost && SUCCEEDED(reason)) return;

    deviceLost_ = true;
    LOG_ERROR("device lost (present=0x%08X, removed reason=0x%08X)", (unsigned)presentHr, (unsigned)reason);

    // DRED: breadcrumb history + page-fault allocations, when the device exposes the interface. Compact: one line
    // per item (the breadcrumb that did not finish, up to 16 existing / 16 recently freed allocations).
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (device_ && SUCCEEDED(device_.As(&dred))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
            for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
                if (!n->pLastBreadcrumbValue || *n->pLastBreadcrumbValue >= n->BreadcrumbCount) continue;
                const UINT32 last = *n->pLastBreadcrumbValue;
                const char* cl = n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?";
                const char* q = n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?";
                const UINT32 op = n->pCommandHistory ? (UINT32)n->pCommandHistory[last] : 0xFFFFFFFFu;
                LOG_ERROR("DRED breadcrumb: cl=%s queue=%s op=%u (%u/%u completed)", cl, q, op, last,
                          n->BreadcrumbCount);
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
            LOG_ERROR("DRED page fault: VA=0x%llX", (unsigned long long)pf.PageFaultVA);
            auto logNodes = [](const char* kind, const D3D12_DRED_ALLOCATION_NODE* head) {
                int count = 0;
                for (const D3D12_DRED_ALLOCATION_NODE* n = head; n && count < 16; n = n->pNext, ++count) {
                    const std::string name =
                        n->ObjectNameW ? WideToUtf8(n->ObjectNameW) : (n->ObjectNameA ? n->ObjectNameA : "");
                    LOG_ERROR("DRED %s alloc: type=%u name=%s", kind, (unsigned)n->AllocationType, name.c_str());
                }
            };
            logNodes("existing", pf.pHeadExistingAllocationNode);
            logNodes("freed", pf.pHeadRecentFreedAllocationNode);
        }
    }
}

void Dx12Context::ProcessCapture() {
    if (!captureInFlight_) return;
    UINT64 completed = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
    if (completed < captureFence_) return;
    if (captureReadback_) {
        uint8_t* data = nullptr;
        if (SUCCEEDED(captureReadback_->Map(0, nullptr, (void**)&data))) {
            const D3D12_SUBRESOURCE_FOOTPRINT& fp = captureFootprint_.Footprint;
            // Back buffer is RGBA8; copy rows into a temp buffer and force alpha to 255.
            std::vector<uint8_t> tmp((size_t)fp.Width * fp.Height * 4);
            for (UINT y = 0; y < fp.Height; ++y) {
                const uint8_t* src = data + captureFootprint_.Offset + (size_t)y * fp.RowPitch;
                uint8_t* dst = tmp.data() + (size_t)y * fp.Width * 4;
                memcpy(dst, src, (size_t)fp.Width * 4);
                for (UINT x = 0; x < fp.Width; ++x) dst[x * 4 + 3] = 255;
            }
            captureReadback_->Unmap(0, nullptr);
            if (!captureInFlightPath_.empty()) {
                if (SavePngRGBA8(captureInFlightPath_, width_, height_, tmp.data(), fp.Width * 4))
                    LOG_INFO("captured %s", PathToUtf8(captureInFlightPath_).c_str());
                else
                    LOG_ERROR("failed to save capture %s", PathToUtf8(captureInFlightPath_).c_str());
            }
            if (captureInFlightMemoryCallback_) {
                captureInFlightMemoryCallback_(width_, height_, std::move(tmp));
            }
        }
        captureReadback_.Reset();
    }
    captureInFlight_ = false;
    captureInFlightPath_.clear();
    captureInFlightMemoryCallback_ = nullptr;
}

void Dx12Context::EndFrameFrameGen(bool vsync, int generatedCount, ID3D12Resource* const* generated,
                                   ID3D12Resource* real, float frameTimeMs) {
    if (!cmdList_ || !queue_ || !swapChain_ || deviceLost_) return;
    // Submit the rendered frame (the final image was already copied out for DLSS-G).
    if (backBuffers_[backBufferIndex_]) {
        D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffers_[backBufferIndex_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        cmdList_->ResourceBarrier(1, &toPresent);
    }
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ++fenceValue_;
    queue_->Signal(fence_.Get(), fenceValue_);
    slotFence_[frameSlot_] = fenceValue_;
    for (Deferred& d : deferred_)
        if (d.fence == kPendingFrame) d.fence = fenceValue_;
    recording_ = false;

    if (!presentAlloc_) {
        if (!CheckHr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&presentAlloc_)),
                     "FrameGen: present allocator") ||
            !CheckHr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presentAlloc_.Get(), nullptr,
                                                IID_PPV_ARGS(&presentList_)),
                     "FrameGen: present list")) {
            presentAlloc_.Reset();
            presentList_.Reset();
            return;
        }
        presentList_->Close();
    }

    const int total = std::max(0, generatedCount) + (real ? 1 : 0);
    if (total == 0) return;
    // Even spacing between the presented frames; vsync's Present already spaces them at the display rate.
    const double intervalMs = (double)std::clamp(frameTimeMs, 1.0f, 100.0f) / (double)total;
    LARGE_INTEGER freq, begin;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&begin);

    for (int i = 0; i < total; ++i) {
        ID3D12Resource* src = i < generatedCount ? generated[i] : real;
        if (!src) continue;
        if (!vsync && i > 0) {
            const double target = intervalMs * (double)i;
            for (;;) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double elapsed = (double)(now.QuadPart - begin.QuadPart) * 1000.0 / (double)freq.QuadPart;
                if (elapsed >= target) break;
                Sleep((DWORD)std::max(0.0, std::min(target - elapsed, 2.0)));
            }
        }
        // One copy + present. The fence wait keeps the next copy off a back buffer the display still reads.
        if (fence_->GetCompletedValue() < fenceValue_) {
            CheckHr(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_), "FrameGen: SetEventOnCompletion");
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        presentAlloc_->Reset();
        presentList_->Reset(presentAlloc_.Get(), nullptr);
        if (swapChain_) backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = backBuffers_[backBufferIndex_].Get();
        D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        presentList_->ResourceBarrier(1, &toCopy);
        presentList_->CopyResource(bb, src);
        D3D12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        presentList_->ResourceBarrier(1, &toPresent);
        presentList_->Close();
        ID3D12CommandList* pl[] = {presentList_.Get()};
        queue_->ExecuteCommandLists(1, pl);
        const HRESULT hr = swapChain_->Present(vsync ? 1 : 0,
                                               (!vsync && caps_.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        ++fenceValue_;
        queue_->Signal(fence_.Get(), fenceValue_);
        CheckDeviceLost(hr);
        if (deviceLost_) return;
    }
}

void Dx12Context::WaitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;
    ++fenceValue_;
    queue_->Signal(fence_.Get(), fenceValue_);
    CheckHr(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_), "SetEventOnCompletion");
    WaitForSingleObject(fenceEvent_, INFINITE);
    ProcessCapture();
    // Everything submitted so far has completed; the open frame's command list has not been submitted yet, so
    // what it references (kPendingFrame) stays.
    deferred_.erase(std::remove_if(deferred_.begin(), deferred_.end(),
                                   [](const Deferred& d) { return d.fence != kPendingFrame; }),
                    deferred_.end());
}

void Dx12Context::DeferRelease(ComPtr<ID3D12Resource> res) {
    deferred_.push_back({recording_ ? kPendingFrame : fenceValue_ + 1, std::move(res)});
}

void Dx12Context::RequestCapture(const std::filesystem::path& pngPath) {
    capturePath_ = pngPath;
}

void Dx12Context::RequestCaptureMemory(MemoryCaptureCallback callback) {
    captureMemoryCallback_ = std::move(callback);
}

void Dx12Context::Resize(uint32_t width, uint32_t height) {
    if (deviceLost_) return;   // no fence waits / D3D calls after the device is gone
    if (width == 0 || height == 0 || (width == width_ && height == height_)) return;
    WaitForGpu();
    ReleaseBackBuffers();
    UINT flags = caps_.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    const HRESULT hr = swapChain_->ResizeBuffers(kBackBufferCount, width, height, kBackBufferFormat, flags);
    CheckDeviceLost(hr);
    if (CheckHr(hr, "ResizeBuffers")) CreateBackBuffers();
    width_ = width;
    height_ = height;
}

void Dx12Context::Shutdown() {
    if (!device_) return;
    WaitForGpu();
    ReleaseBackBuffers();
    captureReadback_.Reset();
    captureInFlight_ = false;
    capturePath_.clear();
    captureInFlightPath_.clear();
    captureMemoryCallback_ = nullptr;
    captureInFlightMemoryCallback_ = nullptr;
    deferred_.clear();
    for (auto& a : allocators_) a.Reset();
    cmdList_.Reset();
    presentList_.Reset();
    presentAlloc_.Reset();
    fence_.Reset();
    queue_.Reset();
    swapChain_.Reset();
    factory_.Reset();
    adapter_.Reset();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    device_.Reset();
}

Dx12Context::~Dx12Context() {
    Shutdown();
}

// ---- UploadBatch -------------------------------------------------------------

UploadBatch::UploadBatch(Dx12Context& ctx) : ctx_(ctx) {
    ctx.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
    ctx.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                    IID_PPV_ARGS(&list_));
    ctx.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
}

UploadBatch::~UploadBatch() {
    Submit();
}

ComPtr<ID3D12Resource> UploadBatch::CreateBuffer(const void* data, size_t bytes,
                                                 D3D12_RESOURCE_STATES finalState, const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || bytes == 0) return {};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(buffer)"))
        return {};
    created_.push_back(res);   // the list below writes it: alive until Submit, whatever the caller does
    if (debugName) res->SetName(debugName);
    if (data) {
        D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
        ComPtr<ID3D12Resource> staging;
        if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                            IID_PPV_ARGS(&staging)),
                     "CreateCommittedResource(upload staging)"))
            return {};
        if (debugName) {
            std::wstring stagingName = std::wstring(debugName) + L".staging";
            staging->SetName(stagingName.c_str());
        }
        void* mapped = nullptr;
        if (FAILED(staging->Map(0, nullptr, &mapped))) {
            LOG_ERROR("failed to map upload staging buffer");
            return {};
        }
        memcpy(mapped, data, bytes);
        staging->Unmap(0, nullptr);
        // Implicit promotion COMMON -> COPY_DEST.
        list_->CopyBufferRegion(res.Get(), 0, staging.Get(), 0, bytes);
        D3D12_RESOURCE_BARRIER barrier =
            CD3DX12_RESOURCE_BARRIER::Transition(res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        list_->ResourceBarrier(1, &barrier);
        staging_.push_back(std::move(staging));
    } else if (finalState != D3D12_RESOURCE_STATE_COMMON) {
        D3D12_RESOURCE_BARRIER barrier =
            CD3DX12_RESOURCE_BARRIER::Transition(res.Get(), D3D12_RESOURCE_STATE_COMMON, finalState);
        list_->ResourceBarrier(1, &barrier);
    }
    return res;
}

ComPtr<ID3D12Resource> UploadBatch::CreateTexture(const ImageRGBA8& image, const wchar_t* debugName) {
    return CreateTextureTyped(image, DXGI_FORMAT_R8G8B8A8_UNORM, debugName);
}

// `format` R8G8B8A8_TYPELESS lets the caller create sRGB and UNORM SRVs on one texture (shader packs).
ComPtr<ID3D12Resource> UploadBatch::CreateTextureTyped(const ImageRGBA8& image, DXGI_FORMAT format,
                                                       const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || image.Empty()) return {};
    const UINT mipCount = (UINT)image.mips.size();
    CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, image.Width(), image.Height(), 1,
                                     (UINT16)mipCount, 1, 0, D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(texture)"))
        return {};
    created_.push_back(res);
    if (debugName) res->SetName(debugName);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> rowCounts(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 total = 0;
    ctx_.Device()->GetCopyableFootprints(&desc, 0, mipCount, 0, footprints.data(), rowCounts.data(),
                                         rowSizes.data(), &total);

    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&staging)),
                 "CreateCommittedResource(texture staging)"))
        return {};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        LOG_ERROR("failed to map texture upload staging buffer");
        return {};
    }
    uint8_t* base = (uint8_t*)mapped;
    for (UINT mip = 0; mip < mipCount; ++mip) {
        const ImageRGBA8::Level& level = image.mips[mip];
        const D3D12_SUBRESOURCE_FOOTPRINT& fp = footprints[mip].Footprint;
        const size_t srcPitch = (size_t)level.width * 4;
        for (UINT y = 0; y < level.height; ++y) {
            memcpy(base + footprints[mip].Offset + (size_t)y * fp.RowPitch,
                   level.pixels.data() + y * srcPitch, srcPitch);
        }
        CD3DX12_TEXTURE_COPY_LOCATION dst(res.Get(), mip);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[mip]);
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    staging->Unmap(0, nullptr);
    staging_.push_back(std::move(staging));

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);
    return res;
}

// One RGBA8 image channel -> an R8_UNORM texture. A channel < 0 uses the red channel (the caller only picks
// single-channel slots through their swizzle, and swizzle 0 is R).
ComPtr<ID3D12Resource> UploadBatch::CreateTextureChannel8(const ImageRGBA8& image, int channel,
                                                          const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || image.Empty() || image.mips[0].pixels.empty()) return {};
    if (channel < 0 || channel > 3) channel = 0;
    const UINT mipCount = (UINT)image.mips.size();
    CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, image.Width(), image.Height(), 1, (UINT16)mipCount, 1, 0,
                                     D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(r8 texture)"))
        return {};
    created_.push_back(res);
    if (debugName) res->SetName(debugName);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> rowCounts(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 total = 0;
    ctx_.Device()->GetCopyableFootprints(&desc, 0, mipCount, 0, footprints.data(), rowCounts.data(), rowSizes.data(),
                                         &total);

    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&staging)),
                 "CreateCommittedResource(r8 staging)"))
        return {};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        LOG_ERROR("failed to map r8 texture upload staging buffer");
        return {};
    }
    uint8_t* base = (uint8_t*)mapped;
    for (UINT mip = 0; mip < mipCount; ++mip) {
        const ImageRGBA8::Level& level = image.mips[mip];
        const D3D12_SUBRESOURCE_FOOTPRINT& fp = footprints[mip].Footprint;
        for (UINT y = 0; y < level.height; ++y) {
            uint8_t* dst = base + footprints[mip].Offset + (size_t)y * fp.RowPitch;
            const uint8_t* src = level.pixels.data() + (size_t)y * level.width * 4 + (size_t)channel;
            for (UINT x = 0; x < level.width; ++x) dst[x] = src[(size_t)x * 4];
        }
        CD3DX12_TEXTURE_COPY_LOCATION dst(res.Get(), mip);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[mip]);
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    staging->Unmap(0, nullptr);
    staging_.push_back(std::move(staging));

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);
    return res;
}

// S9b-3: one mip of raw texels (the sky map's RGBA16F equirect). Same staging / footprint flow as CreateTextureTyped.
ComPtr<ID3D12Resource> UploadBatch::CreateTextureRaw(const void* pixels, uint32_t width, uint32_t height,
                                                    uint32_t bytesPerPixel, DXGI_FORMAT format,
                                                    const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || !pixels || width == 0 || height == 0 || bytesPerPixel == 0) return {};
    CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(texture)"))
        return {};
    created_.push_back(res);
    if (debugName) res->SetName(debugName);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0, total = 0;
    ctx_.Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowSize, &total);

    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&staging)),
                 "CreateCommittedResource(texture staging)"))
        return {};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        LOG_ERROR("failed to map texture upload staging buffer");
        return {};
    }
    uint8_t* base = (uint8_t*)mapped;
    const size_t srcPitch = (size_t)width * bytesPerPixel;
    for (UINT y = 0; y < rowCount; ++y)
        memcpy(base + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch,
               (const uint8_t*)pixels + (size_t)y * srcPitch, srcPitch);
    staging->Unmap(0, nullptr);

    CD3DX12_TEXTURE_COPY_LOCATION dst(res.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprint);
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    staging_.push_back(std::move(staging));   // kept alive until Submit; moved only after the copy reads it

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);
    return res;
}

// ---- S11: block-compressed / cube uploads -----------------------------------------------------
ComPtr<ID3D12Resource> UploadBatch::CreateTextureCompressed(const CompressedImage& image, DXGI_FORMAT format,
                                                          const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || image.Empty() || format == DXGI_FORMAT_UNKNOWN) return {};
    const UINT mipCount = image.mipCount;
    const UINT arraySize = image.faces > 1 ? image.faces : 1;
    CD3DX12_RESOURCE_DESC desc = arraySize > 1
                                     ? CD3DX12_RESOURCE_DESC::Tex2D(format, image.width, image.height, (UINT16)arraySize,
                                                                    (UINT16)mipCount, 1, 0, D3D12_RESOURCE_FLAG_NONE)
                                     : CD3DX12_RESOURCE_DESC::Tex2D(format, image.width, image.height, 1,
                                                                    (UINT16)mipCount, 1, 0, D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(bc texture)"))
        return {};
    created_.push_back(res);
    if (debugName) res->SetName(debugName);

    const UINT subresources = mipCount * arraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> rowCounts(subresources);
    std::vector<UINT64> rowSizes(subresources);
    UINT64 total = 0;
    ctx_.Device()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rowCounts.data(),
                                         rowSizes.data(), &total);

    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&staging)),
                 "CreateCommittedResource(bc staging)"))
        return {};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        LOG_ERROR("failed to map compressed texture upload staging buffer");
        return {};
    }
    uint8_t* base = (uint8_t*)mapped;
    for (UINT sr = 0; sr < subresources; ++sr) {
        // The CompressedImage lists one offset per (face, mip): face-major, so face = sr / mipCount.
        const UINT face = sr / mipCount, mip = sr % mipCount;
        const size_t index = (size_t)face * mipCount + mip;
        if (index >= image.mipOffsets.size()) break;
        const D3D12_SUBRESOURCE_FOOTPRINT& fp = footprints[sr].Footprint;
        const uint8_t* src = image.data.data() + image.mipOffsets[index];
        // Rows of a block-compressed mip are whole blocks: copy row by row with the copyable row pitch.
        for (UINT y = 0; y < rowCounts[sr]; ++y)
            memcpy(base + footprints[sr].Offset + (size_t)y * fp.RowPitch, src + (size_t)y * rowSizes[sr],
                   (size_t)rowSizes[sr]);
        CD3DX12_TEXTURE_COPY_LOCATION dst(res.Get(), sr);
        CD3DX12_TEXTURE_COPY_LOCATION srcLoc(staging.Get(), footprints[sr]);
        list_->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }
    staging->Unmap(0, nullptr);
    staging_.push_back(std::move(staging));

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);
    return res;
}

ComPtr<ID3D12Resource> UploadBatch::CreateTextureCube(const std::array<ImageRGBA8, 6>& faces,
                                                     const wchar_t* debugName) {
    if (!ctx_.Device() || !list_ || faces[0].Empty()) return {};
    const uint32_t w = faces[0].Width(), h = faces[0].Height();
    for (const ImageRGBA8& f : faces)
        if (f.Empty() || f.Width() != w || f.Height() != h) return {};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 6, 1, 1, 0,
                                                              D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(cube texture)"))
        return {};
    created_.push_back(res);
    if (debugName) res->SetName(debugName);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(6);
    std::vector<UINT> rowCounts(6);
    std::vector<UINT64> rowSizes(6);
    UINT64 total = 0;
    ctx_.Device()->GetCopyableFootprints(&desc, 0, 6, 0, footprints.data(), rowCounts.data(), rowSizes.data(),
                                         &total);
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx_.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&staging)),
                 "CreateCommittedResource(cube staging)"))
        return {};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        LOG_ERROR("failed to map cube texture upload staging buffer");
        return {};
    }
    uint8_t* base = (uint8_t*)mapped;
    for (UINT face = 0; face < 6; ++face) {
        const D3D12_SUBRESOURCE_FOOTPRINT& fp = footprints[face].Footprint;
        const size_t srcPitch = (size_t)w * 4;
        for (UINT y = 0; y < h; ++y)
            memcpy(base + footprints[face].Offset + (size_t)y * fp.RowPitch,
                   faces[face].mips[0].pixels.data() + (size_t)y * srcPitch, srcPitch);
        CD3DX12_TEXTURE_COPY_LOCATION dst(res.Get(), face);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[face]);
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    staging->Unmap(0, nullptr);
    staging_.push_back(std::move(staging));

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &barrier);
    return res;
}

ComPtr<ID3D12Resource> UploadBatch::CreateTextureCube(const CompressedImage& cube, DXGI_FORMAT format,
                                                     const wchar_t* debugName) {
    if (cube.faces != 6) return {};
    return CreateTextureCompressed(cube, format, debugName);
}

void UploadBatch::Submit() {
    if (submitted_) return;
    submitted_ = true;
    if (!list_ || !fence_ || !ctx_.Queue()) {
        staging_.clear();
        created_.clear();
        return;
    }
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    ctx_.Queue()->ExecuteCommandLists(1, lists);
    ctx_.Queue()->Signal(fence_.Get(), 1);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        fence_->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
    staging_.clear();
    created_.clear();   // the copies are done: what the caller dropped meanwhile goes now
}

} // namespace mmdx
