#pragma once
// Sound output: a small software mixer on an SDL3 audio stream (signed 16-bit
// stereo): effect voices and one music channel. It carries out the sound manager's commands
// (client/sound_mixer.h decides which voice starts, restarts or stops, and at what gains).
// Presentation only; nothing here touches the simulation.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"
#include "client/sound_mixer.h"

namespace client {

class AudioOut {
public:
    // Opens the default playback device; false (and silence) when there is none.
    AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    bool open();
    ~AudioOut();
    // Carries out one command: a voice starts (or starts again from the beginning) with its
    // channel gains, or stops. The sample must stay alive while its voice plays.
    void run(const SoundCommand& c);
    // Stops every effect voice of the samples in `samples` (their table goes away).
    void forget(const std::vector<Sample>& samples);
    // Effect volume (the original's wave-output volume: fxvol x 1024 of 65535, linear) and
    // music volume, both 0..1.
    void set_fx_volume(float volume);
    void set_music_volume(int volume);  // 0..256
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

// Reads a sound file the way the original's loader does (RIFF WAVE by its format fields,
// "DIGI" files, anything else as raw 8-bit mono at 11025 Hz) into 16-bit samples.
bool decode_wav(const std::vector<std::uint8_t>& file, Sample& out);

// Decodes a compressed audio file on disk (the platform's decoder: MP3 and the like) into
// 16-bit samples. False when the platform has no decoder for it.
bool decode_audio_file(const std::string& path, Sample& out);

} // namespace client
