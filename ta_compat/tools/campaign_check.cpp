// Campaign smoke check: lists the campaigns, reads their missions and starts each one.
//
// Usage: campaign_check <game_dir> [campaign] [first mission] [count] [difficulty] [steps]
//   without a campaign: lists the campaigns and their missions.
//   with one: loads missions [first, first + count), starts each battle, runs `steps` steps and
//   prints the placed units, the goals and how the mission stands.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "sim/battle_quiet.h"
#include "sim/lockstep.h"
#include "ta/campaign.h"

namespace {

const char* goal_name(sim::GoalKind k) {
    static const char* const n[] = {"KillEnemyCommander", "DestroyAllUnits", "KillAllMobileUnits", "BuildUnitType", "CaptureUnitType",
                                    "KillAllOfType", "KillUnitType", "MoveUnitToRadius", "UnitTypeNearX", "UnitTypeNearZ", "VictoryTimer",
                                    "CommanderKilled", "AllUnitsKilled", "AllUnitsKilledOfType", "UnitTypeKilled", "DeathTimer",
                                    "AnyUnitNearX", "AnyUnitNearZ"};
    return n[static_cast<int>(k)];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: campaign_check <game_dir> [campaign] [first] [count] [difficulty] [steps]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    ta::mount_cd_archives(vfs, argv[1]);
    if (argc < 3) {
        for (const std::string& name : ta::list_campaigns(vfs)) {
            ta::Campaign c;
            std::string err;
            if (!ta::load_campaign(vfs, name, c, err)) {
                std::printf("%s: %s\n", name.c_str(), err.c_str());
                continue;
            }
            std::printf("%s (side %s, %zu missions)\n", name.c_str(), c.side.c_str(), c.missions.size());
        }
        return 0;
    }
    ta::Campaign c;
    std::string err;
    if (!ta::load_campaign(vfs, argv[2], c, err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    const int first = argc > 3 ? std::atoi(argv[3]) : 0;
    const int count = argc > 4 ? std::atoi(argv[4]) : static_cast<int>(c.missions.size());
    const int difficulty = argc > 5 ? std::atoi(argv[5]) : 1;
    const int steps = argc > 6 ? std::atoi(argv[6]) : 300;
    int failures = 0;
    for (int m = first; m < first + count && m < static_cast<int>(c.missions.size()); ++m) {
        const ta::CampaignMission& cm = c.missions[static_cast<std::size_t>(m)];
        ta::MissionInfo info;
        if (!ta::load_mission(vfs, cm.file, difficulty, info, err)) {
            std::printf("mission %d %s: %s\n", m, cm.file.c_str(), err.c_str());
            ++failures;
            continue;
        }
        std::printf("mission %d %s \"%s\": schema %d (%s), %zu units, %zu features, max units %d, metal %g/%g energy %g/%g, brief %zu chars\n", m,
                    cm.file.c_str(), cm.name.c_str(), info.schema, info.schema_type.c_str(), info.units.size(), info.features.size(), info.max_units,
                    static_cast<double>(info.human_metal), static_cast<double>(info.computer_metal), static_cast<double>(info.human_energy),
                    static_cast<double>(info.computer_energy), info.brief_text.size());
        for (const sim::MissionGoal& g : info.goals.win)
            std::printf("  win: %s %s count %d line %d deadline %u\n", goal_name(g.kind), g.type_name.c_str(), g.count, g.line, g.deadline);
        for (const sim::MissionGoal& g : info.goals.loss)
            std::printf("  loss: %s %s count %d line %d deadline %u\n", goal_name(g.kind), g.type_name.c_str(), g.count, g.line, g.deadline);
        sim::QuietBattleOutside quiet;
        ta::MissionRequest rq;
        rq.campaign_side = c.side;
        rq.difficulty = difficulty;
        rq.battle_rng_seed = 1234u + static_cast<std::uint32_t>(m);
        rq.loading_rand_seed = 99u;
        rq.crt_rand_state = 7u;
        auto lb = ta::load_mission_battle(vfs, info, rq, quiet, err);
        if (!lb) {
            std::printf("  start failed: %s\n", err.c_str());
            ++failures;
            continue;
        }
        sim::Battle& b = *lb->battle;
        int placed[2] = {0, 0}, orders = 0;
        for (std::size_t i = 1; i < b.units.size(); ++i) {
            if (!b.ticks[i].type_index) continue;
            const int o = b.ticks[i].owner;
            if (o < 2) ++placed[o];
            for (const sim::MissionOrder* x = b.units[i].orders; x; x = x->next) ++orders;
        }
        std::printf("  started: units %d/%d, orders %d, stored %g/%g, goals %s\n", placed[0], placed[1], orders,
                    static_cast<double>(b.players[0].metal_stored), static_cast<double>(b.players[1].metal_stored),
                    b.mission_goals.active ? "active" : "inactive");
        for (int s = 0; s < steps && !b.victory.over; ++s) {
            b.step();
            b.end_frame();
        }
        int alive[2] = {b.players[0].alive_count, b.players[1].alive_count};
        std::printf("  after %u steps: alive %d/%d, outcome %d, decided at %u, checksum %08x, faults %u\n", b.game_tick(), alive[0], alive[1],
                    static_cast<int>(b.victory.outcome[0]), b.victory.decided_tick, sim::state_checksum(b), b.mission_goals.faults);
    }
    std::printf("%s\n", failures ? "FAILURES" : "ok");
    return failures ? 1 : 0;
}
