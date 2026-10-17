// Simulation timing with many moving units: a skirmish on a map of the original game, two
// players with many fully built units each, every unit of both ordered across the map (the
// two groups meet and fight on the way). Prints the time of each block of steps and the state
// checksum, so a change that should not alter the simulation can be checked step for step
// (same checksums) and timed (lower step times).
//
// Usage: move_bench <game_dir> [units per player] [steps] [map]

#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "sim/lockstep.h"
#include "sim/targeting.h"
#include "ta/battle_loader.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: move_bench <game_dir> [units per player] [steps] [map]\n");
        return 2;
    }
    const int per_player = argc > 2 ? std::atoi(argv[2]) : 200;
    const int steps = argc > 3 ? std::atoi(argv[3]) : 600;
    const std::string map = argc > 4 ? argv[4] : "Moon Quartet";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    sim::QuietBattleOutside quiet;
    ta::BattleRequest req;
    req.map = map;
    req.sides[0] = "ARM";
    req.sides[1] = "CORE";
    for (int p = 0; p < 2; ++p) {
        sim::StartPlayer& sp = req.start.players[static_cast<std::size_t>(p)];
        sp.kind = sim::PlayerKind::Local;
        sp.net_id = 100 + p;
        sp.energy = sp.metal = 1000;
    }
    req.start.unit_limit = static_cast<std::uint16_t>(std::max(250, per_player + 10));
    req.start.battle_rng_seed = 0x12345678u;
    req.start.loading_rand_seed = 0x5EED0001u;
    req.start.crt_rand_state = 0x00C0FFEEu;
    req.start.fog_of_war = true;
    req.start.explore = true;
    std::string err;
    auto lb = ta::load_battle(vfs, req, quiet, err);
    if (!lb) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    sim::Battle& b = *lb->battle;

    // The commanders' places.
    std::array<std::array<std::int32_t, 3>, 2> start{};
    for (std::size_t i = 1; i < b.units.size(); ++i)
        if (b.ticks[i].type_index && b.ticks[i].owner < 2) start[b.ticks[i].owner] = b.units[i].pos;
    auto ground = [&](std::int32_t x, std::int32_t z) {
        const sim::MapCell* c = b.map.cell(x >> 20, z >> 20);
        return c ? static_cast<std::int32_t>(c->height) << 16 : 0;
    };
    const char* const kinds[2][4] = {{"ARMPW", "ARMFLASH", "ARMSTUMP", "ARMHAM"}, {"CORAK", "CORTHUD", "CORGATOR", "CORRAID"}};
    // Player 0's units in a block beside its commander, player 1's in a block 1,300 units
    // towards the map's middle from it.
    const std::int32_t dz = start[0][2] < (b.map.height << 19) ? 1300 : -1300;
    int made[2] = {0, 0};
    for (int p = 0; p < 2; ++p) {
        const std::int32_t x0 = (start[0][0] >> 16) - 240, z0 = (start[0][2] >> 16) + 64 + (p ? dz : 0);
        for (int i = 0; i < per_player; ++i) {
            const std::int32_t x = (x0 + (i % 20) * 24) << 16, z = (z0 + (i / 20) * 24) << 16;
            const std::uint16_t type = lb->type_index(kinds[p][i % 4]);
            if (type && b.create_unit(p, type, x, ground(x, z), z, true, 1)) ++made[p];
        }
    }
    // Everyone moves: player 0 past the other block, player 1 to player 0's start.
    for (int p = 0; p < 2; ++p) {
        sim::command::Command sel;
        sel.kind = sim::command::Kind::Select;
        sel.player = static_cast<std::uint8_t>(p);
        for (int id = b.players[static_cast<std::size_t>(p)].first_unit; id <= b.players[static_cast<std::size_t>(p)].last_unit; ++id)
            if (id > 0 && b.ticks[static_cast<std::size_t>(id)].type_index) sel.units.push_back(static_cast<std::uint16_t>(id));
        sim::command::apply(b, sel);
        sim::command::Command mv;
        mv.kind = sim::command::Kind::IssueIntent;
        mv.player = static_cast<std::uint8_t>(p);
        mv.intent = sim::intent::kMove;
        mv.has_pos = true;
        mv.pos = start[0];
        if (p == 0) mv.pos[2] += (2 * dz) << 16;
        sim::command::apply(b, mv);
    }
    std::printf("%s: %d + %d units moving\n", map.c_str(), made[0], made[1]);
    using clock = std::chrono::steady_clock;
    double block = 0, total = 0, worst = 0;
    for (int s = 1; s <= steps; ++s) {
        const auto t0 = clock::now();
        b.step();
        b.end_frame();
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        block += ms, total += ms;
        if (ms > worst) worst = ms;
        if (s % 100 == 0 || s == steps) {
            int alive = 0;
            for (std::size_t i = 1; i < b.units.size(); ++i) alive += b.ticks[i].type_index != 0;
            const int n = s % 100 ? s % 100 : 100;
            std::printf("steps %4d-%4d: %.3f ms per step (worst so far %.2f), %d units, checksum %08x\n", s - n + 1, s, block / n, worst, alive,
                        sim::state_checksum(b));
            block = 0;
        }
    }
    std::printf("total %.1f ms for %d steps (%.3f ms per step)\n", total, steps, total / steps);
    if (const std::string fault = b.first_fault(); !fault.empty()) std::printf("ENGINE FAULT: %s\n", fault.c_str());
    return 0;
}
