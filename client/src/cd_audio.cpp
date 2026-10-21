// CD audio music, routine by routine (spec: docs/RE_MEMORY.md, "Client", "Music (CD audio)").

#include "client/cd_audio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace client {

namespace {
// The C runtime's atoi on a buffer (leading blanks, a sign, digits).
int c_atoi(const std::string& s) { return std::atoi(s.c_str()); }

std::string format_int(const char* fmt, int v) {
    char b[64];
    std::snprintf(b, sizeof b, fmt, v);
    return b;
}
}  // namespace

// ---------------------------------------------------------------- drives (0x4BB190, 0x4BB260)

char next_cdrom_drive(CdDrives& d, char prev) {
    // toupper(prev); a letter: the next one, anything else: from A.
    char c = prev;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    c = (c < 'A' || c > 'Z') ? 'A' : static_cast<char>(c + 1);
    if (c > 'Z') return 0;
    do {
        if (d.drive_type(c) == 5) return c;
        ++c;
    } while (c < '[');
    return 0;
}

std::uint32_t cd_volume_serial(CdDrives& d, char letter) {
    std::uint32_t serial = 0;  // (local_90 = 0; GetVolumeInformationA's result is not looked at)
    std::uint32_t s = 0;
    if (d.volume_serial(letter, s)) serial = s;
    return serial;
}

// ---------------------------------------------------------------- tick timers

std::uint32_t TickTimers::ticks() const {
    // (GetTickCount() * rate) in 32 bits, then an unsigned division by 1000.
    const std::uint32_t ms = clock_ms_ ? clock_ms_() : 0u;
    return static_cast<std::uint32_t>(ms * static_cast<std::uint32_t>(rate_)) / 1000u;
}

void TickTimers::start(int rate) {
    rate_ = rate;
    made_ = 0;
    for (Slot& s : slots_) s.period = -1;
    last_ = ticks();
}

void TickTimers::step(std::int32_t elapsed) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.period < 0) continue;
        s.countdown -= elapsed;
        if (s.countdown < 1) {
            // (the callback may add, kill or pump timers; the slot is set back to its period
            // afterwards, -1 when it killed itself)
            Fn fn = s.fn;
            const int arg = s.arg;
            if (fn) fn(arg);
            slots_[i].countdown = slots_[i].period;
        }
    }
}

void TickTimers::pump() {
    const std::uint32_t now = ticks();
    const std::int32_t elapsed = static_cast<std::int32_t>(now - last_);
    last_ = ticks();
    step(elapsed);
}

int TickTimers::add(int period, int arg, Fn fn) {
    pump();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        if (s.period < 0) {
            s.fn = std::move(fn);
            s.arg = arg;
            s.period = period;
            s.countdown = period;
            ++made_;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool TickTimers::kill(int id) {
    if (made_ <= id || id < 0) return false;
    slots_[static_cast<std::size_t>(id)].period = -1;
    return true;
}

// ---------------------------------------------------------------- the CD music player

CdMusic::CdMusic(Env env) : env_(std::move(env)) {
    // 0x4CFF30: the first auxiliary device whose technology is CD audio (1).
    aux_id_ = -1;
    if (env_.aux) {
        const int n = env_.aux->count();
        for (int id = 0; id < n; ++id) {
            int tech = 0;
            if (env_.aux->caps(id, tech) && tech == 1) {
                aux_id_ = id;
                break;
            }
        }
    }
    // 0x4D0040: its volume (low word), else -1.
    volume_ = 0xFFFFFFFFu;
    std::uint32_t v = 0;
    if (aux_id_ >= 0 && env_.aux && env_.aux->get(aux_id_, v)) volume_ = v & 0xFFFFu;
}

std::uint32_t CdMusic::send(const std::string& c, std::string* reply, int size, bool notify) {
    return env_.device ? env_.device->send(c, reply, size, notify) : 263u;  // (MCIERR_INVALID_DEVICE_NAME)
}

std::int32_t CdMusic::aux_volume() {
    std::uint32_t v = 0;
    if (aux_id_ >= 0 && env_.aux && env_.aux->get(aux_id_, v)) return static_cast<std::int32_t>(v & 0xFFFFu);
    return -1;
}

bool CdMusic::set_volume(std::int32_t volume, bool fading) {
    if (fade_step_ != 0 && !fading) return true;
    if (volume < 0) volume = 0;
    if (volume > 0xFFFF) volume = 0xFFFF;
    if (!fading) volume_ = static_cast<std::uint32_t>(volume);
    const std::uint32_t v = static_cast<std::uint32_t>(volume);
    // auxSetVolume(+0x14, right << 16 | left); -1 (no CD-audio device) fails.
    const bool ok = env_.aux && aux_id_ >= 0 && env_.aux->set(aux_id_, v << 16 | v);
    return ok;
}

void CdMusic::reset_stopped() {
    current_ = count_ != 0 ? 1 : 0;
    state_ = 0;
    fade_step_ = 0;
    if (env_.timers) {
        env_.timers->kill(delay_timer_);
        env_.timers->kill(fade_timer_);
    }
    fade_timer_ = -1;
    delay_timer_ = -1;
}

bool CdMusic::open() {
    if (open_) return true;
    player_window_ = 0;
    // (the hook's object, 0x51FF14, is this one)
    for (int i = 0; i < 100; ++i) types_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 4 + 1);
    chosen_ = 1;
    current_ = 0;
    serial_ = 0;
    on_disc_ = nullptr;
    mode_ = 1;
    mood_ = 0;
    open_ = false;
    count_ = 0;
    data_offset_ = 0;
    if (send("open cdaudio") != 0) {
        // Another program holds the drive: the Windows CD player's window is looked for, then
        // the device asked again.
        if (env_.find_player_window) player_window_ = env_.find_player_window();
        if (send("open cdaudio") != 0) return false;
    }
    send("stop cdaudio");
    state_ = 0;
    current_ = count_ != 0 ? 1 : 0;
    fade_step_ = 0;
    if (env_.timers) {
        env_.timers->kill(delay_timer_);
        env_.timers->kill(fade_timer_);
    }
    fade_timer_ = -1;
    delay_timer_ = -1;
    if (send("set cdaudio time format milliseconds") != 0) {
        if (open_) {
            send("stop cdaudio");
            send("close cdaudio");
            open_ = false;
        }
        return false;
    }
    serial_ = 0;
    count_ = probe();
    // (the window procedure's hook is installed here: display +0x80 = 0x4CE030)
    open_ = true;
    chosen_ = 1;
    current_ = 0;
    on_disc_ = nullptr;
    mode_ = 1;
    mood_ = 0;
    enabled_ = true;
    return true;
}

void CdMusic::close() {
    if (!open_) return;
    send("stop cdaudio");
    send("close cdaudio");
    open_ = false;
}

int CdMusic::probe() {
    current_ = 0;
    state_ = 0;
    if (env_.drives) {
        const char drive = next_cdrom_drive(*env_.drives, 0);
        if (drive != 0) serial_ = cd_volume_serial(*env_.drives, drive);
    }
    const std::uint32_t r = send("status cdaudio number of tracks", &tracks_buf_, 0x20);
    // The shipped program's patch (0x4CDA44 -> 0x4FB92C): when the call left the zero flag set
    // the answer becomes "17" (the replacement library always does); otherwise the routine
    // returns 0 and keeps the old track count.
    if (r != 0) return 0;
    tracks_buf_ = "17";
    count_ = c_atoi(tracks_buf_);
    send("set cdaudio time format tmsf");
    const std::uint32_t t = send("status cdaudio type track 1", &type_buf_, 0x20);
    bool audio_first = false;
    if (t == 0 && type_buf_ == "audio") audio_first = true;
    if (audio_first) {
        data_offset_ = 0;
    } else {
        // A data track first (or no answer): the tracks count from the second.
        data_offset_ = 1;
        --count_;
        if (count_ < 0) count_ = 0;
    }
    if (count_ == 0) return 0;
    current_ = 1;
    return count_;
}

void CdMusic::set_disc_callback(std::function<void()> fn) {
    on_disc_ = std::move(fn);
    probe();
    if (on_disc_) on_disc_();
}

bool CdMusic::device_playing() {
    if (send("status cdaudio mode", &playing_buf_, 0x40) != 0) return false;
    return is_playing_text(playing_buf_);
}

bool CdMusic::first_track_not_audio() {
    send("set cdaudio time format tmsf");
    if (send("status cdaudio type track 1", &check_type_buf_, 0x20) != 0) return true;
    return check_type_buf_ != "audio";
}

void CdMusic::pick() {
    if (count_ == 0) return;
    if (mood_ == 4) {
        send("stop cdaudio");
        reset_stopped();
        return;
    }
    if (state_ == 2) return;
    auto status_playing = [this]() {
        if (send("status cdaudio mode", &pick_mode_buf_, 0x40) != 0) return false;
        return is_playing_text(pick_mode_buf_);
    };
    bool by_mood = mood_ == 2 || mood_ == 3;
    if (!by_mood) {
        switch (mode_) {
        case 0:
            if (state_ == 0) return;
            state_ = 0;
            if (!status_playing()) return;
            send("stop cdaudio");
            reset_stopped();
            return;
        case 1:
            if (!status_playing()) {
                if (current_ < 1) current_ = 1;
                else ++current_;
                play_track(current_);
                if (count_ < current_) current_ = 1;
            }
            break;
        case 2:
            if (!status_playing()) {
                const int r = static_cast<int>(env_.rand ? env_.rand() : 0u);
                play_track(r % count_ + 1);
            }
            break;
        case 3:
            if (!status_playing() || current_ != chosen_) {
                if (chosen_ == 0) chosen_ = 1;
                play_track(chosen_);
            }
            break;
        case 4: by_mood = true; break;
        default: break;
        }
    }
    if (by_mood) {
        int r = static_cast<int>((env_.rand ? env_.rand() : 0u) & 15u);
        const bool playing = status_playing();
        const int want = mood_;
        if (!(playing && current_ >= 0 && current_ < 100 && types_[static_cast<std::size_t>(current_)] == want)) {
            int steps = (r + 1) * count_;
            int t = current_;
            bool found = false;
            while (steps > 0) {
                ++t;
                if (count_ < t) t = 1;
                if (types_[static_cast<std::size_t>(t)] == want) {
                    --r;
                    if (r < 1) {
                        found = true;
                        break;
                    }
                }
                --steps;
            }
            if (found) {
                play_track(t);
            } else {
                // Nothing of the mood: stopped (then the end's volume and state all the same).
                send("stop cdaudio");
                state_ = 0;
                current_ = count_ != 0 ? 1 : 0;
                fade_step_ = 0;
                if (env_.timers) {
                    env_.timers->kill(delay_timer_);
                    env_.timers->kill(fade_timer_);
                }
                fade_timer_ = -1;
                delay_timer_ = -1;
            }
        }
    }
    set_volume(static_cast<std::int32_t>(volume_), true);
    state_ = 1;
}

void CdMusic::delay_done() {
    if (env_.timers) env_.timers->kill(delay_timer_);
    delay_timer_ = -1;
    pick();
}

void CdMusic::fade_tick() {
    fade_level_ += fade_step_;
    if (0 < fade_level_) {
        set_volume(fade_level_, true);
        return;
    }
    if (env_.timers) env_.timers->kill(fade_timer_);
    fade_timer_ = -1;
    fade_level_ = 0;
    fade_step_ = 0;
    set_volume(fade_level_, true);
    if (mood_ == 0) {
        // Building: 120 ticks of the faded track before the pick.
        if (env_.timers) delay_timer_ = env_.timers->add(0x78, 0, [this](int) { delay_done(); });
        return;
    }
    pick();
}

void CdMusic::set_mood(int mood) {
    const int old = mood_;
    if (old == mood) return;
    if (-1 < old && old < 5) mood_track_[static_cast<std::size_t>(old)] = current_;
    mood_ = mood;
    if (!(mode_ == 4 || mood == 2 || mood == 3)) return;
    fade_level_ = static_cast<std::int32_t>(volume_);
    if (old == 4) {
        // From silence: the pick at once at the set volume.
        if (-1 < fade_timer_) {
            if (env_.timers) env_.timers->kill(fade_timer_);
            fade_timer_ = -1;
        }
        if (-1 < delay_timer_) {
            if (env_.timers) env_.timers->kill(delay_timer_);
            delay_timer_ = -1;
        }
        set_volume(static_cast<std::int32_t>(volume_), false);
        pick();
        return;
    }
    if (-1 < fade_timer_) {
        // A fade under way: both timers go and the pick is at once (the fade's step stays).
        if (env_.timers) env_.timers->kill(fade_timer_);
        fade_timer_ = -1;
        if (env_.timers) env_.timers->kill(delay_timer_);
        delay_timer_ = -1;
        pick();
        return;
    }
    // The fade: every 2 ticks the volume falls by a step of -(volume / 18), cut toward 0:
    // hi32(volume x -0x38E38E39) (a floor), then a signed division by 4.
    const std::int64_t prod = static_cast<std::int64_t>(static_cast<std::int32_t>(volume_)) * static_cast<std::int64_t>(-0x38E38E39);
    const std::int32_t hi = static_cast<std::int32_t>(prod >> 32);
    fade_step_ = (hi >> 2) - (hi >> 31);
    if (env_.timers) fade_timer_ = env_.timers->add(2, 0, [this](int) { fade_tick(); });
}

bool CdMusic::play_track(int track) {
    if (!enabled_) return true;
    state_ = 1;
    if (track == 0) {
        pick();
        return true;
    }
    bool playing = false;
    if (send("status cdaudio mode", &play_mode_buf_, 0x40) == 0) playing = is_playing_text(play_mode_buf_);
    if (playing && track == current_) return true;
    const int offset = data_offset_;
    current_ = track;
    const int from = track + offset;
    set_volume(static_cast<std::int32_t>(volume_), true);
    if (send("set cdaudio time format tmsf") != 0) return false;
    std::string c = format_int("play cdaudio from %i", from);
    if (from < count_) c += format_int(" to %i", from + 1);
    c += " notify";
    const std::uint32_t r = send(c, nullptr, 0, true);
    send("set cdaudio time format milliseconds");
    return r == 0;
}

bool CdMusic::stop() {
    const std::uint32_t r = send("stop cdaudio");
    reset_stopped();
    return r == 0;
}

void CdMusic::set_enabled(bool on) {
    enabled_ = on;
    if (!on) {
        send("stop cdaudio");
        reset_stopped();
    }
}

bool CdMusic::pause(bool hold) {
    if (!enabled_) return true;
    if (state_ == 0) return true;
    std::string c;
    if (!hold) {
        send("status cdaudio current track", &resume_buf_, 200, true);
        const int t = c_atoi(resume_buf_);
        c = "play cdaudio";
        if (t < count_) {
            c += " from ";
            send("status cdaudio position", &resume_buf_, 200, true);
            c += resume_buf_;
            send(format_int("status cdaudio position track %i", t + 1), &resume_buf_, 200, true);
            c += " to ";
            c += resume_buf_;
        }
        c += " notify";
        state_ = 1;
    } else {
        c = "pause cdaudio";
        state_ = 2;
    }
    return send(c, nullptr, 0, true) == 0;
}

int CdMusic::step_track(int track) {
    if (count_ == 0) return 0;
    if (count_ < track) track = track % count_;
    if (state_ == 1) {
        play_track(track);
        return current_;
    }
    current_ = track;
    return track;
}

void CdMusic::set_type(int track, std::uint8_t type) {
    if (track >= 0 && track < 100) types_[static_cast<std::size_t>(track)] = type;
}

std::uint8_t CdMusic::type(int track) const {
    return track >= 0 && track < 100 ? types_[static_cast<std::size_t>(track)] : 0;
}

void CdMusic::set_types(const std::uint8_t* types) {
    for (int i = 0; i < count_ && i + 1 < 100; ++i) types_[static_cast<std::size_t>(i + 1)] = types[i];
}

void CdMusic::set_chosen(int track) {
    if (track <= count_) chosen_ = track;
}

void CdMusic::window_message(std::uint32_t message, std::uintptr_t wparam) {
    if (message == 0x219) {
        // WM_DEVICECHANGE: stopped; a disc that arrived (DBT_DEVICEARRIVAL) is probed and the
        // disc routine runs.
        send("stop cdaudio");
        reset_stopped();
        if (wparam == 0x8000) {
            probe();
            if (on_disc_) on_disc_();
        }
        return;
    }
    if (message == 0x3B9 && wparam == 1 && state_ == 1) {
        // MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL: the track ended; the next unless still playing.
        if (send("status cdaudio mode", &notify_mode_buf_, 0x40) == 0 && is_playing_text(notify_mode_buf_)) return;
        pick();
    }
}

void CdMusic::close_player_window() {
    if (player_window_ == 0) return;
    if (env_.close_player_window) env_.close_player_window(player_window_);
    player_window_ = 0;
}

// ---------------------------------------------------------------- track lists

std::uint32_t CdTrackLists::serial(std::size_t entry) const {
    const std::uint8_t* p = bytes.data() + entry * kEntry + 0x20;
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

void CdTrackLists::set_serial(std::size_t entry, std::uint32_t s) {
    std::uint8_t* p = bytes.data() + entry * kEntry + 0x20;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(s >> (8 * i));
}

std::string CdTrackLists::to_text() const {
    static const char* const hex = "0123456789abcdef";
    std::string t;
    t.reserve(kSize * 2);
    for (std::uint8_t b : bytes) {
        t += hex[b >> 4];
        t += hex[b & 15];
    }
    return t;
}

void CdTrackLists::from_text(const std::string& text) {
    bytes.fill(0);
    if (text.size() != kSize * 2) return;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, kSize> b{};
    for (std::size_t i = 0; i < kSize; ++i) {
        const int hi = nib(text[i * 2]), lo = nib(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return;
        b[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    bytes = b;
}

void cd_disc_changed(CdMusic& m, CdTrackLists& lists, bool enabled, int mode, bool battle_running) {
    static const std::uint8_t kDefaults[16] = {1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const int mood = m.mood();
    m.command("stop cdaudio");
    m.command("close cdaudio");
    m.command("open cdaudio");
    m.set_enabled(enabled);
    m.set_mode(mode);
    m.set_mood(mood);
    const std::uint32_t serial = m.serial();
    std::size_t i = 0;
    while (i < CdTrackLists::kEntries && lists.serial(i) != serial) ++i;
    const std::size_t E = CdTrackLists::kEntry;
    if (i < CdTrackLists::kEntries) {
        std::array<std::uint8_t, CdTrackLists::kEntry> tmp{};
        std::copy(lists.bytes.begin() + static_cast<std::ptrdiff_t>(i * E), lists.bytes.begin() + static_cast<std::ptrdiff_t>((i + 1) * E), tmp.begin());
        for (std::size_t k = i; k > 0; --k)
            std::copy(lists.bytes.begin() + static_cast<std::ptrdiff_t>((k - 1) * E), lists.bytes.begin() + static_cast<std::ptrdiff_t>(k * E),
                      lists.bytes.begin() + static_cast<std::ptrdiff_t>(k * E));
        std::copy(tmp.begin(), tmp.end(), lists.bytes.begin());
        m.set_types(lists.types(0));
    } else {
        if (m.tracks() == 16 && m.first_track_not_audio()) m.set_types(kDefaults);
        for (std::size_t k = CdTrackLists::kEntries - 1; k > 0; --k)
            std::copy(lists.bytes.begin() + static_cast<std::ptrdiff_t>((k - 1) * E), lists.bytes.begin() + static_cast<std::ptrdiff_t>(k * E),
                      lists.bytes.begin() + static_cast<std::ptrdiff_t>(k * E));
        std::copy(kDefaults, kDefaults + 16, lists.types(0));
        lists.set_serial(0, serial);
    }
    if (battle_running) m.pick();
    else m.stop();
}

void cd_keep_types(CdMusic& m, CdTrackLists& lists) {
    for (int k = 0; k < m.tracks() && k < 100; ++k) lists.types(0)[k] = m.type(k + 1);
}

// ---------------------------------------------------------------- battle / peace judge

void BattleMusicJudge::reset() {
    // (the battle's start clears the ring and its slot only, 0x4919FC / 0x491A56; the rest is
    // the program's and carries over from battle to battle)
    ring_.fill(0);
    slot_ = 0;
}

void BattleMusicJudge::update(std::uint32_t now, int units, CdMusic& music) {
    if (!(last_ + 0x1e < now)) return;
    const int cur = music.mood();
    ++seconds_since_switch_;
    if (10 < seconds_since_switch_) {
        int all = 0, recent = 0, five = 5;
        int k = slot_ - 1;
        while (true) {
            if (k < 0) k += 30;
            const int v = ring_[static_cast<std::size_t>(k)];
            all += v;
            if (five != 0) {
                --five;
                recent += v;
            }
            if (k == slot_) break;
            --k;
        }
        int want;
        if (cur == 0 && (50 < all || 30 < recent) && 30 < units) {
            want = 1;
        } else {
            want = current_;
            if (cur == 1 && all < 10 && recent == 0 && 60 < seconds_since_switch_) want = 0;
        }
        if (want != current_) {
            music.set_mood(want);
            seconds_since_switch_ = 0;
            current_ = want;
        }
    }
    ++slot_;
    if (0x1d < slot_) slot_ = 0;
    ring_[static_cast<std::size_t>(slot_)] = 0;
    last_ = now;
}

}  // namespace client
