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
// Usage: gui_harness <TotalA.exe> <out_prefix> [screen ...] [--res WxH] [--palettes PALETTE.PAL GUIPAL.PAL] [--type N] [--grey]
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
//   srand:S puts rand() on one shared state S (every call here runs on a new thread, and the C
//   runtime keeps rand() per thread); passes:N:PREFIX runs N interface passes (a screen's
//   per-pass routine, such as the main menu's sparks, included), writing each composited
//   screen as PREFIX<pass>.bmp (compare: ta_play --headless --no-pointer --shots 0..N-1).
//   buildpages:DIR (after "battle", with --grey and the battle palettes) writes every
//   builder's build pages for both viewing sides (compare: ta_play --dump-build-pages DIR).
//   --grey sets display flag 0x100 as the game does (grayed gadgets through the grey table).
//   Keys (docs/RE_MEMORY.md "Client", "Menu keys"): press:KEY[:MODS] delivers one keystroke as
//   the window procedure gets it (WM_KEYDOWN / WM_SYSKEYDOWN to 0x4C1D50, the WM_CHAR that
//   TranslateMessage makes to the key queue; MODS: none, shift, ctrl, alt joined by '+', held
//   through GetAsyncKeyState's import pointed at a table); frame runs one front-end frame
//   (0x499890: its own keys, the interface pass, then the state routine: the menus' states take
//   one more key off the queue, the battle room's runs the room's frame); report prints the top
//   layout's state (key mode, quick-key flag, focus, the field taking keys, every gadget's kind,
//   quick key and text). sweep:KEYS:MODS re-opens the screen opened by the commands before it
//   for each key (comma list or "all") and modifier set and prints one OUTCOME line per trial
//   (the gadgets the layout's handler was called with, focus / field / text / stage changes, a
//   screen change). Multiplayer screens: selprov[:..], tcp[:ADDR], serial, modem, selgame:FILE,
//   newmulti, msgbox:W:TEXT, lounge:FILE (also a live room's "focus NAME" / "keys 0" lines),
//   loungeframe (one more room frame). The build restrictions: unitdefs (the original's unit
//   enumeration, before lounge:), restrict (0x44C7E0 over the room), restrictframe (its frame
//   routine: one unit picture each two clock ticks). Debugging: memx:EXPR[:N], callx:ADDR:SELF:ARGS.
//   The battle loading screen (0x497F40): [maptype:N] [loadmap:NAME] | lounge:FILE, clock:T,
//   loadscreen (its set-up and first frame), loadframe:T,TE,U,A,D,E[:READY[:BITS]],
//   loadplayer:SLOT,PERCENT,READY; frames written as <out>load_<n>.bmp (compare: ta_play
//   --loading-test FILE).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <algorithm>
#include <cctype>
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

    bool init(int w, int h, bool grey = false) {
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
        // (--grey: flag 0x100 as the game sets it, so the front end's set-up builds the grey table
        // from the game palette and grayed gadgets go through it.)
        r_.w16(kDisplay + kDcFlags, static_cast<std::uint16_t>(r_.r32(kDisplay + kDcFlags) | 0x20 | 0x40 | 0x80 | 0x200 | (grey ? 0x100 : 0)));
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

// Replaces a routine (stdcall, `pop` bytes of arguments) by one that copies `bytes` (kept in the
// child) to the buffer its argument number `dst_arg` (1-based) points at, stores `count` at
// [argument 1 + count_off] when count_off >= 0, and returns `result`. The multiplayer screens'
// session-service enumerations (providers, games) answer with prepared tables this way.
bool stub_copy(harness::DbgRemote& r, u32 at, const std::vector<std::uint8_t>& bytes, int dst_arg, int count_off, u32 count, u32 result,
               std::uint16_t pop) {
    const u32 src = bytes.empty() ? 0 : r.alloc(bytes.size());
    if (!bytes.empty()) r.write(src, bytes.data(), bytes.size());
    std::vector<std::uint8_t> c = {0x56, 0x57, 0x8B, 0x7C, 0x24, static_cast<std::uint8_t>(8 + 4 * dst_arg), 0xBE};
    auto put = [&](u32 v) {
        for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put(src);
    c.push_back(0xB9);
    put(static_cast<u32>(bytes.size()));
    c.insert(c.end(), {0xF3, 0xA4});
    if (count_off >= 0) {
        c.insert(c.end(), {0x8B, 0x44, 0x24, 0x0C, 0xC7, 0x80});
        put(static_cast<u32>(count_off));
        put(count);
    }
    c.insert(c.end(), {0x5F, 0x5E, 0xB8});
    put(result);
    c.push_back(0xC2);
    c.push_back(static_cast<std::uint8_t>(pop));
    c.push_back(static_cast<std::uint8_t>(pop >> 8));
    const u32 code = r.alloc(c.size() + 16, PAGE_EXECUTE_READWRITE);
    if (!r.write(code, c.data(), c.size())) return false;
    // A jump from the routine to the code.
    std::uint8_t j[5] = {0xE9};
    const u32 rel = code - (at + 5);
    std::memcpy(j + 1, &rel, 4);
    return r.write(at, j, 5);
}

std::string lounge_map;  // the room file's map (the host chooses it after the room opens)
// The room file's focus (a live room's last pressed gadget: "focus NAME") and whether the chat
// line still takes the keys ("keys 0": no field does).
std::string lounge_focus;
bool lounge_keys = true;
u32 g_rand_state = 0;     // srand: the shared rand() state word (0: rand() not replaced)

// ---------------------------------------------------------------------------------------------
// Keystrokes as Windows delivers them, and the front end's frame that takes them.
//
// The window procedure (0x4B5CC0) hands WM_KEYDOWN to 0x4C1D50 (virtual key, 0), WM_SYSKEYDOWN
// (Alt held, or F10) to 0x4C1D50 (virtual key, 1) and WM_CHAR straight to the key queue
// (0x4C1B20); WM_SYSCHAR goes to the default procedure (nothing reaches the game). The message
// loop's TranslateMessage makes the WM_CHAR from the key down; here ToAscii does that with the
// modifiers held. The modifiers themselves are read with GetAsyncKeyState (0x4C1B80 and the
// Ctrl test in 0x4C1D50), whose import is pointed at a table the harness sets.
constexpr u32 kGuiGetAsyncKeyStateIat = 0x004FC350;
u32 g_keytable = 0;     // 256 bytes: nonzero = the virtual key is held
u32 g_press_log = 0;    // the layout handler's record: [0] count, [1..8] the gadget pressed (GUI +0x60) per call
std::vector<std::pair<u32, u32>> g_handler_thunks;  // (original handler, thunk)

bool key_table_install(harness::DbgRemote& r) {
    if (g_keytable) return true;
    g_keytable = r.alloc(256);
    std::vector<std::uint8_t> c = {0x8B, 0x44, 0x24, 0x04, 0x25, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x80};
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(g_keytable >> (8 * i)));
    c.insert(c.end(), {0xC1, 0xE0, 0x0F, 0xC2, 0x04, 0x00});
    const u32 code = r.alloc(64, PAGE_EXECUTE_READWRITE);
    r.write(code, c.data(), c.size());
    return r.w32(kGuiGetAsyncKeyStateIat, code);
}

// The modifiers held: any of "shift", "ctrl", "alt" joined by '+', or "none".
struct Mods {
    bool shift = false, ctrl = false, alt = false;
};
Mods parse_mods(const std::string& m) {
    Mods o;
    o.shift = m.find("shift") != std::string::npos;
    o.ctrl = m.find("ctrl") != std::string::npos;
    o.alt = m.find("alt") != std::string::npos;
    return o;
}
void set_mods(harness::DbgRemote& r, const Mods& m) {
    key_table_install(r);
    std::uint8_t t[256] = {};
    if (m.shift) t[VK_SHIFT] = t[VK_LSHIFT] = 1;
    if (m.ctrl) t[VK_CONTROL] = t[VK_LCONTROL] = 1;
    if (m.alt) t[VK_MENU] = t[VK_LMENU] = 1;
    r.write(g_keytable, t, sizeof t);
}

// A key's name -> its virtual key (0: unknown).
u32 key_vk(const std::string& k) {
    if (k.size() == 1) {
        const char c = k[0];
        if (c >= 'a' && c <= 'z') return static_cast<u32>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return static_cast<u32>(c);
        if (c >= '0' && c <= '9') return static_cast<u32>(c);
    }
    static const struct {
        const char* n;
        u32 vk;
    } names[] = {{"enter", VK_RETURN}, {"esc", VK_ESCAPE},  {"tab", VK_TAB},      {"space", VK_SPACE},    {"bksp", VK_BACK},     {"left", VK_LEFT},
                 {"up", VK_UP},        {"right", VK_RIGHT}, {"down", VK_DOWN},    {"home", VK_HOME},      {"end", VK_END},       {"pgup", VK_PRIOR},
                 {"pgdn", VK_NEXT},    {"ins", VK_INSERT},  {"del", VK_DELETE},   {"pause", VK_PAUSE},    {"minus", 0xBD},       {"equals", 0xBB},
                 {"comma", 0xBC},      {"period", 0xBE},    {"slash", 0xBF},      {"semicolon", 0xBA},    {"quote", 0xDE},       {"lbracket", 0xDB},
                 {"rbracket", 0xDD},   {"backslash", 0xDC}, {"grave", 0xC0}};
    for (const auto& n : names)
        if (k == n.n) return n.vk;
    if (k.size() > 1 && k[0] == 'f' && k[1] >= '1' && k[1] <= '9') return VK_F1 + static_cast<u32>(std::atoi(k.c_str() + 1)) - 1;
    return 0;
}

// One keystroke (key down; the key's release is not a message the game reads) with the modifiers
// held, delivered as the window procedure receives it. Returns the codes that reached the queue.
std::vector<u32> press_key(harness::DbgRemote& r, u32 vk, const Mods& m) {
    set_mods(r, m);
    const u32 disp = 0x0051F320;
    auto queued = [&]() {
        std::vector<u32> v;
        const u32 cap = r.r32(disp + 0xF2);
        u32 rd = r.r32(disp + 0x172);
        const u32 wr = r.r32(disp + 0x16E);
        for (int guard = 0; rd != wr && guard < 64 && cap; ++guard) {
            v.push_back(r.r32(disp + 0xF6 + rd * 4));
            rd = (rd + 1) % cap;
        }
        return v;
    };
    const std::size_t before = queued().size();
    const bool sys = m.alt || vk == VK_F10;
    r.call(0x004C1D50, 0, {vk, sys ? 1u : 0u});
    if (!sys) {
        BYTE ks[256] = {};
        if (m.shift) ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80;
        if (m.ctrl) ks[VK_CONTROL] = ks[VK_LCONTROL] = 0x80;
        WORD ch = 0;
        const int n = ToAscii(vk, MapVirtualKeyA(vk, 0), ks, &ch, 0);
        if (n == 1) r.call(0x004C1B20, 0, {static_cast<u32>(ch & 0xFF)});
    }
    std::vector<u32> all = queued();
    return std::vector<u32>(all.begin() + static_cast<std::ptrdiff_t>(std::min(before, all.size())), all.end());
}

// Wraps the top layout's handler so each call records the gadget pressed (GUI +0x60).
void watch_handler(harness::DbgRemote& r, u32 gui) {
    if (!g_press_log) g_press_log = r.alloc(64);
    const u32 top = r.r32(gui + 0x18);
    if (!top) return;
    const u32 h = r.r32(top + 8);
    if (!h) return;
    for (const auto& t : g_handler_thunks)
        if (t.second == h) return;
    u32 thunk = 0;
    for (const auto& t : g_handler_thunks)
        if (t.first == h) thunk = t.second;
    if (!thunk) {
        std::vector<std::uint8_t> c;
        auto put = [&](u32 v) {
            for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        c.push_back(0xA1);  // mov eax, [gui + 0x60]
        put(gui + 0x60);
        c.insert(c.end(), {0x8B, 0x0D});  // mov ecx, [count]
        put(g_press_log);
        c.insert(c.end(), {0x83, 0xE1, 0x07});  // and ecx, 7
        c.insert(c.end(), {0x89, 0x04, 0x8D});  // mov [log + 4 + ecx * 4], eax
        put(g_press_log + 4);
        c.insert(c.end(), {0xFF, 0x05});  // inc dword [count]
        put(g_press_log);
        thunk = r.alloc(64, PAGE_EXECUTE_READWRITE);
        c.push_back(0xE9);  // jmp handler
        put(h - (thunk + static_cast<u32>(c.size()) + 4));
        r.write(thunk, c.data(), c.size());
        g_handler_thunks.push_back({h, thunk});
    }
    r.w32(top + 8, thunk);
}

// The top layout's gadget names (index 1..n) and the state a key can change.
struct GuiState {
    u32 top = 0;
    std::string layout;
    std::vector<std::string> names;
    std::vector<int> kinds;
    std::vector<std::string> texts;  // text fields (kind 3) only
    std::vector<int> stages;         // gadget +0x138 (pressed / switch stage), +0x137 (stage count index)
    int focus = -1, active = -1, mstate = 0, sstate = 0, rstate = 0;
};
GuiState gui_state(harness::DbgRemote& r, u32 gui, u32 gs) {
    GuiState st;
    st.top = r.r32(gui + 0x18);
    st.active = static_cast<int>(r.r32(gui + 0x64));
    st.mstate = static_cast<int>(r.r32(gs + 0x2BBE) & 0xFF);
    st.sstate = static_cast<int>(r.r32(gs + 0x2BBF) & 0xFF);
    st.rstate = static_cast<int>(r.r32(gs + 0x2BC0) & 0xFF);
    if (!st.top) return st;
    const u32 gad = r.r32(st.top + 4);
    st.focus = static_cast<int>(r.r32(st.top + 0x20));
    char nm[17] = {};
    if (!gad) return st;
    r.read(gad + 2, nm, 16);
    st.layout = nm;
    const int n = static_cast<int>(static_cast<std::int16_t>(r.r32(gad + 0xB6) & 0xFFFF));
    st.names.assign(static_cast<std::size_t>(n + 1), "");
    st.kinds.assign(static_cast<std::size_t>(n + 1), 0);
    st.texts.assign(static_cast<std::size_t>(n + 1), "");
    st.stages.assign(static_cast<std::size_t>(n + 1), 0);
    for (int k = 1; k <= n && k < 400; ++k) {
        const u32 a = gad + static_cast<u32>(k) * 0x15B;
        char name[17] = {};
        r.read(a + 2, name, 16);
        st.names[k] = name;
        st.kinds[k] = static_cast<int>(r.r32(a) & 0xFF);
        if (st.kinds[k] == 3) {
            char t[0x81] = {};
            r.read(a + 0xB6, t, 0x80);
            st.texts[k] = t;
        }
        st.stages[k] = static_cast<int>(r.r32(a + 0x137) & 0xFFFFFF);
    }
    return st;
}
std::string gname(const GuiState& st, int idx) {
    if (idx < 0) return "-";
    if (idx < static_cast<int>(st.names.size())) return st.names[static_cast<std::size_t>(idx)];
    return "#" + std::to_string(idx);
}
// What changed between two states (and the gadgets the handler was called with).
std::string gui_outcome(harness::DbgRemote& r, const GuiState& a, const GuiState& b) {
    std::string o;
    const u32 calls = g_press_log ? r.r32(g_press_log) : 0;
    for (u32 k = 0; k < calls && k < 8; ++k) {
        const int idx = static_cast<int>(r.r32(g_press_log + 4 + k * 4));
        if (idx != -1) o += " press:" + gname(a, idx);
    }
    const bool same = b.top == a.top && b.layout == a.layout;
    if (!same) o += " screen:" + a.layout + "->" + (b.top ? b.layout : std::string("none"));
    if (b.rstate != a.rstate) o += " request:" + std::to_string(a.rstate) + "->" + std::to_string(b.rstate);
    if (same) {
        if (b.focus != a.focus) o += " focus:" + gname(a, a.focus) + "->" + gname(b, b.focus);
        if (b.active != a.active) o += " active:" + gname(a, a.active) + "->" + gname(b, b.active);
        for (std::size_t k = 1; k < a.texts.size() && k < b.texts.size(); ++k)
            if (a.texts[k] != b.texts[k]) o += " text:" + a.names[k] + "=\"" + b.texts[k] + "\"";
        for (std::size_t k = 1; k < a.stages.size() && k < b.stages.size(); ++k)
            if (a.stages[k] != b.stages[k] && a.kinds[k] != 3) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " state:%s=%06X", a.names[k].c_str(), static_cast<unsigned>(b.stages[k]));
                o += buf;
            }
    }
    return o.empty() ? " nothing" : o;
}
void gui_report(harness::DbgRemote& r, u32 gui, u32 gs, const char* tag) {
    const GuiState st = gui_state(r, gui, gs);
    std::printf("%s: layout %s (%08X) keymode %u gui+A2 %u quickkeys(+CC6) %u focus %s active %s lastkey %02X state %d/%d/%d\n", tag, st.layout.c_str(),
                st.top, st.top ? r.r32(st.top + 0x18) : 0, r.r32(gui + 0xA2), r.r32(gui + 0xCC6), gname(st, st.focus).c_str(), gname(st, st.active).c_str(),
                st.top ? r.r32(st.top + 0x36) & 0xFF : 0, st.mstate, st.sstate, st.rstate);
    const u32 gad = st.top ? r.r32(st.top + 4) : 0;
    if (gad) {
        // The header's Enter and Escape defaults and first focus (+0xCC, +0xDC, +0xEC).
        char cr[17] = {}, esc[17] = {}, df[17] = {};
        r.read(gad + 0xCC, cr, 16);
        r.read(gad + 0xDC, esc, 16);
        r.read(gad + 0xEC, df, 16);
        std::printf("  crdefault \"%s\" escdefault \"%s\" defaultfocus \"%s\"\n", cr, esc, df);
    }
    for (std::size_t k = 1; k < st.names.size(); ++k) {
        const u32 a = gad + static_cast<u32>(k) * 0x15B;
        const u32 qk = r.r32(a + (st.kinds[k] == 5 ? 0x147u : 0x13Au)) & 0xFF, active = r.r32(a + 0x29) & 0xFF, grey = r.r32(a + 0x13C) & 1;
        char link[17] = {};
        if (st.kinds[k] == 5) r.read(a + 0x117 + 0x1F, link, 16);
        std::printf("  %2zu kind %d %-16s active %u grey %u quickkey %02X%s%s%s\n", k, st.kinds[k], st.names[k].c_str(), active, grey, qk,
                    st.kinds[k] == 3 ? (" text \"" + st.texts[k] + "\"").c_str() : "", link[0] ? " link " : "", link);
    }
}

// The battle room's inputs, one item per line (# comments); the same file drives the client's
// battle room (ta_play --lounge FILE):
//   local SLOT                  this machine's player slot
//   map NAME                    the map (every side record names it; the host's decides)
//   player SLOT CONTROLLER KIND SIDE LOGO FLAGS OPTIONS MEMORY CD PING TEAM NAME...
//       CONTROLLER 1 this machine's human, 2 a computer player run here, 3 another machine's,
//       4 a blocked slot; KIND the side record's +0x94 (1 human, 2 computer); LOGO 255 none;
//       FLAGS +0x97 (1 host); OPTIONS the option word +0x9B; MEMORY +0x99; CD +0x9D; PING the
//       player record's latency; TEAM the ally team (5 none)
//   resources ENERGY METAL UNITLIMIT   (hundreds; every side record)
//   allied SLOT OTHER V / alliedby SLOT OTHER V   the alliance bytes (+0x108 / +0x113)
//   chat TEXT                   a line of the room's chat (in order)
//   players N                   the session's player count (default: the humans)
bool lounge_setup(harness::DbgRemote& r, Gui& g, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::vector<std::string> lines;
    char buf[1024];
    while (std::fgets(buf, sizeof buf, fp)) {
        std::string l = buf;
        if (const auto c = l.find('#'); c != std::string::npos) l.resize(c);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) lines.push_back(l);
    }
    std::fclose(fp);
    const u32 gs = g.gs();
    // Every record empty, each with a side record of its own.
    std::vector<std::uint8_t> zero(0x14B, 0);
    u32 side[10];
    for (u32 p = 0; p < 10; ++p) {
        const u32 rec = gs + 0x1B63 + p * 0x14B;
        r.write(rec, zero.data(), zero.size());
        side[p] = r.alloc(0x100);
        r.w32(rec + 0x27, side[p]);
        r.w32(rec + 4, p + 1);
        r.w8(side[p] + 0x96, 0xFF);
        r.w8(side[p] + 0xA7, 3);
        r.w8(side[p] + 0xA8, 1);
        r.w8(rec + 0x13F, 5);
    }
    std::string map;
    int humans = 0, players = -1;
    std::vector<std::string> chat;
    struct Res {
        int slot, w, h;
    };
    std::vector<Res> res;
    for (const std::string& l : lines) {
        char key[32] = {};
        if (std::sscanf(l.c_str(), "%31s", key) != 1) continue;
        const std::string k = key;
        const char* rest = l.c_str() + k.size();
        while (*rest == ' ') ++rest;
        if (k == "local") r.w8(gs + 0x2A42, static_cast<std::uint8_t>(std::atoi(rest)));
        else if (k == "map") map = rest;
        else if (k == "chat") chat.push_back(rest);
        else if (k == "players") players = std::atoi(rest);
        else if (k == "focus") lounge_focus = rest;
        else if (k == "keys") lounge_keys = std::atoi(rest) != 0;
        else if (k == "res") {
            // res SLOT W H: a player's screen size (the local player's comes from its display mode).
            int sl = 0, w = 0, h = 0;
            if (std::sscanf(rest, "%d %d %d", &sl, &w, &h) == 3 && sl >= 0 && sl < 10) {
                res.push_back({sl, w, h});
            }
        }
        else if (k == "resources") {
            unsigned e = 10, m = 10, lim = 250;
            std::sscanf(rest, "%u %u %u", &e, &m, &lim);
            for (u32 p = 0; p < 10; ++p) {
                r.w16(side[p] + 0xA1, static_cast<std::uint16_t>(e));
                r.w16(side[p] + 0xA3, static_cast<std::uint16_t>(m));
                r.w16(side[p] + 0xA5, static_cast<std::uint16_t>(lim));
            }
        } else if (k == "allied" || k == "alliedby") {
            int a = 0, b = 0, v = 0;
            if (std::sscanf(rest, "%d %d %d", &a, &b, &v) == 3 && a >= 0 && a < 10 && b >= 0 && b < 10)
                r.w8(gs + 0x1B63 + static_cast<u32>(a) * 0x14B + (k == "allied" ? 0x108u : 0x113u) + static_cast<u32>(b), static_cast<std::uint8_t>(v));
        } else if (k == "player") {
            int slot, controller, kind, sidev, logo, flags, mem, cd, ping, team, used = 0;
            unsigned options;
            if (std::sscanf(rest, "%d %d %d %d %d %i %i %d %i %d %d %n", &slot, &controller, &kind, &sidev, &logo, &flags, &options, &mem, &cd, &ping,
                            &team, &used) < 11 ||
                slot < 0 || slot > 9)
                continue;
            const std::string name = rest + used;
            const u32 rec = gs + 0x1B63 + static_cast<u32>(slot) * 0x14B, sr = side[slot];
            r.w32(rec, controller == 4 ? 0u : 1u);  // a blocked slot (state 4) holds no player
            r.w8(rec + 0x73, static_cast<std::uint8_t>(controller));
            r.w32(rec + 0x14, static_cast<u32>(ping));
            r.w8(rec + 0x13F, static_cast<std::uint8_t>(team));
            r.write(rec + 0x2B, name.c_str(), std::min<std::size_t>(name.size() + 1, 0x1E));
            r.w8(sr + 0x94, static_cast<std::uint8_t>(kind));
            r.w8(sr + 0x95, static_cast<std::uint8_t>(sidev));
            r.w8(sr + 0x96, static_cast<std::uint8_t>(logo));
            r.w8(sr + 0x97, static_cast<std::uint8_t>(flags));
            r.w16(sr + 0x99, static_cast<std::uint16_t>(mem));
            r.w16(sr + 0x9B, static_cast<std::uint16_t>(options));
            r.w16(sr + 0x9D, static_cast<std::uint16_t>(cd));
            if (kind == 1) ++humans;
        }
    }
    for (u32 p = 0; p < 10; ++p) r.write(side[p], map.c_str(), map.size() + 1);
    r.w16(gs + 0x2A3C, static_cast<std::uint16_t>(players >= 0 ? players : humans));
    const u32 local = r.r32(gs + 0x2A42) & 0xFF;
    for (const Res& x : res) {
        if (static_cast<u32>(x.slot) == local) {
            // The room copies the display mode's size into the local record (0x449BB0).
            r.w32(gs + 0x37F1B, static_cast<u32>(x.w));
            r.w32(gs + 0x37F1F, static_cast<u32>(x.h));
        } else {
            r.w16(side[x.slot] + 0x8B, static_cast<std::uint16_t>(x.w));
            r.w16(side[x.slot] + 0x8D, static_cast<std::uint16_t>(x.h));
        }
    }
    lounge_map = map;
    // A host's room takes the game's options from the game state (0x449BB0): mapping, line of
    // sight, its type, the commander rule and fixed locations; set them from the host's word.
    {
        const u32 sr = r.r32(gs + 0x1B63 + local * 0x14B + 0x27);
        if (sr && (r.r32(sr + 0x97) & 1)) {
            const u32 opt = r.r32(sr + 0x9B) & 0xFFFF;
            r.w32(gs + 0x3922D, (opt >> 8) & 1);
            r.w32(gs + 0x39231, (opt >> 9) & 1);
            r.w32(gs + 0x39235, (opt >> 10) & 1);
            r.w32(gs + 0x39229, (opt >> 11) & 3);
            if (const u32 t = r.r32(gs + 0x29A0)) r.w32(t + 0x118, (opt >> 14) & 1);
        }
    }
    // The map chosen in the game's map list (the multiplayer list).
    const u32 schema = r.r32(gs + 0x391E9);
    if (schema) r.w32(schema, 3);  // a network game
    if (schema && !map.empty()) {
        r.call(0x00435A20, schema, {r.str(map)});
        // Every record carries the map's checksum (the machines have the same map).
        u32 sum = 0;
        if (r.call(0x004373A0, schema, {}, &sum))
            for (u32 p = 0; p < 10; ++p) r.w32(side[p] + 0xA9, sum);
    }
    if (schema) {
        char sel[64] = {};
        r.read(schema + 0xA14, sel, 63);
        std::printf("lounge: map chosen \"%s\", size word %08X\n", sel, r.r32(schema + 0xA04));
    }
    // The network's sends, the session's description, the network input and the CD check.
    r.stub(0x00450F90, 0, 0);
    r.stub(0x00451180, 0, 0);
    r.stub(0x004C9890, 0, 0x1C);
    r.stub(0x00453D40, 0, 0);
    r.stub(0x00456310, 0, 0);
    r.stub(0x00453010, 0, 8);
    r.stub(0x0046C620, 0, 4);
    r.stub(0x0041D6A0, 1, 4);
    // The chat lines through the console (no sound), as the room's own lines arrive.
    if (!r.r32(gs + 0x37F27)) r.w32(gs + 0x37F27, 10);
    for (const std::string& c : chat) r.call(0x00463CA0, 0, {r.str(c), 0, 0, 10});
    std::printf("lounge set-up: map %s, %d humans, schema %08X\n", map.c_str(), humans, schema);
    return true;
}

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
    bool grey = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--grey") {
            grey = true;
        } else if (a == "--type" && i + 1 < argc) {
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
    if (!g.init(640, 480, grey)) {
        std::fprintf(stderr, "front-end set-up failed\n");
        return 1;
    }
    std::printf("front end set up (game state 0x%08X)\n", g.gs());
    const std::string out = argv[2];
    const u32 gs = g.gs();
    // The front end's frame (0x499890) ends with the state function (GS+0x391F5); here a routine
    // that does what the menus' states do with keys: the multiplayer connection and game list
    // states (and the single-player menus' state) take one key off the queue each frame after the
    // interface pass; the battle room's state runs the room's frame (0x44A680) instead.
    const u32 frame_mode = r.alloc(16);  // [0] take a key, [4] the routine to call (0: none)
    {
        std::vector<std::uint8_t> c = {0x80, 0x3D};
        auto put = [&](u32 v) {
            for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        const u32 code = r.alloc(64, PAGE_EXECUTE_READWRITE);
        put(frame_mode);
        c.push_back(0x00);
        c.insert(c.end(), {0x74, 0x05, 0xE8});  // je +5; call take
        put(0x004C1AB0 - (code + static_cast<u32>(c.size()) + 4));
        c.insert(c.end(), {0xA1});  // mov eax, [routine]
        put(frame_mode + 4);
        c.insert(c.end(), {0x85, 0xC0, 0x74, 0x02, 0xFF, 0xD0, 0xC3});  // test; je; call eax; ret
        r.write(code, c.data(), c.size());
        r.w32(gs + 0x391F5, code);
    }
    auto set_frame_mode = [&](bool take, u32 routine) {
        r.w32(frame_mode, take ? 1u : 0u);
        r.w32(frame_mode + 4, routine);
    };
    set_frame_mode(true, 0);
    bool quiet = false;                   // sweep trials: no pictures
    std::vector<std::string> setup_cmds;  // the commands that opened the screen being swept
    std::string trial_key, trial_mods;
    u32 sweep_states = 0;  // the front end's state bytes (GS+0x2BBE..0x2BC0) when the sweep began
    GuiState trial_before;
    for (std::size_t si = 0; si < screens.size(); ++si) {
        const std::string s = screens[si];
        bool ok = true;
        // (the commands before a sweep, its keys and frames included, open the swept screen)
        if (!quiet && s[0] != '@' && s.rfind("sweep:", 0) != 0 && s != "report") setup_cmds.push_back(s);
        if (s.rfind("@trial:", 0) == 0) {
            // A sweep trial's start: every layout closed, the key queue empty, the screen opened anew.
            const std::string t = s.substr(7);
            const auto c = t.find(':');
            trial_key = t.substr(0, c);
            trial_mods = c == std::string::npos ? "none" : t.substr(c + 1);
            for (int k = 0; k < 16 && r.r32(g.gui() + 0x18); ++k) r.call(0x004A9660, 0, {g.gui()});
            r.w32(0x0051F320 + 0x172, r.r32(0x0051F320 + 0x16E));
            r.w8(gs + 0x2BBE, static_cast<std::uint8_t>(sweep_states));
            r.w8(gs + 0x2BBF, static_cast<std::uint8_t>(sweep_states >> 8));
            r.w8(gs + 0x2BC0, static_cast<std::uint8_t>(sweep_states >> 16));
            set_mods(r, Mods{});
            quiet = true;
            continue;
        } else if (s == "@press") {
            // A sweep trial's key: one keystroke, one frame, the outcome on one line.
            quiet = false;
            watch_handler(r, g.gui());
            trial_before = gui_state(r, g.gui(), gs);
            if (g_press_log) r.w32(g_press_log, 0);
            const std::vector<u32> codes = press_key(r, key_vk(trial_key), parse_mods(trial_mods));
            set_mods(r, parse_mods(trial_mods));
            r.call(0x00499890, 0, {});
            set_mods(r, Mods{});
            const GuiState after = gui_state(r, g.gui(), gs);
            std::string q;
            for (u32 v : codes) {
                char b[8];
                std::snprintf(b, sizeof b, "%s%02X", q.empty() ? "" : ",", v);
                q += b;
            }
            std::printf("OUTCOME %s %s %s [%s]%s\n", trial_before.layout.c_str(), trial_key.c_str(), trial_mods.c_str(), q.c_str(),
                        gui_outcome(r, trial_before, after).c_str());
            continue;
        } else if (s.rfind("sweep:", 0) == 0) {
            // sweep:KEYS:MODS — for each key (comma list, or "all": a..z, 0..9, enter, esc, tab,
            // space, bksp and the arrows) and each modifier set (comma list of none, shift, alt,
            // ctrl, joined with '+'), the screen opened anew by the commands given before the
            // sweep, the keystroke delivered as Windows does and one front-end frame run; prints
            // "OUTCOME <layout> <key> <mods> [queued codes] <what happened>".
            const std::string t = s.substr(6);
            const auto c = t.find(':');
            std::string keys = t.substr(0, c), mods = c == std::string::npos ? "none" : t.substr(c + 1);
            if (keys == "all") {
                keys.clear();
                for (char ch = 'a'; ch <= 'z'; ++ch) keys += std::string(1, ch) + ",";
                for (char ch = '0'; ch <= '9'; ++ch) keys += std::string(1, ch) + ",";
                keys += "enter,esc,tab,space,bksp,left,up,right,down";
            }
            auto split = [](const std::string& v) {
                std::vector<std::string> o;
                for (std::size_t a = 0; a <= v.size();) {
                    std::size_t e = v.find(',', a);
                    if (e == std::string::npos) e = v.size();
                    if (e > a) o.push_back(v.substr(a, e - a));
                    a = e + 1;
                }
                return o;
            };
            std::vector<std::string> add;
            for (const std::string& m : split(mods))
                for (const std::string& k : split(keys)) {
                    add.push_back("@trial:" + k + ":" + m);
                    add.insert(add.end(), setup_cmds.begin(), setup_cmds.end());
                    add.push_back("@press");
                }
            sweep_states = r.r32(gs + 0x2BBE) & 0xFFFFFF;
            screens.insert(screens.begin() + static_cast<std::ptrdiff_t>(si + 1), add.begin(), add.end());
            continue;
        } else if (s.rfind("mods:", 0) == 0) {
            set_mods(r, parse_mods(s.substr(5)));
            continue;
        } else if (s.rfind("press:", 0) == 0) {
            // press:KEY[:MODS] — one keystroke as Windows delivers it (see press_key); prints the
            // codes that reached the key queue. The modifiers stay held until "mods:none".
            const std::string t = s.substr(6);
            const auto c = t.find(':');
            const std::string k = t.substr(0, c);
            const Mods m = parse_mods(c == std::string::npos ? "none" : t.substr(c + 1));
            std::string q;
            for (u32 v : press_key(r, key_vk(k), m)) {
                char b[8];
                std::snprintf(b, sizeof b, " %02X", v);
                q += b;
            }
            std::printf("%s: queued%s\n", s.c_str(), q.empty() ? " nothing" : q.c_str());
            continue;
        } else if (s == "frame") {
            // One front-end frame (0x499890: its own keys, the interface pass, the state routine).
            watch_handler(r, g.gui());
            const GuiState a = gui_state(r, g.gui(), gs);
            if (g_press_log) r.w32(g_press_log, 0);
            ok = r.call(0x00499890, 0, {});
            const GuiState b = gui_state(r, g.gui(), gs);
            std::printf("frame: %s,%s\n", ok ? "ok" : "failed", gui_outcome(r, a, b).c_str());
            continue;
        } else if (s == "report") {
            gui_report(r, g.gui(), gs, "report");
            continue;
        }
        // The front end's state (GS+0x2BBE, its sub-state +0x2BBF and the requested one +0x2BC0) as
        // the menu flow has it while the screen is up: 0x0F connection, 0x14 a provider's
        // settings, 0x10 game list, 0x11 battle room; sub-state 1 (the screen's frames).
        auto set_states = [&](std::uint8_t m) {
            r.w8(gs + 0x2BBE, m);
            r.w8(gs + 0x2BBF, 1);
            r.w8(gs + 0x2BC0, 1);
        };
        if (s.rfind("selprov", 0) == 0) set_frame_mode(true, 0), set_states(0x0F);
        else if (s.rfind("tcp", 0) == 0 || s == "serial" || s == "modem") set_frame_mode(false, 0), set_states(0x14);
        else if (s.rfind("selgame:", 0) == 0 || s == "newmulti") set_frame_mode(true, 0), set_states(0x10);
        else if (s.rfind("lounge:", 0) == 0) set_frame_mode(false, 0x0044A680), set_states(0x11);
        // Menus (640 x 480).
        if (s == "main") ok = r.call(kMainMenu, 0, {});
        else if (s == "single") ok = r.call(kSinglePlayer, 0, {});
        else if (s == "options") ok = r.call(kOptionsMenu, 0, {});
        else if (s == "skirmish") ok = r.call(kSkirmish, 0, {});
        else if (s == "selmap") ok = r.call(0x0047AAF0, 0, {});
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
        } else if (s.rfind("srand:", 0) == 0) {
            // The menus' rand() stream: the C runtime keeps its state per thread and every call
            // here runs on a new thread, so rand() (0x4E4870) is replaced by the same generator on
            // one state word (state = state * 214013 + 2531011, result bits 16..30), set here.
            u32& rand_state = g_rand_state;
            if (!rand_state) {
                rand_state = r.alloc(16);
                std::vector<std::uint8_t> c = {0xA1};
                auto put = [&](u32 v) {
                    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
                };
                put(rand_state);
                c.insert(c.end(), {0x69, 0xC0});
                put(214013u);
                c.push_back(0x05);
                put(2531011u);
                c.push_back(0xA3);
                put(rand_state);
                c.insert(c.end(), {0xC1, 0xE8, 0x10, 0x25});
                put(0x7FFF);
                c.push_back(0xC3);
                const u32 code = r.alloc(64, PAGE_EXECUTE_READWRITE);
                r.write(code, c.data(), c.size());
                std::uint8_t j[5] = {0xE9};
                const u32 rel = code - (0x004E4870 + 5);
                std::memcpy(j + 1, &rel, 4);
                r.write(0x004E4870, j, 5);
            }
            r.w32(rand_state, static_cast<u32>(std::strtoul(s.c_str() + 6, nullptr, 0)));
            continue;
        } else if (s.rfind("passes:", 0) == 0) {
            // passes:N:PREFIX — N passes of the interface (0x4A9FD0: input, pointer, the top
            // screen's per-pass routine such as the main menu's sparks), each composited and
            // written as PREFIX<pass>.bmp (pass 1 .. N).
            const std::string t = s.substr(7);
            const auto c = t.find(':');
            const int n = std::atoi(t.c_str());
            const std::string prefix = c == std::string::npos ? out + "pass" : t.substr(c + 1);
            int done = 0;
            const u32 per_pass = std::getenv("GH_SCREEN_ROUTINE_ONLY") ? r.r32(r.r32(g.gui() + 0x18) + 0x1C) : 0;
            const bool trace = std::getenv("GH_TRACE_PASSES") != nullptr;
            for (int k = 1; k <= n; ++k) {
                const u32 r0 = g_rand_state ? r.r32(g_rand_state) : 0, flags0 = r.r32(g.gui() + 0xCCA);
                if (!r.call(per_pass ? per_pass : 0x004A9FD0, 0, per_pass ? std::vector<u32>{} : std::vector<u32>{g.gui()})) break;
                if (trace)
                    std::printf("pass %d: rand %08X -> %08X, layout flag %u -> %u\n", k, r0, g_rand_state ? r.r32(g_rand_state) : 0, flags0,
                                r.r32(g.gui() + 0xCCA));
                Frame f;
                if (!g.grab(f)) break;
                char num[16];
                std::snprintf(num, sizeof num, "%04d", k);
                write_bmp(prefix + num + ".bmp", f);
                ++done;
            }
            std::printf("passes: %d of %d written to %s*.bmp\n", done, n, prefix.c_str());
            continue;
        } else if (s.rfind("buildpages:", 0) == 0) {
            // buildpages:DIR (after "battle"): every builder's build pages as the original makes
            // them, for each viewing side. The unit definitions come from the original's own
            // enumeration (0x42A8D0), each definition's page count byte from the battle loading's
            // rule (consecutive guis\<unitname>1.. descriptions + 1; 1 with only a <unitname>0;
            // asked with the original's path builder and file-size routine), the download table
            // and the raised counts from the original's loader (0x42DCF0). Then for each side
            // (the local player's side record +0x95) and each builder's page 1 .. count - 1: a
            // unit (definition, page in its flags with the build menu bit, complete) and the
            // original's page opening (0x41ACE0, which also sets the tabs and switches from the
            // selection's abilities: all present here), the layout's buttons listed to
            // DIR\orig_pages.txt and its box written as DIR\o_<UNIT>_s<side>_p<page>.bmp.
            const std::string dir = s.substr(11);
            r.w32(gs + 0x1439B, 0);
            r.w32(gs + 0x1438F, 0);
            if (!r.call(0x0042A8D0, 0, {}, nullptr, 120000)) {
                std::printf("buildpages: unit enumeration failed\n");
                continue;
            }
            const u32 ndefs = r.r32(gs + 0x1438F), defs = r.r32(gs + 0x1439B);
            std::printf("buildpages: %u definitions\n", ndefs);
            auto exists = [&](const std::string& name) {
                const u32 path = r.alloc(0x100);
                r.call(0x004290F0, 0, {path, r.str("guis"), r.str(name), r.str("GUI")});
                u32 size = 0;
                r.call(0x004BBC40, 0, {path}, &size);
                return size != 0;
            };
            // The battle loading puts the definitions in name order (the type lookup by name is a
            // binary search) and numbers them by place.
            {
                std::vector<std::vector<std::uint8_t>> recs(ndefs, std::vector<std::uint8_t>(0x249));
                for (u32 k = 1; k < ndefs; ++k) r.read(defs + k * 0x249, recs[k].data(), 0x249);
                std::stable_sort(recs.begin() + 1, recs.end(), [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
                    return _stricmp(reinterpret_cast<const char*>(&a[0x20]), reinterpret_cast<const char*>(&b[0x20])) < 0;
                });
                for (u32 k = 1; k < ndefs; ++k) r.write(defs + k * 0x249, recs[k].data(), 0x249);
            }
            std::vector<std::string> names(ndefs);
            for (u32 k = 1; k < ndefs; ++k) {
                char nm[33] = {};
                r.read(defs + k * 0x249 + 0x20, nm, 32);
                names[k] = nm;
                const bool gui0 = exists(names[k] + "0");
                u32 flags = r.r32(defs + k * 0x249 + 0x241);
                flags = gui0 ? (flags | 0x80000000u) : (flags & 0x7FFFFFFFu);
                r.w32(defs + k * 0x249 + 0x241, flags);
                int n = 1;
                bool any = false;
                while (exists(names[k] + std::to_string(n))) {
                    ++n;
                    any = true;
                }
                r.w8(defs + k * 0x249 + 0x22E, static_cast<std::uint8_t>(any ? n : (gui0 ? 1 : 0)));
                r.w16(defs + k * 0x249 + 0x21E, static_cast<std::uint16_t>(k));  // the type index (the battle loading's)
                r.w32(defs + k * 0x249 + 0x152, 0);  // no build option list (the download step appends none)
                r.w32(defs + k * 0x249 + 0x156, 0);
            }
            for (u32 k = 2; k < ndefs; ++k)
                if (_stricmp(names[k - 1].c_str(), names[k].c_str()) >= 0)
                    std::printf("buildpages: definitions not in name order at %u (%s, %s)\n", k, names[k - 1].c_str(), names[k].c_str());
            if (!r.call(0x0042DCF0, 0, {}, nullptr, 60000)) {
                std::printf("buildpages: download menus failed\n");
                continue;
            }
            {
                const u32 nfiles = r.r32(gs + 0x391C7), table = r.r32(gs + 0x391CB);
                int entries = 0;
                for (u32 f = 0; f < nfiles; ++f) entries += static_cast<int>(r.r32(table + f * 0xBD));
                std::printf("buildpages: %u download files, %d entries\n", nfiles, entries);
            }
            // The selection's abilities: every order available (nothing grayed by them) except
            // loading (its button and BLAST share a place: with loading, BLAST is hidden).
            r.w8(gs + 0x37EC0, 0x80);
            r.w8(gs + 0x37EC1, 0xEF);
            r.w8(gs + 0x37EC2, 0x01);
            {
                std::uint16_t fl = 0;
                r.read(gs + 0x37EBE, &fl, 2);
                fl = static_cast<std::uint16_t>(fl & ~0x7000);
                r.write(gs + 0x37EBE, &fl, 2);
            }
            const u32 local = r.r32(gs + 0x2A42) & 0xFF;
            const u32 prec = gs + 0x1B63 + local * 0x14B;
            u32 side_rec = r.r32(prec + 0x27);
            if (!side_rec) {
                side_rec = r.alloc(0x100);
                r.w32(prec + 0x27, side_rec);
            }
            // No focus outlines in a battle (its loading switches them off, 0x49FA70).
            r.call(0x0049FA70, 0, {g.gui()});
            const u32 unit = r.alloc(0x200);
            FILE* lst = std::fopen((dir + "/orig_pages.txt").c_str(), "wb");
            int pages_done = 0;
            for (u32 side = 0; side < 2; ++side) {
                r.w8(side_rec + 0x95, static_cast<std::uint8_t>(side));
                for (u32 k = 1; k < ndefs; ++k) {
                    const int count = static_cast<int>(r.r32(defs + k * 0x249 + 0x22E) & 0xFF);
                    for (int p = 1; p < count && p <= 7; ++p) {
                        for (int q = 0; q < 16 && r.r32(g.gui() + 0x18); ++q) r.call(0x004A9660, 0, {g.gui()});
                        std::vector<std::uint8_t> zero(0x200, 0);
                        r.write(unit, zero.data(), zero.size());
                        r.w32(unit + 0x92, defs + k * 0x249);
                        r.w16(unit + 0xA6, static_cast<std::uint16_t>(k));
                        r.w16(unit + 0xA8, 1);
                        r.w32(unit + 0x110, 0x00400000u | (static_cast<u32>(p) << 23));
                        r.w16(gs + 0x37E9C, 0);
                        const std::string gui_name = names[k] + std::to_string(p) + ".GUI";
                        if (!r.call(0x0041ACE0, 0, {unit, r.str(gui_name), static_cast<u32>(p)})) {
                            std::printf("buildpages: %s page %d failed\n", names[k].c_str(), p);
                            continue;
                        }
                        const u32 top = r.r32(g.gui() + 0x18);
                        const u32 gadgets = top ? r.r32(top + 4) : 0;
                        if (!gadgets) {
                            std::printf("buildpages: %s page %d: no layout\n", names[k].c_str(), p);
                            continue;
                        }
                        std::vector<std::uint8_t> rec(0x15B);
                        r.read(gadgets, rec.data(), rec.size());
                        auto s16 = [&](int o) { return static_cast<int>(static_cast<std::int16_t>(rec[o] | rec[o + 1] << 8)); };
                        const int hx = s16(0x13), hy = s16(0x15), hw = s16(0x17), hh = s16(0x19);
                        const int n = s16(0xB6);
                        if (lst) std::fprintf(lst, "PAGE %s side %u page %d\n", names[k].c_str(), side, p);
                        for (int i = 1; i <= n; ++i) {
                            r.read(gadgets + static_cast<u32>(i) * 0x15B, rec.data(), rec.size());
                            if (rec[0] != 1) continue;
                            char nm[33] = {};
                            std::memcpy(nm, &rec[2], 32);
                            if (lst)
                                std::fprintf(lst, "  %s %d %d %d %d %d\n", nm, hx + s16(0x13), hy + s16(0x15), s16(0x17), s16(0x19), rec[0x13C] & 1);
                        }
                        Frame f;
                        if (g.grab(f) && hx >= 0 && hy >= 0 && hx + hw <= f.w && hy + hh <= f.h) {
                            Frame box;
                            box.w = hw;
                            box.h = hh;
                            std::memcpy(box.pal, f.pal, sizeof box.pal);
                            box.px.resize(static_cast<std::size_t>(hw) * static_cast<std::size_t>(hh));
                            for (int y = 0; y < hh; ++y)
                                std::memcpy(&box.px[static_cast<std::size_t>(y) * hw], &f.px[static_cast<std::size_t>(hy + y) * f.w + hx], static_cast<std::size_t>(hw));
                            write_bmp(dir + "/o_" + names[k] + "_s" + std::to_string(side) + "_p" + std::to_string(p) + ".bmp", box);
                        }
                        ++pages_done;
                    }
                }
            }
            if (lst) std::fclose(lst);
            std::printf("buildpages: %d pages\n", pages_done);
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
            if (idx != 0xFFFFFFFFu) {
                const u32 g = gadgets + idx * 0x15B;
                std::printf("%s: kind %u, +0x1F %d, +0x23 %d\n", s.c_str(), r.r32(g) & 0xFF, static_cast<int>(r.r32(g + 0x1F)),
                            static_cast<int>(r.r32(g + 0x23)));
                std::uint8_t rec[0x15B] = {};
                r.read(g, rec, sizeof rec);
                auto s16 = [&](int o) { return static_cast<int>(static_cast<std::int16_t>(rec[o] | rec[o + 1] << 8)); };
                std::printf("  x %d y %d w %d h %d attribs %08X | +BA %d +BC %d +BE %d +C0 %d +DA %d | +136 %d +138 %d +140 %d\n", s16(0x13), s16(0x15),
                            s16(0x17), s16(0x19), static_cast<unsigned>(rec[0x1B] | rec[0x1C] << 8 | rec[0x1D] << 16 | rec[0x1E] << 24), s16(0xBA), s16(0xBC),
                            s16(0xBE), s16(0xC0), s16(0xDA), s16(0x136), s16(0x138), s16(0x140));
                for (int o = 0; o < 0x15B; o += 32) {
                    std::printf("  %03X:", o);
                    for (int k = o; k < o + 32 && k < 0x15B; ++k) std::printf(" %02X", rec[k]);
                    std::printf("\n");
                }
            }
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
        } else if (s.rfind("savedir:", 0) == 0) {
            // savedir:PATH: the saved games' directory (the pointer at 0x5091C8 to "SAVEGAME", a
            // path relative to the game's directory) pointed at PATH instead.
            r.w32(0x005091C8, r.str(s.substr(8)));
            continue;
        } else if (s == "loadmenu" || s == "savemenu") {
            // The load menu (0x4931D0) or the save menu (0x493060), as the menus and the game menu
            // open them; then one redraw of the top layout.
            ok = r.call(s == "loadmenu" ? 0x004931D0 : 0x00493060, 0, {});
            const u32 top = r.r32(g.gui() + 0x18);
            if (ok && top) r.call(0x004A81E0, 0, {g.gui(), r.r32(top + 0x10) | 0x40});
            Frame f;
            if (ok && g.grab(f)) write_bmp(out + s + ".bmp", f);
            std::printf("%s: %s, top layout %08X\n", s.c_str(), ok ? "opened" : "failed", top);
            continue;
        } else if (s.rfind("msgbox:", 0) == 0) {
            // msgbox:TEXT: the original's message box (0x4ABD90: 320 pixels, OK, sized to the text).
            ok = r.call(0x004ABD90, 0, {g.gui(), r.str(s.substr(7)), 0x140, 1, 1});
            Frame f;
            if (ok && g.grab(f)) write_bmp(out + "msgbox.bmp", f);
            std::printf("%s: %s\n", s.c_str(), ok ? "shown" : "failed");
            continue;
        } else if (s.rfind("items:", 0) == 0) {
            // items:NAME: the top layout's list gadget's rows (+0xC0 count, +0xC2 the rows, each
            // zero-terminated), its selection and first row.
            const u32 top = r.r32(g.gui() + 0x18);
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            u32 idx = 0xFFFFFFFFu;
            if (gadgets) r.call(0x0049FE60, 0, {gadgets, r.str(s.substr(6))}, &idx);
            if (idx != 0xFFFFFFFFu) {
                const u32 g0 = gadgets + idx * 0x15B;
                std::uint8_t rec[0x15B] = {};
                r.read(g0, rec, sizeof rec);
                const int count = static_cast<std::int16_t>(rec[0xC0] | rec[0xC1] << 8);
                u32 at = static_cast<u32>(rec[0xC2] | rec[0xC3] << 8 | rec[0xC4] << 16 | rec[0xC5] << 24);
                std::printf("%s: %d rows, selected %d, top %d\n", s.c_str(), count, static_cast<std::int16_t>(rec[0xBA] | rec[0xBB] << 8),
                            static_cast<std::int16_t>(rec[0xBC] | rec[0xBD] << 8));
                for (int k = 0; k < count && at; ++k) {
                    char row[256] = {};
                    r.read(at, row, 255);
                    std::printf("  row %d: %s\n", k, row);
                    at += static_cast<u32>(std::strlen(row)) + 1;
                }
            }
            continue;
        } else if (s.rfind("maptype:", 0) == 0) {
            // maptype:N: the map info's provider word (GS+0x391E9 -> +0): 2 skirmish, 3 network,
            // 1 campaign (the skirmish set-up leaves 2; the map choice loads map info only for 2, 3).
            u32 schema = r.r32(gs + 0x391E9);
            if (!schema) {
                schema = r.alloc(0x2000);
                r.w32(gs + 0x391E9, schema);
            }
            r.w32(schema, static_cast<u32>(std::atoi(s.c_str() + 8)));
            continue;
        } else if (s.rfind("skirmap:", 0) == 0) {
            // skirmap:NAME: the settings record's map name (GS+0x29A0 -> +0x11C, the SkirmishMap the
            // map choice selects and writes).
            const u32 rec = r.r32(gs + 0x29A0);
            const std::string t = s.substr(8);
            if (rec) r.write(rec + 0x11C, t.c_str(), t.size() + 1);
            std::printf("%s: settings record %08X\n", s.c_str(), rec);
            continue;
        } else if (s.rfind("tables:", 0) == 0) {
            // tables:FILE: the display's light table (+0xC8) then its shade table (+0xC4), 0x2000
            // bytes each, as the screen shown last left them.
            std::vector<std::uint8_t> t(0x4000, 0);
            if (const u32 lt = r.r32(0x0051F320 + 0xC8)) r.read(lt, t.data(), 0x2000);
            if (const u32 st = r.r32(0x0051F320 + 0xC4)) r.read(st, t.data() + 0x2000, 0x2000);
            if (FILE* fp = std::fopen(s.c_str() + 7, "wb")) {
                std::fwrite(t.data(), 1, t.size(), fp);
                std::fclose(fp);
            }
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
        } else if (s.rfind("selprov", 0) == 0) {
            // selprov[:NAME|NAME...]: the connection screen (SELPROV.GUI, 0x444580) with the session
            // service's provider list answered by the names given (none: an empty list); the
            // registry's extra services (0x45B490) none; the session object released (nothing).
            std::vector<std::uint8_t> names;
            u32 n = 0;
            if (s.size() > 8)
                for (std::size_t a = 8; a <= s.size();) {
                    std::size_t e = s.find('|', a);
                    if (e == std::string::npos) e = s.size();
                    // The descriptions follow each other, each ended by a zero.
                    names.insert(names.end(), s.begin() + static_cast<std::ptrdiff_t>(a), s.begin() + static_cast<std::ptrdiff_t>(e));
                    names.push_back(0);
                    ++n;
                    a = e + 1;
                }
            r.stub(0x004C9B70, 0, 4);
            r.stub(0x0045B490, 0, 4);
            stub_copy(r, 0x004CA250, names, 4, 0x4E5, n, 1, 0x14);
            ok = r.call(0x00444580, 0, {});
        } else if (s.rfind("tcp", 0) == 0 && (s.size() == 3 || s[3] == ':')) {
            // tcp[:ADDRESS]: the TCP/IP address question (TCP.GUI, 0x4421F0) over the connection
            // screen, the registry's TCPADDR answered with ADDRESS (none: not there).
            const std::string addr = s.size() > 4 ? s.substr(4) : std::string();
            std::vector<std::uint8_t> v(addr.begin(), addr.end());
            v.push_back(0);
            r.stub(0x004CA490, 0, 4);
            if (addr.empty()) r.stub(0x0042F980, 0, 0xC);
            else stub_copy(r, 0x0042F980, v, 2, -1, 0, 1, 0xC);
            ok = r.call(0x004421F0, 0, {});
        } else if (s == "serial") {
            // The serial connection's settings (SERIAL.GUI, 0x442560): the session service's
            // set-up does nothing, the registry's SERPORT / SERBAUD are not there.
            r.stub(0x004CA490, 0, 4);
            r.stub(0x0042F980, 0, 0xC);
            ok = r.call(0x00442560, 0, {});
        } else if (s == "modem") {
            // The modem connection's settings (MODEM.GUI, 0x443100): the session service's
            // address query fails (no modem list: the screen as it opens without one).
            r.stub(0x004CA490, 0, 4);
            r.stub(0x004CA900, 0x80004005u, 8);
            r.stub(0x004CA940, 0, 4);
            r.stub(0x0042F980, 0, 0xC);
            ok = r.call(0x00443100, 0, {});
        } else if (s.rfind("selgame:", 0) == 0) {
            // selgame:FILE: the game list (SELGAME.GUI, 0x443CB0) with the session service's games
            // answered from FILE (lines "game NAME;MAP;MAX;PLAYERS;OPTIONS;MEMORY;CD;PING;ENERGY;METAL;
            // UNITLIMIT;MAJOR;MINOR", energy and metal in hundreds; "nick NAME"; "password TEXT").
            std::vector<std::uint8_t> table;
            u32 n = 0;
            std::string nick, password;
            if (FILE* fp = std::fopen(s.c_str() + 8, "rb")) {
                char line[512];
                while (std::fgets(line, sizeof line, fp)) {
                    std::string l = line;
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    if (l.rfind("nick ", 0) == 0) nick = l.substr(5);
                    else if (l.rfind("password ", 0) == 0) password = l.substr(9);
                    if (l.rfind("game ", 0) != 0) continue;
                    std::vector<std::string> f;
                    for (std::size_t a = 5;;) {
                        const std::size_t e = l.find(';', a);
                        f.push_back(l.substr(a, e == std::string::npos ? std::string::npos : e - a));
                        if (e == std::string::npos) break;
                        a = e + 1;
                    }
                    if (f.size() < 13) continue;
                    auto num = [&](std::size_t k) { return static_cast<u32>(std::strtoul(f[k].c_str(), nullptr, 0)); };
                    std::uint8_t e[0x54] = {};
                    const u32 u1 = (num(5) & 0xFFFF) | (num(4) & 0xFFFF) << 16, u2 = (num(6) & 0xFFFF) | (num(7) & 0xFFFF) << 16,
                              u3 = (num(8) & 0xFFFF) | (num(9) & 0xFFFF) << 16, u4 = (num(10) & 0xFFFF) | (num(11) & 0xFF) << 16 | (num(12) & 0xFF) << 24;
                    const u32 mx = num(2);
                    std::memcpy(e + 4, &u1, 4);
                    std::memcpy(e + 8, &u2, 4);
                    std::memcpy(e + 0xC, &u3, 4);
                    std::memcpy(e + 0x10, &u4, 4);
                    std::memcpy(e + 0x14, &mx, 4);
                    // The session's name: the game's name in 16 places, the map's in 15, spaces filling.
                    char nm[32];
                    std::memset(nm, ' ', 31);
                    nm[31] = 0;
                    std::memcpy(nm, f[0].c_str(), std::min<std::size_t>(f[0].size(), 16));
                    std::memcpy(nm + 16, f[1].c_str(), std::min<std::size_t>(f[1].size(), 15));
                    std::memcpy(e + 0x18, nm, 32);
                    // The players' count lives in the option word's low four bits.
                    e[6] = static_cast<std::uint8_t>((e[6] & 0xF0) | (num(3) & 0xF));
                    table.insert(table.end(), e, e + 0x54);
                    ++n;
                }
                std::fclose(fp);
            }
            r.write(gs + 0x2BD2, nick.c_str(), std::min<std::size_t>(nick.size() + 1, 0x11));
            r.write(gs + 0x2BE3, password.c_str(), std::min<std::size_t>(password.size() + 1, 0xB));
            stub_copy(r, 0x004C9E50, table, 2, 0x4E9, n, n, 0xC);
            ok = r.call(0x00443CB0, 0, {});
            std::printf("selgame: %u games\n", n);
        } else if (s.rfind("lounge:", 0) == 0) {
            // lounge:FILE: the battle room (LOUNGE2.GUI, 0x449BB0, then its frame 0x44A680) with the
            // players' records and the chat from FILE (see lounge_setup); the network's sends and
            // the session description do nothing.
            ok = lounge_setup(r, g, s.substr(7));
            // A joining machine's room opens before the host's record reaches it (the session's
            // join, then the room; the records come with the room's first frames): its set-up
            // finds no host record to compare the map checksum with. The host record's version
            // is held at 0 meanwhile (no checksum compare, as without a host record).
            u32 host_rec = 0, host_ver = 0;
            {
                const u32 local = r.r32(gs + 0x2A42) & 0xFF;
                const u32 lrec = r.r32(gs + 0x1B63 + local * 0x14B + 0x27);
                if (ok && lrec && !(r.r32(lrec + 0x97) & 1))
                    for (u32 p = 0; p < 10; ++p) {
                        const u32 sr = r.r32(gs + 0x1B63 + p * 0x14B + 0x27);
                        if (sr && (r.r32(sr + 0x97) & 1) && (r.r32(gs + 0x1B63 + p * 0x14B) != 0)) {
                            host_rec = sr;
                            host_ver = r.r32(sr + 0xA7) & 0xFF;
                            r.w8(sr + 0xA7, 0);
                            break;
                        }
                    }
            }
            if (ok) ok = r.call(0x00449BB0, 0, {});
            if (host_rec) r.w8(host_rec + 0xA7, static_cast<std::uint8_t>(host_ver));
            {
                // The room opens on the list's first map; a host then chooses the file's map (its
                // record takes the name, as the map choice's handler does).
                const u32 local = r.r32(gs + 0x2A42) & 0xFF;
                const u32 rec = r.r32(gs + 0x1B63 + local * 0x14B + 0x27);
                const u32 schema = r.r32(gs + 0x391E9);
                if (ok && schema && rec && (r.r32(rec + 0x97) & 1) && !lounge_map.empty()) {
                    r.call(0x00435A20, schema, {r.str(lounge_map)});
                    char sel[0x8F] = {};
                    r.read(schema + 0xA14, sel, sizeof sel - 1);
                    r.write(rec, sel, std::strlen(sel) + 1);
                    r.w32(rec + 0xA9, 0);
                    u32 sum = 0;
                    if (r.call(0x004373A0, schema, {}, &sum)) r.w32(rec + 0xA9, sum);
                }
            }
            if (ok && (!lounge_focus.empty() || !lounge_keys)) {
                // A live room's focus: the gadget last pressed holds it (layout +0x20) and, after a
                // press of anything but the chat line, no field takes the keys (GUI +0x64).
                const u32 top = r.r32(g.gui() + 0x18);
                const u32 gadgets = top ? r.r32(top + 4) : 0;
                u32 idx = 0xFFFFFFFFu;
                if (gadgets && !lounge_focus.empty()) r.call(0x0049FDF0, 0, {gadgets, r.str(lounge_focus)}, &idx);
                if (top && idx != 0xFFFFFFFFu) r.w32(top + 0x20, idx);
                if (!lounge_keys) r.w32(g.gui() + 0x64, 0xFFFFFFFFu);
                std::printf("lounge: focus %s (%d), keys %d\n", lounge_focus.c_str(), static_cast<int>(idx), lounge_keys ? 1 : 0);
            }
            if (ok) ok = r.call(0x0044A680, 0, {});
            std::printf("lounge: %s, start %s\n", ok ? "set up" : "failed", "");
        } else if (s.rfind("nick:", 0) == 0 || s.rfind("gamename:", 0) == 0 || s.rfind("password:", 0) == 0) {
            // The player's name (GS+0x2BD2), the new game's name (GS+0x2BC1), the password (GS+0x2BE3).
            const auto c = s.find(':');
            const std::string v = s.substr(c + 1);
            const u32 at = s[0] == 'n' ? 0x2BD2u : s[0] == 'g' ? 0x2BC1u : 0x2BE3u;
            r.write(gs + at, v.c_str(), std::min<std::size_t>(v.size() + 1, 0x11));
            continue;
        } else if (s == "newmulti") {
            // The new game question (NEWMULTI.GUI, 0x441080).
            ok = r.call(0x00441080, 0, {});
        } else if (s.rfind("msgbox:", 0) == 0) {
            // msgbox:WIDTH:TEXT: the message box (0x4ABD90) as the multiplayer screens open it
            // (width WIDTH, OK shown, the box fitted to the text).
            const std::string t = s.substr(7);
            const auto c = t.find(':');
            ok = r.call(0x004ABD90, 0, {g.gui(), r.str(t.substr(c + 1)), static_cast<u32>(std::atoi(t.c_str())), 1, 1});
        } else if (s.rfind("stub:", 0) == 0) {
            // stub:ADDR:VALUE:POP replaces a routine by one returning VALUE (e.g. the unit definition
            // exchange's "all compared" 0x46E000 answering 1).
            unsigned a = 0, v = 0, pop = 0;
            std::sscanf(s.c_str() + 5, "%x:%u:%u", &a, &v, &pop);
            r.stub(a, v, static_cast<std::uint16_t>(pop));
            continue;
        } else if (s.rfind("gword:", 0) == 0) {
            // Debugging: gword:NAME:OFF prints the 32-bit word at the top layout's gadget NAME + OFF.
            const std::string t = s.substr(6);
            const auto c = t.find(':');
            const u32 top = r.r32(g.gui() + 0x18);
            const u32 gadgets = top ? r.r32(top + 4) : 0;
            u32 idx = 0xFFFFFFFFu;
            if (gadgets) r.call(0x0049FDF0, 0, {gadgets, r.str(t.substr(0, c))}, &idx);
            const u32 off = static_cast<u32>(std::strtoul(t.c_str() + c + 1, nullptr, 16));
            if (idx != 0xFFFFFFFFu) std::printf("%s: %08X\n", s.c_str(), r.r32(gadgets + idx * 0x15B + off));
            continue;
        } else if (s.rfind("gsptr:", 0) == 0) {
            // Debugging: gsptr:OFF prints the pointer at GS+OFF and the first 48 bytes it points at.
            const u32 off = static_cast<u32>(std::strtoul(s.c_str() + 6, nullptr, 16));
            const u32 p = r.r32(gs + off);
            std::uint8_t b[48] = {};
            if (p) r.read(p, b, sizeof b);
            std::printf("GS+%X -> %08X:", off, p);
            for (std::uint8_t c : b) std::printf(" %02X", c);
            std::printf("  \"");
            for (std::uint8_t c : b) std::printf("%c", c >= 0x20 && c < 0x7F ? c : '.');
            std::printf("\"\n");
            continue;
        } else if (s == "colours") {
            // Debugging: the interface colour table (gui +0x8B2) and the start of the palette
            // GS+0x143A7 and of the GUI's palette copy (gui +0xB2).
            std::uint8_t t[32] = {}, pal[12] = {}, gp[12] = {};
            r.read(g.gui() + 0x8B2, t, sizeof t);
            r.read(gs + 0x143A7, pal, sizeof pal);
            r.read(g.gui() + 0xB2, gp, sizeof gp);
            std::printf("colours:");
            for (int k = 0; k < 21; ++k) std::printf(" %d", t[k]);
            std::printf("\n  GS+0x143A7: %u %u %u / %u %u %u / %u %u %u; gui+0xB2: %u %u %u / %u %u %u / %u %u %u\n", pal[0], pal[1], pal[2], pal[4], pal[5], pal[6],
                        pal[8], pal[9], pal[10], gp[0], gp[1], gp[2], gp[4], gp[5], gp[6], gp[8], gp[9], gp[10]);
            continue;
        } else if (s.rfind("callx:", 0) == 0) {
            // Debugging: callx:ADDR:SELF:ARG,ARG..  calls a routine (SELF and each ARG a memx
            // expression; "buf" a fresh 64-byte block printed afterwards) and prints EAX.
            std::vector<std::string> f;
            for (std::size_t a = 6;;) {
                const std::size_t e = s.find(':', a);
                f.push_back(s.substr(a, e == std::string::npos ? std::string::npos : e - a));
                if (e == std::string::npos) break;
                a = e + 1;
            }
            auto eval = [&](const std::string& e) {
                u32 v = 0, num = 0;
                bool have = false;
                for (char ch : e + "+") {
                    if (std::isxdigit(static_cast<unsigned char>(ch))) {
                        num = num * 16 + static_cast<u32>(std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (std::tolower(ch) - 'a' + 10));
                        have = true;
                    } else {
                        if (have) v += num;
                        num = 0;
                        have = false;
                        if (ch == '*') v = r.r32(v);
                    }
                }
                return v;
            };
            if (f.size() < 2) continue;
            std::vector<u32> args;
            u32 buf = 0;
            if (f.size() > 2)
                for (std::size_t a = 0; a <= f[2].size();) {
                    const std::size_t e = f[2].find(',', a);
                    const std::string t = f[2].substr(a, e == std::string::npos ? std::string::npos : e - a);
                    if (t == "buf") {
                        buf = r.alloc(64);
                        std::vector<std::uint8_t> z(64, 0);
                        r.write(buf, z.data(), z.size());
                        args.push_back(buf);
                    } else if (!t.empty()) {
                        args.push_back(eval(t));
                    }
                    if (e == std::string::npos) break;
                    a = e + 1;
                }
            u32 result = 0;
            const bool done = r.call(eval(f[0]), eval(f[1]), args, &result);
            std::printf("%s: %s, EAX %08X", s.c_str(), done ? "ok" : "failed", result);
            if (buf) {
                std::uint8_t b[32] = {};
                r.read(buf, b, sizeof b);
                std::printf(", buf:");
                for (std::uint8_t x : b) std::printf(" %02X", x);
            }
            std::printf("\n");
            continue;
        } else if (s.rfind("memx:", 0) == 0) {
            // Debugging: memx:EXPR[:N] prints N (default 16) bytes at EXPR: hex numbers added
            // with '+', a '*' reads the 32-bit word at the address so far (e.g. 511DE8*+2A30*).
            const std::string t = s.substr(5);
            const auto c = t.find(':');
            const std::string e = t.substr(0, c);
            const u32 n = c == std::string::npos ? 16u : static_cast<u32>(std::strtoul(t.c_str() + c + 1, nullptr, 10));
            u32 v = 0, num = 0;
            bool have = false;
            for (char ch : e + "+") {
                if (std::isxdigit(static_cast<unsigned char>(ch))) {
                    num = num * 16 + static_cast<u32>(std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (std::tolower(ch) - 'a' + 10));
                    have = true;
                } else {
                    if (have) v += num;
                    num = 0;
                    have = false;
                    if (ch == '*') v = r.r32(v);
                }
            }
            std::vector<std::uint8_t> b(std::min<u32>(n, 256), 0);
            if (v) r.read(v, b.data(), b.size());
            std::printf("%s @%08X:", s.c_str(), v);
            for (std::uint8_t x : b) std::printf(" %02X", x);
            std::printf("\n");
            continue;
        } else if (s.rfind("mem:", 0) == 0) {
            // Debugging: mem:ADDR[:N] prints N (default 16) bytes at the absolute address ADDR and,
            // with *ADDR, at the address stored there.
            std::string a = s.substr(4);
            const bool deref = !a.empty() && a[0] == '*';
            if (deref) a = a.substr(1);
            const auto c = a.find(':');
            u32 addr = static_cast<u32>(std::strtoul(a.c_str(), nullptr, 16));
            const u32 n = c == std::string::npos ? 16u : static_cast<u32>(std::strtoul(a.c_str() + c + 1, nullptr, 10));
            if (deref) addr = r.r32(addr);
            std::vector<std::uint8_t> b(std::min<u32>(n, 256), 0);
            if (addr) r.read(addr, b.data(), b.size());
            std::printf("%s @%08X:", s.c_str(), addr);
            for (std::uint8_t v : b) std::printf(" %02X", v);
            std::printf("\n");
            continue;
        } else if (s == "unitdefs") {
            // The unit definitions from the original's own enumeration (0x42A8D0), before a room
            // (its unit definition exchange object is made from them when the room opens).
            r.w32(gs + 0x1439B, 0);
            r.w32(gs + 0x1438F, 0);
            ok = r.call(0x0042A8D0, 0, {}, nullptr, 120000);
            std::printf("unitdefs: %s, %u definitions\n", ok ? "ok" : "failed", r.r32(gs + 0x1438F));
            continue;
        } else if (s == "restrict") {
            // The battle room's build restrictions (RESTRICT2.GUI, 0x44C7E0) over the room.
            ok = r.call(0x0044C7E0, 0, {});
            std::printf("restrict: %s\n", ok ? "opened" : "failed");
            continue;
        } else if (s == "restrictframe") {
            // One pass of the restrictions' own frame routine (0x44C220: a unit picture every two
            // ticks, the counts the exchange changed).
            ok = r.call(0x0044C220, 0, {});
            continue;
        } else if (s == "loungeframe") {
            // One more frame of the battle room (the chat scrolls a line a frame).
            ok = r.call(0x0044A680, 0, {});
            continue;
        } else if (s.rfind("loadscreen", 0) == 0 || s.rfind("loadframe:", 0) == 0) {
            // The battle's loading screen (0x497F40, the routine the main loop runs while the
            // loading thread works). "loadscreen" runs its first call: the set-up (layouts
            // closed, the game palette and GUIPAL's interface colours, loadgame2bg, the flash
            // state cleared; the loading thread is not started) and the first frame, all rows at
            // 0. "loadframe:T,TE,U,A,D,E[:READY[:BITS]]" sets the six rows (textures, terrain,
            // units, animation, 3D data, explosions; percent) and draws one more frame; READY
            // (network games, game type 3) answers the "everybody loaded" question (0x4568C0)
            // and BITS are ORed into the loading flags (8: synchronised). The screen as
            // presented is written as <out>load_<n>.bmp (n counts the frames). The loading
            // routine's sleep, the registry save, the network's sends and its event pumps do
            // nothing; the remote players' progress comes from "loadplayer:SLOT,PERCENT,READY".
            static int load_n = 0;
            r.stub(0x004B6B20, 1, 0xC);  // the loading thread: "started"
            r.stub(0x00430F00, 0, 0);    // the settings' registry save
            r.stub(0x004B6B50, 0, 4);    // Sleep(200) after each frame
            r.stub(0x00453320, 0, 8);    // network: a player's packets
            r.stub(0x00453D40, 0, 0);    // network: receive
            r.stub(0x004C9790, 0, 4);    // network: synchronised
            r.stub(0x004618A0, 0, 4);    // network: the session's message pump
            r.stub(0x00451DF0, 0, 0xC);  // network: guaranteed broadcast (the own progress)
            u32 bits = 0, ready = 0;
            if (s[4] == 's') {
                r.w16(gs + 0x38D75, static_cast<std::uint16_t>(r.r32(gs + 0x38D75) & ~0xFu));
            } else {
                int v[6] = {};
                const char* q = s.c_str() + 10;
                std::sscanf(q, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
                for (int k = 0; k < 6; ++k) r.w8(gs + 0x38D6F + static_cast<u32>(k), static_cast<std::uint8_t>(v[k]));
                if (const char* c = std::strchr(q, ':')) {
                    ready = static_cast<u32>(std::strtoul(c + 1, nullptr, 0));
                    if (const char* c2 = std::strchr(c + 1, ':')) bits = static_cast<u32>(std::strtoul(c2 + 1, nullptr, 0));
                }
                r.w16(gs + 0x38D75, static_cast<std::uint16_t>((r.r32(gs + 0x38D75) & 0xFFFF) | bits));
            }
            r.stub(0x004568C0, ready, 0);
            ok = r.call(0x00497F40, 0, {});
            Frame f;
            if (ok && g.grab_raw(f)) {
                const std::string name = out + "load_" + std::to_string(load_n) + ".bmp";
                write_bmp(name, f);
                std::uint8_t flash[6] = {}, prev[6] = {};
                r.read(0x0051E6C8, flash, 6);
                r.read(0x0051E820, prev, 6);
                std::printf("%s: frame %d %dx%d -> %s, flags %04X, flash %u %u %u %u %u %u\n", s.c_str(), load_n, f.w, f.h, name.c_str(),
                            r.r32(gs + 0x38D75) & 0xFFFF, flash[0], flash[1], flash[2], flash[3], flash[4], flash[5]);
            } else {
                std::printf("%s: failed\n", s.c_str());
            }
            ++load_n;
            continue;
        } else if (s.rfind("loadmap:", 0) == 0) {
            // loadmap:NAME: the map info's chosen map (0x435A20, as the map choice sets it): the
            // loading screen's map line names it (0x435C30).
            const u32 schema = r.r32(gs + 0x391E9);
            if (schema) r.call(0x00435A20, schema, {r.str(s.substr(8))});
            char sel[0x8F] = {};
            if (schema) r.read(schema + 0xA14, sel, sizeof sel - 1);
            std::printf("%s: map info %08X chose \"%s\"\n", s.c_str(), schema, sel);
            continue;
        } else if (s.rfind("loadplayer:", 0) == 0) {
            // loadplayer:SLOT,PERCENT,READY: a player's loading progress as its packets report it
            // (record +0x20) and whether it said it has loaded (GS+0x29A4[slot]).
            int slot = 0, pct = 0, rd = 0;
            std::sscanf(s.c_str() + 11, "%d,%d,%d", &slot, &pct, &rd);
            if (slot >= 0 && slot < 10) {
                r.w8(gs + 0x1B63 + static_cast<u32>(slot) * 0x14B + 0x20, static_cast<std::uint8_t>(pct));
                r.w32(gs + 0x29A4 + static_cast<u32>(slot) * 4, static_cast<u32>(rd));
            }
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
        if (quiet) continue;
        Frame f;
        if (!ok || !g.grab(f)) {
            std::printf("%s: not drawn\n", s.c_str());
            continue;
        }
        const std::string name = s.substr(0, s.find(':')) + (bw != 640 || bh != 480 ? "_" + std::to_string(bw) : "");
        write_bmp(out + name + ".bmp", f);
        std::printf("%s: %dx%d -> %s%s.bmp\n", s.c_str(), f.w, f.h, out.c_str(), name.c_str());
    }
    (void)bw;
    (void)bh;
    return 0;
}
