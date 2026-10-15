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
//   --scale S           window pixels per screen pixel (default 2)
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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "client/front.h"
#include "client/game_client.h"
#include "client/music.h"
#include "ta/client_campaign.h"
#include "ta/client_menus.h"
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
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--map" || a == "--side" || a == "--players" || a == "--no-ai" || a == "--seed" || a == "--direct") direct = true;
        if (a == "--direct") continue;
        if (a == "--settings") {
            settings_path = next();
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
        else if (a == "--headless") front.headless = true;
        else if (a == "--frames") front.frames = std::atoi(next().c_str());
        else if (a == "--script") script_file = next();
        else if (a == "--out") front.screenshot_prefix = next();
        else if (a == "--gpu-check") front.gpu_check = true;
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
        // Campaign, briefings and the saved games (saves next to the settings file).
        {
            std::string save_dir = settings_path.substr(0, settings_path.find_last_of("\\/") == std::string::npos ? 0 : settings_path.find_last_of("\\/"));
            if (save_dir.empty()) save_dir = ".";
            ta::install_campaign_hooks(fes.hooks, art, save_dir);
        }
#ifdef TA_CLIENT_HAVE_NET
        // Multiplayer: the connection and battle room screens on the main menu's button.
        client::install_multiplayer(fes.hooks, ta::network_config(art, nullptr));
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
        std::printf("front end: %d music tracks, %zu skirmish maps\n", music.track_count(), art.skirmish_maps().size());
        return client::run_front_end(fes, front);
    }

    auto world = std::make_unique<ta::ClientWorld>();
    std::string err;
    if (!world->load(argv[1], setup, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    client::GameClient game(world->battle(), world->content(), opt);
    world->attach(&game);
    const auto at = world->start_position(0);
    game.center_on({at[0] << 16, 0, at[1] << 16});
    const int rc = client::run_client(game, world->content(), front);
    std::printf("game tick %u, scroll (%d, %d), %d selected\n", world->battle().game_tick(), game.scroll_x(), game.scroll_y(),
                game.selected_count());
    // A summary of the players' units at the end (smoke tests).
    sim::Battle& b = world->battle();
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
