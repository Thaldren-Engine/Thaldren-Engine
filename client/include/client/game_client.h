#pragma once
// The playable client: one human player's view of a battle. It turns input into
// selections and commands, runs the simulation at the original's step rate,
// scrolls the view and draws the battle view with the in-game interface over it
// (spec: docs/RE_MEMORY.md, "Client"). No window system code: a front end feeds
// it input events and wall-clock time and shows the draw lists it returns.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "client/commands.h"
#include "client/content.h"
#include "render/draw_list.h"
#include "sim/battle.h"
#include "sim/step_budget.h"

namespace client {

// Key codes: printable characters as lower-case ASCII (digits, letters, punctuation),
// the rest below.
namespace keys {
constexpr int kLeft = 0x100, kRight = 0x101, kUp = 0x102, kDown = 0x103, kPause = 0x104, kEscape = 0x105,
              kEnter = 0x106, kTab = 0x107, kShift = 0x108, kCtrl = 0x109, kAlt = 0x10A, kF1 = 0x110;  // kF1 + n - 1 = Fn
}

struct InputEvent {
    enum class Type : std::uint8_t { MouseMove, MouseDown, MouseUp, KeyDown, KeyUp, Quit } type = Type::MouseMove;
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
};

struct SoundEvent {
    int sample = -1;
    int pan = 0;  // -100 (left) .. 100 (right)
};

class GameClient {
public:
    GameClient(sim::Battle& battle, Content& content, const ClientOptions& options);

    void handle(const InputEvent& e);
    // One pass of the main loop at wall-clock time `now_ms`: the due simulation steps (at
    // most 5), the per-frame sight expiry, scrolling and interface refresh. Returns the
    // number of steps run.
    int frame(std::uint32_t now_ms);
    // The frame to show: battle view, interface and pointer.
    render::DrawList draw();
    std::vector<SoundEvent> take_sounds();
    bool quit_requested() const { return quit_; }

    // Presentation events from the simulation (the battle's outside forwards them).
    void on_sound(std::uint16_t sample, const sim::Vec3i& pos);
    void on_speech(sim::Unit& unit, std::uint32_t id);
    void on_under_attack(sim::Unit& unit);
    void message(const std::string& text, bool highlight = false);
    void selection_changed() { panel_dirty_ = true; }

    // State, for front ends and tests.
    int scroll_x() const { return scroll_x_; }
    int scroll_y() const { return scroll_y_; }
    void set_scroll(int x, int y);
    void center_on(const sim::Vec3i& pos);
    const sim::StepBudget& budget() const { return budget_; }
    int selected_count() const;
    std::uint16_t hover_unit() const { return hover_; }
    const ClientOptions& options() const { return opt_; }

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
    void draw_bottom_bar(render::Image8& o);
    void draw_minimap(render::Image8& o);
    void draw_unit_marks(render::Image8& o);
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
    std::uint16_t hover_ = 0;
    std::uint16_t focus_ = 0;       // unit whose build menu / panel is shown
    bool show_build_ = false;       // build menu instead of orders for the focus unit
    int build_page_ = 0;
    bool panel_dirty_ = true;
    std::vector<UnitScreen> on_screen_;  // units drawn in the last frame (screen positions)
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
    // Drawing resources made once.
    void prepare_drawing();
    bool prepared_ = false;
    render::Sprite8 minimap_;        // the map picture scaled into the 126 x 126 square
    int mm_x_ = 0, mm_y_ = 0, mm_w_ = 126, mm_h_ = 126;
    std::uint8_t overlay_key_ = 0;   // an index the interface layer never draws
};

} // namespace client
