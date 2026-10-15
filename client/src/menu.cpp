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

void LayoutScreen::press(FrontEnd& fe, Widget& w) {
    if (w.grayed) return;
    if (w.stages > 0) {
        // A switch steps forward (left click) or back (right click) through its stages.
        w.stage = press_button_ == 3 ? (w.stage + w.stages - 1) % w.stages : (w.stage + 1) % w.stages;
    }
    fe.play_sound(w.sound >= 0 ? w.sound : fe.menu_content().click_sound);
    on_press(fe, w);
}

void LayoutScreen::scroll_to(Widget& bar, int x, int y) {
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
    auto text_at = [&](const std::string& s, int align, int pressed, std::uint8_t fg) {
        if (!font || s.empty()) return;
        // Characters are dropped from the end until the caption fits (width - 6).
        std::string t = s;
        while (!t.empty() && font->text_width(t) > w.w - 6) t.pop_back();
        const int tw = font->text_width(t);
        const int ty = y + ((w.h - 1) - font->height) / 2 + pressed + font->raise;
        int tx = x + 3 + pressed;
        if (align & 4) tx = std::max(x, x + w.w - tw - 3);
        else if (align & 2) tx = x + (w.w - tw) / 2 + pressed + 1;
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
            // A framed box: light top and left, dark bottom and right (swapped while pressed).
            const std::uint8_t light = mc.text_colour, dark = mc.frame_colour, fill = mc.box_colour;
            fill_rect(out, clip, x, y, x + w.w - 1, y + w.h - 1, fill);
            fill_rect(out, clip, x, y, x + w.w - 1, y, pressed ? dark : light);
            fill_rect(out, clip, x, y, x, y + w.h - 1, pressed ? dark : light);
            fill_rect(out, clip, x, y + w.h - 1, x + w.w - 1, y + w.h - 1, pressed ? light : dark);
            fill_rect(out, clip, x + w.w - 1, y, x + w.w - 1, y + w.h - 1, pressed ? light : dark);
        }
        const std::uint8_t fg = w.grayed ? mc.text_dim : (w.fg ? colour(w.fg) : mc.text_colour);
        text_at(caption(w), w.attribs & 0x27 ? w.attribs : 2, pressed ? 1 : 0, fg);
        break;
    }
    case WidgetKind::Label:
        text_at(w.text, w.attribs, 0, w.fg ? colour(w.fg) : mc.text_colour);
        break;
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

void LayoutScreen::draw(FrontEnd& fe, render::Image8& out) {
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
    draw_over(fe, out);
}

}  // namespace client
