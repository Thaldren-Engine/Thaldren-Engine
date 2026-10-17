#pragma once
// The battle view: projection, culling and draw order of terrain, features,
// units and projectiles, turned into a DrawList (spec: docs/RE_MEMORY.md,
// "Rendering"). Reads simulation state only; never modifies it.

#include <array>
#include <cstdint>
#include <vector>

#include "render/draw_list.h"
#include "render/image.h"
#include "render/model.h"
#include "render/unit_look.h"
#include "sim/heightmap.h"
#include "sim/unit.h"

namespace render {

// World (x, y, z) -> screen: x' = x - scroll_x + origin_x,
// y' = z - (y >> 1) - scroll_y + origin_y (integer parts of 16.16 world values).
struct Camera {
    int screen_width = 640, screen_height = 480;
    int origin_x = 128, origin_y = 32;      // screen position of the view's top-left corner
    int view_width = 512, view_height = 416;
    int scroll_x = 0, scroll_y = 0;         // world position at the view's top-left corner
    ClipRect view_clip() const { return {origin_x, origin_y, origin_x + view_width - 1, origin_y + view_height - 1}; }
};

// The ground image: a grid of 32x32 tiles (two height cells per tile side).
struct Terrain {
    int width = 0, height = 0;                    // in tiles
    const std::vector<std::uint16_t>* map = nullptr;  // tile index per tile, row-major
    const std::vector<Sprite8>* tiles = nullptr;  // 32x32, no key
};

struct FeatureInstance {
    int cell_x = 0, cell_z = 0;                   // the feature's map cell (16 world units)
    int footprint_x = 1, footprint_z = 1;
    int height = 0;                               // < 10: drawn before units
    const Sprite8* image = nullptr;
    const Sprite8* shadow = nullptr;
    bool image_blended = false, shadow_blended = false;
    // 3D features (wrecks, rocks, ...): drawn as a model at their slot's place and attitude.
    const Model* model = nullptr;
    const sim::PoseModel* pose_model = nullptr;
    std::array<std::int32_t, 3> pos{};  // 16.16 world
    std::int16_t pitch = 0, bank = 0;
    std::uint16_t heading = 0;
    // 3D features as the original draws them: through a stand-in unit (a structure with a depth
    // plane, seen like the viewer's own units; drawn with the unit look and its shadow).
    const struct UnitInstance* proxy = nullptr;
};

// A piece blown off a unit (sim/effects.h DebrisPiece): one model piece, turned and placed.
struct DebrisInstance {
    const Model* model = nullptr;
    const sim::PoseModel* pose_model = nullptr;
    int node = -1;
    std::array<std::int32_t, 3> pos{};   // the piece origin, 16.16 world
    std::array<std::int16_t, 3> turn{};  // x, y, z
    std::uint8_t team_colour = 0;
};

// A world effect (sim/effects.h WorldEffect): its glow (lightens the ground), a shard of a
// shattered piece and the explosion art, all at one point.
struct EffectInstance {
    std::array<std::int32_t, 3> pos{};
    const Sprite8* glow = nullptr;
    const Sprite8* art = nullptr;
    bool shard = false;
    std::array<std::array<std::int32_t, 3>, 8> points{};  // around pos, 16.16
    std::array<std::int16_t, 3> angle{};                   // x, y, z spin angles
    const ModelPrimitive* look = nullptr;                  // the shattered quad's colour / texture
    std::uint8_t team_colour = 0;
};

struct UnitInstance {
    const sim::Unit* unit = nullptr;
    const Model* model = nullptr;
    ModelDrawParams params;
    bool ground = true;           // drawn with the ground units (else after projectiles)
    bool shadow = true;           // offset shadow (ground units)
    std::int32_t ground_height = 0;  // terrain height under the unit (shadow placement), integer
    bool translucent = false;     // cloaked: the sprite is mixed with the frame (blend table)
    std::uint32_t type_flags = 0;    // the type's flag word (digger, hover, floater, noshadow; see look_type_flags)
    bool animate_direct = false;     // pieces drawn straight onto the frame use the texture cursor (moving or active)
    bool own_view = false;           // seen under water (tinted) rather than cut: the viewer's own units
    // Selection outline (given by the interface): a closed four-point outline in one colour,
    // drawn in the unit's turn just before its shadow and model (clipped to the view).
    bool outline = false;
    std::array<int, 8> outline_xy{};  // x0, y0, x1, y1, x2, y2, x3, y3 (screen)
    std::uint8_t outline_colour = 0;
};

// One nano lathe particle (sim/nano_spray.h): a dot in its palette colour.
struct NanoDotInstance {
    std::array<std::int32_t, 3> pos{};  // 16.16 world
    std::uint8_t colour = 0;
};

enum class ProjectileLook : std::uint8_t { Line, Sprite, Model };

struct ProjectileInstance {
    ProjectileLook look = ProjectileLook::Line;
    std::array<std::int32_t, 3> pos{}, tail{};   // 16.16 world
    std::uint8_t colour = 0;                       // Line
    const Sprite8* sprite = nullptr;               // Sprite
    bool sprite_blended = false;
    const Model* model = nullptr;                  // Model (rendered as a sprite, see spec)
    const sim::PoseModel* pose_model = nullptr;
    std::int16_t pitch = 0, bank = 0;
    std::uint16_t heading = 0;
    const Sprite8* shadow = nullptr;               // shadow blob blended at the ground point
    std::int32_t ground_height = 0;

    // The original's drawing by the weapon's render type (0x49BE60); -1: the looks above.
    // 0 line(s) head -> tail, 1/3/6 model node(s) with the shadow blob, 2 lens, 4 shell frame
    // with the blob, 5 flame frame (blended), 7 lightning.
    int render_type = -1;
    std::uint8_t colour2 = 0;            // second line's colour (type 0 with a second colour)
    bool second_line = false;
    std::uint16_t ground = 0;            // the shot's ground height word (+0x5E): blob row
    const Sprite8* blob = nullptr;       // anims\fx.gaf "shadow" frame 0
    const Sprite8* frame = nullptr;      // type 4 shell frame / type 5 flame frame (none: not drawn)
    struct Node {
        const ModelPiece* piece = nullptr;
        const std::vector<std::array<std::int32_t, 3>>* vertices = nullptr;
    };
    Node node, child;                    // types 1/3/6: the model's root; type 1: its first child
    std::array<std::int16_t, 3> angles{}, child_angles{};
    std::uint8_t team_colour = 0;
};

// The viewed player's fog of war over the battle view (the original's 0x4843C0 and 0x4848E0;
// see docs/RE_MEMORY.md, "Line of sight and radar"). The map is in blocks of 32 x 32 world units.
// A grid of 32 x 32-pixel cells is laid over the view, each cell centred on a block corner;
// a cell whose four surrounding blocks are all unexplored is filled black, all unseen greyed
// (the frame remapped through the grey table); partly covered cells get an edge frame chosen
// by which corners are covered (frame = corner mask - 1, mask bits 1 top left, 2 top right,
// 4 bottom left, 8 bottom right), from one of four sequences picked by the cell position.
struct FogOfWar {
    int blocks_x = 0, blocks_z = 0;        // map size in blocks
    std::vector<std::uint8_t> unexplored;  // per block, nonzero: never explored (black)
    std::vector<std::uint8_t> unseen;      // per block, nonzero: explored, out of sight (grey); empty: no grey
    std::array<const std::vector<Sprite8>*, 4> black{}, gray{};  // fog edge sequences
    const BlendTable* gray_table = nullptr;  // gray_remap_table
    std::uint8_t fill = 0;                   // colour of black cells
};

struct BattleScene {
    Camera camera;
    Terrain terrain;
    const sim::HeightMap* heights = nullptr;      // feature placement
    std::vector<FeatureInstance> features;
    std::vector<UnitInstance> units;              // in the game's visible-unit order
    std::vector<ProjectileInstance> projectiles;
    std::vector<NanoDotInstance> nano;            // drawn after the ground units, before the projectiles
    std::uint32_t game_tick = 0;                  // pulses the colours of units under construction
    std::vector<DebrisInstance> debris;           // drawn after the projectiles, before the effects
    std::vector<EffectInstance> effects;          // in the effect table's order
    const BlendTable* light = nullptr;            // light table for glows (light_blend_table); none: no glows
    bool unit_shadows = true, feature_shadows = true;  // (kept for older callers; `options` decides)
    ViewOptions options;                          // the player's visual options
    LookTables tables;                            // mixing, shade and water tables for unit looks
    int sea_level = 0;
    const FogOfWar* fog = nullptr;                // drawn over everything (none: no fog)
    // The C runtime random state when the frame is drawn: lightning shots draw from a copy of
    // it (the original's draw calls rand(); the renderer never advances the simulation's).
    std::uint32_t crt_rand_state = 0;
    std::uint8_t clear_index = 0;
};

DrawList build_draw_list(const BattleScene& scene);

// The game's generated explosion glows (three sequences of square noise discs: 12 frames
// shrinking from 64 pixels, 15 from 128, 15 from 200), pixels 0x4F..0x6E for the light blit,
// key 0xFF. The original fills them from the C runtime's rand() while loading; `seed` starts
// our own generator (presentation only).
std::vector<Sprite8> glow_frames(int which, std::uint32_t seed);

// Screen position of a world point (16.16).
inline int screen_x(const Camera& c, std::int32_t wx) {
    return static_cast<std::int16_t>(static_cast<std::uint32_t>(wx - (c.scroll_x << 16)) >> 16) + c.origin_x;
}
inline int screen_y(const Camera& c, std::int32_t wy, std::int32_t wz) {
    return static_cast<std::int16_t>(static_cast<std::uint32_t>(wz - (c.scroll_y << 16)) >> 16) -
           (static_cast<std::int16_t>(static_cast<std::uint32_t>(wy) >> 16) >> 1) + c.origin_y;
}

} // namespace render
