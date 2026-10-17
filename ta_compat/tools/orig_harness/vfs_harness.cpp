// The game's file system against the original. TotalA.exe is started suspended in its
// game directory and its own routines are called: the archive scan (0x41D4C0, run
// several times as the menus do), the finder (0x4BC4B0 / 0x4BC640), the name lister
// (0x4BCA30), the recursive lister (0x4BCB50), the file opener (0x4BB2E0), the whole-file
// loader (0x4BBE50), the localized path builder (0x4290F0) and the unit file enumeration
// (0x42A8D0). Compared with ta::Vfs over the same directory after every scan:
//   - the mounted archives in order (full paths);
//   - every archive's directory tree as the original holds it in memory: names, order,
//     directory flags, file records and the runtime "shadowed" flags;
//   - every path (all archive entries, all loose files, case/separator variants,
//     directories, missing names): which source and which entry it opens;
//   - the bytes of a sample of files (or all of them);
//   - the finder over the patterns the game uses (and more), result by result
//     (name, attributes, size, source), also single-source searches;
//   - the name lister and the recursive lister;
//   - the unit files the game enumerates, their checksums and names, and the engine's
//     final definition order.
//
// Usage:
//   vfs_harness run <TotalA.exe> [--scans N] [--bytes N|all] [--no-units] [--prune NAME] [--verbose]
//   vfs_harness make <steam game dir> <out dir>      (builds the synthetic test folder)

#define NOMINMAX
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "remote.h"
#include "ta/hpi.h"
#include "ta/tdf.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
namespace fs = std::filesystem;

constexpr u32 kScan = 0x0041D4C0, kFindFirst = 0x004BC4B0, kFindNext = 0x004BC640, kFindClose = 0x004BC8D0,
              kEnumNames = 0x004BCA30, kEnumTree = 0x004BCB50, kOpen = 0x004BB2E0, kClose = 0x004BB5D0, kLoadFile = 0x004BBE50,
              kFree = 0x004D85A0, kVirtualPath = 0x004290F0, kUnitEnum = 0x0042A8D0;
constexpr u32 kDisplayContext = 0x0051FBD0, kGameState = 0x00511DE8, kLanguage = 0x0051FB50;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int g_mismatches = 0;
int g_reported = 0;
void bad(const std::string& what) {
    ++g_mismatches;
    if (++g_reported <= 60) std::printf("  MISMATCH %s\n", what.c_str());
}

// ------------------------------------------------------------------ the original

struct Orig {
    harness::Remote r;
    u32 ctx = 0, gs = 0, strings = 0, fd = 0, scratch = 0;

    bool start(const std::string& exe) {
        if (!r.start(exe)) return false;
        if (!r.install_crash_catcher()) return false;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        const u32 set_error_mode = static_cast<u32>(reinterpret_cast<std::uintptr_t>(GetProcAddress(k32, "SetErrorMode")));
        if (!r.call(set_error_mode, 0, {SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX})) return false;
        // The runtime's fatal-error paths fault instead of showing a message box; then its
        // start-up parts (heap, thread data, I/O, code page, floating point), the C
        // initialisers, the command line and environment (as bank_harness does).
        for (u32 fatal : {0x004E7180u, 0x004F18B0u, 0x004F3D00u}) {
            const std::uint8_t ud2[2] = {0x0F, 0x0B};
            if (!r.patch_jump(fatal, fatal) || !r.write(fatal, ud2, 2)) return false;
        }
        for (u32 init : {0x004F1830u, 0x004EB040u, 0x004F15C0u, 0x004F0C50u, 0x004E4320u})
            if (!r.call(init, 0, {})) return false;
        if (!r.call(0x004E4740, 0, {0x00501050, 0x00501060})) return false;
        const u32 cmd = r.alloc(0x100);
        r.write(cmd, "TotalA.exe", 11);
        r.w32(0x0052B650, cmd);
        u32 env = 0;
        if (!r.call(0x004F1460, 0, {}, &env)) return false;
        r.w32(0x00529F3C, env);
        if (!r.call(0x004F0FB0, 0, {}) || !r.call(0x004F0EC0, 0, {})) return false;
        // The display context holds the archive list (+0x618 array, +0x61C count); the
        // game state is only needed by the unit enumeration (version 3.1 at +1/+2).
        ctx = r.alloc(0x1000);
        gs = r.alloc(0x40000);
        r.w32(kDisplayContext, ctx);
        r.w32(kGameState, gs);
        const std::uint8_t version[2] = {3, 1};
        r.write(gs + 1, version, 2);
        strings = r.alloc(0x10000);
        fd = r.alloc(0x1000);
        scratch = r.alloc(0x1000);
        return true;
    }
    u32 str(const std::string& s, u32 slot = 0) {
        const u32 at = strings + slot * 0x400;
        r.write(at, s.c_str(), s.size() + 1);
        return at;
    }
    std::string read_str(u32 at, std::size_t max = 0x200) {
        // In pieces that never cross a page end (the string may sit at the end of a heap block).
        std::string s;
        while (s.size() < max) {
            const u32 p = at + static_cast<u32>(s.size());
            const std::size_t n = std::min<std::size_t>(max - s.size(), 0x1000 - (p & 0xFFF));
            std::vector<char> b(n, 0);
            if (!r.read(p, b.data(), n)) break;
            for (char c : b) {
                if (!c) return s;
                s.push_back(c);
            }
        }
        return s;
    }
    bool call(u32 fn, const std::vector<u32>& args, u32* res = nullptr, DWORD timeout = 20000) {
        if (r.call(fn, 0, args, res, timeout)) return true;
        const auto c = r.crash_info();
        std::printf("  the original's %08x failed (exception %08x at %08x)\n", fn, c.code, c.address);
        return false;
    }

    struct Archive {
        u32 ptr = 0;
        std::string path;
        u32 blob = 0;
        std::vector<std::uint8_t> dir;
    };
    std::vector<Archive> archives() {
        std::vector<Archive> out;
        const u32 n = r.r32(ctx + 0x61C), arr = r.r32(ctx + 0x618);
        for (u32 i = 0; i < n; ++i) {
            Archive a;
            a.ptr = r.r32(arr + 4 * i);
            a.path = read_str(a.ptr + 0x14, 0x104);
            a.blob = r.r32(a.ptr + 8);
            const u32 size = r.r32(a.blob + 8);
            a.dir.resize(size);
            r.read(a.blob, a.dir.data(), size);
            out.push_back(std::move(a));
        }
        return out;
    }

    std::vector<ta::VfsFound> find(const std::string& pattern, int source, bool all) {
        std::vector<ta::VfsFound> out;
        u32 h = 0;
        if (!call(kFindFirst, {str(pattern), fd, static_cast<u32>(source), all ? 1u : 0u}, &h)) return out;
        if (h == 0xFFFFFFFFu) return out;
        for (;;) {
            ta::VfsFound f;
            f.attrib = r.r32(fd);
            f.size = r.r32(fd + 0x10);
            f.name = read_str(fd + 0x14, 260);
            f.source = static_cast<int>(r.r32(h + 0x200));
            out.push_back(f);
            u32 res = 0;
            if (!call(kFindNext, {h, fd}, &res) || res != 0) break;
        }
        call(kFindClose, {h});
        return out;
    }
    std::vector<std::string> read_vector(u32 vec) {
        std::vector<std::string> out;
        const u32 b = r.r32(vec + 4), e = r.r32(vec + 8);
        for (u32 p = b; p < e; p += 4) out.push_back(read_str(r.r32(p), 300));
        return out;
    }
    std::vector<std::string> enumerate(const std::string& pattern, bool dirs_only) {
        r.write(scratch, std::vector<std::uint8_t>(16, 0).data(), 16);
        if (!call(kEnumNames, {str(pattern), dirs_only ? 1u : 0u, scratch})) return {};
        return read_vector(scratch);
    }
    std::vector<std::string> enumerate_tree(const std::string& dir, const std::string& pattern) {
        r.write(scratch, std::vector<std::uint8_t>(16, 0).data(), 16);
        if (!call(kEnumTree, {str(dir), str(pattern, 1), scratch, 0xFFFFFFFFu, 1})) return {};
        return read_vector(scratch);
    }
    // Opens a path the way the game does: source (-1 disk, archive index, -2 none) and the record.
    struct Opened {
        int source = -2;
        u32 offset = 0, size = 0;
        std::uint8_t compression = 0;
    };
    Opened open(const std::string& path, const std::vector<Archive>& list) {
        Opened o;
        u32 h = 0;
        if (!call(kOpen, {str(path), str("rb", 1)}, &h) || !h) return o;
        const u32 arch = r.r32(h + 4);
        if (!arch) {
            o.source = -1;
        } else {
            o.source = -3;
            for (std::size_t k = 0; k < list.size(); ++k)
                if (list[k].ptr == arch) o.source = static_cast<int>(k);
            const u32 rec = r.r32(h + 8);
            o.offset = r.r32(rec);
            o.size = r.r32(rec + 4);
            o.compression = r.rd<std::uint8_t>(rec + 8);
        }
        call(kClose, {h});
        return o;
    }
    bool load(const std::string& path, std::vector<std::uint8_t>& out) {
        out.clear();
        u32 buf = 0;
        if (!call(kLoadFile, {str(path), scratch}, &buf, 60000) || !buf) return false;
        const u32 n = r.r32(scratch);
        out.resize(n);
        if (n) r.read(buf, out.data(), n);
        call(kFree, {buf});
        return true;
    }
    std::string virtual_path(const std::string& dir, const std::string& name, const std::string& ext) {
        const u32 out = scratch + 0x100;
        if (!call(kVirtualPath, {out, str(dir), str(name, 1), ext.empty() ? 0u : str(ext, 2)})) return "?";
        return read_str(out, 0x100);
    }
};

// ------------------------------------------------------------------ comparisons

// Walks the original's in-memory tree (pointers relocated to absolute addresses) beside the engine's.
void compare_tree(const Orig::Archive& oa, const ta::HpiEntry& mine, u32 node_abs, const std::string& where, int depth,
                  std::size_t& entries, std::size_t& shadowed) {
    auto at = [&](u32 abs) -> std::size_t { return abs - oa.blob; };
    auto rd32 = [&](std::size_t off) -> u32 {
        if (off + 4 > oa.dir.size()) return 0;
        u32 v;
        std::memcpy(&v, &oa.dir[off], 4);
        return v;
    };
    if (depth > 64) return;
    const std::size_t n = at(node_abs);
    const u32 count = rd32(n), entries_abs = rd32(n + 4);
    if (count != mine.children.size()) {
        bad(where + ": " + std::to_string(count) + " entries in the original, " + std::to_string(mine.children.size()) + " in ours");
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        const std::size_t e = at(entries_abs) + 9u * i;
        const ta::HpiEntry& m = mine.children[i];
        const std::size_t name_off = at(rd32(e));
        std::string name;
        for (std::size_t k = name_off; k < oa.dir.size() && oa.dir[k]; ++k) name.push_back(static_cast<char>(oa.dir[k]));
        const std::uint8_t flags = e + 8 < oa.dir.size() ? oa.dir[e + 8] : 0;
        const std::string path = where + "\\" + name;
        ++entries;
        if (name != m.name) bad(where + " entry " + std::to_string(i) + ": " + name + " / " + m.name);
        if (((flags & 1) != 0) != m.is_dir) bad(path + ": directory flag");
        if (((flags & 2) != 0) != m.shadowed)
            bad(path + ": shadowed " + std::to_string((flags >> 1) & 1) + " in the original, " + std::to_string(m.shadowed) + " in ours");
        shadowed += (flags & 2) ? 1 : 0;
        if (m.is_dir) {
            compare_tree(oa, m, rd32(e + 4), path, depth + 1, entries, shadowed);
        } else {
            const std::size_t rec = at(rd32(e + 4));
            if (rd32(rec) != m.data_offset || rd32(rec + 4) != m.size || (rec + 8 < oa.dir.size() && oa.dir[rec + 8] != m.compression))
                bad(path + ": file record");
        }
    }
}

void collect_paths(const ta::HpiEntry& node, const std::string& prefix, std::set<std::string>& seen, std::vector<std::string>& out) {
    for (const auto& e : node.children) {
        const std::string p = prefix.empty() ? e.name : prefix + "\\" + e.name;
        if (seen.insert(lower(p)).second) out.push_back(p);
        if (e.is_dir) collect_paths(e, p, seen, out);
    }
}

std::string show(const ta::VfsFound& f) {
    char b[64];
    std::snprintf(b, sizeof b, " [attr %x size %u src %d]", f.attrib, f.size, f.source);
    return f.name + b;
}

void compare_find(Orig& o, ta::Vfs& vfs, const std::string& pattern, int source, bool all, std::size_t& results) {
    const auto a = o.find(pattern, source, all);
    const auto b = vfs.find_files(pattern, source, all);
    results += a.size();
    const std::string tag = "find(" + pattern + ", " + std::to_string(source) + ", " + (all ? "all" : "one") + ")";
    if (a.size() != b.size()) bad(tag + ": " + std::to_string(a.size()) + " results in the original, " + std::to_string(b.size()) + " in ours");
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i].name != b[i].name || a[i].attrib != b[i].attrib || a[i].size != b[i].size || a[i].source != b[i].source) {
            bad(tag + " result " + std::to_string(i) + ": " + show(a[i]) + " / " + show(b[i]));
            break;
        }
    }
}

void compare_list(const std::string& tag, const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) bad(tag + ": " + std::to_string(a.size()) + " names in the original, " + std::to_string(b.size()) + " in ours");
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (a[i] != b[i]) {
            bad(tag + " item " + std::to_string(i) + ": " + a[i] + " / " + b[i]);
            break;
        }
}

struct Options {
    std::string exe;
    int scans = 2;
    long bytes = 200;  // -1 = all
    bool units = true;
    std::string prune;  // archive file renamed away before the last scan
    bool verbose = false;
};

// --verbose: the mount order and what a few probe paths resolve to (the original's answers).
void describe(Orig& o, const std::vector<Orig::Archive>& list) {
    std::printf("  mounted:");
    for (const auto& a : list) std::printf(" %s", fs::path(a.path).filename().string().c_str());
    std::printf("\n");
    for (const char* p : {"test\\common.txt", "test\\gp3_vs_ccx.txt", "test\\ccx_order.txt", "test\\ccx_vs_ufo.txt", "test\\case.txt", "test\\dup.txt",
                          "test\\file_vs_dir", "test\\file_vs_dir\\inner.txt", "test\\fd2", "test\\slash/name.txt", "test\\slash\\name.txt", "test\\late.txt",
                          "test\\hpix.txt", "test\\sub\\arch.txt", "gamedata\\order.tdf"}) {
        const auto r = o.open(p, list);
        std::vector<std::uint8_t> b;
        std::string text = "-";
        if (r.source != -2 && o.load(p, b)) text.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(b.size(), 40)));
        std::printf("    %-30s -> %-12s \"%s\"\n", p,
                    r.source == -1 ? "loose" : r.source < 0 ? "missing" : fs::path(list[static_cast<std::size_t>(r.source)].path).filename().string().c_str(),
                    text.c_str());
    }
    for (const char* pat : {"test\\*", "test\\sub\\*"}) {
        std::printf("    find %s:", pat);
        for (const auto& f : o.find(pat, -1, true))
            std::printf(" %s(%s)", f.name.c_str(), f.source < 0 ? "disk" : fs::path(list[static_cast<std::size_t>(f.source)].path).filename().string().c_str());
        std::printf("\n");
    }
}

void compare_state(Orig& o, ta::Vfs& vfs, const fs::path& game, const Options& opt, int scan_no) {
    const int before = g_mismatches;
    // Mounted archives.
    const auto list = o.archives();
    std::printf("scan %d: %zu archives mounted by the original, %zu by ours\n", scan_no, list.size(), vfs.archives().size());
    if (opt.verbose) describe(o, list);
    if (list.size() != vfs.archives().size()) bad("archive count");
    for (std::size_t k = 0; k < std::min(list.size(), vfs.archives().size()); ++k) {
        const std::string mine = fs::absolute(vfs.archives()[k]->path()).string();
        if (lower(list[k].path) != lower(mine)) bad("archive " + std::to_string(k) + ": " + list[k].path + " / " + mine);
    }
    // Trees with the shadow flags.
    std::size_t entries = 0, shadowed = 0;
    for (std::size_t k = 0; k < std::min(list.size(), vfs.archives().size()); ++k) {
        u32 root;
        std::memcpy(&root, &list[k].dir[0x10], 4);
        compare_tree(list[k], vfs.archives()[k]->root(), root, fs::path(list[k].path).filename().string(), 0, entries, shadowed);
    }
    std::printf("  trees: %zu entries compared, %zu shadowed\n", entries, shadowed);

    // Lookups.
    std::vector<std::string> paths;
    std::set<std::string> seen;
    for (const auto& a : vfs.archives()) collect_paths(a->root(), "", seen, paths);
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(game, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = fs::relative(it->path(), game, ec).string();
        std::replace(rel.begin(), rel.end(), '/', '\\');
        if (seen.insert(lower(rel)).second) paths.push_back(rel);
    }
    const std::size_t base = paths.size();
    for (std::size_t i = 0; i < base; i += 37) {
        std::string up = paths[i], lo = lower(paths[i]), sl = paths[i];
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::replace(sl.begin(), sl.end(), '\\', '/');
        for (const std::string& v : {up, lo, sl, paths[i] + "\\", ".\\" + paths[i], paths[i] + ".missing"}) paths.push_back(v);
    }
    for (const char* extra : {"", "\\", "units", "UNITS\\", "nothing\\here.txt", "units\\\\ARMCOM.FBI", "units/ARMCOM.FBI", "test\\slash/name.txt",
                              "TEST\\SLASH/NAME.TXT", "test/slash/name.txt", "test\\file_vs_dir", "test\\file_vs_dir\\inner.txt", "test\\dup.txt", "test\\fd2", "test\\fd2\\x.txt"})
        paths.push_back(extra);
    std::size_t disk = 0, archive = 0, none = 0;
    for (const std::string& p : paths) {
        const auto a = o.open(p, list);
        const auto l = vfs.locate(p);
        if (a.source == -1) ++disk;
        else if (a.source >= 0) ++archive;
        else ++none;
        if (a.source != l.source) {
            bad("open " + p + ": source " + std::to_string(a.source) + " in the original, " + std::to_string(l.source) + " in ours");
            continue;
        }
        if (a.source >= 0 && (a.offset != l.entry->data_offset || a.size != l.entry->size || a.compression != l.entry->compression))
            bad("open " + p + ": a different entry");
    }
    std::printf("  lookups: %zu paths (%zu loose, %zu in archives, %zu missing)\n", paths.size(), disk, archive, none);

    // Bytes.
    std::vector<std::string> files;
    for (const std::string& p : paths) {
        const auto l = vfs.locate(p);
        if (l) files.push_back(p);
    }
    std::size_t compared = 0, total = 0;
    const std::size_t step = opt.bytes < 0 || static_cast<std::size_t>(opt.bytes) >= files.size() ? 1 : files.size() / static_cast<std::size_t>(std::max(1L, opt.bytes));
    for (std::size_t i = 0; i < files.size(); i += step) {
        std::vector<std::uint8_t> a, b;
        const bool oa = o.load(files[i], a);
        const bool ob = vfs.read(files[i], b);
        if (oa && (!ob || a != b)) bad("bytes of " + files[i]);
        if (!oa && ob && !b.empty()) bad("the original could not load " + files[i]);
        ++compared;
        total += a.size();
    }
    std::printf("  bytes: %zu files, %zu bytes identical unless reported\n", compared, total);

    // Finder.
    std::size_t results = 0, patterns = 0;
    const std::vector<std::string> finds = {
        "rev31.GP3", "*.CCX", "*.UFO", "*.HPI", "*", "*.*", "*.hpi", "Maps\\*.ota", "maps\\*.tnt", "Weapons\\*.tdf", "units\\*.FBI", "units\\*",
        "download\\*.TDF", "textures\\*.GAF", "camps\\*.TDF", "camps\\*", "camps\\briefs\\*", "guis\\*.GUI", "anims\\*.gaf", "scripts\\*.cob",
        "sounds\\*.wav", "objects3d\\*.3do", "unitpics\\*.pcx", "gamedata\\*", "features\\*", "features\\corpses\\*.tdf", "bitmaps\\*", "palettes\\*",
        "music\\*", "Data\\*.zrb", "*.sav", "nothing\\*", "MAPS\\*.OTA", "units\\ARM*.FBI", "units\\?rm*.fbi", "test\\*", "test\\*.*", "test\\sub\\*",
        "TEST\\*.TXT", "test\\file_vs_dir\\*", "test\\file_vs_dir", "units-english\\*", "sections\\*", "install\\*", "*\\*", "units\\*.*",
    };
    for (const std::string& f : finds) {
        compare_find(o, vfs, f, -1, true, results);
        ++patterns;
    }
    // Single sources (and a source past the end), as the recursive walks use them.
    for (int s = 0; s <= static_cast<int>(list.size()); s += std::max<int>(1, static_cast<int>(list.size()) / 6)) {
        for (const std::string& f : {std::string("units\\*"), std::string("test\\*"), std::string("*"), std::string("maps\\*.ota")}) {
            compare_find(o, vfs, f, s, false, results);
            compare_find(o, vfs, f, s, true, results);
            patterns += 2;
        }
    }
    compare_find(o, vfs, "*", -1, false, results);
    compare_find(o, vfs, "nothing\\*", -1, false, results);
    patterns += 2;
    std::printf("  finder: %zu searches, %zu results\n", patterns, results);

    // Name lister (the game's callers) and recursive lister.
    std::size_t names = 0;
    for (const std::string& f : {std::string("Maps\\*.ota"), std::string("Weapons\\*.tdf"), std::string("units\\*.FBI"), std::string("download\\*.TDF"),
                                 std::string("textures\\*.GAF"), std::string("camps\\*.TDF"), std::string("guis\\*"), std::string("*"),
                                 std::string("test\\*"), std::string("units\\*")}) {
        for (bool dirs : {false, true}) {
            const auto a = o.enumerate(f, dirs);
            compare_list("enumerate(" + f + (dirs ? ", dirs" : "") + ")", a, vfs.enumerate(f, dirs));
            names += a.size();
        }
    }
    for (const auto& [d, p] : std::vector<std::pair<std::string, std::string>>{
             {"features", "*.tdf"}, {"camps", "*.tdf"}, {"maps", "*.ota"}, {"test", "*"}, {"units", "*.fbi"}, {"anims", "*.gaf"}, {"sounds", "*"}}) {
        const auto a = o.enumerate_tree(d, p);
        compare_list("enumerate_tree(" + d + ", " + p + ")", a, vfs.enumerate_tree(d, p));
        names += a.size();
    }
    std::printf("  listers: %zu names\n", names);

    // Localized paths (the language set as the game sets it, "english" by default).
    o.r.write(kLanguage, "english", 8);
    for (const auto& [d, n, e] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"units", "LOC", "FBI"}, {"units", "ARMCOM", "FBI"}, {"units", "*", "FBI"}, {"guis", "ARMCOM1.GUI", "GUI"}, {"test", "loc.txt", ""},
             {"test", "x.y.z", "q"}, {"units", "NOLOC", "FBI"}}) {
        const std::string a = o.virtual_path(d, n, e);
        const std::string b = vfs.virtual_path(d, n, e);
        if (a != b) bad("virtual path " + d + "/" + n + ": " + a + " / " + b);
    }
    std::printf("  scan %d: %d mismatches\n", scan_no, g_mismatches - before);
}

void compare_units(Orig& o, ta::Vfs& vfs) {
    const int before = g_mismatches;
    if (!o.call(kUnitEnum, {}, nullptr, 120000)) {
        bad("the original's unit enumeration failed");
        return;
    }
    const u32 count = o.r.r32(o.gs + 0x1438F), table = o.r.r32(o.gs + 0x1439B);
    struct Rec {
        std::string name, unitname;
        u32 checksum;
        std::uint16_t index;
    };
    std::vector<Rec> orig;
    for (u32 i = 1; i < count; ++i) {
        const u32 at = table + i * 0x249;
        Rec r;
        r.name = o.read_str(at, 0x20);
        r.unitname = o.read_str(at + 0x20, 0x20);
        r.checksum = o.r.r32(at + 0x13E);
        r.index = o.r.rd<std::uint16_t>(at + 0x21E);
        orig.push_back(r);
    }
    // Ours: the enumeration the unit loader runs, then the loader's own table.
    const auto files = vfs.enumerate("units\\*.FBI");
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable table_mine = ta::load_unit_defs(vfs, ctx);
    std::map<std::string, const ta::UnitDefRecord*> by_file;
    for (const auto& u : table_mine.units) by_file[lower(u.source_file)] = &u;
    std::printf("units: %zu enumerated by the original (%u records), %zu files enumerated by ours, %zu definitions\n", orig.size(), count,
                files.size(), table_mine.units.size() - 1);
    if (orig.size() != files.size()) bad("unit file count");
    for (std::size_t i = 0; i < std::min(orig.size(), files.size()); ++i) {
        const auto it = by_file.find(lower(files[i]));
        if (it == by_file.end()) {
            bad("unit file " + files[i] + " has no definition in ours");
            continue;
        }
        if (orig[i].unitname != it->second->unit_name || orig[i].checksum != it->second->fbi_checksum || orig[i].index != i + 1)
            bad("unit " + std::to_string(i + 1) + ": " + orig[i].unitname + " / " + it->second->unit_name);
    }
    // Definition indices: the game sorts the records by unit name (stricmp) after this.
    std::vector<std::string> sorted;
    for (const auto& r : orig) sorted.push_back(r.unitname);
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    std::vector<std::string> mine;
    for (std::size_t i = 1; i < table_mine.units.size(); ++i) mine.push_back(table_mine.units[i].unit_name);
    compare_list("definition order", sorted, mine);
    std::printf("  units: %d mismatches\n", g_mismatches - before);
}

int run(const Options& opt) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const fs::path game = fs::path(opt.exe).parent_path();
    Orig o;
    if (!o.start(opt.exe)) {
        std::printf("cannot start %s\n", opt.exe.c_str());
        return 1;
    }
    ta::Vfs vfs;
    for (int s = 1; s <= opt.scans; ++s) {
        if (s == opt.scans && !opt.prune.empty()) {
            fs::rename(game / opt.prune, game / (opt.prune + ".away"));
            std::printf("(%s renamed away before scan %d)\n", opt.prune.c_str(), s);
        }
        if (!o.call(kScan, {}, nullptr, 120000)) return 1;
        if (s == 1) vfs.mount_game_dir(game);
        else vfs.scan();
        compare_state(o, vfs, game, opt, s);
        if (opt.units && (s == 1 || s == opt.scans)) compare_units(o, vfs);
        if (s == opt.scans && !opt.prune.empty()) fs::rename(game / (opt.prune + ".away"), game / opt.prune);
    }
    o.r.stop();
    std::printf("%d mismatches\n", g_mismatches);
    return g_mismatches ? 1 : 0;
}

// ------------------------------------------------------------------ synthetic folder

std::vector<std::uint8_t> bytes_of(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

ta::HpiWriteFile text(const std::string& path, const std::string& content, std::uint8_t compression = 0, bool scramble = false) {
    ta::HpiWriteFile f;
    f.path = path;
    f.data = bytes_of(content);
    f.compression = compression;
    f.scramble_chunks = scramble;
    return f;
}

void write_file(const fs::path& p, const std::vector<std::uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

int make(const fs::path& steam, const fs::path& out) {
    fs::create_directories(out);
    for (const auto& de : fs::directory_iterator(steam)) {
        const std::string ext = lower(de.path().extension().string());
        if (ext == ".exe" || ext == ".dll") fs::copy_file(de.path(), out / de.path().filename(), fs::copy_options::overwrite_existing);
    }
    // Real compressed entries (LZ77 and zlib chunks) copied from the Steam archives.
    std::vector<ta::HpiWriteFile> copied;
    for (const char* name : {"totala1.hpi", "ccdata.ccx"}) {
        ta::HpiError err;
        auto a = ta::HpiArchive::open(steam / name, err);
        if (!a) continue;
        int taken = 0;
        std::vector<const ta::HpiEntry*> stack{&a->root()};
        std::vector<std::string> prefix{""};
        while (!stack.empty() && taken < 6) {
            const ta::HpiEntry* n = stack.back();
            const std::string pre = prefix.back();
            stack.pop_back();
            prefix.pop_back();
            for (const auto& e : n->children) {
                const std::string p = pre.empty() ? e.name : pre + "\\" + e.name;
                if (e.is_dir) {
                    stack.push_back(&e);
                    prefix.push_back(p);
                } else if (e.compression && e.size < 200000 && taken < 6) {
                    ta::HpiWriteFile f;
                    f.path = "copied\\" + std::string(name) + "_" + e.name;
                    f.copy_stored = true;
                    f.stored_size = e.size;
                    f.stored_compression = e.compression;
                    if (a->read_stored(e, f.stored) == ta::HpiError::None) {
                        copied.push_back(f);
                        ++taken;
                    }
                }
            }
        }
    }
    std::string big;
    for (int i = 0; i < 9000; ++i) big += "line " + std::to_string(i % 97) + " of the synthetic file system test\n";

    auto archive = [&](const std::string& name, std::vector<ta::HpiWriteFile> files, std::uint8_t key) {
        write_file(out / name, ta::hpi_build(files, key));
    };
    // rev31.GP3: first source.
    archive("rev31.GP3",
            {text("test\\common.txt", "gp3"), text("test\\gp3_vs_ccx.txt", "gp3"), text("gamedata\\order.tdf", "gp3"),
             text("test\\case.TXT", "gp3 upper"), text("units-english\\LOC.FBI", "localized")},
            0x00);
    // CCX (name order: ALPHA before ZED; both kept open).
    archive("Zed.ccx", {text("test\\common.txt", "zed"), text("test\\ccx_order.txt", "zed"), text("test\\gp3_vs_ccx.txt", "zed")}, 0x7D);
    archive("alpha.ccx",
            {text("test\\common.txt", "alpha"), text("test\\ccx_order.txt", "alpha"), text("test\\ccx_vs_ufo.txt", "alpha"),
             text("TEST\\Case.txt", "alpha lower"), text("test\\big_lz77.txt", big, 1), text("test\\big_zlib.txt", big, 2, true)},
            0xA5);
    // UFOs; one root entry looks like an archive name.
    archive("m.ufo", {text("test\\common.txt", "m"), text("test\\ccx_vs_ufo.txt", "m"), text("inner.ufo", "not an archive"), text("test\\ufo_only.txt", "m")},
            0x01);
    archive("N.ufo", {text("test\\ufo_only.txt", "N"), text("test\\sub\\arch.txt", "N"), text("units\\ZZZ.FBI", "N")}, 0xFF);
    // HPIs: a broken one first (does not count against the ten), twelve good ones.
    write_file(out / "bad.hpi", bytes_of("HAPI not really an archive"));
    fs::create_directories(out / "dir.hpi");
    for (int i = 1; i <= 12; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "h%02d.hpi", i);
        std::vector<ta::HpiWriteFile> f{text("test\\common.txt", nm), text(std::string("test\\only_") + nm + ".txt", nm)};
        if (i == 1) {
            f.push_back(text("test\\file_vs_dir\\inner.txt", "h01 dir"));
            f.push_back(text("test\\fd2\\x.txt", "h01 dir"));
            f.push_back(text("test\\dup.txt", "h01 first", 1));
            f.push_back(text("test\\dup.txt", "h01 second", 2));
            f.push_back(text("test\\slash/name.txt", "slash in a name"));
            f.push_back(text("h05.hpi", "a root entry named like a mounted archive"));
            for (const auto& c : copied) f.push_back(c);
        }
        if (i == 2) {
            f.push_back(text("test\\file_vs_dir", "h02 file"));
            f.push_back(text("test\\dup.txt", "h02"));
            f.push_back(text("Test\\Sub\\ARCH.TXT", "h02"));
        }
        if (i == 4) f.push_back(text("test\\fd2", "h04 file"));
        if (i == 11) f.push_back(text("test\\late.txt", "only after the second scan"));
        if (i == 12) f.push_back(text("units-english\\NOLOC.FBI", "h12"));
        archive(nm, f, static_cast<std::uint8_t>(i * 19));
    }
    archive("x.hpix", {text("test\\hpix.txt", "8.3 name test")}, 0);
    // Loose files.
    write_file(out / "test" / "common.txt", bytes_of("loose"));
    write_file(out / "test" / "sub" / "loose.txt", bytes_of("loose sub"));
    write_file(out / "test" / "LOOSE_ONLY.txt", bytes_of("loose only"));
    write_file(out / "test" / "slash" / "name.txt", bytes_of("loose slash dir"));
    write_file(out / "gamedata" / "order.tdf", bytes_of("loose"));
    write_file(out / "test" / "loc.txt", bytes_of("loose loc"));
    std::printf("synthetic game folder written to %s (%zu copied compressed entries)\n", out.string().c_str(), copied.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "make") return make(argv[2], argv[3]);
    if (argc < 3 || std::string(argv[1]) != "run") {
        std::fprintf(stderr,
                     "usage: vfs_harness run <TotalA.exe> [--scans N] [--bytes N|all] [--no-units] [--prune NAME]\n"
                     "       vfs_harness make <steam game dir> <out dir>\n");
        return 2;
    }
    Options opt;
    opt.exe = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scans" && i + 1 < argc) opt.scans = std::atoi(argv[++i]);
        else if (a == "--bytes" && i + 1 < argc) {
            const std::string v = argv[++i];
            opt.bytes = v == "all" ? -1 : std::atol(v.c_str());
        } else if (a == "--no-units") opt.units = false;
        else if (a == "--prune" && i + 1 < argc) opt.prune = argv[++i];
        else if (a == "--verbose") opt.verbose = true;
    }
    return run(opt);
}
