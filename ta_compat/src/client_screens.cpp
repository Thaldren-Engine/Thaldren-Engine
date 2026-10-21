// The original game's screens on the client's front end (spec: docs/RE_MEMORY.md,
// "Client", "Menus"): main menu, single player, skirmish set-up and map choice, options,
// loading, the in-game menus, the end of a battle.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "client/canvas.h"
#include "client/movie.h"
#include "render/raster.h"
#include "sim/battle.h"
#include "sim/construction.h"
#include "sim/economy.h"
#include "sim/rng.h"
#include "sim/victory.h"
#include "sim/x87.h"
#include "ta/translate.h"
#include "ta/battle_loader.h"
#include "ta/client_loading.h"
#include "ta/client_menus.h"
#include "ta/tdf.h"

namespace ta {
namespace {

using client::FrontEnd;
using client::LayoutScreen;
using client::Widget;
using client::WidgetKind;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool is(const Widget& w, const char* name) { return upper(w.name) == upper(name); }

// An interface sound of the sound bank by its section name (the screens' handlers each name
// theirs; docs/RE_MEMORY.md, "Client", "Menu sounds").
void sfx(FrontEnd& fe, MenuArt& art, const char* name) { fe.play_sound(art.sound(name)); }

// The menus' rand() stream (the main menu's sparks draw from it; what is left of it is the
// simulation thread's rand() state at the start of a battle, as in the original).
sim::Lcg15& menu_rand() {
    static sim::Lcg15 r;
    return r;
}

// ------------------------------------------------------------------ message box (MSGBOX.GUI)
void draw_back_tile(MenuArt& art, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int w, int h);
void dim_rect(FrontEnd& fe, render::Image8& out, int x0, int y0, int x1, int y1);
void draw_run_time_labels(FrontEnd& fe, render::Image8& out, const std::vector<Widget>& widgets, std::size_t first, int ox, int oy);
void add_run_time_label(std::vector<Widget>& widgets, const std::string& text, int x, int y, int width, int attribs);

// The original's message box (0x4ABD90): MSGBOX.GUI opened with flag 0x800 (the screen under it
// dimmed once) and the stock panel; the text (translated; English keeps it) word-wrapped to the
// caller's width (0x4AC4C0; 320 for most, 480 for the skirmish refusals) and cut at the line breaks into labels ("TEXT", centred across the box, from
// y 20, one row every 5 + the font's height); the box as wide as the widest line + 20 and
// lines x 25 + OK's height + 40 high, centred on the screen; OK at 15 pixels from the right and
// bottom edges, the Enter and Escape default and the focus. OK closes it (no sound).
class MessageBox : public LayoutScreen {
public:
    MessageBox(MenuArt& art, std::string text, const render::ClipRect* under, int width = 320)
        : LayoutScreen(art.layout("MSGBOX")), art_(art), text_(translate(text)), width_(width) {
        if (under) {
            under_ = *under;
            has_under_ = true;
        }
    }
    ~MessageBox() override {
        if (panel_client_) panel_client_->dim_panel(false);
    }
    bool see_through() const override { return true; }
    // The screen under it shown as it is (a surface the caller redraws every pass).
    void set_no_dim() { no_dim_ = true; }
    // Over a running battle: the 0x800 flag dims the side panel's layout (the top layout there)
    // while the box is open; the battle view is drawn anew every pass.
    void over_battle(client::GameClient& c) {
        no_dim_ = true;
        panel_client_ = &c;
        c.dim_panel(true);
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        if (!placed_) arrange(fe, out.width, out.height);
        if (has_under_ && !no_dim_) dim_rect(fe, out, under_.x0, under_.y0, under_.x1, under_.y1);
        else if (!no_dim_) dim_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        LayoutScreen::draw(fe, out);
    }

protected:
    void arrange(FrontEnd& fe, int fw, int fh) {
        // The wrapping and the widths are measured in the interface's font (the labels then draw
        // in the small one); the rows are 5 + the text routine's current font height apart.
        const client::MenuContent& mc = fe.menu_content();
        const client::Font* lf = mc.font ? mc.font : mc.small_font;
        std::vector<std::string> lines;
        if (lf) {
            // (A broken line keeps the break's '\r': the box's width (0x4A5030) and the label's
            // centring count its glyph.)
            const std::string wrapped = wrap_for_width(*lf, text_, width_, true);
            std::size_t at = 0;
            while (at <= wrapped.size()) {
                const std::size_t nl = wrapped.find('\n', at);
                const std::string part = wrapped.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                if (!part.empty()) lines.push_back(part);
                if (nl == std::string::npos) break;
                at = nl + 1;
            }
        }
        Widget* ok = widget("OK");
        int widest = 0;
        for (const std::string& l : lines) widest = std::max(widest, lf ? lf->text_width(l) : 0);
        Widget& h = widgets_.front();
        h.w = widest + 20;
        h.h = static_cast<int>(lines.size()) * 25 + (ok ? ok->h : 0) + 40;
        place(fw, fh);
        ox_ = (fw - h.w) / 2;
        oy_ = (fh - h.h) / 2;
        // (0x4C1450: the display's current font, the interface font on the menus — 14 rows on the
        // main menu, single player, skirmish and options screens, gui_harness; it was taken as
        // 13 before, which put the second line of a two-line box a row too high.)
        const int step = 5 + (lf ? lf->height : 13);
        for (std::size_t k = 0; k < lines.size(); ++k) {
            Widget t;
            t.kind = WidgetKind::Label;
            t.name = "TEXT";
            t.x = 0;
            t.y = 20 + static_cast<int>(k) * step;
            t.w = h.w;
            t.h = 15;
            t.attribs = 2;
            t.text = lines[k];
            widgets_.push_back(t);
        }
        if ((ok = widget("OK")) != nullptr) {
            ok->x = h.w - ok->w - 15;
            ok->y = h.h - ok->h - 15;
        }
    }
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) fe.pop();
    }

private:
    MenuArt& art_;
    std::string text_;
    render::ClipRect under_{};
    int width_ = 320;
    bool has_under_ = false;
    bool no_dim_ = false;
    client::GameClient* panel_client_ = nullptr;
};

}  // namespace

std::string wrap_for_width(const client::Font& f, const std::string& t, int width, bool cr) {
    // 0x4AC4C0: characters are copied up to a NUL or 0xFF; after each one, when the next is a
    // space, a line feed or a hyphen and the line so far (from its start to the copy's end) is at
    // least `width` wide, the copy goes back in step with the source to the last space or hyphen
    // in the source (past the line's start when it has none: back into the earlier lines), which
    // becomes CR LF, and the next line starts after it. A line feed in the source starts a line
    // (the width counts from after it). (A word with no space or hyphen before it in the whole
    // text would make the original read before its buffer: the copy stops at the text's start.)
    auto at = [&](std::size_t k) -> unsigned char { return k < t.size() ? static_cast<unsigned char>(t[k]) : 0; };
    std::string out;
    std::vector<std::size_t> breaks;  // where a CR was put
    std::size_t line = 0, i = 0;
    while (at(i) != 0 && at(i) != 0xFF) {
        out.push_back(static_cast<char>(at(i)));
        std::size_t n = i + 1;
        const unsigned char next = at(n);
        if ((next == ' ' || next == '\n' || next == '-') && f.text_width(std::string_view(out).substr(line)) >= width) {
            std::size_t j = i;
            while (at(j) != ' ' && at(j) != '-') {
                out.pop_back();
                if (j == 0) break;
                --j;
            }
            if (!out.empty()) {
                out.back() = '\r';
                breaks.push_back(out.size() - 1);
            }
            out.push_back('\n');
            line = out.size();
            n = j + 1;
        }
        if (at(n) == '\n') line = out.size() + 1;
        i = n;
    }
    if (!cr)
        for (auto b = breaks.rbegin(); b != breaks.rend(); ++b)
            if (*b < out.size() && out[*b] == '\r') out.erase(*b, 1);
    return out;
}

std::string leave_reason_text(int reason) {
    switch (reason) {
    case 3: return "The game is closed";
    case 4: return "You did not have the correct password";
    case 5: return "The game is full";
    case 6: return "You have lost connection with the game";
    case 7: return "You need a unit you don't have for this game";
    case 8: return "You need a newer version of the game to enter";
    case 9: return "No watching is allowed for this game";
    case 10: return "The creator has left the game";
    default: return "You were rejected from the game";
    }
}

void show_message(FrontEnd& fe, MenuArt& art, const std::string& text, const render::ClipRect* under, int width) {
    if (art.layout("MSGBOX")) fe.push(std::make_unique<MessageBox>(art, text, under, width));
    else std::printf("message: %s\n", text.c_str());
}

sim::Lcg15& menus_rand() { return menu_rand(); }

namespace {

// ------------------------------------------------------------------ options
// The screen sizes offered: 640 x 480, 800 x 600 and 1024 x 768, then 1280 x 1024 and
// 1600 x 1200 when the desktop is at least that big; ascending, nothing below 640 x 480.
std::vector<std::array<int, 2>> display_modes(FrontEnd& fe) {
    // The display's own modes when the platform lists them (the original's full-screen path):
    // sorted by width then height, duplicates and modes below 640 x 480 dropped.
    std::vector<std::array<int, 2>> listed;
    fe.display_modes(listed);
    if (!listed.empty()) {
        std::sort(listed.begin(), listed.end());
        listed.erase(std::unique(listed.begin(), listed.end()), listed.end());
        listed.erase(std::remove_if(listed.begin(), listed.end(), [](const std::array<int, 2>& m) { return m[0] < 640 || m[1] < 480; }),
                     listed.end());
        if (!listed.empty()) return listed;
    }
    // Otherwise the original's windowed list.
    int dw = 0, dh = 0;
    fe.desktop_size(dw, dh);
    std::vector<std::array<int, 2>> m = {{640, 480}, {800, 600}, {1024, 768}};
    if (dw > 0x4ff && dh > 0x3ff) m.push_back({1280, 1024});
    if (dw > 0x63f && dh > 0x4af) m.push_back({1600, 1200});
    return m;
}

// One options page (full-screen in the menus, the side panel pages in a battle). The
// widgets map to settings by name, the same on both kinds of pages.
class OptionsPage : public LayoutScreen {
public:
    OptionsPage(MenuArt& art, const std::string& gui, bool in_battle) : LayoutScreen(art.layout(gui)), art_(art), in_battle_(in_battle) {
        focus_ring_ = false;  // the page joins its menu's screen: the menu's button keeps the focus
    }
    void enter(FrontEnd& fe) override { load(fe); }
    bool see_through() const override { return in_battle_; }

protected:
    void load(FrontEnd& fe) {
        const client::Settings& s = fe.settings();
        client::MusicPlayer* m = fe.music();
        for (Widget& w : widgets_) {
            if (w.kind == WidgetKind::ScrollBar) {
                // Sliders: range and value.
                if (is(w, "FXVOL")) set(w, 64, s.get_int("fxvol", 27));
                else if (is(w, "MUSICVOL")) set(w, 64, s.get_int("musicvol", 32));
                else if (is(w, "GAME")) set(w, 21, s.get_int("gamespeed", 10));  // the original's maximum is 21
                else if (is(w, "SCREEN")) set(w, 65, s.get_int("scrollspeed", 0x20));
                else if (is(w, "TXTSCROL")) set(w, 20, s.get_int("textscroll", 10));
                else if (is(w, "MAXLINES")) set(w, 30, s.get_int("textlines", 10));
                else if (is(w, "GAMMA")) set(w, 20, s.get_int("Gamma", 12) == 10 ? 12 : s.get_int("Gamma", 12));  // (0x42F9A0 reads 10 as 12)
            } else if (w.kind == WidgetKind::Button && w.stages > 0) {
                if (is(w, "SPEECH")) w.stage = s.get_int("speechfx", 1) ? std::clamp(s.get_int("unitchat", 10) / 5, 0, 2) : 0;
                else if (is(w, "MODE")) w.stage = std::clamp(s.get_int("Sound Mode", 1), 0, 2);
                else if (is(w, "UNITCHAT")) w.stage = std::clamp(s.get_int("unitchattext", 5) / 5, 0, 2);
                else if (is(w, "LEFTCLICK")) w.stage = std::clamp(s.get_int("Interface Type", 0), 0, 1);
                else if (is(w, "NOTRAK")) w.stage = s.get_int("musicmode", 1) ? 1 : 0;
                else if (is(w, "TRACKMODE")) w.stage = std::clamp(s.get_int("cdmode", 4) - 1, 0, 3);
                else if (is(w, "TRACKTYPE") && m) w.stage = m->types()[static_cast<std::size_t>(std::clamp(track_, 0, m->track_count()))];
                else if (is(w, "SHADING")) w.stage = s.get_int("Shading", 1) ? 1 : 0;
                else if (is(w, "ANTI")) w.stage = s.get_int("Anti-Alias", 1) ? 1 : 0;
                else if (is(w, "BSHADOWS")) w.stage = s.get_int("FeatureShadows", 1) ? 1 : 0;
            }
        }
        if (m && track_ == 0) track_ = std::max(1, m->current_track());
        // Screen size: a slider over the display modes (only outside a battle).
        modes_ = display_modes(fe);
        for (Widget& w : widgets_)
            if ((upper(w.name).compare(0, 3, "VID") == 0 || upper(w.name).compare(0, 3, "MAP") == 0) && in_battle_) w.active = false;
        if (Widget* v = widget("VIDSLDR")) {
            v->range = static_cast<int>(modes_.size()) - 1;
            v->position = 0;
            for (std::size_t i = 0; i < modes_.size(); ++i)
                if (modes_[i][0] == s.get_int("DisplaymodeWidth", 640) && modes_[i][1] == s.get_int("DisplaymodeHeight", 480)) v->position = static_cast<int>(i);
        }
        labels(fe);
    }
    void labels(FrontEnd& fe) {
        if (Widget* t = widget("TRACKNUM")) t->text = track_ ? "#" + std::to_string(track_) : "NO DISC";
        // GAMETEXT stays empty (the original never fills it); MAXLINESTEXT shows the text lines.
        if (Widget* m = widget("MAXLINESTEXT")) {
            const int n = fe.settings().get_int("textlines", 10);
            m->text = n == 0 ? "None" : std::to_string(n);
        }
        // "W X H" of the current mode when the list has it; otherwise the label keeps its text.
        if (Widget* v = widget("VIDVAL")) {
            const int mw = fe.settings().get_int("DisplaymodeWidth", 640), mh = fe.settings().get_int("DisplaymodeHeight", 480);
            for (const auto& m : modes_)
                if (m[0] == mw && m[1] == mh) v->text = std::to_string(mw) + " X " + std::to_string(mh);
        }
    }
    static void set(Widget& w, int range, int v) {
        w.range = range;
        w.position = std::clamp(v, 0, range);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        client::Settings& s = fe.settings();
        client::MusicPlayer* m = fe.music();
        // Every button of the pages sounds "Options" (the sliders are silent); the sound test
        // plays its file instead, and the sound mode switch first starts the menus' loop when it
        // turns to mono outside a battle (and stops every sound when it turns off).
        if (w.kind == WidgetKind::Button && !is(w, "TEST") && !is(w, "MODE")) sfx(fe, art_, "Options");
        if (w.kind == WidgetKind::ScrollBar) {
            if (is(w, "FXVOL")) s.set_int("fxvol", w.position);
            else if (is(w, "MUSICVOL")) s.set_int("musicvol", w.position);
            else if (is(w, "GAME")) s.set_int("gamespeed", std::max(1, w.position));
            else if (is(w, "SCREEN")) s.set_int("scrollspeed", w.position);
            else if (is(w, "TXTSCROL")) s.set_int("textscroll", w.position);
            else if (is(w, "MAXLINES")) s.set_int("textlines", w.position);
            else if (is(w, "GAMMA")) s.set_int("Gamma", w.position);
            else if (is(w, "VIDSLDR") && !modes_.empty()) {
                const auto& md = modes_[static_cast<std::size_t>(std::clamp(w.position, 0, static_cast<int>(modes_.size()) - 1))];
                s.set_int("DisplaymodeWidth", md[0]);
                s.set_int("DisplaymodeHeight", md[1]);
            }
            fe.apply_settings();
            labels(fe);
            return;
        }
        if (is(w, "SPEECH")) {
            s.set_int("speechfx", w.stage != 0);
            s.set_int("unitchat", w.stage * 5);
        } else if (is(w, "MODE")) {
            s.set_int("Sound Mode", w.stage);
            fe.apply_settings();
            if (w.stage == 0) fe.stop_sounds();
            if (w.stage == 1 && !in_battle_) fe.play_sound_loop(art_.sound("BGM"));
            sfx(fe, art_, "Options");
        } else if (is(w, "UNITCHAT")) {
            s.set_int("unitchattext", w.stage * 5);
        } else if (is(w, "LEFTCLICK")) {
            s.set_int("Interface Type", w.stage);
        } else if (is(w, "NOTRAK")) {
            s.set_int("musicmode", w.stage);
        } else if (is(w, "TRACKMODE")) {
            s.set_int("cdmode", w.stage + 1);
            if (m && w.stage + 1 == 3) m->set_repeat_track(track_);
        } else if (is(w, "TRACKTYPE")) {
            if (m && track_ >= 1 && track_ <= m->track_count()) {
                m->types()[static_cast<std::size_t>(track_)] = static_cast<std::uint8_t>(w.stage);
                save_types(fe);
            }
        } else if (is(w, "SHADING")) {
            s.set_int("Shading", w.stage);
        } else if (is(w, "ANTI")) {
            s.set_int("Anti-Alias", w.stage);
        } else if (is(w, "BSHADOWS")) {
            s.set_int("FeatureShadows", w.stage);
        } else if (is(w, "CDPLAY")) {
            if (m) m->play(track_);
        } else if (is(w, "CDSTOP")) {
            if (m) m->stop();
        } else if (is(w, "CDNEXT") || is(w, "CDPREV")) {
            if (m && m->track_count() > 0) {
                track_ += is(w, "CDNEXT") ? 1 : -1;
                if (track_ > m->track_count()) track_ = 1;
                if (track_ < 1) track_ = m->track_count();
                m->play(track_);
                load(fe);
            }
        } else if (is(w, "TEST")) {
            // sounds\explode.wav as a file of its own.
            fe.play_sound_file(art_.sound("ActivateAllStatBars"));
        } else if (is(w, "RESTORE")) {
            restore_defaults(fe);
        } else if (is(w, "UNDO")) {
            fe.settings() = saved_;
            load(fe);
        }
        fe.apply_settings();
        labels(fe);
    }
    void restore_defaults(FrontEnd& fe) {
        // The original's defaults for the page's settings.
        client::Settings& s = fe.settings();
        for (const Widget& w : widgets_) {
            if (is(w, "FXVOL")) s.set_int("fxvol", 27);
            if (is(w, "MUSICVOL")) {
                s.set_int("musicvol", 32);
                s.set_int("cdmode", 4);
                s.set_int("musicmode", 1);
            }
            if (is(w, "SPEECH")) {
                s.set_int("unitchat", 10);
                s.set_int("speechfx", 1);
            }
            if (is(w, "MODE")) s.set_int("Sound Mode", 1);
            if (is(w, "GAME")) s.set_int("gamespeed", 10);
            if (is(w, "SCREEN")) s.set_int("scrollspeed", 0x20);
            if (is(w, "TXTSCROL")) s.set_int("textscroll", 10);
            if (is(w, "MAXLINES")) s.set_int("textlines", 10);
            if (is(w, "UNITCHAT")) s.set_int("unitchattext", 5);
            if (is(w, "LEFTCLICK")) s.set_int("Interface Type", 0);
            if (is(w, "GAMMA")) s.set_int("Gamma", 12);
            if (is(w, "SHADING")) s.set_int("Shading", 1);
            if (is(w, "ANTI")) s.set_int("Anti-Alias", 1);
            if (is(w, "BSHADOWS")) s.set_int("FeatureShadows", 1);
            if (is(w, "VIDSLDR") && !in_battle_) {
                s.set_int("DisplaymodeWidth", 640);
                s.set_int("DisplaymodeHeight", 480);
            }
        }
        load(fe);
    }
    void save_types(FrontEnd& fe) {
        client::MusicPlayer* m = fe.music();
        if (!m) return;
        std::string v;
        for (int t = 1; t <= m->track_count(); ++t) v += static_cast<char>('0' + m->types()[static_cast<std::size_t>(t)]);
        fe.settings().set_text("CDLISTS", v);
    }

    MenuArt& art_;
    bool in_battle_;
    int track_ = 0;
    client::Settings saved_;
    std::vector<std::array<int, 2>> modes_;
};

// The options menu: STARTOPT.GUI in the menus (its pages full-screen behind its buttons),
// PREFS.GUI in a battle (the side panel with the pages beside it).
class OptionsMenu : public LayoutScreen {
public:
    OptionsMenu(MenuArt& art, bool in_battle) : LayoutScreen(art.layout(in_battle ? "PREFS" : "STARTOPT")), art_(art), in_battle_(in_battle) {}
    void enter(FrontEnd& fe) override {
        if (!entered_) {
            saved_ = fe.settings();
            entered_ = true;
            // In a battle the panel slides in: "Panel" at once.
            if (in_battle_) sfx(fe, art_, "Panel");
        }
    }
    void frame(FrontEnd& fe, std::uint32_t now) override {
        LayoutScreen::frame(fe, now);
        // In a battle the light bar runs across the panel 21 pixels a frame from 0 and stops
        // past 276, with "Options" (the 14th battle frame since the menu opened).
        if (!in_battle_ || slide_ > 276) return;
        slide_ += 21;
        if (slide_ > 276) sfx(fe, art_, "Options");
    }
    bool see_through() const override { return in_battle_; }
    const render::Palette* palette() const override {
        if (page_ && page_->palette()) return page_->palette();
        return LayoutScreen::palette();
    }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        using T = client::InputEvent::Type;
        if (e.type == T::MouseDown) to_page_ = page_ && page_->over(e.x, e.y);
        if (page_ && (to_page_ || (e.type == T::MouseMove && to_page_))) {
            page_->handle(fe, e);
            if (e.type == T::MouseUp) to_page_ = false;
            return;
        }
        LayoutScreen::handle(fe, e);
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        if (!placed_) place(out.width, out.height);
        if (page_ && !in_battle_) {
            // The page's picture fills the screen; the menu's buttons stay over it.
            page_->draw(fe, out);
            for (std::size_t i = 1; i < widgets_.size(); ++i)
                if (widgets_[i].active) draw_widget(fe, out, widgets_[i], static_cast<int>(i) == held_);
            draw_focus_ring(fe, out);
            return;
        }
        LayoutScreen::draw(fe, out);
        if (page_) page_->draw(fe, out);
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        static const std::pair<const char*, const char*> pages[] = {
            {"SOUND", "SOUNDS"}, {"MUSIC", "MUSIC"}, {"SPEEDS", "SPEEDS"}, {"VISUALS", "VISUALS"}};
        // The page buttons and OK sound "Options", Cancel "Previous" (Undo nothing).
        if (is(w, "SOUND") || is(w, "MUSIC") || is(w, "SPEEDS") || is(w, "VISUALS") || is(w, "PREV")) sfx(fe, art_, "Options");
        else if (is(w, "CANCEL")) sfx(fe, art_, "Previous");
        for (const auto& [button, gui] : pages)
            if (is(w, button)) {
                // In a battle the pages are SOUNDSRT, MUSICRT, SPEEDSRT and VISUALRT (no S).
                static const std::pair<const char*, const char*> rt[] = {{"SOUNDS", "SOUNDSRT"}, {"MUSIC", "MUSICRT"}, {"SPEEDS", "SPEEDSRT"}, {"VISUALS", "VISUALRT"}};
                std::string g = gui;
                if (in_battle_)
                    for (const auto& [a, b] : rt)
                        if (g == a) g = b;
                if (art_.layout(g)) {
                    page_ = std::make_unique<OptionsPage>(art_, g, in_battle_);
                    page_->reassign_quick_keys(widgets_);  // the page joins this screen
                    page_->enter(fe);
                    placed_ = false;  // the menu is set up anew: its default focus returns
                    // The open page's button shows pressed (status 1), the others not.
                    for (Widget& b : widgets_)
                        if (b.kind == WidgetKind::Button && b.stages == 0)
                            for (const auto& [bn, bg] : pages) b.status = is(b, bn) ? (is(b, button) ? 1 : 0) : b.status;
                }
                return;
            }
        if (is(w, "PREV")) {
            fe.apply_settings();
            fe.pop();
        } else if (is(w, "CANCEL")) {
            fe.settings() = saved_;
            fe.apply_settings();
            fe.pop();
        } else if (is(w, "UNDO")) {
            fe.settings() = saved_;
            fe.apply_settings();
            if (page_) page_->enter(fe);
        }
    }

private:
    MenuArt& art_;
    bool in_battle_;
    bool entered_ = false;
    bool to_page_ = false;
    int slide_ = 0;
    client::Settings saved_;
    std::unique_ptr<OptionsPage> page_;
};
// ------------------------------------------------------------------ select map (SELMAP.GUI)
// A screen dimmed under a dialog (open flag 0x800): its rectangle through shade level 8 (defined
// with the in-game menus below).
void dim_rect(FrontEnd& fe, render::Image8& out, int x0, int y0, int x1, int y1);

// A picture mapped onto a gadget's box as the original's surface gadgets draw theirs (0x4A4980):
// texels (1,1) .. (w-1, h-1) onto the box's corners, clipped to the screen's box; without a
// picture the box is filled with interface colour 7.
void draw_surface_picture(render::Image8& out, const render::ClipRect& box, const render::Sprite8* pic, int x0, int y0, int w, int h,
                          std::uint8_t empty_colour) {
    const int x1 = x0 + w - 1, y1 = y0 + h - 1;
    if (!pic) {
        client::fill_rect(out, box, x0, y0, x1, y1, empty_colour);
        return;
    }
    const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
    const int uv[4][2] = {{1, 1}, {pic->width - 1, 1}, {pic->width - 1, pic->height - 1}, {1, pic->height - 1}};
    render::RasterTarget t;
    t.pixels = out.pixels.data();
    t.pitch = out.width;
    t.clip = box;
    render::textured_quad_uv(t, v, uv, *pic);
}

// What the map choice shows of a map (the map info the original loads for a skirmish or network
// game, 0x435DA0 provider 2/3, from maps\<name>.ota [GlobalHeader], the name a translated one's
// base text when no such file opens): the description (at most 127 characters, "No description
// available" when the key is missing; its lowered text's translation when there is one, 0x4364B5),
// memory and numplayers.
struct MapSummary {
    bool loaded = false;
    std::string description, memory, players;
};
MapSummary map_summary(MenuArt& art, const std::string& map) {
    MapSummary m;
    std::vector<std::uint8_t> f;
    if (!art.vfs().read(art.vfs().virtual_path("maps", map_file_name(art.vfs(), map), "ota"), f)) return m;
    m.loaded = true;
    if (auto root = tdf_parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size())).root)
        if (const TdfSection* gh = root->child("GlobalHeader")) {
            m.description = translate_lowered(gh->get_string("missiondescription", 0x80, "No description available"));
            m.memory = gh->get_string("memory", 0x80, "");
            m.players = gh->get_string("numplayers", 0x80, "");
        }
    return m;
}

// The map choice (0x47AAF0, SELMAP.GUI opened with flags 0x880 over the skirmish set-up: the
// screen under it dimmed once, no stock panel; DSELECTMAP2's top-left part in its box, its palette
// not put up). MAPNAMES lists the maps (MenuArt::skirmish_maps), the first one named exactly as the
// setting is selected and brought into view; moving the selection shows that map (0x47AAA0 ->
// 0x444A20): DESCRIPTION its description (word-wrapped), SIZE "<memory>  Players: <numplayers>",
// MAPPIC its picture. Handler (0x47A910): MAPNAMES (double click, Enter, Space) and LOAD ("Select
// Map", sound SmallButton) take the selected map as the skirmish map and close; PREVMENU (Cancel,
// Escape) plays Previous and closes.
// The battle room's map choice (0x444EA0, handler 0x444CB0) is the same screen opened with flags
// 0x980 (centred on the screen) over the room: a pick or LOAD plays Multi and takes the map,
// PREVMENU plays Previous.
class SelectMap : public LayoutScreen {
public:
    SelectMap(MenuArt& art, std::string current, std::function<void(const std::string&)> chosen, bool room = false)
        : LayoutScreen(art.layout("SELMAP")), art_(art), chosen_(std::move(chosen)), room_(room) {
        background_in_box_ = true;
        if (room) open_centred_ = -1;
        if (Widget* list = widget("MAPNAMES")) {
            set_list(*list, art.skirmish_maps());
            for (std::size_t i = 0; i < list->items.size(); ++i)
                if (list->items[i] == current) {
                    select_list_row(*list, static_cast<int>(i));
                    break;
                }
            show(*list);
        }
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }
    void draw(FrontEnd& fe, render::Image8& out) override {
        // The screen under it (the skirmish set-up's whole box) dimmed.
        dim_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        LayoutScreen::draw(fe, out);
    }

protected:
    void show(const Widget& list) {
        if (list.selected < 0 || list.selected >= static_cast<int>(list.items.size())) return;
        const std::string map = list.items[static_cast<std::size_t>(list.selected)];
        const MapSummary m = map_summary(art_, map);
        if (!m.loaded) return;
        // ("%s  %s: %s" with "Players" translated, 0x444A85, set as a caption: translated again)
        if (Widget* s = widget("SIZE")) s->text = translate(m.memory + "  " + translate("Players") + ": " + m.players);
        if (Widget* pic = widget("MAPPIC")) pic->picture = art_.map_preview(map_file_name(art_.vfs(), map), pic->w, pic->h);
        if (Widget* d = widget("DESCRIPTION")) d->text = translate(m.description);  // (set as a caption)
    }
    void on_list_select(FrontEnd&, Widget& w) override { show(w); }
    void choose(FrontEnd& fe, Widget& list) {
        if (list.selected >= 0 && list.selected < static_cast<int>(list.items.size()) && chosen_) chosen_(list.items[static_cast<std::size_t>(list.selected)]);
        fe.pop();
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "MAPNAMES")) {
            if (room_) sfx(fe, art_, "Multi");
            choose(fe, w);
        } else if (is(w, "LOAD")) {
            sfx(fe, art_, room_ ? "Multi" : "SmallButton");
            if (Widget* l = widget("MAPNAMES")) choose(fe, *l);
        } else if (is(w, "PREVMENU")) {
            sfx(fe, art_, "Previous");
            fe.pop();
        }
    }
    void draw_surface(FrontEnd& fe, render::Image8& out, const Widget& w) override {
        const Widget& h = widgets_.front();
        const render::ClipRect box{std::max(ox_, 0), std::max(oy_, 0), std::min(ox_ + h.w - 1, out.width - 1), std::min(oy_ + h.h - 1, out.height - 1)};
        draw_surface_picture(out, box, w.picture, ox_ + w.x, oy_ + w.y, w.w, w.h, colour(7));
        (void)fe;
    }

private:
    MenuArt& art_;
    std::function<void(const std::string&)> chosen_;
    bool room_ = false;
};

// A joining player's map view (0x444A20 over VIEWMAP.GUI, opened from the battle room with flags
// 0x900: centred, the room dimmed; DVIEWMAP's top-left part in its box, its palette not put up):
// MAPNAME the map's name, SIZE "<memory>  Players: <numplayers>", MAPPIC its picture,
// DESCRIPTION its description. The room keeps it on the host's map (0x44A680 shows the map
// again when the host's changes). OK (0x444BA0) plays Multi and closes.
class ViewMap : public LayoutScreen {
public:
    ViewMap(MenuArt& art, std::function<std::string()> host_map) : LayoutScreen(art.layout("VIEWMAP")), art_(art), host_map_(std::move(host_map)) {
        background_in_box_ = true;
        background_ = art.picture("DVIEWMAP", nullptr);
        open_centred_ = -1;
        show();
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }
    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        if (host_map_ && host_map_() != shown_) show();
        LayoutScreen::frame(fe, now_ms);
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        dim_rect(fe, out, 0, 0, out.width - 1, out.height - 1);
        LayoutScreen::draw(fe, out);
    }

protected:
    void show() {
        shown_ = host_map_ ? host_map_() : std::string();
        // (the map info's shown name, 0x435A20: a translated lowered name or the name as given;
        // set as a caption)
        if (Widget* n = widget("MAPNAME")) n->text = translate(translate_lowered(shown_));
        const MapSummary m = map_summary(art_, shown_);
        if (!m.loaded) return;
        // ("%s  %s: %s" with "Players" translated, 0x444A85, set as a caption: translated again)
        if (Widget* s = widget("SIZE")) s->text = translate(m.memory + "  " + translate("Players") + ": " + m.players);
        if (Widget* pic = widget("MAPPIC")) pic->picture = art_.map_preview(map_file_name(art_.vfs(), shown_), pic->w, pic->h);
        if (Widget* d = widget("DESCRIPTION")) d->text = translate(m.description);  // (set as a caption)
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) {
            sfx(fe, art_, "Multi");
            fe.pop();
        }
    }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        const Widget& h = widgets_.front();
        const render::ClipRect box{std::max(ox_, 0), std::max(oy_, 0), std::min(ox_ + h.w - 1, out.width - 1), std::min(oy_ + h.h - 1, out.height - 1)};
        draw_surface_picture(out, box, w.picture, ox_ + w.x, oy_ + w.y, w.w, w.h, colour(7));
    }

private:
    MenuArt& art_;
    std::function<std::string()> host_map_;
    std::string shown_;
};

// The last characters typed on a screen (the interface pass 0x4A9FD0 keeps the last 15 key codes
// its special keys left over, upper-cased, in the layout and then calls the screen's typing
// hook): the single player menu's DRDEATH and the skirmish set-up's "*III" .. "*X".
struct TypedTail {
    std::string last = std::string(15, '\0');
    // Takes one event's key codes; true when any went in.
    bool take(const client::InputEvent& e) {
        bool any = false;
        for (int c : client::menu_key_codes(e)) {
            // (Tab, Enter, Escape, Space and the arrows are the interface's own keys)
            if (c == 9 || c == 13 || c == 27 || c == 0x20 || (c >= 0xF4 && c <= 0xF7)) continue;
            last.erase(0, 1);
            last.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 0x20 : c));
            any = true;
        }
        return any;
    }
    bool ends_with(const char* s) const {
        const std::size_t n = std::char_traits<char>::length(s);
        return last.compare(last.size() - n, n, s) == 0;
    }
    void clear() { last.assign(15, '\0'); }
};

// ------------------------------------------------------------------ skirmish (SKIRMISH.GUI)
class Skirmish : public LayoutScreen {
public:
    Skirmish(MenuArt& art, client::Settings& s) : LayoutScreen(art.layout("SKIRMISH")), art_(art) {
        load(s);
        described_ = widgets_.size();
        build_rows();
    }
    void enter(FrontEnd& fe) override {
        if (Widget* m = widget("MapName")) m->text = translate(translate_lowered(map_));  // (the map info's shown name, 0x435A20, as a caption)
        (void)fe;
    }

protected:
    struct Row {
        int kind = 0;  // 0 open, 1 player, 2 computer
        int side = 0, ally = 5, metal = 1000, energy = 1000, colour = 0;
    };

    void load(client::Settings& s) {
        rows_.resize(static_cast<std::size_t>(std::clamp(s.get_int("NumSkirmishPlayers", 4), 2, 10)));
        bool any = false;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string p = "Skirmish.Player" + std::to_string(i);
            Row& r = rows_[i];
            r.kind = s.get_int(p + "Controller", 0);
            r.side = s.get_int(p + "Side", static_cast<int>(i & 1));
            r.colour = s.get_int(p + "Color", static_cast<int>(i));
            r.ally = s.get_int(p + "AllyGroup", 5);
            r.metal = s.get_int(p + "Metal", 1000);
            r.energy = s.get_int(p + "Energy", 1000);
            if (r.kind) any = true;
        }
        if (!any) {
            // Nothing set up yet: one player against one computer.
            rows_[0].kind = 1;
            rows_[1].kind = 2;
        }
        sync_setup();
        commander_ends_ = s.get_int("SkirmishCommanderDeath", 1);
        mapping_ = s.get_int("SkirmishMapping", 1);
        los_ = s.get_int("SkirmishLineOfSight", 1);
        los_type_ = s.get_int("SkirmishLOSType", 1);
        difficulty_ = s.get_int("SkirmishDifficulty", 1) & 0xFFFF;
        fixed_ = s.get_int("SkirmishLocation", 1);
        // The original (0x47BBB0): the saved map is looked up (0x435A20 -> 0x435DA0: Maps\<name>.ota
        // loads with a [GlobalHeader] and a network schema; the name kept as saved, any case);
        // when that fails — no setting, or a map that is gone — the map list's first name is taken
        // (0x435D30 with 0: the unsorted list, maps in enumeration order; "" when there are no
        // maps). The registry read at start-up (0x42F9A0) does the same for a missing key.
        map_ = s.get_text("SkirmishMap", "");
        const auto& maps = art_.skirmish_maps();
        bool found = false;
        // (The list holds the shown names; a saved name may be a file name or a translated one.)
        const std::string file = upper(map_file_name(art_.vfs(), map_));
        for (const auto& m : maps) found = found || upper(m) == upper(map_) || upper(map_file_name(art_.vfs(), m)) == file;
        if (!found) map_ = art_.first_skirmish_map();
    }
    // The screen's rows are the set-up table's first rows (the original edits GS+0x29A0 in
    // place); the rows past the shown count keep what they held (MenuArt::setup_kinds).
    void sync_setup() {
        for (std::size_t i = 0; i < rows_.size() && i < art_.setup_kinds.size(); ++i) art_.setup_kinds[i] = rows_[i].kind;
        art_.setup_read = true;
    }
    void save(client::Settings& s) {
        sync_setup();
        s.set_int("NumSkirmishPlayers", static_cast<int>(rows_.size()));
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string p = "Skirmish.Player" + std::to_string(i);
            const Row& r = rows_[i];
            s.set_int(p + "Controller", r.kind);
            s.set_int(p + "Side", r.side);
            s.set_int(p + "Color", r.colour);
            s.set_int(p + "AllyGroup", r.ally);
            s.set_int(p + "Metal", r.metal);
            s.set_int(p + "Energy", r.energy);
        }
        s.set_int("SkirmishCommanderDeath", commander_ends_);
        s.set_int("SkirmishMapping", mapping_);
        s.set_int("SkirmishLineOfSight", los_);
        s.set_int("SkirmishLOSType", los_type_);
        s.set_int("SkirmishDifficulty", difficulty_);
        s.set_int("SkirmishLocation", fixed_);
        s.set_text("SkirmishMap", map_);
    }

    // The per-player rows the original adds to the screen: name, side, colour, alliance,
    // metal and energy, spaced 200 / N apart and centred in the rows' area.
    void build_rows() {
        const int n = static_cast<int>(rows_.size());
        const int step = 200 / n;
        int y = (180 - (n - 1) * step) / 2 + 79;
        const std::string gaf = "anims\\SKIRMISH.GAF";
        const auto* name_art = art_.sequence(gaf, "skirmname");
        const auto* side_art = art_.sequence(gaf, "SIDEx");
        const auto* res_art = art_.sequence(gaf, "skirmmet");
        const auto* team_art = art_.sequence(gaf, "TEAMICONSx");
        colours_ = art_.sequence("textures\\logos.gaf", "32xlogos");
        for (int i = 0; i < n; ++i, y += step) {
            // Appended after the screen was set up: no quick keys; metal and energy carry attribute
            // 0x10000 as in the original.
            auto add = [&](WidgetKind k, const std::string& name, int x, int w, const std::vector<render::Sprite8>* frames, const std::string& help) {
                Widget wd;
                wd.kind = k;
                wd.name = name + std::to_string(i);
                wd.x = x;
                wd.y = y;
                wd.w = frames && !frames->empty() ? frames->front().width : w;
                wd.h = frames && !frames->empty() ? frames->front().height : 20;
                wd.frames = frames;
                wd.attribs = name == "Metal" || name == "Energy" ? 2 | 0x10000 : 2;
                wd.text.clear();
                help_[wd.name] = translate(help);  // (translated, 0x479EBB / 0x479FA8 / 0x47A067)
                widgets_.push_back(wd);
            };
            add(WidgetKind::Button, "Player", 45, 112, name_art, "");
            add(WidgetKind::Button, "Side", 163, 45, side_art, "");
            add(WidgetKind::Surface, "Color", 214, 20, nullptr, "");  // a hotspot: only the logo is drawn
            add(WidgetKind::Button, "Allies", 241, 40, team_art, "Click to select an allegiance symbol.");
            add(WidgetKind::Button, "Metal", 286, 45, res_art, "Left click to increase metal. Right click to decrease metal.");
            add(WidgetKind::Button, "Energy", 337, 45, res_art, "Left click to increase energy. Right click to decrease energy.");
        }
        refresh();
    }

    void refresh() {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Row& r = rows_[i];
            const std::string n = std::to_string(i);
            const bool on = r.kind != 0;
            if (Widget* w = widget("Player" + n)) {
                // (Translated, 0x479873 .. 0x4798D0 / 0x47A1E4 / 0x47A20D, and again as the caption.)
                set_caption(*w, translate(r.kind == 1 ? "Player" : r.kind == 2 ? "Computer" : "Open"));
                w->active = true;
            }
            if (Widget* w = widget("Side" + n)) {
                w->active = on;
                w->stages = w->frames ? static_cast<int>(w->frames->size()) : 0;
                w->stage = r.side;
                w->text.clear();
            }
            if (Widget* w = widget("Color" + n)) w->active = on;
            if (Widget* w = widget("Allies" + n)) {
                w->active = on;
                // One frame per group for a group of several, the next for a group of one;
                // frame 10 for no group.
                int members = 0;
                for (const Row& o : rows_) members += o.kind != 0 && o.ally == r.ally;
                w->stages = w->frames ? static_cast<int>(w->frames->size()) : 0;
                w->stage = r.ally == 5 || members == 0 ? 10 : members == 1 ? r.ally * 2 + 1 : r.ally * 2;
            }
            if (Widget* w = widget("Metal" + n)) {
                w->active = on;
                w->text = std::to_string(r.metal);
            }
            if (Widget* w = widget("Energy" + n)) {
                w->active = on;
                w->text = std::to_string(r.energy);
            }
        }
        auto stage = [&](const char* name, int s, const char* help) {
            if (Widget* w = widget(name)) {
                w->stage = s;
                help_[upper(name)] = translate(help);  // (translated, 0x47A50D .. 0x47A691, 0x47B88B)
            }
        };
        stage("StartLocation", fixed_ ? 0 : 1,
              fixed_ ? "Commanders are placed at pre-determined locations." : "Commanders are randomly placed on the battle field.");
        stage("CommanderDeath", commander_ends_ ? 0 : 1,
              commander_ends_ ? "Game ends when commander is destroyed." : "Game continues after Commander is destroyed.");
        stage("Mapping", mapping_ ? 0 : 1, mapping_ ? "Terrain is blacked out until explored." : "Terrain is visible.");
        stage("LineOfSight", !los_ ? 0 : los_type_ == 1 ? 1 : 2,
              !los_ ? "All mapped terrain is visible." : los_type_ == 1 ? "Terrain elevations affect a unit's view."
                                                                       : "Terrain elevations do not affect a unit's view.");
        stage("Difficulty", difficulty_, "");
        if (Widget* m = widget("MapName")) m->text = translate(translate_lowered(map_));  // (the map info's shown name, 0x435A20, as a caption)
    }

    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        // The help line shows the help of the button under the pointer.
        if (e.type == client::InputEvent::Type::MouseMove) {
            std::string h;
            for (const Widget& w : widgets_)
                if (w.active && e.x >= ox_ + w.x && e.x < ox_ + w.x + w.w && e.y >= oy_ + w.y && e.y < oy_ + w.y + w.h) {
                    const auto it = help_.find(upper(w.name));
                    if (it != help_.end()) h = it->second;
                    if (it == help_.end()) {
                        const auto it2 = help_.find(w.name);
                        if (it2 != help_.end()) h = it2->second;
                    }
                }
            // (The interface pass translates the help it shows, 0x4AA606.)
            if (Widget* t = widget("HELPTEXT")) t->text = translate(h);
        }
        if (typed_.take(e)) player_count_cheat(fe);
        LayoutScreen::handle(fe, e);
    }

    // 0x47B9F0: "*" and a roman numeral typed sets the number of rows (3..10): the settings
    // saved, NumSkirmishPlayers written, the rows read again and rebuilt, "SkirmishCheat", the
    // focus to the next gadget. (Rows 5..7's numerals begin the next ones': the typing goes on.)
    void player_count_cheat(FrontEnd& fe) {
        int n = 0;
        if (typed_.ends_with("*IV")) n = 4;
        else if (typed_.ends_with("*V")) n = 5;
        else if (typed_.ends_with("*VI")) n = 6;
        else if (typed_.ends_with("*VII")) n = 7;
        else if (typed_.ends_with("*VIII")) n = 8;
        else if (typed_.ends_with("*III")) n = 3;
        else if (typed_.ends_with("*IX")) n = 9;
        else if (typed_.ends_with("*X")) n = 10;
        if (n == 0) return;
        if (n == 3 || n == 4 || n >= 8) typed_.clear();
        client::Settings& s = fe.settings();
        save(s);
        s.set_int("NumSkirmishPlayers", n);
        fe.apply_settings();
        load(s);
        widgets_.resize(described_);
        build_rows();
        sfx(fe, art_, "SkirmishCheat");
        move_focus(1);
    }

    void draw_over(FrontEnd&, render::Image8& out) override {
        // Colour buttons show the colour's logo.
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Widget* w = widget("Color" + std::to_string(i));
            if (!w || !w->active || !colours_ || colours_->empty()) continue;
            // The colour's logo mapped onto the box as a textured quad: corners on the box's
            // edges, texture corners (1,1) .. (w-1,h-1) (the scan rules leave the last row and column).
            const render::Sprite8& s = (*colours_)[static_cast<std::size_t>(rows_[i].colour) % colours_->size()];
            const int x0 = ox_ + w->x, y0 = oy_ + w->y, x1 = x0 + w->w - 1, y1 = y0 + w->h - 1;
            const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
            const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
            render::RasterTarget t;
            t.pixels = out.pixels.data();
            t.pitch = out.width;
            t.clip = out.bounds();
            render::textured_quad_uv(t, v, uv, s);
        }
    }

    void on_press(FrontEnd& fe, Widget& w) override {
        const std::string n = upper(w.name);
        auto row_of = [&](const char* prefix) -> int {
            const std::string p = prefix;
            if (n.size() > p.size() && n.compare(0, p.size(), p) == 0 && std::isdigit(static_cast<unsigned char>(n[p.size()])))
                return std::atoi(n.c_str() + p.size());
            return -1;
        };
        int r;
        const bool right = press_button_ == 3;
        // The rows' buttons and the options sound "Skirmish"; Start "BigButton" (also when it
        // is refused), the previous menu "Previous".
        if (is(w, "Start")) sfx(fe, art_, "BigButton");
        else if (is(w, "PrevMenu")) sfx(fe, art_, "Previous");
        else if (row_of("PLAYER") >= 0 || row_of("SIDE") >= 0 || row_of("ALLIES") >= 0 || row_of("COLOR") >= 0 || row_of("METAL") >= 0 ||
                 row_of("ENERGY") >= 0 || is(w, "StartLocation") || is(w, "CommanderDeath") || is(w, "Mapping") || is(w, "LineOfSight") ||
                 is(w, "Difficulty") || is(w, "SelectMap"))
            sfx(fe, art_, "Skirmish");
        if ((r = row_of("PLAYER")) >= 0) {
            // Open -> computer -> player (when there is none) or open; player -> open.
            Row& row = rows_[static_cast<std::size_t>(r)];
            if (row.kind == 0) row.kind = 2;
            else if (row.kind == 1) row.kind = 0;
            else {
                int humans = 0;
                for (const Row& o : rows_) humans += o.kind == 1;
                row.kind = humans == 0 ? 1 : 0;
            }
            if (row.kind != 0) {
                // A colour already taken moves to the first free one.
                bool taken = false;
                for (std::size_t k = 0; k < rows_.size(); ++k)
                    if (static_cast<int>(k) != r && rows_[k].kind != 0 && rows_[k].colour == row.colour) taken = true;
                if (taken)
                    for (int c = 0; c < 10; ++c) {
                        bool used = false;
                        for (const Row& o : rows_) used = used || (o.kind != 0 && o.colour == c);
                        if (!used) {
                            row.colour = c;
                            break;
                        }
                    }
            }
        } else if ((r = row_of("SIDE")) >= 0) {
            const int sides = std::max<int>(1, static_cast<int>(art_.sides().size()));
            rows_[static_cast<std::size_t>(r)].side = (rows_[static_cast<std::size_t>(r)].side + 1) % sides;
        } else if ((r = row_of("ALLIES")) >= 0) {
            rows_[static_cast<std::size_t>(r)].ally = (rows_[static_cast<std::size_t>(r)].ally + 1) % 6;
        } else if ((r = row_of("COLOR")) >= 0) {
            // Steps forward (left) or back (right) to the next colour no other row uses.
            const int count = colours_ ? static_cast<int>(colours_->size()) : 10;
            Row& row = rows_[static_cast<std::size_t>(r)];
            for (int k = 0; k < count; ++k) {
                row.colour = (row.colour + (right ? -1 : 1) + count) % count;
                bool used = false;
                for (std::size_t o = 0; o < rows_.size(); ++o)
                    used = used || (static_cast<int>(o) != r && rows_[o].kind != 0 && rows_[o].colour == row.colour);
                if (!used) break;
            }
        } else if ((r = row_of("METAL")) >= 0 || (r = row_of("ENERGY")) >= 0) {
            int& v = row_of("METAL") >= 0 ? rows_[static_cast<std::size_t>(r)].metal : rows_[static_cast<std::size_t>(r)].energy;
            if (right) v = std::max(200, v - 500);
            else {
                v = std::min(10000, v + 500);
                if (v == 700) v = 500;
            }
        } else if (is(w, "StartLocation")) {
            fixed_ ^= 1;
        } else if (is(w, "CommanderDeath")) {
            commander_ends_ ^= 1;
        } else if (is(w, "Mapping")) {
            mapping_ ^= 1;
        } else if (is(w, "LineOfSight")) {
            if (!los_) {
                los_ = 1;
                los_type_ = 1;
            } else if (los_type_ == 1) {
                los_type_ = 0;
            } else {
                los_ = 0;
                los_type_ = 1;
            }
        } else if (is(w, "Difficulty")) {
            difficulty_ = (difficulty_ + 1) % 3;
        } else if (is(w, "SelectMap")) {
            // 0x47AAF0: without a map to list (0x434BF0 counts none) a 320-wide box instead.
            if (art_.skirmish_maps().empty()) show_message(fe, art_, translate("There are no skirmish maps to choose from"));
            else fe.push(std::make_unique<SelectMap>(art_, map_, [this](const std::string& m) { map_ = m; }));
        } else if (is(w, "PrevMenu")) {
            save(fe.settings());
            fe.apply_settings();
            fe.pop();
            return;
        } else if (is(w, "Start")) {
            art_.vfs().scan();  // the original rescans the archives here (0x47AF2E)
            start(fe);
            return;
        }
        refresh();
        save(fe.settings());
    }

    void start(FrontEnd& fe) {
        // 0x47AE60 (Start): the player count GS+0x2A3C = computer players + 1, the map info loaded
        // (0x435A20, kind 2); then the refusals, each a 480-wide box (0x4ABD90 with 0x1E0, OK,
        // fitted; the text translated): no map info, no player or no computer player, more
        // players than the picked schema's start positions (0x437300 < GS+0x2A3C), one allied
        // group (0x479760). The schema is not picked with GS+0x2A3C: 0x436860 counts for kind 2
        // the highest filled row of the whole set-up table + 1 (rows 0 and 3 filled: 4; rows
        // past the shown count included), GS+0x2A3C only when no row is filled. The battle keeps
        // that schema (the skirmish battle start 0x497180 does not load the map info again).
        int humans = 0, computers = 0;
        for (const Row& r : rows_) {
            humans += r.kind == 1;
            computers += r.kind == 2;
        }
        sync_setup();
        int schema = -1;
        const int start_positions = map_start_position_count(art_.vfs(), map_, art_.setup_players(computers + 1), &schema);
        if (start_positions < 0) {
            show_message(fe, art_, translate("The terrain for the selected map does not exist."), nullptr, 0x1E0);  // (0x47B0CB)
            return;
        }
        if (humans == 0 || computers == 0) {
            show_message(fe, art_, translate("There must be at least one player and one computer opponent"), nullptr, 0x1E0);  // (0x47B0CB)
            return;
        }
        if (start_positions < computers + 1) {
            show_message(fe, art_, translate("There are too many players enabled for this map"), nullptr, 0x1E0);  // (0x47B008)
            return;
        }
        // Everyone in one alliance (other than "none") is refused.
        int group = -1;
        bool one_group = true;
        for (const Row& r : rows_)
            if (r.kind != 0) {
                if (r.ally == 5) {
                    one_group = false;
                    break;
                }
                if (group < 0) group = r.ally;
                else if (group != r.ally) one_group = false;
            }
        if (one_group && group >= 0) {
            show_message(fe, art_, translate("All players may not be in the same allied group."), nullptr, 0x1E0);  // (0x47B0CB)
            return;
        }
        save(fe.settings());
        fe.apply_settings();
        // The battle request: rows are player slots.
        BattleRequest req;
        req.map = map_;
        req.schema = schema;
        int human = 0;
        for (std::size_t i = 0; i < rows_.size() && i < 10; ++i) {
            const Row& r = rows_[i];
            sim::StartPlayer& p = req.start.players[i];
            if (r.kind == 0) continue;
            p.kind = r.kind == 1 ? sim::PlayerKind::Local : sim::PlayerKind::Computer;
            if (r.kind == 1) human = static_cast<int>(i);
            p.net_id = static_cast<std::int32_t>(i) + 1;
            p.colour = static_cast<std::uint8_t>(r.colour);
            p.ally_group = r.ally == 5 ? 0xFF : static_cast<std::uint8_t>(r.ally);
            p.energy = r.energy;
            p.metal = r.metal;
            p.name = start_player_name(r.kind != 1, r.side, true);  // 0x47A760 -> 0x464290
            req.sides[i] = static_cast<std::size_t>(r.side) < art_.sides().size() ? art_.sides()[static_cast<std::size_t>(r.side)] : "ARM";
        }
        req.start.provider = 2;
        req.start.fixed_locations = fixed_ != 0;
        req.start.commander_mode = commander_ends_ ? 1 : 0;
        req.start.fog_of_war = los_ != 0;
        req.start.true_sight = los_ != 0 && los_type_ == 1;
        req.start.explore = mapping_ != 0;
        req.start.ai_difficulty = difficulty_;
        req.start.local_player = static_cast<std::uint8_t>(human);
        req.start.unit_limit = static_cast<std::uint16_t>(fe.unit_limit());  // GS+0x37EEC (0x4971F1)
        // The inputs the original takes from clocks, and the menus' rand() state.
        req.start.battle_rng_seed = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        req.start.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
        req.start.crt_rand_state = menu_rand().state();
        const std::string dir = art_.game_dir();
        const client::ClientOptions opt = client_options(fe.settings(), fe.width(), fe.height());
        fe.start_battle([dir, req, human, opt](const client::LoadProgress& progress, std::string& error) -> std::unique_ptr<client::BattleSession> {
            return SkirmishSession::start(dir, req, human, opt, progress, error);
        });
    }

private:
    MenuArt& art_;
    std::vector<Row> rows_;
    int commander_ends_ = 1, mapping_ = 1, los_ = 1, los_type_ = 1, difficulty_ = 1, fixed_ = 1;
    std::string map_;
    std::map<std::string, std::string> help_;
    const std::vector<render::Sprite8>* colours_ = nullptr;
    TypedTail typed_;
    std::size_t described_ = 0;  // the description's gadgets (the rows come after them)
};

// ------------------------------------------------------------------ single player (SINGLE.GUI)
class SinglePlayer : public LayoutScreen {
public:
    explicit SinglePlayer(MenuArt& art) : LayoutScreen(art.layout("SINGLE")), art_(art) {
        // In Spanish the Skirmish button's quick key becomes 's' (0x4777A0 -> 0x4A1530).
        if (stricmp_c(Vfs::game_language(), "spanish") == 0)
            if (Widget* w = widget("Skirmish")) w->quickkey = w->quickkey_char = 's';
    }
    void enter(FrontEnd& fe) override {
        if (Widget* w = widget("NewCamp")) w->grayed = !fe.hooks().campaign_menu;
        if (Widget* w = widget("LoadGame")) w->grayed = !fe.hooks().load_game_menu;
        // AnyMsn is shown with the all-missions setting (0x4777A0).
        if (Widget* w = widget("AnyMsn")) w->active = fe.settings().get_int("AllMissions", 0) & 1;
    }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        // 0x477510: DRDEATH typed switches the all-missions setting (and AnyMsn) and writes it.
        if (typed_.take(e) && typed_.ends_with("DRDEATH") && typed_.last.compare(8, 7, "DRDEATH") == 0) {
            const bool on = !(fe.settings().get_int("AllMissions", 0) & 1);
            fe.settings().set_int("AllMissions", on ? 1 : 0);
            if (Widget* w = widget("AnyMsn")) w->active = on;
            fe.apply_settings();
        }
        LayoutScreen::handle(fe, e);
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "NewCamp") || is(w, "LoadGame")) sfx(fe, art_, "BigButton");
        else if (is(w, "AnyMsn")) sfx(fe, art_, "bigButton");
        else if (is(w, "Skirmish")) sfx(fe, art_, "Skirmish");
        else if (is(w, "Options")) sfx(fe, art_, "Options");
        else if (is(w, "PrevMenu")) sfx(fe, art_, "Previous");
        if (is(w, "NewCamp")) {
            if (fe.hooks().campaign_menu) fe.push(fe.hooks().campaign_menu(fe));
        } else if (is(w, "AnyMsn")) {
            if (fe.hooks().any_mission_menu) fe.push(fe.hooks().any_mission_menu(fe));
        } else if (is(w, "LoadGame")) {
            if (fe.hooks().load_game_menu) {
                if (auto next = fe.hooks().load_game_menu(fe, nullptr)) fe.push(std::move(next));
                // (no saved games: the load menu closed again at once, this screen was redrawn
                // with its new focus, then the message came over it)
                else ring_follow_ = true;
            }
        } else if (is(w, "Skirmish")) {
            if (art_.layout("SKIRMISH")) fe.push(std::make_unique<Skirmish>(art_, fe.settings()));
        } else if (is(w, "Options")) {
            fe.push(std::make_unique<OptionsMenu>(art_, false));
        } else if (is(w, "PrevMenu")) {
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    TypedTail typed_;
};

// ------------------------------------------------------------------ movies (0x426780)
// Plays the movies in turn, each with a player of its own (a key ends the one playing; the next
// starts), then runs `after`. A movie whose file is missing is passed over, as the original's
// existence check does — after the player's first step, which stops every sound (0x47F750 comes
// before the check in 0x426780).
void play_movies(FrontEnd& fe, MenuArt& art, std::vector<std::string> names, bool repeat_first, std::function<void(FrontEnd&)> after) {
    while (!names.empty()) {
        std::vector<std::uint8_t> bytes = art.movie(names.front());
        names.erase(names.begin());
        if (bytes.empty()) {
            fe.stop_sounds();
            continue;
        }
        auto next = [&art, names, after](FrontEnd& f) mutable { play_movies(f, art, std::move(names), false, std::move(after)); };
        fe.push(std::make_unique<client::MovieScreen>(std::move(bytes), repeat_first, std::move(next)));
        return;
    }
    if (after) after(fe);
}

// ------------------------------------------------------------------ main menu (MAINMENU.GUI)
class MainMenu : public LayoutScreen {
public:
    explicit MainMenu(MenuArt& art) : LayoutScreen(art.layout("MAINMENU")), art_(art) {
        // The version label: shown with the text "v3.1", moved left by half the text's width in
        // the caption font.
        if (Widget* d = widget("DebugString")) {
            d->active = true;
            d->text = "v3.1";
            if (art.content().font) d->x -= art.content().font->text_width(d->text) / 2;
        }
    }
    void enter(FrontEnd& fe) override {
        if (Widget* w = widget("MULTI")) w->grayed = !fe.hooks().multiplayer_menu;
        if (fe.music()) fe.music()->set_mood(client::MusicMood::Silent);
        // The menus' ambience: "BGM" looping (it keeps playing through the menus until a battle
        // loads; a second start does nothing while it plays).
        fe.play_sound_loop(art_.sound("BGM"));
        if (!started_) {
            // The program's first state (0x426E80 state 0): on a full screen, the first start
            // (registry "PlayMovie", default 1) plays 1.zrb and 2.zrb and clears the setting;
            // later starts play 1.zrb unless a network switch started the program (GS+0x39245),
            // which then opens the multiplayer screens (main menu sub-state 6) after the movies.
            started_ = true;
            const bool net_start = fe.hooks().network_start && fe.hooks().network_start() && fe.hooks().multiplayer_menu;
            if (net_start) fe.push(fe.hooks().multiplayer_menu(fe));
            if (art_.full_screen) {
                std::vector<std::string> names = {"1.zrb"};
                if (fe.settings().get_int("PlayMovie", 1) != 0) {
                    names.push_back("2.zrb");
                    fe.settings().set_int("PlayMovie", 0);
                    fe.apply_settings();  // (saved: 0x430F00)
                    play_movies(fe, art_, names, false, {});
                } else if (!net_start) {
                    play_movies(fe, art_, names, false, {});
                }
            }
        }
    }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        // (the INTRO button reads whether Shift is held when it is pressed)
        shift_ = e.shift;
        LayoutScreen::handle(fe, e);
    }
    void frame(FrontEnd&, std::uint32_t) override {
        // The sparks run along the circuit lines of the picture: one move per pass of the
        // program's loop (0x425B80 is the screen's per-pass routine, called by the interface
        // pass; no clock), so as often as frames are shown. The original's loop is held only by
        // the presentation, which waits for the display's vertical blank: the display's refresh
        // rate (the window front end presents with vsync).
        ++pending_;
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        // Each pass the interface first draws the whole screen anew (the focus outline it draws
        // asks for the next full redraw, so with the focus shown that is every pass): the picture,
        // the gadgets, the focus outline. The sparks then move on that surface: the marks of the
        // last pass are gone, the trace test sees this pass's marks and the gadgets, a spark's
        // old place gets the picture's pixel back (also over a gadget) and its mark lies over
        // everything.
        if (pending_ == 0 && shown_.width == out.width && shown_.height == out.height) {
            out.pixels = shown_.pixels;
            return;
        }
        do {
            LayoutScreen::draw(fe, out);
            if (pending_ > 0) step_sparks(out);
        } while (pending_ > 0 && --pending_ > 0);
        shown_ = out;
    }

protected:
    // 100 sparks: an idle one tries a random spot of the picture's top 220 rows and starts
    // when the picture's colour there is a trace (index & 15 above 12), moving 3 pixels a
    // step along x or y; it turns after a random count, and stops off the trace, off the
    // screen or when its life runs out. It draws index 0xAA and restores the picture behind.
    struct Spark {
        std::int16_t x = 0, y = 0;
        bool on = false;
        std::int8_t dx = 0, dy = 0;
        std::uint8_t life = 0, turn = 0;
        int pos = 0;
    };
    void step_sparks(render::Image8& surface) {
        const client::ScreenLayout* L = layout();
        if (!L || !L->background || L->background->width != 640 || L->background->height < 480) return;
        if (surface.width != 640 || surface.height < 480) return;
        const render::Sprite8& bg = *L->background;
        sim::Lcg15& rnd = menu_rand();
        std::uint8_t* scr = surface.pixels.data();
        for (Spark& s : sparks_) {
            if (!s.on) {
                s.x = static_cast<std::int16_t>(static_cast<int>(rnd.next()) % 640);
                s.y = static_cast<std::int16_t>(static_cast<int>(rnd.next()) % 220);
                s.pos = s.x + s.y * 640;
                if ((scr[s.pos] & 15) > 12) {
                    s.on = true;
                    s.life = static_cast<std::uint8_t>(rnd.next() + 1);
                    s.turn = static_cast<std::uint8_t>((rnd.next() & 31) + 1);
                    if ((s.life & 1) == 0) {
                        s.dx = (s.x & 1) == 0 ? 3 : -3;
                        s.dy = 0;
                    } else {
                        s.dy = (s.y & 1) == 0 ? 3 : -3;
                        s.dx = 0;
                    }
                }
                continue;
            }
            scr[s.pos] = bg.pixels[static_cast<std::size_t>(s.pos)];
            s.y = static_cast<std::int16_t>(s.y + s.dy);
            s.x = static_cast<std::int16_t>(s.x + s.dx);
            if (s.x < 0 || s.x > 639 || s.y > 479 || s.y < 0 || s.life == 0) {
                s.on = false;
                continue;
            }
            --s.life;
            s.pos += s.dx;
            if (s.dy != 0) s.pos += s.dy * 640;
            if ((scr[s.pos] & 15) < 13) {
                s.on = false;
                continue;
            }
            scr[s.pos] = 0xAA;
            if (s.turn == 0) {
                if (s.dy == 0) {
                    s.dy = (s.y & 1) ? 3 : -3;
                    s.dx = 0;
                } else {
                    s.dy = 0;
                    s.dx = (s.x & 1) ? -3 : 3;
                }
                s.turn = static_cast<std::uint8_t>((rnd.next() & 15) + 1);
            } else {
                --s.turn;
            }
        }
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "SINGLE") || is(w, "MULTI")) sfx(fe, art_, "BigButton");
        else if (is(w, "EXIT")) sfx(fe, art_, "Exit");
        else if (is(w, "INTRO") || is(w, "Credits")) sfx(fe, art_, "SmlButton");
        if (is(w, "SINGLE")) {
            fe.push(std::make_unique<SinglePlayer>(art_));
        } else if (is(w, "MULTI")) {
            if (fe.hooks().multiplayer_menu) fe.push(fe.hooks().multiplayer_menu(fe));
        } else if (is(w, "EXIT")) {
            fe.exit_to_windows();
        } else if (is(w, "INTRO") || is(w, "Credits")) {
            // (0x425D80) Only on a full screen; the CD check passes on the Steam version. INTRO
            // plays 2.zrb (again and again while Shift was held, until a key), the credits 5.zrb;
            // then the main menu again.
            if (!art_.full_screen) {
                show_message(fe, art_, "Debug:  You must be in full-screen mode to play a movie.", nullptr, 200);  // (0x4ABD90, width 200)
                return;
            }
            play_movies(fe, art_, {is(w, "INTRO") ? "2.zrb" : "5.zrb"}, is(w, "INTRO") && shift_, {});
        }
    }

private:
    MenuArt& art_;
    bool started_ = false, shift_ = false;
    std::array<Spark, 100> sparks_{};
    int pending_ = 0;        // passes whose spark move is still to be drawn
    render::Image8 shown_;  // the screen as the last pass left it
};

// ------------------------------------------------------------------ in-game menus
// A screen's stock panel when its description names no picture (0x4B0230 with commongui's
// "BackTile", its frames' offsets cleared): the nine frames tile the w x h rectangle as a framed
// box: rows of the first frame's height (the top row frames 0-2, middle rows 3-5, the last
// row 6-8, which lies flush with the bottom), columns of its width (left, middle repeated, the
// right one flush with the right edge).
void draw_back_tile(MenuArt& art, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int w, int h) {
    const auto* t = art.sequence("anims\\commongui.GAF", "BackTile");
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
            if (k < t->size()) {
                const render::Sprite8& s = (*t)[k];
                render::blit(out, clip, s, x0 + x + s.x_offset, y0 + y + s.y_offset);
            }
            x += fw;
        }
        y += fh;
    }
}

// A screen dimmed under a dialog (open flag 0x800): its rectangle through shade level 8. The
// original (0x4BF4D0) indexes the level's row with the pixel as a signed byte: pixels from 0x80
// up take level 7's entry.
void dim_rect(FrontEnd& fe, render::Image8& out, int x0, int y0, int x1, int y1) {
    const std::vector<std::uint8_t>& shade = fe.menu_content().shade;
    if (shade.size() < 0x2000) return;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, out.width - 1);
    y1 = std::min(y1, out.height - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            std::uint8_t& p = out.row(y)[x];
            p = shade[static_cast<std::size_t>(8 * 256 + static_cast<std::int8_t>(p))];
        }
}

// The confirmation (YESORNO.GUI, 0x460680; opened centred right of the side panel): the
// question, Yes and No, No focused. Yes leaves the battle for the main menu (kind 0: "Exit to
// Menu", no statistics) or ends the program (kind 2: "Exit Game"); No closes it.
class YesOrNo : public LayoutScreen {
public:
    YesOrNo(MenuArt& art, int kind) : LayoutScreen(art.layout("YESORNO")), art_(art), kind_(kind) {
        open_centred_ = -2;
        if (Widget* w = widget("CHOICE1")) set_caption(*w, "Yes");
        if (Widget* w = widget("CHOICE2")) set_caption(*w, "No");
        if (Widget* w = widget("TITLE"))
            w->text = kind == 0 ? "Surrender this battle and return to main menu?" : "Surrender this battle and exit to Windows?";
        for (std::size_t i = 1; i < widgets_.size(); ++i)
            if (is(widgets_[i], "CHOICE2")) focus_choice_ = static_cast<int>(i);
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        if (focus_choice_ >= 0) focus_gadget_ = focus_choice_;  // (set when the screen opens)
        focus_choice_ = -1;
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
        (void)fe;
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        sfx(fe, art_, "Exit");
        if (is(w, "CHOICE1")) {
            if (kind_ == 2) fe.exit_to_windows();
            else fe.to_main_menu();
        } else if (is(w, "CHOICE2")) {
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    int kind_ = 0;
    int focus_choice_ = -1;
};

// A network battle's local player out but kept watching (0x464F80, no computer player of this
// machine fighting): YESORNO.GUI opened with flags 0x900 (centred on the screen; the side panel
// dimmed under it), TITLE "You're out!  Continue Watching?", CHOICE1 "Yes", CHOICE2 "No" (set as
// captions: translated), CHOICE1 the Enter and CHOICE2 the Escape default. Handler 0x464DE0: a
// press plays "BigButton"; Yes announces the watch mode again (0x450F90) and clears the won flag;
// No ends the game for this machine (end flags: ending set, won cleared). Either closes it.
class WatchQuestion : public LayoutScreen {
public:
    WatchQuestion(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("YESORNO")), art_(art), s_(s) {
        open_centred_ = -1;
        if (Widget* w = widget("TITLE")) set_caption(*w, "You're out!  Continue Watching?");
        if (Widget* w = widget("CHOICE1")) set_caption(*w, "Yes");
        if (Widget* w = widget("CHOICE2")) set_caption(*w, "No");
        enter_default_ = "CHOICE1";
        s.client().dim_panel(true);
    }
    ~WatchQuestion() override { s_.client().dim_panel(false); }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        sfx(fe, art_, "BigButton");
        sim::Battle& b = s_.battle();
        if (is(w, "CHOICE1")) {
            b.end_game.flags = static_cast<std::uint16_t>(b.end_game.flags & ~sim::end_game_flags::kWon);
            fe.pop();
        } else if (is(w, "CHOICE2")) {
            b.end_game.flags = static_cast<std::uint16_t>((b.end_game.flags | sim::end_game_flags::kEnding) & ~sim::end_game_flags::kWon);
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    client::BattleSession& s_;
};

// The restart dialog (RESTART.GUI over DRESTART, 0x4604A0; opened centred right of the side
// panel): the battle's name, the computer players' difficulty, Cancel and Restart (the same
// battle from its start with that difficulty).
class RestartDialog : public LayoutScreen {
public:
    RestartDialog(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("RESTART")), art_(art) {
        open_centred_ = -2;
        // DRESTART's top-left part in the box, its palette not put on the display (0x4288D0 with
        // no flags).
        background_in_box_ = true;
        if (Widget* w = widget("Difficulty")) w->stage = std::clamp(s.difficulty(), 0, std::max(0, w->stages - 1));
        name_ = s.battle_name();
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }

protected:
    void draw_under(FrontEnd& fe, render::Image8&) override {
        if (named_) return;
        named_ = true;
        // The name word-wrapped to the label's width in the small font (0x460523 -> 0x4AC4C0
        // with font -1, the current one: at the end of a word, when the line so far is at least
        // as wide, it breaks at the last space or hyphen before, which becomes "\r\n"); then
        // strtok(text, "\n") (0x460534 / 0x460560: no empty tokens) gives MISSIONNAME the first
        // line and MISSIONNAME1 the second, each broken line keeping its '\r' (drawn and counted
        // like the message box's lines).
        const client::Font* f = fe.menu_content().small_font ? fe.menu_content().small_font : fe.menu_content().font;
        Widget* w1 = widget("MISSIONNAME");
        if (!w1 || !f) return;
        const std::string out = wrap_for_width(*f, name_, w1->w, true);
        std::vector<std::string> tokens;
        for (std::size_t at = 0; at <= out.size() && tokens.size() < 2;) {
            const std::size_t nl = std::min(out.find('\n', at), out.size());
            if (nl > at) tokens.push_back(out.substr(at, nl - at));
            at = nl + 1;
        }
        w1->text = tokens.empty() ? std::string() : tokens[0];
        if (tokens.size() > 1)
            if (Widget* w2 = widget("MISSIONNAME1")) w2->text = tokens[1];
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        client::BattleSession* s = fe.session();
        sfx(fe, art_, "Options");
        if (is(w, "Difficulty")) sfx(fe, art_, "Options");
        if (is(w, "RESTART")) {
            const Widget* d = widget("Difficulty");
            client::BattleLauncher l = s ? s->restart_launcher(d ? d->stage : 1) : client::BattleLauncher();
            if (l) fe.restart_battle(std::move(l));
            else fe.pop();
        } else if (is(w, "CANCEL")) {
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    std::string name_;
    bool named_ = false;
};

// The exit menu (EXITMENU.GUI, 0x4608B0, opened with flags 0x1800: centred right of the side
// panel, the game menu under it dimmed): the stock panel, Exit to Menu, Exit Game, Cancel, and
// in skirmishes and campaigns Restart (captioned "Restart", a new quick key). Exit to Menu and
// Exit Game ask first (YESORNO); Restart opens the restart dialog.
class ExitMenu : public LayoutScreen {
public:
    ExitMenu(MenuArt& art, client::BattleSession* s, render::ClipRect under) : LayoutScreen(art.layout("EXITMENU")), art_(art), under_(under) {
        open_centred_ = -2;
        if (Widget* w = widget("RESTART")) {
            const bool offered = s && !s->network();
            w->active = offered;
            if (offered) set_caption(*w, translate("Restart"));  // (translated, 0x46092E, and again by the caption)
        }
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_rect(fe, out, under_.x0, under_.y0, under_.x1, under_.y1);
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        sfx(fe, art_, "Options");
        if (is(w, "MAINMENU") || is(w, "EXITGAME")) {
            const int kind = is(w, "MAINMENU") ? 0 : 2;
            fe.pop();
            if (art_.layout("YESORNO")) fe.push(std::make_unique<YesOrNo>(art_, kind));
        } else if (is(w, "RESTART")) {
            client::BattleSession* s = fe.session();
            fe.pop();
            if (s && art_.layout("RESTART")) fe.push(std::make_unique<RestartDialog>(art_, *s));
        } else if (is(w, "CANCEL")) {
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    render::ClipRect under_;
};

// The game menu's help pages (HELP, 0x4609B0: HELP.GUI with flags 0x1881 — centred right of the
// side panel, the game menu dimmed under it —, the "dhelp" picture, handler 0x45FAC0). A page
// (0x45F8C0, 17 lines a page; the run-time labels of the page before go first) reads
// gamedata\help.tdf [Help] "Line<n>" for n = page x 17 .. page x 17 + 16: a line read as
// "key|text" (one starting with '|' is " " with an empty text; strncpy of 128 pads the rest)
// becomes two labels at (40, y) width 78 and (125, y) width 300, attribute 1, y from 50 by 18
// per line read. "Page" (a staged button) shows the page of its stage; OK closes ("Options").
class HelpScreen : public LayoutScreen {
public:
    HelpScreen(MenuArt& art, render::ClipRect under) : LayoutScreen(art.layout("HELP")), art_(art), under_(under) {
        open_centred_ = -2;
        background_in_box_ = true;
        base_ = widgets_.size();
        std::vector<std::uint8_t> f;
        if (art.vfs().read("gamedata\\help.tdf", f)) {
            text_ = std::string(reinterpret_cast<const char*>(f.data()), f.size());
            loaded_ = true;
        }
        fill(0);
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_rect(fe, out, under_.x0, under_.y0, under_.x1, under_.y1);
    }
    void draw_over(FrontEnd& fe, render::Image8& out) override { draw_run_time_labels(fe, out, widgets_, base_, ox_, oy_); }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) {
            sfx(fe, art_, "Options");
            fe.pop();
        } else if (is(w, "Page")) {
            sfx(fe, art_, "Options");
            fill(w.stage);
        }
    }

private:
    void fill(int page) {
        widgets_.resize(base_);
        if (!loaded_) return;
        const auto doc = tdf_parse(std::string_view(text_));
        const TdfSection* help = doc.root ? doc.root->child("Help") : nullptr;
        if (!help) return;
        int y = 0x32;
        for (int n = page * 17; n < page * 17 + 17; ++n) {
            bool found = false;
            std::string line = help->get_string("Line" + std::to_string(n), 0x80, "", &found);
            if (!found) continue;
            std::string key, rest;
            if (!line.empty() && line[0] == '|') {
                key = " ";
            } else {
                const std::size_t bar = line.find('|');
                key = line.substr(0, bar);
                rest = bar == std::string::npos ? std::string() : line.substr(bar + 1);
            }
            // (Both parts go through the translation, 0x45FA04 / 0x45FA50.)
            add_run_time_label(widgets_, translate(key), 0x28, y, 0x4E, 1);
            add_run_time_label(widgets_, translate(rest), 0x7D, y, 300, 1);
            y += 0x12;
        }
    }

    MenuArt& art_;
    render::ClipRect under_;
    std::size_t base_ = 0;
    std::string text_;
    bool loaded_ = false;
};

// The game menu's settings page outside campaigns (MISSION, 0x45F1D0: GAMEOPTIONS.GUI with flags
// 0x1881, the "GameSettings" picture, handler 0x45F190: OK). Rows of two run-time labels, the
// name at (18, y) width 110 and the value at (140, y) width 120 (attribute 1): Commander Death
// (y 90), Starting Locations (108), Mapping Mode (126), Line of Sight (144), then network games
// Cheat Codes (162) and Watching (180), other games Difficulty (162); then Map, Starting Metal,
// Starting Energy and Max Units 18 apart. The values: the commander mode (Game Continues / Game
// Ends / Deathmatch); skirmish start locations from its set-up, others from the host's side
// record (bit 0x4000: Random / Fixed); Mapped / Unmapped (GS+0x14281 bit 0); Permanent (bit 1
// clear), else True (bit 2) or Circular; the host record's cheats (0x2000) and watching (0x80)
// switches (Disallowed / Allowed); Easy / Medium / Hard; the map's name; the starting resources
// (network games: the host record's words x 100; else the local player's set-up) and the unit
// limit, in decimal.
class GameSettingsScreen : public LayoutScreen {
public:
    GameSettingsScreen(MenuArt& art, client::BattleSession& s, render::ClipRect under)
        : LayoutScreen(art.layout("GAMEOPTIONS")), art_(art), under_(under) {
        base_ = widgets_.size();
        open_centred_ = -2;
        background_in_box_ = true;
        const sim::Battle& b = s.battle();
        const client::Content& c = s.content();
        const bool net = b.settings.provider == 3;
        const std::uint16_t host = s.host_options();
        static const char* const kMode[] = {"Game Continues", "Game Ends", "Deathmatch"};
        static const char* const kLevel[] = {"Easy", "Medium", "Hard"};
        auto row = [&](const char* name, const std::string& value, int y, bool number = false) {
            // (Every name and value goes through the translation, the map name too: 0x45F2E9 ..
            // 0x45F6AB.)
            add_run_time_label(widgets_, translate(name), 0x12, y, 0x6E, 1);
            add_run_time_label(widgets_, number ? value : translate(value), 0x8C, y, 0x78, 1);  // (numbers as they are)
        };
        const int mode = b.settings.game_mode;
        row("Commander Death:", mode >= 0 && mode < 3 ? kMode[mode] : "", 0x5A);
        const bool fixed = b.settings.provider == 2 ? c.game_info.fixed_locations : (host & 0x4000) != 0;
        row("Starting Locations:", fixed ? "Fixed" : "Random", 0x6C);
        row("Mapping Mode:", b.settings.explore ? "Unmapped" : "Mapped", 0x7E);
        row("Line of Sight:", !b.settings.fog_of_war ? "Permanent" : b.settings.true_sight ? "True" : "Circular", 0x90);
        int y = 0xB4;
        if (net) {
            row("Cheat Codes:", (host & 0x2000) ? "Allowed" : "Disallowed", 0xA2);
            row("Watching:", (host & 0x80) ? "Allowed" : "Disallowed", 0xB4);
            y = 0xC6;
        } else {
            const int d = b.settings.ai_difficulty;
            row("Difficulty:", d >= 0 && d < 3 ? kLevel[d] : "", 0xA2);
        }
        row("Map:", s.battle_name(), y);  // (the map info's name, +0xA14)
        row("Starting Metal:", std::to_string(c.game_info.start_metal), y + 0x12, true);
        row("Starting Energy:", std::to_string(c.game_info.start_energy), y + 0x24, true);
        row("Max Units:", std::to_string(static_cast<unsigned>(b.settings.unit_limit)), y + 0x36, true);
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        dim_rect(fe, out, under_.x0, under_.y0, under_.x1, under_.y1);
    }
    void draw_over(FrontEnd& fe, render::Image8& out) override { draw_run_time_labels(fe, out, widgets_, base_, ox_, oy_); }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) {
            sfx(fe, art_, "Options");
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    render::ClipRect under_;
    std::size_t base_ = 0;
};

class GameMenu : public LayoutScreen {
public:
    explicit GameMenu(MenuArt& art) : LayoutScreen(art.layout("ARMOPT")), art_(art) {}
    ~GameMenu() override {
        // Closing it lets the battle and the music go on again (outside network games).
        if (session_) session_->client().set_paused(false);
        if (music_) music_->set_paused(false);
    }
    bool see_through() const override { return true; }
    void enter(FrontEnd& fe) override {
        client::BattleSession* s = fe.session();
        const bool net = s && s->network();
        // Saving and loading are not offered in network games; the briefing button reads
        // "Settings" outside campaigns.
        if (Widget* w = widget("SAVEGAME")) w->grayed = net || !fe.hooks().save_game_menu;
        if (Widget* w = widget("LOADGAME")) w->grayed = net || !fe.hooks().load_game_menu;
        // (a new caption takes a new quick key: its first letter no other button uses, the "t")
        if (Widget* w = widget("MISSION")) set_caption(*w, translate("Settings"));  // (translated, 0x460D92, and again by the caption)
        if (!opened_) {
            opened_ = true;
            // Opening it pauses the battle outside network games and holds the music.
            if (s && !net) {
                s->client().set_paused(true);
                session_ = s;
            }
            music_ = fe.music();
            if (music_) music_->set_paused(true);
        }
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        client::BattleSession* s = fe.session();
        if (is(w, "OK") || is(w, "SAVEGAME") || is(w, "LOADGAME") || is(w, "PREFS") || is(w, "EXIT") || is(w, "HELP") || is(w, "MISSION"))
            sfx(fe, art_, "Options");
        if (is(w, "OK")) fe.pop();
        else if ((is(w, "SAVEGAME") && s && fe.hooks().save_game_menu) || (is(w, "LOADGAME") && fe.hooks().load_game_menu)) {
            // The game menu closes and the save / load menu opens over the battle (0x4609B0).
            std::unique_ptr<client::Screen> next = is(w, "SAVEGAME") ? fe.hooks().save_game_menu(fe, *s) : fe.hooks().load_game_menu(fe, s);
            // (the battle stays held: the menu that opens holds it, and lets go when it closes)
            if (next) session_ = nullptr;
            fe.pop();
            if (next) fe.push(std::move(next));
        }
        else if (is(w, "PREFS")) fe.push(std::make_unique<OptionsMenu>(art_, true));
        else if (is(w, "HELP") && art_.layout("HELP")) {
            const Widget& h = widgets_.front();
            fe.push(std::make_unique<HelpScreen>(art_, render::ClipRect{ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}));
        } else if (is(w, "MISSION") && s && s->battle().settings.provider == 1 && fe.hooks().mission_briefing) {
            // A campaign: the mission's briefing pages (BRIEFING.GUI, 0x4609B0) over this menu.
            if (auto b = fe.hooks().mission_briefing(fe, *s)) fe.push(std::move(b));
        } else if (is(w, "MISSION") && s && s->battle().settings.provider != 1 && art_.layout("GAMEOPTIONS")) {
            const Widget& h = widgets_.front();
            fe.push(std::make_unique<GameSettingsScreen>(art_, *s, render::ClipRect{ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}));
        }
        else if (is(w, "EXIT") && art_.layout("EXITMENU")) {
            // (the exit menu opens over this one, which keeps its focus, dimmed: gui_harness)
            focus_gadget_ = focus_before_press_;
            const Widget& h = widgets_.front();
            fe.push(std::make_unique<ExitMenu>(art_, s, render::ClipRect{ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}));
        }
    }

private:
    MenuArt& art_;
    bool opened_ = false;
    client::BattleSession* session_ = nullptr;
    client::MusicPlayer* music_ = nullptr;
};

// The unit information screen (F1, 0x4942E0; UNITINFOX.GUI opened centred right of the side
// panel, its stock panel): the type's picture (unitpics\<unitname>.PCX, its own indices on the
// game palette) in HOTR, its name in NAME, and labels (TEXT, attributes 0x411) of the costs and
// statistics: "Cost", "Energy", "Metal", "Build Time" at x 130 / 140, y 32, 47, 62, 77,
// "Statistics", "Max Velocity", "Acceleration", "Turn Rate" at y 92 .. 137, and the values at x
// 240 from y 32, 15 apart (0x489280): a line break, the energy and metal costs (truncated), the
// build time, a line break, then "N/A" three times for a type that does not move, else the speed
// "%.1f m/s " (30 x speed x 0.4), the acceleration "%.2f m/s/s" (the same), the turn rate
// "%.0f deg/s" (30 x rate x 360 / 65536). OK closes it ("smlbutton"). It leaves the battle its
// keys and running; the pointer outside its box reaches the battle.
// Labels made at run time (0x4AB1B0: kind 5 "TEXT", colour word 15; kept inactive here, after the
// layout's own gadgets from `first` on, and drawn by this): the label drawing puts their glyphs through light level 15 (0x4B8310), from the
// label's top edge, in the small font, clipped to the screen's box and the label's width.
void draw_run_time_labels(FrontEnd& fe, render::Image8& out, const std::vector<Widget>& widgets, std::size_t first, int ox, int oy) {
    const client::MenuContent& mc = fe.menu_content();
    const client::Font* lf = mc.small_font ? mc.small_font : mc.font;
    if (!lf || widgets.empty()) return;
    const Widget& h = widgets.front();
    const render::ClipRect c{ox, oy, ox + h.w - 1, oy + h.h - 1};
    for (std::size_t i = first; i < widgets.size(); ++i) {
        const Widget& w = widgets[i];
        if (w.active || w.kind != WidgetKind::Label || w.name != "TEXT") continue;
        int x = ox + w.x, width = w.w;
        const int y = oy + w.y + lf->raise;
        for (const unsigned char ch : w.text) {
            const client::Glyph& g = lf->glyphs[ch];
            if (ch < 0x20 || !g.present) continue;
            if (width < g.width) break;
            const int rows = g.rows > 0 ? g.rows : lf->height;
            if (ch != ' ')
                for (int r = 0; r < rows; ++r)
                    for (int k = 0; k < g.width; ++k) {
                        const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                        if (at >= g.ink.size() || !g.ink[at]) continue;
                        std::uint8_t v = g.colours.empty() ? 15 : g.colours[at];
                        if (mc.light.size() >= 0x2000) v = mc.light[static_cast<std::size_t>(15 * 256 + v)];
                        client::put_pixel(out, c, x + g.left + k, y + g.top + r, v);
                    }
            if ((width -= g.width) < 0) break;
            x += g.width;
        }
    }
}

// A run-time label (0x4AB1B0) at (x, y) of the given width (-1: to 5 pixels from the box's right).
void add_run_time_label(std::vector<Widget>& widgets, const std::string& text, int x, int y, int width, int attribs) {
    Widget w;
    w.kind = WidgetKind::Label;
    w.name = "TEXT";
    w.x = x;
    w.y = y;
    w.w = width >= 0 ? width : (widgets.empty() ? 0 : widgets.front().w) - x - 5;
    w.h = 0xF;
    w.fg = 0xF;
    w.attribs = attribs;
    w.text = text;
    w.active = false;  // (drawn by draw_run_time_labels)
    widgets.push_back(w);
}

class UnitInfo : public LayoutScreen {
public:
    UnitInfo(MenuArt& art, client::BattleSession& s, std::uint16_t type) : LayoutScreen(art.layout("UNITINFOX")), art_(art) {
        base_ = widgets_.size();
        open_centred_ = -2;
        const client::Content& c = s.content();
        if (type >= c.types.size()) return;
        const client::UnitTypeLook& t = c.types[type];
        std::vector<std::uint8_t> f;
        render::Palette pp{};
        if (art.vfs().read("unitpics\\" + t.unit_name + ".PCX", f) && decode_pcx(f, picture_, &pp)) has_picture_ = true;
        picture_.has_key = false;
        picture_.x_offset = picture_.y_offset = 0;
        if (Widget* w = widget("NAME")) w->text = t.display_name;
        // (tests: the values a comparison with the original's screen puts into its definition)
        std::printf("unit info %s:%d:%d:%d:%d:%d\n", t.unit_name.c_str(), static_cast<int>(t.build_time), static_cast<int>(t.max_velocity),
                    static_cast<int>(t.acceleration), static_cast<int>(t.turn_rate), t.mobile ? 1 : 0);
        const int header_w = widgets_.empty() ? 0 : widgets_.front().w;
        auto label = [&](const std::string& text, int x, int y) {
            Widget w;
            w.kind = WidgetKind::Label;
            w.name = "TEXT";
            w.x = x;
            w.y = y;
            w.w = header_w - x - 5;
            w.h = 0xF;
            w.fg = 0xF;
            w.attribs = 0x411;
            w.text = text;
            w.active = false;  // (drawn by draw_over: its glyphs through light level 15)
            widgets_.push_back(w);
        };
        // (The headings go through the translation, 0x49445F .. 0x494651; so do the units and
        // "N/A" of the values, 0x48938F .. 0x4894CC.)
        label(translate("Cost"), 0x82, 0x20);
        label(translate("Energy"), 0x8C, 0x2F);
        label(translate("Metal"), 0x8C, 0x3E);
        label(translate("Build Time"), 0x8C, 0x4D);
        label(translate("Statistics"), 0x82, 0x5C);
        label(translate("Max Velocity"), 0x8C, 0x6B);
        label(translate("Acceleration"), 0x8C, 0x7A);
        label(translate("Turn Rate"), 0x8C, 0x89);
        auto ftol = [](double v) { return static_cast<int>(sim::x87::ftol32(v)); };
        std::vector<std::string> values = {"\n", std::to_string(ftol(t.cost_energy)), std::to_string(ftol(t.cost_metal)), std::to_string(t.build_time), "\n"};
        if (!t.mobile) {
            values.insert(values.end(), {translate("N/A"), translate("N/A"), translate("N/A")});
        } else {
            char b[64];
            // (x87: 30 x (speed x 2^-16 as a float) x 0.4, printed from the double)
            const float v = static_cast<float>(static_cast<double>(t.max_velocity) * (1.0 / 65536.0));
            std::snprintf(b, sizeof b, "%.1f %s ", static_cast<double>(30.0 * static_cast<double>(v) * 0.4), translate("m/s").c_str());
            values.push_back(b);
            const float a = static_cast<float>(static_cast<double>(t.acceleration) * (1.0 / 65536.0));
            std::snprintf(b, sizeof b, "%.2f %s", static_cast<double>(30.0 * static_cast<double>(a) * 0.4), translate("m/s/s").c_str());
            values.push_back(b);
            std::snprintf(b, sizeof b, "%.0f %s", static_cast<double>(30.0 * t.turn_rate * 0.0054931640625), translate("deg/s").c_str());
            values.push_back(b);
        }
        int y = 0x20;
        for (const std::string& v : values) {
            label(v, 0xF0, y);
            y += 0xF;
        }
    }
    bool see_through() const override { return true; }
    bool battle_keys() const override { return true; }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        const bool pointer = e.type == client::InputEvent::Type::MouseMove || e.type == client::InputEvent::Type::MouseDown ||
                             e.type == client::InputEvent::Type::MouseUp;
        if (pointer && placed_ && !widgets_.empty()) {
            const Widget& h = widgets_.front();
            if ((e.x < ox_ || e.y < oy_ || e.x >= ox_ + h.w || e.y >= oy_ + h.h) && held_ < 0) {
                if (client::BattleSession* s = fe.session()) s->client().handle(e);
                if (e.type != client::InputEvent::Type::MouseMove) return;
            }
        }
        LayoutScreen::handle(fe, e);
    }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
        (void)fe;
    }
    void draw_over(FrontEnd& fe, render::Image8& out) override { draw_run_time_labels(fe, out, widgets_, base_, ox_, oy_); }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        if (!has_picture_ || !is(w, "HOTR")) return;
        const Widget& h = widgets_.front();
        render::blit(out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, picture_, ox_ + w.x, oy_ + w.y);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "DONE")) {
            sfx(fe, art_, "smlbutton");
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    render::Sprite8 picture_;
    bool has_picture_ = false;
    std::size_t base_ = 0;
};

// The share screen of network games (SHARE.GUI, 0x4936F0: the tab bar's SHARE or 'h'; refused
// to a watcher; opened with flags 0x800, the layout under it dimmed once). PLYRLIST lists the
// players one may give to (in use, controller 2 or 3 — not this machine's human —, record index
// not 10, live units or never any, not watching); with none the screen closes at once. METAL and
// ENERGY are sliders over the local player's stored metal and energy (whole: ftol), knob as
// high as the slider, its travel width − height; the amount = ftol(knob / (travel − 1) x
// stored), shown in METAL# / ENERGY# ("%d"). SHARUNIT and MAPINFO are two-stage switches. OK
// ("Options"): with a player picked, the amounts go to it (transfers, sent), with SHARUNIT on the
// selected units that are not commanders, not carried and not in the air change hands
// (0x4933E0), with MAPINFO on the explored map is shared and sent (0x4571C0); the screen closes.
// CANCEL ("Previous") closes it.
class ShareScreen : public LayoutScreen {
public:
    ShareScreen(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("SHARE")), art_(art), s_(s) {
        sim::Battle& b = s.battle();
        local_ = b.settings.local_player;
        const sim::PlayerSlot& me = b.players[static_cast<std::size_t>(local_)];
        std::vector<std::string> names;
        for (int k = 0; k < 10; ++k) {
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
            if (!p.active || p.controller < 2 || p.controller > 3 || p.state == 10 || (p.alive_count == 0 && p.units_created != 0) ||
                b.settings.player_sight[static_cast<std::size_t>(k)].watching)
                continue;
            names.push_back(s.player_name(k));
            slots_.push_back(k);
        }
        if (Widget* l = widget("PLYRLIST")) {
            l->items = names;
            l->selected = -1;
        }
        max_[0] = static_cast<int>(sim::x87::ftol32(me.metal_stored));
        max_[1] = static_cast<int>(sim::x87::ftol32(me.energy_stored));
        for (const char* n : {"METAL", "ENERGY"})
            if (Widget* w = widget(n)) {
                travel_ = w->w - w->h;
                w->range = std::max(0, travel_ - 1);
                w->position = 0;
                w->knob = w->h;
            }
        refresh_amounts();
        s.client().dim_panel(true);  // (opened with flags 0x800: the side panel under it dimmed)
    }
    ~ShareScreen() override { s_.client().dim_panel(false); }
    bool see_through() const override { return true; }
    bool empty() const { return slots_.empty(); }

protected:
    void draw_under(FrontEnd&, render::Image8& out) override {
        // (no panel picture: the stock panel)
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    int amount(int k) const {
        const Widget* w = const_cast<ShareScreen*>(this)->widget(k ? "ENERGY" : "METAL");
        if (!w || travel_ < 2) return 0;
        return static_cast<int>(sim::x87::ftol32(static_cast<double>(w->position) / static_cast<double>(travel_ - 1) * max_[k]));
    }
    void refresh_amounts() {
        if (Widget* w = widget("METAL#")) w->text = std::to_string(amount(0));
        if (Widget* w = widget("ENERGY#")) w->text = std::to_string(amount(1));
    }
    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        LayoutScreen::frame(fe, now_ms);
        refresh_amounts();
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "MAPINFO") || is(w, "SHARUNIT")) {
            sfx(fe, art_, "Options");
            return;
        }
        if (is(w, "CANCEL")) {
            sfx(fe, art_, "Previous");
            fe.pop();
            return;
        }
        if (!is(w, "OK")) return;
        sfx(fe, art_, "Options");
        const Widget* list = widget("PLYRLIST");
        if (list && list->selected >= 0 && static_cast<std::size_t>(list->selected) < slots_.size()) {
            const int to = slots_[static_cast<std::size_t>(list->selected)];
            sim::Battle& b = s_.battle();
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(to)];
            if (p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10 && !b.settings.player_sight[static_cast<std::size_t>(to)].watching &&
                (p.alive_count != 0 || p.units_created == 0)) {
                sim::Economy& e = b.economy();
                sim::transfer_metal(e, static_cast<std::uint8_t>(local_), static_cast<std::uint8_t>(to), static_cast<float>(amount(0)), true);
                sim::transfer_energy(e, static_cast<std::uint8_t>(local_), static_cast<std::uint8_t>(to), static_cast<float>(amount(1)), true);
                const Widget* su = widget("SHARUNIT");
                if (su && su->stage) give_units(b, to);
                const Widget* mi = widget("MAPINFO");
                if (mi && mi->stage) {
                    b.share_explored_map(local_, to);
                    s_.send_explored_map(local_, to);
                }
            }
        }
        fe.pop();
    }

private:
    void give_units(sim::Battle& b, int to) {
        // 0x4933E0: the local player's selected units (in table order) that are not in the air
        // (flags & 3 != 2), carry nothing (+0x8A) and are not carried (+0x86), and are no commander.
        const sim::PlayerSlot& me = b.players[static_cast<std::size_t>(local_)];
        const client::Content& c = s_.content();
        for (int i = std::max(1, me.first_unit); i <= me.last_unit && static_cast<std::size_t>(i) < b.units.size(); ++i) {
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            if (!(u.flags & 0x10u) || (u.flags & 3u) == 2 || u.carrier || u.cargo_head) continue;
            const std::uint16_t t = b.ticks[static_cast<std::size_t>(i)].type_index;
            bool commander = false;
            if (t < c.types.size())
                for (const std::string& cat : c.types[t].categories)
                    if (cat == "COMMANDER") commander = true;
            if (commander) continue;
            sim::transfer_ownership(b.missions(), u, to);
        }
    }
    MenuArt& art_;
    client::BattleSession& s_;
    int local_ = 0, travel_ = 0;
    std::array<int, 2> max_{};
    std::vector<int> slots_;
};

// The allies screen of network games (ALLIES.GUI, 0x4478B0 from the tab bar; flags 0x800). The
// ALLYx / TEAMICONSx gadgets become ALLY0.. / TEAMICONS0.. in order. Per player row k (0x447380:
// in use, not watching, controller 1-3, record index not 10, live units or never any, a logo):
// PLAYERk its name, LOGOk its colour logo (32xlogos stretched over the box, texels 1 .. size - 1),
// TEAMICONSk its ally team's symbol (10 none, 2t + 1 a team of one, 2t a team of several; grayed
// in a battle), ALLYk for another machine's human while the local player plays: its stage = (the
// other's answer << 1) | the local alliance byte (0x446FB0). An ALLY press ("Options") toggles the
// local alliance (packet 0x23, 0x452960) and says "<local name> allied with <name>" / "broke
// alliance with" as an own chat line. VICTORY (allied victory) shows the side record's bit and is
// grayed in a team of two or more or for a watcher; OK ("Options") takes it and announces a change.
class AlliesScreen : public LayoutScreen {
public:
    AlliesScreen(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("ALLIES")), art_(art), s_(s) {
        int ally = 0, team = 0;
        for (Widget& w : widgets_) {
            if (is(w, "ALLYx")) w.name = "ALLY" + std::to_string(ally++);
            else if (is(w, "TEAMICONSx")) w.name = "TEAMICONS" + std::to_string(team++);
        }
        logos_ = s.content().team_logos;
        refresh();
        const sim::Battle& b = s.battle();
        const int local = b.settings.local_player;
        if (Widget* v = widget("VICTORY")) {
            v->stage = b.victory.sides[static_cast<std::size_t>(local)].allied_victory ? 1 : 0;
            const int t = s.ally_team(local);
            int in_team = 0;
            if (t != 5)
                for (int k = 0; k < 10; ++k)
                    if (s.ally_team(k) == t && plays(k)) ++in_team;
            v->grayed = in_team >= 2 || b.settings.player_sight[static_cast<std::size_t>(local)].watching;
        }
        s.client().dim_panel(true);
    }
    ~AlliesScreen() override { s_.client().dim_panel(false); }
    bool see_through() const override { return true; }

protected:
    bool plays(int k) const {
        const sim::Battle& b = s_.battle();
        const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
        return p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10 && (p.alive_count != 0 || p.units_created == 0);
    }
    void refresh() {
        const sim::Battle& b = s_.battle();
        const int local = b.settings.local_player;
        for (int k = 0; k < 10; ++k) {
            const std::string n = std::to_string(k);
            Widget* player = widget("PLAYER" + n);
            Widget* logo = widget("LOGO" + n);
            Widget* ally = widget("ALLY" + n);
            Widget* team = widget("TEAMICONS" + n);
            for (Widget* w : {player, logo, ally, team})
                if (w) w->active = false;
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
            if (!plays(k) || b.settings.player_sight[static_cast<std::size_t>(k)].watching) continue;
            if (player) {
                player->active = true;
                player->text = s_.player_name(k);
            }
            if (logo) {
                logo->active = true;
                logo->stage = s_.content().player_colour[static_cast<std::size_t>(k)];
            }
            if (ally && p.controller == 3 && plays(local) && !s_.computer(k)) {
                ally->active = true;
                const std::uint8_t mine = b.alliances[static_cast<std::size_t>(local)][p.state] ? 1 : 0;
                ally->stage = (s_.allied_by(local, k) ? 2 : 0) | mine;
            }
            if (team) {
                team->active = true;
                team->grayed = true;  // (a battle: the teams are fixed)
                const int t = s_.ally_team(k);
                int members = 0;
                if (t != 5)
                    for (int q = 0; q < 10; ++q)
                        if (s_.ally_team(q) == t && plays(q)) ++members;
                team->stage = t == 5 || members == 0 ? 10 : members == 1 ? 2 * t + 1 : 2 * t;
            }
        }
    }
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        if (!logos_ || w.stage < 0 || w.stage >= static_cast<int>(logos_->size()) || w.name.rfind("LOGO", 0) != 0) return;
        const render::Sprite8& s = (*logos_)[static_cast<std::size_t>(w.stage)];
        const Widget& h = widgets_.front();
        const int x0 = ox_ + w.x, y0 = oy_ + w.y, x1 = x0 + w.w - 1, y1 = y0 + w.h - 1;
        const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
        const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
        render::RasterTarget t;
        t.pixels = out.pixels.data();
        t.pitch = out.width;
        t.clip = {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1};
        render::textured_quad_uv(t, v, uv, s);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        sim::Battle& b = s_.battle();
        const int local = b.settings.local_player;
        if (w.name.rfind("ALLY", 0) == 0 && w.name.size() == 5) {
            const int k = w.name[4] - '0';
            if (k < 0 || k > 9 || !plays(k)) return;
            sfx(fe, art_, "Options");
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
            const bool now = !b.alliances[static_cast<std::size_t>(local)][p.state];
            s_.set_alliance(local, k, now);
            s_.send_chat(std::string(now ? "allied with" : "broke alliance with") + " " + s_.player_name(k));
            refresh();
            return;
        }
        if (is(w, "VICTORY")) {
            sfx(fe, art_, "Options");
            return;
        }
        if (is(w, "OK")) {
            sfx(fe, art_, "Options");
            if (const Widget* v = widget("VICTORY")) s_.set_allied_victory(local, v->stage != 0);
            fe.pop();
        }
    }

private:
    MenuArt& art_;
    client::BattleSession& s_;
    const std::vector<render::Sprite8>* logos_ = nullptr;
};

// The chat line of network games (0x494050): TALK.GUI (flags 0x880: no stock panel) with its
// SENDTO switch, or with that switch on TALK2.GUI (flags 0x800: the stock panel) with the
// recipient panel: SENDTYPE (All, Allies, Enemies, Custom: GS+0x2BF0) and a row per other player
// (0x447380(1): PLAYERn its name, LOGOn its colour logo). The handler (0x493BF0): SENDTO
// ("SmallButton") keeps the switch and opens the line again in the other form; SENDTYPE
// ("SmallButton") takes its stage as the recipients; the PLAYER switches only change their look
// (the handler looks for gadgets named LIVEPLYR, which the file does not have). The console
// picture and the text field are the battle's chat line (its keys stay the battle's).
class ChatPanelScreen : public LayoutScreen {
public:
    ChatPanelScreen(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout(s.chat_panel ? "TALK2" : "TALK")), art_(art), s_(s) {
        panel_ = s.chat_panel;
        focus_ring_ = false;
        logos_ = s.content().team_logos;
        s.client().set_chat_external(true);
        for (const char* n : {"CONSOLE", "TALK"})
            if (Widget* w = widget(n)) w->active = false;
        if (Widget* w = widget("SENDTO")) {
            w->active = true;
            w->stage = panel_ ? 1 : 0;
        }
        if (Widget* w = widget("SENDTYPE")) w->stage = s.chat_mode;
        if (panel_) rows();
    }
    ~ChatPanelScreen() override { s_.client().set_chat_external(false); }
    bool see_through() const override { return true; }
    bool battle_keys() const override { return true; }
    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        LayoutScreen::frame(fe, now_ms);
        if (!s_.client().chatting()) fe.pop();
    }

protected:
    void rows() {
        const sim::Battle& b = s_.battle();
        const int local = b.settings.local_player;
        for (int k = 0; k < 10; ++k) {
            const std::string n = std::to_string(k);
            Widget* player = widget("PLAYER" + n);
            Widget* logo = widget("LOGO" + n);
            for (Widget* w : {player, logo})
                if (w) w->active = false;
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
            const bool plays = p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10 && (p.alive_count != 0 || p.units_created == 0);
            if (k == local || !plays || b.settings.player_sight[static_cast<std::size_t>(k)].watching) continue;
            if (player) {
                player->active = true;
                player->text = s_.player_name(k);
            }
            if (logo) {
                logo->active = true;
                logo->stage = s_.content().player_colour[static_cast<std::size_t>(k)];
            }
        }
    }
    void draw_under(FrontEnd&, render::Image8& out) override {
        if (panel_) {
            const Widget& h = widgets_.front();
            draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
        }
        // (CONSOLE and TALK come first in the description: the battle's chat line, under SENDTO)
        s_.client().draw_chat_line(out);
    }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        if (!logos_ || w.stage < 0 || w.stage >= static_cast<int>(logos_->size()) || w.name.rfind("LOGO", 0) != 0) return;
        const render::Sprite8& s = (*logos_)[static_cast<std::size_t>(w.stage)];
        const Widget& h = widgets_.front();
        const int x0 = ox_ + w.x, y0 = oy_ + w.y, x1 = x0 + w.w - 1, y1 = y0 + w.h - 1;
        const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
        const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
        render::RasterTarget t;
        t.pixels = out.pixels.data();
        t.pitch = out.width;
        t.clip = {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1};
        render::textured_quad_uv(t, v, uv, s);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "SENDTO")) {
            sfx(fe, art_, "SmallButton");
            s_.chat_panel = w.stage != 0;
            fe.pop();  // (opened again in the other form by the battle's next pass)
            return;
        }
        if (is(w, "SENDTYPE")) {
            sfx(fe, art_, "SmallButton");
            s_.chat_mode = w.stage > 3 ? 0 : w.stage;
            return;
        }
    }

private:
    MenuArt& art_;
    client::BattleSession& s_;
    bool panel_ = false;
    const std::vector<render::Sprite8>* logos_ = nullptr;
};

// The time-out dialog of network games (TIMEOUT.GUI, 0x453A50, flags 0x800): opened while a
// remote player has been silent past the time-out (0x453C20), closed ("Previous") when nobody is.
// NAME the player's name; OUTPUT the console's last lines (as many as its rows), oldest first;
// TALK a chat line (Enter sends it as an own chat line and empties it); TIMETEXT "will be rejected
// in %d seconds" (time-out + 120 less the seconds since the player was last heard), and at 0 or
// below the player is sent away (status 6 "lost"); REJECT sends it away at once (0x4538F0).
class TimeoutDialog : public LayoutScreen {
public:
    TimeoutDialog(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("TIMEOUT")), art_(art), s_(s) {
        slot_ = s.timed_out_player();
        if (Widget* w = widget("NAME")) w->text = s.player_name(slot_);
        s.client().dim_panel(true);
        update();
    }
    ~TimeoutDialog() override { s_.client().dim_panel(false); }
    bool see_through() const override { return true; }
    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        LayoutScreen::frame(fe, now_ms);
        if (s_.timed_out_player() != slot_) {
            sfx(fe, art_, "Previous");
            fe.pop();
            return;
        }
        update();
        if (s_.timeout_seconds_left(slot_) <= 0) {
            s_.reject_player(slot_, 6);
            sfx(fe, art_, "Previous");
            fe.pop();
        }
    }

protected:
    void update() {
        if (Widget* t = widget("TIMETEXT")) {
            // (The format goes through the translation, 0x45386A; the caption again.)
            char b[256];
            std::snprintf(b, sizeof b, translate("will be rejected in %d seconds").c_str(), s_.timeout_seconds_left(slot_));
            t->text = translate(b);
        }
        if (Widget* out = widget("OUTPUT")) {
            // (0x453640: from the newest line back as many lines as the list has rows less one)
            const client::GameClient::ConsoleRing& ring = s_.client().console();
            const int rows = std::max(1, out->h / std::max(1, (out->row_h > 0 ? out->row_h : 16)));
            int first = ring.next;
            for (int k = 1; k < rows - 1 && first != ring.oldest; ++k)
                if (--first < 0) first = 29;
            std::vector<std::string> lines;
            for (int i = first; i != ring.next; i = i == 29 ? 0 : i + 1)
                lines.push_back(std::string(ring.lines[static_cast<std::size_t>(i)].text.data()));
            out->items = lines;
        }
    }
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_text(FrontEnd&, Widget& w) override {
        if (!is(w, "TALK") || w.text.empty()) return;
        s_.send_chat(w.text);
        w.text.clear();
        w.cursor = 0;
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "REJECT")) s_.reject_player(slot_, 6);
        (void)fe;
    }

private:
    MenuArt& art_;
    client::BattleSession& s_;
    int slot_ = -1;
};

// The host's kick question (0x446080: YESORNO.GUI with flags 0x100, centred; TITLE "Reject %s?",
// CHOICE1 "Yes" sends the player away with status 1 "rejected", CHOICE2 "No").
class RejectQuestion : public LayoutScreen {
public:
    RejectQuestion(MenuArt& art, client::BattleSession& s, int slot) : LayoutScreen(art.layout("YESORNO")), art_(art), s_(s), slot_(slot) {
        open_centred_ = -1;
        if (Widget* w = widget("CHOICE1")) set_caption(*w, "Yes");
        if (Widget* w = widget("CHOICE2")) set_caption(*w, "No");
        // ("%s %s?" with "Reject" translated, 0x446150; the caption again)
        if (Widget* w = widget("TITLE")) w->text = translate(translate("Reject") + " " + s.player_name(slot) + "?");
    }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "CHOICE1")) s_.reject_player(slot_, 1);
        if (is(w, "CHOICE1") || is(w, "CHOICE2")) fe.pop();
    }

private:
    MenuArt& art_;
    client::BattleSession& s_;
    int slot_ = 0;
};

// The host's control screen of network games (CONTROL.GUI, 0x4466B0 from the tab bar; flags
// 0x800; not for a watcher). The other players' rows (0x447380 without the local player): PLAYERk
// with the name, LOGOk the colour logo; a row's button brings the kick question (0x446080).
// WATCHING shows and toggles the local side record's watching switch (bit 0x80, "Options",
// announced at once); OK ("Options"): with watching switched off, every remote watcher is sent
// away (status 9).
class ControlScreen : public LayoutScreen {
public:
    ControlScreen(MenuArt& art, client::BattleSession& s) : LayoutScreen(art.layout("CONTROL")), art_(art), s_(s) {
        logos_ = s.content().team_logos;
        const sim::Battle& b = s.battle();
        const int local = b.settings.local_player;
        for (int k = 0; k < 10; ++k) {
            const std::string n = std::to_string(k);
            Widget* player = widget("PLAYER" + n);
            Widget* logo = widget("LOGO" + n);
            for (Widget* w : {player, logo})
                if (w) w->active = false;
            const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
            if (k == local || !p.active || p.controller < 1 || p.controller > 3 || p.state == 10 ||
                b.settings.player_sight[static_cast<std::size_t>(k)].watching || (p.alive_count == 0 && p.units_created != 0))
                continue;
            if (player) {
                player->active = true;
                set_caption(*player, s.player_name(k));
            }
            if (logo) {
                logo->active = true;
                logo->stage = s.content().player_colour[static_cast<std::size_t>(k)];
            }
        }
        show_switches();
        s.client().dim_panel(true);
    }
    ~ControlScreen() override { s_.client().dim_panel(false); }
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd&, render::Image8& out) override {
        const Widget& h = widgets_.front();
        draw_back_tile(art_, out, {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1}, ox_, oy_, h.w, h.h);
    }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        if (!logos_ || w.stage < 0 || w.stage >= static_cast<int>(logos_->size()) || w.name.rfind("LOGO", 0) != 0) return;
        const render::Sprite8& s = (*logos_)[static_cast<std::size_t>(w.stage)];
        const Widget& h = widgets_.front();
        const int x0 = ox_ + w.x, y0 = oy_ + w.y, x1 = x0 + w.w - 1, y1 = y0 + w.h - 1;
        const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
        const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
        render::RasterTarget t;
        t.pixels = out.pixels.data();
        t.pitch = out.width;
        t.clip = {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1};
        render::textured_quad_uv(t, v, uv, s);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (w.name.rfind("PLAYER", 0) == 0 && w.name.size() == 7) {
            const int k = w.name[6] - '0';
            if (k >= 0 && k <= 9 && art_.layout("YESORNO")) fe.push(std::make_unique<RejectQuestion>(art_, s_, k));
            return;
        }
        if (is(w, "WATCHING")) {
            sfx(fe, art_, "Options");
            s_.set_local_options(static_cast<std::uint16_t>(s_.local_options() ^ 0x80));
            show_switches();
            return;
        }
        if (is(w, "OK")) {
            sfx(fe, art_, "Options");
            if (!(s_.local_options() & 0x80)) {
                const sim::Battle& b = s_.battle();
                for (int k = 0; k < 10; ++k)
                    if (b.players[static_cast<std::size_t>(k)].active && b.players[static_cast<std::size_t>(k)].controller == 3 &&
                        b.victory.sides[static_cast<std::size_t>(k)].watching)
                        s_.reject_player(k, 9);
            }
            fe.pop();
        }
    }

private:
    // WATCHING shows bit 0x80 of the local options, GAMEOPEN (display only) bit 0x8000 inverted.
    void show_switches() {
        const std::uint16_t o = s_.local_options();
        if (Widget* w = widget("WATCHING")) w->stage = (o & 0x80) ? 1 : 0;
        if (Widget* w = widget("GAMEOPEN")) w->stage = (o & 0x8000) ? 0 : 1;
    }

    MenuArt& art_;
    client::BattleSession& s_;
    const std::vector<render::Sprite8>* logos_ = nullptr;
};

// The tab bar of network games (TABMENU.GUI, over the top bar): the options menu, and the
// allies, share and control screens (the multiplayer part's). Tab again closes it.
class TabMenu : public LayoutScreen {
public:
    explicit TabMenu(MenuArt& art) : LayoutScreen(art.layout("TABMENU")), art_(art) {}
    bool see_through() const override { return true; }
    // The battle's key handler (0x495E90) runs while the bar is open (only the game menu stops
    // it): Tab closes the bar (0x495010), F2 opens the game menu over it, Esc and the other keys
    // are the battle's; the bar's quick keys and its escdefault never see a key.
    bool battle_keys() const override { return true; }
    bool tab_bar() const override { return true; }
    void enter(FrontEnd& fe) override {
        // Tab opening (and closing) the bar sounds "SmallButton".
        if (!opened_) sfx(fe, art_, "SmallButton");
        opened_ = true;
        // (0x495010: Allies and Share in a network game for a player not watching; Control also
        // needs the host's rights)
        client::BattleSession* bs = fe.session();
        const bool can = bs && bs->network() &&
                         !bs->battle().settings.player_sight[static_cast<std::size_t>(bs->battle().settings.local_player)].watching;
        if (Widget* w = widget("ALLIES")) w->grayed = !fe.hooks().allies_menu || !can;
        if (Widget* w = widget("SHARE")) w->grayed = !fe.hooks().share_menu || !can;
        if (Widget* w = widget("CONTROL")) w->grayed = !fe.hooks().control_menu || !can || !bs->host();
    }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type == client::InputEvent::Type::KeyDown && e.key == client::keys::kTab) {
            sfx(fe, art_, "SmallButton");
            fe.pop();
            return;
        }
        LayoutScreen::handle(fe, e);
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        client::BattleSession* s = fe.session();
        if (is(w, "OPTIONS") || is(w, "ALLIES") || is(w, "SHARE") || is(w, "CONTROL")) sfx(fe, art_, "BigButton");
        if (!s) return;
        if (is(w, "OPTIONS") && art_.layout("ARMOPT")) fe.push(std::make_unique<GameMenu>(art_));
        else if (is(w, "ALLIES") && fe.hooks().allies_menu) fe.push(fe.hooks().allies_menu(fe, *s));
        else if (is(w, "SHARE") && fe.hooks().share_menu) fe.push(fe.hooks().share_menu(fe, *s));
        else if (is(w, "CONTROL") && fe.hooks().control_menu) fe.push(fe.hooks().control_menu(fe, *s));
        else if (is(w, "CANCEL")) fe.pop();
    }

private:
    MenuArt& art_;
    bool opened_ = false;
};

// ------------------------------------------------------------------ end of the battle (ENDMSN.GUI)
// The statistics screen (0x41F0A0 / 0x41E420 / 0x41F400, run by the end state machine
// 0x41F7F0): ENDMSN.GUI over its picture with the victory or defeat title, one row per player
// shown (a colour box with the name over it, then seven bars: kills, losses, energy and metal
// produced, excess energy and metal, score) whose columns are switched on one every ten clock
// ticks and grow by a fifteenth of their value; a key shows them all. Only "Main Menu" is on,
// at y 416. Spec: docs/RE_MEMORY.md, "Client", "End of a battle".
struct EndBar {
    int x = 0, y = 0;
    std::int32_t value = 0, max = 1;
    float step = 1.0f;
    std::int32_t shown = 0;
    bool active = false, animating = true;
    std::int64_t deadline = -1;  // the next step's time (the first comes at once)
};

class BattleOver : public LayoutScreen {
public:
    BattleOver(MenuArt& art, const client::EndTable& table, const std::array<std::uint8_t, 256>& hud, bool network)
        : LayoutScreen(art.layout("ENDMSN")), art_(art), table_(table), network_(network), hud_(hud) {
        // Only the main menu button, moved to y 416 (outside campaigns).
        for (Widget& w : widgets_) w.active = false;
        if (Widget* m = widget("MainMenu")) {
            m->active = true;
            m->y = 0x1a0;
        }
        if (!widgets_.empty()) widgets_.front().active = true;
        if (!layout()) return;
        title_ = table.won && !table.watching ? art.sequence("anims\\IGTITLES.GAF", "igvictory") : art.sequence("anims\\IGTITLES.GAF", "igdefeat");
        logos_ = art.sequence("textures\\logos.gaf", "32xlogos");
        bar_fill_ = hud[4];
        bar_back_ = hud[8];
        static const int cols[7] = {0x70, 0xba, 0x104, 0x14e, 0x198, 0x1e2, 0x22c};
        int y = 0x5d;
        for (const client::EndRow& r : table_.rows) {
            for (int c = 0; c < 7; ++c) {
                EndBar b;
                b.x = cols[c];
                b.y = y;
                b.value = r.values[static_cast<std::size_t>(c)];
                b.max = table_.maxima[static_cast<std::size_t>(c)];
                // A fifteenth of the value per step (single precision), at least 1.
                const float s = static_cast<float>(static_cast<double>(b.value) * static_cast<double>(1.0f / 15.0f * 1.0f));
                b.step = s <= 1.0f ? 1.0f : s;
                bars_.push_back(b);
            }
            row_y_.push_back(y);
            y += 0x14;
        }
        shown_ = bars_;
    }
    // The battle's last frame and its palette: the end begins by darkening it (the state
    // machine's first states, 0x41F7F0 0-4: a frame for the switch, ten steps one tick apart
    // after the first tick, each putting the whole screen through shade levels 13, 12, .. 4 in
    // turn, two more frames, then a black frame while the statistics screen is set up).
    void begin_fade(render::Image8 last, const render::Palette& palette) {
        last_ = std::move(last);
        battle_palette_ = palette;
        phase_ = 0;
    }
    // A network game left by this machine's player for `reason` (its record's leaving byte).
    void set_leaving_reason(int reason) {
        if (reason != 0 && reason != 2) reject_text_ = leave_reason_text(reason);
    }
    // A campaign mission (0x41F0A0 / 0x41F400 with the campaign buttons, 0x41DA60 the glamour).
    void set_campaign(CampaignEnd c) {
        campaign_ = true;
        camp_ = std::move(c);
        if (!camp_.buttons) return;
        // Start, Load Game, Save Game, the knob, Missions, Difficulty, its label and Main Menu at
        // its own place; outcome1; the mission list with each mission's mark (0xFF lost, 0xFE won,
        // 0xFD not played, then a space), the mission played selected, then the next one after a
        // win; the Difficulty switch at the setting; Enter starts; the focus on the list.
        for (const char* n : {"Start", "LoadGame", "SaveGame", "KNOB", "Missions", "Difficulty", "AdjustDiff", "MainMenu"})
            if (Widget* w = widget(n)) w->active = true;
        if (const client::ScreenLayout* L = layout())
            for (const Widget& d : L->widgets)
                if (is(d, "MainMenu"))
                    if (Widget* m = widget("MainMenu")) m->y = d.y;
        background_ = art_.picture("outcome1", &camp_palette_);
        std::vector<std::string> items;
        for (std::size_t k = 0; k < camp_.missions.size(); ++k) {
            const char r = k < camp_.results.size() ? camp_.results[k] : 'U';
            std::string item;
            if (r == 'L') item = "\xFF ";
            else if (r == 'W') item = "\xFE ";
            else if (r == 'U') item = "\xFD ";
            items.push_back(item + camp_.missions[k]);
        }
        if (Widget* l = widget("Missions")) {
            set_list(*l, items);
            if (camp_.mission < static_cast<int>(items.size())) select_list_row(*l, camp_.mission);
            const int next = camp_.mission + (camp_.won ? 1 : 0);
            if (next < static_cast<int>(items.size())) select_list_row(*l, next);
        }
        if (Widget* d = widget("Difficulty")) d->stage = std::clamp(camp_.difficulty, 0, 2);
        enter_default_ = "Start";
        focus_missions_ = true;
    }
    const render::Palette* palette() const override {
        if (phase_ >= 0 && phase_ <= 5) return &battle_palette_;
        if (glamour_ != 0) return &glam_now_;
        // (0x41F0A0 puts outcome1 on the screen with its palette, also after the glamour)
        if (campaign_ && camp_.buttons && camp_palette_) return camp_palette_;
        return LayoutScreen::palette();
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        if (phase_ >= 0 && phase_ <= 5) {
            if (last_.width == out.width && last_.height == out.height) out.pixels = last_.pixels;
            return;
        }
        if (glamour_ != 0) {
            // The faded last frame with the glamour picture copied over it at (0, 0), all on the
            // palette fading in; "Click to continue." (outlined, centred, 20 above the bottom).
            if (last_.width == out.width && last_.height == out.height) out.pixels = last_.pixels;
            render::blit(out, out.bounds(), *camp_.glamour, camp_.glamour->x_offset, camp_.glamour->y_offset);
            if (glam_text_ && camp_.click_font) {
                const client::Font& f = *camp_.click_font;
                const std::string t = translate("Click to continue.");  // (0x41FF1A)
                const int x = (out.width - f.text_width(t)) >> 1, y = out.height - 20;
                for (const auto& d : {std::array<int, 2>{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) client::draw_text(out, out.bounds(), f, t, x + d[0], y + d[1], hud_[0]);
                client::draw_text(out, out.bounds(), f, t, x, y, hud_[15]);
            }
            return;
        }
        if (focus_missions_ && !placed_) {
            place(out.width, out.height);
            for (std::size_t i = 0; i < widgets_.size(); ++i)
                if (is(widgets_[i], "Missions")) focus_gadget_ = static_cast<int>(i);
        }
        if (phase_ == 6) {  // the set-up frame: the screen cleared
            std::fill(out.pixels.begin(), out.pixels.end(), std::uint8_t{0});
            return;
        }
        LayoutScreen::draw(fe, out);
    }
    void frame(FrontEnd& fe, std::uint32_t now) override {
        const std::uint32_t t = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) * 30 / 1000);
        if (phase_ >= 0 && phase_ <= 5) {
            switch (phase_) {
            case 2:  // ten fade steps, the first after the next tick
                fade_left_ = 10;
                fade_deadline_ = static_cast<std::int64_t>(t) + 1;
                phase_ = 3;
                return;
            case 3:
                if (fade_left_ == 0) {
                    phase_ = 4;
                    return;
                }
                if (static_cast<std::int64_t>(t) > fade_deadline_) {
                    const std::vector<std::uint8_t>& shade = fe.menu_content().shade;
                    if (shade.size() >= 0x2000) {
                        const std::size_t row = static_cast<std::size_t>(fade_left_ + 3) * 256;
                        for (std::uint8_t& p : last_.pixels) p = shade[row + p];
                    }
                    fade_deadline_ = static_cast<std::int64_t>(t) + 1;
                    --fade_left_;
                }
                return;
            case 5:  // the statistics screen's set-up
                if (campaign_ && camp_.final_win) {
                    // The campaign's last mission won: its ending movies (on a full screen), then
                    // the main menu.
                    phase_ = 6;
                    done_ = true;
                    if (art_.full_screen) play_movies(fe, art_, camp_.final_movies, false, [](FrontEnd& f) { f.to_main_menu(); });
                    else fe.to_main_menu();
                    return;
                }
                if (campaign_ && camp_.won && camp_.glamour && camp_.glamour_palette) {
                    // The glamour picture on a black palette fading to the picture's: each step
                    // (one a tick) moves every component by a fifth of its distance (at least 1).
                    glamour_ = 1;
                    glam_now_ = render::Palette{};
                    for (int i = 0; i < 256; ++i) {
                        const render::Rgb& d = (*camp_.glamour_palette)[static_cast<std::size_t>(i)];
                        const int comp[3] = {d.r, d.g, d.b};
                        for (int k = 0; k < 3; ++k) {
                            const int diff = comp[k] / 5;
                            glam_delta_[static_cast<std::size_t>(i * 3 + k)] = comp[k] == 0 ? 0 : std::max(diff, 1);
                        }
                    }
                    glam_next_ = static_cast<std::int64_t>(t) + 1;
                    phase_ = 10;
                    return;
                }
                set_up_ = true;
                phase_ = 6;
                return;
            default:  // 0 the battle's last pass, 1 the switch to the end states, 4 one more state
                if (phase_ == 1 && !reject_text_.empty()) {
                    // 0x41F7F0 case 0 of a network game: the local record's leaving byte other
                    // than 0 and 2 (left on its own) shows why (0x452C40, translated) in a 320-wide
                    // box (OK, fitted) over the copy of the last frame, which is put back each
                    // pass with the console's lines (case 1, 0x464060: not dimmed); the end
                    // goes on once it is closed.
                    auto box = std::make_unique<MessageBox>(art_, translate(reject_text_), nullptr, 320);
                    box->set_no_dim();
                    reject_text_.clear();
                    if (art_.layout("MSGBOX")) {
                        fe.push(std::move(box));
                        return;
                    }
                }
                ++phase_;
                return;
            }
        }
        if (phase_ == 10) {
            // State 6: the fade, then the glamour sound; a second after the fade a key or a held
            // button goes on to the statistics; five seconds more and "Click to continue." shows.
            if (glamour_ == 1) {
                if (static_cast<std::int64_t>(t) >= glam_next_) {
                    bool same = true;
                    for (int i = 0; i < 256; ++i) {
                        render::Rgb& c = glam_now_[static_cast<std::size_t>(i)];
                        const render::Rgb& d = (*camp_.glamour_palette)[static_cast<std::size_t>(i)];
                        std::uint8_t* now[3] = {&c.r, &c.g, &c.b};
                        const int want[3] = {d.r, d.g, d.b};
                        for (int k = 0; k < 3; ++k) {
                            const int v = std::min(*now[k] + glam_delta_[static_cast<std::size_t>(i * 3 + k)], want[k]);
                            *now[k] = static_cast<std::uint8_t>(v);
                            same = same && v == want[k];
                        }
                    }
                    if (same) glamour_ = 2;
                    glam_next_ = static_cast<std::int64_t>(t) + 1;
                }
                glam_deadline_ = static_cast<std::int64_t>(t) + 30;
                return;
            }
            if (!glam_sound_) {
                glam_sound_ = true;
                if (camp_.glamour_sound) fe.play_narration_sample(camp_.glamour_sound.get());
            }
            if (static_cast<std::int64_t>(t) > glam_deadline_) {
                if (glam_input_ || mouse_held_) {
                    fe.stop_narration();
                    glamour_ = 0;
                    glam_input_ = false;
                    set_up_ = true;
                    phase_ = 7;
                    return;
                }
                if (static_cast<std::int64_t>(t) > glam_deadline_ + 150) glam_text_ = true;
            }
            return;
        }
        if (phase_ == 6) phase_ = 7;
        if (!set_up_) {
            set_up_ = true;  // the state machine's set-up frame
            return;
        }
        if (done_) return;
        // Bars still off or growing: the interface pass (each growing bar whose time has come
        // takes its step; the next step one tick later), the frame shown, then the columns.
        bool pending = false;
        for (const EndBar& b : bars_) pending = pending || !b.active || b.shown < b.value;
        if (!pending) {
            done_ = true;
            return;
        }
        for (EndBar& b : bars_) {
            if (!b.active || !b.animating || b.shown >= b.value || static_cast<std::int64_t>(t) <= b.deadline) continue;
            b.shown += static_cast<std::int32_t>(b.step);
            if (b.shown > b.value) {
                b.shown = b.value;
                b.animating = false;
            }
            b.deadline = static_cast<std::int64_t>(t) + 1;
        }
        shown_ = bars_;
        const bool all = key_ && !network_;
        key_ = false;
        if (static_cast<std::int64_t>(t) <= deadline_ && !all) return;
        if (all) {
            for (EndBar& b : bars_) b.active = true;
            fe.play_sound(art_.sound("ActivateAllStatBars"));
        }
        if (column_ < 7) {
            for (std::size_t k = static_cast<std::size_t>(column_); k < bars_.size(); k += 7) bars_[k].active = true;
            fe.play_sound(art_.sound(column_ == 6 ? "EndGameScore" : "EndGameStatBar"));
        }
        deadline_ = static_cast<std::int64_t>(t) + 10;
        ++column_;
    }

protected:
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        if (phase_ == 10) {
            // The glamour picture: a key waits in the key queue; the button counts while held.
            if (e.type == client::InputEvent::Type::KeyDown) glam_input_ = true;
            if (e.type == client::InputEvent::Type::MouseDown) mouse_held_ = true;
            if (e.type == client::InputEvent::Type::MouseUp) mouse_held_ = false;
            return;
        }
        // A key the interface does not take shows every bar at once (while they are coming).
        if (e.type == client::InputEvent::Type::KeyDown && set_up_ && !done_) {
            const Widget* m = widget("MainMenu");
            if (!(m && m->quickkey && m->quickkey == e.key)) {
                key_ = true;
                return;
            }
        }
        LayoutScreen::handle(fe, e);
    }
    void draw_under(FrontEnd&, render::Image8& out) override {
        // The title, put on the screen's own surface at half the screen's width, 28 down.
        if (title_ && !title_->empty()) render::blit(out, clip(), title_->front(), ox_ + out.width / 2, oy_ + 0x1c);
    }
    void draw_over(FrontEnd& fe, render::Image8& out) override {
        const auto& mc = fe.menu_content();
        const render::ClipRect c = clip();
        for (std::size_t r = 0; r < row_y_.size(); ++r) {
            const int y = oy_ + row_y_[r];
            // The colour box: the player's logo frame stretched over 91 x 21 (texels 1 .. size-1).
            if (logos_ && !logos_->empty()) {
                const render::Sprite8& s = (*logos_)[std::min<std::size_t>(static_cast<std::size_t>(table_.rows[r].colour), logos_->size() - 1)];
                const int x0 = ox_ + 0x10, x1 = x0 + 0x5b - 1, y1 = y + 0x15 - 1;
                const render::RasterVertex v[4] = {{x0, y, 0}, {x1, y, 0}, {x1, y1, 0}, {x0, y1, 0}};
                const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
                render::RasterTarget rt;
                rt.pixels = out.pixels.data();
                rt.pitch = out.width;
                rt.clip = c;
                render::textured_quad_uv(rt, v, uv, s);
            }
            // The name: the small font, centred in 90 pixels, halfway down the row, its glyphs
            // lit through light level 15 (the label's colour value).
            if (const client::Font* lf = mc.small_font ? mc.small_font : mc.font) {
                const std::string& n = table_.rows[r].name;
                const int ly = y + (0x14 - lf->height) / 2;
                const int tx = ox_ + 0x10 + 0x5a / 2 - lf->text_width(n) / 2;
                draw_lit_text(out, c, *lf, n, tx, ly, 0x5a, mc.light, 15);
            }
            for (std::size_t k = 0; k < 7; ++k) {
                const EndBar& b = shown_[r * 7 + k];
                if (b.active) draw_bar(fe, out, b);
            }
        }
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "MainMenu") || is(w, "LoadGame") || is(w, "SaveGame") || is(w, "Start") || is(w, "Missions")) sfx(fe, art_, "BigButton");
        else if (is(w, "Difficulty")) sfx(fe, art_, "Skirmish");
        if (is(w, "MainMenu")) {
            fe.to_main_menu();
        } else if (campaign_ && (is(w, "Start") || is(w, "Missions"))) {
            // (0x41EC50: the archives rescanned, the chosen mission's briefing)
            const Widget* l = widget("Missions");
            const int sel = l && l->selected >= 0 ? l->selected : camp_.mission;
            if (camp_.start) camp_.start(fe, sel);
        } else if (campaign_ && is(w, "LoadGame")) {
            if (camp_.load) camp_.load(fe);
        } else if (campaign_ && is(w, "SaveGame")) {
            if (camp_.save) camp_.save(fe);
        } else if (campaign_ && is(w, "Difficulty")) {
            camp_.difficulty = w.stage;
            if (camp_.set_difficulty) camp_.set_difficulty(w.stage);
            fe.settings().set_int("Difficulty", w.stage);
        }
    }
    void on_pick(FrontEnd& fe, Widget& w, int, bool double_click) override {
        if (campaign_ && double_click && is(w, "Missions")) on_press(fe, w);
    }

private:
    render::ClipRect clip() const {
        const Widget& h = widgets_.front();
        return {ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1};
    }
    // Text whose glyph pixels go through a light level (0x4B8310's remap: light[level][p]); the
    // glyphs that fit in `width` from the left (a glyph wider than what is left ends it).
    static void draw_lit_text(render::Image8& out, const render::ClipRect& c, const client::Font& f, const std::string& s, int x, int y, int width,
                              const std::vector<std::uint8_t>& light, int level) {
        for (const unsigned char ch : s) {
            const client::Glyph& g = f.glyphs[ch];
            if (ch < 0x20 || !g.present) continue;
            if (width >= 0 && width < g.width) return;
            const int rows = g.rows > 0 ? g.rows : f.height;
            if (ch != ' ')
                for (int r = 0; r < rows; ++r)
                    for (int k = 0; k < g.width; ++k) {
                        const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                        if (at >= g.ink.size() || !g.ink[at]) continue;
                        std::uint8_t v = g.colours.empty() ? 15 : g.colours[at];
                        if (light.size() >= 0x2000) v = light[static_cast<std::size_t>(level) * 256 + v];
                        client::put_pixel(out, c, x + g.left + k, y + g.top + r, v);
                    }
            if (width >= 0 && (width -= g.width) < 0) return;
            x += g.width;
        }
    }
    // One bar (0x4A4660): a raised box (x .. x + w, y .. y + h: filled with interface colour
    // 20, two-pixel edges in 0 top / left and 17 bottom / right), inside it (2 pixels in) the
    // background colour, then the filled part from the left: trunc(shown / max x (w - 4)) + 1
    // columns, and the number shown centred in the standard font.
    void draw_bar(FrontEnd& fe, render::Image8& out, const EndBar& b) {
        const render::ClipRect c = clip();
        const int x0 = ox_ + b.x, y0 = oy_ + b.y, x1 = x0 + 0x43, y1 = y0 + 0x12;
        const std::uint8_t c0 = colour(0), c17 = colour(17), c20 = colour(20);
        client::fill_rect(out, c, x0, y0, x1, y1, c20);
        auto line = [&](int ax, int ay, int bx, int by, std::uint8_t col) { render::draw_line(out, c, ax, ay, bx, by, col); };
        line(x0, y0, x1, y0, c0);
        line(x0, y0 + 1, x1 - 1, y0 + 1, c0);
        line(x0, y0, x0, y1, c0);
        line(x0 + 1, y0, x0 + 1, y1 - 1, c0);
        line(x1, y0 + 1, x1, y1, c17);
        line(x1 - 1, y0 + 2, x1 - 1, y1, c17);
        line(x0 + 1, y1, x1, y1, c17);
        line(x0 + 2, y1 - 1, x1, y1 - 1, c17);
        client::fill_rect(out, c, x0 + 2, y0 + 2, x1 - 2, y1 - 2, bar_back_);
        const double f = static_cast<double>(b.shown) / static_cast<double>(b.max) * static_cast<double>(0x43 - 4);
        // (the game's conversion: truncated to 64 bits, the low half kept)
        const int len = f > -9223372036854775808.0 && f < 9223372036854775808.0
                            ? static_cast<int>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(f))))
                            : 0;
        client::fill_rect(out, c, x0 + 2, y0 + 2, x0 + 2 + len, y1 - 2, bar_fill_);
        const auto& mc = fe.menu_content();
        if (const client::Font* f2 = mc.small_font ? mc.small_font : mc.font) {
            const std::string n = std::to_string(b.shown);
            const int tx = x0 + 0x43 / 2 - f2->text_width(n) / 2, ty = y0 + 0x12 / 2 - f2->height / 2;
            client::draw_text(out, c, *f2, n, tx, ty + f2->raise, 15);
        }
    }

    MenuArt& art_;
    client::EndTable table_;
    bool network_ = false;
    std::vector<EndBar> bars_, shown_;  // the bars, and as the last frame showed them
    std::vector<int> row_y_;
    const std::vector<render::Sprite8>* title_ = nullptr;
    const std::vector<render::Sprite8>* logos_ = nullptr;
    std::uint8_t bar_fill_ = 0, bar_back_ = 0;
    bool set_up_ = false, done_ = false, key_ = false;
    std::int64_t deadline_ = -1;  // the next column's time (the first comes at once)
    int column_ = 0;
    // The fade (phase -1: none; 0-5 the battle's last frame, 6 the cleared set-up frame).
    render::Image8 last_;
    render::Palette battle_palette_{};
    int phase_ = -1, fade_left_ = 0;
    std::string reject_text_;  // why this machine's player was sent away (shown at the end)
    std::int64_t fade_deadline_ = 0;
    // A campaign mission's end (set_campaign): its buttons, the glamour picture's fade.
    std::array<std::uint8_t, 256> hud_{};
    bool campaign_ = false;
    CampaignEnd camp_;
    const render::Palette* camp_palette_ = nullptr;
    int glamour_ = 0;  // 0 none, 1 fading in, 2 shown
    render::Palette glam_now_{};
    std::array<int, 768> glam_delta_{};
    std::int64_t glam_next_ = 0, glam_deadline_ = 0;
    bool glam_sound_ = false, glam_input_ = false, glam_text_ = false, mouse_held_ = false;
    bool focus_missions_ = false;
};

// A session's end table (from its statistics when it has no table of its own).
client::EndTable end_table_of(client::BattleSession& s, client::BattleOutcome o) {
    client::EndTable t;
    if (s.end_table(t)) return t;
    std::array<client::EndPlayer, 10> in{};
    const std::vector<client::PlayerStats> st = s.stats();
    for (std::size_t i = 0; i < st.size() && i < in.size(); ++i) {
        client::EndPlayer& p = in[i];
        p.active = true;
        p.controller = 1;
        p.colour = st[i].colour;
        p.kills = static_cast<std::int16_t>(st[i].units_killed);
        p.losses = static_cast<std::int16_t>(st[i].units_lost);
        p.energy_produced = st[i].energy_produced;
        p.metal_produced = st[i].metal_produced;
        p.energy_wasted = st[i].energy_wasted;
        p.metal_wasted = st[i].metal_wasted;
        p.name = st[i].name;
    }
    t = client::end_table(in, s.battle().game_tick(), 0.0f, 0.0f);
    t.won = o == client::BattleOutcome::Victory;
    return t;
}

}  // namespace

std::unique_ptr<client::Screen> make_end_screen(MenuArt& art, const client::EndTable& table, const CampaignEnd* campaign) {
    if (!art.layout("ENDMSN")) return nullptr;
    std::array<std::uint8_t, 256> hud{};
    for (int k = 0; k < 256; ++k) hud[static_cast<std::size_t>(k)] = art.gui_colour(art.content().palette, k);
    auto bo = std::make_unique<BattleOver>(art, table, hud, false);
    if (campaign) bo->set_campaign(*campaign);
    return bo;
}

void make_front_end(MenuArt& art, client::MusicPlayer* music, client::FrontEndSetup& setup) {
    setup.content = &art.content();
    setup.music = music;
    setup.main_menu = [&art](FrontEnd&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("MAINMENU")) return nullptr;
        return std::make_unique<MainMenu>(art);
    };
    setup.draw_loading = [&art](FrontEnd&, render::Image8& out, client::LoadingState& st) { draw_loading_screen(art, out, st); };
    setup.battle_over = [&art](FrontEnd&, client::BattleSession& s, client::BattleOutcome o) -> std::unique_ptr<client::Screen> {
        if (!art.layout("ENDMSN")) return nullptr;
        auto bo = std::make_unique<BattleOver>(art, end_table_of(s, o), s.content().hud, s.network());
        CampaignEnd ce;
        if (campaign_end(art, s, o, ce)) bo->set_campaign(std::move(ce));
        // The battle's last frame (without the pointer) for the fade.
        client::GameClient& c = s.client();
        c.set_pointer_hidden(true);
        bo->begin_fade(render::execute(c.draw(), s.content().blend), s.content().palette);
        if (s.network()) bo->set_leaving_reason(s.local_leaving_reason());
        return bo;
    };
    setup.game_menu = [&art](FrontEnd&, client::BattleSession&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("ARMOPT")) return nullptr;
        return std::make_unique<GameMenu>(art);
    };
    // Closing the window in a battle (the battle state's close handler 0x4609A0): the exit
    // question "Surrender this battle and exit to Windows?" over whatever is open.
    setup.close_prompt = [&art](FrontEnd&, client::BattleSession&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("YESORNO")) return nullptr;
        return std::make_unique<YesOrNo>(art, 2);
    };
    setup.tab_menu = [&art](FrontEnd&, client::BattleSession&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("TABMENU")) return nullptr;
        return std::make_unique<TabMenu>(art);
    };
    setup.hooks.share_menu = [&art](FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        // (refused to a watcher; with nobody to give to it closes at once)
        const sim::Battle& b = s.battle();
        if (!s.network() || !art.layout("SHARE") || b.settings.player_sight[static_cast<std::size_t>(b.settings.local_player)].watching) return nullptr;
        auto sc = std::make_unique<ShareScreen>(art, s);
        if (sc->empty()) return nullptr;
        return sc;
    };
    setup.hooks.allies_menu = [&art](FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        if (!s.network() || !art.layout("ALLIES")) return nullptr;
        return std::make_unique<AlliesScreen>(art, s);
    };
    setup.hooks.chat_panel = [&art](FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        if (!art.layout(s.chat_panel ? "TALK2" : "TALK")) return nullptr;
        return std::make_unique<ChatPanelScreen>(art, s);
    };
    setup.hooks.timeout_dialog = [&art](FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        if (!s.network() || !art.layout("TIMEOUT") || s.timed_out_player() < 0) return nullptr;
        return std::make_unique<TimeoutDialog>(art, s);
    };
    setup.hooks.enumerate_units = [&art](FrontEnd& fe) {
        // 0x42A8D0's end: definitions dropped for their version alone (none for a wrong copyright
        // line or a loose file under the CD check): the system's box "Error" with the text
        // translated (0x42B310 -> 0x4B6B80).
        UnitDefContext ctx;
        load_unit_context(art.vfs(), ctx);
        if (unit_files_incompatible_notice(art.vfs(), ctx))
            fe.system_message("Error", translate("Incompatible units found.  They will be ignored.  Please download the latest version of the game."));
    };
    setup.hooks.watch_dialog = [&art](FrontEnd&, client::BattleSession& s, int kind) -> std::unique_ptr<client::Screen> {
        if (kind == 1) {
            if (!art.layout("YESORNO")) return nullptr;
            return std::make_unique<WatchQuestion>(art, s);
        }
        // (0x465803: 500 wide, OK, fitted, translated)
        if (!art.layout("MSGBOX")) return nullptr;
        auto box = std::make_unique<MessageBox>(
            art, translate("You are placed in watch mode because you are hosting AI players which are still alive.  If you exit, they will be terminated."),
            nullptr, 500);
        box->over_battle(s.client());
        return box;
    };
    setup.hooks.control_menu = [&art](FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        const sim::Battle& b = s.battle();
        if (!s.network() || !s.host() || !art.layout("CONTROL") ||
            b.settings.player_sight[static_cast<std::size_t>(b.settings.local_player)].watching)
            return nullptr;
        return std::make_unique<ControlScreen>(art, s);
    };
    setup.hooks.unit_info = [&art](FrontEnd&, client::BattleSession& s, std::uint16_t type) -> std::unique_ptr<client::Screen> {
        if (!art.layout("UNITINFOX")) return nullptr;
        return std::make_unique<UnitInfo>(art, s, type);
    };
    setup.apply_to_battle = [](FrontEnd& fe, client::BattleSession& s) {
        // Options changed in a battle reach its client.
        client::GameClient& c = s.client();
        const client::ClientOptions o = client_options(fe.settings(), fe.width(), fe.height());
        client::ClientOptions& cur = c.options_mut();
        cur.scroll_speed = o.scroll_speed;
        cur.right_click_orders = o.right_click_orders;
        cur.text_lines = o.text_lines;
        cur.text_seconds = o.text_seconds;
        cur.unit_chat = o.unit_chat;
        cur.unit_chat_text = o.unit_chat_text;
        cur.speech = o.speech;
        cur.alt_groups = o.alt_groups;
        cur.clock = o.clock;
        cur.sound_mode = o.sound_mode;
        cur.fx_volume = o.fx_volume;
        if (o.game_speed != static_cast<int>(c.budget().game_speed)) c.set_game_speed(o.game_speed);
    };
}

// For the multiplayer screens (client_net_screens.cpp): the options menu, the stock panel and
// the dimming under a dialog.
std::unique_ptr<client::Screen> make_options_screen(MenuArt& art) { return std::make_unique<OptionsMenu>(art, false); }
void stock_panel(MenuArt& art, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int w, int h) {
    draw_back_tile(art, out, clip, x0, y0, w, h);
}
void dim_screen_rect(client::FrontEnd& fe, render::Image8& out, int x0, int y0, int x1, int y1) { dim_rect(fe, out, x0, y0, x1, y1); }
std::unique_ptr<client::Screen> make_room_map_choice(MenuArt& art, const std::string& current, std::function<void(const std::string&)> chosen) {
    return std::make_unique<SelectMap>(art, current, std::move(chosen), true);
}
std::unique_ptr<client::Screen> make_room_map_view(MenuArt& art, std::function<std::string()> host_map) {
    return std::make_unique<ViewMap>(art, std::move(host_map));
}

}  // namespace ta
