// The shipped program's CD audio device, music\N.mp3 for CD tracks (spec: docs/RE_MEMORY.md,
// "Client", "Music (CD audio)", "The replacement multimedia library").

#include "client/track_file_device.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "client/audio.h"
#include "client/content.h"

namespace client {

// ---------------------------------------------------------------- the stream on the audio output

std::uint64_t AudioTrackOutput::frames() const {
    if (!sample_) return 0;
    return sample_->pcm.size() / static_cast<std::size_t>(std::max(1, sample_->channels));
}

std::uint64_t AudioTrackOutput::virtual_frame() const {
    const std::uint64_t n = frames();
    const std::uint32_t now = clock_ms_ ? clock_ms_() : 0u;
    std::uint64_t f = static_cast<std::uint64_t>(pos_) + static_cast<std::uint64_t>(now - started_ms_) * static_cast<std::uint64_t>(sample_ ? sample_->rate : 0) / 1000u;
    if (n == 0) return 0;
    if (f >= n) f = repeat_ ? f % n : n;
    return f;
}

bool AudioTrackOutput::open(const std::string& path) {
    if (out_) out_->music_halt();
    on_ = false;
    pos_ = 0;
    path_ = path;
    sample_ = load_ ? load_(path) : nullptr;
    if (log_) std::printf("music: open %s%s\n", path.c_str(), sample_ ? "" : " (fails)");
    if (!sample_ && out_) out_->music_start(nullptr, 0, false);
    return sample_ != nullptr;
}

void AudioTrackOutput::play() {
    if (!sample_ || playing()) return;
    if (log_) std::printf("music: play %s at frame %lld\n", path_.c_str(), static_cast<long long>(pos_));
    on_ = true;
    started_ms_ = clock_ms_ ? clock_ms_() : 0u;
    if (out_) {
        out_->set_music_volume(static_cast<int>(volume_ * 256.0f + 0.5f));
        out_->music_start(sample_, static_cast<std::uint64_t>(std::max<std::int64_t>(0, pos_)), repeat_);
    }
}

void AudioTrackOutput::stop() {
    if (!on_) return;
    pos_ = position();
    on_ = false;
    if (out_) out_->music_halt();
    if (log_) std::printf("music: stop %s at frame %lld\n", path_.c_str(), static_cast<long long>(pos_));
}

bool AudioTrackOutput::playing() const {
    if (!on_ || !sample_) return false;
    if (out_) return out_->music_playing();
    return repeat_ || virtual_frame() < frames();
}

void AudioTrackOutput::set_repeat(bool on) {
    if (on_ && !out_) {
        pos_ = static_cast<std::int64_t>(virtual_frame());
        started_ms_ = clock_ms_ ? clock_ms_() : 0u;
    }
    repeat_ = on;
    if (out_) out_->music_set_repeat(on);
}

void AudioTrackOutput::set_volume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    if (out_) out_->set_music_volume(static_cast<int>(volume_ * 256.0f + 0.5f));
}

std::int64_t AudioTrackOutput::position() const {
    if (!on_) return pos_;
    if (out_) return static_cast<std::int64_t>(out_->music_frame());
    return static_cast<std::int64_t>(virtual_frame());
}

void AudioTrackOutput::set_position(std::int64_t frame) {
    const std::uint64_t n = frames();
    if (frame < 0) frame = 0;
    if (static_cast<std::uint64_t>(frame) > n) frame = static_cast<std::int64_t>(n);
    pos_ = frame;
    if (on_) {
        started_ms_ = clock_ms_ ? clock_ms_() : 0u;
        if (out_) out_->music_start(sample_, static_cast<std::uint64_t>(frame), repeat_);
    }
}

// ---------------------------------------------------------------- the device

std::string TrackFileDevice::file_of(int track) { return "music/" + std::to_string(track) + ".mp3"; }

float TrackFileDevice::volume() const {
    // (the four bytes of the value, each sign-extended, times 1/64)
    const int v = env_.saved_music_volume ? env_.saved_music_volume() : 64;
    return static_cast<float>(static_cast<double>(v) * 0.015625);
}

int TrackFileDevice::mode() const { return env_.saved_mode ? env_.saved_mode() : 4; }

void TrackFileDevice::start_thread() {
    if (thread_ != Thread::None) return;
    thread_ = Thread::Sleeping;
    due_ = (env_.clock_ms ? env_.clock_ms() : 0u) + 1000u;  // Sleep(1000) first
}

std::uint32_t TrackFileDevice::send(const std::string& command, std::string*, int, bool) {
    static const std::string kPlay = "play cdaudio from ";
    if (command.find(kPlay) != std::string::npos) {
        // The word after the key's length (from the string's start), up to the next blank.
        const std::string rest = command.substr(kPlay.size());
        const std::string word = rest.substr(0, rest.find(' '));
        playing_ = true;
        const int n = std::atoi(word.c_str());
        restart_ = true;
        const std::string path = "music/" + word + ".mp3";
        const bool opens = env_.open_read_write && env_.open_read_write(path);
        if (opens) {
            track_ = n;
            start_thread();
        } else {
            open_failed_ = true;
            if (open_failed_) {
                restart_ = false;
                resume_ = true;
                open_failed_ = false;
                start_thread();
            }
        }
    }
    // ("stop cdaudio" is recognised and does nothing)
    if (command.find("pause cdaudio") != std::string::npos) {
        playing_ = false;
        restart_ = false;
        open_failed_ = true;
        resume_ = false;
        start_thread();
    }
    // Nothing reaches the system's interface; every command succeeds and no answer is written.
    return 0;
}

bool TrackFileDevice::open_stream(const std::string& path) {
    stream_ = env_.out && env_.out->open(path);
    return stream_;
}

void TrackFileDevice::first_turn() {
    file_ = file_of(track_);
    if (track_ == 0) {
        thread_ = Thread::Ended;
        return;
    }
    // (a file that does not decode leaves no stream; the library's next call on it faults and
    // the program ends there: here the thread ends)
    if (!open_stream(file_)) {
        thread_ = Thread::Ended;
        return;
    }
    env_.out->set_volume(volume());
    env_.out->set_repeat(mode() != 1);
    if (!playing_ && env_.out->playing()) {
        pos_ = env_.out->position();
        env_.out->stop();
    }
    if (playing_ && !env_.out->playing()) {
        if (mode() == 1) {
            ++track_;
            if (track_ == 17) track_ = 2;
            file_ = file_of(track_);
        }
        if (!file_open_ && env_.open_read_write) file_open_ = env_.open_read_write(file_);
        if (file_open_) {
            resume_ = true;
            if (resume_) env_.out->set_position(pos_);
            env_.out->play();
        }
    }
    pre_turn();
}

void TrackFileDevice::pre_turn() {
    if (!stream_) {
        thread_ = Thread::Ended;
        return;
    }
    file_ = file_of(track_);
    if (restart_) {
        pos_ = env_.out->position();
        env_.out->stop();
        restart_ = false;
    }
    thread_ = Thread::Running;
    due_ += 250;  // Sleep(250)
}

void TrackFileDevice::post_turn() {
    env_.out->set_volume(volume());
    if (playing_) {
        if (!env_.out->playing()) {
            if (mode() == 1) {
                ++track_;
                if (17 < track_) track_ = 2;
                file_ = file_of(track_);
            }
            if (!file_open_ && env_.open_read_write) file_open_ = env_.open_read_write(file_);
            if (file_open_) {
                if (!open_stream(file_)) {
                    thread_ = Thread::Ended;  // (the library faults on the missing stream)
                    return;
                }
                if (resume_) {
                    env_.out->set_position(pos_);
                    resume_ = false;
                }
                env_.out->play();
            }
        } else if (mode() == 2) {
            track_ = static_cast<int>(rand_.next() % 15u) + 2;
            if (17 < track_) track_ = 2;
            file_ = file_of(track_);
        }
    }
    if (!playing_) {
        pos_ = env_.out->position();
        env_.out->stop();
    }
    pre_turn();
}

void TrackFileDevice::update() {
    const std::uint32_t now = env_.clock_ms ? env_.clock_ms() : 0u;
    for (int guard = 0; guard < 1000; ++guard) {
        if (thread_ == Thread::None || thread_ == Thread::Ended) return;
        if (static_cast<std::int32_t>(now - due_) < 0) return;
        if (!env_.out) {
            thread_ = Thread::Ended;
            return;
        }
        if (thread_ == Thread::Sleeping) first_turn();
        else post_turn();
    }
}

}  // namespace client
