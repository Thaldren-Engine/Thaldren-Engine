#pragma once
// Music: the original's track choice rules (spec: docs/RE_MEMORY.md, "Client", "Music").
// Tracks are numbered from 1; each has a type (the mood it is played for). The player
// keeps one track playing on the audio output's music channel according to the mode:
//   0 stopped, 1 every track in order, 2 random, 3 one track repeated, 4 by mood.
// A mood change fades the current track out first; the battle judge decides between
// the building and battle moods from combat activity.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "client/audio.h"
#include "sim/rng.h"

namespace client {

enum class MusicMood : std::uint8_t { Building = 0, Battle = 1, Victory = 2, Defeat = 3, Silent = 4 };

class MusicPlayer {
public:
    // `load` decodes track n (1-based); null when it cannot.
    using Loader = std::function<std::shared_ptr<const Sample>(int track)>;
    MusicPlayer(Loader load, int track_count);

    // Settings (the music options screen).
    void set_enabled(bool on);
    bool enabled() const { return enabled_; }
    void set_mode(int mode) { mode_ = mode; }
    int mode() const { return mode_; }
    // The music volume setting (0..64). The player's own volume follows it (0x4D00D0 with
    // the setting flag) except while a fade's step is set — during a fade and after one cut
    // short by another mood change, until a fade ends or the music stops — when the change is
    // not taken at all.
    void set_volume(int volume_0_64);
    int volume() const { return volume_; }
    // The player's own volume (CD volume units: musicvol << 10, at most 0xFFFF).
    int level() const { return level_cfg_; }
    void set_repeat_track(int t) { repeat_ = t; }
    int repeat_track() const { return repeat_; }
    std::vector<std::uint8_t>& types() { return types_; }  // [track] = mood it plays for ([0] unused)
    int track_count() const { return count_; }
    int current_track() const { return current_; }
    bool is_playing() const { return playing_; }
    MusicMood mood() const { return mood_; }

    // A mood change (fades the current track out when needed).
    void set_mood(MusicMood mood);
    // Plays track n now (the options screen's play button); 0 stops.
    void play(int track);
    void stop();
    // The in-game menu holds the track (the original pauses the CD) and lets it go on.
    void set_paused(bool p) {
        paused_ = p;
        if (out_) out_->pause_music(p);
    }
    // Picks the next track by the mode (the original's "track ended" routine).
    void next();
    // Once per pass of the main loop: `now` in 1/30 s ticks. Drives fades, delays and
    // the end of tracks.
    void update(std::uint32_t now, AudioOut* out);

private:
    void start(int track);
    bool playing() const { return playing_; }

    Loader load_;
    int count_ = 0;
    std::vector<std::uint8_t> types_;
    std::array<int, 5> last_of_mood_{};  // the track each mood played last
    bool enabled_ = true;
    int mode_ = 4, volume_ = 32, repeat_ = 1;
    MusicMood mood_ = MusicMood::Silent;
    int current_ = 1;          // track number the next pick starts from
    bool playing_ = false;
    bool paused_ = false;
    // The player's volume in the CD volume's units (musicvol << 10, at most 0xFFFF: the
    // sound manager's +0x20) and the output's level now.
    int level_cfg_ = 32 << 10;
    void apply_level(int level);
    void set_level_setting(int level);  // 0x4D00D0(level, 0)
    // Fade out (0x4CE690, timer 0x4CE5E0 every 2 ticks): the level falls by the step (about
    // 1/18 of the volume; the sound manager's +0x284, left set when a mood change cuts the
    // fade short) until it reaches 0 — the track keeps playing unheard — then the pick, or for
    // the building mood the pick 120 ticks later (timer 0x4CE5B0).
    bool fade_on_ = false, delay_on_ = false;
    int fade_level_ = 0, fade_step_ = 0;
    std::uint32_t next_fade_tick_ = 0, delay_until_ = 0;
    std::shared_ptr<const Sample> pending_;
    AudioOut* out_ = nullptr;
    bool want_start_ = false;
    int want_track_ = 0;
    bool want_stop_ = false;
    std::uint32_t now_ = 0;
    sim::Lcg15 rand_;  // the music's own rand() stream
};

// Building/battle switching from combat activity (local player hurt or hurting: +1 per
// damage, +5 per kill it scores), judged once a second over the last 30 seconds.
class BattleMusicJudge {
public:
    void activity(int amount) { ring_[slot_] += amount; }
    // Once per pass of the main loop: `now` in 1/30 s ticks; `units` = the local player's
    // live units. Calls the player's set_mood when the mood should change.
    void update(std::uint32_t now, int units, MusicPlayer& music);
    void reset();

private:
    std::array<int, 30> ring_{};
    int slot_ = 0;
    int seconds_since_switch_ = 0;
    std::uint32_t last_ = 0;
    bool started_ = false;
    MusicMood current_ = MusicMood::Building;
};

}  // namespace client
