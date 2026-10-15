#pragma once
// Map features: trees, rocks, wrecks and the like standing on the map grid
// (see docs/RE_MEMORY.md, "Features and wrecks").
//
// A feature lives in its map cell (sim::MapCell: type, extra word, flags);
// the other cells of its footprint point back at it. Features that move,
// burn or play an animation also hold one of 2048 animation slots. A slot is
// 48 bytes whose meaning depends on what uses it: a 3D feature keeps its
// position, velocity and attitude there, a 2D one two frame cursors over the
// same bytes. The original never clears a slot it hands out again, so later
// users read what earlier ones left (a 3D feature placed in a slot that last
// held a burning tree's cursors can start with a velocity). The slots are
// therefore kept as raw bytes and every routine reads and writes them at the
// original offsets.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sim/anim.h"
#include "sim/heightmap.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/unit.h"

namespace sim {

namespace feature_type_flags {
constexpr std::uint16_t kFlat = 1u << 0;        // 2D (frame sequence); clear = 3D object
constexpr std::uint16_t kAnimating = 1u << 1;
constexpr std::uint16_t kFlammable = 1u << 4;
constexpr std::uint16_t kGeothermal = 1u << 5;
constexpr std::uint16_t kIndestructible = 1u << 9;
}  // namespace feature_type_flags

// FeatureTypeInfo::sequences
enum FeatureSequence : int { kSeqNormal, kSeqNormalShadow, kSeqBurn, kSeqBurnShadow, kSeqDie, kSeqDieShadow,
                             kSeqReclaim, kSeqReclaimShadow };

// Map cell values (MapCell::feature).
constexpr std::uint16_t kNoFeature = 0xFFFF;
constexpr std::uint16_t kFeaturePart = 0xFFFE;   // another cell of a bigger feature
constexpr std::uint16_t kFeatureVoid = 0xFFFC;   // a map's void cells
constexpr std::uint16_t kFirstSpecialFeature = 0xFFFB;

constexpr int kFeatureSlots = 0x800;
constexpr int kFeatureSlotSize = 0x30;

// One animation slot as raw bytes, with the original's field offsets.
struct FeatureSlot {
    std::array<std::uint8_t, kFeatureSlotSize> b{};

    template <class T> T get(int off) const {
        T v;
        std::memcpy(&v, b.data() + off, sizeof v);
        return v;
    }
    template <class T> void set(int off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }

    // Offsets.
    static constexpr int kNext = 0x00, kPrev = 0x02;   // i16 list links
    static constexpr int kObject = 0x04;               // u32 3D: object state
    static constexpr int kCursorA = 0x04;              // 2D: main frame cursor (12 bytes)
    static constexpr int kPos = 0x08;                  // i32 x3 3D: position
    static constexpr int kCursorB = 0x10;              // 2D: shadow frame cursor (12 bytes)
    static constexpr int kVel = 0x14;                  // i32 x3 3D: velocity
    static constexpr int kAngles = 0x20;               // i16 x3 3D: attitude (pitch, heading, bank)
    static constexpr int kDamage = 0x26;               // u16 3D: damage taken
    static constexpr int kCellX = 0x28, kCellZ = 0x2A; // i16 the feature's cell
    static constexpr int kType = 0x2C;                 // u16 feature type
    static constexpr int kFireCountdown = 0x2E;        // u8 steps until a fire spreads
    static constexpr int kFlags = 0x2F;                // u8 feature_slot_flags

    AnimCursor cursor(int off) const;
    void set_cursor(int off, const AnimCursor& c);
    Vec3i pos() const { return {get<std::int32_t>(kPos), get<std::int32_t>(kPos + 4), get<std::int32_t>(kPos + 8)}; }
    Vec3i vel() const { return {get<std::int32_t>(kVel), get<std::int32_t>(kVel + 4), get<std::int32_t>(kVel + 8)}; }
};

namespace feature_slot_flags {
constexpr std::uint8_t kBurning = 1, kReclaimAnim = 2, kHasShadow = 4, kNoSpread = 8, kReclaimed = 16;
}

struct FeatureState {
    std::array<FeatureSlot, kFeatureSlots> slots{};
    std::int32_t active = -1;  // slots being animated / moved
    std::int32_t idle = -1;    // 3D features that came to rest
    std::int32_t free = 0;     // unused slots
    std::int32_t sweep = 0;    // reproduction sweep: the next cell to visit, counting down
    // Per feature type: the type-wide animation (normal sequence and its shadow) of animating types.
    std::vector<std::array<AnimCursor, 2>> type_cursors;
    std::string fault;         // set where the original would crash or corrupt memory

    // The battle-start state: every slot free (0, 1, 2, ... in order), all bytes zero.
    void reset();
};

struct FeatureEnvironment {
    std::uint32_t game_tick = 0;
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    Vec3i wind{};                    // wind velocity (x and z steer spreading fires)
    bool destructible = true;        // game option: weapons damage features
    // Network role for feature damage: 0 alone, 1 the host (announces the outcome),
    // 2 another machine (asks the host and changes nothing itself).
    std::uint8_t network_role = 0;
};

// What features reach outside their own state.
class FeatureSystems {
public:
    virtual ~FeatureSystems() = default;
    // Path finding's passability over a rectangle of cells (0x440A40).
    virtual void update_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) = 0;
    // The object state of a 3D feature (drawing only); the returned value only has to be nonzero.
    virtual std::uint32_t create_object(std::uint32_t model) = 0;
    virtual void free_object(std::uint32_t object) = 0;
    // Burn weapons of spreading fires: area damage from a temporary shot.
    virtual void area_damage(const WeaponType& weapon, const Vec3i& at) = 0;
    // Feature packet 0x0F (6 bytes). `to_host`: a non-host machine asking the host; else announced to all.
    virtual void feature_packet(const std::uint8_t* packet, bool to_host) = 0;
    // Presentation: smoke of a burning feature, steam of a geothermal vent, the fire sound.
    virtual void feature_smoke(const Vec3i& pos) = 0;
    virtual void feature_steam(const Vec3i& pos) = 0;
    virtual void feature_fire_sound(std::int16_t x, std::int16_t z) = 0;
    virtual void wreck_fire(const Vec3i& pos) = 0;  // a burning wreck's fire (0x472630 with 15, 900)
};

struct FeatureWorld {
    FeatureState& state;
    MapGrid& map;
    const std::vector<FeatureTypeInfo>& types;
    const HeightMap& heights;
    const AnimLibrary& anims;
    FeatureEnvironment& env;
    FeatureSystems& systems;
    BattleRng& rng;   // the battle RNG
    Lcg15& crt_rand;  // the C runtime's rand() stream of the simulation thread
};

// Feature type data for the type-wide cursors (call after the types are set; 0x4224B0's tail).
void init_feature_type_cursors(FeatureWorld& w);

// Puts feature `type` on the cell with index `cell` (row-major) and the cells of its
// footprint, removing features in the way (fails if one cannot be removed). 3D features get a
// slot positioned at `pos` (default: the footprint centre on the ground) with attitude `angles`
// (default 0). `owner` is the cell's owner nibble (10 = nobody). Returns the slot, or -1 (also
// for a 2D feature). 0xFFFC marks a void cell; 0xFFFF does nothing.
std::int32_t place_feature(FeatureWorld& w, std::int32_t cell, std::uint16_t type, const Vec3i* pos,
                           const std::array<std::int16_t, 3>* angles, std::uint8_t owner);
// Removes the feature covering `cell` (an indestructible one only with `force`); false if none was removed.
bool remove_feature(FeatureWorld& w, std::int32_t cell, bool force);
// The next stage replaces the feature at (x, z): its dead (or reclaimed) type, keeping a 3D slot's place.
void replace_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool reclaimed);
// Destroys the feature at (x, z): its dying (or reclaimed) animation, else the next stage at once.
void kill_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool reclaimed);
// Sets the feature at (x, z) on fire if it has a burning animation. `remote`: a fire another
// machine started (it never spreads and is not announced).
void ignite_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool remote);
// Weapon damage to the feature whose own cell is `cell` at (x, z) (area damage, damage packets).
void damage_feature(FeatureWorld& w, MapCell& cell, std::int32_t x, std::int32_t z, const WeaponType& weapon);
// A received feature packet 0x0F. `weapons` is indexed by weapon slot.
void apply_feature_packet(FeatureWorld& w, const std::uint8_t* packet, const std::vector<const WeaponType*>& weapons);
// The per-step feature stage: type animations, the reproduction sweep, then every active slot.
void step_features(FeatureWorld& w);

// Map loading, after the map's features are placed: the cells outside the visible area get
// the edge marker 0xFFFD (where no feature is, or a feature part), which blocks movement:
// the last two columns; at the top, per column, the cells whose ground (half the height up)
// lies above the map's top edge; at the bottom, per column from the last row upwards, while
// a cell's ground lies below the visible height (cells x 16 - 128), the cell above it (so the
// last row itself is never marked); on lava maps every cell whose lowest corner is at or below
// the sea level.
void mark_map_edges(MapGrid& map, std::uint8_t sea_level, bool lava_world);
// The wreck of a dead unit: the type's corpse, `level - 1` stages further down its dead chain, at
// the unit's cells, place and attitude. In deep water it sinks (unless the unit type is itself a
// feature). `burn` (on land) only adds a fire effect.
struct WreckSource {
    std::uint16_t corpse = 0xFFFF;  // the unit type's corpse feature
    bool feature_unit = false;      // the unit type's "is a feature" flag
    std::uint8_t owner = 10;        // the unit's player slot
};
void spawn_wreck(FeatureWorld& w, const Unit& unit, const WreckSource& src, std::int32_t level, bool burn);
// Position of a slot (3D features; what projectiles aim area damage at).
Vec3i feature_slot_pos(const FeatureState& s, std::uint16_t slot);

} // namespace sim
