#pragma once
// A frame as an ordered list of 2D operations on an 8-bit target. The battle
// view builder produces it (projection, culling, draw order, model sprites);
// a backend executes it: the CPU reference backend or the GPU backend. Both
// must produce the same palette indices.

#include <cstdint>
#include <memory>
#include <vector>

#include "render/image.h"

namespace render {

enum class DrawOp : std::uint8_t {
    Blit,       // copy the sprite, key pixels transparent
    BlitBlend,  // mix the sprite's non-key pixels with the frame through the blend table
    Line,       // one-colour line from (x, y) to (x1, y1)
    BlitLight,  // lighten the frame under the sprite's non-key pixels through the light table
    BlitRemap,  // replace the frame under the sprite's non-key pixels through the remap table (fog)
    BlitTable,  // replace the frame under the sprite's non-key pixels through the command's own table
    Lens,       // the 22 x 22 lens around (x, y): see apply_lens (colour = its transparency index)
};

struct DrawCmd {
    DrawOp op = DrawOp::Blit;
    const Sprite8* sprite = nullptr;  // Blit / BlitBlend
    bool persistent = false;          // sprite outlives the frame (backends may cache it)
    int x = 0, y = 0, x1 = 0, y1 = 0;
    std::uint8_t colour = 0;          // Line
    ClipRect clip;
    // BlitTable: a mixing table in the blend table's layout, entry [src * 256 + dst]; it must
    // outlive the frame (backends may keep it by its address).
    const BlendTable* table = nullptr;
};

struct DrawList {
    int width = 0, height = 0;
    std::uint8_t clear_index = 0;
    // The frame's contents before its first command (the game draws into one lasting buffer:
    // what this frame has not drawn yet is still the last frame's). Without it the frame starts
    // as clear_index. Only commands that read the frame outside what they draw (Lens) can see
    // the difference where the frame is covered anyway.
    std::shared_ptr<const Image8> back;
    // Light table for BlitLight, in the blend table's layout: entry [src * 256 + dst]
    // (see light_blend_table). Needed only when the list holds BlitLight commands.
    const BlendTable* light = nullptr;
    // Table for BlitRemap, same layout (see gray_remap_table). Needed only with BlitRemap commands.
    const BlendTable* remap = nullptr;
    std::vector<DrawCmd> cmds;
    // Sprites made for this frame (model sprites, shadows, directly drawn pieces).
    std::vector<std::unique_ptr<Sprite8>> transient;

    const Sprite8* keep(Sprite8 s) {
        transient.push_back(std::make_unique<Sprite8>(std::move(s)));
        return transient.back().get();
    }
    void blit(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::Blit, s, persistent, x, y, 0, 0, 0, clip});
    }
    void blend(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::BlitBlend, s, persistent, x, y, 0, 0, 0, clip});
    }
    void lighten(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::BlitLight, s, persistent, x, y, 0, 0, 0, clip});
    }
    void remap_under(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::BlitRemap, s, persistent, x, y, 0, 0, 0, clip});
    }
    // The frame under the sprite's non-key pixels through 	able (shaded boxes, dimming).
    void through(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent, const BlendTable* table) {
        DrawCmd c{DrawOp::BlitTable, s, persistent, x, y, 0, 0, 0, clip};
        c.table = table;
        cmds.push_back(c);
    }
    void line(int x0, int y0, int x1, int y1, std::uint8_t colour, const ClipRect& clip) {
        cmds.push_back({DrawOp::Line, nullptr, false, x0, y0, x1, y1, colour, clip});
    }
    void lens(int x, int y, std::uint8_t key, const ClipRect& clip) {
        cmds.push_back({DrawOp::Lens, nullptr, false, x, y, 0, 0, key, clip});
    }
};

// The lens's offset table, w = h = 22, hotspot (11, 11), one entry per pixel row by row: for
// a pixel at distance r = sqrt(dx² + dy²) from the hotspot with trunc(r) < w / 4, the
// distance (as a 16-bit value) from the pixel's own index to the index it shows,
// trunc(dx / s) + 11 + (trunc(dy / s) + 11) · w with s = (11 − r) / 8 (doubles; 8 is the
// lens's strength); every other pixel holds 32000 (shows the transparency index). Every
// entry shows a pixel at most 4 columns and rows from the hotspot (inside the capture).
const std::vector<std::int16_t>& lens_offsets();
// The lens drawn into `frame` around (x, y): the frame's 22 × 22 rectangle at (x − 11, y − 11)
// read as rows of the frame's memory without clipping (a column past the right edge is the
// next row's start), each pixel replaced through lens_offsets (the bytes laid out around the
// capture — the table before it, the output after it — are modelled too, though the table
// never reaches them), and the result drawn at (x − 11, y − 11) inside `clip`, pixels equal
// to `key` left out.
void apply_lens(Image8& frame, int x, int y, std::uint8_t key, const ClipRect& clip);
// True when the list holds a command that reads the frame outside what it draws (Lens), or
// starts from a back buffer: backends without such reads execute it on the CPU.
bool needs_frame_reads(const DrawList& list);

// CPU reference backend: executes the list into an 8-bit frame.
Image8 execute(const DrawList& list, const BlendTable& blend);

// The game's light blit as a mixing table: a sprite pixel p in 0x4F..0x6E selects
// light level p - 0x4F of the 32 x 256 light table (palettes\PALETTE.LHT), applied to
// the frame pixel; other pixel values leave the frame alone.
BlendTable light_blend_table(const std::vector<std::uint8_t>& light32);

// The game's grey remap (the original's 0x4BAD30): every palette colour maps to the colour
// nearest (squared RGB distance, searched among colours whose r + g + b lies within 40 of
// the target's, in order of r + g + b) to its grey (r + g + b) / 3. Returned as a mixing
// table: entry [src * 256 + dst] = grey of dst, for any src.
BlendTable gray_remap_table(const Palette& palette);

// The game's colour tables computed from its palette, for when their files are missing (the
// original's 0x4BA750 / 0x4BADF0 / 0x4BABD0, each through the nearest-colour search above):
// the mixing table, entry [s * 256 + d] = s when s = d, else the colour nearest to the
// components' halved sums; the 32-level shade table, level L = each component × (L × 0.06875,
// a double summed level by level), truncated, above 0xFF → 0xFF; the 32-level light table,
// level L = each component × (1 + L / 30), truncated, at most 0xFF.
BlendTable mixing_table(const Palette& palette);
std::vector<std::uint8_t> shade_levels(const Palette& palette);
std::vector<std::uint8_t> light_levels(const Palette& palette);

// Turns every Line command into a transient sprite holding exactly the
// reference line's pixels, so backends without a line primitive stay exact.
void lines_to_sprites(DrawList& list);

} // namespace render
