#pragma once
// Unit state shared with unit scripts, and the script-facing unit values
// (see docs/RE_MEMORY.md, "Unit values").

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/heightmap.h"
#include "sim/pose.h"
#include "sim/script_vm.h"
#include "sim/weapon_slot.h"

namespace sim {

// Unit type data the unit values need.
struct WeaponType;

struct UnitTypeInfo {
    std::uint32_t max_damage = 1;   // full hit points
    std::int32_t model_max_y = 0;   // model height
    // The rest of the model box (16.16, relative to the unit position): projectile hits and area damage.
    std::int32_t model_min_x = 0, model_min_y = 0, model_min_z = 0, model_max_x = 0, model_max_z = 0;
    const WeaponType* explode_as = nullptr;       // explosion when the unit dies
    const WeaponType* self_destruct_as = nullptr; // explosion when it self-destructs
    // Structures: one byte per footprint cell, row by row (sim/footprint.h): bit 0 marks the
    // map cell (cell flag 2), bit 1 occupies the cell while the yard is open, bit 2 while closed.
    std::vector<std::uint8_t> yard_map;
    std::uint16_t corpse = 0xFFFF;  // wreck feature type (sim/features.h), 0xFFFF = none
    bool is_feature = false;        // type flag "is a feature": its wreck does not sink
};

namespace unit_flags {
constexpr std::uint32_t kYardClaimPending = 0x08000000u;                                           // flags
constexpr std::uint8_t kActivated = 1, kArmored = 2, kCloaked = 4, kBuilding = 8, kParalyzed = 16;  // activate flags
constexpr std::uint8_t kInBuildStance = 1, kBusy = 2, kYardOpen = 4, kBuggerOff = 8;              // script flags
constexpr std::uint32_t kAlive = 0x10000000u;                                                      // flags
constexpr std::uint16_t kNetScriptState = 4;                                                       // net dirty
}  // namespace unit_flags

struct MissionOrder;
struct UnitLink;

struct Unit {
    std::uint16_t id = 0;
    std::array<std::int32_t, 3> pos{};   // 16.16 world position
    std::int16_t pitch = 0;
    std::uint16_t heading = 0;
    std::int16_t bank = 0;
    std::int16_t hit_points = 0;
    std::uint8_t activate_flags = 0;
    std::uint8_t script_flags = 0;
    std::uint32_t flags = 0;             // bits 18-19 move orders, 20-21 fire orders, 28 alive
    float build_fraction_left = 0.0f;
    std::uint16_t net_dirty = 0;         // also the events a sleeping primary order waits for (sim/orders.h)
    MissionOrder* orders = nullptr;            // primary order list (sim/orders.h)
    MissionOrder* secondary_orders = nullptr;  // secondary order list
    UnitLink* referrers = nullptr;             // links (order targets) pointing at this unit, newest first
    std::uint16_t hover_phase = 0;             // hover bob phase (sim/ground_settle.h)
    std::int32_t water_occupancy = 0;          // last value the script heard (sim/motion.h)
    std::uint32_t group = 0;                   // the player's selection group the unit belongs to
    std::uint32_t cloak_holdoff_tick = 0;  // firing keeps the unit decloaked until this tick
    const UnitTypeInfo* type = nullptr;
    Unit* carrier = nullptr;     // transport carrying this unit
    Unit* cargo_head = nullptr;  // first unit carried
    Unit* cargo_next = nullptr;  // next unit in the carrier's cargo list
    PoseState pose;
    std::unique_ptr<ScriptInstance> script;
    std::array<WeaponSlot, 3> weapons{};
    // Set when the unit is created (sim/unit_create.h) and used by the footprint system
    // (sim/footprint.h). Defaults are the original's zeroed table.
    std::int16_t cell_x = 0, cell_z = 0;            // first map cell of the footprint
    std::int16_t footprint_x = 0, footprint_z = 0;  // footprint size in cells
    std::int32_t bucket = -1;                       // spatial bucket listing the unit (-1 none, -2 off the map)
    // Spatial bucket lists link their units through cargo_next, as the original does
    // (it shares the field with the cargo list; carried units are never in a bucket).
    std::int16_t los_cell_x = 0, los_cell_z = 0;    // line-of-sight bookkeeping (cleared at creation)
    std::uint8_t los_frame = 0;
    std::uint8_t transport_piece = 0;               // 0xFF after creation
    float metal_extraction = 0.0f;                  // metal extracted per step (from the cells under it)
    std::uint32_t unbuilt_word = 0;                 // cleared when created unfinished (use not reverse engineered)
    std::uint32_t draw_flags = 0;                   // bit 0: the type's z-buffer flag
};

// What unit values need from the rest of the world.
class UnitWorld {
public:
    virtual ~UnitWorld() = default;
    // Unit by id (any id, as the original indexes its table without a bound); nullptr if none.
    virtual Unit* unit_by_id(std::uint16_t id) = 0;
    virtual const HeightMap& heightmap() = 0;
    // Yard opening needs the occupancy grid: whether the cells the open
    // (value != 0) or closed yard map occupies hold no other unit; then, after
    // the flag changed, claiming them and updating passability.
    virtual bool yard_cells_free(Unit& unit, std::int32_t value) = 0;
    virtual void claim_yard_cells(Unit& unit) = 0;
    // Side effects outside the simulation's state (speech, HUD, network) and
    // the cloak notification to path orders.
    virtual void on_activate_flags_changed(Unit& unit, std::uint8_t gained, std::uint8_t lost) = 0;
    // Script requests handled by other systems (the battle wires them).
    virtual std::uint32_t random_mod(std::uint32_t n) = 0;  // the battle RNG
    virtual void emit_sfx(Unit& unit, std::int32_t piece, std::int32_t type) = 0;
    virtual void explode_piece(Unit& unit, std::int32_t piece, std::int32_t type) = 0;
    virtual void attach_unit(Unit& unit, std::int32_t passenger, std::int32_t piece, std::int32_t mode) = 0;
    virtual void drop_unit(Unit& unit, std::int32_t passenger) = 0;
};

enum class UnitValue : std::int32_t {
    Activation = 1, StandingMoveOrders, StandingFireOrders, Health, InBuildStance, Busy, PieceXZ, PieceY,
    UnitXZ, UnitY, UnitHeight, XZAtan, XZHypot, Atan, Hypot, GroundHeight, BuildPercentLeft, YardOpen,
    BuggerOff, Armored,
};

// Piece origin relative to the unit (Z negated), from the piece's parent chain.
std::array<std::int32_t, 3> piece_local_offset(const Unit& unit, std::int32_t piece);

// Packs X and Z (16.16) into one value as scripts see it: X's integer part in
// the high half, Z's integer part in the low half (borrowing from X if negative).
std::int32_t pack_xz(std::int32_t x, std::int32_t z);

std::int32_t unit_get_value(Unit& self, UnitWorld& world, std::int32_t id, std::int32_t a, std::int32_t b);
void unit_set_value(Unit& self, UnitWorld& world, std::int32_t id, std::int32_t value);
// Sets (on) or clears the mask bits of the activate flags and runs the scripts for the changes.
void unit_set_activate(Unit& self, UnitWorld& world, std::uint8_t mask, bool on);

// Connects a unit to its script: piece callbacks drive the unit's pose, value
// callbacks read and write the unit, the rest goes to the world.
class UnitScriptHost : public ScriptHost {
public:
    UnitScriptHost(Unit& unit, UnitWorld& world) : unit_(unit), world_(world) {}
    void set_piece_move(std::int32_t piece, std::int32_t axis, std::int32_t value) override;
    void set_piece_turn(std::int32_t piece, std::int32_t axis, std::int32_t value) override;
    void set_piece_visible(std::int32_t piece, std::int32_t on) override;
    void set_piece_cached(std::int32_t piece, std::int32_t on) override;
    void set_piece_shaded(std::int32_t piece, std::int32_t on) override;
    std::int32_t get_piece_move(std::int32_t piece, std::int32_t axis) override;
    std::int32_t get_piece_turn(std::int32_t piece, std::int32_t axis) override;
    void legacy_explode(std::int32_t, std::int32_t, std::int32_t) override {}  // a no-op in the original
    void play_sound(std::int32_t) override {}  // a no-op in the original
    void emit_sfx(std::int32_t piece, std::int32_t type) override;
    void explode_piece(std::int32_t piece, std::int32_t type) override;
    void attach_unit(std::int32_t passenger, std::int32_t piece, std::int32_t mode) override;
    void drop_unit(std::int32_t passenger) override;
    void set_value(std::int32_t id, std::int32_t value) override;
    std::int32_t get_value(std::int32_t id, std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) override;
    std::int32_t get_transporting(std::int32_t index) override;
    std::int32_t get_carrier() override;
    std::uint32_t random_mod(std::uint32_t n) override;

private:
    Unit& unit_;
    UnitWorld& world_;
};

} // namespace sim
