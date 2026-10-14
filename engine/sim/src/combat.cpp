#include "sim/combat.h"

#include "sim/trig.h"
#include "sim/x87.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

constexpr u32 kDying = unit_tick_flags::kDying;

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }

// Unit by packet id; 0 is no unit.
Unit* unit_at(Combat& c, u16 id) { return id != 0 && id < c.units.size() ? &c.units[id] : nullptr; }

u16 get16(const u8* p) { return static_cast<u16>(p[0] | p[1] << 8); }
void put16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

// ASCII case-insensitive equality (the C runtime's stricmp in the "C" locale).
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool is_commander(Combat& c, const UnitTickState& t) {
    return t.tick_type && same_name(c.players[static_cast<std::size_t>(t.owner)].commander_name, t.tick_type->name);
}

std::int32_t first_human_net_id(const std::vector<PlayerSlot>& players) {
    for (std::size_t s = 0; s < players.size() && s < 10; ++s)
        if (plays(players[s])) return players[s].net_id;
    return -1;
}

void run_script(Unit& u, const char* name, bool run_now, i32 argc, i32 a, i32 b) {
    if (u.script) u.script->call_with_args(u.script->program().find_function(name), nullptr, run_now, argc, a, b, 0, 0);
}

// (u32)(hp * 100) / max with an unsigned divide.
u32 hp_percent_raw(i32 hp100, u32 max) { return max ? static_cast<u32>(hp100) / max : 0; }  // the original faults on 0

} // namespace

std::int32_t player_net_id(const std::vector<PlayerSlot>& players, std::uint8_t slot) {
    if (slot == 10 || slot >= players.size()) return -1;
    return players[slot].controller != 0 ? players[slot].net_id : -1;
}

std::uint8_t player_slot_by_net_id(const std::vector<PlayerSlot>& players, std::int32_t net_id) {
    if (net_id == -1) return 10;
    for (u8 s = 0; s < 10 && s < players.size(); ++s)
        if (player_net_id(players, s) == net_id) return s;
    return 10;
}

void apply_damage(Combat& c, Unit* attacker, Unit& target, i32 amount, i32 kind, i32 heading) {
    i32 dealt = amount;
    if (kind != damage_kind::kHeal) {
        const UnitTickState& tt = c.ticks[target.id];
        if ((target.activate_flags & unit_flags::kArmored) && amount < 30000 && tt.tick_type)
            amount = static_cast<i32>((static_cast<i64>(tt.tick_type->damage_modifier) * amount) >> 16);
        // Veterancy: each five kills of the *target* take 4% off (at most five times).
        i32 tier = tt.kill_count / 5;
        if (tier > 5) tier = 5;
        const u32 scaled = (static_cast<u32>(25 - tier) * static_cast<u32>(amount)) << 2;
        dealt = static_cast<i32>(scaled) / 100;
    }
    DamagePacket p{};
    p[0] = 0x0B;
    put16(&p[1], target.id);
    put16(&p[3], attacker ? attacker->id : 0);
    put16(&p[5], static_cast<u16>(dealt));
    p[7] = static_cast<u8>(static_cast<u32>(heading) >> 8);
    p[8] = static_cast<u8>(kind);
    apply_damage_packet(c, p);
    const PlayerSlot& owner = c.players[static_cast<std::size_t>(c.ticks[target.id].owner)];
    if (owner.active && owner.controller == 3 && kind != damage_kind::kEnvironment) {
        const i32 net = attacker ? c.players[static_cast<std::size_t>(c.ticks[attacker->id].owner)].net_id : first_human_net_id(c.players);
        c.systems.network_send(net, p.data(), 9);
    }
}

void apply_damage_packet(Combat& c, const DamagePacket& p) {
    Unit* v = unit_at(c, get16(&p[1]));
    Unit* a = unit_at(c, get16(&p[3]));
    if (!v || !(v->flags & unit_flags::kAlive) || (v->flags & kDying)) return;
    UnitTickState& vt = c.ticks[v->id];
    const u16 amount = get16(&p[5]);
    const u8 kind = p[8];
    const u32 max = v->type ? v->type->max_damage : 0;
    if (kind == damage_kind::kHeal) {
        // Unsigned clamp: a heal that wraps the sum past the maximum also fills up.
        const i32 hp = v->hit_points + amount;
        v->hit_points = static_cast<std::int16_t>(static_cast<u32>(hp) >= max ? max : static_cast<u32>(hp));
        return;
    }
    vt.damaged_timer = 0xF0;
    if (kind != damage_kind::kEnvironment) c.systems.retarget(a, *v, amount);
    vt.last_damage_kind = kind;
    if (a) {
        const UnitTickState& at = c.ticks[a->id];
        vt.last_attacker = a;
        vt.last_attacker_slot = static_cast<u8>(at.owner);
        if (static_cast<u8>(at.owner) == c.env.local_player || static_cast<u8>(vt.owner) == c.env.local_player)
            c.systems.stats_activity(1);
    }
    const PlayerSlot& owner = c.players[static_cast<std::size_t>(vt.owner)];
    if (kind == damage_kind::kParalyze) {
        if ((v->flags & unit_flags::kAlive) && !(v->flags & kDying) && plays(owner) &&
            !(vt.tick_type && (vt.tick_type->type_flags & 0x4000000)))
            c.systems.add_paralysis(*v, amount);
        return;
    }
    v->hit_points = static_cast<std::int16_t>(v->hit_points - amount);
    if (v->hit_points <= 0) {
        // Units of playing owners die in their next unit pass; others just stay at 0.
        if (plays(owner)) {
            v->flags |= kDying;
            return;
        }
        v->hit_points = 0;
    }
    if (kind == damage_kind::kWeapon) {
        const Angle dir = static_cast<Angle>(p[7] << 8);
        run_script(*v, "HitByWeapon", false, 2, trig::mul_cos(dir, 400), trig::mul_sin(dir, 400));
        i32 pct = static_cast<i32>(hp_percent_raw(v->hit_points * 100, max));
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        run_script(*v, "TakeDamage", false, 1, pct, 0);
    }
}

bool repair_heal(Combat& c, Unit& worker, Unit& target, float rate) {
    const UnitTickState& tt = c.ticks[target.id];
    const i32 max = static_cast<i32>(target.type ? target.type->max_damage : 0);
    if (target.hit_points >= max) return false;
    // Every step rounds to double, as on an x87 at 53-bit precision; then truncation.
    const double build_time = tt.tick_type ? static_cast<double>(tt.tick_type->build_time) : 0.0;
    const double energy_cost = tt.tick_type ? static_cast<double>(tt.tick_type->build_cost_energy) : 0.0;
    i32 hp = x87::ftol32((static_cast<double>(max) * static_cast<double>(rate) - 1.0) / build_time - -1.0);
    i32 energy = x87::ftol32((energy_cost * static_cast<double>(rate) - 1.0) / build_time - -1.0);
    if (hp >= 1) hp = 1;
    if (energy >= 1) energy = 1;
    if (!c.systems.pay_energy(worker, static_cast<float>(energy))) return false;
    apply_damage(c, &worker, target, hp, damage_kind::kHeal, 0);
    return true;
}

void kill_unit(Combat& c, Unit& u, i32 kind, u32& stale_word) {
    if (!(u.flags & unit_flags::kAlive)) return;
    UnitTickState& t = c.ticks[u.id];
    PlayerSlot& owner = c.players[static_cast<std::size_t>(t.owner)];
    const bool commander = is_commander(c, t);
    if (commander) owner.flags = static_cast<u16>(owner.flags & ~1u);
    i32 severity = 0;
    i32 wreck = static_cast<i32>(stale_word);  // uninitialised in the original: whatever the caller left there
    if (kind == damage_kind::kCorpseOnly) {
        wreck = 1;
    } else if (kind == 4 || kind == 5 || kind == 9 || u.hit_points > 0) {
        wreck = 0;
    } else {
        const u32 max = u.type ? u.type->max_damage : 0;
        const u32 sum = hp_percent_raw(u.hit_points * -100, max) + t.prev_health_percent;
        severity = static_cast<i32>(sum) / 2;
        if (severity < 1) severity = 1;
        if (severity > 100) severity = 100;
        if (u.script) u.script->call_in_out(u.script->program().find_function("Killed"), &severity, &wreck, nullptr, nullptr);
    }
    if (u.build_fraction_left != 0.0f) wreck = 0;
    stale_word = static_cast<u32>(wreck);
    KilledPacket p{};
    p[0] = 0x0C;
    put16(&p[1], u.id);
    const u32 net = static_cast<u32>(player_net_id(c.players, t.last_attacker_slot));
    for (int i = 0; i < 4; ++i) p[3 + static_cast<std::size_t>(i)] = static_cast<u8>(net >> (8 * i));
    put16(&p[7], t.last_attacker ? t.last_attacker->id : 0);
    p[9] = static_cast<u8>(severity);
    p[10] = static_cast<u8>((kind << 4) | (wreck & 0xF));
    if (plays(owner)) c.systems.network_send(owner.net_id, p.data(), 11);
    apply_killed_packet(c, p, true);
    if (commander && c.env.game_mode != 0 && plays(owner)) c.systems.commander_died(t.owner);
}

void apply_killed_packet(Combat& c, const KilledPacket& p, bool skip_script) {
    Unit* up = unit_at(c, get16(&p[1]));
    if (!up || !(up->flags & unit_flags::kAlive)) return;
    Unit& u = *up;
    UnitTickState& t = c.ticks[u.id];
    PlayerSlot& owner = c.players[static_cast<std::size_t>(t.owner)];
    if (owner.state == c.env.local_view) c.systems.reveal_dead_unit(u);
    t.last_attacker = unit_at(c, get16(&p[7]));
    t.last_attacker_slot = player_slot_by_net_id(
        c.players, static_cast<i32>(static_cast<u32>(p[3]) | static_cast<u32>(p[4]) << 8 | static_cast<u32>(p[5]) << 16 | static_cast<u32>(p[6]) << 24));
    c.systems.victory_unit_destroyed(u);
    c.systems.clear_orders(u);
    c.systems.forget_speech(u);
    c.systems.leave_group(u);
    c.systems.remove_projectiles(u);
    if (u.carrier) c.systems.detach_from_carrier(u);
    const u8 type = static_cast<u8>(p[10] & 0xF0);
    while (Unit* cargo = u.cargo_head) {
        apply_damage(c, t.last_attacker, *cargo, 30000, type == 0x30 ? damage_kind::kSelfDestruct : 6, 0);
        c.systems.detach_from_carrier(*cargo);
        if (u.cargo_head == cargo) break;  // the original would spin here forever
    }
    c.systems.clear_footprint(u);
    if (c.env.fog_of_war) c.systems.forget_fog(u);
    const auto severity = static_cast<std::int8_t>(p[9]);
    if (!skip_script && severity > 0) run_script(u, "Killed", true, 1, severity, 0);

    // Statistics.
    const u8 as = t.last_attacker_slot;
    const bool built = u.build_fraction_left == 0.0f;
    bool counted = false;
    auto count_kill = [&] {
        ++owner.units_lost;
        if (as != 10 && built && static_cast<u8>(t.owner) != as) ++c.players[as].units_killed;
        if (is_commander(c, t)) {
            if (as != 10) ++c.players[as].commanders_killed;
            ++owner.commanders_lost;
        }
        if (t.last_attacker && built && static_cast<u8>(t.owner) != as) ++c.ticks[t.last_attacker->id].kill_count;
        if (as == c.env.local_player) c.systems.stats_activity(5);
        counted = true;
    };
    switch (type >> 4) {
    case 1:
    case 6: count_kill(); break;
    case 5:
        if (as != 10 && as != static_cast<u8>(t.owner)) count_kill();
        break;
    case 3:
        // Self-destruction counts as a loss unless the local player sees the owner's units anyway.
        if (!(owner.state < 10 && c.players[c.env.local_player].shares_vision[owner.state])) {
            ++owner.units_lost;
            if (is_commander(c, t)) ++owner.commanders_lost;
            counted = true;
        }
        break;
    default: break;
    }
    if (counted && as != 10) c.systems.kill_counted(as, u);

    // Reclaiming a wreck-to-be pays its unbuilt share of metal to the reclaimer.
    if (type == 0x50 && t.last_attacker) {
        UnitTickState& at = c.ticks[t.last_attacker->id];
        const double metal = (1.0 - static_cast<double>(u.build_fraction_left)) *
                             static_cast<double>(t.tick_type ? t.tick_type->build_cost_metal : 0.0f);
        const PlayerSlot& payee = c.players[static_cast<std::size_t>(at.owner)];
        const double make = at.metal_make;
        if (payee.active && payee.controller == 2 && c.env.ai_difficulty == 0)
            at.metal_make = static_cast<float>(make - metal * -0.5);
        else if (payee.active && payee.controller == 2 && c.env.ai_difficulty == 1)
            at.metal_make = static_cast<float>(make - metal * -0.7);
        else
            at.metal_make = static_cast<float>(make + metal);
    }
    if (severity > 0 && built) c.systems.detonate(u, type == 0x30);
    if (p[10] & 0xF) c.systems.spawn_wreck(u, p[10] & 0xF, type != 0x70);

    // Teardown.
    c.systems.release_unit(u);
    u.script.reset();
    u.pose = PoseState{};
    if (t.has_motion) {
        c.systems.release_motion(u);
        t.has_motion = false;
    }
    t.type_index = 0;
    u.flags &= ~(unit_flags::kAlive | 0x30u);
    u.type = c.env.empty_type;
    t.tick_type = c.env.empty_tick_type;
    if (--owner.alive_count == 0) c.systems.player_out_of_units(t.owner);
}

} // namespace sim
