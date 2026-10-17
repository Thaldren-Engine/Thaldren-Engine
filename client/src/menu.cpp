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

// The game's float-to-integer conversion: truncation (out of range: 0x80000000).
int conv(double v) {
    if (!(v > -2147483648.0 && v < 2147483648.0)) return static_cast<int>(0x80000000u);
    return static_cast<int>(v);
}
// The rows a list shows for the keys and the selection (the font's height + 1 apart).
int visible_rows(const Widget& list, int fh) { return (list.h - 2) / (fh + 1); }

// A rectangle of the screen's surface through a row of the light table (level > 0) or the shade
// table (level < 0, row level + 32). The original indexes the row with the pixel as a signed byte:
// pixels from 0x80 up take the previous row's entry.
void shade_rect(const MenuContent& mc, render::Image8& out, const render::ClipRect& clip, int x0, int y0, int x1, int y1, int level) {
    const std::vector<std::uint8_t>& t = level < 0 ? mc.shade : mc.light;
    if (t.size() < 0x2000) return;
    const int row = level < 0 ? std::max(level, -32) + 32 : std::min(level, 31);
    x0 = std::max(x0, clip.x0);
    y0 = std::max(y0, clip.y0);
    x1 = std::min(x1, clip.x1);
    y1 = std::min(y1, clip.y1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            std::uint8_t& p = out.row(y)[x];
            const long at = static_cast<long>(row) * 256 + static_cast<std::int8_t>(p);
            if (at >= 0) p = t[static_cast<std::size_t>(at)];
        }
}

// One line of text with the picture font from (x, y) (the glyphs' own placement), stopping at the
// first glyph wider than what is left of `width` (-1: no limit); characters below 0x20 are skipped.
int draw_line(render::Image8& out, const render::ClipRect& clip, const Font& font, std::string_view text, int x, int y, int width) {
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20) continue;
        const Glyph& g = font.glyphs[c];
        if (width != -1 && width < g.width) break;
        if (c != ' ') draw_text(out, clip, font, std::string_view(&ch, 1), x, y, 0);
        if (width != -1) {
            width -= g.width;
            if (width < 0) break;
        }
        x += g.width;
    }
    return x;
}

}  // namespace

// Text broken into lines at spaces and carriage returns (the original's 0x4A51D0): words are
// added while the line fits `width`; a line is drawn when the next word would not fit, at a
// carriage return and at the end; lines are font height + 2 apart and stop when the height is used.
int draw_wrapped(render::Image8& out, const render::ClipRect& clip, const Font& font, const std::string& text, int x, int y, int width, int height) {
    const std::size_t n = text.size();
    std::size_t start = 0, last_break = 0, at = 0;
    while (start < n && text[start] != '\0') {
        while (at != n && text[at] != ' ' && text[at] != '\r') ++at;
        const int w = font.text_width(std::string_view(text).substr(start, at - start));
        std::size_t end = at;
        bool draw = false;
        if (w > width) {
            end = last_break;
            draw = true;
        } else if (at == n || text[at] == '\r') {
            draw = true;
        }
        if (draw) {
            draw_line(out, clip, font, std::string_view(text).substr(start, end > start ? end - start : 0), x, y, width);
            y += font.height + 2;
            height -= font.height + 2;
            if (end >= n) return y;
            start = last_break = end + 1;
            at = start;
            if (height < 1) return y;
        } else {
            last_break = at;
            ++at;
        }
    }
    return y;
}

void LayoutScreen::draw_text_list(const MenuContent& mc, render::Image8& out, const render::ClipRect& clip, const Widget& w, int x, int y) {
    // The original's text list (0x4A1B40): rows from (x + 2, y + 2), row_h apart, while what is
    // left of the height is at least the font's height; each row's text placed by the list's
    // attributes in [x + 2, x + w]; the selected row (not with attribute 0x100) is then lit through
    // light level 30 over x + 2 .. x + w and its top .. top + row_h. Rows starting "&" lose two
    // characters; "&G" rows are shaded (levels -19 .. -22 in turn) instead.
    const Font* font = mc.font;
    if (!font || w.items.empty()) return;
    const int fh = font->height;
    const int rh = w.row_h ? w.row_h : fh + 1;
    int left = w.h;
    for (int k = 0, i = w.top;; ++k, ++i) {
        if (i < 0 || i >= static_cast<int>(w.items.size())) break;
        const int x0 = x + 2, x1 = x + w.w, top = y + 2 + k * rh, bottom = top + rh;
        std::string t = w.items[static_cast<std::size_t>(i)];
        bool dim = false;
        if (!t.empty() && t[0] == '&') {
            dim = t.size() > 1 && t[1] == 'G';
            t = t.size() > 2 ? t.substr(2) : std::string();
        }
        const int tw = font->text_width(t);
        int tx = x0, width = x1 - x0 + 1;
        if (w.attribs & 1) {
        } else if (w.attribs & 4) {
            tx = x1 - tw;
            width = tw;
        } else if (w.attribs & 2) {
            tx = std::max((x0 + x1 - tw) / 2, x0);
            width = x1 - tx + 1;
        }
        if (fh + 6 < rh) draw_wrapped(out, clip, *font, t, tx, top, width, w.h - 1);
        else draw_line(out, clip, *font, t, tx, top, width);
        if (dim) {
            for (int level = -19; level >= -22; --level) shade_rect(mc, out, clip, x0, top, x1, bottom, level);
        } else if (!(w.attribs & 0x100) && w.selected == i) {
            shade_rect(mc, out, clip, x0, top, x1, bottom, 30);
        }
        left -= rh;
        if (left < fh) break;
    }
}

LayoutScreen::LayoutScreen(const ScreenLayout* layout) : layout_(layout) {
    if (!layout_) return;
    widgets_ = layout_->widgets;
    focus_limit_ = widgets_.size();
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
    if (colours_) return (*colours_)[static_cast<std::size_t>(k & 0xFF)];
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
// The knob offset for a value (clamped to the maximum) as the option pages set it: value x
// (travel - 1) x the single-precision reciprocal of the maximum, rounded up when not whole (so
// a reciprocal a little above the exact one moves a knob that would sit on a whole pixel one on).
int slider_knob(const Widget& s, int value) {
    if (s.range <= 0) return 0;
    const double t = static_cast<double>(std::min(value, s.range)) * (s.span - 1) * static_cast<double>(1.0f / static_cast<float>(s.range));
    const int k = static_cast<int>(t);
    return t - k != 0.0 ? static_cast<int>(t + 1.0) : k;
}
}  // namespace

void LayoutScreen::press(FrontEnd& fe, Widget& w) {
    if (w.grayed) return;
    if (w.slider_step != 0) {
        // A slider's arrow moves its knob one pixel; the value follows the knob.
        if (Widget* s = widget(w.link)) {
            if (s->list_slider) {
                // A list's slider: the knob one pixel on, the list following it.
                const int k = std::max(0, std::min(s->knob_pos + w.slider_step, s->range - 1));
                if (k == s->knob_pos) return;
                s->knob_pos = s->position = s->knob_value = k;
                if (Widget* l = list_of(*s); l && l->row_h != 0)
                    l->top = static_cast<std::int16_t>(conv(static_cast<double>(static_cast<int>(l->items.size()) - l->h / l->row_h) * k / (s->range - 1)));
                return;
            }
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
    // No sound of its own: each screen plays the one its gadget calls for (a gadget may name one).
    if (w.sound >= 0) fe.play_sound(w.sound);
    for (std::size_t i = 0; i < widgets_.size(); ++i)
        if (&widgets_[i] == &w) {
            focus_before_press_ = focus_gadget_;
            focus_gadget_ = static_cast<int>(i);  // a pressed gadget takes the focus
        }
    // A press that opens another screen over this one leaves this screen as it was last drawn:
    // the focus ring stays where it was until this screen is on top again (the original redraws
    // a layout's focus marking only in its full redraws; gui_harness: the single player screen's
    // Load Game key).
    const std::size_t depth = fe.depth();
    ring_follow_ = false;
    on_press(fe, w);
    if (depth != 0 && fe.depth() > depth && !ring_follow_) {
        ring_hold_ = focus_before_press_;
        hold_depth_ = depth;
    }
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
        pointer_x_ = e.x;
        pointer_y_ = e.y;
        // A text field shows the focus marking (lit glyphs) it had at the last full redraw: the
        // marking of a focus that moved away goes with the redraw after the next press.
        lit_field_ = focus_gadget_;
        const int i = hit(e.x, e.y);
        if (i < 0) return;
        Widget& w = widgets_[static_cast<std::size_t>(i)];
        if (w.kind == WidgetKind::ListBox && w.text_list) {
            // A press over the rows (x .. x + w - 1, y + 2 .. y + h - 4) holds the list; a second
            // left press within the double-click time acts on it (the screen's handler).
            const int y0 = oy_ + w.y + 2, y1 = oy_ + w.y + w.h - 4;
            if (e.y < y0 || e.y > y1) return;
            // (the platform's double click: within 500 ms and 2 pixels of the first press)
            const bool dbl = e.button == 1 && last_click_row_ == i && e.time_ms - last_click_ms_ < 500 && std::abs(e.x - last_click_x_) <= 2 &&
                             std::abs(e.y - last_click_y_) <= 2;
            last_click_ms_ = e.time_ms;
            last_click_row_ = dbl ? -1 : i;
            last_click_x_ = e.x;
            last_click_y_ = e.y;
            if (dbl) {
                if (!w.items.empty()) on_press(fe, w);
                return;
            }
            focus_gadget_ = i;
            focus_ = -1;  // a text field stops taking the keys (the list is the active gadget now)
            list_held_ = i;
            list_pointer(fe, i);
            return;
        }
        if (w.kind == WidgetKind::ScrollBar && w.list_slider) {
            // Held (the focus stays where it was); a press on the knob drags it.
            slider_held_ = i;
            const int kx0 = ox_ + w.x + 1, kx1 = ox_ + w.x - 1 + w.w, ky0 = oy_ + w.y + w.knob_pos + 2, ky1 = ky0 + w.knob;
            slider_drag_ = e.x >= kx0 && e.x <= kx1 && e.y >= ky0 && e.y <= ky1;
            slider_anchor_ = e.y;
            slider_anchor_knob_ = w.knob_pos;
            return;
        }
        switch (w.kind) {
        case WidgetKind::Button:
            if (!w.grayed) {
                held_ = i;
                focus_ = -1;  // the button is the active gadget now: a text field stops taking the keys
            }
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
        case WidgetKind::TextField:
            focus_ = i;
            focus_gadget_ = i;
            break;
        default: on_press(fe, w); break;
        }
        return;
    }
    case InputEvent::Type::MouseMove:
        pointer_x_ = e.x;
        pointer_y_ = e.y;
        if (list_held_ >= 0) list_pointer(fe, list_held_);
        if (drag_ >= 0) {
            scroll_to(widgets_[static_cast<std::size_t>(drag_)], e.x, e.y);
            on_press(fe, widgets_[static_cast<std::size_t>(drag_)]);
        }
        return;
    case InputEvent::Type::MouseUp: {
        pointer_x_ = e.x;
        pointer_y_ = e.y;
        if (list_held_ >= 0) {
            // The pass that sees the button up still follows the pointer, then lets go.
            list_pointer(fe, list_held_);
            list_held_ = -1;
        }
        if (slider_held_ >= 0) {
            slider_pointer(fe, slider_held_);
            slider_held_ = -1;
            slider_drag_ = false;
        }
        drag_ = -1;
        const int was = held_;
        held_ = -1;
        if (was >= 0 && hit(e.x, e.y) == was) press(fe, widgets_[static_cast<std::size_t>(was)]);
        return;
    }
    case InputEvent::Type::KeyDown:
    case InputEvent::Type::Text:
        lit_field_ = -2;
        for (const int code : menu_key_codes(e)) key_code(fe, code, e.shift, e.alt);
        return;
    default: return;
    }
}

// ------------------------------------------------------------------ keys
// (spec: docs/RE_MEMORY.md, "Client", "Menu keys": the window procedure's key path 0x4C1D50 /
// WM_CHAR into the key queue 0x4C1B20, the interface pass 0x4A9FD0 with its special keys 0x4A9B90,
// the focus moves 0x4A7960, the buttons' 0x4A6AE0, labels' 0x4A4440 and fields' 0x4A7290 /
// 0x4AB720 key handling.)

std::vector<int> menu_key_codes(const InputEvent& e) {
    std::vector<int> c;
    const int k = e.key;
    if (e.type == InputEvent::Type::Text) {
        if (k >= 0x20 && k < 0x80) c.push_back(k);
        return c;
    }
    if (e.type != InputEvent::Type::KeyDown) return c;
    // Keys the window procedure queues from the key itself, whatever the modifiers.
    switch (k) {
    case keys::kLeft: return {0xF4};
    case keys::kUp: return {0xF5};
    case keys::kRight: return {0xF6};
    case keys::kDown: return {0xF7};
    case keys::kHome: return {0xF0};
    case keys::kEnd: return {0xF1};
    case keys::kDelete: return {0xEF};
    case keys::kPause: return {0xF8};
    default: break;
    }
    if (k >= keys::kF1 && k < keys::kF1 + 12) return {k - keys::kF1 + (e.ctrl ? 0xCE : 0xE2)};
    if (e.alt && !e.ctrl) {
        // A system key (Alt held): letters lower case, digits and signs as they are; the typed
        // character (a system character) never reaches the game.
        if (k >= 'a' && k <= 'z') return {k};
        if (k > 0x20 && k < 0x7F) return {k};
        return c;
    }
    // The typed character (WM_CHAR).
    switch (k) {
    case keys::kEnter: return {e.ctrl ? 0x0A : 0x0D};
    case keys::kEscape: return {0x1B};
    case keys::kTab: return e.ctrl ? c : std::vector<int>{0x09};
    case keys::kBackspace: return {e.ctrl ? 0x7F : 0x08};
    default: break;
    }
    if (k < 0x20 || k >= 0x7F) return c;
    if (e.ctrl) {
        // The key itself (letters 0xAA.., digits 0xC4..), then a letter's control character.
        if (k >= 'a' && k <= 'z') return {k - 'a' + 0xAA, k - 'a' + 1};
        if (k >= '0' && k <= '9') return {k - '0' + 0xC4};
        if (k == ' ') return {0x20};
        if (k == '[') return {k, 0x1B};
        if (k == '\\') return {k, 0x1C};
        if (k == ']') return {k, 0x1D};
        return {k};
    }
    if (e.text_follows) return c;  // the Text event brings the character
    if (!e.shift) return {k};
    if (k >= 'a' && k <= 'z') return {k - 'a' + 'A'};
    static const char plain[] = "1234567890-=[]\\;',./`", shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
    for (int i = 0; plain[i]; ++i)
        if (plain[i] == k) return {static_cast<unsigned char>(shifted[i])};
    return {k};
}

bool edit_line(std::string& text, int& cursor, int max_chars, int width_limit, int attribs, const Font* font, int code) {
    const int len = static_cast<int>(text.size());
    cursor = std::clamp(cursor, 0, len);
    switch (code) {
    case 0x0D:
    case 0x1B: return true;
    case 0x08:
        if (cursor != 0) text.erase(static_cast<std::size_t>(--cursor), 1);
        return false;
    case 0xEF:
        if (len != 0 && cursor < len) text.erase(static_cast<std::size_t>(cursor), 1);
        return false;
    case 0xF0: cursor = 0; return false;
    case 0xF1: cursor = len; return false;
    case 0xF4:
        if (cursor != 0) --cursor;
        return false;
    case 0xF6:
        if (cursor < len) ++cursor;
        return false;
    default: break;
    }
    if (code < 0x20 || code >= 0x80 || len == max_chars) return false;
    if ((attribs & 2) && !std::isalnum(code) && code != '_' && code != ' ' && code != '\'') return false;
    if (font) {
        const char ch = static_cast<char>(code);
        if (font->text_width(text) + font->text_width(std::string_view(&ch, 1)) > width_limit) return false;
    }
    text.insert(static_cast<std::size_t>(cursor), 1, static_cast<char>(code));
    ++cursor;
    return false;
}

int LayoutScreen::gadget_kind(int i) const {
    if (i < 0 || i >= static_cast<int>(widgets_.size())) return 0;
    switch (widgets_[static_cast<std::size_t>(i)].kind) {
    case WidgetKind::Button: return 1;
    case WidgetKind::ListBox: return 2;
    case WidgetKind::TextField: return 3;
    case WidgetKind::ScrollBar: return 4;
    case WidgetKind::Label: return 5;
    case WidgetKind::Surface: return 6;
    default: return 0;
    }
}
std::string* LayoutScreen::field_text(int i) {
    if (gadget_kind(i) != 3) return nullptr;
    return &widgets_[static_cast<std::size_t>(i)].text;
}
int* LayoutScreen::field_cursor(int i) {
    if (gadget_kind(i) != 3) return nullptr;
    return &widgets_[static_cast<std::size_t>(i)].cursor;
}
int LayoutScreen::field_limit(int i) const {
    const Widget& w = widgets_[static_cast<std::size_t>(i)];
    return w.max_chars > 0 ? w.max_chars : 63;
}

void LayoutScreen::activate_field(int i) {
    focus_ = gadget_kind(i) == 3 ? i : -1;
    if (focus_ >= 0)
        if (std::string* t = field_text(i))
            if (int* c = field_cursor(i)) *c = static_cast<int>(t->size());
}

void LayoutScreen::list_key(FrontEnd& fe, int i, int dir) {
    Widget& l = widgets_[static_cast<std::size_t>(i)];
    if (!l.text_list) {
        // A plain list box: the selection one row on, kept in the rows.
        const int count = static_cast<int>(l.items.size());
        if (count == 0) return;
        const int row = std::clamp(l.selected + dir, 0, count - 1);
        if (row != l.selected) {
            l.selected = row;
            on_pick(fe, l, row, false);
        }
        return;
    }
    // Up / Down (0x4A9830 / 0x4A99C0): a shown selection moves one row, the list scrolling with
    // it; a selection out of view is first brought into view. The list's callback follows either
    // way.
    const int fh = layout_ ? layout_->font_height : 14;
    const int rows = visible_rows(l, fh), count = static_cast<int>(l.items.size());
    const int sel = l.selected, top = l.top;
    auto marked = [&](int r) { return r >= 0 && r < count && l.items[static_cast<std::size_t>(r)].compare(0, 2, "&G") == 0; };
    if (sel < top + rows && top <= sel) {
        if (dir < 0 && count != 0 && sel != 0) {
            l.selected = sel - 1;
            if (l.selected < top) l.top = top - 1;
            if (marked(l.selected)) l.selected = sel;
            list_moved(l);
        } else if (dir > 0 && count != 0 && sel != l.max_top - 1 + rows) {
            l.selected = sel + 1;
            if (top + rows - 1 < l.selected) l.top = top + 1;
            if (count - 1 <= l.selected) l.selected = count - 1;
            if (marked(l.selected)) l.selected = sel;
            list_moved(l);
        }
    } else if (count != 0) {
        select_list_row(l, sel);
    }
    on_list_select(fe, l);
}

void LayoutScreen::key_press(FrontEnd& fe, int i) {
    Widget& w = widgets_[static_cast<std::size_t>(i)];
    switch (gadget_kind(i)) {
    case 2:
        focus_gadget_ = i;
        if (w.text_list) on_press(fe, w);
        else if (w.selected >= 0) on_pick(fe, w, w.selected, true);
        return;
    case 3:
        focus_gadget_ = i;
        on_text(fe, w);
        return;
    default:
        press_button_ = 1;
        key_press_ = true;
        press(fe, w);
        key_press_ = false;
        return;
    }
}

void LayoutScreen::move_focus(int dir) {
    // 0x4A7960. Gadgets are ordered by y * 5000 + x (left, right, Tab) or, for up and down, by
    // column * 5000 + y, a gadget's column being the x of the first earlier gadget (in description
    // order) within 9 pixels of its own, else its own x (the search stops at a column of 0). Left
    // and up take the gadget before the focus in that order (from the first: the last), right,
    // down and Tab the one after (from the last: the first); ties go to the first in description
    // order. Candidates: shown gadgets without attribute 0x400 that are buttons (not grayed),
    // lists (not attribute 0x100), text fields, screen-drawn areas and sliders at least as wide
    // as tall (not disabled). Moving clears the field taking the keys; a text field
    // given the focus takes them, its insertion point at the end.
    const int focus = focus_gadget_;
    if (focus < 0 || focus >= static_cast<int>(widgets_.size())) return;
    const int n = static_cast<int>(widgets_.size());
    std::vector<int> column(static_cast<std::size_t>(n) + 1, 0);
    for (int k = 1; k < n; ++k) {
        const int x = widgets_[static_cast<std::size_t>(k)].x;
        int v = x;
        for (int j = 1; j < k && column[static_cast<std::size_t>(j)] != 0; ++j)
            if (column[static_cast<std::size_t>(j)] - x < 10 && column[static_cast<std::size_t>(j)] - x > -10) {
                v = column[static_cast<std::size_t>(j)];
                break;
            }
        column[static_cast<std::size_t>(k)] = v;
    }
    constexpr long long kWrap = 400000000LL;
    const Widget& f = widgets_[static_cast<std::size_t>(focus)];
    const bool rows = dir == 0 || dir == 1, back = dir == 0 || dir == 2;
    const long long here = rows ? f.x + 5000LL * f.y : f.y + 5000LL * column[static_cast<std::size_t>(focus)];
    long long best = back ? here - kWrap : here + kWrap;
    int chosen = focus;
    for (int k = 1; k < n; ++k) {
        const Widget& w = widgets_[static_cast<std::size_t>(k)];
        const int kind = gadget_kind(k);
        if (!w.active || (w.attribs & 0x400)) continue;
        if (kind == 1 && w.grayed) continue;
        if (kind == 4 && (w.grayed || w.w < w.h)) continue;
        if (kind != 1 && kind != 2 && kind != 3 && kind != 4 && kind != 6) continue;
        if (kind == 2 && (w.attribs & 0x100)) continue;
        long long v = rows ? w.x + 5000LL * w.y : w.y + 5000LL * column[static_cast<std::size_t>(k)];
        if (back) {
            if (here <= v) v -= kWrap;
            if (best < v) {
                best = v;
                chosen = k;
            }
        } else {
            if (v <= here) v += kWrap;
            if (v < best) {
                best = v;
                chosen = k;
            }
        }
    }
    focus_gadget_ = chosen;
    activate_field(chosen);
}

void LayoutScreen::key_code(FrontEnd& fe, int code, bool shift, bool alt) {
    if (!layout_) return;
    const int n = static_cast<int>(widgets_.size());
    const int focus = focus_gadget_ >= 0 && focus_gadget_ < n ? focus_gadget_ : -1;
    const int fkind = focus >= 0 ? gadget_kind(focus) : 0;
    const bool field_keys = focus_ >= 0 && gadget_kind(focus_) == 3;
    auto index_named = [&](const std::string& name) -> int {
        if (name.empty()) return -1;
        for (int i = 1; i < n; ++i)
            if (same_name(widgets_[static_cast<std::size_t>(i)].name, name)) return i;
        return -1;
    };
    // A gadget pressed by the key: it takes the focus (a text field also the keys, else no field
    // has them), then the screen's handler.
    auto pressed = [&](int i) {
        activate_field(i);
        presses_.push_back(widgets_[static_cast<std::size_t>(i)].name);
        key_press(fe, i);
    };
    // The focused gadget pressed (Space; Enter without a default): buttons (not grayed), lists and
    // screen-drawn areas; a switch steps on.
    auto press_focused = [&]() -> bool {
        if (focus < 0 || fkind == 3) return false;
        const Widget& w = widgets_[static_cast<std::size_t>(focus)];
        if ((fkind != 1 && fkind != 2 && fkind != 6) || !w.active || (fkind == 1 && w.grayed)) return false;
        pressed(focus);
        return true;
    };
    // ---- the special keys (0x4A9B90)
    switch (code) {
    case 0x09:
        move_focus(shift ? 0 : 1);
        return;
    case 0x0D: {
        if (field_keys) break;  // the field takes it
        const int d = index_named(enter_default_.empty() ? layout_->enter_default : enter_default_);
        if (d < 0 || !widgets_[static_cast<std::size_t>(d)].active || (gadget_kind(d) == 1 && widgets_[static_cast<std::size_t>(d)].grayed)) {
            if (press_focused()) return;
            break;
        }
        pressed(d);
        return;
    }
    case 0x1B: {
        const int d = index_named(layout_->escape_default);
        if (d < 0 || !widgets_[static_cast<std::size_t>(d)].active) break;
        pressed(d);
        return;
    }
    case 0x20:
        if (press_focused()) return;
        break;
    case 0xF4:
    case 0xF6:
        if (fkind == 3) break;
        if (fkind == 4 && widgets_[static_cast<std::size_t>(focus)].w > widgets_[static_cast<std::size_t>(focus)].h) {
            // A slider at least as wide as tall: its value one down / up (0x4A96D0 / 0x4A9780).
            Widget& s = widgets_[static_cast<std::size_t>(focus)];
            if (s.knob_value != s.position) s.knob_pos = slider_knob(s, s.position);
            s.position = std::clamp(s.position + (code == 0xF4 ? -1 : 1), 0, std::max(0, s.range));
            s.knob_pos = slider_knob(s, s.position);
            s.knob_value = s.position;
            on_press(fe, s);
            return;
        }
        if (focus >= 0) move_focus(code == 0xF4 ? 0 : 1);
        return;
    case 0xF5:
    case 0xF7:
        if (fkind == 2) {
            list_key(fe, focus, code == 0xF5 ? -1 : 1);
            return;
        }
        if (focus >= 0) move_focus(code == 0xF5 ? 2 : 3);
        return;
    default: break;
    }
    // ---- the gadgets in description order; the first that takes the key is pressed
    for (int i = 1; i < n; ++i) {
        Widget& w = widgets_[static_cast<std::size_t>(i)];
        if (!w.active) continue;
        const int kind = gadget_kind(i);
        if (kind == 1 || kind == 5) {
            // A quick key (either case), not while a text field takes the keys unless Alt is held.
            if (kind == 1 && w.grayed) continue;
            if (field_keys && focus_ != i && !alt) continue;
            if (!w.quickkey || (code != w.quickkey && code != std::toupper(w.quickkey))) continue;
            if (kind == 1) {
                pressed(i);
                return;
            }
            // A label's key acts on the gadget it names: a button steps on and is pressed;
            // another shown gadget takes the focus (a field the keys) and is pressed; no such
            // gadget: the label itself is pressed.
            const int t = index_named(w.link);
            if (t < 0) {
                pressed(i);
                return;
            }
            Widget& tw = widgets_[static_cast<std::size_t>(t)];
            const int tk = gadget_kind(t);
            if (!tw.active || (tk == 1 && tw.grayed) || (tk == 4 && tw.grayed)) continue;
            pressed(t);
            return;
        }
        if (kind == 3 && focus_ == i) {
            // The field taking the keys (nothing while Alt is held): Enter ends it, Escape empties
            // and ends it; both press it.
            std::string* t = field_text(i);
            int* c = field_cursor(i);
            if (!t || !c || alt || code == 0) continue;
            const Font* font = fe.menu_content().font;
            if (!edit_line(*t, *c, field_limit(i), w.w - 4, w.attribs, font, code)) continue;
            if (code == 0x1B) {
                t->clear();
                *c = 0;
            }
            focus_ = -1;
            pressed(i);
            return;
        }
    }
}

void LayoutScreen::draw_widget(FrontEnd& fe, render::Image8& out, const Widget& w, bool held) {
    const MenuContent& mc = fe.menu_content();
    // Gadgets draw into the screen's own surface (the header gadget's rectangle), which is then
    // put on the frame: whatever lies outside it is cut off (a label placed right of a side
    // panel never shows).
    render::ClipRect clip = out.bounds();
    if (!widgets_.empty()) {
        const Widget& hd = widgets_.front();
        clip.x0 = std::max(clip.x0, ox_);
        clip.y0 = std::max(clip.y0, oy_);
        clip.x1 = std::min(clip.x1, ox_ + hd.w - 1);
        clip.y1 = std::min(clip.y1, oy_ + hd.h - 1);
        if (clip.x0 > clip.x1 || clip.y0 > clip.y1) return;
    }
    const int x = ox_ + w.x, y = oy_ + w.y;
    // Buttons with attribute 0x8000 caption in the small font (0x4A5D50).
    const Font* font = w.kind == WidgetKind::Button && (w.attribs & 0x8000) && mc.small_font ? mc.small_font : mc.font;
    // mark: the quick key to mark in the caption (0: none; only buttons that are not grayed).
    auto text_at = [&](const std::string& s, int align, int pressed, std::uint8_t fg, int mark = 0) {
        if (!font || s.empty()) return;
        // Characters are dropped from the end until the caption fits (width - 6).
        std::string t = s;
        while (!t.empty() && font->text_width(t) > w.w - 6) t.pop_back();
        const int tw = font->text_width(t);
        // The text's top row (centred by the caption font's height, also for small-font captions).
        const int top = y + ((w.h - 1) - (mc.font ? mc.font->height : font->height)) / 2 + pressed;
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
        // Pressed while held, and while its status is set (a selected page's button).
        const bool pressed = (held || w.status != 0) && !w.grayed;
        bool dim = false;  // a grayed button goes through shade row 12 (-20) after its caption
        if (w.frames && !w.frames->empty()) {
            const int n = static_cast<int>(w.frames->size());
            int f = 0;
            if (w.grayed) {
                // 0x4A5F40: attribute 0x100 the last frame; a switch its stage, dimmed; a plain
                // button frame min(state + 2, last), dimmed unless attribute 0x80 (0x1800: frame 0).
                if (w.attribs & 0x100) f = n - 1;
                else if (w.stages > 0) f = std::min(w.stage, n - 1), dim = true;
                else if (w.attribs & 0x1800) f = 0, dim = true;
                else f = std::min(std::max(w.status, 0) + 2, n - 1), dim = !(w.attribs & 0x80);
            } else if (pressed) f = w.stages > 0 ? std::max(0, n - 2) : std::min(1, n - 1);
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
        if (dim && mc.shade.size() >= 0x2000)
            for (int yy = std::max({y, clip.y0, 0}); yy <= std::min({y + w.h - 1, clip.y1, out.height - 1}); ++yy)
                for (int xx = std::max({x, clip.x0, 0}); xx <= std::min({x + w.w - 1, clip.x1, out.width - 1}); ++xx) {
                    std::uint8_t& p = out.row(yy)[xx];
                    p = mc.shade[static_cast<std::size_t>(12 * 256 + static_cast<std::int8_t>(p))];
                }
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
        // A label more than two lines high is word-wrapped to its width (0x4A51D0); otherwise one
        // line, stopping at the first character that does not fit the width (0x4A50E0).
        if (lf->height * 2 < w.h - 1) draw_wrapped(out, clip, *lf, w.text, tx, y + lf->raise, w.w, w.h);
        else draw_line(out, clip, *lf, w.text, tx, y + lf->raise, w.w);
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
        if (w.text_list) {
            draw_text_list(mc, out, clip, w, x, y);
            break;
        }
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
        // The original's text field (0x4A4D70): attribute 1 fills the field with interface colour
        // 0 (else the screen's picture shows); the text from (x, y + 3), the glyphs that fit w - 1;
        // the field with the focus has its glyphs lit through light level 30 (0x4A16F0 sets its
        // colour word); the field taking keys shows the insertion point, a line in interface colour
        // 9 from the text's top down the font's height.
        if (!font) break;
        if (w.attribs & 1) fill_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, colour(0));
        const int index = static_cast<int>(&w - widgets_.data());
        const bool lit = (lit_field_ == -2 ? focus_gadget_ : lit_field_) == index && mc.light.size() >= 0x2000;
        int cx = x, left = w.w - 1;
        for (const char ch : w.text) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20) continue;
            const Glyph& g = font->glyphs[c];
            if (left < g.width) break;
            if (c != ' ')
                for (int r = 0; r < g.rows; ++r)
                    for (int k = 0; k < g.width; ++k) {
                        const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                        if (at >= g.ink.size() || !g.ink[at]) continue;
                        std::uint8_t v = g.colours.empty() ? mc.text_colour : g.colours[at];
                        if (lit) v = mc.light[30 * 256 + v];
                        put_pixel(out, clip, cx + g.left + k, y + 3 + g.top + r, v);
                    }
            left -= g.width;
            if (left < 0) break;
            cx += g.width;
        }
        if (focus_ == index) {
            const int at = std::clamp(w.cursor, 0, static_cast<int>(w.text.size()));
            const int px = x + font->text_width(std::string_view(w.text).substr(0, static_cast<std::size_t>(at)));
            fill_rect(out, clip, px, y + 3, px, y + 3 + font->height, colour(9));
        }
        break;
    }
    default: break;
    }
}

std::vector<std::pair<std::string, std::string>> LayoutScreen::describe() const {
    auto name = [&](int i) { return i >= 0 && i < static_cast<int>(widgets_.size()) ? widgets_[static_cast<std::size_t>(i)].name : std::string("-"); };
    std::vector<std::pair<std::string, std::string>> d = {{"layout", layout_ ? layout_->name : std::string("?")}, {"focus", name(focus_gadget_)}, {"field", name(focus_)}};
    LayoutScreen* self = const_cast<LayoutScreen*>(this);
    for (int i = 1; i < static_cast<int>(widgets_.size()); ++i) {
        const Widget& w = widgets_[static_cast<std::size_t>(i)];
        if (const std::string* t = self->field_text(i)) d.push_back({"text:" + w.name, *t});
        if (w.stages > 0) d.push_back({"stage:" + w.name, std::to_string(w.stage)});
    }
    return d;
}

std::vector<std::string> LayoutScreen::take_presses() {
    std::vector<std::string> p;
    p.swap(presses_);
    return p;
}

void LayoutScreen::place(int fw, int fh) {
    placed_ = true;
    if (!layout_ || widgets_.empty()) return;
    const Widget& h = widgets_.front();
    int x = layout_->x, y = layout_->y;
    if (open_centred_ != 0) x = open_centred_;
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
        for (std::size_t i = 1; i < widgets_.size() && i < focus_limit_; ++i) {
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

void LayoutScreen::reassign_quick_keys(const std::vector<Widget>& before) {
    std::vector<int> used;
    for (const Widget& w : before)
        if (w.quickkey) used.push_back(w.quickkey);
    for (Widget& w : widgets_) {
        if (w.kind != WidgetKind::Button || w.stages > 0 || (w.attribs & 0x1800) || w.text.empty()) continue;
        w.quickkey = w.quickkey_char = 0;
        for (const char ch : w.text) {
            if (ch == ' ') continue;
            const int lc = std::tolower(static_cast<unsigned char>(ch));
            if (std::find(used.begin(), used.end(), lc) != used.end()) continue;
            w.quickkey = lc;
            w.quickkey_char = static_cast<unsigned char>(ch);
            used.push_back(lc);
            break;
        }
    }
}

void LayoutScreen::set_caption(Widget& w, const std::string& text) {
    w.text = text;
    if (w.kind != WidgetKind::Button || (w.attribs & 0x10000)) return;
    w.quickkey = w.quickkey_char = 0;
    if (w.stages > 0) return;
    for (const char ch : text) {
        if (ch == ' ') continue;
        const int lc = std::tolower(static_cast<unsigned char>(ch));
        bool used = false;
        for (const Widget& o : widgets_) used |= &o != &w && o.quickkey == lc;
        if (used) continue;
        w.quickkey = lc;
        w.quickkey_char = static_cast<unsigned char>(ch);
        break;
    }
}

// ------------------------------------------------------------------ text lists
// (spec: docs/RE_MEMORY.md, "Client", "Lists": the original's 0x4A32A0 set, 0x4A2E40 select,
// 0x4A2BE0 follow, 0x4A3EF0 knob, 0x4A3780 pointer, 0x4A4170 slider, 0x4A9830 / 0x4A99C0 keys.)

Widget* LayoutScreen::slider_of(const Widget& list) {
    for (Widget& s : widgets_)
        if (s.kind == WidgetKind::ScrollBar && s.assoc == list.assoc && s.slider_step == 0) return &s;
    return nullptr;
}

Widget* LayoutScreen::list_of(const Widget& w) {
    for (Widget& l : widgets_)
        if (l.kind == WidgetKind::ListBox && l.assoc == w.assoc && &l != &w) return &l;
    return nullptr;
}

void LayoutScreen::set_list(Widget& list, std::vector<std::string> items) {
    const int fh = layout_ ? layout_->font_height : 14;
    list.items = std::move(items);
    list.text_list = true;
    if (list.row_h <= fh + 1) list.row_h = fh + 1;
    list.top = 0;
    list.selected = 0;
    const int count = static_cast<int>(list.items.size());
    // The largest first row: the rows that fit the height whole, counted back from the last.
    list.max_top = count - 1;
    int left = list.h;
    for (int k = count - 1; k >= 0; --k) {
        left -= list.row_h;
        if (left < 0) break;
        list.max_top = k;
    }
    if (!list.active) return;
    Widget* s = slider_of(list);
    if (!s) return;
    const bool over = left < 0;
    s->active = over;
    s->list_slider = true;
    for (Widget& a : widgets_)
        if (a.slider_step != 0 && a.link == s->name) a.active = over;
    if (!over) return;
    // The knob: its length the shown share of the rows (at least 10), its travel the rest.
    const int rows = (list.h - 2) / std::max(list.row_h, fh + 1);
    int knob = static_cast<std::int16_t>(conv(static_cast<double>(rows) / count * (s->h - 3)));
    if (knob < 10) knob = 10;
    s->knob = knob;
    s->range = count <= rows ? 0 : s->h - knob - 3;
    s->knob_pos = 0;
    s->position = s->knob_value = 0;
}

void LayoutScreen::select_list_row(Widget& list, int row) {
    const int fh = layout_ ? layout_->font_height : 14;
    list.selected = row;
    if (visible_rows(list, fh) - 1 + list.top < row || row < list.top) {
        if (list.max_top != 0) list.top = row;
        if (list.max_top < list.top) list.top = list.max_top;
        if (Widget* s = slider_of(list)) {
            const int k = list.max_top == 0 ? 0 : static_cast<std::int16_t>(conv(static_cast<double>(s->range) * list.top / list.max_top));
            s->knob_pos = s->position = s->knob_value = k;
        }
    }
}

void LayoutScreen::list_moved(Widget& list) {
    for (Widget& o : widgets_) {
        if (&o == &list || o.assoc != list.assoc || o.slider_step != 0) continue;
        if (o.kind == WidgetKind::ListBox) {
            o.top = list.top;
            o.selected = list.selected;
        } else if (o.kind == WidgetKind::TextField) {
            if ((list.attribs & 8) && list.selected >= 0 && list.selected < static_cast<int>(list.items.size()))
                o.text = list.items[static_cast<std::size_t>(list.selected)];
        } else if (o.kind == WidgetKind::ScrollBar && list.items.size() > 1) {
            const int k = list.max_top == 0 ? 0 : static_cast<std::int16_t>(conv(static_cast<double>(list.top) * o.range / list.max_top));
            o.knob_pos = o.position = o.knob_value = k;
        }
    }
}

// One pass of a held list (0x4A3780): over the rows the pointer's row is selected; above or below
// them the list scrolls one row every 2 ticks (above, between the steps, the selection goes to the
// first row).
void LayoutScreen::list_pointer(FrontEnd& fe, int index) {
    Widget& w = widgets_[static_cast<std::size_t>(index)];
    const int fh = layout_ ? layout_->font_height : 14;
    const int rh = w.row_h ? w.row_h : fh + 1;
    const int rows = (w.h - 2) / rh;
    const int x0 = ox_ + w.x, x1 = x0 + w.w - 1, y0 = oy_ + w.y + 2, y1 = oy_ + w.y + w.h - 4;
    const int px = pointer_x_, py = pointer_y_;
    const int count = static_cast<int>(w.items.size());
    const int before = w.selected;
    const std::uint32_t tick = fe.now_ms() * 30u / 1000u;
    auto marked = [&](int i) { return i >= 0 && i < count && w.items[static_cast<std::size_t>(i)].compare(0, 2, "&G") == 0; };
    if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
        focus_gadget_ = index;
        int sel = (py - y0) / rh + w.top;
        if (sel < 0) {
            w.selected = before;
        } else {
            if (rows - 1 < sel - w.top) sel = rows - 1 + w.top;
            if (count - 1 <= sel) sel = count - 1;
            if (sel < 0) sel = 0;
            w.selected = sel;
            if ((w.attribs & 0x200) && marked(sel)) w.selected = before;
            for (Widget& o : widgets_)
                if (&o != &w && o.kind == WidgetKind::ListBox && o.assoc == w.assoc)
                    o.selected = std::min(w.selected, static_cast<int>(o.items.size()) - 1);
        }
        if (before != w.selected) on_list_select(fe, w);
        return;
    }
    if (py >= y0) {
        if (py <= y1 || w.max_top <= w.top || tick <= list_scroll_ms_) return;
        w.top += 1;
        list_scroll_ms_ = tick + 2;
        w.selected = rows - 1 + w.top;
        if (marked(w.selected)) w.selected = before;
    } else {
        if (w.top < 1 || tick <= list_scroll_ms_) {
            if (w.selected < 1) return;
            w.selected = 0;
        } else {
            list_scroll_ms_ = tick + 2;
            if (w.top < w.selected) w.selected = w.top;
            w.selected -= 1;
            w.top -= 1;
            if (marked(std::max(w.selected, 0))) w.selected = before;
        }
    }
    list_moved(w);
}

// One pass of a held list slider (0x4A4170): dragging the knob moves it with the pointer, else it
// steps one pixel toward the pointer each pass; the list's first row follows the knob.
void LayoutScreen::slider_pointer(FrontEnd&, int index) {
    Widget& s = widgets_[static_cast<std::size_t>(index)];
    const int y0 = oy_ + s.y;
    const int before = s.knob_pos;
    int k = before;
    if (slider_drag_) {
        k = slider_anchor_knob_ + (pointer_y_ - slider_anchor_);
    } else {
        const int top = y0 + s.knob_pos + 2, bottom = top + s.knob;
        if (pointer_y_ < top) k = before - 1;
        else if (pointer_y_ > bottom) k = before + 1;
    }
    if (s.range - 1 < k) k = s.range - 1;
    if (k < 0) k = 0;
    if (k == before) return;
    s.knob_pos = s.position = s.knob_value = k;
    if (Widget* l = list_of(s)) {
        if (l->row_h != 0) {
            const int count = static_cast<int>(l->items.size());
            l->top = static_cast<std::int16_t>(conv(static_cast<double>(count - l->h / l->row_h) * k / (s.range - 1)));
        }
    }
}

void LayoutScreen::frame(FrontEnd& fe, std::uint32_t) {
    if (list_held_ >= 0) list_pointer(fe, list_held_);
    if (slider_held_ >= 0) slider_pointer(fe, slider_held_);
}

void LayoutScreen::draw_focus_ring(FrontEnd& fe, render::Image8& out) {
    // Six shaded outlines around the focused gadget, one to six pixels out, light levels 31,
    // 28, 24, 19, 13, 6 (the four sides drawn as separate lines, so corners are lit twice).
    const std::vector<std::uint8_t>& light = fe.menu_content().light;
    if (ring_hold_ >= -1 && hold_depth_ != 0) {
        if (fe.depth() <= hold_depth_) hold_depth_ = 0;  // on top again: the ring follows the focus
    }
    const int ring = hold_depth_ != 0 ? ring_hold_ : focus_gadget_;
    if (!focus_ring_ || ring < 0 || ring >= static_cast<int>(widgets_.size()) || light.size() < 0x2000) return;
    const Widget& w = widgets_[static_cast<std::size_t>(ring)];
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
    if (layout_ && (background_ || layout_->background)) {
        const render::Sprite8& bg = background_ ? *background_ : *layout_->background;
        if (background_in_box_) {
            // The picture's top-left part the size of the screen's box, at the box.
            const Widget& h = widgets_.front();
            const render::ClipRect c{std::max(ox_, 0), std::max(oy_, 0), std::min(ox_ + h.w - 1, out.width - 1), std::min(oy_ + h.h - 1, out.height - 1)};
            render::blit(out, c, bg, ox_ + bg.x_offset, oy_ + bg.y_offset);
        } else {
            const bool full = bg.width >= out.width && bg.height >= out.height;
            render::blit(out, out.bounds(), bg, (full ? 0 : ox_) + bg.x_offset, (full ? 0 : oy_) + bg.y_offset);
        }
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
