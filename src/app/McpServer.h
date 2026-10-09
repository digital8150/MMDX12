#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <json.hpp>

namespace mmdx {

struct McpPromise {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    nlohmann::json result;
    std::string error;

    void Resolve(nlohmann::json res) {
        std::lock_guard<std::mutex> lock(mtx);
        result = std::move(res);
        ok = true;
        done = true;
        cv.notify_one();
    }

    void Reject(std::string err) {
        std::lock_guard<std::mutex> lock(mtx);
        error = std::move(err);
        ok = false;
        done = true;
        cv.notify_one();
    }

    bool Wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [&] { return done; });
    }
};

struct McpRequest {
    uint64_t id = 0;
    std::string tool;
    nlohmann::json args;
    std::shared_ptr<McpPromise> promise;
};

class McpServer {
public:
    McpServer();
    ~McpServer();

    bool Start();
    void Stop();

    bool IsRunning() const { return running_; }
    bool IsConnected() const { return connected_; }
    std::string PipeName() const { return pipeNameUtf8_; }

    std::vector<McpRequest> PopRequests();

private:
    void WorkerLoop();

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::string pipeNameUtf8_;
    std::wstring pipeNameWide_;

    void* hPipe_ = (void*)(intptr_t)-1; // INVALID_HANDLE_VALUE
    void* shutdownEvent_ = nullptr;
    void* ioEvent_ = nullptr;
    std::thread workerThread_;

    std::mutex queueMtx_;
    std::vector<McpRequest> queue_;
};

} // namespace mmdx
