#include "render/draw_list.h"
#include "render/unit_look.h"

#include <algorithm>
#include <cstdlib>

namespace render {

Image8 execute(const DrawList& list, const BlendTable& blend) {
    Image8 frame(list.width, list.height, list.clear_index);
    for (const DrawCmd& c : list.cmds) {
        switch (c.op) {
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
