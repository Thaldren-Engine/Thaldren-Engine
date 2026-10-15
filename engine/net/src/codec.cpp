#include "net/codec.h"

#include <array>
#include <memory>

namespace net {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

constexpr int kWindow = 0x1000, kMaxMatch = 17, kRoot = 0x1000;

// Encoder state: the window and a tree node per window position (node 0 exists and can
// pick up stray links, as in the original; the root is node 0x1000).
struct Tree {
    std::array<u8, 0x1011> win{};
    struct Node {
        u16 parent = 0, smaller = 0, larger = 0;
    };
    std::array<Node, 0x1001> n{};

    u16& child(u16 p, bool larger) { return larger ? n[p].larger : n[p].smaller; }
    // Puts q where p is under p's parent.
    void relink_parent(u16 p, u16 q) {
        const u16 par = n[p].parent;
        if (n[par].smaller == p) n[par].smaller = q;
        else n[par].larger = q;
    }
    void remove(u16 p) {
        if (n[p].parent == 0) return;
        if (n[p].larger == 0 || n[p].smaller == 0) {
            const u16 q = n[p].larger == 0 ? n[p].smaller : n[p].larger;
            n[q].parent = n[p].parent;
            const u16 par = n[p].parent;
            if (n[par].larger == p) n[par].larger = q;
            else n[par].smaller = q;
            n[p].parent = 0;
            return;
        }
        u16 q = n[p].smaller;
        while (n[q].larger != 0) q = n[q].larger;
        remove(q);
        relink_parent(p, q);
        n[q] = n[p];
        n[n[q].smaller].parent = q;
        n[n[q].larger].parent = q;
        n[p].parent = 0;
    }
    // Inserts position r; returns the longest match length and sets `pos` (left alone for r = 0).
    int insert(u16 r, u16& pos) {
        if (r == 0) return 0;
        int best = 0;
        u16 p = n[kRoot].larger;
        for (;;) {
            int i = 0;
            int a = 0, b = 0;
            for (; i < kMaxMatch; ++i) {
                a = static_cast<std::int8_t>(win[(p + i) & 0xFFF]);
                b = static_cast<std::int8_t>(win[(r + i) & 0xFFF]);
                if (a != b) break;
            }
            if (best <= i) {
                pos = p;
                best = i;
                if (i > 16) {
                    relink_parent(p, r);
                    n[r] = n[p];
                    n[n[r].smaller].parent = r;
                    n[n[r].larger].parent = r;
                    n[p].parent = 0;
                    return i;
                }
            }
            u16& c = child(p, !(b - a < 0));
            if (c == 0) {
                c = r;
                n[r].parent = p;
                n[r].larger = 0;
                n[r].smaller = 0;
                return best;
            }
            p = c;
        }
    }
};

}  // namespace

CompressResult compress(const std::uint8_t* data, std::size_t size, std::uint8_t stale_byte) {
    auto t = std::make_unique<Tree>();
    t->n[kRoot].parent = 0;
    t->n[kRoot].larger = 1;
    t->n[kRoot].smaller = 0;
    t->n[1].parent = kRoot;
    // Item buffers indexed by the item's flag bit (1, 2, 4, ... 0x80); the literal slot past
    // the last flag bit lies on the original's stack where nothing writes.
    std::array<u8, 0x101> lit{};
    std::array<bool, 0x101> lit_written{};
    std::array<u16, 0x81> code{};
    lit.fill(stale_byte);
    std::size_t in = 0;
    int len = 0;
    while (len < kMaxMatch && in < size) t->win[static_cast<std::size_t>(1 + len++)] = data[in++];
    int r = 1, match = 0;
    u16 match_pos = 0;
    unsigned mask = 1;
    u8 flags = 0;
    CompressResult res;
    auto& out = res.data;
    while (len > 0) {
        if (match > len) match = len;
        int step;
        if (match < 2) {
            lit[mask] = t->win[static_cast<std::size_t>(r)];
            lit_written[mask] = true;
            step = 1;
        } else {
            code[mask] = static_cast<u16>(((match - 2) & 0xF) | (match_pos << 4));
            flags = static_cast<u8>(flags | mask);
            step = match;
        }
        mask <<= 1;
        if (mask & 0x100) {
            out.push_back(flags);
            for (unsigned m = 1; m < 0x100; m <<= 1) {
                if (flags & m) {
                    out.push_back(static_cast<u8>(code[m]));
                    out.push_back(static_cast<u8>(code[m] >> 8));
                } else {
                    out.push_back(lit[m]);
                }
            }
            mask = 1;
            flags = 0;
        }
        for (; step > 0; --step) {
            const u16 s = static_cast<u16>((r + kMaxMatch) & 0xFFF);
            t->remove(s);
            if (in < size) t->win[s] = data[in++];
            else --len;
            r = (r + 1) & 0xFFF;
            if (len != 0) match = t->insert(static_cast<u16>(r), match_pos);
        }
    }
    code[mask] = 0;
    flags = static_cast<u8>(flags | mask);
    int items = 1;
    for (unsigned m = mask; m; m >>= 1) ++items;
    out.push_back(flags);
    for (int k = 0; k < items; ++k) {
        const unsigned m = 1u << k;
        if (m < 0x100 && (flags & m)) {
            out.push_back(static_cast<u8>(code[m]));
            out.push_back(static_cast<u8>(code[m] >> 8));
        } else {
            out.push_back(lit[m]);
            if (k == items - 1) res.trailing_stale = !lit_written[m];
        }
    }
    return res;
}

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size) {
    std::vector<u8> out;
    std::array<u8, 0x1011> win{};
    unsigned r = 1, mask = 1;
    std::size_t pos = 0;
    auto next = [&]() -> u8 { return pos < size ? data[pos++] : 0; };
    if (size == 0) return out;
    u8 flags = next();
    for (;;) {
        if (pos >= size) break;
        if (!(flags & mask)) {
            const u8 b = next();
            out.push_back(b);
            win[r] = b;
            r = (r + 1) & 0xFFF;
        } else {
            const u16 v = static_cast<u16>(next() | (next() << 8));
            const unsigned off = v >> 4, count = (v & 0xF) + 2u;
            if (off == 0) break;
            for (unsigned i = 0; i < count; ++i) {
                const u8 b = win[(off + i) & 0xFFF];
                out.push_back(b);
                win[r] = b;
                r = (r + 1) & 0xFFF;
            }
        }
        mask <<= 1;
        if (mask & 0x100) {
            mask = 1;
            flags = next();
        }
    }
    return out;
}

std::vector<std::uint8_t> wrap(const std::uint8_t* frame, std::size_t size, bool compression, std::uint8_t stale_byte) {
    std::vector<u8> msg;
    bool packed = false;
    if (size >= 13) {
        CompressResult c = compress(frame, size, stale_byte);
        if (c.data.size() + 3 < size && compression) {
            msg.assign(3, 0);
            msg.insert(msg.end(), c.data.begin(), c.data.end());
            packed = true;
        }
    }
    if (!packed) {
        msg.assign(3, 0);
        msg.insert(msg.end(), frame, frame + size);
    }
    u16 sum = 0;
    const std::size_t end = msg.size() >= 3 ? msg.size() - 3 : 0;
    for (std::size_t i = 3; i < end; ++i) {
        msg[i] = static_cast<u8>(msg[i] ^ static_cast<u8>(i));
        sum = static_cast<u16>(sum + msg[i]);
    }
    msg[0] = packed ? kCompressedMessage : kPlainMessage;
    msg[1] = static_cast<u8>(sum);
    msg[2] = static_cast<u8>(sum >> 8);
    return msg;
}

UnwrapResult unwrap(const std::uint8_t* message, std::size_t size, std::vector<std::uint8_t>& out) {
    if (size == 0 || (message[0] != kPlainMessage && message[0] != kCompressedMessage)) {
        out.assign(message, message + size);
        return UnwrapResult::NotWrapped;
    }
    if (size < 4) return UnwrapResult::TooShort;
    std::vector<u8> m(message, message + size);
    const std::size_t end = size - 3;
    u16 sum = 0;
    for (std::size_t i = 3; i < end; ++i) {
        sum = static_cast<u16>(sum + m[i]);
        m[i] = static_cast<u8>(m[i] ^ static_cast<u8>(i));
    }
    if (static_cast<u16>(m[1] | (m[2] << 8)) != sum) return UnwrapResult::BadChecksum;
    if (m[0] == kCompressedMessage) out = decompress(m.data() + 3, size - 3);
    else out.assign(m.begin() + 3, m.end());
    return UnwrapResult::Ok;
}

} // namespace net
