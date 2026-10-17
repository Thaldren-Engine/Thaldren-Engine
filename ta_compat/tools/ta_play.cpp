// Plays a skirmish from an installed copy of the original game with the
// playable client: an SDL3 window (GPU), the in-game interface, sound.
//
// Usage: ta_play <game_dir> [options]
//   Without battle options it starts with the original's menus (main menu, single player,
//   skirmish set-up, options, battles, the end screens); --settings FILE keeps the settings
//   (default settings.txt in the current directory). Any of --map, --side, --players, --no-ai,
//   --seed or --direct starts a skirmish at once instead.
//   --map NAME          map (default Moon Quartet)
//   --side arm|core     the human player's side (default arm); the opponent takes the other
//   --players N         players (2..4; extra players are computer players)
//   --no-ai             the opponents are idle humans instead of computer players
//   --res WxH           screen size (default 640x480; the view grows with it)
//   --scale S           window pixels per screen pixel (default 1)
//   --left-click        the original's default interface (left click orders, right deselects)
//   --no-fog            no line of sight / mapping
//   --speed N           game speed 1..20 (10 normal)
//   --damage-bars       health bars over own units
//   --cpu               draw on the CPU (no GPU backend)
//   --no-sound
//   --seed N            the battle's random seed
//   --headless          no window: run --frames N frames at 30 per second with --script FILE
//                       (see client/front.h) and write --shots a,b,c as <--out PREFIX>NNNN.bmp
//   --gpu-check         headless: also draw the screenshots on the GPU and compare
//   --no-pointer        frames without the pointer (as the original's screen surface has them)
//   --realtime          headless: run the frames at wall-clock speed (network games)
//   --saves DIR         the saved games' directory (default <game_dir>\SAVEGAME, the original's)
//   --end-screen FILE   tests: the menus start at the statistics screen for the table in FILE
//                       (the format of gui_harness's endmsn:)
//   --net-screen SPEC   tests: the menus start at a multiplayer screen (repeat to stack them):
//                       selprov, tcp[:ADDRESS], selgame:FILE, newmulti, lounge:FILE,
//                       message:WIDTH:TEXT (the inputs of gui_harness's commands of those names)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "client/front.h"
#include "client/game_client.h"
#include "client/music.h"
#include "ta/client_campaign.h"
#include "ta/client_menus.h"
#include "ta/client_net_screens.h"
#include "ta/console_commands.h"
#include "ta/client_world.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ta_play <game_dir> [--map NAME] [--side arm|core] [--headless --frames N --script F --shots a,b --out P] ...\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ta::SkirmishSetup setup;
    client::ClientOptions opt;
    client::FrontOptions front;
    std::string script_file;
    int players = 2;
    bool ai = true;
    int side = 0;
    bool direct = false;
    std::string settings_path = "settings.txt";
    std::string end_screen_file, saves_dir;
    std::vector<std::string> net_screens;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--map" || a == "--side" || a == "--players" || a == "--no-ai" || a == "--seed" || a == "--direct") direct = true;
        if (a == "--direct") continue;
        if (a == "--settings") {
            settings_path = next();
            continue;
        }
        if (a == "--end-screen") {
            end_screen_file = next();
            continue;
        }
        if (a == "--saves") {
            saves_dir = next();
            continue;
        } else if (a == "--net-screen") {
            net_screens.push_back(next());
            continue;
        }
        if (a == "--map") setup.map = next();
        else if (a == "--side") side = next() == "core" ? 1 : 0;
        else if (a == "--players") players = std::max(2, std::min(4, std::atoi(next().c_str())));
        else if (a == "--no-ai") ai = false;
        else if (a == "--res") {
            const std::string r = next();
            opt.width = std::max(640, std::atoi(r.c_str()));
            const auto x = r.find('x');
            opt.height = x == std::string::npos ? 480 : std::max(480, std::atoi(r.c_str() + x + 1));
        } else if (a == "--scale") front.scale = std::max(1, std::atoi(next().c_str()));
        else if (a == "--left-click") opt.right_click_orders = false;
        else if (a == "--no-fog") setup.line_of_sight = setup.mapping = false;
        else if (a == "--speed") opt.game_speed = std::atoi(next().c_str());
        else if (a == "--damage-bars") opt.damage_bars = true;
        else if (a == "--cpu") front.gpu = false;
        else if (a == "--no-sound") front.sound = false;
        else if (a == "--seed") setup.seed = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 0));
        else if (a == "--headless") {
            front.headless = true;
#ifdef _WIN32
            // Headless runs never show error dialogs; a crash prints where it happened.
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
            SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
                const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
                const auto at = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
                std::fprintf(stderr, "crash: exception %08lx at module offset 0x%llx\n", ep->ExceptionRecord->ExceptionCode,
                             static_cast<unsigned long long>(at - base));
                std::fflush(stderr);
                return EXCEPTION_EXECUTE_HANDLER;
            });
#endif
        }
        else if (a == "--frames") front.frames = std::atoi(next().c_str());
        else if (a == "--script") script_file = next();
        else if (a == "--out") front.screenshot_prefix = next();
        else if (a == "--gpu-check") front.gpu_check = true;
        else if (a == "--no-pointer") front.draw_pointer = false;
        else if (a == "--realtime") front.realtime = true;
        else if (a == "--shots") {
            std::stringstream ss(next());
            for (std::string t; std::getline(ss, t, ',');) front.screenshot_frames.push_back(std::atoi(t.c_str()));
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    setup.players.clear();
    for (int p = 0; p < players; ++p) setup.players.push_back({p == 0 ? side : 1 - side, p != 0 && ai, -1});
    setup.human = 0;
    opt.player = 0;
    opt.side = side;
    opt.fog = setup.line_of_sight || setup.mapping;
    if (!script_file.empty()) {
        std::ifstream in(script_file);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!client::parse_script(ss.str(), front.script, err)) {
            std::fprintf(stderr, "script: %s\n", err.c_str());
            return 2;
        }
    }

    if (!direct) {
        // The original's front end.
        ta::MenuArt art;
        std::string err;
        if (!art.load(argv[1], err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        client::MusicPlayer music([&art](int n) { return art.music_track(n); }, art.music_tracks());
        client::FrontEndSetup fes;
        fes.width = 640;  // the menus' screen; battles use the display mode chosen in the options
        fes.height = 480;
        fes.settings_path = settings_path;
        ta::make_front_end(art, &music, fes);
        // Campaign, briefings and the saved games: <game dir>\SAVEGAME\<name>.sav as the original
        // keeps them (--saves DIR: another directory).
        {
            std::string save_dir = saves_dir.empty() ? std::string(argv[1]) + "\\SAVEGAME" : saves_dir;
            ta::install_campaign_hooks(fes.hooks, art, save_dir);
        }
#ifdef TA_CLIENT_HAVE_NET
        // Multiplayer: the original's connection, game list and battle room screens on the main
        // menu's button.
        ta::install_net_screens(fes.hooks, art, ta::network_config(art, nullptr));
        if (!net_screens.empty()) {
            const client::NetMenuConfig cfg = ta::network_config(art, nullptr);
            fes.main_menu = [&art, cfg, net_screens](client::FrontEnd&) { return ta::make_net_test_screens(art, cfg, net_screens); };
        }
#endif
        {
            // Track types: the saved list, else the original's default for a 16-track disc
            // (tracks 1-7 battle, the rest building).
            client::Settings s;
            s.load(settings_path);
            const std::string types = s.get_text("CDLISTS", "");
            for (int t = 1; t <= music.track_count(); ++t)
                music.types()[static_cast<std::size_t>(t)] = t - 1 < static_cast<int>(types.size()) ? static_cast<std::uint8_t>(types[static_cast<std::size_t>(t - 1)] - '0')
                                                                 : static_cast<std::uint8_t>(music.track_count() >= 16 && t <= 7 ? 1 : 0);
        }
        if (!end_screen_file.empty()) {
            // Tests: the statistics screen on its own, from a table file (the format gui_harness's
            // endmsn: reads: tick, killmul, timemul, won, local, type and player lines).
            std::ifstream in(end_screen_file);
            std::array<client::EndPlayer, 10> end_players{};
            std::uint32_t tick = 0;
            float km = 0.0f, tm = 0.0f;
            int won = 0, local = 0;
            for (std::string line; std::getline(in, line);) {
                if (const auto c = line.find('#'); c != std::string::npos) line.resize(c);
                std::istringstream ls(line);
                std::string key;
                if (!(ls >> key)) continue;
                if (key == "tick") ls >> tick;
                else if (key == "killmul") ls >> km;
                else if (key == "timemul") ls >> tm;
                else if (key == "won") ls >> won;
                else if (key == "local") ls >> local;
                else if (key == "player") {
                    int slot = 0, colour = 0, controller = 0, state = 0, present = 1, watch = 0, kills = 0, losses = 0;
                    std::uint32_t created = 0;
                    double ep = 0, mp = 0, ew = 0, mw = 0;
                    ls >> slot >> colour >> controller >> state >> created >> present >> watch >> kills >> losses >> ep >> mp >> ew >> mw;
                    if (slot < 0 || slot > 9) continue;
                    std::string name;
                    std::getline(ls >> std::ws, name);
                    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                    client::EndPlayer& p = end_players[static_cast<std::size_t>(slot)];
                    p.active = controller != 0;
                    p.controller = controller;
                    p.state = state;
                    p.units_created = created;
                    p.present = present != 0;
                    p.watching = watch != 0;
                    p.colour = colour;
                    p.kills = static_cast<std::int16_t>(kills);
                    p.losses = static_cast<std::int16_t>(losses);
                    p.energy_produced = ep;
                    p.metal_produced = mp;
                    p.energy_wasted = ew;
                    p.metal_wasted = mw;
                    p.name = name;
                }
            }
            client::EndTable table = client::end_table(end_players, tick, km, tm);
            table.won = won != 0;
            table.watching = local >= 0 && local < 10 && end_players[static_cast<std::size_t>(local)].watching;
            for (const client::EndRow& r : table.rows)
                std::printf("end row %d: %s: %d %d %d %d %d %d %d\n", r.slot, r.name.c_str(), r.values[0], r.values[1], r.values[2], r.values[3],
                            r.values[4], r.values[5], r.values[6]);
            std::printf("end maxima: %d %d %d %d %d %d %d\n", table.maxima[0], table.maxima[1], table.maxima[2], table.maxima[3], table.maxima[4],
                        table.maxima[5], table.maxima[6]);
            fes.main_menu = [&art, table](client::FrontEnd&) { return ta::make_end_screen(art, table); };
            fes.width = opt.width;  // (--res: the screen size the battle had)
            fes.height = opt.height;
        }
        std::printf("front end: %d music tracks, %zu skirmish maps\n", music.track_count(), art.skirmish_maps().size());
        return client::run_front_end(fes, front);
    }

    auto world = std::make_unique<ta::ClientWorld>();
    std::string err;
    if (!world->load(argv[1], setup, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    opt.cheats = true;  // a skirmish: cheat commands allowed
    client::GameClient game(world->battle(), world->content(), opt);
    world->attach(&game);
    const auto commands = ta::install_console_commands(game, *world);
    const auto at = world->start_position(0);
    game.center_on({at[0] << 16, 0, at[1] << 16});
    game.set_pointer_hidden(!front.draw_pointer);  // --no-pointer: frames as the original's screen surface has them
    const int rc = client::run_client(game, world->content(), front);
    std::printf("game tick %u, scroll (%d, %d), %d selected\n", world->battle().game_tick(), game.scroll_x(), game.scroll_y(),
                game.selected_count());
    // A summary of the players' units at the end (smoke tests).
    sim::Battle& b = world->battle();
    {
        const client::ClientOptions& o = game.options();
        std::printf("options: line of sight %d, mapping %d, true sight %d, target everything %d, damage options %04x, no shake %d, "
                    "effects off %d, view %d, dithered fog %d, radar %d, clock %d, debug commands %d\n",
                    b.settings.fog_of_war, b.settings.explore, b.settings.true_sight, b.settings.target_everything, b.settings.damage_options,
                    b.settings.shake_disabled, b.fx_lists.spawning_off, game.viewed_player(), o.dithered_fog, o.radar_everything, o.clock,
                    o.debug_commands);
        for (std::size_t p = 0; p < b.players.size() && p < 4; ++p)
            if (b.players[p].active)
                std::printf("player %zu: energy %.1f metal %.1f\n", p, static_cast<double>(b.players[p].energy_stored),
                            static_cast<double>(b.players[p].metal_stored));
        if (!commands->state().not_built.empty()) {
            std::printf("commands without an effect here:");
            for (const std::string& n : commands->state().not_built) std::printf(" %s", n.c_str());
            std::printf("\n");
        }
    }
    if (const std::string fault = b.first_fault(); !fault.empty()) std::printf("ENGINE FAULT: %s\n", fault.c_str());
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (b.ticks[i].type_index == 0) continue;
        const sim::Unit& u = b.units[i];
        std::printf("  unit %zu player %d %s at (%d, %d) hp %d flags %08x built %.2f order %d\n", i, b.ticks[i].owner,
                    world->defs().units[b.ticks[i].type_index].unit_name.c_str(), u.pos[0] >> 16, u.pos[2] >> 16, u.hit_points, u.flags,
                    static_cast<double>(1.0f - u.build_fraction_left), u.orders ? u.orders->kind : -1);
    }
    for (std::int32_t k = 0; k < b.projectiles.count; ++k) {
        const sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(k)];
        if (p.flags & sim::projectile_flags::kDeleted) continue;
        const int wi = p.weapon ? p.weapon->index : -1;
        std::printf("  shot %d: %s at (%d, %d, %d) from player slot %d\n", k, wi >= 0 ? world->weapon_name(wi).c_str() : "?",
                    p.pos[0] >> 16, p.pos[1] >> 16, p.pos[2] >> 16, p.owner_slot);
    }
    return rc;
}
