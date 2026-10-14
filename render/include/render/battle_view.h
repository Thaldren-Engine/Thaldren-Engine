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
    bool unit_shadows = true, feature_shadows = true;
    std::uint8_t clear_index = 0;
};

DrawList build_draw_list(const BattleScene& scene);

// Screen position of a world point (16.16).
inline int screen_x(const Camera& c, std::int32_t wx) {
    return static_cast<std::int16_t>(static_cast<std::uint32_t>(wx - (c.scroll_x << 16)) >> 16) + c.origin_x;
}
inline int screen_y(const Camera& c, std::int32_t wy, std::int32_t wz) {
    return static_cast<std::int16_t>(static_cast<std::uint32_t>(wz - (c.scroll_y << 16)) >> 16) -
           (static_cast<std::int16_t>(static_cast<std::uint32_t>(wy) >> 16) >> 1) + c.origin_y;
}

} // namespace render
