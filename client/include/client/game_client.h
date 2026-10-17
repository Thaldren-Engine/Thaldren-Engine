#pragma once
// The playable client: one human player's view of a battle. It turns input into
// selections and commands, runs the simulation at the original's step rate,
// scrolls the view and draws the battle view with the in-game interface over it
// (spec: docs/RE_MEMORY.md, "Client"). No window system code: a front end feeds
// it input events and wall-clock time and shows the draw lists it returns.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "client/commands.h"
#include "client/content.h"
#include "client/sound_rules.h"
#include "render/draw_list.h"
#include "sim/battle.h"
#include "sim/step_budget.h"

namespace client {

// Key codes: printable characters as lower-case ASCII (digits, letters, punctuation),
// the rest below.
namespace keys {
constexpr int kLeft = 0x100, kRight = 0x101, kUp = 0x102, kDown = 0x103, kPause = 0x104, kEscape = 0x105,
              kEnter = 0x106, kTab = 0x107, kShift = 0x108, kCtrl = 0x109, kAlt = 0x10A, kBackspace = 0x10B,
              kDelete = 0x10C, kHome = 0x10D, kEnd = 0x10E, kF1 = 0x110;  // kF1 + n - 1 = Fn
}

struct InputEvent {
    // Text: one typed character (key = its code) as the platform composes it from the keys and the
    // keyboard layout; KeyDown still comes for the key itself.
    enum class Type : std::uint8_t { MouseMove, MouseDown, MouseUp, KeyDown, KeyUp, Quit, Text } type = Type::MouseMove;
    int x = 0, y = 0;  // frame coordinates (mouse events)
    int button = 0;    // 1 left, 2 middle, 3 right
    int key = 0;       // keys:: or a character
    bool shift = false, ctrl = false, alt = false;
    std::uint32_t time_ms = 0;  // when it happened (double clicks)
};

struct ClientOptions {
    int width = 640, height = 480;  // frame size; the battle view is (w - 128) x (h - 64)
    int player = 0;                  // the human player's slot (also the viewed player)
    int side = 0;                    // interface side
    int scroll_speed = 32;           // pixels per 1/30 s (the original's default)
    int game_speed = 10;             // 1..20, 10 = normal
    bool right_click_orders = true;  // interface type 1 (else the original's default left-click type 0)
    bool damage_bars = false;        // health bars over own units (the original's default: off)
    bool selection_boxes = true;
    int text_lines = 10, text_seconds = 10;  // console (the original's defaults)
    bool fog = true;                 // draw the viewed player's fog of war
    int unit_chat = 10;              // unit speech level 0..10 (Off 0, Medium 5, Full 10): notices of priority > 10 - level speak
    int unit_chat_text = 5;          // the same for their console text
    bool speech = true;              // unit speech on (the sound options' speech switch)
    bool alt_groups = false;         // the alternate group keys (Alt+digit selects)
    bool clock = false;              // the game clock on the top bar (option)
    int sound_mode = 1;              // 0 off, 1 mono (world sounds centred), 2 3D (placed around the view's centre)
    int fx_volume = 27;              // effect volume 0..64 (0: no sounds start at all)
    bool sing = false;               // unit notices replaced by the two "sing" sounds (a command)
    // Typed commands (chat lines starting with '+', docs/RE_MEMORY.md "Console commands").
    bool cheats = false;             // cheat commands allowed (skirmish; network games whose host allows them)
    bool debug_commands = false;     // the debug commands unlocked (by a command)
    int view = -1;                   // the viewed player (-1: the human player)
    bool dithered_fog = false;       // fog drawn as a checkerboard instead of greyed (setting DitheredFog)
    bool radar_everything = false;   // the minimap shows every unit (cheat)
    bool show_ranges = false;        // weapon range circles (not drawn yet)
    bool shading = true, antialias = true, shadows = true, unit_shadows = true, feature_shadows = true;
};

// The minimap's place in its 126 x 126 square for a map whose scroll extent is sw x sh
// pixels: the map's aspect kept, the short side centred.
struct MinimapBox {
    int x = 0, y = 0, w = 126, h = 126;
};
MinimapBox minimap_box(int sw, int sh);

class GameClient {
public:
    GameClient(sim::Battle& battle, Content& content, const ClientOptions& options);

    void handle(const InputEvent& e);
    // The modifier keys held now (the front polls them every pass: the original asks the
    // keyboard when it needs them, e.g. Shift for the queued-order view and kept order modes).
    void poll_modifiers(bool shift, bool ctrl, bool alt) {
        shift_ = shift;
        ctrl_ = ctrl;
        alt_ = alt;
    }
    // One pass of the main loop at wall-clock time `now_ms`: the due simulation steps (at
    // most 5), the per-frame sight expiry, scrolling and interface refresh. Returns the
    // number of steps run.
    int frame(std::uint32_t now_ms);
    // The frame to show: battle view, interface and pointer.
    render::DrawList draw();
    std::vector<SoundEvent> take_sounds();
    bool quit_requested() const { return quit_; }
    // Network games: the slowest remote player's lag (steps behind) for the step budget, and
    // the pause and speed keys' packets (0x19). Set = a network game (one step per frame at most).
    struct NetworkHooks {
        std::function<std::int32_t()> remote_lag;
        std::function<void(bool)> send_pause;
        std::function<void(int)> send_speed;
    };
    NetworkHooks network;
    // The chat line (Enter): a typed line goes here when Enter is pressed again.
    std::function<void(const std::string&)> on_chat;
    bool chatting() const { return chat_; }
    // Typed commands: a chat line starting with '+' (after leading spaces) runs the rest as a
    // command line with the allowed permission bits (1 options, 2 cheats, 4 debug); returns the
    // bits of the command that ran (0: none). The line is then sent as chat like any other.
    std::function<std::uint32_t(const std::string& line, std::uint32_t allowed)> on_command;
    // Types a chat line as if entered (scripted runs and tests).
    void enter_chat_line(const std::string& text);
    // The pointer's map point (the original keeps it at GS+0x2CAA, 0x498DA0): the pointer
    // clamped to the battle view (or its minimap point) turned into a terrain point.
    sim::Vec3i cursor_world() const;
    // The viewed player (the human player unless a command chose another).
    int viewed_player() const { return opt_.view >= 0 ? opt_.view : opt_.player; }
    // The battle and options, for the front end (in-game menus, end of the battle).
    sim::Battle& battle() { return b_; }
    ClientOptions& options_mut() { return opt_; }
    void set_game_speed(int speed);
    bool paused() const { return budget_.paused; }
    void set_paused(bool p) { budget_.paused = p; }
    std::uint16_t tracked() const { return tracked_; }
    // A menu over the battle draws its own pointer.
    void set_pointer_hidden(bool h) { pointer_hidden_ = h; }

    // Presentation events from the simulation (the battle's outside forwards them).
    void on_sound(std::uint16_t sample, const sim::Vec3i& pos);
    // An interface sound by sample index (centred; the simulation's cues such as a mission
    // goal met).
    void interface_sound(int sample) { play_ui(sample); }
    void on_speech(sim::Unit& unit, std::uint32_t id);
    void on_under_attack(sim::Unit& unit);
    void forget_speech(sim::Unit& unit);  // a unit died: its queued notices go
    void message(const std::string& text, bool highlight = false, bool beep = false);
    void selection_changed() { panel_dirty_ = true; }
    // The minimap's work inside a battle step (the battle's outside forwards it): after a
    // player's sight updates the viewed player's radar marks are drawn anew (player_stage),
    // in the viewed player's 30-step turn its fog picture is rebuilt (sensor_turn), and every
    // step ends with the blink counter (step_end). Without these calls draw() does both.
    void minimap_player_stage(int player);
    void minimap_sensor_turn(int player);
    void minimap_step_end();
    // A whole sight rebuild (an option switched): the fog picture and the marks at once.
    void minimap_sight_rebuilt();
    // Tests: called with (0 marks, 1 fog picture, 2 blink count) and the player at each of the
    // hooks above, after the client's own work (a harness repeats the original's routine there).
    std::function<void(int, int)> minimap_probe;
    int minimap_blink_count() const { return mm_blink_calls_; }
    // The minimap as the frame shows it (its picture at its place, the view's rectangle over
    // it), onto `o` (draw() does this; tests compare it).
    void draw_minimap_onto(render::Image8& o);
    // Tests: the minimap's fog picture and marks made anew from the battle as it is now, and
    // the blink phase its marks use.
    void minimap_refresh() {
        if (!prepared_) prepare_drawing();
        minimap_composite();
        minimap_redraw();
    }
    bool minimap_blink() const { return ((mm_blink_calls_ / 8) & 1) != 0; }

    // State, for front ends and tests.
    int scroll_x() const { return scroll_x_; }
    int scroll_y() const { return scroll_y_; }
    void set_scroll(int x, int y);
    void center_on(const sim::Vec3i& pos);
    const sim::StepBudget& budget() const { return budget_; }
    int selected_count() const;
    std::uint16_t hover_unit() const { return hover_; }
    const ClientOptions& options() const { return opt_; }
    // The selection outlines of the selected units in a battle view scene (draw() does this;
    // tests that build the scene themselves call it).
    void add_selection_outlines(render::BattleScene& scene) const;
    // The queued orders of the units the view shows them for (drawn while Shift is held, into
    // `out` before the fog; draw() does this, tests building their own scene call it).
    void draw_queued_orders(render::DrawList& out, const render::Camera& camera);
    // Placement mode for a structure type (what its build button starts), and one pass of its
    // check at the pointer (frame() runs it while the pointer is over the view), for tests.
    void start_placement(std::uint16_t type);
    void update_placement();
    // The top bar's state (the shown amounts, the rates last read and the refresh timer), for
    // tests that compare it with the original's copy.
    struct TopBarState {
        std::array<float, 2> shown{};
        std::array<float, 4> rates{};
        std::uint32_t timer = 0;
        bool timer_set = false;
    };
    TopBarState top_bar_state() const { return {{shown_[0], shown_[1]}, bar_rates_, bar_timer_, bar_timer_set_}; }
    void set_top_bar_state(const TopBarState& s) {
        shown_[0] = s.shown[0];
        shown_[1] = s.shown[1];
        bar_rates_ = s.rates;
        bar_timer_ = s.timer;
        bar_timer_set_ = s.timer_set;
        bar_copy_valid_ = false;  // (the next frame redraws the bar)
    }
    // The pointer's animation: the look shown and its frame (tests); a new look (not the one
    // shown) starts at its first frame.
    CursorKind pointer_kind() const { return cursor_shown_; }
    bool pointer_ended() const { return cursor_clock_.ended; }
    void set_pointer(CursorKind k);
    int pointer_frame() const { return cursor_clock_.frame; }
    // One pass of the pointer's animation with `dt` ticks (1/30 s) of wall clock since the last
    // (frame() does this; tests drive it directly).
    void advance_pointer(std::uint32_t dt);

private:
    // Geometry.
    render::ClipRect view_rect() const;
    bool in_view(int x, int y) const;
    sim::Vec3i world_at(int sx, int sy) const;  // ground point under a screen point
    int map_width_px() const;
    int map_height_px() const;
    void clamp_scroll();
    struct UnitScreen {
        std::uint16_t id;
        int x, y;
    };
    std::uint16_t unit_at(int sx, int sy) const;
    bool selectable(const sim::Unit& u) const;
    bool own(const sim::Unit& u) const;

    // Input.
    void mouse_down(const InputEvent& e);
    void mouse_up(const InputEvent& e);
    void key_down(const InputEvent& e);
    void left_click_map(const InputEvent& e);
    void right_click_map(const InputEvent& e);
    void click_select(const InputEvent& e);
    void box_select(const InputEvent& e);
    void deselect_all();
    void mark_on_screen();
    void select_group(int group, bool add);
    void assign_group(int group);
    void select_same_type_on_screen(std::uint16_t type);
    bool press_button(const Button& b, const InputEvent& e);
    void give_order(Action mode, const InputEvent& e);
    void scroll_step(std::uint32_t dt_ticks);
    void minimap_jump(int x, int y);
    bool on_minimap(int x, int y);
    sim::Vec3i minimap_world(int x, int y);
    void play_ui(int sample);
    void speak(const sim::Unit& u, std::uint32_t id);
    // Unit notices: the original's speech queue (8 entries by priority, per-notice gaps).
    void select_category(const std::string& category, bool keep);
    void select_on_screen();
    void select_same_types();
    void track_next(bool backward);
    void next_unit();
    void step_build_page(bool forward);
    int feature_under(int sx, int sy) const;  // feature type index under a screen point (-1: none)
    void draw_chat(render::Image8& o);
    void chat_insert(int ch);  // a character typed into the chat line
    void draw_clock_and_units(render::Image8& o);
    void draw_reload_bars(render::Image8& o, const sim::Unit& u, int dy);
    void draw_caption(render::Image8& o, const Button& b, const std::string& text, int pressed);

    // Interface state.
    void refresh_panel();
    const Panel* current_panel() const;
    bool button_enabled(const Button& b) const;
    int button_stage(const Button& b) const;
    CursorKind cursor_kind() const;
    int cursor_id_for(const sim::Unit& u, std::uint8_t intent, const sim::Unit* target, const sim::Vec3i& pos) const;

    // Drawing.
    void draw_overlay(render::Image8& o, std::vector<std::uint8_t>& written);
    void draw_top_bar(render::Image8& o);
    void update_top_bar();
    // The bar's copy of what it showed last (player, shown energy, its rates, shown metal, its
    // rates, storages) and whether this frame changed it (the bar was drawn anew).
    std::array<float, 9> bar_copy_{};
    bool bar_copy_valid_ = false;
    bool bar_redrawn_ = true;
    void draw_bottom_bar(render::Image8& o);
    void draw_minimap(render::Image8& o);
    void draw_unit_marks(render::Image8& o);
    void draw_unit_bars(render::Image8& o);
    void draw_console(render::Image8& o);

    sim::Battle& b_;
    Content& c_;
    ClientOptions opt_;
    const SideLayout* side_ = nullptr;
    sim::StepBudget budget_;
    bool started_ = false;
    std::uint32_t last_scaled_ = 0;
    std::uint32_t frame_counter_ = 0;
    int scroll_x_ = 0, scroll_y_ = 0;
    int mouse_x_ = 0, mouse_y_ = 0;
    bool left_down_ = false, right_down_ = false;
    bool dragging_ = false;
    sim::Vec3i drag_start_{};  // world point of the drag start
    int press_x_ = 0, press_y_ = 0;
    bool keys_down_[0x200] = {};
    bool shift_ = false, ctrl_ = false, alt_ = false;
    Action mode_ = Action::None;  // order mode (None = default commands)
    std::uint16_t build_type_ = 0;  // placement mode: type to build (with mode_ == BuildUnit)
    const Button* held_button_ = nullptr;
    int held_with_ = 0;              // mouse button holding it (1 left, 3 right)
    Action pressed_order_ = Action::None;  // the order-mode button shown pressed (one of a group)
    // Back to the default mode; the order buttons' group shows none pressed.
    void end_order_mode();
    void release_button(const Button& b, const InputEvent& e);
    bool button_pressed_look(const Button& b) const;
    bool mode_kept_ = false;         // an order given with Shift kept the order mode (Shift's release ends it)
    // Placement (updated every pass while the pointer is over the view): the footprint's
    // corner cell, whether it fits and the height it would stand at; the outline's corners.
    int place_cx_ = 0, place_cz_ = 0;
    bool place_ok_ = false;
    int place_h_ = 0;
    bool place_valid_ = false;
    // The pointer's look and animation (the original changes the look only when it differs;
    // a new look starts at its first frame).
    CursorKind cursor_shown_ = CursorKind::Normal;
    FrameClock cursor_clock_;
    bool cursor_set_ = false;
    // The top bar's rates as last refreshed (energy made, used, metal made, used) and the timer
    // of the refresh (every 30 steps, from the player's display timer).
    std::array<float, 4> bar_rates_{};
    std::uint32_t bar_timer_ = 0;
    bool bar_timer_set_ = false;
    // Where an order's target unit was last seen (the queued-order view keeps it per order).
    struct OrderSeen {
        const sim::MissionOrder* order;
        std::uint32_t created;  // the order's creation step (a freed order's memory may come back)
        std::array<std::int16_t, 2> xz;
    };
    std::vector<OrderSeen> order_seen_;
    std::uint16_t hover_ = 0;
    std::uint16_t focus_ = 0;       // unit whose build menu / panel is shown
    bool show_build_ = false;       // build menu instead of orders for the focus unit
    int build_page_ = 0;
    bool panel_dirty_ = true;
    std::vector<UnitScreen> on_screen_;  // units drawn in the last frame (screen positions)
    std::vector<UnitScreen> mm_dots_;    // radar dots drawn on the minimap in the last frame
    std::uint32_t last_click_ms_ = 0;
    std::uint16_t last_click_unit_ = 0;
    struct Line {
        std::string text;
        std::uint32_t tick;
        bool highlight;
    };
    std::deque<Line> console_;
    std::vector<SoundEvent> sounds_;
    std::array<float, 8> shown_{};   // eased top-bar values: energy, metal stored; storages (unused), incomes
    bool quit_ = false;
    // Unit notices: the original's queue (client/sound_rules.h) and its host here.
    struct NoticeHost;
    NoticeQueue notices_;
    NoticeQueue::Levels notice_levels() const;
    void notices_frame();
    std::uint16_t tracked_ = 0;                 // unit the view follows (t / T, Ctrl+C)
    std::array<std::array<int, 2>, 4> views_{}; // stored views (Ctrl+F5..F8 store, F5..F8 recall)
    bool chat_ = false;
    std::string chat_text_;
    int chat_cursor_ = 0;          // the text field's insertion point
    bool text_events_ = false;     // the platform sends typed characters (Text events)
    int chat_provisional_ = -1;    // a key's character standing in for its typed character
    std::string last_command_;  // the last command line (debug: '\' runs it again)
    bool mdrag_ = false;                        // dragging the view with the middle button
    int mdrag_x_ = 0, mdrag_y_ = 0;
    int hover_feature_ = -1;
    bool pointer_hidden_ = false;
    // Drawing resources made once.
    void prepare_drawing();
    bool prepared_ = false;
    // The minimap (its spec: docs/RE_MEMORY.md "Client", "Minimap"): the map picture halved
    // into the square (mm_w_ x mm_h_ at mm_x_, mm_y_), the viewed player's fog picture of it,
    // and that copy with the radar marks (redrawn every step).
    render::Image8 mm_picture_, mm_mapped_, mm_final_;
    int mm_x_ = 0, mm_y_ = 0, mm_w_ = 126, mm_h_ = 126;
    bool mm_hooked_ = false;          // the battle's steps redraw it (else every frame does)
    bool mm_mapped_valid_ = false;
    std::uint32_t mm_blink_calls_ = 0;  // steps ended since the battle's drawing data was made
    void minimap_build_picture();
    void minimap_composite();
    void minimap_redraw();
    int scroll_width() const;   // the map's scroll extent in pixels (width x 16 - 32)
    int scroll_height() const;  // (height x 16 - 128)
    std::uint8_t overlay_key_ = 0;   // an index the interface layer never draws
};

} // namespace client
