// In-app auto-update implementation (see Updater.h for the design and the three-process flow).
#include "app/Updater.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <json.hpp>
#include "core/I18n.h"
#include "core/NetUtil.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#pragma comment(lib, "winhttp.lib")

namespace mmdx::updater {

namespace fs = std::filesystem;

using net::DownloadToFile;
using net::ExtractZip;
using net::HttpGet;
using net::IsLocalSource;
using net::LocalPathOf;
using net::Sha256OfFile;

namespace {

// --- small helpers -----------------------------------------------------------------------

std::string Trimmed(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string LowerAsciiOf(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool ReadSmallFile(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool WriteSmallFile(const fs::path& file, const std::string& data) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    out.flush();
    return (bool)out;
}

bool FileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

uint32_t CurrentPid() { return GetCurrentProcessId(); }

// --- SHA-256 (FIPS 180-4, self-contained; no third-party dependency) -----------------------

// --- paths / journal -----------------------------------------------------------------------

fs::path UpdateRoot() { return ExecutableDir() / Utf8ToPath(kUpdateDirName); }
fs::path ExtractDir() { return UpdateRoot() / L"extract"; }
fs::path OldDir() { return UpdateRoot() / L"old"; }
fs::path StateFile() { return UpdateRoot() / L"state.json"; }

// The journal: {"phase":"pending"|"swapping","version":"1.2.3","pid":1234,"relaunchArgs":""}
//   pending:   extracted + verified; the applier should run the swap.
//   swapping:  the applier died mid-swap; the next start completes it (PerformSwap is
//              idempotent: pass 1 re-renames collided entries aside, pass 2 moves everything in).
struct Journal {
    std::string phase;
    std::string version;
    uint32_t pid = 0;
    std::string relaunchArgs;
    bool Read(const fs::path& file);
    void Write(const fs::path& file) const;
};

bool Journal::Read(const fs::path& file) {
    std::string data;
    if (!ReadSmallFile(file, data)) return false;
    const nlohmann::json j = nlohmann::json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    phase = j.value("phase", std::string());
    version = j.value("version", std::string());
    pid = (uint32_t)j.value("pid", 0u);
    relaunchArgs = j.value("relaunchArgs", std::string());
    return !phase.empty() && !version.empty();
}

void Journal::Write(const fs::path& file) const {
    nlohmann::json j;
    j["phase"] = phase;
    j["version"] = version;
    j["pid"] = pid;
    j["relaunchArgs"] = relaunchArgs;
    WriteSmallFile(file, j.dump(2));
}

bool JournalWrite(const std::string& phase, const std::string& version, uint32_t pid,
                  const std::string& relaunchArgs) {
    Journal j;
    j.phase = phase;
    j.version = version;
    j.pid = pid;
    j.relaunchArgs = relaunchArgs;
    j.Write(StateFile());
    return FileExists(StateFile());
}

// The extracted tree: one top folder MMDX12-<version>-win64/ (package_release.ps1). Find the
// folder holding MMDX12.exe, two levels deep at most.
fs::path FindStagedRoot(const fs::path& extractDir) {
    std::error_code ec;
    if (FileExists(extractDir / L"MMDX12.exe")) return extractDir;
    std::vector<fs::path> dirs;
    for (const fs::directory_entry& e : fs::directory_iterator(extractDir, ec))
        if (e.is_directory(ec)) dirs.push_back(e.path());
    for (const fs::path& d : dirs)
        if (FileExists(d / L"MMDX12.exe")) return d;
    for (const fs::path& d : dirs) {
        std::vector<fs::path> sub;
        for (const fs::directory_entry& e : fs::directory_iterator(d, ec))
            if (e.is_directory(ec)) sub.push_back(e.path());
        for (const fs::path& s : sub)
            if (FileExists(s / L"MMDX12.exe")) return s;
    }
    return {};
}

// The swap replaces only program files: the user's library/ (and the journal itself) survive.
bool IsProgramEntry(const fs::path& p) {
    std::wstring lower = p.filename().wstring();
    for (wchar_t& c : lower)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return lower != L"library" && lower != L"update_tmp";
}

bool WaitForProcessExit(uint32_t pid, uint32_t timeoutMs) {
    if (pid == 0 || pid == CurrentPid()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;  // already gone (or no rights: proceed, renames work on live files)
    const DWORD r = WaitForSingleObject(h, timeoutMs);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

// Spawn a fully formed command line classlessly (no console for a GUI child, no window of ours).
bool SpawnDetached(const std::wstring& cmdLine) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// --- the staged swap -----------------------------------------------------------------------

// Restore every asided entry that has no live replacement (a failed/half-finished swap).
void RollbackAside() {
    const fs::path aside = OldDir();
    if (!FileExists(aside)) return;
    const fs::path root = ExecutableDir();
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(aside, ec)) {
        const fs::path live = root / e.path().filename();
        if (!FileExists(live)) {
            fs::rename(e.path(), live, ec);
            if (ec)
                LOG_ERROR("update: could not restore %s (%s)", PathToUtf8(e.path().filename()).c_str(),
                          ec.message().c_str());
            else
                LOG_INFO("update: restored %s", PathToUtf8(e.path().filename()).c_str());
        }
    }
    fs::remove_all(aside, ec);
}

// Keep the updating app's log: the applier (and the restarted app) start fresh logs, so the
// pre-update log is archived as mmdx12.previous.log. Ignore errors (no log, locked, ...).
void ArchiveLog() {
    std::error_code ec;
    const fs::path logPath = ExecutableDir() / L"mmdx12.log";
    if (!FileExists(logPath)) return;
    fs::remove(ExecutableDir() / L"mmdx12.previous.log", ec);
    fs::rename(logPath, ExecutableDir() / L"mmdx12.previous.log", ec);
}

// Complete the swap. Every failure path returns false with the install rolled back to a
// working state (the asided old files are restored). Idempotent: re-running after an
// interrupted swap finishes what is missing.
bool PerformSwap(const std::string& version, const fs::path& stagedRoot, const std::string& relaunchArgs,
                 std::string& error) {
    const fs::path root = ExecutableDir();
    const fs::path aside = OldDir();
    JournalWrite("swapping", version, CurrentPid(), relaunchArgs);

    // The staged entries to move in (program files only; library/ is skipped).
    std::vector<fs::path> staged;
    std::error_code ec;
    for (fs::directory_iterator it(stagedRoot, ec), end; it != end && !ec; it.increment(ec))
        if (IsProgramEntry(it->path())) staged.push_back(it->path());
    if (ec) {
        error = "cannot read the staged files (" + ec.message() + ")";
        return false;
    }
    // Files first, directories last (a directory rename-aside fails while its contents are mapped).
    std::sort(staged.begin(), staged.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        const bool da = fs::is_directory(a, e1), db = fs::is_directory(b, e2);
        if (da != db) return !da;
        return PathToUtf8(a.filename()) < PathToUtf8(b.filename());
    });

    struct Undo {
        fs::path staged;          // the entry that was swapped
        fs::path aside;           // where the old file was parked (empty: nothing was replaced)
    };
    std::vector<Undo> undo;
    const auto rollback = [&](const std::string& why) {
        error = why;
        LOG_ERROR("update: swap failed: %s - rolling back", why.c_str());
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
            std::error_code e2;
            const fs::path live = root / it->staged.filename();
            if (!it->aside.empty() && FileExists(live))
                fs::remove_all(live, e2);  // drop the new file, the old one comes back
            if (!it->aside.empty() && !FileExists(live)) {
                e2.clear();
                fs::rename(it->aside, live, e2);
            }
            if (e2)
                LOG_ERROR("update: rollback of %s failed (%s)", PathToUtf8(it->staged.filename()).c_str(),
                          e2.message().c_str());
        }
        std::error_code e3;
        fs::remove_all(aside, e3);
        return false;
    };

    fs::create_directories(aside, ec);
    // Pass 1: rename every collided live entry aside (the running exe renames fine).
    for (const fs::path& s : staged) {
        const fs::path live = root / s.filename();
        if (!FileExists(live)) continue;
        fs::path target = aside / s.filename();
        fs::remove_all(target, ec);  // leftovers of an earlier attempt
        fs::rename(live, target, ec);
        if (ec) return rollback("cannot set aside " + PathToUtf8(s.filename()) + " (" + ec.message() + ")");
        undo.push_back({s, target});
    }
    // Pass 2: move the staged entries in (roll back = delete the new live file and rename the
    // asided old one back, which is exactly what the rollback lambda does).
    for (const fs::path& s : staged) {
        const fs::path live = root / s.filename();
        fs::rename(s, live, ec);
        if (ec) return rollback("cannot move in " + PathToUtf8(s.filename()) + " (" + ec.message() + ")");
    }

    fs::remove_all(aside, ec);  // everything moved: the asided old files are gone for good
    return true;
}

// Spawn the restarted app (journal relaunchArgs appended; empty after a plain click-update).
bool SpawnRelaunch(const std::string& relaunchArgs) {
    std::wstring cmd = L"\"" + (ExecutableDir() / L"MMDX12.exe").wstring() + L"\"";
    if (!relaunchArgs.empty()) {
        cmd += L" ";
        cmd += Utf8ToWide(relaunchArgs);
    }
    return SpawnDetached(cmd);
}

// True while a probe file can be created next to the exe (the install folder is writable).
bool InstallDirWritable() {
    const fs::path probe = ExecutableDir() / L"update_probe.tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, probe.c_str(), L"wb") != 0 || !f) return false;
    std::fclose(f);
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

} // namespace

// --- public API ----------------------------------------------------------------------------

int CompareVersions(const std::string& a, const std::string& b) { return net::CompareVersions(a, b); }

bool HasPendingUpdate() {
    Journal j;
    return j.Read(StateFile()) && (j.phase == "pending" || j.phase == "swapping");
}

std::string AppliedVersionBreadcrumb() {
    std::string data;
    if (!ReadSmallFile(UpdateRoot() / L"applied.txt", data)) return {};
    return Trimmed(data);
}

void CleanupUpdateLeftovers() {
    const fs::path root = UpdateRoot();
    if (!FileExists(root)) return;
    // A staged swap waits to be applied: main.cpp completes it before any window exists.
    if (HasPendingUpdate()) return;
    const std::string applied = AppliedVersionBreadcrumb();
    if (!applied.empty())
        LOG_INFO("update: %s installed (this run started after the update)", applied.c_str());
    std::error_code ec;
    fs::remove_all(root, ec);
    if (!FileExists(root)) LOG_INFO("update: update_tmp cleaned");
}

Feed FetchFeed(const std::string& feedUrlOrPath) {
    Feed out;
    std::string body, error;
    if (IsLocalSource(feedUrlOrPath)) {
        if (!ReadSmallFile(LocalPathOf(feedUrlOrPath), body)) {
            out.error = "cannot open feed source " + feedUrlOrPath;
            return out;
        }
    } else if (!HttpGet(feedUrlOrPath, body, error, 1u << 22)) {
        out.error = error;
        return out;
    }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        out.error = "invalid feed response";
        return out;
    }
    out.version = Trimmed(j.value("version", std::string()));
    out.url = Trimmed(j.value("url", std::string()));
    out.page = Trimmed(j.value("page", std::string()));
    out.sha256 = LowerAsciiOf(Trimmed(j.value("sha256", std::string())));
    out.size = j.value("size", (uint64_t)0);
    if (j.contains("notes") && j["notes"].is_object())
        for (auto it = j["notes"].begin(); it != j["notes"].end(); ++it)
            if (it.value().is_string()) out.notes[it.key()] = it.value().get<std::string>();
    if (out.version.empty()) {
        out.error = "feed has no version";
        return out;
    }
    if (out.url.empty()) {
        out.error = "feed has no download url";
        return out;
    }
    out.ok = true;
    return out;
}

// The applier: flow step 2 (no window, no UI). Runs when the updating app spawned
// MMDX12.exe --apply-update --apply-wait <pid>, or at the next start after an interrupted
// swap. Returns the process exit code.
int ApplyPendingUpdateAndRelaunch(uint32_t waitPid, bool fromCli) {
    (void)fromCli;
    const fs::path stateFile = StateFile();
    Journal j;
    if (!j.Read(stateFile)) {
        if (FileExists(UpdateRoot())) LOG_WARN("update: no valid update journal found");
        return 1201;
    }
    LOG_INFO("update: applying %s (phase %s)", j.version.c_str(), j.phase.c_str());
    WaitForProcessExit(waitPid, 60000);

    const fs::path stagedRoot = FindStagedRoot(ExtractDir());
    if (stagedRoot.empty()) {
        LOG_ERROR("update: staged files are missing - restoring the old install");
        RollbackAside();
        std::error_code ec;
        fs::remove_all(UpdateRoot(), ec);
        SpawnRelaunch(j.relaunchArgs);
        return 1203;
    }

    std::string error;
    if (!PerformSwap(j.version, stagedRoot, j.relaunchArgs, error)) {
        LOG_ERROR("update: %s", error.c_str());
        RollbackAside();
        std::error_code ec;
        fs::remove_all(UpdateRoot(), ec);  // the install is old-consistent again
        SpawnRelaunch(j.relaunchArgs);     // give the user the old app back
        return 1202;
    }
    ArchiveLog();

    WriteSmallFile(UpdateRoot() / L"applied.txt", j.version);  // breadcrumb for the next start
    std::error_code ec;
    fs::remove(stateFile, ec);
    LOG_INFO("update: %s installed - restarting", j.version.c_str());
    SpawnRelaunch(j.relaunchArgs);
    return 0;
}

// --- Controller ----------------------------------------------------------------------------

Controller::~Controller() {
    cancel_.store(true, std::memory_order_relaxed);
    if (check_.valid()) check_.wait();
    if (update_.valid()) update_.wait();
}

void Controller::Configure(std::string feedUrl, std::string currentVersion) {
    feedUrl_ = std::move(feedUrl);
    currentVersion_ = std::move(currentVersion);
}

void Controller::StartCheck() {
    if (check_.valid()) return;
    noticeHidden_ = false;
    notice_ = State{};
    notice_.phase = Phase::Checking;
    const std::string url = feedUrl_;
    check_ = std::async(std::launch::async, [url] { return FetchFeed(url); });
}

// Build the notice from a good feed (UI thread; workers communicate through return values).
void Controller::ProcessFeed(const Feed& f) {
    noticeHidden_ = false;
    if (!f.ok) {
        notice_ = State{};
        notice_.error = f.error;
        LOG_WARN("update: check failed: %s", f.error.c_str());
        return;
    }
    feed_ = f;
    if (!IsNewer(f.version, currentVersion_)) {
        LOG_INFO("update: up to date (%s, feed %s)", currentVersion_.c_str(), f.version.c_str());
        notice_ = State{};
        notice_.phase = Phase::UpToDate;
        return;
    }
    notice_ = State{};
    notice_.phase = Phase::Available;
    notice_.version = f.version;
    notice_.page = f.page;
    // Note in the UI language, falling back to en, then the first entry.
    const Language lang = ActiveLanguage();
    const char* code = lang == Language::Korean ? "ko" : lang == Language::Japanese ? "ja"
                     : lang == Language::Chinese ? "zh" : "en";
    auto it = f.notes.find(code);
    if (it == f.notes.end()) it = f.notes.find("en");
    if (it == f.notes.end() && !f.notes.empty()) it = f.notes.begin();
    notice_.note = it == f.notes.end() ? std::string() : it->second;
    // The install folder must be writable for the staged swap; otherwise offer the release page.
    if (!InstallDirWritable()) {
        notice_.phase = Phase::NotWritable;
        LOG_WARN("update: %s available but the install folder is not writable", f.version.c_str());
        return;
    }
    LOG_INFO("update: %s available", f.version.c_str());
}

void Controller::CheckNow() {
    if (check_.valid()) {
        if (check_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        ProcessFeed(check_.get());
        return;
    }
    if (update_.valid()) return;  // an update is running; the UI shows its progress instead
    ProcessFeed(FetchFeed(feedUrl_));
}

void Controller::StartUpdate(std::string relaunchArgs) {
    if (update_.valid() || !feed_.ok) return;
    if (notice_.phase != Phase::Available) return;
    relaunchArgs_ = std::move(relaunchArgs);
    noticeHidden_ = false;
    cancel_.store(false, std::memory_order_relaxed);
    const Feed feed = feed_;
    notice_.phase = Phase::Downloading;
    notice_.version = feed.version;
    notice_.error.clear();
    notice_.stage = Stage::Download;
    notice_.fraction = 0;
    progressDone_.store(0, std::memory_order_relaxed);
    progressTotal_.store(feed.size, std::memory_order_relaxed);

    update_ = std::async(std::launch::async, [this, feed] {
        return UpdateWorker(feed);
    });
}

void Controller::StartFeedUpdate(const Feed& f, std::string relaunchArgs) {
    if (update_.valid()) return;
    if (!f.ok || !IsNewer(f.version, currentVersion_)) return;
    relaunchArgs_ = std::move(relaunchArgs);
    noticeHidden_ = false;
    cancel_.store(false, std::memory_order_relaxed);
    feed_ = f;
    notice_.phase = Phase::Downloading;
    notice_.version = f.version;
    notice_.error.clear();
    notice_.stage = Stage::Download;
    notice_.fraction = 0;
    progressDone_.store(0, std::memory_order_relaxed);
    progressTotal_.store(f.size, std::memory_order_relaxed);
    update_ = std::async(std::launch::async, [this, f] {
        return UpdateWorker(f);
    });
}

// The download/verify/extract worker: "" = staged (applier spawned), "cancelled", else the error.
std::string Controller::UpdateWorker(Feed feed) {
    const fs::path root = UpdateRoot();
    std::error_code ec;
    fs::remove_all(root, ec);  // stale staging from an earlier attempt
    const fs::path zip = root / (L"MMDX12-" + Utf8ToWide(feed.version) + L".zip");

    // 1. download
    std::string error;
    if (!DownloadToFile(feed.url, zip, &progressDone_, &cancel_, error)) {
        if (error != "cancelled") LOG_WARN("update: download failed: %s", error.c_str());
        return error;
    }
    if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");

    // 2. verify size + SHA-256 (streamed; the progress strip switches to "verifying")
    noticeStage_.store(1, std::memory_order_relaxed);
    progressTotal_.store(fs::file_size(zip, ec), std::memory_order_relaxed);
    progressDone_.store(0, std::memory_order_relaxed);
    const std::string digest = Sha256OfFile(zip, &progressDone_);
    if (digest.empty()) {
        LOG_WARN("update: cannot hash the download");
        return std::string("cannot hash the download");
    }
    if (!feed.sha256.empty() && digest != feed.sha256) {
        LOG_WARN("update: checksum mismatch (%s...)", digest.substr(0, 12).c_str());
        return "checksum mismatch (" + digest.substr(0, 12) + "...)";
    }
    if (!feed.sha256.empty()) LOG_INFO("update: sha256 ok (%s...)", digest.substr(0, 12).c_str());
    if (feed.size > 0 && fs::file_size(zip, ec) != feed.size) {
        LOG_WARN("update: size mismatch (%llu/%llu)", (unsigned long long)fs::file_size(zip, ec),
                 (unsigned long long)feed.size);
        return std::string("size mismatch");
    }

    // 3. extract
    if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
    noticeStage_.store(2, std::memory_order_relaxed);
    if (!ExtractZip(zip, ExtractDir(), error)) {
        LOG_WARN("update: extraction failed: %s", error.c_str());
        return error;
    }
    const fs::path stagedRoot = FindStagedRoot(ExtractDir());
    if (stagedRoot.empty() || !FileExists(stagedRoot / L"MMDX12.exe")) {
        LOG_WARN("update: the zip has no MMDX12.exe");
        return std::string("the zip has no MMDX12.exe");
    }

    // 4. journal + spawn the applier (the running exe cannot overwrite itself)
    if (!JournalWrite("pending", feed.version, CurrentPid(), relaunchArgs_)) {
        LOG_WARN("update: cannot write update_tmp/state.json");
        return std::string("cannot write update_tmp/state.json");
    }
    std::wstring cmd = L"\"" + (ExecutableDir() / L"MMDX12.exe").wstring() +
                       L"\" --apply-update --apply-wait " + std::to_wstring(CurrentPid());
    LOG_INFO("update: %s staged, spawning the applier", feed.version.c_str());
    if (!SpawnDetached(cmd)) {
        std::error_code ec2;
        fs::remove_all(root, ec2);  // no stale swap may wait at the next start
        LOG_WARN("update: cannot start the updater helper");
        return std::string("cannot start the updater helper");
    }
    return std::string();
}

void Controller::CancelUpdate() {
    if (update_.valid()) {
        cancel_.store(true, std::memory_order_relaxed);
        update_.wait();
        update_ = {};
    }
    cancel_.store(false, std::memory_order_relaxed);
    std::error_code ec;
    fs::remove_all(UpdateRoot(), ec);
    notice_ = State{};
    notice_.phase = feed_.ok ? Phase::Available : Phase::Idle;
    if (feed_.ok) notice_.version = feed_.version;
    LOG_INFO("update: cancelled");
}

void Controller::OpenReleasePage() const {
    const std::string url = notice_.page.empty() ? "https://mmdx.codingbot.kr/releases/" : notice_.page;
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
        ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const Controller::State& Controller::Get() {
    if (check_.valid() && check_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        ProcessFeed(check_.get());

    if (update_.valid()) {
        if (update_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string error = update_.get();
            const Stage stage = (Stage)noticeStage_.load(std::memory_order_relaxed);
            if (error.empty()) {
                notice_.phase = Phase::Staged;
                notice_.fraction = 1;
            } else if (error == "cancelled") {
                notice_ = State{};
                notice_.phase = feed_.ok ? Phase::Available : Phase::Idle;
                if (feed_.ok) notice_.version = feed_.version;
                LOG_INFO("update: cancelled");
            } else {
                notice_ = State{};
                notice_.phase = Phase::Failed;
                notice_.version = feed_.version;
                notice_.note = feed_.ok && !feed_.notes.empty() ? feed_.notes.begin()->second : std::string();
                notice_.error = error;
                notice_.stage = stage;
            }
        } else if (notice_.phase == Phase::Downloading) {
            notice_.stage = (Stage)noticeStage_.load(std::memory_order_relaxed);
            const uint64_t done = progressDone_.load(std::memory_order_relaxed);
            const uint64_t total = progressTotal_.load(std::memory_order_relaxed);
            notice_.doneMb = (double)done / (1024.0 * 1024.0);
            notice_.totalMb = (double)total / (1024.0 * 1024.0);
            notice_.fraction = total > 0 ? (float)std::clamp((double)done / (double)total, 0.0, 1.0) : 0.0f;
        }
    }
    return notice_;
}

} // namespace mmdx::updater
