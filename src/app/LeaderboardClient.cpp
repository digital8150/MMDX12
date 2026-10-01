#include "app/LeaderboardClient.h"

#include <Windows.h>
#include <winhttp.h>

#include <json.hpp>
#include "core/Log.h"
#include "core/TextUtil.h"

#pragma comment(lib, "winhttp.lib")

namespace mmdx {

namespace {

// RAII guard for WinHTTP handles.
struct HInternet {
    HINTERNET h = nullptr;
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

std::string BodyPrefix(const std::string& body, size_t maxChars) {
    return body.substr(0, std::min(body.size(), maxChars));
}

nlohmann::json ParseJson(const std::string& body) {
    return nlohmann::json::parse(body, nullptr, false);
}

// "HTTP <status>: <server error field or body prefix 200 chars>"
std::string HttpError(int status, const std::string& body) {
    nlohmann::json j = ParseJson(body);
    if (!j.is_discarded() && j.is_object() && j.contains("error") && j["error"].is_string())
        return "HTTP " + std::to_string(status) + ": " + j["error"].get<std::string>();
    return "HTTP " + std::to_string(status) + ": " + BodyPrefix(body, 200);
}

} // namespace

LeaderboardClient::LeaderboardClient(std::string baseUrl) : baseUrl_(std::move(baseUrl)) {}

int LeaderboardClient::Request(const wchar_t* method, const std::string& pathAndQuery,
                               const std::string& body, std::string& responseBody, std::string* error) {
    // Crack the base URL: scheme, host, port, base path.
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    std::wstring baseWide = Utf8ToWide(baseUrl_);
    if (!WinHttpCrackUrl(baseWide.c_str(), (DWORD)baseWide.size(), 0, &uc)) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    std::wstring scheme(uc.lpszScheme, uc.dwSchemeLength);
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    INTERNET_PORT port = uc.nPort;
    std::wstring basePath(uc.lpszUrlPath, uc.dwUrlPathLength);
    while (!basePath.empty() && basePath.back() == L'/') basePath.pop_back();
    const bool https = _wcsicmp(scheme.c_str(), L"https") == 0;

    HInternet session(WinHttpOpen(L"MMDX12/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 10000);

    HInternet connect(WinHttpConnect(session.h, host.c_str(), port, 0));
    if (!connect) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }

    std::wstring fullPath = basePath + Utf8ToWide(pathAndQuery);
    HInternet request(WinHttpOpenRequest(connect.h, method, fullPath.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }

    const wchar_t* extraHeaders =
        (wcscmp(method, L"POST") == 0) ? L"Content-Type: application/json; charset=utf-8\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    BOOL sent = WinHttpSendRequest(request.h, extraHeaders, (DWORD)-1,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        if (error) *error = "network error (WinHTTP " + std::to_string(GetLastError()) + ")";
        return 0;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

    responseBody.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) break;
        if (available == 0) break;
        size_t offset = responseBody.size();
        responseBody.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, responseBody.data() + offset, available, &read)) break;
        if (read == 0) break;
        responseBody.resize(offset + read);
    }
    return (int)status;
}

bool LeaderboardClient::Fetch(const std::string& category, LeaderboardPage& out, std::string* error) {
    out = LeaderboardPage();
    out.category = category;
    std::string response;
    std::string err;
    int status = Request(L"GET", "/leaderboard?category=" + category, "", response, &err);
    if (status == 0) {
        if (error) *error = err;
        out.error = err;
        return false;
    }
    if (status != 200) {
        std::string msg = HttpError(status, response);
        if (error) *error = msg;
        out.error = msg;
        return false;
    }

    nlohmann::json j = ParseJson(response);
    if (j.is_discarded() || !j.is_object()) {
        std::string msg = "invalid leaderboard response";
        if (error) *error = msg;
        out.error = msg;
        return false;
    }

    out.totalCount = j.value("totalCount", 0);
    if (j.contains("topRecords") && j["topRecords"].is_array()) {
        for (const auto& r : j["topRecords"]) {
            if (!r.is_object()) continue;
            LeaderboardEntry e;
            e.rank = r.value("rank", 0);
            e.nickname = r.value("nickname", "");
            e.score = r.value("score", 0);
            e.tier = r.value("tier", "");
            e.avgFps = r.value("avgFps", 0.0f);
            e.low1Fps = r.value("low1Fps", 0.0f);
            e.frametimeStd = r.value("frametimeStd", 0.0f);
            e.resolution = r.value("resolution", "");
            e.gpu = r.value("gpu", "");
            e.os = r.value("os", "");
            e.date = r.value("date", "");
            out.top.push_back(std::move(e));
        }
    }
    out.ok = true;
    return true;
}

SubmitResponse LeaderboardClient::Submit(const SubmitRequest& req) {
    SubmitResponse out;
    nlohmann::json j;
    j["category"] = req.category;
    j["nickname"] = req.nickname;
    j["score"] = req.score;
    j["tier"] = req.tier;
    j["avgFps"] = req.avgFps;
    j["low1Fps"] = req.low1Fps;
    j["frametimeStd"] = req.frametimeStd;
    j["resolution"] = req.resolution;
    j["gpu"] = req.gpu;
    j["os"] = req.os;
    std::string body = j.dump();

    std::string response;
    std::string err;
    int status = Request(L"POST", "/submit", body, response, &err);
    if (status == 0) {
        out.error = err;
        return out;
    }

    if (status == 429) {
        out.error = "제출이 너무 잦습니다 (분당 10회 제한)";
        return out;
    }
    if (status != 200 && status != 201) {
        out.error = HttpError(status, response);
        return out;
    }

    nlohmann::json resp = ParseJson(response);
    if (resp.is_discarded() || !resp.is_object()) {
        out.error = "invalid submit response";
        return out;
    }
    if (!resp.value("success", false)) {
        out.error = HttpError(status, response);
        return out;
    }
    out.ok = true;
    out.rank = resp.value("rank", 0);
    out.totalCount = resp.value("totalCount", 0);
    return out;
}

} // namespace mmdx
