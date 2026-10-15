// Whole-game checks on battles built from the original game's files:
//   determinism  two battles from the same start run the same commands: state checksums
//                equal at every step;
//   lockstep     two "machines" (one battle each) exchange turn messages through a loopback
//                with a delay and random reordering; each gives random commands for its own
//                player; their checksums are compared every step;
//   replay       the lockstep game's recording replayed on a third battle reproduces every
//                recorded checksum.
//
// Usage: game_flow <game_dir> [steps] [seed] [map]

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "sim/lockstep.h"
#include "ta/battle_loader.h"
#include "ta/tdf.h"

namespace {

std::unique_ptr<ta::LoadedBattle> load(ta::Vfs& vfs, const std::string& map, sim::BattleOutside& out) {
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
    req.start.unit_limit = 250;
    req.start.battle_rng_seed = 0x12345678u;
    req.start.loading_rand_seed = 0x5EED0001u;
    req.start.crt_rand_state = 0x00C0FFEEu;
    req.start.fog_of_war = true;
    req.start.explore = true;
    std::string err;
    auto lb = ta::load_battle(vfs, req, out, err);
    if (!lb) std::fprintf(stderr, "load: %s\n", err.c_str());
    return lb;
}

// Random commands a player might give: select some of its units, move them, build.
struct Commander {
    std::mt19937 rng;
    explicit Commander(std::uint32_t seed) : rng(seed) {}
    int pick(int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<std::uint32_t>(n)); }

    std::vector<sim::command::Command> give(sim::Battle& b, const ta::LoadedBattle& lb, int player) {
        std::vector<sim::command::Command> out;
        if (pick(4) != 0) return out;  // a command now and then
        std::vector<std::uint16_t> mine;
        const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(player)];
        for (int id = p.first_unit; id <= p.last_unit; ++id)
            if (id > 0 && b.ticks[static_cast<std::size_t>(id)].type_index && (b.units[static_cast<std::size_t>(id)].flags & sim::unit_flags::kAlive))
                mine.push_back(static_cast<std::uint16_t>(id));
        if (mine.empty()) return out;
        sim::command::Command sel;
        sel.kind = sim::command::Kind::Select;
        sel.player = static_cast<std::uint8_t>(player);
        for (std::uint16_t id : mine)
            if (pick(2)) sel.units.push_back(id);
        if (sel.units.empty()) sel.units.push_back(mine[static_cast<std::size_t>(pick(static_cast<int>(mine.size())))]);
        out.push_back(sel);
        const std::uint16_t first = sel.units.front();
        const sim::Unit& u = b.units[first];
        auto around = [&](int r) {
            std::array<std::int32_t, 3> pos = u.pos;
            pos[0] += (pick(2 * r + 1) - r) << 16;
            pos[2] += (pick(2 * r + 1) - r) << 16;
            pos[0] = std::max(pos[0], 32 << 16);
            pos[2] = std::max(pos[2], 32 << 16);
            return pos;
        };
        sim::command::Command c;
        c.player = static_cast<std::uint8_t>(player);
        c.flags = pick(3) == 0 ? sim::command::flags::kQueued : 0;
        switch (pick(5)) {
        case 0:
        case 1:
            c.kind = sim::command::Kind::IssueIntent;
            c.intent = sim::intent::kMove;
            c.has_pos = true;
            c.pos = around(300);
            break;
        case 2: {
            // A builder builds something of its build list nearby (by the type's name order).
            const std::uint16_t type = b.ticks[first].type_index;
            const ta::UnitDefRecord* d = type < lb.type_defs.size() ? lb.type_defs[type] : nullptr;
            if (!d || d->build_options_count <= 0) return out;
            c.kind = sim::command::Kind::MobileBuild;
            c.units = {first};
            c.unit_type = d->build_options[static_cast<std::size_t>(pick(std::min(d->build_options_count, 30)))];
            c.has_pos = true;
            c.pos = around(160);
            if (b.ticks[first].has_motion) break;
            c.kind = sim::command::Kind::BuildCount;
            c.param0 = 1 + pick(2);
            break;
        }
        case 3:
            c.kind = sim::command::Kind::IssueIntent;
            c.intent = sim::intent::kStop;
            break;
        default:
            c.kind = sim::command::Kind::IssueIntent;
            c.intent = std::getenv("GAME_FLOW_PATROL") ? sim::intent::kPatrol : sim::intent::kMove;
            c.has_pos = true;
            c.pos = around(200);
            break;
        }
        out.push_back(c);
        return out;
    }
};

}  // namespace

// Battle start against a capture of the original (ta_compat/tools/capture, e.g.
// golden_skirmish_moonquartet_300.jsonl: Moon Quartet, 1 human + 3 computer players, hard,
// 1000/1000, battle seed overridden). The computer players' draws (not built here) are
// replayed from the capture in their hooks; everything else draws by itself. Compared: the
// battle generator after every recorded draw up to step 1, and at the start of step 1.
namespace {
std::uint64_t json_u(const std::string& line, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const auto at = line.find(k);
    return at == std::string::npos ? ~0ull : std::strtoull(line.c_str() + at + k.size(), nullptr, 10);
}
struct ReplayAi : sim::QuietBattleOutside {
    sim::Battle* battle = nullptr;
    std::deque<std::pair<std::uint32_t, std::uint32_t>> ai_draws;  // (caller, n) of the computer players' draws, in order
    int mismatches = 0;
    void draw_ai(int count) {
        for (int k = 0; k < count && !ai_draws.empty(); ++k) {
            battle->battle_rng().next_mod(ai_draws.front().second);
            ai_draws.pop_front();
        }
    }
    void ai_player_setup(int) override { draw_ai(8); }   // 0x40A150
    void ai_controller_step(int) override { draw_ai(2); }  // 0x407AE0, 0x407E90
};
int check_start(ta::Vfs& vfs, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("start: cannot open %s\n", path.c_str());
        return 1;
    }
    std::vector<std::string> lines;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) lines.emplace_back(buf);
    std::fclose(f);
    std::uint32_t seed = 0, tick1_rng = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> engine_draws;  // (n, state after) the engine must make itself
    ReplayAi out;
    for (const std::string& l : lines) {
        if (l.find("\"ev\":\"setup\"") != std::string::npos) seed = static_cast<std::uint32_t>(json_u(l, "battle_seed_override"));
        if (l.find("\"ev\":\"randmod\"") != std::string::npos && json_u(l, "tick") == 0) {
            const auto caller = static_cast<std::uint32_t>(json_u(l, "caller"));
            const auto n = static_cast<std::uint32_t>(json_u(l, "n"));
            if (caller >= 0x407000 && caller < 0x40B000) out.ai_draws.push_back({caller, n});
            else engine_draws.push_back({n, static_cast<std::uint32_t>(json_u(l, "state"))});
        }
        if (l.find("\"ev\":\"step\"") != std::string::npos && json_u(l, "tick") == 1) tick1_rng = static_cast<std::uint32_t>(json_u(l, "battle_rng"));
    }
    const std::size_t ai_total = out.ai_draws.size();
    ta::BattleRequest req;
    req.sides = {"ARM", "CORE", "ARM", "CORE"};
    for (int p = 0; p < 4; ++p) {
        sim::StartPlayer& sp = req.start.players[static_cast<std::size_t>(p)];
        sp.kind = p == 0 ? sim::PlayerKind::Local : sim::PlayerKind::Computer;
        sp.net_id = p + 1;
        sp.energy = sp.metal = 1000;
    }
    req.start.ai_difficulty = 2;
    req.start.commander_mode = 1;
    req.start.true_sight = true;
    req.start.fog_of_war = true;
    req.start.battle_rng_seed = seed;
    req.start.unit_limit = 250;
    std::string err;
    out.ai_draws.clear();
    struct Hooked : ReplayAi {
    } hooked;
    for (const std::string& l : lines)
        if (l.find("\"ev\":\"randmod\"") != std::string::npos && json_u(l, "tick") == 0) {
            const auto caller = static_cast<std::uint32_t>(json_u(l, "caller"));
            if (caller >= 0x407000 && caller < 0x40B000) hooked.ai_draws.push_back({caller, static_cast<std::uint32_t>(json_u(l, "n"))});
        }
    // The computer players are built into the battle now: they make their own draws, so the
    // captured ones are only replayed through the hooks when START_REPLAY_AI is set.
    sim::QuietBattleOutside quiet;
    const bool replay_ai = std::getenv("START_REPLAY_AI") != nullptr;
    if (!replay_ai) hooked.ai_draws.clear();
    auto lb = replay_ai ? ta::load_battle(vfs, req, hooked, err, [&](sim::Battle& b) { hooked.battle = &b; })
                        : ta::load_battle(vfs, req, quiet, err);
    if (!lb) {
        std::printf("start: %s\n", err.c_str());
        return 1;
    }
    std::printf("start: computer-player draws %s %zu of %zu; battle generator at step 1: engine %08x, original %08x (%s)\n",
                replay_ai ? "replayed" : "captured (engine makes its own)", replay_ai ? ai_total - hooked.ai_draws.size() : ai_total, ai_total, lb->battle->battle_rng().state(), tick1_rng,
                lb->battle->battle_rng().state() == tick1_rng ? "identical" : "DIFFERENT");
    std::printf("start: the original's own draws at step 0 (unit creation): %zu\n", engine_draws.size());
    return lb->battle->battle_rng().state() == tick1_rng && hooked.ai_draws.empty() ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: game_flow <game_dir> [steps] [seed] [map]\n");
        return 2;
    }
    if (argc > 3 && std::string(argv[2]) == "start") {
        ta::Vfs v;
        v.mount_game_dir(argv[1]);
        return check_start(v, argv[3]);
    }
    const int steps = argc > 2 ? std::atoi(argv[2]) : 300;
    const std::uint32_t seed = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 1u;
    const std::string map = argc > 4 ? argv[4] : "Moon Quartet";
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    int failures = 0;
    const bool verbose = std::getenv("GAME_FLOW_VERBOSE") != nullptr;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("loading...\n");
    std::fflush(stdout);

    // 1. Determinism: the same commands on two battles.
    {
        sim::QuietBattleOutside oa, ob;
        auto a = load(vfs, map, oa);
        auto b = load(vfs, map, ob);
        if (!a || !b) return 1;
        Commander ca(seed);
        std::uint32_t first_diff = 0;
        for (int s = 0; s < steps && !first_diff; ++s) {
            for (int p = 0; p < 2; ++p)
                for (const auto& c : ca.give(*a->battle, *a, p)) {
                    if (std::getenv("GAME_FLOW_NOCMD") && (std::atoi(std::getenv("GAME_FLOW_NOCMD")) == 0 || c.kind != sim::command::Kind::Select)) continue;
                    if (verbose)
                        std::printf("  command kind %d player %d intent %d type %d units %zu queued %d\n", static_cast<int>(c.kind), c.player,
                                    c.intent, c.unit_type, c.units.size(), c.queued());
                    sim::command::apply(*a->battle, c);
                    if (verbose) std::printf("  applied\n");
                    sim::command::apply(*b->battle, c);
                }
            if (verbose) {
                // Stage by stage, comparing both generators after each.
                const char* names[] = {"units+projectiles", "effects", "players", "features", "wind", "meteors", "shake", "nano"};
                for (sim::Battle* x : {a->battle.get(), b->battle.get()})
                    for (std::size_t i = 1; i < x->units.size(); ++i) {
                        if (!x->ticks[i].type_index) continue;
                        const sim::UnitTickState& t = x->ticks[i];
                        std::printf("    unit %zu speed %d turn %d mflags %d goal %d rng %08x |", i, t.motion.speed, t.motion.turn, t.motion.flags,
                                    t.motion.goal ? 1 : 0, x->battle_rng().state());
                        for (const sim::MissionOrder* o = x->units[i].orders; o; o = o->next)
                            std::printf(" [k%d s%d w%x wk%u f%x s0 %x s1 %x p %d,%d,%d]", o->kind, o->step, o->wait, o->wake_tick, o->flags, o->scratch0,
                                        o->scratch1, o->param0, o->param1, o->param2);
                        std::printf("\n");
                    }
                a->battle->advance_tick();
                b->battle->advance_tick();
                for (int st = 0; st < 8; ++st) {
                    for (sim::Battle* x : {a->battle.get(), b->battle.get()}) {
                        switch (st) {
                        case 0: x->step_units_and_projectiles(); break;
                        case 1: x->step_effects(); break;
                        case 2: x->players_stage(); break;
                        case 3: x->step_features(); break;
                        case 4: x->step_wind(); break;
                        case 5: x->step_meteor_shower(); break;
                        case 6: sim::step_camera_shake(x->shake, x->crt_rand); break;
                        default: x->step_nano(); break;
                        }
                    }
                    if (a->battle->battle_rng().state() != b->battle->battle_rng().state() || a->battle->crt_rand.state() != b->battle->crt_rand.state()) {
                        std::printf("  step %u: generators differ after %s\n", a->battle->game_tick(), names[st]);
                    }
                }
            } else {
                a->battle->step();
                b->battle->step();
            }
            a->battle->end_frame();
            b->battle->end_frame();
            if (sim::state_checksum(*a->battle) != sim::state_checksum(*b->battle)) {
                first_diff = a->battle->game_tick();
                sim::Battle& x = *a->battle;
                sim::Battle& y = *b->battle;
                std::printf("  rand %08x/%08x rng %08x/%08x projectiles %d/%d\n", x.crt_rand.state(), y.crt_rand.state(), x.battle_rng().state(),
                            y.battle_rng().state(), x.projectiles.count, y.projectiles.count);
                for (std::size_t i = 1; i < x.units.size(); ++i) {
                    const sim::Unit& u = x.units[i];
                    const sim::Unit& v = y.units[i];
                    if (u.pos != v.pos || u.heading != v.heading || u.flags != v.flags || u.hit_points != v.hit_points ||
                        x.ticks[i].velocity != y.ticks[i].velocity)
                        std::printf("  unit %zu: pos %d,%d,%d / %d,%d,%d heading %u/%u flags %08x/%08x hp %d/%d\n", i, u.pos[0], u.pos[1], u.pos[2],
                                    v.pos[0], v.pos[1], v.pos[2], u.heading, v.heading, u.flags, v.flags, u.hit_points, v.hit_points);
                }
                for (int p = 0; p < 2; ++p)
                    std::printf("  player %d: %g/%g %g/%g\n", p, static_cast<double>(x.players[static_cast<std::size_t>(p)].energy_stored),
                                static_cast<double>(y.players[static_cast<std::size_t>(p)].energy_stored),
                                static_cast<double>(x.players[static_cast<std::size_t>(p)].metal_stored),
                                static_cast<double>(y.players[static_cast<std::size_t>(p)].metal_stored));
            }
            if (verbose) {
                std::printf("  step %u checksum %08x at %.2f s\n", a->battle->game_tick(), sim::state_checksum(*a->battle),
                            static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
                std::fflush(stdout);
            }
        }
        int units = 0;
        for (std::size_t i = 1; i < a->battle->units.size(); ++i) units += a->battle->ticks[i].type_index != 0;
        std::printf("determinism: %d steps, %d units at the end, checksum %08x, %s\n", steps, units, sim::state_checksum(*a->battle),
                    first_diff ? "DIFFERENT" : "identical");
        if (first_diff) {
            std::printf("  first difference after step %u\n", first_diff);
            ++failures;
        }
        // A fault is where the original would crash, hang or read garbage: a battle built from
        // the game's own files must not reach one in a short random game.
        const std::string fault = a->battle->first_fault();
        if (!fault.empty()) {
            std::printf("  engine fault: %s\n", fault.c_str());
            ++failures;
        }
    }

    // 2. Lockstep between two machines over a lossy-order loopback, 3. replay of the recording.
    sim::CommandLog recording;
    {
        sim::QuietBattleOutside oa, ob;
        auto a = load(vfs, map, oa);
        auto b = load(vfs, map, ob);
        if (!a || !b) return 1;
        sim::Lockstep la(0x3, 0, 3), lb(0x3, 1, 3);
        Commander ca(seed * 7 + 1), cb(seed * 13 + 5);
        std::mt19937 net(seed);
        std::deque<std::vector<std::uint8_t>> to_a, to_b;  // in flight
        int waits = 0;
        while (static_cast<int>(a->battle->game_tick()) < steps || static_cast<int>(b->battle->game_tick()) < steps) {
            if (static_cast<int>(a->battle->game_tick()) < steps) {
                for (const auto& c : ca.give(*a->battle, *a, 0)) la.queue_local(c);
                la.send_turn(*a->battle, [&](const std::vector<std::uint8_t>& m) { to_b.push_back(m); });
            }
            if (static_cast<int>(b->battle->game_tick()) < steps) {
                for (const auto& c : cb.give(*b->battle, *b, 1)) lb.queue_local(c);
                lb.send_turn(*b->battle, [&](const std::vector<std::uint8_t>& m) { to_a.push_back(m); });
            }
            // Deliver some of what is in flight (in order per direction, a random amount).
            auto deliver = [&](std::deque<std::vector<std::uint8_t>>& q, sim::Lockstep& l) {
                std::size_t n = q.empty() ? 0 : 1 + net() % q.size();
                while (n-- && !q.empty()) {
                    l.receive(q.front());
                    q.pop_front();
                }
            };
            deliver(to_a, la);
            deliver(to_b, lb);
            bool moved = false;
            if (static_cast<int>(a->battle->game_tick()) < steps && la.try_step(*a->battle)) moved = true, a->battle->end_frame();
            if (static_cast<int>(b->battle->game_tick()) < steps && lb.try_step(*b->battle)) moved = true, b->battle->end_frame();
            if (!moved) ++waits;
            if (verbose && moved) std::printf("  lockstep %u %u\n", a->battle->game_tick(), b->battle->game_tick());
            if (la.desync_tick() || lb.desync_tick()) break;
            if (waits > steps * 50) {
                std::printf("lockstep: stalled\n");
                ++failures;
                break;
            }
        }
        std::size_t compared = 0, diffs = 0;
        for (const auto& [tick, sum] : la.log.checksums) {
            auto it = lb.log.checksums.find(tick);
            if (it == lb.log.checksums.end()) continue;
            ++compared;
            diffs += it->second != sum;
        }
        std::printf("lockstep: %u/%u steps, %zu commands, %zu checksums compared, %zu different, %d waits%s\n", a->battle->game_tick(),
                    b->battle->game_tick(), la.log.commands.size(), compared, diffs, waits,
                    la.desync_tick() || lb.desync_tick() ? " DESYNC REPORTED" : "");
        if (diffs || la.desync_tick() || lb.desync_tick()) ++failures;
        recording = la.log;
    }
    {
        std::vector<std::uint8_t> bytes;
        recording.encode(bytes);
        sim::CommandLog back;
        if (!back.decode(bytes)) {
            std::printf("replay: the recording does not decode\n");
            ++failures;
        } else {
            sim::QuietBattleOutside oc;
            auto c = load(vfs, map, oc);
            if (!c) return 1;
            sim::Replayer r(back);
            bool ok = true;
            while (static_cast<int>(c->battle->game_tick()) < steps && ok) {
                ok = r.step(*c->battle, 1);
                c->battle->end_frame();
            }
            std::printf("replay: %zu bytes, %zu commands, %u steps, %s\n", bytes.size(), back.commands.size(), c->battle->game_tick(),
                        ok ? "every checksum reproduced" : "MISMATCH");
            if (!ok) {
                std::printf("  first mismatch after step %u\n", r.mismatch_tick);
                ++failures;
            }
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "all identical");
    return failures ? 1 : 0;
}
