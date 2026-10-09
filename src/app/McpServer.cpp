#include "app/McpServer.h"

#include <Windows.h>
#include <sddl.h>

#include "app/McpTools.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

namespace {
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
} // namespace

McpServer::McpServer() = default;

McpServer::~McpServer() {
    Stop();
}

bool McpServer::Start() {
    if (running_) return true;

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ioEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shutdownEvent_ || !ioEvent_) {
        LOG_ERROR("MCP: failed to create sync events");
        Stop();
        return false;
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;OW)", SDDL_REVISION_1, &pSD, nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    } else {
        LOG_WARN("MCP: failed to create security descriptor, falling back to default");
    }

    std::wstring name = L"\\\\.\\pipe\\mmdx12_mcp";
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, &sa);

    if (pipe == INVALID_HANDLE_VALUE) {
        // Fall back to PID-suffixed name
        name = L"\\\\.\\pipe\\mmdx12_mcp_" + std::to_wstring(GetCurrentProcessId());
        pipe = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa);
    }

    if (pSD) LocalFree(pSD);

    if (pipe == INVALID_HANDLE_VALUE) {
        LOG_ERROR("MCP: failed to create named pipe (error %lu)", GetLastError());
        Stop();
        return false;
    }

    hPipe_ = pipe;
    pipeNameWide_ = name;
    pipeNameUtf8_ = WideToUtf8(name);
    running_ = true;
    LOG_INFO("MCP listening on %s", pipeNameUtf8_.c_str());

    workerThread_ = std::thread(&McpServer::WorkerLoop, this);
    return true;
}

void McpServer::Stop() {
    if (!running_ && !workerThread_.joinable()) return;
    running_ = false;

    // The worker cancels its own pending I/O when it sees the event; the handle is closed after it has left.
    if (shutdownEvent_) SetEvent((HANDLE)shutdownEvent_);
    if (workerThread_.joinable()) workerThread_.join();
    if (hPipe_ && hPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }

    if (shutdownEvent_) {
        CloseHandle((HANDLE)shutdownEvent_);
        shutdownEvent_ = nullptr;
    }
    if (ioEvent_) {
        CloseHandle((HANDLE)ioEvent_);
        ioEvent_ = nullptr;
    }

    connected_ = false;
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

void McpServer::WorkerLoop() {
    HANDLE hPipe = (HANDLE)hPipe_;
    HANDLE shutdownEv = (HANDLE)shutdownEvent_;
    HANDLE ioEv = (HANDLE)ioEvent_;

    while (running_) {
        OVERLAPPED ovConnect{};
        ovConnect.hEvent = ioEv;
        ResetEvent(ioEv);

        BOOL connected = ConnectNamedPipe(hPipe, &ovConnect);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD unused = 0;
                if (!WaitIo(hPipe, ovConnect, shutdownEv, unused) || !running_) break;
            } else if (err != ERROR_PIPE_CONNECTED) {
                if (!running_) break;
                Sleep(50);
                continue;
            }
        }

        connected_ = true;
        LOG_INFO("MCP client connected on %s", pipeNameUtf8_.c_str());

        std::string readBuf;
        bool clientActive = true;

        while (running_ && clientActive) {
            uint8_t chunk[4096];
            DWORD bytesRead = 0;
            OVERLAPPED ovRead{};
            ovRead.hEvent = ioEv;
            ResetEvent(ioEv);

            BOOL ok = ReadFile(hPipe, chunk, sizeof(chunk), &bytesRead, &ovRead);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    if (!WaitIo(hPipe, ovRead, shutdownEv, bytesRead) || !running_) {
                        clientActive = false;
                        break;
                    }
                } else {
                    clientActive = false;
                    break;
                }
            }

            if (bytesRead == 0) {
                clientActive = false;
                break;
            }

            readBuf.append(reinterpret_cast<const char*>(chunk), bytesRead);

            // Process all newline-delimited JSON objects
            size_t newline = 0;
            while ((newline = readBuf.find('\n')) != std::string::npos) {
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
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(McpToolTimeoutMs(req.tool));
                    bool done = false;
                    while (running_ && !done && std::chrono::steady_clock::now() < deadline)
                        done = req.promise->Wait(std::chrono::milliseconds(100));

                    if (!running_) {
                        clientActive = false;
                        break;
                    }

                    if (!done) {
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
                    if (GetLastError() == ERROR_IO_PENDING) {
                        if (!WaitIo(hPipe, ovWrite, shutdownEv, written) || !running_) {
                            clientActive = false;
                            break;
                        }
                    } else {
                        clientActive = false;
                        break;
                    }
                }
                FlushFileBuffers(hPipe);
            }
        }

        connected_ = false;
        LOG_INFO("MCP client disconnected from %s", pipeNameUtf8_.c_str());
        DisconnectNamedPipe(hPipe);
    }
}

} // namespace mmdx
