#pragma once
// The battle view: projection, culling and draw order of terrain, features,
// units and projectiles, turned into a DrawList (spec: docs/RE_MEMORY.md,
// "Rendering"). Reads simulation state only; never modifies it.

#include <cstdint>
#include <vector>

#include "render/draw_list.h"
#include "render/image.h"
#include "render/model.h"
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
};

struct BattleScene {
    Camera camera;
    Terrain terrain;
    const sim::HeightMap* heights = nullptr;      // feature placement
    std::vector<FeatureInstance> features;
    std::vector<UnitInstance> units;              // in the game's visible-unit order
    std::vector<ProjectileInstance> projectiles;
    std::vector<DebrisInstance> debris;           // drawn after the projectiles, before the effects
    std::vector<EffectInstance> effects;          // in the effect table's order
    const BlendTable* light = nullptr;            // light table for glows (light_blend_table); none: no glows
    bool unit_shadows = true, feature_shadows = true;
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
