// The client's frame: the battle view, then the interface art, the minimap and an
// interface layer (bars, text, marks), then the pointer
// (spec: docs/RE_MEMORY.md, "Client").

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "client/canvas.h"
#include "client/game_client.h"
#include "render/battle_view.h"
#include "render/raster.h"
#include "sim/economy.h"
#include "sim/orders.h"
#include "sim/weapon_slot.h"
#include "sim/x87_trig.h"
#include "sim/sight.h"
#include "sim/victory.h"
#include "sim/unit_tick.h"

namespace client {
namespace {

constexpr std::uint32_t kSelected = sim::unit_tick_flags::kSelected;

// A pixel of a layout dimmed under a dialog (0x4BF4D0 with -24: shade level 8). The routine
// indexes the level's row with the pixel as a signed byte, so pixels from 0x80 up take level
// 7's entry (row 8 less 256).
std::uint8_t dimmed(const std::vector<std::uint8_t>& shade, std::uint8_t p) {
    return shade[static_cast<std::size_t>(8 * 256 + static_cast<std::int8_t>(p))];
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}
// A fixed-point number as the game's C runtime prints it ("%.Nf"): the exact value rounded
// half away from zero (a tie such as 2261393.25 gives .3; today's runtimes round ties to
// even), a minus sign for negative values (also when they round to zero).
std::string crt_fixed(double v, int decimals) {
    if (v != v) return fmt("%f", v);
    const bool neg = std::signbit(v) && v != 0.0;
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.60f", std::fabs(v));  // exact digits of the value
    std::string s = buf;
    const std::size_t dot = s.find('.');
    std::string digits = s.substr(0, dot) + s.substr(dot + 1, static_cast<std::size_t>(decimals));
    const bool up = dot + 1 + static_cast<std::size_t>(decimals) < s.size() && s[dot + 1 + static_cast<std::size_t>(decimals)] >= '5';
    if (up) {
        int k = static_cast<int>(digits.size()) - 1;
        while (k >= 0 && digits[static_cast<std::size_t>(k)] == '9') digits[static_cast<std::size_t>(k--)] = '0';
        if (k < 0) digits.insert(digits.begin(), '1');
        else ++digits[static_cast<std::size_t>(k)];
    }
    const std::size_t int_len = digits.size() - static_cast<std::size_t>(decimals);
    std::string out = neg ? "-" : "";
    out += digits.substr(0, int_len);
    if (decimals > 0) out += "." + digits.substr(int_len);
    return out;
}
// The C runtime's float -> int conversion (truncation; out of range or NaN gives the integer
// indefinite value).
long long ftol(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return static_cast<long long>(static_cast<std::int32_t>(0x80000000u));
    return static_cast<long long>(static_cast<std::int32_t>(v));
}
long long magnitude32(long long v) {
    // |v| as the 32-bit negate does it (the integer indefinite value stays as it is).
    const std::uint32_t u = static_cast<std::uint32_t>(v);
    return static_cast<std::int32_t>(static_cast<std::int32_t>(u) < 0 ? 0u - u : u);
}
std::string fmti(const char* f, long long v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// The bars' text routine: a line is drawn only when its whole box (x .. x + width, y .. y +
// the font's height) lies inside the frame; otherwise nothing of it.
void bar_text(render::Image8& o, const Font& f, const std::string& s, int x, int y, int colour) {
    const int w = f.text_width(s);
    if (x < 0 || y < 0 || x + w > o.width - 1 || y + f.height > o.height - 1) return;
    draw_text(o, o.bounds(), f, s, x, y, colour);
}

// A player's colour logo stretched into a bar's logo box (the box's corners, rows moved by dy):
// the frame's corners (0, 0), (w, 0), (w, h), (0, h) mapped onto the box's, the texels copied
// (the key too), clipped to the frame.
void draw_logo(render::Image8& o, const std::vector<render::Sprite8>* logos, int colour, const Region& r, int dy) {
    if (!logos || logos->empty()) return;
    const render::Sprite8& s = (*logos)[static_cast<std::size_t>(colour) % logos->size()];
    render::RasterTarget t;
    t.pixels = o.pixels.data();
    t.pitch = o.width;
    t.clip = o.bounds();
    const render::RasterVertex v[4] = {{r.x1, r.y1 + dy, 0}, {r.x2, r.y1 + dy, 0}, {r.x2, r.y2 + dy, 0}, {r.x1, r.y2 + dy, 0}};
    const int uv[4][2] = {{0, 0}, {s.width, 0}, {s.width, s.height}, {0, s.height}};
    render::textured_quad_uv(t, v, uv, s);
}

} // namespace

void GameClient::prepare_drawing() {
    prepared_ = true;
    // The minimap: the map's scroll extent fitted into the 126 x 126 square, its picture and
    // the viewed player's fog picture (as the battle's loading and its first sight pass make
    // them), and the marks.
    const MinimapBox box = minimap_box(scroll_width(), scroll_height());
    mm_x_ = box.x;
    mm_y_ = box.y;
    mm_w_ = box.w;
    mm_h_ = box.h;
    minimap_build_picture();
    minimap_composite();
    minimap_redraw();
    // The interface layer's transparent index: one no interface drawing uses.
    std::set<int> used;
    for (int k : {0, 4, 10, 12, 14, 15}) used.insert(c_.hud[static_cast<std::size_t>(k)]);
    used.insert(0x53);
    for (const SideLayout& s : c_.sides) {
        used.insert(s.energy_colour);
        used.insert(s.metal_colour);
    }
    auto add_frames = [&](const std::vector<render::Sprite8>* v) {
        if (v)
            for (const auto& f : *v)
                for (std::uint8_t p : f.pixels) used.insert(p);
    };
    // (the chat line: its picture, the glyphs, the insertion point's colour; the panel's captions
    // dimmed under it)
    used.insert(c_.hud[9]);
    if (c_.chat.picture)
        for (std::uint8_t p : c_.chat.picture->pixels)
            if (!c_.chat.picture->has_key || p != c_.chat.picture->key) used.insert(p);
    if (c_.chat.font)
        for (const Glyph& g : c_.chat.font->glyphs)
            for (std::uint8_t p : g.colours) {
                used.insert(p);
                if (c_.light_levels.size() >= 32 * 256) used.insert(c_.light_levels[30 * 256 + p]);
                if (c_.shade_levels.size() >= 32 * 256) used.insert(dimmed(c_.shade_levels, p));
            }
    add_frames(c_.radar_dots);
    add_frames(c_.team_logos);
    if (c_.radar_dot_hover)
        for (std::uint8_t p : c_.radar_dot_hover->pixels) used.insert(p);
    for (int k = 255; k >= 0; --k)
        if (!used.count(k)) {
            overlay_key_ = static_cast<std::uint8_t>(k);
            break;
        }
}

render::DrawList GameClient::draw() {
    if (!prepared_) prepare_drawing();
    update_top_bar();
    const int w = opt_.width, h = opt_.height;
    render::BattleScene scene;
    scene.camera.screen_width = w;
    scene.camera.screen_height = h;
    scene.camera.view_width = w - 128;
    scene.camera.view_height = h - 64;
    scene.camera.scroll_x = scroll_x_;
    scene.camera.scroll_y = scroll_y_;
    if (c_.fill_scene) c_.fill_scene(scene, viewed_player(), frame_counter_);
    if (!opt_.fog) scene.fog = nullptr;
    scene.dithered_fog = opt_.dithered_fog;
    scene.options.shading = opt_.shading;
    scene.options.antialias = opt_.antialias;
    scene.options.shadows = opt_.shadows;
    scene.options.unit_shadows = opt_.unit_shadows;
    scene.options.feature_shadows = opt_.feature_shadows;
    add_selection_outlines(scene);
    // The top bar's art is drawn right after the terrain (its pieces may reach into the view's
    // first rows; units, shots and effects draw over them there): its own piece at x 129, then
    // (wider screens) the bottom bar's piece repeated.
    scene.after_terrain = [this, w, h](render::DrawList& out) {
        if (!side_) return;
        const render::ClipRect all{0, 0, w - 1, bar_redrawn_ ? h - 1 : 31};
        for (int x = 129; side_->panel_top && x < w;) {
            const render::Sprite8* s = x == 129 || !side_->panel_bottom ? side_->panel_top : side_->panel_bottom;
            out.blit(s, x + s->x_offset, s->y_offset, all, true);
            x += std::max(1, s->width);
        }
    };
    scene.before_fog = [this, &scene, w, h](render::DrawList& out) {
        draw_queued_orders(out, scene.camera);
        if (opt_.damage_bars) {
            render::Image8 bars(w, h, overlay_key_);
            draw_unit_bars(bars);
            render::Sprite8 sp;
            sp.width = w;
            sp.height = h;
            sp.key = overlay_key_;
            sp.has_key = true;
            sp.pixels = std::move(bars.pixels);
            out.blit(out.keep(std::move(sp)), 0, 0, scene.camera.view_clip(), false);
        }
    };
    // The units drawn this frame (picking, double clicks, notices).
    on_screen_.clear();
    for (const render::UnitInstance& ui : scene.units) {
        if (!ui.unit) continue;
        const int sx = render::screen_x(scene.camera, ui.unit->pos[0]);
        const int sy = render::screen_y(scene.camera, ui.unit->pos[1], ui.unit->pos[2]);
        if (sx > -64 && sx < w + 64 && sy > -64 && sy < h + 64) on_screen_.push_back({ui.unit->id, sx, sy});
    }
    std::vector<const sim::Unit*> drawn;
    scene.drawn_units = &drawn;
    std::uint32_t rand_after = scene.crt_rand_state;
    scene.crt_rand_after = &rand_after;
    render::DrawList list = render::build_draw_list(scene);
    // Lightning shots on screen draw the C runtime's rand() while the original draws them
    // (Gfx_DrawProjectiles 0x49BE60, three draws a segment): the simulation's sequence moves on
    // by those draws, as the original's does.
    if (c_.fill_scene && rand_after != scene.crt_rand_state) b_.crt_rand.set_state(rand_after);
    {
        // The original's unit drawing (0x45AC20) brings the drawn units' poses up to date in
        // the simulation's state (debris and shards start from them): the battle's own units
        // among those drawn (feature stand-ins are not battle units).
        std::vector<std::uint16_t> ids;
        if (!b_.units.empty()) {
            const sim::Unit* lo = b_.units.data();
            const sim::Unit* hi = lo + b_.units.size();
            for (const sim::Unit* u : drawn)
                if (u >= lo && u < hi) ids.push_back(static_cast<std::uint16_t>(u - lo));
        }
        b_.units_drawn(ids);
    }
    const render::ClipRect screen{0, 0, w - 1, h - 1};
    // The view's marks (the drag box, the placement outline), then the Space-held panels over
    // the view (the frame draws them after the fog and the marks, before the console).
    {
        render::Image8 marks(w, h, overlay_key_);
        draw_unit_marks(marks);
        render::Sprite8 sp;
        sp.width = w;
        sp.height = h;
        sp.key = overlay_key_;
        sp.has_key = true;
        sp.pixels = std::move(marks.pixels);
        list.blit(list.keep(std::move(sp)), 0, 0, screen, false);
    }
    draw_panels(list);

    // Interface art: the side panel, the top and bottom bars (repeated across wider screens).
    if (side_) {
        if (side_->panel_side) list.blit(side_->panel_side, side_->panel_side->x_offset, side_->panel_side->y_offset, screen, true);
        // (The top bar's art: drawn after the terrain, see scene.after_terrain.)
        for (int x = 129; side_->panel_bottom && x < w; x += std::max(1, side_->panel_bottom->width))
            list.blit(side_->panel_bottom, x + side_->panel_bottom->x_offset, h - 32 + side_->panel_bottom->y_offset, screen, true);
    }
    // The panel and its buttons. While the chat line is open the panel's box is shown dimmed:
    // the chat line opens with flag 0x800, which puts the layout under it (the panel, with what
    // lay under it when it was drawn) through shade level 8 once; closing redraws it.
    // The panel is composed in its box (the side art under it, its picture, the buttons — the
    // disabled ones dimmed through the grey table and shade row 12 as the original draws them),
    // and dimmed as a whole while the chat line is open.
    if (const Panel* p = current_panel()) {
        const int pw = p->w > 0 ? p->w : 128, ph = p->h > 0 ? p->h : h - p->y;
        render::Image8 img(pw, ph, list.clear_index);
        // Pressed: only the button the pointer holds (while over it), the tab of the menu shown
        // and the active order mode's button (see button_pressed_look).
        compose_panel(
            img, p->x, p->y, *p, side_ ? side_->panel_side : nullptr,
            [&](const Button& btn) { return button_frame(btn, button_pressed_look(btn), !button_enabled(btn), button_stage(btn)); },
            [&](const Button& btn) { return button_dims(btn, !button_enabled(btn)); }, c_.minimap_grey, c_.shade_levels);
        if (chat_ && c_.shade_levels.size() >= 32 * 256)
            for (std::uint8_t& px : img.pixels) px = dimmed(c_.shade_levels, px);
        render::Sprite8 sp;
        sp.width = pw;
        sp.height = ph;
        sp.has_key = false;
        sp.pixels = std::move(img.pixels);
        list.blit(list.keep(std::move(sp)), p->x, p->y, screen, false);
    }
    // The minimap (without the battle's steps drawing it, drawn anew for every frame).
    if (!mm_hooked_) {
        minimap_composite();
        minimap_redraw();
    }
    if (mm_final_.width > 0 && mm_final_.height > 0) {
        render::Sprite8 mm;
        mm.width = mm_final_.width;
        mm.height = mm_final_.height;
        mm.has_key = false;
        mm.pixels = mm_final_.pixels;
        // A sprite of this frame only: the GPU backend must upload it anew (marked as lasting,
        // it kept the texture of an earlier frame's minimap whose memory the new one reused,
        // so old marks showed in turns — the flickering circles).
        list.blit(list.keep(std::move(mm)), mm_x_, mm_y_, screen, false);
    }

    // The interface layer.
    render::Sprite8 layer;
    {
        render::Image8 o(w, h, overlay_key_);
        std::vector<std::uint8_t> unused;
        draw_overlay(o, unused);
        layer.width = w;
        layer.height = h;
        layer.key = overlay_key_;
        layer.has_key = true;
        layer.pixels = std::move(o.pixels);
    }
    list.blit(list.keep(std::move(layer)), 0, 0, screen, false);

    // The pointer: the look set by the last pass at the frame its animation reached (frame
    // times from the art, counted down by the wall clock; see advance_pointer).
    if (!cursor_set_) set_pointer(cursor_kind());
    const auto* look = c_.cursors.looks[static_cast<std::size_t>(cursor_shown_)];
    int frame = cursor_clock_.frame;
    if (!look || look->empty()) {
        look = c_.cursors.looks[static_cast<std::size_t>(CursorKind::Normal)];
        frame = 0;
    }
    if (look && !look->empty() && !pointer_hidden_ && !pointer_off_ && !cursor_clock_.ended) {
        const render::Sprite8& s = (*look)[static_cast<std::size_t>(frame) % look->size()];
        list.blit(&s, mouse_x_, mouse_y_, screen, true);
    }
    return list;
}

void GameClient::add_selection_outlines(render::BattleScene& scene) const {
    // Every selected unit (flag 0x10, whoever owns it) of the view gets the outline of its
    // model's box (the root piece's bounds, see the importer) at the box's lowest y: the four
    // corners turned by the unit's three angles, placed like the pick test's corners, joined in
    // interface colour 10. The original draws it in the unit's turn, just before the unit.
    if (!opt_.selection_boxes) return;
    for (render::UnitInstance& ui : scene.units) {
        if (!ui.unit) continue;
        const sim::Unit& u = *ui.unit;
        if (!(u.flags & kSelected) || u.id >= b_.ticks.size()) continue;
        const std::uint16_t t = b_.ticks[u.id].type_index;
        if (t == 0 || t >= c_.types.size()) continue;
        const UnitTypeLook& look = c_.types[t];
        const auto& mn = look.box_min;
        const auto& mx = look.box_max;
        const std::int32_t corners[4][3] = {{mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mx[0], mn[1], mx[2]}, {mn[0], mn[1], mx[2]}};
        const std::int16_t angles[3] = {u.pitch, static_cast<std::int16_t>(u.heading), u.bank};
        const std::uint32_t ox = static_cast<std::uint32_t>(u.pos[0]) - static_cast<std::uint32_t>(scene.camera.scroll_x) * 0x10000u;
        const std::uint32_t oz = static_cast<std::uint32_t>(u.pos[2]) - static_cast<std::uint32_t>(scene.camera.scroll_y) * 0x10000u;
        for (int k = 0; k < 4; ++k) {
            std::int32_t r[3];
            sim::x87::rotate_point_3d(corners[k], r, angles);
            ui.outline_xy[static_cast<std::size_t>(2 * k)] = static_cast<std::int16_t>((static_cast<std::uint32_t>(r[0]) + ox) >> 16) + 128;
            ui.outline_xy[static_cast<std::size_t>(2 * k + 1)] =
                static_cast<std::int16_t>((oz - static_cast<std::uint32_t>(r[2])) >> 16) -
                (static_cast<std::int16_t>((static_cast<std::uint32_t>(r[1]) + static_cast<std::uint32_t>(u.pos[1])) >> 16) >> 1) + 32;
        }
        ui.outline = true;
        ui.outline_colour = c_.hud[10];
    }
}

void GameClient::draw_overlay(render::Image8& o, std::vector<std::uint8_t>&) {
    // (the view's marks: their own layer, before the Space-held panels)
    draw_console(o);
    draw_top_bar(o);
    draw_bottom_bar(o);
    draw_minimap(o);
    // Build buttons' captions: the focus unit's queued count of the type ("+n", none at 0),
    // laid out as the button's description says, in its font and colour (black while pressed).
    if (const Panel* p = current_panel())
        if (focus_)
            for (const Button& btn : p->buttons) {
                if (btn.action != Action::BuildUnit || !btn.unit_type || !btn.font) continue;
                const std::int32_t n = build_count(b_.units[focus_], btn.unit_type);
                if (n == 0) continue;
                draw_caption(o, btn, "+" + std::to_string(n), button_pressed_look(btn) ? 1 : 0);
            }
    // (the captions belong to the panel: dimmed with it while the chat line is open)
    if (const Panel* p = current_panel(); p && chat_ && c_.shade_levels.size() >= 32 * 256) {
        const int pw = p->w > 0 ? p->w : 128, ph = p->h > 0 ? p->h : opt_.height - p->y;
        for (int y = std::max(p->y, 0); y < std::min(p->y + ph, o.height); ++y)
            for (int x = std::max(p->x, 0); x < std::min(p->x + pw, o.width); ++x) {
                std::uint8_t& px = o.row(y)[x];
                if (px != overlay_key_) px = dimmed(c_.shade_levels, px);
            }
    }
    // Paused sign; the victory and defeat signs once the battle is decided (end flags).
    const int cx = (opt_.width + 128) / 2, cy = opt_.height / 2;
    if (budget_.paused && c_.paused_sign) render::blit(o, o.bounds(), *c_.paused_sign, cx, cy);
    if ((b_.end_game.flags & sim::end_game_flags::kWonMovie) && c_.victory_sign) render::blit(o, o.bounds(), *c_.victory_sign, cx, cy);
    if ((b_.end_game.flags & sim::end_game_flags::kLost) && c_.defeat_sign) render::blit(o, o.bounds(), *c_.defeat_sign, cx, cy);
    draw_clock_and_units(o);
    draw_chat(o);
}

void GameClient::draw_clock_and_units(render::Image8& o) {
    // The game clock (option): "Game Time: hh:mm:ss" of game steps at 130, above the bottom bar.
    if (!opt_.clock || !side_ || !side_->font) return;
    const Font& f = *side_->font;
    const std::uint32_t s = b_.game_tick() / 30;
    char buf[64];
    std::snprintf(buf, sizeof buf, "Game Time: %02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    draw_text(o, o.bounds(), f, buf, 130, opt_.height - 34 - f.height, c_.hud[15]);
}

void GameClient::draw_chat(render::Image8& o) {
    // The chat line (TALK.GUI as the original opens it, 0x494050): a box at the description's
    // place — at the bottom edge, over the bottom bar — clipped to the box: its picture (the
    // CONSOLE gadget, its pixels as they are), the text field filled with interface colour 0
    // (attribute 1), the line in the interface's picture font from 3 rows below the field's top
    // (only the glyphs that fit the field's width less 1; the field has the focus, so its glyphs
    // go through light level 30 — the focus marking of a text field, 0x4A16F0, which sets the
    // gadget's colour word to 30 — instead of a focus ring), then the insertion point: a line in
    // interface colour 9 from the text's top down the font's height (inclusive). No blinking.
    if (!chat_) return;
    const Content::ChatLine& t = c_.chat;
    if (!t.loaded) return;
    int bx = t.x, by = t.y;
    if (bx + t.w > opt_.width) bx = (opt_.width - t.w) / 2;
    if (by + t.h > opt_.height) by = (opt_.height - t.h) / 2;
    if (by < 0) by += opt_.height;
    const render::ClipRect box{std::max(bx, 0), std::max(by, 0), std::min(bx + t.w - 1, opt_.width - 1), std::min(by + t.h - 1, opt_.height - 1)};
    // (the picture's top-left at the gadget's place: the routine adds the frame's offsets and
    // the blit takes them off again)
    if (const render::Sprite8* s = t.picture) render::blit(o, box, *s, bx + t.picture_x + s->x_offset, by + t.picture_y + s->y_offset);
    const int fx = bx + t.field_x, fy = by + t.field_y;
    if (t.field_attribs & 1) fill_rect(o, box, fx, fy, fx + t.field_w - 1, fy + t.field_h - 1, c_.hud[0]);
    if (!t.font) return;
    const Font& f = *t.font;
    int x = fx, width = t.field_w - 1;
    for (const unsigned char ch : chat_text_) {
        const Glyph& g = f.glyphs[ch];
        if (ch < 0x20 || !g.present) continue;
        if (width < g.width) break;
        const int rows = g.rows > 0 ? g.rows : f.height;
        if (ch != ' ')
            for (int r = 0; r < rows; ++r)
                for (int k = 0; k < g.width; ++k) {
                    const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                    if (at >= g.ink.size() || !g.ink[at]) continue;
                    std::uint8_t v = g.colours.empty() ? c_.hud[static_cast<std::size_t>(t.field_colour & 0xFF)] : g.colours[at];
                    if (!g.colours.empty() && c_.light_levels.size() >= 32 * 256) v = c_.light_levels[30 * 256 + v];
                    put_pixel(o, box, x + g.left + k, fy + 3 + g.top + r, v);
                }
        width -= g.width;
        x += g.width;
    }
    const int cx = fx + f.text_width(chat_text_.substr(0, static_cast<std::size_t>(std::clamp(chat_cursor_, 0, static_cast<int>(chat_text_.size())))));
    render::draw_line(o, box, cx, fy + 3, cx, fy + 3 + f.height, c_.hud[9]);
}

void GameClient::draw_unit_marks(render::Image8& o) {
    const render::ClipRect view = view_rect();
    // The drag box: an outer and an inner rectangle.
    if (dragging_) {
        const int sx0 = (drag_start_[0] >> 16) - scroll_x_ + 128;
        const int sy0 = (drag_start_[2] >> 16) - (drag_start_[1] >> 17) - scroll_y_ + 32;
        outline_rect(o, view, sx0, sy0, mouse_x_, mouse_y_, c_.hud[15]);
        const int x0 = std::min(sx0, mouse_x_) + 1, x1 = std::max(sx0, mouse_x_) - 1;
        const int y0 = std::min(sy0, mouse_y_) + 1, y1 = std::max(sy0, mouse_y_) - 1;
        if (x0 <= x1 && y0 <= y1) outline_rect(o, view, x0, y0, x1, y1, c_.hud[0]);
    }
    // Placement (pointer over the view): the footprint's outline from its corner cell to the
    // cell past its far corner (both edges drawn: (footprint x 16) + 1 pixels across) at the
    // height the check found, and a second outline one pixel inside it, both in [10] when it
    // fits, else [4].
    if (mode_ == Action::BuildUnit && in_view(mouse_x_, mouse_y_) && place_valid_ && build_type_ < b_.types.size()) {
        const int fx = b_.types[build_type_].construction.footprint_x, fz = b_.types[build_type_].construction.footprint_z;
        const int ax = place_cx_ * 16 - scroll_x_ + 128, bx = (place_cx_ * 16 + fx * 16) - scroll_x_ + 128;
        const int ay = place_cz_ * 16 - (place_h_ >> 1) - scroll_y_ + 32, by = (place_cz_ * 16 + fz * 16) - (place_h_ >> 1) - scroll_y_ + 32;
        const int x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
        const std::uint8_t col = c_.hud[place_ok_ ? 10 : 4];
        outline_rect(o, view, x0, y0, x1, y1, col);
        outline_rect(o, view, x0 + 1, y0 + 1, x1 - 1, y1 - 1, col);
    }
    // (Health bars and group numbers: draw_unit_bars, before the fog.)
}

void GameClient::draw_unit_bars(render::Image8& o) {
    // With the health bar option only (the group number too): own units' health bars and group
    // numbers. The frame draws them after the units, effects and queued orders, before the fog.
    if (!opt_.damage_bars) return;
    const render::ClipRect view = view_rect();
    for (const UnitScreen& s : on_screen_) {
        const sim::Unit& u = b_.units[s.id];
        const sim::UnitTickState& t = b_.ticks[s.id];
        if (t.type_index == 0 || t.owner != opt_.player) continue;
        // (The selection outline is drawn with the unit itself: add_selection_outlines.)
        if (u.hit_points > 0 && u.type && u.type->max_damage > 0) {
            const int x = s.x, y = s.y + 10;
            fill_rect(o, view, x - 17, y - 2, x + 17, y + 2, c_.hud[0]);
            const int maxd = static_cast<int>(u.type->max_damage);
            const int len = static_cast<int>((static_cast<unsigned>(u.hit_points) << 5) / static_cast<unsigned>(maxd));
            const int third = maxd / 3;
            const std::uint8_t col = u.hit_points > 2 * third ? c_.hud[10] : u.hit_points > third ? c_.hud[14] : c_.hud[12];
            fill_rect(o, view, x - 16, y - 1, x - 16 + len, y + 1, col);
            if (u.group != 0 && side_ && side_->font)
                draw_text(o, view, *side_->font, std::string(1, static_cast<char>('0' + u.group % 10)), x, y + 4,
                          c_.hud[15]);
        }
    }
}

void GameClient::draw_console(render::Image8& o) {
    // 0x464060. The lines shown: from the next line to write, back by at most text_lines - 1
    // lines (stopping at the oldest), up to the newest. Each from (138, 52), one console font
    // height apart (only drawn lines move the row on); with screen chat off only unit notices,
    // chat and other players' notes show. A line with a sender first gets the sender's colour
    // logo stretched over a square of 0.8 font heights at (138, y) and its text starts 1.5 squares
    // right of 138. The text: the picture font's glyphs (characters from 0x20 that have one;
    // a space only moves on), else the console font in interface colour 15 (10 when marked).
    const Font* fnt = c_.console_font ? c_.console_font : (side_ ? side_->font : nullptr);
    if (opt_.text_lines == 0 || !fnt) return;
    int first = console_.next;
    for (int k = 1; k < opt_.text_lines; ++k) {
        if (first == console_.oldest) break;
        if (--first < 0) first = 29;
    }
    const int h = fnt->height & 0xFF;
    int y = 52;
    for (int i = first; i != console_.next; i = i == 29 ? 0 : i + 1) {
        const ConsoleLine& l = console_.lines[static_cast<std::size_t>(i)];
        const int kind = l.kind & 0x0F;
        if (!opt_.screen_chat && kind != kUnitNotice && kind != kChat && kind != kPlayerNote) continue;
        int x = 138;
        if (l.player != kNoPlayer) {
            const int s = static_cast<int>(static_cast<double>(h) * 0.8);
            draw_logo(o, c_.team_logos, c_.player_colour[static_cast<std::size_t>(l.player) % c_.player_colour.size()], Region{138, y, 138 + s, y + s}, 0);
            x = static_cast<int>(138.0 - static_cast<double>(s) * -1.5);
        }
        const std::string_view text(l.text.data(), strnlen(l.text.data(), l.text.size()));
        if (c_.picture_font) {
            const Font& f = *c_.picture_font;
            for (unsigned char ch : text) {
                const Glyph& g = f.glyphs[ch];
                if (ch < 0x20 || !g.present) continue;
                if (ch != ' ')
                    for (int r = 0; r < g.rows; ++r)
                        for (int k = 0; k < g.width; ++k) {
                            const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                            if (g.ink[at]) put_pixel(o, o.bounds(), x + g.left + k, y + g.top + r, g.colours[at]);
                        }
                x += g.width;
            }
        } else {
            bar_text(o, *fnt, std::string(text), x, y, (l.kind & 0x20) ? c_.hud[10] : c_.hud[15]);
        }
        y += h;
    }
}

void GameClient::update_top_bar() {
    // Once per drawn frame, before anything is drawn.
    const sim::PlayerSlot& me = b_.players[static_cast<std::size_t>(opt_.player)];
    const sim::PlayerEconomy& ec = b_.player_economy[static_cast<std::size_t>(opt_.player)];
    // The stored amounts shown move an eighth of the way to the real value each frame.
    const float stored[2] = {me.energy_stored, me.metal_stored};
    const float cap[2] = {ec.energy_storage, ec.metal_storage};
    for (int k = 0; k < 2; ++k) {
        const int cur = static_cast<int>(shown_[static_cast<std::size_t>(k)]);
        const int tgt = static_cast<int>(stored[k]);
        int d = tgt - cur;
        int step = 0;
        if (d < 0) step = d / 8 == 0 ? -1 : d / 8;
        else if (d > 0) step = d / 8 == 0 ? 1 : d / 8;
        float v = static_cast<float>(cur + step);
        if (cap[k] < v) v = cap[k];
        shown_[static_cast<std::size_t>(k)] = v;
    }
    // The rates shown are read again only when the player's display timer has fallen behind the
    // game step (it then moves on 30 steps): energy made and used, metal made and used. The
    // client keeps its own copy of that timer (it starts at the record's).
    if (!bar_timer_set_) {
        bar_timer_set_ = true;
        bar_timer_ = ec.display_tick;
    }
    if (bar_timer_ < b_.game_tick()) {
        bar_timer_ += 30;
        bar_rates_ = {ec.energy_income, ec.energy_usage, ec.metal_income, ec.metal_usage};
    }
    // The original redraws the bar (art included) only when what it shows changed; the bar's own
    // rows keep it in between, but the art's rows inside the view (from row 32) are drawn over by
    // the terrain every frame, so they show the art only in frames that redrew the bar.
    const std::array<float, 9> copy = {static_cast<float>(opt_.player), shown_[0], bar_rates_[0], bar_rates_[1], shown_[1],
                                       bar_rates_[2], bar_rates_[3], cap[0], cap[1]};
    bar_redrawn_ = !bar_copy_valid_ || std::memcmp(copy.data(), bar_copy_.data(), sizeof copy) != 0;
    bar_copy_ = copy;
    bar_copy_valid_ = true;
}

void GameClient::draw_top_bar(render::Image8& o) {
    if (!side_ || !side_->font) return;
    const Font& f = *side_->font;
    const render::ClipRect all = o.bounds();
    const sim::PlayerSlot& me = b_.players[static_cast<std::size_t>(opt_.player)];
    const sim::PlayerEconomy& ec = b_.player_economy[static_cast<std::size_t>(opt_.player)];
    // The viewed player's colour logo in LOGO (right after the bar's art).
    draw_logo(o, c_.team_logos, c_.player_colour[static_cast<std::size_t>(opt_.player) % c_.player_colour.size()], side_->logo, 0);
    // (The shown amounts and rates of this frame: update_top_bar.)
    const float stored[2] = {me.energy_stored, me.metal_stored};
    const float cap[2] = {ec.energy_storage, ec.metal_storage};
    struct Res {
        const Region *bar, *num, *max, *zero, *produced, *consumed;
        std::uint8_t colour;
        float shown, cap, stored, threshold;
    };
    const Res res[2] = {
        {&side_->energy_bar, &side_->energy_num, &side_->energy_max, &side_->energy_zero, &side_->energy_produced,
         &side_->energy_consumed, side_->energy_colour, shown_[0], cap[0], stored[0], ec.share_energy_above},
        {&side_->metal_bar, &side_->metal_num, &side_->metal_max, &side_->metal_zero, &side_->metal_produced,
         &side_->metal_consumed, side_->metal_colour, shown_[1], cap[1], stored[1], ec.share_metal_above},
    };
    for (int k = 0; k < 2; ++k) {
        const Res& r = res[k];
        // The bar (only with a storage above 0): from x1 to x1 + width x shown / storage (a
        // shown 0 still fills the x1 column); the sharing threshold's mark, 3 columns in [12],
        // when one is set and the real stored amount is above it.
        if (r.cap > 0.0f) {
            const int w = r.bar->x2 - r.bar->x1;
            const int x2 = static_cast<int>(ftol(static_cast<double>(w) * static_cast<double>(r.shown) / static_cast<double>(r.cap) + r.bar->x1));
            if (x2 >= r.bar->x1) fill_rect(o, all, r.bar->x1, r.bar->y1, x2, r.bar->y2, r.colour);  // (an end left of the start: nothing)
            if (r.threshold > 0.0f && r.stored > r.threshold) {
                const int t = static_cast<int>(ftol(static_cast<double>(w) * static_cast<double>(r.threshold) / static_cast<double>(r.cap) + r.bar->x1));
                fill_rect(o, all, t, r.bar->y1, t + 2, r.bar->y2, c_.hud[12]);
            }
        }
        bar_text(o, f, fmti("%lld", ftol(r.shown)), r.num->x1, r.num->y1, c_.hud[15]);
        bar_text(o, f, "0", r.zero->x1, r.zero->y1, c_.hud[15]);
        const std::string mx = fmti("%lld", ftol(r.cap));
        bar_text(o, f, mx, r.max->x1 - f.text_width(mx), r.max->y1, c_.hud[15]);
        const float income = bar_rates_[static_cast<std::size_t>(2 * k)], usage = bar_rates_[static_cast<std::size_t>(2 * k + 1)];
        std::string inc, use;
        if (k == 0) {
            // Energy in whole units (truncated): made "%d" ("%dK" in thousands above 99999), used
            // "%d" of its size ("%dK" below -99999).
            inc = !(income > 99999.0f) ? fmti("%lld", ftol(income)) : fmti("%lldK", ftol(income) / 1000);
            use = !(usage < -99999.0f) && usage == usage ? fmti("%lld", magnitude32(ftol(usage))) : fmti("%lldK", ftol(usage) / 1000);
        } else {
            // Metal with one decimal, the use by its size.
            inc = crt_fixed(income, 1);
            use = crt_fixed(std::fabs(static_cast<double>(usage)), 1);
        }
        bar_text(o, f, inc, r.produced->x1, r.produced->y1, c_.hud[10]);
        bar_text(o, f, use, r.consumed->x1, r.consumed->y1, c_.hud[12]);
    }
}

void GameClient::draw_caption(render::Image8& o, const Button& b, const std::string& text, int pressed) {
    // A button caption as the panel's gadgets lay it out: dropped from the end until it fits the
    // width less 6; attribute 1 left (x + 3), 4 right, 2 centred, 0x20 centred on the bottom
    // edge (4 rows up); pressed moves it one pixel right and down. Colour: the gadget's
    // interface colour, black while pressed.
    if (!b.font || text.empty()) return;
    const Font& f = *b.font;
    std::string t = text;
    while (!t.empty() && f.text_width(t) > b.w - 6) t.pop_back();
    const int tw = f.text_width(t);
    const std::uint8_t fg = pressed ? c_.hud[0] : c_.hud[b.colorf];
    int tx = b.x + 3 + pressed, top = b.y + ((b.h - 1) - f.height) / 2 + pressed;
    if (b.attribs & 1) {
    } else if (b.attribs & 4) {
        tx = std::max(b.x, b.x + b.w - tw - 3);
    } else if (b.attribs & 2) {
        tx = b.x + (b.w - 1 - tw) / 2 + pressed + 1;
    } else if (b.attribs & 0x20) {
        tx = b.x + (b.w - 1 - tw) / 2 + pressed + 1;
        top = b.y + b.h - 1 + pressed - f.height - 4;
    } else {
        return;
    }
    draw_text(o, o.bounds(), f, t, tx, top + f.raise, fg);
}

void GameClient::draw_bottom_bar(render::Image8& o) {
    if (!side_ || !side_->font) return;
    const Font& f = *side_->font;
    const render::ClipRect all = o.bounds();
    const int dy = opt_.height - 480;  // the side description's rows are for a 480-row screen
    auto centred = [&](const std::string& s, const Region& r) {
        bar_text(o, f, s, r.x1 - f.text_width(s) / 2, r.y1 + dy, 0x53);
    };
    auto bar = [&](const Region& r, int value, int max) {
        // Filled for value / max (value clamped to 0..max) in [10], the rest in [4].
        const int v = std::clamp(value, 0, std::max(max, 0));
        const int x2 = max != 0 ? (r.x2 - r.x1) * v / max + r.x1 : r.x1;
        fill_rect(o, all, r.x1, r.y1 + dy, x2, r.y2 + dy, c_.hud[10]);
        if (x2 != r.x2) fill_rect(o, all, x2 + 1, r.y1 + dy, r.x2, r.y2 + dy, c_.hud[4]);
    };
    auto whole = [](float v) { return static_cast<int>(v); };  // the original's float -> int (truncation)

    // A build button under the pointer comes first: "<name>  M:<metal cost> E:<energy cost>" on
    // the NAME line and the type's description on the DESCRIPTION line (not for types the content excludes).
    if (const Panel* p = current_panel())
        for (const Button& btn : p->buttons) {
            if (mouse_x_ < btn.x || mouse_x_ >= btn.x + btn.w || mouse_y_ < btn.y || mouse_y_ >= btn.y + btn.h) continue;
            if (btn.action != Action::BuildUnit || !btn.unit_type || btn.unit_type >= c_.types.size()) return;
            const UnitTypeLook& look = c_.types[btn.unit_type];
            if (look.no_build_line) return;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s  M:%d E:%d", look.name.c_str(), whole(look.cost_metal), whole(look.cost_energy));
            bar_text(o, f, buf, side_->name.x1, side_->name.y1 + dy, 0x53);
            bar_text(o, f, look.description, side_->description.x1, side_->description.y1 + dy, 0x53);
            return;
        }

    // Else the unit under the pointer (the pointer's pick of this frame; nothing when the pointer
    // is over no unit), else the feature under it.
    const std::uint16_t id = hover_;
    if (!id || b_.ticks[id].type_index == 0) {
        if (id || hover_feature_ < 0 || static_cast<std::size_t>(hover_feature_) >= b_.features.size()) return;
        // A feature: its description and what reclaiming it yields (" M:%d", " E:%d", each only
        // when not zero), left-aligned on the NAME line; flag 0x200: the description alone;
        // flag 0x400: nothing.
        const std::size_t k = static_cast<std::size_t>(hover_feature_);
        const sim::FeatureTypeInfo& ft = b_.features[k];
        const Content::FeatureLook* fl = k < c_.features.size() ? &c_.features[k] : nullptr;
        const std::uint16_t fflags = fl ? fl->flags : 0;
        if (fflags & 0x400) return;
        std::string n = fl ? fl->name : std::string();
        if (!(fflags & 0x200)) {
            char m[32] = "", e[32] = "";
            if (!(ft.metal == 0.0f || ft.metal != ft.metal)) std::snprintf(m, sizeof m, " M:%d", whole(ft.metal));
            if (!(ft.energy == 0.0f || ft.energy != ft.energy)) std::snprintf(e, sizeof e, " E:%d", whole(ft.energy));
            n += std::string(" ") + m + e;
        }
        bar_text(o, f, n, side_->name.x1, side_->name.y1 + dy, 0x53);
        return;
    }
    const sim::Unit& u = b_.units[id];
    const sim::UnitTickState& t = b_.ticks[id];
    const bool mine = t.owner == opt_.player;
    // Units the viewed player does not see are an "Unidentified object".
    if (!mine && b_.settings.fog_of_war && !b_.unit_seen_by(opt_.player, u)) {
        centred("Unidentified object", side_->unit_name);
        return;
    }
    centred(t.type_index < c_.types.size() ? c_.types[t.type_index].name : std::string(), side_->unit_name);
    // Health: own units, and others unless their type hides it (type flag 0x4000).
    auto hides_health = [&](const sim::UnitTickState& tt) { return tt.tick_type && (tt.tick_type->type_flags & 0x4000u); };
    if (u.type && (mine || !hides_health(t))) bar(side_->damage_bar, u.hit_points, static_cast<int>(u.type->max_damage));
    // The owner's colour logo in LOGO2.
    draw_logo(o, c_.team_logos, c_.player_colour[static_cast<std::size_t>(t.owner) % c_.player_colour.size()], side_->logo2, dy);
    if (mine) {
        bar_text(o, f, "+" + crt_fixed(t.metal_make_prev, 1), side_->unit_metal_make.x1, side_->unit_metal_make.y1 + dy, c_.hud[10]);
        bar_text(o, f, "+" + crt_fixed(t.energy_make_prev, 0), side_->unit_energy_make.x1, side_->unit_energy_make.y1 + dy,
                  c_.hud[10]);
        bar_text(o, f, "-" + crt_fixed(t.metal_use_prev, 1), side_->unit_metal_use.x1, side_->unit_metal_use.y1 + dy, c_.hud[12]);
        bar_text(o, f, "-" + crt_fixed(t.energy_use_prev, 0), side_->unit_energy_use.x1, side_->unit_energy_use.y1 + dy,
                  c_.hud[12]);
        // Kills of armed units (unit flag bit 31): "1 kill", "<n> kills", from 5 "<n> kills - Veteran".
        if (t.kill_count > 0 && (u.flags & 0x80000000u)) {
            const std::string k = std::to_string(t.kill_count) + (t.kill_count == 1 ? " kill" : " kills") + (t.kill_count >= 5 ? " - Veteran" : "");
            bar_text(o, f, k, side_->damage_bar.x1, side_->damage_bar.y2 + 2 + dy, c_.hud[15]);
        }
        // The unit's status: what its head order does (the mission table's text of the kind;
        // the text of mission kind 0 without orders), centred on the status line's x1.
        if (!c_.mission_status.empty()) {
            const std::size_t kind = u.orders ? u.orders->kind : 0u;
            centred(c_.mission_status[kind < c_.mission_status.size() ? kind : 0], side_->mission_text);
        }
    }
    // The second line: a stockpile weapon being built (the first secondary order with flag
    // 0x80000: "Weapon" and its progress, param 2 x 100 / the slot's reload time, own units),
    // else the head order's target (own units; when the viewed player sees it).
    int building = 0;
    for (const sim::MissionOrder* so = u.secondary_orders; so; so = so->next)
        if (so->flags & 0x80000u) {
            const int slot = so->param0;
            const sim::WeaponType* wt = slot >= 0 && slot < 3 ? u.weapons[static_cast<std::size_t>(slot)].type : nullptr;
            if (wt && wt->reload_time) building = static_cast<int>(static_cast<std::uint32_t>(so->param2) * 100u) / static_cast<int>(wt->reload_time);
            break;
        }
    if (building == 0) {
        const sim::Unit* target = mine && u.orders ? u.orders->target.unit : nullptr;
        if (!target || b_.ticks[target->id].type_index == 0 || b_.ticks[target->id].type_index >= c_.types.size()) return;
        const sim::UnitTickState& tt = b_.ticks[target->id];
        if (tt.owner != opt_.player && b_.settings.fog_of_war && !b_.unit_seen_by(opt_.player, *target)) return;
        centred(c_.types[tt.type_index].name, side_->unit_name2);
        if (target->type && (tt.owner == opt_.player || !hides_health(tt)))
            bar(side_->damage_bar2, target->hit_points, static_cast<int>(target->type->max_damage));
    } else if (mine) {
        centred("Weapon", side_->unit_name2);
        bar(side_->damage_bar2, std::min(building, 100), 100);
    }
}

} // namespace client
