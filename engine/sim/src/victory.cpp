// Victory, defeat and the end of the game (docs/RE_MEMORY.md, "Victory").

#include <cstring>

#include "sim/battle.h"

namespace sim {

namespace {

bool playing(const PlayerSlot& p) { return p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10; }
bool has_units_or_none_made(const PlayerSlot& p) { return p.alive_count != 0 || p.units_created == 0; }

// The end countdown: started at 4, counted down each turn of the local player; true when it
// ran out now.
bool count_down(EndGameState& e) {
    if (e.countdown < 0) {
        e.countdown = 4;
        return false;
    }
    --e.countdown;
    return e.countdown < 0;
}

}  // namespace

bool Battle::victory_met() const {
    // Victory (0x490230).
    if (settings.provider == 2) {
        const std::size_t l = settings.local_player;
        for (int o = 0; o < 10; ++o) {
            if (o == settings.local_player || alliances[l][static_cast<std::size_t>(o)] != 0) continue;
            if (players[static_cast<std::size_t>(o)].alive_count != 0) return false;
        }
        return true;
    }
    if (settings.provider == 3) {
        // Allied victory (0x490080).
        if (settings.game_mode == 2) return false;
        const std::size_t l = settings.local_player;
        for (int o = 0; o < 10; ++o) {
            const PlayerSlot& p = players[static_cast<std::size_t>(o)];
            if (o == settings.local_player || !playing(p) || victory.sides[static_cast<std::size_t>(o)].watching) continue;
            if (p.units_created == 0) return false;
            if (p.alive_count == 0) continue;
            if (!victory.sides[static_cast<std::size_t>(o)].allied_victory || !victory.sides[l].allied_victory) return false;
            if (!alliances[l][static_cast<std::size_t>(o)] || !alliances[static_cast<std::size_t>(o)][l]) return false;
            for (int k = 0; k < 10; ++k) {
                const PlayerSlot& q = players[static_cast<std::size_t>(k)];
                if (playing(q) && has_units_or_none_made(q) && !alliances[static_cast<std::size_t>(o)][static_cast<std::size_t>(k)]) return false;
            }
        }
        return true;
    }
    return false;
}

void Battle::victory_turn(int slot) {
    // 0x464F80, the local player's turn: campaign conditions, else defeat then victory.
    PlayerSlot& me = players[static_cast<std::size_t>(slot)];
    VictorySide& side = victory.sides[static_cast<std::size_t>(slot)];
    auto mark_decided = [&] {
        if (!victory.decided_tick) victory.decided_tick = unit_env_.game_tick;
    };
    auto win = [&] {
        mark_decided();
        if (count_down(end_game)) {
            end_game.flags = static_cast<std::uint16_t>(end_game.flags | end_game_flags::kEnding | end_game_flags::kWon |
                                                        end_game_flags::kWonMovie);
            victory.outcome[static_cast<std::size_t>(slot)] = Outcome::Won;
            victory.over = true;
        }
    };
    // Defeat (0x490360): the copy-protection mismatch draws rand() once for a forced defeat
    // 9000..17999 steps in; skirmish and network games: no live units left.
    auto defeated = [&]() -> bool {
        if (settings.cd_mismatch) {
            if (victory.cd_force_tick == 0) victory.cd_force_tick = static_cast<std::uint32_t>(static_cast<std::int64_t>(crt_rand.next()) * 9000 / 0x8000) + 9000;
            if (victory.cd_force_tick <= unit_env_.game_tick) {
                victory.cd_force_tick = 0;
                return true;
            }
        }
        if (settings.provider == 2 || settings.provider == 3) return me.alive_count == 0;
        return false;  // (campaigns are judged by their goals below)
    };
    auto won = [&]() -> bool { return victory_met(); };
    if (settings.provider == 1) {
        // Missions (sim/mission_goals.h): every win goal (0x490230), else any loss goal; the
        // goals of a mission without placed units never decide (and skip the copy check).
        if (mission_won(*this)) {
            win();
            return;
        }
        if (!mission_goals.active) return;
        bool lost = false;
        if (settings.cd_mismatch) {
            if (victory.cd_force_tick == 0) victory.cd_force_tick = static_cast<std::uint32_t>(static_cast<std::int64_t>(crt_rand.next()) * 9000 / 0x8000) + 9000;
            if (victory.cd_force_tick <= unit_env_.game_tick) {
                victory.cd_force_tick = 0;
                lost = true;
            }
        }
        if (!lost) lost = mission_lost(*this);
        if (!lost) return;
        mark_decided();
        if (!count_down(end_game)) return;
        end_game.flags = static_cast<std::uint16_t>(((end_game.flags | end_game_flags::kEnding) & ~end_game_flags::kWon) | end_game_flags::kLost);
        victory.outcome[static_cast<std::size_t>(slot)] = Outcome::Lost;
        victory.over = true;
        return;
    }
    if ((!me.active || !side.watching) && defeated()) {
        mark_decided();
        if (!count_down(end_game)) return;
        if (settings.game_mode == 2) {
            deathmatch_respawn(slot);
        } else if (settings.provider == 3 && side.lobby_status == 0 &&
                   (victory.sides[settings.local_player].wants_watch || [&] {
                       int ai = 0;
                       for (const PlayerSlot& p : players)
                           if (&p - players.data() < 10 && p.active && p.controller == 2 && p.state != 10 && has_units_or_none_made(p)) ++ai;
                       return ai > 0;
                   }())) {
            // Out, but watching: line of sight and mapping are switched off for this machine.
            side.watching = true;
            if (slot == settings.local_player) {
                settings.explore = false;
                settings.fog_of_war = false;
                if (sight_ready_) rebuild_sight(true);
            }
        } else {
            end_game.flags = static_cast<std::uint16_t>((end_game.flags | end_game_flags::kEnding) & ~end_game_flags::kWon);
            if (side.lobby_status == 0) end_game.flags = static_cast<std::uint16_t>(end_game.flags | end_game_flags::kLost);
            victory.outcome[static_cast<std::size_t>(slot)] = Outcome::Lost;
            victory.over = true;
        }
        return;
    }
    if (won()) win();
}

void Battle::victory_stage_end() {
    // After the players' loop: a network game (not deathmatch) without any human player still
    // fighting ends.
    if (settings.provider != 3 || settings.game_mode == 2) return;
    int count = 0;
    for (int s = 0; s < 10; ++s) {
        const PlayerSlot& p = players[static_cast<std::size_t>(s)];
        const VictorySide& side = victory.sides[static_cast<std::size_t>(s)];
        if (playing(p) && has_units_or_none_made(p) && (p.controller == 1 || (p.controller == 3 && side.human)) && !side.watching) ++count;
    }
    if (count != 0) return;
    if (!count_down(end_game)) return;
    end_game.flags = static_cast<std::uint16_t>(((end_game.flags | end_game_flags::kEnding) & ~end_game_flags::kWon) | end_game_flags::kLost);
    victory.over = true;
}

void Battle::kill_team(int slot) {
    PlayerSlot& p = players[static_cast<std::size_t>(slot)];
    // Units in a player's range always belong to it, so a playing player's team never reaches
    // the kill routine here; if it did, its wreck word would be stack contents we do not know.
    const bool plays = p.active && (p.controller == 1 || p.controller == 2);
    if (!plays && p.alive_count != 0 && missions_.fault.empty())
        missions_.fault = "commander death of a player that does not play: the kill routine's stale word is unknown";
    std::uint32_t unread = 0;
    kill_team(slot, unread);
}

void Battle::kill_team(int slot, std::uint32_t& stale_word) {
    // 0x486F10: every live unit of the player not already dying is destroyed: a playing owner's
    // units take 30000 damage of kind 3 from themselves, others explode and are announced dead.
    PlayerSlot& p = players[static_cast<std::size_t>(slot)];
    if (p.alive_count == 0) return;
    sync_settings();
    for (int id = p.first_unit; id <= p.last_unit && id < static_cast<int>(units.size()); ++id) {
        if (id <= 0) continue;
        Unit& u = units[static_cast<std::size_t>(id)];
        if (!(u.flags & unit_flags::kAlive) || (u.flags & unit_tick_flags::kDying)) continue;
        const PlayerSlot& owner = players[static_cast<std::size_t>(ticks[static_cast<std::size_t>(id)].owner)];
        if (!owner.active || (owner.controller != 1 && owner.controller != 2)) {
            detonate_unit_death(projectile_world_, u, true);
            u.flags |= unit_tick_flags::kDying;
            kill_unit(combat_, u, 3, stale_word);
        } else {
            apply_damage(combat_, &u, u, 30000, 3, 0);
        }
    }
}

void Battle::deathmatch_respawn(int slot) {
    // The deathmatch mode: a new commander for the local player somewhere on the map (up to
    // 9999 tries for a spot where its footprint fits at nine probe cells, with no feature and,
    // on lava maps, above the sea), its start resources as storage bonus and production.
    sync_settings();
    const std::size_t l = settings.local_player;
    PlayerSlot& me = players[l];
    const VictorySide& side = victory.sides[l];
    std::uint16_t type = 0;
    for (std::size_t k = 1; k < types.size(); ++k)
        if (!me.commander_name.empty() && types[k].tick.name.size() == me.commander_name.size()) {
            bool same = true;
            for (std::size_t i = 0; i < me.commander_name.size() && same; ++i) {
                char a = types[k].tick.name[i], b = me.commander_name[i];
                if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
                if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 32);
                same = a == b;
            }
            if (same) {
                type = static_cast<std::uint16_t>(k);
                break;
            }
        }
    const std::int32_t world_w = map.width * 16, world_h = map.height * 16;
    std::int32_t x = 0, z = 0;
    for (int tries = 9999;;) {
        const std::int32_t mx = world_w / 10, mz = world_h / 10;
        x = static_cast<std::int32_t>((rng_.next_mod(static_cast<std::uint32_t>(world_w - 2 * mx)) + static_cast<std::uint32_t>(mx)) << 16);
        z = static_cast<std::int32_t>((rng_.next_mod(static_cast<std::uint32_t>(world_h - 2 * mz)) + static_cast<std::uint32_t>(mz)) << 16);
        int fits = 0;
        std::uint32_t pz = static_cast<std::uint32_t>(z) - (static_cast<std::uint32_t>(map.height) << 16);
        for (int r = 0; r < 3; ++r) {
            std::uint32_t px = static_cast<std::uint32_t>(x) - (static_cast<std::uint32_t>(map.width) << 16);
            for (int c = 0; c < 3; ++c) {
                if (type && can_place_footprint(route_, type, 0, static_cast<std::int16_t>(px >> 20), static_cast<std::int16_t>(pz >> 20), 1)) ++fits;
                px += static_cast<std::uint32_t>(map.width) << 16;
            }
            pz += static_cast<std::uint32_t>(map.height) << 16;
        }
        bool ok = false;
        if (fits >= 9) {
            // No feature at the spot (0x421DA0).
            std::int16_t cx = static_cast<std::int16_t>(x >> 20), cz = static_cast<std::int16_t>(z >> 20);
            bool feature = false;
            if (cx >= 0 && cz >= 0 && cx < map.width && cz < map.height) {
                const MapCell* cell = &map.cells[static_cast<std::size_t>(cz * map.width + cx)];
                if (cell->feature == kFeaturePart) {
                    cz = static_cast<std::int16_t>(cz - (cell->feature_aux & 0xFF));
                    cx = static_cast<std::int16_t>(cx - (cell->feature_aux >> 8));
                    cell = cx >= 0 && cz >= 0 && cx < map.width && cz < map.height ? &map.cells[static_cast<std::size_t>(cz * map.width + cx)] : nullptr;
                }
                feature = cell && cell->feature < kFirstSpecialFeature;
            }
            if (!feature) ok = !settings.lava_world || ground_average(map, Vec3i{x, 0, z}) > settings.sea_level;
        }
        if (ok || --tries <= 0) break;
    }
    ++victory.respawns;
    Unit* u = type ? create_unit(static_cast<int>(l), type, x, 0, z, true, 1) : nullptr;
    set_storage_bonus(players[static_cast<std::size_t>(slot)], player_economy[static_cast<std::size_t>(slot)],
                      static_cast<std::int32_t>(side.metal_start) * 100, static_cast<std::int32_t>(side.energy_start) * 100);
    if (u) {
        // The start amounts arrive as the new commander's production (credit rule).
        UnitTickState& t = ticks[u->id];
        const PlayerSlot& owner = players[static_cast<std::size_t>(t.owner)];
        auto credit = [&](float& make, std::uint16_t start) {
            const double amount = static_cast<double>(static_cast<float>(static_cast<std::int32_t>(start)) * 100.0f);
            if (owner.active && owner.controller == 2 && settings.ai_difficulty == 0) make = static_cast<float>(static_cast<double>(make) - amount * -0.5);
            else if (owner.active && owner.controller == 2 && settings.ai_difficulty == 1) make = static_cast<float>(static_cast<double>(make) - amount * -0.7);
            else make = static_cast<float>(amount + static_cast<double>(make));
        };
        credit(t.energy_make, side.energy_start);
        credit(t.metal_make, side.metal_start);
    }
    if (sight_ready_) rebuild_sight(true);
}

}  // namespace sim
