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
// Usage: gui_harness <TotalA.exe> <out_prefix> [screen ...] [--res WxH]
//   screens: main single options skirmish ingame (the in-game menu, F2) prefs (its options)
//            and the in-battle option pages soundsrt musicrt speedsrt visualrt; exitmenu
//   Writes <out_prefix><screen>.bmp.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

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

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: gui_harness <TotalA.exe> <out_prefix> [screen ...] [--res WxH]\n");
        return 2;
    }
    std::vector<std::string> screens;
    int bw = 640, bh = 480;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--res" && i + 1 < argc) {
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
            else if (k.size() > 1 && k[0] == 'f') code = 0xE2 + static_cast<u32>(std::atoi(k.c_str() + 1)) - 1;
            r.call(0x004C1B20, 0, {code});
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
            std::printf("battle screen %dx%d\n", bw, bh);
            continue;
        } else if (s == "ingame") ok = r.call(kGameMenu, 0, {});
        else if (s == "prefs") ok = r.call(kOptionsMenu, 0, {});
        else if (s == "exitmenu") ok = r.call(0x004608B0, 0, {});
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
