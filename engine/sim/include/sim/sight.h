#pragma once
// Line of sight, the explored map, radar, sonar, jamming and the cloak hold-off
// (see docs/RE_MEMORY.md, "Line of sight and radar").
//
// The map is seen in blocks of 2 x 2 cells (32 x 32 world units). Every player
// with a controller keeps a byte per block: how many of its units' sight shapes
// cover the block (the "sight counts"). A unit's shape is either a stamp
// from a table of masks picked by its sight distance ("circular" sight) or,
// with "true" sight, rays from a table of rings walked over a per-block height
// band, so hills hide what lies behind them. One map-wide grid of 16-bit words
// holds a bit per player for every block it has ever seen (the explored map).
//
// Three game options pick the behaviour (SightState::flags): reveal (the map
// starts unexplored and units explore it), fog (sight counts are live; without
// it everything explored counts as seen) and true sight.
//
// Radar, sonar and jamming marks are unit flags computed once per step from the
// point of view of one player: the player this machine views (the original's
// behaviour; the marks are read by every player's target lists).

#include <array>
#include <cstdint>
#include <vector>

#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

struct UnitBuckets;
struct TargetingState;

namespace sight_flags {
constexpr std::uint16_t kReveal = 1;     // units explore the map (a rebuild clears the explored map)
constexpr std::uint16_t kFog = 2;        // live sight counts (else explored = seen)
constexpr std::uint16_t kTrueSight = 4;  // ring rays over the height band (else mask stamps)
constexpr std::uint16_t kViewClean = 8;  // presentation: the viewed player's fog edges are up to date
}  // namespace sight_flags

// Unit flag bits this system writes.
namespace sight_unit_flags {
constexpr std::uint32_t kRadar = 0x100, kSonar = 0x200, kJammed = 0x400, kCloakHold = 0x1000;
constexpr std::uint32_t kWantsCloak = 0x800, kDying = 0x4000, kAlive = 0x10000000;
}  // namespace sight_unit_flags

// A sight mask: covered blocks of a stamp, drawn with its top-left corner at the
// unit's block minus the offsets.
struct SightMask {
    std::int32_t width = 0, height = 0;
    std::int32_t x_offset = 0, y_offset = 0;
    std::vector<std::uint8_t> covered;  // row-major, nonzero = covered
};
// A ring: rays ("slices") of block offsets walked outwards from the unit's block.
struct SightPoint {
    std::int16_t x = 0, z = 0;
};
using SightSlice = std::vector<SightPoint>;
using SightRing = std::vector<SightSlice>;

struct SightTables {
    std::vector<SightMask> masks;  // circular sight, by sight distance / 32 - 5 (clamped)
    std::vector<SightRing> rings;  // true sight, rings[r - 1] for radius r
    // The original reads the ring "before the first" for a sight distance below 32
    // (memory in front of the table, heap contents). This stands in for it; empty by
    // default (only the unit's own block is seen).
    SightRing ring_before_first;
};

// Unit type data sight reads (besides the model box of UnitTypeInfo).
struct SightType {
    std::int16_t sight = 0, radar = 0, sonar = 0;
    std::int16_t radar_jam = 0, sonar_jam = 0, min_cloak_distance = 0;
    std::int32_t model_size_x = 0, model_size_y = 0, model_size_z = 0;  // 16.16
    bool can_cloak = false;  // the "cloaks for energy" ability
    bool stealth = false;    // never marked on radar or sonar
};

struct PlayerSight {
    bool allocated = false;  // players with a controller get a sight count map
    std::int32_t width = 0, height = 0;  // blocks
    std::vector<std::uint8_t> counts;    // (width * height + 7) & ~7 bytes
};

// A dying unit of the viewed player keeps seeing for a while (the original's
// "eyeball" slots: 20, compacted on expiry; slots past the count keep old data).
struct LingeringSight {
    std::uint8_t player = 0;  // player record whose counts are stamped
    std::int16_t sight = 0;
    std::uint8_t eye = 0;
    std::array<std::int32_t, 3> pos{};
    std::uint32_t expire_tick = 0;
    std::int16_t cell_x = 0, cell_z = 0;
    std::uint8_t frame = 0;
};

struct SightState {
    std::uint16_t flags = 0;           // sight_flags
    std::int32_t width = 0, height = 0;  // blocks (map cells / 2)
    // Height band per block {top, bottom} (true sight), (width * height + 7) & ~7 entries.
    std::vector<std::array<std::uint8_t, 2>> band;
    std::array<PlayerSight, 11> players{};
    std::array<LingeringSight, 20> lingering{};
    std::int32_t lingering_count = 0;
    std::uint8_t view_dirty = 0;  // presentation (the original's view dirty bit 2)
};

// Per player options the sensor pass reads (from the players' side data).
struct SightPlayerOptions {
    bool shares_radar = false;  // allies see this player's radar marks
    bool watching = false;      // out of the game, watching: sees all marks
};

struct SightEnvironment {
    std::uint32_t game_tick = 0;
    std::uint8_t sea_level = 0;
    std::uint8_t view_player = 0;      // the player this machine shows (record index)
    std::uint16_t player_count = 0;    // the sensor pass runs only with 2 or more
    std::array<SightPlayerOptions, 11> options{};
};

struct SightWorld {
    SightState& state;
    const SightTables& tables;
    std::vector<std::uint16_t>& explored;  // per block, bit = player state index
    std::vector<Unit>& units;              // index = unit id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    const std::vector<std::array<std::uint8_t, 16>>& alliances;  // by player record, indexed by the other's state
    const std::vector<SightType>& types;                          // by type index
    const UnitBuckets& buckets;
    const TargetingState* targeting;  // the players' unit tables (cloak hold-off); null = none
    SightEnvironment& env;
    // Accesses where the original would read or write outside its data (it crashes or
    // corrupts memory; the engine skips them), and reads of memory in front of the ring
    // table (heap contents; SightTables::ring_before_first stands in).
    std::uint32_t faults = 0;
    std::uint32_t heap_reads = 0;
};

// Map setup (the original's map load): blocks, the height band from the cell heights and
// sea level, and an explored map of zeros. `explored` is resized.
void init_sight_map(SightState& s, std::vector<std::uint16_t>& explored, const HeightMap& heights, std::uint8_t sea_level);
// Players' setup: a zeroed sight count map for every player with a controller.
void init_player_sight(SightState& s, const std::vector<PlayerSlot>& players);

// Rebuild of all sight (battle start, option changes): the explored map refilled when
// `clear_explored` (zeros with reveal, else all bits), every playing player's counts set
// (0 with fog, 1 without), then with fog every unit's sight stamped again.
void rebuild_sight(SightWorld& w, bool clear_explored);
// A unit moved (or the players' stage): its sight moves with it when its block (or eye
// height, with true sight) changed.
void update_unit_sight(SightWorld& w, Unit& unit);
// A new unit's first stamp (with fog).
void stamp_new_unit(SightWorld& w, Unit& unit);
// A dying unit: with fog its sight is removed; for the viewed player's own units a
// lingering copy is stamped first (60 steps).
void linger_dead_unit(SightWorld& w, Unit& unit);
void remove_unit_sight(SightWorld& w, Unit& unit);
// Once per drawn frame, after the frame's steps: lingering sight past its tick is removed.
void expire_lingering_sight(SightWorld& w);

// Whether player record `viewer` sees `unit` (its own always; cloaked never; under water
// only when sonar-marked; with fog: a covered block under one of four model corners; without:
// an explored block, explored for the viewed player).
bool unit_visible(SightWorld& w, int viewer, const Unit& unit);
// Whether the block under a 16.16 point is explored for the viewed player, using the
// size of `owner`'s count map (the targeting wreck check).
bool position_explored(const SightWorld& w, int owner, const std::array<std::int32_t, 3>& pos);
// Sight of the viewed player at a block (presentation): seen (fog) or explored (no fog).
bool block_seen(const SightWorld& w, int viewer, std::int32_t bx, std::int32_t bz);
// The same at a 16.16 world point (block of whole x, whole z minus half the height): what the
// drawing of shots and the radar marks of whatever is seen test.
bool point_in_sight(const SightWorld& w, int viewer, const std::array<std::int32_t, 3>& pos);

// The sensor pass (the viewed player's turn of the players' stage): radar and sonar marks
// from the viewed player's active sensors, enemy jammers, cloak hold-offs near visible
// enemies, and marks for whatever the viewed player sees.
void update_sensors(SightWorld& w);

// Explored blocks of player state `from` become explored for `to` (map sharing).
void share_explored(SightWorld& w, std::uint8_t from, std::uint8_t to);

}  // namespace sim
