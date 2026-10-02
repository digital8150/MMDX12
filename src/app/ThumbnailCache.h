#pragma once
// Background thumbnail cache for library assets (characters, stages).
// A worker thread loads cached PNGs or, on a cache miss, the PMX models + textures. The main
// thread (Pump) renders misses through a caller-supplied callback, saves the PNG and creates
// GPU textures usable as ImTextureID.
#include "app/SceneLoader.h"
#include "asset/ImageLoader.h"
#include "render/Dx12Context.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mmdx {

enum class ThumbnailKind : uint8_t { Character = 0, Stage = 1 };

class ThumbnailCache {
public:
    // Renders `models` (one for a character, all parts for a stage) into `out` (level 0 only
    // is required; the cache builds no mips itself). Called on the main thread from Pump().
    // Return false on failure (the entry is then marked failed and never retried this run).
    using RenderFn = std::function<bool(ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out)>;

    ThumbnailCache() = default;
    ~ThumbnailCache();  // calls Shutdown()
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // cacheDir is created if missing. Starts the worker thread.
    void Initialize(Dx12Context& ctx, const std::filesystem::path& cacheDir, RenderFn render);
    // Stops and joins the worker, WaitForGpu, releases textures and frees SRV descriptors.
    void Shutdown();
    // Drops every entry and its texture (e.g. after a library rescan). Pending work is discarded.
    // Call before ImGui::NewFrame(), and the caller must WaitForGpu() first.
    void Clear();

    // Returns 0 while not ready (or failed). The first call for an assetId enqueues it.
    // `sources` are the PMX files (1 for a character, all parts for a stage).
    uint64_t Get(const std::string& assetId, ThumbnailKind kind,  // == ImTextureID (GPU descriptor handle ptr)
                 const std::vector<std::filesystem::path>& sources);
    // Same as Get but never enqueues.
    uint64_t Peek(const std::string& assetId) const;  // == ImTextureID (GPU descriptor handle ptr)
    // Size in pixels of a ready thumbnail (0,0 if not ready).
    void Size(const std::string& assetId, uint32_t& w, uint32_t& h) const;

    // Main thread, once per frame, OUTSIDE of command-list recording (i.e. before
    // ctx.BeginFrame()). Does at most: 1 render (RenderFn) and 4 texture uploads per call.
    void Pump();

    int PendingCount() const;   // queued + loading + waiting for render/upload
    bool Busy() const { return PendingCount() > 0; }

private:
    enum class State { Queued, Loading, NeedsRender, NeedsUpload, Ready, Failed };
    struct Entry {
        State state = State::Queued;
        ThumbnailKind kind = ThumbnailKind::Character;
        std::vector<std::filesystem::path> sources;
        std::filesystem::path cacheFile;
        std::vector<LoadedModelCpu> models;   // filled by worker on cache miss (NeedsRender)
        ImageRGBA8 image;                     // filled by worker (cache hit) or Pump (after render)
        ComPtr<ID3D12Resource> texture;
        uint32_t srv = DescriptorHeap::kInvalid;
        uint32_t width = 0, height = 0;
        uint64_t generation = 0;              // == generation_ at enqueue time
    };

    void ReleaseEntryLocked(Entry& e);  // mutex_ held

    Dx12Context* ctx_ = nullptr;
    RenderFn render_;
    std::filesystem::path cacheDir_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    bool initialized_ = false;
    uint64_t generation_ = 1;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> queue_;

    void WorkerLoop();
};

} // namespace mmdx
