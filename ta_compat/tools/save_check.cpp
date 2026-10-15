// Saved-game check: a battle is played with random commands and frame boundaries; at step N
// it is saved (to a bank file in memory) and played on to step M. The save is then loaded (a
// new battle from the start description, the journal replayed to N) and played on to M with
// the same commands. Every step N+1..M must give the same state: sim::state_checksum, both
// random generators and a deeper hash (unit scripts' thread memory, statics and piece
// animations, motion, economy, players).
//
// Usage: save_check <game_dir> [scenarios] [N] [M] [seed] [--mission <campaign> <index>] [--out <file>]
//   scenarios alternate between Moon Quartet (2 human-commanded players) and a skirmish with
//   computer players; with --mission, a campaign mission. Smoke runs only by default.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/journal.h"
#include "sim/lockstep.h"
#include "ta/campaign.h"
#include "ta/saved_game.h"

namespace {

struct Hash {
    std::uint32_t h = 0x811C9DC5u;
    void add(std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            h ^= (v >> (8 * k)) & 0xFF;
            h *= 0x01000193u;
        }
    }
    void addf(float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, 4);
        add(b);
    }
};

std::uint32_t deep_hash(sim::Battle& b) {
    Hash h;
    h.add(sim::state_checksum(b));
    h.add(b.battle_rng().state());
    h.add(b.crt_rand.state());
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        const sim::Unit& u = b.units[i];
        const sim::UnitTickState& t = b.ticks[i];
        if (!t.type_index) continue;
        if (u.script) {
            for (std::uint32_t w : u.script->thread_memory()) h.add(w);
            for (std::int32_t v : u.script->statics()) h.add(static_cast<std::uint32_t>(v));
            for (std::int32_t v : u.script->piece_anims()) h.add(static_cast<std::uint32_t>(v));
        }
        h.add(static_cast<std::uint32_t>(t.motion.speed));
        h.add(static_cast<std::uint32_t>(t.motion.turn) | (static_cast<std::uint32_t>(t.motion.flags) << 16));
        h.addf(t.energy_make);
        h.addf(t.metal_make);
        h.add(t.kill_count | (static_cast<std::uint32_t>(t.health_percent) << 16));
        for (const sim::MissionOrder* o = u.secondary_orders; o; o = o->next) h.add(o->kind | (o->step << 8));
    }
    for (const sim::PlayerSlot& p : b.players) h.add(p.units_created);
    h.add(b.wind.next_change_tick);
    h.add(b.victory.decided_tick);
    return h.h;
}

// Commands from a generator seeded by (seed, step, player): the same state gives the same commands.
std::vector<sim::command::Command> commands_for(sim::Battle& b, std::uint32_t seed, int player) {
    std::mt19937 rng(seed * 2654435761u ^ b.game_tick() * 40503u ^ static_cast<std::uint32_t>(player) * 977u);
    auto pick = [&](int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<std::uint32_t>(n)); };
    std::vector<sim::command::Command> out;
    if (pick(5) != 0) return out;
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
    const sim::Unit& u = b.units[sel.units.front()];
    sim::command::Command c;
    c.player = static_cast<std::uint8_t>(player);
    c.kind = sim::command::Kind::IssueIntent;
    c.flags = pick(3) == 0 ? sim::command::flags::kQueued : 0;
    if (pick(4) == 0) {
        c.intent = sim::intent::kStop;
    } else {
        c.intent = sim::intent::kMove;
        c.has_pos = true;
        c.pos = u.pos;
        c.pos[0] = std::max(c.pos[0] + ((pick(401) - 200) << 16), 32 << 16);
        c.pos[2] = std::max(c.pos[2] + ((pick(401) - 200) << 16), 32 << 16);
    }
    out.push_back(c);
    return out;
}

// Plays from the battle's current step to `to`: commands of the commanded players, steps,
// frame tails every 1-3 steps (decided by the step). Records the deep hash per step.
void play(sim::Battle& b, sim::JournalRecorder& rec, std::uint32_t seed, std::uint32_t to, const std::vector<int>& commanded,
          std::vector<std::uint32_t>* hashes) {
    while (b.game_tick() < to) {
        for (int p : commanded)
            for (const auto& c : commands_for(b, seed, p)) rec.apply(b, c);
        rec.step(b);
        if (hashes) hashes->push_back(deep_hash(b));
        if ((b.game_tick() * 2654435761u >> 28) % 3 == 0) rec.end_frame(b);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: save_check <game_dir> [scenarios] [N] [M] [seed] [--mission <campaign> <index>] [--out <file>]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int scenarios = argc > 2 ? std::atoi(argv[2]) : 2;
    const std::uint32_t N = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 150;
    const std::uint32_t M = argc > 4 ? static_cast<std::uint32_t>(std::atoi(argv[4])) : 300;
    const std::uint32_t seed0 = argc > 5 ? static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0)) : 1u;
    std::string campaign, out_file;
    int mission_index = -1;
    for (int i = 6; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mission") && i + 2 < argc) {
            campaign = argv[++i];
            mission_index = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            out_file = argv[++i];
        }
    }
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    if (!campaign.empty()) ta::mount_cd_archives(vfs, argv[1]);
    int failures = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const std::uint32_t seed = seed0 + static_cast<std::uint32_t>(sc);
        ta::GameStart g;
        std::vector<int> commanded;
        if (!campaign.empty()) {
            ta::Campaign c;
            std::string err;
            if (!ta::load_campaign(vfs, campaign, c, err) || mission_index < 0 || mission_index >= static_cast<int>(c.missions.size())) {
                std::printf("campaign: %s\n", err.c_str());
                return 1;
            }
            g.kind = ta::GameStart::Kind::Mission;
            g.campaign = campaign;
            g.mission_index = mission_index;
            g.mission_file = c.missions[static_cast<std::size_t>(mission_index)].file;
            g.mission.campaign_side = c.side;
            g.mission.difficulty = static_cast<int>(seed % 3);
            g.mission.battle_rng_seed = seed * 7919u;
            g.mission.loading_rand_seed = seed * 31u;
            g.mission.crt_rand_state = seed * 17u;
            commanded = {0};
        } else {
            ta::BattleRequest& r = g.battle;
            r.map = "Moon Quartet";
            const bool ai = (sc & 1) != 0;
            const int players = ai ? 4 : 2;
            for (int p = 0; p < players; ++p) {
                r.sides[static_cast<std::size_t>(p)] = (p & 1) ? "CORE" : "ARM";
                sim::StartPlayer& sp = r.start.players[static_cast<std::size_t>(p)];
                sp.kind = (ai && p > 0) ? sim::PlayerKind::Computer : sim::PlayerKind::Local;
                sp.net_id = 100 + p;
                sp.energy = sp.metal = 1000;
                if (sp.kind == sim::PlayerKind::Local) commanded.push_back(p);
            }
            r.start.unit_limit = 250;
            r.start.battle_rng_seed = seed * 0x9E3779B9u;
            r.start.loading_rand_seed = seed * 0x85EBCA6Bu;
            r.start.crt_rand_state = seed * 0xC2B2AE35u;
            r.start.fog_of_war = (sc & 2) != 0;
            r.start.explore = (sc & 2) != 0;
        }
        // Run A: play to N, save, play on to M.
        sim::QuietBattleOutside oa;
        std::string err;
        auto a = ta::start_game(vfs, g, oa, err);
        if (!a) {
            std::printf("scenario %d: start failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        sim::Battle& ba = *a->battle;
        sim::Journal ja;
        sim::JournalRecorder ra(ja);
        play(ba, ra, seed, N, commanded, nullptr);
        ta::SavedGame save;
        save.start = g;
        save.tick = ba.game_tick();
        save.checksum = sim::state_checksum(ba);
        save.battle_rng = ba.battle_rng().state();
        save.crt_rand = ba.crt_rand.state();
        save.journal = ja.until(save.tick);
        save.summary = ta::make_summary(ba, g, "save_check scenario " + std::to_string(sc));
        std::vector<std::uint8_t> bytes;
        ta::write_saved_game(save, bytes);
        if (!out_file.empty() && sc == 0) std::ofstream(out_file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::vector<std::uint32_t> ha, hb;
        play(ba, ra, seed, M, commanded, &ha);
        // Run B: load the save, play on to M.
        ta::SavedGame back;
        if (!ta::read_saved_game(bytes, back, err)) {
            std::printf("scenario %d: read failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        ta::SaveSummary sum;
        ta::read_save_summary(bytes, sum, err);
        sim::QuietBattleOutside ob;
        auto b = ta::load_saved_game(vfs, back, ob, err);
        if (!b) {
            std::printf("scenario %d: load failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        sim::Battle& bb = *b->battle;
        sim::Journal jb = back.journal;
        sim::JournalRecorder rb(jb);
        play(bb, rb, seed, M, commanded, &hb);
        std::size_t diff = 0;
        std::uint32_t first = 0;
        for (std::size_t i = 0; i < ha.size() && i < hb.size(); ++i)
            if (ha[i] != hb[i]) {
                if (!diff) first = N + 1 + static_cast<std::uint32_t>(i);
                ++diff;
            }
        int units = 0;
        for (std::size_t i = 1; i < ba.units.size(); ++i) units += ba.ticks[i].type_index != 0;
        std::printf("scenario %d (%s): save at %u (%zu bytes, %zu commands, %zu frames, summary time %u), %zu steps compared to %u, %d units, %s\n", sc,
                    g.kind == ta::GameStart::Kind::Mission ? g.mission_file.c_str() : (commanded.size() > 1 ? "2 commanded" : "1 commanded + AI"), save.tick,
                    bytes.size(), save.journal.commands.size(), save.journal.frame_ends.size(), sum.game_time, ha.size(), M, units,
                    diff || ha.size() != hb.size() ? "DIFFERENT" : "identical");
        if (diff || ha.size() != hb.size()) {
            std::printf("  first difference after step %u (%zu steps differ)\n", first, diff);
            ++failures;
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "all identical");
    return failures ? 1 : 0;
}
