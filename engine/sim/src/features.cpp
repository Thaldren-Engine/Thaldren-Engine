#include "sim/features.h"

#include "sim/effects.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using S = FeatureSlot;

i32 add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 mul(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }
i16 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }

FeatureSlot* slot_at(FeatureWorld& w, i32 i) {
    if (i < 0 || i >= kFeatureSlots) {
        w.state.fault = "feature slot outside the table";
        return nullptr;
    }
    return &w.state.slots[static_cast<std::size_t>(i)];
}

const FeatureTypeInfo* type_at(FeatureWorld& w, u16 t) {
    if (t >= w.types.size()) {
        w.state.fault = "feature type outside the table";
        return nullptr;
    }
    return &w.types[t];
}

MapCell* cell_index(FeatureWorld& w, i32 index) {
    if (index < 0 || static_cast<std::size_t>(index) >= w.map.cells.size()) {
        w.state.fault = "feature cell outside the map";
        return nullptr;
    }
    return &w.map.cells[static_cast<std::size_t>(index)];
}

i32 index_of(const FeatureWorld& w, const MapCell* c) { return static_cast<i32>(c - w.map.cells.data()); }

// Unlinks slot `i` from whichever list holds it and pushes it onto `list` (0x4232F0).
// A slot without a predecessor is taken off the head of the first list that starts
// with it (active, idle, free), as the original checks them.
void move_to_list(FeatureWorld& w, i32 i, i32& list) {
    FeatureState& st = w.state;
    FeatureSlot* s = slot_at(w, i);
    if (!s) return;
    const i32 next = s->get<i16>(S::kNext), prev = s->get<i16>(S::kPrev);
    if (prev == -1) {
        if (st.active == i) st.active = next;
        else if (st.idle == i) st.idle = next;
        else if (st.free == i) st.free = next;
    } else if (FeatureSlot* p = slot_at(w, prev)) {
        p->set<i16>(S::kNext, static_cast<i16>(next));
    }
    if (next != -1)
        if (FeatureSlot* n = slot_at(w, next)) n->set<i16>(S::kPrev, static_cast<i16>(prev));
    s->set<i16>(S::kPrev, -1);
    s->set<i16>(S::kNext, static_cast<i16>(list));
    list = i;
    const i32 after = s->get<i16>(S::kNext);
    if (after != -1)
        if (FeatureSlot* n = slot_at(w, after)) n->set<i16>(S::kPrev, static_cast<i16>(i));
}

// Takes the first free slot onto the active list (-1 when none is left).
i32 take_slot(FeatureWorld& w) {
    const i32 i = w.state.free;
    if (i == -1) return -1;
    move_to_list(w, i, w.state.active);
    FeatureSlot& s = w.state.slots[static_cast<std::size_t>(i)];
    s.set<u8>(S::kFlags, static_cast<u8>(s.get<u8>(S::kFlags) & ~feature_slot_flags::kBurning));
    return i;
}

// The ground point of a footprint's centre: ((footprint + 2 cell) << 19) in x and z, the terrain there.
Vec3i footprint_centre(const FeatureWorld& w, const FeatureTypeInfo& t, i32 x, i32 z) {
    const i32 px = mul(add(t.footprint_x, mul(x, 2)), 0x80000);
    const i32 pz = mul(add(t.footprint_z, mul(z, 2)), 0x80000);
    const i32 py = static_cast<i32>(static_cast<u32>(w.heights.sample(hi16(px), hi16(pz))) << 16);
    return {px, py, pz};
}

void send_packet(FeatureWorld& w, u8 kind, i32 x, i32 z, bool to_host) {
    const u8 pkt[6] = {0x0F, kind, static_cast<u8>(x), static_cast<u8>(static_cast<u32>(x) >> 8), static_cast<u8>(z),
                       static_cast<u8>(static_cast<u32>(z) >> 8)};
    w.systems.feature_packet(pkt, to_host);
}

// One fire spreads: flammable neighbours within 3 cells, then five cells downwind, each
// catching fire at its spread chance; then the burning type's burn weapon (0x4239C0).
void spread_fire(FeatureWorld& w, u16 type, i32 x, i32 z) {
    auto try_ignite = [&](i32 cx, i32 cz) {
        MapCell* c = w.map.cell(cx, cz);
        if (!c || c->feature >= kFirstSpecialFeature || (c->flags & 1)) return;
        const FeatureTypeInfo* t = type_at(w, c->feature);
        if (!t || !(t->flags & feature_type_flags::kFlammable)) return;
        if (static_cast<i32>(w.rng.next_mod(100)) < static_cast<i32>(t->spread_chance)) ignite_feature(w, cx, cz, false);
    };
    for (i32 cz = z - 3; cz <= z + 3; ++cz)
        for (i32 cx = x - 3; cx <= x + 3; ++cx)
            if (cx != x || cz != z) try_ignite(cx, cz);
    // Downwind: steps of twice the wind velocity, in cells (16.16).
    i32 px = static_cast<i32>(static_cast<u32>(x) << 16), pz = static_cast<i32>(static_cast<u32>(z) << 16);
    i32 last_x = x, last_z = z;
    for (int k = 0; k < 5; ++k) {
        px = add(px, mul(w.env.wind[0], 2));
        pz = add(pz, mul(w.env.wind[2], 2));
        const i32 cx = hi16(px), cz = hi16(pz);
        if (cx == last_x && cz == last_z) continue;
        last_x = cx;
        last_z = cz;
        try_ignite(cx, cz);
    }
    const FeatureTypeInfo* t = type_at(w, type);
    if (t && t->burn_weapon) w.systems.area_damage(*t->burn_weapon, footprint_centre(w, *t, x, z));
}

}  // namespace

AnimCursor FeatureSlot::cursor(int off) const {
    AnimCursor c;
    c.frame = get<std::uint16_t>(off);
    c.countdown = get<std::uint16_t>(off + 2);
    c.loop = get<std::uint8_t>(off + 4);
    c.pad[0] = get<std::uint8_t>(off + 5);
    c.pad[1] = get<std::uint8_t>(off + 6);
    c.pad[2] = get<std::uint8_t>(off + 7);
    c.seq = get<std::uint32_t>(off + 8);
    return c;
}

void FeatureSlot::set_cursor(int off, const AnimCursor& c) {
    set<std::uint16_t>(off, c.frame);
    set<std::uint16_t>(off + 2, c.countdown);
    set<std::uint8_t>(off + 4, c.loop);
    set<std::uint8_t>(off + 5, c.pad[0]);
    set<std::uint8_t>(off + 6, c.pad[1]);
    set<std::uint8_t>(off + 7, c.pad[2]);
    set<std::uint32_t>(off + 8, c.seq);
}

void FeatureState::reset() {
    for (int i = 0; i < kFeatureSlots; ++i) {
        FeatureSlot& s = slots[static_cast<std::size_t>(i)];
        s.b.fill(0);
        s.set<std::int16_t>(FeatureSlot::kNext, static_cast<std::int16_t>(i + 1 < kFeatureSlots ? i + 1 : -1));
        s.set<std::int16_t>(FeatureSlot::kPrev, static_cast<std::int16_t>(i - 1));
    }
    active = -1;
    idle = -1;
    free = 0;
    sweep = 0;
    fault.clear();
}

std::int32_t ground_average(const MapGrid& map, const Vec3i& pos) {
    const i32 cx = hi16(pos[0]) / 16, cz = hi16(pos[2]) / 16;
    if (cx < 0 || cx >= map.width || cz < 0 || cz >= map.height) return -1;
    const MapCell& c = map.cells[static_cast<std::size_t>(cz) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(cx)];
    return (static_cast<i32>(c.height_min) + static_cast<i32>(c.height_max)) >> 1;
}

void init_feature_type_cursors(FeatureWorld& w) {
    w.state.type_cursors.assign(w.types.size(), {});
    for (std::size_t i = 0; i < w.types.size(); ++i) {
        const FeatureTypeInfo& t = w.types[i];
        if (!(t.flags & feature_type_flags::kAnimating)) continue;
        for (int k = 0; k < 2; ++k)
            if (t.sequences[static_cast<std::size_t>(k)])
                anim_start(w.state.type_cursors[i][static_cast<std::size_t>(k)], w.anims, t.sequences[static_cast<std::size_t>(k)]);
    }
}

std::int32_t place_feature(FeatureWorld& w, std::int32_t cell, std::uint16_t type, const Vec3i* pos,
                           const std::array<std::int16_t, 3>* angles, std::uint8_t owner) {
    if (type == kNoFeature) return -1;
    MapCell* anchor = cell_index(w, cell);
    if (!anchor) return -1;
    if (type == kFeatureVoid) {
        anchor->feature = kFeatureVoid;
        return -1;
    }
    const i32 W = w.map.width;
    const i32 x = static_cast<i16>(cell % W), z = static_cast<i16>(cell / W);
    const FeatureTypeInfo* t = type_at(w, type);
    if (!t) return -1;
    if (W < x + t->footprint_x) return -1;
    if (z + t->footprint_z > w.map.height) return -1;
    // Whatever stands in the way goes first.
    for (i32 r = 0; r < t->footprint_z; ++r)
        for (i32 c = 0; c < t->footprint_x; ++c) {
            const i32 ci = cell + W * r + c;
            MapCell* mc = cell_index(w, ci);
            if (!mc) return -1;
            if (mc->feature != kNoFeature && !remove_feature(w, ci, false)) return -1;
        }
    i32 slot = -1;
    Vec3i at{};
    if (!(t->flags & feature_type_flags::kFlat)) {
        slot = take_slot(w);
        if (slot < 0) return -1;
        FeatureSlot& s = w.state.slots[static_cast<std::size_t>(slot)];
        s.set<u16>(S::kType, type);
        s.set<u16>(S::kDamage, 0);
        s.set<i16>(S::kCellX, static_cast<i16>(x));
        s.set<i16>(S::kCellZ, static_cast<i16>(z));
        at = pos ? *pos : footprint_centre(w, *t, x, z);
        for (int k = 0; k < 3; ++k) s.set<i32>(S::kPos + 4 * k, at[static_cast<std::size_t>(k)]);
        for (int k = 0; k < 3; ++k) s.set<i16>(S::kAngles + 2 * k, angles ? (*angles)[static_cast<std::size_t>(k)] : i16{0});
        s.set<u32>(S::kObject, w.systems.create_object(t->model));
        anchor->feature = type;
        anchor->feature_aux = static_cast<u16>(slot);
        anchor->flags = static_cast<u8>(anchor->flags | 1);
    } else {
        anchor->feature = type;
        anchor->feature_aux = 0;
        anchor->flags = static_cast<u8>(anchor->flags & ~1);
    }
    anchor->flags = static_cast<u8>(((owner & 0xF) << 3) | (anchor->flags & 0x87));
    for (i32 r = 0; r < t->footprint_z; ++r)
        for (i32 c = 0; c < t->footprint_x; ++c) {
            if (r == 0 && c == 0) continue;
            MapCell* mc = cell_index(w, cell + W * r + c);
            if (!mc) continue;
            mc->feature = kFeaturePart;
            mc->feature_aux = static_cast<u16>((r & 0xFF) | ((c & 0xFF) << 8));
            mc->flags = static_cast<u8>(mc->flags & ~1);
        }
    if (t->flags & feature_type_flags::kGeothermal) w.systems.feature_steam(pos ? *pos : footprint_centre(w, *t, x, z));
    w.systems.update_passability(static_cast<i16>(x), static_cast<i16>(z), t->footprint_x, t->footprint_z);
    return slot;
}

bool remove_feature(FeatureWorld& w, std::int32_t cell, bool force) {
    MapCell* c = cell_index(w, cell);
    if (!c) return false;
    const i32 W = w.map.width;
    if (c->feature == kFeaturePart) {
        cell -= (c->feature_aux & 0xFF) * W + (c->feature_aux >> 8);
        c = cell_index(w, cell);
        if (!c) return false;
    }
    if (c->feature > 0xFFFA) return false;
    const FeatureTypeInfo* t = type_at(w, c->feature);
    if (!t) return false;
    if (!force && (t->flags & feature_type_flags::kIndestructible)) return false;
    if (c->flags & 1) {
        const i32 si = c->feature_aux;
        if (FeatureSlot* s = slot_at(w, si)) {
            if (!(t->flags & feature_type_flags::kFlat)) {
                w.systems.free_object(s->get<u32>(S::kObject));
                s->set<u32>(S::kObject, 0);
            }
            move_to_list(w, si, w.state.free);
        }
    }
    c->feature = kNoFeature;
    c->flags = static_cast<u8>(c->flags & ~1);
    for (i32 r = 0; r < t->footprint_z; ++r)
        for (i32 k = 0; k < t->footprint_x; ++k) {
            MapCell* mc = cell_index(w, cell + W * r + k);
            if (!mc || mc->feature != kFeaturePart) continue;
            mc->flags = static_cast<u8>(mc->flags & ~1);
            mc->feature = kNoFeature;
        }
    w.systems.update_passability(static_cast<i16>(cell % W), static_cast<i16>(cell / W), t->footprint_x, t->footprint_z);
    return true;
}

void replace_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool reclaimed) {
    MapCell* c = w.map.cell(x, z);
    if (!c || c->feature >= kFirstSpecialFeature) return;
    const FeatureTypeInfo* t = type_at(w, c->feature);
    if (!t) return;
    u16 next = reclaimed ? t->reclamate : t->dead;
    const i32 ci = index_of(w, c);
    if (c->flags & 1) {
        FeatureSlot* s = slot_at(w, c->feature_aux);
        if (!s) return;
        if (s->get<u8>(S::kFlags) & feature_slot_flags::kReclaimAnim) next = t->reclamate;
        remove_feature(w, ci, false);
        // The next stage takes the old slot's place and attitude (whatever those bytes hold).
        const Vec3i pos = s->pos();
        const std::array<i16, 3> angles = {s->get<i16>(S::kAngles), s->get<i16>(S::kAngles + 2), s->get<i16>(S::kAngles + 4)};
        place_feature(w, ci, next, &pos, &angles, 10);
        return;
    }
    remove_feature(w, ci, false);
    place_feature(w, ci, next, nullptr, nullptr, 10);
}

void kill_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool reclaimed) {
    MapCell* c = w.map.cell(x, z);
    if (!c) {
        w.state.fault = "feature destroyed outside the map";  // the original dereferences no cell
        return;
    }
    if (c->feature == kFeaturePart) {
        z -= c->feature_aux & 0xFF;
        x -= c->feature_aux >> 8;
        c = w.map.cell(x, z);
        if (!c) {
            w.state.fault = "feature destroyed outside the map";
            return;
        }
    }
    if (c->feature >= kFirstSpecialFeature) return;
    const FeatureTypeInfo* t = type_at(w, c->feature);
    if (!t) return;
    u32 seq = 0, shadow = 0;
    if (t->flags & feature_type_flags::kFlat) {
        seq = t->sequences[reclaimed ? kSeqReclaim : kSeqDie];
        shadow = t->sequences[reclaimed ? kSeqReclaimShadow : kSeqDieShadow];
    }
    if (seq == 0) {
        replace_feature(w, x, z, reclaimed);
        return;
    }
    if (c->flags & 1) return;  // already animating (burning): nothing happens
    const i32 i = take_slot(w);
    if (i < 0) return;
    FeatureSlot& s = w.state.slots[static_cast<std::size_t>(i)];
    s.set<u16>(S::kType, c->feature);
    u8 f = static_cast<u8>((s.get<u8>(S::kFlags) & ~feature_slot_flags::kReclaimed) | (reclaimed ? feature_slot_flags::kReclaimed : 0));
    s.set<u8>(S::kFlags, f);
    c->feature_aux = static_cast<u16>(i);
    c->flags = static_cast<u8>(c->flags | 1);
    AnimCursor a = s.cursor(S::kCursorA);
    anim_start(a, w.anims, seq);
    s.set_cursor(S::kCursorA, a);
    f = s.get<u8>(S::kFlags);
    if (shadow) {
        AnimCursor b = s.cursor(S::kCursorB);
        anim_start(b, w.anims, shadow);
        s.set_cursor(S::kCursorB, b);
        f = static_cast<u8>(f | feature_slot_flags::kHasShadow);
    } else {
        f = static_cast<u8>(f & ~feature_slot_flags::kHasShadow);
    }
    s.set<i16>(S::kCellX, static_cast<i16>(x));
    s.set<u8>(S::kFlags, static_cast<u8>(((reclaimed ? 1 : 0) << 1) | (f & 0xFC)));
    s.set<i16>(S::kCellZ, static_cast<i16>(z));
}

void ignite_feature(FeatureWorld& w, std::int32_t x, std::int32_t z, bool remote) {
    MapCell* c = w.map.cell(x, z);
    if (!c || c->feature >= kFirstSpecialFeature) return;
    const FeatureTypeInfo* t = type_at(w, c->feature);
    if (!t || t->sequences[kSeqBurn] == 0 || (c->flags & 1)) return;
    const i32 i = take_slot(w);
    if (i < 0) return;
    FeatureSlot& s = w.state.slots[static_cast<std::size_t>(i)];
    s.set<u16>(S::kType, c->feature);
    c->feature_aux = static_cast<u16>(i);
    c->flags = static_cast<u8>(c->flags | 1);
    AnimCursor a = s.cursor(S::kCursorA);
    anim_start(a, w.anims, t->sequences[kSeqBurn]);
    s.set_cursor(S::kCursorA, a);
    u8 f = s.get<u8>(S::kFlags);
    if (t->sequences[kSeqBurnShadow]) {
        AnimCursor b = s.cursor(S::kCursorB);
        anim_start(b, w.anims, t->sequences[kSeqBurnShadow]);
        s.set_cursor(S::kCursorB, b);
        f = static_cast<u8>(f | feature_slot_flags::kHasShadow);
    } else {
        f = static_cast<u8>(f & ~feature_slot_flags::kHasShadow);
    }
    s.set<u8>(S::kFlags, static_cast<u8>(f | feature_slot_flags::kBurning));
    s.set<i16>(S::kCellX, static_cast<i16>(x));
    s.set<i16>(S::kCellZ, static_cast<i16>(z));
    const u32 half = static_cast<u32>(t->spark_time >> 1);
    s.set<u8>(S::kFireCountdown, static_cast<u8>(w.rng.next_mod(half) + half));
    s.set<u8>(S::kFlags, static_cast<u8>((s.get<u8>(S::kFlags) & ~feature_slot_flags::kNoSpread) | (remote ? feature_slot_flags::kNoSpread : 0)));
    w.systems.feature_fire_sound(static_cast<i16>(x), static_cast<i16>(z));
    if (!remote) send_packet(w, 0xFE, x, z, false);
}

void damage_feature(FeatureWorld& w, MapCell& cell, std::int32_t x, std::int32_t z, const WeaponType& weapon) {
    if (!w.env.destructible) return;
    if (cell.feature > 0xFFFA) return;
    const FeatureTypeInfo* t = type_at(w, cell.feature);
    if (!t || (t->flags & feature_type_flags::kIndestructible)) return;
    const bool host = w.env.network_role == 1;
    if (w.env.network_role == 2) {
        send_packet(w, weapon.index, x, z, true);  // the host decides
        return;
    }
    bool destroyed = false;
    bool slot_path = false;
    if (!(t->flags & feature_type_flags::kFlammable) || weapon.fire_starter == 0) {
        if (!(cell.flags & 1)) {
            const u32 sum = static_cast<u32>(weapon.default_damage) + cell.feature_aux;
            if (sum < t->damage) {
                cell.feature_aux = static_cast<u16>(sum);
            } else {
                kill_feature(w, x, z, false);
                destroyed = true;
            }
        } else {
            slot_path = true;
        }
    } else if (!(cell.flags & 1)) {
        ignite_feature(w, x, z, false);
    } else {
        slot_path = true;
    }
    if (slot_path && !(t->flags & feature_type_flags::kFlat)) {
        FeatureSlot* s = slot_at(w, cell.feature_aux);
        if (!s || s->get<i16>(S::kCellX) != x || s->get<i16>(S::kCellZ) != z) return;
        const u16 taken = static_cast<u16>(s->get<u16>(S::kDamage) + weapon.default_damage);
        s->set<u16>(S::kDamage, taken);
        if (t->damage <= taken) {
            kill_feature(w, s->get<i16>(S::kCellX), s->get<i16>(S::kCellZ), false);
            destroyed = true;
        }
    }
    if (host && destroyed) send_packet(w, 0xFD, x, z, false);
}

void apply_feature_packet(FeatureWorld& w, const std::uint8_t* p, const std::vector<const WeaponType*>& weapons) {
    const i32 x = p[2] | (p[3] << 8), z = p[4] | (p[5] << 8);
    switch (p[1]) {
    case 0xFD: kill_feature(w, x, z, false); break;
    case 0xFE: ignite_feature(w, x, z, true); break;
    case 0xFF: kill_feature(w, x, z, true); break;
    default: {
        MapCell* c = w.map.cell(x, z);
        const WeaponType* wt = p[1] < weapons.size() ? weapons[p[1]] : nullptr;
        if (!c || !wt) {
            w.state.fault = "feature damage packet outside the map";
            return;
        }
        damage_feature(w, *c, x, z, *wt);
        break;
    }
    }
}

void step_features(FeatureWorld& w) {
    FeatureState& st = w.state;
    // Type-wide animations of animating feature types.
    if (st.type_cursors.size() < w.types.size()) st.type_cursors.resize(w.types.size());
    for (std::size_t i = 0; i < w.types.size(); ++i)
        if (w.types[i].flags & feature_type_flags::kAnimating) {
            anim_advance(st.type_cursors[i][0], w.anims);
            anim_advance(st.type_cursors[i][1], w.anims);
        }
    // The reproduction sweep: one cell per step, from the last cell backwards.
    const i32 W = w.map.width, H = w.map.height;
    if (--st.sweep < 0) {
        st.sweep = W * H - 1;
    } else if (MapCell* c = cell_index(w, st.sweep)) {
        if (c->feature < kFirstSpecialFeature && !(c->flags & 1)) {
            const FeatureTypeInfo* t = type_at(w, c->feature);
            if (t && static_cast<i32>(w.rng.next_mod(100)) < static_cast<i32>(t->reproduce)) {
                const u32 area = t->reproduce_area;
                const u32 rx = w.rng.next_mod(area), rz = w.rng.next_mod(area);
                // The row is the sweep index divided by the map height (the width would be right).
                MapCell* to = w.map.cell(st.sweep % W + static_cast<i32>(rx - (area >> 1)),
                                         st.sweep / H + static_cast<i32>(rz - (area >> 1)));
                if (to && c->unit == 0 && to->feature == kNoFeature) place_feature(w, index_of(w, to), c->feature, nullptr, nullptr, 10);
            }
        }
    }
    // Active slots, following each slot's link as it was before the slot was handled.
    const u32 tick = w.env.game_tick;
    i32 i = st.active;
    while (i != -1) {
        FeatureSlot* s = slot_at(w, i);
        if (!s) break;
        const i32 next = s->get<i16>(S::kNext);
        const u16 type = s->get<u16>(S::kType);
        const FeatureTypeInfo* t = type_at(w, type);
        if (!t) break;
        if (!(t->flags & feature_type_flags::kFlat)) {
            Vec3i v = s->vel();
            if (v[0] == 0 && v[2] == 0 && v[1] == 0) {
                move_to_list(w, i, st.idle);  // at rest
                i = next;
                continue;
            }
            Vec3i p = s->pos();
            for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(k)] = add(p[static_cast<std::size_t>(k)], v[static_cast<std::size_t>(k)]);
            for (int k = 0; k < 3; ++k) s->set<i32>(S::kPos + 4 * k, p[static_cast<std::size_t>(k)]);
            const i32 g = ground_average(w.map, p);
            if (mul(g, 0x10000) < p[1]) {
                if (p[1] < static_cast<i32>(static_cast<u32>(w.env.sea_level) << 16)) {
                    s->set<i32>(S::kVel, 0);
                    s->set<i32>(S::kVel + 4, -0x2CCC);  // sinking
                    s->set<i32>(S::kVel + 8, 0);
                } else {
                    s->set<i32>(S::kVel + 4, add(v[1], -w.env.gravity));
                }
            } else {
                s->set<i32>(S::kPos + 4, mul(g, 0x10000));
                for (int k = 0; k < 3; ++k) s->set<i32>(S::kVel + 4 * k, 0);
            }
        } else if (!(s->get<u8>(S::kFlags) & feature_slot_flags::kBurning)) {
            AnimCursor a = s->cursor(S::kCursorA);
            anim_advance(a, w.anims);
            s->set_cursor(S::kCursorA, a);
            if (s->get<u8>(S::kFlags) & feature_slot_flags::kHasShadow) {
                AnimCursor b = s->cursor(S::kCursorB);
                anim_advance(b, w.anims);
                s->set_cursor(S::kCursorB, b);
            }
            if (s->get<u32>(S::kCursorA + 8) == 0) replace_feature(w, s->get<i16>(S::kCellX), s->get<i16>(S::kCellZ), false);
        } else {
            const i32 cx = s->get<i16>(S::kCellX), cz = s->get<i16>(S::kCellZ);
            if (tick % 3 == 0) {
                // Smoke from a random point of the current flame frame (C runtime rand()).
                const Vec3i base = footprint_centre(w, *t, cx, cz);
                const i32 ground = base[1] >> 16;
                const AnimFrame* fr = anim_frame(s->cursor(S::kCursorA), w.anims);
                const AnimFrame none{};
                if (!fr) {
                    w.state.fault = "burning feature with no flame frame (the original reads through a null frame)";
                    fr = &none;
                }
                const u16 fw = fr->width, fh = fr->height;
                const i32 r1 = static_cast<i32>(w.crt_rand.next());
                const i16 dx = static_cast<i16>((static_cast<i64>(r1) * static_cast<i32>(fw >> 1)) / 0x8000);
                const u16 sx = static_cast<u16>(static_cast<u16>(hi16(base[0])) + dx - static_cast<u16>(fr->x_offset) + (fw >> 2));
                const i32 r2 = static_cast<i32>(w.crt_rand.next());
                const i16 dy = static_cast<i16>((static_cast<i64>(r2) * static_cast<i32>(fh >> 1)) / 0x8000);
                const u16 sy = static_cast<u16>(static_cast<u16>(ground) + static_cast<u16>(fr->y_offset - dy) * 2 - (fh >> 2) * 2);
                const Vec3i puff = {static_cast<i32>((static_cast<u32>(sx) << 16) | (static_cast<u32>(base[0]) & 0xFFFF)),
                                    static_cast<i32>(static_cast<u32>(sy) << 16), base[2]};
                w.systems.feature_smoke(puff);
            }
            AnimCursor a = s->cursor(S::kCursorA);
            anim_advance(a, w.anims);
            s->set_cursor(S::kCursorA, a);
            if (s->get<u8>(S::kFlags) & feature_slot_flags::kHasShadow) {
                AnimCursor b = s->cursor(S::kCursorB);
                anim_advance(b, w.anims);
                s->set_cursor(S::kCursorB, b);
            }
            if (s->get<u32>(S::kCursorA + 8) == 0) {
                // Burnt out: the burnt type replaces it.
                if (MapCell* c = w.map.cell(cx, cz)) {
                    const u16 burnt = t->burnt;
                    const i32 ci = index_of(w, c);
                    remove_feature(w, ci, false);
                    if (burnt != kNoFeature) place_feature(w, ci, burnt, nullptr, nullptr, 10);
                }
            } else {
                const u8 left = s->get<u8>(S::kFireCountdown);
                if (left != 0 && !(s->get<u8>(S::kFlags) & feature_slot_flags::kNoSpread)) {
                    s->set<u8>(S::kFireCountdown, static_cast<u8>(left - 1));
                    if (left - 1 == 0) spread_fire(w, type, cx, cz);
                }
            }
        }
        i = next;
    }
}

void spawn_wreck(FeatureWorld& w, const Unit& unit, const WreckSource& src, std::int32_t level, bool burn) {
    u16 corpse = src.corpse;
    while (level >= 2) {
        if (corpse > 0xFFFA) return;
        --level;
        const FeatureTypeInfo* t = type_at(w, corpse);
        if (!t) return;
        corpse = t->dead;
    }
    if (corpse >= kFirstSpecialFeature) return;
    MapCell* c = w.map.cell(unit.cell_x, unit.cell_z);
    if (!c) return;
    const std::array<i16, 3> angles = {unit.pitch, static_cast<i16>(unit.heading), unit.bank};
    const i32 ground = w.heights.sample(hi16(unit.pos[0]), hi16(unit.pos[2]));
    const i32 ci = index_of(w, c);
    if (static_cast<i32>(w.env.sea_level) < ground) {
        place_feature(w, ci, corpse, &unit.pos, &angles, src.owner);
    } else {
        const i32 slot = place_feature(w, ci, corpse, &unit.pos, &angles, src.owner);
        if (slot >= 0) {
            if (!src.feature_unit) {
                FeatureSlot& s = w.state.slots[static_cast<std::size_t>(slot)];
                s.set<i32>(S::kVel + 4, -0x2CCC);  // sinks; the x velocity is left as it was
                s.set<i32>(S::kVel + 8, 0);
            }
            burn = false;
        }
    }
    if (burn) w.systems.wreck_fire(unit.pos);
}

Vec3i feature_slot_pos(const FeatureState& s, std::uint16_t slot) {
    if (slot >= kFeatureSlots) return {};
    return s.slots[slot].pos();
}

void mark_map_edges(MapGrid& map, std::uint8_t sea_level, bool lava_world) {
    constexpr u16 kEdge = 0xFFFD;
    auto mark = [](MapCell& c) {
        if (c.feature == 0xFFFF || c.feature == 0xFFFE) c.feature = kEdge;
    };
    const i32 w = map.width, h = map.height;
    // The last two columns.
    for (i32 z = 0; z < h && w >= 2; ++z) {
        mark(*map.cell(w - 2, z));
        mark(*map.cell(w - 1, z));
    }
    // Top: while the ground stands above the top edge.
    for (i32 x = 0; x < w; ++x)
        for (i32 z = 0; z < h; ++z) {
            MapCell& c = *map.cell(x, z);
            if (z * 16 - (c.height >> 1) >= 0) break;
            mark(c);
        }
    // Bottom: the cell above each one whose ground is below the visible height.
    const i32 visible = h * 16 - 128;
    for (i32 x = 0; x < w; ++x)
        for (i32 z = h - 1; z >= 1; --z) {
            if (z * 16 - (map.cell(x, z)->height >> 1) <= visible) break;
            mark(*map.cell(x, z - 1));
        }
    if (lava_world)
        for (MapCell& c : map.cells)
            if (c.height_min <= sea_level) mark(c);
}

} // namespace sim
