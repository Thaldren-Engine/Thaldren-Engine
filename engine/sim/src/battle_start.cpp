// Battle start (docs/RE_MEMORY.md, "Battle start").

#include "sim/battle_start.h"

#include <algorithm>

#include "sim/battle.h"

namespace sim {

namespace {

bool playing(const PlayerSlot& p) { return p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10; }

const StartRule* find_rule(const BattleStart& s, std::int32_t id, StartRule& legacy) {
    if (!s.start_rules.empty()) {
        for (const StartRule& r : s.start_rules)
            if (r.id == id) return &r;
        return nullptr;
    }
    if (id < 0 || id >= s.start_position_count || id >= 10) return nullptr;
    legacy.id = id;
    legacy.x = static_cast<std::int16_t>(s.start_positions[static_cast<std::size_t>(id)][0]);
    legacy.z = static_cast<std::int16_t>(s.start_positions[static_cast<std::size_t>(id)][1]);
    return &legacy;
}

std::uint16_t type_by_name(Battle& b, const std::string& name) {
    for (std::size_t k = 1; k < b.types.size(); ++k) {
        const std::string& t = b.types[k].tick.name;
        if (t.size() != name.size() || t.empty()) continue;
        bool same = true;
        for (std::size_t i = 0; i < t.size() && same; ++i) {
            char x = t[i], y = name[i];
            if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
            if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
            same = x == y;
        }
        if (same) return static_cast<std::uint16_t>(k);
    }
    return 0;
}

std::int32_t start_energy(const BattleStart& s, int k) {
    const StartPlayer& sp = s.players[static_cast<std::size_t>(k)];
    return sp.energy >= 0 ? sp.energy : static_cast<std::int32_t>(s.start_energy);
}
std::int32_t start_metal(const BattleStart& s, int k) {
    const StartPlayer& sp = s.players[static_cast<std::size_t>(k)];
    return sp.metal >= 0 ? sp.metal : static_cast<std::int32_t>(s.start_metal);
}

// Starting resources (0x465E30), for every record: skirmish the player's settings, network
// games the *local* player's side values x 100 for everyone.
void init_resources(Battle& b, const BattleStart& s) {
    for (int k = 0; k < 10; ++k) {
        PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
        if (s.provider == 3) {
            const VictorySide& side = b.victory.sides[s.local_player < 10 ? s.local_player : static_cast<std::size_t>(k)];
            p.energy_stored = static_cast<float>(static_cast<std::uint32_t>(side.energy_start) * 100u);
            p.metal_stored = static_cast<float>(static_cast<std::uint32_t>(side.metal_start) * 100u);
        } else if (s.provider == 2) {
            p.energy_stored = static_cast<float>(start_energy(s, k));
            p.metal_stored = static_cast<float>(start_metal(s, k));
        }
    }
}

}  // namespace

void shuffle_start_positions(std::vector<std::int32_t>& ids, std::uint32_t& state) {
    Lcg15 rand;
    rand.set_state(state);
    bool shuffle = true;
    if (ids.size() < 3) shuffle = (static_cast<std::int64_t>(rand.next()) * 2) / 0x8000 != 0;
    if (shuffle)
        for (std::size_t p = 1; p < ids.size(); ++p) {
            const std::uint32_t j = (rand.next() & 0x7FFF) % static_cast<std::uint32_t>(p);
            std::swap(ids[p], ids[j]);
        }
    state = rand.state();
}

bool start_battle(Battle& b, const BattleStart& start, std::string& error) {
    int count = 0;
    for (const StartPlayer& p : start.players) count += p.kind != PlayerKind::Empty;
    if (!count) {
        error = "no players";
        return false;
    }
    // 0x497180: the battle generator from the clock, the loading thread's rand() from time().
    b.battle_rng().seed(start.battle_rng_seed);
    b.crt_rand.set_state(start.loading_rand_seed);  // battle loading runs on its own thread
    // Options.
    BattleSettings& st = b.settings;
    st.provider = start.provider;
    st.unit_limit = start.unit_limit;
    st.units_per_player = start.unit_limit;
    st.game_mode = start.commander_mode;
    st.fog_of_war = start.fog_of_war;
    st.explore = start.explore;
    st.true_sight = start.true_sight;
    st.target_everything = start.target_everything;
    st.command_mode = start.command_mode;
    st.ai_difficulty = start.ai_difficulty;
    st.local_player = start.local_player;
    st.local_view = start.local_player;
    st.net_game = start.net_game;
    // Player records and the unit table: ten ranges of `unit_limit` entries.
    b.init_unit_table(static_cast<std::size_t>(start.unit_limit) * 10 + 1);
    int last_player = 0;
    for (int s = 0; s < 10; ++s) {
        const StartPlayer& sp = start.players[static_cast<std::size_t>(s)];
        PlayerSlot& slot = b.players[static_cast<std::size_t>(s)];
        b.assign_unit_range(s, 1 + s * start.unit_limit, (s + 1) * start.unit_limit);
        slot.state = static_cast<std::uint8_t>(s);
        slot.net_id = sp.net_id;
        if (sp.kind == PlayerKind::Empty) continue;
        last_player = s + 1;
        slot.active = true;
        slot.controller = static_cast<std::uint8_t>(sp.kind);
        slot.commander_name = sp.commander_name;
        VictorySide& side = b.victory.sides[static_cast<std::size_t>(s)];
        side.watching = sp.watcher;
        side.allied_victory = sp.allied_victory;
        side.human = sp.kind != PlayerKind::Computer;
        side.energy_start = static_cast<std::uint16_t>(start_energy(start, s) / 100);
        side.metal_start = static_cast<std::uint16_t>(start_metal(start, s) / 100);
    }
    st.player_count = static_cast<std::uint16_t>(last_player);
    for (int s = 0; s < 10; ++s)
        for (int o = 0; o < 10; ++o) {
            const StartPlayer& a = start.players[static_cast<std::size_t>(s)];
            const StartPlayer& c = start.players[static_cast<std::size_t>(o)];
            b.alliances[static_cast<std::size_t>(s)][static_cast<std::size_t>(o)] =
                s == o || (a.ally_group != 0xFF && a.ally_group == c.ally_group) ? 1 : 0;
        }
    // Map loading (0x4917D0): passability, the wind once at step 0 (denominator 5000, next
    // change at 0), the meteor schedule, every record with a controller set up (0x464700:
    // economy, sight counts, controller and unit table), the end of the game cleared.
    const bool sight = !b.sight_tables.masks.empty();
    if (sight) b.init_sight();
    b.init_routes();
    b.wind.denominator = 5000;
    b.wind.next_change_tick = 0;
    b.step_wind();
    b.meteors.reset(b.meteors.weapon);
    for (int s = 0; s < 10; ++s)
        if (b.players[static_cast<std::size_t>(s)].controller != 0) b.init_player_economy(s);
    b.start_players();
    b.end_game.flags = static_cast<std::uint16_t>(b.end_game.flags & ~(end_game_flags::kEnding | end_game_flags::kWon |
                                                                       end_game_flags::kWonMovie | end_game_flags::kLost));
    b.end_game.countdown = -1;
    // Start positions and commanders.
    StartRule legacy;
    if (start.provider == 3) {
        const VictorySide& local = b.victory.sides[start.local_player < 10 ? start.local_player : 0];
        for (int s = 0; s < 10; ++s) {
            PlayerSlot& p = b.players[static_cast<std::size_t>(s)];
            if (!p.active || (p.controller != 1 && p.controller != 2)) continue;
            // A random spot first (two battle generator draws), then the assigned position.
            const std::int32_t world_w = b.map.width * 16, world_h = b.map.height * 16;
            std::int32_t x = static_cast<std::int32_t>((b.battle_rng().next_mod(static_cast<std::uint32_t>(world_w - 0xA0)) + 0x50) << 16);
            std::int32_t z = static_cast<std::int32_t>((b.battle_rng().next_mod(static_cast<std::uint32_t>(world_h - 0xA0)) + 0x50) << 16);
            const StartPlayer& sp = start.players[static_cast<std::size_t>(s)];
            if (sp.watcher) continue;
            if (const StartRule* r = find_rule(start, sp.start_position, legacy)) {
                x = static_cast<std::int32_t>(r->x) << 16;
                z = static_cast<std::int32_t>(r->z) << 16;
            }
            const std::uint16_t type = sp.commander_type ? sp.commander_type : type_by_name(b, sp.commander_name);
            if (type) b.create_unit(p.state, type, x, 0, z, true, 1);
            set_storage_bonus(p, b.player_economy[static_cast<std::size_t>(s)], std::max<std::int32_t>(local.metal_start * 100, 200),
                              std::max<std::int32_t>(local.energy_start * 100, 200));
        }
    } else if (start.provider == 2) {
        std::vector<std::int32_t> ids;
        std::vector<int> order;
        for (int s = 0; s < 10; ++s)
            if (playing(b.players[static_cast<std::size_t>(s)])) {
                order.push_back(s);
                const std::int32_t want = start.players[static_cast<std::size_t>(s)].start_position;
                ids.push_back(start.fixed_locations || want < 0 ? s : want);
            }
        if (!start.fixed_locations) {
            std::uint32_t state = b.crt_rand.state();
            shuffle_start_positions(ids, state);
            b.crt_rand.set_state(state);
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            // 0x496EE0: storage bonus (at least 200), the commander at the position (y 0).
            const int s = order[k];
            PlayerSlot& p = b.players[static_cast<std::size_t>(s)];
            const StartPlayer& sp = start.players[static_cast<std::size_t>(s)];
            set_storage_bonus(p, b.player_economy[static_cast<std::size_t>(s)], std::max(start_metal(start, s), 200),
                              std::max(start_energy(start, s), 200));
            const StartRule* r = find_rule(start, ids[k], legacy);
            if (!r) {
                error = "could not find start position " + std::to_string(ids[k]);
                return false;
            }
            const std::uint16_t type = sp.commander_type ? sp.commander_type : type_by_name(b, sp.commander_name);
            if (type) b.create_unit(s, type, static_cast<std::int32_t>(r->x) << 16, 0, static_cast<std::int32_t>(r->z) << 16, true, 1);
        }
        init_resources(b, start);
    }
    if (sight) b.rebuild_sight(true);
    // One players' stage at step 0, then the stored amounts are set again.
    b.players_stage();
    init_resources(b, start);
    // From here on the simulation thread's rand() is used.
    b.crt_rand.set_state(start.crt_rand_state);
    return true;
}

}  // namespace sim
