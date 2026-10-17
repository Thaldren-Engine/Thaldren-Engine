// The original's menus and in-game panels drawn offscreen, for pixel comparison with the client.
//
// The game is never started: TotalA.exe is held at the start of its WinMain (dbg_remote.h:
// the C runtime is up, none of the game's own code has run, every window / dialog / screen /
// DirectX / sound entry point is a trap). The harness then calls the original's own start-up
// pieces that need no window (game state, archives, translations, the front end's resources
// and GUI system), points its display context at an in-memory 8-bit surface instead of
// DirectDraw, calls a screen's own set-up routine (the same routine the menu flow calls) and
// the GUI compositor, and writes the surface with the display palette as an 8-bit BMP. Sound,
// CD audio, the pointer and the frame presentation are replaced by routines that do nothing.
//
// Usage: gui_harness <TotalA.exe> <out_prefix> [screen ...] [--res WxH] [--palettes PALETTE.PAL GUIPAL.PAL] [--type N]
//   screens: main single options skirmish ingame (the in-game menu, F2) prefs (its options)
//            and the in-battle option pages soundsrt musicrt speedsrt visualrt; exitmenu; tabmenu
//   Writes <out_prefix><screen>.bmp.
//   In-battle panels (after "battle"): open:NAME opens a GUI layout with the build menu's
//   handler (as the battle opens <unit><page>.GUI); text:GADGET=TEXT sets a gadget's caption
//   (as the build menu sets its "+n" counts) and redraws it; mouse:X,Y,HELD,MESSAGE puts one
//   pointer event (HELD 1 left / 2 right, MESSAGE 0x201 left down, 0x202 up, 0x204 right down,
//   0x205 up) into the input queue and runs one pass of the GUI's input; state:GADGET=V sets a
//   gadget's state as the battle does for the tabs and switches; each pass prints the
//   battle's interface flags (GS+0x37EBE: 0x80 next page, 0x100 previous page) and the order
//   mode (GS+0x2CC3); shot:NAME writes the composited screen.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dbg_remote.h"

namespace {

using harness::u32;

// Addresses in TotalA.exe (docs/RE_MEMORY.md "Client", "Front end").
constexpr u32 kGameStatePtr = 0x00511DE8, kDisplayPtr = 0x0051FBD0, kDisplay = 0x0051F320;
constexpr u32 kGameStateAlloc = 0x0041D920, kResetViewport = 0x004B52E0, kMountArchives = 0x0041D4C0, kLoadTranslations = 0x004C54F0;
constexpr u32 kInitFrontEnd = 0x00491200, kBlitGuiRoot = 0x004AB170, kAllocSurface = 0x004C69F0, kSetActiveSurface = 0x004C61F0,
              kSetOverrideSurface = 0x004C69A0, kFillSurface = 0x004BF6F0;
constexpr u32 kMainMenu = 0x004263B0, kSinglePlayer = 0x004777A0, kOptionsMenu = 0x00460160, kSkirmish = 0x0047BBB0,
              kGameMenu = 0x00460CC0;
constexpr u32 kGuiOpen = 0x004AA8F0, kGuiPopAll = 0x004263B0;
// GameState offsets.
constexpr u32 kGsDisplay = 0x0C, kGsAudio = 0x10, kGsGui = 0x519, kGsOffscreen = 0x37E1B, kGsOffscreenW = 0x37E1F, kGsOffscreenH = 0x37E23;
// Display context offsets.
constexpr u32 kDcOverride = 0xBC, kDcUseOverride = 0xDC, kDcWidth = 0xD4, kDcHeight = 0xD8, kDcFlags = 0xF0, kDcAlpha = 0xC0, kDcShade = 0xC4,
              kDcLight = 0xC8, kDcPalette = 0x214;

struct Frame {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    std::uint8_t pal[256][4] = {};
};

void put32(std::vector<std::uint8_t>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
bool write_bmp(const std::string& path, const Frame& f) {
    const u32 stride = (static_cast<u32>(f.w) + 3u) & ~3u, size = stride * static_cast<u32>(f.h), off = 14 + 40 + 1024;
    std::vector<std::uint8_t> b = {'B', 'M'};
    put32(b, off + size);
    put32(b, 0);
    put32(b, off);
    put32(b, 40);
    put32(b, static_cast<u32>(f.w));
    put32(b, static_cast<u32>(f.h));
    b.insert(b.end(), {1, 0, 8, 0});
    put32(b, 0);
    put32(b, size);
    put32(b, 2835);
    put32(b, 2835);
    put32(b, 256);
    put32(b, 0);
    for (int i = 0; i < 256; ++i) b.insert(b.end(), {f.pal[i][2], f.pal[i][1], f.pal[i][0], 0});
    for (int y = f.h - 1; y >= 0; --y) {
        b.insert(b.end(), f.px.begin() + static_cast<std::ptrdiff_t>(y) * f.w, f.px.begin() + static_cast<std::ptrdiff_t>(y + 1) * f.w);
        for (u32 k = static_cast<u32>(f.w); k < stride; ++k) b.push_back(0);
    }
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fwrite(b.data(), 1, b.size(), fp);
    std::fclose(fp);
    return true;
}

class Gui {
public:
    explicit Gui(harness::DbgRemote& r) : r_(r) {}

    bool init(int w, int h) {
        w_ = w;
        h_ = h;
        // Routines that would reach sound, CD audio, the pointer, the screen or a notice do nothing.
        struct S {
            u32 at, value;
            std::uint16_t pop;
        };
        const S stubs[] = {
            {0x0047ED40, 0, 0},   // sound set-up from totala.ini
            {0x0042F7E0, 0, 0},   // the sound bank
            {0x004CEDC0, 0, 4},   // CD audio on/off
            {0x004CE7A0, 0, 4},   // CD play mode
            {0x004CD9D0, 0, 4},   // CD track count
            {0x00490FE0, 0, 0},   // CD reopen
            {0x004CE690, 0, 4},   // CD track category
            {0x004CE910, 0, 4},   // CD play / pause
            {0x0045BCC0, 0, 0},   // volumes and brightness from the settings
            {0x0047F210, 0, 8},   // play a sound by name
            {0x0047F1A0, 0, 8},   // play an interface sound
            {0x0047F0C0, 0, 8},   // play a sound by index
            {0x004C2470, 0, 0},   // hide the pointer
            {0x004C2870, 0, 0},   // show the pointer
            {0x004C22F0, 0, 8},   // move the pointer
            {0x004C63A0, 0, 0},   // present the frame
            {0x004B5070, 1, 0x14},  // DirectX version check: accepted
            {0x00429000, 0, 0},   // data version notice
            {0x004CE1D0, 0, 0},   // CD player window present?
            {0x004CFF20, 0, 0},   // no sound driver?
            {0x0047F680, 0, 0},   // unit speech
            {0x00494E70, 0, 0},   // music mood by activity
            {0x00457710, 0, 0},   // lobby-launched multiplayer (none)
        };
        for (const S& s : stubs)
            if (!r_.stub(s.at, s.value, s.pop)) return false;
        if (!r_.call(kGameStateAlloc, 0, {})) return false;
        gs_ = r_.r32(kGameStatePtr);
        if (!gs_) return false;
        // The display context: no window, no DirectDraw; tables in memory; the screen size.
        r_.w32(kDisplayPtr, kDisplay);
        if (!r_.call(kResetViewport, 0, {kDisplay})) return false;
        r_.w32(kDisplay + kDcAlpha, r_.alloc(0x10000));
        r_.w32(kDisplay + kDcShade, r_.alloc(0x2000));
        r_.w32(kDisplay + kDcLight, r_.alloc(0x2000));
        r_.w32(kDisplay + 0xCC, r_.alloc(0x1000));  // grey remap
        r_.w32(kDisplay + 0xD0, r_.alloc(0x1000));  // dim remap
        r_.w16(kDisplay + kDcFlags, static_cast<std::uint16_t>(r_.r32(kDisplay + kDcFlags) | 0x20 | 0x40 | 0x80 | 0x200));
        r_.w32(kDisplay + 0xF2, 30);  // the key queue's capacity
        r_.w32(kDisplay + 0x44, 1);   // "DIB" display: the windowed display-mode list, no DirectDraw
        r_.w32(kDisplay + 0xE8, 30);  // ticks per second
        // The pointer's event queue (32 events of 6 words: x, y, buttons held, -, message, -).
        r_.w32(kDisplay + 0x186, 32);
        r_.w32(kDisplay + 0x18A, r_.alloc(32 * 24));
        r_.w32(kDisplay + 0x18E, 0);
        r_.w32(kDisplay + 0x192, 0);
        set_screen(640, 480);
        if (!r_.call(0x004B62D0, 0, {30})) return false;  // the periodic timer table (30 ticks a second)
        if (!r_.call(kMountArchives, 0, {})) return false;
        r_.w32(gs_ + kGsDisplay, kDisplay);
        r_.w8(gs_ + 1, 3);
        r_.w8(gs_ + 2, 1);
        r_.w8(gs_ + 3, 1);
        r_.call(kLoadTranslations, 0, {r_.str("gamedata\\translate.tdf"), r_.str("english")});
        // An audio engine that never opened a device, except that the CD (music) counts as open so
        // the music options are offered as in a game with its music.
        const u32 audio = r_.alloc(0x400);
        r_.w32(audio, 1);
        r_.w32(gs_ + kGsAudio, audio);
        if (!r_.call(kInitFrontEnd, 0, {})) return false;
        return true;
    }

    // The screen size (the battle's display mode for in-game panels): a new offscreen surface.
    void set_screen(int w, int h) {
        r_.w32(kDisplay + kDcWidth, static_cast<u32>(w));
        r_.w32(kDisplay + kDcHeight, static_cast<u32>(h));
        if (!gs_) return;
        u32 surf = 0;
        r_.call(kAllocSurface, 0, {r_.str("OFFSCREEN"), static_cast<u32>(w), static_cast<u32>(h)}, &surf);
        r_.w32(gs_ + kGsOffscreenW, static_cast<u32>(w));
        r_.w32(gs_ + kGsOffscreenH, static_cast<u32>(h));
        r_.w32(gs_ + kGsOffscreen, surf);
        r_.call(kSetActiveSurface, 0, {surf});
        r_.call(kSetOverrideSurface, 0, {surf});
    }
    u32 gs() const { return gs_; }
    u32 gui() const { return gs_ + kGsGui; }

    // Composites the open GUI layouts onto the screen surface and reads it.
    bool grab(Frame& f) {
        if (!r_.call(kBlitGuiRoot, 0, {gui(), 0, 0})) return false;
        return grab_raw(f);
    }
    // Reads the screen surface as it is (what the last frame presented).
    bool grab_raw(Frame& f) {
        const u32 surf = r_.r32(kDisplay + kDcOverride);
        u32 hdr[4] = {};
        if (!surf || !r_.read(surf, hdr, sizeof hdr)) return false;
        f.w = static_cast<int>(hdr[0]);
        f.h = static_cast<int>(hdr[1]);
        const int pitch = static_cast<int>(hdr[2]);
        if (f.w <= 0 || f.h <= 0 || pitch < f.w) return false;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(pitch) * static_cast<std::size_t>(f.h));
        if (!r_.read(hdr[3], raw.data(), raw.size())) return false;
        f.px.resize(static_cast<std::size_t>(f.w) * static_cast<std::size_t>(f.h));
        for (int y = 0; y < f.h; ++y) std::memcpy(&f.px[static_cast<std::size_t>(y) * f.w], &raw[static_cast<std::size_t>(y) * pitch], static_cast<std::size_t>(f.w));
        return r_.read(kDisplay + kDcPalette, f.pal, sizeof f.pal);
    }

private:
    harness::DbgRemote& r_;
    u32 gs_ = 0;
    int w_ = 640, h_ = 480;
};

// The end-of-battle statistics' inputs, one item per line (# comments):
//   tick N              the game step the battle ended at
//   killmul F / timemul F   the map's score factors (its [GlobalHeader] killmul, timemul)
//   won 0|1             the local player won (end flags 0x10)
//   local N             the local player's slot
//   type N              the game type (2 skirmish)
//   player SLOT COLOUR CONTROLLER STATE CREATED PRESENT WATCH KILLS LOSSES EPROD MPROD EWASTE MWASTE NAME...
// The same file drives the client's end screen (ta_play --end-screen).
bool end_setup(harness::DbgRemote& r, Gui& g, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, fp)) > 0;) text.append(buf, n);
    std::fclose(fp);
    const u32 gs = g.gs();
    u32 tick = 0, won = 0, type = 2, local = 0;
    float km = 0.0f, tm = 0.0f;
    std::vector<std::uint8_t> zero(0x14B, 0);
    u32 infos = r.alloc(10 * 0x100);
    for (u32 p = 0; p < 10; ++p) {
        const u32 rec = gs + 0x1B63 + p * 0x14B;
        r.write(rec, zero.data(), zero.size());
        r.w32(rec + 0x27, infos + p * 0x100);
    }
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        if (const auto c = line.find('#'); c != std::string::npos) line.resize(c);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        char key[32] = {};
        if (std::sscanf(line.c_str(), "%31s", key) != 1) continue;
        const std::string k = key;
        const char* rest = line.c_str() + k.size();
        if (k == "tick") tick = static_cast<u32>(std::strtoul(rest, nullptr, 10));
        else if (k == "killmul") km = static_cast<float>(std::atof(rest));
        else if (k == "timemul") tm = static_cast<float>(std::atof(rest));
        else if (k == "won") won = static_cast<u32>(std::atoi(rest));
        else if (k == "local") local = static_cast<u32>(std::atoi(rest));
        else if (k == "type") type = static_cast<u32>(std::atoi(rest));
        else if (k == "player") {
            int slot, colour, controller, state, present, watch, kills, losses, used = 0;
            unsigned created;
            double ep, mp, ew, mw;
            if (std::sscanf(rest, "%d %d %d %d %u %d %d %d %d %lf %lf %lf %lf %n", &slot, &colour, &controller, &state, &created, &present, &watch, &kills,
                            &losses, &ep, &mp, &ew, &mw, &used) < 13 ||
                slot < 0 || slot > 9)
                continue;
            const std::string name = rest + used;
            const u32 rec = gs + 0x1B63 + static_cast<u32>(slot) * 0x14B, info = infos + static_cast<u32>(slot) * 0x100;
            r.w32(rec, controller ? 1u : 0u);
            r.w8(rec + 0x73, static_cast<std::uint8_t>(controller));
            r.w8(rec + 0x146, static_cast<std::uint8_t>(state));
            r.w32(rec + 0x140, created);
            r.w8(rec + 0x22, present ? 0 : 1);
            r.w8(info + 0x9B, watch ? 0x40 : 0);
            r.w8(info + 0x96, static_cast<std::uint8_t>(colour));
            r.w16(rec + 0xFC, static_cast<std::uint16_t>(kills));
            r.w16(rec + 0xFE, static_cast<std::uint16_t>(losses));
            r.write(rec + 0xAC, &ep, 8);
            r.write(rec + 0xB4, &mp, 8);
            r.write(rec + 0xCC, &ew, 8);
            r.write(rec + 0xD4, &mw, 8);
            r.write(rec + 0x2B, name.c_str(), std::min<std::size_t>(name.size() + 1, 0x1E));
        }
    }
    r.w32(gs + 0x38A47, tick);
    r.w8(gs + 0x2A42, static_cast<std::uint8_t>(local));
    u32 schema = r.r32(gs + 0x391E9);
    if (!schema) {
        schema = r.alloc(0x2000);
        r.w32(gs + 0x391E9, schema);
    }
    r.w32(schema, type);
    r.write(schema + 0xD54, &km, 4);
    r.write(schema + 0xD58, &tm, 4);
    std::uint16_t ef = 0;
    r.read(gs + 0x3923B, &ef, 2);
    ef = static_cast<std::uint16_t>(won ? (ef | 0x10) : (ef & ~0x10));
    r.write(gs + 0x3923B, &ef, 2);
    if (!r.call(0x0041DC20, 0, {})) return false;
    for (u32 k = 0; k < 10; ++k) {
        const u32 row = gs + 0x38DD9 + k * 0x3A;
        char name[0x1F] = {};
        r.read(row, name, 0x1E);
        if (!name[0]) continue;
        std::int32_t v[7];
        r.read(row + 0x1E, v, sizeof v);
        std::printf("end row %u: %s: %d %d %d %d %d %d %d\n", k, name, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    }
    std::int32_t mx[7];
    r.read(gs + 0x3918F, mx, sizeof mx);
    std::printf("end maxima: %d %d %d %d %d %d %d\n", mx[0], mx[1], mx[2], mx[3], mx[4], mx[5], mx[6]);
    // The state machine at its statistics state (a skirmish's way there), the network
    // packet pass doing nothing, the clock held.
    r.stub(0x00453D40, 0, 0);
    r.stub(0x004B6340, 1000, 0);
    r.w32(gs + 0x39057, 5);
    if (!r.call(0x0041F7F0, 0, {})) return false;
    std::printf("end screen set up: state %u\n", r.r32(gs + 0x39057));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: gui_harness <TotalA.exe> <out_prefix> [screen ...] [--res WxH]\n");
        return 2;
    }
    std::vector<std::string> screens;
    int bw = 640, bh = 480;
    std::string battle_pal, battle_guipal, battle_shade;
    int game_type = -1;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--type" && i + 1 < argc) {
            game_type = std::atoi(argv[++i]);
        } else if (a == "--palettes" && i + 2 < argc) {
            // palettes\PALETTE.PAL and palettes\GUIPAL.PAL as files (the battle's colours).
            battle_pal = argv[++i];
            battle_guipal = argv[++i];
        } else if (a == "--shade" && i + 1 < argc) {
            // palettesPALETTE.SHD as a file: the battle's shade table on the display (dimmed screens).
            battle_shade = argv[++i];
        } else if (a == "--res" && i + 1 < argc) {
            const std::string v = argv[++i];
            bw = std::atoi(v.c_str());
            const auto x = v.find('x');
            bh = x == std::string::npos ? 480 : std::atoi(v.c_str() + x + 1);
        } else {
            screens.push_back(a);
        }
    }
    if (screens.empty()) screens = {"main", "single", "options"};
    harness::DbgRemote r;
    if (!r.start(argv[1])) {
        std::fprintf(stderr, "cannot start %s held at WinMain\n", argv[1]);
        return 1;
    }
    Gui g(r);
    if (!g.init(640, 480)) {
        std::fprintf(stderr, "front-end set-up failed\n");
        return 1;
    }
    std::printf("front end set up (game state 0x%08X)\n", g.gs());
    const std::string out = argv[2];
    const u32 gs = g.gs();
    for (const std::string& s : screens) {
        bool ok = true;
        // Menus (640 x 480).
        if (s == "main") ok = r.call(kMainMenu, 0, {});
        else if (s == "single") ok = r.call(kSinglePlayer, 0, {});
        else if (s == "options") ok = r.call(kOptionsMenu, 0, {});
        else if (s == "skirmish") ok = r.call(kSkirmish, 0, {});
        else if (s == "sounds" || s == "soundsrt") ok = r.call(0x0045DE30, 0, {});
        else if (s == "music" || s == "musicrt") ok = r.call(0x0045D7C0, 0, {});
        else if (s == "speeds" || s == "speedsrt") ok = r.call(0x0045ED50, 0, {});
        else if (s == "visuals" || s == "visualrt") {
            // The windowed display-mode list (the DIB path) instead of DirectDraw's.
            const u32 dib = r.r32(0x0051F320 + 0x44);
            r.w32(0x0051F320 + 0x44, 1);
            ok = r.call(0x0045E5E0, 0, {0});
            r.w32(0x0051F320 + 0x44, dib);
        }
        else if (s.rfind("tick:", 0) == 0) {
            // Whole frames of the game's own main loop (interface, front-end state machine).
            const int n = std::atoi(s.c_str() + 5);
            for (int k = 0; k < n && ok; ++k) ok = r.call(0x00499890, 0, {});
            if (!ok) std::printf("%s: frame failed\n", s.c_str());
            continue;
        } else if (s.rfind("key:", 0) == 0) {
            // A key into the game's input queue (characters as typed; esc, enter, tab, f1..f12).
            const std::string k = s.substr(4);
            u32 code = static_cast<unsigned char>(k[0]);
            if (k == "esc") code = 27;
            else if (k == "enter") code = 13;
            else if (k == "tab") code = 9;
            else if (k == "bksp") code = 8;
            else if (k == "space") code = 0x20;
            else if (k == "del") code = 0xEF;
            else if (k == "home") code = 0xF0;
            else if (k == "end") code = 0xF1;
            else if (k == "left") code = 0xF4;
            else if (k == "up") code = 0xF5;
            else if (k == "right") code = 0xF6;
            else if (k == "down") code = 0xF7;
            else if (k.size() > 1 && k[0] == 'f') code = 0xE2 + static_cast<u32>(std::atoi(k.c_str() + 1)) - 1;
            r.call(0x004C1B20, 0, {code});
            continue;
        } else if (s.rfind("open:", 0) == 0) {
            // A layout opened as the battle opens a build menu page (handler: the build menu's).
            u32 lay = 0;
            ok = r.call(kGuiOpen, 0, {g.gui(), r.str(s.substr(5)), 0}, &lay);
            if (lay) {
                r.w32(lay + 8, 0x0041AA00);
                r.w32(lay + 0xC, 0);
                r.call(0x004A81E0, 0, {g.gui(), r.r32(lay + 0x10) | 0x40});
            }
            std::printf("%s: layout %08X\n", s.c_str(), lay);
            continue;
        } else if (s.rfind("text:", 0) == 0) {
            const std::string t = s.substr(5);
            const auto eq = t.find('=');
            const u32 top = r.r32(g.gui() + 0x18);
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            u32 idx = 0xFFFFFFFFu;
            if (gadgets && eq != std::string::npos) r.call(0x0049FE60, 0, {gadgets, r.str(t.substr(0, eq))}, &idx);
            if (idx != 0xFFFFFFFFu) {
                const std::string text = t.substr(eq + 1);
                r.write(gadgets + idx * 0x15B + 0xB6, text.c_str(), text.size() + 1);
                r.call(0x004A5F40, 0, {g.gui(), idx});
            }
            std::printf("%s: gadget %d\n", s.c_str(), static_cast<int>(idx));
            continue;
        } else if (s.rfind("state:", 0) == 0) {
            // A gadget's state (pressed / stage), as the battle sets the tabs' and switches' (0x4A11C0).
            const std::string t = s.substr(6);
            const auto eq = t.find('=');
            const u32 top = r.r32(g.gui() + 0x18);
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            u32 idx = 0xFFFFFFFFu;
            if (gadgets && eq != std::string::npos) r.call(0x0049FE60, 0, {gadgets, r.str(t.substr(0, eq))}, &idx);
            if (idx != 0xFFFFFFFFu) {
                r.call(0x004A11C0, 0, {g.gui(), idx, static_cast<u32>(std::atoi(t.c_str() + eq + 1))});
                r.call(0x004A5F40, 0, {g.gui(), idx});
            }
            std::printf("%s: gadget %d\n", s.c_str(), static_cast<int>(idx));
            continue;
        } else if (s.rfind("mouse:", 0) == 0) {
            int x = 0, y = 0, held = 0, msg = 0;
            std::sscanf(s.c_str() + 6, "%d,%d,%d,%i", &x, &y, &held, &msg);
            const u32 cap = r.r32(0x0051F320 + 0x186), buf = r.r32(0x0051F320 + 0x18A), wr = r.r32(0x0051F320 + 0x18E);
            const u32 ev[6] = {static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(held), 0, static_cast<u32>(msg), 0};
            r.write(buf + wr * 24, ev, sizeof ev);
            r.w32(0x0051F320 + 0x18E, (wr + 1) % cap);
            ok = r.call(0x004A9FD0, 0, {g.gui()});
            std::uint16_t flags = 0;
            std::uint8_t mode = 0;
            r.read(gs + 0x37EBE, &flags, 2);
            r.read(gs + 0x2CC3, &mode, 1);
            std::printf("%s: %s, interface flags %04X, order mode %d\n", s.c_str(), ok ? "ok" : "failed", flags, mode);
            continue;
        } else if (s.rfind("shot:", 0) == 0) {
            Frame f;
            if (!g.grab(f)) {
                std::printf("%s: not drawn\n", s.c_str());
                continue;
            }
            const std::string name = s.substr(5);
            write_bmp(out + name + ".bmp", f);
            std::printf("%s -> %s%s.bmp\n", s.c_str(), out.c_str(), name.c_str());
            continue;
        } else if (s == "battle") {
            // A battle's screen: its display mode, no layouts open, a test pattern where the battle
            // would be (so any shading of it shows), the in-battle flags.
            for (int k = 0; k < 16 && r.r32(g.gui() + 0x18); ++k) r.call(0x004A9660, 0, {g.gui()});
            g.set_screen(bw, bh);
            const u32 surf = r.r32(0x0051F320 + 0xBC);
            std::vector<std::uint8_t> pat(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh));
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) pat[static_cast<std::size_t>(y) * bw + x] = static_cast<std::uint8_t>(16 + ((x / 16 + y / 16) & 1) * 64 + (x % 16));
            r.write(r.r32(surf + 0xC), pat.data(), pat.size());
            r.w8(gs + 0x2A44, static_cast<std::uint8_t>(r.r32(gs + 0x2A44) | 4));
            r.w8(gs + 0x38A51, static_cast<std::uint8_t>(r.r32(gs + 0x38A51) | 1));
            // An empty unit type table (the build menu's handler looks gadget names up in it).
            r.w32(gs + 0x1439B, r.alloc(0x1000));
            r.w32(gs + 0x1438F, 1);
            // The game type (the first word behind GS+0x391E9; 2 and 3 show the menu's briefing
            // button as "Settings", 3 is a network game).
            if (game_type >= 0 && r.r32(gs + 0x391E9)) r.w32(r.r32(gs + 0x391E9), static_cast<u32>(game_type));
            // The battle's interface colours (built by the battle loading from the game palette
            // and GUIPAL, 0x004AC7D0) and the game palette on the display, when the files are given.
            if (!battle_pal.empty()) {
                std::vector<std::uint8_t> pal, gpal;
                auto load = [](const std::string& p, std::vector<std::uint8_t>& out) {
                    FILE* fp = std::fopen(p.c_str(), "rb");
                    if (!fp) return false;
                    out.assign(1024, 0);
                    const std::size_t n = std::fread(out.data(), 1, out.size(), fp);
                    std::fclose(fp);
                    return n == 1024;
                };
                if (load(battle_pal, pal) && load(battle_guipal, gpal)) {
                    const u32 rp = r.alloc(1024), rg = r.alloc(1024);
                    r.write(rp, pal.data(), pal.size());
                    r.write(rg, gpal.data(), gpal.size());
                    r.call(0x004AC7D0, 0, {g.gui(), rp, rg});
                    r.write(0x0051F320 + 0x214, pal.data(), pal.size());
                    if (!battle_shade.empty()) {
                        if (FILE* fp = std::fopen(battle_shade.c_str(), "rb")) {
                            std::vector<std::uint8_t> shd(0x2000, 0);
                            const std::size_t n = std::fread(shd.data(), 1, shd.size(), fp);
                            std::fclose(fp);
                            if (n == shd.size()) r.write(r.r32(0x0051F320 + 0xC4), shd.data(), shd.size());
                        }
                    }
                } else {
                    std::printf("battle palettes not read\n");
                }
            }
            std::printf("battle screen %dx%d\n", bw, bh);
            continue;
        } else if (s.rfind("missionname:", 0) == 0) {
            // The map's mission name (map info +0xA14; the restart dialog shows it).
            u32 schema = r.r32(gs + 0x391E9);
            if (!schema) {
                schema = r.alloc(0x2000);
                r.w32(gs + 0x391E9, schema);
            }
            const std::string t = s.substr(12);
            r.write(schema + 0xA14, t.c_str(), t.size() + 1);
            continue;
        } else if (s.rfind("gadget:", 0) == 0) {
            // gadget:NAME: the top layout's gadget record's kind and colour words (+0x1F, +0x23).
            const u32 top = r.r32(g.gui() + 0x18);
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            u32 idx = 0xFFFFFFFFu;
            if (gadgets) r.call(0x0049FE60, 0, {gadgets, r.str(s.substr(7))}, &idx);
            if (idx != 0xFFFFFFFFu)
                std::printf("%s: kind %u, +0x1F %d, +0x23 %d\n", s.c_str(), r.r32(gadgets + idx * 0x15B) & 0xFF, static_cast<int>(r.r32(gadgets + idx * 0x15B + 0x1F)),
                            static_cast<int>(r.r32(gadgets + idx * 0x15B + 0x23)));
            continue;
        } else if (s == "lightinfo") {
            // Debugging: the display's light table (0xC8) and its level-15 row's first entries, and
            // the display flags (bit 0x80: lit blits on).
            const u32 t = r.r32(0x0051F320 + 0xC8);
            std::uint8_t row[256] = {};
            if (t) r.read(t + 15 * 256, row, sizeof row);
            std::printf("light table %08X flags %04X: level 15:", t, r.r32(0x0051F320 + 0xF0) & 0xFFFF);
            for (int k = 80; k < 96; ++k) std::printf(" %u", row[k]);
            std::printf("\n");
            continue;
        } else if (s == "shadeinfo") {
            // Debugging: the display's shade table (dimmed screens) and a few of its level-8 entries.
            const u32 t = r.r32(0x0051F320 + 0xC4);
            std::uint8_t row[256] = {};
            if (t) r.read(t + 8 * 256, row, sizeof row);
            std::printf("shade table %08X: level 8: 175->%u 230->%u 255->%u 66->%u\n", t, row[175], row[230], row[255], row[66]);
            continue;
        } else if (s.rfind("endmsn:", 0) == 0) {
            // The end of a battle's statistics (after "battle"): the players' records from a
            // spec file, the original's statistics pass (0x41DC20, its rows printed), then the
            // end-of-battle state machine (0x41F7F0) entered at its statistics state at clock
            // tick 1000 (see end_spec below).
            if (!end_setup(r, g, s.substr(7))) std::printf("%s: failed\n", s.c_str());
            continue;
        } else if (s.rfind("endrun:", 0) == 0) {
            // endrun:FIRST,LAST[,SHOT...]: frames FIRST..LAST of the state machine, frame f at
            // clock tick 1000 + T(f + 1), T(n) = (n * 1000 / 30) * 30 / 1000 (the client's headless clock: frame
            // f here is its frame f + 1, its frame 0 the set-up); the screen as presented after the
            // frames listed is written as <out>end_<f>.bmp.
            std::vector<int> v;
            for (const char* q = s.c_str() + 7; *q;) {
                v.push_back(std::atoi(q));
                while (*q && *q != ',') ++q;
                if (*q == ',') ++q;
            }
            if (v.size() < 2) continue;
            for (int f = v[0]; f <= v[1]; ++f) {
                const u32 tick = 1000u + static_cast<u32>(static_cast<std::uint64_t>(f + 1) * 1000u / 30u * 30u / 1000u);
                r.stub(0x004B6340, tick, 0);
                if (!r.call(0x0041F7F0, 0, {})) {
                    std::printf("endrun: frame %d failed\n", f);
                    break;
                }
                if (std::find(v.begin() + 2, v.end(), f) != v.end()) {
                    Frame fr;
                    if (g.grab_raw(fr)) {
                        write_bmp(out + "end_" + std::to_string(f) + ".bmp", fr);
                        std::printf("endrun: frame %d (tick %u, state %u, column %u) -> %send_%d.bmp\n", f, tick, r.r32(gs + 0x39057), r.r32(gs + 0x3906B),
                                    out.c_str(), f);
                    }
                }
            }
            continue;
        } else if (s.rfind("talk", 0) == 0) {
            // talk[:NET]: the chat line as Enter opens it in a battle (0x494050): TALK.GUI, or
            // with NET = 1 a network game (game type 3); NET = 2 also with the recipient panel
            // asked for (GS+0x2BEF bit 0, TALK2.GUI). The local player's side record is made when
            // the battle set-up left none.
            const int net = s.size() > 5 ? std::atoi(s.c_str() + 5) : 0;
            u32 schema = r.r32(gs + 0x391E9);
            if (!schema) {
                schema = r.alloc(0x2000);
                r.w32(gs + 0x391E9, schema);
                r.w32(schema, game_type >= 0 ? static_cast<u32>(game_type) : 2u);
            }
            if (net) r.w32(schema, 3);
            const u32 local = r.r32(gs + 0x2A42) & 0xFF;
            if (!r.r32(gs + 0x1B8A + local * 0x14B)) r.w32(gs + 0x1B8A + local * 0x14B, r.alloc(0x100));
            std::uint8_t b = static_cast<std::uint8_t>(r.r32(gs + 0x2BEF) & 0xFE);
            if (net == 2) b |= 1;
            r.w8(gs + 0x2BEF, b);
            ok = r.call(0x00494050, 0, {});
            const u32 top = r.r32(g.gui() + 0x18);
            std::printf("%s: %s, top layout %08X, interface flags %04X\n", s.c_str(), ok ? "opened" : "failed", top, r.r32(gs + 0x37EBE) & 0xFFFF);
            // The gadgets' colour words as loaded (+0x1F; the text's lit blit takes it as its level).
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            for (const char* n : {"CONSOLE", "TALK", "SENDTO"}) {
                u32 idx = 0xFFFFFFFFu;
                if (gadgets) r.call(0x0049FE60, 0, {gadgets, r.str(n)}, &idx);
                if (idx != 0xFFFFFFFFu)
                    std::printf("  gadget %s: kind %u, +0x1F %d, +0x23 %d, active %u\n", n, r.r32(gadgets + idx * 0x15B) & 0xFF,
                                static_cast<int>(r.r32(gadgets + idx * 0x15B + 0x1F)), static_cast<int>(r.r32(gadgets + idx * 0x15B + 0x23)),
                                r.r32(gadgets + idx * 0x15B + 0x29) & 0xFF);
            }
        } else if (s.rfind("bg:", 0) == 0) {
            // bg:FILE.bmp: an 8-bit BMP of the screen's size (a client frame) put on the screen
            // surface (what a layout opened over the battle takes in as the screen under it).
            FILE* fp = std::fopen(s.c_str() + 3, "rb");
            std::vector<std::uint8_t> b;
            if (fp) {
                char buf[65536];
                for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, fp)) > 0;) b.insert(b.end(), buf, buf + n);
                std::fclose(fp);
            }
            auto rd = [&](std::size_t at) -> u32 { return at + 4 <= b.size() ? static_cast<u32>(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24) : 0u; };
            const u32 surf = r.r32(0x0051F320 + 0xBC);
            const int w = static_cast<int>(rd(18)), h = static_cast<int>(rd(22));
            const u32 sw = surf ? r.r32(surf) : 0, sh = surf ? r.r32(surf + 4) : 0, pitch = surf ? r.r32(surf + 8) : 0;
            if (b.size() < 54 || w != static_cast<int>(sw) || h != static_cast<int>(sh)) {
                std::printf("%s: not a %ux%u 8-bit BMP\n", s.c_str(), sw, sh);
                continue;
            }
            const u32 off = rd(10), stride = (static_cast<u32>(w) + 3u) & ~3u;
            std::vector<std::uint8_t> px(static_cast<std::size_t>(pitch) * static_cast<std::size_t>(h), 0);
            for (int y = 0; y < h; ++y) {
                const std::size_t src = off + static_cast<std::size_t>(h - 1 - y) * stride;
                if (src + static_cast<std::size_t>(w) <= b.size()) std::memcpy(&px[static_cast<std::size_t>(y) * pitch], &b[src], static_cast<std::size_t>(w));
            }
            r.write(r.r32(surf + 0xC), px.data(), px.size());
            std::printf("%s: put on the screen\n", s.c_str());
            continue;
        } else if (s == "pass") {
            // One pass of the interface (keys and pointer events queued so far).
            ok = r.call(0x004A9FD0, 0, {g.gui()});
            const u32 top = r.r32(g.gui() + 0x18);
            char line[134] = {};
            r.read(0x0051E788, line, 133);
            std::printf("pass: %s, top layout %08X, interface flags %04X, stored line \"%s\"\n", ok ? "ok" : "failed", top, r.r32(gs + 0x37EBE) & 0xFFFF, line);
            continue;
        } else if (s.rfind("clock:", 0) == 0) {
            // The interface clock (0x4B6340, ticks) held at a value.
            r.stub(0x004B6340, static_cast<u32>(std::strtoul(s.c_str() + 6, nullptr, 10)), 0);
            continue;
        } else if (s == "ingame") ok = r.call(kGameMenu, 0, {});
        else if (s == "prefs") ok = r.call(kOptionsMenu, 0, {});
        else if (s == "exitmenu") ok = r.call(0x004608B0, 0, {});
        else if (s == "tabmenu") ok = r.call(0x00495010, 0, {});  // the network games' Tab bar
        else if (s == "help") ok = r.call(0x0045FB30, 0, {});
        else {
            std::printf("%s: unknown screen\n", s.c_str());
            continue;
        }
        // One frame's redraw of the top layout (what the frame loop does for a changed layout).
        if (ok) {
            const u32 top = r.r32(g.gui() + 0x18);
            if (top) r.call(0x004A81E0, 0, {g.gui(), r.r32(top + 0x10) | 0x40});
        }
        Frame f;
        if (!ok || !g.grab(f)) {
            std::printf("%s: not drawn\n", s.c_str());
            continue;
        }
        const std::string name = s + (bw != 640 || bh != 480 ? "_" + std::to_string(bw) : "");
        write_bmp(out + name + ".bmp", f);
        std::printf("%s: %dx%d -> %s%s.bmp\n", s.c_str(), f.w, f.h, out.c_str(), name.c_str());
    }
    (void)bw;
    (void)bh;
    return 0;
}
