#pragma once
// The shipped program's CD audio device (spec: docs/RE_MEMORY.md, "Client", "Music (CD audio)",
// "The replacement multimedia library"). The 3.1 program on sale today imports its
// multimedia calls from a replacement of the system library beside it; that library forwards
// everything to the system except the command-string call, which never reaches the system: it
// answers 0 to every command, writes no answer, and plays music\<N>.mp3 for "play cdaudio from
// N" through its own stream on a thread of its own:
//   - "play cdaudio from W...": W (the word after "from ", up to the next blank) names
//     music/W.mp3; when that file opens for reading and writing the track becomes atoi(W) and
//     a restart is asked for; otherwise the track stays and a resume is asked for. Either way
//     the thread is started (once per program: 1 s of sleep first).
//   - "pause cdaudio": stopped where it is (the position kept).
//   - "stop cdaudio", "open", "close", "status", "set" ...: nothing.
//   - the thread, every 250 ms: the volume from the saved music volume setting (musicvol / 64);
//     a restart takes the position and stops; a stopped stream while playing opens the track's
//     file anew (in mode 1 the next track first, 17 wrapping to 2), puts it at the kept
//     position when a resume was asked for, and plays; in mode 2 a playing stream draws the
//     next track at random (rand() % 15 + 2); while not playing it stops, keeping the position.
//     The stream repeats unless the saved mode ("cdmode") is 1.
// Presentation only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "client/cd_audio.h"
#include "sim/rng.h"

namespace client {

// The stream the device plays through (one at a time; opening another replaces it).
class TrackOutput {
public:
    virtual ~TrackOutput() = default;
    // A new stream for the file (the one held goes and stops); false when it does not decode
    // (no stream is held then).
    virtual bool open(const std::string& path) = 0;
    virtual void play() = 0;          // from its position
    virtual void stop() = 0;          // where it is; the position stays
    virtual bool playing() const = 0;
    virtual void set_repeat(bool on) = 0;
    virtual void set_volume(float v) = 0;  // 0..1 (the library's own scale)
    virtual std::int64_t position() const = 0;  // in the file's sample frames
    virtual void set_position(std::int64_t frame) = 0;
};

class AudioOut;
struct Sample;

// The stream on the client's audio output's music channel. Without an output (headless runs)
// it keeps the stream's position on the given clock, so that a stream runs out after its
// length as it would play. `log`: one line per open / play / stop.
class AudioTrackOutput : public TrackOutput {
public:
    using Loader = std::function<std::shared_ptr<const Sample>(const std::string& path)>;
    AudioTrackOutput(Loader load, std::function<std::uint32_t()> clock_ms) : load_(std::move(load)), clock_ms_(std::move(clock_ms)) {}
    void attach(AudioOut* out) { out_ = out; }
    void set_log(bool on) { log_ = on; }
    bool open(const std::string& path) override;
    void play() override;
    void stop() override;
    bool playing() const override;
    void set_repeat(bool on) override;
    void set_volume(float v) override;
    std::int64_t position() const override;
    void set_position(std::int64_t frame) override;
    const std::string& path() const { return path_; }

private:
    std::uint64_t frames() const;
    std::uint64_t virtual_frame() const;
    Loader load_;
    std::function<std::uint32_t()> clock_ms_;
    AudioOut* out_ = nullptr;
    bool log_ = false;
    std::shared_ptr<const Sample> sample_;
    std::string path_;
    bool on_ = false, repeat_ = false;
    std::int64_t pos_ = 0;           // the position while stopped
    std::uint32_t started_ms_ = 0;   // (no output) the play's start
    float volume_ = 1.0f;
};

class TrackFileDevice : public CdCommandDevice {
public:
    struct Env {
        TrackOutput* out = nullptr;
        std::function<std::uint32_t()> clock_ms;
        // A path relative to the game's folder ("music/5.mp3") opens for reading and writing.
        std::function<bool(const std::string& path)> open_read_write;
        // The saved settings the library reads from the registry on each use.
        std::function<int()> saved_music_volume;  // "musicvol" (0..64)
        std::function<int()> saved_mode;          // "cdmode"
    };
    explicit TrackFileDevice(Env env) : env_(std::move(env)) {}
    std::uint32_t send(const std::string& command, std::string* reply, int reply_size, bool notify) override;
    // The thread's turns due by now (call once per main-loop pass, or more often).
    void update();

    // State (tests).
    int track() const { return track_; }
    bool started() const { return thread_ != Thread::None; }
    bool thread_ended() const { return thread_ == Thread::Ended; }
    std::int64_t kept_position() const { return pos_; }

private:
    enum class Thread : std::uint8_t { None, Sleeping, Running, Ended };
    void start_thread();
    void first_turn();
    void pre_turn();
    void post_turn();
    static std::string file_of(int track);
    bool open_stream(const std::string& path);
    float volume() const;
    int mode() const;

    Env env_;
    // The library's globals: playing (0x10009028), restart (0x10009044), resume (0x10009024),
    // the open failure (0x10009040), the track (0x10009030), the kept position (0x10009038).
    bool playing_ = false, restart_ = false, resume_ = false, open_failed_ = false;
    int track_ = 0;
    std::int64_t pos_ = 0;
    Thread thread_ = Thread::None;
    std::uint32_t due_ = 0;
    bool stream_ = false;       // a stream is held
    bool file_open_ = false;    // the thread's file object stays open after its first success
    std::string file_;          // the thread's current file name
    sim::Lcg15 rand_;           // the thread's own rand() (never seeded: starts at 1)
};

}  // namespace client
