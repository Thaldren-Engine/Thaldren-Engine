// Differential tester for the unit script VM. Starts the original TotalA.exe
// suspended, builds script objects in its memory with logging callbacks, and
// drives the original interpreter (thread start, calls, ticks) with every
// scripts\*.cob of the game. After each operation the original's thread
// block, piece animation table, statics, callback log and battle RNG state are
// compared with our sim::ScriptInstance driven identically.
//
// Usage: cob_harness <TotalA.exe> <game_dir> [ticks_per_script] [only_script]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sim/rng.h"
#include "sim/script_vm.h"
#include "ta/cob.h"
#include "ta/hpi.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

// Original routines (thiscall unless noted).
constexpr u32 kCallNamed = 0x004B0940;       // (name, sink, run_now)
constexpr u32 kInvokeByIndex = 0x004B0B00;   // (index, sink, run_now, argc, a, b, c, d)
constexpr u32 kCallInOut = 0x004B0C40;       // (index, &a, &b, &c, &d)
constexpr u32 kTick = 0x004B0D60;            // (ticks)
constexpr u32 kBattleRngState = 0x0051FC88;

constexpr u32 kObjectSize = 0x544;
constexpr int kSinkSlot = 99;

// Callback table of the original script object: (slot byte offset / 4) -> argument count.
constexpr int kVtableSlots = 20;
constexpr int kSlotArgs[kVtableSlots] = {3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 3, 1, 2, 2, 3, 1, 2, 5, 1, 0};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
};

// thiscall callback: log {slot, args...} at *log_ptr, then return *ret_ptr++.
void emit_callback(Emitter& e, int slot, int nargs, u32 log_ptr_addr, u32 ret_ptr_addr) {
    e.u8(0xA1); e.u32le(log_ptr_addr);              // mov eax,[log_ptr]
    e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(slot));  // mov dword [eax],slot
    for (int i = 0; i < nargs; ++i) {
        e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(static_cast<std::uint8_t>(4 + 4 * i));  // mov edx,[esp+4+4i]
        e.u8(0x89); e.u8(0x50); e.u8(static_cast<std::uint8_t>(4 + 4 * i));              // mov [eax+4+4i],edx
    }
    e.u8(0x83); e.u8(0x05); e.u32le(log_ptr_addr); e.u8(static_cast<std::uint8_t>(4 * (nargs + 1)));  // add [log_ptr],n
    e.u8(0xA1); e.u32le(ret_ptr_addr);              // mov eax,[ret_ptr]
    e.u8(0x8B); e.u8(0x00);                         // mov eax,[eax]
    e.u8(0x83); e.u8(0x05); e.u32le(ret_ptr_addr); e.u8(0x04);  // add [ret_ptr],4
    if (nargs == 0) {
        e.u8(0xC3);
    } else {
        e.u8(0xC2); e.u8(static_cast<std::uint8_t>(4 * nargs)); e.u8(0x00);  // ret 4n
    }
}

// Thread procedure: block = {fn, this, nargs, args[8], result}; calls fn as thiscall.
const std::uint8_t kTrampoline[] = {
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57,  // push ebp; mov ebp,esp; push ebx; push esi; push edi
    0x8B, 0x5D, 0x08,                    // mov ebx,[ebp+8]
    0x8B, 0x4B, 0x08,                    // mov ecx,[ebx+8]
    0x8D, 0x73, 0x0C,                    // lea esi,[ebx+12]
    0x85, 0xC9,                          // loop: test ecx,ecx
    0x74, 0x06,                          // jz call
    0x49,                                // dec ecx
    0xFF, 0x34, 0x8E,                    // push dword [esi+ecx*4]
    0xEB, 0xF6,                          // jmp loop
    0x8B, 0x4B, 0x04,                    // call: mov ecx,[ebx+4]
    0xFF, 0x13,                          // call dword [ebx]
    0x89, 0x43, 0x2C,                    // mov [ebx+0x2C],eax
    0x5F, 0x5E, 0x5B, 0x5D,              // pop edi; pop esi; pop ebx; pop ebp
    0x33, 0xC0,                          // xor eax,eax
    0xC2, 0x04, 0x00,                    // ret 4
};

class Remote {
public:
    bool start(const std::string& exe, const std::string& dir) {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::string cmd = "\"" + exe + "\"";
        if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi_))
            return false;
        code_ = alloc(0x2000, PAGE_EXECUTE_READWRITE);
        data_ = alloc(kDataSize, PAGE_READWRITE);
        if (!code_ || !data_) return false;
        log_ptr_ = data_;
        ret_ptr_ = data_ + 4;
        log_base_ = data_ + 0x1000;
        ret_base_ = log_base_ + kLogSize;
        work_ = ret_base_ + kRetSize;
        Emitter e;
        e.b.assign(kTrampoline, kTrampoline + sizeof kTrampoline);
        while (e.b.size() % 16) e.u8(0xCC);
        for (int s = 0; s < kVtableSlots; ++s) {
            stub_[s] = code_ + static_cast<u32>(e.b.size());
            emit_callback(e, s, kSlotArgs[s], log_ptr_, ret_ptr_);
            while (e.b.size() % 16) e.u8(0xCC);
        }
        sink_stub_ = code_ + static_cast<u32>(e.b.size());
        emit_callback(e, kSinkSlot, 1, log_ptr_, ret_ptr_);
        return write(code_, e.b.data(), e.b.size());
    }
    void stop() {
        if (pi_.hProcess) {
            TerminateProcess(pi_.hProcess, 0);
            CloseHandle(pi_.hThread);
            CloseHandle(pi_.hProcess);
        }
        pi_ = {};
    }
    bool write(u32 at, const void* p, std::size_t n) {
        SIZE_T k = 0;
        return WriteProcessMemory(pi_.hProcess, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at)), p, n, &k) && k == n;
    }
    bool read(u32 at, void* p, std::size_t n) {
        SIZE_T k = 0;
        return ReadProcessMemory(pi_.hProcess, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), p, n, &k) && k == n;
    }
    bool w32(u32 at, u32 v) { return write(at, &v, 4); }
    u32 r32(u32 at) {
        u32 v = 0;
        read(at, &v, 4);
        return v;
    }
    // Calls fn(this, args...) in the child; false on failure or timeout.
    bool call(u32 fn, u32 self, const std::vector<u32>& args, u32* result = nullptr) {
        u32 block[12] = {fn, self, static_cast<u32>(args.size())};
        for (std::size_t i = 0; i < args.size(); ++i) block[3 + i] = args[i];
        const u32 at = work_ + kCallBlock;
        if (!write(at, block, sizeof block)) return false;
        HANDLE th = CreateRemoteThread(pi_.hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<std::uintptr_t>(code_)),
                                       reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at)), 0, nullptr);
        if (!th) return false;
        const DWORD w = WaitForSingleObject(th, 3000);
        CloseHandle(th);
        if (w != WAIT_OBJECT_0) return false;
        if (result) *result = r32(at + 0x2C);
        return true;
    }

    static constexpr u32 kDataSize = 0x01000000;
    static constexpr u32 kLogSize = 0x00400000, kRetSize = 0x00400000;
    // Work area layout.
    static constexpr u32 kObject = 0x0000, kVtable = 0x0600, kSinkObj = 0x0680, kSinkVtbl = 0x0690, kCallBlock = 0x06C0,
                         kOutInts = 0x0700, kNames = 0x0800, kStatics = 0x1000, kAnims = 0x2000, kCob = 0x20000;

    PROCESS_INFORMATION pi_{};
    u32 code_ = 0, data_ = 0, log_ptr_ = 0, ret_ptr_ = 0, log_base_ = 0, ret_base_ = 0, work_ = 0;
    u32 stub_[kVtableSlots] = {};
    u32 sink_stub_ = 0;

private:
    u32 alloc(SIZE_T n, DWORD prot) {
        return static_cast<u32>(reinterpret_cast<std::uintptr_t>(
            VirtualAllocEx(pi_.hProcess, nullptr, n, MEM_RESERVE | MEM_COMMIT, prot)));
    }
};

// Our side of the same callbacks.
struct LogHost : sim::ScriptHost, sim::ScriptReturnSink {
    std::vector<u32> log;
    const std::vector<u32>* ret = nullptr;
    std::size_t ret_at = 0;
    sim::BattleRng rng;

    bool overflow = false;  // more callbacks in one scenario than the shared buffers hold
    i32 next() {
        if (ret_at >= ret->size() || log.size() > Remote::kLogSize / 4 - 64) {
            overflow = true;
            return 0;
        }
        return static_cast<i32>((*ret)[ret_at++]);
    }
    i32 rec(int slot, std::initializer_list<i32> args) {
        log.push_back(static_cast<u32>(slot));
        for (i32 a : args) log.push_back(static_cast<u32>(a));
        return next();
    }
    void set_piece_move(i32 p, i32 a, i32 v) override { rec(0, {p, a, v}); }
    void set_piece_turn(i32 p, i32 a, i32 v) override { rec(1, {p, a, v}); }
    void set_piece_visible(i32 p, i32 on) override { rec(2, {p, on}); }
    void set_piece_cached(i32 p, i32 on) override { rec(3, {p, on}); }
    void set_piece_shaded(i32 p, i32 on) override { rec(4, {p, on}); }
    i32 get_piece_move(i32 p, i32 a) override { return rec(5, {p, a}); }
    i32 get_piece_turn(i32 p, i32 a) override { return rec(6, {p, a}); }
    void legacy_explode(i32 p, i32 a, i32 b) override { rec(10, {p, a, b}); }
    void play_sound(i32 s) override { rec(11, {s}); }
    void emit_sfx(i32 p, i32 t) override { rec(12, {p, t}); }
    void explode_piece(i32 p, i32 t) override { rec(13, {p, t}); }
    void attach_unit(i32 u, i32 p, i32 m) override { rec(14, {u, p, m}); }
    void drop_unit(i32 u) override { rec(15, {u}); }
    void set_value(i32 id, i32 v) override { rec(16, {id, v}); }
    i32 get_value(i32 id, i32 a, i32 b, i32 c, i32 d) override { return rec(17, {id, a, b, c, d}); }
    i32 get_transporting(i32 i) override { return rec(18, {i}); }
    i32 get_carrier() override { return rec(19, {}); }
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void on_return(i32 v) override { rec(kSinkSlot, {v}); }
    u32 sink_address = 1;
    u32 handle() const override { return sink_address; }
};

u32 random_value(std::mt19937& g) {
    const u32 k = g() % 100;
    if (k < 50) return g() % 4;
    if (k < 70) return g() % 101;
    if (k < 85) return (g() % 65536) - 32768u;
    return static_cast<u32>(g());
}

struct Stats {
    std::size_t scripts = 0, ops = 0, mismatches = 0, faults = 0, hangs = 0, load_errors = 0;
    std::uint64_t op_counts[static_cast<std::size_t>(sim::ScriptOp::Count_)] = {};
};

// Runs one scenario for `cob`. Returns false if the child must be restarted.
bool run_script(Remote& r, const std::string& name, const std::vector<std::uint8_t>& file, int ticks, unsigned seed,
                Stats& st) {
    ta::CobFile cob;
    std::string err;
    if (!ta::load_cob(file, cob, err)) {
        ++st.load_errors;
        std::printf("%s: load error: %s\n", name.c_str(), err.c_str());
        return true;
    }
    const auto& prog = cob.program;
    if (file.size() + Remote::kCob > 0x200000 || prog.piece_names.size() * 0x4C > Remote::kCob - Remote::kAnims ||
        prog.static_count * 4 > static_cast<i32>(Remote::kAnims - Remote::kStatics)) {
        std::printf("%s: too large for the harness layout\n", name.c_str());
        return true;
    }
    ++st.scripts;
    std::mt19937 g(seed);

    // Remote: relocated COB image, zeroed object, vtable, sink, statics, anims.
    const u32 W = r.work_;
    const u32 cob_at = W + Remote::kCob;
    std::vector<std::uint8_t> img = file;
    auto rel = [&](std::size_t off) {
        u32 v;
        std::memcpy(&v, &img[off], 4);
        v += cob_at;
        std::memcpy(&img[off], &v, 4);
    };
    auto hdr = [&](int i) {
        u32 v;
        std::memcpy(&v, &img[static_cast<std::size_t>(i) * 4], 4);
        return v;
    };
    const u32 nfun = hdr(1), nsnd = hdr(5), names_off = hdr(7), pieces_off = hdr(8), snd_off = hdr(10);
    for (u32 i = 0; i < nfun; ++i) rel(names_off + i * 4);
    for (u32 i = 0; i < prog.piece_names.size(); ++i) rel(pieces_off + i * 4);
    for (u32 i = 0; i < nsnd; ++i) rel(snd_off + i * 8 + 4);
    for (int i : {6, 7, 8, 9, 10}) rel(static_cast<std::size_t>(i) * 4);
    r.write(cob_at, img.data(), img.size());

    std::vector<std::uint8_t> zero(0x20000, 0);
    r.write(W, zero.data(), zero.size());  // object, tables, statics, anims
    u32 vt[kVtableSlots + 1];
    for (int s = 0; s < kVtableSlots; ++s) vt[s] = r.stub_[s];
    vt[kVtableSlots] = 0;
    r.write(W + Remote::kVtable, vt, sizeof vt);
    r.w32(W + Remote::kSinkObj, W + Remote::kSinkVtbl);
    r.w32(W + Remote::kSinkVtbl, r.sink_stub_);
    const u32 obj = W + Remote::kObject;
    r.w32(obj + 0x00, W + Remote::kVtable);
    r.w32(obj + 0x04, 30);
    r.w32(obj + 0x08, cob_at);
    r.w32(obj + 0x10, W + Remote::kStatics);
    r.w32(obj + 0x14, W + Remote::kAnims);

    // Shared callback return values and RNG.
    std::vector<u32> rets(Remote::kRetSize / 4);
    for (auto& v : rets) v = random_value(g);
    r.write(r.ret_base_, rets.data(), rets.size() * 4);
    r.w32(r.ret_ptr_, r.ret_base_);
    const u32 rng_state = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng_state);

    LogHost host;
    host.ret = &rets;
    host.sink_address = W + Remote::kSinkObj;
    host.rng.set_state(rng_state);
    sim::ScriptInstance ours(prog, host, 30);
    ours.set_step_limit(20000000);
    ours.set_op_counter(st.op_counts);
    // Ours runs first: if it faults (out-of-range access or an endless loop) the original
    // would crash or hang, so the scenario ends before the original is asked to run it.
    auto ours_faulted = [&]() {
        if (host.overflow) {
            ++st.faults;
            std::printf("%s: stopped: callback buffers exhausted\n", name.c_str());
            return true;
        }
        if (ours.fault().empty()) return false;
        ++st.faults;
        if (ours.fault().rfind("step limit", 0) != 0 || st.faults <= 20)
        {
            std::printf("%s: stopped: %s\n", name.c_str(), ours.fault().c_str());
            for (int t = 0; t < sim::ScriptInstance::kThreads; ++t) {
                const u32 pc = ours.thread_word(t, sim::ScriptInstance::kPc);
                if (ours.thread_word(t, sim::ScriptInstance::kState) != sim::ScriptInstance::kRunning) continue;
                std::size_t fn = 0;
                for (std::size_t k = 0; k < prog.entry.size(); ++k)
                    if (prog.entry[k] <= pc && prog.entry[k] >= prog.entry[fn]) fn = k;
                std::printf("    running thread %d at %u in %s\n", t, pc, prog.function_names[fn].c_str());
            }
        }
        return true;
    };

    // Function names for named calls.
    std::vector<std::string> call_names = prog.function_names;
    call_names.push_back("NoSuchFunction");

    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        auto bad = [&](const char* m) {
            if (ok) std::printf("%s: %s: %s\n", name.c_str(), what.c_str(), m);
            ok = false;
        };
        // Callback log.
        const u32 end = r.r32(r.log_ptr_);
        std::vector<u32> log((end - r.log_base_) / 4);
        if (!log.empty()) r.read(r.log_base_, log.data(), log.size() * 4);
        if (log != host.log) {
            bad("callback log differs");
            const std::size_t n = (std::max)(log.size(), host.log.size());
            for (std::size_t i = 0, shown = 0; i < n && shown < 12; ++i) {
                const long long a = i < log.size() ? static_cast<long long>(log[i]) : -1;
                const long long b = i < host.log.size() ? static_cast<long long>(host.log[i]) : -1;
                if (a != b) { std::printf("    log[%zu] orig %lld ours %lld\n", i, a, b); ++shown; }
            }
        }
        host.log.clear();
        r.w32(r.log_ptr_, r.log_base_);
        // Thread block (+ live count) and dirty flag.
        u32 blk[sim::ScriptInstance::kThreads * sim::ScriptInstance::kThreadWords + 1];
        r.read(obj + 0x1C, blk, sizeof blk);
        const auto& mine = ours.thread_memory();
        for (std::size_t i = 0; i < mine.size(); ++i) {
            const bool same = blk[i] == mine[i];
            if (!same) {
                if (ok) std::printf("    thread word %zu (thread %zu, word %zu): orig %08x ours %08x\n", i,
                                    i / sim::ScriptInstance::kThreadWords, i % sim::ScriptInstance::kThreadWords, blk[i], mine[i]);
                bad("thread block differs");
            }
        }
        if ((r.r32(obj + 0x18) != 0) != ours.anim_dirty()) bad("animation dirty flag differs");
        std::vector<i32> an(ours.piece_anims().size());
        if (!an.empty()) r.read(W + Remote::kAnims, an.data(), an.size() * 4);
        if (an != ours.piece_anims()) bad("piece animation table differs");
        std::vector<i32> sv(ours.statics().size());
        if (!sv.empty()) r.read(W + Remote::kStatics, sv.data(), sv.size() * 4);
        if (sv != ours.statics()) bad("statics differ");
        if (r.r32(kBattleRngState) != host.rng.state()) bad("battle RNG state differs");
        if (r.r32(r.ret_ptr_) != r.ret_base_ + host.ret_at * 4) bad("callback count differs");
        if (!ok) ++st.mismatches;
        return ok;
    };

    r.w32(r.log_ptr_, r.log_base_);
    auto remote_failed = [&]() {
        ++st.hangs;
        std::printf("%s: original did not finish (hang or crash) - restarting child\n", name.c_str());
        return false;
    };

    // Create, then ticks with random calls in between.
    {
        const std::string n = "Create";
        r.write(W + Remote::kNames, n.c_str(), n.size() + 1);
        u32 res = 0;
        const bool mine = ours.call(n, nullptr, true);
        if (ours_faulted()) return true;
        if (!r.call(kCallNamed, obj, {W + Remote::kNames, 0, 1}, &res)) return remote_failed();
        ++st.ops;
        if ((res != 0) != mine) { std::printf("%s: Create result differs\n", name.c_str()); ++st.mismatches; return true; }
        if (!compare("Create")) return true;
    }
    for (int tick = 0; tick < ticks; ++tick) {
        if (g() % 4 == 0 && !prog.entry.empty()) {
            const u32 kind = g() % 3;
            const i32 fn = static_cast<i32>(g() % (prog.entry.size() + 1)) - (g() % 8 == 0 ? 1 : 0);
            const bool use_sink = g() % 2;
            const bool run_now = g() % 2;
            const i32 argc = static_cast<i32>(g() % 5);
            i32 a[4];
            // Mostly 0/1: scripts such as RequestState loop forever on states that do not exist.
            for (auto& x : a) x = g() % 10 < 8 ? static_cast<i32>(g() % 2) : static_cast<i32>(random_value(g));
            u32 res = 0;
            bool mine = false;
            std::string what;
            const u32 sink = use_sink ? W + Remote::kSinkObj : 0;
            if (kind == 0) {
                what = "invoke " + std::to_string(fn);
                mine = ours.call_with_args(fn, use_sink ? &host : nullptr, run_now, argc, a[0], a[1], a[2], a[3]);
                if (ours_faulted()) return true;
                if (!r.call(kInvokeByIndex, obj, {static_cast<u32>(fn), sink, run_now ? 1u : 0u, static_cast<u32>(argc),
                                                  static_cast<u32>(a[0]), static_cast<u32>(a[1]), static_cast<u32>(a[2]),
                                                  static_cast<u32>(a[3])}, &res))
                    return remote_failed();
            } else if (kind == 1) {
                what = "in/out " + std::to_string(fn);
                r.write(W + Remote::kOutInts, a, sizeof a);
                const u32 o = W + Remote::kOutInts;
                i32 b[4] = {a[0], a[1], a[2], a[3]};
                mine = ours.call_in_out(fn, &b[0], &b[1], &b[2], &b[3]);
                if (ours_faulted()) return true;
                if (!r.call(kCallInOut, obj, {static_cast<u32>(fn), o, o + 4, o + 8, o + 12}, &res)) return remote_failed();
                i32 got[4];
                r.read(o, got, sizeof got);
                if (std::memcmp(got, b, sizeof b) != 0) {
                    std::printf("%s: in/out values differ\n", name.c_str());
                    ++st.mismatches;
                    return true;
                }
            } else {
                const std::string& n = call_names[g() % call_names.size()];
                what = "call " + n;
                r.write(W + Remote::kNames, n.c_str(), n.size() + 1);
                mine = ours.call(n, use_sink ? &host : nullptr, run_now);
                if (ours_faulted()) return true;
                if (!r.call(kCallNamed, obj, {W + Remote::kNames, sink, run_now ? 1u : 0u}, &res)) return remote_failed();
            }
            ++st.ops;
            if ((res != 0) != mine) { std::printf("%s: %s result differs\n", name.c_str(), what.c_str()); ++st.mismatches; return true; }
            if (!compare(what)) return true;
        }
        const i32 dt = g() % 10 == 0 ? static_cast<i32>(g() % 3) : 1;
        ours.tick(dt);
        if (ours_faulted()) return true;
        if (!r.call(kTick, obj, {static_cast<u32>(dt)})) return remote_failed();
        ++st.ops;
        if (!compare("tick " + std::to_string(tick))) return true;
    }
    return true;
}

// A random COB image that uses every instruction (plus invalid words), for opcode coverage
// beyond what the stock scripts contain.
std::vector<std::uint8_t> synthetic_cob(std::mt19937& g) {
    const u32 pieces = 1 + g() % 6, statics = 1 + g() % 5, functions = 1 + g() % 5;
    struct Op {
        u32 word;
        int operands;  // 0 none, 1 piece, 2 piece+axis, 3 const, 4 local, 5 static, 6 jump, 7 fn+argc, 8 sound
    };
    static const Op kOps[] = {
        {0x10001000, 2}, {0x10002000, 2}, {0x10003000, 2}, {0x10004000, 2}, {0x10005000, 1}, {0x10006000, 1},
        {0x10007000, 1}, {0x10008000, 1}, {0x10009000, 1}, {0x1000A000, 8}, {0x1000B000, 2}, {0x1000C000, 2},
        {0x1000D000, 1}, {0x1000E000, 1}, {0x1000F000, 1}, {0x10011000, 2}, {0x10012000, 2}, {0x10013000, 0},
        {0x10021001, 3}, {0x10021002, 4}, {0x10021004, 5}, {0x10021000, 3}, {0x10021003, 3}, {0x10022000, 0},
        {0x10023002, 4}, {0x10023004, 5}, {0x10023001, 3}, {0x10024000, 0}, {0x10031000, 0}, {0x10032000, 0},
        {0x10033000, 0}, {0x10034000, 0}, {0x10035000, 0}, {0x10036000, 0}, {0x10037000, 0}, {0x10038000, 0},
        {0x10041000, 0}, {0x10042000, 0}, {0x10043000, 0}, {0x10044000, 0}, {0x10045000, 0}, {0x10051000, 0},
        {0x10052000, 0}, {0x10053000, 0}, {0x10054000, 0}, {0x10055000, 0}, {0x10056000, 0}, {0x10057000, 0},
        {0x10058000, 0}, {0x10059000, 0}, {0x1005A000, 0}, {0x10061000, 7}, {0x10062000, 7}, {0x10063000, 7},
        {0x10064000, 6}, {0x10065000, 0}, {0x10066000, 6}, {0x10067000, 0}, {0x10068000, 0}, {0x10071000, 1},
        {0x10082000, 0}, {0x10083000, 0}, {0x10084000, 0}, {0x12345678, 0}, {0x1F021001, 3},
    };
    // Lay out functions as runs of instructions; record instruction starts for jump targets.
    std::vector<u32> code, entries, starts;
    std::vector<std::pair<std::size_t, bool>> fixups;  // operand position, is jump
    for (u32 f = 0; f < functions; ++f) {
        entries.push_back(static_cast<u32>(code.size()));
        // Locals for the four call arguments, then a body.
        for (int k = 0; k < 4; ++k) { starts.push_back(static_cast<u32>(code.size())); code.push_back(0x10022000); }
        const int body = 5 + static_cast<int>(g() % 40);
        for (int i = 0; i < body; ++i) {
            const Op& op = kOps[g() % (sizeof kOps / sizeof kOps[0])];
            starts.push_back(static_cast<u32>(code.size()));
            // Feed most instructions some fresh constants so pops have something to take.
            if (g() % 2) {
                code.push_back(0x10021001);
                code.push_back(g() % 3 == 0 ? static_cast<u32>(g() % 2000) : g() % 4);
            }
            code.push_back(op.word);
            switch (op.operands) {
            case 1: code.push_back(g() % pieces); break;
            case 2: code.push_back(g() % pieces); code.push_back(g() % 3); break;
            case 3: code.push_back(random_value(g)); break;
            case 4: code.push_back(g() % 8); break;
            case 5: code.push_back(g() % statics); break;
            case 6: fixups.emplace_back(code.size(), true); code.push_back(0); break;
            case 7: code.push_back(g() % (functions + 1)); code.push_back(g() % 5); break;
            case 8: code.push_back(g() % 4); break;
            default: break;
            }
        }
        starts.push_back(static_cast<u32>(code.size()));
        code.push_back(0x10021001);
        code.push_back(g() % 4);
        code.push_back(0x10065000);
    }
    for (auto& [at, jump] : fixups) code[at] = starts[g() % starts.size()];

    // File: header, code, entry table, name offset tables, strings.
    std::vector<std::uint8_t> f(44, 0);
    auto put = [&](u32 v) { for (int i = 0; i < 4; ++i) f.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto set_hdr = [&](int i, u32 v) { std::memcpy(&f[static_cast<std::size_t>(i) * 4], &v, 4); };
    set_hdr(0, 4); set_hdr(1, functions); set_hdr(2, pieces); set_hdr(3, static_cast<u32>(code.size()));
    set_hdr(4, statics); set_hdr(5, 0);
    set_hdr(9, static_cast<u32>(f.size()));
    for (u32 w : code) put(w);
    set_hdr(6, static_cast<u32>(f.size()));
    for (u32 e : entries) put(e);
    const std::size_t names_at = f.size();
    set_hdr(7, static_cast<u32>(names_at));
    for (u32 i = 0; i < functions; ++i) put(0);
    const std::size_t pieces_at = f.size();
    set_hdr(8, static_cast<u32>(pieces_at));
    for (u32 i = 0; i < pieces; ++i) put(0);
    set_hdr(10, static_cast<u32>(f.size()));
    auto add_str = [&](std::size_t slot, const std::string& s) {
        const u32 at = static_cast<u32>(f.size());
        std::memcpy(&f[slot], &at, 4);
        f.insert(f.end(), s.begin(), s.end());
        f.push_back(0);
    };
    for (u32 i = 0; i < functions; ++i) add_str(names_at + i * 4, i == 0 ? "Create" : "Fn" + std::to_string(i));
    for (u32 i = 0; i < pieces; ++i) add_str(pieces_at + i * 4, "p" + std::to_string(i));
    return f;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: cob_harness <TotalA.exe> <game_dir> [ticks_per_script] [only_script] [rounds]\n");
        return 2;
    }
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 300;
    const std::string only = argc > 4 && std::string(argv[4]) != "*" ? argv[4] : "";
    const int rounds = argc > 5 ? std::atoi(argv[5]) : 1;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    const auto files = vfs.enumerate("scripts\\*.cob");

    Remote r;
    std::string exe = argv[1];
    std::string dir = exe.substr(0, exe.find_last_of("\\/"));
    if (!r.start(exe, dir)) {
        std::fprintf(stderr, "cannot start %s\n", exe.c_str());
        return 1;
    }
    Stats st;
    if (only.rfind("synthetic:", 0) == 0) {
        const int n = std::atoi(only.c_str() + 10);
        std::mt19937 g(777);
        for (int i = 0; i < n; ++i) {
            const auto img = synthetic_cob(g);
            if (!run_script(r, "synthetic#" + std::to_string(i), img, ticks, static_cast<unsigned>(i + 1), st)) {
                r.stop();
                if (!r.start(exe, dir)) return 1;
            }
        }
    }
    for (int round = 0; round < rounds && only.rfind("synthetic:", 0) != 0; ++round) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (!only.empty() && _stricmp(files[i].c_str(), only.c_str()) != 0) continue;
            std::vector<std::uint8_t> data;
            if (!vfs.read("scripts\\" + files[i], data)) continue;
            if (!run_script(r, files[i], data, ticks, static_cast<unsigned>(i * 7919 + round * 104729 + 1), st)) {
                r.stop();
                if (!r.start(exe, dir)) return 1;
            }
        }
    }
    r.stop();
    std::printf("executed instructions per op:");
    for (std::size_t k = 0; k < static_cast<std::size_t>(sim::ScriptOp::Count_); ++k)
        std::printf(" %zu:%llu", k, static_cast<unsigned long long>(st.op_counts[k]));
    std::printf("\n");
    std::printf("scripts %zu, operations %zu, mismatching scripts %zu, faults %zu, original hangs %zu, load errors %zu\n",
                st.scripts, st.ops, st.mismatches, st.faults, st.hangs, st.load_errors);
    return st.mismatches || st.load_errors ? 1 : 0;
}
