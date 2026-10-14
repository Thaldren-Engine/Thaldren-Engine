#pragma once
// Damage, healing and unit death (see docs/RE_MEMORY.md, "Damage, healing and death").
//
// Damage and death travel as small packets: the simulation builds one, applies
// it locally and hands it to the network, and a received packet is applied the
// same way.

#include <array>
#include <cstdint>
#include <vector>

#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Damage kinds with special handling.
namespace damage_kind {
constexpr std::int32_t kWeapon = 1, kParalyze = 2, kSelfDestruct = 3, kCorpseOnly = 7, kHeal = 10, kEnvironment = 0xB;
}

// Kill types (high nibble of the kill packet's last byte) are the damage kind
// that killed the unit; the low nibble is the wreck level.
using DamagePacket = std::array<std::uint8_t, 9>;   // 0x0B, target id, attacker id, amount, heading byte, kind
using KilledPacket = std::array<std::uint8_t, 11>;  // 0x0C, unit id, attacker net id, attacker id, severity, kind|wreck

struct CombatEnvironment {
    std::uint8_t local_player = 0;       // slot of the player at this machine
    std::uint8_t local_view = 0;         // state index whose view this machine shows
    std::int32_t game_mode = 0;          // nonzero: a commander's death ends its player
    std::int32_t ai_difficulty = 0;      // 0 easy, 1 medium, others hard
    bool fog_of_war = false;
    const UnitTypeInfo* empty_type = nullptr;          // type given to dead units
    const UnitTickTypeInfo* empty_tick_type = nullptr;
};

// Systems damage and death reach outside this module.
class CombatSystems {
public:
    virtual ~CombatSystems() = default;
    virtual void retarget(Unit* attacker, Unit& victim, std::uint16_t amount) = 0;  // the victim may answer the attack
    virtual void add_paralysis(Unit& victim, std::uint16_t amount) = 0;            // via the victim's orders
    virtual bool pay_energy(Unit& worker, float amount) = 0;                       // false if stalled
    virtual void stats_activity(std::int32_t kind) = 0;                            // local statistics display
    virtual void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) = 0;
    // Death.
    virtual void reveal_dead_unit(Unit& unit) = 0;
    virtual void victory_unit_destroyed(Unit& unit) = 0;
    virtual void clear_orders(Unit& unit) = 0;
    virtual void forget_speech(Unit& unit) = 0;
    virtual void leave_group(Unit& unit) = 0;
    virtual void remove_projectiles(Unit& unit) = 0;
    virtual void detach_from_carrier(Unit& unit) = 0;  // must unlink it from the carrier's cargo
    virtual void clear_footprint(Unit& unit) = 0;
    virtual void forget_fog(Unit& unit) = 0;
    virtual void kill_counted(std::uint8_t attacker_slot, Unit& victim) = 0;  // kill-lead messages, kill flash
    virtual void detonate(Unit& unit, bool self_destruct) = 0;
    virtual void spawn_wreck(Unit& unit, std::int32_t level, bool burn) = 0;
    virtual void release_unit(Unit& unit) = 0;
    virtual void release_motion(Unit& unit) = 0;
    virtual void player_out_of_units(int player) = 0;
    virtual void commander_died(int player) = 0;
};

struct Combat {
    std::vector<Unit>& units;  // index = unit id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    CombatEnvironment& env;
    CombatSystems& systems;
};

// Network id of a player slot (-1 for none) and the reverse lookup (10 for none).
std::int32_t player_net_id(const std::vector<PlayerSlot>& players, std::uint8_t slot);
std::uint8_t player_slot_by_net_id(const std::vector<PlayerSlot>& players, std::int32_t net_id);

// Deals `amount` of `kind` (scaled by armor and the attacker's kills unless it
// heals), then tells the network if the target's owner is watched.
void apply_damage(Combat& c, Unit* attacker, Unit& target, std::int32_t amount, std::int32_t kind, std::int32_t heading);
void apply_damage_packet(Combat& c, const DamagePacket& packet);
// One step of repair or self-heal at `rate`; false if already whole or out of energy.
bool repair_heal(Combat& c, Unit& worker, Unit& target, float rate);
// Kills a dying unit: runs its death script, then tells the network and applies the kill.
// `stale_word` is the caller's stack word the original reads as the uninitialised wreck
// level (it matters when the script has no Killed function); it receives the final wreck level.
void kill_unit(Combat& c, Unit& unit, std::int32_t kind, std::uint32_t& stale_word);
void apply_killed_packet(Combat& c, const KilledPacket& packet, bool skip_script);

} // namespace sim
