// The playable client's input, selection, command and timing rules
// (spec: docs/RE_MEMORY.md, "Client").

#include "client/game_client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sim/features.h"
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/targeting.h"
#include "sim/unit_tick.h"
#include "sim/x87_trig.h"

namespace client {
namespace {

constexpr std::uint32_t kSelected = sim::unit_tick_flags::kSelected;
constexpr std::uint32_t kSelectable = sim::unit_tick_flags::kSelectable;
constexpr std::uint32_t kOnScreen = 0x40, kOnScreenOld = 0x80;  // interface marks in the unit flags
constexpr std::uint32_t kBuildMenuShown = 0x400000, kBuildPageShift = 23, kBuildPageMask = 7u << 23;
constexpr std::uint32_t kDoubleClickMs = 350;

int sgn_div8(int d) {
    // A step of an eased display value: an eighth of the difference (toward zero), at least one.
    if (d < 0) {
        int s = d / 8;
        return s == 0 ? -1 : s;
    }
    if (d < 1) return 0;
    int s = d / 8;
    return s == 0 ? 1 : s;
}

} // namespace

int button_frame(const Button& b, bool held, bool disabled, int stage) {
    const int n = b.frames ? static_cast<int>(b.frames->size()) : 0;
    if (n == 0) return -1;
    if (b.staged) {
        if (held) return n - 1;
        if (disabled) return std::max(0, n - 2);
        return std::clamp(stage, 0, std::max(0, n - 3));
    }
    if (disabled) return n >= 3 ? 2 : 0;
    if (held) return n >= 2 ? 1 : 0;
    return 0;
}

GameClient::GameClient(sim::Battle& battle, Content& content, const ClientOptions& options)
    : b_(battle), c_(content), opt_(options) {
    if (opt_.side >= 0 && static_cast<std::size_t>(opt_.side) < c_.sides.size()) side_ = &c_.sides[static_cast<std::size_t>(opt_.side)];
    budget_.game_speed = budget_.effective_speed = static_cast<std::uint16_t>(std::clamp(opt_.game_speed, 1, 20));
    for (const std::string& s : c_.start_messages) message(s);
    mouse_x_ = (opt_.width + 128) / 2;  // the pointer starts over the view's centre
    mouse_y_ = opt_.height / 2;
    const auto& me = b_.players[static_cast<std::size_t>(opt_.player)];
    shown_[0] = me.energy_stored;
    shown_[1] = me.metal_stored;
}

// ---- geometry ------------------------------------------------------------------------------

render::ClipRect GameClient::view_rect() const { return {128, 32, opt_.width - 1, opt_.height - 33}; }

bool GameClient::in_view(int x, int y) const {
    const render::ClipRect r = view_rect();
    return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

int GameClient::map_width_px() const { return b_.map.width * 16; }
int GameClient::map_height_px() const { return b_.map.height * 16; }

void GameClient::clamp_scroll() {
    const int vw = opt_.width - 128, vh = opt_.height - 64;
    scroll_x_ = std::max(0, std::min(scroll_x_, map_width_px() - vw));
    scroll_y_ = std::max(0, std::min(scroll_y_, map_height_px() - vh));
}

void GameClient::set_scroll(int x, int y) {
    scroll_x_ = x;
    scroll_y_ = y;
    clamp_scroll();
}

void GameClient::center_on(const sim::Vec3i& pos) {
    set_scroll((pos[0] >> 16) - (opt_.width - 128) / 2, (pos[2] >> 16) - (pos[1] >> 17) - (opt_.height - 64) / 2);
}

sim::Vec3i GameClient::world_at(int sx, int sy) const {
    // The screen point shows the ground point whose projection z - height / 2 lands on it.
    const int x = sx - 128 + scroll_x_;
    const int zs = sy - 32 + scroll_y_;
    int z = zs, h = 0;
    for (int k = 0; k < 4; ++k) {
        const int cx = std::clamp(x >> 4, 0, std::max(0, b_.map.width - 1));
        const int cz = std::clamp(z >> 4, 0, std::max(0, b_.map.height - 1));
        h = b_.map.cells.empty() ? 0 : b_.map.cells[static_cast<std::size_t>(cz * b_.map.width + cx)].height;
        z = zs + h / 2;
    }
    return {x << 16, h << 16, z << 16};
}

bool GameClient::own(const sim::Unit& u) const { return b_.ticks[u.id].owner == opt_.player; }

bool GameClient::selectable(const sim::Unit& u) const {
    const sim::UnitTickState& t = b_.ticks[u.id];
    return t.type_index != 0 && t.owner == opt_.player && (u.flags & kSelectable) && u.build_fraction_left == 0.0f &&
           t.transfer_holdoff == 0 && (!u.carrier || (u.carrier->flags & sim::unit_tick_flags::kCarrierSelectable));
}

std::uint16_t GameClient::unit_at(int sx, int sy) const {
    // The original's pick test: each drawn unit's model base rectangle (the bounds' four
    // bottom corners, turned with the unit, projected) must hold the point strictly inside
    // every edge; of several, the type with the smallest size rank wins (the first of equals).
    std::uint16_t best = 0;
    std::int64_t best_rank = 0x7fff0000;
    for (const UnitScreen& s : on_screen_) {
        if (s.id >= b_.units.size() || b_.ticks[s.id].type_index == 0) continue;
        const std::uint16_t t = b_.ticks[s.id].type_index;
        if (t >= c_.types.size()) continue;
        const UnitTypeLook& look = c_.types[t];
        const sim::Unit& u = b_.units[s.id];
        // The model's own bounds (its pieces at rest), as the original reads them from the model.
        const auto& mn = look.box_min;
        const auto& mx = look.box_max;
        const std::int32_t corners[4][3] = {{mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mx[0], mn[1], mx[2]}, {mn[0], mn[1], mx[2]}};
        const std::int16_t angles[3] = {u.pitch, static_cast<std::int16_t>(u.heading), u.bank};
        int px[4], py[4];
        for (int k = 0; k < 4; ++k) {
            std::int32_t r[3];
            sim::x87::rotate_point_3d(corners[k], r, angles);
            px[k] = static_cast<std::int16_t>(static_cast<std::uint32_t>(r[0] + u.pos[0] - scroll_x_ * 0x10000) >> 16) + 128;
            py[k] = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[2] - scroll_y_ * 0x10000 - r[2]) >> 16) -
                    (static_cast<std::int16_t>(static_cast<std::uint32_t>(r[1] + u.pos[1]) >> 16) >> 1) + 32;
        }
        bool inside = true;
        for (int k = 0; k < 4 && inside; ++k) {
            const int n = (k + 1) % 4;
            const int a = (px[n] - px[k]) * (sy - py[k]);
            const int b = (py[n] - py[k]) * (sx - px[k]);
            if (b - a == 0 || b < a) inside = false;
        }
        if (inside && look.pick_rank < best_rank) {
            best_rank = look.pick_rank;
            best = s.id;
        }
    }
    return best;
}
int GameClient::selected_count() const {
    int n = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        n += b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected);
    return n;
}

// ---- main loop -------------------------------------------------------------------------------

int GameClient::frame(std::uint32_t now_ms) {
    const std::uint32_t scaled = sim::StepBudget::scale_ms(now_ms);
    if (!started_) {
        started_ = true;
        budget_.last_scaled_tick = scaled;
        last_scaled_ = scaled;
    }
    const std::uint32_t dt = scaled - last_scaled_;
    last_scaled_ = scaled;
    int steps = budget_.update(now_ms, network.remote_lag ? network.remote_lag() : 0);
    if (network.remote_lag) steps = std::min(steps, 1);  // network games: one step per frame at most
    for (int k = 0; k < steps; ++k) step_battle(b_);
    end_frame(b_);  // once per frame, after the frame's steps (even none)
    // Console lines expire one per frame, each (text_seconds + 1) seconds after it came.
    if (!console_.empty() && console_.front().tick + static_cast<std::uint32_t>(opt_.text_seconds + 1) * 30u < b_.game_tick())
        console_.pop_front();
    scroll_step(dt);
    // The tracked unit keeps the view centred on it.
    if (tracked_) {
        if (tracked_ < b_.units.size() && b_.ticks[tracked_].type_index && b_.units[tracked_].hit_points > 0) center_on(b_.units[tracked_].pos);
        else tracked_ = 0;
    }
    hover_ = in_view(mouse_x_, mouse_y_) ? unit_at(mouse_x_, mouse_y_) : 0;
    if (!in_view(mouse_x_, mouse_y_) && on_minimap(mouse_x_, mouse_y_)) {
        // On the minimap: the nearest radar dot within 2 pixels (squared distance under 4).
        int bd = 99999;
        for (const UnitScreen& d : mm_dots_) {
            const int dd = (d.x - mouse_x_) * (d.x - mouse_x_) + (d.y - mouse_y_) * (d.y - mouse_y_);
            if (dd < 4 && dd < bd) {
                bd = dd;
                hover_ = d.id;
            }
        }
    }
    hover_feature_ = in_view(mouse_x_, mouse_y_) && !hover_ ? feature_under(mouse_x_, mouse_y_) : -1;
    speech_step();
    if (panel_dirty_) refresh_panel();
    ++frame_counter_;
    return steps;
}

void GameClient::scroll_step(std::uint32_t dt_ticks) {
    // Scroll speed times the frame's elapsed 1/30 s, at most 128 pixels: arrow keys, or the
    // pointer on the first/last row or column of the screen.
    int step = static_cast<int>(std::min<std::uint32_t>(static_cast<std::uint32_t>(opt_.scroll_speed) * dt_ticks, 128u));
    if (step == 0) return;
    int x = scroll_x_, y = scroll_y_;
    if (keys_down_[keys::kLeft] || (mouse_x_ == 0 && mouse_y_ < opt_.height)) x -= step;
    else if (keys_down_[keys::kRight] || mouse_x_ == opt_.width - 1) x += step;
    if (keys_down_[keys::kUp] || (mouse_y_ == 0 && mouse_x_ < opt_.width)) y -= step;
    else if (keys_down_[keys::kDown] || mouse_y_ == opt_.height - 1) y += step;
    if (x != scroll_x_ || y != scroll_y_) set_scroll(x, y);
}

void GameClient::minimap_jump(int x, int y) {
    // The minimap point becomes the view's centre.
    const int mw = map_width_px(), mh = map_height_px();
    if (mw <= 0 || mh <= 0) return;
    const int size = 126;
    int w = size, h = size, ox = 0, oy = 0;
    if (mw < mh) {
        w = mw * size / mh;
        ox = (size - w) / 2;
    } else {
        h = mh * size / mw;
        oy = (size - h) / 2;
    }
    set_scroll((x - ox) * mw / std::max(w, 1) - (opt_.width - 128) / 2, (y - oy) * mh / std::max(h, 1) - (opt_.height - 64) / 2);
}

// ---- sounds and messages ---------------------------------------------------------------------

void GameClient::play_ui(int sample) {
    if (sample >= 0) sounds_.push_back({sample, 0});
}

std::vector<SoundEvent> GameClient::take_sounds() {
    std::vector<SoundEvent> out;
    out.swap(sounds_);
    return out;
}

void GameClient::on_sound(std::uint16_t sample, const sim::Vec3i& pos) {
    // World sounds play when the viewed player sees the spot (sight counts with the line of
    // sight option, else the explored map; the block of x and z minus half the height).
    // Mono mode plays them centred at full volume wherever they are; 3D mode places them
    // around the view's centre, full volume within half the view's size and falling off
    // with the distance beyond it, silent past the map's size.
    if (sample == 0xFFFF || sample >= c_.samples.size() || opt_.sound_mode == 0) return;
    if (!b_.point_seen_by(opt_.player, pos)) return;
    if (opt_.sound_mode != 2) {
        sounds_.push_back({sample, 0, 256});
        return;
    }
    const int vw = (opt_.width - 128) / 16, vh = (opt_.height - 64) / 16;
    const double rx = (pos[0] >> 16) - (vw / 2) * 16 - scroll_x_;
    const double ry = scroll_y_ + (vh / 2) * 16 + ((pos[1] >> 16) >> 1) - (pos[2] >> 16);
    const double dist = std::sqrt(rx * rx + ry * ry);
    const double near_d = ((vw + vh) / 2) * 16.0, far_d = (b_.map.width + b_.map.height) * 16.0;
    if (dist > far_d) return;
    const int volume = dist <= near_d ? 256 : static_cast<int>(256.0 * near_d / dist);
    const int pan = dist > 0 ? static_cast<int>(std::clamp(rx / dist * 100.0, -100.0, 100.0)) : 0;
    sounds_.push_back({sample, pan, volume});
}
void GameClient::speak(const sim::Unit& u, std::uint32_t id) {
    const std::uint16_t t = b_.ticks[u.id].type_index;
    if (t >= c_.types.size() || id >= c_.types[t].sounds.size()) return;
    play_ui(c_.types[t].sounds[id]);
}

namespace {
// The original's notice table: priority, minimum gap between two sounds of the notice (in
// seconds of game time) and the console text (id = the simulation's speech id).
struct SpeechRule {
    int priority, gap;
    const char* text;
};
constexpr SpeechRule kSpeech[24] = {
    {0, 0, nullptr},           {10, 0, nullptr},        {9, 20, "Under Attack"}, {4, 2, nullptr},
    {4, 2, nullptr},           {5, 1, nullptr},         {3, 4, "Arrived"},       {8, 1, "Cannot Comply"},
    {3, 3, "Nanolathe Complete"}, {4, 2, nullptr},      {3, 1, nullptr},         {2, 1, nullptr},
    {7, 1, nullptr},           {7, 1, nullptr},         {7, 1, "Cloaked"},       {7, 1, "Visible"},
    {4, 1, nullptr},           {10, 0, "five"},         {10, 0, "four"},         {10, 0, "three"},
    {10, 0, "two"},            {10, 0, "one"},          {10, 0, "zero"},         {10, 0, "Self destruct terminated"},
};
constexpr int kSpeechVolume = 131;  // notices play 5.85 dB below full volume
}  // namespace

void GameClient::on_speech(sim::Unit& u, std::uint32_t id) {
    // Notices of the viewed player's own live units join the queue, unless the notice is
    // still in its gap or already queued; a full queue (8) drops its last entry, which
    // only shows its text.
    if (id == 0 || id >= 24 || b_.ticks[u.id].type_index == 0 || !own(u) || u.hit_points <= 0) return;
    const std::uint32_t tick = b_.game_tick();
    if (speech_next_[id] > tick) return;
    for (const Speech& s : speech_)
        if (s.id == id) return;
    if (speech_.size() == 8) {
        speech_play(7, false, true);
        speech_.pop_back();
    }
    Speech s;
    s.id = id;
    s.tick = tick;
    s.unit = u.id;
    s.priority = static_cast<std::uint8_t>(kSpeech[id].priority);
    if (kSpeech[id].text) s.text = kSpeech[id].text;
    std::size_t at = 0;
    while (at < speech_.size() && kSpeech[speech_[at].id].priority >= kSpeech[id].priority) ++at;
    speech_.insert(speech_.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
}

void GameClient::speech_play(std::size_t index, bool sound, bool text) {
    const Speech& s = speech_[index];
    const std::uint16_t t = b_.ticks[s.unit].type_index;
    const std::vector<int>* list = t < c_.types.size() ? &c_.types[t].sound_lists[s.id] : nullptr;
    const int count = list ? static_cast<int>(list->size()) : 0;
    // One of the notice's sounds, by the simulation thread's rand() (drawn every time).
    const std::uint32_t r = b_.crt_rand.next();
    const int pick = static_cast<int>(static_cast<std::int64_t>(r) * count / 0x8000);
    if (static_cast<int>(s.priority) > 10 - opt_.unit_chat && count > 0 && sound && opt_.speech) {
        const int sample = (*list)[static_cast<std::size_t>(pick)];
        if (sample >= 0) {
            sounds_.push_back({sample, 0, kSpeechVolume});
            const Sample& sm = c_.samples[static_cast<std::size_t>(sample)];
            speech_length_ = sm.rate > 0 ? static_cast<std::uint32_t>(sm.pcm.size() / static_cast<std::size_t>(std::max(1, sm.channels)) * 30 /
                                                                    static_cast<std::size_t>(sm.rate))
                                         : 0;
        }
        speech_next_[s.id] = b_.game_tick() + static_cast<std::uint32_t>(kSpeech[s.id].gap) * 30u;
    }
    if (text && static_cast<int>(s.priority) > 10 - opt_.unit_chat_text && !s.text.empty() && t < c_.types.size())
        message(c_.types[t].name + ": " + s.text);
}

void GameClient::speech_step() {
    // One queued notice per frame: it speaks unless the last spoken one is still playing
    // (then only its text shows).
    if (speech_.empty()) return;
    const std::uint32_t tick = b_.game_tick();
    if (tick < speech_last_ + speech_length_) {
        speech_play(0, false, true);
    } else {
        speech_play(0, true, true);
        speech_last_ = tick;
    }
    speech_.erase(speech_.begin());
}

void GameClient::forget_speech(sim::Unit& u) {
    speech_.erase(std::remove_if(speech_.begin(), speech_.end(), [&](const Speech& s) { return s.unit == u.id; }), speech_.end());
    if (tracked_ == u.id) tracked_ = 0;
}

void GameClient::on_under_attack(sim::Unit& u) {
    // Spoken unless the unit is on screen.
    for (const UnitScreen& s : on_screen_)
        if (s.id == u.id) return;
    on_speech(u, 2);
}
void GameClient::message(const std::string& text, bool highlight) {
    if (text.empty()) return;
    console_.push_back({text.substr(0, 64), b_.game_tick(), highlight});
    while (console_.size() > 30) console_.pop_front();
}

// ---- selection ---------------------------------------------------------------------------------

void GameClient::deselect_all() {
    // Every unit of the table loses the selection and on-screen marks.
    for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kSelected | kOnScreen | kOnScreenOld);
    focus_ = 0;
    panel_dirty_ = true;
}

void GameClient::mark_on_screen() {
    for (const UnitScreen& s : on_screen_)
        if (s.id < b_.units.size() && b_.ticks[s.id].owner == opt_.player)
            b_.units[s.id].flags = (b_.units[s.id].flags & ~kOnScreenOld) | kOnScreen;
}

void GameClient::click_select(const InputEvent& e) {
    if (!hover_) return;
    sim::Unit& u = b_.units[hover_];
    if (!selectable(u)) return;
    const bool dbl = hover_ == last_click_unit_ && e.time_ms - last_click_ms_ < kDoubleClickMs;
    last_click_ms_ = e.time_ms;
    last_click_unit_ = hover_;
    if (dbl) {
        select_same_type_on_screen(b_.ticks[hover_].type_index);
        return;
    }
    if (e.shift) {
        u.flags ^= kSelected;
        if (u.flags & kSelected) speak(u, 1);
        focus_ = 0;
        panel_dirty_ = true;
        return;
    }
    for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kSelected | kOnScreen | kOnScreenOld);
    mark_on_screen();
    u.flags |= kSelected;
    speak(u, 1);
    focus_ = 0;
    panel_dirty_ = true;
}

void GameClient::select_same_type_on_screen(std::uint16_t type) {
    int n = 0;
    std::uint16_t last = 0;
    for (const UnitScreen& s : on_screen_) {
        sim::Unit& u = b_.units[s.id];
        if (b_.ticks[s.id].type_index != type || !selectable(u)) continue;
        u.flags |= kSelected;
        ++n;
        last = s.id;
    }
    if (n == 1) speak(b_.units[last], 1);
    else if (n > 1) play_ui(c_.ui_sounds.select_multiple);
    focus_ = 0;
    panel_dirty_ = true;
}

void GameClient::box_select(const InputEvent& e) {
    // The box's corners are world points; units whose screen point lies inside are selected
    // (shift toggles them).
    const sim::Vec3i end = world_at(e.x, e.y);
    auto sx = [&](const sim::Vec3i& p) { return (p[0] >> 16) - scroll_x_ + 128; };
    auto sy = [&](const sim::Vec3i& p) { return (p[2] >> 16) - (p[1] >> 17) - scroll_y_ + 32; };
    const int x0 = std::min(sx(drag_start_), sx(end)), x1 = std::max(sx(drag_start_), sx(end));
    const int y0 = std::min(sy(drag_start_), sy(end)), y1 = std::max(sy(drag_start_), sy(end));
    if (!e.shift)
        for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kSelected | kOnScreen | kOnScreenOld);
    int n = 0;
    std::uint16_t last = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        sim::Unit& u = b_.units[i];
        if (!selectable(u)) continue;
        const int ux = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[0]) >> 16) - scroll_x_ + 128;
        const int uy = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[2]) >> 16) - scroll_y_ -
                       (static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[1]) >> 16) >> 1) + 32;
        if (ux >= x0 && ux <= x1 && uy >= y0 && uy <= y1) {
            if (e.shift) u.flags ^= kSelected;
            else u.flags |= kSelected;
        }
        if (u.flags & kSelected) {
            ++n;
            last = static_cast<std::uint16_t>(i);
        }
    }
    focus_ = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kOnScreen | kOnScreenOld);
    mark_on_screen();
    panel_dirty_ = true;
    if (n == 1) speak(b_.units[last], 1);
    else if (n > 1) play_ui(c_.ui_sounds.select_multiple);
}

void GameClient::select_group(int group, bool add) {
    int n = 0;
    std::uint16_t last = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        sim::Unit& u = b_.units[i];
        if (!selectable(u)) continue;
        if (static_cast<int>(u.group) == group) {
            u.flags |= kSelected;
            ++n;
            last = static_cast<std::uint16_t>(i);
        } else if (!add) {
            u.flags &= ~kSelected;
        }
    }
    focus_ = 0;
    panel_dirty_ = true;
    if (n > 0) play_ui(c_.ui_sounds.select_squad);
    (void)last;
}

void GameClient::assign_group(int group) {
    // Selected units join the group; units of the group that are not selected leave it.
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        sim::Unit& u = b_.units[i];
        if (b_.ticks[i].type_index == 0 || b_.ticks[i].owner != opt_.player) continue;
        if (u.flags & kSelected) u.group = static_cast<std::uint32_t>(group);
        else if (static_cast<int>(u.group) == group) u.group = 0;
    }
    play_ui(c_.ui_sounds.create_squad);
}

// ---- the panel ---------------------------------------------------------------------------------

void GameClient::refresh_panel() {
    panel_dirty_ = false;
    // One selected unit with a build menu: its menu (or orders, as its flags say).
    std::uint16_t only = 0;
    int n = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        if (b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected)) {
            ++n;
            only = static_cast<std::uint16_t>(i);
        }
    focus_ = n == 1 ? only : 0;
    if (mode_ != Action::None && n == 0) mode_ = Action::None;
}

const Panel* GameClient::current_panel() const {
    if (!side_) return nullptr;
    if (focus_) {
        const sim::Unit& u = b_.units[focus_];
        const std::uint16_t t = b_.ticks[focus_].type_index;
        if (t < c_.types.size() && !c_.types[t].build_pages.empty() && (u.flags & kBuildMenuShown)) {
            const int page = static_cast<int>((u.flags & kBuildPageMask) >> kBuildPageShift);
            const auto& pages = c_.types[t].build_pages;
            return &pages[static_cast<std::size_t>(std::clamp(page - 1, 0, static_cast<int>(pages.size()) - 1))];
        }
    }
    if (selected_count() > 0) return &side_->orders;
    return &side_->idle;
}

bool GameClient::button_enabled(const Button& btn) const {
    // Order buttons are enabled when some selected unit can do the order.
    std::uint32_t any = 0;
    bool builder = false;
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        if (b_.ticks[i].type_index == 0 || b_.ticks[i].owner != opt_.player || !(b_.units[i].flags & kSelected)) continue;
        const std::uint16_t t = b_.ticks[i].type_index;
        if (t < b_.types.size()) any |= b_.types[t].mission.abilities;
        if (t < c_.types.size() && !c_.types[t].build_pages.empty()) builder = true;
    }
    using namespace sim::targeting_abilities;
    switch (btn.action) {
    case Action::Move: return any & kMove;
    case Action::Patrol: return any & kPatrol;
    case Action::Attack: return any & kAttack;
    case Action::Guard: return any & kGuard;
    case Action::Reclaim: return any & kReclaim;
    case Action::Repair: return any & kRepair;
    case Action::Capture: return any & kCapture;
    case Action::Load:
    case Action::Unload: return any & kLoad;
    case Action::Special: return any & kSpecialAttack;
    case Action::FireOrders: return any & 2u;
    case Action::MoveOrders: return any & 1u;
    case Action::OnOff: return any & 4u;
    case Action::Cloak: return any & 0x2000u;
    case Action::BuildTab: return builder && focus_ != 0;
    case Action::Stop: case Action::OrdersTab: return any != 0 || focus_ != 0;
    case Action::PrevPage: case Action::NextPage: return focus_ != 0;
    case Action::BuildUnit: return btn.unit_type != 0;
    default: return false;
    }
}

int GameClient::button_stage(const Button& btn) const {
    // A staged button shows the common state of the selection (the "mixed" frame when they differ).
    int stage = -1;
    const int mixed_fire = 3, mixed_move = 3, mixed_switch = 2;
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        if (b_.ticks[i].type_index == 0 || b_.ticks[i].owner != opt_.player || !(b_.units[i].flags & kSelected)) continue;
        const sim::Unit& u = b_.units[i];
        const std::uint16_t t = b_.ticks[i].type_index;
        const std::uint32_t ab = t < b_.types.size() ? b_.types[t].mission.abilities : 0;
        int v = -1, mixed = 0;
        switch (btn.action) {
        case Action::FireOrders: if (ab & 2u) v = static_cast<int>((u.flags >> 20) & 3); mixed = mixed_fire; break;
        case Action::MoveOrders: if (ab & 1u) v = static_cast<int>((u.flags >> 18) & 3); mixed = mixed_move; break;
        case Action::OnOff: if (ab & 4u) v = u.activate_flags & 1; mixed = mixed_switch; break;
        case Action::Cloak: if (ab & 0x2000u) v = static_cast<int>((u.flags >> 11) & 1); mixed = mixed_switch; break;
        default: return 0;
        }
        if (v < 0) continue;
        if (stage == -1) stage = v;
        else if (stage != v) return mixed;
    }
    return std::max(stage, 0);
}

bool GameClient::press_button(const Button& btn, const InputEvent& e) {
    if (!button_enabled(btn)) return false;
    const int player = opt_.player;
    sim::Unit* focus = focus_ ? &b_.units[focus_] : nullptr;
    switch (btn.action) {
    case Action::OrdersTab:
        if (focus) focus->flags &= ~(kBuildMenuShown | kBuildPageMask);
        play_ui(c_.ui_sounds.orders_tab);
        return true;
    case Action::BuildTab:
        if (focus) focus->flags = (focus->flags & ~kBuildPageMask) | kBuildMenuShown | (1u << kBuildPageShift);
        play_ui(c_.ui_sounds.build_tab);
        return true;
    case Action::PrevPage:
    case Action::NextPage:
        if (focus) {
            const std::uint16_t t = b_.ticks[focus_].type_index;
            const int pages = t < c_.types.size() ? static_cast<int>(c_.types[t].build_pages.size()) : 0;
            if (pages > 0) {
                int page = static_cast<int>((focus->flags & kBuildPageMask) >> kBuildPageShift);
                page = btn.action == Action::NextPage ? (page % pages) + 1 : (page <= 1 ? pages : page - 1);
                focus->flags = (focus->flags & ~kBuildPageMask) | kBuildMenuShown | (static_cast<std::uint32_t>(page) << kBuildPageShift);
            }
        }
        play_ui(c_.ui_sounds.next_build_menu);
        return true;
    case Action::Stop:
        command_selected_kind(b_, player, sim::mission::kStop, 0, false);
        play_ui(c_.ui_sounds.immediate);
        return true;
    case Action::FireOrders:
    case Action::MoveOrders: {
        // Left click steps forward, right click back (three stages).
        const int cur = button_stage(btn);
        const int next = e.button == 3 ? (cur + 2) % 3 : (cur + 1) % 3;
        command_selected_kind(b_, player, btn.action == Action::FireOrders ? sim::mission::kStandingFireOrder : sim::mission::kStandingMoveOrder,
                              next, false);
        play_ui(btn.action == Action::FireOrders ? c_.ui_sounds.set_fire : c_.ui_sounds.set_move);
        return true;
    }
    case Action::OnOff: {
        const int cur = button_stage(btn);
        command_selected_kind(b_, player, cur == 1 ? sim::mission::kDeactivate : sim::mission::kActivate, 0, false);
        play_ui(c_.ui_sounds.immediate);
        return true;
    }
    case Action::Cloak: {
        const int cur = button_stage(btn);
        command_selected_kind(b_, player, cur == 1 ? sim::mission::kCloakOff : sim::mission::kCloakOn, 0, false);
        play_ui(c_.ui_sounds.immediate);
        return true;
    }
    case Action::BuildUnit: {
        if (!focus) return false;
        if (!b_.ticks[focus_].has_motion) {
            // A factory: left adds to the queue, right takes away; shift by five.
            const int n = e.shift ? 5 : 1;
            change_build_count(b_, *focus, btn.unit_type, e.button == 3 ? -n : n);
            play_ui(e.button == 3 ? c_.ui_sounds.sub_build : c_.ui_sounds.add_build);
        } else {
            mode_ = Action::BuildUnit;
            build_type_ = btn.unit_type;
            play_ui(c_.ui_sounds.add_build);
        }
        return true;
    }
    default:
        // Order modes wait for a map click.
        mode_ = btn.action;
        play_ui(c_.ui_sounds.special);
        return true;
    }
}

// ---- commands ----------------------------------------------------------------------------------

bool GameClient::on_minimap(int x, int y) {
    if (!prepared_) prepare_drawing();
    return x >= mm_x_ && x < mm_x_ + mm_w_ && y >= mm_y_ && y < mm_y_ + mm_h_;
}

sim::Vec3i GameClient::minimap_world(int x, int y) {
    // The map point a minimap pixel stands for, on the ground.
    const int wx = std::clamp((x - mm_x_) * map_width_px() / std::max(1, mm_w_), 0, std::max(0, map_width_px() - 1));
    const int wz = std::clamp((y - mm_y_) * map_height_px() / std::max(1, mm_h_), 0, std::max(0, map_height_px() - 1));
    const int h = b_.map.cells.empty() ? 0 : b_.map.cells[static_cast<std::size_t>((wz >> 4) * b_.map.width + (wx >> 4))].height;
    return {wx << 16, h << 16, wz << 16};
}

void GameClient::give_order(Action mode, const InputEvent& e) {
    // Orders on the minimap go to the map point it shows (no target unit).
    const bool mm = on_minimap(e.x, e.y);
    const sim::Vec3i pos = mm ? minimap_world(e.x, e.y) : world_at(e.x, e.y);
    sim::Unit* target = !mm && hover_ ? &b_.units[hover_] : nullptr;
    std::uint8_t intent = sim::intent::kDefault;
    switch (mode) {
    case Action::Move: intent = sim::intent::kMove; break;
    case Action::Patrol: intent = sim::intent::kPatrol; break;
    case Action::Attack: intent = sim::intent::kAttack; break;
    case Action::Guard: intent = sim::intent::kGuard; break;
    case Action::Reclaim: intent = sim::intent::kReclaim; break;
    case Action::Repair: intent = sim::intent::kRepair; break;
    case Action::Capture: intent = sim::intent::kCapture; break;
    case Action::Load: intent = sim::intent::kPickup; break;
    case Action::Unload: intent = sim::intent::kUnload; break;
    case Action::Special: intent = sim::intent::kSpecialAttack; break;
    default: break;
    }
    const int n = command_selected(b_, opt_.player, intent, target, &pos, e.shift);
    // The first ordered unit acknowledges.
    if (n > 0)
        for (std::size_t i = 1; i < b_.units.size(); ++i)
            if (b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected) &&
                &b_.units[i] != target) {
                speak(b_.units[i], 5);
                break;
            }
}

int GameClient::cursor_id_for(const sim::Unit& u, std::uint8_t intent, const sim::Unit* target, const sim::Vec3i& pos) const {
    // The pointer a command would show, by the mission it resolves to (lower = preferred).
    sim::Unit& mu = const_cast<sim::Unit&>(u);
    const std::uint8_t kind = sim::resolve_mission(b_.targeting(), intent, mu, const_cast<sim::Unit*>(target), &pos);
    using namespace sim::mission;
    switch (kind) {
    case kAttackChase: case kAttackNoMove: case kAirToGround: case kAirToGroundHover: case kAirToAir: case kAirStrike:
    case kAttackKamikaze: case kAttackUnitType: return 1;
    case kAttackSpecial: return 2;
    case kCapture: return 4;
    case kFollowGround: case kAirFollow: return 5;
    case kRepairUnit: case kAirRepairUnit: case kHelpBuild: case kAirHelpBuild: return 6;
    case kPatrol: case kAirPatrol: case kRepairPatrol: case kAirRepairPatrol: case kQueuedPatrol: return 7;
    case kAirPickup: return 8;
    case kTeleport: return 9;
    case kResurrect: return 10;
    case kReclaim: case kReclaimUnit: case kAirReclaim: case kAirReclaimUnit: return 11;
    case kGroundPickup: return 12;
    case kGroundUnload: case kAirUnload: case kAirLanding: return 13;
    case kMoveGround: case kAirMove: case kQueuedMove: return 14;
    default: return 19;
    }
}

CursorKind GameClient::cursor_kind() const {
    if (mouse_x_ < 128 || !in_view(mouse_x_, mouse_y_)) return CursorKind::Normal;
    if (mode_ == Action::BuildUnit) return CursorKind::FindSite;
    if (dragging_) return CursorKind::Normal;
    const sim::Unit* target = hover_ ? &b_.units[hover_] : nullptr;
    const bool any = selected_count() > 0;
    if (!any) return target && selectable(*target) ? CursorKind::Select : CursorKind::Normal;
    std::uint8_t intent = sim::intent::kDefault;
    switch (mode_) {
    case Action::Move: intent = sim::intent::kMove; break;
    case Action::Patrol: intent = sim::intent::kPatrol; break;
    case Action::Attack: intent = sim::intent::kAttack; break;
    case Action::Guard: intent = sim::intent::kGuard; break;
    case Action::Reclaim: intent = sim::intent::kReclaim; break;
    case Action::Repair: intent = sim::intent::kRepair; break;
    case Action::Capture: intent = sim::intent::kCapture; break;
    case Action::Load: intent = sim::intent::kPickup; break;
    case Action::Unload: intent = sim::intent::kUnload; break;
    case Action::Special: intent = sim::intent::kSpecialAttack; break;
    default: break;
    }
    if (mode_ == Action::None) {
        if (target && selectable(*target)) return CursorKind::Select;
        if (opt_.right_click_orders) {
            // Interface type 1: the plain pointer, red over enemies, green over allies.
            if (target) {
                const int a = b_.ticks[target->id].owner;
                const bool ally = b_.alliances[static_cast<std::size_t>(opt_.player)][b_.players[static_cast<std::size_t>(a)].state] != 0;
                return ally ? CursorKind::Normal : CursorKind::Red;
            }
            return CursorKind::Normal;
        }
    }
    const sim::Vec3i pos = world_at(mouse_x_, mouse_y_);
    int best = 19;
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        if (b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected) && &b_.units[i] != target)
            best = std::min(best, cursor_id_for(b_.units[i], intent, target, pos));
    switch (best) {
    case 1: case 2: return CursorKind::Attack;
    case 3: return CursorKind::TooFar;
    case 4: return CursorKind::Capture;
    case 5: return CursorKind::Guard;
    case 6: return CursorKind::Repair;
    case 7: return CursorKind::Patrol;
    case 8: case 12: return CursorKind::Load;
    case 10: case 11: return CursorKind::Reclaim;
    case 13: return CursorKind::Unload;
    case 14: return CursorKind::Move;
    default: return CursorKind::Normal;
    }
}

// ---- input -------------------------------------------------------------------------------------

void GameClient::handle(const InputEvent& e) {
    shift_ = e.shift;
    ctrl_ = e.ctrl;
    alt_ = e.alt;
    if (e.type == InputEvent::Type::MouseMove || e.type == InputEvent::Type::MouseDown || e.type == InputEvent::Type::MouseUp)
        hover_ = in_view(e.x, e.y) ? unit_at(e.x, e.y) : 0;
    switch (e.type) {
    case InputEvent::Type::Quit: quit_ = true; break;
    case InputEvent::Type::MouseMove:
        mouse_x_ = e.x;
        mouse_y_ = e.y;
        if (mdrag_) {
            const int cx = (e.x - mdrag_x_) / 4, cy = (e.y - mdrag_y_) / 4;
            if (cx || cy) {
                set_scroll((scroll_x_ / 16 + cx) * 16, (scroll_y_ / 16 + cy) * 16);
                mdrag_x_ += cx * 4;
                mdrag_y_ += cy * 4;
            }
            break;
        }
        if (left_down_ && !dragging_ && in_view(press_x_, press_y_) && mode_ != Action::BuildUnit &&
            (std::abs(e.x - press_x_) > 3 || std::abs(e.y - press_y_) > 3))
            dragging_ = true;
        if (left_down_ && on_minimap(press_x_, press_y_) && mode_ == Action::None) minimap_jump(e.x, e.y);
        break;
    case InputEvent::Type::MouseDown: mouse_down(e); break;
    case InputEvent::Type::MouseUp: mouse_up(e); break;
    case InputEvent::Type::KeyDown:
        if (e.key >= 0 && e.key < 0x200) keys_down_[e.key] = true;
        key_down(e);
        break;
    case InputEvent::Type::KeyUp:
        if (e.key >= 0 && e.key < 0x200) keys_down_[e.key] = false;
        break;
    }
}

void GameClient::mouse_down(const InputEvent& e) {
    mouse_x_ = e.x;
    mouse_y_ = e.y;
    if (e.button == 1) {
        left_down_ = true;
        press_x_ = e.x;
        press_y_ = e.y;
        drag_start_ = world_at(e.x, e.y);
        held_button_ = nullptr;
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h && button_enabled(btn)) held_button_ = &btn;
        if (on_minimap(e.x, e.y) && mode_ == Action::None) minimap_jump(e.x, e.y);
    } else if (e.button == 2 || (e.button == 3 && e.ctrl && !opt_.right_click_orders)) {
        // Dragging the view: every 4 pixels of pointer travel scroll one cell (16 pixels).
        mdrag_ = true;
        mdrag_x_ = e.x;
        mdrag_y_ = e.y;
        tracked_ = 0;
    } else if (e.button == 3) {
        right_down_ = true;
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h) {
                    if (btn.action == Action::BuildUnit || btn.action == Action::FireOrders || btn.action == Action::MoveOrders)
                        press_button(btn, e);
                    return;
                }
    }
}

void GameClient::mouse_up(const InputEvent& e) {
    mouse_x_ = e.x;
    mouse_y_ = e.y;
    if (mdrag_ && (e.button == 2 || e.button == 3)) {
        mdrag_ = false;
        return;
    }
    if (e.button == 1) {
        const bool was_drag = dragging_;
        left_down_ = false;
        dragging_ = false;
        if (held_button_) {
            const Button* btn = held_button_;
            held_button_ = nullptr;
            if (e.x >= btn->x && e.x < btn->x + btn->w && e.y >= btn->y && e.y < btn->y + btn->h) press_button(*btn, e);
            return;
        }
        if (was_drag) {
            box_select(e);
            return;
        }
        if (in_view(e.x, e.y) && in_view(press_x_, press_y_)) left_click_map(e);
        else if (on_minimap(e.x, e.y) && mode_ != Action::None && mode_ != Action::BuildUnit) {
            give_order(mode_, e);
            if (!e.shift) mode_ = Action::None;
        }
    } else if (e.button == 3) {
        right_down_ = false;
        if (in_view(e.x, e.y)) right_click_map(e);
        else if (on_minimap(e.x, e.y) && mode_ == Action::None && opt_.right_click_orders && selected_count() > 0)
            give_order(Action::None, e);
    }
}

void GameClient::left_click_map(const InputEvent& e) {
    if (mode_ == Action::BuildUnit) {
        // Placement: the footprint must fit; shift keeps the mode for more.
        const sim::Vec3i p = world_at(e.x, e.y);
        const std::uint16_t t = build_type_;
        const int fx = t < b_.types.size() ? b_.types[t].construction.footprint_x : 1;
        const int fz = t < b_.types.size() ? b_.types[t].construction.footprint_z : 1;
        const int cx = ((p[0] >> 16) >> 4) - fx / 2, cz = ((p[2] >> 16) >> 4) - fz / 2;
        if (!structure_fits(b_, t, cx, cz)) {
            play_ui(c_.ui_sounds.not_ok_to_build);
            return;
        }
        play_ui(c_.ui_sounds.ok_to_build);
        const WorldPos at{((cx * 16) + fx * 8) << 16, p[1], ((cz * 16) + fz * 8) << 16};
        command_build(b_, opt_.player, t, at, e.shift);
        for (std::size_t i = 1; i < b_.units.size(); ++i)
            if (b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected)) {
                speak(b_.units[i], 5);
                break;
            }
        if (!e.shift) mode_ = Action::None;
        return;
    }
    if (mode_ != Action::None) {
        give_order(mode_, e);
        if (!e.shift) mode_ = Action::None;
        return;
    }
    // Default mode: select what is under the pointer; type 0 gives the default command
    // elsewhere, type 1 deselects.
    if (hover_ && selectable(b_.units[hover_])) {
        click_select(e);
        return;
    }
    if (opt_.right_click_orders) {
        deselect_all();
        return;
    }
    if (selected_count() > 0) give_order(Action::None, e);
}

void GameClient::right_click_map(const InputEvent& e) {
    if (mode_ != Action::None) {
        mode_ = Action::None;  // any order mode is cancelled
        return;
    }
    if (opt_.right_click_orders) {
        if (selected_count() > 0) give_order(Action::None, e);
        return;
    }
    deselect_all();
}

void GameClient::set_game_speed(int speed) {
    speed = std::clamp(speed, 1, 20);
    budget_.game_speed = budget_.effective_speed = static_cast<std::uint16_t>(speed);
    const int d = speed - 10;
    message(d == 0 ? "Game Speed Normal" : "Game Speed  " + std::string(d > 0 ? "+" : "-") + std::to_string(std::abs(d)));
}

void GameClient::key_down(const InputEvent& e) {
    const int k = e.key;
    // The chat line takes every key while it is open.
    if (chat_) {
        if (k == keys::kEnter) {
            chat_ = false;
            if (!chat_text_.empty()) {
                if (on_chat) on_chat(chat_text_);
                else message(chat_text_);
            }
            chat_text_.clear();
        } else if (k == keys::kEscape) {
            chat_ = false;
            chat_text_.clear();
        } else if (k == keys::kBackspace) {
            if (!chat_text_.empty()) chat_text_.pop_back();
        } else if (k >= 32 && k < 127 && chat_text_.size() < 60) {
            chat_text_.push_back(e.shift ? static_cast<char>(std::toupper(k)) : static_cast<char>(k));
        }
        return;
    }
    if (k == keys::kEnter) {
        chat_ = true;
        chat_text_.clear();
        return;
    }
    if (k == keys::kPause) {
        budget_.paused = !budget_.paused;  // the paused sign shows it
        if (network.send_pause) network.send_pause(budget_.paused);
        return;
    }
    if (k == keys::kEscape) {
        if (mode_ != Action::None) mode_ = Action::None;
        else deselect_all();
        return;
    }
    if (k == '+' || k == '=') {
        if (budget_.game_speed < 20) {
            set_game_speed(budget_.game_speed + 1);
            if (network.send_speed) network.send_speed(budget_.game_speed);
        }
        return;
    }
    if (k == '-' || k == '_') {
        if (budget_.game_speed > 1) {
            set_game_speed(budget_.game_speed - 1);
            if (network.send_speed) network.send_speed(budget_.game_speed);
        }
        return;
    }
    if (k == '`' || k == '~' || k == '!' || k == '#' || k == '*') {
        opt_.damage_bars = !opt_.damage_bars;
        return;
    }
    if (k == ',' || k == '.') {
        step_build_page(k == '.');
        return;
    }
    if (k >= '1' && k <= '9') {
        if (e.ctrl) assign_group(k - '0');
        else if (opt_.alt_groups != e.alt) {
            // With the group keys switched, the plain digit picks a build menu page instead.
            if (focus_) {
                const std::uint16_t t = b_.ticks[focus_].type_index;
                const int pages = t < c_.types.size() ? static_cast<int>(c_.types[t].build_pages.size()) : 0;
                const int page = k - '1';
                if (page < pages) {
                    sim::Unit& u = b_.units[focus_];
                    u.flags = (u.flags & ~(kBuildPageMask | kBuildMenuShown)) | (page > 0 ? kBuildMenuShown : 0u) |
                              (page > 0 ? (static_cast<std::uint32_t>(page & 7) << kBuildPageShift) : 0u);
                    play_ui(c_.ui_sounds.next_build_menu);
                }
            }
        } else {
            select_group(k - '0', e.shift);
        }
        return;
    }
    if (k >= keys::kF1 + 4 && k <= keys::kF1 + 7) {
        // F5..F8 recall a stored view; with Ctrl they store the current one.
        const std::size_t v = static_cast<std::size_t>(k - (keys::kF1 + 4));
        play_ui(c_.ui_sounds.select_squad);
        if (e.ctrl) views_[v] = {scroll_x_, scroll_y_};
        else {
            tracked_ = 0;
            set_scroll(views_[v][0], views_[v][1]);
        }
        return;
    }
    if (e.ctrl && k >= 'a' && k <= 'z') {
        switch (k) {
        case 'a':
            for (std::size_t i = 1; i < b_.units.size(); ++i)
                if (selectable(b_.units[i])) b_.units[i].flags |= kSelected;
            panel_dirty_ = true;
            play_ui(c_.ui_sounds.select_multiple);
            return;
        case 'c':
            // The commander: selected (its category), and the view follows it.
            select_category("CTRL_C", e.shift);
            for (std::size_t i = 1; i < b_.units.size(); ++i) {
                const std::uint16_t t = b_.ticks[i].type_index;
                if (t && t < c_.types.size() && c_.types[t].commander && own(b_.units[i])) tracked_ = static_cast<std::uint16_t>(i);
            }
            return;
        case 'd': command_selected_kind(b_, opt_.player, sim::mission::kSelfDestruct, 0, false); return;
        case 's': select_on_screen(); return;
        case 'z': select_same_types(); return;
        default: {
            std::string cat = "CTRL_";
            cat += static_cast<char>(std::toupper(k));
            select_category(cat, e.shift);
            return;
        }
        }
    }
    if (k == 't') {
        track_next(e.shift);
        return;
    }
    if (k == 'n') {
        next_unit();
        return;
    }
    // Quick keys of the shown panel's buttons.
    if (!e.ctrl && k > 0 && k < 0x100)
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (btn.quickkey == k) {
                    press_button(btn, e);
                    return;
                }
}

void GameClient::select_category(const std::string& category, bool keep) {
    // Selectable own units of the category join the selection; the others leave it unless
    // shift keeps them.
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        sim::Unit& u = b_.units[i];
        if (!selectable(u)) continue;
        const std::uint16_t t = b_.ticks[i].type_index;
        bool in = false;
        if (t < c_.types.size())
            for (const std::string& c : c_.types[t].categories)
                if (c == category) in = true;
        if (in) u.flags |= kSelected;
        else if (!keep) u.flags &= ~kSelected;
    }
    focus_ = 0;
    panel_dirty_ = true;
}

void GameClient::select_on_screen() {
    // Every unit loses the selection and the on-screen marks; the selectable own units drawn
    // this frame are selected.
    bool any = false;
    for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kSelected | kOnScreen | kOnScreenOld);
    for (const UnitScreen& s : on_screen_) {
        sim::Unit& u = b_.units[s.id];
        if (selectable(u)) {
            u.flags |= kSelected;
            any = true;
        }
    }
    if (any) {
        focus_ = 0;
        panel_dirty_ = true;
    }
}

void GameClient::select_same_types() {
    // Every selectable own unit of a type that is selected now.
    std::vector<bool> types(b_.types.size() + 1, false);
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        if (b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected) && b_.ticks[i].type_index < types.size())
            types[b_.ticks[i].type_index] = true;
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        if (selectable(b_.units[i]) && b_.ticks[i].type_index < types.size() && types[b_.ticks[i].type_index]) b_.units[i].flags |= kSelected;
    focus_ = 0;
    panel_dirty_ = true;
}

void GameClient::track_next(bool backward) {
    // The next (or previous) selected own unit after the tracked one, in unit table order,
    // wrapping over the player's range; none selected stops tracking.
    const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(opt_.player)];
    const int first = p.first_unit, last = p.last_unit;
    if (first <= 0 || last < first) return;
    int at = tracked_ >= first && tracked_ <= last ? tracked_ : first;
    const int n = last - first + 1;
    for (int k = 1; k <= n; ++k) {
        int id = backward ? at - k : at + k;
        while (id < first) id += n;
        while (id > last) id -= n;
        if (static_cast<std::size_t>(id) < b_.units.size() && b_.ticks[static_cast<std::size_t>(id)].type_index &&
            (b_.units[static_cast<std::size_t>(id)].flags & kSelected)) {
            tracked_ = static_cast<std::uint16_t>(id);
            return;
        }
    }
    tracked_ = 0;
}

void GameClient::next_unit() {
    // The first own unit not yet visited (on-screen marks clear) is centred on; when every
    // unit is marked the marks start over.
    const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(opt_.player)];
    auto find = [&]() -> std::uint16_t {
        for (int id = std::max(1, p.first_unit); id <= p.last_unit && static_cast<std::size_t>(id) < b_.units.size(); ++id)
            if (b_.ticks[static_cast<std::size_t>(id)].type_index && !(b_.units[static_cast<std::size_t>(id)].flags & (kOnScreen | kOnScreenOld)))
                return static_cast<std::uint16_t>(id);
        return 0;
    };
    std::uint16_t id = find();
    if (!id) {
        for (std::size_t i = 1; i < b_.units.size(); ++i) b_.units[i].flags &= ~(kOnScreen | kOnScreenOld);
        id = find();
    }
    if (!id) return;
    tracked_ = 0;
    center_on(b_.units[id].pos);
    for (const UnitScreen& s : on_screen_)
        if (b_.ticks[s.id].owner == opt_.player) b_.units[s.id].flags = (b_.units[s.id].flags & ~kOnScreenOld) | kOnScreen;
    b_.units[id].flags = (b_.units[id].flags & ~kOnScreenOld) | kOnScreen;
}

void GameClient::step_build_page(bool forward) {
    // '.' and ',' walk the focus unit's menus: the orders, then its build pages in turn.
    if (focus_) {
        sim::Unit& u = b_.units[focus_];
        const std::uint16_t t = b_.ticks[focus_].type_index;
        const int pages = t < c_.types.size() ? static_cast<int>(c_.types[t].build_pages.size()) : 0;
        if (pages > 0) {
            const bool shown = (u.flags & kBuildMenuShown) != 0;
            int page = static_cast<int>((u.flags & kBuildPageMask) >> kBuildPageShift);
            if (forward) {
                if (!shown) page = 1;
                else if (page >= pages) page = 0;
                else ++page;
            } else {
                if (!shown) page = pages;
                else if (page <= 1) page = 0;
                else --page;
            }
            u.flags &= ~(kBuildMenuShown | kBuildPageMask);
            if (page > 0) u.flags |= kBuildMenuShown | (static_cast<std::uint32_t>(page & 7) << kBuildPageShift);
            panel_dirty_ = true;
        }
    }
    play_ui(c_.ui_sounds.next_build_menu);
}

int GameClient::feature_under(int sx, int sy) const {
    const sim::Vec3i p = world_at(sx, sy);
    const int cx = (p[0] >> 16) >> 4, cz = (p[2] >> 16) >> 4;
    // A big feature covers several cells; only its corner cell names it.
    for (int dz = 0; dz < 8; ++dz)
        for (int dx = 0; dx < 8; ++dx) {
            const int x = cx - dx, z = cz - dz;
            if (x < 0 || z < 0 || x >= b_.map.width || z >= b_.map.height) continue;
            const std::uint16_t f = b_.map.cells[static_cast<std::size_t>(z * b_.map.width + x)].feature;
            if (f >= sim::kFirstSpecialFeature || f >= b_.features.size()) {
                if (dx == 0 && dz == 0 && f >= sim::kFirstSpecialFeature) continue;
                continue;
            }
            const sim::FeatureTypeInfo& ft = b_.features[f];
            if (dx < std::max<int>(1, ft.footprint_x) && dz < std::max<int>(1, ft.footprint_z)) return f;
        }
    return -1;
}

} // namespace client
