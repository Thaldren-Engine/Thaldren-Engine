// The saved-game bank container against the original: random stores (sections, integer,
// double and string keys, named and numbered blobs written in pieces) are built in the
// original's store (0x4B3750 ...) and written by its bank writer (0x4B39C0) twice, with the
// accounts stored and squashed. Compared: ta::write_bank's bytes with the stored file's
// header and accounts (the directory is squashed by the original when that is smaller), and
// ta::read_bank's reading of both files with the content put in.
//
// Usage: bank_harness <TotalA.exe> [scenarios] [first seed] [scratch dir]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>

#include "remote.h"
#include "ta/saved_game.h"
#include "ta/tdf.h"

namespace {

using harness::u32;

constexpr u32 kInitEmpty = 0x004B3750, kSection = 0x004B4560, kSetInt = 0x004B4630, kSetDouble = 0x004B46C0, kSetString = 0x004B4750,
              kBlob = 0x004B4BA0, kIndexedBlob = 0x004B4B50, kWriteBytes = 0x004B4CF0, kWriteFile = 0x004B39C0;

std::vector<std::uint8_t> read_file(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool same_bank(const ta::Bank& a, const ta::Bank& b, std::string& why) {
    if (ta::stricmp_c(a.name, b.name) != 0) return why = "bank name", false;
    if (a.sections.size() != b.sections.size()) return why = "section count", false;
    for (std::size_t s = 0; s < a.sections.size(); ++s) {
        const ta::BankSection& x = a.sections[s];
        const ta::BankSection& y = b.sections[s];
        if (x.name != y.name || x.ints != y.ints || x.strings != y.strings || x.doubles.size() != y.doubles.size()) return why = "section " + x.name, false;
        for (std::size_t k = 0; k < x.doubles.size(); ++k)
            if (x.doubles[k].first != y.doubles[k].first || std::memcmp(&x.doubles[k].second, &y.doubles[k].second, 8) != 0) return why = "double", false;
        if (x.blobs.size() != y.blobs.size()) return why = "blob count in " + x.name, false;
        for (std::size_t k = 0; k < x.blobs.size(); ++k)
            if (x.blobs[k].named != y.blobs[k].named || x.blobs[k].name != y.blobs[k].name || (!x.blobs[k].named && x.blobs[k].index != y.blobs[k].index) ||
                x.blobs[k].data != y.blobs[k].data)
                return why = "blob " + std::to_string(k) + " in " + x.name, false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bank_harness <TotalA.exe> [scenarios] [first seed] [scratch dir]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const u32 first = argc > 3 ? static_cast<u32>(std::strtoul(argv[3], nullptr, 0)) : 1u;
    const std::string dir = argc > 4 ? argv[4] : ".";
    std::size_t mismatches = 0, keys = 0, blobs = 0, squashed = 0, bytes_compared = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        // The runtime's fatal-error paths fault instead of showing a message box; then its
        // start-up parts (heap, thread data, I/O, code page, floating point).
        for (u32 fatal : {0x004E7180u, 0x004F18B0u, 0x004F3D00u}) {
            const std::uint8_t ud2[2] = {0x0F, 0x0B};
            if (!r.patch_jump(fatal, fatal) || !r.write(fatal, ud2, 2)) return 1;
        }
        for (u32 init : {0x004F1830u, 0x004EB040u, 0x004F15C0u, 0x004F0C50u, 0x004E4320u})
            if (!r.call(init, 0, {})) {
                std::printf("C runtime start-up %08x failed\n", init);
                return 1;
            }
        // The C initialisers (the exit table among them; not the program's constructors).
        if (!r.call(0x004E4740, 0, {0x00501050, 0x00501060})) {
            std::printf("C runtime initialisers failed\n");
            return 1;
        }
        // The command line and environment (the memory manager reads its options from them).
        {
            const u32 cmd = r.alloc(0x100);
            r.write(cmd, "TotalA.exe", 11);
            r.w32(0x0052B650, cmd);
            u32 env = 0;
            if (!r.call(0x004F1460, 0, {}, &env)) return 1;
            r.w32(0x00529F3C, env);
            if (!r.call(0x004F0FB0, 0, {}) || !r.call(0x004F0EC0, 0, {})) {
                std::printf("C runtime argument set-up failed\n");
                return 1;
            }
        }
        const u32 store = r.alloc(0x100), strs = r.alloc(0x10000), data = r.alloc(0x10000);
        u32 sp = strs;
        auto put = [&](const std::string& s) {
            const u32 at = sp;
            r.write(at, s.c_str(), s.size() + 1);
            sp += static_cast<u32>(s.size()) + 1;
            return at;
        };
        if (std::getenv("BANK_PROBE")) {
            for (u32 fn : {0x004DA780u, 0x004D80D0u, 0x004E8890u, 0x004D83C0u}) {
                u32 res = 0;
                const bool ok = r.call(fn, 0, {16}, &res);
                std::printf("probe %08x: %s %08x\n", fn, ok ? "ok" : "FAILED", res);
            }
        }
        bool bad = !r.call(kInitEmpty, store, {});
        if (bad) std::printf("scenario %u: the store set-up failed\n", seed);
        ta::Bank bank;
        bank.name = "Total Annihilation 3.0";
        const int nsec = 1 + static_cast<int>(g() % 4);
        for (int s = 0; s < nsec && !bad; ++s) {
            const std::string sname = s == 0 ? "Summary" : "Section" + std::to_string(s) + "_" + std::to_string(g() % 100);
            bad |= !r.call(kSection, store, {put(sname)});
            ta::BankSection& bs = bank.section(sname);
            const int nkeys = static_cast<int>(g() % 8);
            for (int k = 0; k < nkeys && !bad; ++k) {
                const std::string key = "Key" + std::to_string(k) + (g() % 2 ? " x" : "");
                switch (g() % 3) {
                case 0: {
                    const u32 v = g();
                    bad |= !r.call(kSetInt, store, {put(key), v});
                    bs.set_int(key, static_cast<std::int32_t>(v));
                    break;
                }
                case 1: {
                    const double d = static_cast<double>(g() % 100000) / 7.0 - 3000.0;
                    u32 w[2];
                    std::memcpy(w, &d, 8);
                    bad |= !r.call(kSetDouble, store, {put(key), w[0], w[1]});
                    bs.set_double(key, d);
                    break;
                }
                default: {
                    std::string v;
                    for (int c = static_cast<int>(g() % 20); c > 0; --c) v.push_back(static_cast<char>('a' + g() % 26));
                    bad |= !r.call(kSetString, store, {put(key), put(v)});
                    bs.set_string(key, v);
                    break;
                }
                }
                ++keys;
            }
            const int nblobs = static_cast<int>(g() % 4);
            for (int k = 0; k < nblobs && !bad; ++k) {
                const bool named = g() % 2 != 0;
                const std::string bname = "Blob" + std::to_string(k);
                const std::int32_t index = static_cast<std::int32_t>(g() % 50);
                bool exists = false;
                for (const auto& b : bs.blobs) exists |= named ? (b.named && b.name == bname) : (!b.named && b.index == index);
                if (exists) continue;
                bad |= !r.call(named ? kBlob : kIndexedBlob, store, {named ? put(bname) : static_cast<u32>(index)});
                bs.blobs.push_back(ta::BankSection::Blob{named, named ? bname : std::string(), index, {}});
                for (int piece = 1 + static_cast<int>(g() % 3); piece > 0 && !bad; --piece) {
                    std::vector<std::uint8_t> bytes(g() % 600);
                    for (auto& x : bytes) x = static_cast<std::uint8_t>(g() % 7 == 0 ? g() : 'q');
                    if (!bytes.empty()) r.write(data, bytes.data(), bytes.size());
                    bad |= !r.call(kWriteBytes, store, {data, static_cast<u32>(bytes.size())});
                    bs.blobs.back().data.insert(bs.blobs.back().data.end(), bytes.begin(), bytes.end());
                }
                ++blobs;
            }
        }
        // Sections without keys or blobs are not written.
        std::vector<ta::BankSection> kept;
        for (const auto& s : bank.sections)
            if (!s.ints.empty() || !s.doubles.empty() || !s.strings.empty()) kept.push_back(s);
            else {
                bool any = false;
                for (const auto& b : s.blobs) any |= !b.data.empty();
                if (any) kept.push_back(s);
            }
        bank.sections = kept;
        for (auto& s : bank.sections) {
            std::vector<ta::BankSection::Blob> nonempty;
            for (const auto& b : s.blobs)
                if (!b.data.empty()) nonempty.push_back(b);
            s.blobs = nonempty;
        }
        const std::string pa = dir + "\\bank_a.sav", pb = dir + "\\bank_b.sav";
        const u32 bank_name = put(bank.name);
        u32 res = 0;
        if (!bad) bad |= !r.call(kWriteFile, store, {put(pa), bank_name, 0, 0}, &res) || !res;
        if (!bad) bad |= !r.call(kWriteFile, store, {put(pb), bank_name, 1, 0}, &res) || !res;
        if (bad) {
            std::printf("scenario %u: the original's store or writer failed\n", seed);
            ++mismatches;
            continue;
        }
        const std::vector<std::uint8_t> a = read_file(pa), b = read_file(pb);
        std::vector<std::uint8_t> mine;
        std::vector<std::size_t> stale;
        ta::write_bank(bank, mine, &stale);
        // Header (but the directory offset and its squashed flag) and accounts byte for byte.
        const std::uint32_t dir_a = a.size() >= 0x10 ? static_cast<std::uint32_t>(a[0xC] | a[0xD] << 8 | a[0xE] << 16 | a[0xF] << 24) : 0;
        bool ok = mine.size() >= dir_a && a.size() >= 0x22;
        for (std::size_t i = 0; ok && i < dir_a; ++i) {
            if (i >= 0x0C && i < 0x10) continue;
            if (i == 0x18) continue;
            bool unknown = false;  // a stale stack word of the original's writer
            for (std::size_t at : stale) unknown |= i >= at && i < at + 4;
            if (unknown) continue;
            if (mine[i] != a[i]) {
                // A named blob's descriptor carries a stale word when no numbered blob came before.
                std::printf("scenario %u: stored bank differs at byte %zu (%02x/%02x)\n", seed, i, a[i], mine[i]);
                if (std::getenv("BANK_PROBE")) std::ofstream(dir + "\\bank_mine.sav", std::ios::binary).write(reinterpret_cast<const char*>(mine.data()), static_cast<std::streamsize>(mine.size()));
                ok = false;
            }
        }
        bytes_compared += dir_a;
        squashed += a.size() > 0x18 && a[0x18];
        std::string err, why;
        ta::Bank ra, rb;
        if (ok && (!ta::read_bank(a, ra, err) || !ta::read_bank(b, rb, err))) {
            std::printf("scenario %u: read_bank: %s\n", seed, err.c_str());
            ok = false;
        }
        if (ok && !same_bank(ra, bank, why)) {
            std::printf("scenario %u: the stored bank reads back different (%s)\n", seed, why.c_str());
            ok = false;
        }
        if (ok && !same_bank(rb, bank, why)) {
            std::printf("scenario %u: the squashed bank reads back different (%s)\n", seed, why.c_str());
            ok = false;
        }
        if (!ok) ++mismatches;
    }
    std::printf("scenarios %d: keys %zu, blobs %zu, %zu header+account bytes compared, %zu squashed directories\n", scenarios, keys, blobs,
                bytes_compared, squashed);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
