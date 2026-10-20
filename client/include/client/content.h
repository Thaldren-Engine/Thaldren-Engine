#pragma once
// Everything the playable client shows besides the simulation itself: the
// interface layout and art, fonts, cursors, sounds and per-unit-type display
// data. An importer fills it from game data; the client never reads files.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/canvas.h"
#include "render/battle_view.h"
#include "render/draw_list.h"
#include "render/image.h"

namespace client {

// What a button does.
enum class Action : std::uint8_t {
    None,
    OrdersTab, BuildTab, PrevPage, NextPage,
    // Order modes (the next map click gives the order).
    Move, Patrol, Attack, Guard, Reclaim, Repair, Capture, Load, Unload, Special,
    // Immediate orders.
    Stop,
    // Standing orders and switches (several stages).
    FireOrders, MoveOrders, OnOff, Cloak,
    // Build menu entry.
    BuildUnit,
};

struct Button {
    Action action = Action::None;
    int x = 0, y = 0, w = 0, h = 0;                       // screen rectangle
    const std::vector<render::Sprite8>* frames = nullptr;  // look: see button_frame()
    int quickkey = 0;                                     // character that presses it (0 = none)
    std::uint16_t unit_type = 0;                          // BuildUnit: battle type index
    bool staged = false;                                  // shows a stage (standing orders, switches)
    // The description's gadget fields the panel's drawing and input use: attribute bits (0x10
    // tab, 0x40 order mode, 0x100 staged; caption alignment 1 left, 4 right, 2 centred, 0x20
    // centred at the bottom), the caption colour (interface colour index) and font.
    int attribs = 0;
    std::uint8_t colorf = 0;
    const Font* font = nullptr;
    std::string name;     // the gadget's name (a build button's: the unit type's)
    bool grayed = false;  // shown disabled whatever the selection (the description's flag, or an
                          // unknown unit on a build page)
};

// Frame of a button's look: a staged button shows its stage (the last frame while
// held down); a plain one frame 0, 1 while held, min(2, last) when disabled.
int button_frame(const Button& b, bool held, bool disabled, int stage);

struct Panel {
    const render::Sprite8* background = nullptr;
    int x = 0, y = 0;
    int w = 0, h = 0;  // the description's box (header gadget)
    std::vector<Button> buttons;
};

// A disabled (grayed) button that is not staged is dimmed after it is drawn: its whole box goes
// through the grey table, then through shade row 12 (indexed with the pixel as a signed byte),
// unless its attribs have 0x80.
bool button_dims(const Button& b, bool disabled);

// A panel's picture and buttons drawn into img, whose top-left is the screen point (ox, oy):
// `under` first (when given, at its offsets), the panel's picture, then each button in order with
// frame_of(button) (-1: none), dimming the boxes of those dims(button) names right after the
// button (grey: the grey table; shade: 32 rows of 256).
void compose_panel(render::Image8& img, int ox, int oy, const Panel& p, const render::Sprite8* under,
                   const std::function<int(const Button&)>& frame_of, const std::function<bool(const Button&)>& dims,
                   const std::array<std::uint8_t, 256>& grey, const std::vector<std::uint8_t>& shade);

// An inclusive screen rectangle as the side description gives it.
struct Region {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// The interface of one side.
struct SideLayout {
    // Top bar.
    Region logo, energy_bar, energy_num, energy_max, energy_zero, metal_bar, metal_num, metal_max, metal_zero;
    Region energy_produced, energy_consumed, metal_produced, metal_consumed;
    // Bottom bar (y values are for a 480-row screen; they move with the screen height).
    Region logo2, unit_name, damage_bar, unit_metal_make, unit_metal_use, unit_energy_make, unit_energy_use;
    Region mission_text, unit_name2, damage_bar2, name, description;
    Region total_units, total_time;     // top bar: unit count and the game clock
    std::array<Region, 3> reload{};     // bottom bar: the weapons' reload bars
    const std::vector<render::Sprite8>* logos = nullptr;  // the side's logo by player colour (LOGO / LOGO2)
    std::uint8_t energy_colour = 0, metal_colour = 0;
    const Font* font = nullptr;  // console and bar text
    const render::Sprite8* panel_top = nullptr;
    const render::Sprite8* panel_bottom = nullptr;
    const render::Sprite8* panel_side = nullptr;
    Panel orders;  // order buttons of a selection without build menu shown
    Panel idle;    // shown with nothing selected (statistics)
};

// Mouse pointer looks.
enum class CursorKind : std::uint8_t {
    Normal, Select, Move, Attack, Guard, Patrol, Repair, Reclaim, Capture, Load, Unload, FindSite, TooFar, Busy, Red,
    Count
};

// An animated look's timing: each frame's duration in 1/30 s ticks and whether it starts
// over after the last frame (else it ends).
struct FrameTiming {
    std::vector<std::uint16_t> durations;
    bool loop = true;
};

// An animated look's place: the frame shown and the time left on it (1/30 s ticks, 16 bits).
struct FrameClock {
    int frame = 0;
    std::int16_t left = 0;
    bool ended = false;  // a look that does not loop went past its last frame (nothing shown)
    // Starts the look at its first frame with that frame's time.
    void start(const FrameTiming& t) {
        frame = 0;
        ended = false;
        left = static_cast<std::int16_t>(t.durations.empty() ? 0xFFFF : t.durations[0]);
    }
    // `dt` ticks of wall clock since the last step: they count the frame's time down; at zero or
    // below the next frame comes with its own time added (several at once after a long wait).
    // After the last frame a looping look starts over, another one ends. One-frame looks stay.
    void step(const FrameTiming& t, std::uint32_t dt) {
        const int n = static_cast<int>(t.durations.size());
        if (ended || n < 2) return;
        left = static_cast<std::int16_t>(static_cast<std::uint16_t>(left) - static_cast<std::uint16_t>(dt));
        while (left < 1) {
            if (++frame >= n) {
                if (!t.loop) {
                    ended = true;
                    return;
                }
                frame = 0;
            }
            left = static_cast<std::int16_t>(static_cast<std::uint16_t>(left) + t.durations[static_cast<std::size_t>(frame)]);
        }
    }
};

struct CursorSet {
    std::array<const std::vector<render::Sprite8>*, static_cast<std::size_t>(CursorKind::Count)> looks{};
    std::array<FrameTiming, static_cast<std::size_t>(CursorKind::Count)> timing{};
    // The pointer looks by the game's own pointer number (1 attack .. 20 busy; 21 the path
    // marker), with their timing: the queued orders' markers use them.
    std::array<const std::vector<render::Sprite8>*, 22> by_number{};
    std::array<FrameTiming, 22> by_number_timing{};
};

// Sound indices into the client's sample table (-1 = none).
// Unit notices by the simulation's speech id (1 select .. 22 count 0).
using UnitSounds = std::array<int, 24>;

struct InterfaceSounds {
    int immediate = -1;        // stop and other immediate orders
    int orders_tab = -1, build_tab = -1, next_build_menu = -1;
    int add_build = -1, sub_build = -1;       // factory queue
    int ok_to_build = -1, not_ok_to_build = -1;  // placement
    int set_fire = -1, set_move = -1, special = -1;  // standing orders, order modes
    int select_squad = -1, create_squad = -1, select_multiple = -1, message = -1;
    int small_button = -1;           // the chat line key
    int honk = -1, sing = -1;        // the notices' sing switch
    int victory = -1;                // a mission goal met
    int panel = -1, options = -1;    // the Space-held panels sliding out (Panel) and coming to rest (Options)
};

// Display data of a unit type (index = battle type index).
struct UnitTypeLook {
    std::string name, description;
    UnitSounds sounds{};                         // first sound of each notice
    std::array<std::vector<int>, 24> sound_lists{}; // every sound of each notice (one is picked)
    std::vector<std::string> categories;          // the type's categories (upper case)
    // Build menu pages 1..N, made for each side the viewing player may play (a page without a
    // description of its own takes the viewing player's side's stock page): [side][page - 1].
    // Empty: no build menu.
    std::vector<std::vector<Panel>> build_pages;
    const std::vector<Panel>* pages_for(int side) const {
        if (build_pages.empty()) return nullptr;
        return &build_pages[side >= 0 && static_cast<std::size_t>(side) < build_pages.size() ? static_cast<std::size_t>(side) : 0];
    }
    // Box around the model at rest (16.16, unit space): the selection outline.
    std::array<std::int32_t, 3> box_min{}, box_max{};
    bool commander = false;
    // The pointer's pick test (on box_min / box_max): the rank among several hits (smaller
    // wins: (size y / 2 + size z) * size x >> 16 of the type definition's model sizes).
    std::array<std::int32_t, 3> pick_min{}, pick_max{};  // the definition's bounds (footprint x, z)
    std::int64_t pick_rank = 0;
    // The bottom bar's build-button line: the type's build costs (no line for types the
    // importer marks).
    float cost_metal = 0.0f, cost_energy = 0.0f;
    // The unit information screen (F1): the type's name, its picture's file name, build time,
    // speed, acceleration (16.16 world units per step), turn rate and whether it moves (BMcode).
    std::string display_name, unit_name;
    std::int32_t build_time = 0, max_velocity = 0, acceleration = 0;
    std::uint16_t turn_rate = 0;
    bool mobile = false;
    bool no_build_line = false;
    // Interface data: built where it stands (a structure: its build button starts placement;
    // otherwise factories queue it), has build options, and the ranges the queued-order view
    // draws (cloak distance, sight, kamikaze distance and the area of the explosion weapon).
    bool structure = false;
    bool builds = false;
    bool kamikaze = false;
    std::int16_t min_cloak_distance = 0, sight_distance = 0;
    std::uint16_t kamikaze_distance = 0, explosion_area = 0;
};

// A decoded sound: signed 16-bit samples.
struct Sample {
    int rate = 22050;
    int channels = 1;
    std::vector<std::int16_t> pcm;
    std::string name;  // the sound's file name (logs)
};

// Shot looks by weapon index (the battle view draws shots through these).
struct ShotLook {
    render::ProjectileLook look = render::ProjectileLook::Line;
    bool hidden = false;          // drawn some other way (not modelled)
    std::uint8_t colour = 0, colour2 = 0;
    bool second_line = false;
    const std::vector<render::Sprite8>* frames = nullptr;
    const render::Model* model = nullptr;
    const sim::PoseModel* pose_model = nullptr;
};

struct Content {
    // Palette, tables and the interface colours (index k of the interface palette -> game palette).
    render::Palette palette{};
    render::BlendTable blend;
    std::array<std::uint8_t, 256> hud{};
    std::vector<SideLayout> sides;               // by side index
    std::vector<UnitTypeLook> types;              // by battle type index
    CursorSet cursors;
    InterfaceSounds ui_sounds;
    std::vector<Sample> samples;                  // sound table
    std::vector<ShotLook> shots;                  // by weapon index
    // Minimap: the map's source picture (the map file's own, as stored, or one sampled from
    // the tiles at twice the minimap's size; the minimap halves its top-left part), the grey
    // remap of out-of-sight parts, radar dots by colour, the dot of the unit under the pointer,
    // the nuke mark by colour.
    const render::Sprite8* minimap_picture = nullptr;
    std::array<std::uint8_t, 256> minimap_grey{};
    const std::vector<render::Sprite8>* nuke_dots = nullptr;
    const std::vector<render::Sprite8>* radar_dots = nullptr;
    const render::Sprite8* radar_dot_hover = nullptr;
    const Font* small_font = nullptr;
    // The paused sign (drawn at the view's centre).
    const render::Sprite8* paused_sign = nullptr;
    std::vector<std::string> start_messages;      // console lines shown at the start
    // Features: display name and reclaim values by feature type index (hover info).
    struct FeatureLook {
        std::string name;
        int metal = 0, energy = 0;
        std::uint16_t flags = 0;  // the definition's flag word (0x200: name only, 0x400: no line)
    };
    std::vector<FeatureLook> features;
    // A unit's status line on the bottom bar: the text of its head order's mission kind (index
    // = mission kind; entry 0 is also shown for a unit without orders).
    std::vector<std::string> mission_status;
    // What the queued-order view (Shift held) draws for an order of each mission kind (index =
    // mission kind): bits 1 the build box, 2 the path to it, 4 a ring, 8 the marker at its
    // target, 0x10 the unit's own ranges; and the marker's pointer number (0: none).
    std::vector<std::uint8_t> mission_draw, mission_marker;
    // The players' colour logos (frame = colour; stretched into the bars' LOGO / LOGO2 boxes)
    // and each player's colour.
    const std::vector<render::Sprite8>* team_logos = nullptr;
    std::array<std::uint8_t, 10> player_colour{};
    const render::Sprite8* victory_sign = nullptr;  // over the view when the battle is won / lost
    const render::Sprite8* defeat_sign = nullptr;
    // The chat line (Enter): its layout as the game's description gives it (the box at x, y; a
    // negative y counts from the bottom edge), the picture drawn in it and the text field, and
    // the interface's picture font.
    struct ChatLine {
        bool loaded = false;
        int x = 128, y = -33, w = 512, h = 33;
        const render::Sprite8* picture = nullptr;
        int picture_x = 0, picture_y = 0;
        int field_x = 0, field_y = 0, field_w = 0, field_h = 0;  // relative to the box
        int field_attribs = 0, field_colour = 15, max_chars = 63;
        const Font* font = nullptr;
    };
    ChatLine chat;
    // The console's fonts: its lines are spaced (and their senders' logos sized) by the console
    // font, and drawn in the interface's picture font when it is loaded (the glyphs keep their
    // own colours), else in the console font.
    const Font* console_font = nullptr;
    const Font* picture_font = nullptr;
    std::vector<std::uint8_t> shade_levels;  // the shade table (32 levels x 256; a layout dimmed under a dialog)
    std::vector<std::uint8_t> light_levels;  // the light table (32 levels x 256; the focused text field's text)
    // The Space-held game information panel's art (the interface's light bar, its second frame,
    // offsets cleared) and its text font (the interface's second picture font).
    const render::Sprite8* info_panel = nullptr;
    const Font* small_picture_font = nullptr;
    // The players' names as the battle's records hold them (the kills and losses panel, the
    // lead messages).
    std::array<std::string, 10> player_names;
    // What the game menu's settings page shows of the set-up (0x45F1D0): the
    // skirmish's fixed start locations, the local player's starting resources.
    struct GameInfo {
        bool fixed_locations = false;
        long long start_metal = 0, start_energy = 0;
    };
    GameInfo game_info;
    // Builds the battle view of the viewing player (projection, culling and draw order are
    // the renderer's; which units, features and shots are seen is the content's).
    std::function<void(render::BattleScene& scene, int view_player, std::uint32_t frame_counter)> fill_scene;
};

} // namespace client
