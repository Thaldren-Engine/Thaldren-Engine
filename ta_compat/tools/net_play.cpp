// Network games between engine instances, as the original plays them (each machine runs its
// own players' units; the others' follow packets). See docs/RE_MEMORY.md, "Network and sync".
//
//   net_play <game_dir> loop [machines] [steps] [seed] [map]
//       Two or more machines in one process over the loopback transport, each with one
//       human player giving scripted random orders. Every 30 steps (and at the end) each
//       machine's own units are compared with their copies on the other machines.
//   net_play <game_dir> host <port> <machines> [steps] [seed] [map]
//   net_play <game_dir> join <address> <port> <machines> <slot> [steps] [seed] [map]
//       The same over TCP between processes (the host is slot 0). Steps run at 30 per second.

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/net_battle.h"
#include "net/transport.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "ta/battle_loader.h"

namespace {

const char* kSides[] = {"ARM", "CORE", "ARM", "CORE", "ARM", "CORE", "ARM", "CORE", "ARM", "CORE"};

struct Machine {
    int slot = 0;
    sim::QuietBattleOutside quiet;
    std::unique_ptr<net::NetBattle> net;
    std::unique_ptr<ta::LoadedBattle> lb;
    std::mt19937 rng;
    std::uint32_t clock = 1000;
    std::vector<std::string> chat;
};

std::unique_ptr<ta::LoadedBattle> load(ta::Vfs& vfs, const std::string& map, int machines, int slot, std::uint32_t seed, net::NetBattle& out) {
    ta::BattleRequest req;
    req.map = map;
    for (int p = 0; p < machines; ++p) {
        req.sides[static_cast<std::size_t>(p)] = kSides[p];
        sim::StartPlayer& sp = req.start.players[static_cast<std::size_t>(p)];
        sp.kind = p == slot ? sim::PlayerKind::Local : sim::PlayerKind::Remote;
        sp.net_id = 0x100 + p;
        sp.start_position = p;
        sp.colour = static_cast<std::uint8_t>(p);
        sp.name = std::string("Player ") + std::to_string(p + 1);
    }
    req.start.provider = 3;
    req.start.net_game = true;
    req.start.local_player = static_cast<std::uint8_t>(slot);
    req.start.unit_limit = 250;
    req.start.commander_mode = 0;
    // Each machine seeds its own generators from its own clock (inputs).
    req.start.battle_rng_seed = seed * 7919u + static_cast<std::uint32_t>(slot) * 104729u + 1u;
    req.start.loading_rand_seed = seed + static_cast<std::uint32_t>(slot);
    req.start.crt_rand_state = 0xC0FFEEu + static_cast<std::uint32_t>(slot);
    std::string err;
    // The battle start already announces the local commanders: the network side is attached
    // before it runs.
    auto lb = ta::load_battle(vfs, req, out, err, [&](sim::Battle& b) { out.attach(b); });
    if (!lb) std::fprintf(stderr, "load: %s\n", err.c_str());
    return lb;
}

void hook_up(Machine& m) {
    sim::Battle& b = *m.lb->battle;
    auto& w = b.net_state().weapons;
    for (const sim::WeaponType& wt : m.lb->weapons) {
        if (w.size() <= wt.index) w.resize(static_cast<std::size_t>(wt.index) + 1, nullptr);
        if (!w[wt.index]) w[wt.index] = &wt;
    }
    m.net->attach(b);  // again, now that the player records are set up (registers the local players)
    m.net->flush_now();
    m.net->on_chat = [&m](int slot, const std::string& text) { m.chat.push_back(std::to_string(slot) + ": " + text); };
}

// Scripted orders for the machine's own player: move, build, stop.
void give_orders(Machine& m) {
    sim::Battle& b = *m.lb->battle;
    if (m.rng() % 6 != 0) return;
    const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(m.slot)];
    std::vector<std::uint16_t> mine;
    for (int id = p.first_unit; id <= p.last_unit; ++id)
        if (id > 0 && b.ticks[static_cast<std::size_t>(id)].type_index && (b.units[static_cast<std::size_t>(id)].flags & sim::unit_flags::kAlive))
            mine.push_back(static_cast<std::uint16_t>(id));
    if (mine.empty()) return;
    sim::command::Command sel;
    sel.kind = sim::command::Kind::Select;
    sel.player = static_cast<std::uint8_t>(m.slot);
    sel.units.push_back(mine[m.rng() % mine.size()]);
    sim::command::apply(b, sel);
    const sim::Unit& u = b.units[sel.units[0]];
    auto around = [&](int r) {
        std::array<std::int32_t, 3> pos = u.pos;
        pos[0] += (static_cast<int>(m.rng() % static_cast<unsigned>(2 * r + 1)) - r) << 16;
        pos[2] += (static_cast<int>(m.rng() % static_cast<unsigned>(2 * r + 1)) - r) << 16;
        pos[0] = std::clamp(pos[0], 48 << 16, (b.map.width * 16 - 48) << 16);
        pos[2] = std::clamp(pos[2], 48 << 16, (b.map.height * 16 - 48) << 16);
        return pos;
    };
    sim::command::Command c;
    c.player = static_cast<std::uint8_t>(m.slot);
    const std::uint16_t type = b.ticks[sel.units[0]].type_index;
    const ta::UnitDefRecord* d = type < m.lb->type_defs.size() ? m.lb->type_defs[type] : nullptr;
    const unsigned what = m.rng() % 4;
    if (what < 2 && d && d->build_options_count > 0) {
        if (b.ticks[sel.units[0]].has_motion) {
            c.kind = sim::command::Kind::MobileBuild;
            c.units = {sel.units[0]};
            c.unit_type = d->build_options[m.rng() % static_cast<unsigned>(std::min(d->build_options_count, 30))];
            c.has_pos = true;
            c.pos = around(200);
        } else {
            c.kind = sim::command::Kind::BuildCount;
            c.units = {sel.units[0]};
            c.unit_type = d->build_options[m.rng() % static_cast<unsigned>(std::min(d->build_options_count, 30))];
            c.param0 = 1;
        }
    } else {
        c.kind = sim::command::Kind::IssueIntent;
        c.intent = sim::intent::kMove;
        c.has_pos = true;
        c.pos = around(400);
    }
    sim::command::apply(b, c);
}

struct Comparison {
    std::size_t owned = 0, copies = 0, missing = 0, wrong_type = 0, hp_differs = 0, dead_here_alive_there = 0;
    double sum_err = 0, max_err = 0;
};

void compare(std::vector<Machine*>& ms, Comparison& cmp) {
    for (Machine* owner : ms) {
        sim::Battle& a = *owner->lb->battle;
        const sim::PlayerSlot& p = a.players[static_cast<std::size_t>(owner->slot)];
        for (int id = p.first_unit; id <= p.last_unit; ++id) {
            const auto i = static_cast<std::size_t>(id);
            const bool alive = a.ticks[i].type_index && (a.units[i].flags & sim::unit_flags::kAlive);
            for (Machine* other : ms) {
                if (other == owner) continue;
                sim::Battle& b = *other->lb->battle;
                const bool there = b.ticks[i].type_index && (b.units[i].flags & sim::unit_flags::kAlive);
                if (!alive) {
                    if (there) ++cmp.dead_here_alive_there;
                    continue;
                }
                ++cmp.owned;
                if (!there) {
                    ++cmp.missing;
                    continue;
                }
                ++cmp.copies;
                if (b.ticks[i].type_index != a.ticks[i].type_index) ++cmp.wrong_type;
                if (b.units[i].hit_points != a.units[i].hit_points) ++cmp.hp_differs;
                const double dx = (a.units[i].pos[0] - b.units[i].pos[0]) / 65536.0, dz = (a.units[i].pos[2] - b.units[i].pos[2]) / 65536.0;
                const double e = std::sqrt(dx * dx + dz * dz);
                cmp.sum_err += e;
                cmp.max_err = std::max(cmp.max_err, e);
            }
        }
    }
}

void report(const char* what, std::uint32_t tick, const Comparison& c) {
    std::printf("%s step %u: own units %zu, copies %zu (missing %zu, other type %zu, other hit points %zu), stale copies of dead units %zu, "
                "position error mean %.1f max %.1f\n",
                what, tick, c.owned, c.copies, c.missing, c.wrong_type, c.hp_differs, c.dead_here_alive_there,
                c.copies ? c.sum_err / static_cast<double>(c.copies) : 0.0, c.max_err);
}

int run_loop(ta::Vfs& vfs, int machines, int steps, std::uint32_t seed, const std::string& map) {
    net::LoopbackHub hub;
    std::vector<std::unique_ptr<Machine>> ms;
    for (int s = 0; s < machines; ++s) {
        auto m = std::make_unique<Machine>();
        m->slot = s;
        m->rng.seed(seed * 31u + static_cast<std::uint32_t>(s));
        m->net = std::make_unique<net::NetBattle>(m->quiet, hub.add_machine());
        ms.push_back(std::move(m));
    }
    // Everybody is in the session before the battles load (their start announces units).
    for (auto& m : ms) {
        m->lb = load(vfs, map, machines, m->slot, seed, *m->net);
        if (!m->lb) return 1;
        hook_up(*m);
    }
    std::vector<Machine*> all;
    for (auto& m : ms) all.push_back(m.get());
    Comparison total;
    int failures = 0;
    for (int step = 0; step < steps; ++step) {
        for (Machine* m : all) {
            give_orders(*m);
            if (step == 10) m->net->send_chat("hello from " + std::to_string(m->slot));
            m->net->set_clock(++m->clock);
            m->lb->battle->step();
            m->lb->battle->end_frame();
            if (!m->net->fault().empty()) {
                std::printf("machine %d step %u: %s\n", m->slot, m->lb->battle->game_tick(), m->net->fault().c_str());
                ++failures;
                return 1;
            }
        }
        if ((step + 1) % 60 == 0 || step + 1 == steps) {
            Comparison c;
            compare(all, c);
            report("compare", all[0]->lb->battle->game_tick(), c);
            total.owned += c.owned;
            total.copies += c.copies;
            total.missing += c.missing;
            total.wrong_type += c.wrong_type;
        }
    }
    // Let the last frames arrive (no more steps on the owners): poll and compare once more.
    for (int k = 0; k < 40; ++k)
        for (Machine* m : all) {
            m->net->set_clock(++m->clock);
            m->net->flush_now();
        }
    for (Machine* m : all) {
        const net::NetCounters& n = m->net->counters();
        std::printf("machine %d: %llu packets sent (%llu unit state), %llu received, %llu refused; %llu frames, %llu bytes sent; chat lines %zu; "
                    "remote units made %llu, states read %llu; lag %d\n",
                    m->slot, static_cast<unsigned long long>(n.packets_sent), static_cast<unsigned long long>(n.state_packets),
                    static_cast<unsigned long long>(n.packets_received), static_cast<unsigned long long>(n.refused),
                    static_cast<unsigned long long>(m->net->sender().frames_sent()), static_cast<unsigned long long>(m->net->sender().bytes_sent()),
                    m->chat.size(), static_cast<unsigned long long>(m->lb->battle->net_state().units_made),
                    static_cast<unsigned long long>(m->lb->battle->net_state().states_read), m->net->remote_lag());
        if (m->chat.size() != static_cast<std::size_t>(machines - 1)) ++failures;
    }
    std::printf("loopback: %zu messages, %llu bytes; %s\n", static_cast<std::size_t>(hub.messages()), static_cast<unsigned long long>(hub.bytes()),
                failures ? "FAILED" : "ok");
    if (total.copies == 0 || total.wrong_type) ++failures;
    return failures ? 1 : 0;
}

int run_tcp(ta::Vfs& vfs, bool host, const std::string& address, std::uint16_t port, int machines, int slot, int steps, std::uint32_t seed,
            const std::string& map) {
    std::string err;
    std::unique_ptr<net::Transport> t = host ? net::tcp_host(port, err) : net::tcp_join(address, port, err);
    if (!t) {
        std::printf("transport: %s\n", err.c_str());
        return 1;
    }
    Machine m;
    m.slot = slot;
    m.rng.seed(seed * 31u + static_cast<std::uint32_t>(slot));
    m.net = std::make_unique<net::NetBattle>(m.quiet, *t);
    m.lb = load(vfs, map, machines, slot, seed, *m.net);
    if (!m.lb) return 1;
    hook_up(m);
    // Wait until every other machine is heard from (their unit packets), up to 30 s.
    const auto start = std::chrono::steady_clock::now();
    auto clock_now = [&]() {
        return 1000u + static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() * 30 / 1000);
    };
    int step = 0;
    while (step < steps) {
        const std::uint32_t now = clock_now();
        if (now - 1000u <= static_cast<std::uint32_t>(step)) {
            t->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        give_orders(m);
        if (step == 30) m.net->send_chat("hello from " + std::to_string(slot));
        m.net->set_clock(now);
        m.lb->battle->step();
        m.lb->battle->end_frame();
        ++step;
        if (!t->connected()) {
            std::printf("connection lost at step %d\n", step);
            return 1;
        }
    }
    for (int k = 0; k < 60; ++k) {
        m.net->set_clock(clock_now());
        m.net->flush_now();
        m.net->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int remote_units = 0, own = 0;
    sim::Battle& b = *m.lb->battle;
    for (std::size_t i = 1; i < b.units.size(); ++i)
        if (b.ticks[i].type_index && (b.units[i].flags & sim::unit_flags::kAlive)) (b.ticks[i].owner == slot ? own : remote_units)++;
    const net::NetCounters& n = m.net->counters();
    std::printf("slot %d: %d steps, %d own units, %d remote units, %llu packets sent, %llu received, chat lines %zu, lag %d\n", slot, step, own, remote_units,
                static_cast<unsigned long long>(n.packets_sent), static_cast<unsigned long long>(n.packets_received), m.chat.size(), m.net->remote_lag());
    for (const std::string& c : m.chat) std::printf("  chat %s\n", c.c_str());
    return remote_units > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: net_play <game_dir> loop [machines] [steps] [seed] [map]\n"
                     "       net_play <game_dir> host <port> <machines> [steps] [seed] [map]\n"
                     "       net_play <game_dir> join <address> <port> <machines> <slot> [steps] [seed] [map]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    const std::string mode = argv[2];
    if (mode == "loop") {
        const int machines = argc > 3 ? std::clamp(std::atoi(argv[3]), 2, 10) : 2;
        const int steps = argc > 4 ? std::atoi(argv[4]) : 300;
        const std::uint32_t seed = argc > 5 ? static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0)) : 1u;
        return run_loop(vfs, machines, steps, seed, argc > 6 ? argv[6] : "Moon Quartet");
    }
    if (mode == "host" && argc > 4) {
        const int steps = argc > 5 ? std::atoi(argv[5]) : 600;
        const std::uint32_t seed = argc > 6 ? static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 0)) : 1u;
        return run_tcp(vfs, true, "", static_cast<std::uint16_t>(std::atoi(argv[3])), std::atoi(argv[4]), 0, steps, seed,
                       argc > 7 ? argv[7] : "Moon Quartet");
    }
    if (mode == "join" && argc > 6) {
        const int steps = argc > 7 ? std::atoi(argv[7]) : 600;
        const std::uint32_t seed = argc > 8 ? static_cast<std::uint32_t>(std::strtoul(argv[8], nullptr, 0)) : 1u;
        return run_tcp(vfs, false, argv[3], static_cast<std::uint16_t>(std::atoi(argv[4])), std::atoi(argv[5]), std::atoi(argv[6]), steps, seed,
                       argc > 9 ? argv[9] : "Moon Quartet");
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
