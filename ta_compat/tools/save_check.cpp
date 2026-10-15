// Saved-game check (the original's format, docs/RE_MEMORY.md "Saved games"): a battle is
// played with random commands to step N and saved; the file is read back (its bank written
// again must give the same bytes), loaded into a new battle the way the original loads it,
// saved again at once, and the two saves are compared section by section (what a load cannot
// give back is listed: the loaded battle has run its step-0 players' stage at the saved step).
// The loaded battle then plays on to step M (it must run without faults; it is not expected
// to follow the first one: the original's saves are a partial state).
//
// Usage: save_check <game_dir> [scenarios] [N] [M] [seed] [--mission <campaign> <index>] [--out <file>]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "ta/campaign.h"
#include "ta/saved_game.h"

namespace {

// Commands from a generator seeded by (seed, step, player).
void command(sim::Battle& b, std::uint32_t seed, int player) {
    std::mt19937 rng(seed * 2654435761u ^ b.game_tick() * 40503u ^ static_cast<std::uint32_t>(player) * 977u);
    auto pick = [&](int n) { return n <= 0 ? 0 : static_cast<int>(rng() % static_cast<std::uint32_t>(n)); };
    if (pick(5) != 0) return;
    std::vector<std::uint16_t> mine;
    const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id)
        if (id > 0 && b.ticks[static_cast<std::size_t>(id)].type_index && (b.units[static_cast<std::size_t>(id)].flags & sim::unit_flags::kAlive))
            mine.push_back(static_cast<std::uint16_t>(id));
    if (mine.empty()) return;
    sim::command::Command sel;
    sel.kind = sim::command::Kind::Select;
    sel.player = static_cast<std::uint8_t>(player);
    for (std::uint16_t id : mine)
        if (pick(2)) sel.units.push_back(id);
    if (sel.units.empty()) sel.units.push_back(mine[static_cast<std::size_t>(pick(static_cast<int>(mine.size())))]);
    sim::command::apply(b, sel);
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
    sim::command::apply(b, c);
}

void play(sim::Battle& b, std::uint32_t seed, std::uint32_t to, const std::vector<int>& commanded) {
    while (b.game_tick() < to) {
        for (int p : commanded) command(b, seed, p);
        b.step();
        if ((b.game_tick() * 2654435761u >> 28) % 3 == 0) b.end_frame();
    }
}

ta::SaveContext context_for(const sim::Battle& b, int game_type, const std::string& campaign, const std::string& mission, const std::string& name) {
    ta::SaveContext c;
    c.description = name;
    c.game_id = 1234567;
    c.game_type = game_type;
    c.campaign = campaign;
    c.mission = mission;
    c.max_units = b.settings.unit_limit;
    c.players = b.settings.player_count;
    c.difficulty = b.settings.ai_difficulty;
    return c;
}

// Section-by-section comparison of two banks: equal keys, blobs; reports the differences.
std::size_t compare_banks(const ta::Bank& a, const ta::Bank& b, std::vector<std::string>& out) {
    std::size_t diffs = 0;
    for (const ta::BankSection& s : a.sections) {
        const ta::BankSection* t = b.find(s.name);
        if (!t) {
            out.push_back("section " + s.name + " missing after the load");
            ++diffs;
            continue;
        }
        for (const auto& [k, v] : s.ints)
            if (t->get_int(k, v ^ 0x5A5A5A5A) != v) out.push_back(s.name + "." + k + ": " + std::to_string(v) + " -> " + std::to_string(t->get_int(k, 0))), ++diffs;
        for (const auto& [k, v] : s.doubles)
            if (t->get_double(k, -1e300) != v) out.push_back(s.name + "." + k + " (double)"), ++diffs;
        for (const auto& [k, v] : s.strings)
            if (t->get_string(k, "\x01") != v) out.push_back(s.name + "." + k + " (string)"), ++diffs;
        for (const auto& bl : s.blobs) {
            const ta::BankSection::Blob* tb = bl.named ? t->find_blob(bl.name) : t->find_numbered(bl.index);
            const std::string bn = bl.named ? bl.name : "#" + std::to_string(bl.index);
            if (!tb) {
                out.push_back(s.name + "." + bn + " missing");
                ++diffs;
            } else if (tb->data != bl.data) {
                std::size_t at = 0;
                while (at < bl.data.size() && at < tb->data.size() && bl.data[at] == tb->data[at]) ++at;
                out.push_back(s.name + "." + bn + " differs at byte " + std::to_string(at));
                ++diffs;
            }
        }
    }
    return diffs;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: save_check <game_dir> [scenarios] [N] [M] [seed] [--mission <campaign> <index>] [--out <file>]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 2;
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
        std::vector<int> commanded;
        sim::QuietBattleOutside oa;
        std::string err, mission_name, map_name;
        std::unique_ptr<ta::LoadedBattle> a;
        int game_type = 2;
        if (!campaign.empty()) {
            ta::Campaign c;
            if (!ta::load_campaign(vfs, campaign, c, err) || mission_index < 0 || mission_index >= static_cast<int>(c.missions.size())) {
                std::printf("campaign: %s\n", err.c_str());
                return 1;
            }
            ta::MissionInfo info;
            const int difficulty = static_cast<int>(seed % 3);
            if (!ta::load_mission(vfs, c.missions[static_cast<std::size_t>(mission_index)].file, difficulty, info, err)) {
                std::printf("mission: %s\n", err.c_str());
                return 1;
            }
            ta::MissionRequest rq;
            rq.campaign_side = c.side;
            rq.difficulty = difficulty;
            rq.battle_rng_seed = seed * 7919u;
            rq.loading_rand_seed = seed * 31u;
            rq.crt_rand_state = seed * 17u;
            a = ta::load_mission_battle(vfs, info, rq, oa, err);
            mission_name = c.missions[static_cast<std::size_t>(mission_index)].name;
            game_type = 1;
            commanded = {0};
        } else {
            ta::BattleRequest r;
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
            a = ta::load_battle(vfs, r, oa, err);
            mission_name = r.map;
        }
        if (!a) {
            std::printf("scenario %d: start failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        sim::Battle& ba = *a->battle;
        play(ba, seed, N, commanded);
        ta::SaveContext ctx = context_for(ba, game_type, campaign, mission_name, "save_check scenario " + std::to_string(sc));
        std::vector<std::string> notes;
        const ta::Bank bank = ta::make_save_bank(*a, ctx, &notes);
        std::vector<std::uint8_t> bytes;
        ta::write_bank(bank, bytes, nullptr, ctx.bank);
        if (!out_file.empty() && sc == 0) std::ofstream(out_file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        // Container round trip.
        ta::SavedGame back;
        if (!ta::read_saved_game(bytes, back, err)) {
            std::printf("scenario %d: read failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        std::vector<std::uint8_t> again;
        ta::write_bank(back.bank, again, nullptr, ctx.bank);
        const bool same_bytes = again == bytes;
        // Load and save again at once.
        sim::QuietBattleOutside ob;
        ta::LoadInputs in;
        in.battle_rng_seed = seed * 3u;
        in.loading_rand_seed = seed * 5u;
        in.crt_rand_state = seed * 11u;
        in.unit_limit = ba.settings.unit_limit;
        ta::LoadedView view;
        auto b = ta::load_saved_game(vfs, back, in, ob, view, err);
        if (!b) {
            std::printf("scenario %d: load failed: %s\n", sc, err.c_str());
            ++failures;
            continue;
        }
        sim::Battle& bb = *b->battle;
        ta::SaveContext ctx2 = context_for(bb, game_type, campaign, mission_name, ctx.description);
        ctx2.time_block = view.time_block;
        ctx2.camera_x = view.camera_x;
        ctx2.camera_z = view.camera_z;
        ctx2.player_side = view.side;
        ctx2.player_logo = view.logo;
        const ta::Bank bank2 = ta::make_save_bank(*b, ctx2, &notes);
        std::vector<std::string> diffs;
        const std::size_t nd = compare_banks(bank, bank2, diffs);
        int units_a = 0, units_b = 0;
        for (std::size_t i = 1; i < ba.units.size(); ++i) units_a += (ba.units[i].flags & sim::unit_flags::kAlive) != 0;
        for (std::size_t i = 1; i < bb.units.size(); ++i) units_b += (bb.units[i].flags & sim::unit_flags::kAlive) != 0;
        const std::uint32_t loaded_at = bb.game_tick();
        play(bb, seed, M, commanded);
        const std::string fault = bb.first_fault();
        std::printf("scenario %d (%s): saved at %u, %zu bytes, %d units; container round trip %s; loaded at %u with %d units; re-save: %zu differences; "
                    "played on to %u%s%s\n",
                    sc, mission_name.c_str(), N, bytes.size(), units_a, same_bytes ? "identical" : "DIFFERENT", loaded_at, units_b, nd, bb.game_tick(),
                    fault.empty() ? "" : ", fault: ", fault.c_str());
        for (std::size_t k = 0; k < diffs.size() && k < 12; ++k) std::printf("    %s\n", diffs[k].c_str());
        for (std::size_t k = 0; k < view.notes.size() && k < 6; ++k) std::printf("    load note: %s\n", view.notes[k].c_str());
        for (std::size_t k = 0; k < notes.size() && k < 6; ++k) std::printf("    save note: %s\n", notes[k].c_str());
        if (!same_bytes || units_a != units_b || loaded_at != N) ++failures;
    }
    std::printf("%s\n", failures ? "FAILED" : "done");
    return failures ? 1 : 0;
}
