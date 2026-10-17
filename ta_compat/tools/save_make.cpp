// Writes a saved game's Summary (the keys the load and save menus list and show) as a test
// fixture for the menus: the original's bank container, the Summary keys in the original's
// order, and a "Radar Image" blob with a test pattern.
//
// Usage: save_make <out.sav> [key=value ...]
//   desc=TEXT  type=1|2  players=N  map=NAME (skirmish map / campaign mission)  campaign=NAME
//   side=N  diff=N  time=STEPS  radar=WxH (0x0: no image)  between=1  squash=0|1

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ta/saved_game.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: save_make <out.sav> [desc=.. type=.. players=.. map=.. campaign=.. side=.. diff=.. time=.. radar=WxH between=1]\n");
        return 2;
    }
    std::string desc = "test", map = "Moon Quartet", campaign;
    int type = 2, players = 2, side = 0, diff = 1, time = 0, rw = 64, rh = 64, between = 0, squash = 1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "desc") desc = v;
        else if (k == "type") type = std::atoi(v.c_str());
        else if (k == "players") players = std::atoi(v.c_str());
        else if (k == "map") map = v;
        else if (k == "campaign") campaign = v;
        else if (k == "side") side = std::atoi(v.c_str());
        else if (k == "diff") diff = std::atoi(v.c_str());
        else if (k == "time") time = std::atoi(v.c_str());
        else if (k == "between") between = std::atoi(v.c_str());
        else if (k == "squash") squash = std::atoi(v.c_str());
        else if (k == "radar") {
            rw = std::atoi(v.c_str());
            const auto x = v.find('x');
            rh = x == std::string::npos ? rw : std::atoi(v.c_str() + x + 1);
        }
    }
    ta::Bank bank;
    bank.name = "Total Annihilation 3.0";
    ta::BankSection& s = bank.section("Summary");
    s.set_int("BUILD DATE: Jul 30 1998", 0);
    s.set_int("BUILD TIME: 11:16:36", 0);
    s.set_int("maxunits", 250);
    if (!campaign.empty()) s.set_string("Campaign", campaign);
    s.set_string("Mission", map);
    s.set_string("Map", map);
    s.set_int("Difficulty", diff);
    s.set_int("Side", side);
    s.set_int("Players", players);
    s.set_int("Gametype", type);
    s.set_string("Thumbs", "");
    if (type == 2) {
        s.set_int("CommanderDeath", 1);
        s.set_int("Location", 1);
        s.set_int("Mapping", 1);
        s.set_int("LineOfSight", 1);
        s.set_int("LineOfSightType", 1);
    }
    if (between) s.set_int("BetweenMissions", 1);
    s.set_string("Description", desc);
    s.set_int("Game ID", 12345);
    s.set_int("Game Time", time);
    if (rw > 0 || rh > 0) {
        std::vector<std::uint8_t>& d = s.blob("Radar Image").data;
        auto put = [&](std::int32_t v) {
            for (int b = 0; b < 4; ++b) d.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
        };
        put(rw);
        put(rh);
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) d.push_back(static_cast<std::uint8_t>(((x / 8 + y / 8) & 1) ? 0x60 + (x * 7 + y * 3) % 32 : 0xC0 + (x + y) % 16));
    }
    std::vector<std::uint8_t> bytes;
    ta::BankWriteOptions o;
    o.squash = squash != 0;
    ta::write_bank(bank, bytes, nullptr, o);
    std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("%s: %zu bytes\n", argv[1], bytes.size());
    return 0;
}
