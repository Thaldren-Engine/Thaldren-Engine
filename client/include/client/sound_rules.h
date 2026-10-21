#pragma once
// When the battle client starts a sound, and how (spec: docs/RE_MEMORY.md, "Client", "Sound
// engine", "World sounds", "Unit notices"): the world sounds' sight, view and volume rules and
// the unit notices' queue. Pure rules on plain inputs (the battle client feeds them; the sound
// harness feeds the same inputs to the original). Presentation only, except the notices' draw
// of the simulation thread's rand(), which the original makes too.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace client {

// A sound the client starts (index into the content's samples), as the original hands it to
// its sound manager: the volume in hundredths of a decibel and, in 3D mode, the place relative
// to the listener with the distance model's limits (client/sound_mixer.h).
struct SoundEvent {
    int sample = -1;
    int volume = -585;
    bool placed = false;
    std::array<float, 3> pos{};
    float min_distance = 1.0f, max_distance = 1.0f;
    bool one_shot = false;  // a unit notice: its own buffer
};

// The settings and the view a world sound is judged by.
struct SoundView {
    int fx_volume = 27;   // 0..64
    int sound_mode = 1;   // 0 off, 1 mono, 2 3D
    int scroll_x = 0, scroll_y = 0;
    int view_w = 32, view_h = 26;  // the battle view in 16-pixel cells
    int map_w = 0, map_h = 0;      // the map in 16-pixel cells
};

// The sight block (32 x 32 pixels) a world sound at `pos` (16.16) is judged by: x, and z less
// half the height.
void world_sound_block(const std::array<std::int32_t, 3>& pos, int& bx, int& bz);

// A world sound at `pos` (0x47F300): nothing without effect volume or sound mode, off the map,
// or where `seen` says the viewed player does not see it (asked only when it is on the map);
// mono mode: centred, 5.85 dB down in the view and 15.85 dB down away from it; 3D mode: placed
// relative to the view's centre. False when nothing starts.
bool world_sound(const SoundView& v, const std::array<std::int32_t, 3>& pos, const std::function<bool()>& seen, int sample, SoundEvent& out);

// The unit notices' table (by the simulation's speech id 1..23): priority, the shortest gap
// between two sounds of the notice in seconds of game time, and the console text.
struct NoticeRule {
    int priority, gap;
    const char* text;
};
const NoticeRule& notice_rule(std::uint32_t id);

// The unit notices' queue (0x47FAD0 queue, 0x47FD70 play, 0x47F680 per frame, 0x47F8C0 forget):
// eight entries by priority; one taken per frame.
class NoticeQueue {
public:
    struct Host {
        virtual ~Host() = default;
        virtual std::uint32_t tick() = 0;    // the game step
        virtual std::uint32_t rand() = 0;    // the simulation thread's rand() (drawn for every notice taken)
        virtual int sound_count(std::uint16_t unit, std::uint32_t id) = 0;  // sounds of the unit's notice
        // The notice sounds: the pick-th of its sounds (a one-shot, 5.85 dB down, when there are
        // effects at all).
        virtual void speak(std::uint16_t unit, std::uint32_t id, int pick) = 0;
        virtual bool unit_alive(std::uint16_t unit) = 0;  // the alive flag (texts need it)
        virtual void show(std::uint16_t unit, const std::string& text) = 0;  // "<name>: <text>" on the console
    };
    struct Levels {
        int unit_chat = 10, unit_chat_text = 5;  // 0..10: notices of priority above 10 - level speak / show
        bool speech = true;                      // the speech switch
    };
    // Queues notice `id` of `unit` (the caller has checked the unit: the viewed player's, alive,
    // not dying). `text`, when given, is shown instead of the table's text (0x47F780: the
    // caller's text replaces it; translated either way).
    void queue(Host& h, const Levels& l, std::uint16_t unit, std::uint32_t id, const char* text = nullptr);
    // The text a queued entry shows (tests).
    const std::string& text_at(std::size_t index) const { return entries_[index].text; }
    // The frame's notice: it speaks unless the last one spoken is still within its 30 steps,
    // then only its text shows.
    void frame(Host& h, const Levels& l);
    // A unit's queued notices go (it died).
    void forget(std::uint16_t unit);
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::uint32_t id = 0, tick = 0;
        std::uint16_t unit = 0;
        int priority = 0;
        std::string text;
    };
    void play(Host& h, const Levels& l, std::size_t index, bool sound, bool text);
    std::vector<Entry> entries_;                // highest priority first
    std::array<std::uint32_t, 24> next_{};      // per notice: the first step it may queue again
    std::uint32_t last_ = 0, length_ = 30;      // the last spoken notice's step; the steps a notice holds the voice
};

}  // namespace client
