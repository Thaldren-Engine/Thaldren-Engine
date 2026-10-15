#pragma once
// Menu screens built from a layout description: a background picture and a list
// of widgets (buttons, labels, list boxes, text fields, scroll bars, pictures).
// An importer turns the game's own screen description files into ScreenLayouts;
// the client draws them and runs their input (spec: docs/RE_MEMORY.md, "Client",
// "Menus"). Part of the front end's entry-point interface (client/front_end.h).

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/canvas.h"
#include "client/content.h"
#include "client/front_end.h"
#include "client/game_client.h"
#include "render/image.h"

namespace client {

class FrontEnd;

enum class WidgetKind : std::uint8_t {
    Header,     // the screen itself (first entry)
    Button,     // plain, or a multi-stage switch (stages > 0)
    ListBox,
    TextField,
    ScrollBar,
    Label,
    Surface,    // an area drawn by the screen's code (map preview, statistics graph, ...)
    FontRef,
    Picture,
};

struct Widget {
    WidgetKind kind = WidgetKind::Button;
    std::string name;   // as the description names it (matched case-insensitively)
    int x = 0, y = 0, w = 0, h = 0;
    int attribs = 0;    // the description's attribute bits (as given)
    int fg = 0, bg = 0; // colours (description)
    bool active = true; // shown and usable
    bool grayed = false;
    int quickkey = 0;   // lower-case character that presses it (0: none)
    int quickkey_char = 0;  // the quick key as the description gives it (marked in the caption)
    int stages = 0;     // multi-stage button: number of stages (0: plain)
    int stage = 0;      // current stage
    int status = 0;     // the description's status value
    std::string text;   // caption; for staged buttons the stage captions separated by '|'
    // Look: frames of the button art (empty: drawn as a plain framed box with the
    // caption). Plain buttons: frame 0 up, 1 pressed, 2 (when present) grayed.
    // Staged buttons: one frame per stage, then the pressed frame.
    const std::vector<render::Sprite8>* frames = nullptr;
    const render::Sprite8* picture = nullptr;  // Picture widgets
    // List boxes: rows, first shown row, selected row (-1: none).
    std::vector<std::string> items;
    int top = 0, selected = -1;
    // Text fields: maximum length.
    int max_chars = 0;
    // Scroll bars: range and position; the list box it moves (by name).
    int range = 0, position = 0, knob = 0;
    // Slider art (the SLIDERS sequence in frames): first frame of its set (0 vertical, 10
    // horizontal), -1 without art; the knob's travel in pixels (span) and its size.
    int slider_base = -1, span = 0;
    int knob_pos = 0, knob_value = -1;  // the knob's pixel offset and the value it was set from
    // Arrow buttons of a slider: -1 steps the linked slider (link) down, +1 up.
    int slider_step = 0;
    bool vertical = true;
    std::string link;   // label -> button it belongs to, scroll bar -> list box
    int sound = -1;     // sample played when pressed (-1: the menu's click)
};

// One screen's description.
struct ScreenLayout {
    std::string name;
    const render::Sprite8* background = nullptr;  // full-screen picture (may be null)
    const render::Palette* palette = nullptr;     // the picture's palette (null: the game palette)
    std::array<std::uint8_t, 256> colours{};      // interface colour k -> index in that palette
    int x = 0, y = 0;                             // origin (dialogs are centred by the front end)
    std::vector<Widget> widgets;                  // [0] is the header
    std::string default_focus, enter_default, escape_default;
};

// Art and sounds of the menus (filled by an importer).
struct MenuContent {
    render::Palette palette{};   // the game palette (screens may switch to their picture's)
    render::BlendTable blend;      // the game's mixing table (the GPU backend needs one)
    const Font* font = nullptr;        // captions and lists
    const Font* small_font = nullptr;
    const Font* big_font = nullptr;    // titles
    std::vector<Sample> samples;
    int click_sound = -1, error_sound = -1;
    const std::vector<render::Sprite8>* cursor = nullptr;  // the menu pointer
    // Loads a screen's layout by name (nullptr when the game has none). The result stays valid
    // for the life of the content.
    std::function<const ScreenLayout*(const std::string& name)> layout;
    // A full-screen picture by name (backgrounds that are not part of a layout), may be null.
    std::function<const render::Sprite8*(const std::string& name)> picture;
    // Button art sized w x h when the description gives none (the game's stock buttons).
    std::function<const std::vector<render::Sprite8>*(int w, int h)> stock_button;
    // The game's light table (32 levels x 256 entries, palettes\PALETTE.LHT; empty when absent):
    // shaded outlines (the focus ring) replace a pixel p by light[level * 256 + p].
    std::vector<std::uint8_t> light;
    // Interface colours (palette indices).
    std::uint8_t text_colour = 255, text_dim = 8, text_highlight = 0xF4, box_colour = 0, frame_colour = 8;
};

// A screen run from a ScreenLayout: draws the background and widgets, handles
// pointer/keyboard input (press and release on the same widget, quick keys,
// Enter/Escape defaults, list and scroll bar clicks, typing into the focused text
// field). Derive and override on_press() and the other callbacks.
class LayoutScreen : public Screen {
public:
    explicit LayoutScreen(const ScreenLayout* layout);
    void handle(FrontEnd& fe, const InputEvent& e) override;
    void draw(FrontEnd& fe, render::Image8& out) override;
    const render::Palette* palette() const override { return layout_ ? layout_->palette : nullptr; }

    Widget* widget(const std::string& name);
    const ScreenLayout* layout() const { return layout_; }
    // True when an active widget lies under the point.
    bool over(int x, int y) { return hit(x, y) >= 0; }
    // Gives the plain buttons new quick keys as if this screen's gadgets followed `before`'s in one
    // screen (an options page joins its menu's screen): the first caption character, not a space,
    // that no earlier gadget has.
    void reassign_quick_keys(const std::vector<Widget>& before);


protected:
    // A button was clicked (released over it) or its quick key pressed.
    virtual void on_press(FrontEnd&, Widget&) {}
    // A list row was picked (double = double click / Enter).
    virtual void on_pick(FrontEnd&, Widget&, int /*row*/, bool /*double_click*/) {}
    // Text field committed with Enter.
    virtual void on_text(FrontEnd&, Widget&) {}
    // Draws a Surface widget.
    virtual void draw_surface(FrontEnd&, render::Image8&, const Widget&) {}
    // Called before the widgets are drawn (after the background).
    virtual void draw_under(FrontEnd&, render::Image8&) {}
    virtual void draw_over(FrontEnd&, render::Image8&) {}
    void press(FrontEnd& fe, Widget& w);
    // Text of a widget for its current stage (staged captions are '|'-separated).
    static std::string caption(const Widget& w);
    // Draws one widget (derived screens may call it for widgets they add).
    void draw_widget(FrontEnd& fe, render::Image8& out, const Widget& w, bool held);
    std::uint8_t colour(int k) const;

    std::vector<Widget> widgets_;  // the screen's own copy (state)
    int ox_ = 0, oy_ = 0;          // origin on the frame
    // Places the screen on a frame of the given size (done once, at the first draw): a header
    // at -1 is centred on the frame, at -2 centred in the area right of the 128-pixel side
    // panel; one that would cross the right or bottom edge is centred that way; a negative y
    // counts from the bottom edge.
    void place(int frame_w, int frame_h);
    // The focus ring around the focused gadget (shaded outlines through the light table).
    void draw_focus_ring(FrontEnd& fe, render::Image8& out);
    bool placed_ = false;
    bool focus_ring_ = true;  // false: the screen draws no focus ring (option pages: their menu's ring)
    int focus_ = -1;               // index of the text field taking keys (-1: none)
    int held_ = -1;                // index of the pressed button (pointer down)
    int drag_ = -1;                // index of the scroll bar being dragged
    int press_button_ = 1;         // mouse button of the last press (1 left, 3 right)
    int focus_gadget_ = -1;        // the gadget with the focus ring (-1: none)
    std::uint32_t last_click_ms_ = 0;
    int last_click_row_ = -1;

private:
    const ScreenLayout* layout_;
    int hit(int x, int y);
    void scroll_to(Widget& bar, int x, int y);
};

}  // namespace client
