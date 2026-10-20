// The original game's front end screens (spec: docs/RE_MEMORY.md, "Client", "Menus").

#include "ta/client_menus.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "client/audio.h"
#include "client/canvas.h"
#include "render/raster.h"
#include "sim/battle.h"
#include "sim/economy.h"
#include "sim/victory.h"
#include "ta/console_commands.h"
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
                // One sound per section (sections naming the same file keep their own copies);
                // the file is sounds\<name> with its extension replaced by .wav.
                std::string file = s->get_string("sound", 64, "");
                const auto dot = file.find_last_of('.');
                if (dot != std::string::npos) file.resize(dot);
                std::vector<std::uint8_t> w;
                client::Sample smp;
                if (file.empty() || !vfs_.read("sounds\\" + file + ".wav", w) || !client::decode_wav(w, smp)) continue;
                smp.name = upper(s->name());
                sound_index_[upper(s->name())] = static_cast<int>(content_.samples.size());
                content_.samples.push_back(std::move(smp));
            }
    vfs_.read("palettes\\PALETTE.LHT", content_.light);
    if (content_.light.size() < 0x2000) content_.light.clear();
    vfs_.read("palettes\\PALETTE.SHD", content_.shade);
    if (content_.shade.size() < 0x2000) content_.shade.clear();
    content_.click_sound = sound("SmallButton");
    content_.error_sound = sound("NOTOKTOBUILD");
    content_.cursor = sequence("anims\\CURSORS.GAF", "cursornormal");
    // Its timing: the sequence entry's loop byte (+2) and each frame's time (the frame entry's
    // second word, low 16 bits).
    if (const auto* g = gaf("anims\\CURSORS.GAF")) {
        GafDirectory dir;
        const int s = gaf_read_directory(*g, dir) ? gaf_find_sequence(dir, "cursornormal") : -1;
        auto u32_at = [&](std::size_t o) -> std::uint32_t {
            if (o + 4 > g->size()) return 0;
            std::uint32_t v;
            std::memcpy(&v, &(*g)[o], 4);
            return v;
        };
        const std::uint32_t entry = s >= 0 ? u32_at(12 + static_cast<std::size_t>(s) * 4) : 0;
        if (entry && entry + 0x28 <= g->size()) {
            const std::uint16_t frames = static_cast<std::uint16_t>((*g)[entry] | ((*g)[entry + 1] << 8));
            content_.cursor_timing.loop = (*g)[entry + 2] != 0;
            for (std::uint16_t i = 0; i < frames; ++i)
                content_.cursor_timing.durations.push_back(static_cast<std::uint16_t>(u32_at(entry + 0x28 + 8u * i + 4)));
        }
    }
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

const client::Font* MenuArt::fnt_font(const std::string& name) {
    std::vector<std::uint8_t> f;
    if (!vfs_.read("fonts\\" + name + ".FNT", f)) return nullptr;
    fonts_.emplace_back();
    if (!parse_fnt(f, fonts_.back())) {
        fonts_.pop_back();
        return nullptr;
    }
    return &fonts_.back();
}

std::vector<int> MenuArt::sequence_times(const std::string& gaf_path, const std::string& name, bool* loop) {
    // Sequence entry: u16 frames, u16 (its low byte: loops), u32, name[32], then per frame
    // {u32 offset, u32 duration in ticks}.
    std::vector<int> times;
    const auto* g = gaf(gaf_path);
    if (!g) return times;
    GafDirectory dir;
    if (!gaf_read_directory(*g, dir)) return times;
    const int s = gaf_find_sequence(dir, name);
    if (s < 0) return times;
    const std::vector<std::uint8_t>& f = *g;
    auto u32 = [&](std::size_t at) {
        return at + 4 <= f.size() ? static_cast<std::uint32_t>(f[at] | f[at + 1] << 8 | f[at + 2] << 16 | static_cast<std::uint32_t>(f[at + 3]) << 24) : 0u;
    };
    const std::size_t entry = u32(12 + static_cast<std::size_t>(s) * 4);
    if (entry + 40 > f.size()) return times;
    const int n = f[entry] | f[entry + 1] << 8;
    if (loop) *loop = f[entry + 2] != 0;
    for (int k = 0; k < n; ++k) times.push_back(static_cast<int>(u32(entry + 40 + static_cast<std::size_t>(k) * 8 + 4)));
    return times;
}

std::vector<std::uint8_t> MenuArt::movie(const std::string& name) const {
    // The original builds "<CD>\Data\<name>.zrb" (on the Steam version the CD is the game
    // folder itself) and plays it when the file is there (0x426780, 0x41D7B0).
    std::vector<std::uint8_t> bytes;
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(game_dir_) / "Data" / name;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size == 0) return bytes;
    std::ifstream in(p, std::ios::binary);
    if (!in) return bytes;
    bytes.resize(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) bytes.clear();
    return bytes;
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
    if (content_.font) L->font_height = content_.font->height;
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
        w.assoc = g.assoc;
        w.max_chars = g.max_chars;
        w.vertical = g.h > g.w;
        if (w.kind == WidgetKind::ListBox && content_.font) {
            // A list's height is cut to whole steps of the font's height + 2 when its screen opens.
            const int step = content_.font->height + 2;
            w.h -= w.h % step;
        }
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
                    // (0x4A81E0: also a switch with attribute 0x4000; at most "stagebuttn4".)
                    if (g.text == "Off|On" || g.stages == 1 || (g.attribs & 0x4000)) {
                        w.stages = 2;
                        stock_name = "stagebuttn1";
                    } else {
                        stock_name = "stagebuttn" + std::to_string(std::min(g.stages, 4));
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
            // A switch's attributes become "left-aligned caption" (only 0x4000 kept) when the
            // screen loads (0x4A81E0, after splitting its captions at '|'): NEWGAME's centred
            // Difficulty switch shows its caption at the left like every other switch.
            if (w.stages > 0) w.attribs = (w.attribs & 0x4000) | 1;
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
    // The original's map list (0x434BF0): Maps\*.ota in the archives' enumeration order, each map
    // whose [GlobalHeader] has a schema for network play — [Schema 0], [Schema 1], ... up to the
    // first missing one, with a Type of "Network 1" .. "Network 4" (any case); the name is the
    // file's without its extension. Then sorted (0x4AEFA0: a bubble sort by the runtime's
    // case-blind comparison, which compares lower-case letters, so stable and '_' before letters).
    for (const std::string& file : vfs_.enumerate("maps\\*.ota")) {
        std::vector<std::uint8_t> f;
        if (!vfs_.read("maps\\" + file, f)) continue;
        auto root = parse_tdf(f);
        const TdfSection* gh = root ? root->child("GlobalHeader") : nullptr;
        if (!gh) continue;
        bool network = false;
        for (int t = 1; t <= 4 && !network; ++t)
            for (int n = 0;; ++n) {
                const TdfSection* s = gh->child("Schema " + std::to_string(n));
                if (!s) break;
                if (ieq(s->get_string("Type", 0x20, ""), "Network " + std::to_string(t))) {
                    network = true;
                    break;
                }
            }
        if (!network) continue;
        const auto dot = file.find_last_of('.');
        maps_.push_back(dot == std::string::npos ? file : file.substr(0, dot));
    }
    auto lower_less = [](const std::string& a, const std::string& b) {
        for (std::size_t i = 0;; ++i) {
            const int ca = i < a.size() ? std::tolower(static_cast<unsigned char>(a[i])) : 0;
            const int cb = i < b.size() ? std::tolower(static_cast<unsigned char>(b[i])) : 0;
            if (ca != cb) return ca < cb;
            if (ca == 0) return false;
        }
    };
    std::stable_sort(maps_.begin(), maps_.end(), lower_less);
    return maps_;
}

const render::Sprite8* MenuArt::map_preview(const std::string& map, int w, int h) {
    const std::string key = upper(map) + "|" + std::to_string(w) + "x" + std::to_string(h);
    if (auto it = previews_.find(key); it != previews_.end()) return it->second.get();
    std::unique_ptr<render::Sprite8> out;
    std::vector<std::uint8_t> tnt;
    if (vfs_.read("maps\\" + map + ".tnt", tnt) && tnt.size() >= 0x40) {
        auto rd32 = [&](std::size_t o) -> std::uint32_t {
            if (o + 4 > tnt.size()) return 0;
            std::uint32_t v;
            std::memcpy(&v, &tnt[o], 4);
            return v;
        };
        // The map file's own small picture (0x4295B0: header word 10 its offset, bit 0 of word 11
        // says it is there; u32 width, u32 height, pixels), fitted into a w x h picture
        // (0x4665D0): the part of the map the scroll extent covers (map width x 16 - 32 by
        // height x 16 - 128) keeps its shape — the longer side fills the box, centred across the
        // other — mapped with the textured-quad rules from texels (0,0) .. (pw-1, sh-1) onto
        // (x0, y0) .. (x0 + dw, y0 + dh) of a picture cleared to 0. (The original reuses the map
        // picture's buffer for the result.)
        const std::uint32_t at = rd32(0x28);
        const int pw = static_cast<int>(rd32(at)), ph = static_cast<int>(rd32(at + 4));
        const int ew = static_cast<int>(rd32(4)) * 16 - 32, eh = static_cast<int>(rd32(8)) * 16 - 128;
        if ((tnt[0x2C] & 1) && pw > 0 && ph > 0 && pw <= 4096 && ph <= 4096 && at + 8 + static_cast<std::size_t>(pw) * ph <= tnt.size() && ew != 0 &&
            eh != 0) {
            render::Sprite8 src;
            src.width = pw;
            src.height = ph;
            src.has_key = false;
            src.pixels.assign(tnt.begin() + static_cast<std::ptrdiff_t>(at) + 8, tnt.begin() + static_cast<std::ptrdiff_t>(at) + 8 + static_cast<std::ptrdiff_t>(pw) * ph);
            int dw, dh, sw, sh, x0, y0;
            if (ew >= eh) {
                dw = w;
                dh = eh * h / ew;
                sw = pw;
                sh = eh * ph / ew;
                x0 = 0;
                y0 = (h - dh) / 2;
            } else {
                dw = ew * w / eh;
                dh = h;
                sw = ew * pw / eh;
                sh = ph;
                x0 = (w - dw) / 2;
                y0 = 0;
            }
            out = std::make_unique<render::Sprite8>();
            out->width = w;
            out->height = h;
            out->has_key = false;
            out->pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            render::RasterTarget t;
            t.pixels = out->pixels.data();
            t.pitch = w;
            t.clip = {0, 0, w - 1, h - 1};
            const render::RasterVertex v[4] = {{x0, y0, 0}, {x0 + dw, y0, 0}, {x0 + dw, y0 + dh, 0}, {x0, y0 + dh, 0}};
            const int uv[4][2] = {{0, 0}, {sw - 1, 0}, {sw - 1, sh - 1}, {0, sh - 1}};
            render::textured_quad_uv(t, v, uv, src);
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

std::string image_output_dir(const std::string& game_dir, const client::Settings& settings) {
    // 0x42F9A0: the registry's "Image Output Directory", else "%s\%s" of the program's folder
    // and the user's name (GetUserNameA), "user images" when there is none.
    const std::string set = settings.get_text("Image Output Directory", "");
    if (!set.empty()) return set;
    const char* user = std::getenv("USERNAME");
    if (!user || !*user) user = std::getenv("USER");
    return game_dir + "\\" + (user && *user ? std::string(user) : std::string("user images"));
}

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
    o.fx_volume = s.get_int("fxvol", 27);
    // the display switches (0x42F9A0: bits of the options word, all on but DitheredFog)
    o.screen_chat = s.get_int("screenchat", 1) != 0;
    o.antialias = (s.get_int("Anti-Alias", 1) & 1) != 0;
    o.shadows = (s.get_int("Shadows", 1) & 1) != 0;
    o.feature_shadows = (s.get_int("FeatureShadows", 1) & 1) != 0;
    o.unit_shadows = (s.get_int("VehicleShadows", 1) & 1) != 0;
    o.shading = (s.get_int("Shading", 1) & 1) != 0;
    o.dithered_fog = (s.get_int("DitheredFog", 0) & 1) != 0;
    return o;
}

std::unique_ptr<SkirmishSession> SkirmishSession::start(const std::string& game_dir, const BattleRequest& request, int human,
                                                        const client::ClientOptions& options, const client::LoadProgress& progress,
                                                        std::string& error,
                                                        const std::function<sim::BattleOutside&(sim::BattleOutside&)>& wrap,
                                                        const std::function<void(sim::Battle&)>& attach) {
    auto s = std::unique_ptr<SkirmishSession>(new SkirmishSession);
    s->dir_ = game_dir;
    s->human_ = human;
    s->options_ = options;
    s->network_start_ = static_cast<bool>(wrap) || static_cast<bool>(attach);
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
    // Cheat commands: always in skirmish, never in campaigns, network games as the host's
    // "cheat codes" option says (the caller's options carry it).
    o.cheats = request.start.provider == 2 || (request.start.provider == 3 && options.cheats);
    s->client_ = std::make_unique<client::GameClient>(s->world_->battle(), s->world_->content(), o);
    s->world_->attach(s->client_.get());
    install_console_commands(*s->client_, *s->world_);
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
        if (r.before_start || !r.unit_rules.empty()) {
            auto before = r.before_start;
            auto rules = r.unit_rules;
            req.prepare_start = [before, rules](LoadedBattle& lb, sim::BattleStart& start) {
                // 0x46E160: each definition takes its rule from the exchange: buildable (the
                // "version compatible" bit) and the most one player may own; one without a rule
                // is neither buildable nor allowed any.
                if (!rules.empty() && lb.battle)
                    for (std::size_t k = 1; k < lb.unit_defs.units.size() && k < lb.battle->types.size(); ++k) {
                        UnitDefRecord& d = lb.unit_defs.units[k];
                        const auto it = rules.find(d.fbi_checksum);
                        const bool on = it != rules.end() && it->second.first;
                        const std::int32_t most = it != rules.end() ? it->second.second : 0;
                        d.type_flags = on ? (d.type_flags | unit_flags::kVersionCompatible) : (d.type_flags & ~unit_flags::kVersionCompatible);
                        d.max_this_unit = most;
                        auto& t = lb.battle->types[k];
                        t.create.type_flags = d.type_flags;
                        t.create.max_per_player = most;
                        t.settle.type_flags = d.type_flags;
                    }
                // The loading handshake runs once the data is loaded, before the battle starts.
                if (before) before(start);
            };
        }
        const int human = r.start.local_player;
        client::Settings none;
        // The user's settings (screen size, speeds, interface type) as the front end passes them.
        client::ClientOptions opt = client_options(r.settings ? *r.settings : (fe ? fe->settings() : none), 640, 480);
        opt.cheats = r.cheats;
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

int SkirmishSession::difficulty() const { return world_ ? world_->request().start.ai_difficulty : 1; }

std::string SkirmishSession::battle_name() const {
    // The map's [GlobalHeader] missionname (map info +0xA14).
    if (!world_) return {};
    std::vector<std::uint8_t> ota;
    if (!world_->vfs_.read("maps\\" + world_->request().map + ".ota", ota)) return {};
    const TdfParseResult parsed = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
    const TdfSection* h = parsed.root ? parsed.root->child("GlobalHeader") : nullptr;
    return h ? h->get_string("missionname", 0x100, "") : std::string();
}

client::BattleLauncher SkirmishSession::restart_launcher(int difficulty) {
    // The same request (map, players, options) again with the difficulty chosen; the inputs the
    // original takes from its clocks are taken anew, and the C runtime's rand() goes on from
    // where the battle left it (one rand() for the whole program in the original).
    if (network_start_ || !world_) return {};
    BattleRequest req = world_->request();
    req.start.ai_difficulty = difficulty;
    req.start.battle_rng_seed = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    req.start.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
    req.start.crt_rand_state = world_->battle().crt_rand.state();
    const std::string dir = dir_;
    const int human = human_;
    const client::ClientOptions opt = options_;
    return [dir, req, human, opt](const client::LoadProgress& progress, std::string& error) -> std::unique_ptr<client::BattleSession> {
        return SkirmishSession::start(dir, req, human, opt, progress, error);
    };
}

bool SkirmishSession::end_table(client::EndTable& t) {
    // The players' records at the end (the original's statistics pass, 0x41DC20) and the map's
    // score factors: [GlobalHeader] killmul and timemul of its OTA (map info +0xD54 / +0xD58).
    sim::Battle& b = world_->battle();
    const auto& req = world_->request();
    std::array<client::EndPlayer, 10> in{};
    for (int p = 0; p < 10; ++p) {
        const sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
        const sim::PlayerEconomy& e = b.player_economy[static_cast<std::size_t>(p)];
        client::EndPlayer& o = in[static_cast<std::size_t>(p)];
        o.active = s.active;
        o.controller = s.controller;
        o.state = s.state;
        o.units_created = s.units_created;
        o.watching = b.settings.player_sight[static_cast<std::size_t>(p)].watching;
        o.colour = req.start.players[static_cast<std::size_t>(p)].colour;
        o.kills = static_cast<std::int16_t>(s.units_killed);
        o.losses = static_cast<std::int16_t>(s.units_lost);
        o.energy_produced = e.energy_produced;
        o.metal_produced = e.metal_produced;
        o.energy_wasted = e.energy_wasted;
        o.metal_wasted = e.metal_wasted;
        o.name = req.start.players[static_cast<std::size_t>(p)].name;
        if (o.name.empty()) o.name = (req.start.players[static_cast<std::size_t>(p)].kind == sim::PlayerKind::Computer ? "Computer " : "Player ") + std::to_string(p + 1);
    }
    float kill_mul = 0.0f, time_mul = 0.0f;
    {
        std::vector<std::uint8_t> ota;
        if (world_->vfs_.read("maps\\" + req.map + ".ota", ota)) {
            const TdfParseResult parsed = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
            if (const TdfSection* h = parsed.root ? parsed.root->child("GlobalHeader") : nullptr) {
                kill_mul = static_cast<float>(h->get_float("killmul", 0.0));
                time_mul = static_cast<float>(h->get_float("timemul", 0.0));
            }
        }
    }
    t = client::end_table(in, b.game_tick(), kill_mul, time_mul);
    const int me = b.settings.local_player;
    t.won = (b.end_game.flags & sim::end_game_flags::kWon) != 0;
    t.watching = me >= 0 && me < 10 && b.players[static_cast<std::size_t>(me)].active && b.settings.player_sight[static_cast<std::size_t>(me)].watching;
    return true;
}

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
