#pragma once
// The client's sound desk: the sound manager (client/sound_mixer.h) in front of the audio
// output, the effect volume and the narration channel, and the sound log of headless runs
// (every start and cut-off with its name, volume, copy and placement). Presentation only.

#include <cstdint>
#include <string>
#include <vector>

#include "client/audio.h"
#include "client/sound_mixer.h"

namespace client {

class SoundDesk {
public:
    void attach(AudioOut* out) { out_ = out; }
    void set_log(bool on) { log_ = on; }
    bool logging() const { return log_; }
    // The effect volume setting (fxvol 0..64): the wave-output volume fxvol x 1024 (at most
    // 65535) of 65535, linear.
    void set_fx_volume(int fxvol);
    void set_max_playing(int n) { mixer_.set_max_playing(n); }
    // A sound start (see SoundRequest); `now_ms` is the main loop's wall clock.
    bool play(const SoundRequest& r, std::uint32_t now_ms);
    // A battle client's sound (index into `samples`, as GameClient::take_sounds gives them).
    void play_event(const std::vector<Sample>& samples, int sample, int volume, bool placed, const std::array<float, 3>& pos, float min_distance,
                    float max_distance, bool one_shot, std::uint32_t now_ms);
    // Narration: its own streamed channel at full volume, outside the playing limit; a new one
    // replaces the last.
    void play_stream(const Sample* s);
    void stop_stream();
    // The main loop's 100 ms sweep.
    void tick(std::uint32_t now_ms) { mixer_.tick(now_ms); }
    void stop_all(std::uint32_t now_ms) { mixer_.stop_all(now_ms); }
    // A sample table goes away: its sounds stop and are forgotten.
    void forget(const std::vector<Sample>& samples);
    // Carries out the commands so far; with the log on, prints them after `label`.
    void flush(const std::string& label);
    std::size_t started() const { return started_; }
    SoundMixer& mixer() { return mixer_; }

private:
    SoundMixer mixer_;
    AudioOut* out_ = nullptr;
    bool log_ = false;
    std::size_t started_ = 0;
    std::vector<SoundCommand> extra_;  // the narration channel's commands
};

}  // namespace client
