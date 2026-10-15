#pragma once
// Menu screens built from a layout description: a background picture and a list
// of widgets (buttons, labels, list boxes, text fields, scroll bars, pictures).
// An importer turns the game's own screen description files into ScreenLayouts;
// the client draws them and runs their input (spec: docs/RE_MEMORY.md, "Client",
// "Menus"). Part of the front end's entry-point interface (client/front_end.h).

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
    bool vertical = true;
    std::string link;   // label -> button it belongs to, scroll bar -> list box
    int sound = -1;     // sample played when pressed (-1: the menu's click)
};

// One screen's description.
struct ScreenLayout {
    std::string name;
    const render::Sprite8* background = nullptr;  // full-screen picture (may be null)
    int x = 0, y = 0;                             // origin (dialogs are centred by the front end)
    std::vector<Widget> widgets;                  // [0] is the header
    std::string default_focus, enter_default, escape_default;
};

// Art and sounds of the menus (filled by an importer).
struct MenuContent {
    render::Palette palette{};
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

    Widget* widget(const std::string& name);
    const ScreenLayout* layout() const { return layout_; }

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

    std::vector<Widget> widgets_;  // the screen's own copy (state)
    int ox_ = 0, oy_ = 0;          // origin on the frame
    Widget* focus_ = nullptr;      // text field taking keys
    Widget* held_ = nullptr;       // pressed button (pointer down)
    std::uint32_t last_click_ms_ = 0;
    int last_click_row_ = -1;

private:
    const ScreenLayout* layout_;
    Widget* hit(int x, int y);
};

}  // namespace client
