#pragma once
// In-app auto-update (Windows): the app checks a version feed in the background, and one click
// stages + installs a portable zip of the next version across a restart. The zip is the one
// tools\package_release.ps1 makes: one top folder MMDX12-<version>-win64/ holding MMDX12.exe,
// DLLs, shaders/, assets/, licenses/ and a library/ template.
//
// Feed: GET https://mmdx.codingbot.kr/latest.json ->
//   { "version": "1.0.1", "url": "<zip>", "size": 1, "sha256": "<hex>", "page": "<releases>",
//     "manifest": "<manifest url>", "manifestSha256": "<hex>",
//     "notes": { "ko": "...", "en": "...", ... } }
// `url` may be https/http, a file: URL or a local path (testing); `size` and `sha256` are
// verified before the zip is trusted. `notes` falls back to "en", then the first entry.
// `manifest` / `manifestSha256` are optional: when present and valid the download uses the delta
// path (only changed/new files), otherwise it falls back to the full zip. Old apps ignore them.
//
// Delta path: the manifest lists every program file of the new version (path under the install
// root, size, SHA-256) and an object store base url. The app hashes its own matching-size files,
// hard-links the unchanged ones into the staged tree, and downloads only the changed/new objects
// (verifying each SHA-256). Files the install has but the manifest lacks vanish with the swap (the
// top-level entries are replaced wholesale, as in the zip path). Any delta failure except cancel
// logs a [W] and continues with the full zip in the same run.
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
#include <vector>

#include "app/Components.h"

namespace mmdx::updater {

// The channel is fixed at build time: a stable install never sees a nightly and the other way round. Moving between
// them means another install folder, so no downgrade path exists to get wrong.
#ifdef MMDX12_NIGHTLY
constexpr bool kNightlyBuild = true;
constexpr const char* kDefaultFeedUrl = "https://mmdx.codingbot.kr/updates/nightly/nightly.json";
// The public half of the nightly signing key (modulus, core/FeedSignature.h). Its private half is only a secret of the
// workflow's publish job, never on the update server: a taken-over server cannot make this build install anything.
constexpr const char* kFeedPublicKey =
    "tG/O+zvNyVa1IT5RT9IFx/G+pq2sfp2rfzWCRMH9mWZBOCOO4HmyD3YpOt+PifdSuNOJBG2N1BUVIBh0w45V846goGrRDDSHx9hcNjNI6g8+DGYj"
    "REXU3PSfyK7bhWfxOff3wpQW+5AgkhAwCIYh9UdZk5loudvgnp7KCfyen+Mc/U379MQRpUHu7Vpj8NdU7xO1WlA3QDSBt4R7U1S9HyZS6oRQgAXs"
    "vIFDbOgLLsflH0QRBYbsizGpncIxaQjX1jghJ1weD0SMAZC7imHjDWGp6aFDm4wauoE1+mZLlYEtO5ZvyjpTccPwWy2cz4v/jETuyYqnwrsdA/oz"
    "J+SZG4OFOHZP63j314QkdiDSeZ1eENvmKGGA/T6RwmFv0YSGgraByYaWzxdLyOix3/NpHvEJ7I4frAB/Alb4AkRhNm2uq9rRJvaR0en/qbiqfikZ"
    "ebGpVf2f+7w52SYUUbD5nvzqM7gzrNmQ1LobTn3KBLoll3l7yCM1G1muuyGrMgY7";
#else
constexpr bool kNightlyBuild = false;
constexpr const char* kDefaultFeedUrl = "https://mmdx.codingbot.kr/latest.json";
constexpr const char* kFeedPublicKey = nullptr;  // stable feeds are not signed (yet): the feed's hashes are all it has
#endif
constexpr const char* kUpdateDirName = "update_tmp";  // staging + journal, next to the exe

struct Feed {
    bool ok = false;
    std::string version;   // "1.0.1"
    std::string url;       // zip URL (https / http / file: / local path)
    std::string page;      // release page (offered when the install folder is not writable)
    std::string sha256;    // lowercase hex of the zip
    uint64_t size = 0;     // zip bytes (0: not checked)
    // Optional delta fields: a per-file manifest (see Manifest below). Old apps ignore them; when
    // both are present and valid the update only downloads the files whose SHA-256 changed.
    std::string manifest;         // manifest URL (https / http / file: / local path)
    std::string manifestSha256;   // lowercase hex of the manifest file
    std::map<std::string, std::string> notes;  // language code -> short plain text
    std::string error;     // !ok: human readable
};

// The per-file manifest (UTF-8 JSON) — the cross-version contract:
//   { "format": 1, "version": "1.9.2", "objects": "objects/",
//     "files": [ { "path": "shaders/mmd.hlsl", "size": 12345, "sha256": "<lowercase hex>" }, ... ] }
// `path` is forward-slash, relative to the install root (the zip's top folder content), no
// library/. `objects` is the base URL of the content-addressed store, absolute or relative to
// the manifest URL; an object lives at <objects><sha[0:2]>/<sha> and holds the raw bytes.
struct ManifestFile {
    std::string path;    // forward slashes, relative to the install root
    uint64_t size = 0;
    std::string sha256;  // lowercase hex
};
struct Manifest {
    int format = 0;
    std::string version;
    std::string objects;  // object store base URL (as written in the manifest)
    std::vector<ManifestFile> files;
};
// Parse manifest JSON. True with `out` filled; false + error otherwise.
bool ParseManifest(const std::string& json, Manifest& out, std::string& error);

// --- optional components (Components.h) -----------------------------------------------------
// A lite install (tools/package_release.ps1 -lite) lacks the upscaler DLLs and the Noto CJK fonts.
// This fetches one component on demand from the update object store, using the manifest of the
// INSTALLED version so the bytes always match this exe. It reuses the feed / manifest / object
// helpers above; the manifest URL is derived from the feed's `manifest` (same folder,
// "<current version>.json"). SHA-256 per file is verified; downloads land in a .part file and are
// renamed into place. One background job at a time, polled from the UI thread (no callbacks).
class ComponentFetcher {
public:
    ComponentFetcher() = default;
    ~ComponentFetcher();

    void Configure(std::string feedUrl, std::string currentVersion);  // once, at App start

    enum class Phase { Idle, Running, Done, Failed };
    struct State {
        Phase phase = Phase::Idle;
        std::string id;      // Components.h id of the finished/running/failed component
        std::string error;   // Korean, for the UI (Failed)
        float fraction = 0;
        double doneMb = 0, totalMb = 0;
    };
    const State& Get();       // drains a finished worker; call every frame
    bool Busy() const { return job_.valid(); }
    void ClearDone();         // Done/Failed -> Idle (after the UI reacted)

    // Starts fetching component `comp` (a Components.h def). No-op while a job is running.
    void Start(const components::Def& comp);

private:
    std::string Worker(components::Def comp);
    State state_;
    std::string feedUrl_, currentVersion_;
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> progressDone_{0}, progressTotal_{0};
    std::future<std::string> job_;
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
    // Downloading sub-step for the progress label. In the delta path the order is Scan -> Download
    // -> Verify -> Extract; the full-zip path skips Scan.
    enum class Stage { Scan, Download, Verify, Extract };
    struct State {
        Phase phase = Phase::Idle;
        std::string version;             // feed version (Available / Downloading / Staged)
        std::string note;                // localized note (plain text; "" when the feed has none)
        std::string page;                // release page URL
        std::string error;               // Failed: human readable
        float fraction = 0;              // Downloading: 0..1
        double doneMb = 0, totalMb = 0;  // Downloading: progress in MB
        Stage stage = Stage::Download;   // Downloading: which label the progress strip shows
        // Delta path (Downloading): how many files changed and the full-zip size, for
        // "N files changed · X MB / Y MB".
        bool delta = false;
        int deltaFiles = 0;              // changed + new files to fetch
        double fullMb = 0;               // full zip size (MB) for the "of the whole" figure
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
    // Delta path (inside UpdateWorker): fetch + verify the manifest, build the staged tree from
    // hard links + changed objects. "" on success; "cancelled"; else an error to fall back on.
    std::string StageDelta(const Feed& feed, int& changedFiles, uint64_t& changedBytes);

    State notice_;
    Feed feed_;                         // latest good feed
    std::string feedUrl_, currentVersion_, relaunchArgs_;
    bool noticeHidden_ = false;
    std::atomic<int> noticeStage_{0};   // worker -> UI: Downloading sub-step (Stage as int)
    std::future<Feed> check_;
    std::future<std::string> update_;   // "" = staged, "cancelled" = aborted, else the error
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> progressDone_{0}, progressTotal_{0};
    std::atomic<int> deltaFiles_{0};    // worker -> UI: delta changed files (0 = full-zip path)
    std::atomic<uint64_t> deltaFullBytes_{0};  // worker -> UI: full zip size for the delta label
};

} // namespace mmdx::updater
