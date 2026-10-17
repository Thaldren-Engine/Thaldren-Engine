// Builds without SDL3: no sound output.
#include "client/audio.h"

namespace client {

struct AudioOut::Impl {};

bool AudioOut::open() { return false; }
AudioOut::AudioOut() = default;
AudioOut::~AudioOut() = default;
void AudioOut::run(const SoundCommand&) {}
void AudioOut::forget(const std::vector<Sample>&) {}
void AudioOut::set_fx_volume(float) {}
void AudioOut::set_music_volume(int) {}
void AudioOut::play_music(std::shared_ptr<const Sample>) {}
void AudioOut::stop_music() {}
void AudioOut::pause_music(bool) {}
bool AudioOut::music_playing() const { return false; }
double AudioOut::music_position() const { return 0.0; }

} // namespace client
