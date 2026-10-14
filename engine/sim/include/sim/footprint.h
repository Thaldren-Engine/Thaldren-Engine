#pragma once
// Unit footprints: the map cells a unit occupies, the coarse spatial buckets
// that list units by area, and the yard maps of structures
// (see docs/RE_MEMORY.md, "Footprints and occupancy").
//
// A map cell names up to two occupants: `unit` for ground units and
// structures, `unit2` for the other layer. When a unit wants a cell somebody
// else holds, the holder keeps it unless the holder's owner is a watched
// (remote) player; the loser is marked to claim again later and the winner is
// marked so that its release lets the losers try again.

#include <cstdint>
#include <string>
#include <vector>

#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

constexpr std::int32_t kNoBucket = -1, kOffMapBucket = -2;  // Unit::bucket values besides indexes

// Units listed by squares of 8 x 8 cells (128 x 128 world units), plus one list
// for units whose footprint is off the map. Lists are linked through
// Unit::cargo_next, newest first.
struct UnitBuckets {
    std::int32_t width = 0, height = 0;  // in buckets: (cells + 7) / 8
    std::vector<Unit*> heads;            // row-major
    Unit* off_map = nullptr;
    void resize_for_map(std::int32_t cells_x, std::int32_t cells_z);
};

namespace footprint_flags {
constexpr std::uint32_t kWantsCells = 0x08000000u;   // unit flags: claim again (lost a cell, or yard changed)
constexpr std::uint32_t kHoldsWanted = 0x04000000u;  // unit flags: holds cells another unit wanted
constexpr std::uint32_t kStructure = 0x20000000u;    // unit flags: claims through its type's yard map
constexpr std::uint8_t kCellYardMark = 2;            // map cell flags: marked by a yard map's bit 0
constexpr std::uint8_t kYardMark = 1, kYardOpen = 2, kYardClosed = 4;  // yard map byte bits
}  // namespace footprint_flags

// Systems the footprint changes reach outside this module (movement classes'
// passability and motion objects belong to the movement system).
class FootprintSystems {
public:
    virtual ~FootprintSystems() = default;
    // Cells changed: every movement class recomputes its passability for the
    // footprint at (x, z) of size (w, h) cells (the original's 0x440A40).
    virtual void update_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) = 0;
    // The unit released its cells (0x440A70): with a motion object, the movement classes
    // changed since the motion object's footprint step recompute the unit's footprint and the
    // step is set to the game tick; without one, every movement class does.
    virtual void passability_after_release(Unit& unit) = 0;
    // A unit with a motion object claims cells: its motion object's footprint step
    // (motion +0x26) becomes the game tick.
    virtual void motion_cells_claimed(Unit& unit) = 0;
};

struct FootprintWorld {
    MapGrid& map;
    UnitBuckets& buckets;
    std::vector<Unit>& units;  // index = id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    FootprintSystems& systems;
    // Set when the original would read or write outside its tables (the engine skips the access).
    std::string fault;
};

// First cell of a footprint of (fx, fz) cells centred on world (x, z) (16.16).
void footprint_origin(std::int32_t x, std::int32_t z, std::int16_t fx, std::int16_t fz, std::int16_t& cell_x,
                      std::int16_t& cell_z);

// Takes the unit's cells (0x47CC30): files it in the bucket under its position (the
// off-map list when the footprint is not inside the map), then claims its yard map's
// cells (structures, followed by the height range and passability updates) or the
// cells of its layer (ground: unit, layer 2: unit2).
void claim_footprint(FootprintWorld& w, Unit& unit);
// Gives the cells back (0x47D0E0) and lets units waiting for them claim again.
void release_cells(FootprintWorld& w, Unit& unit);
// Gives the cells back and leaves the bucket lists (0x47CBD0; used when a unit dies).
void clear_footprint(FootprintWorld& w, Unit& unit);
// A unit marked kWantsCells claims again (0x47C790). Structures take or free each
// cell by the current yard state; other units claim `unit` (ground layer) or `unit2`.
void claim_wanted_cells(FootprintWorld& w, Unit& unit);
// Every unit (and carried unit) in the buckets around the area whose footprint
// overlaps it claims again if it wants cells (0x47E5C0 with 0x47ED30).
void reclaim_area(FootprintWorld& w, std::int16_t x, std::int16_t z, std::int16_t size_x, std::int16_t size_z);
// The unit wants its cells again and its neighbours re-claim (0x47DB20).
void force_reclaim_around(FootprintWorld& w, Unit& unit);
// Whether the cells the yard map occupies when open (value != 0) or closed hold
// no other unit (0x47D970). False near the map's top and left edges.
bool yard_cells_free(FootprintWorld& w, const Unit& unit, std::int32_t value);
// Recomputes each cell's height range (min and max of its corner heights) over an
// area clipped to the map (0x483210).
// The unit leaves / joins the head of its spatial bucket's list (0x47CB00 / 0x47CB40; used by
// transport attach and detach).
void bucket_unlink(FootprintWorld& w, Unit& unit);
void bucket_prepend(FootprintWorld& w, Unit& unit);
void refresh_height_range(MapGrid& map, std::int16_t x, std::int16_t z, std::int16_t size_x, std::int16_t size_z);

} // namespace sim
