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
#include <utility>
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
    // Text lists (set_list): the row height, the largest first row, and the gadgets that work with
    // the list (a slider, a text field) share its nonzero group number.
    int row_h = 0, max_top = 0;
    bool text_list = false;
    int assoc = 0;
    bool list_slider = false;  // a slider moved by its list (knob_pos is the knob's offset, knob its length)
    // Text fields: maximum length, the insertion point.
    int max_chars = 0;
    int cursor = 0;
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
    int sound = -1;     // sample played when pressed (-1: none; the screen plays its own)
    bool hot = true;    // takes the pointer (false: the pointer reaches what lies under it)
    // A button's art drawn through this light-table level (0: as it is). The original keeps it
    // in the gadget's colour word, which loading clears; screens set it (0x4B8310).
    int light = 0;
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
    int font_height = 14;                         // the text height of the screen's font (lists' rows)
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
    FrameTiming cursor_timing;                             // its frames' times (1/30 s) and looping
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
    // The game's shade table (32 levels x 256 entries, palettes\PALETTE.SHD; empty when absent): a
    // screen dimmed under a dialog replaces a pixel p by shade[level * 256 + p].
    std::vector<std::uint8_t> shade;
    // Interface colours (palette indices).
    std::uint8_t text_colour = 255, text_dim = 8, text_highlight = 0xF4, box_colour = 0, frame_colour = 8;
};

// Text broken into lines at spaces and carriage returns as the original's labels and lists do:
// words are added while the line fits `width`, lines are the font's height + 2 apart, and drawing
// stops when `height` is used up. Returns the y after the last line.
int draw_wrapped(render::Image8& out, const render::ClipRect& clip, const Font& font, const std::string& text, int x, int y, int width, int height);

// The keyboard in the menus (spec: docs/RE_MEMORY.md, "Client", "Menu keys"). A platform key
// event becomes the codes the original's key queue would get for it: typed characters as typed
// (the Text event; without Text events, from the key and Shift), Enter 13, Escape 27, Tab 9,
// Backspace 8, the arrows 0xF4 left, 0xF5 up, 0xF6 right, 0xF7 down, Home 0xF0, End 0xF1, Delete
// 0xEF, Pause 0xF8, F1..F12 0xE2..0xED (with Ctrl 0xCE..0xD9); with Alt (and not Ctrl) a letter
// gives its lower-case letter, a digit or sign key its own character and Enter, Escape, Tab,
// Backspace and Space nothing; with Ctrl a letter gives 0xAA..0xC3 and then its control
// character (1..26), a digit 0xC4..0xCD.
std::vector<int> menu_key_codes(const InputEvent& e);

// The original's editing of a text field's line by one key code (0x4AB720): a character
// 0x20..0x7F goes in at the insertion point while the line is shorter than max_chars and its
// width with the character is at most width_limit (attribute 2: letters, digits, '_', ' ', '\''
// only); Backspace removes the character before the point, Delete (0xEF) the one at it, Home /
// End / Left / Right move the point. Returns true for Enter (13) and Escape (27), which end the
// editing (the caller empties the line for Escape).
bool edit_line(std::string& text, int& cursor, int max_chars, int width_limit, int attribs, const Font* font, int code);
// Paste (Ctrl+V = code 0xBF, Insert = 0xEE; 0x4AB720): the line becomes the clipboard's text
// (as it is, up to max_chars - 1 characters), then its last character goes while it is wider
// than the field (field_width, no margin); the insertion point stays. Nothing without text on
// the clipboard.
void paste_line(std::string& text, int max_chars, int field_width, const Font* font);
// The system clipboard's text in the game's character set (set by the platform layer; unset:
// no clipboard).
extern std::function<bool(std::string&)> clipboard_text;

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
    // Sets a gadget's caption the way the original does: a plain button (no attribute 0x10000)
    // takes a new quick key from it (the first character, not a space, no other gadget has).
    void set_caption(Widget& w, const std::string& text);
    void frame(FrontEnd& fe, std::uint32_t now_ms) override;

    // Text lists (spec: docs/RE_MEMORY.md, "Client", "Lists"). set_list gives a list its rows: the
    // first row selected and shown, the row height (at least the font's height + 1), the largest
    // first row; the slider of its group gets its knob and travel and is shown only when the rows
    // do not fit.
    void set_list(Widget& list, std::vector<std::string> items);
    // Selects a row, showing it at the top when it is not in view; the slider follows.
    void select_list_row(Widget& list, int row);
    // The gadgets of the list's group follow it: its slider's knob, a text field the row's text
    // (lists with attribute 8).
    void list_moved(Widget& list);

    // Tests: the screen's state (layout, focus, the field taking keys, every field's text and
    // switch's stage, as name / value pairs) and the gadgets pressed since the last call.
    std::vector<std::pair<std::string, std::string>> describe() const;
    std::vector<std::string> take_presses();


protected:
    // A button was clicked (released over it) or its quick key pressed.
    virtual void on_press(FrontEnd&, Widget&) {}
    // A list row was picked (double = double click / Enter).
    virtual void on_pick(FrontEnd&, Widget&, int /*row*/, bool /*double_click*/) {}
    // A text list's selection was moved by the pointer or the arrow keys (the original's list
    // callback; the arrow keys call it even when the selection stays).
    virtual void on_list_select(FrontEnd&, Widget&) {}
    // Text field committed with Enter.
    virtual void on_text(FrontEnd&, Widget&) {}
    // Draws a Surface widget.
    virtual void draw_surface(FrontEnd&, render::Image8&, const Widget&) {}
    // Called before the widgets are drawn (after the background).
    virtual void draw_under(FrontEnd&, render::Image8&) {}
    virtual void draw_over(FrontEnd&, render::Image8&) {}
    void press(FrontEnd& fe, Widget& w);

    // ---- keys (the original's interface pass for one key code; docs/RE_MEMORY.md "Menu keys")
    // The original's kind of gadget i: 1 button, 2 list, 3 text field, 4 slider, 5 label, 6 a
    // screen-drawn area, 0 anything else. Screens that keep lists or fields of their own say so.
    virtual int gadget_kind(int i) const;
    // A text field's line and insertion point (null: none); a field becoming the one taking the
    // keys puts its point at the end.
    virtual std::string* field_text(int i);
    virtual int* field_cursor(int i);
    virtual int field_limit(int i) const;
    // The Up / Down keys on the focused list (dir -1 / +1): the selection moves, the list's
    // callback follows.
    virtual void list_key(FrontEnd& fe, int i, int dir);
    // A gadget pressed by a key (a button, a list acted on, a text field ended with Enter or
    // Escape, a label or area): the screen's handler.
    virtual void key_press(FrontEnd& fe, int i);
    // Whether the focused gadget shows the focus ring (lists and fields of some screens do not).
    virtual bool rings(int /*i*/) const { return true; }
    void key_code(FrontEnd& fe, int code, bool shift, bool alt);
    std::string enter_default_;  // the Enter default the screen's set-up names (empty: the description's)
    void move_focus(int dir);
    void activate_field(int i);  // the field taking the keys (-1 none), its point at the end
    std::vector<std::string> presses_;  // tests: the gadgets pressed (take_presses)
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
    // A screen opened centred (the original's open flags 0x100 and 0x1000): -1 centred on the frame,
    // -2 centred right of the side panel, whatever the description's position (0: the description's).
    int open_centred_ = 0;
    std::size_t focus_limit_ = 0;  // gadgets the first focus is chosen from (those of the description)
    bool focus_ring_ = true;  // false: the screen draws no focus ring (option pages: their menu's ring)
    bool background_in_box_ = false;  // the picture drawn only inside the screen's box, from its top-left
    const render::Sprite8* background_ = nullptr;  // another picture than the layout's (null: the layout's)
    // Interface colours other than the layout's (screens opened in a battle take the battle's,
    // made from the game palette); null: the layout's.
    const std::array<std::uint8_t, 256>* colours_ = nullptr;
    int focus_ = -1;               // index of the text field taking keys (-1: none)
    bool key_press_ = false;            // the press being handled came from a key
    int lit_field_ = -2;                // the gadget whose focus marking is shown (-2: the focus's)
    // The focus ring held where it was while a screen opened by a press covers this one (until
    // the stack is back to hold_depth_; 0: not held). A press handler that redraws this screen
    // before covering it sets ring_follow_.
    int ring_hold_ = -1;
    std::size_t hold_depth_ = 0;
    bool ring_follow_ = false;
    int held_ = -1;                // index of the pressed button (pointer down)
    int drag_ = -1;                // index of the scroll bar being dragged
    int press_button_ = 1;         // mouse button of the last press (1 left, 3 right)
    int focus_gadget_ = -1;        // the gadget with the focus ring (-1: none)
    int focus_before_press_ = -1;  // the focus before the last press (a press that opens a dialog keeps it)
    std::uint32_t last_click_ms_ = 0;
    int last_click_row_ = -1;
    int last_click_x_ = 0, last_click_y_ = 0;
    // A text list held by the pointer (its index, -1: none), the pointer, the next auto-scroll time.
    int list_held_ = -1;
    int pointer_x_ = 0, pointer_y_ = 0;
    std::uint32_t list_scroll_ms_ = 0;
    // A list's slider held: its index, dragging the knob (else stepping toward the pointer each
    // frame), the pointer row and knob at the press.
    int slider_held_ = -1;
    bool slider_drag_ = false;
    int slider_anchor_ = 0, slider_anchor_knob_ = 0;
    void draw_text_list(const MenuContent& mc, render::Image8& out, const render::ClipRect& clip, const Widget& w, int x, int y);
    void list_pointer(FrontEnd& fe, int index);
    void slider_pointer(FrontEnd& fe, int index);
    Widget* list_of(const Widget& w);
    Widget* slider_of(const Widget& list);

private:
    const ScreenLayout* layout_;
    int hit(int x, int y);
    void scroll_to(Widget& bar, int x, int y);
};

}  // namespace client
