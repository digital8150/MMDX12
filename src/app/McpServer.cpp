#include "app/McpServer.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>

#include "app/McpTools.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

namespace {
// Most MCP clients (agent sessions, the smoke test, a second bridge) talking to one app at the same time. The listener
// always keeps one free instance waiting, so a new bridge connects while others are busy.
constexpr DWORD kMaxPipeInstances = 8;

// Waits for an overlapped operation that returned ERROR_IO_PENDING. On shutdown the operation is cancelled and
// waited for, so the kernel never writes into the caller's OVERLAPPED / buffer after it went out of scope.
bool WaitIo(HANDLE pipe, OVERLAPPED& ov, HANDLE shutdownEv, DWORD& bytes) {
    HANDLE waitHandles[] = {shutdownEv, ov.hEvent};
    const DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

HANDLE CreateInstance(const std::wstring& name, bool first, void* securityDescriptor) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), securityDescriptor, FALSE};
    return CreateNamedPipeW(name.c_str(),
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, kMaxPipeInstances, 65536, 65536, 0, &sa);
}
} // namespace

McpServer::McpServer() = default;

McpServer::~McpServer() {
    Stop();
}

bool McpServer::Start() {
    if (running_) return true;

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shutdownEvent_) {
        LOG_ERROR("MCP: failed to create sync events");
        Stop();
        return false;
    }

    // owner-only pipe; kept for every further instance the listener creates
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)", SDDL_REVISION_1, &pSD, nullptr))
        securityDescriptor_ = pSD;
    else
        LOG_WARN("MCP: failed to create security descriptor, falling back to default");

    // The first instance claims the name: the base name for the first app, a PID-suffixed one for further apps.
    std::wstring name = L"\\\\.\\pipe\\mmdx12_mcp";
    HANDLE pipe = CreateInstance(name, true, securityDescriptor_);
    if (pipe == INVALID_HANDLE_VALUE) {
        name = L"\\\\.\\pipe\\mmdx12_mcp_" + std::to_wstring(GetCurrentProcessId());
        pipe = CreateInstance(name, true, securityDescriptor_);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        LOG_ERROR("MCP: failed to create named pipe (error %lu)", GetLastError());
        Stop();
        return false;
    }

    pipeNameWide_ = name;
    pipeNameUtf8_ = WideToUtf8(name);
    running_ = true;
    LOG_INFO("MCP listening on %s (up to %lu clients)", pipeNameUtf8_.c_str(), kMaxPipeInstances);

    listenerThread_ = std::thread(&McpServer::ListenLoop, this, (void*)pipe);
    return true;
}

void McpServer::Stop() {
    if (!running_ && !listenerThread_.joinable()) return;
    running_ = false;

    // Every thread cancels its own pending I/O when it sees the event and closes its own pipe instance.
    if (shutdownEvent_) SetEvent((HANDLE)shutdownEvent_);
    if (listenerThread_.joinable()) listenerThread_.join();
    {
        std::lock_guard<std::mutex> lock(clientsMtx_);
        for (Client& c : clients_)
            if (c.thread.joinable()) c.thread.join();
        clients_.clear();
    }

    if (shutdownEvent_) {
        CloseHandle((HANDLE)shutdownEvent_);
        shutdownEvent_ = nullptr;
    }
    if (securityDescriptor_) {
        LocalFree(securityDescriptor_);
        securityDescriptor_ = nullptr;
    }

    clientCount_ = 0;
    std::lock_guard<std::mutex> lock(queueMtx_);
    for (auto& req : queue_) {
        if (req.promise) req.promise->Reject("Server stopped");
    }
    queue_.clear();
}

std::vector<McpRequest> McpServer::PopRequests() {
    std::vector<McpRequest> out;
    std::lock_guard<std::mutex> lock(queueMtx_);
    out.swap(queue_);
    return out;
}

// Waits for a client on the free instance, hands the connected instance to its own thread, makes the next free one.
void McpServer::ListenLoop(void* firstPipe) {
    HANDLE pipe = (HANDLE)firstPipe;
    HANDLE shutdownEv = (HANDLE)shutdownEvent_;
    HANDLE connectEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (running_ && pipe != INVALID_HANDLE_VALUE && connectEv) {
        OVERLAPPED ov{};
        ov.hEvent = connectEv;
        ResetEvent(connectEv);
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                DWORD unused = 0;
                connected = WaitIo(pipe, ov, shutdownEv, unused) && running_;
                if (!running_) break;
            }
        }
        if (!connected) {
            DisconnectNamedPipe(pipe);   // a failed connect: retry on the same instance
            if (WaitForSingleObject(shutdownEv, 50) == WAIT_OBJECT_0) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clientsMtx_);
            // reap the threads of clients that already left
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (it->done->load()) {
                    if (it->thread.joinable()) it->thread.join();
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
            Client c;
            c.done = std::make_shared<std::atomic<bool>>(false);
            c.thread = std::thread(&McpServer::ClientLoop, this, (void*)pipe, c.done);
            clients_.push_back(std::move(c));
        }

        // the next free instance (when all are in use, wait for one to come back)
        pipe = INVALID_HANDLE_VALUE;
        while (running_) {
            pipe = CreateInstance(pipeNameWide_, false, securityDescriptor_);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (WaitForSingleObject(shutdownEv, 200) == WAIT_OBJECT_0) break;
        }
    }

    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (connectEv) CloseHandle(connectEv);
}

// One connected client: newline-delimited JSON requests in, one response line per request out. Requests are queued
// for the main thread (App::PumpMcp); several clients' requests interleave there.
void McpServer::ClientLoop(void* pipeHandle, std::shared_ptr<std::atomic<bool>> done) {
    HANDLE hPipe = (HANDLE)pipeHandle;
    HANDLE shutdownEv = (HANDLE)shutdownEvent_;
    HANDLE ioEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const int clients = ++clientCount_;
    LOG_INFO("MCP client connected on %s (%d connected)", pipeNameUtf8_.c_str(), clients);

    std::string readBuf;
    bool clientActive = ioEv != nullptr;
    while (running_ && clientActive) {
        uint8_t chunk[4096];
        DWORD bytesRead = 0;
        OVERLAPPED ovRead{};
        ovRead.hEvent = ioEv;
        ResetEvent(ioEv);

        BOOL ok = ReadFile(hPipe, chunk, sizeof(chunk), &bytesRead, &ovRead);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING || !WaitIo(hPipe, ovRead, shutdownEv, bytesRead) || !running_) break;
        }
        if (bytesRead == 0) break;

        readBuf.append(reinterpret_cast<const char*>(chunk), bytesRead);

        // Process all newline-delimited JSON objects
        size_t newline = 0;
        while (clientActive && (newline = readBuf.find('\n')) != std::string::npos) {
            std::string line = readBuf.substr(0, newline);
            readBuf.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            nlohmann::json resp;
            uint64_t reqId = 0;
            try {
                auto j = nlohmann::json::parse(line);
                reqId = j.value("id", 0ull);
                resp["id"] = reqId;

                McpRequest req;
                req.id = reqId;
                req.tool = j.value("tool", "");
                req.args = j.value("args", nlohmann::json::object());
                req.promise = std::make_shared<McpPromise>();

                {
                    std::lock_guard<std::mutex> lock(queueMtx_);
                    queue_.push_back(req);
                }

                // Wait in slices so a shutdown (main thread in Stop) never waits for a command it will not run.
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(McpToolTimeoutMs(req.tool));
                bool finished = false;
                while (running_ && !finished && std::chrono::steady_clock::now() < deadline)
                    finished = req.promise->Wait(std::chrono::milliseconds(100));

                if (!running_) {
                    clientActive = false;
                    break;
                }

                if (!finished) {
                    resp["ok"] = false;
                    resp["error"] = "Command timed out";
                } else if (!req.promise->ok) {
                    resp["ok"] = false;
                    resp["error"] = req.promise->error.empty() ? "Command execution failed" : req.promise->error;
                } else {
                    resp["ok"] = true;
                    resp["result"] = req.promise->result;
                }
            } catch (const std::exception& e) {
                resp["id"] = reqId;
                resp["ok"] = false;
                resp["error"] = std::string("Malformed JSON: ") + e.what();
            }

            // Write response line
            std::string outStr = resp.dump() + "\n";
            OVERLAPPED ovWrite{};
            ovWrite.hEvent = ioEv;
            ResetEvent(ioEv);
            DWORD written = 0;
            if (!WriteFile(hPipe, outStr.data(), (DWORD)outStr.size(), &written, &ovWrite)) {
                if (GetLastError() != ERROR_IO_PENDING || !WaitIo(hPipe, ovWrite, shutdownEv, written) || !running_) {
                    clientActive = false;
                    break;
                }
            }
            FlushFileBuffers(hPipe);
        }
    }

    const int left = --clientCount_;
    LOG_INFO("MCP client disconnected from %s (%d connected)", pipeNameUtf8_.c_str(), left);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    if (ioEv) CloseHandle(ioEv);
    done->store(true);
}

} // namespace mmdx
