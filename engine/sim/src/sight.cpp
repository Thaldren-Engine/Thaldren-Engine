#include "sim/sight.h"

#include "sim/footprint.h"
#include "sim/targeting.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

i32 wrap_add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wrap_sub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 wrap_mul(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }
i32 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }
// C division by 2^n (truncation towards zero) done the compiler's way: add 2^n - 1 to negatives.
i32 div_pow2(i32 v, int n) { return wrap_add(v, (v >> 31) & ((1 << n) - 1)) >> n; }
// High half of a signed square (the original's 64-bit multiply and 32-bit shift).
i32 hi_square(i32 d) { return static_cast<i32>((static_cast<i64>(d) * static_cast<i64>(d)) >> 32); }

// One sight shape being stamped: the original's query block. The block and frame
// point into the unit (or the lingering slot) and are updated in place.
struct Query {
    int player = 0;  // player record whose counts change
    i16 sight = 0;
    u8 eye = 0;
    i32 x = 0, y = 0, z = 0;
    i16* cell_x = nullptr;
    i16* cell_z = nullptr;
    u8* frame = nullptr;
};

u8 eye_of(const Unit& u) { return u.type ? static_cast<u8>(static_cast<u32>(u.type->model_max_y) >> 16) : 0; }

const SightType* sight_type(const SightWorld& w, const Unit& u) {
    if (u.id >= w.ticks.size()) return nullptr;
    const u16 k = w.ticks[u.id].type_index;
    return k < w.types.size() ? &w.types[k] : nullptr;
}

int owner_of(const SightWorld& w, const Unit& u) { return u.id < w.ticks.size() ? w.ticks[u.id].owner : 0; }

u8 state_of(const SightWorld& w, int player) {
    return player >= 0 && static_cast<std::size_t>(player) < w.players.size() ? w.players[static_cast<std::size_t>(player)].state : 0;
}

Query unit_query(SightWorld& w, Unit& u) {
    Query q;
    q.player = owner_of(w, u);
    const SightType* t = sight_type(w, u);
    q.sight = t ? t->sight : 0;
    q.eye = eye_of(u);
    q.x = u.pos[0];
    q.y = u.pos[1];
    q.z = u.pos[2];
    const i32 floor_y = static_cast<i32>((static_cast<u32>(w.env.sea_level) + 1u) << 16);
    if (q.y < floor_y) q.y = floor_y;
    q.cell_x = &u.los_cell_x;
    q.cell_z = &u.los_cell_z;
    q.frame = &u.los_frame;
    return q;
}

Query lingering_query(SightWorld& w, LingeringSight& e) {
    Query q;
    q.player = e.player;
    q.sight = e.sight;
    q.eye = e.eye;
    q.x = e.pos[0];
    q.y = e.pos[1];
    q.z = e.pos[2];
    q.cell_x = &e.cell_x;
    q.cell_z = &e.cell_z;
    q.frame = &e.frame;
    (void)w;
    return q;
}

PlayerSight* counts_of(SightWorld& w, int player) {
    if (player < 0 || static_cast<std::size_t>(player) >= w.state.players.size()) return nullptr;
    return &w.state.players[static_cast<std::size_t>(player)];
}

void bump(SightWorld& w, PlayerSight* p, i32 index, int delta) {
    if (!p || index < 0 || static_cast<std::size_t>(index) >= p->counts.size()) {
        ++w.faults;  // the original writes outside the player's count map
        return;
    }
    u8& c = p->counts[static_cast<std::size_t>(index)];
    c = static_cast<u8>(c + delta);
}

// The mask frame for a sight distance: distance / 32 - 5, clamped to the table.
int mask_frame(const SightWorld& w, i16 sight) {
    i32 f = div_pow2(sight, 5) - 5;
    if (f < 0) return 0;
    const i32 n = static_cast<i32>(w.tables.masks.size());
    if (f >= n) f = n - 1;
    return f;
}

const SightMask* mask_at(SightWorld& w, int frame) {
    if (frame < 0 || static_cast<std::size_t>(frame) >= w.tables.masks.size()) {
        ++w.faults;  // no such frame: the original dereferences a null frame
        return nullptr;
    }
    return &w.tables.masks[static_cast<std::size_t>(frame)];
}

// The ring a sight distance walks: radius distance / 32 (at least 0), at most the table
// size - 1 (so the last table is never used), taken from the entry before it.
const SightRing* ring_for(SightWorld& w, i16 sight) {
    const i32 r = div_pow2(sight, 5);
    const i32 r0 = r < 0 ? 0 : r;
    const i32 last = static_cast<i32>(static_cast<i16>(static_cast<u16>(w.tables.rings.size()))) - 1;
    const u16 radius = r0 < last ? static_cast<u16>(r0) : static_cast<u16>(last);
    const i32 index = static_cast<i16>(static_cast<u16>(radius - 1));
    if (index == -1) {
        ++w.heap_reads;  // memory in front of the table (heap contents)
        return &w.tables.ring_before_first;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= w.tables.rings.size()) {
        ++w.faults;
        return nullptr;
    }
    return &w.tables.rings[static_cast<std::size_t>(index)];
}

void mark_view_dirty(SightWorld& w, int player) {
    if (state_of(w, player) == w.env.view_player) {
        w.state.flags = static_cast<u16>(w.state.flags & ~sight_flags::kViewClean);
        w.state.view_dirty |= 4;
    }
}

// Adds `delta` to the counts under the query's stored shape (0x482270 / 0x481D50).
void stamp_counts(SightWorld& w, const Query& q, int delta) {
    mark_view_dirty(w, q.player);
    SightState& s = w.state;
    PlayerSight* p = counts_of(w, q.player);
    const i32 fw = p ? p->width : 0;
    const i32 cx = *q.cell_x, cz = *q.cell_z;
    if (s.flags & sight_flags::kTrueSight) {
        if (static_cast<u32>(cx) >= static_cast<u32>(s.width) || static_cast<u32>(cz) >= static_cast<u32>(s.height)) return;
        const SightRing* ring = ring_for(w, q.sight);
        bump(w, p, wrap_add(wrap_mul(fw, cz), cx), delta);
        if (!ring) return;
        const i32 eye = *q.frame;
        for (const SightSlice& slice : *ring) {
            i32 best_n = 0, best_d = -1, k = 1;
            for (const SightPoint& pt : slice) {
                const i32 bx = static_cast<i16>(static_cast<u16>(pt.x + cx));
                const i32 bz = static_cast<i16>(static_cast<u16>(pt.z + cz));
                if (static_cast<u32>(bx) < static_cast<u32>(s.width) && static_cast<u32>(bz) < static_cast<u32>(s.height)) {
                    const std::size_t at = static_cast<std::size_t>(bz) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(bx);
                    const i32 top = at < s.band.size() ? s.band[at][0] : 0;
                    const i32 bottom = at < s.band.size() ? s.band[at][1] : 0;
                    const i32 c = wrap_mul(best_d, k);
                    const i32 a = wrap_mul(top - eye, best_n);
                    if (wrap_sub(a, c) != 0 && c <= a) {
                        bump(w, p, wrap_add(bx, wrap_mul(fw, bz)), delta);
                        const i32 b = wrap_mul(bottom - eye, best_n);
                        if (wrap_sub(b, c) != 0 && c <= b) {
                            best_n = k;
                            best_d = bottom - eye;
                        }
                    }
                }
                ++k;
            }
        }
        return;
    }
    const SightMask* m = mask_at(w, *q.frame);
    if (!m) return;
    i32 cols = m->width;
    if (s.width <= cx + cols) cols = s.width - cx;
    i32 rows = m->height;
    if (s.height <= cz + rows) rows = s.height - cz;
    const i32 x0 = cx < 0 ? -cx : 0;
    const i32 z0 = cz < 0 ? -cz : 0;
    if (!(z0 < rows && x0 < cols)) return;
    for (i32 z = z0; z < rows; ++z)
        for (i32 x = x0; x < cols; ++x)
            if (m->covered[static_cast<std::size_t>(z * m->width + x)]) bump(w, p, (cz + z) * fw + cx + x, delta);
}

bool set_explored(SightWorld& w, i32 index, u16 bit) {
    if (index < 0 || static_cast<std::size_t>(index) >= w.explored.size()) {
        ++w.faults;
        return false;
    }
    u16& e = w.explored[static_cast<std::size_t>(index)];
    if (bit & e) return false;
    e = static_cast<u16>(bit ^ e);
    return true;
}

// Marks the blocks under the query's shape explored for its player (0x481930).
void reveal(SightWorld& w, const Query& q) {
    SightState& s = w.state;
    bool changed = false;
    const u16 bit = static_cast<u16>(1u << (state_of(w, q.player) & 31));
    const i32 cx = *q.cell_x, cz = *q.cell_z;
    const i32 W = s.width, H = s.height;
    if (s.flags & sight_flags::kTrueSight) {
        if (static_cast<u32>(cx) < static_cast<u32>(W) && static_cast<u32>(cz) < static_cast<u32>(H)) {
            const SightRing* ring = ring_for(w, q.sight);
            changed = set_explored(w, wrap_add(wrap_mul(W, cz), cx), bit) || changed;
            if (ring) {
                const i32 eye = *q.frame;
                for (const SightSlice& slice : *ring) {
                    i32 best_n = 0, best_d = -1, k = 1;
                    for (const SightPoint& pt : slice) {
                        const i32 bx = static_cast<i16>(static_cast<u16>(pt.x + cx));
                        const i32 bz = static_cast<i16>(static_cast<u16>(pt.z + cz));
                        if (static_cast<u32>(bx) < static_cast<u32>(W) && static_cast<u32>(bz) < static_cast<u32>(H)) {
                            const std::size_t at = static_cast<std::size_t>(bz) * static_cast<std::size_t>(W) + static_cast<std::size_t>(bx);
                            const i32 top = at < s.band.size() ? s.band[at][0] : 0;
                            const i32 bottom = at < s.band.size() ? s.band[at][1] : 0;
                            const i32 c = wrap_mul(best_d, k);
                            const i32 a = wrap_mul(top - eye, best_n);
                            if (wrap_sub(a, c) != 0 && c <= a) {
                                if (set_explored(w, wrap_add(wrap_mul(W, bz), bx), bit)) changed = true;
                                const i32 b = wrap_mul(bottom - eye, best_n);
                                if (wrap_sub(b, c) != 0 && c <= b) {
                                    best_n = k;
                                    best_d = bottom - eye;
                                }
                            }
                        }
                        ++k;
                    }
                }
            }
        }
    } else {
        // The mask comes from the sight distance again, not from the stored frame.
        const SightMask* m = w.tables.masks.empty() ? nullptr : mask_at(w, mask_frame(w, q.sight));
        if (w.tables.masks.empty()) ++w.faults;
        if (m) {
            i32 cols = m->width;
            if (W <= cx + cols) cols = W - cx;
            i32 rows = m->height;
            if (H <= cz + rows) rows = H - cz;
            const i32 x0 = cx < 0 ? -cx : 0;
            i32 z = cz < 0 ? -cz : 0;
            if (z < rows) {
                do {
                    if (x0 < cols)
                        for (i32 x = x0; x < cols; ++x)
                            if (m->covered[static_cast<std::size_t>(z * m->width + x)] && set_explored(w, (z + cz) * W + x0 + cx + (x - x0), bit))
                                changed = true;
                    ++z;
                } while (z < rows);
            }
        }
    }
    if (changed) mark_view_dirty(w, q.player);
}

// Moves a query's sight to its current position (0x4825B0).
void update_query(SightWorld& w, const Query& q) {
    SightState& s = w.state;
    if ((s.flags & sight_flags::kTrueSight) == sight_flags::kTrueSight) {
        const i32 bx = hi16(q.x) >> 5;
        i32 eye = static_cast<i32>(q.eye) + hi16(q.y);
        if (eye < 0) eye = 0;
        if (eye > 0xFF) eye = 0xFF;
        const i32 diff = static_cast<i32>(*q.frame) - eye;
        const i32 bz = (hi16(q.z) - (eye >> 1)) >> 5;
        const i32 adiff = diff < 0 ? -diff : diff;
        if (static_cast<i32>(*q.cell_x) != bx || static_cast<i32>(*q.cell_z) != bz || adiff > 5) {
            if (*q.frame != 0 && (s.flags & sight_flags::kFog)) stamp_counts(w, q, -1);
            *q.cell_x = static_cast<i16>(bx);
            *q.cell_z = static_cast<i16>(bz);
            if (static_cast<u32>(bx) >= static_cast<u32>(s.width) || static_cast<u32>(bz) >= static_cast<u32>(s.height)) {
                *q.frame = 0;
                return;
            }
            *q.frame = static_cast<u8>(eye);
            if (s.flags & sight_flags::kFog) stamp_counts(w, q, +1);
            if (s.flags & sight_flags::kReveal) reveal(w, q);
        }
        return;
    }
    if (w.tables.masks.empty()) {
        ++w.faults;
        return;
    }
    const int f = mask_frame(w, q.sight);
    const SightMask& m = w.tables.masks[static_cast<std::size_t>(f)];
    const i32 cx = div_pow2(q.x, 21) - m.x_offset;
    const i32 cz = div_pow2(q.z, 21) - div_pow2(hi16(q.y), 6) - m.y_offset;
    if (static_cast<i32>(*q.cell_x) != cx || static_cast<i32>(*q.cell_z) != cz || static_cast<i32>(*q.frame) != f) {
        if ((s.flags & sight_flags::kFog) == sight_flags::kFog) {
            stamp_counts(w, q, -1);
            *q.cell_x = static_cast<i16>(cx);
            *q.cell_z = static_cast<i16>(cz);
            *q.frame = static_cast<u8>(f);
            stamp_counts(w, q, +1);
        } else {
            *q.cell_x = static_cast<i16>(cx);
            *q.cell_z = static_cast<i16>(cz);
            *q.frame = static_cast<u8>(f);
        }
        if (s.flags & sight_flags::kReveal) reveal(w, q);
    }
}

// First stamp of a shape (fog only): frame 0, then with true sight a normal update,
// else the mask placed, counted and revealed (0x482AC0 and its copies).
void first_stamp(SightWorld& w, const Query& q) {
    SightState& s = w.state;
    if ((s.flags & sight_flags::kFog) != sight_flags::kFog) return;
    *q.frame = 0;
    if ((s.flags & sight_flags::kTrueSight) == sight_flags::kTrueSight) {
        update_query(w, q);
        return;
    }
    if (w.tables.masks.empty()) {
        ++w.faults;
        return;
    }
    const int f = mask_frame(w, q.sight);
    const SightMask& m = w.tables.masks[static_cast<std::size_t>(f)];
    *q.cell_x = static_cast<i16>(static_cast<i16>(div_pow2(q.x, 21)) - static_cast<i16>(m.x_offset));
    *q.cell_z = static_cast<i16>(static_cast<i16>(div_pow2(q.z, 21)) - static_cast<i16>(div_pow2(hi16(q.y), 6)) -
                                 static_cast<i16>(m.y_offset));
    *q.frame = static_cast<u8>(f);
    stamp_counts(w, q, +1);
    reveal(w, q);
}

bool block_bit(const SightWorld& w, int viewer, i32 bx, i32 bz) {
    if (viewer < 0 || static_cast<std::size_t>(viewer) >= w.state.players.size()) return false;
    const PlayerSight& p = w.state.players[static_cast<std::size_t>(viewer)];
    if (static_cast<u32>(bx) >= static_cast<u32>(p.width) || static_cast<u32>(bz) >= static_cast<u32>(p.height)) return false;
    const std::size_t at = static_cast<std::size_t>(p.width) * static_cast<std::size_t>(bz) + static_cast<std::size_t>(bx);
    if (w.state.flags & sight_flags::kFog) return at < p.counts.size() && p.counts[at] != 0;
    return at < w.explored.size() && (w.explored[at] & (1u << (w.env.view_player & 31))) != 0;
}

// The block test at a world point (whole x, whole z minus half the height).
bool point_seen(const SightWorld& w, int viewer, i32 x, i32 y, i32 z) {
    return block_bit(w, viewer, hi16(x) >> 5, (hi16(z) - (hi16(y) >> 1)) >> 5);
}

// Every unit of the bucket rows and columns the circle touches whose position lies
// within it (0x47E890).
template <class F> void for_each_in_radius(const UnitBuckets& b, const std::array<i32, 3>& c, i32 radius, F&& f) {
    const i32 W = b.width, H = b.height;
    auto clamp = [](i32 v, i32 n) {
        if (static_cast<u32>(v) >= static_cast<u32>(n)) return v < 0 ? 0 : n - 1;
        return v;
    };
    const i32 x0 = clamp(wrap_sub(c[0], radius) >> 23, W);
    const i32 z0 = clamp(wrap_sub(c[2], radius) >> 23, H);
    const i32 x1 = clamp(wrap_add(c[0], radius) >> 23, W);
    const i32 z1 = clamp(wrap_add(c[2], radius) >> 23, H);
    const i32 r2 = hi_square(radius);
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const std::size_t at = static_cast<std::size_t>(W) * static_cast<std::size_t>(z) + static_cast<std::size_t>(x);
            if (at >= b.heads.size()) continue;
            for (Unit* u = b.heads[at]; u; u = u->cargo_next) {
                const i32 d = wrap_add(hi_square(wrap_sub(u->pos[0], c[0])), hi_square(wrap_sub(u->pos[2], c[2])));
                if (d <= r2) f(*u);
            }
        }
}

bool visible_enemy_near(SightWorld& w, int slot, const std::array<i32, 3>& pos, i32 radius) {
    if (!w.targeting || slot < 0 || static_cast<std::size_t>(slot) >= w.targeting->tables.size() ||
        !w.targeting->tables[static_cast<std::size_t>(slot)].exists) {
        ++w.faults;  // the original reads a missing unit table
        return false;
    }
    const i32 r2 = wrap_mul(radius, radius);
    for (const Unit* e : w.targeting->tables[static_cast<std::size_t>(slot)].seen) {
        const i32 d = wrap_add(hi_square(wrap_sub(pos[0], e->pos[0])), hi_square(wrap_sub(pos[2], e->pos[2])));
        if (d <= r2 && (e->flags & sight_unit_flags::kAlive) && !(e->flags & sight_unit_flags::kDying)) return true;
    }
    return false;
}

}  // namespace

void init_sight_map(SightState& s, std::vector<std::uint16_t>& explored, const HeightMap& heights, std::uint8_t sea_level) {
    const i32 cw = heights.width(), ch = heights.height();
    s.width = cw / 2;
    s.height = ch / 2;
    const std::size_t blocks = (static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) + 7) & ~std::size_t{7};
    s.band.assign(blocks, {0, 0xFF});
    const i32 W = s.width, H = s.height;
    auto raise = [](std::array<u8, 2>* c, i32 v) {
        if (!c) return;
        (*c)[0] = static_cast<u8>(static_cast<i32>((*c)[0]) <= v ? v : (*c)[0]);
        (*c)[1] = static_cast<u8>(static_cast<i32>((*c)[1]) < v ? (*c)[1] : v);
    };
    // Every cell's height projected onto the blocks it shows in on screen (its z moved up by
    // half its height), into the block of its column and the one to the left.
    for (i32 x = 0; x < cw; ++x) {
        std::array<u8, 2>* left = nullptr;
        std::array<u8, 2>* own = nullptr;
        const u32 bx_left = static_cast<u32>((x - 1) >> 1);
        const u32 bx = static_cast<u32>(x >> 1);
        for (i32 z = 0; z < ch; ++z) {
            const i32 h = heights.at(x, z);
            const i32 t = z * 16 - (h >> 1);
            const i32 bz = t >> 5;
            if (bz >= 0) {
                const i32 v = ((bz * 32 + 31) * h) / (t + 31);
                raise(left, v);
                raise(own, v);
                if (bx_left < static_cast<u32>(W) && static_cast<u32>(bz) < static_cast<u32>(H))
                    left = &s.band[static_cast<std::size_t>(bz) * static_cast<std::size_t>(W) + bx_left];
                else
                    left = nullptr;
                if (left) raise(left, v);
                if (bx_left == bx || static_cast<u32>(W) <= bx || static_cast<u32>(H) <= static_cast<u32>(bz))
                    own = nullptr;
                else
                    own = &s.band[static_cast<std::size_t>(bz) * static_cast<std::size_t>(W) + bx];
                if (own) raise(own, v);
            }
            raise(left, h);
            raise(own, h);
        }
    }
    for (auto& c : s.band) {
        u32 top = (static_cast<u32>(c[1]) + static_cast<u32>(c[0]) * 2) / 3;
        u32 bottom = (static_cast<u32>(c[0]) + static_cast<u32>(c[1]) * 2) / 3;
        if (top <= sea_level) top = sea_level;
        if (bottom <= sea_level) bottom = sea_level;
        c[0] = static_cast<u8>(top);
        c[1] = static_cast<u8>(bottom);
    }
    const std::size_t cells = static_cast<std::size_t>(static_cast<u32>(cw * ch) & 0x7FFFFFFFu);
    explored.assign(cells / 4, 0);
    s.lingering_count = 0;
}

void init_player_sight(SightState& s, const std::vector<PlayerSlot>& players) {
    for (std::size_t i = 0; i < players.size() && i < s.players.size(); ++i) {
        if (players[i].controller == 0) continue;
        PlayerSight& p = s.players[i];
        p.allocated = true;
        p.width = s.width;
        p.height = s.height;
        p.counts.assign((static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height) + 7) & ~std::size_t{7}, 0);
    }
}

void rebuild_sight(SightWorld& w, bool clear_explored) {
    SightState& s = w.state;
    if (clear_explored) {
        const u16 fill = (s.flags & sight_flags::kReveal) ? 0 : 0xFFFF;
        for (u16& e : w.explored) e = fill;
    }
    const u8 base = static_cast<u8>((~(s.flags >> 1)) & 1);
    for (std::size_t i = 0; i < 10 && i < w.players.size(); ++i) {
        const PlayerSlot& p = w.players[i];
        if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) continue;
        for (u8& c : s.players[i].counts) c = base;
    }
    for (std::size_t id = 1; id < w.units.size() && id < w.ticks.size(); ++id) {
        if (w.ticks[id].type_index == 0) continue;
        if ((s.flags & sight_flags::kFog) != sight_flags::kFog) continue;
        Unit& u = w.units[id];
        first_stamp(w, unit_query(w, u));
    }
    s.view_dirty |= 4;
    s.flags = static_cast<u16>(s.flags & ~sight_flags::kViewClean);
}

void update_unit_sight(SightWorld& w, Unit& unit) { update_query(w, unit_query(w, unit)); }

void stamp_new_unit(SightWorld& w, Unit& unit) { first_stamp(w, unit_query(w, unit)); }

void linger_dead_unit(SightWorld& w, Unit& unit) {
    SightState& s = w.state;
    if ((s.flags & sight_flags::kFog) != sight_flags::kFog || s.lingering_count >= 20) return;
    LingeringSight& e = s.lingering[static_cast<std::size_t>(s.lingering_count)];
    e.player = w.env.view_player;
    const SightType* t = sight_type(w, unit);
    e.sight = t ? t->sight : 0;
    e.pos = unit.pos;
    e.eye = eye_of(unit);
    const i32 floor_y = static_cast<i32>((static_cast<u32>(w.env.sea_level) + 1u) << 16);
    if (e.pos[1] < floor_y) e.pos[1] = floor_y;
    e.expire_tick = w.env.game_tick + 60;
    first_stamp(w, lingering_query(w, e));
    ++s.lingering_count;
}

void remove_unit_sight(SightWorld& w, Unit& unit) { stamp_counts(w, unit_query(w, unit), -1); }

void expire_lingering_sight(SightWorld& w) {
    SightState& s = w.state;
    bool any = false;
    for (i32 i = 0; i < s.lingering_count; ++i) {
        LingeringSight& e = s.lingering[static_cast<std::size_t>(i)];
        if (e.expire_tick < w.env.game_tick) {
            stamp_counts(w, lingering_query(w, e), -1);
            any = true;
        }
    }
    // (The original reads an uninitialised flag when nothing expired; the compaction below
    // then finds nothing to remove, so the result is the same.)
    if (!any) return;
    i32 out = 0;
    while (out < s.lingering_count && !(s.lingering[static_cast<std::size_t>(out)].expire_tick < w.env.game_tick)) ++out;
    for (i32 i = out + 1; i < s.lingering_count; ++i) {
        const LingeringSight& e = s.lingering[static_cast<std::size_t>(i)];
        if (w.env.game_tick <= e.expire_tick) s.lingering[static_cast<std::size_t>(out++)] = e;
    }
    if (out < s.lingering_count) s.lingering_count = out;
}

bool unit_visible(SightWorld& w, int viewer, const Unit& unit) {
    if (owner_of(w, unit) == viewer) return true;
    if (unit.activate_flags & unit_flags::kCloaked) return false;
    const UnitTypeInfo* t = unit.type;
    const SightType* st = sight_type(w, unit);
    if (!t || !st) {
        ++w.faults;
        return false;
    }
    i32 x = wrap_add(t->model_min_x, unit.pos[0]);
    i32 y = wrap_add(t->model_max_y, unit.pos[1]);
    i32 z = wrap_add(t->model_min_z, unit.pos[2]);
    if (!(unit.flags & sight_unit_flags::kSonar) && y < static_cast<i32>(static_cast<u32>(w.env.sea_level) << 16)) return false;
    if (point_seen(w, viewer, x, y, z)) return true;
    x = wrap_add(x, st->model_size_x);
    if (point_seen(w, viewer, x, y, z)) return true;
    z = wrap_add(z, st->model_size_z);
    y = wrap_sub(y, st->model_size_y);
    if (point_seen(w, viewer, x, y, z)) return true;
    x = wrap_sub(x, st->model_size_x);
    return point_seen(w, viewer, x, y, z);
}

bool position_explored(const SightWorld& w, int owner, const std::array<std::int32_t, 3>& pos) {
    if (owner < 0 || static_cast<std::size_t>(owner) >= w.state.players.size()) return false;
    const PlayerSight& p = w.state.players[static_cast<std::size_t>(owner)];
    const i32 bx = hi16(pos[0]) >> 5, bz = (hi16(pos[2]) - (hi16(pos[1]) >> 1)) >> 5;
    if (static_cast<u32>(bx) >= static_cast<u32>(p.width) || static_cast<u32>(bz) >= static_cast<u32>(p.height)) return false;
    const std::size_t at = static_cast<std::size_t>(p.width) * static_cast<std::size_t>(bz) + static_cast<std::size_t>(bx);
    return at < w.explored.size() && (w.explored[at] & (1u << (w.env.view_player & 31))) != 0;
}

bool block_seen(const SightWorld& w, int viewer, std::int32_t bx, std::int32_t bz) { return block_bit(w, viewer, bx, bz); }

void update_sensors(SightWorld& w) {
    using namespace sight_unit_flags;
    if (w.env.player_count < 2) return;
    const int L = w.env.view_player;
    if (static_cast<std::size_t>(L) >= w.players.size()) {
        ++w.faults;
        return;
    }
    const PlayerSlot& lp = w.players[static_cast<std::size_t>(L)];
    const u8 l_state = lp.state;
    const std::size_t end = w.units.size() < w.ticks.size() ? w.units.size() : w.ticks.size();
    // 1. Marks kept for the viewed player's own units, units of allies sharing radar, and
    // everything while watching; cleared for the rest.
    for (std::size_t id = 1; id < end; ++id) {
        Unit& u = w.units[id];
        if (!(u.flags & kAlive)) continue;
        u.flags &= ~kCloakHold;
        const int owner = w.ticks[id].owner;
        bool keep = owner == L;
        if (!keep && static_cast<std::size_t>(owner) < w.alliances.size() &&
            w.alliances[static_cast<std::size_t>(owner)][l_state & 15] != 0 && w.env.options[static_cast<std::size_t>(owner) % 11].shares_radar)
            keep = true;
        if (!keep && lp.active && w.env.options[static_cast<std::size_t>(L)].watching) keep = true;
        if (keep)
            u.flags |= kRadar | kSonar;
        else
            u.flags &= ~(kRadar | kSonar | kJammed);
    }
    // 2. The viewed player's active radars and sonars mark other players' units.
    for (int id = lp.first_unit; id <= lp.last_unit; ++id) {
        if (id < 0 || static_cast<std::size_t>(id) >= end) {
            ++w.faults;
            continue;
        }
        Unit& u = w.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kAlive) || (u.flags & kDying) || !(u.activate_flags & unit_flags::kActivated)) continue;
        const SightType* t = sight_type(w, u);
        if (!t || (t->radar == 0 && t->sonar == 0)) continue;
        const i32 radar_reach = static_cast<i32>(t->radar) + hi16(u.pos[1]) * 2;
        const i32 radar_sq = wrap_mul(radar_reach, radar_reach);
        const i32 sonar_sq = static_cast<i32>(t->sonar) * static_cast<i32>(t->sonar);
        const i16 reach = t->radar <= t->sonar ? t->sonar : t->radar;
        const std::array<i32, 3> c = u.pos;
        const i32 sea = static_cast<i32>(static_cast<u32>(w.env.sea_level) << 16);
        for_each_in_radius(w.buckets, c, static_cast<i32>(static_cast<u32>(static_cast<i32>(reach)) << 16), [&](Unit& v) {
            if (v.id >= w.ticks.size()) return;
            const UnitTickState& vt = w.ticks[v.id];
            if (vt.type_index == 0 || vt.owner == L) return;
            const SightType* vs = vt.type_index < w.types.size() ? &w.types[vt.type_index] : nullptr;
            if (!vs || vs->stealth) return;
            const i32 d = wrap_add(hi_square(wrap_sub(v.pos[0], c[0])), hi_square(wrap_sub(v.pos[2], c[2])));
            if (v.pos[1] <= sea && d < sonar_sq) v.flags |= kSonar;
            const i32 top = v.type ? wrap_add(v.type->model_max_y, v.pos[1]) : v.pos[1];
            if (sea <= top && d < radar_sq) v.flags |= kRadar;
        });
    }
    // 3. Jammers of the other players clear the marks around them.
    for (std::size_t id = 1; id < end; ++id) {
        Unit& u = w.units[id];
        if (!(u.flags & kAlive) || w.ticks[id].owner == l_state || !(u.activate_flags & unit_flags::kActivated)) continue;
        const SightType* t = sight_type(w, u);
        if (!t) continue;
        const std::array<i32, 3> c = u.pos;
        if (t->radar_jam != 0)
            for_each_in_radius(w.buckets, c, static_cast<i32>(static_cast<u32>(static_cast<i32>(t->radar_jam)) << 16),
                               [](Unit& v) { v.flags = (v.flags & ~kRadar) | kJammed; });
        if (t->sonar_jam != 0)
            for_each_in_radius(w.buckets, c, static_cast<i32>(static_cast<u32>(static_cast<i32>(t->sonar_jam)) << 16),
                               [](Unit& v) { v.flags = (v.flags & ~kSonar) | kJammed; });
    }
    // 4. Cloaking units of playing players with a visible enemy near hold off cloaking.
    for (std::size_t id = 1; id < end; ++id) {
        Unit& u = w.units[id];
        if (!(u.flags & kAlive)) continue;
        const int owner = w.ticks[id].owner;
        if (static_cast<std::size_t>(owner) >= w.players.size()) continue;
        const PlayerSlot& op = w.players[static_cast<std::size_t>(owner)];
        if (!op.active || (op.controller != 1 && op.controller != 2)) continue;
        const SightType* t = sight_type(w, u);
        if (!t || !t->can_cloak) continue;
        if (visible_enemy_near(w, owner, u.pos, t->min_cloak_distance)) {
            u.cloak_holdoff_tick = w.env.game_tick + 90;
            u.flags |= kCloakHold;
        }
    }
    // 5. Whatever the viewed player sees counts as on radar.
    for (std::size_t id = 1; id < end; ++id) {
        Unit& u = w.units[id];
        if (!(u.flags & kAlive) || (u.flags & kRadar) || (u.activate_flags & unit_flags::kCloaked)) continue;
        if (point_seen(w, L, u.pos[0], u.pos[1], u.pos[2])) u.flags |= kRadar;
    }
}

void share_explored(SightWorld& w, std::uint8_t from, std::uint8_t to) {
    const i32 n = static_cast<i32>(static_cast<u32>(w.state.width * 2) * static_cast<u32>(w.state.height * 2));
    const i32 count = div_pow2(n, 2);
    const u16 fb = static_cast<u16>(1u << (from & 31)), tb = static_cast<u16>(1u << (to & 31));
    for (i32 i = 0; i < count && static_cast<std::size_t>(i) < w.explored.size(); ++i)
        if (w.explored[static_cast<std::size_t>(i)] & fb) w.explored[static_cast<std::size_t>(i)] |= tb;
}

}  // namespace sim
