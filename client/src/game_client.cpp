// The playable client's input, selection, command and timing rules
// (spec: docs/RE_MEMORY.md, "Client").

#include "client/game_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/targeting.h"
#include "sim/unit_tick.h"

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
    // The drawn unit whose box (the model's extent around its screen point) holds the
    // point; the nearest centre wins.
    std::uint16_t best = 0;
    int best_d = 1 << 30;
    for (const UnitScreen& s : on_screen_) {
        if (s.id >= b_.units.size() || b_.ticks[s.id].type_index == 0) continue;
        const std::uint16_t t = b_.ticks[s.id].type_index;
        if (t >= c_.types.size()) continue;
        const UnitTypeLook& look = c_.types[t];
        const int r = std::max({std::abs(look.box_min[0] >> 16), std::abs(look.box_max[0] >> 16), std::abs(look.box_min[2] >> 16),
                                std::abs(look.box_max[2] >> 16), 8});
        const int top = s.y - r - (look.box_max[1] >> 17);
        if (sx < s.x - r || sx > s.x + r || sy < top || sy > s.y + r) continue;
        const int d = (sx - s.x) * (sx - s.x) + (sy - s.y) * (sy - s.y);
        if (d < best_d) {
            best_d = d;
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
    const int steps = budget_.update(now_ms);
    for (int k = 0; k < steps; ++k) step_battle(b_);
    b_.expire_lingering_sight();  // once per frame, after the frame's steps (even none)
    // Console lines expire one per frame, each (text_seconds + 1) seconds after it came.
    if (!console_.empty() && console_.front().tick + static_cast<std::uint32_t>(opt_.text_seconds + 1) * 30u < b_.game_tick())
        console_.pop_front();
    scroll_step(dt);
    hover_ = in_view(mouse_x_, mouse_y_) ? unit_at(mouse_x_, mouse_y_) : 0;
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
    // World sounds play when the viewed player sees the spot; panned by its place on screen.
    if (sample == 0xFFFF || sample >= c_.samples.size()) return;
    if (opt_.fog && b_.settings.fog_of_war && !b_.point_seen_by(opt_.player, pos)) return;
    const int sx = (pos[0] >> 16) - scroll_x_ - (opt_.width - 128) / 2;
    const int pan = std::clamp(sx * 100 / std::max(1, opt_.width), -100, 100);
    sounds_.push_back({sample, pan});
}

void GameClient::speak(const sim::Unit& u, std::uint32_t id) {
    const std::uint16_t t = b_.ticks[u.id].type_index;
    if (t >= c_.types.size() || id >= c_.types[t].sounds.size()) return;
    play_ui(c_.types[t].sounds[id]);
}

void GameClient::on_speech(sim::Unit& u, std::uint32_t id) {
    // Unit notices are heard for the viewing player's own units.
    if (b_.ticks[u.id].type_index == 0 || !own(u)) return;
    speak(u, id);
    static const char* const texts[24] = {nullptr, nullptr, "Under Attack", nullptr, nullptr, nullptr, "Arrived", "Cannot Comply",
                                          "Nanolathe Complete", nullptr, nullptr, nullptr, nullptr, nullptr, "Cloaked", "Visible"};
    if (id < 24 && texts[id] && b_.ticks[u.id].type_index < c_.types.size())
        message(c_.types[b_.ticks[u.id].type_index].name + ": " + texts[id]);
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

void GameClient::give_order(Action mode, const InputEvent& e) {
    const sim::Vec3i pos = world_at(e.x, e.y);
    sim::Unit* target = hover_ ? &b_.units[hover_] : nullptr;
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
        if (left_down_ && !dragging_ && in_view(press_x_, press_y_) && mode_ != Action::BuildUnit &&
            (std::abs(e.x - press_x_) > 3 || std::abs(e.y - press_y_) > 3))
            dragging_ = true;
        if (left_down_ && press_x_ < 128 && press_y_ < 128) minimap_jump(e.x, e.y);
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
        if (e.x < 128 && e.y < 128) minimap_jump(e.x, e.y);
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
    } else if (e.button == 3) {
        right_down_ = false;
        if (in_view(e.x, e.y)) right_click_map(e);
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

void GameClient::key_down(const InputEvent& e) {
    const int k = e.key;
    if (k == keys::kPause) {
        budget_.paused = !budget_.paused;  // the paused sign shows it
        return;
    }
    if (k == keys::kEscape) {
        if (mode_ != Action::None) mode_ = Action::None;
        else deselect_all();
        return;
    }
    if (k == '+' || k == '=') {
        if (budget_.game_speed < 20) {
            budget_.game_speed = budget_.effective_speed = static_cast<std::uint16_t>(budget_.game_speed + 1);
            const int d = budget_.game_speed - 10;
            message(d == 0 ? "Game Speed Normal" : "Game Speed  " + std::string(d > 0 ? "+" : "-") + std::to_string(std::abs(d)));
        }
        return;
    }
    if (k == '-' || k == '_') {
        if (budget_.game_speed > 1) {
            budget_.game_speed = budget_.effective_speed = static_cast<std::uint16_t>(budget_.game_speed - 1);
            const int d = budget_.game_speed - 10;
            message(d == 0 ? "Game Speed Normal" : "Game Speed  " + std::string(d > 0 ? "+" : "-") + std::to_string(std::abs(d)));
        }
        return;
    }
    if (k == '`' || k == '~') {
        opt_.damage_bars = !opt_.damage_bars;
        return;
    }
    if (k >= '1' && k <= '9') {
        if (e.ctrl) assign_group(k - '0');
        else select_group(k - '0', e.shift);
        return;
    }
    if (e.ctrl && k == 'a') {
        for (std::size_t i = 1; i < b_.units.size(); ++i)
            if (selectable(b_.units[i])) b_.units[i].flags |= kSelected;
        panel_dirty_ = true;
        play_ui(c_.ui_sounds.select_multiple);
        return;
    }
    if (e.ctrl && k == 'c') {
        // The commander: selected and centred.
        for (std::size_t i = 1; i < b_.units.size(); ++i) {
            const std::uint16_t t = b_.ticks[i].type_index;
            if (t && t < c_.types.size() && c_.types[t].commander && selectable(b_.units[i])) {
                deselect_all();
                b_.units[i].flags |= kSelected;
                center_on(b_.units[i].pos);
                speak(b_.units[i], 1);
                panel_dirty_ = true;
                break;
            }
        }
        return;
    }
    if (e.ctrl && k == 'd') {
        command_selected_kind(b_, opt_.player, sim::mission::kSelfDestruct, 0, false);
        return;
    }
    if (k == keys::kF1 + 1 && false) return;
    // Quick keys of the shown panel's buttons.
    if (!e.ctrl && k > 0 && k < 0x100)
        if (const Panel* p = current_panel())
            for (const Button& btn : p->buttons)
                if (btn.quickkey == k) {
                    press_button(btn, e);
                    return;
                }
}

} // namespace client
