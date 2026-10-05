// miniaudio implementation (shared by the player in src/audio) and the duration probe used
// by the library scanner.
#define MINIAUDIO_IMPLEMENTATION
#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include "core/AudioProbe.h"

namespace mmdx {

double AudioDurationSec(const std::filesystem::path& path) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file_w(path.c_str(), &config, &decoder) != MA_SUCCESS) return 0;
    ma_uint64 frames = 0;
    double seconds = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && decoder.outputSampleRate > 0)
        seconds = (double)frames / decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);
    return seconds;
}

} // namespace mmdx
