// The original game's front end screens (spec: docs/RE_MEMORY.md, "Client", "Menus").

#include "ta/client_menus.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "client/audio.h"
#include "client/canvas.h"
#include "sim/battle.h"
#include "sim/economy.h"
#include "sim/victory.h"
#include "ta/gaf.h"
#include "ta/net_setup.h"
#include "ta/unitdefs.h"
#include "ta/render_assets.h"
#include "ta/tdf.h"
#include "ta/tnt.h"

namespace ta {
namespace {

using client::FrontEnd;
using client::Widget;
using client::WidgetKind;

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool ieq(const std::string& a, const std::string& b) { return upper(a) == upper(b); }
bool g_link_empty(const GuiGadget& g) { return g.link.empty(); }

std::unique_ptr<TdfSection> parse_tdf(const std::vector<std::uint8_t>& f) {
    return tdf_parse(std::string_view(reinterpret_cast<const char*>(f.data()), f.size())).root;
}

std::uint8_t nearest(const render::Palette& pal, int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; ++i) {
        const int d = std::abs(pal[static_cast<std::size_t>(i)].r - r) + std::abs(pal[static_cast<std::size_t>(i)].g - g) +
                      std::abs(pal[static_cast<std::size_t>(i)].b - b);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return static_cast<std::uint8_t>(best);
}

// The background the original shows behind each screen (by GUI file name).
const char* background_of(const std::string& gui) {
    static const std::pair<const char*, const char*> table[] = {
        {"MAINMENU", "FrontendX"},     {"SINGLE", "singlebg"},         {"SKIRMISH", "Skirmsetup4x"},
        {"SELMAP", "DSELECTMAP2"},     {"STARTOPT", "options4x"},      {"SOUNDS", "optsound4x"},
        {"MUSIC", "optmusic4x"},       {"VISUALS", "optvisual4x"},     {"SPEEDS", "optinterface4x"},
        {"ENDMSN", "outcome0"},        {"HELP", "dhelp"},              {"RESTART", "DRESTART"},
        {"LOADGAME", "DLoadgame2"},    {"SAVEGAME", "DSavegame2"},     {"BRIEFING", "igmbrief"},
    };
    const std::string u = upper(gui);
    for (const auto& [g, b] : table)
        if (u == g) return b;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------- art

bool MenuArt::load(const std::string& game_dir, std::string& error) {
    game_dir_ = game_dir;
    vfs_.mount_game_dir(game_dir);
    if (!load_palette(vfs_, content_.palette) || !load_blend_table(vfs_, content_.blend)) {
        error = "cannot load the palette / blend table";
        return false;
    }
    std::vector<std::uint8_t> gp;
    if (vfs_.read("palettes\\GUIPAL.PAL", gp) && gp.size() >= 1024)
        for (std::size_t i = 0; i < 256; ++i) guipal_[i] = {gp[i * 4], gp[i * 4 + 1], gp[i * 4 + 2]};
    // Fonts: the interface's picture fonts (hattfont12 for captions, hattfont11 small).
    fonts_.push_back(gaf_font("anims\\hattfont12.GAF"));
    content_.font = &fonts_.back();
    fonts_.push_back(gaf_font("anims\\hattfont11.GAF"));
    content_.small_font = &fonts_.back();
    content_.big_font = content_.font;
    // The sound bank's named sounds.
    std::vector<std::uint8_t> f;
    if (vfs_.read("gamedata\\ALLSOUND.TDF", f))
        if (auto root = parse_tdf(f))
            for (const auto& s : root->children()) {
                const std::string file = s->get_string("sound", 64, "");
                std::vector<std::uint8_t> w;
                client::Sample smp;
                if (file.empty() || !vfs_.read("sounds\\" + file + ".wav", w) || !client::decode_wav(w, smp)) continue;
                sound_index_[upper(s->name())] = static_cast<int>(content_.samples.size());
                content_.samples.push_back(std::move(smp));
            }
    vfs_.read("palettes\\PALETTE.LHT", content_.light);
    if (content_.light.size() < 0x2000) content_.light.clear();
    content_.click_sound = sound("SmallButton");
    content_.error_sound = sound("NOTOKTOBUILD");
    content_.cursor = sequence("anims\\CURSORS.GAF", "cursornormal");
    content_.text_colour = gui_colour(content_.palette, 15);
    content_.text_dim = gui_colour(content_.palette, 8);
    content_.box_colour = gui_colour(content_.palette, 0);
    content_.frame_colour = gui_colour(content_.palette, 8);
    content_.layout = [this](const std::string& n) { return layout(n); };
    content_.picture = [this](const std::string& n) { return picture(n); };
    content_.stock_button = [this](int w, int h) -> const std::vector<render::Sprite8>* {
        const auto* all = sequence("anims\\commongui.GAF", "BUTTONS0");
        (void)w;
        (void)h;
        return all;
    };
    // Sides.
    if (vfs_.read("gamedata\\sidedata.tdf", f))
        if (auto root = parse_tdf(f))
            for (int k = 0;; ++k) {
                const TdfSection* s = root->child("SIDE" + std::to_string(k));
                if (!s) break;
                sides_.push_back(s->get_string("name", 32, ""));
            }
    // Music: music\1.mp3, 2.mp3, ... next to the game.
    for (int n = 1; n < 100; ++n) {
        if (!std::filesystem::exists(std::filesystem::path(game_dir) / "music" / (std::to_string(n) + ".mp3"))) break;
        music_tracks_ = n;
    }
    return true;
}

std::uint8_t MenuArt::gui_colour(const render::Palette& palette, int k) const {
    const render::Rgb& c = guipal_[static_cast<std::size_t>(k & 0xFF)];
    return nearest(palette, c.r, c.g, c.b);
}

const std::vector<std::uint8_t>* MenuArt::gaf(const std::string& path) {
    const std::string key = upper(path);
    auto it = gafs_.find(key);
    if (it == gafs_.end()) {
        std::vector<std::uint8_t> f;
        vfs_.read(path, f);
        it = gafs_.emplace(key, std::move(f)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

const std::vector<render::Sprite8>* MenuArt::sequence(const std::string& gaf_path, const std::string& name) {
    const std::string key = upper(gaf_path) + "|" + upper(name);
    if (auto it = seq_index_.find(key); it != seq_index_.end()) return it->second;
    const std::vector<render::Sprite8>* out = nullptr;
    if (const auto* g = gaf(gaf_path)) {
        GafDirectory dir;
        if (gaf_read_directory(*g, dir)) {
            const int s = gaf_find_sequence(dir, name);
            std::vector<render::Sprite8> frames;
            if (s >= 0 && gaf_sequence_frames(*g, s, frames) && !frames.empty()) {
                sequences_.push_back(std::move(frames));
                out = &sequences_.back();
            }
        }
    }
    seq_index_[key] = out;
    return out;
}

client::Font MenuArt::gaf_font(const std::string& path) {
    // Frame n of the font's one sequence is character n, its offset the baseline; the
    // glyphs keep their own colours.
    client::Font font;
    const auto* g = gaf(path);
    if (!g) return font;
    GafDirectory dir;
    std::vector<render::Sprite8> frames;
    if (!gaf_read_directory(*g, dir) || !gaf_sequence_frames(*g, 0, frames)) return font;
    // The text height is the 'I' frame's height plus 2 (as the original measures it). The
    // original lowers every glyph's offset by the 'I' frame's height and draws a glyph with its
    // offset at the text position, so a glyph's top row lies (I height - its y offset) below
    // the text's top; glyphs may reach outside the cell (descenders).
    if (frames.size() <= 'I') return font;
    const int i_height = frames['I'].height;
    font.height = i_height + 2;
    font.raise = 0;
    for (std::size_t c = 0; c < frames.size() && c < 256; ++c) {
        const render::Sprite8& f = frames[c];
        client::Glyph& gl = font.glyphs[c];
        gl.width = f.width;
        gl.present = f.width > 0 && c >= 0x20;
        gl.rows = std::max(1, static_cast<int>(f.height));
        gl.top = i_height - f.y_offset;
        gl.left = -f.x_offset;
        gl.ink.assign(static_cast<std::size_t>(f.width) * static_cast<std::size_t>(gl.rows), 0);
        gl.colours.assign(gl.ink.size(), 0);
        if (c == ' ') continue;  // a space only advances
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) {
                const std::uint8_t p = f.at(x, y);
                if (f.has_key && p == f.key) continue;
                const std::size_t at = static_cast<std::size_t>(y * f.width + x);
                gl.ink[at] = 1;
                gl.colours[at] = p;
            }
    }
    return font;
}

const render::Sprite8* MenuArt::picture(const std::string& name, const render::Palette** palette) {
    const std::string key = upper(name);
    auto it = pictures_.find(key);
    if (it == pictures_.end()) {
        std::vector<std::uint8_t> f;
        render::Sprite8 s;
        render::Palette p{};
        std::pair<const render::Sprite8*, const render::Palette*> v{nullptr, nullptr};
        if (vfs_.read("bitmaps\\" + name + ".pcx", f) && decode_pcx(f, s, &p)) {
            sprites_.push_back(std::move(s));
            palettes_.push_back(p);
            v = {&sprites_.back(), &palettes_.back()};
        }
        it = pictures_.emplace(key, v).first;
    }
    if (palette) *palette = it->second.second;
    return it->second.first;
}

int MenuArt::sound(const std::string& name) {
    const auto it = sound_index_.find(upper(name));
    return it == sound_index_.end() ? -1 : it->second;
}

const client::ScreenLayout* MenuArt::layout(const std::string& gui_in) {
    std::string gui = upper(gui_in);
    if (gui.size() > 4 && gui.compare(gui.size() - 4, 4, ".GUI") == 0) gui.resize(gui.size() - 4);
    if (auto it = layouts_.find(gui); it != layouts_.end()) return it->second.get();
    std::vector<std::uint8_t> f;
    std::vector<GuiGadget> gs;
    if (!vfs_.read("guis\\" + gui + ".GUI", f) || !parse_gui(f, gs) || gs.empty()) {
        layouts_[gui] = nullptr;
        return nullptr;
    }
    auto L = std::make_unique<client::ScreenLayout>();
    L->name = gui;
    const GuiGadget& head = gs[0];
    L->x = head.x;
    L->y = head.y;
    L->enter_default = head.crdefault;
    L->escape_default = head.escdefault;
    L->default_focus = head.defaultfocus;
    if (const char* bg = background_of(gui)) {
        L->background = picture(bg, &L->palette);
        // Only some screens put their picture's palette on the display (the main menu, the end of
        // a battle); the others keep the one shown before, the main menu's, and their interface
        // colours come from it.
        if (gui != "MAINMENU" && gui != "ENDMSN") {
            const render::Palette* menu_pal = nullptr;
            if (picture("FrontendX", &menu_pal) && menu_pal) L->palette = menu_pal;
        }
    }
    const render::Palette& pal = L->palette ? *L->palette : content_.palette;
    for (int k = 0; k < 256; ++k) L->colours[static_cast<std::size_t>(k)] = gui_colour(pal, k);
    const std::string own_gaf = "anims\\" + gui + ".GAF";
    std::vector<Widget> slider_arrows;  // appended after the gadgets (the original adds them at the end)
    for (const GuiGadget& g : gs) {
        Widget w;
        switch (g.id) {
        case 0: w.kind = WidgetKind::Header; break;
        case 1: w.kind = WidgetKind::Button; break;
        case 2: w.kind = WidgetKind::ListBox; break;
        case 3: w.kind = WidgetKind::TextField; break;
        case 4: w.kind = WidgetKind::ScrollBar; break;
        case 5: w.kind = WidgetKind::Label; break;
        case 6: w.kind = WidgetKind::Surface; break;
        case 7: w.kind = WidgetKind::FontRef; break;
        case 12: w.kind = WidgetKind::Picture; break;
        default: w.kind = WidgetKind::Label; w.active = false; break;
        }
        w.name = g.name;
        w.x = g.x;
        w.y = g.y;
        w.w = g.w;
        w.h = g.h;
        w.attribs = g.attribs;
        w.fg = g.colorf;
        w.bg = g.colorb;
        if (g.id != 0) w.active = w.active && g.active;
        w.grayed = g.grayed;
        w.quickkey = g.quickkey > 0 && g.quickkey < 256 ? std::tolower(g.quickkey) : 0;
        w.quickkey_char = g.quickkey > 0 && g.quickkey < 256 ? g.quickkey : 0;
        w.stages = g.stages;
        w.status = g.status;
        w.text = g.text;
        w.link = g.link;
        w.vertical = g.h > g.w;
        if (w.kind == WidgetKind::Button && (g.attribs & 0x1800) == 0) {
            // Art: the gadget's own sequence in the screen's GAF, else commongui's sequence of
            // its name; else (attribute 0x80 clear) a switch takes "stagebuttn<stages>" (a
            // one-stage switch, or one whose caption is "Off|On", becomes a two-stage
            // "stagebuttn1") and a plain button the stock "BUTTONS0"; attribute 0x80 takes
            // "CHECKBOX". In those stock sequences the frame group (every fourth frame) whose
            // size is nearest the gadget's (|dw| + |dh|, first of equals) is used, and the
            // gadget takes that frame's size.
            w.frames = sequence(own_gaf, g.name);
            if (!w.frames) w.frames = sequence("anims\\commongui.GAF", g.name);
            if (!w.frames) {
                std::string stock_name;
                if (g.attribs & 0x80) {
                    stock_name = "CHECKBOX";
                } else if (g.stages > 0) {
                    if (g.text == "Off|On" || g.stages == 1) {
                        w.stages = 2;
                        stock_name = "stagebuttn1";
                    } else {
                        stock_name = "stagebuttn" + std::to_string(g.stages);
                    }
                } else {
                    stock_name = "BUTTONS0";
                }
                if (const auto* all = sequence("anims\\commongui.GAF", stock_name); all && !all->empty()) {
                    std::size_t base = 0;
                    int best = 1000;
                    for (std::size_t i = 0; i < all->size(); i += 4) {
                        const int d = std::abs(g.h - (*all)[i].height) + std::abs(g.w - (*all)[i].width);
                        if (d < best) {
                            best = d;
                            base = i;
                        }
                    }
                    // The frames from the chosen group on (staged switches index from it).
                    sequences_.emplace_back(all->begin() + static_cast<std::ptrdiff_t>(base), all->end());
                    for (render::Sprite8& s : sequences_.back()) s.x_offset = s.y_offset = 0;  // stock frames lose their offsets
                    w.frames = &sequences_.back();
                    w.w = (*all)[base].width;
                    w.h = (*all)[base].height;
                }
            } else if (!w.frames->empty()) {
                w.w = w.frames->front().width;
                w.h = w.frames->front().height;
            }
        } else if (w.kind == WidgetKind::ScrollBar) {
            // Slider art: the SLIDERS sequence (the screen's GAF, else commongui), offsets
            // dropped; frames from 0 for a vertical slider (w <= h), from 10 for a horizontal one:
            // +0 / +1 / +2 track start, middle (repeated), end; +3 knob (+4 its middle and +5 its
            // end when vertical); +6 / +8 the two arrow buttons, placed at the ends, the bar
            // shrinking between them. The cross size takes the first frame's; a horizontal knob is
            // frame +5's width wide and travels w - knob - 4 pixels.
            const auto* seq = sequence(own_gaf, "SLIDERS");
            if (!seq) seq = sequence("anims\\commongui.GAF", "SLIDERS");
            if (seq && seq->size() >= 20) {
                sequences_.emplace_back(*seq);
                for (render::Sprite8& s : sequences_.back()) s.x_offset = s.y_offset = 0;
                const auto& fr = sequences_.back();
                w.frames = &fr;
                w.slider_base = w.w <= w.h ? 0 : 10;
                const auto base = static_cast<std::size_t>(w.slider_base);
                if (w.w < w.h) w.w = fr[base].width;
                else w.h = fr[base].height;
                Widget a, b;
                a.kind = b.kind = WidgetKind::Button;
                a.link = b.link = w.name;
                a.active = b.active = w.active;
                a.slider_step = -1;
                b.slider_step = 1;
                a.attribs = 0x3400;
                b.attribs = 0x2C00;
                sequences_.emplace_back(fr.begin() + static_cast<std::ptrdiff_t>(base) + 6, fr.begin() + static_cast<std::ptrdiff_t>(base) + 8);
                a.frames = &sequences_.back();
                sequences_.emplace_back(fr.begin() + static_cast<std::ptrdiff_t>(base) + 8, fr.begin() + static_cast<std::ptrdiff_t>(base) + 10);
                b.frames = &sequences_.back();
                const render::Sprite8& arrow = fr[base + 6];
                a.x = w.x;
                a.y = w.y;
                a.w = arrow.width;
                a.h = arrow.height;
                b.w = fr[base + 8].width;
                b.h = fr[base + 8].height;
                if (w.h < w.w) {
                    b.x = w.x - arrow.width + w.w;
                    b.y = w.y;
                    w.w -= 2 * arrow.width;
                    w.x += arrow.width;
                    w.knob = fr[base + 5].width;
                    w.span = w.w - w.knob - 4;
                } else {
                    b.x = w.x;
                    b.y = w.y - arrow.height + w.h;
                    w.h -= 2 * arrow.height;
                    w.y += arrow.height;
                }
                slider_arrows.push_back(a);
                slider_arrows.push_back(b);
            } else {
                w.span = std::max(w.w, w.h) - 6;
            }
        } else if (w.kind == WidgetKind::Picture) {
            if (const auto* s = sequence(own_gaf, g.name)) w.picture = &s->front();
            else if (const auto* c = sequence("anims\\commongui.GAF", g.name)) w.picture = &c->front();
        }
        L->widgets.push_back(std::move(w));
    }
    for (Widget& a : slider_arrows) L->widgets.push_back(std::move(a));
    // Without a named Enter / Escape default the first button whose name begins with "OK" or
    // "NEXT" takes Enter and the first beginning with "PREV" or "Cancel" takes Escape (names
    // compared without case).
    {
        auto begins = [](const std::string& s, const char* p) {
            const std::size_t n = std::strlen(p);
            return s.size() >= n && ieq(s.substr(0, n), p);
        };
        for (std::size_t i = 1; i < gs.size(); ++i)
            if (gs[i].id == 1 && L->enter_default.empty() && (begins(gs[i].name, "OK") || begins(gs[i].name, "NEXT"))) L->enter_default = gs[i].name;
        for (std::size_t i = 1; i < gs.size(); ++i)
            if (gs[i].id == 1 && L->escape_default.empty() && (begins(gs[i].name, "PREV") || begins(gs[i].name, "Cancel")))
                L->escape_default = gs[i].name;
    }
    // Quick keys come from the captions (the front end clears the files' keys when a screen
    // opens): in gadget order, each plain button (switches get none) and each label with a
    // caption takes the first character of its caption, not a space, that no gadget has as its
    // key yet (letters compared without case); the key keeps the caption's case.
    {
        auto& ws = L->widgets;
        for (Widget& w : ws)
            if (w.kind == WidgetKind::Button) w.quickkey = w.quickkey_char = 0;
        for (std::size_t i = 0; i < ws.size() && i < gs.size(); ++i) {
            Widget& w = ws[i];
            const bool button = w.kind == WidgetKind::Button && (gs[i].attribs & 0x1800) == 0 && (gs[i].attribs & 0x10000) == 0;
            const bool label = gs[i].id == 5 && !w.text.empty() && !g_link_empty(gs[i]);  // only labels linked to a gadget
            if (!button && !label) continue;
            if (button && w.stages > 0) continue;
            w.quickkey = w.quickkey_char = 0;
            for (const char c : w.text) {
                if (c == ' ') continue;
                const int lc = std::tolower(static_cast<unsigned char>(c));
                bool used = false;
                for (const Widget& o : ws) used |= (o.kind == WidgetKind::Button || o.kind == WidgetKind::Label) && o.quickkey == lc;
                if (used) continue;
                w.quickkey = lc;
                w.quickkey_char = static_cast<unsigned char>(c);
                break;
            }
        }
    }
    auto* raw = L.get();
    layouts_[gui] = std::move(L);
    return raw;
}

const std::vector<std::string>& MenuArt::skirmish_maps() {
    if (maps_done_) return maps_;
    maps_done_ = true;
    for (const std::string& file : vfs_.enumerate("maps\\*.ota")) {
        std::vector<std::uint8_t> f;
        if (!vfs_.read("maps\\" + file, f)) continue;
        auto root = parse_tdf(f);
        const TdfSection* gh = root ? root->child("GlobalHeader") : nullptr;
        if (!gh) continue;
        bool network = false;
        for (const auto& c : gh->children())
            if (upper(c->get_string("Type", 64, "")).find("NETWORK") != std::string::npos) network = true;
        if (!network) continue;
        std::string name = file.substr(0, file.size() - 4);
        if (vfs_.size_or_zero("maps\\" + name + ".tnt") == 0) continue;
        maps_.push_back(name);
    }
    std::sort(maps_.begin(), maps_.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
    maps_.erase(std::unique(maps_.begin(), maps_.end(), [](const std::string& a, const std::string& b) { return ieq(a, b); }), maps_.end());
    return maps_;
}

const render::Sprite8* MenuArt::map_preview(const std::string& map, int w, int h) {
    const std::string key = upper(map) + "|" + std::to_string(w) + "x" + std::to_string(h);
    if (auto it = previews_.find(key); it != previews_.end()) return it->second.get();
    std::unique_ptr<render::Sprite8> out;
    std::vector<std::uint8_t> tnt;
    if (vfs_.read("maps\\" + map + ".tnt", tnt) && tnt.size() > 48) {
        auto rd32 = [&](std::size_t o) -> std::uint32_t {
            if (o + 4 > tnt.size()) return 0;
            std::uint32_t v;
            std::memcpy(&v, &tnt[o], 4);
            return v;
        };
        // The TNT's own minimap (header word 10), cut to the map's shape, scaled to fit.
        const std::uint32_t at = rd32(40);
        const int pw = static_cast<int>(rd32(at)), ph = static_cast<int>(rd32(at + 4));
        const int mw = static_cast<int>(rd32(4)), mh = static_cast<int>(rd32(8));
        if (at && pw > 0 && ph > 0 && pw <= 1024 && ph <= 1024 && at + 8 + static_cast<std::size_t>(pw) * ph <= tnt.size()) {
            int cw = pw, ch = ph;
            if (mw > mh && mw > 0) ch = std::max(1, ph * mh / mw);
            else if (mh > mw && mh > 0) cw = std::max(1, pw * mw / mh);
            int ow = w, oh = h;
            if (cw > ch) oh = std::max(1, h * ch / cw);
            else if (ch > cw) ow = std::max(1, w * cw / ch);
            out = std::make_unique<render::Sprite8>();
            out->width = ow;
            out->height = oh;
            out->has_key = false;
            out->x_offset = -(w - ow) / 2;
            out->y_offset = -(h - oh) / 2;
            out->pixels.resize(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
            for (int y = 0; y < oh; ++y)
                for (int x = 0; x < ow; ++x)
                    out->pixels[static_cast<std::size_t>(y * ow + x)] =
                        tnt[at + 8 + static_cast<std::size_t>(y * ch / oh) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x * cw / ow)];
        }
    }
    auto* raw = out.get();
    previews_[key] = std::move(out);
    return raw;
}

std::shared_ptr<const client::Sample> MenuArt::music_track(int n) {
    if (auto it = music_cache_.find(n); it != music_cache_.end()) return it->second;
    auto s = std::make_shared<client::Sample>();
    const std::string path = (std::filesystem::path(game_dir_) / "music" / (std::to_string(n) + ".mp3")).string();
    std::shared_ptr<const client::Sample> out;
    if (client::decode_audio_file(path, *s)) {
        out = s;
        std::printf("music: track %d (%.0f s)\n", n, static_cast<double>(s->pcm.size()) / std::max(1, s->channels) / std::max(1, s->rate));
    }
    else std::printf("music: cannot decode %s\n", path.c_str());
    // Keep the last few decoded tracks.
    if (music_cache_.size() > 3) music_cache_.erase(music_cache_.begin());
    music_cache_[n] = out;
    return out;
}

// ---------------------------------------------------------------------------------- session

client::ClientOptions client_options(const client::Settings& s, int width, int height) {
    client::ClientOptions o;
    // A battle runs at the chosen display mode (the menus stay 640 x 480).
    o.width = std::max(640, s.get_int("DisplaymodeWidth", width));
    o.height = std::max(480, s.get_int("DisplaymodeHeight", height));
    o.scroll_speed = s.get_int("scrollspeed", 0x20);
    o.game_speed = std::clamp(s.get_int("gamespeed", 10), 1, 20);
    o.right_click_orders = s.get_int("Interface Type", 0) == 1;
    o.damage_bars = s.get_int("damagebars", 0) != 0;
    o.text_lines = s.get_int("textlines", 10);
    o.text_seconds = s.get_int("textscroll", 10);
    o.unit_chat = s.get_int("unitchat", 10);
    o.unit_chat_text = s.get_int("unitchattext", 5);
    o.speech = s.get_int("speechfx", 1) != 0;
    o.alt_groups = s.get_int("SwitchAlt", 0) != 0;
    o.clock = s.get_int("clock", 0) != 0;
    o.sound_mode = s.get_int("Sound Mode", 1);
    return o;
}

std::unique_ptr<SkirmishSession> SkirmishSession::start(const std::string& game_dir, const BattleRequest& request, int human,
                                                        const client::ClientOptions& options, const client::LoadProgress& progress,
                                                        std::string& error,
                                                        const std::function<sim::BattleOutside&(sim::BattleOutside&)>& wrap,
                                                        const std::function<void(sim::Battle&)>& attach) {
    auto s = std::unique_ptr<SkirmishSession>(new SkirmishSession);
    s->world_ = std::make_unique<ClientWorld>();
    s->world_->net_wrap = wrap;
    s->world_->net_attach = attach;
    if (!s->world_->load(game_dir, request, human, error, progress)) return nullptr;
    client::ClientOptions o = options;
    o.player = human;
    const std::string side = request.sides[static_cast<std::size_t>(human)];
    o.side = 0;
    {
        std::vector<std::uint8_t> sd;
        s->world_->vfs_.read("gamedata\\sidedata.tdf", sd);
        if (auto root = parse_tdf(sd))
            for (int k = 0;; ++k) {
                const TdfSection* sec = root->child("SIDE" + std::to_string(k));
                if (!sec) break;
                if (ieq(sec->get_string("name", 32, ""), side)) o.side = k;
            }
    }
    o.fog = request.start.fog_of_war || request.start.explore;
    s->client_ = std::make_unique<client::GameClient>(s->world_->battle(), s->world_->content(), o);
    s->world_->attach(s->client_.get());
    SkirmishSession* raw = s.get();
    s->world_->on_activity = [raw](std::int32_t amount) {
        if (raw->combat_activity) raw->combat_activity(amount);
    };
    const auto at = s->world_->start_position(human);
    s->client_->center_on({at[0] << 16, 0, at[1] << 16});
    return s;
}

#ifdef TA_CLIENT_HAVE_NET
client::NetMenuConfig network_config(MenuArt& art, client::FrontEnd* fe) {
    client::NetMenuConfig cfg;
    if (fe) cfg.player_name = fe->settings().get_text("Nickname", "Player");
    const std::string dir = art.game_dir();
    cfg.load_network_game = [dir, fe](const client::NetGameRequest& r, const client::LoadProgress& progress,
                                      std::string& error) -> std::unique_ptr<client::BattleSession> {
        BattleRequest req;
        req.map = r.map;
        req.sides = r.sides;
        req.start = r.start;
        const int human = r.start.local_player;
        client::Settings none;
        // The user's settings (screen size, speeds, interface type) as the front end passes them.
        const client::ClientOptions opt = client_options(r.settings ? *r.settings : (fe ? fe->settings() : none), 640, 480);
        return SkirmishSession::start(dir, req, human, opt, progress, error, r.wrap_outside, r.attach);
    };
    cfg.unit_checks = [&art]() -> net::UnitChecks {
        UnitDefContext ctx;
        load_unit_context(art.vfs(), ctx);
        const UnitDefTable defs = load_unit_defs(art.vfs(), ctx);
        return unit_sync_checks(art.vfs(), defs);
    };
    return cfg;
}
#endif

client::BattleOutcome SkirmishSession::outcome() {
    const sim::Battle& b = world_->battle();
    if (surrendered_) return client::BattleOutcome::Quit;
    if (b.end_game.flags & sim::end_game_flags::kWon) return client::BattleOutcome::Victory;
    if (b.end_game.flags & sim::end_game_flags::kLost) return client::BattleOutcome::Defeat;
    return client::BattleOutcome::None;
}

void SkirmishSession::surrender() { surrendered_ = true; }

std::vector<client::PlayerStats> SkirmishSession::stats() {
    std::vector<client::PlayerStats> out;
    sim::Battle& b = world_->battle();
    for (int p = 0; p < 10; ++p) {
        const sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
        if (!s.active) continue;
        const sim::PlayerEconomy& e = b.player_economy[static_cast<std::size_t>(p)];
        client::PlayerStats st;
        const auto& req = world_->request();
        st.name = req.start.players[static_cast<std::size_t>(p)].name;
        if (st.name.empty()) st.name = (req.start.players[static_cast<std::size_t>(p)].kind == sim::PlayerKind::Computer ? "Computer " : "Player ") +
                                       std::to_string(p + 1);
        st.colour = req.start.players[static_cast<std::size_t>(p)].colour;
        st.units_killed = s.units_killed;
        st.units_lost = s.units_lost;
        st.energy_produced = static_cast<float>(e.energy_produced);
        st.metal_produced = static_cast<float>(e.metal_produced);
        st.energy_wasted = static_cast<float>(e.energy_wasted);
        st.metal_wasted = static_cast<float>(e.metal_wasted);
        st.energy_used = static_cast<float>(e.energy_consumed);
        st.metal_used = static_cast<float>(e.metal_consumed);
        st.won = p == b.settings.local_player && (b.end_game.flags & sim::end_game_flags::kWon);
        st.lost = p == b.settings.local_player && (b.end_game.flags & sim::end_game_flags::kLost);
        out.push_back(st);
    }
    return out;
}

}  // namespace ta
