// The typed commands against the original (docs/RE_MEMORY.md, "Console commands"):
//  1. the registry the original builds (0x419560 and the profile commands 0x406F00): every
//     name and permission bits against ta::console_table();
//  2. random command lines through the original's line runner (0x417B50: tokenizer 0x4B7440,
//     dispatcher 0x4B7900) with every registered handler replaced by a stub that copies the
//     words it gets, against sim::console::split + Registry::dispatch: the bits returned, the
//     command that ran (or the default command) and its words;
//  3. the default command (0x417890) creating units by name: the snap (0x47DDC0) and creation
//     (0x485F50) calls and the include file it would open, against sim::console::place_unit_row;
//  4. "+meteor" without an argument (0x438070) against sim::console::start_meteor_shower;
//  5. the flag commands' handlers run for real (switch words, sight rebuild argument) and the
//     resource commands (+atm, +nometal, +noenergy, +give), against the spec.
//
// Usage: console_harness <TotalA.exe> [scenarios] [lines] [first seed]

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/console.h"
#include "sim/rng.h"
#include "sim/sky.h"
#include "ta/console_commands.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using sim::console::Words;

constexpr u32 kGameStatePtr = 0x00511DE8, kRegisterAll = 0x00419560, kRegisterProfile = 0x00406F00, kRunLine = 0x00417B50,
              kTokenize = 0x004B7440, kDefaultCmd = 0x00417890, kVecBegin = 0x0051FC9D, kVecEnd = 0x0051FCA1, kDefaultFn = 0x0051FC90,
              kDefaultBits = 0x0051FC94, kCreate = 0x00485F50, kSnap = 0x0047DDC0, kOpenFile = 0x004BB5B0, kRand = 0x004E4870,
              kMeteorNow = 0x00438070, kSaveSettings = 0x00430F00, kRebuildSight = 0x004816A0, kConsoleAdd = 0x00463CA0,
              kTransferEnergy = 0x00464B30, kTransferMetal = 0x00464C60, kSfx = 0x0051E608;
constexpr u32 kDefSize = 0x249, kPlayerSize = 0x14B;
enum : u32 {
    gLocal = 0x2A42, gView = 0x2A43, gCursor = 0x2CAA, gPlayers = 0x1B63, gPixW = 0x1422B, gMapW = 0x14233, gMapH = 0x14237,
    gDefCount = 0x1438F, gDefs = 0x1439B, gTick = 0x38A47, gOptions = 0x37F2F, gVisual = 0x37F06, gMapFlags = 0x14281,
};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void bytes(std::initializer_list<std::uint8_t> l) {
        for (auto v : l) u8(v);
    }
    void align() {
        while (b.size() % 16) b.push_back(0xCC);
    }
};

struct Layout {
    u32 heap = 0, bump = 0, stubs = 0, log_ptr = 0, log = 0, gs = 0, line = 0, tokens = 0, defs = 0, rand_state = 0, logos = 0;
    std::size_t stub_used = 0;
};

constexpr u32 kTokenCopy = 0xD4, kHandlerRecord = 8 + kTokenCopy;

// A stub for a handler (stdcall, one argument: the words): logs {index, words address} and a
// copy of the 0xD4-byte words block.
void emit_handler_stub(Emitter& e, u32 log_ptr, u32 index) {
    e.bytes({0x56, 0x57});                       // push esi; push edi
    e.bytes({0x8B, 0x3D}); e.u32le(log_ptr);     // mov edi,[log]
    e.bytes({0xC7, 0x07}); e.u32le(index);       // mov dword [edi],index
    e.bytes({0x8B, 0x74, 0x24, 0x0C});           // mov esi,[esp+12]
    e.bytes({0x89, 0x77, 0x04});                 // mov [edi+4],esi
    e.bytes({0x83, 0xC7, 0x08});                 // add edi,8
    e.u8(0xB9); e.u32le(kTokenCopy / 4);         // mov ecx,0x35
    e.bytes({0xF3, 0xA5});                       // rep movsd
    e.bytes({0x89, 0x3D}); e.u32le(log_ptr);     // mov [log],edi
    e.bytes({0x5F, 0x5E, 0xC2, 0x04, 0x00});     // pop edi; pop esi; ret 4
    e.align();
}

// A logging stub: {id, stack args...}, `pop` stack bytes on return, EAX = 0.
void emit_log_stub(Emitter& e, u32 log_ptr, u32 id, int args, int pop) {
    e.u8(0xA1); e.u32le(log_ptr);                          // mov eax,[log]
    e.bytes({0xC7, 0x00}); e.u32le(id);                     // mov [eax],id
    for (int i = 0; i < args; ++i) {
        e.bytes({0x8B, 0x54, 0x24, static_cast<std::uint8_t>(4 + 4 * i)});  // mov edx,[esp+4+4i]
        e.bytes({0x89, 0x50, static_cast<std::uint8_t>(4 + 4 * i)});        // mov [eax+4+4i],edx
    }
    e.bytes({0x83, 0x05}); e.u32le(log_ptr); e.u8(static_cast<std::uint8_t>(4 + 4 * args));  // add [log],n
    e.bytes({0x33, 0xC0});                                  // xor eax,eax
    if (pop == 0) e.u8(0xC3);
    else e.bytes({0xC2, static_cast<std::uint8_t>(pop), 0x00});
    e.align();
}

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(16u << 20);
    L.bump = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log = r.alloc(1u << 20);
    L.gs = r.alloc(0x40000);
    L.line = r.alloc(0x1000);
    L.tokens = r.alloc(0x1000);
    L.defs = r.alloc(64 * kDefSize);
    L.rand_state = r.alloc(0x1000);
    L.logos = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.gs) return false;
    Emitter e;
    auto here = [&] { return L.stubs + static_cast<u32>(e.b.size()); };
    // Bump allocators in place of the heap routines and operator new.
    const int size_at[4] = {8, 4, 12, 4};
    const u32 targets[4] = {0x004D83B0, 0x004B4F10, 0x004D84A0, 0x004E8890};
    for (int k = 0; k < 4; ++k) {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.bump);
        e.bytes({0x8B, 0x4C, 0x24, static_cast<std::uint8_t>(size_at[k])});
        e.bytes({0x83, 0xC1, 0x03, 0x83, 0xE1, 0xFC});
        e.bytes({0x01, 0x0D}); e.u32le(L.bump);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    for (u32 f : {0x004B4F20u, 0x004E8820u}) {  // frees: nothing
        const u32 at = here();
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(f, at)) return false;
    }
    {  // rand() on our own state
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.rand_state);
        e.bytes({0x69, 0xC0}); e.u32le(214013u);
        e.u8(0x05); e.u32le(2531011u);
        e.u8(0xA3); e.u32le(L.rand_state);
        e.bytes({0xC1, 0xE8, 0x10});
        e.u8(0x25); e.u32le(0x7FFF);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(kRand, at)) return false;
    }
    // The default command's outside calls: snap (def, pos*) logs and moves the point; create
    // (8 args) logs; the file open (path) copies the path.
    {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.log_ptr);                        // mov eax,[log]
        e.bytes({0xC7, 0x00}); e.u32le(1);                      // id 1
        e.bytes({0x8B, 0x54, 0x24, 0x04, 0x89, 0x50, 0x04});    // def
        e.bytes({0x8B, 0x4C, 0x24, 0x08});                      // ecx = pos
        e.bytes({0x8B, 0x11, 0x89, 0x50, 0x08});                // x
        e.bytes({0x8B, 0x51, 0x04, 0x89, 0x50, 0x0C});          // y
        e.bytes({0x8B, 0x51, 0x08, 0x89, 0x50, 0x10});          // z
        // x = (x & 0xFFF00000) | 0x80000, y = 0x370000
        e.bytes({0x8B, 0x11, 0x81, 0xE2}); e.u32le(0xFFF00000u);
        e.bytes({0x81, 0xCA}); e.u32le(0x80000u);
        e.bytes({0x89, 0x11});
        e.bytes({0xC7, 0x41, 0x04}); e.u32le(0x370000u);
        e.bytes({0x83, 0x05}); e.u32le(L.log_ptr); e.u8(0x14);
        e.bytes({0xC2, 0x08, 0x00});
        e.align();
        if (!r.patch_jump(kSnap, at)) return false;
    }
    {
        const u32 at = here();
        emit_log_stub(e, L.log_ptr, 2, 8, 0x20);
        if (!r.patch_jump(kCreate, at)) return false;
    }
    {
        const u32 at = here();
        e.bytes({0x56, 0x57});
        e.bytes({0x8B, 0x3D}); e.u32le(L.log_ptr);
        e.bytes({0xC7, 0x07}); e.u32le(3);
        e.bytes({0x8B, 0x74, 0x24, 0x0C});
        e.bytes({0x83, 0xC7, 0x04});
        e.u8(0xB9); e.u32le(16);
        e.bytes({0xF3, 0xA5});
        e.bytes({0x89, 0x3D}); e.u32le(L.log_ptr);
        e.bytes({0x5F, 0x5E, 0x33, 0xC0, 0xC2, 0x04, 0x00});
        e.align();
        if (!r.patch_jump(kOpenFile, at)) return false;
    }
    for (u32 f : {kSaveSettings, 0x00437C80u}) {  // settings save, drawing cache rebuild: nothing
        const u32 at = here();
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(f, at)) return false;
    }
    {  // sight rebuild (clear): logged
        const u32 at = here();
        emit_log_stub(e, L.log_ptr, 4, 1, 4);
        if (!r.patch_jump(kRebuildSight, at)) return false;
    }
    {  // console line (text, kind, x, colour): logged with a copy of 64 bytes of text
        const u32 at = here();
        e.bytes({0x56, 0x57});
        e.bytes({0x8B, 0x3D}); e.u32le(L.log_ptr);
        e.bytes({0xC7, 0x07}); e.u32le(5);
        e.bytes({0x8B, 0x74, 0x24, 0x0C});
        e.bytes({0x83, 0xC7, 0x04});
        e.u8(0xB9); e.u32le(16);
        e.bytes({0xF3, 0xA5});
        e.bytes({0x89, 0x3D}); e.u32le(L.log_ptr);
        e.bytes({0x5F, 0x5E, 0x33, 0xC0, 0xC2, 0x10, 0x00});
        e.align();
        if (!r.patch_jump(kConsoleAdd, at)) return false;
    }
    {
        const u32 at = here();
        emit_log_stub(e, L.log_ptr, 6, 4, 0x10);
        if (!r.patch_jump(kTransferEnergy, at)) return false;
    }
    {
        const u32 at = here();
        emit_log_stub(e, L.log_ptr, 7, 4, 0x10);
        if (!r.patch_jump(kTransferMetal, at)) return false;
    }
    L.stub_used = e.b.size();
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump, L.heap);
    r.w32(kGameStatePtr, L.gs);
    return true;
}

std::string read_cstr(harness::Remote& r, u32 at) {
    std::string s;
    for (int i = 0; i < 256; ++i) {
        char c = 0;
        r.read(at + static_cast<u32>(i), &c, 1);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

// Words out of a copied words block (pointers relative to the block's own address).
Words words_of(const std::vector<std::uint8_t>& block, u32 addr) {
    Words w;
    u32 count = 0;
    std::memcpy(&count, block.data() + 0xD0, 4);
    for (u32 k = 0; k < count && k < 20; ++k) {
        u32 p = 0;
        std::memcpy(&p, block.data() + 4 * k, 4);
        std::string s;
        for (u32 at = p - addr; at < kTokenCopy && block[at]; ++at) s.push_back(static_cast<char>(block[at]));
        w.words.push_back(s);
    }
    return w;
}

std::string random_case(std::mt19937& g, const std::string& s) {
    std::string o = s;
    const int mode = static_cast<int>(g() % 4);
    for (char& c : o) {
        if (mode == 1) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (mode == 2) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (mode == 3 && g() % 2) c = static_cast<char>(std::isupper(static_cast<unsigned char>(c)) ? std::tolower(c) : std::toupper(c));
    }
    return o;
}

const char* kJunk[] = {"ARMCOM", "corak", "ARM*", "*", "?", "cor??", "nosuch", "1", "-5", "0x10", "12abc", "+los", "Film", "Chris",
                       "Include", "Reload", "Assert", "metal", "energy", "all", "%1", "move", "", "a", "zz", "Now", "losx", "LO", "Dithe"};
const char* kSeps[] = {" ", "  ", "\t", "\v", "\f", "\r", " \t "};

std::string random_line(std::mt19937& g, const std::vector<ta::ConsoleTableEntry>& table) {
    for (;;) {
        std::string line;
        if (g() % 4 == 0) line += kSeps[g() % 7];
        const int n = static_cast<int>(g() % 6 == 0 ? g() % 24 : g() % 5);
        std::size_t used = 0;
        for (int k = 0; k < n; ++k) {
            std::string word = (k == 0 && g() % 4 != 0) ? random_case(g, table[g() % table.size()].name) : kJunk[g() % (sizeof kJunk / sizeof kJunk[0])];
            if (word.empty()) continue;
            if (g() % 15 == 0) word.insert(g() % (word.size() + 1), "#");
            line += word;
            used += word.size() + 1;
            line += kSeps[g() % 7];
        }
        if (used < 0x70) return line;  // longer word text never finishes in the original
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: console_harness <TotalA.exe> [scenarios] [lines] [first seed]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 4;
    const int lines = argc > 3 ? std::atoi(argv[3]) : 100;
    const u32 first_seed = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    const auto& table = ta::console_table();
    std::size_t mismatches = 0, n_lines = 0, n_ran = 0, n_default = 0, n_rows = 0, n_units = 0, n_includes = 0, n_meteors = 0, n_flags = 0,
                n_resources = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first_seed + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        r.install_crash_catcher();
        Layout L;
        if (!setup(r, L)) {
            std::printf("scenario %u: set-up failed\n", seed);
            return 1;
        }
        auto fail = [&](const std::string& what) {
            const auto c = r.crash_info();
            std::printf("scenario %u: %s failed (code %08x at %08x)\n", seed, what.c_str(), c.code, c.address);
            ++mismatches;
        };
        auto reset_log = [&] { r.w32(L.log_ptr, L.log); };
        // ---- 1. the registry ----
        if (!r.call(kRegisterAll, 0, {}) || !r.call(kRegisterProfile, 0, {})) {
            fail("registration");
            continue;
        }
        const u32 vb = r.r32(kVecBegin), ve = r.r32(kVecEnd);
        const u32 count = (ve - vb) / 12;
        std::vector<std::string> names;
        bool reg_ok = count == table.size();
        for (u32 k = 0; k < count; ++k) {
            const std::string name = read_cstr(r, r.r32(vb + 12 * k));
            const u32 bits = r.r32(vb + 12 * k + 8);
            names.push_back(name);
            if (ta::console_table_bits(name) != bits) {
                std::printf("scenario %u: entry %s bits orig %u ours %u\n", seed, name.c_str(), bits, ta::console_table_bits(name));
                reg_ok = false;
            }
        }
        const u32 real_default = r.r32(kDefaultFn);
        if (real_default != kDefaultCmd || r.r32(kDefaultBits) != 4) reg_ok = false;
        if (!reg_ok) {
            std::printf("scenario %u: registry differs (orig %u entries, ours %zu)\n", seed, count, table.size());
            ++mismatches;
            continue;
        }
        // Handler stubs over the registry.
        {
            Emitter e;
            std::vector<u32> handlers(count);
            const u32 base = L.stubs + 0x2000;
            for (u32 k = 0; k <= count; ++k) {
                const u32 at = base + static_cast<u32>(e.b.size());
                emit_handler_stub(e, L.log_ptr, k == count ? 0xFFFFu : k);
                if (k < count) r.w32(vb + 12 * k + 4, at);
                else r.w32(kDefaultFn, at);
            }
            r.write(base, e.b.data(), e.b.size());
        }
        // Ours: the same names with logging handlers.
        sim::console::Registry reg;
        std::string ran_name;
        Words ran_words;
        int ran_count = 0;
        for (const auto& t : table)
            reg.add(t.name, [&, n = std::string(t.name)](const Words& w) { ran_name = n, ran_words = w, ++ran_count; }, t.bits);
        reg.set_default([&](const Words& w) { ran_name = "<default>", ran_words = w, ++ran_count; }, 4);
        // ---- 2. lines ----
        static const u32 kMasks[] = {0, 1, 2, 3, 4, 5, 7, 8, 0xF, 0xFFFFFFFFu};
        for (int ln = 0; ln < lines; ++ln) {
            const std::string line = random_line(g, table);
            const u32 mask = g() % 8 == 0 ? g() : kMasks[g() % 10];
            r.write(L.line, line.c_str(), line.size() + 1);
            reset_log();
            u32 orig_bits = 0;
            if (!r.call(kRunLine, 0, {L.line, mask}, &orig_bits)) {
                fail("line \"" + line + "\"");
                break;
            }
            const u32 n = (r.r32(L.log_ptr) - L.log) / kHandlerRecord;
            ran_count = 0;
            ran_name.clear();
            const Words w = sim::console::split(line);
            const u32 our_bits = reg.dispatch(w, mask);
            bool ok = orig_bits == our_bits && n == static_cast<u32>(ran_count);
            if (ok && n == 1) {
                std::vector<std::uint8_t> rec(kHandlerRecord);
                r.read(L.log, rec.data(), rec.size());
                u32 idx = 0, addr = 0;
                std::memcpy(&idx, rec.data(), 4);
                std::memcpy(&addr, rec.data() + 4, 4);
                const std::string orig_name = idx == 0xFFFF ? "<default>" : names[idx];
                const Words ow = words_of(std::vector<std::uint8_t>(rec.begin() + 8, rec.end()), addr);
                ok = orig_name == ran_name && ow.words == ran_words.words;
                n_default += idx == 0xFFFF;
                if (!ok) std::printf("  ran orig %s (%d words) ours %s (%d words)\n", orig_name.c_str(), ow.count(), ran_name.c_str(), ran_words.count());
            }
            ++n_lines;
            n_ran += n;
            if (!ok) {
                std::printf("scenario %u line %d \"%s\" mask %08x: orig bits %u handlers %u, ours bits %u handlers %d\n", seed, ln, line.c_str(), mask,
                            orig_bits, n, our_bits, ran_count);
                ++mismatches;
                break;
            }
        }
        // ---- 3. the default command's unit row ----
        {
            static const char* kNames[] = {"ARMCOM", "ARMPW", "ARMSOLAR", "ARMMEX", "CORCOM", "CORAK", "CORSOLAR", "CORMEX", "ARMFLASH",
                                           "CORGATOR", "ARMLAB", "CORLAB", "ARMVP", "ARMWIN", "CORWIN", "ARMRADAR", "CORRAD", "ARMBULL"};
            const int ndefs = 6 + static_cast<int>(g() % 12);
            std::vector<std::string> dnames(static_cast<std::size_t>(ndefs));
            std::vector<std::array<i32, 2>> ext(static_cast<std::size_t>(ndefs));
            std::vector<std::uint8_t> db(static_cast<std::size_t>(ndefs) * kDefSize, 0);
            for (int k = 1; k < ndefs; ++k) {
                dnames[static_cast<std::size_t>(k)] = kNames[g() % (sizeof kNames / sizeof kNames[0])];
                ext[static_cast<std::size_t>(k)] = {-static_cast<i32>(g() % 0x400000), static_cast<i32>(g() % 0x400000)};
                std::uint8_t* d = db.data() + static_cast<std::size_t>(k) * kDefSize;
                std::memcpy(d + 0x20, dnames[static_cast<std::size_t>(k)].c_str(), dnames[static_cast<std::size_t>(k)].size() + 1);
                std::memcpy(d + 0x15E, &ext[static_cast<std::size_t>(k)][0], 4);
                std::memcpy(d + 0x16A, &ext[static_cast<std::size_t>(k)][1], 4);
            }
            r.write(L.defs, db.data(), db.size());
            r.w32(L.gs + gDefs, L.defs);
            r.w32(L.gs + gDefCount, static_cast<u32>(ndefs));
            for (int call = 0; call < 6; ++call) {
                const i32 pixw = static_cast<i32>(256 + g() % 3800);
                r.w32(L.gs + gPixW, static_cast<u32>(pixw));
                std::array<i32, 3> cursor = {static_cast<i32>(g() % static_cast<u32>(pixw + 64) << 16) | static_cast<i32>(g() & 0xFFFF),
                                             static_cast<i32>(g() % 0x1000000), static_cast<i32>(g() % 0x10000000)};
                r.write(L.gs + gCursor, cursor.data(), 12);
                static const char* kPatterns[] = {"ARMCOM", "armcom", "ARM*", "*", "COR?OM", "*SOLAR", "nomatch", "?", "C*", "ARM???", "*COM*", "debugstuff"};
                std::string line = kPatterns[g() % (sizeof kPatterns / sizeof kPatterns[0])];
                if (g() % 2) line += " " + std::to_string(static_cast<int>(g() % 12) - 1);
                r.write(L.line, line.c_str(), line.size() + 1);
                r.w32(L.tokens + 0xD0, 0);
                if (!r.call(kTokenize, L.tokens, {L.line, 0})) {
                    fail("tokenize");
                    break;
                }
                reset_log();
                if (!r.call(kDefaultCmd, 0, {L.tokens})) {
                    fail("default command \"" + line + "\"");
                    break;
                }
                // The original's log.
                std::vector<u32> orig;
                {
                    const u32 end = r.r32(L.log_ptr);
                    orig.resize((end - L.log) / 4);
                    if (!orig.empty()) r.read(L.log, orig.data(), orig.size() * 4);
                }
                // Ours.
                std::vector<u32> ours;
                const Words w = sim::console::split(line);
                const std::string pattern = w.text(0);
                std::array<i32, 3> pos = cursor;
                sim::console::UnitRowHooks hooks;
                hooks.matches = [&](std::uint16_t t) { return t < dnames.size() && sim::console::wildcard_match(dnames[t], pattern); };
                hooks.extents_x = [&](std::uint16_t t) { return ext[t]; };
                hooks.snap = [&](std::uint16_t t, std::array<i32, 3>& p) {
                    for (u32 v : {1u, L.defs + t * kDefSize, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])}) ours.push_back(v);
                    p[0] = static_cast<i32>((static_cast<u32>(p[0]) & 0xFFF00000u) | 0x80000u);
                    p[1] = 0x370000;
                };
                hooks.create = [&](std::uint16_t t, const std::array<i32, 3>& p) {
                    for (u32 v : {2u, static_cast<u32>(w.number(1, 0)), static_cast<u32>(t), static_cast<u32>(p[0]), static_cast<u32>(p[1]),
                                  static_cast<u32>(p[2]), 1u, 1u, 0u})
                        ours.push_back(v);
                    ++n_units;
                };
                const int matched = sim::console::place_unit_row(pos, pixw * 0x10000, ndefs, hooks);
                if (matched == 0) {
                    // The include file's name (16 words of the path copied).
                    ours.push_back(3u);
                    char path[64] = {};
                    std::snprintf(path, sizeof path, "debugdat\\%s.txt", pattern.c_str());
                    for (int k = 0; k < 16; ++k) {
                        u32 v = 0;
                        std::memcpy(&v, path + 4 * k, 4);
                        ours.push_back(v);
                    }
                    ++n_includes;
                }
                bool ok = orig.size() == ours.size();
                if (ok && matched == 0) {
                    // Compare the path only up to its terminator (the rest is stack contents).
                    const std::string op(reinterpret_cast<const char*>(orig.data() + 1)), pp(reinterpret_cast<const char*>(ours.data() + 1));
                    ok = op == pp;
                } else if (ok) {
                    ok = orig == ours;
                }
                ++n_rows;
                if (!ok) {
                    std::printf("scenario %u: default command \"%s\" differs (orig %zu log words, ours %zu)\n", seed, line.c_str(), orig.size(), ours.size());
                    ++mismatches;
                    break;
                }
            }
        }
        // ---- 4. a meteor shower now ----
        for (int k = 0; k < 4; ++k) {
            sim::MeteorShower m;
            m.duration = static_cast<i32>(g() % 3000);
            m.schedule_gap = static_cast<i32>(g() % 3000);
            const u32 tick = g() % 100000;
            const i32 mw = static_cast<i32>(16 + g() % 240), mh = static_cast<i32>(16 + g() % 240);
            sim::Lcg15 rand;
            rand.set_state(g());
            r.w32(L.rand_state, rand.state());
            r.w32(0x00512324, static_cast<u32>(m.duration));
            r.w32(0x00512338, static_cast<u32>(m.schedule_gap));
            r.w32(L.gs + gTick, tick);
            r.w32(L.gs + gMapW, static_cast<u32>(mw));
            r.w32(L.gs + gMapH, static_cast<u32>(mh));
            if (!r.call(kMeteorNow, 0, {})) {
                fail("meteor");
                break;
            }
            sim::console::start_meteor_shower(m, tick, mw, mh, rand);
            const bool ok = (r.r32(0x00512318) != 0) == m.active && r.r32(0x0051231C) == m.end_tick && r.r32(0x005122E8) == m.next_schedule &&
                            r.r32(0x00512330) == m.next_spawn && r.rd<std::int16_t>(0x00512334) == m.origin_x && r.rd<std::int16_t>(0x00512336) == m.origin_z &&
                            r.rd<std::int16_t>(0x00512320) == m.aim_x && r.rd<std::int16_t>(0x00512322) == m.aim_z && r.r32(L.rand_state) == rand.state();
            ++n_meteors;
            if (!ok) {
                std::printf("scenario %u: meteor start differs\n", seed);
                ++mismatches;
                break;
            }
        }
        // ---- 5. switch and resource commands run for real ----
        {
            struct Flag {
                const char* name;
                u32 offset, mask;
                int rebuild;  // sight rebuild argument (-1: none)
            };
            // The game state word and bit each switch flips (the engine field in brackets).
            static const Flag kFlags[] = {
                {"NoShake", gOptions, 0x10, -1},     // [settings.shake_disabled]
                {"ShootAll", gOptions, 0x400, -1},   // [settings.target_everything]
                {"DoubleShot", gOptions, 0x80, -1},  // [settings.damage_options 0x80]
                {"HalfShot", gOptions, 0x100, -1},   // [settings.damage_options 0x100]
                {"TreeDeath", gOptions, 0x8, -1},    // [settings.damage_options 8]
                {"Radar", gOptions, 0x200, -1},      // [client radar_everything]
                {"Clock", gOptions, 0x40, -1},       // [client clock]
                {"SelBoxes", gOptions, 0x4, -1},     // [client selection_boxes]
                {"Dither", gVisual, 0x40, -1},       // [client dithered_fog]
                {"Shading", gVisual, 0x20, -1},
                {"AntiAlias", gVisual, 0x2, -1},
                {"Shadow", gVisual, 0x4, -1},
                {"TShadow", gVisual, 0x8, -1},
                {"FShadow", gVisual, 0x10, -1},
                {"LOS", gMapFlags, 0x2, 0},          // [settings.fog_of_war], rebuild keeping the explored map
                {"Mapping", gMapFlags, 0x1, 1},      // [settings.explore], rebuild clearing it
                {"LOSType", gMapFlags, 0x4, 0},      // [settings.true_sight]
            };
            for (int k = 0; k < 24; ++k) {
                const Flag& f = kFlags[g() % (sizeof kFlags / sizeof kFlags[0])];
                // Find the handler address in the original's own table lists.
                u32 handler = 0;
                for (u32 list : {0x00501D38u, 0x00501F48u, 0x00501FD0u})
                    for (u32 at = list; r.r32(at); at += 12)
                        if (read_cstr(r, r.r32(at)) == f.name) handler = r.r32(at + 4);
                const std::uint16_t before = static_cast<std::uint16_t>(g());
                r.write(L.gs + f.offset, &before, 2);
                reset_log();
                r.w32(L.tokens + 0xD0, 0);
                if (!handler || !r.call(handler, 0, {L.tokens})) {
                    fail(f.name);
                    break;
                }
                const std::uint16_t after = r.rd<std::uint16_t>(L.gs + f.offset);
                const u32 logged = (r.r32(L.log_ptr) - L.log) / 4;
                const bool ok = after == static_cast<std::uint16_t>(before ^ f.mask) &&
                                (f.rebuild < 0 ? logged == 0 : logged == 2 && r.r32(L.log + 4) == static_cast<u32>(f.rebuild));
                ++n_flags;
                if (!ok) {
                    std::printf("scenario %u: %s flips %04x -> %04x (logged %u)\n", seed, f.name, before, after, logged);
                    ++mismatches;
                    break;
                }
            }
            // NowISee: both sight options off, rebuild clearing the explored map.
            {
                const std::uint16_t before = static_cast<std::uint16_t>(g());
                r.write(L.gs + gMapFlags, &before, 2);
                reset_log();
                if (!r.call(0x00417540, 0, {L.tokens})) fail("NowISee");
                const bool ok = r.rd<std::uint16_t>(L.gs + gMapFlags) == (before & 0xFFFC) && r.r32(L.log) == 4 && r.r32(L.log + 4) == 1;
                ++n_flags;
                if (!ok) {
                    std::printf("scenario %u: NowISee differs\n", seed);
                    ++mismatches;
                }
            }
            // SFX: the effects switch.
            {
                const std::uint8_t before = static_cast<std::uint8_t>(g() % 2);
                r.write(kSfx, &before, 1);
                if (!r.call(0x00419550, 0, {L.tokens})) fail("SFX");
                ++n_flags;
                if (r.rd<std::uint8_t>(kSfx) != (before ^ 1)) {
                    std::printf("scenario %u: SFX differs\n", seed);
                    ++mismatches;
                }
            }
            // Resource commands on random player records.
            auto player_rec = [&](int p) { return L.gs + gPlayers + static_cast<u32>(p) * kPlayerSize; };
            auto setup_players = [&](std::array<bool, 10>& playing) {
                for (int p = 0; p < 10; ++p) {
                    std::vector<std::uint8_t> rec(kPlayerSize, 0);
                    const bool active = g() % 4 != 0;
                    const std::uint8_t controller = static_cast<std::uint8_t>(g() % 5);
                    const std::uint8_t state = g() % 6 == 0 ? 10 : static_cast<std::uint8_t>(p);
                    const u32 act = active ? 1u : 0u;
                    std::memcpy(rec.data(), &act, 4);
                    rec[0x73] = controller;
                    rec[0x146] = state;
                    float e = static_cast<float>(g() % 100000) / 7.0f, m = static_cast<float>(g() % 100000) / 3.0f;
                    if (g() % 10 == 0) e = -e;
                    std::memcpy(rec.data() + 0x8C, &e, 4);
                    std::memcpy(rec.data() + 0x98, &m, 4);
                    r.write(player_rec(p), rec.data(), rec.size());
                    playing[static_cast<std::size_t>(p)] = active && controller >= 1 && controller <= 3 && state != 10;
                }
            };
            for (int k = 0; k < 12; ++k) {
                std::array<bool, 10> playing{};
                setup_players(playing);
                const std::uint8_t local = static_cast<std::uint8_t>(g() % 10), view = static_cast<std::uint8_t>(g() % 10);
                r.write(L.gs + gLocal, &local, 1);
                r.write(L.gs + gView, &view, 1);
                const int which = static_cast<int>(g() % 4);
                std::string line;
                const int target = static_cast<int>(g() % 12) - 1;
                const int amount = static_cast<int>(g() % 5000) - 100;
                if (which == 0) line = "atm";
                else if (which == 1) line = g() % 3 ? "nometal " + std::to_string(target) + " " + std::to_string(amount) : "nometal";
                else if (which == 2) line = g() % 3 ? "noenergy " + std::to_string(target) + " " + std::to_string(amount) : "noenergy";
                else line = "give " + std::to_string(target) + " " + std::to_string(amount) + (g() % 2 ? " metal" : " ENERGY");
                const Words w = sim::console::split(line);
                // Expected (the spec) before running.
                std::array<float, 10> e_exp{}, m_exp{};
                for (int p = 0; p < 10; ++p) {
                    e_exp[static_cast<std::size_t>(p)] = r.rd<float>(player_rec(p) + 0x8C);
                    m_exp[static_cast<std::size_t>(p)] = r.rd<float>(player_rec(p) + 0x98);
                }
                std::vector<u32> transfer_exp;
                auto plays = [&](std::uint32_t n) { return (n & 0xFF) < 10 && playing[n & 0xFF]; };
                if (which == 0) {
                    e_exp[view] = static_cast<float>(static_cast<double>(e_exp[view]) - -1000.0);
                    m_exp[view] = static_cast<float>(static_cast<double>(m_exp[view]) - -1000.0);
                } else if (which == 1 || which == 2) {
                    const std::uint32_t p = w.count() == 1 ? local : static_cast<std::uint8_t>(w.number(1, 0));
                    if (plays(p)) (which == 1 ? m_exp : e_exp)[p & 0xFF] = static_cast<float>(w.number(2, 0));
                } else if (plays(static_cast<std::uint32_t>(w.number(1, 0)))) {
                    const float a = static_cast<float>(w.number(2, 0));
                    u32 abits = 0;
                    std::memcpy(&abits, &a, 4);
                    const bool metal = sim::console::c_stricmp(w.text(3), "metal") == 0;
                    transfer_exp = {metal ? 7u : 6u, local, static_cast<u32>(static_cast<std::uint8_t>(w.number(1, 0))), abits, 1u};
                }
                r.write(L.line, line.c_str(), line.size() + 1);
                r.w32(L.tokens + 0xD0, 0);
                reset_log();
                u32 handler = 0;
                for (u32 list : {0x00501D38u, 0x00501F48u})
                    for (u32 at = list; r.r32(at); at += 12)
                        if (sim::console::c_stricmp(read_cstr(r, r.r32(at)), w.text(0)) == 0) handler = r.r32(at + 4);
                if (!handler || !r.call(kTokenize, L.tokens, {L.line, 0}) || !r.call(handler, 0, {L.tokens})) {
                    fail(line);
                    break;
                }
                bool ok = true;
                for (int p = 0; p < 10 && ok; ++p) {
                    const float e = r.rd<float>(player_rec(p) + 0x8C), m = r.rd<float>(player_rec(p) + 0x98);
                    ok = std::memcmp(&e, &e_exp[static_cast<std::size_t>(p)], 4) == 0 && std::memcmp(&m, &m_exp[static_cast<std::size_t>(p)], 4) == 0;
                }
                std::vector<u32> logged((r.r32(L.log_ptr) - L.log) / 4);
                if (!logged.empty()) r.read(L.log, logged.data(), logged.size() * 4);
                if (logged.size() == 5) logged[1] &= 0xFF, logged[2] &= 0xFF;  // byte arguments
                ok = ok && logged == transfer_exp;
                ++n_resources;
                if (!ok) {
                    std::printf("scenario %u: \"%s\" differs (logged %zu words, expected %zu)\n", seed, line.c_str(), logged.size(), transfer_exp.size());
                    ++mismatches;
                    break;
                }
            }
        }
    }
    std::printf("lines %zu (%zu commands ran, %zu by the default command), unit rows %zu (%zu units, %zu include files), meteor starts %zu, "
                "switches %zu, resource commands %zu\n",
                n_lines, n_ran, n_default, n_rows, n_units, n_includes, n_meteors, n_flags, n_resources);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
