// Menu screens run from a layout description (spec: docs/RE_MEMORY.md, "Client",
// "Menus": the widget look and input rules).

#include "client/menu.h"

#include <algorithm>
#include <cctype>

namespace client {
namespace {

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool inside(const Widget& w, int ox, int oy, int x, int y) {
    return x >= ox + w.x && x < ox + w.x + w.w && y >= oy + w.y && y < oy + w.y + w.h;
}

int row_height(const MenuContent& mc) { return mc.font ? mc.font->height + 1 : 12; }

}  // namespace

LayoutScreen::LayoutScreen(const ScreenLayout* layout) : layout_(layout) {
    if (!layout_) return;
    widgets_ = layout_->widgets;
    ox_ = layout_->x;
    oy_ = layout_->y;
    for (std::size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].kind == WidgetKind::TextField && widgets_[i].active && focus_ < 0) focus_ = static_cast<int>(i);
}

Widget* LayoutScreen::widget(const std::string& name) {
    for (Widget& w : widgets_)
        if (same_name(w.name, name)) return &w;
    return nullptr;
}

std::string LayoutScreen::caption(const Widget& w) {
    if (w.stages <= 0) return w.text;
    std::size_t start = 0;
    for (int k = 0; k < w.stage; ++k) {
        const auto bar = w.text.find('|', start);
        if (bar == std::string::npos) return w.text.substr(start);
        start = bar + 1;
    }
    const auto bar = w.text.find('|', start);
    return w.text.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
}

std::uint8_t LayoutScreen::colour(int k) const {
    if (!layout_) return static_cast<std::uint8_t>(k);
    return layout_->colours[static_cast<std::size_t>(k & 0xFF)];
}

int LayoutScreen::hit(int x, int y) {
    // The last drawn widget under the point wins (later entries lie on top).
    for (int i = static_cast<int>(widgets_.size()) - 1; i >= 1; --i) {
        const Widget& w = widgets_[static_cast<std::size_t>(i)];
        if (!w.active || w.kind == WidgetKind::Label || w.kind == WidgetKind::FontRef || w.kind == WidgetKind::Header) continue;
        if (inside(w, ox_, oy_, x, y)) return i;
    }
    return -1;
}

namespace {
// A slider's value from its knob offset: offset / (travel - 1) x maximum, truncated.
int slider_value(const Widget& s, int knob) {
    if (s.span < 2) return 0;
    return static_cast<int>(static_cast<double>(knob) / (s.span - 1) * s.range);
}
// The knob offset for a value (clamped to the maximum): value / maximum x (travel - 1), rounded
// up when not whole.
int slider_knob(const Widget& s, int value) {
    if (s.range <= 0) return 0;
    const double t = static_cast<double>(std::min(value, s.range)) / s.range * (s.span - 1);
    const int k = static_cast<int>(t);
    return t - k != 0.0 ? static_cast<int>(t + 1.0) : k;
}
}  // namespace

void LayoutScreen::press(FrontEnd& fe, Widget& w) {
    if (w.grayed) return;
    if (w.slider_step != 0) {
        // A slider's arrow moves its knob one pixel; the value follows the knob.
        if (Widget* s = widget(w.link)) {
            if (s->knob_value != s->position) s->knob_pos = slider_knob(*s, s->position);
            s->knob_pos = std::clamp(s->knob_pos + w.slider_step, 0, std::max(0, s->span - 1));
            s->position = slider_value(*s, s->knob_pos);
            s->knob_value = s->position;
            on_press(fe, *s);
        }
        return;
    }
    if (w.stages > 0) {
        // A switch steps forward (left click) or back (right click) through its stages.
        w.stage = press_button_ == 3 ? (w.stage + w.stages - 1) % w.stages : (w.stage + 1) % w.stages;
    }
    fe.play_sound(w.sound >= 0 ? w.sound : fe.menu_content().click_sound);
    for (std::size_t i = 0; i < widgets_.size(); ++i)
        if (&widgets_[i] == &w) focus_gadget_ = static_cast<int>(i);  // a pressed gadget takes the focus
    on_press(fe, w);
}

void LayoutScreen::scroll_to(Widget& bar, int x, int y) {
    if (bar.slider_base >= 0 && !bar.vertical) {
        // The knob follows the pointer; the value follows the knob.
        bar.knob_pos = std::clamp(x - (ox_ + bar.x) - 3 - bar.knob / 2, 0, std::max(0, bar.span - 1));
        bar.position = slider_value(bar, bar.knob_pos);
        bar.knob_value = bar.position;
        return;
    }
    // The knob follows the pointer; a scroll bar linked to a list moves the list's first row.
    const int span = bar.vertical ? bar.h : bar.w;
    const int at = bar.vertical ? y - (oy_ + bar.y) : x - (ox_ + bar.x);
    const int range = std::max(bar.range, 0);
    bar.position = span > 0 ? std::clamp(at * (range + 1) / std::max(span, 1), 0, range) : 0;
    if (!bar.link.empty())
        if (Widget* list = widget(bar.link)) list->top = bar.position;
}

void LayoutScreen::handle(FrontEnd& fe, const InputEvent& e) {
    switch (e.type) {
    case InputEvent::Type::MouseDown: {
        press_button_ = e.button;
        const int i = hit(e.x, e.y);
        if (i < 0) return;
        Widget& w = widgets_[static_cast<std::size_t>(i)];
        switch (w.kind) {
        case WidgetKind::Button:
            if (!w.grayed) held_ = i;
            break;
        case WidgetKind::ListBox: {
            const int row = (e.y - (oy_ + w.y)) / row_height(fe.menu_content()) + w.top;
            if (row >= 0 && row < static_cast<int>(w.items.size())) {
                const bool dbl = row == last_click_row_ && e.time_ms - last_click_ms_ < 350;
                last_click_ms_ = e.time_ms;
                last_click_row_ = row;
                w.selected = row;
                on_pick(fe, w, row, dbl);
            }
            break;
        }
        case WidgetKind::ScrollBar:
            drag_ = i;
            scroll_to(w, e.x, e.y);
            on_press(fe, w);
            break;
        case WidgetKind::TextField: focus_ = i; break;
        default: on_press(fe, w); break;
        }
        return;
    }
    case InputEvent::Type::MouseMove:
        if (drag_ >= 0) {
            scroll_to(widgets_[static_cast<std::size_t>(drag_)], e.x, e.y);
            on_press(fe, widgets_[static_cast<std::size_t>(drag_)]);
        }
        return;
    case InputEvent::Type::MouseUp: {
        drag_ = -1;
        const int was = held_;
        held_ = -1;
        if (was >= 0 && hit(e.x, e.y) == was) press(fe, widgets_[static_cast<std::size_t>(was)]);
        return;
    }
    case InputEvent::Type::KeyDown: {
        const int k = e.key;
        if (focus_ >= 0) {
            Widget& f = widgets_[static_cast<std::size_t>(focus_)];
            if (k == keys::kBackspace) {
                if (!f.text.empty()) f.text.pop_back();
                return;
            }
            if (k == keys::kEnter) {
                on_text(fe, f);
                return;
            }
            if (k >= 32 && k < 127) {
                char c = static_cast<char>(k);
                if (e.shift) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (f.max_chars <= 0 || static_cast<int>(f.text.size()) < f.max_chars) f.text.push_back(c);
                return;
            }
        }
        auto press_named = [&](const std::string& name) -> bool {
            if (name.empty()) return false;
            Widget* w = widget(name);
            if (!w || !w->active) return false;
            if (w->kind == WidgetKind::ListBox) {
                if (w->selected >= 0) on_pick(fe, *w, w->selected, true);
                return true;
            }
            press_button_ = 1;
            press(fe, *w);
            return true;
        };
        if (k == keys::kEnter && layout_ && press_named(layout_->enter_default)) return;
        if (k == keys::kEscape && layout_ && press_named(layout_->escape_default)) return;
        if (k > 0 && k < 0x100 && !e.ctrl)
            for (Widget& w : widgets_)
                if (w.kind == WidgetKind::Button && w.active && w.quickkey && w.quickkey == std::tolower(k)) {
                    press_button_ = 1;
                    press(fe, w);
                    return;
                }
        return;
    }
    default: return;
    }
}

void LayoutScreen::draw_widget(FrontEnd& fe, render::Image8& out, const Widget& w, bool held) {
    const MenuContent& mc = fe.menu_content();
    const render::ClipRect clip = out.bounds();
    const int x = ox_ + w.x, y = oy_ + w.y;
    const Font* font = mc.font;
    // mark: the quick key to mark in the caption (0: none; only buttons that are not grayed).
    auto text_at = [&](const std::string& s, int align, int pressed, std::uint8_t fg, int mark = 0) {
        if (!font || s.empty()) return;
        // Characters are dropped from the end until the caption fits (width - 6).
        std::string t = s;
        while (!t.empty() && font->text_width(t) > w.w - 6) t.pop_back();
        const int tw = font->text_width(t);
        const int top = y + ((w.h - 1) - font->height) / 2 + pressed;  // the text's top row
        const int ty = top + font->raise;
        int tx = x + 3 + pressed;
        if (align & 1) {
        } else if (align & 4) {
            tx = std::max(x, x + w.w - tw - 3);
        } else if (align & 2) {
            tx = x + (w.w - 1 - tw) / 2 + pressed + 1;
            // The quick key's first occurrence (exact case) is underlined: a row under the
            // character in interface colour 2 (0 while pressed).
            const auto at = mark ? t.find(static_cast<char>(mark)) : std::string::npos;
            draw_text(out, clip, *font, t, tx, ty, fg);
            if (at != std::string::npos) {
                const int cx = tx + font->text_width(t.substr(0, at));
                const int cw = font->text_width(t.substr(at, 1));
                if (cw > 0) fill_rect(out, clip, cx, top + font->height - 1, cx + cw - 1, top + font->height - 1, colour(pressed ? 0 : 2));
            }
            return;
        } else if (align & 0x20) {
            // Centred on the bottom edge; the quick key's character in interface colour 10.
            tx = x + (w.w - 1 - tw) / 2 + pressed + 1;
            const int by = y + w.h - 1 + pressed - font->height - 4 + font->raise;
            const auto at = mark ? t.find(static_cast<char>(mark)) : std::string::npos;
            if (at == std::string::npos) {
                draw_text(out, clip, *font, t, tx, by, fg);
                return;
            }
            const std::string a = t.substr(0, at), k = t.substr(at, 1), b = t.substr(at + 1);
            draw_text(out, clip, *font, a, tx, by, fg);
            draw_text(out, clip, *font, k, tx + font->text_width(a), by, colour(10));
            draw_text(out, clip, *font, b, tx + font->text_width(a) + font->text_width(k), by, fg);
            return;
        } else {
            return;  // no alignment bit: no caption
        }
        draw_text(out, clip, *font, t, tx, ty, fg);
    };
    switch (w.kind) {
    case WidgetKind::Button: {
        const bool pressed = held && !w.grayed;
        if (w.frames && !w.frames->empty()) {
            const int n = static_cast<int>(w.frames->size());
            int f = 0;
            if (w.grayed) f = w.stages > 0 ? std::min(w.stage, n - 1) : std::min(2, n - 1);
            else if (pressed) f = w.stages > 0 ? std::max(0, n - 2) : std::min(1, n - 1);
            else f = w.stages > 0 ? std::min(w.stage, n - 1) : 0;
            const render::Sprite8& s = (*w.frames)[static_cast<std::size_t>(f)];
            render::blit(out, clip, s, x + s.x_offset, y + s.y_offset);
        } else {
            // A filled box with a two-pixel bevel (interface colours of the screen): filled with
            // colour 20 (grayed: 19); top and left edges in colour 17 and bottom and right in
            // colour 0 (pressed: the other way round; grayed: top/left 0, bottom/right 19).
            const int x2 = x + w.w - 1, y2 = y + w.h - 1;
            std::uint8_t fill = colour(20), tl = colour(17), br = colour(0);
            if (w.grayed) fill = colour(19), tl = colour(0), br = colour(19);
            else if (pressed) tl = colour(0), br = colour(17);
            fill_rect(out, clip, x, y, x2, y2, fill);
            fill_rect(out, clip, x, y, x2, y, tl);
            fill_rect(out, clip, x, y + 1, x2 - 1, y + 1, tl);
            fill_rect(out, clip, x, y, x, y2, tl);
            fill_rect(out, clip, x + 1, y, x + 1, y2 - 1, tl);
            fill_rect(out, clip, x2, y + 1, x2, y2, br);
            fill_rect(out, clip, x2 - 1, y + 2, x2 - 1, y2, br);
            fill_rect(out, clip, x + 1, y2, x2, y2, br);
            fill_rect(out, clip, x + 2, y2 - 1, x2, y2 - 1, br);
        }
        const std::uint8_t fg = w.grayed ? mc.text_dim : (w.fg ? colour(w.fg) : mc.text_colour);
        text_at(caption(w), w.attribs, pressed ? 1 : 0, fg, w.grayed ? 0 : w.quickkey_char);
        break;
    }
    case WidgetKind::Label: {
        // Labels draw with the small picture font from their top edge: x as given, attribute 2
        // centred on the middle (x + w/2 - text width/2), 4 right-aligned; a background colour
        // fills the rectangle first; the quick key's first occurrence is underlined in
        // interface colour 2 on the text's last row.
        const Font* lf = mc.small_font ? mc.small_font : font;
        if (!lf) break;
        if (w.bg) fill_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, colour(w.bg));
        const int tw = lf->text_width(w.text);
        int tx = x;
        if (w.attribs & 4) tx = x + w.w - tw;
        else if (w.attribs & 2) tx = x + w.w / 2 - tw / 2;
        // One line clipped to the width (taller labels wrap; drawn here as one line).
        draw_text(out, clip, *lf, w.text, tx, y + lf->raise, w.fg ? colour(w.fg) : mc.text_colour, -1, w.w);
        if (w.quickkey_char && !w.grayed)
            if (const auto at = w.text.find(static_cast<char>(w.quickkey_char)); at != std::string::npos) {
                const int cx = tx + lf->text_width(w.text.substr(0, at));
                const int cw = lf->text_width(w.text.substr(at, 1));
                if (cw > 0) fill_rect(out, clip, cx, y + lf->height - 1, cx + cw - 1, y + lf->height - 1, colour(2));
            }
        break;
    }
    case WidgetKind::Picture:
        if (w.picture) render::blit(out, clip, *w.picture, x + w.picture->x_offset, y + w.picture->y_offset);
        else if (w.frames && !w.frames->empty()) {
            const render::Sprite8& s = (*w.frames)[static_cast<std::size_t>(std::clamp(w.stage, 0, static_cast<int>(w.frames->size()) - 1))];
            render::blit(out, clip, s, x + s.x_offset, y + s.y_offset);
        }
        break;
    case WidgetKind::ListBox: {
        const int rh = row_height(mc);
        const int rows = std::max(1, w.h / rh);
        for (int r = 0; r < rows; ++r) {
            const int i = w.top + r;
            if (i < 0 || i >= static_cast<int>(w.items.size())) break;
            const int ry = y + r * rh;
            if (i == w.selected) fill_rect(out, clip, x, ry, x + w.w - 1, ry + rh - 1, mc.text_colour);
            if (font)
                draw_text(out, clip, *font, w.items[static_cast<std::size_t>(i)], x + 2, ry + font->raise,
                          i == w.selected ? mc.box_colour : mc.text_colour, -1, w.w - 4);
        }
        break;
    }
    case WidgetKind::ScrollBar: {
        if (w.slider_base >= 0 && w.frames && w.frames->size() >= 20) {
            const auto& fr = *w.frames;
            const auto f = [&](int k) -> const render::Sprite8& { return fr[static_cast<std::size_t>(w.slider_base + k)]; };
            auto put = [&](const render::Sprite8& s, int px, int py) { render::blit(out, clip, s, px, py); };
            const int x2 = x + w.w - 1, y2 = y + w.h - 1;
            if (w.w < w.h) {
                // Vertical: track down the bar; the knob centred across it.
                int cy = y;
                put(f(0), x, cy);
                cy += f(0).height;
                while (cy + f(1).height <= y2) {
                    put(f(1), x, cy);
                    cy += f(1).height;
                }
                put(f(2), x, y2 - f(2).height + 1);
                const int kx = x + f(2).width / 2 - f(3).width / 2;
                const int kp = w.knob_value == w.position ? w.knob_pos : slider_knob(w, w.position);
                int top = kp + 3 + y;
                const int len = std::min(w.knob > 0 ? w.knob : 10, w.h - 6);
                const int bottom = std::min(top + len - 1, y + w.h - 4);
                top = std::min(top, bottom - len + 1);
                put(f(3), kx, top);
                int ky = top + f(3).height;
                while (ky <= bottom - f(4).height) {
                    put(f(4), kx, ky);
                    ky += f(4).height;
                }
                put(f(4), kx, bottom - f(4).height);
                put(f(5), kx, bottom - f(5).height + 1);
            } else {
                // Horizontal: track along the bar; the knob centred on the end piece's height.
                int cx = x;
                put(f(0), cx, y);
                cx += f(0).width;
                while (cx + f(1).width <= x2) {
                    put(f(1), cx, y);
                    cx += f(1).width;
                }
                put(f(2), x2 - f(2).width + 1, y);
                const int kp = w.knob_value == w.position ? w.knob_pos : slider_knob(w, w.position);
                const int ky = y + f(2).height / 2 - f(3).height / 2;
                const int kx = std::min(kp + 3 + x, x2 - f(3).width - 2);
                put(f(3), kx, ky);
            }
            break;
        }
        outline_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, mc.frame_colour);
        const int span = w.vertical ? w.h : w.w;
        const int knob = std::max(6, std::min(span, w.knob > 0 ? w.knob : 8));
        const int range = std::max(w.range, 1);
        const int at = (span - knob) * std::clamp(w.position, 0, range) / range;
        if (w.vertical) fill_rect(out, clip, x + 1, y + at, x + w.w - 2, y + at + knob - 1, mc.text_colour);
        else fill_rect(out, clip, x + at, y + 1, x + at + knob - 1, y + w.h - 2, mc.text_colour);
        break;
    }
    case WidgetKind::TextField: {
        if (font) {
            std::string t = w.text;
            const bool focused = focus_ >= 0 && &widgets_[static_cast<std::size_t>(focus_)] == &w;
            if (focused) t += '_';
            while (!t.empty() && font->text_width(t) > w.w - 4) t.erase(t.begin());
            draw_text(out, clip, *font, t, x + 2, y + (w.h - font->height) / 2 + font->raise, mc.text_colour);
        }
        break;
    }
    default: break;
    }
}

void LayoutScreen::place(int fw, int fh) {
    placed_ = true;
    if (!layout_ || widgets_.empty()) return;
    const Widget& h = widgets_.front();
    int x = layout_->x, y = layout_->y;
    if (x == -1) {
        x = (fw - h.w) / 2;
        y = (fh - h.h) / 2;
    } else if (x == -2) {
        x = (fw - 128 - h.w) / 2 + 128;
        y = (fh - h.h) / 2;
    }
    if (x + h.w > fw) x = (fw - h.w) / 2;
    if (y + h.h > fh) y = (fh - h.h) / 2;
    if (y < 0) y += fh;
    ox_ = x;
    oy_ = y;
    // The focus: the header's default, else the first focusable gadget after the header in reading
    // order (key x + 5000 y; keys not past the header's own come after all others).
    focus_gadget_ = -1;
    if (!layout_->default_focus.empty())
        for (std::size_t i = 1; i < widgets_.size(); ++i)
            if (same_name(widgets_[i].name, layout_->default_focus)) {
                focus_gadget_ = static_cast<int>(i);
                break;
            }
    if (focus_gadget_ < 0) {
        const long long here = x + 5000LL * y;
        long long best = here + 100000000LL;
        for (std::size_t i = 1; i < widgets_.size(); ++i) {
            const Widget& w = widgets_[i];
            if (!w.active || (w.attribs & 0x400)) continue;
            const bool ok = (w.kind == WidgetKind::Button && !w.grayed) || (w.kind == WidgetKind::ListBox && !(w.attribs & 0x100)) ||
                            w.kind == WidgetKind::TextField || w.kind == WidgetKind::Surface || (w.kind == WidgetKind::ScrollBar && w.w >= w.h);
            if (!ok) continue;
            long long k = w.x + 5000LL * w.y;
            if (k <= here) k += 100000000LL;
            if (k < best) {
                best = k;
                focus_gadget_ = static_cast<int>(i);
            }
        }
    }
}

void LayoutScreen::draw_focus_ring(FrontEnd& fe, render::Image8& out) {
    // Six shaded outlines around the focused gadget, one to six pixels out, light levels 31,
    // 28, 24, 19, 13, 6 (the four sides drawn as separate lines, so corners are lit twice).
    const std::vector<std::uint8_t>& light = fe.menu_content().light;
    if (focus_gadget_ < 0 || light.size() < 0x2000) return;
    const Widget& w = widgets_[static_cast<std::size_t>(focus_gadget_)];
    if (!w.active || w.kind == WidgetKind::TextField || w.kind == WidgetKind::Label || w.kind == WidgetKind::ListBox) return;
    const int gx0 = ox_, gy0 = oy_, gx1 = ox_ + widgets_.front().w - 1, gy1 = oy_ + widgets_.front().h - 1;  // the screen's own surface
    auto px = [&](int x, int y, int level) {
        if (x < gx0 || y < gy0 || x > gx1 || y > gy1 || x < 0 || y < 0 || x >= out.width || y >= out.height) return;
        std::uint8_t& p = out.row(y)[x];
        p = light[static_cast<std::size_t>(level) * 256 + p];
    };
    int x0 = ox_ + w.x, y0 = oy_ + w.y, x1 = x0 + w.w - 1, y1 = y0 + w.h - 1;
    int level = 31;
    for (int i = 0; i < 6; ++i) {
        --x0, --y0, ++x1, ++y1;
        for (int x = x0; x <= x1; ++x) px(x, y0, level);
        for (int y = y0; y <= y1; ++y) px(x1, y, level);
        for (int x = x0; x <= x1; ++x) px(x, y1, level);
        for (int y = y0; y <= y1; ++y) px(x0, y, level);
        level -= 3 + i;
    }
}

void LayoutScreen::draw(FrontEnd& fe, render::Image8& out) {
    if (!placed_) place(out.width, out.height);
    if (layout_ && layout_->background) {
        const render::Sprite8& bg = *layout_->background;
        const bool full = bg.width >= out.width && bg.height >= out.height;
        render::blit(out, out.bounds(), bg, (full ? 0 : ox_) + bg.x_offset, (full ? 0 : oy_) + bg.y_offset);
    }
    draw_under(fe, out);
    for (std::size_t i = 1; i < widgets_.size(); ++i) {
        const Widget& w = widgets_[i];
        if (!w.active) continue;
        if (w.kind == WidgetKind::Surface) {
            draw_surface(fe, out, w);
            continue;
        }
        draw_widget(fe, out, w, static_cast<int>(i) == held_);
    }
    draw_focus_ring(fe, out);
    draw_over(fe, out);
}

}  // namespace client
