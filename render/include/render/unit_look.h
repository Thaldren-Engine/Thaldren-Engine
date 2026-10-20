#pragma once
// How one unit becomes pixels (spec: docs/RE_MEMORY.md, "Rendering", "Unit look"):
// the model sprite (plain or lit, optionally drawn at twice the size and averaged down),
// the depth-plane composite of units with a depth plane (pieces drawn each frame, units on
// board, the construction look, the cut or tint under the sea level) and the shadows.
// Everything here works on sprites, so the CPU and GPU backends share the result.

#include <array>
#include <cstdint>
#include <vector>

#include "render/image.h"
#include "render/model.h"
#include "sim/pose.h"

namespace render {

// Tables the looks read (all from the palette files).
struct LookTables {
    const BlendTable* mix = nullptr;                   // colour mixing (anti-aliasing average, shadows)
    const std::vector<std::uint8_t>* shade = nullptr;  // 32 levels x 256 colours (lit path)
    const std::vector<std::uint8_t>* water = nullptr;  // 256: the colour a pixel takes under water
};

// The player's visual options.
struct ViewOptions {
    bool shadows = true;          // master switch for unit shadows
    bool unit_shadows = true;     // shadows of mobile units
    bool feature_shadows = true;
    bool antialias = true;        // structures drawn at twice the size and averaged down
    bool shading = true;          // structures drawn lit
};

// The unit type flag bits the looks test.
namespace look_type_flags {
constexpr std::uint32_t kHover = 0x1000, kFloater = 0x80000, kNoShadow = 0x2000000, kDigger = 0x40000000;
}

// One unit as the drawing code sees it.
struct LookUnit {
    const Model* model = nullptr;
    sim::PoseState pose;                  // synced copy (never the simulation's own)
    std::array<std::int32_t, 3> pos{};    // 16.16 world
    std::uint8_t team_colour = 0;
    std::uint32_t texture_frame = 0;      // texture animation counter (without cursors)
    const std::vector<std::uint16_t>* cursor_frames = nullptr;  // each primitive cursor's current frame
    std::uint32_t type_flags = 0;
    bool structure = false;               // claims its footprint through a yard map
    bool zbuffer = false;                 // the type asks for a depth plane
    float build_left = 0.0f;              // nonzero: under construction
    std::uint16_t id = 0;
    int depth_bias() const { return (type_flags & look_type_flags::kDigger) ? 125 : 50; }
};

// A sprite with an optional depth plane (one byte per pixel, larger = higher).
struct LookFrame {
    Sprite8 image;
    std::vector<std::uint8_t> depth;  // empty: no depth plane
};

// The model sprite: bounds over the visible pieces plus a 2-pixel margin, filled with the
// key 1; a depth plane when asked or when the unit has the z-buffer flag or is under
// construction. `which`: 1 the cached pieces, -1 all visible pieces (pieces of a unit
// under construction are always all drawn). Structures are drawn lit when shading is on
// and at twice the size when anti-aliasing is on.
// The double-size drawing happens in the shared scratch surface; `scratch`, when given,
// receives that surface's state afterwards (it replaces a composite in progress).
LookFrame build_unit_sprite(const LookUnit& u, bool force_depth, int which, const ViewOptions& opt, const LookTables& tables,
                            LookFrame* scratch = nullptr);

// Draws a model's pieces into `f` (hotspot = the unit origin): `which` as above, 0 = the
// pieces that are not cached. `lit` selects the lit path, `double_size` the anti-aliased one.
void rasterize_unit_pieces(LookFrame& f, const LookUnit& u, int which, bool lit, bool double_size, const LookTables& tables,
                           LookFrame* scratch = nullptr);

// Everything the depth-plane path draws for a unit, as one sprite: its sprite, the
// construction look, the pieces that are not cached, the units on board (depth tested)
// and the cut / tint below the sea level.
struct DepthLookInput {
    const LookUnit* unit = nullptr;
    std::vector<const LookUnit*> cargo;  // shown units on board
    bool own_view = false;               // tint instead of cut under water
    int sea_level = 0;
    std::uint32_t tick = 0;
    ViewOptions options;
    LookTables tables;
};
LookFrame compose_depth_look(const DepthLookInput& in, const LookFrame& sprite);

// Shadow images. The mobile shadow is the sprite's shape in colour 0 (depth path: cut below
// `cut_depth` when >= 0); the structure shadow is the model projected along the light
// (x + y/4, -z - y/4) in colour 0, minus where the unit's own sprite will cover it.
Sprite8 mobile_shadow(const LookFrame& sprite, int cut_depth);
Sprite8 structure_shadow(const LookUnit& u, const Sprite8& unit_sprite);

// Cut (pixels whose depth <= level become the key) and tint (through the water table).
void cut_below(LookFrame& f, std::uint8_t level);
void tint_below(LookFrame& f, std::uint8_t level, const std::vector<std::uint8_t>& water);

// The water table (the original's 0x4BAF30): each colour maps to the palette colour nearest
// (in the sorted-by-brightness window search) to (r/2, g/2, b/2 + 50).
std::vector<std::uint8_t> water_tint_table(const Palette& palette);

} // namespace render
