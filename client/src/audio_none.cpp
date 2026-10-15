// Builds without SDL3: no sound output and no WAV decoding.
#include "client/audio.h"

namespace client {

struct AudioOut::Impl {};

bool AudioOut::open(const std::vector<Sample>*, int) { return false; }
AudioOut::AudioOut() = default;
AudioOut::~AudioOut() = default;
void AudioOut::play(int, int) {}
void AudioOut::play_sample(const Sample*, int, int) {}
void AudioOut::set_samples(const std::vector<Sample>*) {}
void AudioOut::set_fx_volume(int) {}
void AudioOut::set_music_volume(int) {}
void AudioOut::play_music(std::shared_ptr<const Sample>) {}
void AudioOut::stop_music() {}
bool AudioOut::music_playing() const { return false; }
double AudioOut::music_position() const { return 0.0; }
bool decode_wav(const std::vector<std::uint8_t>&, Sample&) { return false; }

} // namespace client
