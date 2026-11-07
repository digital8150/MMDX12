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
#include "app/Components.h"
#include "core/FeedSignature.h"
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

bool IsSha256(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
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
    LogCloseFile();  // an open log cannot be renamed
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

// --- delta path ----------------------------------------------------------------------------

// Resolve an URL/path from the manifest against the manifest's own URL: an absolute URL / local
// path is used as-is, otherwise it is joined onto the manifest's directory (or URL prefix).
std::string ResolveRelative(const std::string& base, const std::string& ref) {
    if (ref.empty()) return {};
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0 || ref.rfind("file:", 0) == 0)
        return ref;
    // A Windows drive path (C:\... or C:/...) or a UNC path is absolute.
    if ((ref.size() >= 2 && ref[1] == ':') || ref.rfind("\\\\", 0) == 0 || ref[0] == '/' || ref[0] == '\\')
        return ref;
    // Split the base at the last '/' and append.
    const size_t slash = base.find_last_of("/\\");
    if (slash == std::string::npos) return ref;
    return base.substr(0, slash + 1) + ref;
}

// Join a base URL with a relative "aa/hash" object key, keeping a single slash.
std::string JoinUrl(const std::string& base, const std::string& rel) {
    std::string out = base;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    out += rel;
    return out;
}

// Verify `file` against `expectSha` (lowercase hex) and `expectSize` (0: not checked).
bool VerifyFile(const fs::path& file, uint64_t expectSize, const std::string& expectSha,
                std::atomic<uint64_t>* progress) {
    if (expectSize > 0) {
        std::error_code ec;
        if (fs::file_size(file, ec) != expectSize) return false;
    }
    if (expectSha.empty()) return true;
    return LowerAsciiOf(Sha256OfFile(file, progress)) == expectSha;
}

// Hard-link `src` to `dst`; fall back to a copy (different volumes / filesystems). False on error.
bool LinkOrCopy(const fs::path& src, const fs::path& dst, std::string& error) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (!CreateHardLinkW(dst.c_str(), src.c_str(), nullptr)) {
        ec.clear();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot link/copy " + PathToUtf8(src.filename()) + " (" + ec.message() + ")";
            return false;
        }
    }
    return true;
}

// Fetch a URL / local path as raw bytes into memory (manifest, or an object when small). The
// manifest is the only intended use; objects are downloaded to files. Returns false on error.
bool FetchBytes(const std::string& url, size_t maxBytes, std::string& out, std::string& error) {
    if (IsLocalSource(url)) {
        if (!ReadSmallFile(LocalPathOf(url), out)) {
            error = "cannot open " + url;
            return false;
        }
        if (out.size() > maxBytes) {
            error = "source too large";
            return false;
        }
        return true;
    }
    return HttpGet(url, out, error, maxBytes, net::HttpOption::AcceptGzip);
}

// Download one object (raw bytes) to `dest`, decompressing a gzip-encoded response. The object
// url as written is <base><sha[0:2]>/<sha>; a gzip_static sibling is the server's business, so
// no content-type sniffing here. The caller verifies the SHA-256.
bool DownloadObject(const std::string& url, const fs::path& dest, std::atomic<uint64_t>* done,
                    std::atomic<bool>* cancel, std::string& error) {
    return DownloadToFile(url, dest, done, cancel, error, net::HttpOption::AcceptGzip);
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
    // Only the signed bytes count from here on; nothing outside them is ever read.
    std::string payload;
    if (!net::OpenSignedFeed(body, kFeedPublicKey, payload, out.error)) return out;
    const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        out.error = "invalid feed response";
        return out;
    }
    out.version = Trimmed(j.value("version", std::string()));
    out.url = Trimmed(j.value("url", std::string()));
    out.page = Trimmed(j.value("page", std::string()));
    out.sha256 = LowerAsciiOf(Trimmed(j.value("sha256", std::string())));
    out.size = j.value("size", (uint64_t)0);
    out.manifest = Trimmed(j.value("manifest", std::string()));
    out.manifestSha256 = LowerAsciiOf(Trimmed(j.value("manifestSha256", std::string())));
    if (j.contains("manifests") && j["manifests"].is_object())
        for (auto it = j["manifests"].begin(); it != j["manifests"].end(); ++it)
            if (it.value().is_string()) out.manifests[it.key()] = LowerAsciiOf(Trimmed(it.value().get<std::string>()));
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
    // Every file the updater takes is checked against these two, so a feed without them promises nothing.
    if (!IsSha256(out.sha256)) {
        out.error = "feed has no zip hash";
        return out;
    }
    if (!out.manifest.empty() && !IsSha256(out.manifestSha256)) {
        out.error = "feed has no manifest hash";
        return out;
    }
    out.ok = true;
    return out;
}

// Parse the delta manifest JSON (see Updater.h for the contract). Paths are normalized to
// lowercase-forward-slash keys; the manifest itself is trusted only after its sha256 is checked
// by the caller.
bool ParseManifest(const std::string& json, Manifest& out, std::string& error) {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "invalid manifest JSON";
        return false;
    }
    out = Manifest{};
    out.format = j.value("format", 0);
    out.version = Trimmed(j.value("version", std::string()));
    out.objects = Trimmed(j.value("objects", std::string()));
    if (out.format != 1) {
        error = "unsupported manifest format " + std::to_string(out.format);
        return false;
    }
    if (out.version.empty()) {
        error = "manifest has no version";
        return false;
    }
    if (out.objects.empty()) {
        error = "manifest has no objects base";
        return false;
    }
    if (!j.contains("files") || !j["files"].is_array()) {
        error = "manifest has no files array";
        return false;
    }
    for (const auto& e : j["files"]) {
        if (!e.is_object()) continue;
        ManifestFile f;
        f.path = e.value("path", std::string());
        f.size = e.value("size", (uint64_t)0);
        f.sha256 = LowerAsciiOf(Trimmed(e.value("sha256", std::string())));
        f.path = Trimmed(f.path);
        if (f.path.empty()) continue;
        for (char& c : f.path)
            if (c == '\\') c = '/';
        if (!IsSha256(f.sha256)) {
            error = "manifest file has a bad sha256: " + f.path;
            return false;
        }
        out.files.push_back(std::move(f));
    }
    if (out.files.empty()) {
        error = "manifest has no files";
        return false;
    }
    return true;
}

// The applier: flow step 2 (no window, no UI). Runs when the updating app spawned
// MMDX12.exe --apply-update --apply-wait <pid>, or at the next start after an interrupted
// swap. Returns the process exit code.
int ApplyPendingUpdateAndRelaunch(uint32_t waitPid, bool fromCli) {
    (void)fromCli;
    // Continue the updating app's log (a fresh one would wipe its download lines before
    // ArchiveLog keeps them as mmdx12.previous.log).
    LogAppendToExisting();
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
    WriteSmallFile(UpdateRoot() / L"applied.txt", j.version);  // breadcrumb for the next start
    std::error_code ec;
    fs::remove(stateFile, ec);
    LOG_INFO("update: %s installed - restarting", j.version.c_str());
    ArchiveLog();  // last: closes our log so the rename can succeed
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

// Delta path: fetch + verify the manifest, scan the local install, build the staged tree from
// hard links of unchanged files + downloads of the changed/new objects. "" on success,
// "cancelled", else a human error (the caller falls back to the full zip).
std::string Controller::StageDelta(const Feed& feed, int& changedFiles, uint64_t& changedBytes) {
    const fs::path root = UpdateRoot();
    const fs::path extract = ExtractDir();
    std::string error;

    // 1. fetch the manifest (verify its sha256 before trusting it).
    std::string body;
    if (!FetchBytes(feed.manifest, 1u << 24, body, error)) {
        LOG_WARN("update: delta manifest fetch failed: %s", error.c_str());
        return "manifest fetch failed (" + error + ")";
    }
    const fs::path manifestFile = root / L"manifest.json";
    if (!WriteSmallFile(manifestFile, body)) return std::string("cannot write the manifest");
    const std::string got = LowerAsciiOf(Sha256OfFile(manifestFile, nullptr));
    if (got != feed.manifestSha256) {
        LOG_WARN("update: manifest sha256 mismatch (%s...)", got.substr(0, 12).c_str());
        return "manifest checksum mismatch";
    }
    Manifest m;
    if (!ParseManifest(body, m, error)) {
        LOG_WARN("update: bad manifest: %s", error.c_str());
        return "bad manifest (" + error + ")";
    }
    if (m.version != feed.version)
        LOG_WARN("update: manifest version %s differs from feed %s (using the feed version)",
                 m.version.c_str(), feed.version.c_str());
    const std::string objects = ResolveRelative(feed.manifest, m.objects);
    if (objects.empty()) return std::string("manifest has no objects base");

    // 2. scan: hash every local file the manifest lists; unchanged -> hard-link, else download.
    noticeStage_.store((int)Stage::Scan, std::memory_order_relaxed);
    progressDone_.store(0, std::memory_order_relaxed);
    uint64_t manifestBytes = 0;
    for (const ManifestFile& f : m.files) manifestBytes += f.size;
    progressTotal_.store(manifestBytes, std::memory_order_relaxed);

    std::vector<size_t> toDownload;
    std::vector<bool> changedFlags(m.files.size(), false);
    uint64_t scanned = 0;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const ManifestFile& f = m.files[i];
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
        const fs::path live = ExecutableDir() / Utf8ToPath(f.path);
        bool same = false;
        if (FileExists(live)) {
            std::error_code fec;
            if (fs::file_size(live, fec) == f.size) {
                const std::string digest = LowerAsciiOf(Sha256OfFile(live, nullptr));
                same = digest == f.sha256;
            }
        }
        if (!same) {
            // An optional component (Components.h) that is not installed stays uninstalled: a lite
            // install must not silently gain the upscaler DLLs / CJK fonts through a delta update.
            // Installed components keep being updated like any other file.
            if (!FileExists(live) && components::IsComponentPath(f.path)) {
                scanned += f.size;
                progressDone_.store(scanned, std::memory_order_relaxed);
                continue;
            }
            toDownload.push_back(i);
            changedFlags[i] = true;
            changedBytes += f.size;
        }
        scanned += f.size;
        progressDone_.store(scanned, std::memory_order_relaxed);
    }
    changedFiles = (int)toDownload.size();
    deltaFiles_.store(changedFiles, std::memory_order_relaxed);
    deltaFullBytes_.store(feed.size, std::memory_order_relaxed);
    LOG_INFO("update: delta %d files changed, %.1f MB to download (full zip %.1f MB)", changedFiles,
             (double)changedBytes / (1024.0 * 1024.0), (double)feed.size / (1024.0 * 1024.0));
    if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");

    // 3. build the staged tree: hard-link the unchanged files, download the changed/new objects.
    const fs::path top = extract / (L"MMDX12-" + Utf8ToWide(feed.version) + L"-win64");
    std::error_code ec;
    fs::remove_all(extract, ec);
    fs::create_directories(top, ec);
    for (size_t i = 0; i < m.files.size(); ++i) {
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
        const ManifestFile& f = m.files[i];
        const fs::path rel = Utf8ToPath(f.path);
        if (f.path.empty() || f.path[0] == '/' || f.path.find("..") != std::string::npos ||
            f.path.find(':') != std::string::npos) {
            LOG_WARN("update: manifest has an unsafe path: %s", f.path.c_str());
            return "manifest has an unsafe path";
        }
        const fs::path dst = top / rel;
        if (!changedFlags[i]) {
            const fs::path src = ExecutableDir() / rel;
            // An absent optional component (Components.h) that was not installed stays absent from
            // the staged tree: skip the link (there is nothing to link) so a lite install stays lite.
            if (!FileExists(src) && components::IsComponentPath(f.path)) continue;
            if (!LinkOrCopy(src, dst, error)) {
                LOG_WARN("update: delta link failed: %s", error.c_str());
                return "delta link failed (" + error + ")";
            }
        }
    }
    // Objects after linking: download progress measures only the changed bytes.
    noticeStage_.store((int)Stage::Download, std::memory_order_relaxed);
    progressDone_.store(0, std::memory_order_relaxed);
    progressTotal_.store(changedBytes, std::memory_order_relaxed);
    for (size_t i = 0; i < m.files.size(); ++i) {
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
        if (!changedFlags[i]) continue;
        const ManifestFile& f = m.files[i];
        const std::string key = f.sha256.substr(0, 2) + "/" + f.sha256;
        const std::string url = JoinUrl(objects, key);
        const fs::path tmp = root / L"objects" / Utf8ToWide(f.sha256.substr(0, 2)) / Utf8ToWide(f.sha256);
        std::string derr;
        if (!DownloadObject(url, tmp, &progressDone_, &cancel_, derr)) {
            if (derr == "cancelled") return derr;
            LOG_WARN("update: delta object failed for %s: %s", f.path.c_str(), derr.c_str());
            return "object download failed for " + f.path + " (" + derr + ")";
        }
        if (!VerifyFile(tmp, f.size, f.sha256, nullptr)) {
            LOG_WARN("update: delta object hash mismatch for %s", f.path.c_str());
            return "object hash mismatch for " + f.path;
        }
        const fs::path dst = top / Utf8ToPath(f.path);
        fs::create_directories(dst.parent_path(), ec);
        fs::remove(dst, ec);
        fs::rename(tmp, dst, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(tmp, dst, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
            if (ec) return "cannot place " + f.path;
        }
    }
    if (!FileExists(top / L"MMDX12.exe")) {
        LOG_WARN("update: the manifest has no MMDX12.exe");
        return std::string("the manifest has no MMDX12.exe");
    }
    return std::string();
}

// The download/verify/extract worker: "" = staged (applier spawned), "cancelled", else the error.
std::string Controller::UpdateWorker(Feed feed) {
    const fs::path root = UpdateRoot();
    std::error_code ec;
    fs::remove_all(root, ec);  // stale staging from an earlier attempt
    noticeStage_.store((int)Stage::Download, std::memory_order_relaxed);
    deltaFiles_.store(0, std::memory_order_relaxed);
    deltaFullBytes_.store(feed.size, std::memory_order_relaxed);

    // Delta path first: when the feed carries a manifest, build the staged tree from it. Any
    // failure except cancel falls back to the full zip in this same run.
    bool delta = false;
    if (!feed.manifest.empty()) {
        int changedFiles = 0;
        uint64_t changedBytes = 0;
        const std::string derr = StageDelta(feed, changedFiles, changedBytes);
        if (derr == "cancelled") return derr;
        if (derr.empty()) {
            delta = true;
            LOG_INFO("update: delta staged");
        } else {
            LOG_WARN("update: delta failed (%s), falling back to the full zip", derr.c_str());
            fs::remove_all(root, ec);
            deltaFiles_.store(0, std::memory_order_relaxed);
            noticeStage_.store((int)Stage::Download, std::memory_order_relaxed);
            progressDone_.store(0, std::memory_order_relaxed);
            progressTotal_.store(feed.size, std::memory_order_relaxed);
        }
    }

    if (!delta) {
        const fs::path zip = root / (L"MMDX12-" + Utf8ToWide(feed.version) + L".zip");
        // 1. download
        std::string error;
        if (!DownloadToFile(feed.url, zip, &progressDone_, &cancel_, error)) {
            if (error != "cancelled") LOG_WARN("update: download failed: %s", error.c_str());
            return error;
        }
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");

        // 2. verify size + SHA-256 (streamed; the progress strip switches to "verifying")
        noticeStage_.store((int)Stage::Verify, std::memory_order_relaxed);
        progressTotal_.store(fs::file_size(zip, ec), std::memory_order_relaxed);
        progressDone_.store(0, std::memory_order_relaxed);
        const std::string digest = Sha256OfFile(zip, &progressDone_);
        if (digest.empty()) {
            LOG_WARN("update: cannot hash the download");
            return std::string("cannot hash the download");
        }
        if (digest != feed.sha256) {
            LOG_WARN("update: checksum mismatch (%s...)", digest.substr(0, 12).c_str());
            return "checksum mismatch (" + digest.substr(0, 12) + "...)";
        }
        LOG_INFO("update: sha256 ok (%s...)", digest.substr(0, 12).c_str());
        if (feed.size > 0 && fs::file_size(zip, ec) != feed.size) {
            LOG_WARN("update: size mismatch (%llu/%llu)", (unsigned long long)fs::file_size(zip, ec),
                     (unsigned long long)feed.size);
            return std::string("size mismatch");
        }

        // 3. extract
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
        noticeStage_.store((int)Stage::Extract, std::memory_order_relaxed);
        std::string error2;
        if (!ExtractZip(zip, ExtractDir(), error2)) {
            LOG_WARN("update: extraction failed: %s", error2.c_str());
            return error2;
        }
    }

    const fs::path stagedRoot = FindStagedRoot(ExtractDir());
    if (stagedRoot.empty() || !FileExists(stagedRoot / L"MMDX12.exe")) {
        LOG_WARN("update: the staged tree has no MMDX12.exe");
        return std::string("the staged tree has no MMDX12.exe");
    }

    // journal + spawn the applier (the running exe cannot overwrite itself)
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
            notice_.deltaFiles = deltaFiles_.load(std::memory_order_relaxed);
            notice_.delta = notice_.deltaFiles > 0;
            notice_.fullMb = (double)deltaFullBytes_.load(std::memory_order_relaxed) / (1024.0 * 1024.0);
        }
    }
    return notice_;
}

// --- ComponentFetcher (Components.h) -------------------------------------------------------

ComponentFetcher::~ComponentFetcher() {
    cancel_.store(true, std::memory_order_relaxed);
    if (job_.valid()) job_.wait();
}

void ComponentFetcher::Configure(std::string feedUrl, std::string currentVersion) {
    feedUrl_ = std::move(feedUrl);
    currentVersion_ = std::move(currentVersion);
}

void ComponentFetcher::Start(const components::Def& comp) {
    if (job_.valid()) return;
    cancel_.store(false, std::memory_order_relaxed);
    state_ = State{};
    state_.phase = Phase::Running;
    state_.id = comp.id;
    progressDone_.store(0, std::memory_order_relaxed);
    progressTotal_.store(0, std::memory_order_relaxed);
    job_ = std::async(std::launch::async, [this, comp] { return Worker(comp); });
    LOG_INFO("component: fetching %s", comp.id);
}

void ComponentFetcher::ClearDone() {
    if (state_.phase == Phase::Done || state_.phase == Phase::Failed) state_ = State{};
}

const ComponentFetcher::State& ComponentFetcher::Get() {
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::string error = job_.get();
        const std::string id = state_.id;
        state_ = State{};
        state_.id = id;
        if (error.empty()) {
            state_.phase = Phase::Done;
            state_.fraction = 1;
            LOG_INFO("component: %s installed", id.c_str());
        } else if (error == "cancelled") {
            state_.phase = Phase::Idle;
            LOG_INFO("component: cancelled");
        } else {
            state_.phase = Phase::Failed;
            state_.error = error;
            LOG_WARN("component: %s failed: %s", id.c_str(), error.c_str());
        }
    } else if (job_.valid() && state_.phase == Phase::Running) {
        const uint64_t done = progressDone_.load(std::memory_order_relaxed);
        const uint64_t total = progressTotal_.load(std::memory_order_relaxed);
        state_.doneMb = (double)done / (1024.0 * 1024.0);
        state_.totalMb = (double)total / (1024.0 * 1024.0);
        state_.fraction = total > 0 ? (float)std::clamp((double)done / (double)total, 0.0, 1.0) : 0.0f;
    }
    return state_;
}

std::string ComponentFetcher::Worker(components::Def comp) {
    // A nightly's zip holds every component, and its feed names no older manifests.
    if (kNightlyBuild) return std::string("not available in this build");

    // 1. the signed feed: where the manifests live, and the hash of this version's.
    const Feed feed = FetchFeed(feedUrl_);
    if (!feed.ok) return "feed: " + feed.error;
    if (feed.manifest.empty()) return std::string("feed has no manifest url");
    const auto manifestHash = feed.manifests.find(currentVersion_);
    if (manifestHash == feed.manifests.end()) return std::string("feed has no manifest hash for this version");

    // 2. the INSTALLED version's manifest: same folder as the feed's manifest, "<current version>.json".
    //    Never the new version's manifest: the upscaler DLLs / fonts must match this exe.
    const std::string manifestUrl = ResolveRelative(feed.manifest, currentVersion_ + ".json");
    std::string body, error;
    if (!FetchBytes(manifestUrl, 1u << 24, body, error)) return "manifest: " + error;
    if (net::Sha256OfBytes(body) != manifestHash->second) return std::string("manifest: checksum mismatch");
    Manifest m;
    if (!ParseManifest(body, m, error)) return "manifest: " + error;
    if (m.version != currentVersion_) return "manifest: for version " + m.version + ", not this build";
    const std::string objects = ResolveRelative(manifestUrl, m.objects);
    if (objects.empty()) return std::string("manifest has no objects base");

    // 3. resolve every file of the component in the manifest and sum its size (progress).
    struct Item {
        const ManifestFile* file = nullptr;
        fs::path dest;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    for (int i = 0; i < comp.fileCount; ++i) {
        const std::string path = comp.files[i];
        const ManifestFile* found = nullptr;
        for (const ManifestFile& f : m.files)
            if (f.path == path) {
                found = &f;
                break;
            }
        if (!found) return std::string("component file not in the manifest: ") + path;
        items.push_back({found, ExecutableDir() / Utf8ToPath(path)});
        total += found->size;
    }
    progressTotal_.store(total, std::memory_order_relaxed);
    progressDone_.store(0, std::memory_order_relaxed);

    // 4. download + verify each file into a sibling .part, then rename it into place.
    for (const Item& it : items) {
        if (cancel_.load(std::memory_order_relaxed)) return std::string("cancelled");
        const ManifestFile& f = *it.file;
        const fs::path tmp = it.dest.parent_path() / (it.dest.filename().wstring() + L".component.part");
        std::error_code ec;
        fs::create_directories(tmp.parent_path(), ec);
        const std::string key = f.sha256.substr(0, 2) + "/" + f.sha256;
        const std::string url = JoinUrl(objects, key);
        std::string derr;
        if (!DownloadObject(url, tmp, &progressDone_, &cancel_, derr)) {
            fs::remove(tmp, ec);
            if (derr == "cancelled") return derr;
            return "download failed for " + f.path + " (" + derr + ")";
        }
        if (!VerifyFile(tmp, f.size, f.sha256, nullptr)) {
            fs::remove(tmp, ec);
            return "checksum mismatch for " + f.path;
        }
        fs::remove(it.dest, ec);
        fs::rename(tmp, it.dest, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(tmp, it.dest, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
            if (ec) return "cannot place " + f.path;
        }
    }
    return std::string();
}

} // namespace mmdx::updater
