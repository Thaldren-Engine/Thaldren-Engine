#include "sim/unit_tick.h"

namespace sim {
namespace {

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }

// The wind generator's script hears every wind change (queued, run by this step's script pass).
// The last argument pushed (the speed) is left in the stale word.
void notify_wind(Unit& unit, const UnitTickState& t, UnitTickEnvironment& env) {
    if (!(t.tick_type && t.tick_type->wind_generator > 0.0f) || !env.wind_changed || !unit.script) return;
    const auto speed = static_cast<std::uint32_t>(env.wind_speed) << 4;
    unit.script->call_with_args(unit.script->program().find_function("SetDirection"), nullptr, false, 1, env.wind_heading, 0, 0, 0);
    unit.script->call_with_args(unit.script->program().find_function("SetSpeed"), nullptr, false, 1, static_cast<std::int32_t>(speed), 0, 0, 0);
    env.stale_word = speed;
}

} // namespace

std::int32_t run_unit_pass(std::vector<Unit>& units, std::vector<UnitTickState>& ticks, std::vector<PlayerSlot>& players,
                           UnitTickEnvironment& env, UnitTickSystems& systems, std::int16_t& camera_countdown) {
    std::int32_t live = 0;
    for (std::size_t slot = 0; slot < players.size() && slot < 10; ++slot) {
        PlayerSlot& player = players[slot];
        if (!player.active || player.controller < 1 || player.controller > 3 || player.state == 10) continue;
        for (int i = player.first_unit; i <= player.last_unit; ++i) {
            Unit& u = units[static_cast<std::size_t>(i)];
            UnitTickState& t = ticks[static_cast<std::size_t>(i)];
            if (t.type_index == 0) continue;
            ++live;
            notify_wind(u, t, env);
            if (plays(player)) systems.weapons(u, env.stale_word);
            if (u.script) u.script->tick(1, env.stale_word);
            if (t.damaged_timer) --t.damaged_timer;
            if (t.transfer_holdoff) --t.transfer_holdoff;
            // A selected unit stays selected only while selectable, fully built,
            // not just transferred, and not inside a carrier that forbids it.
            if (u.flags & unit_tick_flags::kSelected) {
                const bool keep = (u.flags & unit_tick_flags::kSelectable) && u.build_fraction_left == 0.0f && t.transfer_holdoff == 0 &&
                                  (!u.carrier || (u.carrier->flags & unit_tick_flags::kCarrierSelectable));
                if (!keep) u.flags &= ~unit_tick_flags::kSelected;
            }
            if (env.game_tick % 30 == 0) {
                // Unsigned divide of the signed product; a "negative" quotient clamps to 0.
                const std::uint32_t num = static_cast<std::uint32_t>(static_cast<std::int32_t>(u.hit_points) * 100);
                const std::uint32_t den = u.type ? u.type->max_damage : 0;
                std::int32_t pct = den ? static_cast<std::int32_t>(num / den) : 0;  // the original faults on 0
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                t.prev_health_percent = t.health_percent;
                t.health_percent = static_cast<std::uint8_t>(pct);
            }
            const PlayerSlot& owner = players[static_cast<std::size_t>(t.owner)];
            if (plays(owner)) {
                if (env.water_damage_enabled && env.water_damage != 0 && env.game_tick % 30 == 0 &&
                    static_cast<std::int16_t>(u.pos[1] >> 16) <= static_cast<std::int16_t>(env.sea_level) &&
                    !(t.tick_type && (t.tick_type->type_flags & 0x1000))) {
                    systems.damage(nullptr, u, env.water_damage, 0xB, 0);
                    // The damage packet's kind byte lands in the word's low byte.
                    env.stale_word = (env.stale_word & 0xFFFFFF00u) | 0x0Bu;
                }
                const std::uint16_t heal = t.tick_type ? t.tick_type->heal_time : 0;
                // Unsigned comparison: negative hit points never heal.
                if (heal != 0 && static_cast<std::uint32_t>(static_cast<std::int32_t>(u.hit_points)) < (u.type ? u.type->max_damage : 0) &&
                    (env.game_tick & 7) == 0) {
                    systems.repair_heal(u, u, static_cast<float>(static_cast<std::int32_t>(heal) * 8 / 30));
                    env.stale_word = env.addresses.unit(u.id);  // a saved register: the unit's address
                }
                systems.primary_orders(u, env.stale_word);
                systems.secondary_orders(u, env.stale_word);
                if (t.has_motion) {
                    systems.motion(u, env.stale_word);
                    systems.settle_height(u, env.stale_word);
                }
            }
            if (u.flags & unit_tick_flags::kDying) systems.killed(u, t.last_damage_kind, env.stale_word);
        }
        if (env.broadcast_unit_states && plays(player)) systems.broadcast_player_units(static_cast<int>(slot), env.stale_word);
    }
    // Camera auto-follow (user interface; the original also skips it while Shift is held).
    if (env.camera_auto_follow && --camera_countdown < 1) {
        camera_countdown = 90;
        systems.camera_follow_step(env.stale_word);
    }
    return live;
}

} // namespace sim
