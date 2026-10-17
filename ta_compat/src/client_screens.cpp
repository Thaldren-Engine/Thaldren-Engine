// The original game's screens on the client's front end (spec: docs/RE_MEMORY.md,
// "Client", "Menus"): main menu, single player, skirmish set-up and map choice, options,
// loading, the in-game menus, the end of a battle.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "client/canvas.h"
#include "render/raster.h"
#include "sim/battle.h"
#include "sim/economy.h"
#include "sim/rng.h"
#include "sim/victory.h"
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

// The menus' rand() stream (the main menu's sparks draw from it; what is left of it is the
// simulation thread's rand() state at the start of a battle, as in the original).
sim::Lcg15& menu_rand() {
    static sim::Lcg15 r;
    return r;
}

// ------------------------------------------------------------------ message box (MSGBOX.GUI)
class MessageBox : public LayoutScreen {
public:
    MessageBox(MenuArt& art, std::string text) : LayoutScreen(art.layout("MSGBOX")), text_(std::move(text)) {}
    bool see_through() const override { return true; }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "OK")) fe.pop();
    }
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        // A plain panel with the text broken into lines.
        const auto& mc = fe.menu_content();
        const Widget& h = widgets_.front();
        client::fill_rect(out, out.bounds(), ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1, mc.box_colour);
        client::outline_rect(out, out.bounds(), ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1, mc.text_colour);
        if (!mc.font) return;
        std::vector<std::string> lines;
        std::string line;
        for (std::size_t i = 0; i <= text_.size(); ++i) {
            const bool end = i == text_.size() || text_[i] == ' ';
            if (!end) continue;
            std::size_t start = 0;
            for (std::size_t k = i; k-- > 0;)
                if (text_[k] == ' ') {
                    start = k + 1;
                    break;
                }
            const std::string word = text_.substr(start, i - start);
            const std::string trial = line.empty() ? word : line + " " + word;
            if (mc.font->text_width(trial) > h.w - 30 && !line.empty()) {
                lines.push_back(line);
                line = word;
            } else {
                line = trial;
            }
        }
        if (!line.empty()) lines.push_back(line);
        int y = oy_ + 30;
        for (const std::string& l : lines) {
            client::draw_text(out, out.bounds(), *mc.font, l, ox_ + (h.w - mc.font->text_width(l)) / 2, y, mc.text_colour);
            y += mc.font->height + 2;
        }
    }

private:
    std::string text_;
};

void show_message(FrontEnd& fe, MenuArt& art, const std::string& text) {
    if (art.layout("MSGBOX")) fe.push(std::make_unique<MessageBox>(art, text));
    else std::printf("message: %s\n", text.c_str());
}

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
                else if (is(w, "GAMMA")) set(w, 20, s.get_int("Gamma", 12));
            } else if (w.kind == WidgetKind::Button && w.stages > 0) {
                if (is(w, "SPEECH")) w.stage = s.get_int("speechfx", 1) ? std::clamp(s.get_int("unitchat", 10) / 5, 0, 2) : 0;
                else if (is(w, "MODE")) w.stage = std::clamp(s.get_int("Sound Mode", 1), 0, 2);
                else if (is(w, "UNITCHAT")) w.stage = std::clamp(s.get_int("unitchattext", 5) / 5, 0, 2);
                else if (is(w, "LEFTCLICK")) w.stage = std::clamp(s.get_int("Interface Type", 0), 0, 1);
                else if (is(w, "NOTRAK")) w.stage = s.get_int("musicmode", 1) ? 1 : 0;
                else if (is(w, "TRACKMODE")) w.stage = std::clamp(s.get_int("cdmode", 4) - 1, 0, 3);
                else if (is(w, "TRACKTYPE") && m) w.stage = m->types()[static_cast<std::size_t>(std::clamp(track_, 0, m->track_count()))];
                else if (is(w, "SHADING")) w.stage = s.get_int("Shading", 1) ? 1 : 0;
                else if (is(w, "ANTI")) w.stage = s.get_int("Anti Alias", 1) ? 1 : 0;
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
            s.set_int("Anti Alias", w.stage);
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
            fe.play_sound(art_.sound("OKTOBUILD"));
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
            if (is(w, "ANTI")) s.set_int("Anti Alias", 1);
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
        }
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
    client::Settings saved_;
    std::unique_ptr<OptionsPage> page_;
};
// ------------------------------------------------------------------ select map (SELMAP.GUI)
class SelectMap : public LayoutScreen {
public:
    SelectMap(MenuArt& art, std::string current, std::function<void(const std::string&)> chosen)
        : LayoutScreen(art.layout("SELMAP")), art_(art), chosen_(std::move(chosen)) {
        Widget* list = widget("MAPNAMES");
        if (list) {
            list->items = art.skirmish_maps();
            for (std::size_t i = 0; i < list->items.size(); ++i)
                if (upper(list->items[i]) == upper(current)) list->selected = static_cast<int>(i);
            if (list->selected < 0 && !list->items.empty()) list->selected = 0;
            const int rows = std::max(1, list->h / ((art.content().font ? art.content().font->height : 12) + 1));
            list->top = std::max(0, std::min(list->selected - rows / 2, static_cast<int>(list->items.size()) - rows));
            if (Widget* s = widget("SLIDER")) {
                s->range = std::max(0, static_cast<int>(list->items.size()) - rows);
                s->position = list->top;
                s->link = "MAPNAMES";
            }
        }
        describe();
    }
    bool see_through() const override { return true; }

protected:
    void describe() {
        const Widget* list = widget("MAPNAMES");
        if (!list || list->selected < 0) return;
        const std::string map = list->items[static_cast<std::size_t>(list->selected)];
        std::vector<std::uint8_t> f;
        std::string desc, size;
        if (art_.vfs().read("maps\\" + map + ".ota", f))
            if (auto root = tdf_parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size())).root)
                if (const TdfSection* gh = root->child("GlobalHeader")) {
                    desc = gh->get_string("missiondescription", 256, "");
                    size = gh->get_string("size", 64, "");
                }
        if (Widget* d = widget("DESCRIPTION")) d->text = desc;
        if (Widget* s = widget("SIZE")) s->text = size;
        if (Widget* pic = widget("MAPPIC")) pic->picture = art_.map_preview(map, pic->w, pic->h);
    }
    void on_pick(FrontEnd& fe, Widget& w, int row, bool dbl) override {
        (void)row;
        describe();
        if (dbl) choose(fe, w);
    }
    void choose(FrontEnd& fe, Widget& list) {
        if (list.selected >= 0 && chosen_) chosen_(list.items[static_cast<std::size_t>(list.selected)]);
        fe.pop();
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "LOAD")) {
            if (Widget* l = widget("MAPNAMES")) choose(fe, *l);
        } else if (is(w, "PREVMENU")) {
            fe.pop();
        }
    }
    void draw_surface(FrontEnd&, render::Image8& out, const Widget& w) override {
        if (w.picture) render::blit(out, out.bounds(), *w.picture, ox_ + w.x + w.picture->x_offset, oy_ + w.y + w.picture->y_offset);
    }

private:
    MenuArt& art_;
    std::function<void(const std::string&)> chosen_;
};

// ------------------------------------------------------------------ skirmish (SKIRMISH.GUI)
class Skirmish : public LayoutScreen {
public:
    Skirmish(MenuArt& art, client::Settings& s) : LayoutScreen(art.layout("SKIRMISH")), art_(art) {
        load(s);
        build_rows();
    }
    void enter(FrontEnd& fe) override {
        if (Widget* m = widget("MapName")) m->text = map_;
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
        commander_ends_ = s.get_int("SkirmishCommanderDeath", 1);
        mapping_ = s.get_int("SkirmishMapping", 1);
        los_ = s.get_int("SkirmishLineOfSight", 1);
        los_type_ = s.get_int("SkirmishLOSType", 1);
        difficulty_ = s.get_int("SkirmishDifficulty", 1) & 0xFFFF;
        fixed_ = s.get_int("SkirmishLocation", 1);
        map_ = s.get_text("SkirmishMap", "");
        const auto& maps = art_.skirmish_maps();
        bool found = false;
        for (const auto& m : maps) found = found || upper(m) == upper(map_);
        if (!found) map_ = maps.empty() ? std::string("Moon Quartet") : maps.front();
    }
    void save(client::Settings& s) {
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
                help_[wd.name] = help;
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
                set_caption(*w, r.kind == 1 ? "Player" : r.kind == 2 ? "Computer" : "Open");
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
                help_[upper(name)] = help;
            }
        };
        stage("StartLocation", fixed_ ? 0 : 1,
              fixed_ ? "Commanders are placed at pre-determined locations." : "Commanders are randomly placed on the map.");
        stage("CommanderDeath", commander_ends_ ? 0 : 1,
              commander_ends_ ? "Game ends when commander is destroyed." : "Game continues after Commander is destroyed.");
        stage("Mapping", mapping_ ? 0 : 1, mapping_ ? "Terrain is blacked out until explored." : "Terrain is visible.");
        stage("LineOfSight", !los_ ? 0 : los_type_ == 1 ? 1 : 2,
              !los_ ? "All mapped terrain is visible." : los_type_ == 1 ? "Terrain elevations affect a unit's view."
                                                                       : "Terrain elevations do not affect a unit's view.");
        stage("Difficulty", difficulty_, "");
        if (Widget* m = widget("MapName")) m->text = map_;
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
            if (Widget* t = widget("HELPTEXT")) t->text = h;
        }
        LayoutScreen::handle(fe, e);
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
            fe.push(std::make_unique<SelectMap>(art_, map_, [this](const std::string& m) { map_ = m; }));
        } else if (is(w, "PrevMenu")) {
            save(fe.settings());
            fe.apply_settings();
            fe.pop();
            return;
        } else if (is(w, "Start")) {
            start(fe);
            return;
        }
        refresh();
        save(fe.settings());
    }

    void start(FrontEnd& fe) {
        int humans = 0, computers = 0;
        for (const Row& r : rows_) {
            humans += r.kind == 1;
            computers += r.kind == 2;
        }
        if (humans == 0 || computers == 0) {
            show_message(fe, art_, "There must be at least one player and one computer opponent");
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
            show_message(fe, art_, "All players may not be in the same allied group.");
            return;
        }
        save(fe.settings());
        fe.apply_settings();
        // The battle request: rows are player slots.
        BattleRequest req;
        req.map = map_;
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
            p.name = r.kind == 1 ? "Player" : "Computer";
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
        req.start.unit_limit = static_cast<std::uint16_t>(std::clamp(fe.settings().get_int("UnitLimit", 250), 20, 500));
        // The inputs the original takes from clocks, and the menus' rand() state.
        req.start.battle_rng_seed = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        req.start.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
        req.start.crt_rand_state = menu_rand().state();
        const std::string dir = art_.game_dir();
        const client::ClientOptions opt = client_options(fe.settings(), fe.width(), fe.height());
        fe.play_sound(art_.sound("BigButton"));
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
};

// ------------------------------------------------------------------ single player (SINGLE.GUI)
class SinglePlayer : public LayoutScreen {
public:
    explicit SinglePlayer(MenuArt& art) : LayoutScreen(art.layout("SINGLE")), art_(art) {}
    void enter(FrontEnd& fe) override {
        if (Widget* w = widget("NewCamp")) w->grayed = !fe.hooks().campaign_menu;
        if (Widget* w = widget("LoadGame")) w->grayed = !fe.hooks().load_game_menu;
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "NewCamp")) {
            if (fe.hooks().campaign_menu) fe.push(fe.hooks().campaign_menu(fe));
        } else if (is(w, "LoadGame")) {
            if (fe.hooks().load_game_menu) fe.push(fe.hooks().load_game_menu(fe, nullptr));
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
};

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
    }
    void frame(FrontEnd&, std::uint32_t now) override {
        // The sparks run along the circuit lines of the picture, one move per 1/30 s.
        const std::uint32_t t = now * 30 / 1000;
        if (!started_) {
            last_ = t;
            started_ = true;
        }
        for (; last_ < t; ++last_) step_sparks();
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
    void step_sparks() {
        const client::ScreenLayout* L = layout();
        if (!L || !L->background || L->background->width != 640 || L->background->height < 480) return;
        const render::Sprite8& bg = *L->background;
        if (surface_.pixels.empty()) {
            surface_ = render::Image8(640, 480, 0);
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x) surface_.row(y)[x] = bg.at(x, y);
        }
        sim::Lcg15& rnd = menu_rand();
        std::uint8_t* scr = surface_.pixels.data();
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
    void draw_under(FrontEnd&, render::Image8& out) override {
        if (surface_.pixels.empty()) return;
        for (int y = 0; y < std::min(out.height, 480); ++y)
            std::copy(surface_.row(y), surface_.row(y) + std::min(out.width, 640), out.row(y));
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "SINGLE")) {
            fe.play_sound(art_.sound("BigButton"));
            fe.push(std::make_unique<SinglePlayer>(art_));
        } else if (is(w, "MULTI")) {
            if (fe.hooks().multiplayer_menu) fe.push(fe.hooks().multiplayer_menu(fe));
        } else if (is(w, "EXIT")) {
            fe.quit();
        }
        // INTRO and the credits play movies (not built).
    }

private:
    MenuArt& art_;
    std::array<Spark, 100> sparks_{};
    render::Image8 surface_;
    std::uint32_t last_ = 0;
    bool started_ = false;
};

// ------------------------------------------------------------------ in-game menus
class ExitMenu : public LayoutScreen {
public:
    explicit ExitMenu(MenuArt& art) : LayoutScreen(art.layout("EXITMENU")) {}
    bool see_through() const override { return true; }

protected:
    void draw_under(FrontEnd& fe, render::Image8& out) override {
        const Widget& h = widgets_.front();
        client::fill_rect(out, out.bounds(), ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1, fe.menu_content().box_colour);
        client::outline_rect(out, out.bounds(), ox_, oy_, ox_ + h.w - 1, oy_ + h.h - 1, fe.menu_content().frame_colour);
    }
    void on_press(FrontEnd& fe, Widget& w) override {
        if (is(w, "MAINMENU")) {
            // Leaving counts as giving up: the end screens follow.
            if (fe.session()) fe.session()->surrender();
            fe.end_battle(client::BattleOutcome::Quit);
        } else if (is(w, "EXITGAME")) {
            fe.quit();
        } else if (is(w, "CANCEL")) {
            fe.pop();
        }
    }
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
        if (Widget* w = widget("MISSION")) set_caption(*w, "Settings");
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
        if (is(w, "OK")) fe.pop();
        else if (is(w, "SAVEGAME") && s && fe.hooks().save_game_menu) fe.push(fe.hooks().save_game_menu(fe, *s));
        else if (is(w, "LOADGAME") && fe.hooks().load_game_menu) fe.push(fe.hooks().load_game_menu(fe, s));
        else if (is(w, "PREFS")) fe.push(std::make_unique<OptionsMenu>(art_, true));
        else if (is(w, "EXIT") && art_.layout("EXITMENU")) fe.push(std::make_unique<ExitMenu>(art_));
    }

private:
    MenuArt& art_;
    bool opened_ = false;
    client::BattleSession* session_ = nullptr;
    client::MusicPlayer* music_ = nullptr;
};

// The tab bar of network games (TABMENU.GUI, over the top bar): the options menu, and the
// allies, share and control screens (the multiplayer part's). Tab again closes it.
class TabMenu : public LayoutScreen {
public:
    explicit TabMenu(MenuArt& art) : LayoutScreen(art.layout("TABMENU")), art_(art) {}
    bool see_through() const override { return true; }
    void enter(FrontEnd& fe) override {
        if (Widget* w = widget("ALLIES")) w->grayed = !fe.hooks().allies_menu;
        if (Widget* w = widget("SHARE")) w->grayed = !fe.hooks().share_menu;
        if (Widget* w = widget("CONTROL")) w->grayed = !fe.hooks().control_menu;
    }
    void handle(FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type == client::InputEvent::Type::KeyDown && e.key == client::keys::kTab) {
            fe.pop();
            return;
        }
        LayoutScreen::handle(fe, e);
    }

protected:
    void on_press(FrontEnd& fe, Widget& w) override {
        client::BattleSession* s = fe.session();
        if (!s) return;
        if (is(w, "OPTIONS") && art_.layout("ARMOPT")) fe.push(std::make_unique<GameMenu>(art_));
        else if (is(w, "ALLIES") && fe.hooks().allies_menu) fe.push(fe.hooks().allies_menu(fe, *s));
        else if (is(w, "SHARE") && fe.hooks().share_menu) fe.push(fe.hooks().share_menu(fe, *s));
        else if (is(w, "CONTROL") && fe.hooks().control_menu) fe.push(fe.hooks().control_menu(fe, *s));
        else if (is(w, "CANCEL")) fe.pop();
    }

private:
    MenuArt& art_;
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
        : LayoutScreen(art.layout("ENDMSN")), art_(art), table_(table), network_(network) {
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
    void frame(FrontEnd& fe, std::uint32_t now) override {
        const std::uint32_t t = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) * 30 / 1000);
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
        if (is(w, "MainMenu")) fe.to_main_menu();
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

// ------------------------------------------------------------------ loading screen
// The load picture; "Map: <name>" centred near the bottom; six stage rows (textures,
// terrain, units, animation, 3D data, explosions) with a bar from x 205 growing 3.5 pixels
// per percent, red while loading and green when done.
void draw_loading(MenuArt& art, const std::string& map, render::Image8& out, float fraction, const std::string& stage) {
    const render::Palette* pal = nullptr;
    if (const render::Sprite8* bg = art.picture("loadgame2bg", &pal)) render::blit(out, out.bounds(), *bg, bg->x_offset, bg->y_offset);
    const render::Palette& p = pal ? *pal : art.content().palette;
    const client::Font* f = art.content().font;
    static const std::pair<const char*, int> rows[] = {{"Textures", 0x87}, {"Terrain", 0xb1}, {"Units", 0xda},
                                                       {"Animation", 0x106}, {"3D Data", 0x130}, {"Explosions", 0x15b}};
    // The client's loading reports one fraction: the rows fill in turn.
    const auto* bar = art.sequence("anims\\commongui.GAF", "LIGHTBAR");
    for (int r = 0; r < 6; ++r) {
        const float part = std::clamp(fraction * 6.0f - static_cast<float>(r), 0.0f, 1.0f);
        const int pct = static_cast<int>(part * 100.0f);
        const int y = rows[r].second;
        if (f) client::draw_text(out, out.bounds(), *f, rows[r].first, 0x5a, y, art.gui_colour(p, 15));
        const std::uint8_t col = art.gui_colour(p, pct < 100 ? 12 : 10);
        if (pct > 0) client::fill_rect(out, out.bounds(), 0xcd, y, 0xcd + pct * 7 / 2, y + 20, col);
        if (bar && !bar->empty()) render::blit(out, {0xcd, y, 0xcd + pct * 7 / 2, y + 20}, bar->front(), 0xcd, y);
    }
    if (f) {
        const std::string t = "Map: " + map;
        client::draw_text(out, out.bounds(), *f, t, out.width / 2 - f->text_width(t) / 2, out.height - f->height * 3 / 2, art.gui_colour(p, 15));
    }
    (void)stage;
}

}  // namespace

std::unique_ptr<client::Screen> make_end_screen(MenuArt& art, const client::EndTable& table) {
    if (!art.layout("ENDMSN")) return nullptr;
    std::array<std::uint8_t, 256> hud{};
    for (int k = 0; k < 256; ++k) hud[static_cast<std::size_t>(k)] = art.gui_colour(art.content().palette, k);
    return std::make_unique<BattleOver>(art, table, hud, false);
}

void make_front_end(MenuArt& art, client::MusicPlayer* music, client::FrontEndSetup& setup) {
    setup.content = &art.content();
    setup.music = music;
    setup.main_menu = [&art](FrontEnd&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("MAINMENU")) return nullptr;
        return std::make_unique<MainMenu>(art);
    };
    setup.draw_loading = [&art](FrontEnd& fe, render::Image8& out, float f, const std::string& stage) {
        draw_loading(art, fe.settings().get_text("SkirmishMap", ""), out, f, stage);
    };
    setup.battle_over = [&art](FrontEnd&, client::BattleSession& s, client::BattleOutcome o) -> std::unique_ptr<client::Screen> {
        if (!art.layout("ENDMSN")) return nullptr;
        return std::make_unique<BattleOver>(art, end_table_of(s, o), s.content().hud, s.network());
    };
    setup.game_menu = [&art](FrontEnd&, client::BattleSession&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("ARMOPT")) return nullptr;
        return std::make_unique<GameMenu>(art);
    };
    setup.tab_menu = [&art](FrontEnd&, client::BattleSession&) -> std::unique_ptr<client::Screen> {
        if (!art.layout("TABMENU")) return nullptr;
        return std::make_unique<TabMenu>(art);
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
        if (o.game_speed != static_cast<int>(c.budget().game_speed)) c.set_game_speed(o.game_speed);
    };
}

}  // namespace ta
