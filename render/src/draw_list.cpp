#include "render/draw_list.h"
#include "render/unit_look.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace render {

const std::vector<std::int16_t>& lens_offsets() {
    static const std::vector<std::int16_t> table = [] {
        // (The size and the strength 8 are the ones the battle's start asks for.)
        constexpr int w = 22, h = 22, hx = w / 2, hy = h / 2, quarter = w / 4, strength = 8;
        const double d = static_cast<double>(strength);
        std::vector<std::int16_t> t(static_cast<std::size_t>(w * h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int dx = x - hx, dy = y - hy;
                // x87 at 53 bits: the integer sum loaded, its square root, truncated.
                const double r = std::sqrt(static_cast<double>(dx * dx + dy * dy));
                std::int16_t e = 32000;
                if (static_cast<int>(r) < quarter) {
                    const double s = (static_cast<double>(hx) - r) / d;
                    const int ey = static_cast<int>(static_cast<double>(dy) / s);
                    const int ex = static_cast<int>(static_cast<double>(dx) / s);
                    e = static_cast<std::int16_t>(ex + hx + (ey + hy) * w - (y * w + x));
                }
                t[static_cast<std::size_t>(y * w + x)] = e;
            }
        return t;
    }();
    return table;
}

void apply_lens(Image8& frame, int x, int y, std::uint8_t key, const ClipRect& clip) {
    constexpr int w = 22, h = 22, n = w * h, hx = w / 2, hy = h / 2;
    const std::vector<std::int16_t>& t = lens_offsets();
    // The lens's memory: the table (2n bytes), the capture (n), the output (n).
    std::uint8_t mem[4 * n];
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<std::uint16_t>(t[static_cast<std::size_t>(i)]);
        mem[2 * i] = static_cast<std::uint8_t>(v);
        mem[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    std::uint8_t* const capture = mem + 2 * n;
    std::uint8_t* const output = capture + n;
    // The capture: rows of the frame's memory (row pitch = width), unclipped. (Rows outside the
    // frame are never reached from a point inside the view: 0.)
    const long long size = static_cast<long long>(frame.width) * frame.height;
    for (int r = 0; r < h; ++r)
        for (int k = 0; k < w; ++k) {
            const long long a = static_cast<long long>(y - hy + r) * frame.width + (x - hx + k);
            capture[r * w + k] = a >= 0 && a < size ? frame.pixels[static_cast<std::size_t>(a)] : 0;
        }
    for (int i = 0; i < n; ++i) {
        const int e = t[static_cast<std::size_t>(i)];
        const int at = 2 * n + i + e;  // (in [0, 4n) for every entry of the table)
        output[i] = e == 32000 ? key : at >= 0 && at < 4 * n ? mem[at] : key;
    }
    const int left = x - hx, top = y - hy;
    const int x0 = std::max({clip.x0, left, 0}), x1 = std::min({clip.x1, left + w - 1, frame.width - 1});
    const int y0 = std::max({clip.y0, top, 0}), y1 = std::min({clip.y1, top + h - 1, frame.height - 1});
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            const std::uint8_t v = output[(py - top) * w + (px - left)];
            if (v != key) frame.row(py)[px] = v;
        }
}

bool needs_frame_reads(const DrawList& list) {
    if (list.back) return true;
    for (const DrawCmd& c : list.cmds)
        if (c.op == DrawOp::Lens) return true;
    return false;
}

Image8 execute(const DrawList& list, const BlendTable& blend) {
    Image8 frame = list.back && list.back->width == list.width && list.back->height == list.height ? *list.back
                                                                                                    : Image8(list.width, list.height, list.clear_index);
    for (const DrawCmd& c : list.cmds) {
        switch (c.op) {
        case DrawOp::Lens:
            apply_lens(frame, c.x, c.y, c.colour, c.clip);
            break;
        case DrawOp::Blit:
            if (c.sprite) blit(frame, c.clip, *c.sprite, c.x, c.y);
            break;
        case DrawOp::BlitBlend:
            if (c.sprite) blit_blend(frame, c.clip, *c.sprite, c.x, c.y, blend);
            break;
        case DrawOp::Line:
            draw_line(frame, c.clip, c.x, c.y, c.x1, c.y1, c.colour);
            break;
        case DrawOp::BlitLight:
            if (c.sprite && list.light) blit_blend(frame, c.clip, *c.sprite, c.x, c.y, *list.light);
            break;
        case DrawOp::BlitRemap:
            if (c.sprite && list.remap) blit_blend(frame, c.clip, *c.sprite, c.x, c.y, *list.remap);
            break;
        case DrawOp::BlitTable:
            if (c.sprite && c.table && c.table->size() >= 65536) blit_blend(frame, c.clip, *c.sprite, c.x, c.y, *c.table);
            break;
        }
    }
    return frame;
}

BlendTable light_blend_table(const std::vector<std::uint8_t>& light32) {
    BlendTable t(65536);
    for (int s = 0; s < 256; ++s)
        for (int d = 0; d < 256; ++d) {
            const int level = s - 0x4F;
            std::uint8_t v = static_cast<std::uint8_t>(d);
            if (level >= 0 && level < 32 && static_cast<std::size_t>(level * 256 + d) < light32.size())
                v = light32[static_cast<std::size_t>(level * 256 + d)];
            t[static_cast<std::size_t>(s * 256 + d)] = v;
        }
    return t;
}

namespace {

// The original's nearest-colour search (0x4BA920 / 0x4BA9D0): colours sorted by r + g + b
// (a selection sort that swaps whenever a later entry is smaller), then among those whose
// sum lies within 40 of the target's, the first with the smallest squared RGB distance;
// with none in the window, the sorted entry where the scan stopped.
struct SortedPalette {
    int lum[256];
    std::uint8_t idx[256];
    explicit SortedPalette(const Palette& palette) {
        for (int i = 0; i < 256; ++i) {
            lum[i] = palette[static_cast<std::size_t>(i)].r + palette[static_cast<std::size_t>(i)].g + palette[static_cast<std::size_t>(i)].b;
            idx[i] = static_cast<std::uint8_t>(i);
        }
        for (int i = 0; i < 256; ++i)
            for (int j = i + 1; j < 256; ++j)
                if (lum[j] < lum[i]) {
                    std::swap(lum[i], lum[j]);
                    std::swap(idx[i], idx[j]);
                }
    }
    std::uint8_t nearest(const Palette& palette, int r, int g, int b) const {
        const int sum = r + g + b;
        int best = 1000000000, at = 0, k = 0;
        for (; k < 256; ++k) {
            if (lum[k] < sum - 0x28) continue;
            if (lum[k] > sum + 0x28) break;
            const Rgb& q = palette[idx[k]];
            const int d = (q.r - r) * (q.r - r) + (q.g - g) * (q.g - g) + (q.b - b) * (q.b - b);
            if (d < best) {
                best = d;
                at = k;
            }
        }
        if (best == 1000000000) at = k & 0xFF;
        return idx[at];
    }
};

} // namespace

BlendTable gray_remap_table(const Palette& palette) {
    const SortedPalette sp(palette);
    std::uint8_t gray[256];
    for (int c = 0; c < 256; ++c) {
        const Rgb& p = palette[static_cast<std::size_t>(c)];
        const int g = (p.r + p.g + p.b) / 3;
        gray[c] = sp.nearest(palette, g, g, g);
    }
    BlendTable t(65536);
    for (int s = 0; s < 256; ++s)
        for (int d = 0; d < 256; ++d) t[static_cast<std::size_t>(s * 256 + d)] = gray[d];
    return t;
}

BlendTable mixing_table(const Palette& palette) {
    const SortedPalette sp(palette);
    BlendTable t(65536);
    for (int s = 0; s < 256; ++s)
        for (int d = 0; d < 256; ++d) {
            const Rgb& a = palette[static_cast<std::size_t>(s)];
            const Rgb& b = palette[static_cast<std::size_t>(d)];
            t[static_cast<std::size_t>(s * 256 + d)] =
                s == d ? static_cast<std::uint8_t>(s) : sp.nearest(palette, (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2);
        }
    return t;
}

std::vector<std::uint8_t> shade_levels(const Palette& palette) {
    // x87 at 53 bits: the level's factor kept as a double, the component loaded as an integer,
    // multiplied, truncated (__ftol); the low 16 bits above 0xFF give 0xFF.
    const SortedPalette sp(palette);
    std::vector<std::uint8_t> t(32 * 256);
    double f = 0.0;
    for (int level = 0; level < 32; ++level) {
        for (int c = 0; c < 256; ++c) {
            const Rgb& p = palette[static_cast<std::size_t>(c)];
            auto part = [&](int v) {
                const auto r = static_cast<std::int64_t>(static_cast<double>(v) * f);
                return static_cast<std::uint16_t>(r) > 0xFF ? 0xFF : static_cast<int>(static_cast<std::uint8_t>(r));
            };
            t[static_cast<std::size_t>(level * 256 + c)] = sp.nearest(palette, part(p.r), part(p.g), part(p.b));
        }
        f -= -0.06875;
    }
    return t;
}

std::vector<std::uint8_t> light_levels(const Palette& palette) {
    const SortedPalette sp(palette);
    std::vector<std::uint8_t> t(32 * 256);
    for (int level = 0; level < 32; ++level) {
        const double f = 1.0 - static_cast<double>(level) * (-1.0 / 30.0);  // (the constants at 0x4FDBD0 / 0x4FDBD8)
        for (int c = 0; c < 256; ++c) {
            const Rgb& p = palette[static_cast<std::size_t>(c)];
            auto part = [&](int v) {
                const auto r = static_cast<std::int32_t>(static_cast<std::int64_t>(static_cast<double>(v) * f));
                return r > 0xFF ? 0xFF : static_cast<int>(static_cast<std::uint8_t>(r));
            };
            t[static_cast<std::size_t>(level * 256 + c)] = sp.nearest(palette, part(p.r), part(p.g), part(p.b));
        }
    }
    return t;
}

std::vector<std::uint8_t> water_tint_table(const Palette& palette) {
    const SortedPalette sp(palette);
    std::vector<std::uint8_t> t(256);
    for (int c = 0; c < 256; ++c) {
        const Rgb& p = palette[static_cast<std::size_t>(c)];
        const int b = (p.b >> 1) + 0x3C < 0x100 ? (p.b >> 1) + 0x32 : 0xFF;
        t[static_cast<std::size_t>(c)] = sp.nearest(palette, p.r >> 1, p.g >> 1, b);
    }
    return t;
}

void lines_to_sprites(DrawList& list) {
    for (DrawCmd& c : list.cmds) {
        if (c.op != DrawOp::Line) continue;
        const int x0 = std::min(c.x, c.x1), y0 = std::min(c.y, c.y1);
        const int w = std::abs(c.x1 - c.x) + 1, h = std::abs(c.y1 - c.y) + 1;
        // Draw the reference line into a scratch image the size of its box. The segment is
        // clipped before it is stepped (which changes the pixels it lights), so the command's
        // clip goes along, moved into the box's coordinates.
        Image8 mask(w, h, 0);
        const ClipRect box_clip{c.clip.x0 - x0, c.clip.y0 - y0, c.clip.x1 - x0, c.clip.y1 - y0};
        draw_line(mask, box_clip, c.x - x0, c.y - y0, c.x1 - x0, c.y1 - y0, 1);
        Sprite8 s;
        s.width = w;
        s.height = h;
        s.key = c.colour == 0 ? 1 : 0;
        s.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), s.key);
        for (std::size_t i = 0; i < s.pixels.size(); ++i)
            if (mask.pixels[i]) s.pixels[i] = c.colour;
        const Sprite8* sp = list.keep(std::move(s));
        c.op = DrawOp::Blit;
        c.sprite = sp;
        c.persistent = false;
        c.x = x0;
        c.y = y0;
    }
}

} // namespace render
