#pragma once
// World effects and debris (see docs/RE_MEMORY.md, "Debris and effects").
//
// They look like decoration, but the original keeps them in simulation state
// and two things make them matter: scripts that blow a unit apart draw from
// the battle RNG (per call, and per shattered polygon), and how many polygons
// can still be shattered depends on the fixed table of world effects
// (explosions, splashes, shards), whose entries live as long as their
// animations and shards fly. So the table, the shard slots, the falling
// debris pieces and the ring buffer the debris lives in are all modelled
// exactly here; only pixels and particles are left to the presentation.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/anim.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/unit.h"

namespace sim {

constexpr int kMaxWorldEffects = 300;
constexpr int kMaxShards = 300;
constexpr int kMaxDebris = 100;
constexpr std::int32_t kDebrisCacheSize = 100000;

// A world effect: an explosion or splash animation, and/or a flying shard of a shattered piece.
struct WorldEffect {
    std::int32_t shard = -1;     // shard slot (-1 none)
    AnimCursor art;              // the explosion art
    AnimCursor glow;             // its glow (one of the generated explosions)
    Vec3i pos{};
    Vec3i carry{};               // half the unit's velocity when it shattered
    Vec3i vel{};                 // own velocity (gravity acts on y)
    std::array<std::int32_t, 3> spin{};
    std::array<std::int16_t, 3> angle{};
    std::uint16_t flags = 0;     // bit 0: explodes where it lands
};

// A shard: one quad of a unit's piece, turned into a thin box (8 points around its centre).
struct Shard {
    std::uint8_t free = 0xFF;    // 0xFF = unused
    std::array<std::array<std::int32_t, 3>, 8> points{};
    // Its look (presentation): the source primitive and the unit's owner (team colour).
    const PoseModel* model = nullptr;
    int node = -1, primitive = -1;
    int owner = 0;
};

// A piece blown off a unit that falls (and bounces) on its own.
struct DebrisPiece {
    std::uint16_t unit = 0;      // the unit it came from (id; the unit may be gone)
    std::int32_t piece = 0;
    std::array<std::int32_t, 3> spin{};  // added to its turn each step (z, x, y order of the original)
    Vec3i vel{};
    std::int32_t ttl = 0;        // steps left
    std::int32_t shatter = 0;    // the script's shatter word (1 / 0)
    std::uint32_t flags = 0;     // debris_flags
    // The piece as it was (a copy of the unit's piece state, placed in the world).
    const PoseModel* model = nullptr;
    int node = -1;
    std::array<std::int32_t, 3> move{};
    std::array<std::int16_t, 3> turn{};
    Vec3i pos{};                 // piece origin in the world
    int owner = 0;
};

namespace debris_flags {
constexpr std::uint32_t kFire = 1, kSmoke = 2, kShatter = 4, kFall = 8, kExplodeOnLanding = 16, kShardExplodes = 32;
}

// The 100000-byte ring buffer debris pieces live in. Allocation walks on from where
// the last one ended and evicts the pieces it overwrites.
struct DebrisCache {
    struct Block {
        std::int32_t owner = -1;  // debris slot it was given to (-1 none)
        std::int32_t size = 0;    // including the 8-byte header
    };
    std::map<std::int32_t, Block> blocks;  // by header offset
    std::int32_t cursor = 0;
    void reset();
};

struct EffectsState {
    std::array<WorldEffect, kMaxWorldEffects> effects{};
    std::int32_t count = 0;
    std::array<Shard, kMaxShards> shards{};
    std::array<std::int32_t, kMaxDebris> debris_block{};  // payload offset in the cache, -1 = empty
    std::array<DebrisPiece, kMaxDebris> debris{};
    DebrisCache cache;
    std::string fault;
    // The battle-start state (the original's explosion setup at map load).
    void reset();
};

struct EffectsEnvironment {
    std::int32_t gravity = 0;
    std::uint8_t sea_level = 0;
    bool no_sea_level_trigger = false;
    bool lava_world = false;
    // Sequences (sim/anim.h addresses): the generated glows, the general explosion and the
    // script bitmap explosions (explode 1-5, nuke), the water and lava splashes.
    std::array<std::uint32_t, 3> glow{};
    std::array<std::uint32_t, 6> bitmap{};
    std::uint32_t water_splash = 0, lava_splash = 0;
};

class EffectsSystems {
public:
    virtual ~EffectsSystems() = default;
    // The velocity of a unit's motion object; false for units without one.
    virtual bool unit_velocity(const Unit& unit, Vec3i& vel) = 0;
    // The unit's player slot (team colour of shards and debris).
    virtual int unit_owner(const Unit& unit) = 0;
    // Presentation: the smoke an explosion above the sea leaves (0x472630 with 7, 15).
    virtual void explosion_smoke(const Vec3i& pos) = 0;
};

struct EffectsWorld {
    EffectsState& state;
    const MapGrid& map;
    const AnimLibrary& anims;
    EffectsEnvironment& env;
    EffectsSystems& systems;
    BattleRng& rng;
};

// Adds a world effect at `pos`: animation `seq` (0 = none) with generated glow `glow_index`
// (-1 none); `flags` 0 also leaves smoke when above the sea. Nothing when the table is full.
void spawn_world_effect(EffectsWorld& w, const Vec3i& pos, std::uint32_t seq, std::int32_t glow_index, std::int32_t flags);
// The explosion art of a projectile impact (sim/projectile.h hook).
void spawn_impact(EffectsWorld& w, const Vec3i& pos, const WeaponType& weapon, bool water_art, bool in_water);
// A unit script's explode command (piece, type flags): hides the piece and throws it as debris
// or shatters it, and adds the bitmap explosions the flags ask for.
void explode_piece(EffectsWorld& w, Unit& unit, std::int32_t piece, std::uint32_t type);
// The per-step stage: debris pieces, then world effects (shards fly and land, animations
// advance), then finished effects are removed.
void step_effects(EffectsWorld& w);
// The ground height the effects use: the average of the cell's lowest and highest
// corner (-1 off the map). Also used by features.
std::int32_t ground_average(const MapGrid& map, const Vec3i& pos);

} // namespace sim
