#pragma once
// MP4 writer for the offline video render: H.264 video + AAC audio through Media Foundation
// (IMFSinkWriter, hardware encoders allowed). Frames arrive as RGBA8 images (top-down) and are
// converted to NV12 (BT.709, limited range) on the CPU. The audio track is decoded from the song
// file with miniaudio and written in step with the video, covering exactly the frames written.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace mmdx {

struct ImageRGBA8;

class VideoEncoder {
public:
    struct Desc {
        uint32_t width = 1920, height = 1080;  // even values
        uint32_t fps = 30;
        uint32_t videoBitrate = 40'000'000;    // bits per second (H.264 High profile)
        std::filesystem::path audioPath;       // song file; empty = no audio track
        double audioStartSeconds = 0.0;        // song position of video frame 0 (negative: silence until the song starts)
    };

    VideoEncoder();
    ~VideoEncoder();  // Finish() if still open
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Creates the file (parent directories too). On failure returns false and sets `error`
    // (Korean, user-facing). An audio file that fails to decode only drops the audio track.
    bool Open(const std::filesystem::path& mp4Path, const Desc& desc, std::string* error);
    bool IsOpen() const;
    // Appends one frame (must be desc.width x desc.height) and the audio up to its end time.
    bool AddFrame(const ImageRGBA8& frame);
    uint32_t FramesWritten() const;
    // Finalizes the file (playable even when the render was cancelled midway). Idempotent.
    bool Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
