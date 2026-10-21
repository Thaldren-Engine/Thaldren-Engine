// The original game's multiplayer screens on the client's front end (spec: docs/RE_MEMORY.md,
// "Client", "Multiplayer screens"; the network room: "Network and sync").

#include "ta/translate.h"
#include "ta/client_net_screens.h"

#ifdef TA_CLIENT_HAVE_NET

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include <thread>

#include "client/canvas.h"
#include "client/net_menu.h"
#include "net/dplay_serial.h"
#include "net/dplay_session.h"
#include "net/dplay_tcpip.h"
#include "net/protocol.h"
#include "render/raster.h"
#include "ta/cd_check.h"
#include "ta/client_world.h"
#include "ta/net_setup.h"
#include "ta/online_services.h"
#include "ta/restrict_list.h"
#include "ta/tdf.h"
#include "ta/unitdefs.h"

namespace ta {

// The options menu and the stock panel / dimming of the original's screens (client_screens.cpp).
std::unique_ptr<client::Screen> make_options_screen(MenuArt& art);
void stock_panel(MenuArt& art, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int w, int h);
void dim_screen_rect(client::FrontEnd& fe, render::Image8& out, int x0, int y0, int x1, int y1);

namespace {

using client::FrontEnd;
using client::InputEvent;
using client::LayoutScreen;
using client::Widget;
using client::WidgetKind;
namespace keys = client::keys;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool is(const Widget& w, const char* name) { return upper(w.name) == upper(name); }
void sfx(FrontEnd& fe, MenuArt& art, const char* name) { fe.play_sound(art.sound(name)); }

// ------------------------------------------------------------------ layouts
// A screen's layout with the picture the original puts behind it: the multiplayer screens load
// their pictures without putting the picture's palette on the display (the connection screen
// first puts the game palette there and makes the interface colours from it), except the battle
// room, whose picture's palette goes on the display. The interface colours stay the connection
// screen's (0x444580 makes them, 0x4AC7D0, from the game palette then on the display; the room's
// set-up 0x449BB0 does not make them again), also for the room: its chat line's insertion point
// (colour 9) is index 9, not the nearest colour in the room's own palette.
const client::ScreenLayout* net_layout(MenuArt& art, const char* gui, const char* picture, bool own_palette) {
    static std::map<std::string, std::unique_ptr<client::ScreenLayout>> cache;
    const std::string key = upper(gui) + "|" + (picture ? picture : "");
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    const client::ScreenLayout* base = art.layout(gui);
    if (!base) {
        cache[key] = nullptr;
        return nullptr;
    }
    auto L = std::make_unique<client::ScreenLayout>(*base);
    L->background = nullptr;
    L->palette = nullptr;
    if (picture) {
        const render::Palette* pal = nullptr;
        L->background = art.picture(picture, &pal);
        if (own_palette) L->palette = pal;
    }
    for (int k = 0; k < 256; ++k) L->colours[static_cast<std::size_t>(k)] = art.gui_colour(art.content().palette, k);
    const client::ScreenLayout* out = L.get();
    cache[key] = std::move(L);
    return out;
}

// ------------------------------------------------------------------ drawing rules
// Text in a picture font along a line (0x4A50E0): characters below 0x20 and those without a glyph
// are skipped; with a width limit a glyph wider than what is left ends the line; level > 0 draws
// the glyphs through that row of the light table (else their own colours).
void line_text(render::Image8& out, const render::ClipRect& clip, const client::Font& font, const std::string& s, int x, int y, int width, int level,
               const std::vector<std::uint8_t>& light) {
    int left = width;
    for (unsigned char c : s) {
        if (c < 0x20) continue;
        const client::Glyph& g = font.glyphs[c];
        if (!g.present) continue;
        if (width >= 0 && left < g.width) return;
        if (c != ' ') {
            const int rows = g.rows > 0 ? g.rows : font.height;
            for (int r = 0; r < rows; ++r)
                for (int k = 0; k < g.width; ++k) {
                    const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                    if (!g.ink[at]) continue;
                    std::uint8_t v = g.colours.empty() ? 0 : g.colours[at];
                    if (level > 0 && light.size() >= 0x2000) v = light[static_cast<std::size_t>(std::min(level, 31)) * 256 + v];
                    client::put_pixel(out, clip, x + g.left + k, y - font.raise + g.top + r, v);
                }
        }
        if (width >= 0) left -= g.width;
        x += g.width;
    }
}

// A rectangle through a row of the shade table (level < 0: row level + 32, at most 32 down) or
// the light table (level >= 0, at most 31) (0x4BF4D0); the pixel indexes the row as a signed byte.
void shade_rect(FrontEnd& fe, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int x1, int y1, int level) {
    const auto& mc = fe.menu_content();
    const std::vector<std::uint8_t>& t = level < 0 ? mc.shade : mc.light;
    if (t.size() < 0x2000) return;
    const int row = level < 0 ? std::max(level, -32) + 32 : std::min(level, 31);
    x0 = std::max({x0, clip.x0, 0});
    y0 = std::max({y0, clip.y0, 0});
    x1 = std::min({x1, clip.x1, out.width - 1});
    y1 = std::min({y1, clip.y1, out.height - 1});
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            std::uint8_t& p = out.row(y)[x];
            p = t[static_cast<std::size_t>(row * 256 + static_cast<std::int8_t>(p))];
        }
}

// Nine frames tiling a w x h box (0x4B0230): rows of the first frame's height (the top row frames
// 0-2, middle rows 3-5, the last row 6-8 flush with the bottom), columns of its width (left,
// middle repeated, the right one flush with the right edge); a one-frame sequence is blitted once.
void tile9(render::Image8& out, const render::ClipRect& clip, const std::vector<render::Sprite8>* t, int x0, int y0, int w, int h) {
    if (!t || t->empty()) return;
    const render::Sprite8& f0 = t->front();
    if (t->size() < 2) {
        render::blit(out, clip, f0, x0 + f0.x_offset, y0 + f0.y_offset);
        return;
    }
    const int fw = f0.width, fh = f0.height;
    if (fw <= 0 || fh <= 0) return;
    for (int y = 0; y < h;) {
        const int row = y == 0 ? 0 : (h - fh + 1 <= y ? 6 : 3);
        if (h < fh + y) y = h - fh;
        for (int x = 0; x < w;) {
            int col;
            if (fw + x < w) {
                col = x != 0 ? 1 : 0;
            } else {
                col = 2;
                x = w - fw;
            }
            const std::size_t k = static_cast<std::size_t>(row + col);
            if (k < t->size()) render::blit(out, clip, (*t)[k], x0 + x + (*t)[k].x_offset, y0 + y + (*t)[k].y_offset);
            x += fw;
        }
        y += fh;
    }
}

// A list box's state (the original's kind 2 gadget): rows of text, the first shown, the one
// picked; per-row marks (1: the row drawn dimmed, as a row whose text starts with "&G").
struct ListState {
    std::vector<std::string> items;
    int top = 0, selected = 0;
    // As the pass's redraw drew it (held: shown so when a list was pressed after that redraw: the
    // pressed list is drawn again, 0x4A1B40, but the lists of its association only take its pick,
    // 0x4A3780, and show it from the next pass's redraw).
    bool held = false, pressed = false;
    int drawn_top = 0, drawn_selected = 0;
};

// An edit field's state (kind 3): the text, the insertion point and the limit.
struct EditState {
    std::string text;
    int point = 0;
    int max_chars = 63;
};

// A screen of the multiplayer part: its list boxes and edit fields drawn and run by the
// original's rules (0x4A1B40 list boxes, 0x4A4D70 edit fields, 0x4AB720 editing); the other
// gadgets by the front end's layout screen.
class NetScreen : public LayoutScreen {
public:
    NetScreen(MenuArt& art, const client::ScreenLayout* layout) : LayoutScreen(layout), art_(art) {
        for (std::size_t i = 1; i < widgets_.size(); ++i) {
            Widget& w = widgets_[i];
            if (w.kind == WidgetKind::ListBox) {
                lists_[i] = ListState{};
                w.kind = WidgetKind::Surface;
                kinds_[i] = 2;
            } else if (w.kind == WidgetKind::TextField) {
                EditState e;
                e.max_chars = w.max_chars > 0 ? w.max_chars : 63;
                edits_[i] = e;
                w.kind = WidgetKind::Surface;
                kinds_[i] = 3;
            }
        }
        focus_ = -1;
    }

protected:
    int index_of(const std::string& name) const {
        for (std::size_t i = 0; i < widgets_.size(); ++i)
            if (upper(widgets_[i].name) == upper(name)) return static_cast<int>(i);
        return -1;
    }
    ListState* list(const std::string& name) {
        const int i = index_of(name);
        auto it = lists_.find(i);
        return it == lists_.end() ? nullptr : &it->second;
    }
    EditState* edit(const std::string& name) {
        const int i = index_of(name);
        auto it = edits_.find(i);
        return it == edits_.end() ? nullptr : &it->second;
    }
    // A list's scroll bar (the slider of the same association) shows only when the rows do not
    // fit the list's height (0x4A32A0); its arrows go with it.
    void fit_slider(const std::string& list_name, const std::string& slider_name) {
        const ListState* L = list(list_name);
        const int i = index_of(list_name);
        if (!L || i < 0 || !font()) return;
        int left = widgets_[static_cast<std::size_t>(i)].h;
        for (std::size_t k = 0; k < L->items.size(); ++k) left -= font()->height + 1;
        for (Widget& w : widgets_)
            if (upper(w.name) == upper(slider_name) || (w.slider_step != 0 && upper(w.link) == upper(slider_name))) w.active = left < 0;
    }
    void set_edit(const std::string& name, const std::string& text) {
        if (EditState* e = edit(name)) {
            e->text = text.substr(0, static_cast<std::size_t>(std::max(0, e->max_chars)));
            e->point = static_cast<int>(e->text.size());
        }
    }
    // The edit field taking the keys (its index, -1 none) and holding the focus (0x4A7830 with
    // 0x49FC50: the screen gives it both).
    void focus_edit(const std::string& name) {
        focus_name_ = name;
        activate_field(index_of(name));
    }

    // A list row was picked (double: a second click on the same row soon after, or Enter).
    virtual void on_list(FrontEnd&, int /*index*/, int /*row*/, bool /*double_click*/) {}
    // Enter in an edit field.
    virtual void on_enter(FrontEnd&, int /*index*/) {}

    const client::Font* font() const { return art_.content().font; }

    void draw(FrontEnd& fe, render::Image8& out) override {
        if (!placed_) {
            place(out.width, out.height);
            if (!focus_name_.empty()) focus_gadget_ = index_of(focus_name_);
        }
        LayoutScreen::draw(fe, out);
        for (auto& [i, L] : lists_) L.held = L.pressed = false;
        list_pressed_ = false;
    }
    bool list_pressed_ = false;  // a list was pressed this pass (after the pass's redraw)
    void pass_redrawn() override {
        for (auto& [i, L] : lists_) {
            L.held = true;
            L.drawn_top = L.top;
            L.drawn_selected = L.selected;
        }
    }
    // List boxes and edit fields take no focus ring (the edit field's focus is its lit text).
    bool rings(int i) const override { return !lists_.count(i) && !edits_.count(i); }
    int gadget_kind(int i) const override {
        if (auto k = kinds_.find(i); k != kinds_.end() && (k->second == 2 || k->second == 3)) return k->second;
        return LayoutScreen::gadget_kind(i);
    }
    std::string* field_text(int i) override {
        auto it = edits_.find(i);
        return it == edits_.end() ? nullptr : &it->second.text;
    }
    int* field_cursor(int i) override {
        auto it = edits_.find(i);
        return it == edits_.end() ? nullptr : &it->second.point;
    }
    int field_limit(int i) const override {
        auto it = edits_.find(i);
        return it == edits_.end() ? 63 : it->second.max_chars;
    }
    // Up / Down on a focused list (0x4A9830 / 0x4A99C0): a shown selection moves one row (not past
    // the last, the list scrolling with it), one out of view is brought into view; the list's
    // callback follows.
    void list_key(FrontEnd& fe, int i, int dir) override {
        auto it = lists_.find(i);
        if (it == lists_.end()) return;
        ListState& L = it->second;
        const Widget& w = widgets_[static_cast<std::size_t>(i)];
        const int rows = font() ? (w.h - 2) / (font()->height + 1) : 1, count = static_cast<int>(L.items.size());
        const int max_top = std::max(0, count - rows);
        if (L.selected < L.top + rows && L.top <= L.selected) {
            if (dir < 0 && count != 0 && L.selected != 0) {
                if (--L.selected < L.top) --L.top;
            } else if (dir > 0 && count != 0 && L.selected != max_top - 1 + rows) {
                ++L.selected;
                if (L.top + rows - 1 < L.selected) ++L.top;
                if (count - 1 <= L.selected) L.selected = count - 1;
            }
        } else if (count != 0) {
            L.top = std::min(L.selected, max_top);
        }
        on_list(fe, i, L.selected, false);
    }
    // A list acted on (Space) or a field ended (Enter, Escape) by a key: the screen's handler.
    void key_press(FrontEnd& fe, int i) override {
        if (auto it = lists_.find(i); it != lists_.end()) {
            focus_gadget_ = i;
            on_list(fe, i, it->second.selected, true);
            return;
        }
        if (edits_.count(i)) {
            focus_gadget_ = i;
            on_enter(fe, i);
            return;
        }
        LayoutScreen::key_press(fe, i);
    }
    std::string focus_name_;  // a gadget the screen gives the focus when it opens
    // No picture behind the screen's gadgets (a dialog over the stock panel): lists and edit
    // fields without attribute 1 get the stock "Listbox" box 3 pixels around them (0x4B0230).
    bool boxed_ = false;

    void draw_surface(FrontEnd& fe, render::Image8& out, const Widget& w) override {
        const int i = static_cast<int>(&w - widgets_.data());
        if (auto it = lists_.find(i); it != lists_.end()) draw_list(fe, out, w, it->second);
        else if (auto e = edits_.find(i); e != edits_.end()) draw_edit(fe, out, w, e->second, i == field_mark_, shows_point(i));
        else draw_other(fe, out, w);
    }
    virtual void draw_other(FrontEnd&, render::Image8&, const Widget&) {}

    render::ClipRect screen_clip(const render::Image8& out) const {
        render::ClipRect c = out.bounds();
        if (!widgets_.empty()) {
            const Widget& h = widgets_.front();
            c.x0 = std::max(c.x0, ox_);
            c.y0 = std::max(c.y0, oy_);
            c.x1 = std::min(c.x1, ox_ + h.w - 1);
            c.y1 = std::min(c.y1, oy_ + h.h - 1);
        }
        return c;
    }

    // 0x4A1B40, text lists: rows one font height + 1 apart from 2 pixels in; each row's text
    // (attribs 1 left from x + 2, 4 right-aligned on x + w, 2 centred between them) along the line
    // with the width limit; the picked row (unless attribs 0x100, and when the list has rows) then
    // goes through light row 30 from x + 2 to x + w, one row taller than the spacing. Rows are
    // drawn while a whole font height is left.
    void draw_list(FrontEnd& fe, render::Image8& out, const Widget& w, const ListState& now) {
        ListState L = now;
        if (now.held && list_pressed_) {
            L.top = now.drawn_top;
            L.selected = now.drawn_selected;
        }
        const client::Font* f = font();
        if (!f) return;
        const render::ClipRect clip = screen_clip(out);
        const int x = ox_ + w.x, y = oy_ + w.y;
        if (boxed_ && !(w.attribs & 0x80)) tile9(out, clip, art_.sequence("anims\\commongui.GAF", "Listbox"), x - 3, y - 3, w.w + 6, w.h + 6);
        const int rh = f->height + 1;
        int left = w.h;
        for (int r = 0, i = L.top; i < static_cast<int>(L.items.size()); ++r, ++i) {
            const int x0 = x + 2, x1 = x + w.w, ry = y + 2 + r * rh;
            const std::string& t = L.items[static_cast<std::size_t>(i)];
            const int tw = f->text_width(t);
            int tx = x0, width = x1 - x0 + 1;
            if (w.attribs & 1) {
            } else if (w.attribs & 4) {
                tx = x1 - tw;
                width = tw;
            } else if (w.attribs & 2) {
                tx = std::max(x0, (x0 + x1 - tw) / 2);
                width = x1 - tx + 1;
            }
            line_text(out, clip, *f, t, tx, ry, width, 0, fe.menu_content().light);
            if (!(w.attribs & 0x100) && L.selected == i) shade_rect(fe, out, clip, x0, ry, x1, ry + rh, 30);
            // A list pressed this pass is drawn again over its own box (x .. x + w - 1): the
            // column x + w keeps the pick the pass's redraw shaded there.
            if (now.pressed && !(w.attribs & 0x100) && now.drawn_selected != L.selected && now.drawn_top == L.top && now.drawn_selected == i)
                shade_rect(fe, out, clip, x1, ry, x1, ry + rh, 30);
            left -= rh;
            if (left < f->height) break;
        }
    }

    // 0x4A4D70: attribs 1 fills the field with interface colour 0 (else the picture under it
    // shows); the text from (x, y + 3) along the line within w - 1, through light row 30 when the
    // last focus marking marked the field (lit: one pass behind the focus, see field_mark_); the
    // field taking the keys (focused) shows the insertion point, a line in interface colour 9 from
    // y + 3 down one font height, at the width of the text before it.
    void draw_edit(FrontEnd& fe, render::Image8& out, const Widget& w, const EditState& e, bool lit, bool focused) {
        const client::Font* f = font();
        const render::ClipRect clip = screen_clip(out);
        const int x = ox_ + w.x, y = oy_ + w.y;
        if (w.attribs & 1) client::fill_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, colour(0));
        else if (boxed_) tile9(out, clip, art_.sequence("anims\\commongui.GAF", "Listbox"), x - 3, y - 3, w.w + 6, w.h + 6);
        if (!f) return;
        line_text(out, clip, *f, e.text, x, y + 3, w.w - 1, lit ? 30 : 0, fe.menu_content().light);
        if (focused) {
            const int cx = x + f->text_width(e.text.substr(0, static_cast<std::size_t>(std::clamp(e.point, 0, static_cast<int>(e.text.size())))));
            client::fill_rect(out, clip, cx, y + 3, cx, y + 3 + f->height, colour(9));
        }
    }

    void handle(FrontEnd& fe, const InputEvent& e) override {
        using T = InputEvent::Type;
        if (e.type == T::MouseDown) {
            note_pass_focus();  // the pass's redraw comes before the pointer's events
            for (auto& [i, L] : lists_) {
                const Widget& w = widgets_[static_cast<std::size_t>(i)];
                if (!w.active || e.x < ox_ + w.x || e.y < oy_ + w.y || e.x >= ox_ + w.x + w.w || e.y >= oy_ + w.y + w.h) continue;
                // A pressed list holds the focus; no field takes the keys; the list is drawn
                // again (0x4A1B40), the others of its association at the next pass.
                focus_gadget_ = i;
                focus_ = -1;
                L.held = false;
                L.pressed = list_pressed_ = true;
                const int rh = font() ? font()->height + 1 : 12;
                const int row = (e.y - (oy_ + w.y) - 2) / rh + L.top;
                if (row >= 0 && row < static_cast<int>(L.items.size())) {
                    const bool dbl = row == last_row_ && i == last_list_ && e.time_ms - last_ms_ < 350;
                    last_ms_ = e.time_ms;
                    last_row_ = row;
                    last_list_ = i;
                    L.selected = row;
                    on_list(fe, i, row, dbl);
                }
                return;
            }
            for (auto& [i, E] : edits_) {
                const Widget& w = widgets_[static_cast<std::size_t>(i)];
                if (!w.active || e.x < ox_ + w.x || e.y < oy_ + w.y || e.x >= ox_ + w.x + w.w || e.y >= oy_ + w.y + w.h) continue;
                focus_gadget_ = i;
                focus_ = i;
                return;
            }
        }
        // Keys: the layout screen's rules (the original's interface pass) with this screen's lists
        // and fields.
        LayoutScreen::handle(fe, e);
    }

protected:
    MenuArt& art_;
    std::map<int, ListState> lists_;
    std::map<int, EditState> edits_;
    std::map<int, int> kinds_;
    std::uint32_t last_ms_ = 0;
    int last_row_ = -1, last_list_ = -1;
};

// ------------------------------------------------------------------ message box (MSGBOX.GUI)
// 0x4ABD90: the text word-wrapped to the given width (0x4AC4C0), each line a centred label 20
// pixels down and then the font height + 5 apart; the box as wide as the widest line + 20 (or the
// given width), as tall as lines x 25 + the OK button's height + 40, centred on the screen, OK at
// 15 pixels in from the bottom-right corner; the screen under it dimmed (open flag 0x800); the
// stock panel. Without OK (the caller's switch 0) the gadget is hidden and the box keeps its room.
class NetMessage : public LayoutScreen {
public:
    NetMessage(MenuArt& art, const std::string& text_in, int width, bool fit, std::function<void(FrontEnd&)> done = {}, bool with_ok = true)
        : LayoutScreen(art.layout("MSGBOX")), art_(art), done_(std::move(done)) {
        // The text goes through the translation first (0x4ABDC7; a caller's width stays the one
        // it measured).
        const std::string text = translate(text_in);
        // Measured in the interface's caption font (the labels then draw in the small one).
        const client::Font* f = art.content().font;
        // Word wrap (0x4AC4C0): at the end of a word, when the line is at least as wide as the
        // limit, its last space or hyphen becomes "\r\n"; the result is cut into labels at each
        // '\n' (strtok: no empty ones), a broken line keeping its '\r' (counted by the box's width,
        // 0x4A5030, and by the label's centring).
        const std::string wrapped = f ? wrap_for_width(*f, text, width, true) : text;
        for (std::size_t at = 0; at <= wrapped.size();) {
            const std::size_t nl = std::min(wrapped.find('\n', at), wrapped.size());
            if (nl > at) lines_.push_back(wrapped.substr(at, nl - at));
            at = nl + 1;
        }
        int w = width;
        if (fit) {
            w = 0;
            for (const std::string& l : lines_) w = std::max(w, f ? f->text_width(l) : 0);
            w += 20;
        }
        if (!widgets_.empty()) {
            Widget& h = widgets_.front();
            const Widget* ok = widget("OK");
            h.w = w;
            h.h = static_cast<int>(lines_.size()) * 25 + (ok ? ok->h : 0) + 40;
            open_centred_ = -1;
            if (Widget* o = widget("OK")) {
                o->x = h.w - o->w - 15;
                o->y = h.h - o->h - 15;
                if (!with_ok) o->active = false;
            }
        }
        line_height_ = f ? f->height : 12;
    }
    bool see_through() const override { return true; }
    // Closes itself at its (passes + 1)th own pass: drawn in the pass that opened it and in
    // `passes` more (a box the original covers with the next screen it opens).
    void close_after(int passes) { close_after_ = passes; }
    void frame(FrontEnd& fe, std::uint32_t now) override {
        LayoutScreen::frame(fe, now);
        if (close_after_ > 0 && ++passes_ > close_after_) fe.pop();
    }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_screen_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        const Widget& h = widgets_.front();
        stock_panel(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
        const client::Font* f = fe.menu_content().small_font ? fe.menu_content().small_font : fe.menu_content().font;
        if (!f) return;
        int y = 20;
        for (const std::string& l : lines_) {
            Widget lab;
            lab.kind = WidgetKind::Label;
            lab.x = 0;
            lab.y = y;
            lab.w = h.w;
            lab.h = line_height_;
            lab.attribs = 2;
            lab.fg = 0;
            lab.text = l;
            draw_widget(fe, out, lab, false);
            y += line_height_ + 5;
        }
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (!is(w, "OK")) return;
        auto done = done_;
        fe.pop();
        if (done) done(fe);
    }

protected:
    MenuArt& art_;
    std::vector<std::string> lines_;
    int line_height_ = 12;
    std::function<void(FrontEnd&)> done_;
    int close_after_ = 0, passes_ = 0;
};

void show_net_message(FrontEnd& fe, MenuArt& art, const std::string& text, int width = 200, std::function<void(FrontEnd&)> done = {}) {
    if (art.layout("MSGBOX")) fe.push(std::make_unique<NetMessage>(art, text, width, true, std::move(done)));
    else std::printf("message: %s\n", text.c_str());
}

// A message left for the menu loop (0x425730 into the buffer 0x511FB8, shown by 0x426E80): a box
// as wide as the text + 20 (measured untranslated; the box translates it). The width (0x4A5030) is the interface's
// picture font's when it has one (GUI +0x14: hattfont12, set by the start-up 0x491200 and kept in
// the menus), else the current bit font's: the sum of the font's frame widths of every byte up to
// the end (a byte without a frame adds nothing; control characters and line breaks are counted
// like the others).
void show_loop_message(FrontEnd& fe, MenuArt& art, const char* text) {
    int width = 0;
    if (const client::Font* f = art.content().font)
        for (const char* p = text; *p; ++p) width += f->glyphs[static_cast<unsigned char>(*p)].width;
    show_net_message(fe, art, text, width + 20);
}

// The service could not be used (0x426E80 state 0x14: 0x450D80 failed): the text for the menu
// loop and back to the provider list (state 0x0F).
constexpr const char* kServiceError = "An error occurred trying to use this service";

// A player could not be added (0x451220): 500 wide. The text is picked after the failed player's
// record has been set back to controller 0 (0x463C60): the first record 0-9 with a controller
// and the host flag (side record +0x97 bit 0) — this machine's own no longer counts — or, with
// none, the record after the table (index 10, cleared when the game state is made (0x41D920) and
// never seated); when that record is in use with controller 1 or 2 the text asks to re-create the
// session, otherwise to re-join it. Only a record left over from an earlier session could give
// the first text; the client keeps no records between sessions, so it is always the second, also
// on the host.
void show_add_player_failed(FrontEnd& fe, MenuArt& art) {
    show_net_message(fe, art, translate("Direct Play failed to add new player.\n\nRecommended you go to previous screen and re-join the game session.\n"), 500);
}

// ------------------------------------------------------------------ shared state of the part
// The session service's providers as the original lists them (the system's provider list in the
// order seen with the original program: serial, modem, TCP/IP; no IPX on current systems), here
// run by the engine's own implementation of the protocol (net/dplay_session.h).
enum class ProviderKind { Serial, Modem, Tcpip };
struct Provider {
    std::string name;
    ProviderKind kind = ProviderKind::Tcpip;
};
const std::vector<Provider>& providers() {
    static const std::vector<Provider> p = {{"Serial Connection For DirectPlay", ProviderKind::Serial},
                                            {"Modem Connection For DirectPlay", ProviderKind::Modem},
                                            {"Internet TCP/IP Connection For DirectPlay", ProviderKind::Tcpip}};
    return p;
}

// What the screens share: the configuration, the chosen transport, the address and names.
struct NetPart {
    MenuArt& art;
    client::NetMenuConfig cfg;
    int provider = 0;
    // The session (the original's session protocol over the chosen provider) and what was found.
    std::unique_ptr<net::dplay::Session> session;
    std::vector<net::dplay::FoundSession> found;
    std::string port = "COM1", phone;  // serial / modem
    std::uint32_t baud = 115200;
    bool answer = false;               // modem: wait for a call instead of dialing
    // TCP/IP on this machine only (setting NetLoopback; tests): 0 = off, else the loopback address
    // the sockets are bound to ("1": 127.0.0.1; a 127.x.y.z address keeps concurrent test runs
    // apart, each with its own session port).
    std::uint32_t loopback = 0;
    std::string address;      // the host's address (empty: this machine)
    std::string nickname;     // the player's name (the original's GS+0x2BD2)
    std::string password;     // GS+0x2BE3
    std::string game_name;    // GS+0x2BC1 (the new game's name)
    bool test = false;        // tests: no network
    NetPart(MenuArt& a, client::NetMenuConfig c) : art(a), cfg(std::move(c)) {}
};

// 0x4436E0 as a network session starts (the joined game 0x427FE2, the hosted one 0x44100B): the
// scores reporting is tried with the session provider's GUID (ta/cd_check.h). No reporter.dll
// ships, and this program could not load the 32-bit one: the load fails, no service is listed,
// the report screen is not opened and nothing is shown (the hourglass pointer the original sets
// around it is put back before a frame is drawn).
void start_scores_reporting(const NetPart& p) {
    static const std::array<std::uint8_t, 16> kTcp{0xE0, 0x5E, 0xE9, 0x36, 0x77, 0x85, 0xCF, 0x11, 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82};
    static const std::array<std::uint8_t, 16> kIpx{0x00, 0xC4, 0x5B, 0x68, 0x2C, 0x9D, 0xCF, 0x11, 0xA9, 0xCD, 0x00, 0xAA, 0x00, 0x68, 0x86, 0xE3};
    static const std::array<std::uint8_t, 16> kModem{0x60, 0xA7, 0xEA, 0x44, 0x68, 0xCB, 0xCF, 0x11, 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E};
    static const std::array<std::uint8_t, 16> kSerial{0x60, 0x68, 0x1D, 0x0F, 0xD9, 0x88, 0xCF, 0x11, 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E};
    ta::ScoresReportingEnv env;
    if (p.provider == 1) env.provider = kTcp;
    else if (p.provider == 2) env.provider = kIpx;
    else if (p.provider == 3) env.provider = kModem;
    else if (p.provider == 4) env.provider = kSerial;
    env.load_library = ta::reporter_library_not_loadable;
    ta::ScoresReportingState st;
    const ta::ScoresReporting r = ta::start_scores_reporting(env, st);
    if (ta::scores_reporting_shown(r, st) != ta::ScoresReportingShown::Nothing) std::printf("scores reporting: unexpected outcome\n");
}

std::string os_user_name() {
    // The original takes the system's user name when no name was given (GetUserNameA).
    for (const char* v : {"USERNAME", "USER"})
        if (const char* s = std::getenv(v); s && *s) return std::string(s).substr(0, 16);
    return "Player";
}

std::unique_ptr<client::Screen> make_selgame(std::shared_ptr<NetPart> part, std::vector<struct GameEntry> games);

// ------------------------------------------------------------------ connection (SELPROV.GUI)
// 0x444580 (handler 0x4441A0): over selconnect2 (the screen cleared first), the game palette on
// the display; DPLAY lists the providers; SELECT or a pick (double click) goes on with the picked
// provider (BigButton): a TCP/IP provider asks for the address (TCP.GUI); SETTINGS opens the
// options (Options); PREVMENU returns to the main menu (Previous).
class TcpAsk;
class SelProv : public NetScreen {
public:
    explicit SelProv(std::shared_ptr<NetPart> part) : NetScreen(part->art, net_layout(part->art, "SELPROV", "selconnect2", false)), part_(part) {
        enter_default_ = "SELECT";  // 0x444580 names it (Enter = Select)
        if (ListState* L = list("DPLAY"))
            for (const Provider& p : providers()) L->items.push_back(p.name);
        fit_slider("DPLAY", "SLIDER");
        add_service_buttons();
    }
    void enter(FrontEnd& fe) override {
        NetScreen::enter(fe);
        // -n (0x443FF0 with no pick): the provider named by the switch, if this machine has it
        // (1 TCP/IP, 2 IPX, 3 modem, 4 serial); the switch is used up either way.
        auto& st = part_->cfg.startup;
        if (!st || st->provider == 0) return;
        const int want = st->provider;
        st->provider = 0;
        for (std::size_t k = 0; k < providers().size(); ++k) {
            const ProviderKind kind = providers()[k].kind;
            if ((want == 1 && kind == ProviderKind::Tcpip) || (want == 3 && kind == ProviderKind::Modem) ||
                (want == 4 && kind == ProviderKind::Serial)) {
                if (ListState* L = list("DPLAY")) L->selected = static_cast<int>(k);
                choose(fe, static_cast<int>(k));
                return;
            }
        }
    }

protected:
    void on_list(FrontEnd& fe, int, int row, bool dbl) override {
        if (dbl) choose(fe, row);
    }
    void on_press(FrontEnd& fe, Widget& w) override;
    void choose(FrontEnd& fe, int row);
    void add_service_buttons();
    void press_service(FrontEnd& fe, const Widget& w);
    std::shared_ptr<NetPart> part_;
    // The online services' links (0x45B490 into the table 0x5127C8) and what answers them.
    std::unique_ptr<OnlineServices> services_;
    std::vector<OnlineLink> links_;
};

// The online-service buttons (0x444580 after the screen opened: 0x45B490 then 0x44475D): for
// each link a copy of the hidden template SERVICEX (its whole record) added after the screen's
// gadgets, its caption set to the link's name (0x4A09C0: translated, a quick key from it), named
// SERVICE<n> (n the link's index), at the template's y and then each one the template's height +
// 1 lower, made active. No online.dll (or one older than version 3): no buttons.
void SelProv::add_service_buttons() {
    services_ = std::make_unique<OnlineServices>(real_online_system(), art_.game_dir());
    links_ = services_->links([](const std::string& s) { return translate(s); });
    const int t = index_of("SERVICEX");
    if (t < 0 || links_.empty()) return;
    int y = widgets_[static_cast<std::size_t>(t)].y;
    for (std::size_t i = 0; i < links_.size(); ++i) {
        const Widget tmpl = widgets_[static_cast<std::size_t>(t)];
        widgets_.push_back(tmpl);
        set_caption(widgets_.back(), links_[i].name);
        Widget& w = widgets_.back();
        w.name = "SERVICE" + std::to_string(i);
        w.y = y;
        w.active = true;
        y += tmpl.h + 1;
    }
}

// A service button pressed (0x4441A0: a gadget whose name starts with SERVICE): BigButton; the
// link of the button's number to online.dll (0x45B250; the pointer shows the wait shape only
// while the call runs, no frame is drawn in between). Started: online.dll is let go and the
// program ends (exit(0), no further frame). Otherwise a 320-wide message box (fitted, with OK) of
// the reason, translated (and again by the box): the installation CD question, online.dll's
// reason, or without one "The <name> service failed or took ... file for more information.".
void SelProv::press_service(FrontEnd& fe, const Widget& w) {
    sfx(fe, art_, "BigButton");
    const int n = std::atoi(w.name.c_str() + 7);
    if (n < 0 || static_cast<std::size_t>(n) >= links_.size() || !services_) return;
    std::string message;
    const int r = services_->press(links_[static_cast<std::size_t>(n)].id, message, fe.hooks().open_external, [](const std::string& s) { return translate(s); });
    if (r == 0) {
        fe.exit_at_once();
        return;
    }
    if (message.empty()) message = OnlineServices::failure_text(links_[static_cast<std::size_t>(n)].name);
    show_net_message(fe, art_, translate(message), 320);
}

// ------------------------------------------------------------------ the address (TCP.GUI)
// 0x4421F0 (handler 0x442050): opened with flags 0x800 over the connection screen (the screen
// under it dimmed, the stock panel); ADDRESS takes the saved address (registry TCPADDR) and the
// keys; OK (Smlbutton) saves it and goes on to the game list; PREV (Cancel) returns (Previous).
class TcpAsk : public NetScreen {
public:
    explicit TcpAsk(std::shared_ptr<NetPart> part) : NetScreen(part->art, part->art.layout("TCP")), part_(part) {
        if (EditState* e = edit("ADDRESS")) e->max_chars = 63;
        focus_edit("ADDRESS");
        boxed_ = true;
    }
    void enter(FrontEnd& fe) override {
        if (!entered_) {
            entered_ = true;
            // (0x4221F0: the -n address, else the saved one; with the switch's address the
            // dialog goes on at once, and a joining start uses the address up)
            auto& st = part_->cfg.startup;
            if (st && !st->address.empty()) {
                set_edit("ADDRESS", st->address);
                if (!st->host) st->address.clear();
                ok(fe);
                return;
            }
            set_edit("ADDRESS", fe.settings().get_text("TCPADDR", ""));
        }
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_screen_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        const Widget& h = widgets_.front();
        stock_panel(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_enter(FrontEnd& fe, int) override { ok(fe); }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) ok(fe);
        else if (is(w, "PREV")) {
            sfx(fe, art_, "Previous");
            fe.pop();
        }
    }
    void ok(FrontEnd& fe);

private:
    std::shared_ptr<NetPart> part_;
    bool entered_ = false;
};

void SelProv::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "SELECT")) {
        if (ListState* L = list("DPLAY")) choose(fe, L->selected);
    } else if (upper(w.name).compare(0, 7, "SERVICE") == 0) {
        press_service(fe, w);
    } else if (is(w, "SETTINGS")) {
        sfx(fe, art_, "Options");
        fe.push(make_options_screen(art_));
    } else if (is(w, "PREVMENU")) {
        sfx(fe, art_, "Previous");
        fe.pop();
    }
}

// ------------------------------------------------------------------ the session (engine side)
// Opens the chosen provider (TCP/IP at the typed address, the serial port, the modem's port).
bool open_provider(NetPart& part, std::string& error) {
    part.session.reset();
    std::unique_ptr<net::dplay::Provider> p;
    const ProviderKind kind = providers()[static_cast<std::size_t>(part.provider)].kind;
    if (kind == ProviderKind::Tcpip) {
        net::dplay::TcpipOptions o;
        o.address = part.address;
        if (part.loopback) o.bind_ip = part.loopback;
        p = net::dplay::tcpip_provider(o, error);
    } else {
        net::dplay::SerialOptions o;
        o.port = part.port;
        o.baud = part.baud;
        o.modem = kind == ProviderKind::Modem;
        o.answer = part.answer;
        o.phone = part.phone;
        p = net::dplay::serial_provider(o, error);
    }
    if (!p) return false;
    std::function<void(const std::string&)> log;
    if (const char* path = std::getenv("TA_DPLAY_LOG")) {
        // Tests: every session message, one line each (dplay_play decode reads it).
        auto f = std::make_shared<std::ofstream>(path, std::ios::app);
        log = [f](const std::string& l) {
            *f << l << "\n";
            f->flush();
        };
    }
    part.session = std::make_unique<net::dplay::Session>(std::move(p), log);
    return true;
}

// Asks for the games (0x4C9E50: the session service enumerates for 1.5 s; a modem first dials).
void find_games(NetPart& part) {
    part.found.clear();
    if (!part.session) return;
    using clock = std::chrono::steady_clock;
    const auto limit = clock::now() + std::chrono::seconds(45);
    while (!part.session->provider().ready() && clock::now() < limit && part.session->error().empty()) {
        part.session->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    part.session->find();
    const auto until = clock::now() + std::chrono::milliseconds(1500);
    while (clock::now() < until) {
        part.session->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    part.found = part.session->sessions();
}

// The games asked for as the game list does it (0x441460): before the session service is asked
// (0x4C9E50) a box is put on the screen, 150 wide, fitted to its text, no OK (0x4ABD90 with
// 0x96, 0, 1; the text translated): "Updating..." with the TCP/IP provider, "Connecting  (ESC to
// abort)" with any other; it goes when the answer is in (0x4A9660). The enumeration repeats while
// the service is still connecting (a modem dialling); Escape then gives up. A failed enumeration
// (an address the service cannot use, Escape) returns failure (-1). Here the box stays up while
// the session is pumped pass by pass; `done` gets whether the games were found.
class FindGamesBox : public NetMessage {
public:
    FindGamesBox(std::shared_ptr<NetPart> part, std::function<void(FrontEnd&, bool)> done)
        : NetMessage(part->art, translate(box_text(*part)), 0x96, true, {}, false), part_(std::move(part)), found_(std::move(done)) {}
    static const char* box_text(const NetPart& p) {
        return providers()[static_cast<std::size_t>(p.provider)].kind == ProviderKind::Tcpip ? "Updating..." : "Connecting  (ESC to abort)";
    }
    void handle(FrontEnd& fe, const InputEvent& e) override {
        // (0x4C9E50 reads only the key queue: Escape stops it.)
        if (e.type == InputEvent::Type::KeyDown && e.key == client::keys::kEscape) finish(fe, false);
    }
    void frame(FrontEnd& fe, std::uint32_t) override {
        if (finished_) return;
        // (the box is on the screen before the service is asked: one drawn pass first)
        if (!shown_) {
            shown_ = true;
            return;
        }
        using clock = std::chrono::steady_clock;
        NetPart& part = *part_;
        if (!part.session) return finish(fe, false);
        part.session->pump();
        if (!part.session->error().empty()) return finish(fe, false);
        if (!asked_) {
            if (!started_) {
                started_ = true;
                part.found.clear();
                limit_ = clock::now() + std::chrono::seconds(45);
            }
            if (!part.session->provider().ready()) {
                if (clock::now() >= limit_) finish(fe, false);
                return;
            }
            asked_ = true;
            part.session->find();
            until_ = clock::now() + std::chrono::milliseconds(1500);
            return;
        }
        if (clock::now() < until_) return;
        part.found = part.session->sessions();
        finish(fe, true);
    }

private:
    void finish(FrontEnd& fe, bool ok) {
        if (finished_) return;
        finished_ = true;
        auto done = found_;
        fe.pop();
        if (done) done(fe, ok);
    }
    std::shared_ptr<NetPart> part_;
    std::function<void(FrontEnd&, bool)> found_;
    bool shown_ = false, started_ = false, asked_ = false, finished_ = false;
    std::chrono::steady_clock::time_point limit_{}, until_{};
};

// 0x443CB0 when the games could not be asked for: the game list closes, "Invalid TCP/IP Address"
// (whatever the provider) is shown 200 wide with OK, and the menu state goes back to the
// connection screen (sub-state 3: the service closed, 0x450DD0; state 0x0F: SELPROV opened anew,
// 0x444580). The new connection screen is opened over the box two passes later, so the box is
// seen for two frames and then lies under the connection screen; here it closes itself then and
// the connection screen below shows.
void show_invalid_address(FrontEnd& fe, NetPart& part) {
    part.session.reset();
    if (!part.art.layout("MSGBOX")) return;
    auto box = std::make_unique<NetMessage>(part.art, translate("Invalid TCP/IP Address"), 200, true);
    box->close_after(1);
    fe.push(std::move(box));
}

// ------------------------------------------------------------------ serial (SERIAL.GUI)
// 0x442560 (handler 0x4423C0): PORTS (COM1..COM4, the registry's SERPORT picked), SPEEDS (115200,
// 57600, 38400, 19200, 14400, 9600; SERBAUD); HOST waits on the cable for a game of its own, JOIN
// asks the cable for games; one stop bit, no parity, hardware flow control (0x441C30).
class SerialAsk : public NetScreen {
public:
    explicit SerialAsk(std::shared_ptr<NetPart> part) : NetScreen(part->art, part->art.layout("SERIAL")), part_(part) {
        if (ListState* L = list("PORTS")) L->items = {"COM1", "COM2", "COM3", "COM4"};
        if (ListState* L = list("SPEEDS")) L->items = {"115200", "57600", "38400", "19200", "14400", "9600"};
        boxed_ = true;
    }
    void enter(FrontEnd& fe) override {
        if (entered_) return;
        entered_ = true;
        if (ListState* L = list("PORTS")) L->selected = std::clamp(fe.settings().get_int("SERPORT", 0), 0, 3);
        if (ListState* L = list("SPEEDS")) L->selected = std::clamp(fe.settings().get_int("SERBAUD", 0), 0, 5);
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_screen_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        if (widgets_.empty()) return;
        const Widget& h = widgets_.front();
        stock_panel(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override;

private:
    std::shared_ptr<NetPart> part_;
    bool entered_ = false;
};

// ------------------------------------------------------------------ modem (MODEM.GUI)
// 0x443100 (handler 0x442A30): NUMBER (the number to dial; the address book ACCOUNTS of the
// registry's MODEMACCOUNTS / MODEMNUMBERS), MODEMS (the system's modems: here the ports a modem
// answers Hayes commands on), HOST answers a call, JOIN dials.
class ModemAsk : public NetScreen {
public:
    explicit ModemAsk(std::shared_ptr<NetPart> part) : NetScreen(part->art, part->art.layout("MODEM")), part_(part) {
        if (ListState* L = list("MODEMS")) L->items = {"COM1", "COM2", "COM3", "COM4"};
        if (EditState* e = edit("NUMBER")) e->max_chars = 40;
        // (No field takes the keys until one is clicked or reached with the keys: 0x443100
        // activates none.)
        boxed_ = true;
    }
    void enter(FrontEnd& fe) override {
        if (entered_) return;
        entered_ = true;
        set_edit("NUMBER", fe.settings().get_text("MODEMNUMBER", ""));
        if (ListState* L = list("MODEMS")) L->selected = std::clamp(fe.settings().get_int("MODEMPORT", 0), 0, 3);
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_screen_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        if (widgets_.empty()) return;
        const Widget& h = widgets_.front();
        stock_panel(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override;

private:
    std::shared_ptr<NetPart> part_;
    bool entered_ = false;
};

void SelProv::choose(FrontEnd& fe, int row) {
    if (row < 0 || row >= static_cast<int>(providers().size())) return;
    sfx(fe, art_, "BigButton");
    part_->provider = row;
    switch (providers()[static_cast<std::size_t>(row)].kind) {
    case ProviderKind::Serial: fe.push(std::make_unique<SerialAsk>(part_)); break;
    case ProviderKind::Modem: fe.push(std::make_unique<ModemAsk>(part_)); break;
    case ProviderKind::Tcpip: fe.push(std::make_unique<TcpAsk>(part_)); break;
    }
}

// ------------------------------------------------------------------ the game list (SELGAME.GUI)
// One game as the session service describes it: its name and map (the session's name: 16 places
// of game name, 15 of map name), the most players, and the host's side record fields +0x99..+0xA8
// (memory, option word with the player count in its low four bits, CD word, the host's best ping,
// energy and metal in hundreds, unit limit, version).
struct GameEntry {
    std::string name, map;
    int max_players = 10, players = 1;
    std::uint16_t options = 0, memory = 0, cd = 0, ping = 0, energy = 10, metal = 10, unit_limit = 250;
    std::uint8_t major = 3, minor = 1;
    bool password = false;
};

// The game list's columns (0x441460): name (16 characters), "%d/%d" players, map (15, trailing
// spaces dropped), status ("VER!" when the version's major number is below this program's 3,
// else "Lock" (option bit 15), "Play" (bit 4), "Open"), memory (hidden column), metal and energy
// (hundreds x 100), ping, commander ("No" for continues, "Yes" for game ends, "DM"), full map
// ("Blk" with mapping, else "Gray") and line of sight ("No" with the option, else "Yes").
std::array<std::string, 11> game_columns(const GameEntry& g) {
    std::array<std::string, 11> c;
    c[0] = g.name.substr(0, 16);
    c[1] = std::to_string(g.options & 0xF) + "/" + std::to_string(g.max_players);
    std::string m = g.map.substr(0, 15);
    while (!m.empty() && m.back() == ' ') m.pop_back();
    // In a language other than English the name is lowered and translated (0x44172F): an
    // untranslated name shows in lower case (at most 127 characters).
    const std::string& lang = Vfs::game_language();
    if (!lang.empty() && stricmp_c(lang, "english") != 0) m = translate(lower_ascii(m)).substr(0, 0x7F);
    c[2] = m;
    // (The words go through the translation: 0x4417D3, 0x4417F2, 0x441924, 0x441964, 0x4419A3.)
    c[3] = translate(g.major < 3 ? "VER!" : (g.options & 0x8000) ? "Lock" : (g.options & 0x10) ? "Play" : "Open");
    c[4] = std::to_string(g.memory);
    c[5] = std::to_string(g.metal * 100);
    c[6] = std::to_string(g.energy * 100);
    c[7] = std::to_string(g.ping);
    const int cm = g.options & 0x1800;
    c[8] = translate(cm == 0 ? "No" : cm == 0x800 ? "Yes" : "DM");
    c[9] = translate((g.options & 0x100) ? "Blk" : "Gray");
    c[10] = translate((g.options & 0x200) ? "No" : "Yes");
    return c;
}

class NewMulti;
std::vector<GameEntry> found_entries(const NetPart& part);
void open_game_list(FrontEnd& fe, const std::shared_ptr<NetPart>& part);
class SelGame : public NetScreen {
public:
    SelGame(std::shared_ptr<NetPart> part, std::vector<GameEntry> games)
        : NetScreen(part->art, net_layout(part->art, "SELGAME", "selectgame2x", false)), part_(part), games_(std::move(games)) {
        if (EditState* e = edit("NICKNAME")) e->max_chars = 10;
        if (EditState* e = edit("PASSWORD")) e->max_chars = 10;
        set_edit("NICKNAME", part_->nickname);
        set_edit("PASSWORD", part_->password);
        focus_name_ = "GAMENAME";  // 0x4A7830 on the game list
        fill();
    }
    void enter(FrontEnd& fe) override;
    void startup_check(FrontEnd& fe);
    // The games found (the list's rows anew).
    void set_games(std::vector<GameEntry> games) {
        games_ = std::move(games);
        fill();
    }
    bool waiting_ = false;  // the games are being asked for (the box over the list)

protected:
    void fill() {
        static const char* cols[11] = {"GAMENAME", "PLAYERS", "MAPNAME", "STATUS", "", "METAL", "ENERGY", "PING", "COMMANDER", "FULLMAP", "LOS"};
        for (int k = 0; k < 11; ++k) {
            if (!*cols[k]) continue;
            if (ListState* L = list(cols[k])) {
                L->items.clear();
                for (const GameEntry& g : games_) L->items.push_back(game_columns(g)[static_cast<std::size_t>(k)]);
                L->selected = 0;
                L->top = 0;
            }
        }
        fit_slider("GAMENAME", "SLIDER");
        picked(0);
    }
    // 0x441220: a game picked: Watch is grayed without games, for an older version, when the game
    // forbids watching (option bit 7 clear), is closed (bit 15) or playing (bit 4); Join without
    // games, for an older version, closed or playing; the password field shows for a game with one.
    void picked(int row) {
        const bool none = games_.empty() || row < 0 || row >= static_cast<int>(games_.size());
        const GameEntry g = none ? GameEntry{} : games_[static_cast<std::size_t>(row)];
        const bool old = !none && g.major < 3;
        if (Widget* w = widget("WATCH")) w->grayed = none || old || !(g.options & 0x80) || (g.options & 0x8000) || (g.options & 0x10);
        if (Widget* w = widget("JOINGAME")) w->grayed = none || old || (g.options & 0x8000) || (g.options & 0x10);
        if (Widget* w = widget("PASSWORD")) w->active = !none && g.password;
        for (auto& [i, L] : lists_) L.selected = row;
    }
    void on_list(FrontEnd& fe, int, int row, bool dbl) override {
        picked(row);
        if (dbl) join(fe, false);
    }
    void on_press(FrontEnd& fe, Widget& w) override;
    void join(FrontEnd& fe, bool watch);
    void rebuild(FrontEnd& fe);
    std::shared_ptr<NetPart> part_;
    std::vector<GameEntry> games_;
};

// ------------------------------------------------------------------ new game (NEWMULTI.GUI)
// 0x441080 (handler 0x440D70) over createnew: GAMENAME (the last game name), NICKNAME (the
// player's name, else the system's user name), PASSWORD; OK (Next) opens the battle room as the
// host; CANCEL (Previous) returns to the game list.
class NewMulti : public NetScreen {
public:
    explicit NewMulti(std::shared_ptr<NetPart> part) : NetScreen(part->art, net_layout(part->art, "NEWMULTI", "createnew", false)), part_(part) {
        if (EditState* e = edit("GAMENAME")) e->max_chars = 16;
        if (EditState* e = edit("NICKNAME")) e->max_chars = 16;
        if (EditState* e = edit("PASSWORD")) e->max_chars = 10;
        if (part_->nickname.empty()) part_->nickname = os_user_name();
        set_edit("GAMENAME", part_->game_name);
        set_edit("NICKNAME", part_->nickname);
        set_edit("PASSWORD", part_->password);
        // No field takes the keys until one is clicked (the screen activates none).
    }

protected:
    void on_enter(FrontEnd& fe, int) override { next(fe); }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) next(fe);
        else if (is(w, "CANCEL")) {
            sfx(fe, art_, "Previous");
            fe.pop();
        }
    }
    void next(FrontEnd& fe);

private:
    std::shared_ptr<NetPart> part_;
};

}  // namespace

// The battle room and the network side live in the second part of this file.
namespace net_screens_detail {
std::unique_ptr<client::Screen> make_lounge(std::shared_ptr<void> part, bool host, const std::string& room_file);
// The room the host enters when its session or its player could not be made (enter_broken_room).
std::unique_ptr<client::Screen> make_broken_lounge(std::shared_ptr<void> part);
// The room of a live session (the session protocol's transport, this machine's player id).
std::unique_ptr<client::Screen> make_live_lounge(FrontEnd& fe, std::shared_ptr<void> part, bool host, std::unique_ptr<net::Transport> transport,
                                                 std::int32_t id);
}

namespace {

std::shared_ptr<void> as_void(const std::shared_ptr<NetPart>& p) { return std::static_pointer_cast<void>(p); }

// A game list entry from a session's description: its name (16 places of game name, 15 of map
// name) and the host's record bytes +0x99..+0xA8 in its four application words.
GameEntry entry_of(const net::dplay::FoundSession& f) {
    GameEntry g;
    const std::string& n = f.desc.name;
    g.name = n.substr(0, std::min<std::size_t>(16, n.size()));
    g.map = n.size() > 16 ? n.substr(16) : std::string();
    while (!g.name.empty() && g.name.back() == ' ') g.name.pop_back();
    g.max_players = static_cast<int>(f.desc.max_players);
    g.memory = static_cast<std::uint16_t>(f.desc.user[0]);
    g.options = static_cast<std::uint16_t>(f.desc.user[0] >> 16);
    g.cd = static_cast<std::uint16_t>(f.desc.user[1]);
    g.ping = static_cast<std::uint16_t>(f.desc.user[1] >> 16);
    g.energy = static_cast<std::uint16_t>(f.desc.user[2]);
    g.metal = static_cast<std::uint16_t>(f.desc.user[2] >> 16);
    g.unit_limit = static_cast<std::uint16_t>(f.desc.user[3]);
    g.major = static_cast<std::uint8_t>(f.desc.user[3] >> 16);
    g.minor = static_cast<std::uint8_t>(f.desc.user[3] >> 24);
    g.password = (f.desc.flags & net::dplay::kPasswordRequired) != 0 || (g.cd & 1) != 0;
    return g;
}

std::vector<GameEntry> found_entries(const NetPart& part) {
    std::vector<GameEntry> out;
    for (const auto& f : part.found) out.push_back(entry_of(f));
    return out;
}

// 0x443CB0: the game list opened, then the games asked for over it (0x441460's box); when they
// cannot be, the list closes and "Invalid TCP/IP Address" shows (show_invalid_address).
void open_game_list(FrontEnd& fe, const std::shared_ptr<NetPart>& part) {
    if (part->test || !part->session || !part->art.layout("MSGBOX")) {
        if (!part->test) find_games(*part);
        fe.push(make_selgame(part, found_entries(*part)));
        return;
    }
    part->found.clear();
    std::unique_ptr<client::Screen> s = make_selgame(part, {});
    SelGame* list = static_cast<SelGame*>(s.get());
    list->waiting_ = true;
    fe.push(std::move(s));
    fe.push(std::make_unique<FindGamesBox>(part, [part, list](FrontEnd& f, bool ok) {
        list->waiting_ = false;
        if (ok) {
            list->set_games(found_entries(*part));
            list->startup_check(f);
            return;
        }
        f.pop();  // the game list (0x4A9660)
        show_invalid_address(f, *part);
    }));
}

// The player's data in the session (0x451220): the password typed, then the words 0 and 0x50.
std::vector<std::uint8_t> session_player_data(const std::string& password) {
    std::vector<std::uint8_t> d(21, 0);
    std::memcpy(d.data(), password.data(), std::min<std::size_t>(password.size(), 16));
    d[0x13] = 0x50;
    return d;
}

// The host's player could not be made (0x426E80, state 0x10 sub-state 0x11): 0x451540 sets this
// machine's host flag (side record +0x97 bit 0) and the player count to 0 and opens the session
// without looking at the result; 0x451220 then fails to add the player, sets its record back to
// controller 0 and opens the message box (0x4ABD90: MSGBOX pushed onto the open screens, not a
// loop of its own); 0x450A10 (seating the player) is skipped and the state goes to 0x11 all the
// same: the next pass opens LOUNGE2 over the message box (0x449BB0). The room is broken: no
// record is in use (no player rows: this machine's player is missing), the host's controls are
// this machine's (the host flag stayed), the map is the list's first, and what it sends goes to a
// session that is not there. The message box shows again once the room is left. The client has
// no session to send to: the room runs without one.
void enter_broken_room(FrontEnd& fe, std::shared_ptr<NetPart> part) {
    part->session.reset();
    show_add_player_failed(fe, part->art);
    fe.push(net_screens_detail::make_broken_lounge(as_void(part)));
}

// Into the battle room over the session (tests without a session: the 1:1 room from a file).
void enter_room(FrontEnd& fe, std::shared_ptr<NetPart> part, bool host) {
    if (part->test || !part->session) {
        fe.push(net_screens_detail::make_lounge(as_void(part), host, std::string()));
        return;
    }
    using clock = std::chrono::steady_clock;
    net::dplay::Session& s = *part->session;
    std::int32_t id = s.create_player(part->nickname, session_player_data(part->password));
    if (id == 0) {  // a joining machine waits for the host's id
        const auto until = clock::now() + std::chrono::seconds(10);
        net::SessionEvent e;
        while (!id && clock::now() < until) {
            s.pump();
            while (s.next_event(e))
                if (e.kind == net::SessionEvent::Kind::PlayerCreated && e.local) id = e.id;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (id == 0 || id == -1) {  // (session ids are any 32-bit value: XORed with the session key)
        // The host goes on into a broken room (enter_broken_room); a joining machine's press ends
        // with the message (0x4517B0 fails: state 0x12 to sub-state 0, the game list again).
        if (host) enter_broken_room(fe, part);
        else show_add_player_failed(fe, part->art);
        return;
    }
    // The original's battle room (LOUNGE2) over the session.
    std::unique_ptr<net::Transport> transport = std::move(part->session);
    fe.push(net_screens_detail::make_live_lounge(fe, as_void(part), host, std::move(transport), id));
}

void TcpAsk::ok(FrontEnd& fe) {
    sfx(fe, art_, "Smlbutton");
    const EditState* e = edit("ADDRESS");
    part_->address = e ? e->text : std::string();
    fe.settings().set_text("TCPADDR", part_->address);
    fe.pop();
    // The game list at that address (the original: the session service's games found there).
    std::string err;
    if (!part_->test) {
        if (!open_provider(*part_, err)) {
            show_loop_message(fe, art_, kServiceError);
            return;
        }
    }
    open_game_list(fe, part_);
}

void SerialAsk::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "PREV") || is(w, "CANCEL")) {
        sfx(fe, art_, "Previous");
        fe.pop();
        return;
    }
    if (!is(w, "HOST") && !is(w, "JOIN")) return;
    sfx(fe, art_, "Smlbutton");
    static const std::uint32_t speeds[] = {115200, 57600, 38400, 19200, 14400, 9600};
    const int port = list("PORTS") ? list("PORTS")->selected : 0, speed = list("SPEEDS") ? list("SPEEDS")->selected : 0;
    fe.settings().set_int("SERPORT", port);
    fe.settings().set_int("SERBAUD", speed);
    part_->port = "COM" + std::to_string(port + 1);
    part_->baud = speeds[std::clamp(speed, 0, 5)];
    const bool host = is(w, "HOST");
    fe.pop();
    std::string err;
    if (!part_->test && !open_provider(*part_, err)) {
        show_loop_message(fe, art_, kServiceError);
        return;
    }
    if (host) {
        fe.push(std::make_unique<NewMulti>(part_));
        return;
    }
    open_game_list(fe, part_);
}

void ModemAsk::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "PREV") || is(w, "CANCEL")) {
        sfx(fe, art_, "Previous");
        fe.pop();
        return;
    }
    if (!is(w, "HOST") && !is(w, "JOIN")) return;
    sfx(fe, art_, "Smlbutton");
    const EditState* n = edit("NUMBER");
    part_->phone = n ? n->text : std::string();
    const int port = list("MODEMS") ? list("MODEMS")->selected : 0;
    fe.settings().set_text("MODEMNUMBER", part_->phone);
    fe.settings().set_int("MODEMPORT", port);
    part_->port = "COM" + std::to_string(port + 1);
    part_->answer = is(w, "HOST");
    fe.pop();
    std::string err;
    if (!part_->test && !open_provider(*part_, err)) {
        show_loop_message(fe, art_, kServiceError);
        return;
    }
    if (part_->answer) {
        fe.push(std::make_unique<NewMulti>(part_));
        return;
    }
    open_game_list(fe, part_);
}

void SelGame::enter(FrontEnd& fe) {
    NetScreen::enter(fe);
    if (!waiting_) startup_check(fe);
}

void SelGame::startup_check(FrontEnd& fe) {
    // (0x443CB0 / 0x4437C0, once the games are in: a start-up address left by a hosting start
    // (-h) with a player name opens the new game screen; the address is used up)
    auto& st = part_->cfg.startup;
    if (!st || st->address.empty() || !st->host || part_->nickname.empty()) return;
    st->address.clear();
    sfx(fe, art_, "BigButton");
    fe.push(std::make_unique<NewMulti>(part_));
}

void SelGame::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "UPDATE")) {
        sfx(fe, art_, "Multi");
        if (!part_->test) {
            // (Back from a room the session went with it: the provider is opened anew.)
            std::string err;
            if (!part_->session) open_provider(*part_, err);
            if (part_->session && art_.layout("MSGBOX")) {
                // (0x4437C0 does not look at the result: a failed enumeration leaves the list as
                // it was.)
                fe.push(std::make_unique<FindGamesBox>(part_, [this](FrontEnd&, bool ok) {
                    if (ok) set_games(found_entries(*part_));
                }));
                return;
            }
            if (part_->session) {
                find_games(*part_);
                games_ = found_entries(*part_);
            }
        }
        fill();
    } else if (is(w, "PREVMENU")) {
        sfx(fe, art_, "Previous");
        fe.pop();
    } else if (is(w, "WATCH")) {
        join(fe, true);
    } else if (is(w, "JOINGAME")) {
        join(fe, false);
    } else if (is(w, "STARTNEW")) {
        sfx(fe, art_, "BigButton");
        if (const EditState* n = edit("NICKNAME")) part_->nickname = n->text;
        fe.push(std::make_unique<NewMulti>(part_));
    }
}

// 0x4437C0 (WATCH, JOINGAME, a double click on the list): the game's description is taken; a
// session of another version only leaves the text for the menu loop (0x425730), which shows it in
// a message box as wide as the text + 20 (0x426E80, state 0x10 with 0x4A5030), no sound; a closed
// (flags bit 31) or playing (bit 20) session only plays "Previous"; then the empty-name refusal,
// the password, Watch's bit 0x40 and the join (state 0x13 / 0x12).
void SelGame::join(FrontEnd& fe, bool watch) {
    ListState* L = list("GAMENAME");
    int row = L ? L->selected : -1;
    if (row < 0 || row >= static_cast<int>(games_.size())) return;
    const GameEntry& g = games_[static_cast<std::size_t>(row)];
    if (g.major != 3) {  // (the description's version byte against the program's, GS+1)
        show_loop_message(fe, art_, "You do not have a compatible version for this game.");
        return;
    }
    if ((g.options & 0x8000) || (g.options & 0x10)) {
        sfx(fe, art_, "Previous");
        return;
    }
    const EditState* n = edit("NICKNAME");
    part_->nickname = n ? n->text : std::string();
    if (part_->nickname.empty()) {
        // 0x4A7190: the name field takes the keys (the message box over it first).
        focus_gadget_ = index_of("NICKNAME");
        activate_field(focus_gadget_);
        show_net_message(fe, art_, translate("You must enter your name"));
        return;
    }
    if (const EditState* p = edit("PASSWORD")) part_->password = p->text;
    sfx(fe, art_, watch ? "Multi" : "BigButton");
    fe.settings().set_text("Nickname", part_->nickname);
    if (!part_->test && !part_->session) {
        // Back from a room (the session went with it): the provider anew and the games found
        // there; the game is the one of the same name.
        std::string err;
        const std::string want = g.name;
        if (open_provider(*part_, err)) find_games(*part_);
        int found = -1;
        for (std::size_t k = 0; k < part_->found.size(); ++k)
            if (entry_of(part_->found[k]).name == want) found = static_cast<int>(k);
        if (found < 0) {
            rebuild(fe);
            return;
        }
        row = found;
    }
    if (!part_->test && part_->session && row < static_cast<int>(part_->found.size())) {
        // 0x4517B0 / 0x4C9FD0: open the session (join), then this machine's player. A failed
        // open (whatever the reason, a wrong password too) shows nothing: the state goes back to
        // 0 and the game list is built anew (0x443CB0, the games asked for again).
        net::dplay::Session& s = *part_->session;
        s.join(part_->found[static_cast<std::size_t>(row)], part_->password);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (s.state() == net::dplay::Session::State::Joining && std::chrono::steady_clock::now() < until) {
            s.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (s.state() != net::dplay::Session::State::Joined) {
            std::string err;
            open_provider(*part_, err);
            rebuild(fe);
            return;
        }
    }
    if (!part_->test) start_scores_reporting(*part_);
    enter_room(fe, part_, false);
}

// 0x443CB0 again: the games asked for anew, the fields from the kept name and password, the
// list taking the focus.
void SelGame::rebuild(FrontEnd& fe) {
    set_edit("NICKNAME", part_->nickname);
    set_edit("PASSWORD", part_->password);
    focus_edit("GAMENAME");
    if (!part_->test && part_->session && art_.layout("MSGBOX")) {
        // (the box over the list, as at its first opening; a failure closes the list)
        games_.clear();
        fill();
        fe.push(std::make_unique<FindGamesBox>(part_, [this](FrontEnd& f, bool ok) {
            if (ok) {
                set_games(found_entries(*part_));
                return;
            }
            f.pop();  // the game list
            show_invalid_address(f, *part_);
        }));
        return;
    }
    if (!part_->test && part_->session) find_games(*part_);
    if (!part_->test) games_ = found_entries(*part_);
    fill();
}

void NewMulti::next(FrontEnd& fe) {
    // 0x440D70: the password is taken, then an empty game name or player name is refused (the
    // field gets the keys, a 320-wide message); otherwise the names are kept (the settings writer
    // saves them) and the game is created.
    sfx(fe, art_, "BigButton");
    if (const EditState* e = edit("PASSWORD")) part_->password = e->text;
    fe.settings().set_text("Password", part_->password);
    const EditState* gn = edit("GAMENAME");
    if (!gn || gn->text.empty()) {
        focus_edit("GAMENAME");
        show_net_message(fe, art_, translate("You must enter a game name"), 320);
        return;
    }
    const EditState* nn = edit("NICKNAME");
    if (!nn || nn->text.empty()) {
        focus_edit("NICKNAME");
        show_net_message(fe, art_, translate("You must enter your name"), 320);
        return;
    }
    part_->game_name = gn->text;
    part_->nickname = nn->text;
    fe.settings().set_text("Game Name", part_->game_name);
    fe.settings().set_text("Nickname", part_->nickname);
    fe.pop();
    if (!part_->test) {
        // 0x451540: the session is opened as its host (TCP/IP: a fresh one on every address). It
        // does not look at the open's result: a failed open makes the player's creation (0x451220)
        // fail and the room is entered broken (enter_broken_room).
        std::string err;
        if (!part_->session) {
            part_->address.clear();
            if (!open_provider(*part_, err)) {
                enter_broken_room(fe, part_);
                return;
            }
        }
        net::dplay::HostSettings hs;
        hs.session_name = part_->game_name;
        hs.password = part_->password;
        if (!part_->session->host(hs, err)) {
            enter_broken_room(fe, part_);
            return;
        }
    }
    if (!part_->test) start_scores_reporting(*part_);
    enter_room(fe, part_, true);
}

std::unique_ptr<client::Screen> make_selgame(std::shared_ptr<NetPart> part, std::vector<GameEntry> games) {
    return std::make_unique<SelGame>(std::move(part), std::move(games));
}

// Reads a test file of the game list (the gui_harness selgame: format).
std::vector<GameEntry> read_games(const std::string& path, std::string& nick, std::string& password) {
    std::vector<GameEntry> out;
    std::ifstream in(path);
    for (std::string l; std::getline(in, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        if (l.rfind("nick ", 0) == 0) nick = l.substr(5);
        else if (l.rfind("password ", 0) == 0) password = l.substr(9);
        if (l.rfind("game ", 0) != 0) continue;
        std::vector<std::string> f;
        for (std::size_t a = 5;;) {
            const std::size_t e = l.find(';', a);
            f.push_back(l.substr(a, e == std::string::npos ? std::string::npos : e - a));
            if (e == std::string::npos) break;
            a = e + 1;
        }
        if (f.size() < 13) continue;
        auto num = [&](std::size_t k) { return static_cast<unsigned>(std::strtoul(f[k].c_str(), nullptr, 0)); };
        GameEntry g;
        g.name = f[0];
        g.map = f[1];
        g.max_players = static_cast<int>(num(2));
        g.options = static_cast<std::uint16_t>((num(4) & 0xFFF0u) | (num(3) & 0xFu));
        g.memory = static_cast<std::uint16_t>(num(5));
        g.cd = static_cast<std::uint16_t>(num(6));
        g.ping = static_cast<std::uint16_t>(num(7));
        g.energy = static_cast<std::uint16_t>(num(8));
        g.metal = static_cast<std::uint16_t>(num(9));
        g.unit_limit = static_cast<std::uint16_t>(num(10));
        g.major = static_cast<std::uint8_t>(num(11));
        g.minor = static_cast<std::uint8_t>(num(12));
        g.password = (g.cd & 1) != 0;
        out.push_back(g);
    }
    return out;
}

// The tests' base: pushes the screens asked for once, then stays under them.
class TestBase : public client::Screen {
public:
    TestBase(std::shared_ptr<NetPart> part, std::vector<std::string> specs) : part_(std::move(part)), specs_(std::move(specs)) {}
    void enter(FrontEnd& fe) override {
        if (pushed_) return;
        pushed_ = true;
        for (const std::string& s : specs_) {
            const auto colon = s.find(':');
            const std::string kind = s.substr(0, colon), arg = colon == std::string::npos ? std::string() : s.substr(colon + 1);
            if (kind == "selprov") fe.push(std::make_unique<SelProv>(part_));
            else if (kind == "serial") fe.push(std::make_unique<SerialAsk>(part_));
            else if (kind == "modem") fe.push(std::make_unique<ModemAsk>(part_));
            else if (kind == "tcp") {
                fe.settings().set_text("TCPADDR", arg);
                fe.push(std::make_unique<TcpAsk>(part_));
            } else if (kind == "selgame") {
                std::string nick, pw;
                auto games = read_games(arg, nick, pw);
                part_->nickname = nick;
                part_->password = pw;
                fe.push(make_selgame(part_, std::move(games)));
            } else if (kind == "newmulti") {
                // newmulti[:GAMENAME[:NICK]]
                const auto c2 = arg.find(':');
                part_->game_name = arg.substr(0, c2);
                if (c2 != std::string::npos) part_->nickname = arg.substr(c2 + 1);
                fe.push(std::make_unique<NewMulti>(part_));
            } else if (kind == "lounge") {
                fe.push(net_screens_detail::make_lounge(as_void(part_), true, arg));
            } else if (kind == "message") {
                // message:TEXT (200 wide), message:WIDTH:TEXT, message:WIDTH:OK:FIT:TEXT (the
                // inputs of gui_harness msgbox:)
                std::string t = arg;
                int width = 200, with_ok = 1, fit = 1;
                auto num_field = [&](int& v) {
                    const auto c = t.find(':');
                    if (c == std::string::npos || c == 0 || t.find_first_not_of("0123456789") != c) return false;
                    v = std::atoi(t.c_str());
                    t = t.substr(c + 1);
                    return true;
                };
                if (num_field(width)) {
                    const std::string keep = t;
                    int a = 0, b = 0;
                    if (num_field(a) && num_field(b)) with_ok = a, fit = b;
                    else t = keep;
                }
                if (part_->art.layout("MSGBOX")) fe.push(std::make_unique<NetMessage>(part_->art, t, width, fit != 0, nullptr, with_ok != 0));
            }
        }
    }
    void handle(FrontEnd&, const InputEvent&) override {}
    void draw(FrontEnd&, render::Image8& out) override { std::fill(out.pixels.begin(), out.pixels.end(), static_cast<std::uint8_t>(0)); }

private:
    std::shared_ptr<NetPart> part_;
    std::vector<std::string> specs_;
    bool pushed_ = false;
};

}  // namespace

void install_net_screens(client::FrontEndHooks& hooks, MenuArt& art, const client::NetMenuConfig& config) {
    hooks.multiplayer_menu = [&art, config](FrontEnd& fe) -> std::unique_ptr<client::Screen> {
        art.vfs().scan();  // the original rescans the archives when MULTI is pressed (0x425E0B)
        auto part = std::make_shared<NetPart>(art, config);
        part->nickname = fe.settings().get_text("Nickname", "");
        if (const std::string lb = fe.settings().get_text("NetLoopback", "0"); lb.find('.') != std::string::npos)
            part->loopback = net::dplay::resolve_ip(lb);
        else if (fe.settings().get_int("NetLoopback", 0) != 0)
            part->loopback = net::dplay::kLoopback;
        // (0x42F9A0: Password; Game Name unless -h gave one)
        part->password = fe.settings().get_text("Password", "").substr(0, 10);
        part->game_name = fe.settings().get_text("Game Name", "").substr(0, 16);
        if (config.startup && !config.startup->game_name.empty()) part->game_name = config.startup->game_name.substr(0, 16);
        return std::make_unique<SelProv>(part);
    };
}

std::unique_ptr<client::Screen> make_net_test_screens(MenuArt& art, const client::NetMenuConfig& config, const std::vector<std::string>& specs) {
    auto part = std::make_shared<NetPart>(art, config);
    part->test = true;
    return std::make_unique<TestBase>(part, specs);
}

namespace {

// ------------------------------------------------------------------ the battle room (LOUNGE2.GUI)
// A slot of the room as the original's player record and side record hold it.
struct LoungeSlot {
    bool used = false;
    int controller = 0;  // 1 this machine's human, 2 a computer player run here, 3 another machine's, 4 blocked
    int state = 0;       // 10: out of the game
    std::string name;
    std::uint32_t ping = 0;  // the record's latency (+0x14)
    int team = 5;            // ally team (+0x13F), 5 none
    std::array<std::uint8_t, 10> allied{}, allied_by{};  // +0x108 / +0x113
    int kind = 0, side = 0, logo = 0xFF, flags = 0;      // side record +0x94 .. +0x97
    std::uint16_t options = 0, memory = 0, cd = 0, screen_w = 0, screen_h = 0;
    std::uint8_t major = 3, minor = 1;
    bool synching = false;  // (host) the unit definition exchange with this player still runs
    bool ping_mark = false; // (host) the sync object's mark for the player (0x46E0B0): ":s" after the ping
    bool playing() const { return used && controller >= 1 && controller <= 3 && state != 10; }
    bool watching() const { return used && (options & 0x40) != 0; }
    bool ready() const { return (options & 0x20) != 0; }
};

// The console's ring as the room shows it (0x463CA0): 30 places; a new line drops the oldest when
// (head + 1) mod the text lines setting meets the tail.
struct ChatRing {
    std::array<std::string, 30> text;
    int head = 0, tail = 0;
    void add(const std::string& line, int lines) {
        if (line.empty() || lines <= 0) return;
        if ((head + 1) % lines == tail && ++tail == 30) tail = 0;
        text[static_cast<std::size_t>(head)] = line.substr(0, 0x3F);
        if (++head == 30) head = 0;
    }
};

// Everything the room screen shows.
struct LoungeState {
    int local = 0;
    std::array<LoungeSlot, 10> slots;
    int session_players = 1;  // the session's player count (GS+0x2A3C)
    std::string map;          // the map chosen in this machine's map list
    bool have_map = true;     // this machine has the map (and its checksum agrees with the host's)
    std::uint16_t energy = 10, metal = 10, unit_limit = 250;  // the sliders (hundreds)
    ChatRing chat;
    std::uint32_t clock = 0, clock_step = 0;  // tests: the clock held (ticks of 1/30 s) and its step a frame
    int host() const {
        for (int s = 0; s < 10; ++s)
            if (slots[static_cast<std::size_t>(s)].controller && (slots[static_cast<std::size_t>(s)].flags & 1)) return s;
        return 10;
    }
};

// The first map of the multiplayer map list in the order the archives give the files (0x434BF0,
// unsorted): the map a room starts on (0x435D30 picks it whenever the room opens).
std::string first_network_map(MenuArt& art) {
    static std::string first;
    static bool done = false;
    if (done) return first;
    done = true;
    for (const std::string& file : art.vfs().enumerate("maps\\*.ota")) {
        std::vector<std::uint8_t> f;
        if (!art.vfs().read("maps\\" + file, f)) continue;
        auto root = tdf_parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size())).root;
        const TdfSection* gh = root ? root->child("GlobalHeader") : nullptr;
        if (!gh) continue;
        bool network = false;
        for (const auto& c : gh->children())
            if (upper(c->get_string("Type", 64, "")).find("NETWORK") != std::string::npos) network = true;
        const std::string name = file.substr(0, file.size() - 4);
        if (!network || art.vfs().size_or_zero("maps\\" + name + ".tnt") == 0) continue;
        first = translate_lowered(name);  // (the list's shown name, 0x434DF9)
        break;
    }
    return first;
}

// The room file of the tests (the gui_harness lounge: format).
LoungeState read_lounge(const std::string& path) {
    LoungeState st;
    std::ifstream in(path);
    int players = -1, humans = 0;
    for (std::string l; std::getline(in, l);) {
        if (const auto c = l.find('#'); c != std::string::npos) l.resize(c);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        std::istringstream ls(l);
        std::string k;
        if (!(ls >> k)) continue;
        if (k == "local") ls >> st.local;
        else if (k == "map") {
            std::getline(ls >> std::ws, st.map);
        } else if (k == "players") ls >> players;
        else if (k == "clock") ls >> st.clock;
        else if (k == "clockstep") ls >> st.clock_step;
        else if (k == "synching") {
            int sl = 0;
            ls >> sl;
            if (sl >= 0 && sl < 10) st.slots[static_cast<std::size_t>(sl)].synching = true;
        }
        else if (k == "resources") ls >> st.energy >> st.metal >> st.unit_limit;
        else if (k == "chat") {
            std::string t;
            std::getline(ls >> std::ws, t);
            st.chat.add(t, 10);
        } else if (k == "res") {
            int s = 0, w = 0, h = 0;
            ls >> s >> w >> h;
            if (s >= 0 && s < 10) {
                st.slots[static_cast<std::size_t>(s)].screen_w = static_cast<std::uint16_t>(w);
                st.slots[static_cast<std::size_t>(s)].screen_h = static_cast<std::uint16_t>(h);
            }
        } else if (k == "allied" || k == "alliedby") {
            int a = 0, b = 0, v = 0;
            ls >> a >> b >> v;
            if (a >= 0 && a < 10 && b >= 0 && b < 10) (k == "allied" ? st.slots[static_cast<std::size_t>(a)].allied : st.slots[static_cast<std::size_t>(a)].allied_by)[static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(v);
        } else if (k == "player") {
            int s = 0, controller = 0, kind = 0, side = 0, logo = 0, flags = 0, mem = 0, cd = 0, ping = 0, team = 5;
            std::string opt, fl, cds;
            ls >> s >> controller >> kind >> side >> logo >> fl >> opt >> mem >> cds >> ping >> team;
            if (s < 0 || s > 9) continue;
            LoungeSlot& p = st.slots[static_cast<std::size_t>(s)];
            flags = static_cast<int>(std::strtol(fl.c_str(), nullptr, 0));
            cd = static_cast<int>(std::strtol(cds.c_str(), nullptr, 0));
            std::getline(ls >> std::ws, p.name);
            p.used = controller != 4;  // a blocked slot has no player (state 4 only)
            p.controller = controller;
            p.kind = kind;
            p.side = side;
            p.logo = logo;
            p.flags = flags;
            p.options = static_cast<std::uint16_t>(std::strtoul(opt.c_str(), nullptr, 0));
            p.memory = static_cast<std::uint16_t>(mem);
            p.cd = static_cast<std::uint16_t>(cd);
            p.ping = static_cast<std::uint32_t>(ping);
            p.team = team;
            if (kind == 1) ++humans;
        }
    }
    st.session_players = players >= 0 ? players : humans;
    return st;
}

// ------------------------------------------------------------------ the live room
// A live room: the session's transport (until the game takes it), the session, the engine's
// room (net/lobby.h) and this machine's C runtime rand() (the host's start position shuffle).
struct LiveRoom {
    std::unique_ptr<net::Transport> transport;
    std::shared_ptr<net::NetSession> session;
    std::shared_ptr<net::BattleRoom> room;
    std::shared_ptr<sim::Lcg15> crt_rand;
    client::NetMenuConfig cfg;
    int sides = 2;          // the sides the SIDE buttons step through (GS+0x37F39)
    int text_lines = 10;    // the console's lines (GS+0x37F27)
    int unit_limit = 250;   // the settings' unit limit: the MAXUNITS slider's most (GS+0x37EEA)
    std::string dump;       // tests: the room's states written as room files and frames (TA_ROOM_DUMP)
};

// Why a machine is out of the room (0x452C40), shown over the game list.
std::string out_text(int reason) { return leave_reason_text(reason); }

// Writes a room in the room file format (read_lounge / gui_harness lounge:).
void write_lounge(const std::string& path, const struct LoungeState& st);

// The kick question (0x446080: YESORNO.GUI opened with flags 0x100, centred): TITLE "Reject
// <name>?", CHOICE1 "Yes" sends the player away (status 1, 0x446020), CHOICE2 "No".
class RejectQuestion : public LayoutScreen {
public:
    RejectQuestion(MenuArt& art, const std::string& name, std::function<void()> yes) : LayoutScreen(art.layout("YESORNO")), art_(art), yes_(std::move(yes)) {
        open_centred_ = -1;
        if (Widget* w = widget("CHOICE1")) set_caption(*w, "Yes");
        if (Widget* w = widget("CHOICE2")) set_caption(*w, "No");
        if (Widget* w = widget("TITLE")) w->text = "Reject " + name + "?";
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        stock_panel(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "CHOICE1")) {
            auto yes = yes_;
            fe.pop();
            if (yes) yes();
        } else if (is(w, "CHOICE2")) {
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    std::function<void()> yes_;
};

}  // namespace

std::unique_ptr<client::Screen> make_room_map_choice(MenuArt& art, const std::string& current, std::function<void(const std::string&)> chosen);
std::unique_ptr<client::Screen> make_room_map_view(MenuArt& art, std::function<std::string()> host_map);

namespace {

// ------------------------------------------------------------------ build restrictions (RESTRICT2.GUI)
// One unit definition of the restrictions' list (0x44C7E0): every definition but the
// "norestrict" ones, its row "<name>\r<side> <metal>M  <energy>E" (the costs cut to whole
// numbers), its picture unitpics\<unitname>.PCX; sorted by the row's text (byte order).
struct RestrictEntry {
    std::uint32_t key = 0;  // the definition's key (the FBI file's simple checksum)
    std::int32_t def_index = 0;  // its index in the definition table (the row's +0x52)
    std::string text, picture;
    bool wacky = false;     // "wacky" definitions start at a count of 0 (0x46D040)
};
// Every definition's key by index (the blank one, 0, included), for the list files.
std::vector<std::uint32_t>& restrict_def_keys_store() {
    static std::vector<std::uint32_t> keys;
    return keys;
}
const std::vector<RestrictEntry>& restrict_entries(MenuArt& art) {
    static std::vector<RestrictEntry> entries;
    static bool done = false;
    if (done) return entries;
    done = true;
    UnitDefContext ctx;
    load_unit_context(art.vfs(), ctx);
    const UnitDefTable defs = load_unit_defs(art.vfs(), ctx);
    std::vector<std::uint32_t>& keys = restrict_def_keys_store();
    for (const UnitDefRecord& d : defs.units) keys.push_back(d.fbi_checksum);
    for (std::size_t k = 1; k < defs.units.size(); ++k) {
        const UnitDefRecord& d = defs.units[k];
        if (d.can_do_flags & (1u << 15)) continue;  // norestrict
        RestrictEntry e;
        e.key = d.fbi_checksum;
        e.def_index = static_cast<std::int32_t>(k);
        // (the side goes through the translation, 0x44CA9D)
        e.text = d.name + "\r" + translate(d.side) + " " + std::to_string(static_cast<int>(d.build_cost_metal)) + "M  " +
                 std::to_string(static_cast<int>(d.build_cost_energy)) + "E";
        e.picture = "unitpics\\" + d.unit_name + ".PCX";
        e.wacky = (d.can_do_flags & (1u << 16)) != 0;
        entries.push_back(std::move(e));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const RestrictEntry& a, const RestrictEntry& b) {
        return std::lexicographical_compare(a.text.begin(), a.text.end(), b.text.begin(), b.text.end(),
                                            [](char x, char y) { return static_cast<unsigned char>(x) < static_cast<unsigned char>(y); });
    });
    return entries;
}
const std::vector<std::uint32_t>& restrict_def_keys(MenuArt& art) {
    restrict_entries(art);
    return restrict_def_keys_store();
}

// What the restrictions work on: each definition's count (the most one player may build, -1 no
// limit) and whether it is enabled, in the room's unit definition exchange; `set_count` /
// `set_enabled` change this machine's entry (the host's also go to every peer).
struct RestrictHooks {
    std::function<std::int32_t(std::uint32_t key)> count;
    std::function<bool(std::uint32_t key)> enabled;
    std::function<void(std::uint32_t key, std::int32_t count)> set_count;
    std::function<void(std::uint32_t key, bool on)> set_enabled;
};

// The restrictions' list files (spec: ta/restrict_list.h): Load List (0x44BC10: LOADLIST.GUI
// with flags 0x981, centred, the screen under it dimmed; DLoadList in its box; handler 0x44B3C0)
// and Save List (0x44B990: SAVELIST.GUI with flags 0x880 at its own position, DSaveList;
// handler at 0x44B690). GAMES lists SAVEGAME\*.LST (0x44B4E0) without their extensions, newest
// first; picking a row (0x44B600) puts its name in GAMENAME. Load: DELETE and GAMENAME off, no
// field takes the keys; LOAD (OK) / Enter / a double click: "Options", the selected file read into
// the restrictions' counts and the rows in view refreshed (0x44BFD0 on the restrictions), closes.
// Save: SAVEGAME made first (0x4BCF00); DELETE (Delete List) off when there is no file; GAMENAME
// takes the focus and letters, digits, '_', ' ' and '\'' (20 at most); LOAD (OK) / Enter / a
// double click: "Options", then a non-empty name writes SAVEGAME\<name>.LST (the extension rule of
// 0x4290F0: a '.' in the name cuts it there), an existing file replaced; closes. Delete List:
// "SMLBUTTON", SAVEGAME\<file> removed at once, the list again. Cancel: "Previous", closes.
class RestrictListFiles : public LayoutScreen {
public:
    RestrictListFiles(MenuArt& art, bool save, std::string dir, const std::array<std::uint8_t, 256>* colours,
                      std::function<void(const std::vector<std::uint8_t>&)> load, std::function<std::vector<std::uint8_t>()> make)
        : LayoutScreen(art.layout(save ? "SAVELIST" : "LOADLIST")), art_(art), save_(save), dir_(std::move(dir)), load_(std::move(load)), make_(std::move(make)) {
        background_in_box_ = true;
        background_ = art_.picture(save_ ? "DSaveList" : "DLoadList");
        if (!save_) open_centred_ = -1;
        colours_ = colours;
        if (save_) {
            std::error_code ec;
            std::filesystem::create_directories(dir_, ec);
        }
        relist();
        if (Widget* w = widget("GAMENAME")) {
            if (save_) w->attribs |= 2;
            else w->active = false;
        }
        if (!save_) focus_ = -1;
        if (Widget* w = widget("DELETE")) w->active = save_ && !files_.empty();
        show_name();
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }
    void draw(FrontEnd& fe, render::Image8& out) override {
        if (!placed_) {
            place(out.width, out.height);
            if (save_)
                for (std::size_t i = 0; i < widgets_.size(); ++i)
                    if (widgets_[i].kind == WidgetKind::TextField && widgets_[i].active) {
                        // The name field takes the focus and the keys (0x4A7190).
                        focus_gadget_ = focus_ = static_cast<int>(i);
                        widgets_[i].cursor = static_cast<int>(widgets_[i].text.size());
                    }
        }
        // (Open flag 0x800: the screen under it dimmed, shade row 8.)
        const std::vector<std::uint8_t>& shade = fe.menu_content().shade;
        if (shade.size() >= 0x2000)
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x) {
                    std::uint8_t& p = out.row(y)[x];
                    p = shade[static_cast<std::size_t>(8 * 256 + static_cast<std::int8_t>(p))];
                }
        LayoutScreen::draw(fe, out);
    }

protected:
    void relist() {
        files_ = list_files_newest_first(dir_, "LST");
        std::vector<std::string> names;
        for (const auto& f : files_) names.push_back(name_without_extension(f.filename().string()));
        if (Widget* l = widget("GAMES")) set_list(*l, std::move(names));
    }
    const std::filesystem::path* selected() {
        const Widget* l = widget("GAMES");
        if (!l || l->selected < 0 || l->selected >= static_cast<int>(files_.size())) return nullptr;
        return &files_[static_cast<std::size_t>(l->selected)];
    }
    // 0x44B600: the selected row's name in GAMENAME (none: empty).
    void show_name() {
        Widget* g = widget("GAMENAME");
        if (!g) return;
        const std::filesystem::path* f = selected();
        g->text = f ? name_without_extension(f->filename().string()) : std::string();
        if (focus_ >= 0 && &widgets_[static_cast<std::size_t>(focus_)] == g) g->cursor = static_cast<int>(g->text.size());
    }
    void on_list_select(FrontEnd&, Widget&) override { show_name(); }
    void on_text(FrontEnd& fe, Widget& w) override { on_press(fe, w); }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "CANCEL")) {
            sfx(fe, art_, "Previous");
            fe.pop();
            return;
        }
        if (save_ && is(w, "DELETE")) {
            sfx(fe, art_, "SMLBUTTON");
            if (const std::filesystem::path* f = selected()) {
                std::error_code ec;
                std::filesystem::remove(*f, ec);
            }
            relist();
            show_name();
            return;
        }
        if (!(is(w, "GAMES") || is(w, "LOAD") || (save_ && is(w, "GAMENAME")))) return;
        sfx(fe, art_, "Options");
        if (save_) {
            const Widget* g = widget("GAMENAME");
            const std::string name = g ? g->text : std::string();
            if (!name.empty() && make_) {
                // 0x4290F0("SAVEGAME", name, "LST"): SAVEGAME-<language>\ when that file opens.
                std::string rel = art_.vfs().virtual_path("SAVEGAME", name, "LST");
                std::filesystem::path p;
                if (rel.rfind("SAVEGAME\\", 0) == 0) p = std::filesystem::path(dir_) / rel.substr(9);
                else p = std::filesystem::path(art_.game_dir()) / rel;
                const std::vector<std::uint8_t> bytes = make_();
                std::ofstream o(p, std::ios::binary | std::ios::trunc);
                o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
        } else if (const std::filesystem::path* f = selected()) {
            std::ifstream in(*f, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in.is_open() && load_) load_(bytes);
        }
        fe.pop();
    }

private:
    MenuArt& art_;
    bool save_;
    std::string dir_;
    std::function<void(const std::vector<std::uint8_t>&)> load_;
    std::function<std::vector<std::uint8_t>()> make_;
    std::vector<std::filesystem::path> files_;
};

// The restrictions (0x44C7E0, handler 0x44C420, its frame 0x44C220), opened by the room's Build
// Restrictions with flags 0x880 over the room; UnitRestrict5x behind it on the room's palette.
// DESCLIST (rows 32 pixels high, no picked row) shows the entries, PICLIST their pictures (one
// more loaded every two clock ticks), SLIDER0..11 and COUNT0..11 the counts of the twelve rows in
// view (0..100, 101 "No Limit"); SCROLLSLIDER moves the lists. A row's flags: 1 not enabled (its
// text dimmed four times, its picture shaded), 2 a count of 0 (a red cross over the picture). Only
// the host moves the sliders (a joining player's are grayed) and has Load List, Save List and
// Reset List. Each refresh of the rows in view puts every row's count back through its slider
// (0x44BFD0 -> 0x44BE70), which on the host answers every peer for those definitions.
class Restrictions : public LayoutScreen {
public:
    Restrictions(MenuArt& art, const client::ScreenLayout* layout, const render::Palette* palette, bool host, RestrictHooks hooks)
        : LayoutScreen(layout), art_(art), palette_(palette), host_(host), hooks_(std::move(hooks)), entries_(restrict_entries(art)) {
        flags_.assign(entries_.size(), 0);
        pictures_.resize(entries_.size());
        counts_.resize(entries_.size());
        old_.resize(entries_.size());
        for (std::size_t k = 0; k < entries_.size(); ++k) {
            counts_[k] = hooks_.count ? hooks_.count(entries_[k].key) : -1;
            old_[k] = counts_[k] == -1 ? 0x65 : counts_[k];  // (OLDCOUNTS keeps 101 for "no limit")
        }
        for (int i = 0; i < 12; ++i)
            if (Widget* s = widget("SLIDER" + std::to_string(i))) s->range = 0x65;
        if (Widget* d = widget("DESCLIST")) {
            d->attribs |= 0x100;
            d->row_h = 0x20;
        }
        if (Widget* p = widget("PICLIST")) {
            p->attribs |= 0x180;
            p->row_h = 0x20;
            p->kind = WidgetKind::Surface;  // (drawn here)
        }
        refresh_rows();
        if (Widget* d = widget("DESCLIST")) {
            std::vector<std::string> rows;
            for (const RestrictEntry& e : entries_) rows.push_back(e.text);
            set_list(*d, rows);
            d->row_h = 0x20;
            d->max_top = std::max(0, static_cast<int>(rows.size()) - d->h / 0x20);
            // The scroll slider is sized by the first list of its group, the picture list
            // (0x4A3EF0): knob = whole rows shown x the slider's height / the rows, the travel
            // the height less the knob.
            const Widget* pl = widget("PICLIST");
            if (Widget* s = slider_of(*d); s && pl && !rows.empty()) {
                s->knob = (pl->h / 0x20) * s->h / static_cast<int>(rows.size());
                s->range = s->h - s->knob;
            }
        }
        top_ = 0;
        refresh_rows();
        for (const char* n : {"Load", "Save", "Reset"})
            if (Widget* w = widget(n)) w->grayed = !host_;
    }
    const render::Palette* palette() const override { return palette_; }
    bool rings(int i) const override { return i <= 0 || i >= static_cast<int>(widgets_.size()) || !is(widgets_[static_cast<std::size_t>(i)], "PICLIST"); }

protected:
    // 0x44BFD0: the twelve rows in view: each slider grayed unless the host may change that
    // definition (it is enabled), its knob from the count (-1: the most, 101), then its own
    // routine (0x44BE70): the count back from the knob (101: -1, "No Limit"), the row's flags,
    // the label, and the entry set (the host's answer to every peer).
    void refresh_rows() {
        for (int i = 0; i < 12; ++i) {
            const int row = top_ + i;
            Widget* s = widget("SLIDER" + std::to_string(i));
            if (!s || row < 0 || row >= static_cast<int>(entries_.size())) continue;
            const RestrictEntry& e = entries_[static_cast<std::size_t>(row)];
            const bool on = hooks_.enabled ? hooks_.enabled(e.key) : true;
            const bool locked = !host_ || !on;
            flags_[static_cast<std::size_t>(row)] = locked ? 1 : 0;
            set_knob(*s, counts_[static_cast<std::size_t>(row)] == -1 ? s->range : counts_[static_cast<std::size_t>(row)]);
            s->grayed = locked;
            slider_moved(i);
        }
    }
    void slider_moved(int i) {
        const int row = top_ + i;
        Widget* s = widget("SLIDER" + std::to_string(i));
        if (!s || row < 0 || row >= static_cast<int>(entries_.size())) return;
        const RestrictEntry& e = entries_[static_cast<std::size_t>(row)];
        const int v = knob_value(*s);
        const std::int32_t count = v < 0x65 ? v : -1;
        if (Widget* l = widget("COUNT" + std::to_string(i))) l->text = v < 0x65 ? std::to_string(v) : translate("No Limit");  // (0x44BECA)
        counts_[static_cast<std::size_t>(row)] = count;
        if (hooks_.set_count) hooks_.set_count(e.key, count);
        const bool on = hooks_.enabled ? hooks_.enabled(e.key) : true;
        flags_[static_cast<std::size_t>(row)] = static_cast<std::uint8_t>((on ? 0 : 1) | (count == 0 ? 2 : 0));
        s->knob_value = s->position = v;
        last_[static_cast<std::size_t>(i)] = v;
    }
    // The room's knob rules (0x45B9B0 / 0x45BA20, as the room's sliders).
    static void set_knob(Widget& w, int value) {
        if (w.span < 2 || w.range <= 0) return;
        const double t = static_cast<double>(std::min(value, w.range)) / w.range * (w.span - 1);
        const int k = static_cast<int>(t);
        w.knob_pos = t - k != 0.0 ? static_cast<int>(t + 1.0) : k;
        w.position = value;
        w.knob_value = value;
    }
    static int knob_value(const Widget& w) {
        if (w.span < 2) return 0;
        return static_cast<int>(static_cast<double>(w.knob_pos) / (w.span - 1) * w.range);
    }

    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        LayoutScreen::frame(fe, now_ms);
        clock_ = now_ms * 30u / 1000u;
        // 0x44C220: a picture every two ticks.
        if (next_clock_ < clock_) {
            next_clock_ = clock_ + 2;
            if (loaded_ < static_cast<int>(entries_.size())) {
                auto& p = pictures_[static_cast<std::size_t>(loaded_)];
                std::vector<std::uint8_t> f;
                render::Sprite8 s;
                if (art_.vfs().read(entries_[static_cast<std::size_t>(loaded_)].picture, f) && decode_pcx(f, s, nullptr)) p = std::make_unique<render::Sprite8>(std::move(s));
                ++loaded_;
            }
        }
        // The counts the exchange changed (a joining player sees the host's).
        bool changed = false;
        for (std::size_t k = 0; k < entries_.size() && hooks_.count; ++k) {
            const std::int32_t c = hooks_.count(entries_[k].key);
            if (c != counts_[k] && !host_) {
                counts_[k] = c;
                changed = true;
            }
        }
        // The list moved by its slider: the pictures follow, the rows in view are refreshed.
        if (Widget* d = widget("DESCLIST"); d && d->top != top_) {
            top_ = d->top;
            changed = true;
        }
        if (changed) refresh_rows();
        // The host's sliders moved.
        for (int i = 0; i < 12 && host_; ++i) {
            const Widget* s = widget("SLIDER" + std::to_string(i));
            if (s && last_[static_cast<std::size_t>(i)] != knob_value(*s)) slider_moved(i);
        }
    }

    void draw_surface(FrontEnd& fe, render::Image8& out, const Widget& w) override {
        if (!is(w, "PICLIST")) return;
        // 0x4A1B40, picture lists: from (x + 2, y + 2), a row each 32 pixels while the row's top
        // is above the list's bottom: the picture (when loaded) mapped onto x + 2 .. x + w - 1
        // by the row's height (texels 1 .. size - 1), then flag 1 shades it, else flag 2 crosses
        // it out in interface colour 12.
        const Widget& h = widgets_.front();
        render::ClipRect clip{std::max(ox_ + w.x, 0), std::max(oy_ + w.y, 0), std::min(ox_ + w.x + w.w - 1, out.width - 1),
                              std::min(oy_ + w.y + w.h - 1, out.height - 1)};
        clip.x1 = std::min(clip.x1, ox_ + h.w - 1);
        clip.y1 = std::min(clip.y1, oy_ + h.h - 1);
        const int x0 = ox_ + w.x + 2, x1 = ox_ + w.x + w.w - 1, bottom = oy_ + w.y + w.h - 1;
        for (int row = top_, y0 = oy_ + w.y + 2; y0 < bottom && row < static_cast<int>(entries_.size()); ++row, y0 += 0x20) {
            const render::Sprite8* p = pictures_[static_cast<std::size_t>(row)].get();
            if (!p) continue;
            const int y1 = y0 + 0x20 - 1;
            const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
            const int uv[4][2] = {{1, 1}, {p->width - 1, 1}, {p->width - 1, p->height - 1}, {1, p->height - 1}};
            render::RasterTarget t;
            t.pixels = out.pixels.data();
            t.pitch = out.width;
            t.clip = clip;
            render::textured_quad_uv(t, v, uv, *p);
            const std::uint8_t f = flags_[static_cast<std::size_t>(row)];
            if (f & 1) {
                shade_rect(fe, out, clip, x0, y0, x1, y1, -20);
            } else if (f & 2) {
                const std::uint8_t c = colour(12);
                line(out, clip, x0 + 1, y1 - 1, x1 - 2, y0 + 1, c);
                line(out, clip, x0 + 2, y1 - 1, x1 - 1, y0 + 1, c);
                line(out, clip, x0 + 1, y0 + 2, x1 - 1, y1 - 2, c);
                line(out, clip, x0 + 2, y0 + 2, x1 - 2, y1 - 2, c);
            }
        }
    }
    static void line(render::Image8& out, const render::ClipRect& clip, int xa, int ya, int xb, int yb, std::uint8_t c) {
        // (Bresenham; the original's line routine 0x4BE950 is not checked against it.)
        const int dx = std::abs(xb - xa), dy = -std::abs(yb - ya), sx = xa < xb ? 1 : -1, sy = ya < yb ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            client::put_pixel(out, clip, xa, ya, c);
            if (xa == xb && ya == yb) break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                xa += sx;
            }
            if (e2 <= dx) {
                err += dx;
                ya += sy;
            }
        }
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        // The rows whose flags say "not enabled" are drawn dimmed (the list's "&G" rule).
        if (Widget* d = widget("DESCLIST")) {
            const std::size_t n = std::min(d->items.size(), entries_.size());
            for (std::size_t k = 0; k < n; ++k) d->items[k] = flags_[k] == 1 ? "&G" + entries_[k].text : entries_[k].text;
        }
        LayoutScreen::draw(fe, out);
        // Grayed sliders (a joining player's) and their arrows go through shade row 12.
        for (std::size_t i = 0; i < widgets_.size(); ++i) {
            const Widget& w = widgets_[i];
            bool arrow = false;
            if (w.slider_step != 0 && w.active)
                if (const Widget* s = widget(w.link)) arrow = s->grayed && s->kind == WidgetKind::ScrollBar && !s->list_slider;
            if (!(w.kind == WidgetKind::ScrollBar && w.grayed && w.active) && !arrow) continue;
            shade_rect(fe, out, out.bounds(), ox_ + w.x, oy_ + w.y, ox_ + w.x + w.w - 1, oy_ + w.y + w.h - 1, -20);
        }
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        for (int i = 0; i < 12; ++i)
            if (is(w, ("SLIDER" + std::to_string(i)).c_str())) {
                if (host_) slider_moved(i);
                return;
            }
        if (is(w, "OK")) {
            sfx(fe, art_, "Options");
            close(fe);
        } else if (is(w, "Cancel")) {
            // The counts as they were when the dialog opened (OLDCOUNTS: 101 where there was no
            // limit), each answered anew.
            sfx(fe, art_, "Previous");
            for (std::size_t k = 0; k < entries_.size(); ++k) {
                counts_[k] = old_[k];
                if (hooks_.set_count) hooks_.set_count(entries_[k].key, old_[k]);
            }
            close(fe);
        } else if (is(w, "Reset")) {
            // 100 each ("wacky" ones 0), the changed ones answered.
            sfx(fe, art_, "Options");
            for (std::size_t k = 0; k < entries_.size(); ++k) {
                const std::int32_t c = entries_[k].wacky ? 0 : 100;
                if (c != counts_[k] && hooks_.set_count) hooks_.set_count(entries_[k].key, c);
                counts_[k] = c;
            }
            refresh_rows();
        } else if (is(w, "Load") || is(w, "Save")) {
            sfx(fe, art_, "Options");
            const bool save = is(w, "Save");
            // 0x44BC10: no list file: the dialog closed again and the message instead.
            if (!save && list_files_newest_first(list_dir_, "LST").empty()) {
                show_message(fe, art_, translate("There are no saved lists to choose from"));
                return;
            }
            fe.push(std::make_unique<RestrictListFiles>(
                art_, save, list_dir_, layout() ? &layout()->colours : nullptr, [this](const std::vector<std::uint8_t>& b) { load_list(b); },
                [this]() { return save_list(); }));
        }
    }
    // 0x44B140 into the rows' counts, then the rows in view through their sliders (0x44BFD0).
    void load_list(const std::vector<std::uint8_t>& bytes) {
        std::vector<RestrictListRow> rows(entries_.size());
        for (std::size_t k = 0; k < entries_.size(); ++k) rows[k] = {entries_[k].def_index, counts_[k]};
        read_restrict_list(bytes, restrict_def_keys(art_), rows);
        for (std::size_t k = 0; k < entries_.size(); ++k) counts_[k] = rows[k].count;
        refresh_rows();
    }
    // 0x44B230: the counts as the rows hold them.
    std::vector<std::uint8_t> save_list() {
        std::vector<RestrictListRow> rows(entries_.size());
        for (std::size_t k = 0; k < entries_.size(); ++k) rows[k] = {entries_[k].def_index, counts_[k]};
        std::vector<std::uint8_t> out;
        write_restrict_list(restrict_def_keys(art_), rows, out);
        return out;
    }

public:
    std::string list_dir_;  // the list files' folder (SAVEGAME in the game folder)
protected:
    // Closing (0x44C420 with no gadget): on the host each listed definition is enabled again,
    // or disabled with a count of 0, and answered.
    void close(FrontEnd& fe) {
        if (host_ && hooks_.set_enabled)
            for (std::size_t k = 0; k < entries_.size(); ++k) hooks_.set_enabled(entries_[k].key, counts_[k] != 0);
        fe.pop();
    }

private:
    MenuArt& art_;
    const render::Palette* palette_;
    bool host_;
    RestrictHooks hooks_;
    const std::vector<RestrictEntry>& entries_;
    std::vector<std::uint8_t> flags_;
    std::vector<std::unique_ptr<render::Sprite8>> pictures_;
    std::vector<std::int32_t> counts_, old_;
    int top_ = 0, loaded_ = 0;
    std::array<int, 12> last_{};  // each slider's value as last read
    std::uint32_t clock_ = 0, next_clock_ = 0;
};

// The restrictions' layout: RESTRICT2 with UnitRestrict5x behind it, drawn on the room's palette
// with the room's interface colours (0x4288D0 without a palette of its own).
const client::ScreenLayout* restrict_layout(MenuArt& art, const client::ScreenLayout* room) {
    static std::unique_ptr<client::ScreenLayout> made;
    if (made) return made.get();
    const client::ScreenLayout* base = art.layout("RESTRICT2");
    if (!base) return nullptr;
    made = std::make_unique<client::ScreenLayout>(*base);
    made->background = art.picture("UnitRestrict5x", nullptr);
    made->palette = room ? room->palette : nullptr;
    if (room) made->colours = room->colours;
    return made.get();
}

// The room screen. The rows are made from the description's templates (0x4455B0): each template
// (PLAYERx, READYx, LOGOx, SIDEx, PINGx, MEMx, RESx, ALLYx, CDx, TEAMICONSx) is copied for rows 0-9,
// named with the row's digit, 20 pixels a row down; the local player's PLAYER button becomes a
// label (2 pixels lower, left-aligned) and so do the other rows' RES buttons (centred).
//
// Live (a session): every frame runs the room's network part (net::BattleRoom: records 0x20,
// ally teams 0x24, logos 0x17 / 0x18, slot table 0x26, alliances 0x23, chat 0x05, the unit
// definition exchange 0x1A, the start 0x08) and shows its records; each control changes this
// machine's records (or, on the host, the game's) the way the room's handler (0x447B10) does,
// and the records go out to the others.
class Lounge : public NetScreen {
public:
    Lounge(std::shared_ptr<NetPart> part, bool host, LoungeState st, bool test, std::shared_ptr<LiveRoom> live = {})
        : NetScreen(part->art, net_layout(part->art, "LOUNGE2", "battleroom", true)), part_(part), host_(host), test_(test), st_(std::move(st)),
          live_(std::move(live)) {
        rows_base_ = widgets_.size();
        if (live_) text_lines_ = live_->text_lines;
        build_rows();
        if (EditState* e = edit("MESSAGE")) e->max_chars = 0x7F;
        focus_edit("MESSAGE");
        focus_name_ = "MESSAGE";
        if (Widget* w = widget("OUTPUT")) w->attribs |= 0x100;  // no picked row
        logos_ = art_.sequence("textures\\logos.gaf", "32xlogos");
        start_art_ = art_.sequence("anims\\LOUNGE2.GAF", "battlestart");
        // The start grill over the start button (0x449BB0: frame 0, shown).
        if (Widget* b = widget("battlestart")) {
            b->active = true;
            b->stage = 0;
            kinds_[index_of("battlestart")] = 200;
        }
        // A client's map button views the map (0x449BB0).
        if (!host_)
            if (Widget* m = widget("MAP")) {
                m->attribs = 2;
                set_caption(*m, "View Map");
            }
        // The host's game options are its to change; the others see them grayed... (0x449BB0: the
        // switches of a client are grayed).
        if (!host_)
            for (const char* n : {"COMMANDER", "MAPPING", "LOSTYPE", "WATCHING", "CHEATING", "FIXEDLOC", "GAMEOPEN"})
                if (Widget* w = widget(n)) w->grayed = true;
        if (!host_)
            for (const char* n : {"MAXUNITS", "ENERGY", "METAL"})
                if (Widget* w = widget(n)) w->grayed = true;
        fixed_clock_ = st_.clock;
        clock_ = fixed_clock_;
        if (live_) unit_limit_setting_ = live_->unit_limit;
        set_sliders();
        // A joining machine's room opens on the first map of its list (0x449BB0); unless the host
        // plays that map, the first making of the rows finds the host's map missing (0x448C70):
        // "<name> does not have this map" on the chat (also sent to the others) and the ready
        // bit off. The name is the session's player name (tests: none, as gui_harness has no
        // session).
        if (!live_ && !host_ && !st_.map.empty() && upper(st_.map) != upper(first_network_map(art_))) {
            const std::string line = "<" + (test ? std::string() : part_->nickname) + "> " + translate("does not have this map");  // (0x448F6C)
            st_.chat.add(line, text_lines_);
            st_.slots[static_cast<std::size_t>(st_.local)].options &= static_cast<std::uint16_t>(~0x20u);
            join_line_ = line;
        }
        if (live_) {
            shown_map_ = live_->room->is_host() ? st_.map : first_network_map(art_);
            live_->room->on_chat_line = [this](int, const std::string& line) {
                // 0x453D40: a chat packet's text goes on the console as it came (colour 8).
                st_.chat.add(line, text_lines_);
                dirty_ = true;
            };
        }
        refresh();
    }
    ~Lounge() override {
        if (live_ && live_->room) live_->room->on_chat_line = nullptr;
    }
    std::string join_line_;  // the line above, sent to the others once the room runs
    int text_lines_ = 10;
    int unit_limit_setting_ = 250;
    const render::Palette* palette() const override { return LayoutScreen::palette(); }
    void enter(FrontEnd& fe) override {
        if (launched_) fe.pop();  // back from the game: the room is over
    }

protected:
    static constexpr const char* kTemplates[10] = {"PLAYERx", "READYx", "LOGOx", "SIDEx", "PINGx", "MEMx", "RESx", "ALLYx", "CDx", "TEAMICONSx"};

    void build_rows() {
        const int local = st_.local;
        built_local_ = local;
        // Each machine shows its own records in its own order (the original's record order: its
        // own player first on a joining machine, then the others as they came, i.e. the host's
        // order): the gadgets keep the slot's number, the row only places them.
        for (int row_slot = 0; row_slot < 10; ++row_slot)
            for (int t = 0; t < 10; ++t) {
                const int row = row_slot;
                const int ti = index_of(kTemplates[t]);
                if (ti < 0) continue;
                Widget w = widgets_[static_cast<std::size_t>(ti)];
                w.name = std::string(kTemplates[t]).substr(0, std::strlen(kTemplates[t]) - 1) + std::to_string(row);
                w.y += 20 * display_row(row);
                w.active = true;
                if (t == 0) {
                    if (row == local) {
                        if (w.kind == WidgetKind::Button) to_label(w);
                        w.attribs = 1;
                    } else {
                        w.attribs |= 0x8000;
                    }
                } else if (t == 1) {
                    if (row != local) {
                        w.grayed = true;
                        w.status = 0;
                    } else {
                        // 0x4455B0: the local player's ready button takes R (its name's first
                        // letter) and Enter.
                        w.quickkey = w.quickkey_char = 'r';
                        enter_default_ = w.name;
                    }
                } else if (t == 6) {
                    if (row != local && w.kind == WidgetKind::Button) to_label(w);
                } else if (t == 7) {
                    if (row == local) w.active = false;
                } else if (t == 9) {
                    w.active = false;
                    w.stage = 10;
                }
                widgets_.push_back(w);
                kinds_[static_cast<int>(widgets_.size() - 1)] = t + 100;
            }
    }
    // The rows made again for another local slot (a joining player is seated by the host's slot
    // table).
    void rebuild_rows() {
        widgets_.resize(rows_base_);
        for (auto it = kinds_.begin(); it != kinds_.end();) it = it->first >= static_cast<int>(rows_base_) ? kinds_.erase(it) : std::next(it);
        if (focus_gadget_ >= static_cast<int>(rows_base_)) focus_gadget_ = index_of("MESSAGE");
        if (held_ >= static_cast<int>(rows_base_)) held_ = -1;
        build_rows();
    }
    // A button turned into a label (0x445300): 2 pixels lower, its caption kept.
    static void to_label(Widget& w) {
        w.kind = WidgetKind::Label;
        w.y += 2;
        w.attribs |= 0x10;
        w.frames = nullptr;
        w.quickkey = w.quickkey_char = 0;  // (no underlined key)
    }
    Widget* row_widget(const char* prefix, int row) { return widget(std::string(prefix) + std::to_string(row)); }
    // Where a slot's row is shown: the host shows its slot table as it is; a joining machine shows
    // its own player in row 0 and the others after it in slot order.
    int display_row(int slot) const {
        if (host_) return slot;
        const int local = st_.local;
        if (slot == local) return 0;
        return slot < local ? slot + 1 : slot;
    }

    // The host's option word (or this machine's own when there is no host).
    std::uint16_t host_options() const {
        const int h = st_.host();
        return st_.slots[static_cast<std::size_t>(h == 10 ? st_.local : h)].options;
    }

    // The sliders (0x449BB0): metal and energy 0..10000 at the host's amounts, the unit limit
    // 20 + its position.
    void set_sliders() {
        if (Widget* w = widget("METAL")) {
            w->range = 10001;
            set_knob(*w, st_.metal * 100);
        }
        if (Widget* w = widget("ENERGY")) {
            w->range = 10001;
            set_knob(*w, st_.energy * 100);
        }
        if (Widget* w = widget("MAXUNITS")) {
            // The settings' unit limit is the most (0x449BB0: range = limit - 20).
            w->range = std::max(0, unit_limit_setting_ - 20);
            set_knob(*w, std::clamp(st_.unit_limit - 20, 0, w->range));
        }
        slider_texts();
    }
    // The room's sliders keep a knob position (0x45B9B0 sets it from a value: value (at most the
    // maximum) / maximum x (travel - 1), whole pixels, one more when not whole; 0x45BA20 reads the
    // value back: knob / (travel - 1) x maximum, truncated), so a value read back can differ.
    static void set_knob(Widget& w, int value) {
        if (w.span < 2 || w.range <= 0) return;
        const double t = static_cast<double>(std::min(value, w.range)) / w.range * (w.span - 1);
        const int k = static_cast<int>(t);
        w.knob_pos = t - k != 0.0 ? static_cast<int>(t + 1.0) : k;
        w.position = value;
        w.knob_value = value;
    }
    static int knob_value(const Widget& w) {
        if (w.span < 2) return 0;
        return static_cast<int>(static_cast<double>(w.knob_pos) / (w.span - 1) * w.range);
    }
    // 0x445C70 / 0x445D60 / 0x445B70: the amounts beside the sliders as read back from the knobs,
    // metal and energy rounded down to hundreds; the unit limit 20 + the slider (the host's own),
    // else the host's record's.
    void slider_texts() {
        if (const Widget* w = widget("METAL"))
            if (Widget* t = widget("METALTEXT")) t->text = std::to_string(knob_value(*w) / 100 * 100);
        if (const Widget* w = widget("ENERGY"))
            if (Widget* t = widget("ENERGYTEXT")) t->text = std::to_string(knob_value(*w) / 100 * 100);
        if (const Widget* w = widget("MAXUNITS"))
            if (Widget* t = widget("MAXUNITSTEXT")) t->text = std::to_string(host_ ? knob_value(*w) + 20 : st_.unit_limit);
    }

    // 0x445ED0, 0x446A50, 0x448C70: the rows and the options from the records.
    void refresh() {
        // This machine's players (its human and computer players) take the local player's ready bit.
        for (LoungeSlot& q : st_.slots)
            if (q.used && (q.controller == 1 || q.controller == 2))
                q.options = static_cast<std::uint16_t>((q.options & ~0x20u) | (st_.slots[static_cast<std::size_t>(st_.local)].options & 0x20u));
        const LoungeSlot& me = st_.slots[static_cast<std::size_t>(st_.local)];
        const bool me_ready = me.ready();
        const std::uint16_t opt = host_options();
        auto stage = [&](const char* n, int v) {
            if (Widget* w = widget(n)) w->stage = v;
        };
        stage("COMMANDER", (opt & 0x1800) >> 11);
        stage("MAPPING", (~opt >> 8) & 1);
        stage("LOSTYPE", !(opt & 0x200) ? 2 : ((~opt >> 10) & 1));
        stage("WATCHING", (opt >> 7) & 1);
        stage("CHEATING", (opt & 0x2000) >> 13);
        stage("FIXEDLOC", (opt & 0x4000) >> 14);
        stage("GAMEOPEN", (~opt >> 15) & 1);
        // Chat: the lines from the ring's tail to its head; when more lines wait than the list
        // shows, the tail moves on one line a frame.
        if (ListState* L = list("OUTPUT")) {
            const Widget* ow = widget("OUTPUT");
            ChatRing& c = st_.chat;
            const int waiting = c.head >= c.tail ? c.head - c.tail : c.head + 30 - c.tail;
            const int rows = font() && ow ? ow->h / (font()->height + 2) : 1;
            if (rows < waiting && ++c.tail > 29) c.tail = 0;
            L->items.clear();
            for (int k = c.tail; k != c.head; k = (k + 1) % 30) L->items.push_back(c.text[static_cast<std::size_t>(k)]);
        }
        // The map's name; red ("NOT SELECTED") without one, blinking red each second when this
        // machine does not have it.
        if (Widget* m = widget("MAPNAME")) {
            if (st_.map.empty()) {
                m->text = translate("NOT SELECTED");  // (set through 0x4A0BF0: translated)
                map_red_ = true;
            } else {
                // (the map info's shown name, 0x435C20: the lowered name's translation when there
                // is one, 0x435A20; set through 0x4A0BF0: translated again)
                m->text = translate(translate_lowered(st_.map));
                map_red_ = !st_.have_map && ((clock_ / 30) & 1) != 0;
            }
        }
        // A client without the host's map cannot view it (0x448C70: MAP grayed).
        if (live_ && !host_)
            if (Widget* m = widget("MAP")) m->grayed = !st_.have_map;
        // Team icons (0x446A50): a group of several players frame 2 x team, of one 2 x team + 1,
        // none 10.
        for (int s = 0; s < 10; ++s) {
            const LoungeSlot& p = st_.slots[static_cast<std::size_t>(s)];
            Widget* w = row_widget("TEAMICONS", s);
            if (!w || !p.playing() || p.controller == 4) continue;
            int members = 0;
            if (p.team != 5)
                for (const LoungeSlot& q : st_.slots) members += q.playing() && q.team == p.team;
            w->stage = members == 0 ? 10 : members == 1 ? p.team * 2 + 1 : p.team * 2;
        }
        for (int s = 0; s < 10; ++s) {
            const LoungeSlot& p = st_.slots[static_cast<std::size_t>(s)];
            Widget* player = row_widget("PLAYER", s);
            Widget* cd = row_widget("CD", s);
            Widget* logo = row_widget("LOGO", s);
            Widget* side = row_widget("SIDE", s);
            Widget* ally = row_widget("ALLY", s);
            Widget* team = row_widget("TEAMICONS", s);
            Widget* res = row_widget("RES", s);
            Widget* ping = row_widget("PING", s);
            Widget* mem = row_widget("MEM", s);
            Widget* ready = row_widget("READY", s);
            if (!p.playing() && !p.watching()) {
                if (cd) cd->active = false;
                if (player) {
                    // ("[%s]" of the translated "BLOCKED", 0x4492BD)
                    std::string t = p.controller == 4 ? "[" + translate("BLOCKED") + "]" : "UNUSED";
                    caption_fit(*player, t.substr(0, 0x1E));
                    player->grayed = me_ready;
                }
                for (Widget* w : {logo, side, team, res, ping, mem})
                    if (w) w->active = false;
                if (ally && s != st_.local) ally->active = false;
                if (ready) {
                    ready->grayed = true;
                    ready->status = 0;
                    ready->active = false;
                }
                continue;
            }
            const bool human_here = p.controller == 1 || (p.controller == 3 && p.kind == 1);
            if (cd) {
                cd->active = human_here && (p.cd & 4);
                cd->grayed = false;
            }
            if (logo) {
                logo->active = p.logo != 0xFF || me_ready;
                logo->stage = p.logo;
            }
            if (player) {
                caption_fit(*player, p.name.substr(0, 0x1E));
                player->grayed = me_ready;
            }
            if (side) {
                side->stage = p.watching() ? 2 : p.side;
                side->active = true;
                side->grayed = !(p.controller == 1 || p.controller == 2) || me_ready;
            }
            if (ally) {
                ally->stage = (me.allied_by[static_cast<std::size_t>(s)] << 1) | me.allied[static_cast<std::size_t>(s)];
                const bool other_human = !(p.controller == 1 || p.watching() || p.controller == 2 || (p.controller == 3 && p.kind == 2));
                ally->active = other_human && !me.watching();
                ally->grayed = me_ready;
            }
            if (team) {
                team->active = !p.watching();
                team->grayed = !(p.controller == 1 || p.controller == 2) || me_ready;
            }
            if (res) {
                // (For others the original prints "%s" without an argument: the stale stack word
                // there points at "n/a", the text the ping column was given before.)
                const std::string t = human_here ? std::to_string(p.screen_w) + "x" + std::to_string(p.screen_h) : std::string("n/a");
                if (res->kind == WidgetKind::Button) {
                    set_caption(*res, t);  // (0x4A0BF0 on every refresh: the quick key found anew)
                } else {
                    res->text = translate(t);
                }
                if (p.controller != 1) res->active = true;
                else res->grayed = me_ready;
            }
            if (ping) {
                if (!(p.controller == 3 && p.kind == 1)) ping->text = translate("n/a");
                else ping->text = std::to_string(p.ping) + (host_ && p.ping_mark ? ":s" : "");
                ping->active = true;
            }
            if (mem) {
                mem->text = human_here ? std::to_string(p.memory) : translate("n/a");  // (the same stale word)
                mem->active = true;
                // Too little memory for the map: the label's background in interface colour 12.
                mem->bg = p.memory < memory_needed() ? 12 : 0;
            }
            if (ready) {
                ready->active = true;
                ready->status = p.ready() ? 1 : 0;
                ready->grayed = p.controller != 1;
            }
        }
        // Start (the host): grayed until everyone is ready (0x456760); "Synching" instead while
        // the unit definitions are compared.
        if (Widget* st = widget("START")) {
            const bool synced = std::none_of(st_.slots.begin(), st_.slots.end(), [](const LoungeSlot& q) { return q.synching; });
            st->active = !host_ || synced;
            st->grayed = !all_ready();
            if (Widget* sy = widget("SYNCHING")) sy->active = host_ && !synced;
            if (!host_) {
                // A joining player's START stays grayed (0x449BB0).
                st->active = true;
                if (live_) st->grayed = true;
            }
        }
    }
    bool all_ready() const {
        int remote = 0;
        for (const LoungeSlot& p : st_.slots) remote += p.used && p.controller == 3;
        if (remote == 0) return false;
        bool all_watching = true;
        for (const LoungeSlot& p : st_.slots) {
            if (!p.used) {
                all_watching = false;
                continue;
            }
            if ((p.controller >= 1 && p.controller <= 3) && !p.ready()) return false;
            if (!p.watching()) all_watching = false;
        }
        if (st_.session_players == 1) return false;
        return !all_watching;
    }
    // The memory a map needs (0x435920) from its TNT file's size (map info +0xA04): below
    // 0x3E6666 bytes 16 MB, 0x600000 24, 0x800000 32, 0xA00000 48, 0xC00000 64, else 128.
    int memory_needed() {
        if (st_.map != size_map_) {
            size_map_ = st_.map;
            map_size_ = static_cast<std::int64_t>(art_.vfs().size_or_zero(art_.vfs().virtual_path("maps", map_file_name(art_.vfs(), st_.map), "tnt")));
        }
        const std::int64_t s = map_size_;
        return s < 0x3E6666 ? 16 : s < 0x600000 ? 24 : s < 0x800000 ? 32 : s < 0xA00000 ? 48 : s < 0xC00000 ? 64 : 128;
    }
    std::string size_map_;
    std::int64_t map_size_ = 0;
    void caption_fit(Widget& w, const std::string& t) {
        // Every refresh sets the whole caption (0x4A0BF0: a plain button finds its quick key in
        // it anew, 0x4A05E0), then 0x4A5D50 drops characters from the end until the caption
        // fits the width - 6 (the key stays).
        if (w.kind == WidgetKind::Label) w.text = t;
        else set_caption(w, t);
        const client::Font* f = (w.attribs & 0x8000) || w.kind == WidgetKind::Label ? art_.content().small_font : font();
        while (f && !w.text.empty() && f->text_width(w.text) > w.w - 6) w.text.pop_back();
    }

    void draw_other(FrontEnd&, render::Image8& out, const Widget& w) override {
        const int i = static_cast<int>(&w - widgets_.data());
        const auto k = kinds_.find(i);
        const render::ClipRect clip = screen_clip(out);
        if (k != kinds_.end() && k->second == 200 && start_art_ && !start_art_->empty()) {
            const render::Sprite8& s = (*start_art_)[static_cast<std::size_t>(std::clamp(w.stage, 0, static_cast<int>(start_art_->size()) - 1))];
            render::blit(out, clip, s, ox_ + w.x + s.x_offset, oy_ + w.y + s.y_offset);  // its top-left at the gadget
            return;
        }
        if (k != kinds_.end() && k->second == 102 && logos_ && w.stage >= 0 && w.stage < static_cast<int>(logos_->size())) {
            // The player's 32xlogos frame mapped onto the box as a textured quad (texels 1 .. size - 1).
            const render::Sprite8& s = (*logos_)[static_cast<std::size_t>(w.stage)];
            const int x0 = ox_ + w.x, y0 = oy_ + w.y, x1 = x0 + w.w - 1, y1 = y0 + w.h - 1;
            const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
            const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
            render::RasterTarget t;
            t.pixels = out.pixels.data();
            t.pitch = out.width;
            t.clip = clip;
            render::textured_quad_uv(t, v, uv, s);
        }
    }
    void draw_over(FrontEnd& fe, render::Image8& out) override {
        draw_over_rows(fe, out);
        // (Each frame of the room also prints every player's version, "%i.%i", on its logo box
        // (0x44A680), but the interface pass draws the logos over it before the frame is shown.)
        if (live_ && !live_->dump.empty()) dump_state(out);
    }
    const Widget* row_widget_c(const char* prefix, int row) const {
        const std::string n = std::string(prefix) + std::to_string(row);
        for (const Widget& w : widgets_)
            if (upper(w.name) == n) return &w;
        return nullptr;
    }
    int room_frames_ = 0;
    void draw_over_rows(FrontEnd& fe, render::Image8& out) {
        // The start button's art through its light row (0x4B8310), then the grill again over it.
        if (Widget* st = widget("START"); st && st->active && start_level_ > 0 && st->frames && fe.menu_content().light.size() >= 0x2000) {
            const auto& light = fe.menu_content().light;
            lit_frames_.assign(st->frames->begin(), st->frames->end());
            for (render::Sprite8& s : lit_frames_)
                for (std::uint8_t& p : s.pixels)
                    if (!s.has_key || p != s.key) p = light[static_cast<std::size_t>(start_level_) * 256 + p];
            Widget lit = *st;
            lit.frames = &lit_frames_;
            draw_widget(fe, out, lit, held_ == index_of("START"));
            if (const Widget* g = widget("battlestart"); g && g->active) draw_other(fe, out, *g);
        }
        // Grayed sliders (a client's metal, energy and unit limit) and their arrows go through
        // shade row 12 (-20).
        for (std::size_t i = 0; i < widgets_.size(); ++i) {
            const Widget& w = widgets_[i];
            const bool slider = w.kind == WidgetKind::ScrollBar && w.grayed && w.active;
            bool arrow = false;
            if (w.slider_step != 0 && w.active)
                if (const Widget* s = widget(w.link)) arrow = s->grayed;
            if (!slider && !arrow) continue;
            shade_rect(fe, out, screen_clip(out), ox_ + w.x, oy_ + w.y, ox_ + w.x + w.w - 1, oy_ + w.y + w.h - 1, -20);
            // Labels drawn after it (in gadget order) that reach into it go on top again.
            for (std::size_t j = i + 1; j < widgets_.size(); ++j) {
                const Widget& l = widgets_[j];
                if (l.kind != WidgetKind::Label || !l.active) continue;
                if (l.x < w.x + w.w && w.x < l.x + l.w && l.y < w.y + w.h && w.y < l.y + l.h) draw_widget(fe, out, l, false);
            }
        }
        // PLAYER0's colour word is 24 (0x44A680): a button's art goes through light row 24; as a
        // label (the local player's row) its text does.
        if (Widget* p = widget("PLAYER0"); p && p->active && p->kind == WidgetKind::Button && p->frames && fe.menu_content().light.size() >= 0x2000) {
            const auto& light = fe.menu_content().light;
            lit_player_.assign(p->frames->begin(), p->frames->end());
            for (render::Sprite8& s : lit_player_)
                for (std::uint8_t& px : s.pixels)
                    if (!s.has_key || px != s.key) px = light[24 * 256 + px];
            Widget lit = *p;
            lit.frames = &lit_player_;
            draw_widget(fe, out, lit, held_ == index_of("PLAYER0"));
        }
        if (Widget* p = widget("PLAYER0"); p && p->active && p->kind == WidgetKind::Label && fe.menu_content().small_font) {
            const client::Font& f = *fe.menu_content().small_font;
            line_text(out, screen_clip(out), f, p->text, ox_ + p->x, oy_ + p->y + f.raise, p->w, 24, fe.menu_content().light);
        }
        // Grayed labels (the local player's name while ready) go through shade row 12 (0x4A56B0).
        for (std::size_t i = 0; i < widgets_.size(); ++i) {
            const Widget& w = widgets_[i];
            if (w.kind != WidgetKind::Label || !w.active || !w.grayed) continue;
            shade_rect(fe, out, screen_clip(out), ox_ + w.x, oy_ + w.y, ox_ + w.x + w.w - 1, oy_ + w.y + w.h - 1, -20);
            // Gadgets after it in gadget order that reach into its box are drawn over it again.
            for (std::size_t j = i + 1; j < widgets_.size(); ++j) {
                const Widget& l = widgets_[j];
                if (!l.active || (l.kind != WidgetKind::Label && l.kind != WidgetKind::Button)) continue;
                if (l.x < w.x + w.w && w.x < l.x + l.w && l.y < w.y + w.h && w.y < l.y + l.h) draw_widget(fe, out, l, static_cast<int>(j) == held_);
            }
        }
    }

    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        // (Tests: the clock held, moving on `clockstep` ticks a frame.)
        clock_ = fixed_clock_ ? fixed_clock_ + st_.clock_step * frames_seen_++ : now_ms * 30u / 1000u;
        ++room_frames_;
        if (live_ && !live_frame(fe)) return;
        // The rows are made again only when something changed (GS+0x2BEE bit 0).
        if (dirty_) {
            dirty_ = false;
            refresh();
            if (host_) animate(fe);
        }
    }
    // 0x44A680, the host: once everybody is ready the start grill opens (frame 1 at once with
    // "Options", then one frame each 4 ticks up to 8, "Panel" at 4) and the start button's art
    // glows (its colour word = the clock's low five bits: the light row it is drawn through).
    void animate(FrontEnd& fe) {
        Widget* grill = widget("battlestart");
        if (!grill) return;
        const int frames = start_art_ ? static_cast<int>(start_art_->size()) : 9;
        if (grill->stage > 0 && clock_ > anim_tick_) {
            if (grill->stage < 8) {
                ++grill->stage;
                dirty_ = true;
            }
            anim_tick_ += 4;
            if (grill->stage == 4) sfx(fe, art_, "Panel");
        }
        if (grill->stage != 0 && grill->stage < frames - 1) dirty_ = true;
        const bool ready = all_ready();
        // The grill takes the pointer (its "hot" bit, gadget +0xC8) until everybody is ready
        // (0x44A680 clears it, for good): then the pointer reaches START under it.
        if (ready) grill->hot = false;
        if (ready) {
            dirty_ = true;
            if (grill->stage == 0) {
                grill->stage = 1;
                anim_tick_ = clock_;
                sfx(fe, art_, "Options");
            }
            start_level_ = static_cast<int>(clock_ & 0x1F);
        }
        if (Widget* st = widget("START")) st->grayed = !ready;
        if (grill->stage > 0) grill_clocks_.push_back(clock_);  // (tests: the room dumps)
    }
    std::vector<std::uint32_t> grill_clocks_;  // the clock of each room frame since the grill began to open
    bool dirty_ = true;
    std::uint32_t fixed_clock_ = 0, anim_tick_ = 0, frames_seen_ = 0;
    int start_level_ = 0;  // the start button's colour word (its art's light row; 0 plain)

    // ---------------------------------------------------------------- live
    // The room's state from the engine's room: each record as the room shows it.
    LoungeState live_state() {
        const net::BattleRoom& room = *live_->room;
        LoungeState st;
        st.chat = st_.chat;
        st.clock = st_.clock;
        st.clock_step = st_.clock_step;
        const int local = room.local_slot() < 0 ? 0 : room.local_slot();
        st.local = local;
        const auto& ps = room.players();
        int count = 0;
        for (int s = 0; s < 10; ++s) {
            LoungeSlot& q = st.slots[static_cast<std::size_t>(s)];
            const net::RoomPlayer& p = ps[static_cast<std::size_t>(s)];
            if (!p.net_id) {
                q.controller = room.blocked(s) ? 4 : 0;
                continue;
            }
            ++count;
            const bool own = p.net_id == room.net_id();
            const bool mine = own || room.runs(s);
            q.used = true;
            q.controller = own ? 1 : mine ? 2 : 3;
            // (A computer player's name is its session player's: "AI:" and its machine's player.)
            q.name = p.name;
            q.ping = p.latency_ms;
            q.team = p.ally_team;
            q.kind = p.kind;
            q.side = p.side;
            q.logo = p.logo;
            q.flags = p.flags;
            q.options = static_cast<std::uint16_t>((p.options & ~0x0700u) | ((p.map_flags & 7u) << 8));
            q.memory = p.memory_mb;
            q.cd = p.cd;
            q.screen_w = p.screen_w;
            q.screen_h = p.screen_h;
            q.major = p.version_major;
            q.minor = p.version_minor;
            q.synching = room.synching(s);
            q.ping_mark = room.is_host() && q.controller == 3 && p.kind == 1 && !q.synching;
        }
        if (room.local_slot() < 0) {
            // Not seated yet: this machine's own record.
            LoungeSlot& q = st.slots[0];
            q = LoungeSlot{};
            q.used = true;
            q.controller = 1;
            q.kind = 1;
            q.name = part_->nickname;
            q.options = 0;
            count = std::max(count, 1);
        }
        LoungeSlot& me = st.slots[static_cast<std::size_t>(local)];
        for (int s = 0; s < 10; ++s) {
            me.allied[static_cast<std::size_t>(s)] = room.allied()[static_cast<std::size_t>(s)];
            me.allied_by[static_cast<std::size_t>(s)] = room.allied_by()[static_cast<std::size_t>(s)];
        }
        // 0x446C70: players of one ally team are allied both ways.
        if (me.team != 5)
            for (int s = 0; s < 10; ++s) {
                const LoungeSlot& q = st.slots[static_cast<std::size_t>(s)];
                if (q.playing() && q.team == me.team) me.allied[static_cast<std::size_t>(s)] = me.allied_by[static_cast<std::size_t>(s)] = 1;
            }
        st.session_players = count;
        const int h = st.host();
        if (h != 10) {
            const net::RoomPlayer& hp = ps[static_cast<std::size_t>(h)];
            st.map = hp.map;
            st.energy = hp.energy;
            st.metal = hp.metal;
            st.unit_limit = hp.unit_limit;
        } else {
            // No host record yet: the map chosen in this machine's own list (the first one,
            // 0x449BB0).
            st.map = room.is_host() ? ps[static_cast<std::size_t>(local)].map : first_network_map(art_);
        }
        st.have_map = room.is_host() || ps[static_cast<std::size_t>(local)].map_ready;
        return st;
    }
    static bool same_slot(const LoungeSlot& a, const LoungeSlot& b) {
        return a.used == b.used && a.controller == b.controller && a.name == b.name && a.ping == b.ping && a.team == b.team && a.allied == b.allied &&
               a.allied_by == b.allied_by && a.kind == b.kind && a.side == b.side && a.logo == b.logo && a.flags == b.flags && a.options == b.options &&
               a.memory == b.memory && a.cd == b.cd && a.screen_w == b.screen_w && a.screen_h == b.screen_h && a.synching == b.synching &&
               a.ping_mark == b.ping_mark;
    }
    bool same_state(const LoungeState& a, const LoungeState& b) const {
        if (a.local != b.local || a.session_players != b.session_players || a.map != b.map || a.have_map != b.have_map || a.energy != b.energy ||
            a.metal != b.metal || a.unit_limit != b.unit_limit)
            return false;
        for (int s = 0; s < 10; ++s)
            if (!same_slot(a.slots[static_cast<std::size_t>(s)], b.slots[static_cast<std::size_t>(s)])) return false;
        return true;
    }
    // One frame of the room's network part (0x44A680 with 0x453D40 and 0x456310); false when the
    // room is over (left, sent away, or handed to the game).
    bool live_frame(FrontEnd& fe) {
        net::BattleRoom& room = *live_->room;
        if (launched_) return false;
        room.frame(clock_, live_->crt_rand.get());
        if (room.refused() || room.out_reason() >= 0) {
            // Out of the room (the local record's out byte +0x22): back to the game list, which
            // shows why (0x443CB0) — nothing for one's own leaving (2).
            const int reason = room.out_reason() >= 0 ? room.out_reason() : 0;
            MenuArt& art = art_;
            launched_ = true;
            fe.pop();
            if (reason != 2) show_net_message(fe, art, translate(out_text(reason)), 320);  // (translated, 0x443F6D)
            return false;
        }
        if (room.started()) {
            // The host's start (0x08): the loading screen and the battle.
            launched_ = true;
            client::launch_network_battle(fe, live_->cfg, client::NetLaunch{std::move(live_->transport), live_->session, live_->room, live_->crt_rand});
            return false;
        }
        if (host_) host_rules(fe);
        LoungeState next = live_state();
        if (next.local != built_local_) {
            st_.local = next.local;
            rebuild_rows();
            dirty_ = true;
        }
        if (!same_state(next, st_)) {
            const bool resources = next.energy != st_.energy || next.metal != st_.metal || next.unit_limit != st_.unit_limit;
            const std::string map_before = st_.map;
            next.chat = st_.chat;
            st_ = next;
            dirty_ = true;
            if (!host_ && resources) set_sliders();
            // 0x448C70: a new map this machine does not have: "<name> does not have this map"
            // (sent to the others too) and its ready bit off.
            if (!host_ && st_.map != shown_map_ && !st_.map.empty()) {
                shown_map_ = st_.map;
                if (!st_.have_map) {
                    const std::string line = client::chat_line(part_->nickname, std::string(), translate("does not have this map"));  // (0x448F6C)
                    st_.chat.add(line, text_lines_);
                    room.say(line);
                    room.set_ready(false);
                }
            }
            (void)map_before;
        }
        // The host's sliders: their amounts go into its records (0x445C70 / 0x445D60 / 0x445B70).
        if (host_) {
            const Widget* e = widget("ENERGY");
            const Widget* m = widget("METAL");
            const Widget* u = widget("MAXUNITS");
            const auto ev = static_cast<std::uint16_t>(e ? knob_value(*e) / 100 : st_.energy);
            const auto mv = static_cast<std::uint16_t>(m ? knob_value(*m) / 100 : st_.metal);
            const auto uv = static_cast<std::uint16_t>(u ? knob_value(*u) + 20 : st_.unit_limit);
            if (ev != st_.energy || mv != st_.metal || uv != st_.unit_limit) {
                room.set_resources(ev, mv, uv);
                slider_texts();
            }
        }
        // The CD word, the periodic records and the definition exchange run in the room itself.
        return true;
    }
    // 0x448C70 on the host: in the "deathmatch" commander mode no computer player stays (status
    // 11); when the host forbids watching, its watchers play again (side 0).
    void host_rules(FrontEnd&) {
        net::BattleRoom& room = *live_->room;
        const std::uint16_t opt = host_options();
        for (int s = 0; s < 10; ++s) {
            const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            if ((opt & 0x1800) == 0x1000 && q.used && q.controller == 2) room.remove_computer(s, 11);
            if (!(opt & 0x80) && q.used && (q.controller == 1 || q.controller == 2) && q.watching())
                room.edit_own(s, [](net::RoomPlayer& p) {
                    p.options = static_cast<std::uint16_t>(p.options & ~net::room::kWatch);
                    p.side = 0;
                });
        }
    }
    void leave(FrontEnd& fe) {
        if (live_ && !launched_) {
            live_->room->leave();
            launched_ = true;
        }
        fe.pop();
    }

    // Tests (TA_ROOM_DUMP): each changed room state as a room file and the room's frame.
    void dump_state(const render::Image8& out) {
        const std::string sig = signature();
        if (sig == dumped_sig_) return;
        dumped_sig_ = sig;
        char name[32];
        std::snprintf(name, sizeof name, "%04d", dump_n_++);
        {
            LoungeState s = st_;
            s.clock = clock_;  // the room clock of this frame (gui_harness: clock:T)
            write_lounge(live_->dump + name + ".txt", s);
            // The focus (the gadget last pressed) and whether the chat line takes the keys.
            std::ofstream o(live_->dump + name + ".txt", std::ios::app);
            if (focus_gadget_ > 0 && focus_gadget_ < static_cast<int>(widgets_.size())) o << "focus " << widgets_[static_cast<std::size_t>(focus_gadget_)].name << "\n";
            o << "keys " << (focus_ >= 0 ? 1 : 0) << "\n";
            // The start grill's opening began at this clock (gui_harness: the room's frames from
            // there, one a tick).
            if (const Widget* g = widget_c("battlestart"); g && g->stage > 0 && !grill_clocks_.empty()) {
                o << "grill";
                for (std::uint32_t c : grill_clocks_) o << ' ' << c;
                o << "\n";
            }
        }
        write_frame(live_->dump + name + ".bmp", out);
    }
    std::string signature() const {
        std::ostringstream o;
        o << st_.local << '|' << st_.map << '|' << st_.have_map << '|' << st_.energy << ',' << st_.metal << ',' << st_.unit_limit << '|' << st_.chat.head
          << ',' << st_.chat.tail;
        for (const LoungeSlot& q : st_.slots)
            o << '|' << q.used << q.controller << q.name << ',' << q.ping << ',' << q.team << ',' << q.side << ',' << q.logo << ',' << q.flags << ','
              << q.options << ',' << q.synching;
        if (const Widget* g = widget_c("battlestart")) o << "|g" << g->stage;
        o << "|f" << focus_gadget_ << ',' << focus_;
        return o.str();
    }
    const Widget* widget_c(const std::string& n) const {
        for (const Widget& w : widgets_)
            if (upper(w.name) == upper(n)) return &w;
        return nullptr;
    }
    void write_frame(const std::string& path, const render::Image8& out) {
        const render::Palette* pal = palette();
        const render::Palette& p = pal ? *pal : art_.content().palette;
        const std::uint32_t stride = (static_cast<std::uint32_t>(out.width) + 3u) & ~3u, size = stride * static_cast<std::uint32_t>(out.height),
                            off = 14 + 40 + 1024;
        std::vector<std::uint8_t> b = {'B', 'M'};
        auto put32 = [&](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        put32(off + size);
        put32(0);
        put32(off);
        put32(40);
        put32(static_cast<std::uint32_t>(out.width));
        put32(static_cast<std::uint32_t>(out.height));
        b.insert(b.end(), {1, 0, 8, 0});
        put32(0);
        put32(size);
        put32(2835);
        put32(2835);
        put32(256);
        put32(0);
        for (int i = 0; i < 256; ++i) b.insert(b.end(), {p[static_cast<std::size_t>(i)].b, p[static_cast<std::size_t>(i)].g, p[static_cast<std::size_t>(i)].r, 0});
        for (int y = out.height - 1; y >= 0; --y) {
            const std::uint8_t* row = out.row(y);
            b.insert(b.end(), row, row + out.width);
            for (std::uint32_t k = static_cast<std::uint32_t>(out.width); k < stride; ++k) b.push_back(0);
        }
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    std::string dumped_sig_;
    static inline int dump_n_ = 0;  // (numbered across rooms: a player may leave and join again)

    void on_enter(FrontEnd& fe, int) override { send_line(fe); }
    void send_line(FrontEnd&) {
        EditState* e = edit("MESSAGE");
        if (!e || e->text.empty()) return;
        const LoungeSlot& me = st_.slots[static_cast<std::size_t>(st_.local)];
        if (live_ && upper(e->text) == "+SYNCERR") {
            // (0x447B10: the unit definition exchange's error list instead; the engine keeps none.)
        } else {
            // 0x463E50: "<name> text" on the console (colour 4), sent to the others.
            const std::string line = client::chat_line(live_ ? part_->nickname : me.name, std::string(), e->text);
            st_.chat.add(line, text_lines_);
            if (live_) live_->room->say(line);
        }
        e->text.clear();
        e->point = 0;
        dirty_ = true;
        // The chat line keeps the keys (0x4A7190).
        focus_gadget_ = index_of("MESSAGE");
        activate_field(focus_gadget_);
    }
    // Which row a gadget belongs to (its name's prefix and digit); -1 for others.
    static int row_of(const std::string& n, const char* prefix) {
        const std::size_t l = std::strlen(prefix);
        if (n.size() != l + 1 || n.compare(0, l, prefix) != 0 || n[l] < '0' || n[l] > '9') return -1;
        return n[l] - '0';
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        const std::string n = upper(w.name);
        if (n == "PREVMENU") {
            sfx(fe, art_, "Previous");
            leave(fe);
            return;
        }
        if (!live_) {
            test_press(fe, w, n);
            return;
        }
        live_press(fe, w, n);
    }
    // Tests (a room from a file, no session): the handler's effects on this screen alone — the
    // ready button (the host's Enter and focus go to START), the slot question, the map choice
    // and view, the restrictions; the rest only sounds.
    void test_press(FrontEnd& fe, Widget&, const std::string& n) {
        const int local = st_.local;
        LoungeSlot& me = st_.slots[static_cast<std::size_t>(local)];
        if (n == "START") {
            sfx(fe, art_, "BigButton");
            return;
        }
        if (int s = row_of(n, "READY"); s >= 0) {
            sfx(fe, art_, "Multi");
            if (s != local || !st_.have_map) return;
            me.options = static_cast<std::uint16_t>(me.options ^ 0x20u);
            dirty_ = true;
            if (host_) {
                enter_default_ = "START";
                focus_gadget_ = index_of("START");
            }
            return;
        }
        if (int s = row_of(n, "PLAYER"); s >= 0) {
            if (s == local) return;
            sfx(fe, art_, "Multi");
            LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            dirty_ = true;
            const std::uint16_t opt = host_options();
            if (q.controller == 3) {
                if (host_) fe.push(std::make_unique<RejectQuestion>(art_, q.name, []() {}));
                return;
            }
            if (q.controller == 0 && host_) {
                q.controller = 4;  // open -> blocked
                return;
            }
            if (q.controller == 2) {
                if (clock_ - computer_since_[s] >= 0x1F) q = LoungeSlot{};
                return;
            }
            if (q.controller == 4) q.controller = 0;
            if (opt & 0x8000) {
                show_net_message(fe, art_, translate("Can't add another player when game is closed."), 500);
                return;
            }
            int computers = 0;
            std::array<bool, 10> logos{};
            for (const LoungeSlot& c : st_.slots) {
                computers += c.used && (c.controller == 2 || (c.controller == 3 && c.kind == 2));
                if (c.playing()) logos[static_cast<std::size_t>(std::min(c.logo, 9))] = true;
            }
            if ((opt & 0x1800) != 0x1000 && computers == 0) {
                q = LoungeSlot{};
                q.used = true;
                q.controller = 2;
                q.kind = 2;
                q.name = ("AI:" + me.name).substr(0, 16);
                q.logo = 0;
                for (int k = 0; k < 10; ++k)
                    if (!logos[static_cast<std::size_t>(k)]) {
                        q.logo = k;
                        break;
                    }
                q.options = static_cast<std::uint16_t>(me.options & 0x20u);
                computer_since_[s] = clock_;
                ++st_.session_players;
            }
            return;
        }
        if (n == "RESTRICTIONS") {
            sfx(fe, art_, "Options");
            open_restrictions(fe);
            return;
        }
        if (n == "MAP" || n == "MAPNAME") {
            sfx(fe, art_, "Multi");
            const std::string map = st_.map;
            if (host_) fe.push(make_room_map_choice(art_, map, [](const std::string&) {}));
            else fe.push(make_room_map_view(art_, [map]() { return map; }));
            return;
        }
        sfx(fe, art_, "Multi");
    }
    void say_line(const std::string& text) {
        const std::string line = client::chat_line(part_->nickname, std::string(), text);
        st_.chat.add(line, text_lines_);
        live_->room->say(line);
        dirty_ = true;
    }
    // The room's handler (0x447B10) for this machine's controls.
    void live_press(FrontEnd& fe, Widget& w, const std::string& n) {
        net::BattleRoom& room = *live_->room;
        const int local = st_.local;
        LoungeSlot& me = st_.slots[static_cast<std::size_t>(local)];
        std::uint16_t opt = host_options();
        dirty_ = true;
        if (int s = row_of(n, "LOGO"); s >= 0) {
            // A logo of this machine's: the local player asks the host for that row's logo + 1.
            const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            if (q.used && (q.controller == 1 || q.controller == 2)) {
                sfx(fe, art_, "Multi");
                room.set_logo(static_cast<std::uint8_t>(q.logo + 1));
            }
            return;
        }
        if (int s = row_of(n, "PLAYER"); s >= 0) {
            if (s == local) return;
            sfx(fe, art_, "Multi");
            LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            if (q.controller == 0 && host_) {
                room.set_blocked(s, true);  // open -> blocked
                return;
            }
            if (q.controller == 2) {
                // A computer player some time after it came (record +8, 31 ticks) leaves.
                if (clock_ - computer_since_[room.players()[static_cast<std::size_t>(s)].net_id] >= 0x1F) room.remove_computer(s, 1);
                return;
            }
            if (q.controller == 3) {
                if (host_) {
                    const std::string name = q.name;
                    auto live = live_;
                    fe.push(std::make_unique<RejectQuestion>(art_, name, [live, s]() { live->room->kick(s, 1); }));
                }
                return;
            }
            if (q.controller == 4) room.set_blocked(s, false);  // blocked -> open, then a computer
            if (opt & 0x8000) {
                show_net_message(fe, art_, translate("Can't add another player when game is closed."), 500);
                return;
            }
            // 0x457B90: one computer player per machine (this machine's records, controller 2).
            int computers = 0;
            for (const LoungeSlot& c : st_.slots) computers += c.used && c.controller == 2;
            if ((opt & 0x1800) != 0x1000 && computers == 0) {
                if (host_) {
                    // 0x451220: a computer player in this slot, the first logo nobody plays.
                    if (room.add_computer_at(s, 0) >= 0) computer_since_[room.players()[static_cast<std::size_t>(s)].net_id] = clock_;
                } else {
                    // A joining machine's: its session player, then the host seats it.
                    room.add_own_computer();
                }
            }
            return;
        }
        if (int s = row_of(n, "SIDE"); s >= 0) {
            sfx(fe, art_, "Multi");
            const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            const int sides = std::max(1, live_->sides);
            const bool allow_watch = (opt & 0x80) != 0;
            room.edit_own(s, [&](net::RoomPlayer& p) {
                if (p.options & net::room::kWatch) {
                    p.options = static_cast<std::uint16_t>(p.options & ~net::room::kWatch);
                    p.side = 0;
                    return;
                }
                if (++p.side >= sides) {
                    p.side = 0;
                    if (allow_watch && q.controller == 1) p.options = static_cast<std::uint16_t>(p.options | net::room::kWatch);
                }
            });
            return;
        }
        if (int s = row_of(n, "ALLY"); s >= 0) {
            // The local player's alliance with that player (0x23). On a teammate the local player
            // then leaves its team: 0x446E90 breaks every alliance in it (forced, so the toggled
            // byte is 0 again), team 5, 0x452BD0 sends it (0x24). The sound and the line read the
            // byte afterwards (the +0x113 test, byte * 2 == 3, never holds).
            bool now = me.allied[static_cast<std::size_t>(s)] == 0;
            room.set_allied(s, now);
            if (me.team != 5 && me.team == st_.slots[static_cast<std::size_t>(s)].team) {
                room.set_ally_team_of(local, net::room::kNoTeam);
                now = false;
            }
            sfx(fe, art_, now ? "Ally" : "Multi");
            // (" %s %s" with the words translated, 0x44725E / 0x448020)
            say_line(std::string(" ") + translate(now ? "allied with" : "broke alliance with") + " " + st_.slots[static_cast<std::size_t>(s)].name);
            return;
        }
        if (int s = row_of(n, "TEAMICONS"); s >= 0) {
            sfx(fe, art_, "Ally");
            const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            const int team = (q.team + 1) % 6;
            room.set_ally_team_of(s, static_cast<std::uint8_t>(team));  // (0x446E90 first, forced)
            return;
        }
        if (int s = row_of(n, "RES"); s >= 0) {
            if (s != local) return;
            // 0x446310: the next display mode (Shift: the one before) for the battle.
            sfx(fe, art_, "Multi");
            std::vector<std::array<int, 2>> modes;
            fe.display_modes(modes);
            if (modes.empty()) modes = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 1024}, {1600, 1200}};
            int at = -1;
            for (std::size_t k = 0; k < modes.size(); ++k)
                if (modes[k][0] == me.screen_w && modes[k][1] == me.screen_h) at = static_cast<int>(k);
            if (at < 0) return;
            at = press_button_ == 3 ? (at + static_cast<int>(modes.size()) - 1) % static_cast<int>(modes.size()) : (at + 1) % static_cast<int>(modes.size());
            const auto& m = modes[static_cast<std::size_t>(at)];
            room.set_screen(static_cast<std::uint16_t>(m[0]), static_cast<std::uint16_t>(m[1]));
            fe.settings().set_int("DisplaymodeWidth", m[0]);
            fe.settings().set_int("DisplaymodeHeight", m[1]);
            return;
        }
        if (int s = row_of(n, "READY"); s >= 0) {
            if (s != local) return;
            sfx(fe, art_, "Multi");
            if (!st_.have_map) return;  // (0x4A1110: the button back to not ready)
            const bool ready = !me.ready();
            room.set_ready(ready);
            for (LoungeSlot& q : st_.slots)
                if (q.used && (q.controller == 1 || q.controller == 2)) q.options = static_cast<std::uint16_t>((q.options & ~0x20u) | (ready ? 0x20u : 0u));
            if (host_) {
                // The host's Enter now starts the game; the focus goes to START.
                enter_default_ = "START";
                focus_gadget_ = index_of("START");
            }
            return;
        }
        if (n == "START") {
            if (!host_) return;
            sfx(fe, art_, "BigButton");
            start_game(fe);
            return;
        }
        if (n == "RESTRICTIONS") {
            sfx(fe, art_, "Options");
            // (The restrictions dialog: see open_restrictions.)
            open_restrictions(fe);
            return;
        }
        if (n == "MAP" || n == "MAPNAME") {
            sfx(fe, art_, "Multi");
            open_map(fe);
            return;
        }
        if (!host_) return;
        // The host's switches (0x447B10): the option word, then the records and the session's
        // description.
        if (n == "COMMANDER") {
            const std::uint16_t mode = static_cast<std::uint16_t>(((opt & 0x1800) + 0x800) & 0x1800);
            opt = static_cast<std::uint16_t>((opt & ~0x1800) | (mode > 0x1000 ? 0 : mode));
        } else if (n == "LOSTYPE") {
            if (!(opt & 0x200)) opt = static_cast<std::uint16_t>(opt | 0x600);
            else if (opt & 0x400) opt = static_cast<std::uint16_t>(opt & ~0x400);
            else opt = static_cast<std::uint16_t>(opt & ~0x200);
        } else if (n == "WATCHING") {
            opt = static_cast<std::uint16_t>(opt ^ 0x80);
            if (!(opt & 0x80) && me.watching()) room.set_watch(false);
        } else if (n == "CHEATING") {
            opt = static_cast<std::uint16_t>(opt ^ 0x2000);
        } else if (n == "FIXEDLOC") {
            opt = static_cast<std::uint16_t>(opt ^ 0x4000);
        } else if (n == "MAPPING") {
            opt = static_cast<std::uint16_t>(opt ^ 0x100);
        } else if (n == "GAMEOPEN") {
            opt = static_cast<std::uint16_t>(opt ^ 0x8000);
        } else {
            return;
        }
        sfx(fe, art_, "Multi");
        room.set_option_word(opt);
    }
    // 0x446E90: a player leaving its ally team breaks the alliances with the team's members.
    // START on the host (0x447B10): enough game CDs, not everyone in one team, a map; watchers
    // leave when watching is not allowed (status 9); then the game is marked playing (option bit
    // 0x10, the session closes to newcomers) and the start goes out (0x08).
    void start_game(FrontEnd& fe) {
        net::BattleRoom& room = *live_->room;
        int cds = 0, humans = 0, computers = 0;
        for (const LoungeSlot& q : st_.slots) {
            if (!q.used) continue;
            const bool human = q.controller == 1 || (q.controller == 3 && q.kind == 1);
            humans += human;
            computers += q.controller == 2 || (q.controller == 3 && q.kind == 2);
            cds += human && (q.cd & 4);
        }
        if (cds < 1 || (cds < 2 && humans > 3) || (cds < 3 && humans > 6)) {
            show_net_message(fe, art_, translate("There are not enough game CDs present to play"), 200);
            return;
        }
        for (int t = 0; t < 5; ++t) {
            int n = 0;
            for (const LoungeSlot& q : st_.slots) n += q.playing() && q.team == t;
            if (n == humans + computers) {
                show_net_message(fe, art_, translate("Can not start game with all players on the same team."), 200);
                return;
            }
        }
        if (st_.map.empty()) {
            open_map(fe);
            return;
        }
        const std::uint16_t opt = host_options();
        if (!(opt & 0x80))
            for (int s = 0; s < 10; ++s) {
                const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
                if (q.used && q.controller == 3 && q.watching()) room.kick(s, 9);
            }
        room.set_option_word(static_cast<std::uint16_t>(opt | 0x10));
        if (!room.start()) {
            room.set_option_word(opt);
            return;
        }
        // (0x447B10 at the start: the host's switches become the multiplayer settings, saved)
        client::Settings& set = fe.settings();
        set.set_int("MultiLineOfSight", (opt >> 9) & 1);
        set.set_int("MultiLOSType", (opt >> 10) & 1);
        set.set_int("MultiCommanderDeath", (opt & 0x1800) >> 11);
        set.set_int("SkirmishLocation", (opt >> 14) & 1);
        set.set_int("MultiMapping", (opt >> 8) & 1);
        fe.apply_settings();
        launched_ = true;
        client::launch_network_battle(fe, live_->cfg, client::NetLaunch{std::move(live_->transport), live_->session, live_->room, live_->crt_rand});
    }
    // MAP / MAPNAME: the host chooses the map (0x444EA0), a joining player views the host's
    // (0x444A20, VIEWMAP).
    void open_map(FrontEnd& fe) {
        if (host_) {
            // 0x444EA0: no map name in the map info (+0xA14 empty, 0x435C40) or no map to list
            // (0x434BF0 counts none): a 320-wide box (OK, fitted, translated) instead.
            if (st_.map.empty() || art_.skirmish_maps().empty()) {
                show_net_message(fe, art_, translate("There are no multiplayer maps to choose from"), 320);
                return;
            }
            auto live = live_;
            MenuArt* art = &art_;
            fe.push(make_room_map_choice(art_, st_.map, [live, art](const std::string& map) {
                // 0x444CB0: the host's records take the map and its checksum; the others count
                // as not ready until their next records.
                live->room->set_map(map, map_sync_checksum(art->vfs(), map));
                live->room->unready_others();
            }));
        } else {
            auto live = live_;
            fe.push(make_room_map_view(art_, [live]() { return live->room->map(); }));
        }
    }
    void open_restrictions(FrontEnd& fe);

    std::shared_ptr<NetPart> part_;
    bool host_ = false, test_ = false;
    LoungeState st_;
    std::shared_ptr<LiveRoom> live_;
    bool launched_ = false;
    std::size_t rows_base_ = 0;
    int built_local_ = -1;
    std::string shown_map_;
    std::map<std::int32_t, std::uint32_t> computer_since_;  // by player id (tests: by slot): when the computer player came
    const std::vector<render::Sprite8>* logos_ = nullptr;
    const std::vector<render::Sprite8>* start_art_ = nullptr;
    std::uint32_t clock_ = 0;
    std::vector<render::Sprite8> lit_frames_, lit_player_;
    bool map_red_ = false;
    std::array<bool, 10> mem_red_{};
};

void Lounge::open_restrictions(FrontEnd& fe) {
    // 0x44C7E0 over the room: the counts of the room's unit definition exchange.
    const client::ScreenLayout* l = restrict_layout(art_, net_layout(art_, "LOUNGE2", "battleroom", true));
    if (!l) return;
    RestrictHooks h;
    if (live_) {
        auto live = live_;
        h.count = [live](std::uint32_t key) { return live->room->unit_limit(key); };
        // (A row may be changed once the definition was compared with every peer: its verdict.)
        h.enabled = [live](std::uint32_t key) { return live->room->unit_confirmed(key); };
        h.set_count = [live](std::uint32_t key, std::int32_t c) { live->room->set_unit_limit(key, c); };
        h.set_enabled = [live](std::uint32_t key, bool on) { live->room->set_unit_enabled(key, on); };
    } else {
        // Tests: a room without an exchange: no definition compared yet (every row locked).
        h.enabled = [](std::uint32_t) { return false; };
    }
    auto r = std::make_unique<Restrictions>(art_, l, palette(), host_, h);
    // The list files: SAVEGAME in the game folder (the original's working folder).
    r->list_dir_ = part_->cfg.save_dir.empty() ? art_.game_dir() + "\\SAVEGAME" : part_->cfg.save_dir;
    fe.push(std::move(r));
}

void write_lounge(const std::string& path, const LoungeState& st) {
    std::ofstream o(path);
    o << "local " << st.local << "\n";
    if (st.clock) o << "clock " << st.clock << "\n";
    if (!st.map.empty()) o << "map " << st.map << "\n";
    o << "players " << st.session_players << "\n";
    o << "resources " << st.energy << " " << st.metal << " " << st.unit_limit << "\n";
    for (int s = 0; s < 10; ++s) {
        const LoungeSlot& q = st.slots[static_cast<std::size_t>(s)];
        if (q.used || q.controller == 4) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "player %d %d %d %d %d 0x%X 0x%X %u 0x%X %u %d %s\n", s, q.controller, q.kind, q.side, q.logo, q.flags, q.options,
                          q.memory, q.cd, q.ping, q.team, q.name.c_str());
            o << buf;
        }
        if (q.used && (q.screen_w || q.screen_h)) o << "res " << s << " " << q.screen_w << " " << q.screen_h << "\n";
        if (q.synching) o << "synching " << s << "\n";
        for (int k = 0; k < 10; ++k) {
            if (q.allied[static_cast<std::size_t>(k)]) o << "allied " << s << " " << k << " " << int(q.allied[static_cast<std::size_t>(k)]) << "\n";
            if (q.allied_by[static_cast<std::size_t>(k)]) o << "alliedby " << s << " " << k << " " << int(q.allied_by[static_cast<std::size_t>(k)]) << "\n";
        }
    }
    for (int k = st.chat.tail; k != st.chat.head; k = (k + 1) % 30) o << "chat " << st.chat.text[static_cast<std::size_t>(k)] << "\n";
}

}  // namespace

namespace net_screens_detail {
std::unique_ptr<client::Screen> make_lounge(std::shared_ptr<void> p, bool host, const std::string& room_file) {
    auto part = std::static_pointer_cast<NetPart>(p);
    if (!room_file.empty()) {
        LoungeState st = read_lounge(room_file);
        const bool h = (st.slots[static_cast<std::size_t>(st.local)].flags & 1) != 0;
        return std::make_unique<Lounge>(part, h, std::move(st), true);
    }
    LoungeState st;
    st.slots[0].used = true;
    st.slots[0].controller = 1;
    st.slots[0].kind = 1;
    st.slots[0].flags = host ? 1 : 0;
    st.slots[0].name = part->nickname;
    return std::make_unique<Lounge>(part, host, std::move(st), false);
}

std::unique_ptr<client::Screen> make_broken_lounge(std::shared_ptr<void> p) {
    // (enter_broken_room) Record 0 — this machine's (GS+0x2A42) — not in use with controller 0
    // (set back by 0x451220), the host flag kept; the session's player count 0 (0x451540).
    auto part = std::static_pointer_cast<NetPart>(p);
    LoungeState st;
    st.slots[0].used = false;
    st.slots[0].controller = 0;
    st.slots[0].flags = 1;
    st.session_players = 0;
    return std::make_unique<Lounge>(part, true, std::move(st), false);
}

std::unique_ptr<client::Screen> make_live_lounge(FrontEnd& fe, std::shared_ptr<void> p, bool host, std::unique_ptr<net::Transport> transport,
                                                 std::int32_t id) {
    auto part = std::static_pointer_cast<NetPart>(p);
    auto live = std::make_shared<LiveRoom>();
    live->cfg = part->cfg;
    live->cfg.player_name = part->nickname;
    live->transport = std::move(transport);
    live->session = std::make_shared<net::NetSession>(*live->transport);
    live->room = std::make_shared<net::BattleRoom>(*live->session, host, id, part->nickname);
    live->crt_rand = std::make_shared<sim::Lcg15>();
    // This machine's C runtime rand(): the host's start position shuffle draws it (an input).
    live->crt_rand->set_state(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    // The start-up switches: -p packets a second (0x4619E0), -e the dropped share (it draws rand()).
    if (part->cfg.packet_rate_set) {
        net::NetTiming t = net::NetTiming::from_rate(part->cfg.packet_rate);
        t.compression = live->session->sender.timing().compression;
        live->session->sender.set_timing(t);
    }
    live->session->sender.set_send_error(part->cfg.send_error);
    {
        std::shared_ptr<sim::Lcg15> r = live->crt_rand;
        live->session->sender.set_rand([r]() { return static_cast<std::int32_t>(r->next()); });
    }
    live->text_lines = fe.settings().get_int("textlines", 10);
    if (const char* d = std::getenv("TA_ROOM_DUMP")) live->dump = d;
    {  // the sides the side buttons step through (gamedata\sidedata.tdf)
        std::vector<std::uint8_t> sd;
        int n = 0;
        if (part->art.vfs().read("gamedata\\sidedata.tdf", sd))
            if (auto root = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size())).root)
                while (root->child("SIDE" + std::to_string(n))) ++n;
        live->sides = n > 0 ? n : 2;
    }
    net::BattleRoom& room = *live->room;
    MenuArt* art = &part->art;
    room.map_checksum = [art](const std::string& map) { return map_sync_checksum(art->vfs(), map); };
    if (live->cfg.unit_checks) room.set_unit_checks(live->cfg.unit_checks());
    room.defer_loading_handshake(true);  // the handshake runs once this machine's data is loaded
    {  // this machine's own definition entries (0x46D040): "wacky" ones at a count of 0
        std::map<std::uint32_t, std::int32_t> counts;
        for (const RestrictEntry& e : restrict_entries(part->art))
            if (e.wacky) counts[e.key] = 0;
        room.set_unit_limits(std::move(counts));
    }
    // This machine's record (0x449BB0): its screen size, the CD.
    room.set_screen(static_cast<std::uint16_t>(fe.settings().get_int("DisplaymodeWidth", 640)),
                    static_cast<std::uint16_t>(fe.settings().get_int("DisplaymodeHeight", 480)));
    const int limit = static_cast<std::uint16_t>(fe.unit_limit());  // GS+0x37EEC (0x449C3B)
    if (host) {
        room.set_game_name(part->game_name);
        room.set_map_ready(true);
        // The room opens on the first map of the multiplayer list (0x435D30); the host's sliders
        // at 1000 metal and energy and the settings' unit limit; its switches from the game's
        // multiplayer settings (0x449BB0: bit 0 of MultiMapping, MultiLineOfSight, MultiLOSType,
        // MultiCommanderDeath & 3 as the commander mode; fixed locations from SkirmishLocation).
        const std::string map = first_network_map(part->art);
        room.set_map(map, map_sync_checksum(part->art.vfs(), map));
        room.set_resources(10, 10, static_cast<std::uint16_t>(limit));
        const client::Settings& s = fe.settings();
        std::uint16_t opt = static_cast<std::uint16_t>(room.players()[0].options);
        opt = static_cast<std::uint16_t>(opt & ~0x5F00u);
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("MultiMapping", 1) & 1) << 8));
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("MultiLineOfSight", 1) & 1) << 9));
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("MultiLOSType", 1) & 1) << 10));
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("SkirmishLocation", 1) & 1) << 14));
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("MultiCommanderDeath", 1) & 3) << 11));
        room.set_option_word(opt);
    }
    LoungeState st = LoungeState{};
    st.slots[0].used = true;
    st.slots[0].controller = 1;
    st.slots[0].kind = 1;
    st.slots[0].flags = host ? 1 : 0;
    st.slots[0].name = part->nickname;
    st.unit_limit = static_cast<std::uint16_t>(limit);
    st.map = room.map();
    live->unit_limit = limit;
    return std::make_unique<Lounge>(part, host, std::move(st), false, live);
}
}  // namespace net_screens_detail

}  // namespace ta

#endif
