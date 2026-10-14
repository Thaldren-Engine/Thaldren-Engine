#include "sim/effects.h"

#include <cmath>

#include "sim/x87.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

i32 add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 sub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 mul(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }
i16 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }
i32 sea16(const EffectsWorld& w) { return static_cast<i32>(static_cast<u32>(w.env.sea_level) << 16); }

// A debris piece is given `payload` bytes of the ring buffer; returns the payload offset or -1.
i32 cache_alloc(EffectsState& st, i32 slot, i32 payload) {
    DebrisCache& c = st.cache;
    if (payload < 1) return -1;
    const i32 need = payload + 8;
    if (kDebrisCacheSize < need) return -1;
    auto evict = [&](i32 at, const DebrisCache::Block& b) {
        if (b.owner >= 0 && b.owner < kMaxDebris && st.debris_block[static_cast<std::size_t>(b.owner)] == at + 8)
            st.debris_block[static_cast<std::size_t>(b.owner)] = -1;
    };
    auto block_at = [&](i32 at) -> DebrisCache::Block* {
        auto it = c.blocks.find(at);
        if (it == c.blocks.end() || it->second.size <= 0) {
            st.fault = "debris cache walked off its blocks";
            return nullptr;
        }
        return &it->second;
    };
    i32 start = c.cursor;
    if (kDebrisCacheSize < c.cursor + 8 + payload) {
        // Not enough room before the end: everything after the cursor goes, then start over.
        for (i32 q = c.cursor; q < kDebrisCacheSize;) {
            DebrisCache::Block* b = block_at(q);
            if (!b) return -1;
            evict(q, *b);
            q += b->size;
        }
        start = 0;
    }
    i32 sum = 0;
    for (i32 q = start; sum < need;) {
        DebrisCache::Block* b = block_at(q);
        if (!b) return -1;
        evict(q, *b);
        sum += b->size;
        q += b->size;
    }
    // The walked blocks become one; a large enough rest stays a block of its own.
    c.blocks.erase(c.blocks.lower_bound(start), c.blocks.lower_bound(start + sum));
    i32 size = sum;
    i32 next;
    if (static_cast<u32>(sum - need) < 9) {
        next = start + sum;
        if (next >= kDebrisCacheSize) next = 0;
    } else {
        next = start + need;
        c.blocks[next] = {-1, sum - need};
        size = need;
    }
    c.cursor = next;
    c.blocks[start] = {slot, size};
    return start + 8;
}

// x87 single-precision vector helpers of the original (each result stored as a float).
struct V3f {
    float x, y, z;
};
V3f vsub(const V3f& a, const V3f& b) {  // b - a
    return {static_cast<float>(static_cast<double>(b.x) - a.x), static_cast<float>(static_cast<double>(b.y) - a.y),
            static_cast<float>(static_cast<double>(b.z) - a.z)};
}
V3f vcross(const V3f& a, const V3f& b) {
    return {static_cast<float>(static_cast<double>(a.y) * b.z - static_cast<double>(a.z) * b.y),
            static_cast<float>(static_cast<double>(a.z) * b.x - static_cast<double>(a.x) * b.z),
            static_cast<float>(static_cast<double>(a.x) * b.y - static_cast<double>(a.y) * b.x)};
}
V3f vnormalize(const V3f& v) {
    const double s = (static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y) + static_cast<double>(v.z) * v.z;
    const double len = std::sqrt(s);
    return {static_cast<float>(v.x / len), static_cast<float>(v.y / len), static_cast<float>(v.z / len)};
}
float to_float_units(i32 v) {
    // FILD, times the single constant 0x37800080 (just above 1/65536), stored as a float.
    std::uint32_t bits = 0x37800080u;
    float k;
    std::memcpy(&k, &bits, 4);
    return static_cast<float>(static_cast<double>(v) * static_cast<double>(k));
}

struct ExplodeArgs {
    Unit* unit = nullptr;
    i32 piece = 0;
    std::array<i32, 3> spin{};
    Vec3i vel{};
    i32 ttl = 900;
    i32 shatter = 0;
    u32 flags = 0;
};

// A piece cut into shards: every textured quad (not the selection outline) becomes a world
// effect with a shard, while the table has room (0x421700).
void shatter_piece(EffectsWorld& w, ExplodeArgs& a) {
    EffectsState& st = w.state;
    Unit& u = *a.unit;
    const PosePiece& pc = u.pose.pieces[static_cast<std::size_t>(a.piece)];
    const PoseModel* model = u.pose.model();
    if (!model || pc.node < 0 || static_cast<std::size_t>(pc.node) >= model->nodes.size()) {
        st.fault = "shattered piece without a model";
        return;
    }
    const PoseModelNode& node = model->nodes[static_cast<std::size_t>(pc.node)];
    if (a.shatter == 0) a.shatter = 1;
    for (std::size_t pi = 0; pi < node.primitives.size(); ++pi) {
        if (st.count >= kMaxWorldEffects) return;
        const PosePrimitive& prim = node.primitives[pi];
        if (prim.indices.size() != 4 || prim.flat || node.selection == static_cast<i32>(pi)) continue;
        WorldEffect& e = st.effects[static_cast<std::size_t>(st.count++)];
        e.pos = {add(u.pos[0], pc.xform[0]), add(u.pos[1], pc.xform[1]), add(u.pos[2], pc.xform[2])};
        int si = -1;
        for (int k = 0; k < kMaxShards; ++k)
            if (st.shards[static_cast<std::size_t>(k)].free == 0xFF) {
                si = k;
                break;
            }
        e.shard = si;
        if (si < 0) return;  // no shard left: the effect stays without one, and nothing more is cut
        Shard& sh = st.shards[static_cast<std::size_t>(si)];
        sh.free = 0;
        e.flags = static_cast<u16>((e.flags & ~1u) | ((a.flags >> 5) & 1u));
        Vec3i mv;
        if (w.systems.unit_velocity(u, mv)) e.carry = {mv[0] >> 1, mv[1] >> 1, mv[2] >> 1};
        e.vel[0] = static_cast<i32>((0x50u - w.rng.next_mod(0xA0)) << 9);
        e.vel[2] = static_cast<i32>((0x50u - w.rng.next_mod(0xA0)) << 9);
        e.vel[1] = add(static_cast<i32>((0x50u - w.rng.next_mod(0xA0)) << 9), mul(w.env.gravity, 30));
        for (int k = 0; k < 3; ++k) e.spin[static_cast<std::size_t>(k)] = static_cast<i32>(800u - w.rng.next_mod(0x640));
        e.angle = {0, 0, 0};
        e.art.seq = 0;
        e.glow.seq = 0;
        // The quad twice: front (as stored) and back (reversed).
        for (int k = 0; k < 4; ++k) {
            const std::size_t vi = prim.indices[static_cast<std::size_t>(k)];
            std::array<i32, 3> p{};
            if (vi * 3 + 2 < pc.points.size()) p = {pc.points[vi * 3], pc.points[vi * 3 + 1], pc.points[vi * 3 + 2]};
            else st.fault = "shattered quad index past the piece's points";
            sh.points[static_cast<std::size_t>(k)] = p;
            sh.points[static_cast<std::size_t>(7 - k)] = p;
        }
        // Its facing: the normal of the first three points.
        auto fv = [&](int k) {
            const auto& p = sh.points[static_cast<std::size_t>(k)];
            return V3f{to_float_units(p[0]), to_float_units(p[1]), to_float_units(p[2])};
        };
        const V3f p0 = fv(0), p1 = fv(1), p2 = fv(2);
        const V3f n = vnormalize(vcross(vsub(p1, p2), vsub(p1, p0)));
        u32 r = w.rng.next_mod(200);
        e.vel[0] = add(e.vel[0], mul(static_cast<i32>(r), static_cast<i16>(x87::ftol32(static_cast<double>(n.x) * 512.0))));
        r = w.rng.next_mod(200);
        e.vel[2] = sub(e.vel[2], mul(static_cast<i32>(r), static_cast<i16>(x87::ftol32(static_cast<double>(n.z) * 512.0))));
        const i32 nx = x87::ftol32(static_cast<double>(n.x) * 65535.0), ny = x87::ftol32(static_cast<double>(n.y) * 65535.0),
                  nz = x87::ftol32(static_cast<double>(n.z) * 65535.0);
        for (int k = 4; k < 8; ++k) {
            auto& p = sh.points[static_cast<std::size_t>(k)];
            p[0] = sub(p[0], mul(nx, a.shatter));
            p[1] = sub(p[1], mul(ny, a.shatter));
            p[2] = sub(p[2], mul(nz, a.shatter));
        }
        // Centred on its own middle.
        i32 sx = 0, sy = 0, sz = 0;
        for (const auto& p : sh.points) {
            sx = add(sx, p[0]);
            sy = add(sy, p[1]);
            sz = add(sz, p[2]);
        }
        sx /= 8;
        sy /= 8;
        sz /= 8;
        for (auto& p : sh.points) {
            p[0] = sub(p[0], sx);
            p[1] = sub(p[1], sy);
            p[2] = sub(p[2], sz);
        }
        sh.model = model;
        sh.node = pc.node;
        sh.primitive = static_cast<int>(pi);
        sh.owner = w.systems.unit_owner(u);
    }
}

// The piece is hidden and either shattered or copied into a falling debris piece (0x421620).
void throw_piece(EffectsWorld& w, ExplodeArgs& a) {
    EffectsState& st = w.state;
    Unit& u = *a.unit;
    PosePiece& pc = u.pose.pieces[static_cast<std::size_t>(a.piece)];
    pc.flags = static_cast<std::uint16_t>(pc.flags & ~pose_flags::kVisible);
    if (a.flags & debris_flags::kShatter) {
        shatter_piece(w, a);
        return;
    }
    int slot = -1;
    for (int k = 0; k < kMaxDebris; ++k)
        if (st.debris_block[static_cast<std::size_t>(k)] == -1) {
            slot = k;
            break;
        }
    if (slot < 0) return;
    const PoseModel* model = u.pose.model();
    std::size_t vertices = 0;
    if (model && pc.node >= 0 && static_cast<std::size_t>(pc.node) < model->nodes.size())
        vertices = model->nodes[static_cast<std::size_t>(pc.node)].vertices.size();
    const i32 at = cache_alloc(st, slot, static_cast<i32>(vertices * 12 + 0x66));
    if (at < 0) return;
    st.debris_block[static_cast<std::size_t>(slot)] = at;
    DebrisPiece& d = st.debris[static_cast<std::size_t>(slot)];
    d.unit = u.id;
    d.piece = a.piece;
    d.spin = a.spin;
    d.vel = a.vel;
    d.ttl = a.ttl;
    d.shatter = a.shatter;
    d.flags = a.flags;
    d.model = model;
    d.node = pc.node;
    d.move = pc.move;
    d.turn = pc.turn;
    d.pos = {add(pc.xform[0], u.pos[0]), add(pc.xform[1], u.pos[1]), add(pc.xform[2], u.pos[2])};
    d.owner = w.systems.unit_owner(u);
}

// One step of a debris piece; false when it is gone (0x4213B0).
bool step_debris(EffectsWorld& w, DebrisPiece& d) {
    const i32 left = d.ttl;
    d.ttl = left - 1;
    if (left == 0) return false;
    if (sea16(w) < d.pos[1]) {
        const i32 g = ground_average(w.map, d.pos);
        if (add(d.vel[1], d.pos[1]) <= mul(g, 0x10000)) {
            // Bounce: half the speed, up.
            d.vel[1] = -(d.vel[1] >> 1);
            d.vel[0] >>= 1;
            d.vel[2] >>= 1;
            if (d.vel[1] < 0x20000) {
                if (d.flags & debris_flags::kExplodeOnLanding) spawn_world_effect(w, d.pos, w.env.bitmap[0], 0, 0);
                return false;
            }
        }
        for (int k = 0; k < 3; ++k) d.pos[static_cast<std::size_t>(k)] = add(d.pos[static_cast<std::size_t>(k)], d.vel[static_cast<std::size_t>(k)]);
        d.turn[0] = static_cast<i16>(d.turn[0] + static_cast<i16>(d.spin[1]));
        d.turn[1] = static_cast<i16>(d.turn[1] + static_cast<i16>(d.spin[2]));
        d.turn[2] = static_cast<i16>(d.turn[2] + static_cast<i16>(d.spin[0]));
        if (d.flags & debris_flags::kFall) d.vel[1] = sub(d.vel[1], w.env.gravity);
        return true;
    }
    if ((d.flags & debris_flags::kExplodeOnLanding) && !w.env.no_sea_level_trigger)
        spawn_world_effect(w, d.pos, w.env.lava_world ? w.env.lava_splash : w.env.water_splash, -1, 1);
    return false;
}

bool effect_done(const WorldEffect& e) { return e.shard < 0 && e.art.seq == 0 && e.glow.seq == 0; }

}  // namespace

void DebrisCache::reset() {
    blocks.clear();
    blocks[0] = {-1, kDebrisCacheSize};
    cursor = 0;
}

void EffectsState::reset() {
    count = 0;
    for (Shard& s : shards) s.free = 0xFF;
    debris_block.fill(-1);
    cache.reset();
    fault.clear();
}

void spawn_world_effect(EffectsWorld& w, const Vec3i& pos, std::uint32_t seq, std::int32_t glow_index, std::int32_t flags) {
    EffectsState& st = w.state;
    if (st.count >= kMaxWorldEffects) return;
    WorldEffect& e = st.effects[static_cast<std::size_t>(st.count++)];
    e.pos = pos;
    if (seq == 0) e.art.seq = 0;
    else anim_start(e.art, w.anims, seq);
    if (glow_index < 0) e.glow.seq = 0;
    else anim_start(e.glow, w.anims, w.env.glow[static_cast<std::size_t>(glow_index) % 3]);
    if (flags == 0 && static_cast<i16>(w.env.sea_level) < hi16(pos[1])) w.systems.explosion_smoke(pos);
    e.shard = -1;
}

void spawn_impact(EffectsWorld& w, const Vec3i& pos, const WeaponType& weapon, bool water_art, bool in_water) {
    spawn_world_effect(w, pos, water_art ? weapon.water_explosion_seq : weapon.explosion_seq, 0, in_water ? 1 : 0);
}

void explode_piece(EffectsWorld& w, Unit& unit, std::int32_t piece, std::uint32_t type) {
    if (!(type & 0x20)) {
        ExplodeArgs a;
        a.unit = &unit;
        a.piece = piece;
        for (int k = 0; k < 3; ++k) a.spin[static_cast<std::size_t>(k)] = static_cast<i32>(w.rng.next_mod(3000));
        a.vel[0] = static_cast<i32>((20u - w.rng.next_mod(40)) << 14);
        a.vel[1] = static_cast<i32>(w.rng.next_mod(10) << 16);
        a.vel[2] = static_cast<i32>((20u - w.rng.next_mod(40)) << 14);
        a.ttl = 900;
        const bool shatter = (type & 1) != 0;
        a.shatter = shatter ? 1 : 0;
        // Script flags -> debris flags: fire, smoke, shatter, fall, and "explodes on landing"
        // (bit 4 for a falling piece, bit 5 for shards).
        a.flags = ((type >> 4) & 1) | (((type >> 3) & 1) << 1) | (shatter ? 4u : 0u) | (((type >> 2) & 1) << 3) |
                  (((type >> 1) & 1) << (shatter ? 5 : 4));
        if (piece < 0 || static_cast<std::size_t>(piece) >= unit.pose.pieces.size()) {
            w.state.fault = "explode on a piece the unit does not have";  // the original corrupts memory
        } else {
            throw_piece(w, a);
        }
    }
    if (type & 0x3F00) {
        const auto o = piece_local_offset(unit, piece);
        const Vec3i at = {add(unit.pos[0], o[0]), add(unit.pos[1], o[1]), add(unit.pos[2], o[2])};
        for (int k = 0; k < 6; ++k)
            if (type & (0x100u << k)) spawn_world_effect(w, at, w.env.bitmap[static_cast<std::size_t>(k)], 2, 0);
    }
}

void step_effects(EffectsWorld& w) {
    EffectsState& st = w.state;
    for (int k = 0; k < kMaxDebris; ++k) {
        if (st.debris_block[static_cast<std::size_t>(k)] == -1) continue;
        if (!step_debris(w, st.debris[static_cast<std::size_t>(k)])) st.debris_block[static_cast<std::size_t>(k)] = -1;
    }
    // The count is read again each time round: effects added here are stepped too.
    for (i32 k = 0; k < st.count; ++k) {
        WorldEffect& e = st.effects[static_cast<std::size_t>(k)];
        if (e.shard >= 0) {
            const Vec3i old = e.pos;
            e.pos[0] = add(e.pos[0], add(e.carry[0], e.vel[0]));
            e.pos[1] = add(e.pos[1], add(e.carry[1], e.vel[1]));
            e.pos[2] = add(e.pos[2], add(e.vel[2], e.carry[2]));
            e.vel[1] = sub(e.vel[1], w.env.gravity);
            for (int a = 0; a < 3; ++a)
                e.angle[static_cast<std::size_t>(a)] =
                    static_cast<i16>(e.angle[static_cast<std::size_t>(a)] + static_cast<i16>(e.spin[static_cast<std::size_t>(a)]));
            const i32 g = ground_average(w.map, e.pos);
            bool free_it = false;
            if (sea16(w) < e.pos[1] || static_cast<i32>(w.env.sea_level) <= g) {
                if (hi16(e.pos[1]) <= g) {
                    e.pos = old;
                    e.vel[1] = -(e.vel[1] / 2);
                    if (hi16(e.vel[1]) < 1) {
                        if (e.flags & 1) spawn_world_effect(w, e.pos, w.env.bitmap[0], 0, 0);
                        free_it = true;
                    }
                }
            } else {
                if ((e.flags & 1) && !w.env.no_sea_level_trigger)
                    spawn_world_effect(w, e.pos, w.env.lava_world ? w.env.lava_splash : w.env.water_splash, -1, 1);
                free_it = true;
            }
            if (free_it) {
                if (e.shard < kMaxShards) st.shards[static_cast<std::size_t>(e.shard)].free = 0xFF;
                e.shard = -1;
            }
        }
        if (e.art.seq) anim_advance(e.art, w.anims);
        if (e.glow.seq) anim_advance(e.glow, w.anims);
    }
    // Finished effects leave the table; the rest keep their order.
    for (bool removed = true; removed;) {
        removed = false;
        for (i32 k = 0; k < st.count; ++k) {
            if (!effect_done(st.effects[static_cast<std::size_t>(k)])) continue;
            for (i32 j = k; j < st.count - 1; ++j) st.effects[static_cast<std::size_t>(j)] = st.effects[static_cast<std::size_t>(j + 1)];
            --st.count;
            removed = true;
            break;
        }
    }
}

} // namespace sim
