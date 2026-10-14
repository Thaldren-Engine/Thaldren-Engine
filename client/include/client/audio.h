#pragma once
// Sound output: a small software mixer on an SDL3 audio stream (signed 16-bit
// stereo). Presentation only; nothing here touches the simulation.

#include <cstdint>
#include <memory>
#include <vector>

#include "client/content.h"

namespace client {

class AudioOut {
public:
    // Opens the default playback device; false (and silence) when there is none.
    AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    bool open(const std::vector<Sample>* samples, int max_voices = 16);
    ~AudioOut();
    // Starts sample `index` panned -100 (left) .. 100 (right). When every voice is busy
    // the oldest one is replaced.
    void play(int index, int pan);
    bool ok() const { return impl_ != nullptr; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Decodes a RIFF WAVE file (PCM 8/16-bit, IMA or MS ADPCM) into 16-bit samples.
bool decode_wav(const std::vector<std::uint8_t>& file, Sample& out);

} // namespace client
