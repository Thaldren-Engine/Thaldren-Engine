#pragma once
// Projectiles: the fixed table of shots in flight, their creation by weapons,
// the per-step flight update, impacts, explosions and area damage
// (see docs/RE_MEMORY.md, "Projectiles").
//
// The table has a fixed capacity and keeps the contents of every slot between
// uses: a new shot starts with whatever the previous occupant left in the
// fields it does not set itself, and that matters (feature-hit debounce,
// speeds of shots that never set one, ...). Deleted shots are only marked
// during the step and squeezed out at its end.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sim/combat.h"
#include "sim/heightmap.h"
#include "sim/rng.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

using Vec3i = std::array<std::int32_t, 3>;  // 16.16 world position or velocity

namespace weapon_flags {
constexpr std::uint32_t kLineOfSight = 1u << 0, kBallistic = 1u << 1, kBeam = 1u << 3, kMeteor = 1u << 5,
                        kParalyzer = 1u << 7, kDropped = 1u << 8, kStartSmoke = 1u << 9, kEndSmoke = 1u << 10,
                        kSoundTrigger = 1u << 11, kGuidance = 1u << 12, kTracks = 1u << 13, kUnitsOnly = 1u << 14,
                        kGroundBounce = 1u << 15, kWaterWeapon = 1u << 16, kSmokeTrail = 1u << 18,
                        kSelfPropelled = 1u << 20, kPropeller = 1u << 21, kNoExplode = 1u << 22, kBurnBlow = 1u << 23,
                        kTwoPhase = 1u << 24, kCruise = 1u << 25, kNoAutoRange = 1u << 27, kInterceptor = 1u << 30;
}

// The parts of a weapon definition that projectiles use.
struct WeaponType {
    std::uint8_t index = 0;               // definition slot (identifies the weapon in network packets)
    std::uint32_t flags = 0;              // weapon_flags
    std::int32_t velocity = 0;            // 16.16 per step
    std::int32_t start_velocity = 0;
    std::int32_t acceleration = 0;
    std::int32_t range = 0;               // world units
    std::uint16_t weapon_timer = 0;       // steps
    std::uint16_t turn_rate = 0;          // angle per step
    std::uint16_t burst = 0, burst_rate = 0, spray_angle = 0;
    std::uint16_t duration = 0;           // beam length in steps
    std::uint16_t random_decay = 0, smoke_delay = 0, flight_time = 0, hold_time = 0;
    std::uint16_t area_of_effect = 0;     // diameter, world units
    std::uint16_t default_damage = 0;
    float edge_effectiveness = 0.0f;
    std::int32_t shake_magnitude = 0, shake_duration = 0;
    std::uint16_t sound_start = 0xFFFF, sound_hit = 0xFFFF, sound_water = 0xFFFF;  // 0xFFFF = none
    // Damage by unit name, in the order of a case-insensitive sort (first spelling
    // of a name wins); other units take default_damage.
    std::vector<std::pair<std::string, std::int32_t>> damage;
};

namespace projectile_flags {
constexpr std::uint16_t kBeamFull = 1;      // a beam stopped growing: its tail follows
constexpr std::uint16_t kDeleted = 2;       // removed at the end of the step
constexpr std::uint16_t kPhaseMask = 0x30;  // two-phase weapons: 0 = launch phase
constexpr std::uint16_t kPhase2 = 0x10;
}

struct Projectile {
    const WeaponType* weapon = nullptr;
    Vec3i pos{};
    Vec3i trail{};         // beam tail / cruise waypoint
    Vec3i vel{};
    Vec3i aim{};
    std::int16_t tumble = 0;         // meteors: spin accumulator
    std::uint16_t yaw = 0;           // direction of travel around the vertical axis
    std::uint16_t elevation = 0;     // climb angle (meteors: second spin accumulator)
    std::int32_t speed = 0;
    std::int32_t distance = 0;       // muzzle to aim point at launch (16.16)
    std::uint32_t spawn_tick = 0;    // launch step; bursts: step of the last round
    std::uint32_t expire_tick = 0;
    std::uint32_t next_smoke_tick = 0;
    Unit* target = nullptr;
    Unit* owner = nullptr;
    std::int32_t intercepted = -1;   // table slot of the shot it chases (-1 none); slots, not shots
    std::int16_t last_feature_x = 0, last_feature_z = 0;  // feature cell last hit (hit once per cell)
    std::int16_t ground_height = 0;  // average height of the cell below
    std::uint16_t burst_left = 0;    // nonzero: a burst launcher, not a shot
    std::int16_t piece = 0;          // the owner's firing piece
    std::int16_t propeller = 0;      // propeller spin
    std::uint8_t owner_slot = 0;     // owner's player slot, 10 = none
    std::int16_t compact_index = 0;  // slot index before the last compaction
    std::uint16_t flags = 0;         // projectile_flags
};

constexpr int kMaxProjectiles = 300;

struct ProjectileTable {
    std::array<Projectile, kMaxProjectiles> slots{};
    std::int32_t count = 0;
    // Camera following a unit's shots: the unit (set by the interface), the
    // followed slot (a slot, so after compaction it may follow another shot) and
    // where the camera rests when the followed shot ends.
    const Unit* camera_unit = nullptr;
    std::int32_t camera_slot = -1;
    Vec3i camera_rest{};
    std::uint16_t camera_rest_ticks = 0;
    // Set when the original would have crashed (division by zero, ...); the
    // engine continues with a defined value.
    std::string fault;
    // Detonations nest when interceptors set off other shots.
    std::int32_t detonation_depth = 0;
    bool runaway_chain = false;
};

constexpr std::int32_t kMaxDetonationDepth = 1000;

// A map cell (16 x 16 world units) as projectiles see it.
struct MapCell {
    std::uint16_t unit = 0;        // occupying unit id, 0 = none
    std::uint16_t unit2 = 0;       // second (overlapping) unit id
    std::uint8_t height = 0;
    std::uint8_t height_max = 0, height_min = 0;
    std::uint16_t feature = 0xFFFF;  // feature type index; >= 0xFFFB none, 0xFFFE = part of a bigger feature
    // 0xFFFE cells: low byte = cells back in z, high byte = cells back in x to the
    // feature's own cell. Feature cells: the feature's animation slot or damage taken.
    std::uint16_t feature_aux = 0;
    std::uint8_t flags = 0;        // bit 0: the feature has an animation slot
};

struct MapGrid {
    std::int32_t width = 0, height = 0;  // cells
    std::vector<MapCell> cells;
    MapCell* cell(std::int32_t x, std::int32_t z);  // nullptr outside
    MapCell* cell_at(const Vec3i& world);           // cell under a 16.16 position, nullptr outside
};

struct FeatureTypeInfo {
    std::int16_t footprint_x = 0, footprint_z = 0;
    std::uint8_t height = 0;
};

struct ProjectileEnvironment {
    std::uint32_t game_tick = 0;
    Vec3i wind{};                    // drift per step of ballistic and dropped shots
    std::int32_t gravity = 0;
    std::uint8_t sea_level = 0;
    bool no_sea_level_trigger = false;  // map without water impacts
    std::uint16_t options = 0;          // bit 7: double damage, bit 8: half damage
    bool net_game = false;
};

// Systems projectiles reach outside the simulation state they own.
class ProjectileSystems {
public:
    virtual ~ProjectileSystems() = default;
    // Weapon slots belong to the weapon system.
    virtual const WeaponType* slot_weapon(const Unit& unit, int slot) = 0;
    virtual std::uint16_t slot_fire_heading(const Unit& unit, int slot) = 0;
    virtual std::int32_t motion_speed(const Unit& unit) = 0;
    // Presentation (no effect on the simulation).
    virtual void play_sound(std::uint16_t sound, const Vec3i& pos) = 0;
    virtual void spawn_smoke(const Vec3i& pos) = 0;         // trail/end puff
    virtual void spawn_muzzle_smoke(const Vec3i& pos) = 0;  // launch puff
    // Explosion art of the weapon (water art if `water_art`); `in_water` selects the water variant of the effect.
    virtual void spawn_explosion(const Vec3i& pos, const WeaponType& weapon, bool water_art, bool in_water) = 0;
    virtual void camera_shake(std::int32_t x, std::int32_t y, std::int32_t duration) = 0;
    // Feature damage (burning, accumulated damage, destruction) at the feature's own cell.
    virtual void damage_feature(MapCell& cell, std::int32_t x, std::int32_t z, const WeaponType& weapon) = 0;
    virtual void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) = 0;
    // Position of a feature animation slot (features with flags bit 0).
    virtual Vec3i feature_anim_pos(std::uint16_t slot) = 0;
};

struct ProjectileWorld {
    ProjectileTable& table;
    Combat& combat;  // units (index = id), their states, players (11 records), damage
    const HeightMap& heights;
    MapGrid& map;
    const std::vector<FeatureTypeInfo>& features;
    ProjectileEnvironment& env;
    ProjectileSystems& systems;
    BattleRng& rng;
};

// The firing weapon slot.
struct FireSlot {
    const WeaponType* weapon = nullptr;
    int index = 0;                   // 0..2: which of the owner's weapons (script FirePrimary/Secondary/Tertiary)
    std::uint16_t fire_heading = 0;  // aim of a ballistic shot
    std::uint16_t fire_pitch = 0;
    std::uint32_t aim_drop = 0;      // ballistic: offset whose steps of gravity are taken off the launch climb
};

// Takes the next free slot (nullptr when full); clears its deleted flag and target only.
Projectile* alloc_projectile(ProjectileTable& table);
// Common launch set-up: weapon, start, aim (if given), timers, owner and its firing
// piece (runs the owner's Query script), launch sound.
void init_projectile(ProjectileWorld& w, Projectile& p, const WeaponType& weapon, const Vec3i& start, const Vec3i* aim,
                     std::uint32_t spawn_tick, Unit* owner);
// Shots aimed straight at a point (line of sight, self-propelled): false if the table is full.
bool fire_directed(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& muzzle, const Vec3i& aim,
                   Unit* target);
// Shells launched at the slot's aim angles.
bool fire_ballistic(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& muzzle, const Vec3i& aim,
                    Unit* target);
// A shot drifting with its owner's motion (unused by the original game).
bool spawn_from_motion(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& pos);
// An ownerless shot with a given velocity (meteors); optionally announced to the network.
bool spawn_free_projectile(ProjectileWorld& w, const WeaponType& weapon, const Vec3i& pos, const Vec3i& vel, bool broadcast);

// One simulation step of every shot, then compaction.
void update_projectiles(ProjectileWorld& w);
// Impact: effects, then direct or area damage. `p` may be a projectile outside the table.
void detonate_projectile(ProjectileWorld& w, Projectile& p, Unit* hit);
// Received "detonate" packet (0x0E: aim x, y, z, weapon index): the first matching shot explodes.
void detonate_matching(ProjectileWorld& w, const std::uint8_t* packet);
// A dying unit's burst launchers are removed (each removal compacts the table at once).
void remove_owner_bursts(ProjectileWorld& w, Unit& owner);
// The explosion of a dying unit (its explode-as or self-destruct weapon).
void detonate_unit_death(ProjectileWorld& w, Unit& unit, bool self_destruct);
// Removes deleted shots, keeping order.
void compact_projectiles(ProjectileTable& table);

} // namespace sim
