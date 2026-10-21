#include "client/sound_desk.h"

#include <algorithm>
#include <cstdio>

namespace client {

namespace {
constexpr int kStreamVoice = -1;
}

void SoundDesk::set_fx_volume(int fxvol) {
    const int v = std::clamp(fxvol * 1024, 0, 0xFFFF);
    if (out_) out_->set_fx_volume(static_cast<float>(v) / 65535.0f);
}

bool SoundDesk::play(const SoundRequest& r, std::uint32_t now_ms) { return mixer_.play(r, now_ms); }

void SoundDesk::play_event(const std::vector<Sample>& samples, int sample, int volume, bool placed, const std::array<float, 3>& pos,
                           float min_distance, float max_distance, bool one_shot, std::uint32_t now_ms) {
    if (sample < 0 || static_cast<std::size_t>(sample) >= samples.size()) return;
    SoundRequest r;
    r.sample = &samples[static_cast<std::size_t>(sample)];
    r.volume = volume;
    r.placed = placed;
    r.pos = pos;
    r.min_distance = min_distance;
    r.max_distance = max_distance;
    r.one_shot = one_shot;
    mixer_.play(r, now_ms);
}

void SoundDesk::play_stream(const Sample* s) {
    stop_stream();
    if (!s || s->pcm.empty()) return;
    SoundCommand c;
    c.voice = kStreamVoice;
    c.sample = s;
    c.volume = 0;
    extra_.push_back(c);
}

void SoundDesk::stop_stream() {
    SoundCommand c;
    c.kind = SoundCommand::Kind::Stop;
    c.voice = kStreamVoice;
    extra_.push_back(c);
}

void SoundDesk::forget(const std::vector<Sample>& samples) {
    mixer_.forget(samples);
    flush("");
    if (out_) out_->forget(samples);
}

void SoundDesk::flush(const std::string& label) {
    std::vector<SoundCommand> cmds = mixer_.take_commands();
    cmds.insert(cmds.end(), extra_.begin(), extra_.end());
    extra_.clear();
    for (const SoundCommand& c : cmds) {
        if (out_) out_->run(c);
        if (c.kind == SoundCommand::Kind::Start) ++started_;
        if (!log_ || label.empty()) continue;
        if (c.kind == SoundCommand::Kind::Stop) {
            if (c.voice == kStreamVoice) continue;
            std::printf("%s: stop %s copy %d%s\n", label.c_str(), c.sample ? c.sample->name.c_str() : "?", c.copy, c.evicted ? " (cut off)" : "");
            continue;
        }
        std::printf("%s: sound %s vol %d", label.c_str(), c.sample ? c.sample->name.c_str() : "?", c.volume);
        if (c.voice == kStreamVoice) std::printf(" stream");
        else std::printf(" copy %d", c.copy);
        if (c.pan != 0 || c.distance_atten != 0) std::printf(" pan %d dist %d", c.pan, c.distance_atten);
        if (c.restart) std::printf(" restart");
        if (c.loop) std::printf(" loop");
        std::printf("\n");
    }
}

// ---------------------------------------------------------------- the wave-out path

namespace {
constexpr int kWaveVoice = -2;  // PlaySoundA's one sound
}

bool WaveOutChannel::playing(std::uint32_t now_ms) const {
    if (!current_) return false;
    if (current_loop_) return true;
    const std::size_t frames = current_->pcm.size() / static_cast<std::size_t>(std::max(1, current_->channels));
    const std::uint64_t ms = static_cast<std::uint64_t>(frames) * 1000u / static_cast<std::uint64_t>(std::max(1, current_->rate));
    return static_cast<std::uint64_t>(now_ms - started_ms_) < ms;
}

bool WaveOutChannel::start(const Sample* s, bool loop, std::uint32_t now_ms) {
    // (a new sound stops the one playing; data that is not a RIFF WAVE file plays nothing)
    if (current_ && out_) {
        SoundCommand stop;
        stop.kind = SoundCommand::Kind::Stop;
        stop.voice = kWaveVoice;
        out_->run(stop);
    }
    current_ = nullptr;
    if (!s || !s->riff || s->pcm.empty()) {
        if (log_) std::printf("wave: %s does not play\n", s ? s->name.c_str() : "(none)");
        return false;
    }
    current_ = s;
    current_loop_ = loop;
    started_ms_ = now_ms;
    ++started_;
    if (log_) std::printf("wave: play %s%s\n", s->name.c_str(), loop ? " (loop)" : "");
    if (out_) {
        SoundCommand c;
        c.voice = kWaveVoice;
        c.sample = s;
        c.loop = loop;
        c.gain_left = c.gain_right = 1.0f;  // the file's own level
        out_->run(c);
    }
    return true;
}

bool WaveOutChannel::play(const Sample* s, bool loop, std::uint32_t now_ms) {
    if (loop) loop_ = s;
    return start(s, loop, now_ms);
}

void WaveOutChannel::pass(std::uint32_t now_ms) {
    // PlaySoundA(loop, SND_ASYNC | SND_MEMORY | SND_NOSTOP): once more (not looping) when
    // nothing plays.
    if (loop_ && !playing(now_ms)) start(loop_, false, now_ms);
}

void WaveOutChannel::stop_all() {
    if (current_ && out_) {
        SoundCommand stop;
        stop.kind = SoundCommand::Kind::Stop;
        stop.voice = kWaveVoice;
        out_->run(stop);
    }
    if (log_ && current_) std::printf("wave: stop\n");
    current_ = nullptr;
    loop_ = nullptr;
}

}  // namespace client
