// The original game's multiplayer screens on the client's front end (spec: docs/RE_MEMORY.md,
// "Client", "Multiplayer screens"; the network room: "Network and sync").

#include "ta/client_net_screens.h"

#ifdef TA_CLIENT_HAVE_NET

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "ta/net_setup.h"
#include "ta/tdf.h"

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
// room, whose picture's palette goes on the display.
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
    const render::Palette& p = L->palette ? *L->palette : art.content().palette;
    for (int k = 0; k < 256; ++k) L->colours[static_cast<std::size_t>(k)] = art.gui_colour(p, k);
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
        else if (auto e = edits_.find(i); e != edits_.end()) draw_edit(fe, out, w, e->second, i == focus_);
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
    void draw_list(FrontEnd& fe, render::Image8& out, const Widget& w, const ListState& L) {
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
            left -= rh;
            if (left < f->height) break;
        }
    }

    // 0x4A4D70: attribs 1 fills the field with interface colour 0 (else the picture under it
    // shows); the text from (x, y + 3) along the line within w - 1, through light row 30 while the
    // field has the focus; the insertion point a line in interface colour 9 from y + 3 down one font
    // height, at the width of the text before it.
    void draw_edit(FrontEnd& fe, render::Image8& out, const Widget& w, const EditState& e, bool focused) {
        const client::Font* f = font();
        const render::ClipRect clip = screen_clip(out);
        const int x = ox_ + w.x, y = oy_ + w.y;
        if (w.attribs & 1) client::fill_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, colour(0));
        else if (boxed_) tile9(out, clip, art_.sequence("anims\\commongui.GAF", "Listbox"), x - 3, y - 3, w.w + 6, w.h + 6);
        if (!f) return;
        line_text(out, clip, *f, e.text, x, y + 3, w.w - 1, focused ? 30 : 0, fe.menu_content().light);
        if (focused) {
            const int cx = x + f->text_width(e.text.substr(0, static_cast<std::size_t>(std::clamp(e.point, 0, static_cast<int>(e.text.size())))));
            client::fill_rect(out, clip, cx, y + 3, cx, y + 3 + f->height, colour(9));
        }
    }

    void handle(FrontEnd& fe, const InputEvent& e) override {
        using T = InputEvent::Type;
        if (e.type == T::MouseDown) {
            for (auto& [i, L] : lists_) {
                const Widget& w = widgets_[static_cast<std::size_t>(i)];
                if (!w.active || e.x < ox_ + w.x || e.y < oy_ + w.y || e.x >= ox_ + w.x + w.w || e.y >= oy_ + w.y + w.h) continue;
                // A pressed list holds the focus; no field takes the keys.
                focus_gadget_ = i;
                focus_ = -1;
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
// stock panel.
class NetMessage : public LayoutScreen {
public:
    NetMessage(MenuArt& art, const std::string& text, int width, bool fit, std::function<void(FrontEnd&)> done = {})
        : LayoutScreen(art.layout("MSGBOX")), art_(art), done_(std::move(done)) {
        // Measured in the interface's caption font (the labels then draw in the small one).
        const client::Font* f = art.content().font;
        // Word wrap: at the end of a word, when the line is at least as wide as the limit, the line
        // breaks at its last space or hyphen.
        std::string line;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] != ' ' && text[i] != '\n') {
                line += text[i];
                continue;
            }
            const bool nl = i < text.size() && text[i] == '\n';
            if (f && f->text_width(line) >= width) {
                const auto at = line.find_last_of(" -", line.size() - 1);
                if (at != std::string::npos && at > 0) {
                    lines_.push_back(line.substr(0, line[at] == '-' ? at + 1 : at));
                    line = line.substr(at + 1);
                }
            }
            if (nl || i == text.size()) {
                lines_.push_back(line);
                line.clear();
            } else {
                line += ' ';
            }
        }
        while (lines_.size() > 1 && lines_.back().empty()) lines_.pop_back();
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
            }
        }
        line_height_ = f ? f->height : 12;
    }
    bool see_through() const override { return true; }

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

private:
    MenuArt& art_;
    std::vector<std::string> lines_;
    int line_height_ = 12;
    std::function<void(FrontEnd&)> done_;
};

void show_net_message(FrontEnd& fe, MenuArt& art, const std::string& text, int width = 200, std::function<void(FrontEnd&)> done = {}) {
    if (art.layout("MSGBOX")) fe.push(std::make_unique<NetMessage>(art, text, width, true, std::move(done)));
    else std::printf("message: %s\n", text.c_str());
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
    bool loopback = false;             // TCP/IP on this machine only (setting NetLoopback; tests)
    std::string address;      // the host's address (empty: this machine)
    std::string nickname;     // the player's name (the original's GS+0x2BD2)
    std::string password;     // GS+0x2BE3
    std::string game_name;    // GS+0x2BC1 (the new game's name)
    bool test = false;        // tests: no network
    NetPart(MenuArt& a, client::NetMenuConfig c) : art(a), cfg(std::move(c)) {}
};

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
    }

protected:
    void on_list(FrontEnd& fe, int, int row, bool dbl) override {
        if (dbl) choose(fe, row);
    }
    void on_press(FrontEnd& fe, Widget& w) override;
    void choose(FrontEnd& fe, int row);
    std::shared_ptr<NetPart> part_;
};

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
        if (part.loopback) o.bind_ip = net::dplay::kLoopback;
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
    part.session = std::make_unique<net::dplay::Session>(std::move(p));
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
    c[2] = m;
    c[3] = g.major < 3 ? "VER!" : (g.options & 0x8000) ? "Lock" : (g.options & 0x10) ? "Play" : "Open";
    c[4] = std::to_string(g.memory);
    c[5] = std::to_string(g.metal * 100);
    c[6] = std::to_string(g.energy * 100);
    c[7] = std::to_string(g.ping);
    const int cm = g.options & 0x1800;
    c[8] = cm == 0 ? "No" : cm == 0x800 ? "Yes" : "DM";
    c[9] = (g.options & 0x100) ? "Blk" : "Gray";
    c[10] = (g.options & 0x200) ? "No" : "Yes";
    return c;
}

class NewMulti;
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

// The player's data in the session (0x451220): the password typed, then the words 0 and 0x50.
std::vector<std::uint8_t> session_player_data(const std::string& password) {
    std::vector<std::uint8_t> d(21, 0);
    std::memcpy(d.data(), password.data(), std::min<std::size_t>(password.size(), 16));
    d[0x13] = 0x50;
    return d;
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
    if (id <= 0) {
        show_net_message(fe, part->art, "Direct Play failed to add new player");
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
    if (!part_->test && open_provider(*part_, err)) find_games(*part_);
    fe.push(make_selgame(part_, found_entries(*part_)));
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
        show_net_message(fe, art_, err);
        return;
    }
    if (host) {
        fe.push(std::make_unique<NewMulti>(part_));
        return;
    }
    if (!part_->test) find_games(*part_);
    fe.push(make_selgame(part_, found_entries(*part_)));
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
        show_net_message(fe, art_, err);
        return;
    }
    if (part_->answer) {
        fe.push(std::make_unique<NewMulti>(part_));
        return;
    }
    if (!part_->test) find_games(*part_);
    fe.push(make_selgame(part_, found_entries(*part_)));
}

void SelGame::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "UPDATE")) {
        sfx(fe, art_, "Multi");
        if (!part_->test && part_->session) {
            find_games(*part_);
            games_ = found_entries(*part_);
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

void SelGame::join(FrontEnd& fe, bool watch) {
    ListState* L = list("GAMENAME");
    const int row = L ? L->selected : -1;
    if (row < 0 || row >= static_cast<int>(games_.size())) return;
    const GameEntry& g = games_[static_cast<std::size_t>(row)];
    if (g.major != 3) {
        show_net_message(fe, art_, "You do not have a compatible version of Total Annihilation to join this game.");
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
        show_net_message(fe, art_, "You must enter your name");
        return;
    }
    if (const EditState* p = edit("PASSWORD")) part_->password = p->text;
    sfx(fe, art_, watch ? "Multi" : "BigButton");
    fe.settings().set_text("Nickname", part_->nickname);
    if (!part_->test && part_->session && row < static_cast<int>(part_->found.size())) {
        // 0x4517B0: open the session (join), then this machine's player.
        net::dplay::Session& s = *part_->session;
        s.join(part_->found[static_cast<std::size_t>(row)], part_->password);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (s.state() == net::dplay::Session::State::Joining && std::chrono::steady_clock::now() < until) {
            s.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (s.state() != net::dplay::Session::State::Joined) {
            show_net_message(fe, art_, s.result() == net::dplay::kErrInvalidPassword ? "Incorrect password" : "Unable to join the game");
            return;
        }
    }
    enter_room(fe, part_, false);
}

void NewMulti::next(FrontEnd& fe) {
    sfx(fe, art_, "BigButton");
    if (const EditState* e = edit("GAMENAME")) part_->game_name = e->text;
    if (const EditState* e = edit("NICKNAME")) part_->nickname = e->text;
    if (const EditState* e = edit("PASSWORD")) part_->password = e->text;
    fe.settings().set_text("Nickname", part_->nickname);
    fe.pop();
    if (!part_->test) {
        // 0x451540: the session is opened as its host (TCP/IP: a fresh one on every address).
        std::string err;
        if (!part_->session) {
            part_->address.clear();
            if (!open_provider(*part_, err)) {
                show_net_message(fe, art_, err);
                return;
            }
        }
        net::dplay::HostSettings hs;
        hs.session_name = part_->game_name;
        hs.password = part_->password;
        if (!part_->session->host(hs, err)) {
            show_net_message(fe, art_, err);
            part_->session.reset();
            return;
        }
    }
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
                const auto c2 = arg.find(':');
                show_net_message(fe, part_->art, c2 == std::string::npos ? arg : arg.substr(c2 + 1), c2 == std::string::npos ? 200 : std::atoi(arg.c_str()));
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
        part->loopback = fe.settings().get_int("NetLoopback", 0) != 0;
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
        first = name;
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
std::string out_text(int reason) {
    switch (reason) {
    case 3: return "The game is closed";
    case 4: return "You did not have the correct password";
    case 5: return "The game is full";
    case 6: return "You have lost connection with the host";
    case 7: return "You need a unit you don't have for this game";
    case 8: return "You need a newer version of the game to join";
    case 9: return "No watching is allowed for this game";
    case 10: return "The creator has left the game";
    default: return "You were rejected from the game";
    }
}

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
            const std::string line = "<" + (test ? std::string() : part_->nickname) + "> does not have this map";
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
        for (int row = 0; row < 10; ++row)
            for (int t = 0; t < 10; ++t) {
                const int ti = index_of(kTemplates[t]);
                if (ti < 0) continue;
                Widget w = widgets_[static_cast<std::size_t>(ti)];
                w.name = std::string(kTemplates[t]).substr(0, std::strlen(kTemplates[t]) - 1) + std::to_string(row);
                w.y += 20 * row;
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
                m->text = "NOT SELECTED";
                map_red_ = true;
            } else {
                m->text = st_.map;
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
                    std::string t = p.controller == 4 ? "[BLOCKED]" : "UNUSED";
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
                    if (res->text != t) set_caption(*res, t);
                } else {
                    res->text = t;
                }
                if (p.controller != 1) res->active = true;
                else res->grayed = me_ready;
            }
            if (ping) {
                if (!(p.controller == 3 && p.kind == 1)) ping->text = "n/a";
                else ping->text = std::to_string(p.ping) + (host_ && p.ping_mark ? ":s" : "");
                ping->active = true;
            }
            if (mem) {
                mem->text = human_here ? std::to_string(p.memory) : std::string("n/a");  // (the same stale word)
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
            map_size_ = static_cast<std::int64_t>(art_.vfs().size_or_zero("maps\\" + st_.map + ".tnt"));
        }
        const std::int64_t s = map_size_;
        return s < 0x3E6666 ? 16 : s < 0x600000 ? 24 : s < 0x800000 ? 32 : s < 0xA00000 ? 48 : s < 0xC00000 ? 64 : 128;
    }
    std::string size_map_;
    std::int64_t map_size_ = 0;
    void caption_fit(Widget& w, const std::string& t) {
        // 0x4A5D50: characters drop from the end until the caption fits the width - 6.
        std::string c = t;
        const client::Font* f = (w.attribs & 0x8000) || w.kind == WidgetKind::Label ? art_.content().small_font : font();
        while (f && !c.empty() && f->text_width(c) > w.w - 6) c.pop_back();
        if (w.kind == WidgetKind::Label) w.text = c;
        else if (w.text != c) set_caption(w, c);
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
    }
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
            const bool mine = own || (room.is_host() && p.kind == 2);
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
            st.map = ps[static_cast<std::size_t>(local)].map;
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
            if (reason != 2) show_net_message(fe, art, out_text(reason), 320);
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
                    const std::string line = client::chat_line(part_->nickname, std::string(), "does not have this map");
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
        write_lounge(live_->dump + name + ".txt", st_);
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
    int dump_n_ = 0;

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
            if (n == "START") {
                sfx(fe, art_, "BigButton");
                return;
            }
            sfx(fe, art_, "Multi");
            return;
        }
        live_press(fe, w, n);
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
                if (clock_ - computer_since_[static_cast<std::size_t>(s)] >= 0x1F) room.remove_computer(s, 1);
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
                show_net_message(fe, art_, "Can't add another player when game is closed.", 500);
                return;
            }
            int computers = 0;
            for (const LoungeSlot& c : st_.slots) computers += c.used && (c.controller == 2 || (c.controller == 3 && c.kind == 2));
            if ((opt & 0x1800) != 0x1000 && computers == 0 && host_) {
                // 0x451220: a computer player in this slot, the first logo nobody plays.
                if (room.add_computer_at(s, 0) >= 0) computer_since_[static_cast<std::size_t>(s)] = clock_;
            }
            // (A joining machine's computer player: not built.)
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
            // The local player's alliance with that player (0x23); leaving them allied by team
            // leaves the team (0x446E90, team 5, 0x24). The sound and a line for everyone.
            const bool now = me.allied[static_cast<std::size_t>(s)] == 0;
            room.set_allied(s, now);
            if (me.team != 5 && me.team == st_.slots[static_cast<std::size_t>(s)].team) leave_team(local);
            sfx(fe, art_, now ? "Ally" : "Multi");
            say_line(std::string(" ") + (now ? "allied with" : "broke alliance with") + " " + st_.slots[static_cast<std::size_t>(s)].name);
            return;
        }
        if (int s = row_of(n, "TEAMICONS"); s >= 0) {
            sfx(fe, art_, "Ally");
            const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
            const int team = (q.team + 1) % 6;
            leave_team(s);
            room.set_ally_team_of(s, static_cast<std::uint8_t>(team));
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
    void leave_team(int s) {
        const LoungeSlot& q = st_.slots[static_cast<std::size_t>(s)];
        if (q.team == 5 || s != st_.local) return;
        for (int k = 0; k < 10; ++k)
            if (k != s && st_.slots[static_cast<std::size_t>(k)].playing() && st_.slots[static_cast<std::size_t>(k)].team == q.team)
                live_->room->set_allied(k, false);
    }
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
            show_net_message(fe, art_, "There are not enough game CDs present to play", 200);
            return;
        }
        for (int t = 0; t < 5; ++t) {
            int n = 0;
            for (const LoungeSlot& q : st_.slots) n += q.playing() && q.team == t;
            if (n == humans + computers) {
                show_net_message(fe, art_, "Can not start game with all players on the same team.", 200);
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
        launched_ = true;
        client::launch_network_battle(fe, live_->cfg, client::NetLaunch{std::move(live_->transport), live_->session, live_->room, live_->crt_rand});
    }
    // MAP / MAPNAME: the host chooses the map (0x444EA0), a joining player views the host's
    // (0x444A20, VIEWMAP).
    void open_map(FrontEnd& fe) {
        if (host_) {
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
    std::array<std::uint32_t, 10> computer_since_{};
    const std::vector<render::Sprite8>* logos_ = nullptr;
    const std::vector<render::Sprite8>* start_art_ = nullptr;
    std::uint32_t clock_ = 0;
    std::vector<render::Sprite8> lit_frames_, lit_player_;
    bool map_red_ = false;
    std::array<bool, 10> mem_red_{};
};

void Lounge::open_restrictions(FrontEnd& fe) {
    (void)fe;  // (built in its own part below)
}

void write_lounge(const std::string& path, const LoungeState& st) {
    std::ofstream o(path);
    o << "local " << st.local << "\n";
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
    // This machine's record (0x449BB0): its screen size, the CD.
    room.set_screen(static_cast<std::uint16_t>(fe.settings().get_int("DisplaymodeWidth", 640)),
                    static_cast<std::uint16_t>(fe.settings().get_int("DisplaymodeHeight", 480)));
    const int limit = std::clamp(fe.settings().get_int("UnitLimit", 250), 20, 500);
    if (host) {
        room.set_game_name(part->game_name);
        room.set_map_ready(true);
        // The room opens on the first map of the multiplayer list (0x435D30); the host's sliders
        // at 1000 metal and energy and the settings' unit limit; its switches from the game's
        // settings (mapping, line of sight and its type, commander, fixed locations).
        const std::string map = first_network_map(part->art);
        room.set_map(map, map_sync_checksum(part->art.vfs(), map));
        room.set_resources(10, 10, static_cast<std::uint16_t>(limit));
        const client::Settings& s = fe.settings();
        std::uint16_t opt = static_cast<std::uint16_t>(room.players()[0].options);
        opt = static_cast<std::uint16_t>(opt & ~0x5F00u);
        if (s.get_int("SkirmishMapping", 1)) opt |= 0x100;
        if (s.get_int("SkirmishLineOfSight", 1)) opt |= 0x200;
        if (s.get_int("SkirmishLOSType", 1) == 1) opt |= 0x400;
        opt = static_cast<std::uint16_t>(opt | ((s.get_int("SkirmishCommanderDeath", 1) ? 1u : 0u) << 11));
        if (s.get_int("SkirmishLocation", 1)) opt |= 0x4000;
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
