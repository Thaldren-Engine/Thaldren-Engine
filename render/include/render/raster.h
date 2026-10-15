#pragma once
// Polygon scan conversion with the original's integer rules: 16.16 edge
// walking with a +0xFFFF bias on x, top-inclusive / bottom-exclusive rows,
// left-inclusive / right-exclusive spans, a right and bottom clip that also
// drops the last column and row of the clip rectangle, affine texture
// coordinates and an optional byte depth test (spec: docs/RE_MEMORY.md,
// "Rendering", "Polygon scan conversion").

#include <cstdint>

#include "render/image.h"

namespace render {

struct RasterTarget {
    std::uint8_t* pixels = nullptr;
    std::uint8_t* depth = nullptr;   // optional; same layout as pixels
    int pitch = 0;
    ClipRect clip;                   // inclusive
};

struct RasterVertex {
    int x = 0, y = 0, z = 0;         // pixels; z is the depth value (low byte compared)
};

// Flat polygon with `count` vertices (any count >= 1, walked in order).
void fill_polygon(RasterTarget& t, const RasterVertex* v, int count, std::uint8_t colour);

// Quad mapped with the whole texture: vertex 0 -> (0,0), 1 -> (w-1,0),
// 2 -> (w-1,h-1), 3 -> (0,h-1). The texture's key is not honoured (texels are copied).
// A texel address outside the texture (the original reads neighbouring
// memory there) is clamped and reported through `out_of_range`.
void textured_quad(RasterTarget& t, const RasterVertex* v, const Sprite8& texture, bool* out_of_range = nullptr);

// The lit (shaded) path's vertex: a light level 0..31 per vertex, interpolated like depth.
struct LitVertex {
    int x = 0, y = 0, z = 0, light = 0;
};

// Lit flat polygon: each pixel takes shade[level * 256 + colour] (shade: 32 x 256 table).
// Same scan rules as fill_polygon; the caller clips to the whole target.
void fill_polygon_lit(RasterTarget& t, const LitVertex* v, int count, std::uint8_t colour, const std::uint8_t* shade);

// Lit textured quad: texel through the shade table when the target has a depth plane;
// without one the texels are copied unlit (as the original does).
void textured_quad_lit(RasterTarget& t, const LitVertex* v, const Sprite8& texture, const std::uint8_t* shade);

// Outline of a polygon as the construction look draws it (0x4C0820 / 0x4C0A90): the
// polygon is scan converted like fill_polygon but without clipping, and each row with
// right > left gets only its two end pixels (x = left and x = right), each depth tested
// (written when the stored depth byte <= the interpolated depth) when there is a depth plane.
void outline_polygon(RasterTarget& t, const RasterVertex* v, int count, std::uint8_t colour);

} // namespace render
