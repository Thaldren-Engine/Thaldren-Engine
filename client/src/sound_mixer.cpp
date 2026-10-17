// The sound manager's decisions (spec: docs/RE_MEMORY.md, "Client", "Sound engine").

#include "client/sound_mixer.h"

#include <algorithm>
#include <cmath>

namespace client {

float sound_gain(int volume) {
    if (volume <= -10000) return 0.0f;
    if (volume >= 0) return 1.0f;
    return static_cast<float>(std::pow(10.0, volume / 2000.0));
}

namespace {

std::uint64_t frames_of(const Sample& s) { return s.pcm.size() / static_cast<std::size_t>(std::max(1, s.channels)); }

// The placement of a sound around the listener: the distance model (full volume within the
// minimum distance, then falling with minimum / distance, no further than at the maximum
// distance) and the pan by the direction (fully to one side 100 dB off the other channel).
void place(const SoundRequest& r, int& atten, int& pan) {
    atten = 0;
    pan = 0;
    if (!r.placed) return;
    const double x = r.pos[0], y = r.pos[1], z = r.pos[2];
    const double d = std::sqrt(x * x + y * y + z * z);
    if (d > r.min_distance && r.min_distance > 0.0f) {
        const double far = std::min(d, static_cast<double>(std::max(r.max_distance, r.min_distance)));
        atten = static_cast<int>(std::lround(2000.0 * std::log10(r.min_distance / far)));
    }
    if (x != 0.0 || z != 0.0) {
        // The angle between the listener's left and the sound, 0 (left) .. pi (right).
        const double horiz = std::sqrt(x * x + z * z);
        const double angle = std::acos(std::clamp(-x / horiz, -1.0, 1.0));
        pan = static_cast<int>(std::lround(10000.0 * 2.0 * angle / 3.14159265358979323846 - 10000.0));
    }
}

}  // namespace

SoundMixer::SoundMixer(int max_playing) { set_max_playing(max_playing); }

bool SoundMixer::playing(const Buffer& b, std::uint32_t now_ms) const {
    if (!b.started || b.stopped || !b.sample) return false;
    if (b.loop) return true;
    const std::uint64_t elapsed = static_cast<std::uint32_t>(now_ms - b.start_ms);
    return elapsed * static_cast<std::uint64_t>(std::max(1, b.sample->rate)) < frames_of(*b.sample) * 1000u;
}

std::uint64_t SoundMixer::position(const Buffer& b, std::uint32_t now_ms) const {
    if (!b.sample || b.sample->rate <= 0) return 0;
    std::uint64_t f = static_cast<std::uint64_t>(static_cast<std::uint32_t>(now_ms - b.start_ms)) * static_cast<std::uint64_t>(b.sample->rate) / 1000u;
    const std::uint64_t n = frames_of(*b.sample);
    if (n > 0) f = b.loop ? f % n : std::min(f, n);
    return f;
}

void SoundMixer::stop(Buffer& b, bool evicted) {
    const bool was = playing(b, now_);  // (a stop of a sound that has ended is not heard)
    b.stopped = true;
    if (!was) return;
    SoundCommand c;
    c.kind = SoundCommand::Kind::Stop;
    c.voice = b.voice;
    c.sample = b.sample;
    c.copy = b.copy;
    c.evicted = evicted;
    commands_.push_back(c);
}

void SoundMixer::evict_oldest() {
    // The tracked sound started first (smallest serial) that does not loop stops.
    int best = -1;
    for (int i = 0; i < 32; ++i) {
        const Entry& e = table_[static_cast<std::size_t>(i)];
        if (!e.buffer || e.loop) continue;
        if (best < 0 || e.serial < table_[static_cast<std::size_t>(best)].serial) best = i;
    }
    if (best < 0) {
        // Only looping sounds tracked: the original reads past its table here; nothing stops.
        count_ = 0;
        return;
    }
    Entry& e = table_[static_cast<std::size_t>(best)];
    stop(*e.buffer, true);
    e = Entry{};
    --count_;
}

std::unique_ptr<SoundMixer::Buffer> SoundMixer::new_buffer(const Sample* s, int copy) {
    auto b = std::make_unique<Buffer>();
    b->voice = next_voice_++;
    b->sample = s;
    b->copy = copy;
    return b;
}

bool SoundMixer::start(std::array<std::unique_ptr<Buffer>, 4>& copies, const SoundRequest& r, std::uint32_t now_ms) {
    if (r.loop)
        for (const Entry& e : table_)
            if (e.buffer && e.loop) return false;  // one looping sound at a time
    while (count_ >= max_playing_) {
        const int before = count_;
        evict_oldest();
        if (count_ >= before) break;
    }
    // A stopped copy plays; else a new copy while there is room for one (the last free place
    // of the four); else the copy that has played furthest starts again.
    Buffer* use = nullptr;
    int free_at = 0, best = 0;
    std::uint64_t furthest = 0;
    bool restart = false;
    for (int i = 0; i < 4; ++i) {
        Buffer* b = copies[static_cast<std::size_t>(i)].get();
        if (!b) {
            free_at = i;
            continue;
        }
        if (!playing(*b, now_ms)) {
            use = b;
            break;
        }
        const std::uint64_t p = position(*b, now_ms);
        if (p > furthest) {
            furthest = p;
            best = i;
        }
    }
    if (!use) {
        if (free_at > 0) {
            copies[static_cast<std::size_t>(free_at)] = new_buffer(r.sample, free_at);
            use = copies[static_cast<std::size_t>(free_at)].get();
        } else {
            use = copies[static_cast<std::size_t>(best)].get();
            restart = true;
        }
    }
    if (!use) return false;
    use->started = true;
    use->stopped = false;
    use->loop = r.loop;
    use->start_ms = now_ms;
    SoundCommand c;
    c.kind = SoundCommand::Kind::Start;
    c.voice = use->voice;
    c.sample = use->sample;
    c.loop = r.loop;
    c.volume = r.volume;
    c.copy = use->copy;
    c.restart = restart;
    c.placed = r.placed;
    c.pos = r.pos;
    c.min_distance = r.min_distance;
    c.max_distance = r.max_distance;
    place(r, c.distance_atten, c.pan);
    const int base = r.volume + c.distance_atten;
    c.gain_left = sound_gain(base - std::max(c.pan, 0));
    c.gain_right = sound_gain(base + std::min(c.pan, 0));
    commands_.push_back(c);
    for (Entry& e : table_)
        if (!e.buffer) {
            e.buffer = use;
            e.serial = ++serial_;
            e.loop = r.loop;
            ++count_;
            break;
        }
    return true;
}

bool SoundMixer::play(const SoundRequest& r, std::uint32_t now_ms) {
    now_ = now_ms;
    if (!r.sample || r.sample->pcm.empty()) return false;
    if (r.one_shot) {
        // A file sound: the sweep first, then one of the eight one-shot places (none free:
        // nothing plays) with a buffer of its own.
        sweep(now_ms);
        for (auto& slot : one_shots_) {
            if (slot[0]) continue;
            slot[0] = new_buffer(r.sample, 0);
            if (start(slot, r, now_ms)) return true;
            release_one_shot(slot[0]);
            return false;
        }
        return false;
    }
    BankSound* bs = nullptr;
    for (auto& b : bank_)
        if (b->sample == r.sample) bs = b.get();
    if (!bs) {
        bank_.push_back(std::make_unique<BankSound>());
        bs = bank_.back().get();
        bs->sample = r.sample;
        bs->copies[0] = new_buffer(r.sample, 0);
    }
    return start(bs->copies, r, now_ms);
}

void SoundMixer::release_one_shot(std::unique_ptr<Buffer>& b) {
    if (!b) return;
    stop(*b, false);
    // Its first table entry goes with it.
    for (Entry& e : table_)
        if (e.buffer == b.get()) {
            e = Entry{};
            --count_;
            break;
        }
    released_.push_back(std::move(b));
}

void SoundMixer::sweep(std::uint32_t now_ms) {
    now_ = now_ms;
    for (auto& slot : one_shots_)
        if (slot[0] && !playing(*slot[0], now_ms)) release_one_shot(slot[0]);
    for (Entry& e : table_)
        if (e.buffer && !playing(*e.buffer, now_ms)) {
            e = Entry{};
            --count_;
        }
    // Released buffers nothing refers to any more can go.
    released_.erase(std::remove_if(released_.begin(), released_.end(),
                                   [&](const std::unique_ptr<Buffer>& b) {
                                       for (const Entry& e : table_)
                                           if (e.buffer == b.get()) return false;
                                       return true;
                                   }),
                    released_.end());
}

void SoundMixer::tick(std::uint32_t now_ms) {
    now_ = now_ms;
    if (static_cast<std::uint32_t>(now_ms - last_sweep_) < 100u) return;
    sweep(now_ms);
    last_sweep_ = now_ms;
}

void SoundMixer::stop_all(std::uint32_t now_ms) {
    now_ = now_ms;
    for (Entry& e : table_)
        if (e.buffer) {
            stop(*e.buffer, false);
            e = Entry{};
            --count_;
        }
}

void SoundMixer::forget(const std::vector<Sample>& samples) {
    if (samples.empty()) return;
    const Sample* lo = samples.data();
    const Sample* hi = samples.data() + samples.size();
    auto ours = [&](const Sample* s) { return s >= lo && s < hi; };
    for (Entry& e : table_)
        if (e.buffer && ours(e.buffer->sample)) {
            stop(*e.buffer, false);
            e = Entry{};
            --count_;
        }
    for (auto& slot : one_shots_)
        if (slot[0] && ours(slot[0]->sample)) release_one_shot(slot[0]);
    bank_.erase(std::remove_if(bank_.begin(), bank_.end(), [&](const std::unique_ptr<BankSound>& b) { return ours(b->sample); }), bank_.end());
    released_.erase(std::remove_if(released_.begin(), released_.end(), [&](const std::unique_ptr<Buffer>& b) { return ours(b->sample); }),
                    released_.end());
}

std::vector<SoundCommand> SoundMixer::take_commands() {
    std::vector<SoundCommand> out;
    out.swap(commands_);
    return out;
}

}  // namespace client
