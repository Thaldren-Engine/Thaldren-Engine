#pragma once
// Sound output: a small software mixer on an SDL3 audio stream (signed 16-bit
// stereo): effect voices and one music channel. Presentation only; nothing here
// touches the simulation.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"

namespace client {

class AudioOut {
public:
    // Opens the default playback device; false (and silence) when there is none.
    AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    // max_voices: sounds playing at once (the original's "MixingBuffers" setting, default 8).
    bool open(const std::vector<Sample>* samples, int max_voices = 8);
    ~AudioOut();
    // Starts sample `index` panned -100 (left) .. 100 (right). When every voice is busy
    // the oldest one is replaced; a sound already playing four times restarts its copy
    // that has played furthest instead of starting a fifth.
    void play(int index, int pan);
    // The same for any sample (it must stay alive while it plays); volume 0..256.
    void play_sample(const Sample* sample, int pan, int volume = 256);
    // Effect sample table (the menus' and the battle's differ).
    void set_samples(const std::vector<Sample>* samples);
    // Volumes 0..256: effects (voices) and music.
    void set_fx_volume(int volume);
    void set_music_volume(int volume);
    // Music channel: one track at a time.
    void play_music(std::shared_ptr<const Sample> track);
    void stop_music();
    // Holds the music where it is (true) or lets it go on (false).
    void pause_music(bool paused);
    bool music_playing() const;
    // Seconds of the music track played so far (0 when none).
    double music_position() const;
    bool ok() const { return impl_ != nullptr; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Decodes a RIFF WAVE file (PCM 8/16-bit, IMA or MS ADPCM) into 16-bit samples.
bool decode_wav(const std::vector<std::uint8_t>& file, Sample& out);

// Decodes a compressed audio file on disk (the platform's decoder: MP3 and the like) into
// 16-bit samples. False when the platform has no decoder for it.
bool decode_audio_file(const std::string& path, Sample& out);

} // namespace client
