// Network wire format against the original: the bit stream writer and reader of the unit
// state packets (0x415C10 / 0x415DC0), the dictionary coder (0x4D0F60 compress, 0x4D1480
// decompress) and the message envelope (0x44FC10 wrap with the session send replaced by a
// logging stub, 0x44F9C0 unwrap with the session receive replaced by a stub that hands over
// a prepared message). Inputs are random byte strings of several kinds (repetitive, text-like,
// noise, unit-state-like) and random bit fields.
//
// Usage: net_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdio>
#include <cstdlib>
#include <random>

#include "net/codec.h"
#include "remote.h"
#include "sim/bitstream.h"

namespace {

using harness::u32;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kCalloc = 0x004E8EA0, kFree = 0x004E8820, kSessionSend = 0x004C97B0, kSessionReceive = 0x004C9840,
              kStats1 = 0x00415F40, kStats2 = 0x00415EF0;
constexpr u32 kCompress = 0x004D0F60, kDecompress = 0x004D1480, kWrap = 0x0044FC10, kUnwrap = 0x0044F9C0;
constexpr u32 kBitInit = 0x00415B60, kBitPut = 0x00415C10, kBitGet = 0x00415DC0;
constexpr u32 kIatThreadId = 0x004FC190, kIatExchange = 0x004FC174, kIatSetEvent = 0x004FC170;

struct Emitter {
    std::vector<u8> b;
    void u8_(u8 v) { b.push_back(v); }
    void raw(std::initializer_list<int> v) {
        for (int x : v) b.push_back(static_cast<u8>(x));
    }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void align() {
        while (b.size() % 16) b.push_back(0xCC);
    }
};

std::vector<u8> random_data(std::mt19937& g) {
    const int kind = static_cast<int>(g() % 5);
    std::size_t n = g() % 2600;
    if (g() % 6 == 0) n = g() % 40;
    std::vector<u8> d(n);
    switch (kind) {
    case 0:  // noise
        for (auto& c : d) c = static_cast<u8>(g());
        break;
    case 1: {  // few symbols (long matches), signed range included
        const u8 base = static_cast<u8>(g());
        const unsigned k = 1 + g() % 6;
        for (auto& c : d) c = static_cast<u8>(base + g() % k * 37);
        break;
    }
    case 2:  // text-like
        for (auto& c : d) c = static_cast<u8>(" etaoinshrdlu\n"[g() % 14]);
        break;
    case 3: {  // repeated records with small changes (unit state packets)
        std::vector<u8> rec(5 + g() % 30);
        for (auto& c : rec) c = static_cast<u8>(g());
        for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<u8>(rec[i % rec.size()] + (g() % 9 == 0 ? g() % 4 : 0));
        break;
    }
    default:  // zeros and 0x80 bytes
        for (auto& c : d) c = g() % 3 == 0 ? 0x80 : 0;
        break;
    }
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: net_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 4;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 50;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t n_compress = 0, n_decompress = 0, n_wrap = 0, n_packed = 0, n_unwrap = 0, n_bits = 0, n_bitfields = 0, mismatches = 0,
                n_stale = 0;
    std::vector<u8> zeros(0x10000, 0);
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        const u32 gs = r.alloc(0x40000), stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE), heap = r.alloc(0x20000),
                  bump = r.alloc(0x1000), log = r.alloc(0x1000), in_buf = r.alloc(0x10000), out_buf = r.alloc(0x10000),
                  cond = r.alloc(0x1000), recv_src = r.alloc(0x10000), bits = r.alloc(0x2000), unpack = r.alloc(0x10000);
        r.w32(kGameStatePtr, gs);
        Emitter e;
        auto stub_at = [&](u32 target) {
            const u32 at = stubs + static_cast<u32>(e.b.size());
            return std::pair<u32, u32>{at, target};
        };
        std::vector<std::pair<u32, u32>> jumps;
        // calloc(n, size): bump allocation from zeroed memory.
        jumps.push_back(stub_at(kCalloc));
        e.raw({0x8B, 0x44, 0x24, 0x04, 0x0F, 0xAF, 0x44, 0x24, 0x08, 0x83, 0xC0, 0x0F, 0x83, 0xE0, 0xF0});  // eax = n*size rounded
        e.u8_(0x8B); e.u8_(0x0D); e.u32le(bump);                                                         // mov ecx,[bump]
        e.u8_(0x01); e.u8_(0x05); e.u32le(bump);                                                         // add [bump],eax
        e.raw({0x8B, 0xC1, 0xC3});                                                                       // mov eax,ecx; ret
        e.align();
        jumps.push_back(stub_at(kFree));
        e.u8_(0xC3);
        e.align();
        // Session send (dplay, from, to, buffer, size): log buffer and size, return 0.
        jumps.push_back(stub_at(kSessionSend));
        e.raw({0x8B, 0x44, 0x24, 0x10}); e.u8_(0xA3); e.u32le(log);      // buffer
        e.raw({0x8B, 0x44, 0x24, 0x14}); e.u8_(0xA3); e.u32le(log + 4);  // size
        e.raw({0x33, 0xC0, 0xC2, 0x14, 0x00});
        e.align();
        // Session receive (dplay, buffer, size*): copy the prepared message.
        jumps.push_back(stub_at(kSessionReceive));
        e.raw({0x56, 0x57});                                    // push esi; push edi
        e.raw({0x8B, 0x7C, 0x24, 0x10});                        // mov edi,[esp+0x10] buffer
        e.u8_(0xBE); e.u32le(recv_src + 4);                     // mov esi, src
        e.u8_(0x8B); e.u8_(0x0D); e.u32le(recv_src);            // mov ecx,[src len]
        e.raw({0x8B, 0x44, 0x24, 0x14, 0x89, 0x08});            // mov eax,[esp+0x14]; mov [eax],ecx
        e.raw({0xF3, 0xA4, 0x5F, 0x5E, 0x33, 0xC0, 0xC2, 0x0C, 0x00});  // rep movsb; pop; pop; xor; ret 0xc
        e.align();
        jumps.push_back(stub_at(kStats1));
        e.raw({0xC2, 0x0C, 0x00});
        e.align();
        jumps.push_back(stub_at(kStats2));
        e.raw({0xC2, 0x0C, 0x00});
        e.align();
        // Imports: thread id, exchange, set event.
        const u32 f_tid = stubs + static_cast<u32>(e.b.size());
        e.raw({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
        e.align();
        const u32 f_xchg = stubs + static_cast<u32>(e.b.size());
        e.raw({0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x44, 0x24, 0x08, 0x87, 0x01, 0xC2, 0x08, 0x00});
        e.align();
        const u32 f_set = stubs + static_cast<u32>(e.b.size());
        e.raw({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00});
        e.align();
        r.write(stubs, e.b.data(), e.b.size());
        bool ok_setup = true;
        for (auto& [at, target] : jumps) ok_setup = ok_setup && r.patch_jump(target, at);
        ok_setup = ok_setup && r.make_writable(kIatSetEvent, 0x40) && r.w32(kIatThreadId, f_tid) && r.w32(kIatExchange, f_xchg) && r.w32(kIatSetEvent, f_set);
        if (!ok_setup) {
            std::printf("scenario %u: set-up failed\n", seed);
            return 1;
        }
        auto reset_heap = [&]() {
            r.write(heap, zeros.data(), 0x10000);
            r.write(heap + 0x10000, zeros.data(), 0x10000);
            r.w32(bump, heap);
        };
        bool bad = false;
        for (int op = 0; op < ops && !bad; ++op) {
            const int what = static_cast<int>(g() % 4);
            if (what == 0 || what == 1) {
                // Compress, then decompress the original's output with both.
                std::vector<u8> data = random_data(g);
                if (data.empty()) data.push_back(static_cast<u8>(g()));
                r.write(in_buf, data.data(), data.size());
                reset_heap();
                u32 len = 0;
                if (!r.call(kCompress, 0, {out_buf, in_buf, static_cast<u32>(data.size())}, &len) || len > 0xF000) {
                    std::printf("scenario %u op %d: the original's compressor failed\n", seed, op);
                    ++mismatches;
                    bad = true;
                    break;
                }
                std::vector<u8> orig(len);
                r.read(out_buf, orig.data(), len);
                const net::CompressResult mine = net::compress(data.data(), data.size(), orig.empty() ? 0 : orig.back());
                ++n_compress;
                n_stale += mine.trailing_stale;
                std::size_t cmp = orig.size();
                if (mine.trailing_stale && cmp) --cmp;  // the stale stack byte is an input
                bool same = mine.data.size() == orig.size() && std::equal(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(cmp), mine.data.begin());
                if (!same) {
                    std::printf("scenario %u op %d: compressed data differ (size %zu: original %u, engine %zu)\n", seed, op, data.size(), len,
                                mine.data.size());
                    ++mismatches;
                    bad = true;
                    break;
                }
                reset_heap();
                std::vector<u8> back(data.size() + 64, 0xEE);
                r.write(out_buf + 0x8000, back.data(), back.size());
                r.write(in_buf, orig.data(), orig.size());
                u32 dlen = 0;
                if (!r.call(kDecompress, 0, {out_buf + 0x8000, in_buf}, &dlen)) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                std::vector<u8> od(dlen);
                r.read(out_buf + 0x8000, od.data(), dlen);
                ++n_decompress;
                if (od != data || net::decompress(orig.data(), orig.size()) != data) {
                    std::printf("scenario %u op %d: decompression differs\n", seed, op);
                    ++mismatches;
                    bad = true;
                }
            } else if (what == 2) {
                // Envelope: wrap with the original condenser, unwrap with both.
                std::vector<u8> data = random_data(g);
                if (data.size() > 0xAF0) data.resize(0xAF0);
                const bool compression_off = g() % 5 == 0;
                r.w32(gs + 0x4ED, compression_off ? 1 : 0);
                r.w32(gs + 0x37F35, 0);
                r.write(in_buf, data.data(), data.size());
                r.write(cond, zeros.data(), 0x40);
                r.w32(cond + 0x10, in_buf);
                r.w32(cond + 0x18, out_buf);
                const u32 dlen32 = static_cast<u32>(data.size());
                r.write(cond + 0x1D, &dlen32, 4);
                reset_heap();
                r.w32(log, 0);
                r.w32(log + 4, 0);
                if (!r.call(kWrap, cond, {gs + 0x14, 0})) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                const u32 msg_len = r.r32(log + 4);
                std::vector<u8> orig(msg_len);
                if (msg_len) r.read(r.r32(log), orig.data(), msg_len);
                const auto mine = net::wrap(data.data(), data.size(), !compression_off, orig.size() > 3 ? orig.back() : 0);
                ++n_wrap;
                n_packed += !orig.empty() && orig[0] == 4;
                // A compressed body may end in a stale byte (never read): compare all but the last.
                bool same = mine.size() == orig.size() && mine.size() >= 3;
                for (std::size_t i = 0; same && i + 1 < orig.size(); ++i) same = mine[i] == orig[i];
                if (same && !orig.empty() && orig[0] == 3) same = mine.back() == orig.back();
                if (!same) {
                    std::printf("scenario %u op %d: messages differ (frame %zu bytes: original %u, engine %zu)\n", seed, op, data.size(), msg_len,
                                mine.size());
                    ++mismatches;
                    bad = true;
                    break;
                }
                // Unwrap the original's message in the original.
                r.w32(recv_src, msg_len);
                r.write(recv_src + 4, orig.data(), orig.size());
                r.write(cond, zeros.data(), 0x40);
                r.w32(cond + 0, unpack);
                r.w32(gs + 0x14 + 0x4B5, 7);  // the session's "from" id (nonzero: a player's message)
                const u32 dest = out_buf + 0x8000, size_cell = log + 0x10;
                r.w32(size_cell, 0x8000);
                reset_heap();
                u32 rc = 0;
                if (!r.call(kUnwrap, cond, {gs + 0x14, dest, size_cell}, &rc)) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                std::vector<u8> ou(r.r32(size_cell));
                r.read(dest, ou.data(), ou.size());
                std::vector<u8> back;
                const auto res = net::unwrap(orig.data(), orig.size(), back);
                ++n_unwrap;
                // Both refuse a message too short to hold a body (an empty frame).
                const bool refused = rc != 0 && res != net::UnwrapResult::Ok;
                if (!refused && (rc != 0 || res != net::UnwrapResult::Ok || ou != data || back != data)) {
                    std::printf("scenario %u op %d: unwrap differs (rc %08x, frame %zu bytes)\n", seed, op, rc, data.size());
                    ++mismatches;
                    bad = true;
                }
            } else {
                // Bit fields: the original writer and reader vs the engine's.
                if (!r.call(kBitInit, bits, {})) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                sim::BitWriter w;
                std::vector<std::pair<u32, int>> fields;
                u32 total = 0;
                while (total < 7000) {
                    const int nb = 1 + static_cast<int>(g() % 32);
                    const u32 v = g();  // high bits beyond the field are given too
                    if (!r.call(kBitPut, bits, {v, static_cast<u32>(nb)})) {
                        bad = true;
                        break;
                    }
                    w.put(v, nb);
                    fields.push_back({v, nb});
                    total += static_cast<u32>(nb);
                    ++n_bitfields;
                }
                const u32 index = r.r32(bits), bit = r.r32(bits + 4);
                const std::size_t size = index * 4 + ((bit + 7) >> 3);
                std::vector<u8> ob(size);
                r.read(r.r32(bits + 12), ob.data(), size);
                const auto mb = w.bytes();
                ++n_bits;
                bool same = !bad && mb == ob;
                // Read the fields back with the original reader.
                const u32 rd = bits + 0x1800;
                r.w32(rd, r.r32(bits + 12));
                r.w32(rd + 4, 0);
                r.w32(rd + 8, 0);
                sim::BitReader br(mb.data(), mb.size());
                for (auto& [v, nb] : fields) {
                    u32 got = 0;
                    if (!same || !r.call(kBitGet, rd, {static_cast<u32>(nb)}, &got)) break;
                    const u32 m = nb == 32 ? 0xFFFFFFFFu : ((1u << nb) - 1u);
                    same = got == (v & m) && br.get(nb) == got;
                }
                if (!same) {
                    std::printf("scenario %u op %d: bit streams differ\n", seed, op);
                    ++mismatches;
                    bad = true;
                }
            }
        }
    }
    std::printf("scenarios %d: compress %zu (%zu ending in a stale byte), decompress %zu, wrap %zu (%zu compressed), unwrap %zu, bit streams %zu (%zu fields)\n",
                scenarios, n_compress, n_stale, n_decompress, n_wrap, n_packed, n_unwrap, n_bits, n_bitfields);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
