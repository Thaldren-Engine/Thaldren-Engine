// The Space-held panels over the battle view and the kills ranking behind the second one
// (spec: docs/RE_MEMORY.md, "Client", "Space-held panels").

#include <algorithm>
#include <cstdio>

#include "client/canvas.h"
#include "client/game_client.h"
#include "render/raster.h"

namespace client {
namespace {

// A layer drawn twice (fills 0 and 255): the pixels both drawings agree on are drawn, the rest
// is the key (an index none of the drawn pixels uses).
render::Sprite8 keyed_layer(const render::Image8& a, const render::Image8& b) {
    render::Sprite8 s;
    s.width = a.width;
    s.height = a.height;
    std::vector<bool> used(256, false);
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        if (a.pixels[i] == b.pixels[i]) used[a.pixels[i]] = true;
    int key = 255;
    while (key > 0 && used[static_cast<std::size_t>(key)]) --key;
    s.key = static_cast<std::uint8_t>(key);
    s.has_key = true;
    s.pixels.resize(a.pixels.size());
    for (std::size_t i = 0; i < a.pixels.size(); ++i) s.pixels[i] = a.pixels[i] == b.pixels[i] ? a.pixels[i] : s.key;
    return s;
}

// A row of the shade or light table as a mixing table: entry [src * 256 + dst] = the row's entry
// for dst taken as a signed byte (0x4BF4D0 indexes the row with the pixel as a signed byte, so
// pixels from 0x80 up read the row before), for any src.
render::BlendTable row_table(const std::vector<std::uint8_t>& table32, int row) {
    render::BlendTable t(65536, 0);
    for (int d = 0; d < 256; ++d) {
        const int at = row * 256 + static_cast<std::int8_t>(d);
        const std::uint8_t v = at >= 0 && static_cast<std::size_t>(at) < table32.size() ? table32[static_cast<std::size_t>(at)] : 0;
        for (int s = 0; s < 256; ++s) t[static_cast<std::size_t>(s * 256 + d)] = v;
    }
    return t;
}

// A box of the frame (inclusive) through a mixing table.
void through_box(render::DrawList& list, const render::BlendTable& table, int x0, int y0, int x1, int y1, const render::ClipRect& clip) {
    if (x1 < x0 || y1 < y0 || table.size() < 65536) return;
    render::Sprite8 s;
    s.width = x1 - x0 + 1;
    s.height = y1 - y0 + 1;
    s.has_key = false;
    s.pixels.assign(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height), 1);
    list.through(list.keep(std::move(s)), x0, y0, clip, false, &table);
}

}  // namespace

int GameClient::draw_picture_text(render::Image8& o, const render::ClipRect& clip, const Font& f, const std::string& s, int x, int y,
                                  int width, int light_row) const {
    for (unsigned char ch : s) {
        const Glyph& g = f.glyphs[ch];
        if (ch < 0x20 || !g.present) continue;
        if (width != -1 && width < g.width) return x;
        if (ch != ' ')
            for (int r = 0; r < g.rows; ++r)
                for (int k = 0; k < g.width; ++k) {
                    const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                    if (!g.ink[at]) continue;
                    std::uint8_t v = g.colours[at];
                    if (light_row != 0 && c_.light_levels.size() >= 32 * 256)
                        v = c_.light_levels[static_cast<std::size_t>(std::clamp(light_row, 0, 31) * 256 + v)];
                    put_pixel(o, clip, x + g.left + k, y + g.top + r, v);
                }
        if (width != -1) {
            width -= g.width;
            if (width < 0) return x;
        }
        x += g.width;
    }
    return x;
}

bool GameClient::panel_player(int k) const {
    // 0x4948E0's test of a player record: in use, a controller of 1..3, not out of the game, still
    // holding units or never having had any, not a watcher.
    const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(k)];
    return p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10 && (p.alive_count != 0 || p.units_created == 0) &&
           !b_.settings.player_sight[static_cast<std::size_t>(k)].watching;
}

void GameClient::on_kill_counted(int a, const sim::Unit& victim) {
    // The end of the kill handler (0x4866D0): in skirmish and network games a counted kill may
    // move the attacker up the ranking: the best rank among players (with a controller, not
    // watching) that have fewer kills and a better rank is taken, the players from there down
    // to its old rank move down one; reaching the top prints "<name> has taken the lead with
    // <n> kills". (Kills are commander kills in the deathmatch mode.) With the kills and losses
    // panel kept out (F4), the attacker's kills and the victim's player's losses flash.
    if (a < 0 || a >= 10) return;
    auto score = [&](int k) -> std::int16_t {
        const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(k)];
        return static_cast<std::int16_t>(b_.settings.game_mode == 2 ? p.commanders_killed : p.units_killed);
    };
    const sim::PlayerSlot& pa = b_.players[static_cast<std::size_t>(a)];
    const std::uint8_t old = rank_[static_cast<std::size_t>(a)];
    if (pa.active && pa.controller >= 1 && pa.controller <= 3 && pa.state != 10 && (b_.settings.provider == 2 || b_.settings.provider == 3) &&
        old != 0) {
        const std::int16_t mine = score(a);
        std::uint8_t best = old;
        for (int k = 0; k < 10; ++k) {
            const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(k)];
            if (p.controller == 0 || b_.settings.player_sight[static_cast<std::size_t>(k)].watching) continue;
            if (score(k) < mine && rank_[static_cast<std::size_t>(k)] < best) best = rank_[static_cast<std::size_t>(k)];
        }
        if (best < old) {
            for (std::uint8_t& r : rank_)
                if (best <= r && r < old) ++r;
            rank_[static_cast<std::size_t>(a)] = best;
            if (best == 0) {
                char line[100];
                // (The format goes through the translation, 0x486C4B.)
                std::snprintf(line, sizeof line, tr("%s has taken the lead with %d kills").c_str(), c_.player_names[static_cast<std::size_t>(a)].c_str(),
                              static_cast<int>(mine));
                message(line, kGameNote);
            }
        }
    }
    if (opt_.scoreboard) {
        kill_flash_[static_cast<std::size_t>(a)] = 0x1E;
        const int owner = victim.id < b_.ticks.size() ? b_.ticks[victim.id].owner : -1;
        const int v = owner >= 0 && owner < 10 ? b_.players[static_cast<std::size_t>(owner)].state : -1;
        if (v >= 0 && v < 10) loss_flash_[static_cast<std::size_t>(v)] = 0x1E;
    }
}

void GameClient::panels_frame(std::uint32_t now_ms) {
    // Both panels are out while Space is held, unless a text field takes the keys (the chat
    // line); the kills and losses panel also while F4 keeps it out.
    const bool space = keys_down_[' '] && !chat_;
    // 0x4689C0: one move per 15 ms of wall clock: out by a third of the way left (at least 1)
    // to -31, back by a third (at least 1) to 0; "Panel" when it starts out from rest at 0 or
    // back from -31, "Options" when it comes to rest.
    if (static_cast<std::int32_t>(now_ms - info_next_ms_) > 0) {
        info_next_ms_ = now_ms + 15;
        int v = info_slide_;
        if (space) {
            if (v > -31) {
                if (v == 0) play_ui(c_.ui_sounds.panel);
                v -= std::max(1, (v + 31) / 3);
                if (v == -31) play_ui(c_.ui_sounds.options);
            }
        } else if (v < 0) {
            if (v == -31) play_ui(c_.ui_sounds.panel);
            v += std::max(1, -v / 3);
            if (v == 0) play_ui(c_.ui_sounds.options);
        }
        info_slide_ = v;
    }
    // 0x4948E0 (skirmish and network games, every frame): the flashes fade by 2 whenever the
    // interface clock (1/30 s) has passed the time kept, which then becomes the clock + 1; the
    // panel moves a quarter of the way (at least 1) to 125 (out) or 0.
    const std::uint32_t ticks = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now_ms) * 30 / 1000);
    bps_.now = ticks;
    if (b_.settings.provider != 2 && b_.settings.provider != 3) return;
    if (static_cast<std::int32_t>(ticks - score_next_tick_) > 0) {
        score_next_tick_ = ticks + 1;
        for (std::size_t k = 0; k < 10; ++k) {
            if (kill_flash_[k]) kill_flash_[k] = static_cast<std::uint8_t>(kill_flash_[k] - 2);
            if (loss_flash_[k]) loss_flash_[k] = static_cast<std::uint8_t>(loss_flash_[k] - 2);
        }
    }
    int v = score_slide_;
    if (opt_.scoreboard || space) {
        if (v < 0x7D) {
            if (v == 0) play_ui(c_.ui_sounds.panel);
            v += std::max(1, (0x7D - v) / 4);
            if (v > 0x7C) {
                v = 0x7D;
                play_ui(c_.ui_sounds.options);
            }
        }
    } else if (v >= 1) {
        if (v == 0x7D) play_ui(c_.ui_sounds.panel);
        v -= std::max(1, v / 4);
        if (v < 1) {
            v = 0;
            play_ui(c_.ui_sounds.options);
        }
    }
    score_slide_ = v;
}

void GameClient::draw_panels(render::DrawList& list) {
    const int w = opt_.width, h = opt_.height;
    const render::ClipRect screen{0, 0, w - 1, h - 1};
    if (shade8_.empty() && c_.shade_levels.size() >= 32 * 256) shade8_ = row_table(c_.shade_levels, 8);
    if (light31_.empty() && c_.light_levels.size() >= 32 * 256) {
        light31_ = row_table(c_.light_levels, 31);
        light20_ = row_table(c_.light_levels, 20);
    }
    // The kills and losses panel (0x4948E0, whole screen clip): the box (w - slide .. + 125, 32 ..
    // players x 40 + 46) through shade level 8; "Kills" and "Losses" headings; then by rank from 0
    // each listed player of that rank: the local player's row lit (light levels 31 then 20), its
    // colour logo stretched over the row, its name, its kills (flashing) and its losses
    // (flashing, right-aligned). A rank nobody holds moves every listed player below it up one.
    if ((b_.settings.provider == 2 || b_.settings.provider == 3) && score_slide_ >= 1 && c_.picture_font) {
        const Font& f = *c_.picture_font;
        const int x0 = w - score_slide_, x1 = x0 + 0x7D;
        const int count = b_.settings.player_count;
        through_box(list, shade8_, x0, 0x20, x1, count * 0x28 + 0x2E, screen);
        const int tw = (x1 - x0) - 6;
        render::Image8 a(w, h, 0), bb(w, h, 255);
        auto text_width = [&](const std::string& s) {
            int n = 0;
            for (unsigned char ch : s)
                if (f.glyphs[ch].present) n += f.glyphs[ch].width;
            return n;
        };
        auto both = [&](const auto& fn) {
            fn(a);
            fn(bb);
        };
        both([&](render::Image8& o) {
            // (Both translated, 0x494AC5 / 0x494B10.)
            draw_picture_text(o, screen, f, tr("Kills"), x0 + 2, 0x20, tw, 0);
            draw_picture_text(o, screen, f, tr("Losses"), x1 - text_width(tr("Losses")) - 2, 0x20, tw, 0);
        });
        // (the headings go first: the local player's lit row may reach under them)
        list.blit(list.keep(keyed_layer(a, bb)), 0, 0, screen, false);
        std::fill(a.pixels.begin(), a.pixels.end(), 0);
        std::fill(bb.pixels.begin(), bb.pixels.end(), 255);
        int y = 0x20 + 0xF;
        for (int r = 0; r < count; ++r) {
            int k = 0;
            for (; k < 10; ++k)
                if (panel_player(k) && rank_[static_cast<std::size_t>(k)] == r) break;
            if (k == 10) {
                for (int q = 0; q < 10; ++q)
                    if (panel_player(q) && r < rank_[static_cast<std::size_t>(q)]) --rank_[static_cast<std::size_t>(q)];
                continue;
            }
            if (k == opt_.player) {
                through_box(list, light31_, x0 + 4, y - 1, x1 - 4, y + 0x26, screen);
                through_box(list, light20_, x0 + 4, y - 1, x1 - 4, y + 0x26, screen);
            }
            const sim::PlayerSlot& p = b_.players[static_cast<std::size_t>(k)];
            const int kills = static_cast<std::int16_t>(b_.settings.game_mode == 2 ? p.commanders_killed : p.units_killed);
            const int losses = static_cast<std::int16_t>(b_.settings.game_mode == 2 ? p.commanders_lost : p.units_lost);
            const int lx0 = x0 + 7, ly0 = y + 1, lx1 = x0 + tw, ly1 = y + 0x25;
            both([&](render::Image8& o) {
                if (c_.team_logos && !c_.team_logos->empty()) {
                    const render::Sprite8& s =
                        (*c_.team_logos)[static_cast<std::size_t>(c_.player_colour[static_cast<std::size_t>(k)]) % c_.team_logos->size()];
                    render::RasterTarget t;
                    t.pixels = o.pixels.data();
                    t.pitch = o.width;
                    t.clip = screen;
                    const render::RasterVertex v[4] = {{lx0, ly0, 0}, {lx1, ly0, 0}, {lx1, ly1, 0}, {lx0, ly1, 0}};
                    const int uv[4][2] = {{1, 1}, {s.width - 1, 1}, {s.width - 1, s.height - 1}, {1, s.height - 1}};
                    render::textured_quad_uv(t, v, uv, s);
                }
                draw_picture_text(o, screen, f, c_.player_names[static_cast<std::size_t>(k)], lx0 + 2, ly0 + 5, tw, 0);
                char num[16];
                std::snprintf(num, sizeof num, "%d", kills);
                draw_picture_text(o, screen, f, num, lx0 + 2, ly0 + 0x14, tw, kill_flash_[static_cast<std::size_t>(k)]);
                std::snprintf(num, sizeof num, "%d", losses);
                draw_picture_text(o, screen, f, num, lx1 - text_width(num) - 2, ly0 + 0x14, tw, loss_flash_[static_cast<std::size_t>(k)]);
            });
            y += 0x28;
        }
        list.blit(list.keep(keyed_layer(a, bb)), 0, 0, screen, false);
    }
    // The game information panel (0x4689C0, clipped to the view): the light bar's art with its
    // top-left at (view left, view bottom + slide), and in the second picture font "Game Time :
    // hh:mm:ss" (game steps), "Total Units : n  (Max m)" and "Game Speed Normal" or "Game Speed
    // +d" (the effective speed less 10), followed by " (+d)" of the set speed when they differ.
    if (info_slide_ != 0) {
        const render::ClipRect view = view_rect();
        const int x = view.x0, y = view.y1 + info_slide_;
        if (c_.info_panel) list.blit(c_.info_panel, x, y, view, true);
        if (const Font* f = c_.small_picture_font) {
            const std::uint32_t t = b_.game_tick();
            const std::uint32_t hours = t / 108000u;
            const std::int32_t rest = static_cast<std::int32_t>(t - hours * 108000u);
            const std::int32_t minutes = rest / 1800, seconds = (rest - minutes * 1800) / 30;
            // (The words go through the translation: 0x468B74, 0x468BD7, 0x468C1D, 0x468C57.)
            char time_text[256], units_text[256], speed_text[256], speed[128];
            std::snprintf(time_text, sizeof time_text, "%s : %02d:%02d:%02d", tr("Game Time").c_str(), static_cast<int>(hours), minutes, seconds);
            const sim::PlayerSlot& me = b_.players[static_cast<std::size_t>(opt_.player)];
            std::snprintf(units_text, sizeof units_text, "%s : %d  (Max %d)", tr("Total Units").c_str(), static_cast<int>(me.alive_count),
                          static_cast<int>(static_cast<std::uint16_t>(b_.settings.unit_limit)));
            if (budget_.effective_speed == 10) std::snprintf(speed, sizeof speed, "%s", tr("Normal").c_str());
            else std::snprintf(speed, sizeof speed, "%+d", static_cast<int>(budget_.effective_speed) - 10);
            std::snprintf(speed_text, sizeof speed_text, "%s %s", tr("Game Speed").c_str(), speed);
            std::string speed_line = speed_text;
            if (budget_.effective_speed != budget_.game_speed) {
                char more[16];
                std::snprintf(more, sizeof more, " (%+d)", static_cast<int>(budget_.game_speed) - 10);
                speed_line += more;
            }
            render::Image8 a(w, h, 0), bb(w, h, 255);
            for (render::Image8* o : {&a, &bb}) {
                draw_picture_text(*o, view, *f, time_text, x + 0x19, y + 10, -1, 0);
                draw_picture_text(*o, view, *f, units_text, x + 0xBE, y + 10, -1, 0);
                draw_picture_text(*o, view, *f, speed_line, x + 0x17C, y + 10, -1, 0);
            }
            list.blit(list.keep(keyed_layer(a, bb)), 0, 0, view, false);
        }
    }
    if (opt_.bps) draw_bps(list);
}

void GameClient::draw_bps(render::DrawList& list) {
    // 0x416150: the rates are remade once more than 30 ticks of the 1/30 s clock have passed:
    // (total x 30 - total then x 30) / ticks, in unsigned 32 bits.
    BpsState& s = bps_;
    const std::uint32_t passed = s.now - s.tick;
    if (passed > 30) {
        s.send_rate = (s.sent * 30u - s.prev_sent * 30u) / passed;
        s.receive_rate = (s.received * 30u - s.prev_received * 30u) / passed;
        s.prev_sent = s.sent;
        s.prev_received = s.received;
        s.tick = s.now;
    }
    // 0x468380 (after the game information panel, clipped to the view): in the side's font and
    // the interface's colour 15, "Send - %1.1f K/s" (the rate x 0.001) at (129, height - 95), and
    // under it (one font height down) a box 129 .. 193 x 8 rows high, outlined, filled from the
    // left by rate x 100 / 5600 percent (at most 100); then "Receive - ..." one row below the
    // box and its box the same way. The text routine draws a line only when it fits the frame.
    if (!side_ || !side_->font) return;
    const Font& f = *side_->font;
    const int w = opt_.width, h = opt_.height;
    const render::ClipRect view = view_rect();
    const std::uint8_t colour = c_.hud[15];
    render::Image8 a(w, h, 0), bb(w, h, 255);
    int y = h - 0x5F;
    for (int k = 0; k < 2; ++k) {
        const std::uint32_t rate = k == 0 ? s.send_rate : s.receive_rate;
        char text[64];
        std::snprintf(text, sizeof text, k == 0 ? "Send - %1.1f K/s" : "Receive - %1.1f K/s", static_cast<double>(rate) * 0.001);
        const int tw = f.text_width(text);
        const int y0 = y + f.height, y1 = y0 + 8;
        std::uint32_t pct = rate * 100u / 5600u;
        int fill = static_cast<int>(pct);
        if (fill > 99) fill = 100;
        for (render::Image8* o : {&a, &bb}) {
            if (!(0x81 < 0 || y < 0 || 0x81 + tw > w - 1 || y + f.height > h - 1)) draw_text(*o, o->bounds(), f, text, 0x81, y, colour);
            outline_rect(*o, view, 0x81, y0, 0xC1, y1, colour);
            if (fill > 0) fill_rect(*o, view, 0x81, y0, (0xC1 - 0x81) * fill / 100 + 0x81, y1, colour);
        }
        y = y1 + 1;
    }
    list.blit(list.keep(keyed_layer(a, bb)), 0, 0, view, false);
}

void GameClient::draw_profile(render::DrawList& list) {
    // Render_DrawBattleFrame's end with GS+0x38DD5 set: per category n (0x46B900; Network, Units,
    // Logic, Render Static, Render Stuff, Render Fog, SFX, Weapon, Misc) a line in colour 255 from
    // (width - 290, 38) to (639, 9 x the font's height + 41) (meant as a box; the same line nine
    // times), the name at (width - 85, n x height + 40) in the text colour set last, and a bar
    // from width - 90 - 2 x (its share of the last pass's milliseconds, in percent) to width - 90,
    // one font height tall, in colour n + 1.
    if (!side_ || !side_->font) return;
    static const char* const kNames[9] = {"Network", "Units", "Logic", "Render Static", "Render Stuff", "Render Fog", "SFX", "Weapon", "Misc"};
    const Font& f = *side_->font;
    const int w = opt_.width, h = opt_.height;
    const render::ClipRect screen{0, 0, w - 1, h - 1};
    const std::uint8_t text = last_text_colour();
    render::Image8 a(w, h, 0), bb(w, h, 255);
    const int fh = f.height;
    for (int n = 0; n < 9; ++n) {
        const int y = fh * n + 0x28;
        const std::int32_t pct = static_cast<std::int32_t>(profile_shown_[static_cast<std::size_t>(n)] * 100u) /
                                 static_cast<std::int32_t>(profile_total_);
        for (render::Image8* o : {&a, &bb}) {
            render::draw_line(*o, screen, w - 0x122, 0x26, 0x27F, fh * 9 + 0x29, 0xFF);
            const int tw = f.text_width(kNames[n]);
            if (!(w - 0x55 < 0 || y < 0 || w - 0x55 + tw > w - 1 || y + fh > h - 1)) draw_text(*o, o->bounds(), f, kNames[n], w - 0x55, y, text);
            fill_rect(*o, screen, w - 0x5A + pct * -2, y, w - 0x5A, y + fh, static_cast<std::uint8_t>(n + 1));
        }
    }
    list.blit(list.keep(keyed_layer(a, bb)), 0, 0, screen, false);
}

}  // namespace client
