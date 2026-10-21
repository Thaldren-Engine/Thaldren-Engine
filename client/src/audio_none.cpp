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
void AudioOut::music_start(std::shared_ptr<const Sample>, std::uint64_t, bool) {}
void AudioOut::music_halt() {}
void AudioOut::music_set_repeat(bool) {}
bool AudioOut::music_playing() const { return false; }
std::uint64_t AudioOut::music_frame() const { return 0; }

} // namespace client
