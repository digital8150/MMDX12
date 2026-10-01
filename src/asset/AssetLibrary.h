#pragma once
// Scans a user library folder and classifies what it finds into characters, stages and
// songs (dance motion sets). Classification is content-based, so users can drop
// downloaded archives in any folder layout.
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

struct CharacterAsset {
    std::string id;               // stable key: library-relative path of the .pmx, '/' separated, UTF-8
    std::string displayName;      // PMX model name, or file stem if the name is empty
    std::string folderName;       // name of the directory containing the pmx (UTF-8)
    std::filesystem::path pmxPath;
    uint32_t vertexCount = 0, boneCount = 0, materialCount = 0;
};

struct StageAsset {
    std::string id;               // library-relative path of the directory, '/' separated
    std::string displayName;      // directory name
    std::vector<std::filesystem::path> pmxParts;  // all distinct stage pmx files in that dir, sorted
    uint32_t vertexCount = 0;     // sum over parts
};

struct SongAsset {
    std::string id;               // library-relative path of the directory, '/' separated
    std::string displayName;      // directory name (if dir is named "src", the parent's name)
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
    double scanSeconds = 0;
};

struct ScanProgress {
    std::atomic<int> filesVisited{0};
    std::atomic<int> filesTotal{0};   // 0 until the directory walk finishes
};

// Synchronous; call from a worker thread. `progress` may be null.
LibraryScanResult ScanLibrary(const std::filesystem::path& root, ScanProgress* progress = nullptr);

} // namespace mmdx
