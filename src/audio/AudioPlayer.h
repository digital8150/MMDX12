#pragma once
// Music playback (miniaudio high-level engine). Supports WAV/MP3/FLAC (+ OGG Vorbis via
// miniaudio's stb_vorbis if available). The playback cursor is the master clock for
// motion sync.
#include <filesystem>
#include <memory>

namespace mmdx {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize();                                   // creates the ma_engine
    bool Load(const std::filesystem::path& path);        // unloads previous; false on failure
    void Unload();
    bool IsLoaded() const;
    // Decodes the file into the engine's resource cache from any thread (blocking), so a later Load of the same
    // path on the main thread is instant. Each successful Preload needs one ReleasePreload (after the Load).
    bool Preload(const std::filesystem::path& path);
    void ReleasePreload(const std::filesystem::path& path);

    void Play();
    void Pause();
    bool IsPlaying() const;
    bool AtEnd() const;

    void Seek(double seconds);           // clamped to [0, duration]
    double PositionSeconds() const;      // 0 if not loaded
    double DurationSeconds() const;      // 0 if not loaded/unknown

    void SetVolume(float volume01);      // applied to the engine (persists across Load)
    float Volume() const;
    void SetMuted(bool muted);
    bool Muted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
