#include "audio/AudioPlayer.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

struct AudioPlayer::Impl {
    ma_engine engine{};
    bool engineOk = false;
    ma_sound sound{};
    bool soundOk = false;
    float volume = 0.8f;
    bool muted = false;
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() {
    Unload();
    if (impl_->engineOk) ma_engine_uninit(&impl_->engine);
    impl_->engineOk = false;
}

bool AudioPlayer::Initialize() {
    if (impl_->engineOk) return true;
    if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) return false;
    impl_->engineOk = true;
    ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
    return true;
}

bool AudioPlayer::Load(const std::filesystem::path& path) {
    Unload();
    if (!impl_->engineOk) return false;
    ma_result r = ma_sound_init_from_file_w(&impl_->engine, path.c_str(),
                                            MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            nullptr, nullptr, &impl_->sound);
    if (r != MA_SUCCESS) {
        LOG_WARN("audio load failed: %s (%d)", PathToUtf8(path).c_str(), (int)r);
        return false;
    }
    impl_->soundOk = true;
    return true;
}

void AudioPlayer::Unload() {
    if (impl_->soundOk) ma_sound_uninit(&impl_->sound);
    impl_->soundOk = false;
}

bool AudioPlayer::IsLoaded() const { return impl_->soundOk; }

void AudioPlayer::Play() {
    if (impl_->soundOk) ma_sound_start(&impl_->sound);
}

void AudioPlayer::Pause() {
    if (impl_->soundOk) ma_sound_stop(&impl_->sound);
}

bool AudioPlayer::IsPlaying() const {
    return impl_->soundOk && ma_sound_is_playing(&impl_->sound) != 0;
}

bool AudioPlayer::AtEnd() const {
    return impl_->soundOk && ma_sound_at_end(&impl_->sound) != 0;
}

void AudioPlayer::Seek(double seconds) {
    if (!impl_->soundOk) return;
    float sampleRate = 0;
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sampleRateU32 = 0;
    if (ma_sound_get_data_format(&impl_->sound, &format, &channels, &sampleRateU32, nullptr, 0) != MA_SUCCESS ||
        sampleRateU32 <= 0)
        return;
    sampleRate = (float)sampleRateU32;
    double duration = DurationSeconds();
    if (seconds < 0) seconds = 0;
    if (duration > 0 && seconds > duration) seconds = duration;
    ma_sound_seek_to_pcm_frame(&impl_->sound, (ma_uint64)(seconds * sampleRate));
}

double AudioPlayer::PositionSeconds() const {
    if (!impl_->soundOk) return 0;
    float pos = 0;
    ma_sound_get_cursor_in_seconds(&impl_->sound, &pos);
    return pos;
}

double AudioPlayer::DurationSeconds() const {
    if (!impl_->soundOk) return 0;
    float len = 0;
    ma_sound_get_length_in_seconds(&impl_->sound, &len);
    return len;
}

void AudioPlayer::SetVolume(float volume01) {
    impl_->volume = volume01;
    if (impl_->engineOk) ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
}

float AudioPlayer::Volume() const { return impl_->volume; }

void AudioPlayer::SetMuted(bool muted) {
    impl_->muted = muted;
    if (impl_->engineOk) ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
}

bool AudioPlayer::Muted() const { return impl_->muted; }

} // namespace mmdx
