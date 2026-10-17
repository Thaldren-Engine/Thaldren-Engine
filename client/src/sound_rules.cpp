// When the battle client starts a sound (spec: docs/RE_MEMORY.md, "Client", "World sounds",
// "Unit notices").

#include "client/sound_rules.h"

#include "client/sound_mixer.h"

namespace client {

namespace {
int hi16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); }

constexpr NoticeRule kNotices[24] = {
    {0, 0, nullptr},              {10, 0, nullptr},   {9, 20, "Under Attack"}, {4, 2, nullptr},
    {4, 2, nullptr},              {5, 1, nullptr},    {3, 4, "Arrived"},       {8, 1, "Cannot Comply"},
    {3, 3, "Nanolathe Complete"}, {4, 2, nullptr},    {3, 1, nullptr},         {2, 1, nullptr},
    {7, 1, nullptr},              {7, 1, nullptr},    {7, 1, "Cloaked"},       {7, 1, "Visible"},
    {4, 1, nullptr},              {10, 0, "five"},    {10, 0, "four"},         {10, 0, "three"},
    {10, 0, "two"},               {10, 0, "one"},     {10, 0, "zero"},         {10, 0, "Self destruct terminated"},
};
}  // namespace

const NoticeRule& notice_rule(std::uint32_t id) { return kNotices[id < 24 ? id : 0]; }

void world_sound_block(const std::array<std::int32_t, 3>& pos, int& bx, int& bz) {
    bx = hi16(pos[0]) >> 5;
    bz = (hi16(pos[2]) - (hi16(pos[1]) >> 1)) >> 5;
}

bool world_sound(const SoundView& v, const std::array<std::int32_t, 3>& pos, const std::function<bool()>& seen, int sample, SoundEvent& out) {
    if (sample < 0 || v.fx_volume == 0 || v.sound_mode == 0) return false;
    // On the map (its 16-pixel cells, x and z rounded towards zero).
    const std::int32_t cx = pos[0] / (1 << 20), cz = pos[2] / (1 << 20);
    if (cx < 0 || cz < 0 || cx >= v.map_w || cz >= v.map_h) return false;
    if (!seen()) return false;
    const int hx = hi16(pos[0]), hy = hi16(pos[1]), hz = hi16(pos[2]);
    out = SoundEvent{};
    out.sample = sample;
    out.volume = kSoundVolumeNormal;
    if (v.sound_mode == 2) {
        // 3D: relative to the view's centre (x right, z towards the top of the screen, no
        // height), full volume within half the view's size, quieter up to the map's width +
        // height (and no quieter beyond).
        out.placed = true;
        out.pos = {static_cast<float>(hx - (v.view_w / 2) * 16 - v.scroll_x), 0.0f,
                   static_cast<float>(v.scroll_y + (v.view_h / 2) * 16 + (hy >> 1) - hz)};
        out.min_distance = static_cast<float>(((v.view_w + v.view_h) / 2) << 4);
        out.max_distance = static_cast<float>((v.map_w + v.map_h) * 16);
    } else if (!(v.scroll_x <= hx && v.scroll_y <= hz && hx <= v.scroll_x + v.view_w * 16 && hz <= v.scroll_y + v.view_h * 16)) {
        // Mono: centred; away from the view's rectangle (x and z, height not counted) 10 dB
        // quieter.
        out.volume = kSoundVolumeOutOfView;
    }
    return true;
}

void NoticeQueue::queue(Host& h, const Levels& l, std::uint16_t unit, std::uint32_t id) {
    if (id == 0 || id >= 24) return;
    const std::uint32_t tick = h.tick();
    if (next_[id] > tick) return;  // still within its gap
    for (const Entry& e : entries_)
        if (e.id == id) return;  // one of its kind queued already
    if (entries_.size() == 8) {
        // A full queue: its last entry only shows its text, and goes.
        play(h, l, 7, false, true);
        entries_.pop_back();
    }
    Entry e;
    e.id = id;
    e.tick = tick;
    e.unit = unit;
    e.priority = kNotices[id].priority;
    if (kNotices[id].text) e.text = kNotices[id].text;
    // Before the first entry of lower priority.
    std::size_t at = 0;
    while (at < entries_.size() && entries_[at].priority >= e.priority) ++at;
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
}

void NoticeQueue::play(Host& h, const Levels& l, std::size_t index, bool sound, bool text) {
    const Entry& e = entries_[index];
    const int count = h.sound_count(e.unit, e.id);
    // One of the notice's sounds, by the simulation thread's rand() (drawn every time).
    const std::uint32_t r = h.rand();
    const int pick = static_cast<int>(static_cast<std::int64_t>(r) * count / 0x8000);
    if (e.priority > 10 - l.unit_chat && count > 0 && sound && l.speech) {
        h.speak(e.unit, e.id, pick);
        next_[e.id] = h.tick() + static_cast<std::uint32_t>(kNotices[e.id].gap) * 30u;
    }
    if (text && e.priority > 10 - l.unit_chat_text && !e.text.empty() && h.unit_alive(e.unit)) h.show(e.unit, e.text);
}

void NoticeQueue::frame(Host& h, const Levels& l) {
    if (entries_.empty()) return;
    const std::uint32_t tick = h.tick();
    if (tick < last_ + length_) {
        play(h, l, 0, false, true);
    } else {
        play(h, l, 0, true, true);
        last_ = tick;
    }
    entries_.erase(entries_.begin());
}

void NoticeQueue::forget(std::uint16_t unit) {
    std::vector<Entry> kept;
    for (Entry& e : entries_)
        if (e.unit != unit) kept.push_back(std::move(e));
    entries_.swap(kept);
}

}  // namespace client
