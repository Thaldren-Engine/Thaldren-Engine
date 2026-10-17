// The battle loading screen (spec: docs/RE_MEMORY.md, "Client", "Loading screen (exact)").

#include "ta/client_loading.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "client/canvas.h"
#include "client/menu.h"
#include "ta/client_menus.h"

namespace ta {
namespace {

// Text in the interface's picture font from (x, y) (0x4A50E0): characters below 0x20 and those
// without a glyph are skipped, a space only moves on; with a width limit (>= 0) a glyph wider
// than what is left ends the text; level > 0 puts the glyphs' pixels through that row of the
// light table, else they keep their own colours.
void picture_text(render::Image8& out, const client::Font& font, const std::string& s, int x, int y, int width, int level,
                  const std::vector<std::uint8_t>& light) {
    const render::ClipRect clip = out.bounds();
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

// The console font's height (fonts\COMIX.FNT's first byte, the display's current one-bit font):
// the map line stands one and a half of it above the bottom.
int console_font_height(MenuArt& art) {
    static int h = -1;
    if (h < 0) {
        std::vector<std::uint8_t> f;
        h = art.vfs().read("fonts\\COMIX.FNT", f) && !f.empty() ? f[0] : 0;
    }
    return h;
}

}  // namespace

void draw_loading_screen(MenuArt& art, render::Image8& out, client::LoadingState& st) {
    const render::ClipRect clip = out.bounds();
    const render::Palette& pal = art.content().palette;  // the game palette is on the display
    const client::Font* font = art.content().font;       // the interface's picture font (hattfont12)
    const std::vector<std::uint8_t>& light = art.content().light;
    // The picture as it is (its own palette is not applied), copied to the top-left corner.
    if (const render::Sprite8* bg = art.picture("loadgame2bg")) {
        for (int y = 0; y < bg->height && y < out.height; ++y)
            for (int x = 0; x < bg->width && x < out.width; ++x) out.row(y)[x] = bg->pixels[static_cast<std::size_t>(y * bg->width + x)];
    }
    // Highlights fade by two at each redraw on a later clock tick (30 a second).
    const std::uint32_t tick = static_cast<std::uint32_t>(static_cast<std::uint64_t>(st.now_ms) * 30u / 1000u);
    if (st.flash_tick < tick) {
        st.flash_tick = tick;
        for (int& f : st.flash)
            if (f) f = std::max(0, f - 2);
    }
    // "Map: <name>" centred, one and a half console-font heights above the bottom (not for
    // missions).
    if (st.map_line && font) {
        const std::string t = "Map: " + st.map;
        const int y = static_cast<int>(static_cast<double>(out.height) - static_cast<double>(console_font_height(art)) * 1.5);
        picture_text(out, *font, t, out.width / 2 - font->text_width(t) / 2, y, -1, 0, light);
    }
    // The six rows: label at x 90, a bar from x 205 growing 3.5 pixels per percent (20 high),
    // interface colour 12 while under 100 %, 10 at 100 %; the light bar frame over it.
    static const char* const labels[client::kLoadRows] = {"Textures", "Terrain", "Units", "Animation", "3D Data", "Explosions"};
    static const int ys[client::kLoadRows] = {0x87, 0xB1, 0xDA, 0x106, 0x130, 0x15B};
    const std::vector<render::Sprite8>* bar = art.sequence("anims\\commongui.GAF", "LIGHTBAR");
    for (int r = 0; r < client::kLoadRows; ++r) {
        const std::size_t i = static_cast<std::size_t>(r);
        const int pct = std::clamp(st.rows[i], 0, 255) & 0xFF;
        const std::uint8_t col = art.gui_colour(pal, pct < 100 ? 12 : 10);
        if (pct == 100 && st.seen[i] != 100) st.flash[i] = 30;
        st.seen[i] = pct;
        const int y = ys[r];
        if (font) picture_text(out, *font, labels[r], 0x5A, y, -1, st.flash[i], light);
        client::fill_rect(out, clip, 0xCD, y, 0xCD + pct * 7 / 2, y + 20, col);
        // (The frame's offsets are cleared: its top-left corner goes at the point.)
        if (bar && !bar->empty()) render::blit(out, clip, bar->front(), 0xCD + bar->front().x_offset, y + bar->front().y_offset);
    }
    if (!st.network) return;
    // Network games: until every machine has loaded, every counted player's bar along the
    // bottom (620 pixels shared out from x 11, each two pixels narrower than its share,
    // y 420..435: interface colour 4, the loaded part in colour 10, the name over it) and the
    // count of players ready; then only "Synchronization complete". The line is at (10, 400).
    const int n = static_cast<int>(st.players.size());
    int ready = 0;
    for (const client::LoadingPlayer& p : st.players)
        if (p.percent == 100 && p.loaded) ++ready;
    if (n > 0 && !st.synchronised) {
        const int w = 620 / n;
        int x = 11;
        for (const client::LoadingPlayer& p : st.players) {
            client::fill_rect(out, clip, x, 420, x + w - 2, 435, art.gui_colour(pal, 4));
            client::fill_rect(out, clip, x, 420, x + (p.percent & 0xFF) * (w - 2) / 100, 435, art.gui_colour(pal, 10));
            if (font) picture_text(out, *font, p.name, x, 420, w - 2, 0, light);
            x += w;
        }
    }
    std::string status;
    if (st.synchronised) {
        status = "Synchronization complete";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s.  %i %s", "Waiting for other players", ready, ready == 1 ? "player ready" : "players ready");
        status = buf;
    }
    if (font) picture_text(out, *font, status, 10, 400, -1, 0, light);
    // After the frame this machine's players report the average of the six rows.
    int sum = 0;
    for (int v : st.rows) sum += std::clamp(v, 0, 255);
    for (client::LoadingPlayer& p : st.players)
        if (p.local) p.percent = sum / client::kLoadRows;
}

}  // namespace ta
