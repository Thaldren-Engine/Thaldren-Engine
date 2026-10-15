// Music track choice and battle/peace switching (spec: docs/RE_MEMORY.md, "Client", "Music").

#include "client/music.h"

#include <algorithm>

namespace client {

MusicPlayer::MusicPlayer(Loader load, int track_count) : load_(std::move(load)), count_(std::max(0, track_count)) {
    types_.assign(static_cast<std::size_t>(count_) + 1, 0);
}

void MusicPlayer::set_enabled(bool on) {
    enabled_ = on;
    if (!on) stop();
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
        out_->set_music_volume(volume_ * 256 / 64);
        if (s) out_->play_music(std::move(s));
        else out_->stop_music();
    }
}

void MusicPlayer::stop() {
    playing_ = false;
    fade_level_ = -1;
    pick_after_delay_ = false;
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
            return;
        case 2:
            if (!playing_) start(static_cast<int>(rand_.next() % static_cast<std::uint32_t>(count_)) + 1);
            return;
        case 3:
            if (!playing_ || current_ != repeat_) start(repeat_ == 0 ? 1 : repeat_);
            return;
        default: return;
        }
    }
    // By mood: keep a playing track of the mood, else the r-th track of the mood after the
    // current one (r = rand() & 15, at least the first), else silence.
    int r = static_cast<int>(rand_.next() & 15u);
    const std::uint8_t want = static_cast<std::uint8_t>(mood_);
    if (playing_ && current_ >= 1 && current_ <= count_ && types_[static_cast<std::size_t>(current_)] == want) return;
    int t = current_;
    for (int steps = (r + 1) * count_; steps > 0; --steps) {
        ++t;
        if (t > count_) t = 1;
        if (types_[static_cast<std::size_t>(t)] == want && --r < 1) {
            start(t);
            return;
        }
    }
    stop();
}

void MusicPlayer::set_mood(MusicMood mood) {
    const MusicMood old = mood_;
    if (old == mood) return;
    last_of_mood_[static_cast<std::size_t>(old)] = current_;
    mood_ = mood;
    if (!(mode_ == 4 || mood == MusicMood::Victory || mood == MusicMood::Defeat)) return;
    if (old == MusicMood::Silent || fade_level_ >= 0) {
        // From silence (or during a fade): straight to the new mood's track.
        fade_level_ = -1;
        pick_after_delay_ = false;
        if (out_) out_->set_music_volume(volume_ * 256 / 64);
        next();
        return;
    }
    // Fade the current track out: every 2 ticks the volume falls by 1/18 of the full value.
    fade_level_ = volume_ * 256 / 64;
    fade_step_ = std::max(1, fade_level_ * 2 / 9 / 4);
    next_fade_tick_ = now_ + 2;
}

void MusicPlayer::update(std::uint32_t now, AudioOut* out) {
    now_ = now;
    out_ = out;
    if (fade_level_ >= 0 && now >= next_fade_tick_) {
        next_fade_tick_ = now + 2;
        fade_level_ -= fade_step_;
        if (fade_level_ > 0) {
            if (out_) out_->set_music_volume(fade_level_);
        } else {
            fade_level_ = -1;
            if (out_) {
                out_->stop_music();
                out_->set_music_volume(volume_ * 256 / 64);
            }
            playing_ = false;
            if (mood_ == MusicMood::Building) {
                pick_after_delay_ = true;
                delay_until_ = now + 120;
            } else {
                next();
            }
        }
    }
    if (pick_after_delay_ && now >= delay_until_) {
        pick_after_delay_ = false;
        next();
    }
    // The end of a track picks the next one.
    if (playing_ && fade_level_ < 0 && out_ && !out_->music_playing()) {
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
