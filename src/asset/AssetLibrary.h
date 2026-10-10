#pragma once
// Scans a user library folder and classifies what it finds into characters, stages and
// songs (dance motion sets).
//
// Precedence, strongest first:
//   1. App overrides (LibraryOverrides: "use this file as a stage", "hide").
//   2. `mmdx.json` sidecars: a folder declares what it is and, optionally, which files to use.
//   3. Folder names: any folder called characters/models/..., stages/..., songs/motions/...
//      (several languages, see AssetLibrary.cpp) sets the type of the models below it.
//   4. Content: humanoid skeletons are characters, static scenes are stages, VMD sets are
//      songs (several songs in one folder are told apart by length and file names).
// So a tidy library needs no configuration, and a messy one still mostly works.
#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

struct CharacterAsset {
    std::string id;               // stable key: library-relative path of the model file, '/' separated, UTF-8
    std::string displayName;      // model name, or file stem if the name is empty
    std::string folderName;       // name of the directory containing the model (UTF-8)
    std::string format;           // "PMX", "glTF", "VRM", "FBX", "OBJ"
    std::filesystem::path modelPath;
    uint32_t vertexCount = 0, boneCount = 0, materialCount = 0;
};

struct StageAsset {
    std::string id;               // library-relative path of the directory (PMX parts) or of the model file
    std::string displayName;
    std::string format;
    std::vector<std::filesystem::path> parts;  // model files drawn together, sorted
    uint32_t vertexCount = 0;     // sum over parts
};

struct SongAsset {
    std::string id;               // library-relative path of the directory ("#<dance file>" appended when a folder holds several songs)
    std::string displayName;      // directory name (if dir is named "src", the parent's name), or the dance file's name
    std::filesystem::path danceVmd;                 // primary dance motion (bone keys > 0)
    std::vector<std::filesystem::path> extraVmds;   // facial/morph-only vmds of the same set
    std::filesystem::path cameraVmd;                // may be empty
    std::filesystem::path audioPath;                // may be empty (.wav/.mp3/.flac/.ogg)
    float durationSec = 0;        // dance maxFrame / 30
};

struct LibraryScanResult {
    std::filesystem::path root;
    std::vector<CharacterAsset> characters;  // sorted by displayName
    std::vector<StageAsset> stages;          // sorted by displayName
    std::vector<SongAsset> songs;            // sorted by displayName
    std::vector<std::string> warnings;       // e.g. "skipped broken.pmx: <error>"
    std::vector<std::string> notes;          // classification decisions worth knowing (skipped props, guessed matches)
    double scanSeconds = 0;
};

struct ScanProgress {
    std::atomic<int> filesVisited{0};
    std::atomic<int> filesTotal{0};   // 0 until the directory walk finishes
    std::atomic<bool> cancel{false};  // set by the owner to stop early (app exit): the result is then incomplete
};

// Per-file decisions made in the app, keyed by library-relative path ('/' separated).
enum class AssetKind { Auto, Character, Stage, Hidden };
struct LibraryOverrides {
    std::map<std::string, AssetKind> byPath;
};

// Synchronous; call from a worker thread. `progress` and `overrides` may be null.
LibraryScanResult ScanLibrary(const std::filesystem::path& root, ScanProgress* progress = nullptr,
                              const LibraryOverrides* overrides = nullptr);

// Copies the suggested folder layout (assets/library_template: characters/, stages/, songs/ with
// READMEs) into a missing or empty library folder (only README files count as empty).
// Returns true when it created something.
bool CreateLibrarySkeleton(const std::filesystem::path& root, const std::filesystem::path& templateDir);

} // namespace mmdx
