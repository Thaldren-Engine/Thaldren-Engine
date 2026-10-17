// Sound against the original, with its sound output replaced by a stand-in that logs every
// call (the platform mixer's buffer interface: duplicate, status, play position, volume, place,
// play, stop):
//   mixer   - the sound manager itself (0x4CF570 start, 0x4CF540 looping start, 0x4CF8A0
//             one-shot start, 0x4CF0B0 sweep, 0x4CF150 stop all) on random starts, stops,
//             limits and playing times, against client::SoundMixer;
//   world   - the world sound routine 0x47F300 and the interface sound 0x47F0C0 on a synthetic
//             game state (settings, sight and explored grids, map and view), against
//             client::world_sound and the interface rule, both through the sound managers;
//   notices - the unit notices' queue (0x47FAD0 queue, 0x47F680 per frame, 0x47F8C0 forget,
//             0x47FD70 play) with rand(), the console and the file sound stubbed, against
//             client::NoticeQueue.
// Compared after every operation: what the sound output was told (which sound and copy
// starts or restarts, at what volume and place; which playing copy stops), the number of
// tracked sounds, the notices' sounds, picks and texts, and the rand() state. Spec:
// docs/RE_MEMORY.md, "Client", "Sound engine".
//
// Usage: sound_harness <TotalA.exe> [scenarios] [operations] [first seed] [--verbose]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "client/sound_mixer.h"
#include "client/sound_rules.h"
#include "remote.h"
#include "sim/rng.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kStart = 0x004CF570, kStartLoop = 0x004CF540, kOneShot = 0x004CF8A0, kSweep = 0x004CF0B0, kStopAll = 0x004CF150,
              kCreateFromFile = 0x004CF370, kWorld = 0x0047F300, kInterface = 0x0047F0C0, kQueue = 0x0047FAD0, kNoticeFrame = 0x0047F680,
              kForget = 0x0047F8C0, kAlloc = 0x004D83B0, kFree = 0x004D85A0, kRand = 0x004E4870, kPrintf = 0x004E42B0,
              kConsole = 0x00463CA0, kPath = 0x004290F0, kPlayFile = 0x004D0640, kNoticeQueuePtr = 0x0051E68C, kNoticeNext = 0x005086EC;

// Log record ids.
enum : u32 {
    kDuplicate = 1, kQuery = 2, kStatus = 3, kPosition = 4, kSetPosition = 5, kSetVolume = 6, kPlay = 7, kStop = 8, kRelease = 9,
    kAddRef = 10, kPlace = 11, kSetMin = 12, kSetMax = 13, kSetMode = 14, k3dRelease = 15, kCreated = 20, kPrintfCall = 30,
    kConsoleCall = 31, kPathCall = 32, kPlayFileCall = 33
};

struct Emit {
    std::vector<std::uint8_t> b;
    void u8(int v) { b.push_back(static_cast<std::uint8_t>(v)); }
    void d(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void bytes(std::initializer_list<int> l) {
        for (int v : l) u8(v);
    }
};

struct Layout {
    u32 data = 0, code = 0;
    u32 cnt = 0, pool = 0, sndpool = 0, heap = 0, rand_state = 0, log = 0;
    u32 ds_vt = 0, dsb_vt = 0, d3d_vt = 0, ds = 0, mgr = 0, queue = 0, strings = 0;
    u32 gs = 0, grids = 0;
};

struct LogRec {
    u32 id, self, a1, a2, a3, a4, status, extra;
};

// The stand-in output's code: a logged stub per method (EBX = the record while the body runs).
class Stubs {
public:
    Stubs(harness::Remote& r, Layout& L) : r_(r), L_(L) {}
    u32 at() const { return L_.code + static_cast<u32>(e_.b.size()); }
    void prologue(u32 id, bool status = true) {
        e_.u8(0x53);                               // push ebx
        e_.bytes({0x8B, 0x1D}); e_.d(L_.cnt);      // mov ebx,[cnt]
        e_.bytes({0xFF, 0x05}); e_.d(L_.cnt);      // inc dword [cnt]
        e_.bytes({0x81, 0xE3}); e_.d(4095);        // and ebx,4095
        e_.bytes({0xC1, 0xE3, 0x05});              // shl ebx,5
        e_.bytes({0x81, 0xC3}); e_.d(L_.log);      // add ebx,log
        e_.bytes({0xC7, 0x03}); e_.d(id);          // mov [ebx],id
        for (int k = 0; k < 5; ++k) {
            e_.bytes({0x8B, 0x44, 0x24, 8 + 4 * k});  // mov eax,[esp+8+4k]
            e_.bytes({0x89, 0x43, 4 + 4 * k});        // mov [ebx+4+4k],eax
        }
        if (!status) return;
        e_.bytes({0x8B, 0x44, 0x24, 0x08});        // mov eax,[esp+8] (self)
        e_.bytes({0x8B, 0x40, 0x04});              // mov eax,[eax+4] (its status now)
        e_.bytes({0x89, 0x43, 0x18});              // mov [ebx+24],eax
    }
    void epilogue(int ret, bool zero = true) {
        if (zero) e_.bytes({0x33, 0xC0});         // xor eax,eax
        e_.u8(0x5B);                               // pop ebx
        if (ret) {
            e_.u8(0xC2);
            e_.u8(ret);
            e_.u8(0);
        } else {
            e_.u8(0xC3);
        }
        align();
    }
    void align() {
        while (e_.b.size() % 16) e_.u8(0xCC);
    }
    // A new stand-in buffer from the pool into EAX (status stopped, play position 0).
    void new_buffer() {
        e_.u8(0xA1); e_.d(L_.pool);                      // mov eax,[pool]
        e_.bytes({0x81, 0x05}); e_.d(L_.pool); e_.d(0x40); // add dword [pool],0x40
        e_.bytes({0xC7, 0x00}); e_.d(L_.dsb_vt);           // mov [eax],dsb_vt
        e_.bytes({0xC7, 0x40, 0x04}); e_.d(0);              // mov [eax+4],0
        e_.bytes({0xC7, 0x40, 0x08}); e_.d(0);              // mov [eax+8],0
        e_.bytes({0xC7, 0x40, 0x20}); e_.d(L_.d3d_vt);      // mov [eax+0x20],d3d_vt
    }
    bool build() {
        std::vector<u32> ds(16, 0), dsb(24, 0), d3d(24, 0);
        const u32 trap = at();
        e_.u8(0xCC);
        align();
        for (auto& v : ds) v = trap;
        for (auto& v : dsb) v = trap;
        for (auto& v : d3d) v = trap;
        // IDirectSound::DuplicateSoundBuffer(this, original, out)
        ds[0x14 / 4] = at();
        prologue(kDuplicate);
        new_buffer();
        e_.bytes({0x8B, 0x54, 0x24, 0x10});  // mov edx,[esp+16] (out)
        e_.bytes({0x89, 0x02});              // mov [edx],eax
        e_.bytes({0x89, 0x43, 0x1C});        // mov [ebx+28],eax
        epilogue(12);
        // IDirectSoundBuffer
        dsb[0] = at();  // QueryInterface(this, iid, out): the buffer's place object
        prologue(kQuery);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0x8D, 0x41, 0x20, 0x8B, 0x54, 0x24, 0x10, 0x89, 0x02});
        epilogue(12);
        dsb[1] = at();
        prologue(kAddRef);
        epilogue(4);
        dsb[2] = at();
        prologue(kRelease);
        epilogue(4);
        dsb[0x24 / 4] = at();  // GetStatus(this, out)
        prologue(kStatus);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x0C, 0x8B, 0x41, 0x04, 0x89, 0x02});
        epilogue(8);
        dsb[0x10 / 4] = at();  // GetCurrentPosition(this, play, write)
        prologue(kPosition);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x41, 0x08, 0x8B, 0x54, 0x24, 0x0C, 0x85, 0xD2, 0x74, 0x02, 0x89, 0x02, 0x8B, 0x54, 0x24,
                  0x10, 0x85, 0xD2, 0x74, 0x02, 0x89, 0x02});
        epilogue(12);
        dsb[0x34 / 4] = at();  // SetCurrentPosition(this, pos)
        prologue(kSetPosition);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0xC7, 0x41, 0x08}); e_.d(0);
        epilogue(8);
        dsb[0x3C / 4] = at();  // SetVolume(this, v)
        prologue(kSetVolume);
        epilogue(8);
        dsb[0x30 / 4] = at();  // Play(this, r1, r2, flags)
        prologue(kPlay);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0xC7, 0x41, 0x04}); e_.d(1);
        epilogue(16);
        dsb[0x48 / 4] = at();  // Stop(this)
        prologue(kStop);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0xC7, 0x41, 0x04}); e_.d(0);
        epilogue(4);
        // IDirectSound3DBuffer (self = buffer + 0x20)
        d3d[2] = at();
        prologue(k3dRelease);
        epilogue(4);
        d3d[0x4C / 4] = at();  // SetPosition(this, x, y, z, apply)
        prologue(kPlace);
        epilogue(20);
        d3d[0x44 / 4] = at();  // SetMinDistance(this, d, apply)
        prologue(kSetMin);
        epilogue(12);
        d3d[0x40 / 4] = at();  // SetMaxDistance
        prologue(kSetMax);
        epilogue(12);
        d3d[0x48 / 4] = at();  // SetMode(this, mode, apply)
        prologue(kSetMode);
        epilogue(12);
        // The one-shot's buffer from a file (thiscall, five arguments): a new sound of one buffer.
        const u32 create = at();
        prologue(kCreated, false);
        new_buffer();
        e_.bytes({0x8B, 0x15}); e_.d(L_.sndpool);              // mov edx,[sndpool]
        e_.bytes({0x81, 0x05}); e_.d(L_.sndpool); e_.d(16);    // add dword [sndpool],16
        e_.bytes({0x89, 0x02});                                 // mov [edx],eax
        e_.bytes({0xC7, 0x42, 0x04}); e_.d(0);
        e_.bytes({0xC7, 0x42, 0x08}); e_.d(0);
        e_.bytes({0xC7, 0x42, 0x0C}); e_.d(0);
        e_.bytes({0x89, 0x43, 0x1C});                           // mov [ebx+28],eax
        e_.bytes({0x8B, 0xC2});                                 // mov eax,edx
        epilogue(20, false);
        // Allocation on our own heap; freeing does nothing.
        const u32 alloc = at();
        e_.u8(0xA1); e_.d(L_.heap);
        e_.bytes({0x8B, 0x4C, 0x24, 0x08, 0x83, 0xC1, 0x0F, 0x83, 0xE1, 0xF0, 0x01, 0x0D}); e_.d(L_.heap);
        e_.u8(0xC3);
        align();
        const u32 free_ = at();
        e_.u8(0xC3);
        align();
        // rand(): the C runtime's generator on our own state.
        const u32 rand_ = at();
        e_.u8(0xA1); e_.d(L_.rand_state);
        e_.bytes({0x69, 0xC0}); e_.d(214013u);
        e_.u8(0x05); e_.d(2531011u);
        e_.u8(0xA3); e_.d(L_.rand_state);
        e_.bytes({0xC1, 0xE8, 0x10, 0x25}); e_.d(0x7FFF);
        e_.u8(0xC3);
        align();
        // sprintf (cdecl): logged (dest, format, name, text), nothing written.
        const u32 printf_ = at();
        prologue(kPrintfCall, false);
        epilogue(0);
        // The console (stdcall, four arguments): logged.
        const u32 console = at();
        prologue(kConsoleCall, false);
        epilogue(16);
        // The virtual path (stdcall: out, dir, name, ext): logged; the name copied to out.
        const u32 path = at();
        prologue(kPathCall, false);
        e_.bytes({0x8B, 0x44, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x10});        // mov eax,[esp+8]; mov edx,[esp+16]
        e_.bytes({0x8A, 0x0A, 0x88, 0x08, 0x40, 0x42, 0x84, 0xC9, 0x75, 0xF6});  // copy the name
        e_.bytes({0x8B, 0x44, 0x24, 0x08});                                // mov eax,[esp+8]
        epilogue(16, false);
        // A file sound by path (thiscall: path, volume, place): logged.
        const u32 play_file = at();
        prologue(kPlayFileCall, false);
        epilogue(12);

        if (GetEnvironmentVariableA("SOUND_HARNESS_STUBS", nullptr, 0) != 0)
            std::printf("stubs at %08X: dup %08X query %08X status %08X pos %08X play %08X stop %08X create %08X path %08X end %08X\n", L_.code, ds[5],
                        dsb[0], dsb[9], dsb[4], dsb[12], dsb[18], create, path, at());
        if (!r_.write(L_.code, e_.b.data(), e_.b.size())) return false;
        if (!r_.write(L_.ds_vt, ds.data(), ds.size() * 4) || !r_.write(L_.dsb_vt, dsb.data(), dsb.size() * 4) ||
            !r_.write(L_.d3d_vt, d3d.data(), d3d.size() * 4))
            return false;
        const std::pair<u32, u32> jumps[] = {{kCreateFromFile, create}, {kAlloc, alloc}, {kFree, free_}, {kRand, rand_}, {kPrintf, printf_},
                                             {kConsole, console},       {kPath, path},   {kPlayFile, play_file}};
        for (const auto& [from, to] : jumps)
            if (!r_.patch_jump(from, to)) return false;
        return true;
    }

private:
    harness::Remote& r_;
    Layout& L_;
    Emit e_;
};

// What the output was told, in a comparable form.
struct Event {
    char kind = 'S';  // S start, X stop (of a playing copy)
    int owner = -1;   // bank sound index, or 1000 + one-shot number
    int copy = 0;
    bool restart = false, loop = false, placed = false;
    int volume = 0;
    float x = 0, y = 0, z = 0, min_d = 0, max_d = 0;
    bool operator==(const Event& o) const {
        if (kind != o.kind || owner != o.owner || copy != o.copy) return false;
        if (kind == 'X') return true;
        return restart == o.restart && loop == o.loop && placed == o.placed && volume == o.volume &&
               (!placed || (x == o.x && y == o.y && z == o.z && min_d == o.min_d && max_d == o.max_d));
    }
    std::string text() const {
        char s[200];
        if (kind == 'X') std::snprintf(s, sizeof s, "stop %d/%d", owner, copy);
        else
            std::snprintf(s, sizeof s, "start %d/%d vol %d%s%s%s", owner, copy, volume, restart ? " restart" : "", loop ? " loop" : "",
                          placed ? " placed" : "");
        std::string t = s;
        if (kind == 'S' && placed) {
            std::snprintf(s, sizeof s, " (%g,%g,%g) %g..%g", x, y, z, min_d, max_d);
            t += s;
        }
        return t;
    }
};

float as_float(u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
u32 as_bits(float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    return v;
}

// One scenario's world: the stand-in output's buffers and the client's mixer side by side.
struct World {
    harness::Remote& r;
    Layout& L;
    std::mt19937& g;
    bool verbose = false;
    // Bank sounds: the original's sound arrays and the client's samples.
    std::vector<u32> sounds;
    std::vector<std::unique_ptr<client::Sample>> samples;   // bank (index) and one-shots (1000 + n)
    std::map<const client::Sample*, int> owner_of;
    struct Buf {
        int owner = -1, copy = 0;
        bool loop = false;
        u32 start = 0;
        const client::Sample* s = nullptr;
    };
    std::map<u32, Buf> bufs;  // stand-in buffer -> what it is
    u32 log_seen = 0;
    client::SoundMixer mixer;
    int one_shots = 0;

    World(harness::Remote& rr, Layout& ll, std::mt19937& gg) : r(rr), L(ll), g(gg) {}
    u32 pick(u32 n) { return n ? g() % n : 0; }

    std::unique_ptr<client::Sample> make_sample(int frames, int rate) {
        auto s = std::make_unique<client::Sample>();
        s->rate = rate;
        s->channels = 1;
        s->pcm.assign(static_cast<std::size_t>(frames), 1000);
        return s;
    }
    bool reset(int bank) {
        // The manager: fields as its constructor leaves them, the output object, the limit.
        std::vector<std::uint8_t> zero(0x300, 0);
        r.write(L.mgr, zero.data(), zero.size());
        r.w32(L.mgr + 0x24, L.ds);
        r.w32(L.ds, L.ds_vt);
        r.w32(L.mgr + 0x2C, 8);
        r.w32(L.mgr + 0x288, 0xFFFFFFFFu);
        r.w32(L.pool, L.data + 0x20100);
        r.w32(L.sndpool, L.data + 0x40000);
        r.w32(L.cnt, 0);
        log_seen = 0;
        mixer = client::SoundMixer(8);
        sounds.clear();
        samples.clear();
        owner_of.clear();
        bufs.clear();
        one_shots = 0;
        for (int k = 0; k < bank; ++k) {
            // A sound: its array of four buffers with the first.
            const u32 snd = r.r32(L.sndpool);
            r.w32(L.sndpool, snd + 16);
            const u32 b = r.r32(L.pool);
            r.w32(L.pool, b + 0x40);
            const u32 init[9] = {L.dsb_vt, 0, 0, 0, 0, 0, 0, 0, L.d3d_vt};
            r.write(b, init, sizeof init);
            const u32 arr[4] = {b, 0, 0, 0};
            r.write(snd, arr, sizeof arr);
            sounds.push_back(snd);
            samples.push_back(make_sample(static_cast<int>(200 + pick(30000)), pick(4) ? 11025 : 22050));
            owner_of[samples.back().get()] = k;
            bufs[b] = {k, 0, false, 0, samples.back().get()};
        }
        return true;
    }
    // The playing state of every stand-in buffer at time `t` (status and play position).
    void advance(u32 t) {
        for (auto& [addr, b] : bufs) {
            const u32 status = r.r32(addr + 4);
            if (status != 1 || !b.s) continue;
            const std::uint64_t elapsed = t - b.start;
            const std::uint64_t frames = b.s->pcm.size();
            if (!b.loop && elapsed * static_cast<std::uint64_t>(b.s->rate) >= frames * 1000u) {
                r.w32(addr + 4, 0);
                continue;
            }
            std::uint64_t pos = elapsed * static_cast<std::uint64_t>(b.s->rate) / 1000u;
            pos = b.loop ? pos % frames : std::min(pos, frames);
            r.w32(addr + 8, static_cast<u32>(pos));
        }
    }
    // The original's events since the last look, from the log.
    std::vector<Event> original_events(u32 t) {
        std::vector<Event> out;
        const u32 n = r.r32(L.cnt);
        // Map each sound's array slots to copies.
        for (std::size_t k = 0; k < sounds.size(); ++k)
            for (int i = 0; i < 4; ++i) {
                const u32 b = r.r32(sounds[k] + 4 * static_cast<u32>(i));
                if (!b) continue;
                auto it = bufs.find(b);
                if (it == bufs.end()) bufs[b] = {static_cast<int>(k), i, false, 0, samples[k].get()};
                else it->second.owner = static_cast<int>(k), it->second.copy = i;
            }
        int sets = 0;
        Event cur;
        bool have = false;
        for (u32 i = log_seen; i < n; ++i) {
            LogRec rec{};
            r.read(L.log + (i & 4095) * 32, &rec, sizeof rec);
            if (rec.id == kCreated) {
                // A one-shot's buffer: the next one-shot sample.
                const int idx = 1000 + one_shots - 1;
                bufs[rec.extra] = {idx, 0, false, 0, one_shot_sample(idx)};
            } else if (rec.id == kDuplicate) {
                const Buf& o = bufs[rec.a1];
                int slot = 0;
                if (o.owner >= 0 && o.owner < 1000)
                    for (int s = 0; s < 4; ++s)
                        if (r.r32(sounds[static_cast<std::size_t>(o.owner)] + 4 * static_cast<u32>(s)) == rec.extra) slot = s;
                bufs[rec.extra] = {o.owner, slot, false, 0, o.s};
            } else if (rec.id == kSetPosition) {
                ++sets;
            } else if (rec.id == kSetVolume) {
                cur.volume = static_cast<i32>(rec.a1);
            } else if (rec.id == kPlace) {
                cur.placed = true;
                cur.x = as_float(rec.a1), cur.y = as_float(rec.a2), cur.z = as_float(rec.a3);
            } else if (rec.id == kSetMin) {
                cur.min_d = as_float(rec.a1);
            } else if (rec.id == kSetMax) {
                cur.max_d = as_float(rec.a1);
            } else if (rec.id == kSetMode) {
                if (rec.a1 == 2) cur.placed = false;
                have = true;
            } else if (rec.id == kPlay) {
                Buf& b = bufs[rec.self];
                b.loop = (rec.a3 & 1) != 0;
                b.start = t;
                cur.kind = 'S';
                cur.owner = b.owner;
                cur.copy = b.copy;
                cur.loop = b.loop;
                cur.restart = sets >= 2;
                out.push_back(cur);
                cur = Event{};
                sets = 0;
                have = false;
            } else if (rec.id == kStop) {
                if (rec.status == 1) {
                    Event e;
                    e.kind = 'X';
                    e.owner = bufs[rec.self].owner;
                    e.copy = bufs[rec.self].copy;
                    out.push_back(e);
                }
            }
        }
        (void)have;
        log_seen = n;
        return out;
    }
    std::vector<std::unique_ptr<client::Sample>> one_shot_samples;
    client::Sample* one_shot_sample(int idx) {
        const std::size_t k = static_cast<std::size_t>(idx - 1000);
        return k < one_shot_samples.size() ? one_shot_samples[k].get() : nullptr;
    }
    std::vector<Event> client_events() {
        std::vector<Event> out;
        for (const client::SoundCommand& c : mixer.take_commands()) {
            Event e;
            e.kind = c.kind == client::SoundCommand::Kind::Start ? 'S' : 'X';
            auto it = owner_of.find(c.sample);
            e.owner = it != owner_of.end() ? it->second : -1;
            e.copy = c.copy;
            e.restart = c.restart;
            e.loop = c.loop;
            e.volume = c.volume;
            e.placed = c.placed;
            e.x = c.pos[0], e.y = c.pos[1], e.z = c.pos[2];
            e.min_d = c.min_distance, e.max_d = c.max_distance;
            out.push_back(e);
        }
        return out;
    }
    // A new one-shot sample (the original's file sound and the client's).
    const client::Sample* new_one_shot() {
        one_shot_samples.push_back(make_sample(static_cast<int>(500 + pick(20000)), 11025));
        const client::Sample* s = one_shot_samples.back().get();
        owner_of[s] = 1000 + one_shots;
        ++one_shots;
        return s;
    }
};

bool same(const std::vector<Event>& a, const std::vector<Event>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}
void show(const char* who, const std::vector<Event>& v) {
    std::printf("    %s:", who);
    for (const Event& e : v) std::printf(" [%s]", e.text().c_str());
    std::printf("\n");
}

// ---------------------------------------------------------------- the game state for 0x47F300
constexpr u32 gViewed = 0x2A43, gPlayers = 0x1B63, gPlayerSize = 0x14B, gSightMap = 0x1BDF - 0x1B63, gSightW = 0x1BE3 - 0x1B63,
              gSightH = 0x1BE7 - 0x1B63, gLosFlags = 0x14281, gExplored = 0x14273, gMapW = 0x14233, gMapH = 0x14237, gCells = 0x14287,
              gViewW = 0x1423B, gViewH = 0x1423F, gScrollX = 0x1431F, gScrollY = 0x14323, gBankCount = 0x33A0F, gBank = 0x33A13,
              gFxVol = 0x37F0C, gSoundBits = 0x37F19, gTick = 0x38A47, gChat = 0x37F17, gChatText = 0x37F18, gCategories = 0x37E13;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sound_harness <TotalA.exe> [scenarios] [operations] [first seed] [--verbose]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 6;
    const int operations = argc > 3 ? std::atoi(argv[3]) : 200;
    const u32 first_seed = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 10)) : 1;
    bool verbose = false;
    for (int i = 5; i < argc; ++i)
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    harness::Remote r;
    if (!r.start(argv[1])) {
        std::printf("cannot start %s\n", argv[1]);
        return 1;
    }
    r.install_crash_catcher();
    Layout L;
    L.data = r.alloc(0x100000);
    L.code = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.gs = r.alloc(0x40000);
    L.grids = r.alloc(0x40000);
    if (!L.data || !L.code || !L.gs || !L.grids) return 1;
    L.cnt = L.data;
    L.pool = L.data + 4;
    L.sndpool = L.data + 8;
    L.heap = L.data + 12;
    L.rand_state = L.data + 16;
    L.log = L.data + 0x100;
    L.ds_vt = L.data + 0x80000;
    L.dsb_vt = L.data + 0x80100;
    L.d3d_vt = L.data + 0x80300;
    L.ds = L.data + 0x80400;
    L.mgr = L.data + 0x80800;
    L.queue = L.data + 0x80C00;
    L.strings = L.data + 0x81000;
    r.w32(L.heap, L.data + 0x48000);
    Stubs st(r, L);
    if (!st.build()) {
        std::printf("cannot install the stand-in output\n");
        return 1;
    }
    r.w32(kGameStatePtr, L.gs);
    r.w32(L.gs + 0x10, L.mgr);

    std::size_t mismatches = 0, ops_total = 0, starts = 0, stops = 0, restarts = 0, placed = 0, world_calls = 0, ui_calls = 0,
                notices_spoken = 0, notice_texts = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first_seed + static_cast<u32>(sc);
        std::mt19937 g(seed);
        World w(r, L, g);
        w.verbose = verbose;
        const int bank = 2 + static_cast<int>(w.pick(5));
        w.reset(bank);
        // Bank entries for the routines that start sounds by index.
        r.w32(L.gs + gBankCount, static_cast<u32>(bank));
        for (int k = 0; k < bank; ++k) r.w32(L.gs + gBank + 4 * static_cast<u32>(k), w.sounds[static_cast<std::size_t>(k)]);
        u32 t = 1000;
        int max_playing = 8;
        bool three_d = false;
        float min_d = 0, max_d = 0;
        // ---- part 1: the sound manager
        for (int op = 0; op < operations; ++op, ++ops_total) {
            t += w.pick(4) == 0 ? w.pick(3000) : w.pick(200);
            w.advance(t);
            w.original_events(t);  // (status reads of the advance are not events)
            const u32 what = w.pick(100);
            bool ok_orig = true;
            u32 ret = 0;
            bool ret_client = false;
            std::string desc;
            if (what < 3) {
                // (at least 2: with a limit of 1 and the looping sound tracked, the original reads past
                // its table looking for a sound to stop, and faults)
                max_playing = 2 + static_cast<int>(w.pick(7));
                r.w32(L.mgr + 0x2C, static_cast<u32>(max_playing));
                w.mixer.set_max_playing(max_playing);
                desc = "limit " + std::to_string(max_playing);
                continue;
            }
            if (what < 8) {
                three_d = !three_d;
                min_d = static_cast<float>(16 * (1 + w.pick(40)));
                max_d = static_cast<float>(16 * (50 + w.pick(400)));
                r.w32(L.mgr + 4, three_d ? 1u : 0u);
                r.w32(L.mgr + 8, as_bits(min_d));
                r.w32(L.mgr + 12, as_bits(max_d));
                continue;
            }
            const int volume = w.pick(3) == 0 ? -static_cast<int>(w.pick(3000)) : (w.pick(2) ? -585 : -1585);
            const bool with_pos = w.pick(2) != 0;
            const i32 px = static_cast<i32>(w.pick(2000)) - 1000, pz = static_cast<i32>(w.pick(2000)) - 1000;
            const u32 pos_at = L.strings;
            const i32 pos[3] = {px, 0, pz};
            r.write(pos_at, pos, sizeof pos);
            client::SoundRequest q;
            q.volume = volume;
            q.placed = three_d && with_pos;
            q.pos = {static_cast<float>(px), 0.0f, static_cast<float>(pz)};
            q.min_distance = min_d;
            q.max_distance = max_d;
            if (what < 70) {
                const int k = static_cast<int>(w.pick(static_cast<u32>(bank)));
                q.sample = w.samples[static_cast<std::size_t>(k)].get();
                ok_orig = r.call(kStart, L.mgr, {w.sounds[static_cast<std::size_t>(k)], static_cast<u32>(volume), with_pos ? pos_at : 0}, &ret);
                ret_client = w.mixer.play(q, t);
                desc = "start " + std::to_string(k);
            } else if (what < 76) {
                const int k = static_cast<int>(w.pick(static_cast<u32>(bank)));
                q.sample = w.samples[static_cast<std::size_t>(k)].get();
                q.loop = true;
                q.placed = false;
                ok_orig = r.call(kStartLoop, L.mgr, {w.sounds[static_cast<std::size_t>(k)], static_cast<u32>(volume)}, &ret);
                ret_client = w.mixer.play(q, t);
                desc = "loop " + std::to_string(k);
            } else if (what < 88) {
                q.sample = w.new_one_shot();
                q.one_shot = true;
                const int frames = static_cast<int>(q.sample->pcm.size());
                ok_orig = r.call(kOneShot, L.mgr, {0, static_cast<u32>(frames), 11025, 8, 1, static_cast<u32>(volume), with_pos ? pos_at : 0}, &ret);
                ret_client = w.mixer.play(q, t);
                desc = "one-shot";
            } else if (what < 97) {
                ok_orig = r.call(kSweep, L.mgr, {});
                w.mixer.sweep(t);
                ret = 1;
                ret_client = true;
                desc = "sweep";
            } else {
                ok_orig = r.call(kStopAll, L.mgr, {});
                w.mixer.stop_all(t);
                ret = 1;
                ret_client = true;
                desc = "stop all";
            }
            if (!ok_orig) {
                const auto c = r.crash_info();
                std::printf("scenario %u op %d (%s): the original's call failed (code %08X at %08X, esp %08X, data %08X)\n", seed, op, desc.c_str(),
                            c.code, c.address, c.esp, L.data);
                if (verbose) {
                    // The calls the original made before it failed.
                    const u32 n = r.r32(L.cnt);
                    for (u32 i = w.log_seen; i < n; ++i) {
                        LogRec rec{};
                        r.read(L.log + (i & 4095) * 32, &rec, sizeof rec);
                        std::printf("  call %u: id %u self %08X args %08X %08X %08X %08X status %u extra %08X\n", i, rec.id, rec.self, rec.a1, rec.a2,
                                    rec.a3, rec.a4, rec.status, rec.extra);
                    }
                }
                ++mismatches;
                break;
            }
            const std::vector<Event> eo = w.original_events(t), ec = w.client_events();
            for (const Event& e : eo) {
                starts += e.kind == 'S';
                stops += e.kind == 'X';
                restarts += e.restart;
                placed += e.placed;
            }
            const int count_o = static_cast<int>(r.r32(L.mgr + 0x30));
            const bool bad = !same(eo, ec) || (ret != 0) != ret_client || count_o != w.mixer.tracked();
            if (bad || verbose) {
                std::printf("%s scenario %u op %d t %u: %s -> %u/%d tracked %d/%d\n", bad ? "MISMATCH" : "  ", seed, op, t, desc.c_str(), ret,
                            ret_client, count_o, w.mixer.tracked());
                show("original", eo);
                show("client  ", ec);
            }
            if (bad) {
                ++mismatches;
                break;
            }
        }

        // ---- part 2: world and interface sounds through 0x47F300 / 0x47F0C0
        w.reset(bank);
        for (int k = 0; k < bank; ++k) r.w32(L.gs + gBank + 4 * static_cast<u32>(k), w.sounds[static_cast<std::size_t>(k)]);
        r.w32(L.mgr + 0x2C, 8);
        {
            // Sight: a 64 x 64 block grid for the viewed player, explored bits for every player.
            const int bw = 40 + static_cast<int>(w.pick(24)), bh = 40 + static_cast<int>(w.pick(24));
            std::vector<std::uint8_t> sight(static_cast<std::size_t>(bw * bh));
            std::vector<std::uint16_t> explored(static_cast<std::size_t>(bw * bh));
            for (auto& v : sight) v = static_cast<std::uint8_t>(w.pick(3) ? 0 : 1 + w.pick(200));
            for (auto& v : explored) v = static_cast<std::uint16_t>(g());
            const u32 sight_at = L.grids, explored_at = L.grids + 0x10000;
            r.write(sight_at, sight.data(), sight.size());
            r.write(explored_at, explored.data(), explored.size() * 2);
            const int viewed = static_cast<int>(w.pick(10));
            const std::uint8_t vb = static_cast<std::uint8_t>(viewed);
            r.write(L.gs + gViewed, &vb, 1);
            const u32 pa = L.gs + gPlayers + static_cast<u32>(viewed) * gPlayerSize;
            r.w32(pa + gSightMap, sight_at);
            r.w32(pa + gSightW, static_cast<u32>(bw));
            r.w32(pa + gSightH, static_cast<u32>(bh));
            r.w32(L.gs + gExplored, explored_at);
            r.w32(L.gs + gCells, L.grids + 0x30000);
            for (int op = 0; op < operations; ++op, ++ops_total) {
                t += w.pick(4) == 0 ? w.pick(3000) : w.pick(200);
                w.advance(t);
                w.original_events(t);
                client::SoundView v;
                v.fx_volume = w.pick(8) == 0 ? 0 : static_cast<int>(1 + w.pick(64));
                v.sound_mode = static_cast<int>(w.pick(7) == 0 ? 0 : 1 + w.pick(2));
                v.view_w = 32 + static_cast<int>(w.pick(70));
                v.view_h = 26 + static_cast<int>(w.pick(50));
                v.map_w = 32 + static_cast<int>(w.pick(100));
                v.map_h = 32 + static_cast<int>(w.pick(100));
                v.scroll_x = static_cast<int>(w.pick(static_cast<u32>(v.map_w * 16)));
                v.scroll_y = static_cast<int>(w.pick(static_cast<u32>(v.map_h * 16)));
                const bool los = w.pick(2) != 0;
                std::uint8_t lf = los ? 2 : 0;
                r.write(L.gs + gLosFlags, &lf, 1);
                r.w32(L.gs + gFxVol, static_cast<u32>(v.fx_volume));
                const std::uint16_t bits = static_cast<std::uint16_t>(v.sound_mode | 0x40);
                r.write(L.gs + gSoundBits, &bits, 2);
                r.w32(L.gs + gMapW, static_cast<u32>(v.map_w));
                r.w32(L.gs + gMapH, static_cast<u32>(v.map_h));
                r.w32(L.gs + gViewW, static_cast<u32>(v.view_w));
                r.w32(L.gs + gViewH, static_cast<u32>(v.view_h));
                r.w32(L.gs + gScrollX, static_cast<u32>(v.scroll_x));
                r.w32(L.gs + gScrollY, static_cast<u32>(v.scroll_y));
                const int k = static_cast<int>(w.pick(static_cast<u32>(bank)));
                const bool interface_sound = w.pick(5) == 0;
                bool ok_orig = true;
                r.w32(L.mgr + 4, v.sound_mode == 2 ? 1u : 0u);  // (the sound mode's 3D switch)
                std::vector<client::SoundEvent> wanted;
                if (interface_sound) {
                    ++ui_calls;
                    ok_orig = r.call(kInterface, 0, {static_cast<u32>(k), 0});
                    if (v.fx_volume != 0 && v.sound_mode != 0) {
                        client::SoundEvent e;
                        e.sample = k;
                        wanted.push_back(e);
                    }
                } else {
                    ++world_calls;
                    // A position around and off the map (16.16), with a height.
                    const i32 pos[3] = {static_cast<i32>((static_cast<i32>(w.pick(static_cast<u32>(v.map_w * 16 + 200))) - 100) * 65536 + static_cast<i32>(w.pick(65536))),
                                        static_cast<i32>(w.pick(300) * 65536 + w.pick(65536)),
                                        static_cast<i32>((static_cast<i32>(w.pick(static_cast<u32>(v.map_h * 16 + 200))) - 100) * 65536 + static_cast<i32>(w.pick(65536)))};
                    r.write(L.strings, pos, sizeof pos);
                    ok_orig = r.call(kWorld, 0, {static_cast<u32>(k), L.strings, 0});
                    const std::array<i32, 3> p = {pos[0], pos[1], pos[2]};
                    auto seen = [&] {
                        int bx = 0, bz = 0;
                        client::world_sound_block(p, bx, bz);
                        if (static_cast<u32>(bx) >= static_cast<u32>(bw) || static_cast<u32>(bz) >= static_cast<u32>(bh)) return false;
                        const std::size_t at = static_cast<std::size_t>(bz) * static_cast<std::size_t>(bw) + static_cast<std::size_t>(bx);
                        return los ? sight[at] != 0 : ((explored[at] >> viewed) & 1) != 0;
                    };
                    client::SoundEvent e;
                    if (client::world_sound(v, p, seen, k, e)) wanted.push_back(e);
                }
                if (!ok_orig) {
                    const auto c = r.crash_info();
                    std::printf("scenario %u world op %d: the original's call failed (code %08X at %08X)\n", seed, op, c.code, c.address);
                    ++mismatches;
                    break;
                }
                // The client's events through its manager.
                for (const client::SoundEvent& e : wanted) {
                    client::SoundRequest q;
                    q.sample = w.samples[static_cast<std::size_t>(e.sample)].get();
                    q.volume = e.volume;
                    q.placed = e.placed;
                    q.pos = e.pos;
                    q.min_distance = e.min_distance;
                    q.max_distance = e.max_distance;
                    w.mixer.play(q, t);
                }
                const std::vector<Event> eo = w.original_events(t), ec = w.client_events();
                const bool bad = !same(eo, ec);
                if (bad || verbose) {
                    std::printf("%s scenario %u world op %d: %s sound %d fx %d mode %d\n", bad ? "MISMATCH" : "  ", seed, op,
                                interface_sound ? "interface" : "world", k, v.fx_volume, v.sound_mode);
                    show("original", eo);
                    show("client  ", ec);
                }
                if (bad) {
                    ++mismatches;
                    break;
                }
            }
        }

        // ---- part 3: the unit notices' queue
        {
            const u32 units = L.data + 0x90000, defs = L.data + 0x91000, cats = L.data + 0x92000, names = L.data + 0x93000,
                      texts = L.data + 0x98000, fixed = L.data + 0x9C000;
            constexpr int kUnits = 6, kCats = 2;
            // Unit types (a name, a sound category) and units (alive or not).
            const char* const type_names[kCats] = {"Commander", "Construction Kbot"};
            std::vector<std::uint8_t> zero(0x249 * kCats, 0);
            r.write(defs, zero.data(), zero.size());
            for (int c = 0; c < kCats; ++c) {
                r.write(defs + static_cast<u32>(c) * 0x249, type_names[c], std::strlen(type_names[c]) + 1);
                const std::uint16_t cat = static_cast<std::uint16_t>(c);
                r.write(defs + static_cast<u32>(c) * 0x249 + 0x20E, &cat, 2);
            }
            std::vector<int> unit_type(kUnits);
            std::vector<bool> alive(kUnits);
            for (int u = 0; u < kUnits; ++u) {
                unit_type[static_cast<std::size_t>(u)] = static_cast<int>(w.pick(kCats));
                alive[static_cast<std::size_t>(u)] = w.pick(6) != 0;
                const u32 at = units + static_cast<u32>(u) * 0x118;
                r.w32(at + 0x92, defs + static_cast<u32>(unit_type[static_cast<std::size_t>(u)]) * 0x249);
                r.w32(at + 0x110, alive[static_cast<std::size_t>(u)] ? 0x10000000u : 0u);
                const std::uint16_t id = static_cast<std::uint16_t>(u + 1);
                r.write(at + 0xA8, &id, 2);
            }
            // Sound categories: per notice a few sounds (or none), named n<c>_<id>_<k>.
            std::vector<std::uint8_t> cz(0x160 * kCats, 0);
            r.write(cats, cz.data(), cz.size());
            int count[kCats][24] = {};
            std::map<u32, std::array<int, 3>> name_at;  // name address -> category, notice, pick
            u32 next_name = names;
            std::vector<std::uint8_t> blank(64 * 4, 0);
            for (int c = 0; c < kCats; ++c)
                for (int id = 1; id < 24; ++id) {
                    const int n = w.pick(4) == 0 ? 0 : 1 + static_cast<int>(w.pick(3));
                    count[c][id] = n;
                    const u32 rec = cats + static_cast<u32>(c) * 0x160 + 0x40 + static_cast<u32>(id) * 12;
                    r.w32(rec, static_cast<u32>(n));
                    if (n == 0) continue;
                    r.w32(rec + 4, next_name);
                    const u32 text_list = texts + (next_name - names);
                    r.w32(rec + 8, text_list);
                    r.write(text_list, blank.data(), static_cast<std::size_t>(n) * 64);
                    for (int k = 0; k < n; ++k) {
                        char nm[64] = {};
                        std::snprintf(nm, sizeof nm, "n%d_%d_%d", c, id, k);
                        r.write(next_name + static_cast<u32>(k) * 64, nm, sizeof nm);
                        name_at[next_name + static_cast<u32>(k) * 64] = {c, id, k};
                    }
                    next_name += static_cast<u32>(n) * 64;
                }
            r.w32(L.gs + gCategories, cats);
            // The notices' fixed texts.
            std::map<int, u32> text_at;
            u32 tp = fixed;
            for (int id = 1; id < 24; ++id)
                if (const char* tx = client::notice_rule(static_cast<u32>(id)).text) {
                    r.write(tp, tx, std::strlen(tx) + 1);
                    text_at[id] = tp;
                    tp += 64;
                }
            // The queue (as its set-up leaves it) and the notices' next-allowed steps.
            std::vector<std::uint8_t> qz(0xB0, 0);
            r.write(L.queue, qz.data(), qz.size());
            r.w32(L.queue + 0xA1, 30);
            r.w32(L.queue + 0xA5, 150);
            r.w32(kNoticeQueuePtr, L.queue);
            for (int id = 0; id < 24; ++id) r.w32(kNoticeNext + static_cast<u32>(id) * 0x18, 0);
            sim::Lcg15 crt;
            crt.set_state(g());
            r.w32(L.rand_state, crt.state());
            u32 tick = w.pick(100);

            struct Host : client::NoticeQueue::Host {
                u32* now = nullptr;
                sim::Lcg15* crt = nullptr;
                std::vector<int>* type;
                std::vector<bool>* alive;
                int (*count)[24];
                int fx = 27, mode = 1;
                std::vector<std::string> events;
                std::uint32_t tick() override { return *now; }
                std::uint32_t rand() override { return crt->next(); }
                int sound_count(std::uint16_t unit, std::uint32_t id) override { return count[(*type)[unit - 1u]][id]; }
                void speak(std::uint16_t unit, std::uint32_t id, int pick) override {
                    char s[80];
                    std::snprintf(s, sizeof s, "sound n%d_%u_%d%s", (*type)[unit - 1u], id, pick, fx != 0 && mode != 0 ? " played" : "");
                    events.push_back(s);
                }
                bool unit_alive(std::uint16_t unit) override { return (*alive)[unit - 1u]; }
                void show(std::uint16_t unit, const std::string& text) override {
                    static const char* const names_[2] = {"Commander", "Construction Kbot"};
                    events.push_back("text " + std::to_string(unit) + " " + names_[(*type)[unit - 1u]] + ": " + text);
                }
            } host;
            host.now = &tick;
            host.crt = &crt;
            host.type = &unit_type;
            host.alive = &alive;
            host.count = count;
            client::NoticeQueue nq;
            client::NoticeQueue::Levels lv;
            auto put_settings = [&] {
                const std::uint8_t c1 = static_cast<std::uint8_t>(lv.unit_chat), c2 = static_cast<std::uint8_t>(lv.unit_chat_text);
                r.write(L.gs + gChat, &c1, 1);
                r.write(L.gs + gChatText, &c2, 1);
                const std::uint16_t bits = static_cast<std::uint16_t>(host.mode | (lv.speech ? 0x40 : 0));
                r.write(L.gs + gSoundBits, &bits, 2);
                r.w32(L.gs + gFxVol, static_cast<u32>(host.fx));
                r.w32(L.gs + gTick, tick);
            };
            put_settings();
            w.log_seen = r.r32(L.cnt);
            for (int op = 0; op < operations; ++op, ++ops_total) {
                const u32 what = w.pick(100);
                std::string desc;
                bool ok_orig = true;
                if (what < 8) {
                    // Settings: speech levels, the speech switch, effect volume and sound mode.
                    lv.unit_chat = static_cast<int>(w.pick(3)) * 5;
                    lv.unit_chat_text = static_cast<int>(w.pick(3)) * 5;
                    lv.speech = w.pick(5) != 0;
                    host.fx = w.pick(6) == 0 ? 0 : 27;
                    host.mode = static_cast<int>(w.pick(5) == 0 ? 0 : 1 + w.pick(2));
                    put_settings();
                    continue;
                }
                if (what < 30) {
                    tick += w.pick(4) == 0 ? w.pick(400) : w.pick(20);
                    r.w32(L.gs + gTick, tick);
                    continue;
                }
                if (what < 70) {
                    const int u = 1 + static_cast<int>(w.pick(kUnits));
                    const u32 id = 1 + w.pick(23);
                    const auto it = text_at.find(static_cast<int>(id));
                    ok_orig = r.call(kQueue, L.queue, {units + static_cast<u32>(u - 1) * 0x118, id, it == text_at.end() ? 0u : it->second});
                    nq.queue(host, lv, static_cast<std::uint16_t>(u), id);
                    desc = "queue " + std::to_string(u) + "/" + std::to_string(id);
                } else if (what < 95) {
                    ok_orig = r.call(kNoticeFrame, 0, {});
                    nq.frame(host, lv);
                    desc = "frame";
                } else {
                    const int u = 1 + static_cast<int>(w.pick(kUnits));
                    ok_orig = r.call(kForget, 0, {units + static_cast<u32>(u - 1) * 0x118});
                    nq.forget(static_cast<std::uint16_t>(u));
                    desc = "forget " + std::to_string(u);
                }
                if (!ok_orig) {
                    const auto c = r.crash_info();
                    std::printf("scenario %u notices op %d (%s): the original's call failed (code %08X at %08X)\n", seed, op, desc.c_str(), c.code, c.address);
                    ++mismatches;
                    break;
                }
                // The original's notices from the log: the sound's path (its name: which
                // sound), the file sound (played), the console text.
                std::vector<std::string> eo;
                const u32 n = r.r32(L.cnt);
                for (u32 i = w.log_seen; i < n; ++i) {
                    LogRec rec{};
                    r.read(L.log + (i & 4095) * 32, &rec, sizeof rec);
                    if (rec.id == kPathCall) {
                        const auto it = name_at.find(rec.a2);
                        char s[80];
                        if (it != name_at.end()) std::snprintf(s, sizeof s, "sound n%d_%d_%d", it->second[0], it->second[1], it->second[2]);
                        else std::snprintf(s, sizeof s, "sound ?%08X", rec.a2);
                        eo.push_back(s);
                    } else if (rec.id == kPlayFileCall) {
                        if (!eo.empty() && static_cast<i32>(rec.a1) == client::kSoundVolumeNormal && rec.a2 == 0) eo.back() += " played";
                        else eo.push_back("file sound at volume " + std::to_string(static_cast<i32>(rec.a1)));
                    } else if (rec.id == kPrintfCall) {
                        char name[64] = {}, text[64] = {};
                        r.read(rec.a2, name, 63);
                        r.read(rec.a3, text, 63);
                        eo.push_back(std::string("text ? ") + name + ": " + text);
                    } else if (rec.id == kConsoleCall) {
                        if (!eo.empty() && eo.back().compare(0, 7, "text ? ") == 0)
                            eo.back() = "text " + std::to_string(rec.a2 & 0xFFFF) + eo.back().substr(6);
                    }
                }
                w.log_seen = n;
                const bool same_events = eo == host.events, same_rand = r.r32(L.rand_state) == crt.state(),
                           same_size = static_cast<int>(r.r32(L.queue + 0x99)) == static_cast<int>(nq.size());
                for (const std::string& e : eo) {
                    notices_spoken += e.compare(0, 6, "sound ") == 0;
                    notice_texts += e.compare(0, 5, "text ") == 0;
                }
                const bool bad = !same_events || !same_rand || !same_size;
                if (bad || verbose) {
                    std::printf("%s scenario %u notices op %d tick %u: %s queue %u/%zu rand %s\n", bad ? "MISMATCH" : "  ", seed, op, tick, desc.c_str(),
                                r.r32(L.queue + 0x99), nq.size(), same_rand ? "same" : "differs");
                    for (const std::string& e : eo) std::printf("    original: %s\n", e.c_str());
                    for (const std::string& e : host.events) std::printf("    client  : %s\n", e.c_str());
                }
                host.events.clear();
                if (bad) {
                    ++mismatches;
                    break;
                }
            }
        }
    }
    std::printf("scenarios %d, operations %zu: %zu starts (%zu restarts, %zu placed), %zu cut-offs; world %zu, interface %zu; notices: %zu sounds, %zu texts\n",
                scenarios, ops_total, starts, restarts, placed, stops, world_calls, ui_calls, notices_spoken, notice_texts);
    std::printf("%zu mismatches\n", mismatches);
    r.stop();
    return mismatches ? 1 : 0;
}
