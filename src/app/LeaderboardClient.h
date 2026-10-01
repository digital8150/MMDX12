#pragma once
// Client for the benchmark leaderboard API
// (home.codingbot.kr/api/benchmark). WinHTTP, synchronous:
// call from a worker thread.
#include <string>
#include <vector>

namespace mmdx {

struct LeaderboardEntry {
    int rank = 0;
    std::string nickname, tier, resolution, gpu, os, date;  // date: ISO-8601 string from server
    int score = 0;
    float avgFps = 0, low1Fps = 0, frametimeStd = 0;
};

struct LeaderboardPage {
    std::string category;
    std::vector<LeaderboardEntry> top;  // "topRecords", up to 50
    int totalCount = 0;
    bool ok = false;     // false => `error` describes the failure
    std::string error;
};

struct SubmitRequest {
    std::string category, nickname, tier, resolution, gpu, os;
    int score = 0;
    float avgFps = 0, low1Fps = 0, frametimeStd = 0;
};

struct SubmitResponse {
    bool ok = false;
    int rank = 0, totalCount = 0;
    std::string error;  // human-readable (HTTP status / server "error" field / WinHTTP failure)
};

class LeaderboardClient {
public:
    explicit LeaderboardClient(std::string baseUrl);  // e.g. "https://home.codingbot.kr/api/benchmark"
    bool Fetch(const std::string& category, LeaderboardPage& out, std::string* error);  // GET /leaderboard?category=
    SubmitResponse Submit(const SubmitRequest& req);                                   // POST /submit (JSON)

private:
    // Returns HTTP status (0 on transport failure, with *error set).
    int Request(const wchar_t* method, const std::string& pathAndQuery, const std::string& body,
                std::string& responseBody, std::string* error);
    std::string baseUrl_;
};

} // namespace mmdx
