#include "app/ThumbnailCache.h"

#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <cstdio>
#include <system_error>

namespace mmdx {

namespace {

constexpr uint32_t kThumbVersion = 5;   // bump to invalidate every cached PNG
constexpr int kMaxUploadsPerPump = 4;

uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

ThumbnailCache::~ThumbnailCache() {
    Shutdown();
}

void ThumbnailCache::Initialize(Dx12Context& ctx, const std::filesystem::path& cacheDir, RenderFn render) {
    std::lock_guard<std::mutex> l(mutex_);
    if (initialized_) return;
    ctx_ = &ctx;
    render_ = std::move(render);
    cacheDir_ = cacheDir;
    std::error_code ec;
    std::filesystem::create_directories(cacheDir_, ec);
    stop_ = false;
    worker_ = std::thread(&ThumbnailCache::WorkerLoop, this);
    initialized_ = true;
}

void ThumbnailCache::Shutdown() {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!initialized_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!initialized_) return;
        if (ctx_) ctx_->WaitForGpu();
        ++generation_;
        queue_.clear();
        for (auto& [id, e] : entries_) ReleaseEntryLocked(e);
        entries_.clear();
        ctx_ = nullptr;
        initialized_ = false;
    }
}

void ThumbnailCache::Clear() {
    std::lock_guard<std::mutex> l(mutex_);
    if (!ctx_) return;
    ctx_->WaitForGpu();
    ++generation_;
    queue_.clear();
    for (auto& [id, e] : entries_) ReleaseEntryLocked(e);
    entries_.clear();
}

void ThumbnailCache::ReleaseEntryLocked(Entry& e) {
    if (e.texture) ctx_->DeferRelease(std::move(e.texture));
    if (e.srv != DescriptorHeap::kInvalid) {
        ctx_->SrvHeap().Free(e.srv, 1);
        e.srv = DescriptorHeap::kInvalid;
    }
    e.image = {};
    e.models.clear();
}

uint64_t ThumbnailCache::Get(const std::string& assetId, ThumbnailKind kind,
                             const std::vector<std::filesystem::path>& sources) {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = entries_.find(assetId);
    if (it != entries_.end()) {
        const Entry& e = it->second;
        if (e.state == State::Ready && e.srv != DescriptorHeap::kInvalid)
            return ctx_->SrvHeap().Gpu(e.srv).ptr;
        return 0;
    }
    Entry& e = entries_[assetId];
    e.state = State::Queued;
    e.kind = kind;
    e.sources = sources;
    e.generation = generation_;
    queue_.push_back(assetId);
    cv_.notify_one();
    return 0;
}

uint64_t ThumbnailCache::Peek(const std::string& assetId) const {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = entries_.find(assetId);
    if (it == entries_.end()) return 0;
    const Entry& e = it->second;
    if (e.state == State::Ready && e.srv != DescriptorHeap::kInvalid)
        return ctx_->SrvHeap().Gpu(e.srv).ptr;
    return 0;
}

void ThumbnailCache::Size(const std::string& assetId, uint32_t& w, uint32_t& h) const {
    std::lock_guard<std::mutex> l(mutex_);
    w = 0;
    h = 0;
    auto it = entries_.find(assetId);
    if (it == entries_.end()) return;
    const Entry& e = it->second;
    if (e.state == State::Ready) {
        w = e.width;
        h = e.height;
    }
}

int ThumbnailCache::PendingCount() const {
    std::lock_guard<std::mutex> l(mutex_);
    int n = 0;
    for (const auto& [id, e] : entries_) {
        if (e.state == State::Queued || e.state == State::Loading || e.state == State::NeedsRender ||
            e.state == State::NeedsUpload)
            ++n;
    }
    return n;
}

void ThumbnailCache::WorkerLoop() {
    for (;;) {
        std::string id;
        {
            std::unique_lock<std::mutex> l(mutex_);
            cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            id = queue_.front();
            queue_.pop_front();
            auto it = entries_.find(id);
            if (it == entries_.end() || it->second.state != State::Queued) continue;
            it->second.state = State::Loading;
        }

        // Unlocked: load the entry (filesystem + PMX parsing can take a while).
        State result = State::Failed;
        ImageRGBA8 img;                  // cache-hit path
        std::vector<LoadedModelCpu> lms; // cache-miss path
        std::filesystem::path cacheFile;
        uint64_t generation = 0;
        try {
            ThumbnailKind kind{};
            std::vector<std::filesystem::path> sources;
            {
                std::lock_guard<std::mutex> l(mutex_);
                auto it = entries_.find(id);
                if (it != entries_.end()) {
                    kind = it->second.kind;
                    sources = it->second.sources;
                    generation = it->second.generation;
                }
            }

            // Cache file name: 64-bit FNV-1a over a key with asset id, version and source
            // file metadata (size, last write time). Touches the filesystem, hence on the
            // worker thread.
            std::string key = id + "|" + std::to_string(kThumbVersion);
            for (const std::filesystem::path& src : sources) {
                uint64_t size = 0;
                uint64_t mtime = 0;
                std::error_code ec;
                auto st = std::filesystem::status(src, ec);
                if (!ec && std::filesystem::is_regular_file(st)) {
                    auto fsize = std::filesystem::file_size(src, ec);
                    if (!ec) size = (uint64_t)fsize;
                    auto lw = std::filesystem::last_write_time(src, ec);
                    if (!ec) mtime = (uint64_t)lw.time_since_epoch().count();
                }
                key += "|" + PathToUtf8(src) + "|" + std::to_string(size) + "|" + std::to_string(mtime);
            }
            uint64_t h = Fnv1a64(key);
            char name[32];
            snprintf(name, sizeof(name), "%016llx.png", (unsigned long long)h);
            cacheFile = cacheDir_ / name;

            // Cache hit?
            bool loaded = false;
            std::error_code ec;
            if (std::filesystem::exists(cacheFile, ec)) {
                std::string err;
                if (LoadImageRGBA8(cacheFile, img, &err)) {
                    loaded = true;
                } else {
                    std::filesystem::remove(cacheFile, ec);
                    LOG_WARN("thumbnail cache load failed, re-rendering: %s", PathToUtf8(cacheFile).c_str());
                }
            }

            if (loaded) {
                result = State::NeedsUpload;
            } else {
                // Cache miss: load the PMX models + textures.
                const char* label = id.c_str();
                lms.reserve(sources.size());
                size_t loadedParts = 0;
                for (const std::filesystem::path& src : sources) {
                    auto m = LoadedModelCpu{};
                    m.pmx = std::make_shared<PmxModel>();
                    std::string err;
                    if (!LoadModelFile(src, kind == ThumbnailKind::Character ? ModelRole::Character : ModelRole::Stage, *m.pmx, &err)) {
                        if (kind == ThumbnailKind::Character) {
                            LOG_WARN("thumbnail model load failed: %s: %s", PathToUtf8(src).c_str(), err.c_str());
                            result = State::Failed;
                            lms.clear();
                            break;
                        }
                        LOG_WARN("thumbnail stage part load failed, skipped: %s: %s", PathToUtf8(src).c_str(),
                                 err.c_str());
                        continue;
                    }
                    DecodeModelTextures(m, nullptr, 0.f, 1.f, label);
                    lms.push_back(std::move(m));
                    ++loadedParts;
                }
                if (!lms.empty() && (kind == ThumbnailKind::Character || loadedParts > 0)) {
                    result = State::NeedsRender;
                } else {
                    result = State::Failed;
                    lms.clear();
                }
            }
        } catch (...) {
            LOG_WARN("thumbnail load failed: %s", id.c_str());
            result = State::Failed;
            img = {};
            lms.clear();
        }

        // Write the result back (drop it when the entry is gone or the generation changed).
        {
            std::lock_guard<std::mutex> l(mutex_);
            auto it = entries_.find(id);
            if (it == entries_.end() || it->second.generation != generation) {
                // dropped; the entry was cleared or re-enqueued meanwhile
                continue;
            }
            it->second.cacheFile = cacheFile;
            if (result == State::NeedsUpload) {
                it->second.image = std::move(img);
            } else if (result == State::NeedsRender) {
                it->second.models = std::move(lms);
            }
            it->second.state = result;
        }
    }
}

void ThumbnailCache::Pump() {
    if (!ctx_) return;

    // 1. Render step: at most one NeedsRender entry per Pump call.
    {
        std::string id;
        ThumbnailKind kind{};
        std::filesystem::path cacheFile;
        uint64_t generation = 0;
        std::vector<LoadedModelCpu> models;
        bool have = false;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (auto& [key, e] : entries_) {
                if (e.state != State::NeedsRender) continue;
                id = key;
                kind = e.kind;
                cacheFile = e.cacheFile;
                generation = e.generation;
                models = std::move(e.models);
                e.state = State::Loading;  // not picked twice
                have = true;
                break;
            }
        }
        if (have) {
            ImageRGBA8 img;
            bool ok = false;
            if (!models.empty() && render_)
                ok = render_(kind, models, img);
            models.clear();  // frees CPU memory

            if (ok) {
                if (img.Empty()) {
                    LOG_WARN("thumbnail render produced no pixels: %s", id.c_str());
                } else {
                    if (!SavePngRGBA8(cacheFile, img.mips[0].width, img.mips[0].height,
                                      img.mips[0].pixels.data(), img.mips[0].width * 4))
                        LOG_WARN("thumbnail png save failed: %s", PathToUtf8(cacheFile).c_str());
                    // Keep level 0 only: a single-mip texture is fine for UI.
                    if (img.mips.size() > 1) img.mips.resize(1);
                }
            } else {
                img = {};
            }

            std::lock_guard<std::mutex> l(mutex_);
            auto it = entries_.find(id);
            if (it != entries_.end() && it->second.generation == generation) {
                if (ok) {
                    it->second.image = std::move(img);
                    it->second.state = State::NeedsUpload;
                } else {
                    it->second.state = State::Failed;
                }
            }
        }
    }

    // 2. Upload step: up to kMaxUploadsPerPump NeedsUpload entries in one batch.
    struct ToUpload {
        std::string id;
        uint64_t generation;
        ImageRGBA8 image;
    };
    std::vector<ToUpload> uploads;
    {
        std::lock_guard<std::mutex> l(mutex_);
        for (auto& [key, e] : entries_) {
            if ((int)uploads.size() >= kMaxUploadsPerPump) break;
            if (e.state != State::NeedsUpload) continue;
            if (e.image.Empty()) {  // must not pass an empty image to CreateTexture
                e.state = State::Failed;
                continue;
            }
            ToUpload& u = uploads.emplace_back();
            u.id = key;
            u.generation = e.generation;
            u.image = std::move(e.image);
            e.state = State::Loading;
        }
    }
    if (!uploads.empty()) {
        UploadBatch batch(*ctx_);
        std::vector<ComPtr<ID3D12Resource>> textures(uploads.size());
        for (size_t i = 0; i < uploads.size(); ++i)
            textures[i] = batch.CreateTexture(uploads[i].image, L"thumbnail");
        batch.Submit();

        // Allocate SRV descriptors and create the views (unlocked).
        std::vector<uint32_t> srvs(uploads.size(), DescriptorHeap::kInvalid);
        for (size_t i = 0; i < uploads.size(); ++i) {
            srvs[i] = ctx_->SrvHeap().Allocate(1);
            if (srvs[i] == DescriptorHeap::kInvalid || !textures[i]) continue;
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Texture2D.MipLevels = (UINT)-1;
            ctx_->Device()->CreateShaderResourceView(textures[i].Get(), &d, ctx_->SrvHeap().Cpu(srvs[i]));
        }

        {
            std::lock_guard<std::mutex> l(mutex_);
            for (size_t i = 0; i < uploads.size(); ++i) {
                ToUpload& u = uploads[i];
                auto it = entries_.find(u.id);
                if (it != entries_.end() && it->second.generation == u.generation && textures[i] &&
                    srvs[i] != DescriptorHeap::kInvalid) {
                    Entry& e = it->second;
                    e.texture = textures[i];
                    e.srv = srvs[i];
                    e.width = u.image.Width();
                    e.height = u.image.Height();
                    e.image = {};  // free CPU pixels
                    e.state = State::Ready;
                } else {
                    if (textures[i]) ctx_->DeferRelease(textures[i]);
                    if (srvs[i] != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(srvs[i], 1);
                    if (it != entries_.end() && it->second.generation == u.generation)
                        it->second.state = State::Failed;  // texture or SRV allocation failed
                }
            }
        }
    }
}

} // namespace mmdx
