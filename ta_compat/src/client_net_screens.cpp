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

#include "client/canvas.h"
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
                    on_enter(fe, edit_focus_);
                    return;
                }
            }
        }
        if (e.type == T::Text) return;
        LayoutScreen::handle(fe, e);
    }

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
// The engine's transports, listed as the original lists its session service's providers.
struct Provider {
    std::string name;
    bool udp = true;
};
const std::vector<Provider>& providers() {
    static const std::vector<Provider> p = {{"Internet TCP/IP Connection (UDP)", true}, {"Internet TCP/IP Connection (TCP)", false}};
    return p;
}

// What the screens share: the configuration, the chosen transport, the address and names.
struct NetPart {
    MenuArt& art;
    client::NetMenuConfig cfg;
    int provider = 0;
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

void SelProv::choose(FrontEnd& fe, int row) {
    if (row < 0 || row >= static_cast<int>(providers().size())) return;
    sfx(fe, art_, "BigButton");
    part_->provider = row;
    fe.push(std::make_unique<TcpAsk>(part_));
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

void TcpAsk::ok(FrontEnd& fe) {
    sfx(fe, art_, "Smlbutton");
    const EditState* e = edit("ADDRESS");
    part_->address = e ? e->text : std::string();
    fe.settings().set_text("TCPADDR", part_->address);
    fe.pop();
    // The game list at that address (the original: the session service's games found there).
    std::vector<GameEntry> games;
    fe.push(make_selgame(part_, std::move(games)));
}

void SelGame::on_press(FrontEnd& fe, Widget& w) {
    if (is(w, "UPDATE")) {
        sfx(fe, art_, "Multi");
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
    fe.push(net_screens_detail::make_lounge(as_void(part_), false, std::string()));
}

void NewMulti::next(FrontEnd& fe) {
    sfx(fe, art_, "BigButton");
    if (const EditState* e = edit("GAMENAME")) part_->game_name = e->text;
    if (const EditState* e = edit("NICKNAME")) part_->nickname = e->text;
    if (const EditState* e = edit("PASSWORD")) part_->password = e->text;
    fe.settings().set_text("Nickname", part_->nickname);
    fe.pop();
    fe.push(net_screens_detail::make_lounge(as_void(part_), true, std::string()));
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
        auto part = std::make_shared<NetPart>(art, config);
        part->nickname = fe.settings().get_text("Nickname", "");
        return std::make_unique<SelProv>(part);
    };
}

std::unique_ptr<client::Screen> make_net_test_screens(MenuArt& art, const client::NetMenuConfig& config, const std::vector<std::string>& specs) {
    auto part = std::make_shared<NetPart>(art, config);
    part->test = true;
    return std::make_unique<TestBase>(part, specs);
}

namespace net_screens_detail {
std::unique_ptr<client::Screen> make_lounge(std::shared_ptr<void>, bool, const std::string&) { return nullptr; }
}  // namespace net_screens_detail

}  // namespace ta

#endif
