#include "sim/footprint.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
namespace ff = footprint_flags;

void set_fault(FootprintWorld& w, const char* what) {
    if (w.fault.empty()) w.fault = what;
}

// The cell `index` cells after the first one (row-major, as the original walks a
// pointer through the grid; footprints running past the right edge continue on the next row).
MapCell* cell_at(FootprintWorld& w, i32 index) {
    if (index < 0 || static_cast<std::size_t>(index) >= w.map.cells.size()) {
        set_fault(w, "footprint cell outside the map grid");
        return nullptr;
    }
    return &w.map.cells[static_cast<std::size_t>(index)];
}

std::uint8_t yard_byte(FootprintWorld& w, const Unit& u, i32 k) {
    if (!u.type || k < 0 || static_cast<std::size_t>(k) >= u.type->yard_map.size()) {
        set_fault(w, "yard map read past its end");
        return 0;
    }
    return u.type->yard_map[static_cast<std::size_t>(k)];
}

// `self` wants a cell `holder` occupies. The holder keeps it unless its owner is
// a watched player; returns whether the cell goes to `self`.
bool contest(FootprintWorld& w, Unit& self, u16 holder) {
    if (holder >= w.units.size() || holder >= w.ticks.size()) {
        set_fault(w, "cell occupant outside the unit table");
        return false;
    }
    Unit& h = w.units[holder];
    const std::size_t owner = static_cast<std::size_t>(w.ticks[holder].owner);
    const PlayerSlot* p = owner < w.players.size() ? &w.players[owner] : nullptr;
    if (p && p->active && p->controller == 3) {
        h.flags |= ff::kWantsCells;
        self.flags |= ff::kHoldsWanted;
        return true;
    }
    h.flags |= ff::kHoldsWanted;
    self.flags |= ff::kWantsCells;
    return false;
}

void take(FootprintWorld& w, Unit& self, u16& slot) {
    if (slot == 0 || contest(w, self, slot)) slot = self.id;
}

Unit** bucket_head(FootprintWorld& w, i32 bucket) {
    if (bucket == kOffMapBucket) return &w.buckets.off_map;
    if (bucket < 0 || static_cast<std::size_t>(bucket) >= w.buckets.heads.size()) {
        set_fault(w, "spatial bucket outside the bucket grid");
        return nullptr;
    }
    return &w.buckets.heads[static_cast<std::size_t>(bucket)];
}

void unlink_bucket(FootprintWorld& w, Unit& u) {
    Unit** link = bucket_head(w, u.bucket);
    if (!link) return;
    while (*link != &u) {
        if (!*link) {
            set_fault(w, "unit missing from its spatial bucket");
            return;
        }
        link = &(*link)->cargo_next;
    }
    *link = u.cargo_next;
    u.cargo_next = nullptr;
}

// Moves the unit to another bucket list (carried units only record it).
void relink_bucket(FootprintWorld& w, Unit& u, i32 bucket) {
    if (bucket == u.bucket) return;
    if (!u.carrier) {
        if (u.bucket != kNoBucket) unlink_bucket(w, u);
        if (Unit** head = bucket_head(w, bucket)) {
            u.cargo_next = *head;
            *head = &u;
        }
    }
    u.bucket = bucket;
}

// Walks the footprint rows: fn(cell index, yard index) for each cell.
template <class Fn> void each_cell(FootprintWorld& w, i32 first, i16 fx, i16 fz, Fn fn) {
    i32 k = 0;
    for (i32 row = 0; row < fz; ++row)
        for (i32 col = 0; col < fx; ++col) fn(first + row * w.map.width + col, k++);
}

} // namespace

void UnitBuckets::resize_for_map(std::int32_t cells_x, std::int32_t cells_z) {
    // The original rounds the map's world size up to 128-unit squares.
    width = (cells_x + 7) >> 3;
    height = (cells_z + 7) >> 3;
    heads.assign(static_cast<std::size_t>(width > 0 && height > 0 ? width * height : 0), nullptr);
    off_map = nullptr;
}

void build_bucket_heights(UnitBuckets& b, const MapGrid& map, std::uint8_t sea_level) {
    const std::int32_t w = b.width, h = b.height;
    if (w <= 0 || h <= 0) {
        b.top.clear();
        return;
    }
    std::vector<std::uint8_t> ground(static_cast<std::size_t>(w * h), sea_level);
    for (std::int32_t z = 0; z < map.height; ++z)
        for (std::int32_t x = 0; x < map.width; ++x) {
            std::uint8_t& g = ground[static_cast<std::size_t>((z >> 3) * w + (x >> 3))];
            const std::uint8_t top = map.cells[static_cast<std::size_t>(z * map.width + x)].height_max;
            if (g < top) g = top;
        }
    // Along each row: a bucket takes the highest of the pairs it belongs to (the last one
    // only its own pair; a lone bucket nothing).
    std::vector<std::uint8_t> rows(ground.size(), 0);
    for (std::int32_t z = 0; z < h; ++z) {
        std::uint8_t prev = 0, pair = 0;
        for (std::int32_t x = 0; x + 1 < w; ++x) {
            const std::uint8_t a = ground[static_cast<std::size_t>(z * w + x)], c = ground[static_cast<std::size_t>(z * w + x + 1)];
            pair = a < c ? c : a;
            rows[static_cast<std::size_t>(z * w + x)] = prev < pair ? pair : prev;
            prev = pair;
        }
        rows[static_cast<std::size_t>(z * w + w - 1)] = pair;
    }
    // The same down each column, over the row results.
    b.top.assign(ground.size(), 0);
    for (std::int32_t x = 0; x < w; ++x) {
        std::uint8_t prev = 0, pair = 0;
        for (std::int32_t z = 0; z + 1 < h; ++z) {
            const std::uint8_t a = rows[static_cast<std::size_t>(z * w + x)], c = rows[static_cast<std::size_t>((z + 1) * w + x)];
            pair = a < c ? c : a;
            b.top[static_cast<std::size_t>(z * w + x)] = prev < pair ? pair : prev;
            prev = pair;
        }
        b.top[static_cast<std::size_t>((h - 1) * w + x)] = pair;
    }
}

void footprint_origin(std::int32_t x, std::int32_t z, std::int16_t fx, std::int16_t fz, std::int16_t& cell_x,
                      std::int16_t& cell_z) {
    // Centre minus half the footprint, rounded to the nearest cell (32-bit wrapping).
    const u32 hx = static_cast<u32>(static_cast<i32>(fx)) << 19, hz = static_cast<u32>(static_cast<i32>(fz)) << 19;
    cell_x = static_cast<i16>(static_cast<i32>(static_cast<u32>(x) - hx + 0x80000u) >> 20);
    cell_z = static_cast<i16>(static_cast<i32>(static_cast<u32>(z) - hz + 0x80000u) >> 20);
}

void claim_footprint(FootprintWorld& w, Unit& u) {
    const i16 fx = u.footprint_x, fz = u.footprint_z;
    if (u.id < w.ticks.size() && w.ticks[u.id].has_motion) w.systems.motion_cells_claimed(u);
    if (u.cell_x < 0 || u.cell_z < 0 || u.cell_x + fx >= w.map.width || u.cell_z + fz >= w.map.height) {
        relink_bucket(w, u, kOffMapBucket);
        return;
    }
    relink_bucket(w, u, (u.pos[2] >> 23) * w.buckets.width + (u.pos[0] >> 23));
    const i32 first = u.cell_z * w.map.width + u.cell_x;
    if (u.flags & ff::kStructure) {
        each_cell(w, first, fx, fz, [&](i32 index, i32 k) {
            const u8 yard = yard_byte(w, u, k);
            MapCell* c = cell_at(w, index);
            if (!c) return;
            const u8 occupied = (u.script_flags & unit_flags::kYardOpen) ? ff::kYardOpen : ff::kYardClosed;
            if (yard & occupied) take(w, u, c->unit);
            if (yard & ff::kYardMark) c->flags = static_cast<u8>(c->flags | ff::kCellYardMark);
        });
        refresh_height_range(w.map, static_cast<i16>(u.cell_x - 1), static_cast<i16>(u.cell_z - 1), static_cast<i16>(fx + 2),
                             static_cast<i16>(fz + 2));
        w.systems.update_passability(u.cell_x, u.cell_z, fx, fz);
        return;
    }
    const u32 layer = u.flags & 3;
    if (layer != 1 && layer != 2) return;
    each_cell(w, first, fx, fz, [&](i32 index, i32) {
        if (MapCell* c = cell_at(w, index)) take(w, u, layer == 1 ? c->unit : c->unit2);
    });
}

void release_cells(FootprintWorld& w, Unit& u) {
    const i16 fx = u.footprint_x, fz = u.footprint_z;
    if (u.bucket != kOffMapBucket) {
        const i32 first = u.cell_z * w.map.width + u.cell_x;
        if (u.flags & ff::kStructure) {
            each_cell(w, first, fx, fz, [&](i32 index, i32 k) {
                const u8 yard = yard_byte(w, u, k);
                MapCell* c = cell_at(w, index);
                if (!c) return;
                if (c->unit == u.id) c->unit = 0;
                if (yard & ff::kYardMark) c->flags = static_cast<u8>(c->flags & ~ff::kCellYardMark);
            });
            refresh_height_range(w.map, static_cast<i16>(u.cell_x - 1), static_cast<i16>(u.cell_z - 1),
                                 static_cast<i16>(fx + 2), static_cast<i16>(fz + 2));
        } else {
            const u32 layer = u.flags & 3;
            if (layer == 1 || layer == 2)
                each_cell(w, first, fx, fz, [&](i32 index, i32) {
                    MapCell* c = cell_at(w, index);
                    if (!c) return;
                    u16& slot = layer == 1 ? c->unit : c->unit2;
                    if (slot == u.id) slot = 0;
                });
        }
    }
    u.flags &= ~ff::kWantsCells;
    if (u.flags & ff::kHoldsWanted) {
        u.flags &= ~ff::kHoldsWanted;
        reclaim_area(w, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
    }
    w.systems.passability_after_release(u);
}

void clear_footprint(FootprintWorld& w, Unit& u) {
    release_cells(w, u);
    if (!u.carrier && u.bucket != kNoBucket) unlink_bucket(w, u);
    u.bucket = kNoBucket;
}

void claim_wanted_cells(FootprintWorld& w, Unit& u) {
    if (!(u.flags & ff::kWantsCells)) return;
    u.flags &= ~ff::kWantsCells;
    const i16 fx = u.footprint_x, fz = u.footprint_z;
    // The original looks up the first cell with a bounds check and walks from there;
    // off the map that is a null pointer.
    const bool inside = u.cell_x >= 0 && u.cell_x < w.map.width && u.cell_z >= 0 && u.cell_z < w.map.height;
    if (!inside) {
        if (fx > 0 && fz > 0) set_fault(w, "claim with the first footprint cell off the map");
        return;
    }
    const i32 first = u.cell_z * w.map.width + u.cell_x;
    if (u.flags & ff::kStructure) {
        each_cell(w, first, fx, fz, [&](i32 index, i32 k) {
            const u8 occupied = (u.script_flags & unit_flags::kYardOpen) ? ff::kYardOpen : ff::kYardClosed;
            const u8 yard = yard_byte(w, u, k);
            MapCell* c = cell_at(w, index);
            if (!c) return;
            if (yard & occupied) take(w, u, c->unit);
            else if (c->unit == u.id) c->unit = 0;
        });
        return;
    }
    // Ground units claim `unit`; every other layer (even none) claims `unit2`.
    const bool ground = (u.flags & 3) == 1;
    each_cell(w, first, fx, fz, [&](i32 index, i32) {
        if (MapCell* c = cell_at(w, index)) take(w, u, ground ? c->unit : c->unit2);
    });
}

void reclaim_area(FootprintWorld& w, std::int16_t x, std::int16_t z, std::int16_t size_x, std::int16_t size_z) {
    const i32 x0 = x, z0 = z, x1 = x0 + size_x, z1 = z0 + size_z;
    auto overlaps = [&](const Unit& v) {
        return x0 < v.cell_x + v.footprint_x && v.cell_x < x1 && z0 < v.cell_z + v.footprint_z && v.cell_z < z1;
    };
    // Buckets one beyond the area on each side; x outer, z inner.
    for (i32 bx = (x0 >> 3) - 1; bx <= (x1 >> 3) + 1; ++bx)
        for (i32 bz = (z0 >> 3) - 1; bz <= (z1 >> 3) + 1; ++bz) {
            if (static_cast<u32>(bx) >= static_cast<u32>(w.buckets.width) || static_cast<u32>(bz) >= static_cast<u32>(w.buckets.height))
                continue;
            for (Unit* v = w.buckets.heads[static_cast<std::size_t>(bz * w.buckets.width + bx)]; v; v = v->cargo_next) {
                if (overlaps(*v)) claim_wanted_cells(w, *v);
                for (Unit* c = v->cargo_head; c; c = c->cargo_next)
                    if (overlaps(*c)) claim_wanted_cells(w, *c);
            }
        }
}

void force_reclaim_around(FootprintWorld& w, Unit& u) {
    u.flags |= ff::kWantsCells;
    reclaim_area(w, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
    w.systems.update_passability(u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
}

bool yard_cells_free(FootprintWorld& w, const Unit& u, std::int32_t value) {
    const i32 x1 = static_cast<i16>(u.cell_x + u.footprint_x), z1 = static_cast<i16>(u.cell_z + u.footprint_z);
    if (u.cell_x < 1 || u.cell_z < 1 || x1 >= w.map.width || z1 >= w.map.height) return false;
    const u8 occupied = value != 0 ? ff::kYardOpen : ff::kYardClosed;
    i32 k = 0;
    for (i32 cz = u.cell_z; cz < z1; ++cz)
        for (i32 cx = u.cell_x; cx < x1; ++cx) {
            if (!(yard_byte(w, u, k++) & occupied)) continue;
            const u16 holder = w.map.cells[static_cast<std::size_t>(cz * w.map.width + cx)].unit;
            if (holder != 0 && holder != u.id) return false;
        }
    return true;
}

void refresh_height_range(MapGrid& map, std::int16_t x, std::int16_t z, std::int16_t size_x, std::int16_t size_z) {
    const i32 w = map.width, h = map.height;
    i32 x1 = static_cast<i32>(x) + size_x, z1 = static_cast<i32>(z) + size_z;
    const i32 x0 = x < 0 ? 0 : x, z0 = z < 0 ? 0 : z;
    if (x1 >= w) x1 = w - 1;
    if (z1 >= h) z1 = h - 1;
    auto at = [&](i32 cx, i32 cz) -> MapCell& { return map.cells[static_cast<std::size_t>(cz * w + cx)]; };
    for (i32 cz = z0; cz < z1; ++cz)
        for (i32 cx = x0; cx < x1; ++cx) {
            u8 lo = at(cx, cz).height, hi = lo;
            auto take_height = [&](u8 v) {
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            };
            if (cx < w - 1) take_height(at(cx + 1, cz).height);
            if (cz < h - 1) {
                take_height(at(cx, cz + 1).height);
                if (cx < w - 1) take_height(at(cx + 1, cz + 1).height);
            }
            at(cx, cz).height_min = lo;
            at(cx, cz).height_max = hi;
        }
}

void bucket_unlink(FootprintWorld& w, Unit& unit) {
    if (unit.bucket == kNoBucket) {
        set_fault(w, "unit without a spatial bucket leaves its bucket");
        return;
    }
    unlink_bucket(w, unit);
}

void bucket_prepend(FootprintWorld& w, Unit& unit) {
    if (unit.bucket == kNoBucket) {
        set_fault(w, "unit without a spatial bucket joins its bucket");
        return;
    }
    if (Unit** head = bucket_head(w, unit.bucket)) {
        unit.cargo_next = *head;
        *head = &unit;
    }
}

} // namespace sim
