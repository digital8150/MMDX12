#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <json.hpp>

#include "app/McpTools.h"
#include "core/TextUtil.h"

using json = nlohmann::json;

namespace {

struct InstanceInfo {
    std::wstring pipePath;
    std::string name;
    uint32_t pid = 0;
};

std::vector<InstanceInfo> EnumerateInstances() {
    std::vector<InstanceInfo> list;
    WIN32_FIND_DATAW data{};
    HANDLE hFind = FindFirstFileW(L"\\\\.\\pipe\\*", &data);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcsncmp(data.cFileName, L"mmdx12_mcp", 10) == 0) {
                InstanceInfo info;
                info.pipePath = L"\\\\.\\pipe\\" + std::wstring(data.cFileName);
                info.name = mmdx::WideToUtf8(data.cFileName);
                const char* prefix = "mmdx12_mcp_";
                if (info.name.rfind(prefix, 0) == 0) {
                    try {
                        info.pid = (uint32_t)std::stoul(info.name.substr(strlen(prefix)));
                    } catch (...) {}
                }
                list.push_back(info);
            }
        } while (FindNextFileW(hFind, &data));
        FindClose(hFind);
    }
    return list;
}

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient() { Disconnect(); }

    bool Connect(const std::wstring& path, uint32_t timeoutMs = 5000) {
        Disconnect();
        auto start = std::chrono::steady_clock::now();
        while (true) {
            hPipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (hPipe_ != INVALID_HANDLE_VALUE) {
                pipePath_ = path;
                readBuf_.clear();
                return true;
            }
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(path.c_str(), 1000);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) break;
            Sleep(50);
        }
        return false;
    }

    void Disconnect() {
        if (hPipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe_);
            hPipe_ = INVALID_HANDLE_VALUE;
        }
        pipePath_.clear();
        readBuf_.clear();
    }

    bool IsConnected() const { return hPipe_ != INVALID_HANDLE_VALUE; }
    uint32_t ServerPid() const {
        ULONG pid = 0;
        return IsConnected() && GetNamedPipeServerProcessId(hPipe_, &pid) ? (uint32_t)pid : 0;
    }
    const std::wstring& PipePath() const { return pipePath_; }

    // Sends one request line and returns the response line whose "id" matches `id`. Lines with another id
    // are answers to earlier calls that timed out here: they are dropped.
    bool Transact(const std::string& reqLine, uint64_t id, std::string& respLine, uint32_t timeoutMs) {
        if (!IsConnected()) return false;

        OVERLAPPED ovWrite{};
        ovWrite.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD written = 0;
        BOOL ok = WriteFile(hPipe_, reqLine.data(), (DWORD)reqLine.size(), &written, &ovWrite);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovWrite.hEvent, timeoutMs) != WAIT_OBJECT_0) CancelIoEx(hPipe_, &ovWrite);
            ok = GetOverlappedResult(hPipe_, &ovWrite, &written, TRUE);
        }
        CloseHandle(ovWrite.hEvent);
        if (!ok || written != reqLine.size()) {
            Disconnect();
            return false;
        }

        OVERLAPPED ovRead{};
        ovRead.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        const auto startTime = std::chrono::steady_clock::now();
        bool result = false;
        while (true) {
            const size_t nl = readBuf_.find('\n');
            if (nl != std::string::npos) {
                std::string lineOut = readBuf_.substr(0, nl);
                readBuf_.erase(0, nl + 1);
                if (!lineOut.empty() && lineOut.back() == '\r') lineOut.pop_back();
                const json j = json::parse(lineOut, nullptr, false);
                if (!j.is_discarded() && j.is_object() && j.value("id", 0ull) == id) {
                    respLine = std::move(lineOut);
                    result = true;
                    break;
                }
                continue;  // a stale answer
            }

            const auto elapsed = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= timeoutMs) break;
            const DWORD remainingMs = (DWORD)(timeoutMs - elapsed);

            DWORD bytesRead = 0;
            ResetEvent(ovRead.hEvent);
            ok = ReadFile(hPipe_, chunk_, sizeof(chunk_), &bytesRead, &ovRead);
            if (!ok) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    Disconnect();
                    break;
                }
                if (WaitForSingleObject(ovRead.hEvent, remainingMs) != WAIT_OBJECT_0) {
                    // timed out: cancel and wait, so the read never lands in chunk_ later (data that did arrive is kept)
                    CancelIoEx(hPipe_, &ovRead);
                    if (GetOverlappedResult(hPipe_, &ovRead, &bytesRead, TRUE) && bytesRead > 0)
                        readBuf_.append(chunk_, bytesRead);
                    break;
                }
                if (!GetOverlappedResult(hPipe_, &ovRead, &bytesRead, FALSE)) {
                    Disconnect();
                    break;
                }
            }
            if (bytesRead == 0) {
                Disconnect();
                break;
            }
            readBuf_.append(chunk_, bytesRead);
        }
        CloseHandle(ovRead.hEvent);
        return result;
    }

private:
    HANDLE hPipe_ = INVALID_HANDLE_VALUE;
    std::wstring pipePath_;
    std::string readBuf_;
    char chunk_[65536];
};

bool LaunchAppProcess(const std::filesystem::path& exeDir, const std::string& extraArgs, uint32_t& outPid) {
    std::filesystem::path exePath = exeDir / "MMDX12.exe";
    if (!std::filesystem::exists(exePath)) return false;

    std::wstring cmd = L"\"" + exePath.wstring() + L"\" --mcp";
    if (!extraArgs.empty()) {
        cmd += L" " + mmdx::Utf8ToWide(extraArgs);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    std::wstring dirW = exeDir.wstring();
    BOOL ok = CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, dirW.c_str(), &si, &pi);
    if (!ok) return false;

    outPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool ConnectAutoOrTarget(PipeClient& client, const std::string& targetPipe, uint32_t targetPid, uint32_t timeoutMs, std::string& outErr) {
    if (!targetPipe.empty()) {
        std::wstring wpath;
        if (targetPipe.rfind("\\\\.\\pipe\\", 0) == 0) {
            wpath = mmdx::Utf8ToWide(targetPipe);
        } else {
            wpath = L"\\\\.\\pipe\\" + mmdx::Utf8ToWide(targetPipe);
        }
        if (client.Connect(wpath, timeoutMs)) return true;
        outErr = "Failed to connect to pipe: " + targetPipe;
        return false;
    }

    if (targetPid != 0) {
        std::wstring wpath = L"\\\\.\\pipe\\mmdx12_mcp_" + std::to_wstring(targetPid);
        if (client.Connect(wpath, timeoutMs)) return true;
        outErr = "Failed to connect to PID " + std::to_string(targetPid);
        return false;
    }

    // Auto connect:
    // 1. Try base pipe
    std::wstring base = L"\\\\.\\pipe\\mmdx12_mcp";
    if (client.Connect(base, 1000)) return true;

    // 2. Scan instances
    auto instances = EnumerateInstances();
    if (instances.empty()) {
        outErr = "No running MMDX12 instances found";
        return false;
    }
    if (instances.size() == 1) {
        if (client.Connect(instances[0].pipePath, timeoutMs)) return true;
        outErr = "Failed to connect to instance: " + instances[0].name;
        return false;
    }

    outErr = "Multiple MMDX12 instances found (" + std::to_string(instances.size()) + "). Specify pipe or pid.";
    return false;
}

// Connects to the pipe served by process `pid` (mmdx12_mcp if it owns it, else mmdx12_mcp_<pid>).
bool ConnectToProcess(PipeClient& client, uint32_t pid, uint32_t timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    const std::wstring own = L"\\\\.\\pipe\\mmdx12_mcp_" + std::to_wstring(pid);
    while (true) {
        if (client.Connect(own, 0)) return true;
        if (client.Connect(L"\\\\.\\pipe\\mmdx12_mcp", 0)) {
            if (client.ServerPid() == pid) return true;
            client.Disconnect();
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) return false;
        Sleep(250);
    }
}

} // namespace

int Run(int argc, char* argv[]) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::string argPipe;
    uint32_t argPid = 0;
    bool argLaunch = false;
    uint32_t argTimeoutMs = 30000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pipe" && i + 1 < argc) {
            argPipe = argv[++i];
        } else if (a == "--pid" && i + 1 < argc) {
            argPid = (uint32_t)std::stoul(argv[++i]);
        } else if (a == "--launch") {
            argLaunch = true;
        } else if (a == "--timeout" && i + 1 < argc) {
            argTimeoutMs = (uint32_t)std::stoul(argv[++i]);
        }
    }

    std::filesystem::path exeDir = mmdx::ExecutableDir();
    PipeClient client;
    std::string connectErr;

    ConnectAutoOrTarget(client, argPipe, argPid, 1500, connectErr);
    if (!client.IsConnected() && argLaunch) {
        uint32_t launchedPid = 0;
        if (LaunchAppProcess(exeDir, "", launchedPid)) {
            ConnectToProcess(client, launchedPid, 60000);
        }
    }

    uint64_t bridgeSeq = 1;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            json errResp = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32700}, {"message", "Parse error"}}}
            };
            std::cout << errResp.dump() << "\n";
            std::cout.flush();
            continue;
        }

        auto reqId = req.contains("id") ? req["id"] : json(nullptr);
        std::string method = req.value("method", "");

        if (method == "initialize") {
            json res = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", {
                    {"protocolVersion", mmdx::kMcpProtocolVersion},
                    {"capabilities", {{"tools", json::object()}}},
                    {"serverInfo", {
                        {"name", "mmdx12_mcp"},
                        {"version", "1.0.0"}
                    }}
                }}
            };
            std::cout << res.dump() << "\n";
            std::cout.flush();
            continue;
        }

        if (method == "notifications/initialized") {
            // No response required for notification
            continue;
        }

        if (method == "ping") {
            json res = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", json::object()}
            };
            std::cout << res.dump() << "\n";
            std::cout.flush();
            continue;
        }

        if (method == "tools/list") {
            json res = {
                {"jsonrpc", "2.0"},
                {"id", reqId},
                {"result", {
                    {"tools", mmdx::GetMcpToolDefinitions()}
                }}
            };
            std::cout << res.dump() << "\n";
            std::cout.flush();
            continue;
        }

        if (method == "tools/call") {
            json params = req.value("params", json::object());
            std::string toolName = params.value("name", "");
            json args = params.value("arguments", json::object());

            // 1. list_instances
            if (toolName == "list_instances") {
                auto instances = EnumerateInstances();
                json arr = json::array();
                for (const auto& inst : instances) {
                    arr.push_back({
                        {"name", inst.name},
                        {"pipe", mmdx::WideToUtf8(inst.pipePath)},
                        {"pid", inst.pid}
                    });
                }
                json outRes = {{"instances", arr}};
                json res = {
                    {"jsonrpc", "2.0"},
                    {"id", reqId},
                    {"result", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", outRes.dump(2)}}
                        })},
                        {"isError", false}
                    }}
                };
                std::cout << res.dump() << "\n";
                std::cout.flush();
                continue;
            }

            // 2. connect
            if (toolName == "connect") {
                std::string pipeToConnect = args.value("pipe_name", "");
                if (pipeToConnect.empty()) pipeToConnect = args.value("pipe", "");
                uint32_t pidToConnect = args.value("pid", 0u);

                std::string err;
                bool ok = ConnectAutoOrTarget(client, pipeToConnect, pidToConnect, 5000, err);
                if (ok) {
                    json outRes = {
                        {"connected", true},
                        {"pipe", mmdx::WideToUtf8(client.PipePath())}
                    };
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", outRes.dump(2)}}
                            })},
                            {"isError", false}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                } else {
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", err.empty() ? "Connect failed" : err}}
                            })},
                            {"isError", true}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                }
                continue;
            }

            // 3. launch_app
            if (toolName == "launch_app") {
                std::string extraArgs = args.value("extra_args", "");
                if (extraArgs.empty()) extraArgs = args.value("args", "");

                uint32_t launchedPid = 0;
                if (!LaunchAppProcess(exeDir, extraArgs, launchedPid)) {
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", "Failed to launch MMDX12.exe from " + mmdx::PathToUtf8(exeDir)}}
                            })},
                            {"isError", true}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                    continue;
                }

                // The library scan runs before the pipe opens: wait up to 60 s for the launched process's pipe
                // (the base name if it was free, else the pid-suffixed one).
                std::string err = "its MCP pipe did not open within 60 s";
                bool ok = ConnectToProcess(client, launchedPid, 60000);

                if (ok) {
                    json outRes = {
                        {"launched", true},
                        {"pid", launchedPid},
                        {"pipe", mmdx::WideToUtf8(client.PipePath())}
                    };
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", outRes.dump(2)}}
                            })},
                            {"isError", false}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                } else {
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", "Launched PID " + std::to_string(launchedPid) + " but failed to connect to pipe: " + err}}
                            })},
                            {"isError", true}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                }
                continue;
            }

            // Forward to MMDX12 pipe
            if (!client.IsConnected()) {
                // Try auto-reconnect once
                std::string reconnectErr;
                ConnectAutoOrTarget(client, argPipe, argPid, 1000, reconnectErr);
            }

            if (!client.IsConnected()) {
                json res = {
                    {"jsonrpc", "2.0"},
                    {"id", reqId},
                    {"result", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", "Not connected to MMDX12 instance. Use connect or launch_app tool first."}}
                        })},
                        {"isError", true}
                    }}
                };
                std::cout << res.dump() << "\n";
                std::cout.flush();
                continue;
            }

            json pipeReq = {
                {"id", bridgeSeq++},
                {"tool", toolName},
                {"args", args}
            };

            std::string respLine;
            // the app answers within McpToolTimeoutMs (its own timeout error included): wait a little longer
            const uint32_t callTimeout = std::max<uint32_t>(argTimeoutMs, mmdx::McpToolTimeoutMs(toolName) + 5000);
            const bool ok = client.Transact(pipeReq.dump() + "\n", pipeReq["id"].get<uint64_t>(), respLine, callTimeout);

            if (!ok) {
                json res = {
                    {"jsonrpc", "2.0"},
                    {"id", reqId},
                    {"result", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", client.IsConnected()
                                                 ? "MMDX12 did not answer in time (the call may still finish in the app)"
                                                 : "Lost the connection to MMDX12 (the app closed or crashed). Use launch_app or connect."}}
                        })},
                        {"isError", true}
                    }}
                };
                std::cout << res.dump() << "\n";
                std::cout.flush();
                continue;
            }

            try {
                json pipeResp = json::parse(respLine);
                bool pipeOk = pipeResp.value("ok", false);
                if (pipeOk) {
                    json contentArr = json::array();
                    json resultData = pipeResp.value("result", json::object());
                    // an image travels as an image block only, not again inside the text
                    std::string image;
                    if (resultData.contains("image_base64") && resultData["image_base64"].is_string()) {
                        image = resultData["image_base64"].get<std::string>();
                        resultData.erase("image_base64");
                    }
                    contentArr.push_back({
                        {"type", "text"},
                        {"text", resultData.dump(2)}
                    });
                    if (!image.empty())
                        contentArr.push_back({{"type", "image"}, {"data", std::move(image)}, {"mimeType", "image/png"}});

                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", contentArr},
                            {"isError", false}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                } else {
                    std::string errMsg = pipeResp.value("error", "Execution failed");
                    json res = {
                        {"jsonrpc", "2.0"},
                        {"id", reqId},
                        {"result", {
                            {"content", json::array({
                                {{"type", "text"}, {"text", errMsg}}
                            })},
                            {"isError", true}
                        }}
                    };
                    std::cout << res.dump() << "\n";
                    std::cout.flush();
                }
            } catch (const std::exception& e) {
                json res = {
                    {"jsonrpc", "2.0"},
                    {"id", reqId},
                    {"result", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", std::string("Malformed pipe response: ") + e.what()}}
                        })},
                        {"isError", true}
                    }}
                };
                std::cout << res.dump() << "\n";
                std::cout.flush();
            }
            continue;
        }

        // Unknown method
        json errResp = {
            {"jsonrpc", "2.0"},
            {"id", reqId},
            {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}
        };
        std::cout << errResp.dump() << "\n";
        std::cout.flush();
    }

    return 0;
}

int main(int argc, char* argv[]) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {  // stdout is the protocol: report on stderr
        std::cerr << "mmdx12_mcp: fatal error: " << e.what() << std::endl;
        return 1;
    }
}
