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
    // The edit field taking the keys (its index, -1 none).
    void focus_edit(const std::string& name) { edit_focus_ = index_of(name); }

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
        // List boxes and edit fields take no focus ring (the edit field's focus is its lit text).
        if (lists_.count(focus_gadget_) || edits_.count(focus_gadget_)) focus_gadget_ = -1;
        LayoutScreen::draw(fe, out);
    }
    std::string focus_name_;  // a gadget the screen gives the focus when it opens
    // No picture behind the screen's gadgets (a dialog over the stock panel): lists and edit
    // fields without attribute 1 get the stock "Listbox" box 3 pixels around them (0x4B0230).
    bool boxed_ = false;

    void draw_surface(FrontEnd& fe, render::Image8& out, const Widget& w) override {
        const int i = static_cast<int>(&w - widgets_.data());
        if (auto it = lists_.find(i); it != lists_.end()) draw_list(fe, out, w, it->second);
        else if (auto e = edits_.find(i); e != edits_.end()) draw_edit(fe, out, w, e->second, i == edit_focus_);
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
                edit_focus_ = i;
                return;
            }
        }
        if (edit_focus_ >= 0 && (e.type == T::Text || e.type == T::KeyDown)) {
            EditState& E = edits_[edit_focus_];
            const Widget& w = widgets_[static_cast<std::size_t>(edit_focus_)];
            if (e.type == T::Text) saw_text_ = true;
            int k = e.key;
            const bool character = (e.type == T::Text) || (e.type == T::KeyDown && !saw_text_ && k >= 0x20 && k < 0x7F);
            if (e.type == T::KeyDown && !saw_text_ && e.shift && k >= 'a' && k <= 'z') k = std::toupper(k);
            if (character) {
                // In when shorter than the limit and its width fits the field's width - 4 (0x4AB720).
                const client::Font* f = font();
                const std::string c(1, static_cast<char>(k));
                if (static_cast<int>(E.text.size()) < E.max_chars && (!f || f->text_width(E.text) + f->text_width(c) <= w.w - 4) &&
                    (!(w.attribs & 2) || std::isalnum(k) || k == '_' || k == ' ' || k == '\'')) {
                    E.text.insert(static_cast<std::size_t>(E.point), c);
                    ++E.point;
                }
                return;
            }
            if (e.type == T::KeyDown) {
                if (k == keys::kBackspace) {
                    if (E.point > 0) E.text.erase(static_cast<std::size_t>(--E.point), 1);
                    return;
                }
                if (k == keys::kDelete) {
                    if (E.point < static_cast<int>(E.text.size())) E.text.erase(static_cast<std::size_t>(E.point), 1);
                    return;
                }
                if (k == keys::kHome) {
                    E.point = 0;
                    return;
                }
                if (k == keys::kEnd) {
                    E.point = static_cast<int>(E.text.size());
                    return;
                }
                if (k == keys::kLeft) {
                    E.point = std::max(0, E.point - 1);
                    return;
                }
                if (k == keys::kRight) {
                    E.point = std::min(static_cast<int>(E.text.size()), E.point + 1);
                    return;
                }
                if (k == keys::kEnter) {
                    presses_.push_back(widgets_[static_cast<std::size_t>(edit_focus_)].name);
                    on_enter(fe, edit_focus_);
                    return;
                }
            }
        }
        if (e.type == T::Text) return;
        LayoutScreen::handle(fe, e);
    }

public:
    // Tests: the edit fields' state in place of the layout screen's text fields.
    std::vector<std::pair<std::string, std::string>> describe() const override {
        auto d = LayoutScreen::describe();
        for (auto& [k, v] : d)
            if (k == "field") v = edit_focus_ >= 0 ? widgets_[static_cast<std::size_t>(edit_focus_)].name : std::string("-");
        for (const auto& [i, E] : edits_) d.push_back({"text:" + widgets_[static_cast<std::size_t>(i)].name, E.text});
        return d;
    }

protected:
    MenuArt& art_;
    std::map<int, ListState> lists_;
    std::map<int, EditState> edits_;
    std::map<int, int> kinds_;
    int edit_focus_ = -1;
    bool saw_text_ = false;
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
        focus_edit("NUMBER");
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
    client::NetMenuConfig cfg = part->cfg;
    cfg.player_name = part->nickname;
    fe.push(client::make_room_screen(cfg, std::move(part->session), host, id, part->game_name));
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
        edit_focus_ = index_of("NICKNAME");
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

// The room screen. The rows are made from the description's templates (0x4455B0): each template
// (PLAYERx, READYx, LOGOx, SIDEx, PINGx, MEMx, RESx, ALLYx, CDx, TEAMICONSx) is copied for rows 0-9,
// named with the row's digit, 20 pixels a row down; the local player's PLAYER button becomes a
// label (2 pixels lower, left-aligned) and so do the other rows' RES buttons (centred).
class Lounge : public NetScreen {
public:
    Lounge(std::shared_ptr<NetPart> part, bool host, LoungeState st, bool test)
        : NetScreen(part->art, net_layout(part->art, "LOUNGE2", "battleroom", true)), part_(part), host_(host), test_(test), st_(std::move(st)) {
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
        set_sliders();
        // A joining machine's room opens on the first map of its list (0x449BB0); unless the host
        // plays that map, the first making of the rows finds the host's map missing (0x448C70):
        // "<name> does not have this map" on the chat (also sent to the others) and the ready
        // bit off. The name is the session's player name (tests: none, as gui_harness has no
        // session).
        if (!host_ && !st_.map.empty() && upper(st_.map) != upper(first_network_map(art_))) {
            const std::string line = "<" + (test ? std::string() : part_->nickname) + "> does not have this map";
            st_.chat.add(line, text_lines_);
            st_.slots[static_cast<std::size_t>(st_.local)].options &= static_cast<std::uint16_t>(~0x20u);
            join_line_ = line;
        }
        refresh();
    }
    std::string join_line_;  // the line above, sent to the others once the room runs
    int text_lines_ = 10;
    const render::Palette* palette() const override { return LayoutScreen::palette(); }

protected:
    static constexpr const char* kTemplates[10] = {"PLAYERx", "READYx", "LOGOx", "SIDEx", "PINGx", "MEMx", "RESx", "ALLYx", "CDx", "TEAMICONSx"};

    void build_rows() {
        const int local = st_.local;
        const std::size_t base = widgets_.size();
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
        (void)base;
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
    int unit_limit_setting_ = 250;

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
            if (!host_) st->active = true;
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

    void on_enter(FrontEnd& fe, int) override { send_line(fe); }
    void send_line(FrontEnd&) {
        EditState* e = edit("MESSAGE");
        if (!e || e->text.empty()) return;
        // 0x463E50: "<name> text" on the console, sent to the others.
        const LoungeSlot& me = st_.slots[static_cast<std::size_t>(st_.local)];
        const std::string line = "<" + me.name + "> " + e->text;
        st_.chat.add(line, 10);
        e->text.clear();
        e->point = 0;
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        const std::string n = upper(w.name);
        if (n == "PREVMENU") {
            sfx(fe, art_, "Previous");
            fe.pop();
            return;
        }
        if (n == "START") {
            sfx(fe, art_, "BigButton");
            return;
        }
        sfx(fe, art_, "Multi");
    }

    std::shared_ptr<NetPart> part_;
    bool host_ = false, test_ = false;
    LoungeState st_;
    const std::vector<render::Sprite8>* logos_ = nullptr;
    const std::vector<render::Sprite8>* start_art_ = nullptr;
    std::uint32_t clock_ = 0;
    std::vector<render::Sprite8> lit_frames_, lit_player_;
    bool map_red_ = false;
    std::array<bool, 10> mem_red_{};
};

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
}  // namespace net_screens_detail

}  // namespace ta

#endif
