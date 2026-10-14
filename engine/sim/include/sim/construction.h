#pragma once
// Construction, repair by builders, reclaiming and capturing
// (see docs/RE_MEMORY.md, "Construction and reclaim").
//
// A unit under construction is a normal unit whose build fraction left runs
// from 1.0 down to 0.0; builders take a slice of it every step, paying the
// matching part of its energy and metal cost through their own resource slot,
// and its hit points grow with it. Factories build on their pad and carry the
// new unit there until it is finished; mobile builders put a structure (or
// unit) on a site and work on it from within their build distance; helpers
// join any unfinished unit. Reclaiming turns features and units back into
// resources; capturing hands a unit over to the builder's player.
//
// The order behaviours (factory build, mobile build, help build, repair,
// reclaim of features and of units, capture, being carried, parking) run
// through sim/missions.h; this module holds them and the routines they share.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/combat.h"
#include "sim/heightmap.h"
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Unit type data construction reads besides the other modules' type data.
struct ConstructionTypeInfo {
    std::int16_t footprint_x = 0, footprint_z = 0;  // cells
    std::uint8_t movement = 1;                      // 0: structure (yard map placement)
    std::uint16_t build_distance = 0;               // reach of a builder's nano lathe (world units)
    std::int16_t reclaim_reach = 0;                 // whole part of the model's average size: added to a reclaimer's reach
    bool has_build_options = false;                 // the type can build (finishing needs a builder that can)
    std::uint8_t waterline = 0;
    std::uint8_t max_slope = 0;
    std::int16_t max_water_depth = 0, min_water_depth = 0;
    std::vector<std::uint8_t> yard_map;  // structures: one byte per footprint cell (placement bits 3-7 too)
};

// Systems construction reaches outside the simulation core and the modules it calls directly.
class ConstructionSystems {
public:
    virtual ~ConstructionSystems() = default;
    // Unit creation (sim/unit_create.h): a new unit for `player`; nullptr when it cannot be made.
    virtual Unit* create_unit(int player, std::uint16_t type, const std::array<std::int32_t, 3>& pos, bool fully_built,
                              std::uint32_t layer) = 0;
    // The placement check of a footprint (0x47DB70): `mode` is the layer (1 ground) the
    // check is for; structures use their yard map.
    virtual bool can_place(std::uint16_t type, std::uint16_t exclude, std::int16_t cell_x, std::int16_t cell_z,
                           std::uint32_t mode) = 0;
    // Transport attach / detach (packet 0x0A, sent and applied: 0x48AAC0). `transport` null detaches.
    virtual void transport_attach(Unit& cargo, Unit* transport, std::uint8_t piece, std::uint8_t layer) = 0;
    // Movement goals of the order (sim/motion.h): standing next to the cell rectangle at `cell`
    // of `size` (packed x low, z high; 0x438AD0), or between two radii of `pos` (0x438A00).
    virtual void attach_edge_goal(Unit& unit, MissionOrder& order, std::uint32_t cell, std::uint32_t size) = 0;
    virtual void attach_ring_goal(Unit& unit, MissionOrder& order, const std::array<std::int32_t, 3>& pos, std::int32_t outer,
                                  std::int32_t inner) = 0;
    // Nano spray from a box to a point (reclaiming, capturing; 0x472200). The other direction
    // is MissionSystems::nano_beam.
    virtual void nano_beam_reverse(const std::array<std::int32_t, 6>& box, const std::array<std::int32_t, 3>& to) = 0;
    // Features (sim/features.h): the feature at cell (x, z) is destroyed, as reclaimed (0x423550).
    virtual void kill_feature(std::int32_t x, std::int32_t z, bool reclaimed) = 0;
    // Victory conditions: a unit is about to change hands.
    virtual void victory_unit_transferred(Unit& unit) = 0;
    // Network.
    virtual void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) = 0;
    // The unit's script function was started with arguments (network notice 0x456290).
    virtual void broadcast_script_call_args(Unit& unit, std::int16_t function, std::int32_t argc, std::int32_t a) = 0;
    // User interface (no effect on the simulation): a finished structure of a playing player
    // opens the builder menu; the build menu of the focused unit and the orders panel refresh.
    virtual void builder_menu(Unit& built) = 0;
    virtual void build_menu_refresh(Unit& builder) = 0;
    virtual void orders_panel_changed() = 0;
    // The map info's network provider type (3: reclaimed features are announced).
    virtual std::int32_t network_provider() = 0;
};

struct ConstructionEnvironment {
    std::int32_t ai_difficulty = 0;     // 0 easy, 1 medium, others hard: AI players get part of refunds
    std::uint8_t sea_level = 0;
    std::uint16_t build_menu_focus = 0;  // unit id whose build menu is shown (0 none)
};

struct Construction {
    MapGrid& map;
    const std::vector<FeatureTypeInfo>& features;
    const std::vector<ConstructionTypeInfo>& types;  // by type index
    const HeightMap& heights;
    ConstructionSystems& systems;
    ConstructionEnvironment& env;
};

// Mission kinds handled here.
bool construction_kind(std::uint8_t kind);
// One step of a construction behaviour (same contract as run_mission_behaviour).
std::int32_t run_construction_behaviour(Missions& m, Unit& unit, MissionOrder& order, std::uint32_t& event);

// One slice of building (`work` > 0, in build-time units) or unbuilding (`work` < 0) of
// `target` by `builder` (0x41BA60). Building pays the slice's energy and metal through the
// builder's resource slot and fails without changes while its player stalls; hit points
// follow the build fraction. Unbuilding refunds metal to the target's own slot and kills
// the target once nothing is left. A unit that becomes finished is finalized. True when a
// building slice was paid.
bool apply_build_progress(Missions& m, Unit& builder, Unit& target, float work);
// An unfinished unit nobody works on loses `steps` steps' worth of building (0x41BCD0).
void unfinished_decay(Missions& m, Unit& unit, std::int32_t steps);
// The end of construction (0x41B8D0): the unit is finished, leaves its factory pad, may
// activate, and the network hears about it. Needs a live builder that can build.
void finalize_built_unit(Missions& m, Unit* builder, Unit* built);
// Hit points one reclaim pulse of `steps` steps takes off a unit (0x438650).
std::int32_t reclaim_pulse(Missions& m, Unit& worker, Unit& target, std::int32_t steps);
// Whether `worker` may reclaim `target` (0x489960).
bool can_reclaim(Missions& m, Unit& worker, Unit& target);
// Transfers the unit to player `new_owner` (0x488570, local version without a packet).
void transfer_ownership(Missions& m, Unit& unit, int new_owner);

// Yard map placement of a structure type with its footprint corner at the cell (0x47D2E0,
// as the simulation calls it: no view checks, every occupant blocks).
bool structure_fits(const Construction& c, std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z);
// Height a structure stands at on the cells (0x47D820): the lowest slope-sampled cell height,
// or the sea level minus the waterline when no cell samples the slope.
std::uint8_t structure_place_height(const Construction& c, std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z);
// A structure's position snapped to its footprint cells (0x47DDC0); mobile types unchanged.
void snap_to_footprint(const Construction& c, std::uint16_t type, std::array<std::int32_t, 3>& pos);
// Reclaiming a feature at a 16.16 position yields its energy and metal to the unit's slot and
// destroys it as reclaimed (0x4237D0). False when there is nothing to reclaim there.
bool reclaim_feature(Missions& m, Unit& unit, const std::array<std::int32_t, 3>& pos);

// Transport attach / detach (packet 0x0A applied, 0x48AB70): `transport` null drops the cargo
// back into its spatial bucket; otherwise it joins the transport's cargo list at `piece`, and a
// playing owner's cargo is told to be carried (unless the transport is an air base).
struct TransportLinks {
    // Unlink from / push onto the unit's spatial bucket list (0x47CB00 / 0x47CB40).
    virtual ~TransportLinks() = default;
    virtual void bucket_unlink(Unit& unit) = 0;
    virtual void bucket_prepend(Unit& unit) = 0;
    virtual std::uint32_t type_flags(const Unit& unit) = 0;  // the unit type's flag word
    virtual void selection_changed() = 0;                    // user interface: the selection lost a unit
};
void apply_transport_attach(Missions& m, TransportLinks& links, Unit& cargo, Unit* transport, std::uint8_t piece, std::uint8_t layer);
// The cargo's orders make way for a "be carried" order (0x4384A0).
void enqueue_be_carried(Missions& m, Unit& cargo);

// Command input: a factory's (or builder's) queue of `type` changes by `delta` (0x43B0B0):
// the last order of `kind` for that type grows or shrinks, or a new one is queued.
void adjust_build_count(Missions& m, Unit& unit, std::uint8_t kind, std::int32_t type, std::int32_t delta);

} // namespace sim
