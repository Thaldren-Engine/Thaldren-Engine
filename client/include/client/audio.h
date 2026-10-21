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
    // The wave-output volume (fxvol x 1024 of 65535, linear, 0..1). On Windows Vista and later
    // the original's waveOutSetVolume sets the program's whole audio session: it scales every
    // sound the program makes, the music included (measured: docs/RE_MEMORY.md, "Sound
    // volume"). Here it scales the effect voices, the narration and the music channel.
    void set_fx_volume(float volume);
    // The music stream's own volume, 0..256 (the music library's 0..1).
    void set_music_volume(int volume);
    // Music channel: one stream at a time. Starts `track` at sample frame `frame` (repeating
    // from its start at the end when `repeat`).
    void music_start(std::shared_ptr<const Sample> track, std::uint64_t frame, bool repeat);
    // Stops the stream where it is (its position stays for music_frame).
    void music_halt();
    void music_set_repeat(bool repeat);
    // Playing: started, not halted, not run out.
    bool music_playing() const;
    // The stream's position in its sample frames.
    std::uint64_t music_frame() const;
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
