#pragma once
// CD audio music (spec: docs/RE_MEMORY.md, "Client", "Music (CD audio)"). The original plays its
// music as CD audio tracks through the system's command-string interface ("open cdaudio",
// "status cdaudio mode", "play cdaudio from N to M notify" ...) and changes the CD's volume
// through the auxiliary-device volume. This is that controller, routine by routine, over
// injectable devices:
//   - CdCommandDevice: where the command strings go. The shipped program's commands go to a
//     replacement of the system library that plays music\N.mp3 instead
//     (client/track_file_device.h); a real CD goes through the system's interface
//     (client/cd_audio_system.h, Windows).
//   - CdAuxVolume: the auxiliary devices (the CD-audio one's volume).
//   - CdDrives: the CD-ROM drives (the disc's volume serial names its track list).
//   - TickTimers: the program's tick timers (30 a second), pumped once per main-loop pass.
// Everything here is presentation; nothing touches the simulation.

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace client {

// The command-string interface (mciSendStringA's shape). `reply` (may be null) is written only
// when the device writes an answer; a device that does not leaves the caller's buffer as it was.
// `reply_size` is the caller's buffer size. `notify` asks for the track-end message to the
// program's window. Returns 0 on success, an error number otherwise.
class CdCommandDevice {
public:
    virtual ~CdCommandDevice() = default;
    virtual std::uint32_t send(const std::string& command, std::string* reply, int reply_size, bool notify) = 0;
};

// The auxiliary devices (aux*): the CD-audio device's volume.
class CdAuxVolume {
public:
    virtual ~CdAuxVolume() = default;
    virtual int count() { return 0; }                                    // auxGetNumDevs
    virtual bool caps(int /*id*/, int& technology) { technology = 0; return false; }  // auxGetDevCapsA (wTechnology)
    virtual bool get(int /*id*/, std::uint32_t& /*volume*/) { return false; }        // auxGetVolume (low word = left)
    virtual bool set(int /*id*/, std::uint32_t /*volume*/) { return false; }         // auxSetVolume (right << 16 | left)
};

// The drives: GetDriveTypeA of "X:\" (5 = CD-ROM) and GetVolumeInformationA's serial number.
class CdDrives {
public:
    virtual ~CdDrives() = default;
    virtual unsigned drive_type(char /*letter*/) { return 1; }  // 1: no such root
    virtual bool volume_serial(char /*letter*/, std::uint32_t& /*serial*/) { return false; }
};

// The next CD-ROM drive after `prev` (0: from A), 0 when none (0x4BB190).
char next_cdrom_drive(CdDrives& d, char prev);
// The serial number of the volume in drive `letter`; 0 when there is none (0x4BB260).
std::uint32_t cd_volume_serial(CdDrives& d, char letter);

// The program's tick timers (0x4B62D0 start, 0x4B63F0 add, 0x4B6370 pump, 0x4B64D0 kill): ten
// slots, a clock of (milliseconds x rate) / 1000 (32-bit unsigned arithmetic, as the original
// computes it from GetTickCount), a period and a countdown per slot. A pump takes the ticks
// since the last pump off every countdown and calls each slot whose countdown reached 0 or
// less, then sets it back to the period. Adding a timer pumps first.
class TickTimers {
public:
    using Fn = std::function<void(int arg)>;
    explicit TickTimers(std::function<std::uint32_t()> clock_ms) : clock_ms_(std::move(clock_ms)) {}
    void start(int rate);                // 0x4B62D0: every slot free, the clock read
    int add(int period, int arg, Fn fn);  // 0x4B63F0: the slot's number, -1 when none is free
    bool kill(int id);                    // 0x4B64D0
    void pump();                          // 0x4B6370
    int rate() const { return rate_; }

private:
    std::uint32_t ticks() const;
    void step(std::int32_t elapsed);
    struct Slot {
        Fn fn;
        int arg = 0;
        std::int32_t period = -1, countdown = 0;
    };
    std::function<std::uint32_t()> clock_ms_;
    std::array<Slot, 10> slots_{};
    int made_ = 0;  // 0x51FC80: timers made since the start (never lowered)
    int rate_ = 30;
    std::uint32_t last_ = 0;
};

// The CD music player: the sound manager's CD fields (+0x00 open, +0x14 the CD's aux device,
// +0x20 volume, +0x1FC mode, +0x200 tracks, +0x204 chosen track, +0x208 current track, +0x20C
// state, +0x210 disc serial, +0x214 track types, +0x278 mood, +0x27C switched on, +0x280 data
// track offset, +0x284 fade step, +0x28C disc-change callback) and its routines.
class CdMusic {
public:
    struct Env {
        CdCommandDevice* device = nullptr;
        CdAuxVolume* aux = nullptr;
        CdDrives* drives = nullptr;
        TickTimers* timers = nullptr;
        std::function<std::uint32_t()> rand;  // the caller's C runtime rand() stream
        // The CD player program's window (EnumWindows / GetClassNameA "SJE_CdPlayerClass"): its
        // handle or 0; closing it sends WM_CLOSE and WM_QUIT and waits 500 ms.
        std::function<std::uintptr_t()> find_player_window;
        std::function<void(std::uintptr_t)> close_player_window;
    };
    explicit CdMusic(Env env);  // the constructor's part (0x4CEE50 / 0x4CFF30 / 0x4D0040)

    // 0x4CE260: opens the CD (from the start or after the program was switched back to);
    // false when the device does not open.
    bool open();
    void close();                              // 0x4CE410
    int probe();                               // 0x4CDA00: tracks of the disc (0 none)
    // 0x4CD9D0: the routine called when a disc arrives; probes and calls it.
    void set_disc_callback(std::function<void()> fn);
    void pick();                               // 0x4CDB40: the track the mode / mood wants
    void set_mood(int mood);                   // 0x4CE690: 0 building, 1 battle, 2 victory, 3 defeat, 4 silent
    int mood() const { return mood_; }         // 0x4CE680
    bool play_track(int track);                // 0x4CEB60 (0: the mode / mood picks)
    bool stop();                               // 0x4CED40
    bool pause(bool hold);                     // 0x4CE910: hold (the game menu) or go on
    int step_track(int track);                 // 0x4CE8C0: the music page's next / previous
    void set_enabled(bool on);                 // 0x4CEDC0
    void set_mode(int mode) { mode_ = mode; }  // 0x4CE7A0: 0 stop, 1 all, 2 random, 3 one, 4 by mood
    int mode() const { return mode_; }
    void set_type(int track, std::uint8_t type);  // 0x4CE7C0
    std::uint8_t type(int track) const;           // 0x4CE7E0
    void set_types(const std::uint8_t* types);    // 0x4CE3E0: the first `tracks()` bytes for tracks 1..
    void set_chosen(int track);                   // 0x4CE580
    int chosen() const { return chosen_; }        // 0x4CE5A0
    int tracks() const { return count_; }         // 0x4CE450
    int current() const { return current_; }      // 0x4CE7F0
    std::uint32_t serial() const { return serial_; }  // 0x4CD9C0
    bool first_track_not_audio();                 // 0x4CE460
    bool device_playing();                        // 0x4CE800: "status cdaudio mode" is "playing"
    // 0x4D00D0: the CD's volume (0..65535; `fading`: a fade's step, not the setting). False when
    // the aux device refuses it.
    bool set_volume(std::int32_t volume, bool fading);
    std::uint32_t volume() const { return volume_; }
    // 0x4D0040: the CD's aux device volume (low word), -1 without one (the CDAudioVolume setting).
    std::int32_t aux_volume();
    // 0x4CE030: the window procedure's hook (MM_MCINOTIFY 0x3B9, WM_DEVICECHANGE 0x219).
    void window_message(std::uint32_t message, std::uintptr_t wparam);
    bool player_window_open() const { return player_window_ != 0; }  // 0x4CE1D0
    void close_player_window();                                      // 0x4CE190
    bool is_open() const { return open_; }
    int state() const { return state_; }        // 0 stopped, 1 playing, 2 held
    int data_offset() const { return data_offset_; }
    std::int32_t fade_step() const { return fade_step_; }
    int last_track_of_mood(int mood) const { return mood >= 0 && mood < 5 ? mood_track_[static_cast<std::size_t>(mood)] : 0; }
    // A command string of the program's own (0x490FE0 sends "stop", "close", "open" itself).
    std::uint32_t command(const std::string& c) { return send(c); }

private:
    std::uint32_t send(const std::string& c, std::string* reply = nullptr, int size = 0, bool notify = false);
    static bool is_playing_text(const std::string& s) { return s == "playing"; }
    void reset_stopped();  // stop's common part (state, current, fade, timers)
    void fade_tick();      // 0x4CE5E0
    void delay_done();     // 0x4CE5B0

    Env env_;
    bool open_ = false;
    int aux_id_ = -1;
    std::uint32_t volume_ = 0xFFFFFFFFu;
    int mode_ = 1, count_ = 0, chosen_ = 0, current_ = 0, state_ = 0;
    std::uint32_t serial_ = 0;
    std::array<std::uint8_t, 100> types_{};
    int mood_ = 0;
    bool enabled_ = false;
    int data_offset_ = 0;
    std::int32_t fade_step_ = 0;
    std::function<void()> on_disc_;
    // The program's globals: the fade's level (0x51FF10), its timer (0x50B544) and the 120-tick
    // delay's (0x50B540), the last track per mood (0x51FF20), the CD player's window (0x51FF18).
    std::int32_t fade_level_ = 0;
    int fade_timer_ = -1, delay_timer_ = -1;
    std::array<int, 5> mood_track_{};
    std::uintptr_t player_window_ = 0;
    // The routines' answer buffers. Each is a stack buffer of its routine in the original, which
    // a device that writes no answer leaves as it was; these keep what the device last wrote
    // there (empty: the stack's old bytes, never a valid answer; see RE_MEMORY).
    std::string tracks_buf_, type_buf_, pick_mode_buf_, check_type_buf_, playing_buf_, play_mode_buf_, resume_buf_, notify_mode_buf_;
};

// The track lists (the "CDLISTS" registry value, 0x51E828): 20 entries of 0x88 bytes, the most
// recent disc first: 0x20 bytes nothing reads, the disc's volume serial (+0x20), the types of
// tracks 1..100 (+0x24).
struct CdTrackLists {
    static constexpr std::size_t kEntries = 20, kEntry = 0x88, kSize = kEntries * kEntry;
    std::array<std::uint8_t, kSize> bytes{};
    std::uint32_t serial(std::size_t entry) const;
    void set_serial(std::size_t entry, std::uint32_t s);
    std::uint8_t* types(std::size_t entry) { return bytes.data() + entry * kEntry + 0x24; }
    // The setting's text: the bytes in hex (anything else, or a missing setting: all zero, as
    // the original clears the table when the value is missing).
    std::string to_text() const;
    void from_text(const std::string& text);
};
// 0x490FE0, after a disc change and at the start: the drive asked again ("stop", "close",
// "open cdaudio"), the switch and the mode set, the mood set again; the disc's entry (by its
// serial) moved to the front and its types taken; a disc not listed gets a new front entry
// (the last one dropped), with tracks 1-7 battle and 8-16 building when it has 16 tracks and
// its first track is not audio (those types also go to the player). Then the pick in a running
// battle, else stopped.
void cd_disc_changed(CdMusic& m, CdTrackLists& lists, bool enabled, int mode, bool battle_running);
// 0x490F80 / 0x4916A0: the player's types into the front entry (the setting is written after).
void cd_keep_types(CdMusic& m, CdTrackLists& lists);

// Building/battle switching from combat activity (0x494E70): the local player hurt or hurting
// +1 per damage, +5 per kill it scores; judged once a second (30 ticks of the interface clock)
// over the last 30 seconds.
class BattleMusicJudge {
public:
    void activity(int amount) { ring_[static_cast<std::size_t>(slot_)] += amount; }
    // Once per pass in a battle: `now` the 30-a-second interface clock; `units` the local
    // player's live units. Sets the music's mood when it should change.
    void update(std::uint32_t now, int units, CdMusic& music);
    void reset();

private:
    std::array<int, 30> ring_{};
    int slot_ = 0;
    int seconds_since_switch_ = 0;
    std::uint32_t last_ = 0;
    bool started_ = false;
    int current_ = -1;  // 0x5091D0 (-1 at the program's start)
};

}  // namespace client
