#pragma once
// The sound manager's decisions (spec: docs/RE_MEMORY.md, "Client", "Sound engine"): which
// sound buffer a start uses, which playing sound a new one cuts off, its volume and its place
// around the listener. It models a buffer-per-copy mixer: every bank sound owns up to four
// copies, at most `max_playing` sounds are tracked as playing, one-shot file sounds get their
// own buffer in one of eight slots. Pure logic on a millisecond clock (the caller's wall
// clock); the audio output only carries out the commands it produces. Presentation only.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"

namespace client {

// Volumes are in hundredths of a decibel (0 full, -10000 silent), as the platform mixer takes them.
constexpr int kSoundVolumeNormal = -585;      // every sound starts 5.85 dB down
constexpr int kSoundVolumeOutOfView = -1585;  // mono mode: a world sound away from the view

struct SoundRequest {
    const Sample* sample = nullptr;
    int volume = kSoundVolumeNormal;
    // Placed around the listener (3D mode): position relative to the listener (x right, z
    // ahead, y up) and the distance model's minimum and maximum distances.
    bool placed = false;
    std::array<float, 3> pos{};
    float min_distance = 1.0f, max_distance = 1.0f;
    bool loop = false;      // plays until stopped (only one looping sound at a time)
    bool one_shot = false;  // a file sound with a buffer of its own (unit speech, test sound)
};

// What the output must do: start (or restart from the beginning) voice `voice` with the given
// channel gains, or stop it. Voice numbers stay with their buffer (a restart reuses it).
struct SoundCommand {
    enum class Kind : std::uint8_t { Start, Stop } kind = Kind::Start;
    int voice = 0;
    const Sample* sample = nullptr;
    float gain_left = 1.0f, gain_right = 1.0f;  // linear amplitude
    bool loop = false;
    // For logs: the volume asked for, the pan (hundredths of a dB taken off the far channel,
    // negative = left) and the distance attenuation (hundredths of a dB) of a placed sound.
    int volume = 0, pan = 0, distance_atten = 0;
    bool placed = false;    // placed around the listener (its request's position and distances)
    std::array<float, 3> pos{};
    float min_distance = 0.0f, max_distance = 0.0f;
    int copy = 0;           // which of the sound's copies (0 the first, 1..3 duplicates)
    bool restart = false;   // a playing copy started again from the beginning
    bool evicted = false;   // Stop: cut off to make room
};

class SoundMixer {
public:
    explicit SoundMixer(int max_playing = 8);
    // "MixingBuffers": sounds tracked as playing at once.
    void set_max_playing(int n) { max_playing_ = n < 1 ? 1 : n; }
    int max_playing() const { return max_playing_; }
    // Starts a sound at wall-clock time `now_ms`; false when nothing starts.
    bool play(const SoundRequest& r, std::uint32_t now_ms);
    // The periodic sweep (the main loop's, every 100 ms of wall clock; also before every
    // one-shot): one-shot sounds that ended free their slots, finished table entries go.
    void sweep(std::uint32_t now_ms);
    // The main loop's timer: sweeps when 100 ms or more passed since the last sweep.
    void tick(std::uint32_t now_ms);
    // Stops every tracked sound (movies, battle start, sound switched off).
    void stop_all(std::uint32_t now_ms);
    // Commands for the output since the last call.
    std::vector<SoundCommand> take_commands();
    int tracked() const { return count_; }
    // Forgets a sample table's sounds (their samples go away): their buffers stop.
    void forget(const std::vector<Sample>& samples);

private:
    struct Buffer {
        int voice = 0;
        const Sample* sample = nullptr;
        int copy = 0;
        bool started = false, stopped = true, loop = false;
        std::uint32_t start_ms = 0;
    };
    struct Entry {
        Buffer* buffer = nullptr;
        int serial = 0;
        bool loop = false;
    };
    struct BankSound {
        const Sample* sample = nullptr;
        std::array<std::unique_ptr<Buffer>, 4> copies;
    };
    bool playing(const Buffer& b, std::uint32_t now_ms) const;
    std::uint64_t position(const Buffer& b, std::uint32_t now_ms) const;
    void stop(Buffer& b, bool evicted);
    void evict_oldest();
    std::unique_ptr<Buffer> new_buffer(const Sample* s, int copy);
    bool start(std::array<std::unique_ptr<Buffer>, 4>& copies, const SoundRequest& r, std::uint32_t now_ms);
    void release_one_shot(std::unique_ptr<Buffer>& b);

    int max_playing_ = 8;
    std::array<Entry, 32> table_{};
    int count_ = 0, serial_ = 0, next_voice_ = 1;
    std::vector<std::unique_ptr<BankSound>> bank_;
    std::array<std::array<std::unique_ptr<Buffer>, 4>, 8> one_shots_{};
    std::vector<std::unique_ptr<Buffer>> released_;  // buffers of freed one-shots (voices end)
    std::vector<SoundCommand> commands_;
    std::uint32_t last_sweep_ = 0, now_ = 0;
    bool swept_ = false;
};

// Linear amplitude of a volume in hundredths of a decibel.
float sound_gain(int volume);

}  // namespace client
