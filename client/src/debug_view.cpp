// The original's debug view: developer mode's F11 (0x495E90 case 0xEC; GS+0x3923B bit 1), its
// keys (0x4956C0), the status line and the +NetStats numbers over the view (Render_DrawBattleFrame
// 0x468CF0, 0x415FA0), the terrain views (0x418310 with GS+0x14280), the first selected unit's
// route (0x417F30 -> the route goal's 0x417E00) and the bottom bar's numbers (0x46A860).
// Spec: docs/RE_MEMORY.md, "Developer mode".

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "client/canvas.h"
#include "client/game_client.h"
#include "render/raster.h"
#include "sim/battle.h"
#include "sim/motion.h"

namespace client {

namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;

// A layer drawn twice (fills 0 and 255): the pixels both drawings agree on are drawn, the rest is
// the key (an index none of the drawn pixels uses).
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

// The text routine (0x4C14F0) draws a line only when its whole box (x .. x + width, y .. y + the
// font's height) lies inside the surface's clip rectangle.
void clip_text(render::Image8& o, const render::ClipRect& clip, const Font& f, const std::string& s, int x, int y, int colour) {
    set_last_text_colour(static_cast<std::uint8_t>(colour));
    const int w = f.text_width(s);
    if (x < clip.x0 || x > clip.x1 || x + w < clip.x0 || x + w > clip.x1 || y < clip.y0 || y > clip.y1 || y + f.height < clip.y0 ||
        y + f.height > clip.y1)
        return;
    draw_text(o, clip, f, s, x, y, colour);
}

// Whole division toward zero by 16 (the cell of a pixel scroll, as the original's sar with
// rounding).
int sixteenth(int v) { return (v + ((v >> 31) & 15)) >> 4; }

}  // namespace

// ---- keys ------------------------------------------------------------------------------------

void GameClient::toggle_debug_view() {
    // 0x495E90 case 0xEC (F11) with developer mode: the view's bit flips; off, the INFO bit and
    // the terrain view go and the battle layout's quick keys come back; on, they go.
    if (!opt_.debug_commands) return;
    debug_.on = !debug_.on;
    if (!debug_.on) {
        debug_.info = false;
        debug_.grid = 0;
        debug_.quick_keys = true;
    } else {
        debug_.quick_keys = false;
    }
}

bool GameClient::debug_key(int k) {
    // 0x4956C0: every key the battle's key routine took while the view is on comes here too.
    switch (k) {
    case '=':
        // Every player in the game (a record with a controller 1..3, not out of the game) gets its
        // storages full: the stored amounts become the capacities (+0x8C = +0xA4, +0x98 = +0xA8).
        // Only on this machine: nothing is sent.
        for (std::size_t s = 0; s < b_.players.size() && s < 10; ++s) {
            sim::PlayerSlot& p = b_.players[s];
            if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) continue;
            p.energy_stored = b_.player_economy[s].energy_storage;
            p.metal_stored = b_.player_economy[s].metal_storage;
        }
        return true;
    case 'P':
    case 'p':
        // The display's page flipping on / off (0x4C61B0; shown as PFSTATE).
        debug_.page_flip = k == 'P';
        return true;
    case ']': {
        // The unit under the pointer (GS+0x2CBA) dies at its next pass: no last attacker (player
        // 10), the dying flag set.
        const std::uint16_t id = hover_;
        if (id < b_.units.size() && b_.ticks[id].type_index != 0) {
            b_.ticks[id].last_attacker_slot = 10;
            b_.ticks[id].last_attacker = nullptr;
            b_.units[id].flags |= sim::unit_tick_flags::kDying;
        }
        return true;
    }
    case 'i': debug_.info = !debug_.info; return true;
    case 'm':
        if (++debug_.grid == 5) debug_.grid = 0;
        return true;
    default: return false;
    }
}

// ---- the terrain views (0x418310) --------------------------------------------------------------

void GameClient::draw_terrain_debug(render::DrawList& out, const render::Camera& camera, std::uint32_t& rand_state) const {
    // The view's cells as the contour lines walk them (see draw_contours): row after row while a
    // cell's top-left corner lies above the screen's bottom. Per cell the corners c0 (x, z),
    // c1 (x + 1, z), c2 (x + 1, z + 1), c3 (x, z + 1) at (16 (x + 8) - scroll x, 16 (z + 2) -
    // height / 2 - scroll y); then by the mode:
    //  1  the first selected unit's movement class: the cell's grade 0 / 1 / 2 as the two
    //     diagonals in interface colour 4 / 14 / 10 (3: none); a route search goal cell gets "G"
    //     (smlfont) at c0 in colour rand() & 0xFF (the program's one rand(): the simulation's
    //     sequence moves on); an open / closed cell (flags without the goal bit not 0 or 3) an
    //     arrow towards c0 + 8 from where it was entered (14 pixels, heads of 4), in colour 15
    //     (open), 4 (closed), else the colour of the arrow before (the first one: a stack byte
    //     the terrain blit left there, see below);
    //  2  the top and left edges in colour 15 above the sea level, else 13; a cell with a unit
    //     filled in the unit's id & 0xFF, else with a feature in (feature - 0x38) & 0xFF; a second
    //     unit: both diagonals in its id & 0xFF; a yard map mark: a diamond in colour 15;
    //  3  the edges as in 2 and the cell's metal as a number at c0 + 2 in colour 15 (smlfont);
    //  4  the edges in colour 0 and, where the viewed player's sight counts see the 2 x 2 block,
    //     a box of 11 x 11 at c0 in colour 15.
    // Then the contour lines of the cell when +contour is on. Everything is clipped to the view;
    // a number or mark is drawn only when its whole box lies in it.
    const int mode = debug_.grid;
    const sim::MapGrid& map = b_.map;
    if (map.width < 2 || map.height < 2 || map.cells.size() < static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height)) return;
    const int w = opt_.width, h = opt_.height;
    const render::ClipRect clip = camera.view_clip();
    render::Image8 a(w, h, 0), bb(w, h, 255);
    auto line = [&](int x0, int y0, int x1, int y1, std::uint8_t c) {
        render::draw_line(a, clip, x0, y0, x1, y1, c);
        render::draw_line(bb, clip, x0, y0, x1, y1, c);
    };
    auto text = [&](const Font* f, const std::string& s, int x, int y, int c) {
        if (!f) return;
        clip_text(a, clip, *f, s, x, y, c);
        clip_text(bb, clip, *f, s, x, y, c);
    };
    // The movement class of the first selected unit (mode 1).
    const sim::MoveClass* cls = nullptr;
    if (mode == 1) {
        const int p = opt_.player;
        if (p >= 0 && p < static_cast<int>(b_.players.size())) {
            const sim::PlayerSlot& ps = b_.players[static_cast<std::size_t>(p)];
            // 0x48C190(0, 0): the local player's units from its second one to its last, then its
            // first: the first selected.
            const int lo = ps.first_unit, hi = ps.last_unit;
            auto selected = [&](int i) {
                return i > 0 && static_cast<std::size_t>(i) < b_.units.size() && (b_.units[static_cast<std::size_t>(i)].flags & sim::unit_tick_flags::kSelected);
            };
            int found = 0;
            for (int i = lo + 1; i <= hi && !found; ++i)
                if (selected(i)) found = i;
            if (!found && selected(lo)) found = lo;
            if (found) {
                const std::uint16_t t = b_.ticks[static_cast<std::size_t>(found)].type_index;
                if (t < b_.types.size()) {
                    const int mc = b_.types[t].route.move_class;
                    if (mc >= 0 && mc < static_cast<int>(b_.move_classes.size())) cls = &b_.move_classes[static_cast<std::size_t>(mc)];
                }
            }
        }
    }
    // The arrows' colour byte keeps what the last one used; before the first it holds what the
    // terrain blit (0x483FA0) left at that stack word: the view's top row moved down to the first
    // whole terrain tile row (32 - scroll y mod 32 when the scroll is not on a tile edge).
    const int tile_off = camera.scroll_y - ((camera.scroll_y + ((camera.scroll_y >> 31) & 31)) >> 5 << 5);
    std::uint8_t arrow = static_cast<std::uint8_t>(tile_off ? 32 + (32 - tile_off) : 32);
    static const signed char kDx[8] = {0, -1, -1, -1, 0, 1, 1, 1}, kDz[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    static const int kGradeColour[3] = {4, 14, 10};
    sim::Lcg15 rng;
    rng.set_state(rand_state);
    const int sx0 = camera.scroll_x, sy0 = camera.scroll_y;
    const int x0 = sixteenth(sx0);
    int z = sixteenth(sy0);
    const int x1 = std::min((opt_.width - 128) / 16 + 1 + x0, map.width - 1);
    const int last_row = map.height - 1;
    auto cell = [&](int x, int zz) -> const sim::MapCell& {
        return map.cells[static_cast<std::size_t>(zz) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)];
    };
    const std::int32_t I = opt_.contour_interval;
    const sim::RouteSearch& rs = b_.route_search;
    const sim::PlayerSight* sight = nullptr;
    {
        const int v = viewed_player();
        if (v >= 0 && v < static_cast<int>(b_.sight.players.size())) sight = &b_.sight.players[static_cast<std::size_t>(v)];
    }
    if (z < last_row && x0 >= 0 && z >= 0) {
        bool more;
        do {
            more = false;
            for (int x = x0; x < x1; ++x) {
                const sim::MapCell& c = cell(x, z);
                const i32 bx = (x + 8) * 16, by = (z + 2) * 16;
                const i32 ht[4] = {cell(x, z).height, cell(x + 1, z).height, cell(x + 1, z + 1).height, cell(x, z + 1).height};
                const i32 px[4] = {bx - sx0, bx + 16 - sx0, bx + 16 - sx0, bx - sx0};
                const i32 py[4] = {by - (ht[0] >> 1) - sy0, by - (ht[1] >> 1) - sy0, by + 16 - (ht[2] >> 1) - sy0, by + 16 - (ht[3] >> 1) - sy0};
                if (py[0] < opt_.height) more = true;
                if (mode == 1) {
                    if (cls && !cls->grid.empty() && cls->width > 0) {
                        const std::size_t at = static_cast<std::size_t>((z >> 4) * cls->width + x);
                        const u32 g = at < cls->grid.size() ? (cls->grid[at] >> ((z & 15) * 2)) & 3u : 3u;
                        if (g < 3) {
                            const std::uint8_t col = c_.hud[static_cast<std::size_t>(kGradeColour[g])];
                            line(px[0], py[0], px[2], py[2], col);
                            line(px[1], py[1], px[3], py[3], col);
                        }
                    }
                    const std::size_t ri = static_cast<std::size_t>(z) * static_cast<std::size_t>(rs.width) + static_cast<std::size_t>(x);
                    const std::uint8_t fl = rs.width > 0 && ri < rs.cell_flags.size() ? rs.cell_flags[ri] : 0;
                    const std::uint8_t dir = rs.width > 0 && ri < rs.cell_dir.size() ? rs.cell_dir[ri] : 0;
                    if (fl & 4) {
                        const int col = static_cast<int>(rng.next() & 0xFF);
                        text(c_.small_font, "G", px[0], py[0], col);
                    }
                    if ((fl & 0xFB) != 0 && (fl & 0xFB) != 3) {
                        if (fl == 1) arrow = c_.hud[15];
                        else if (fl == 2) arrow = c_.hud[4];
                        const int cx = px[0] + 8, cy = py[0] + 8;
                        const int d = dir & 7;
                        line(cx + kDx[d] * -14, cy + kDz[d] * -14, cx, cy, arrow);
                        const int d1 = (dir + 1) & 7, d2 = (dir - 1) & 7;
                        line(cx + kDx[d1] * -4, cy + kDz[d1] * -4, cx, cy, arrow);
                        line(cx + kDx[d2] * -4, cy + kDz[d2] * -4, cx, cy, arrow);
                    }
                } else if (mode == 2 || mode == 3) {
                    const std::uint8_t col = b_.settings.sea_level < c.height ? c_.hud[15] : c_.hud[13];
                    line(px[0], py[0], px[1], py[1], col);
                    line(px[0], py[0], px[3], py[3], col);
                    if (mode == 2) {
                        if (c.unit != 0 || c.feature != 0xFFFF) {
                            const std::uint8_t fc = static_cast<std::uint8_t>(c.unit != 0 ? c.unit & 0xFF : (c.feature - 0x38) & 0xFF);
                            const render::RasterVertex v[4] = {{px[0], py[0], 0}, {px[1], py[1], 0}, {px[2], py[2], 0}, {px[3], py[3], 0}};
                            for (render::Image8* o : {&a, &bb}) {
                                render::RasterTarget t;
                                t.pixels = o->pixels.data();
                                t.pitch = o->width;
                                t.clip = clip;
                                render::fill_polygon(t, v, 4, fc);
                            }
                        }
                        if (c.unit2 != 0) {
                            const std::uint8_t uc = static_cast<std::uint8_t>(c.unit2 & 0xFF);
                            line(px[0], py[0], px[2], py[2], uc);
                            line(px[1], py[1], px[3], py[3], uc);
                        }
                        if (c.flags & 2) {
                            const std::uint8_t dc = c_.hud[15];
                            line((px[0] + px[1]) / 2, (py[0] + py[1]) / 2 + 2, (px[2] + px[1]) / 2 - 2, (py[1] + py[2]) / 2, dc);
                            line((px[1] + px[2]) / 2 - 2, (py[1] + py[2]) / 2, (px[3] + px[2]) / 2, (py[2] + py[3]) / 2 - 2, dc);
                            line((px[2] + px[3]) / 2, (py[2] + py[3]) / 2 - 2, (px[0] + px[3]) / 2 + 2, (py[3] + py[0]) / 2, dc);
                            line((px[3] + px[0]) / 2 + 2, (py[3] + py[0]) / 2, (px[0] + px[1]) / 2, (py[0] + py[1]) / 2 + 2, dc);
                        }
                    } else {
                        text(c_.small_font, std::to_string(static_cast<int>(c.metal)), px[0] + 2, py[0] + 2, c_.hud[15]);
                    }
                } else if (mode == 4) {
                    line(px[0], py[0], px[1], py[1], c_.hud[0]);
                    line(px[0], py[0], px[3], py[3], c_.hud[0]);
                    if (sight && sight->width > 0) {
                        const std::size_t si = static_cast<std::size_t>(sight->width) * static_cast<std::size_t>(z / 2) + static_cast<std::size_t>(x / 2);
                        if (si < sight->counts.size() && sight->counts[si] != 0)
                            for (render::Image8* o : {&a, &bb}) fill_rect(*o, clip, px[0] - 5, py[0] - 5, px[0] + 5, py[0] + 5, c_.hud[15]);
                    }
                }
                if (I > 0) contour_cell(x, z, px, py, ht, line);
            }
        } while (more && ++z < last_row);
    }
    rand_state = rng.state();
    out.blit(out.keep(keyed_layer(a, bb)), 0, 0, clip, false);
}

void GameClient::draw_pointer_cross(render::DrawList& out) const {
    // Mode 2, right after the terrain marks: a cross of 5 x 5 pixels in colour 15 at the pointer's
    // map point (GS+0x2CAA: x, z less half the height), clipped to the view.
    const sim::Vec3i p = cursor_world();
    const int x = static_cast<std::int16_t>(static_cast<u32>(p[0]) >> 16) - scroll_x_;
    const int y = static_cast<std::int16_t>(static_cast<u32>(p[2]) >> 16) - (static_cast<std::int16_t>(static_cast<u32>(p[1]) >> 16) >> 1) - scroll_y_;
    const render::ClipRect clip = view_rect();
    out.line(x + 0x7E, y + 0x20, x + 0x82, y + 0x20, c_.hud[15], clip);
    out.line(x + 0x80, y + 0x1E, x + 0x80, y + 0x22, c_.hud[15], clip);
}

// ---- the selected unit's route (0x417F30 -> 0x417E00) --------------------------------------

void GameClient::draw_route_debug(render::DrawList& out) const {
    // With the view and INFO on (after the health bars, before the fog): the first selected unit
    // of the local player (0x48C190), when it moves with a ground route goal (the only motion goal
    // with a drawing of its own; the others' slot does nothing): its footprint box (0x46B9D0: from
    // its cell at (16 (x + 8) - scroll x, 16 z - scroll y - the cell's height / 2 + 32), the
    // footprint's size in cells, colour 15) and its route's legs between the waypoints at the
    // ground's height (0x485070), colour 9 with a route, else 12. Clipped to the view.
    const int p = opt_.player;
    if (p < 0 || p >= static_cast<int>(b_.players.size())) return;
    const sim::PlayerSlot& ps = b_.players[static_cast<std::size_t>(p)];
    auto selected = [&](int i) {
        return i > 0 && static_cast<std::size_t>(i) < b_.units.size() && (b_.units[static_cast<std::size_t>(i)].flags & sim::unit_tick_flags::kSelected);
    };
    int found = 0;
    for (int i = ps.first_unit + 1; i <= ps.last_unit && !found; ++i)
        if (selected(i)) found = i;
    if (!found && selected(ps.first_unit)) found = ps.first_unit;
    if (!found) return;
    const sim::Unit& u = b_.units[static_cast<std::size_t>(found)];
    const sim::UnitTickState& t = b_.ticks[static_cast<std::size_t>(found)];
    if (!t.has_motion || !t.motion.goal) return;
    const auto* g = dynamic_cast<const sim::RouteGoal*>(t.motion.goal.get());
    if (!g) return;
    const render::ClipRect clip = view_rect();
    const sim::MapGrid& map = b_.map;
    auto cell_height = [&](int x, int z) -> int {
        if (x < 0 || z < 0 || x >= map.width || z >= map.height) return 0;
        return map.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)].height;
    };
    {
        const int x0 = (u.cell_x + 8) * 16 - scroll_x_;
        const int y0 = (u.cell_z * 16 - scroll_y_) - (cell_height(u.cell_x, u.cell_z) >> 1) + 0x20;
        const int x1 = u.footprint_x * 16 + x0, y1 = u.footprint_z * 16 + y0;
        render::Image8 a(opt_.width, opt_.height, 0), bb(opt_.width, opt_.height, 255);
        for (render::Image8* o : {&a, &bb}) outline_rect(*o, clip, x0, y0, x1, y1, c_.hud[15]);
        out.blit(out.keep(keyed_layer(a, bb)), 0, 0, clip, false);
    }
    // 0x485070: the ground's height at a world point (bilinear over the cell; -1 off the map).
    auto ground = [&](i32 wx, i32 wz) -> int {
        const int ix = wx >> 4, iz = wz >> 4, fx = wx & 15, fz = wz & 15;
        if (ix < 0 || ix + 1 >= map.width || iz < 0 || iz + 1 >= map.height) return -1;
        const int h00 = cell_height(ix, iz), h10 = cell_height(ix + 1, iz), h01 = cell_height(ix, iz + 1), h11 = cell_height(ix + 1, iz + 1);
        auto div16 = [](int v) { return (v + ((v >> 31) & 15)) >> 4; };
        const int top = div16((h10 - h00) * fx) + h00;
        const int bottom = div16((h11 - h01) * fx) + h01;
        return div16((bottom - top) * fz) + top;
    };
    const std::uint8_t col = c_.hud[(g->flags & 1) ? 9 : 12];
    for (int k = 0; k + 1 < g->count && k + 1 < static_cast<int>(g->points.size()); ++k) {
        const i32 ax = static_cast<std::int16_t>(g->points[static_cast<std::size_t>(k)] & 0xFFFF);
        const i32 az = static_cast<std::int16_t>(g->points[static_cast<std::size_t>(k)] >> 16);
        const i32 bx = static_cast<std::int16_t>(g->points[static_cast<std::size_t>(k + 1)] & 0xFFFF);
        const i32 bz = static_cast<std::int16_t>(g->points[static_cast<std::size_t>(k + 1)] >> 16);
        const int sxa = ax - scroll_x_ + 0x80, sya = az - scroll_y_ - (ground(ax, az) >> 1) + 0x20;
        const int sxb = bx - scroll_x_ + 0x80, syb = bz - scroll_y_ - (ground(bx, bz) >> 1) + 0x20;
        out.line(sxa, sya, sxb, syb, col, clip);
    }
}

// ---- the status line and +NetStats ---------------------------------------------------------

std::string GameClient::net_stats_text() {
    // 0x415FA0: once more than 30 ticks of the 1/30 s clock passed since the last update, the
    // rates are made anew from the totals (unsigned: (total x 30 - total then x 30) / ticks):
    // pS / pR the packets' bytes sent / received, S the messages sent and their bytes before
    // compression, R the messages received and their bytes, C the share of the packets' bytes
    // the compression saved (whole percent, signed; 0 without packets sent).
    NetStatsCounters k = net_stats_counters ? net_stats_counters() : NetStatsCounters{};
    k.packet_bytes_received -= netstats_base_.packet_bytes_received;
    k.packet_bytes_sent -= netstats_base_.packet_bytes_sent;
    k.frames_sent -= netstats_base_.frames_sent;
    k.frame_bytes_sent -= netstats_base_.frame_bytes_sent;
    k.frames_received -= netstats_base_.frames_received;
    k.frame_bytes_received -= netstats_base_.frame_bytes_received;
    NetStatsLine& s = netstats_;
    const u32 now = sim::StepBudget::scale_ms(last_now_ms_);
    const u32 dt = now - s.tick;
    if (dt > 30) {
        auto rate = [&](u32 total, u32 then) { return (total * 30u - then * 30u) / dt; };
        s.pr = rate(k.packet_bytes_received, s.prev.packet_bytes_received);
        const u32 d = k.packet_bytes_sent - s.prev.packet_bytes_sent;
        s.ps = (d * 30u) / dt;
        if (static_cast<i32>(d) < 1) s.compression = 0;
        else
            s.compression = static_cast<i32>((s.wire_prev + d) - k.wire_bytes_sent) * 100 / static_cast<i32>(d);
        s.wire_prev = k.wire_bytes_sent;
        s.s_bytes = rate(k.frame_bytes_sent, s.prev.frame_bytes_sent);
        s.r_bytes = rate(k.frame_bytes_received, s.prev.frame_bytes_received);
        s.s_count = rate(k.frames_sent, s.prev.frames_sent);
        s.r_count = rate(k.frames_received, s.prev.frames_received);
        s.prev = k;
        s.tick = now;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "pS=%4d pR=%4d (S=%d/%4d, R=%d/%4d) C=%3d%%\n", static_cast<int>(s.ps), static_cast<int>(s.pr),
                  static_cast<int>(s.s_count), static_cast<int>(s.s_bytes), static_cast<int>(s.r_count), static_cast<int>(s.r_bytes),
                  static_cast<int>(s.compression));
    return buf;
}

void GameClient::draw_debug_line(render::Image8& o) {
    // With the view on (Render_DrawBattleFrame, after the console): in the console font (COMIX) and
    // interface colour 15, at y = 3 x the font's height - 10: "FRATE: %d" at x 131 (the display's
    // frame counter, 0x4B66A0: the wall-clock milliseconds since its last call are added up and
    // the frames counted; past 2000 the sum restarts at 1000; past 1000 the count is shown and
    // the sum loses 1000), "[Release]" at 188 and "MODE DEBUG INFO ON|OFF" at 494; in network
    // games the +NetStats numbers (0x415FA0) one font height lower at 188. A line is drawn only
    // when its whole box lies inside the frame.
    if (!debug_.on || no_console_) return;
    const Font* f = c_.console_font;
    if (!f) return;
    DebugViewState& d = debug_;
    d.frate_acc += static_cast<i32>(last_now_ms_ - d.frate_last);
    d.frate_last = last_now_ms_;
    ++d.frate_count;
    if (d.frate_acc > 2000) d.frate_acc = 1000;
    if (d.frate_acc > 1000) {
        d.frate_shown = d.frate_count;
        d.frate_acc -= 1000;
        d.frate_count = 0;
    }
    const int y = (f->height & 0xFF) * 3 - 10;
    const std::uint8_t col = c_.hud[15];
    char buf[64];
    std::snprintf(buf, sizeof buf, "FRATE: %d\n", static_cast<int>(d.frate_shown));
    const render::ClipRect all = o.bounds();
    clip_text(o, all, *f, buf, 0x83, y, col);
    clip_text(o, all, *f, "[Release]", 0xBC, y, col);
    std::snprintf(buf, sizeof buf, "MODE %s INFO %s", "DEBUG", d.info ? "ON" : "OFF");
    clip_text(o, all, *f, buf, 0x1EE, y, col);
    if (network.remote_lag) clip_text(o, all, *f, net_stats_text(), 0xBC, y + (f->height & 0xFF), col);
}

// ---- the bottom bar's numbers (0x46A860 with INFO on) ------------------------------------------

bool GameClient::draw_info_bar(render::Image8& o) {
    // With the view and INFO on the bottom bar (its art as always) shows, in the console font and
    // colour 0x53, on the row a font height and one above the screen's bottom: "PFSTATE %d,
    // PFABLE %d" at x 130 (page flipping on, possible), "MOVEORD: %d FIREORD: %d" at 264 with a
    // unit under the pointer (its flag bits 18-19 and 20-21), "DELTATIME: %d" at 400 (the steps the
    // last pass's budget gave), "GAMETIME: %d" at 520 (the game tick); 16 rows higher: "X: %d  Y: %d"
    // (the scroll), "UNITS %d\%d" (the last unit pass's units, the units drawn), "PACKETS: %d %d %d"
    // (packets received from players 1, 2, 3) and "XYH: %d %d %d" (the pointer's cell and its
    // height).
    if (!(debug_.on && debug_.info)) return false;
    const Font* f = c_.console_font;
    if (!f) return true;
    const int fh = f->height & 0xFF;
    int y = opt_.height - fh - 1;
    auto put = [&](const std::string& s, int x) { clip_text(o, o.bounds(), *f, s, x, y, 0x53); };
    char buf[96];
    std::snprintf(buf, sizeof buf, "PFSTATE %d, PFABLE %d\n", debug_.page_flip ? 1 : 0, pfable_ ? 1 : 0);
    put(buf, 0x82);
    if (hover_ != 0 && hover_ < b_.units.size()) {
        const u32 fl = b_.units[hover_].flags;
        std::snprintf(buf, sizeof buf, "MOVEORD: %d FIREORD: %d\n", static_cast<int>((fl >> 0x12) & 3), static_cast<int>((fl >> 0x14) & 3));
        put(buf, 0x108);
    }
    std::snprintf(buf, sizeof buf, "DELTATIME: %d\n", budget_steps_);
    put(buf, 400);
    std::snprintf(buf, sizeof buf, "GAMETIME: %d\n", static_cast<int>(b_.game_tick()));
    put(buf, 0x208);
    y -= 0x10;
    std::snprintf(buf, sizeof buf, "X: %d  Y: %d\n", scroll_x_, scroll_y_);
    put(buf, 0x82);
    std::snprintf(buf, sizeof buf, "UNITS %d\\%d\n", static_cast<int>(b_.live_units), static_cast<int>(visible_units_));
    put(buf, 0x108);
    auto packets = [&](int s) { return static_cast<int>(packets_from ? packets_from(s) : 0u); };
    std::snprintf(buf, sizeof buf, "PACKETS: %d %d %d\n", packets(1), packets(2), packets(3));
    put(buf, 400);
    const sim::Vec3i p = cursor_world();
    const int cx = static_cast<std::int16_t>(static_cast<u32>(p[0]) >> 20), cz = static_cast<std::int16_t>(static_cast<u32>(p[2]) >> 20);
    int ch = 0;
    const std::size_t ci = static_cast<std::size_t>(static_cast<std::int64_t>(cz) * b_.map.width + cx);
    if (ci < b_.map.cells.size()) ch = b_.map.cells[ci].height;
    std::snprintf(buf, sizeof buf, "XYH: %d %d %d\n", cx, cz, ch);
    put(buf, 0x208);
    return true;
}

}  // namespace client
