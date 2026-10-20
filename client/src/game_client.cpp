// The playable client's input, selection, command and timing rules
// (spec: docs/RE_MEMORY.md, "Client").

#include "client/game_client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sim/features.h"
#include "sim/mission_goals.h"
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
    if (disabled) return std::min(2, n - 1);
    if (held) return n >= 2 ? 1 : 0;
    return 0;
}

bool button_dims(const Button& b, bool disabled) { return disabled && !b.staged && !(b.attribs & 0x80); }

void compose_panel(render::Image8& img, int ox, int oy, const Panel& p, const render::Sprite8* under,
                   const std::function<int(const Button&)>& frame_of, const std::function<bool(const Button&)>& dims,
                   const std::array<std::uint8_t, 256>& grey, const std::vector<std::uint8_t>& shade) {
    const render::ClipRect all = img.bounds();
    auto put = [&](const render::Sprite8* s, int x, int y) {
        if (s) render::blit(img, all, *s, x - ox, y - oy);
    };
    if (under) put(under, under->x_offset, under->y_offset);
    if (p.background) put(p.background, p.x + p.background->x_offset, p.y + p.background->y_offset);
    for (const Button& btn : p.buttons) {
        const int f = frame_of(btn);
        if (f >= 0 && btn.frames) {
            const render::Sprite8& s = (*btn.frames)[static_cast<std::size_t>(f)];
            put(&s, btn.x + s.x_offset, btn.y + s.y_offset);
        }
        if (!dims(btn) || shade.size() < 32 * 256) continue;
        const int x0 = std::max(btn.x - ox, 0), y0 = std::max(btn.y - oy, 0);
        const int x1 = std::min(btn.x + btn.w - 1 - ox, img.width - 1), y1 = std::min(btn.y + btn.h - 1 - oy, img.height - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                std::uint8_t& px = img.row(y)[x];
                px = grey[px];
                px = shade[static_cast<std::size_t>(12 * 256 + static_cast<std::int8_t>(px))];
            }
    }
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
    // (0x41C3C0) Negative -> 0, else at most the scroll extent less the view (which is
    // negative for a map smaller than the view: the original lets the scroll go there).
    const int vw = opt_.width - 128, vh = opt_.height - 64;
    const int lx = scroll_width() - vw, ly = scroll_height() - vh;
    if (scroll_x_ < 0) scroll_x_ = 0;
    else if (scroll_x_ > lx) scroll_x_ = lx;
    if (scroll_y_ < 0) scroll_y_ = 0;
    else if (scroll_y_ > ly) scroll_y_ = ly;
}

void GameClient::set_scroll(int x, int y) {
    // A scroll that moves the view at once (keys, edges, drags, the minimap, a stored view)
    // also makes it the camera's target (GS+0x14327), so the per-step camera keeps it there.
    scroll_x_ = x;
    scroll_y_ = y;
    clamp_scroll();
    target_x_ = scroll_x_;
    target_y_ = scroll_y_;
}

void GameClient::center_on(const sim::Vec3i& pos) {
    // 0x41C8E0 with "at once": the point (z less half the height, 16.16 sums) in the view's
    // centre.
    set_scroll(static_cast<std::int16_t>(static_cast<std::uint32_t>(pos[0]) >> 16) - (opt_.width - 128) / 2,
               static_cast<std::int16_t>(static_cast<std::uint32_t>(pos[2] - (pos[1] >> 1)) >> 16) - (opt_.height - 64) / 2);
}

void GameClient::clamp_target() {
    // (as 0x41C7C0 / 0x41C8E0 / 0x41CA10 clamp the target: negative -> 0, else at most the
    // scroll extent less the view)
    const int lx = scroll_width() - (opt_.width - 128), ly = scroll_height() - (opt_.height - 64);
    if (target_x_ < 0) target_x_ = 0;
    else if (target_x_ > lx) target_x_ = lx;
    if (target_y_ < 0) target_y_ = 0;
    else if (target_y_ > ly) target_y_ = ly;
}

void GameClient::ease_to_point(int x, int z) {
    // 0x41C7C0 eased: the view's target becomes the map point (x, z) in its centre; the camera
    // stage moves the view there over the next steps.
    target_x_ = x - (opt_.width - 128) / 2;
    target_y_ = z - (opt_.height - 64) / 2;
    clamp_target();
}

void GameClient::ease_to_unit(const sim::Vec3i& pos) {
    // 0x41C8E0 eased: the same for a unit's position (z less half its height).
    target_x_ = static_cast<std::int16_t>(static_cast<std::uint32_t>(pos[0]) >> 16) - (opt_.width - 128) / 2;
    target_y_ = static_cast<std::int16_t>(static_cast<std::uint32_t>(pos[2] - (pos[1] >> 1)) >> 16) - (opt_.height - 64) / 2;
    clamp_target();
}

void GameClient::camera_step() {
    // The camera stage of a battle step (0x41CA10): a followed unit that lost its alive flag
    // ends the following, else its position becomes the target; then each axis of the view
    // moves half of the way to the target (C division), at most 320 pixels; then the shake
    // the step made (the simulation's camera shake offsets, 0x41C6F0) is added, and the view is
    // clamped (the target is not).
    if (tracked_) {
        if (tracked_ >= b_.units.size() || !(b_.units[tracked_].flags & 0x10000000u)) {
            tracked_ = 0;
        } else {
            ease_to_unit(b_.units[tracked_].pos);
        }
    }
    auto ease = [](int& v, int target) {
        const int d = v - target;
        if (d == 0) return;
        if (d >= 1) v = d < 0x141 ? v - d / 2 : v - 0x140;
        else v = d > -0x141 ? v - d / 2 : v + 0x140;
    };
    ease(scroll_x_, target_x_);
    ease(scroll_y_, target_y_);
    scroll_x_ += b_.shake.offset_x;
    scroll_y_ += b_.shake.offset_y;
    clamp_scroll();
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
    for (int k = 0; k < steps; ++k) {
        step_battle(b_);
        camera_step();  // the step's camera stage: following, easing, shake
    }
    end_frame(b_);  // once per frame, after the frame's steps (even none)
    // Console lines expire one per pass that ran steps (0x463EF0 at the end of the step routine):
    // the oldest goes when it came more than (text_seconds + 1) seconds of game steps ago.
    if (steps > 0) expire_console();
    scroll_step(dt);
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
    if (pointer_off_) {
        hover_ = 0;  // nothing is under a pointer off the picture
        hover_feature_ = -1;
    }
    // The pointer over the view in placement mode: the placement check of this pass (the
    // pointer's look stays as it was); otherwise the pointer's look follows what it is over.
    if (mode_ == Action::BuildUnit && in_view(mouse_x_, mouse_y_)) update_placement();
    else set_pointer(cursor_kind());
    // An order (or placement) given with Shift kept its mode: letting Shift go ends it.
    if (mode_kept_ && !shift_ && !keys_down_[keys::kShift]) end_order_mode();
    advance_pointer(dt);
    notices_frame();
    panels_frame(now_ms);
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
    // (A pointer off the picture is at no edge.)
    const bool on = !pointer_off_;
    // (the arrow keys do not scroll while the chat line is open: its text field takes them)
    const bool arrows = !chat_;
    if ((arrows && keys_down_[keys::kLeft]) || (on && mouse_x_ == 0 && mouse_y_ < opt_.height)) x -= step;
    else if ((arrows && keys_down_[keys::kRight]) || (on && mouse_x_ == opt_.width - 1)) x += step;
    if ((arrows && keys_down_[keys::kUp]) || (on && mouse_y_ == 0 && mouse_x_ < opt_.width)) y -= step;
    else if ((arrows && keys_down_[keys::kDown]) || (on && mouse_y_ == opt_.height - 1)) y += step;
    if (x != scroll_x_ || y != scroll_y_) {
        // A scroll that moves the view ends unit tracking (the original's 0x41CE90).
        set_scroll(x, y);
        tracked_ = 0;
    }
}

void GameClient::minimap_jump(int x, int y) {
    // The minimap point becomes the view's centre, which ends unit tracking (0x41D0F0).
    // (The point's share of the map's scroll extent, less half the view.)
    tracked_ = 0;
    const int sw = scroll_width(), sh = scroll_height();
    if (sw <= 0 || sh <= 0) return;
    const MinimapBox m = minimap_box(sw, sh);
    set_scroll((x - m.x) * sw / std::max(m.w, 1) - (opt_.width - 128) / 2, (y - m.y) * sh / std::max(m.h, 1) - (opt_.height - 64) / 2);
}

// ---- sounds and messages ---------------------------------------------------------------------

void GameClient::play_ui(int sample) {
    // An interface sound (0x47F0C0): centred, 5.85 dB down; nothing while the effect volume or
    // the sound mode is off.
    if (sample < 0 || opt_.fx_volume == 0 || opt_.sound_mode == 0) return;
    SoundEvent e;
    e.sample = sample;
    sounds_.push_back(e);
}

std::vector<SoundEvent> GameClient::take_sounds() {
    std::vector<SoundEvent> out;
    out.swap(sounds_);
    return out;
}

void GameClient::on_sound(std::uint16_t sample, const sim::Vec3i& pos) {
    // A world sound (0x47F300; client/sound_rules.h): judged by the effect settings, the map,
    // the viewed player's sight and the view.
    if (sample == 0xFFFF || sample >= c_.samples.size()) return;
    SoundView v;
    v.fx_volume = opt_.fx_volume;
    v.sound_mode = opt_.sound_mode;
    v.scroll_x = scroll_x_;
    v.scroll_y = scroll_y_;
    v.view_w = (opt_.width - 128) / 16;
    v.view_h = (opt_.height - 64) / 16;
    v.map_w = static_cast<int>(b_.map.width);
    v.map_h = static_cast<int>(b_.map.height);
    SoundEvent e;
    if (world_sound(v, pos, [&] { return b_.point_seen_by(viewed_player(), pos); }, sample, e)) sounds_.push_back(e);
}
void GameClient::speak(const sim::Unit& u, std::uint32_t id) {
    // The interface's own notices (selection) go through the notice queue like the
    // simulation's: no sound of their own (the original never plays a unit sound directly).
    on_speech(b_.units[u.id], id);
}

// The notices' host: the battle's step and rand(), the unit's type sounds, the console.
struct GameClient::NoticeHost : NoticeQueue::Host {
    GameClient& c;
    explicit NoticeHost(GameClient& cl) : c(cl) {}
    std::uint32_t tick() override { return c.b_.game_tick(); }
    std::uint32_t rand() override { return c.b_.crt_rand.next(); }
    const std::vector<int>* list(std::uint16_t unit, std::uint32_t id) const {
        const std::uint16_t t = c.b_.ticks[unit].type_index;
        return t < c.c_.types.size() && id < 24 ? &c.c_.types[t].sound_lists[id] : nullptr;
    }
    int sound_count(std::uint16_t unit, std::uint32_t id) override {
        const std::vector<int>* l = list(unit, id);
        return l ? static_cast<int>(l->size()) : 0;
    }
    void speak(std::uint16_t unit, std::uint32_t id, int pick) override {
        // The sound file plays as a one-shot (5.85 dB down, centred) when there are effects;
        // with the sing switch every notice sings instead (and honks in every eighth second).
        const std::vector<int>* l = list(unit, id);
        if (!l || pick < 0 || static_cast<std::size_t>(pick) >= l->size()) return;
        int sample = (*l)[static_cast<std::size_t>(pick)];
        if (c.opt_.sing) sample = ((c.b_.game_tick() / 30) & 7) == 0 ? c.c_.ui_sounds.honk : c.c_.ui_sounds.sing;
        if (sample < 0 || c.opt_.fx_volume == 0 || c.opt_.sound_mode == 0) return;
        SoundEvent e;
        e.sample = sample;
        e.one_shot = true;
        c.sounds_.push_back(e);
    }
    bool unit_alive(std::uint16_t unit) override { return (c.b_.units[unit].flags & 0x10000000u) != 0; }
    void show(std::uint16_t unit, const std::string& text) override {
        const std::uint16_t t = c.b_.ticks[unit].type_index;
        if (t < c.c_.types.size()) c.message(c.c_.types[t].name + ": " + text, kUnitNotice, kNoPlayer, unit);
    }
};

NoticeQueue::Levels GameClient::notice_levels() const {
    NoticeQueue::Levels l;
    l.unit_chat = opt_.unit_chat;
    l.unit_chat_text = opt_.unit_chat_text;
    l.speech = opt_.speech;
    return l;
}

void GameClient::notices_frame() {
    // One queued notice per frame.
    NoticeHost h(*this);
    notices_.frame(h, notice_levels());
}

void GameClient::on_speech(sim::Unit& u, std::uint32_t id) {
    // Notices of the viewed player's units with the alive flag and without the dying flag
    // (0x47F780) join the queue.
    if (id == 0 || id >= 24 || b_.ticks[u.id].type_index == 0 || b_.ticks[u.id].owner != viewed_player() || !(u.flags & 0x10000000u) ||
        (u.flags & 0x4000u))
        return;
    NoticeHost h(*this);
    notices_.queue(h, notice_levels(), u.id, id);
}

void GameClient::forget_speech(sim::Unit& u) {
    notices_.forget(u.id);
    if (tracked_ == u.id) tracked_ = 0;
}

void GameClient::on_under_attack(sim::Unit& u) {
    // Spoken unless the unit is on screen.
    for (const UnitScreen& s : on_screen_)
        if (s.id == u.id) return;
    on_speech(u, 2);
}
bool GameClient::expire_console() {
    if (console_.next == console_.oldest) return false;
    if (!(console_.lines[console_.oldest].tick + static_cast<std::uint32_t>(opt_.text_seconds + 1) * 30u < b_.game_tick())) return false;
    if (++console_.oldest == 30) console_.oldest = 0;
    return true;
}

void GameClient::message(const std::string& text, std::uint8_t kind, std::uint8_t player, std::uint16_t param) {
    // 0x463CA0: nothing without text or while the console shows no lines. The ring holds 30
    // lines; when the line after the next to write, counted modulo the number of lines shown
    // (not 30), is the oldest, the oldest goes. The text keeps 63 characters; the slot's flag
    // bits above the kind stay what they were.
    if (text.empty() || text[0] == 0 || opt_.text_lines == 0) return;
    if ((console_.next + 1) % opt_.text_lines == console_.oldest)
        if (++console_.oldest == 30) console_.oldest = 0;
    ConsoleLine& l = console_.lines[console_.next];
    l.text.fill(0);
    for (std::size_t k = 0; k < 63 && k < text.size() && text[k]; ++k) l.text[k] = text[k];
    l.tick = b_.game_tick();
    l.kind = static_cast<std::uint8_t>((l.kind & 0xF0) | (kind & 0x0F));
    l.param = param;
    l.player = player;
    if (++console_.next == 30) console_.next = 0;
    // A line with a sender beeps "MessageArrived".
    if (player != kNoPlayer) play_ui(c_.ui_sounds.message);
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

bool GameClient::in_category(std::uint16_t type, const char* category) const {
    if (type >= c_.types.size()) return false;
    for (const std::string& c : c_.types[type].categories)
        if (c == category) return true;
    return false;
}

void GameClient::select_group(int group, bool add) {
    // 0x48D9A0 over the player's selectable units: the group's units are selected, the others
    // leave the selection unless Shift adds. When the group holds a CTRL_F unit and an armed
    // unit (unit flag bit 31), its CTRL_F units are left out (deselected).
    const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(opt_.player)];
    const int first = std::max(1, p.first_unit), last = std::min<int>(p.last_unit, static_cast<int>(b_.units.size()) - 1);
    bool has_f = false, armed = false;
    for (int i = first; i <= last; ++i) {
        const sim::Unit& u = b_.units[static_cast<std::size_t>(i)];
        if (!selectable(u) || static_cast<int>(u.group) != group) continue;
        if (in_category(b_.ticks[static_cast<std::size_t>(i)].type_index, "CTRL_F")) has_f = true;
    }
    if (has_f)
        for (int i = first; i <= last; ++i) {
            const sim::Unit& u = b_.units[static_cast<std::size_t>(i)];
            if (selectable(u) && static_cast<int>(u.group) == group && (u.flags & 0x80000000u)) armed = true;
        }
    for (int i = first; i <= last; ++i) {
        sim::Unit& u = b_.units[static_cast<std::size_t>(i)];
        if (!selectable(u)) continue;
        if (static_cast<int>(u.group) == group) {
            if (armed && in_category(b_.ticks[static_cast<std::size_t>(i)].type_index, "CTRL_F")) u.flags &= ~kSelected;
            else u.flags |= kSelected;
        } else if (!add) {
            u.flags &= ~kSelected;
        }
    }
    end_order_mode();  // (0x495860: the order mode returns to the default)
    focus_ = 0;
    panel_dirty_ = true;
    play_ui(c_.ui_sounds.select_squad);  // (the group key sounds whether or not the group has units)
}

bool GameClient::jump_to_noted_unit() {
    // From the oldest console line on: the first naming a unit (its parameter) that is alive
    // and not yet visited gets the marks 0x30 (shown in colour 10, visited) and the view eases
    // to the unit's whole x and z (height not counted, 0x41C7C0).
    std::uint16_t i = console_.oldest;
    if (console_.next == i) return false;
    do {
        ConsoleLine& l = console_.lines[i];
        if (l.param != 0 && !(l.kind & 0x10) && l.param < b_.units.size() && (b_.units[l.param].flags & 0x10000000u)) {
            l.kind |= 0x30;
            const sim::Unit& u = b_.units[l.param];
            ease_to_point(static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[0]) >> 16),
                          static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[2]) >> 16));
            return true;
        }
        if (++i == 30) i = 0;
    } while (console_.next != i);
    return false;
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
    const Panel* before = current_panel();
    const std::uint16_t focus_before = focus_;
    // One selected unit with a build menu: its menu (or orders, as its flags say).
    std::uint16_t only = 0;
    int n = 0;
    for (std::size_t i = 1; i < b_.units.size(); ++i)
        if (b_.ticks[i].type_index != 0 && b_.ticks[i].owner == opt_.player && (b_.units[i].flags & kSelected)) {
            ++n;
            only = static_cast<std::uint16_t>(i);
        }
    focus_ = n == 1 ? only : 0;
    if (mode_ != Action::None && n == 0) end_order_mode();
    // Another panel (or the same menu for another unit) is made afresh: no button of the order
    // group shows pressed. The same panel for the same unit keeps its buttons' states.
    if (current_panel() != before || focus_ != focus_before) pressed_order_ = Action::None;
}

const Panel* GameClient::current_panel() const {
    if (!side_) return nullptr;
    if (focus_) {
        const sim::Unit& u = b_.units[focus_];
        const std::uint16_t t = b_.ticks[focus_].type_index;
        if (t < c_.types.size() && !c_.types[t].build_pages.empty() && (u.flags & kBuildMenuShown)) {
            const int page = static_cast<int>((u.flags & kBuildPageMask) >> kBuildPageShift);
            const auto& pages = *c_.types[t].pages_for(opt_.side);
            return &pages[static_cast<std::size_t>(std::clamp(page - 1, 0, static_cast<int>(pages.size()) - 1))];
        }
    }
    if (selected_count() > 0) return &side_->orders;
    return &side_->idle;
}

bool GameClient::button_enabled(const Button& btn) const {
    if (btn.grayed) return false;  // grayed by its description or its build page
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
    case Action::None: return true;  // drawn as its description says (not grayed: enabled)
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

void GameClient::end_order_mode() {
    mode_ = Action::None;
    mode_kept_ = false;
    pressed_order_ = Action::None;
}

bool GameClient::press_button(const Button& btn, const InputEvent& e) {
    if (!button_enabled(btn)) return false;
    const int player = opt_.player;
    sim::Unit* focus = focus_ ? &b_.units[focus_] : nullptr;
    switch (btn.action) {
    case Action::OrdersTab:
        // The orders: the build menu flag goes (the page is kept).
        if (focus) focus->flags &= ~kBuildMenuShown;
        pressed_order_ = Action::None;
        play_ui(c_.ui_sounds.orders_tab);
        return true;
    case Action::BuildTab:
        // The build menu: its flag is set, the page it was on stays.
        if (focus) focus->flags |= kBuildMenuShown;
        pressed_order_ = Action::None;
        play_ui(c_.ui_sounds.build_tab);
        return true;
    case Action::PrevPage:
    case Action::NextPage:
        // Either mouse button: next / previous page, wrapping (the menu shown).
        if (focus) {
            const std::uint16_t t = b_.ticks[focus_].type_index;
            const int pages = t < c_.types.size() ? build_page_count(t) : 0;
            if (pages > 0) {
                int page = static_cast<int>((focus->flags & kBuildPageMask) >> kBuildPageShift);
                page = btn.action == Action::NextPage ? (page == pages ? 1 : page + 1) : (page <= 1 ? pages : page - 1);
                focus->flags = (focus->flags & ~kBuildPageMask) | kBuildMenuShown | (static_cast<std::uint32_t>(page & 7) << kBuildPageShift);
                pressed_order_ = Action::None;
            }
        }
        play_ui(c_.ui_sounds.next_build_menu);
        return true;
    case Action::Stop:
        // Stop ends any order mode (its button group shows none pressed).
        end_order_mode();
        command_selected_kind(b_, player, sim::mission::kStop, 0, false);
        play_ui(c_.ui_sounds.immediate);
        return true;
    case Action::FireOrders:
    case Action::MoveOrders: {
        // Either way round the three stages go forward (the "mixed" look counts as the last).
        const int cur = button_stage(btn);
        const int next = cur >= 2 ? 0 : cur + 1;
        command_selected_kind(b_, player, btn.action == Action::FireOrders ? sim::mission::kStandingFireOrder : sim::mission::kStandingMoveOrder,
                              next, false);
        play_ui(btn.action == Action::FireOrders ? c_.ui_sounds.set_fire : c_.ui_sounds.set_move);
        return true;
    }
    case Action::OnOff: {
        const int cur = button_stage(btn);
        command_selected_kind(b_, player, cur == 1 ? sim::mission::kDeactivate : sim::mission::kActivate, 0, false);
        play_ui(c_.ui_sounds.special);  // (the switches sound "specialorders", 0x41A490)
        return true;
    }
    case Action::Cloak: {
        const int cur = button_stage(btn);
        command_selected_kind(b_, player, cur == 1 ? sim::mission::kCloakOff : sim::mission::kCloakOn, 0, false);
        play_ui(c_.ui_sounds.special);
        return true;
    }
    case Action::BuildUnit: {
        // A structure's button (either mouse button) starts placement; a mobile type's button
        // changes the focus unit's queue (left +1, right -1; Shift by five) while it is selected.
        const bool structure = btn.unit_type < c_.types.size() && c_.types[btn.unit_type].structure;
        if (structure) {
            mode_ = Action::BuildUnit;
            mode_kept_ = false;
            build_type_ = btn.unit_type;
            place_valid_ = false;
            play_ui(c_.ui_sounds.add_build);
            return true;
        }
        if (!focus || !(focus->flags & kSelected)) return false;
        const int n = e.shift ? 5 : 1;
        change_build_count(b_, *focus, btn.unit_type, e.button == 3 ? -n : n);
        play_ui(e.button == 3 ? c_.ui_sounds.sub_build : c_.ui_sounds.add_build);
        return true;
    }
    default:
        // Order modes wait for a map click; the button toggles (a second press ends the mode),
        // and is the one of its group shown pressed.
        if (pressed_order_ == btn.action) {
            end_order_mode();
        } else {
            mode_ = btn.action;
            mode_kept_ = false;
            pressed_order_ = btn.action;
        }
        // (0x419BE0: repair, reclaim, capture and unload sound "specialorders"; move, patrol,
        // attack, guard, load and the special attack "immediateorders"; either way round.)
        play_ui(btn.action == Action::Repair || btn.action == Action::Reclaim || btn.action == Action::Capture || btn.action == Action::Unload
                    ? c_.ui_sounds.special
                    : c_.ui_sounds.immediate);
        return true;
    }
}

bool GameClient::button_pressed_look(const Button& btn) const {
    // While the pointer holds a button: plain buttons look pressed with the pointer over them,
    // order-mode buttons too (and not while it is off them, whatever their state); tabs and
    // switches keep their state. Otherwise: tabs show the menu shown, the order-mode button of
    // the group its state.
    const bool over = mouse_x_ >= btn.x && mouse_x_ < btn.x + btn.w && mouse_y_ >= btn.y && mouse_y_ < btn.y + btn.h;
    const bool tab = (btn.attribs & 0x10) != 0, mode_button = (btn.attribs & 0x40) != 0;
    if (held_button_ == &btn && !tab && !btn.staged) return over;
    if (tab) {
        const bool build_shown = focus_ && (b_.units[focus_].flags & kBuildMenuShown);
        return btn.action == Action::OrdersTab ? !build_shown : btn.action == Action::BuildTab && build_shown;
    }
    if (mode_button) return pressed_order_ != Action::None && pressed_order_ == btn.action;
    return false;
}

void GameClient::release_button(const Button& btn, const InputEvent& e) {
    // A held button acts when the pointer is let go over it (either mouse button).
    if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h) press_button(btn, e);
}

// ---- commands ----------------------------------------------------------------------------------

bool GameClient::on_minimap(int x, int y) {
    if (!prepared_) prepare_drawing();
    return x >= mm_x_ && x < mm_x_ + mm_w_ && y >= mm_y_ && y < mm_y_ + mm_h_;
}

sim::Vec3i GameClient::minimap_world(int x, int y) {
    // The map point a minimap pixel stands for (0x498D00): its share of the map's scroll
    // extent (whole units, 16-bit), at the terrain's height there.
    const std::int16_t wx = static_cast<std::int16_t>(scroll_width() * (x - mm_x_) / std::max(1, mm_w_));
    const std::int16_t wz = static_cast<std::int16_t>(scroll_height() * (y - mm_y_) / std::max(1, mm_h_));
    const std::int16_t h = static_cast<std::int16_t>(b_.height_map().sample(wx, wz));
    return {static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(wx)) << 16),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(h)) << 16),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(wz)) << 16)};
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
    // No acknowledgement here: each new order acknowledges itself ("ok" notice) when its
    // behaviour first runs (the simulation's order acknowledgement); the original's command
    // routine plays nothing.
    (void)command_selected(b_, opt_.player, intent, target, &pos, e.shift);
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
    if (e.type == InputEvent::Type::MouseMove || e.type == InputEvent::Type::MouseDown || e.type == InputEvent::Type::MouseUp) {
        pointer_off_ = false;  // the pointer is where the system pointer is on the picture again
        hover_ = in_view(e.x, e.y) ? unit_at(e.x, e.y) : 0;
    }
    switch (e.type) {
    case InputEvent::Type::Quit: quit_ = true; break;
    case InputEvent::Type::PointerLeave:
        pointer_off_ = true;
        hover_ = 0;
        break;
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
        // A character key without Ctrl or Alt reaches the battle as its typed character (the
        // original's key queue gets plain characters only from WM_CHAR: Shift+1 is a '!', a
        // 't' with Shift a 'T'); the Text event that follows carries it.
        if (!chat_ && e.text_follows) break;
        key_down(e);
        break;
    case InputEvent::Type::KeyUp:
        if (e.key >= 0 && e.key < 0x200) keys_down_[e.key] = false;
        break;
    case InputEvent::Type::Text:
        text_events_ = true;
        if (!chat_) {
            InputEvent k = e;
            k.type = InputEvent::Type::KeyDown;
            k.text_follows = false;
            key_down(k, true);
            break;
        }
        {
            if (chat_provisional_ >= 0 && chat_provisional_ < static_cast<int>(chat_text_.size())) {
                chat_text_.erase(static_cast<std::size_t>(chat_provisional_), 1);
                if (chat_cursor_ > chat_provisional_) --chat_cursor_;
            }
            chat_provisional_ = -1;
            chat_insert(e.key);
        }
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
                if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h && button_enabled(btn)) {
                    // Switches act at once (left button only); the others when let go.
                    if (btn.staged) {
                        press_button(btn, e);
                        held_button_ = nullptr;
                        return;
                    }
                    held_button_ = &btn;
                    held_with_ = 1;
                }
        if (on_minimap(e.x, e.y) && mode_ == Action::None) minimap_jump(e.x, e.y);
    } else if (e.button == 3 && !e.ctrl && [&] {
                   // The right button holds panel buttons as the left does (not switches).
                   if (const Panel* p = current_panel())
                       for (const Button& btn : p->buttons)
                           if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h) return true;
                   return false;
               }()) {
        right_down_ = true;
        held_button_ = nullptr;
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (e.x >= btn.x && e.x < btn.x + btn.w && e.y >= btn.y && e.y < btn.y + btn.h && button_enabled(btn) && !btn.staged) {
                    held_button_ = &btn;
                    held_with_ = 3;
                }
    } else if (e.button == 2 || (e.button == 3 && e.ctrl && !opt_.right_click_orders)) {
        // Dragging the view: every 4 pixels of pointer travel scroll one cell (16 pixels).
        mdrag_ = true;
        mdrag_x_ = e.x;
        mdrag_y_ = e.y;
        tracked_ = 0;
    } else if (e.button == 3) {
        right_down_ = true;
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
        if (held_button_ && held_with_ == 1) {
            const Button* btn = held_button_;
            held_button_ = nullptr;
            release_button(*btn, e);
            return;
        }
        if (was_drag) {
            box_select(e);
            return;
        }
        if (in_view(e.x, e.y) && in_view(press_x_, press_y_)) left_click_map(e);
        else if (on_minimap(e.x, e.y) && mode_ != Action::None && mode_ != Action::BuildUnit) {
            give_order(mode_, e);
            if (e.shift) mode_kept_ = true;
            else end_order_mode();
        }
    } else if (e.button == 3) {
        right_down_ = false;
        if (held_button_ && held_with_ == 3) {
            const Button* btn = held_button_;
            held_button_ = nullptr;
            release_button(*btn, e);
            return;
        }
        if (in_view(e.x, e.y)) right_click_map(e);
        else if (on_minimap(e.x, e.y) && mode_ == Action::None && opt_.right_click_orders && selected_count() > 0)
            give_order(Action::None, e);
    }
}

void GameClient::left_click_map(const InputEvent& e) {
    if (mode_ == Action::BuildUnit) {
        // Placement, as this pass's placement check found it (refused: the "not ok" sound);
        // the build order goes to the footprint's centre at the height found. Shift keeps the
        // mode until Shift is let go.
        if (!place_valid_) update_placement();
        if (!place_ok_) {
            play_ui(c_.ui_sounds.not_ok_to_build);
            return;
        }
        const std::uint16_t t = build_type_;
        const int fx = t < b_.types.size() ? b_.types[t].construction.footprint_x : 1;
        const int fz = t < b_.types.size() ? b_.types[t].construction.footprint_z : 1;
        const WorldPos at{(fx + place_cx_ * 2) * 0x80000, place_h_ << 16, (fz + place_cz_ * 2) * 0x80000};
        command_build(b_, opt_.player, t, at, e.shift);  // the build order acknowledges itself
        play_ui(c_.ui_sounds.ok_to_build);
        if (e.shift) mode_kept_ = true;
        else end_order_mode();
        return;
    }
    if (mode_ != Action::None) {
        give_order(mode_, e);
        if (e.shift) mode_kept_ = true;
        else end_order_mode();
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
        end_order_mode();  // any order mode is cancelled
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

void GameClient::enter_chat_line(const std::string& typed) {
    // The original's talk line (0x493BF0): leading spaces go; a line starting with '+' runs the
    // rest as a command line, allowed bits 1 (options), plus 2 when cheats are allowed, all
    // three once the debug commands are unlocked. A cheat that ran sends the line to everyone.
    // Whatever is left is then sent (and shown) as a chat line, the '+' included.
    std::size_t p = 0;
    while (p < typed.size() && typed[p] == ' ') ++p;
    const std::string line = typed.substr(p);
    if (!line.empty() && line[0] == '+') {
        std::uint32_t allowed = opt_.debug_commands ? 7u : 1u;
        if (opt_.cheats) allowed |= 2u;
        last_command_ = line.substr(1, 0x4F);
        if (on_command) on_command(last_command_, allowed);
    }
    if (line.empty()) return;
    if (on_chat) on_chat(line);
    else message(line, kChat);
}

sim::Vec3i GameClient::cursor_world() const {
    // The pointer on the minimap stands for the map point at its share of the map's scroll
    // range (map size less 32 and 128 units); elsewhere it is clamped to the battle view.
    int px, py;
    if (prepared_ && mouse_x_ >= mm_x_ && mouse_x_ < mm_x_ + mm_w_ && mouse_y_ >= mm_y_ && mouse_y_ < mm_y_ + mm_h_) {
        px = (mouse_x_ - mm_x_) * (map_width_px() - 32) / std::max(1, mm_w_);
        py = (mouse_y_ - mm_y_) * (map_height_px() - 128) / std::max(1, mm_h_);
    } else {
        const render::ClipRect r = view_rect();
        px = scroll_x_ + std::clamp(mouse_x_, r.x0, r.x1) - r.x0;
        py = scroll_y_ + std::clamp(mouse_y_, r.y0, r.y1) - r.y0;
    }
    const std::array<std::int32_t, 3> t = sim::screen_to_terrain(b_, px, py);
    return {t[0], t[1], t[2]};
}

void GameClient::chat_insert(int ch) {
    // A character goes in at the insertion point when the line is shorter than the field's
    // maximum, it is 0x20..0x7F (a field with attribute 2 takes only letters, digits, '_', ' '
    // and '\''), and the line with it is at most the field's width less 4 pixels wide.
    const Content::ChatLine& t = c_.chat;
    const int max_chars = t.loaded && t.max_chars > 0 ? t.max_chars : 63;
    if (static_cast<int>(chat_text_.size()) == max_chars || ch < 0x20 || ch >= 0x80) return;
    if ((t.field_attribs & 2) && !std::isalnum(ch) && ch != '_' && ch != ' ' && ch != '\'') return;
    if (t.font && t.field_w > 0) {
        const char one[2] = {static_cast<char>(ch), 0};
        if (t.font->text_width(chat_text_) + t.font->text_width(one) > t.field_w - 4) return;
    }
    chat_cursor_ = std::clamp(chat_cursor_, 0, static_cast<int>(chat_text_.size()));
    chat_text_.insert(chat_text_.begin() + chat_cursor_, static_cast<char>(ch));
    ++chat_cursor_;
}

void GameClient::key_down(const InputEvent& e, bool typed) {
    int k = e.key;
    // A character key that came without its typed character (scripted runs): Shift makes the
    // character a US keyboard would type, as the original's queue would get it.
    if (!typed && !chat_ && e.shift && !e.ctrl && !e.alt && k >= 0x20 && k < 0x7F) {
        static const char kFrom[] = "1234567890-=[];',./`\\";
        static const char kTo[] = "!@#$%^&*()_+{}:\"<>?~|";
        if (k >= 'a' && k <= 'z') k = std::toupper(k);
        else if (const char* p = std::strchr(kFrom, k)) k = kTo[p - kFrom];
    }
    // The chat line takes every key while it is open (its text field's editing, 0x4AB720):
    // Enter sends the line and closes it, Escape closes it with the line dropped; Backspace
    // removes the character before the insertion point, Delete the one at it; Left / Right
    // move the point, Home / End to the start / end; Up / Down do nothing (no history).
    if (chat_) {
        const int n = static_cast<int>(chat_text_.size());
        chat_cursor_ = std::clamp(chat_cursor_, 0, n);
        if (k == keys::kEnter) {
            chat_ = false;
            const std::string line = chat_text_;
            chat_text_.clear();
            chat_cursor_ = 0;
            enter_chat_line(line);
        } else if (k == keys::kEscape) {
            chat_ = false;
            chat_text_.clear();
            chat_cursor_ = 0;
        } else if (k == keys::kBackspace) {
            if (chat_cursor_ > 0) chat_text_.erase(static_cast<std::size_t>(--chat_cursor_), 1);
        } else if (k == keys::kDelete) {
            if (chat_cursor_ < n) chat_text_.erase(static_cast<std::size_t>(chat_cursor_), 1);
        } else if (k == keys::kHome) {
            chat_cursor_ = 0;
        } else if (k == keys::kEnd) {
            chat_cursor_ = n;
        } else if (k == keys::kLeft) {
            if (chat_cursor_ > 0) --chat_cursor_;
        } else if (k == keys::kRight) {
            if (chat_cursor_ < n) ++chat_cursor_;
        } else if (k >= 32 && k < 127 && !text_events_) {
            // Without the platform's typed characters: the key, Shift giving capitals. Until the
            // first typed character arrives the key's character stands in for it (replaced when
            // its typed character follows).
            const std::size_t before = chat_text_.size();
            chat_insert(e.shift ? std::toupper(k) : k);
            if (chat_text_.size() > before) chat_provisional_ = chat_cursor_ - 1;
            return;
        }
        chat_provisional_ = -1;
        return;
    }
    if (k == keys::kEnter) {
        // Opens the chat line (0x494050, TALK.GUI) with an empty line.
        chat_ = true;
        chat_text_.clear();
        chat_cursor_ = 0;
        play_ui(c_.ui_sounds.small_button);
        return;
    }
    if (k == keys::kPause) {
        budget_.paused = !budget_.paused;  // the paused sign shows it
        if (network.send_pause) network.send_pause(budget_.paused);
        return;
    }
    if (k == keys::kEscape) {
        if (mode_ != Action::None) end_order_mode();
        else deselect_all();
        return;
    }
    if (k == '+' || k == '=' || k == '-' || k == '_') {
        // (0x495E90: not in a network game for a watcher; the debug view mode blocks it too)
        const bool watcher = network.remote_lag && opt_.player >= 0 && opt_.player < 10 &&
                             b_.players[static_cast<std::size_t>(opt_.player)].active &&
                             b_.settings.player_sight[static_cast<std::size_t>(opt_.player)].watching;
        if (watcher) return;
        const bool up = k == '+' || k == '=';
        if (up ? budget_.game_speed < 20 : budget_.game_speed > 1) {
            set_game_speed(budget_.game_speed + (up ? 1 : -1));
            if (network.send_speed) network.send_speed(budget_.game_speed);
        }
        return;
    }
    if (k == '`' || k == '~' || k == '!' || k == '#' || k == '*') {
        // Damage bars on / off, and the settings are saved (0x430F00).
        opt_.damage_bars = !opt_.damage_bars;
        if (save_setting) save_setting("damagebars", opt_.damage_bars ? 1 : 0);
        return;
    }
    if (k == ',' || k == '.') {
        step_build_page(k == '.');
        return;
    }
    if (k >= '1' && k <= '9') {
        // Ctrl+digit makes a group. A digit picks the focus unit's build menu page (the digit
        // less one; 0 the orders) when Alt is not held, a group with Alt — the other way round
        // with the SwitchAlt setting (0x495E90, 0x41C060).
        if (e.ctrl) {
            assign_group(k - '0');
            return;
        }
        const bool page_key = opt_.alt_groups ? e.alt : !e.alt;
        if (page_key) {
            if (focus_) {
                const std::uint16_t t = b_.ticks[focus_].type_index;
                const int pages = t < c_.types.size() ? build_page_count(t) : 0;
                const int page = k - '1';
                // (pages 1..N and the orders: the definition's page count is N + 1)
                if (pages > 0 && page <= pages) {
                    sim::Unit& u = b_.units[focus_];
                    u.flags = (u.flags & ~(kBuildPageMask | kBuildMenuShown)) | (page > 0 ? kBuildMenuShown : 0u) |
                              (page > 0 ? (static_cast<std::uint32_t>(page & 7) << kBuildPageShift) : 0u);
                    panel_dirty_ = true;
                    play_ui(c_.ui_sounds.next_build_menu);
                }
            }
        } else {
            select_group(k - '0', e.shift);
        }
        return;
    }
    if (k == keys::kF1 + 2 && !e.ctrl && !e.shift) {
        // F3: the view goes to the next unit a console line names (0x464000).
        for (ConsoleLine& l : console_.lines) l.kind &= static_cast<std::uint8_t>(~0x20);
        if (!jump_to_noted_unit()) {
            for (ConsoleLine& l : console_.lines) l.kind &= static_cast<std::uint8_t>(~0x10);
            jump_to_noted_unit();
        }
        return;
    }
    if (k == keys::kF1 + 3 && !e.ctrl) {
        // F4: the kills and losses panel stays out (visual option bit 0x80).
        opt_.scoreboard = !opt_.scoreboard;
        return;
    }
    if (k == keys::kF1 + 11 && !e.ctrl) {
        // F12: the console is emptied (0x463C80).
        console_.next = console_.oldest = 0;
        return;
    }
    if (k == 'h' && !e.ctrl && network.remote_lag) {
        // 'h' (network games): the share screen (0x4936F0; refused for a watcher).
        if (on_share_key) on_share_key();
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
    if (e.ctrl && ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z'))) {
        switch (std::tolower(k)) {
        case 'a':
            // 0x48BD50: every selectable own unit joins the selection; the order mode ends.
            for (std::size_t i = 1; i < b_.units.size(); ++i)
                if (selectable(b_.units[i])) b_.units[i].flags |= kSelected;
            focus_ = 0;
            end_order_mode();
            panel_dirty_ = true;  // (silent: the Ctrl selections play no sound)
            return;
        case 'c':
            // The commander: its category CTRL_C selected, and the view follows the last own unit
            // of the "Commander" category (0x41C310).
            select_category("CTRL_C", e.shift);
            {
                const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(opt_.player)];
                for (int i = std::max(1, p.first_unit); i <= p.last_unit && static_cast<std::size_t>(i) < b_.units.size(); ++i) {
                    const std::uint16_t t = b_.ticks[static_cast<std::size_t>(i)].type_index;
                    if (t && t < c_.types.size() && in_category(t, "COMMANDER")) tracked_ = static_cast<std::uint16_t>(i);
                }
            }
            return;
        case 'd':
            // Self-destruct: pending self-destruct orders of the selected units are taken back;
            // only when none was, the selected units get one (0x495E90 case 0xAD).
            toggle_self_destruct(b_, opt_.player);
            return;
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
    if (k == 't' || k == 'T') {
        // 't' the next selected unit is followed, 'T' the previous one (0x41C2E0).
        track_next(k == 'T');
        return;
    }
    if (k == 'n') {
        next_unit();
        return;
    }
    if (k == '\\' && !e.ctrl) {
        // With the debug commands unlocked, '\' runs the last command line again, every
        // permission allowed.
        if (opt_.debug_commands && on_command) on_command(last_command_, 0xFFFFFFFFu);
        return;
    }
    // Quick keys of the shown panel's buttons (either case).
    if (!e.ctrl && k > 0 && k < 0x100)
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (btn.quickkey > 0 && std::tolower(btn.quickkey) == std::tolower(k)) {
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
    end_order_mode();  // (0x495860)
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
        end_order_mode();  // (0x495860)
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
    end_order_mode();  // (0x495860)
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
    // (0x48D4D0: the view eases there over the next steps; a followed unit stays followed)
    ease_to_unit(b_.units[id].pos);
    for (const UnitScreen& s : on_screen_)
        if (b_.ticks[s.id].owner == opt_.player) b_.units[s.id].flags = (b_.units[s.id].flags & ~kOnScreenOld) | kOnScreen;
    b_.units[id].flags = (b_.units[id].flags & ~kOnScreenOld) | kOnScreen;
}

void GameClient::step_build_page(bool forward) {
    // '.' and ',' walk the focus unit's menus: the orders, then its build pages in turn.
    if (focus_) {
        sim::Unit& u = b_.units[focus_];
        const std::uint16_t t = b_.ticks[focus_].type_index;
        const int pages = t < c_.types.size() ? build_page_count(t) : 0;
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

// ---- placement and the pointer ----------------------------------------------------------------

void GameClient::start_placement(std::uint16_t type) {
    mode_ = Action::BuildUnit;
    mode_kept_ = false;
    build_type_ = type;
    place_valid_ = false;
}

void GameClient::update_placement() {
    // The ground point under the pointer (the map position of the view pixel, clamped to the
    // view), the footprint's corner cell centred on it (half the footprint, rounded toward the
    // point), and the interface's placement check: an unexplored centre refuses; occupants
    // count only where the viewing player sees the centre.
    place_valid_ = true;
    place_ok_ = false;
    const std::uint16_t t = build_type_;
    if (t >= b_.types.size()) return;
    const render::ClipRect v = view_rect();
    const int mx = std::clamp(mouse_x_, v.x0, v.x1), my = std::clamp(mouse_y_, v.y0, v.y1);
    const std::array<std::int32_t, 3> p = sim::screen_to_terrain(b_, scroll_x_ + mx - v.x0, scroll_y_ + my - v.y0);
    const int fx = b_.types[t].construction.footprint_x, fz = b_.types[t].construction.footprint_z;
    place_cx_ = static_cast<std::int16_t>((p[0] - fx * 0x80000 + 0x80000) >> 20);
    place_cz_ = static_cast<std::int16_t>((p[2] - fz * 0x80000 + 0x80000) >> 20);
    const int cx = place_cx_, cz = place_cz_;
    bool fits = false;
    std::uint8_t height = 0;
    const sim::Construction& con = b_.construction();
    const std::int32_t metal = con.env.last_place_metal;  // the check's metal sum belongs to the simulation: kept
    if (cx >= 1 && cz >= 1 && cx + fx < b_.map.width && cz + fz < b_.map.height) {
        const int h = b_.map.cells[static_cast<std::size_t>(cz * b_.map.width + cx)].height;
        const int px = static_cast<std::int16_t>((fx + cx * 2) * 8), pz = static_cast<std::int16_t>((fz + cz * 2) * 8);
        const int bx = px >> 5, bz = (pz - (h >> 1)) >> 5;
        const sim::PlayerSight& ps = b_.sight.players[static_cast<std::size_t>(opt_.player)];
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << (opt_.player & 15));
        if (static_cast<std::uint32_t>(bx) < static_cast<std::uint32_t>(ps.width) && static_cast<std::uint32_t>(bz) < static_cast<std::uint32_t>(ps.height)) {
            const std::size_t at = static_cast<std::size_t>(bz) * static_cast<std::size_t>(ps.width) + static_cast<std::size_t>(bx);
            const bool explored = at < b_.explored.size() && (b_.explored[at] & bit);
            if (explored) {
                const bool seen = (b_.sight.flags & sim::sight_flags::kFog) ? at < ps.counts.size() && ps.counts[at] != 0 : true;
                fits = sim::structure_fits(con, t, static_cast<std::int16_t>(cx), static_cast<std::int16_t>(cz), seen, &height);
            }
        }
    }
    con.env.last_place_metal = metal;
    place_ok_ = fits;
    place_h_ = fits ? height : sim::structure_place_height(con, t, static_cast<std::int16_t>(cx), static_cast<std::int16_t>(cz));
}

void GameClient::set_pointer(CursorKind k) {
    // A new look starts at its first frame with that frame's time; the same look goes on.
    if (cursor_set_ && k == cursor_shown_) return;
    cursor_set_ = true;
    cursor_shown_ = k;
    cursor_clock_.start(c_.cursors.timing[static_cast<std::size_t>(k)]);
}

void GameClient::advance_pointer(std::uint32_t dt) {
    // Wall-clock ticks (1/30 s) since the last pass step the look's frames by their own times
    // (see FrameClock; a look that does not loop ends: no pointer shown).
    cursor_clock_.step(c_.cursors.timing[static_cast<std::size_t>(cursor_shown_)], dt);
}

} // namespace client
