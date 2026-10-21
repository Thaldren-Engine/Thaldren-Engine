// Music track choice and battle/peace switching (spec: docs/RE_MEMORY.md, "Client", "Music").

#include "client/music.h"

#include <algorithm>
#include <cstdint>

namespace client {

namespace {

// The CD volume (0..0xFFFF) as the audio output's music level (0..256).
int output_level(int level) { return (std::clamp(level, 0, 0xFFFF) * 256 + 0x7FFF) / 0xFFFF; }

// The fade's step (0x4CE690): the high word of volume x 0xC71C71C7 (a signed multiply: about
// −2/9 of it), divided by 4 rounding toward zero.
int fade_step_of(int level) {
    const std::int64_t p = static_cast<std::int64_t>(level) * static_cast<std::int32_t>(0xC71C71C7u);
    const auto hi = static_cast<std::int32_t>(static_cast<std::uint64_t>(p) >> 32);
    return (hi >> 2) - (hi >> 31);
}

}  // namespace

MusicPlayer::MusicPlayer(Loader load, int track_count) : load_(std::move(load)), count_(std::max(0, track_count)) {
    types_.assign(static_cast<std::size_t>(count_) + 1, 0);
}

void MusicPlayer::set_enabled(bool on) {
    enabled_ = on;
    if (!on) stop();
}

void MusicPlayer::apply_level(int level) {
    if (out_) out_->set_music_volume(output_level(level));
}

void MusicPlayer::set_level_setting(int level) {
    // 0x4D00D0(level, 0): not taken at all while a fade's step is set.
    if (fade_step_ != 0) return;
    level_cfg_ = std::clamp(level, 0, 0xFFFF);
    apply_level(level_cfg_);
}

void MusicPlayer::set_volume(int volume_0_64) {
    // (0x45BCC0: musicvol << 10)
    volume_ = volume_0_64;
    set_level_setting(volume_0_64 << 10);
}

void MusicPlayer::start(int track) {
    if (!enabled_ || track < 1 || track > count_) {
        // Playing a track that is not there fails: silence until something else picks one.
        playing_ = false;
        if (out_) out_->stop_music();
        return;
    }
    current_ = track;
    std::shared_ptr<const Sample> s = load_ ? load_(track) : nullptr;
    playing_ = s != nullptr;
    paused_ = false;
    if (out_) {
        // (0x4CEB60: the player's volume back on the output before the play)
        apply_level(level_cfg_);
        if (s) out_->play_music(std::move(s));
        else out_->stop_music();
    }
}

void MusicPlayer::stop() {
    // (0x4CED40, and the pick's own stop: the next pick walks from track 1 again; the fade's
    // step cleared, both timers off)
    playing_ = false;
    current_ = count_ != 0 ? 1 : 0;
    fade_on_ = delay_on_ = false;
    fade_step_ = 0;
    if (out_) out_->stop_music();
}

void MusicPlayer::play(int track) {
    if (track == 0) {
        stop();
        return;
    }
    start(track);
}

void MusicPlayer::next() {
    if (count_ == 0) return;
    if (mood_ == MusicMood::Silent) {
        stop();
        return;
    }
    if (paused_) return;
    const bool by_mood = mood_ == MusicMood::Victory || mood_ == MusicMood::Defeat || mode_ == 4;
    if (!by_mood) {
        switch (mode_) {
        case 0:
            if (playing_) stop();
            return;
        case 1:
            if (!playing_) {
                current_ = current_ < 1 ? 1 : current_ + 1;
                start(current_);
                if (current_ > count_) current_ = 1;
            }
            break;
        case 2:
            if (!playing_) start(static_cast<int>(rand_.next() % static_cast<std::uint32_t>(count_)) + 1);
            break;
        case 3:
            if (!playing_ || current_ != repeat_) start(repeat_ == 0 ? 1 : repeat_);
            break;
        default: break;
        }
        // (0x4CDB40 ends by putting the player's volume back on the output)
        apply_level(level_cfg_);
        return;
    }
    // By mood: keep a playing track of the mood (a track faded to nothing still plays), else
    // the r-th track of the mood after the current one (r = rand() & 15, at least the first),
    // else silence.
    int r = static_cast<int>(rand_.next() & 15u);
    const std::uint8_t want = static_cast<std::uint8_t>(mood_);
    if (playing_ && current_ >= 1 && current_ <= count_ && types_[static_cast<std::size_t>(current_)] == want) {
        apply_level(level_cfg_);
        return;
    }
    int t = current_;
    for (int steps = (r + 1) * count_; steps > 0; --steps) {
        ++t;
        if (t > count_) t = 1;
        if (types_[static_cast<std::size_t>(t)] == want && --r < 1) {
            start(t);
            apply_level(level_cfg_);
            return;
        }
    }
    stop();
}

void MusicPlayer::set_mood(MusicMood mood) {
    // 0x4CE690.
    const MusicMood old = mood_;
    if (old == mood) return;
    if (static_cast<std::size_t>(old) < last_of_mood_.size()) last_of_mood_[static_cast<std::size_t>(old)] = current_;
    mood_ = mood;
    if (!(mode_ == 4 || mood == MusicMood::Victory || mood == MusicMood::Defeat)) return;
    fade_level_ = level_cfg_;
    if (old == MusicMood::Silent) {
        // From silence: both timers off, the volume set (not taken while a fade's step is
        // left set), straight to the new mood's track.
        fade_on_ = delay_on_ = false;
        set_level_setting(level_cfg_);
        next();
        return;
    }
    if (fade_on_) {
        // A fade running: both timers off, straight to the pick (the fade's step stays set).
        fade_on_ = delay_on_ = false;
        next();
        return;
    }
    // The fade out (also while a building pick waits: that timer keeps running).
    fade_step_ = fade_step_of(level_cfg_);
    fade_on_ = true;
    next_fade_tick_ = now_ + 2;
}

void MusicPlayer::update(std::uint32_t now, AudioOut* out) {
    now_ = now;
    out_ = out;
    if (fade_on_ && now >= next_fade_tick_) {
        // 0x4CE5E0, every 2 ticks.
        next_fade_tick_ = now + 2;
        fade_level_ += fade_step_;
        if (fade_level_ > 0) {
            apply_level(fade_level_);
        } else {
            fade_on_ = false;
            fade_level_ = 0;
            fade_step_ = 0;
            apply_level(0);  // (the track goes on, unheard)
            if (mood_ == MusicMood::Building) {
                delay_on_ = true;
                delay_until_ = now + 120;
            } else {
                next();
            }
        }
    }
    if (delay_on_ && now >= delay_until_) {
        // 0x4CE5B0.
        delay_on_ = false;
        next();
    }
    // The end of a track picks the next one (also during a fade: the pick puts the volume
    // back, the fade goes on lowering it).
    if (playing_ && !paused_ && out_ && !out_->music_playing()) {
        playing_ = false;
        next();
    }
}

void BattleMusicJudge::reset() {
    ring_.fill(0);
    slot_ = 0;
    seconds_since_switch_ = 0;
    started_ = false;
    current_ = MusicMood::Building;
}

void BattleMusicJudge::update(std::uint32_t now, int units, MusicPlayer& music) {
    if (!started_) {
        started_ = true;
        last_ = now;
        return;
    }
    if (now <= last_ + 30) return;
    last_ = now;
    ++seconds_since_switch_;
    if (seconds_since_switch_ > 10) {
        int all = 0, recent = 0;
        // The whole ring, walked back from the slot before the current one, and the five
        // slots before the current one.
        for (int k = 1; k <= 30; ++k) {
            const int v = ring_[static_cast<std::size_t>((slot_ - k + 30) % 30)];
            all += v;
            if (k <= 5) recent += v;
        }
        MusicMood want = current_;
        if (music.mood() == MusicMood::Building && (all > 50 || recent > 30) && units > 30) want = MusicMood::Battle;
        else if (music.mood() == MusicMood::Battle && all < 10 && recent == 0 && seconds_since_switch_ > 60) want = MusicMood::Building;
        if (want != current_) {
            music.set_mood(want);
            seconds_since_switch_ = 0;
            current_ = want;
        }
    }
    slot_ = (slot_ + 1) % 30;
    ring_[static_cast<std::size_t>(slot_)] = 0;
}

}  // namespace client
