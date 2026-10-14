// Battle start (docs/RE_MEMORY.md, "Battle start").

#include "sim/battle_start.h"

#include <algorithm>

#include "sim/battle.h"

namespace sim {

bool start_battle(Battle& b, const BattleStart& start, std::string& error) {
    // Provisional set-up; replaced by the original's sequence.
    int count = 0;
    for (const StartPlayer& p : start.players) count += p.kind != PlayerKind::Empty;
    if (!count) {
        error = "no players";
        return false;
    }
    b.battle_rng().seed(start.battle_rng_seed);
    b.crt_rand.set_state(start.crt_rand_state);
    b.settings.unit_limit = start.unit_limit;
    b.settings.player_count = static_cast<std::uint16_t>(count);
    b.settings.units_per_player = start.unit_limit;
    b.settings.game_mode = start.commander_mode;
    b.settings.fog_of_war = start.fog_of_war;
    b.settings.explore = start.explore;
    b.settings.true_sight = start.true_sight;
    b.settings.target_everything = start.target_everything;
    b.settings.command_mode = start.command_mode;
    b.settings.ai_difficulty = start.ai_difficulty;
    b.settings.local_player = start.local_player;
    b.settings.local_view = start.local_player;
    b.settings.net_game = start.net_game;
    b.init_unit_table(static_cast<std::size_t>(start.unit_limit) * 10 + 1);
    for (int s = 0; s < 10; ++s) {
        const StartPlayer& sp = start.players[static_cast<std::size_t>(s)];
        PlayerSlot& slot = b.players[static_cast<std::size_t>(s)];
        b.assign_unit_range(s, 1 + s * start.unit_limit, (s + 1) * start.unit_limit);
        if (sp.kind == PlayerKind::Empty) continue;
        slot.active = true;
        slot.controller = sp.kind == PlayerKind::Local ? 1 : 2;
        slot.net_id = sp.net_id;
        slot.state = static_cast<std::uint8_t>(s);
        b.init_player_economy(s);
        set_storage_bonus(slot, b.player_economy[static_cast<std::size_t>(s)], static_cast<std::int32_t>(start.start_metal), static_cast<std::int32_t>(start.start_energy));
        slot.energy_stored = start.start_energy;
        slot.metal_stored = start.start_metal;
    }
    for (int s = 0; s < 10; ++s)
        for (int o = 0; o < 10; ++o) {
            const StartPlayer& a = start.players[static_cast<std::size_t>(s)];
            const StartPlayer& c = start.players[static_cast<std::size_t>(o)];
            b.alliances[static_cast<std::size_t>(s)][static_cast<std::size_t>(o)] =
                s == o || (a.ally_group != 0xFF && a.ally_group == c.ally_group) ? 1 : 0;
        }
    if (b.settings.fog_of_war) b.init_sight();
    b.init_routes();
    for (int s = 0; s < 10; ++s) {
        const StartPlayer& sp = start.players[static_cast<std::size_t>(s)];
        if (sp.kind == PlayerKind::Empty || !sp.commander_type || sp.start_position < 0) continue;
        const auto& at = start.start_positions[static_cast<std::size_t>(sp.start_position)];
        const std::int32_t gy = std::max<std::int32_t>(b.height_map().sample(static_cast<std::int16_t>(at[0]), static_cast<std::int16_t>(at[1])), 0);
        b.create_unit(s, sp.commander_type, at[0] << 16, gy << 16, at[1] << 16, true, 1);
    }
    b.start_players();
    if (b.settings.fog_of_war) b.rebuild_sight(true);
    return true;
}

}  // namespace sim
