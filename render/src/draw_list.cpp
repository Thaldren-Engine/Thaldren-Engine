#include "render/draw_list.h"

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
        }
    }
    return frame;
}

void lines_to_sprites(DrawList& list) {
    for (DrawCmd& c : list.cmds) {
        if (c.op != DrawOp::Line) continue;
        const int x0 = std::min(c.x, c.x1), y0 = std::min(c.y, c.y1);
        const int w = std::abs(c.x1 - c.x) + 1, h = std::abs(c.y1 - c.y) + 1;
        // Draw the reference line into a scratch image the size of its box.
        Image8 mask(w, h, 0);
        draw_line(mask, mask.bounds(), c.x - x0, c.y - y0, c.x1 - x0, c.y1 - y0, 1);
        // Clip as the reference would (clip applies in frame coordinates).
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
