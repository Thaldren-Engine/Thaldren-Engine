#pragma once
// Drawable articulated models: per-piece polygons (flat palette colour or a
// textured quad) and the routines that turn a posed model into pixels the way
// the original does (spec: docs/RE_MEMORY.md, "Rendering", "Models").

#include <array>
#include <cstdint>
#include <vector>

#include "render/image.h"
#include "render/raster.h"
#include "sim/pose.h"

namespace render {

// A texture: one frame, an animation, or one frame per team colour.
struct ModelTexture {
    std::vector<Sprite8> frames;
    bool team_coloured = false;  // frame = the owner's colour index
    bool animated = false;       // more than one frame
};

struct ModelPrimitive {
    std::vector<std::uint16_t> indices;
    std::array<std::uint16_t, 3> normal_indices{};  // the three vertices the lit path takes the face normal from
    std::uint8_t colour = 0;            // palette index for flat polygons
    bool flat = false;                  // flat colour; otherwise textured (quads only)
    const ModelTexture* texture = nullptr;
};

struct ModelPiece {
    std::vector<ModelPrimitive> primitives;
    int first_drawn = 0;                // 1 when primitive 0 is the selection outline
};

// Pieces are indexed like the nodes of the matching sim::PoseModel.
struct Model {
    std::vector<ModelPiece> pieces;
};

struct ModelDrawParams {
    int depth_bias = 50;               // added to the vertex height for the depth value (75 more for diggers)
    std::uint8_t team_colour = 0;      // frame index for team-coloured textures
    std::uint32_t animation_frame = 0; // global animation counter for animated textures
    bool all_pieces = false;           // draw cached and uncached pieces (units under construction)
    bool direct_cursor = true;         // pieces drawn straight onto the frame take the animation cursor (else frame 0)
};

// Projects one transformed point (unit space, 16.16) the way the sprite
// rasteriser does: x = floor(x), y = floor(-z) - (floor(y) >> 1), z = floor(y) + bias.
RasterVertex project_sprite_point(const std::int32_t* p, int depth_bias);

// The cached-model sprite: bounds over every visible piece (always including
// the unit origin) plus a 2-pixel margin, background = key 1, then the
// visible cached pieces (all visible pieces with `all_pieces`) drawn from the
// last piece to the first, primitives in stored order, no depth test.
// The hotspot is the unit origin.
Sprite8 rasterize_model_sprite(const Model& model, const sim::PoseState& pose, const ModelDrawParams& params);

// The same sprite drawn with a depth plane (cleared to 0; each pixel keeps the depth
// written, z = height + bias): the original's two-plane sprite of units under construction.
Sprite8 rasterize_model_sprite_depth(const Model& model, const sim::PoseState& pose, const ModelDrawParams& params,
                                     std::vector<std::uint8_t>& depth);

// The two pulsing colours (palette 0xA0..0xAF) of a unit under construction at step `tick`.
std::array<std::uint8_t, 2> nanoframe_colours(std::uint16_t unit_id, std::uint32_t tick);

// The look of a unit under construction (0x458DD0 with 0x458D30 and 0x458FA0) applied to its
// two-plane sprite: a band at a depth set by the build fraction left sweeps the model; above,
// below and in the band the pixels vanish, keep their texture or take the first pulsing
// colour, by stage (a frame, then a solid shell rising, then the texture rising under the
// shell, then a last band sweeping down); then every primitive of every visible piece is
// outlined (row ends, depth tested) in the second colour.
void apply_nanoframe(Sprite8& sprite, std::vector<std::uint8_t>& depth, const Model& model, const sim::PoseState& pose,
                     const ModelDrawParams& params, float build_left, std::uint16_t unit_id, std::uint32_t tick);

// Draws the visible pieces that are not cached straight onto `target`
// (last piece first), projecting every point relative to the unit's screen
// position: unit_screen_* = unit position minus the view scroll, 16.16, and
// (origin_x, origin_y) = the view's screen origin.
void draw_uncached_pieces(RasterTarget& target, const Model& model, const sim::PoseState& pose,
                          std::int32_t unit_view_x, std::int32_t unit_y, std::int32_t unit_view_z,
                          int origin_x, int origin_y, const ModelDrawParams& params);

// Draws one piece's primitives at already projected points (skipping the selection
// outline), flat polygons and textured quads, as the direct drawing paths do.
void draw_model_piece(RasterTarget& t, const ModelPiece& piece, const std::vector<RasterVertex>& pts,
                      const ModelDrawParams& params, bool use_animation);

// The unit's pose as the drawing code sees it: a copy synced to the unit's
// attitude (the original syncs the unit's own state while drawing; we never
// touch simulation state).
sim::PoseState pose_for_drawing(const sim::PoseState& pose, std::int16_t pitch, std::uint16_t heading,
                                std::int16_t bank);

} // namespace render
