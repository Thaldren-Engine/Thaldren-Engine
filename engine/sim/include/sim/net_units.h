#pragma once
// Network play inside the battle (docs/RE_MEMORY.md, "Network and sync"): each machine
// simulates only its own players' units; the units of players on other machines
// (controller 3) run their scripts and are driven by received packets. The Battle members
// that build and apply those packets are declared in sim/battle.h ("Network play") and
// implemented in engine/sim/src/net_units.cpp.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

class OrderGoal;
struct MissionOrder;

// Inputs the original takes from fresh heap memory: a remote aircraft's goal received from
// the network does not set its position unless the sender's goal had one (flag 0x20), nor
// its formation offset.
struct NetUnitInputs {
    std::array<std::int32_t, 3> fresh_goal_pos{};
    std::int32_t fresh_goal_offset = 0;
};

// Goals of remote aircraft (owned here: the original's remote flight goal owns them itself).
struct NetUnitState {
    std::vector<std::shared_ptr<OrderGoal>> remote_goals;  // by unit id
    std::vector<const struct WeaponType*> weapons;         // by weapon index (fire packets name weapons by index)
    std::string fault;                                     // where the original would have crashed
    std::uint64_t states_written = 0, states_read = 0, full_states = 0, units_made = 0;
};

class BitWriter;
class BitReader;

// Plain forms of what the unit state packets carry (the battle and the harnesses use them).
struct NetPointGoal {      // 0x44DDC0 / 0x44E080
    std::uint16_t flags = 0;
    std::int16_t piece = 0, radius = 0, altitude = 0, heading = 0;
    std::uint16_t link_id = 0;
    std::array<std::int32_t, 3> pos{};
};
struct NetManoeuvreGoal {  // 0x44E930 / 0x44E9C0
    std::uint16_t flags = 0;
    std::array<std::int32_t, 3> pos{}, vel{};
    std::uint16_t target_heading = 0;
};
void write_point_goal_state(BitWriter& w, const NetPointGoal& g);
// Fields the stream does not carry are 0, except the position (left as it is).
void read_point_goal_state(BitReader& r, NetPointGoal& g);
void write_manoeuvre_goal_state(BitWriter& w, const NetManoeuvreGoal& g);
void read_manoeuvre_goal_state(BitReader& r, NetManoeuvreGoal& g);
// Route goal (0x44F4A0): the blocked bit, then up to three waypoints (none without a route).
void write_route_state(BitWriter& w, bool blocked, std::uint8_t route_flags, std::int32_t count, const std::uint32_t* points);
// The route goal's flags after sending: changed bit off, sent copy of the blocked bit.
std::uint8_t route_flags_after_send(std::uint8_t route_flags, std::uint8_t motion_flags);
// Remote route goal (0x44F5C0): blocked bit, count, waypoints.
void read_route_state(BitReader& r, bool& blocked, std::int32_t& count, std::array<std::uint32_t, 3>& points);

// One unit's whole state (0x48B200 writes it, 0x48B3F0 applies it).
struct NetWholeState {
    std::uint16_t type = 0;
    std::int16_t hit_points = 0;
    float fraction_left = 0.0f;
    std::uint8_t activate = 0;
    std::uint32_t layer = 0;
    bool carried = false;
    std::uint16_t carrier_id = 0;
    std::int8_t piece = 0;
    std::array<std::int32_t, 3> pos{};
    std::uint16_t heading = 0, bank = 0, pitch = 0;
    bool has_motion = false;
    std::int32_t speed = 0;
};
void write_whole_state(BitWriter& w, int type_bits, const NetWholeState& s);
// The receiving unit as the whole state changes it, in the original's order.
class WholeStateTarget {
public:
    virtual ~WholeStateTarget() = default;
    virtual std::uint16_t type() = 0;
    virtual void make(std::uint16_t type) = 0;  // packet 9 built from the unit's own position
    virtual void add_flags(std::uint32_t bits) = 0;
    virtual void set_hit_points(std::int16_t hp) = 0;
    virtual float fraction_left() = 0;
    virtual void set_fraction_left(float f) = 0;
    virtual void activate(std::uint8_t mask, bool on) = 0;
    virtual bool carried() = 0;
    virtual void transport(std::uint16_t carrier, std::uint8_t piece, std::uint8_t layer) = 0;  // carrier 0: dropped
    // The new position's cells against the current ones (footprint cell rule) and layer.
    virtual bool same_cells(std::int32_t x, std::int32_t z, std::uint32_t layer) = 0;
    virtual void set_position(const std::array<std::int32_t, 3>& pos) = 0;
    virtual void move_cells(const std::array<std::int32_t, 3>& pos, std::uint32_t layer) = 0;  // release, store, cells, layer, claim, sight
    virtual void set_angles(std::uint16_t pitch, std::uint16_t heading, std::uint16_t bank) = 0;
    virtual bool has_motion() = 0;
    virtual void set_speed(std::int32_t speed) = 0;
};
void apply_whole_state(BitReader& r, int type_bits, WholeStateTarget& unit);

// Bits of a unit type index in the state packets (0x42D65B): the bit length of the type count.
int unit_type_bits(std::size_t type_count);

// The sender's encoding of a build fraction (1 + the integer part of fraction x 254; 0 for a
// finished unit) and the receiver's decoding (x 1/255 as a float).
std::uint32_t encode_build_fraction(float fraction_left);
double decode_build_fraction(std::uint32_t byte);

}  // namespace sim
