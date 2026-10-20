// Plays a skirmish from an installed copy of the original game with the
// playable client: an SDL3 window (GPU), the in-game interface, sound.
//
// Usage: ta_play [<game_dir>] [options]   (without a folder: the game folder the program sits in)
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
//                       selprov, tcp[:ADDRESS], serial, modem, selgame:FILE, newmulti, lounge:FILE,
//                       message:WIDTH:TEXT (the inputs of gui_harness's commands of those names)
//   --key-sweep KEYS:MODS[:X,Y[:TEXT]] tests: each key with each modifier set on the screens
//                       --net-screen opens (a fresh front end per key; X,Y: a click there first,
//                       TEXT: then typed),
//                       one OUTCOME line each (compare: gui_harness sweep:KEYS:MODS)
//   --loading-test FILE tests: writes loading screen frames for the states in FILE and exits
//                       (the format is described where it is read; compare: gui_harness loadframe:)
//   --loading-trace     tests (with a direct battle): prints the loader's loading-screen reports
//   --pointer-selftest  tests: checks the window front ends' pointer and focus rules and exits
//   Environment (tests): TA_ROOM_DUMP=PREFIX writes each state a live battle room draws as a
//   room file and its frame (PREFIXnnnn.txt / .bmp; compare: gui_harness lounge:), TA_DPLAY_LOG=FILE
//   logs every message of the session protocol.
//   --dump-build-pages DIR  tests: writes every builder's build pages (listing and pictures, for
//                       each viewing side) to DIR and exits (compare: gui_harness buildpages:)

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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

#ifdef CLIENT_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#include "client/front.h"
#include "client/game_client.h"
#include "client/music.h"
#include "render/presenter.h"
#include "ta/client_campaign.h"
#include "ta/client_loading.h"
#include "ta/client_menus.h"
#include "ta/client_net_screens.h"
#include "ta/console_commands.h"
#include "ta/client_world.h"

namespace {

// The folder the executable is in. Like the original, the program can sit in the game folder
// and find the game's files beside it.
std::string own_folder(const char* argv0) {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s = n ? std::string(path, n) : std::string(argv0 ? argv0 : "");
#else
    std::string s = argv0 ? argv0 : "";
#endif
    const auto cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string(".") : s.substr(0, cut);
}

// Tests (--pointer-selftest): the window front ends' pointer rules driven through a sequence of
// what the system does (focus, minimise, the system pointer's moves inside and outside the
// window), and SDL mouse events converted as the event loops do. Prints each check; returns 0
// when all pass. (The hold itself is the platform's; no window is opened here.)
int pointer_selftest() {
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) ++failed;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };
    using T = client::InputEvent::Type;
    const int W = 640, H = 480;
    // What the client shows: the pointer (x, y) or off the picture.
    struct Shown {
        bool off = false;
        int x = 0, y = 0;
    } p;
    auto feed = [&](const client::InputEvent& e) {
        if (e.type == T::PointerLeave) p.off = true;
        else if (e.type == T::MouseMove) p = {false, e.x, e.y};
    };
    auto motion = [&](int x, int y) { feed(client::pointer_motion(static_cast<float>(x), static_cast<float>(y), 1, W, H, W, H)); };
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H; };
    // 1. In the window with the focus: held, the pointer where the system pointer is.
    motion(300, 200);
    check(client::pointer_held(true, true, inside(300, 200)) && !p.off && p.x == 300 && p.y == 200, "focused, inside: held, pointer at (300, 200)");
    // 2. Alt+Tab: free at once; the pointer stays where the system pointer is (no event, no warp).
    check(!client::pointer_held(false, true, inside(300, 200)) && p.x == 300 && p.y == 200, "focus lost: free, pointer unchanged");
    // 3. The system pointer moves over the unfocused window: the pointer follows it.
    motion(300, 479);
    check(!p.off && p.x == 300 && p.y == 479, "unfocused, inside: the pointer follows (300, 479)");
    // 4. It leaves the window: the pointer is off the picture (not left on the edge).
    motion(300, 600);
    check(p.off, "unfocused, outside: pointer off the picture");
    // 5. Focus back with the system pointer outside: not held (never pulled in), still off; the
    //    motion the platform reports at the focus change is outside too.
    motion(900, 600);
    check(!client::pointer_held(true, true, inside(900, 600)) && p.off, "focus back, outside: not held, pointer still off");
    // 6. It comes into the window: held from there, the pointer where it came in.
    motion(630, 470);
    check(client::pointer_held(true, true, inside(630, 470)) && !p.off && p.x == 630 && p.y == 470, "comes in: held, pointer at (630, 470)");
    // 7. Out and back with the system pointer moved inside meanwhile: the pointer is where it is
    //    now (the platform's motion at the focus change), not where it was.
    check(!client::pointer_held(false, true, true), "Alt+Tab again: free");
    motion(100, 100);
    check(client::pointer_held(true, true, inside(100, 100)) && !p.off && p.x == 100 && p.y == 100,
          "focus back, inside: held at once, pointer at the system pointer (100, 100)");
    // 8. Minimised / hidden: free; restored with the pointer inside: held.
    check(!client::pointer_held(true, false, true) && client::pointer_held(true, true, true), "minimised: free; restored: held");
    // 9. Scaled windows: window pixels to frame pixels.
    {
        const client::InputEvent e = client::pointer_motion(641.0f, 401.0f, 2, W, H, 2 * W, 2 * H);
        check(e.type == T::MouseMove && e.x == 320 && e.y == 200, "scale 2: (641, 401) -> (320, 200)");
        const client::InputEvent f = client::pointer_motion(1280.0f, 10.0f, 2, W, H, 2 * W, 2 * H);
        check(f.type == T::PointerLeave, "scale 2: x 1280 is outside");
    }
#ifdef CLIENT_HAVE_SDL
    // 10. SDL events as the event loops convert them (no window: its size is the frame's).
    {
        SDL_Event ev{};
        client::InputEvent e;
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.x = -5.0f;
        ev.motion.y = 100.0f;
        check(client::pointer_event(nullptr, ev, 1, W, H, e) && e.type == T::PointerLeave, "SDL motion at x -5: PointerLeave");
        ev.motion.x = 639.0f;
        ev.motion.y = 0.0f;
        check(client::pointer_event(nullptr, ev, 1, W, H, e) && e.type == T::MouseMove && e.x == 639 && e.y == 0, "SDL motion at (639, 0): move");
        ev = SDL_Event{};
        ev.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
        check(client::pointer_event(nullptr, ev, 1, W, H, e) && e.type == T::PointerLeave, "SDL window mouse leave: PointerLeave");
        ev = SDL_Event{};
        ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        ev.button.button = SDL_BUTTON_RIGHT;
        ev.button.x = 1279.0f;
        ev.button.y = 959.0f;
        check(client::pointer_event(nullptr, ev, 2, W, H, e) && e.type == T::MouseDown && e.button == 3 && e.x == 639 && e.y == 479,
              "SDL right button at (1279, 959), scale 2: down at (639, 479)");
        ev = SDL_Event{};
        ev.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        check(!client::pointer_event(nullptr, ev, 1, W, H, e), "SDL focus gained: no pointer event of its own");
    }
#endif
    std::printf("pointer self-test: %d of %d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    // The game folder: the first argument when it isn't an option, else the executable's own
    // folder (the program placed in the game folder, as the original runs).
    const bool dir_given = argc >= 2 && std::string(argv[1]).rfind("--", 0) != 0;
    const std::string game_dir = dir_given ? std::string(argv[1]) : own_folder(argc > 0 ? argv[0] : nullptr);
    {
        std::ifstream probe(game_dir + "\\TotalA.exe", std::ios::binary);
        std::ifstream probe2(game_dir + "\\rev31.gp3", std::ios::binary);
        if (!dir_given && !probe && !probe2)
            std::fprintf(stderr, "no game files found next to the program (%s); place it in the game folder or pass the folder:\n"
                                 "usage: ta_play [<game_dir>] [--map NAME] [--side arm|core] [--headless --frames N --script F --shots a,b --out P] ...\n",
                         game_dir.c_str());
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
    std::string settings_path = own_folder(argc > 0 ? argv[0] : nullptr) + "\\settings.txt";
    front.title = "Total Annihilation";
    std::string end_screen_file, saves_dir, dump_pages_dir, loading_test_file;
    bool loading_trace = false;
    std::vector<std::string> net_screens;
    std::string key_sweep;  // --key-sweep KEYS:MODS
    for (int i = dir_given ? 2 : 1; i < argc; ++i) {
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
        if (a == "--loading-test") {
            loading_test_file = next();
            continue;
        }
        if (a == "--pointer-selftest") return pointer_selftest();
        if (a == "--loading-trace") {  // tests: print the loader's loading-screen reports
            loading_trace = true;
            continue;
        }
        if (a == "--saves") {
            saves_dir = next();
            continue;
        } else if (a == "--net-screen") {
            net_screens.push_back(next());
            continue;
        } else if (a == "--key-sweep") {
            key_sweep = next();
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
        else if (a == "--dump-build-pages") {
            dump_pages_dir = next();
            direct = true;
        }
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
        if (!art.load(game_dir, err)) {
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
            std::string save_dir = saves_dir.empty() ? game_dir + "\\SAVEGAME" : saves_dir;
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
        if (!loading_test_file.empty()) {
            // Tests: loading screen frames from a file, one item per line (# comments):
            //   map NAME | nomap          the map line (nomap: none, as for missions)
            //   net 0|1, sync 0|1         a network game; every machine loaded
            //   player NAME PERCENT LOADED LOCAL   a network game's player (slot order)
            //   remote INDEX PERCENT LOADED        a listed player's progress as its packets say
            //   clock MS                  the clock at the next frames
            //   frame T,TE,U,A,D,E        the rows (percent) and one redraw, written as
            //                             <--out PREFIX>load_<n>.bmp (n counts the frames from 0)
            // (compare: gui_harness loadscreen / loadframe: / loadplayer: / clock:).
            std::ifstream in(loading_test_file);
            client::LoadingState st;
            int n = 0;
            for (std::string line; std::getline(in, line);) {
                if (const auto c = line.find('#'); c != std::string::npos) line.resize(c);
                std::istringstream ls(line);
                std::string key;
                if (!(ls >> key)) continue;
                if (key == "map") {
                    std::getline(ls >> std::ws, st.map);
                    while (!st.map.empty() && (st.map.back() == '\r' || st.map.back() == ' ')) st.map.pop_back();
                    st.map_line = true;
                } else if (key == "nomap") {
                    st.map_line = false;
                } else if (key == "net") {
                    int v = 0;
                    ls >> v;
                    st.network = v != 0;
                } else if (key == "sync") {
                    int v = 0;
                    ls >> v;
                    st.synchronised = v != 0;
                } else if (key == "player") {
                    client::LoadingPlayer p;
                    int loaded = 0, local = 0;
                    ls >> p.name >> p.percent >> loaded >> local;
                    p.loaded = loaded != 0;
                    p.local = local != 0;
                    st.players.push_back(p);
                } else if (key == "remote") {
                    int k = 0, pct = 0, loaded = 0;
                    ls >> k >> pct >> loaded;
                    if (k >= 0 && k < static_cast<int>(st.players.size())) {
                        st.players[static_cast<std::size_t>(k)].percent = pct;
                        st.players[static_cast<std::size_t>(k)].loaded = loaded != 0;
                    }
                } else if (key == "clock") {
                    ls >> st.now_ms;
                } else if (key == "frame") {
                    std::string v;
                    ls >> v;
                    std::sscanf(v.c_str(), "%d,%d,%d,%d,%d,%d", &st.rows[0], &st.rows[1], &st.rows[2], &st.rows[3], &st.rows[4], &st.rows[5]);
                    render::Image8 img(fes.width, fes.height, 0);
                    ta::draw_loading_screen(art, img, st);
                    const std::string name = front.screenshot_prefix + "load_" + std::to_string(n) + ".bmp";
                    render::write_bmp(name, img, art.content().palette);
                    std::printf("loading frame %d -> %s, flash %d %d %d %d %d %d\n", n, name.c_str(), st.flash[0], st.flash[1], st.flash[2],
                                st.flash[3], st.flash[4], st.flash[5]);
                    ++n;
                }
            }
            return 0;
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
        if (!key_sweep.empty()) {
            // KEYS:MODS[:X,Y] (a click at X,Y before each key)
            const auto c = key_sweep.find(':');
            std::string mods = c == std::string::npos ? std::string("none") : key_sweep.substr(c + 1);
            int cx = -1, cy = -1;
            std::string typed;
            if (const auto c2 = mods.find(':'); c2 != std::string::npos) {
                std::sscanf(mods.c_str() + c2 + 1, "%d,%d", &cx, &cy);
                if (const auto c3 = mods.find(':', c2 + 1); c3 != std::string::npos) typed = mods.substr(c3 + 1);
                mods.resize(c2);
            }
            return client::run_key_sweep(fes, key_sweep.substr(0, c), mods, cx, cy, typed);
        }
        return client::run_front_end(fes, front);
    }

    auto world = std::make_unique<ta::ClientWorld>();
    std::string err;
    std::function<void(float, const std::string&)> trace;
    if (loading_trace) {
        // Tests: the loader's reports as the loading screen hears them (rows, map line), timed.
        const auto t0 = std::chrono::steady_clock::now();
        trace = [t0](float f, const std::string& stage) {
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::printf("load report %6lld ms: %s %d\n", ms, stage.c_str(), static_cast<int>(f * 100.0f));
        };
    }
    if (!world->load(game_dir, setup, err, trace)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!dump_pages_dir.empty()) {
        // Tests: every builder's build pages for each viewing side, as the client lays them out
        // and draws them (no selection abilities missing, the BUILD tab pressed, no counts):
        // a listing of their buttons and a picture of each page's box.
        const client::Content& c = world->content();
        std::ofstream out(dump_pages_dir + "/client_pages.txt");
        int written = 0;
        for (std::size_t t = 1; t < c.types.size(); ++t) {
            const client::UnitTypeLook& look = c.types[t];
            const std::string& uname = world->defs().units[t].unit_name;
            for (std::size_t s = 0; s < look.build_pages.size(); ++s)
                for (std::size_t p = 0; p < look.build_pages[s].size(); ++p) {
                    const client::Panel& pg = look.build_pages[s][p];
                    out << "PAGE " << uname << " side " << s << " page " << (p + 1) << "\n";
                    for (const client::Button& b : pg.buttons)
                        out << "  " << b.name << " " << b.x << " " << b.y << " " << b.w << " " << b.h << " " << (b.grayed ? 1 : 0) << "\n";
                    const int pw = pg.w > 0 ? pg.w : 128, ph = pg.h > 0 ? pg.h : 352;
                    render::Image8 img(pw, ph, 0);
                    auto disabled = [&](const client::Button& b) { return b.grayed; };
                    client::compose_panel(
                        img, pg.x, pg.y, pg, nullptr,
                        [&](const client::Button& b) { return client::button_frame(b, b.action == client::Action::BuildTab, disabled(b), 0); },
                        [&](const client::Button& b) { return client::button_dims(b, disabled(b)); }, c.minimap_grey, c.shade_levels);
                    render::write_bmp(dump_pages_dir + "/c_" + uname + "_s" + std::to_string(s) + "_p" + std::to_string(p + 1) + ".bmp", img, c.palette);
                    ++written;
                }
        }
        std::printf("build pages: %d written to %s\n", written, dump_pages_dir.c_str());
        return 0;
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
        std::printf("  unit %zu player %d %s at (%d, %d) hp %d flags %08x built %.2f order %d secondary %d group %u\n", i, b.ticks[i].owner,
                    world->defs().units[b.ticks[i].type_index].unit_name.c_str(), u.pos[0] >> 16, u.pos[2] >> 16, u.hit_points, u.flags,
                    static_cast<double>(1.0f - u.build_fraction_left), u.orders ? u.orders->kind : -1,
                    u.secondary_orders ? u.secondary_orders->kind : -1, static_cast<unsigned>(u.group));
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
