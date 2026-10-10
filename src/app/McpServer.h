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

// The app side of MCP: a named pipe (owner-only) that several clients (bridges) may use at once, each served by its
// own thread; their requests are queued for the main thread (PopRequests, App::PumpMcp).
class McpServer {
public:
    McpServer();
    ~McpServer();

    bool Start();
    void Stop();

    bool IsRunning() const { return running_; }
    bool IsConnected() const { return clientCount_ > 0; }
    int ClientCount() const { return clientCount_; }
    std::string PipeName() const { return pipeNameUtf8_; }

    std::vector<McpRequest> PopRequests();

private:
    void ListenLoop(void* firstPipe);
    void ClientLoop(void* pipe, std::shared_ptr<std::atomic<bool>> done);

    struct Client {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::atomic<bool> running_{false};
    std::atomic<int> clientCount_{0};
    std::string pipeNameUtf8_;
    std::wstring pipeNameWide_;

    void* shutdownEvent_ = nullptr;
    void* securityDescriptor_ = nullptr;   // LocalAlloc'ed, shared by every pipe instance
    std::thread listenerThread_;
    std::mutex clientsMtx_;
    std::vector<Client> clients_;

    std::mutex queueMtx_;
    std::vector<McpRequest> queue_;
};

} // namespace mmdx
