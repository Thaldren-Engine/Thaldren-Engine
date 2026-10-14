// Builds without SDL3: no sound output and no WAV decoding.
#include "client/audio.h"

namespace client {

struct AudioOut::Impl {};

bool AudioOut::open(const std::vector<Sample>*, int) { return false; }
AudioOut::AudioOut() = default;
AudioOut::~AudioOut() = default;
void AudioOut::play(int, int) {}
bool decode_wav(const std::vector<std::uint8_t>&, Sample&) { return false; }

} // namespace client
