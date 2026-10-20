// Network games between engine instances, as the original plays them (each machine runs its
// own players' units; the others' follow packets). See docs/RE_MEMORY.md, "Network and sync".
//
//   net_play <game_dir> loop [machines] [steps] [seed] [map]
//       Two or more machines in one process over the loopback transport, each with one
//       human player giving scripted random orders. Every 30 steps (and at the end) each
//       machine's own units are compared with their copies on the other machines.
//   net_play <game_dir> features [steps] [seed] [map] [every]
//       Two machines on the loopback; armed units of each shoot at the features near the
//       other's commander (trees, rocks, metal, wrecks) and at each other, commanders reclaim.
//       Every `every` steps (and at the end) the machines' feature maps are compared (cell,
//       type, damage, burning). NET_PLAY_FEATURE_DUMP=<prefix> writes each machine's map to
//       <prefix><slot>.txt at every comparison. Fails when the final maps differ (the host's
//       own damage counters aside: only the host keeps them).
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

#include "net/lobby.h"
#include "net/net_battle.h"
#include "net/transport.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "ta/battle_loader.h"
#include "ta/net_setup.h"

#include "feature_maps.h"

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
        sp.host = p == 0;
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
    ta::set_network_weapons(b, m.lb->weapons);
    m.net->attach(b);  // again, now that the player records are set up (registers the local players)
    m.net->flush_now();
    m.net->on_chat = [&m](int slot, const std::string& text) { m.chat.push_back(std::to_string(slot) + ": " + text); };
}

// Scripted orders for the machine's own player: move, build, stop.
void give_orders(Machine& m) {
    sim::Battle& b = *m.lb->battle;
    if (m.rng() % 40 != 0) return;
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
    if (what == 3) {
        // Attack a unit of another player (as this machine sees it).
        std::vector<std::uint16_t> theirs;
        for (std::size_t id = 1; id < b.units.size(); ++id)
            if (b.ticks[id].type_index && b.ticks[id].owner != m.slot && (b.units[id].flags & sim::unit_flags::kAlive))
                theirs.push_back(static_cast<std::uint16_t>(id));
        if (!theirs.empty()) {
            const std::uint16_t t = theirs[m.rng() % theirs.size()];
            c.kind = sim::command::Kind::IssueIntent;
            c.intent = sim::intent::kAttack;
            c.target = t;
            c.has_pos = true;
            c.pos = b.units[t].pos;
            sim::command::apply(b, c);
            if (std::getenv("NET_PLAY_VERBOSE")) {
                const sim::Unit& me = b.units[sel.units[0]];
                std::printf("  machine %d step %u: unit %u attacks %u: first order kind %d\n", m.slot, b.game_tick(), me.id, t,
                            me.orders ? me.orders->kind : -1);
            }
            return;
        }
    }
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

using feature_maps::FeatureCell;
using feature_maps::FeatureDiff;
using feature_maps::feature_map;
using feature_maps::compare_features;
using feature_maps::dump_features;

int run_loop(ta::Vfs& vfs, int machines, int steps, std::uint32_t seed, const std::string& map, int feature_every = 0) {
    net::LoopbackHub hub;
    std::vector<std::unique_ptr<net::NetSession>> sessions;
    std::vector<std::unique_ptr<net::BattleRoom>> rooms;
    // NET_PLAY_UDP=<loss>: the machines talk over UDP on this host instead of the loopback, with
    // that share of datagrams lost (resends and frames out of order).
    std::vector<std::unique_ptr<net::Transport>> udp;
    const char* udp_env = std::getenv("NET_PLAY_UDP");
    for (int s = 0; udp_env && s < machines; ++s) {
        std::string err;
        net::UdpOptions o;
        o.loss = std::atof(udp_env);
        o.seed = seed * 13u + static_cast<std::uint32_t>(s) + 1u;
        udp.push_back(s == 0 ? net::udp_host(47700, err, o) : net::udp_join("127.0.0.1", 47700, err, o));
        if (!udp.back()) {
            std::printf("udp: %s\n", err.c_str());
            return 1;
        }
    }
    for (int s = 0; s < machines; ++s) {
        sessions.push_back(std::make_unique<net::NetSession>(udp_env ? *udp[static_cast<std::size_t>(s)] : hub.add_machine()));
        rooms.push_back(std::make_unique<net::BattleRoom>(*sessions.back(), s == 0, 0x100 * (s + 1), "Player " + std::to_string(s + 1)));
        rooms.back()->set_side(static_cast<std::uint8_t>(s & 1));
        rooms.back()->map_checksum = [&vfs](const std::string& m) { return ta::map_sync_checksum(vfs, m); };
    }
    // Every machine's unit definition checksums (the room compares them).
    {
        ta::UnitDefContext ctx;
        ta::load_unit_context(vfs, ctx);
        const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
        const auto checks = ta::unit_sync_checks(vfs, defs);
        for (auto& r : rooms) r->set_unit_checks(checks);
        std::printf("unit definitions: %zu checksums\n", checks.size());
    }
    rooms[0]->set_map(map, ta::map_sync_checksum(vfs, map));
    rooms[0]->set_map_ready(true);
    std::vector<sim::Lcg15> crt(rooms.size());
    for (std::size_t k = 0; k < crt.size(); ++k) crt[k].set_state(0xC0FFEEu + static_cast<std::uint32_t>(k));
    const bool fixed = std::getenv("NET_PLAY_SHUFFLE") == nullptr;
    rooms[0]->set_options(0, 0, 250, 100, 100, fixed);  // 10000 energy and metal
    std::uint32_t clock = 1000;
    // The battle room, then loading: a machine begins its battle when its loading handshake is
    // over (the clients only when the host's battle traffic released its last frames).
    std::vector<std::unique_ptr<Machine>> ms(static_cast<std::size_t>(machines));
    auto begin_battle = [&](std::size_t k) -> bool {
        auto m = std::make_unique<Machine>();
        const net::BattleRoom& room = *rooms[k];
        m->slot = room.local_slot();
        m->rng.seed(seed * 31u + static_cast<std::uint32_t>(k));
        m->net = std::make_unique<net::NetBattle>(m->quiet, *sessions[k]);
        m->clock = clock;
        ta::BattleRequest req;
        req.map = room.map();
        req.sides = room.sides();
        req.start = room.battle_start();
        req.start.battle_rng_seed = seed * 7919u + static_cast<std::uint32_t>(k) * 104729u + 1u;
        req.start.loading_rand_seed = seed + static_cast<std::uint32_t>(k);
        req.start.crt_rand_state = crt[k].state();
        std::string err;
        Machine* raw = m.get();
        m->lb = ta::load_battle(vfs, req, *m->net, err, [raw](sim::Battle& b) { raw->net->attach(b); });
        if (!m->lb) {
            std::printf("load: %s\n", err.c_str());
            return false;
        }
        hook_up(*m);
        m->net->set_room(room.players());
        const auto bs = room.battle_start();
        std::printf("machine %zu: battle begins at pass clock %u; slot %d, start position %d, logos", k, clock, m->slot,
                    bs.players[static_cast<std::size_t>(m->slot)].start_position);
        for (int s = 0; s < machines; ++s) std::printf(" %d", room.players()[static_cast<std::size_t>(s)].logo);
        std::printf(", groups");
        for (int s = 0; s < machines; ++s) std::printf(" %d", room.players()[static_cast<std::size_t>(s)].group);
        std::printf(", disabled units %zu\n", room.disabled_units().size());
        ms[k] = std::move(m);
        return true;
    };
    int pass = 0;
    for (; pass < 3000 && std::any_of(ms.begin(), ms.end(), [](const auto& m) { return !m; }); ++pass) {
        ++clock;
        if (udp_env) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (std::size_t k = 0; k < rooms.size(); ++k) {
            if (ms[k]) {
                Machine& m = *ms[k];
                m.net->set_clock(++m.clock);
                m.lb->battle->step();
                m.lb->battle->end_frame();
                continue;
            }
            rooms[k]->frame(clock, &crt[k]);
            if (rooms[k]->loaded() && !begin_battle(k)) return 1;
        }
        if (pass == 8)
            for (std::size_t k = 1; k < rooms.size(); ++k) {
                rooms[k]->set_ready(true);  // (map ready: from the map checksum)
                rooms[k]->next_logo();
            }
        if (pass > 10 && !rooms[0]->started() && rooms[0]->can_start()) rooms[0]->start();
    }
    if (std::any_of(ms.begin(), ms.end(), [](const auto& m) { return !m; })) {
        std::printf("battle room: not every machine got to its battle\n");
        for (std::size_t k = 0; k < rooms.size(); ++k) {
            std::printf("  machine %zu: slot %d started %d loaded %d battle %d |", k, rooms[k]->local_slot(), rooms[k]->started(), rooms[k]->loaded(),
                        ms[k] ? 1 : 0);
            for (const auto& p : rooms[0]->players())
                if (p.net_id) std::printf(" [%x ready %d map %d logo %d]", p.net_id, p.ready(), p.map_ready, p.logo);
            std::printf("\n");
        }
        return 1;
    }
    std::printf("battle room: %d players seated, game started by the host, all loaded after %d passes\n", machines, pass);
    std::vector<Machine*> all;
    for (auto& m : ms) all.push_back(m.get());
    Comparison total;
    int failures = 0;
    // The features scenario: each machine's feature map from the start, its host flag, a dump file.
    const bool features = feature_every > 0;
    std::vector<std::vector<FeatureCell>> feature_start;
    std::vector<bool> is_host;
    std::vector<std::FILE*> dumps;
    std::size_t feature_compares = 0, feature_compares_equal = 0;
    if (features) {
        for (Machine* m : all) {
            feature_start.push_back(feature_map(*m->lb->battle));
            is_host.push_back(m->lb->battle->victory.sides[static_cast<std::size_t>(m->slot)].host);
            const char* prefix = std::getenv("NET_PLAY_FEATURE_DUMP");
            dumps.push_back(prefix ? std::fopen((std::string(prefix) + std::to_string(m->slot) + ".txt").c_str(), "w") : nullptr);
            std::printf("machine %d: %s\n", m->slot, is_host.back() ? "host" : "not the host");
        }
        if (feature_start.size() > 1 && !(feature_start[0] == feature_start[1])) std::printf("feature maps differ at the start\n");
        if (std::getenv("NET_PLAY_VERBOSE")) {
            std::printf("fire-starting weapons:");
            for (const ta::WeaponDefRecord& w : all[0]->lb->weapon_defs.slots)
                if (w.loaded && w.fire_starter) std::printf(" %s(%u)", w.name.c_str(), w.fire_starter);
            std::printf("\n");
            // Types that reproduce draw the battle generator of each machine (they would diverge).
            const sim::Battle& b0 = *all[0]->lb->battle;
            std::printf("reproducing feature types:");
            for (std::size_t k = 0; k < b0.features.size(); ++k)
                if (b0.features[k].reproduce)
                    std::printf(" %s(%u%%, area %u)",
                                all[0]->lb->feature_defs.records().size() > k ? all[0]->lb->feature_defs.records()[k].name.c_str() : "?",
                                b0.features[k].reproduce, b0.features[k].reproduce_area);
            std::printf("\n");
        }
    }
    auto feature_check = [&](const char* what) {
        std::vector<std::vector<FeatureCell>> now;
        for (std::size_t k = 0; k < all.size(); ++k) {
            now.push_back(feature_map(*all[k]->lb->battle));
            dump_features(dumps[k], all[k]->lb->battle->game_tick(), now.back(), feature_start[k], all[k]->lb->battle->map.width);
        }
        const FeatureDiff d = compare_features(now, is_host, all[0]->lb->battle->map.width);
        std::size_t changed = 0, burning = 0;
        for (std::size_t i = 0; i < now[0].size(); ++i) {
            changed += !(now[0][i].type == feature_start[0][i].type);
            burning += now[0][i].state == 'B';
        }
        ++feature_compares;
        feature_compares_equal += d.cells == d.damage_host_only;
        std::printf("%s step %u features: %zu cells changed since the start, %zu burning (machine 0); differing cells %zu (type %zu, burning/dying %zu, "
                    "damage %zu); host-only damage counters %zu\n",
                    what, all[0]->lb->battle->game_tick(), changed, burning, d.cells - d.damage_host_only, d.type, d.state, d.damage,
                    d.damage_host_only);
        for (const std::string& s : d.first) std::printf("    %s\n", s.c_str());
        return d;
    };
    for (int step = 0; step < steps; ++step) {
        if (udp_env) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (features && step == 30)
            // Each machine places armed units near the next player's commander, among the
            // features there (a skirmish line through the trees).
            for (Machine* m : all) {
                sim::Battle& b = *m->lb->battle;
                const int foe = (m->slot + 1) % machines;
                const sim::Unit& target = b.units[static_cast<std::size_t>(b.players[static_cast<std::size_t>(foe)].first_unit)];
                feature_maps::place_feature_shooters(b, m->slot, target.pos, [&](const char* n) { return m->lb->type_index(n); });
            }
        if (features && step > 30 && step < steps - steps / 3)
            for (Machine* m : all) feature_maps::feature_orders(*m->lb->battle, m->slot, m->rng, step);
        if (features && (step + 1) % feature_every == 0) feature_check("compare");
        if (std::getenv("NET_PLAY_DUEL") && step == 30)
            // A skirmish line: each machine places a few of its own armed units near the first
            // unit of the next player (announced to the others by packet 9 as any new unit).
            for (Machine* m : all) {
                sim::Battle& b = *m->lb->battle;
                const int foe = (m->slot + 1) % machines;
                const sim::Unit& target = b.units[static_cast<std::size_t>(b.players[static_cast<std::size_t>(foe)].first_unit)];
                const std::uint16_t type = m->lb->type_index(m->slot & 1 ? "CORAK" : "ARMPW");
                for (int k = 0; k < 4 && type; ++k)
                    b.create_unit(m->slot, type, target.pos[0] + ((k * 40 - 60) << 16), target.pos[1], target.pos[2] + (160 << 16), true, 1);
            }
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
        if (std::getenv("NET_PLAY_VERBOSE") && (step + 1) % 150 == 0)
            for (Machine* m : all) {
                sim::Battle& b = *m->lb->battle;
                std::printf("  machine %d step %u:", m->slot, b.game_tick());
                for (std::size_t id = 1; id < b.units.size(); ++id)
                    if (b.ticks[id].type_index && (b.units[id].flags & sim::unit_flags::kAlive))
                        std::printf(" [%zu %d,%d hp %d]", id, b.units[id].pos[0] >> 16, b.units[id].pos[2] >> 16, b.units[id].hit_points);
                std::printf(" shots %d\n", b.projectiles.count);
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
    // The end of the game's statistics exchange (0x4572A0, 0x28 with the ask byte / 0x29) on
    // every machine at once, a pass per 250 ms: it must settle.
    {
        int passes = 0;
        bool all_done = false;
        while (!all_done && passes < 40) {
            ++passes;
            all_done = true;
            for (Machine* m : all) {
                m->clock += 8;
                m->net->set_clock(m->clock);
                m->net->poll();
                all_done = m->net->end_exchange() && all_done;
            }
        }
        // A last pass: the final acknowledgements were sent in the pass before.
        for (Machine* m : all) m->net->poll();
        std::printf("end of game exchange: %s after %d passes\n", all_done ? "done" : "NOT done", passes);
        if (!all_done) ++failures;
    }
    if (features) {
        for (int k = 0; k < 10; ++k)
            for (Machine* m : all) m->net->poll();
        const FeatureDiff d = feature_check("final");
        std::printf("features: %zu of %zu comparisons identical (host-only damage counters aside)\n", feature_compares_equal, feature_compares);
        for (std::FILE* f : dumps)
            if (f) std::fclose(f);
        if (d.cells != d.damage_host_only) ++failures;
    }
    if (udp_env) {
        for (int k = 0; k < 50; ++k) {
            for (auto& t : udp) t->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (std::size_t k = 0; k < udp.size(); ++k) {
            const net::UdpCounters c = net::udp_counters(*udp[k]);
            std::printf("udp machine %zu: %llu datagrams delivered, %llu lost on the way out, %llu sent again, %llu duplicates dropped; "
                        "frames out of order %llu\n",
                        k, static_cast<unsigned long long>(c.delivered), static_cast<unsigned long long>(c.dropped_by_loss),
                        static_cast<unsigned long long>(c.resent), static_cast<unsigned long long>(c.duplicates),
                        static_cast<unsigned long long>(sessions[k]->receiver.order_mismatches()));
        }
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
        std::printf("  packets by type (sent and received):");
        for (std::size_t t = 0; t < n.by_type.size(); ++t)
            if (n.by_type[t]) std::printf(" %02zx:%llu", t, static_cast<unsigned long long>(n.by_type[t]));
        std::printf("\n");
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
                     "       net_play <game_dir> features [steps] [seed] [map] [every]\n"
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
    if (mode == "features") {
        const int steps = argc > 3 ? std::atoi(argv[3]) : 1500;
        const std::uint32_t seed = argc > 4 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 0)) : 1u;
        const int every = argc > 6 ? std::max(1, std::atoi(argv[6])) : 60;
        const char* n = std::getenv("NET_PLAY_MACHINES");  // more machines (each asks the host alone)
        return run_loop(vfs, n ? std::clamp(std::atoi(n), 2, 10) : 2, steps, seed, argc > 5 ? argv[5] : "Coast To Coast", every);
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
