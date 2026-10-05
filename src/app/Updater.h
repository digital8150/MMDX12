#pragma once
// In-app auto-update (Windows): the app checks a version feed in the background, and one click
// stages + installs a portable zip of the next version across a restart. The zip is the one
// tools\package_release.ps1 makes: one top folder MMDX12-<version>-win64/ holding MMDX12.exe,
// DLLs, shaders/, assets/, licenses/ and a library/ template.
//
// Feed: GET https://mmdx.codingbot.kr/latest.json ->
//   { "version": "1.0.1", "url": "<zip>", "size": 1, "sha256": "<hex>", "page": "<releases>",
//     "notes": { "ko": "...", "en": "...", ... } }
// `url` may be https/http, a file: URL or a local path (testing); `size` and `sha256` are
// verified before the zip is trusted. `notes` falls back to "en", then the first entry.
//
// The flow spans three processes and is designed so a failure at any point leaves a working
// install (the running exe cannot overwrite itself, so the swap is rename-aside + move):
//   1. App (any version): background check (silent, logged; skipped in headless runs) -> the
//      select screen shows a notice (UiUpdate.cpp). "Update": download -> verify -> extract into
//      <exe>/update_tmp/extract/ -> write update_tmp/state.json (phase "pending") -> spawn
//      "<exe>/MMDX12.exe --apply-update --apply-wait <pid>" -> exit. Nothing else is touched.
//   2. Applier (same exe, no window; --apply-update or a leftover "pending" state at startup):
//      waits for the old process, renames every collided program file aside into
//      update_tmp/old/ (loaded DLLs rename fine) and moves the extracted tree into place. The
//      user's library/ is skipped. A failure mid-swap rolls the renames back and relaunches the
//      old exe; an interrupted swap (state phase "swapping") is applied by the next start.
//      On success it spawns the new exe with the staged relaunch args and leaves
//      update_tmp/applied.txt.
//   3. Next App start: CleanupUpdateLeftovers logs the applied breadcrumb and removes
//      update_tmp. The user's library/, mmdx12.ini, recovery/ and logs are never inside it.
// An install folder that is not writable is detected before anything is downloaded
// (Controller::Phase::NotWritable; the release page is offered instead).

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <string>

namespace mmdx::updater {

constexpr const char* kDefaultFeedUrl = "https://mmdx.codingbot.kr/latest.json";
constexpr const char* kUpdateDirName = "update_tmp";  // staging + journal, next to the exe

struct Feed {
    bool ok = false;
    std::string version;   // "1.0.1"
    std::string url;       // zip URL (https / http / file: / local path)
    std::string page;      // release page (offered when the install folder is not writable)
    std::string sha256;    // lowercase hex of the zip
    uint64_t size = 0;     // zip bytes (0: not checked)
    std::map<std::string, std::string> notes;  // language code -> short plain text
    std::string error;     // !ok: human readable
};

// ".-component" semantic compare ("1.2.10" > "1.2.9"): negative / 0 / positive.
int CompareVersions(const std::string& a, const std::string& b);
inline bool IsNewer(const std::string& candidate, const std::string& current) {
    return CompareVersions(candidate, current) > 0;
}

// GET the feed: https URL, file: URL or a local path (testing). Synchronous; worker thread only.
Feed FetchFeed(const std::string& feedUrlOrPath);

// True while a staged/started swap is waiting to be applied (small file check; main.cpp applies
// it before any window exists).
bool HasPendingUpdate();
// Version breadcrumb left by the applier in update_tmp/applied.txt (empty when none).
std::string AppliedVersionBreadcrumb();
// Next start: log the breadcrumb and remove update_tmp (no-op while a swap is pending, and it
// repairs the exe from update_tmp/old if a swap died after renaming it aside).
void CleanupUpdateLeftovers();

// Flow step 2: complete the swap and spawn the new exe (no UI). Returns the process exit code:
// 0 applied, 1201 nothing to apply, 1202 swap failed (rolled back, old exe relaunched),
// 1203 swap failed and the rollback could not restore the exe.
int ApplyPendingUpdateAndRelaunch(uint32_t waitPid, bool fromCli);

// Stage + spawn the applier now (App side, after Staged). False: spawn failed (the staged files
// are cleaned up so no stale swap waits at the next start).
bool SpawnUpdateApplier();

// App-side controller: one background job at a time, polled from the UI thread (no callbacks).
class Controller {
public:
    Controller() = default;
    ~Controller();

    enum class Phase { Idle, Checking, UpToDate, Available, Downloading, NotWritable, Failed, Staged };
    enum class Stage { Download, Verify, Extract };  // Downloading sub-step for the progress label
    struct State {
        Phase phase = Phase::Idle;
        std::string version;             // feed version (Available / Downloading / Staged)
        std::string note;                // localized note (plain text; "" when the feed has none)
        std::string page;                // release page URL
        std::string error;               // Failed: human readable
        float fraction = 0;              // Downloading: 0..1
        double doneMb = 0, totalMb = 0;  // Downloading: progress in MB
        Stage stage = Stage::Download;   // Downloading: which label the progress strip shows
    };

    void Configure(std::string feedUrl, std::string currentVersion);  // once, at App start
    void StartCheck();                  // background check (skipped in headless runs by the App)
    void CheckNow();                    // synchronous check (ui-script testing)
    void StartUpdate(std::string relaunchArgs);  // Available -> Downloading (stages, then applies)
    // fetch + stage in one step (ui-script "updateinstall"): sets the feed, then StartUpdate
    void StartFeedUpdate(const Feed& f, std::string relaunchArgs);
    void CancelUpdate();                // Downloading: abort, clean staging, back to Available
    void HideNotice() { noticeHidden_ = true; }  // "Later": hidden until the next start
    bool NoticeVisible() const { return !noticeHidden_; }  // false after "Later"; Check resets it
    void OpenReleasePage() const;       // shell-open the feed's release page

    const State& Get();                 // drains finished workers; call every frame

private:
    void ProcessFeed(const Feed& f);    // shared tail of StartCheck / CheckNow
    std::string UpdateWorker(Feed f);   // download -> verify -> extract -> journal + applier

    State notice_;
    Feed feed_;                         // latest good feed
    std::string feedUrl_, currentVersion_, relaunchArgs_;
    bool noticeHidden_ = false;
    std::atomic<int> noticeStage_{0};   // worker -> UI: Downloading sub-step (Stage as int)
    std::future<Feed> check_;
    std::future<std::string> update_;   // "" = staged, "cancelled" = aborted, else the error
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> progressDone_{0}, progressTotal_{0};
};

} // namespace mmdx::updater
