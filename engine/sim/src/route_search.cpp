#include "sim/route_search.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

// Cell steps by direction: 0 = -z, then turning toward -x (2), +z (4), +x (6).
constexpr i32 kDirX[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr i32 kDirZ[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
constexpr u8 kTurnCost[8] = {0, 40, 60, 80, 100, 80, 60, 40};

constexpr u8 kOpen = 1, kClosed = 2, kBlocked = 3, kStateMask = 3, kGoal = 4, kProbe = 8;

i16 cell_x(u32 cell) { return static_cast<i16>(cell & 0xFFFF); }
i16 cell_z(u32 cell) { return static_cast<i16>(cell >> 16); }
u32 pack(i32 x, i32 z) { return (static_cast<u32>(x) & 0xFFFFu) | (static_cast<u32>(z) << 16); }

void set_fault(RouteContext& c, const std::string& what) {
    if (c.fault.empty()) c.fault = what;
}

// Whether a feature makes the cell impassable (the same test everywhere): a
// feature type with the blocking flag, an unknown high marker, or the part of a
// bigger feature whose own cell holds a blocking type.
bool feature_blocks(RouteContext& c, std::size_t index) {
    const MapCell& cell = c.map.cells[index];
    const u16 f = cell.feature;
    const std::size_t count = c.feature_blocks.size();
    if (f == 0xFFFF) return false;
    if (f < 0xFFFB) return f < count ? c.feature_blocks[f] != 0 : true;
    if (f != 0xFFFE) return true;
    const i64 back = static_cast<i64>(cell.feature_aux & 0xFF) * c.map.width + (cell.feature_aux >> 8);
    const i64 at = static_cast<i64>(index) - back;
    if (at < 0) {
        set_fault(c, "feature part points before the map");
        return false;
    }
    const u16 of = c.map.cells[static_cast<std::size_t>(at)].feature;
    if (of > 0xFFFA) return false;
    if (of >= count) {  // the original reads past its feature table
        set_fault(c, "feature part points at an unknown feature type");
        return false;
    }
    return c.feature_blocks[of] != 0;
}

// The unit holding the cell blocks the class: structures and other units
// without motion always, others while their footprint step is older than the
// class's lock step.
bool unit_blocks(RouteContext& c, const MoveClass& m, u16 id) {
    if (id == 0) return false;
    if (id >= c.ticks.size()) {
        set_fault(c, "cell occupant outside the unit table");
        return true;
    }
    const UnitTickState& t = c.ticks[id];
    if (!t.has_motion) return true;
    return t.motion.claim_tick < m.lock_tick;
}

// Grade of one cell for the class (0x47DE60): 0, 1 (rough) or 3.
u32 cell_grade(RouteContext& c, const MoveClass& m, std::size_t index) {
    if (feature_blocks(c, index)) return 0;
    const MapCell& cell = c.map.cells[index];
    if (unit_blocks(c, m, cell.unit)) return 0;
    const i32 sea = c.env.sea_level;
    const u8 lo = cell.height_min, hi = cell.height_max;
    if (static_cast<i32>(lo) < sea - m.max_water_depth) return 0;
    if (sea - m.min_water_depth < static_cast<i32>(hi)) return 0;
    const u8 d = static_cast<u8>(hi - lo);
    if (lo < sea) {
        if (d > m.bad_water_slope) return d <= m.max_water_slope ? 1u : 0u;
    } else if (d > m.bad_slope) {
        return d <= m.max_slope ? 1u : 0u;
    }
    return 3;
}

// Lowest grade over a rectangle (0x47DFC0); 0 when the rectangle reaches the
// last row or column or leaves the map.
u32 rect_grade(RouteContext& c, const MoveClass& m, i32 x, i32 z, i32 w, i32 h) {
    if (x < 0 || z < 0 || c.map.width <= w + x || c.map.height <= h + z) return 0;
    u32 g = 3;
    const i32 sea = c.env.sea_level;
    for (i32 row = 0; row < h; ++row) {
        for (i32 col = 0; col < w; ++col) {
            const std::size_t index = static_cast<std::size_t>((z + row) * c.map.width + x + col);
            if (feature_blocks(c, index)) return 0;
            const MapCell& cell = c.map.cells[index];
            if (unit_blocks(c, m, cell.unit)) return 0;
            const u8 lo = cell.height_min, hi = cell.height_max;
            if (static_cast<i32>(lo) < sea - m.max_water_depth) return 0;
            if (sea - m.min_water_depth < static_cast<i32>(hi)) return 0;
            const u8 d = static_cast<u8>(hi - lo);
            if (static_cast<i32>(lo) < sea) {
                if (d > m.bad_water_slope) {
                    if (d > m.max_water_slope) return 0;
                    if (g > 1) g = 1;
                }
            } else if (d > m.bad_slope) {
                if (d > m.max_slope) return 0;
                if (g > 1) g = 1;
            }
        }
    }
    return g;
}

// Grade of the class's footprint at (x, z) with the ring of cells around it
// (0x47E1F0): a free footprint next to anything less than free is rough.
u32 footprint_grade(RouteContext& c, const MoveClass& m, i32 x, i32 z) {
    const i32 fx = m.footprint_x, fz = m.footprint_z;
    const u32 g = rect_grade(c, m, x, z, fx, fz);
    if (g <= 1) return g;
    if (rect_grade(c, m, x - 1, z - 1, fx + 1, 1) != 3) return 1;
    if (rect_grade(c, m, fx + x, z - 1, 1, fz + 1) != 3) return 1;
    if (rect_grade(c, m, x, fz + z, fx + 1, 1) != 3) return 1;
    return rect_grade(c, m, x - 1, z, 1, fz + 1) != 3 ? 1u : 3u;
}

// One pass of the grid build over a line of cells: the lowest grade over each
// window of `fp` cells starting at the cell, and a free window next to a less
// than free cell becomes rough. `line` holds the grades with two zero cells
// before it and fp + 1 zero cells after it.
template <class Store>
void window_pass(std::vector<u8>& buf, i32 n, i32 fp, Store store) {
    u8* line = buf.data() + 2;
    line[-2] = 0;
    line[-1] = 0;
    for (i32 k = 0; k <= fp; ++k) line[n + k] = 0;
    u8 prev = 0;
    for (i32 x = 0; static_cast<u32>(x) < static_cast<u32>(n); ++x) {
        const i32 e = x + fp - 1;  // last cell of the window
        u8 m;
        if (line[e] <= prev) {
            m = line[e];
        } else if (line[x - 1] > prev) {
            m = prev;  // the old minimum is still inside the window
        } else {
            m = line[x];
            for (i32 i = x + 1; i <= e; ++i)
                if (line[i] <= m) m = line[i];
        }
        prev = m;
        if (m == 3 && (line[x - 1] < 3 || line[e + 1] < 3)) store(x, 1u);
        else store(x, static_cast<u32>(m));
    }
}

void build_class(RouteContext& c, MoveClass& m) {
    m.width = c.map.width;
    m.height = c.map.height;
    m.grid.assign(static_cast<std::size_t>(((static_cast<u32>(m.height) + 15) >> 4) * static_cast<u32>(m.width)), 0);
    if (m.footprint_x < 0 || m.footprint_z < 0) {
        set_fault(c, "movement class with a negative footprint");
        return;
    }
    u32 len = static_cast<u32>(m.footprint_z + m.height);
    if (static_cast<u32>(m.width + m.footprint_x) > len) len = static_cast<u32>(m.width + m.footprint_x);
    std::vector<u8> buf(len + 3 + 2, 0);
    // Rows: the cells' own grades.
    for (i32 z = 0; static_cast<u32>(z) < static_cast<u32>(m.height); ++z) {
        for (i32 x = 0; static_cast<u32>(x) < static_cast<u32>(m.width); ++x)
            buf[static_cast<std::size_t>(x) + 2] = static_cast<u8>(cell_grade(c, m, static_cast<std::size_t>(z * m.width + x)));
        window_pass(buf, m.width, m.footprint_x, [&](i32 x, u32 g) { m.set_grade(x, z, g); });
    }
    // Columns: the row results.
    for (i32 x = 0; static_cast<u32>(x) < static_cast<u32>(m.width); ++x) {
        for (i32 z = 0; static_cast<u32>(z) < static_cast<u32>(m.height); ++z) buf[static_cast<std::size_t>(z) + 2] = static_cast<u8>(m.grade(x, z));
        window_pass(buf, m.height, m.footprint_z, [&](i32 z, u32 g) { m.set_grade(x, z, g); });
    }
}

// --- the search -------------------------------------------------------------

void touch(RouteSearch& s, i32 index) {
    const u32 i = static_cast<u32>(index);
    s.touched[i >> 8] |= 1u << ((i >> 3) & 31);
}

// Clears the state of every cell group touched by the last search (0x40D900).
void clear_touched(RouteSearch& s) {
    for (std::size_t w = 0; w < s.touched.size(); ++w) {
        u32 bits = s.touched[w];
        if (!bits) continue;
        s.touched[w] = 0;
        for (u32 b = 0; bits; ++b, bits >>= 1) {
            if (!(bits & 1)) continue;
            const std::size_t first = w * 256 + b * 8;
            for (std::size_t k = first; k < first + 8 && k < s.cell_flags.size(); ++k) s.cell_flags[k] = 0;
        }
    }
}

// Passability of a cell for the unit being served (0x40D7B0): 0 blocked, 1
// rough, 2 not yet seen by its player, 3 free.
u32 passability(RouteContext& c, i32 x, i32 z) {
    const RouteSearch& s = c.search;
    const MoveClass& m = c.classes[static_cast<std::size_t>(s.move_class)];
    if (static_cast<u32>(x) >= static_cast<u32>(m.width) || static_cast<u32>(z) >= static_cast<u32>(m.height)) return 0;
    const i32 bx = (x >> 1) + (m.footprint_x >> 2), bw = c.map.width >> 1;
    const i32 bz = (z >> 1) + (m.footprint_z >> 2);
    if (bx >= bw || bz >= (c.map.height >> 1)) return 0;
    const std::size_t at = static_cast<std::size_t>(bz * bw + bx);
    const u32 seen = at < c.explored.size() ? c.explored[at] : 0xFFFFu;
    if (!((1u << (s.player & 31)) & seen)) return 2;
    return m.grade(x, z);
}

// Estimate from a cell to the goal, scaled for the player's load.
i32 estimate(RouteContext& c, i32 x, i32 z) {
    const i32 d = c.search.order_goal->distance(x, z);
    return static_cast<i32>(static_cast<u32>(static_cast<u64>((static_cast<i64>(d) * c.search.scale) >> 16)));
}

void sift_up(RouteSearch& s, i32 i) {
    if (i == 0) return;
    const i32 n = s.heap[static_cast<std::size_t>(i)];
    i32 p = (i - 1) >> 1;
    i32 pn = s.heap[static_cast<std::size_t>(p)];
    if (!(s.pool[static_cast<std::size_t>(n)].f < s.pool[static_cast<std::size_t>(pn)].f)) return;
    s.heap[static_cast<std::size_t>(i)] = pn;
    s.pool[static_cast<std::size_t>(pn)].heap_index = i;
    i = p;
    while (i != 0) {
        p = (i - 1) >> 1;
        pn = s.heap[static_cast<std::size_t>(p)];
        if (s.pool[static_cast<std::size_t>(pn)].f <= s.pool[static_cast<std::size_t>(n)].f) break;
        s.heap[static_cast<std::size_t>(i)] = pn;
        s.pool[static_cast<std::size_t>(pn)].heap_index = i;
        i = p;
    }
    s.heap[static_cast<std::size_t>(i)] = n;
    s.pool[static_cast<std::size_t>(n)].heap_index = i;
}

void sift_down(RouteSearch& s, i32 i) {
    const i32 n = s.heap[static_cast<std::size_t>(i)];
    const i32 key = s.pool[static_cast<std::size_t>(n)].f;
    while (true) {
        const i32 r = i * 2 + 2, l = i * 2 + 1;
        if (r < s.open_count) {
            const i32 ln = s.heap[static_cast<std::size_t>(l)], rn = s.heap[static_cast<std::size_t>(r)];
            if (s.pool[static_cast<std::size_t>(rn)].f < s.pool[static_cast<std::size_t>(ln)].f) {
                if (key <= s.pool[static_cast<std::size_t>(rn)].f) break;
                s.heap[static_cast<std::size_t>(i)] = rn;
                s.pool[static_cast<std::size_t>(rn)].heap_index = i;
                i = r;
            } else {
                if (key <= s.pool[static_cast<std::size_t>(ln)].f) break;
                s.heap[static_cast<std::size_t>(i)] = ln;
                s.pool[static_cast<std::size_t>(ln)].heap_index = i;
                i = l;
            }
            continue;
        }
        if (l >= s.open_count) break;
        const i32 ln = s.heap[static_cast<std::size_t>(l)];
        if (key <= s.pool[static_cast<std::size_t>(ln)].f) break;
        s.heap[static_cast<std::size_t>(i)] = ln;
        s.pool[static_cast<std::size_t>(ln)].heap_index = i;
        i = l;
    }
    s.heap[static_cast<std::size_t>(i)] = n;
    s.pool[static_cast<std::size_t>(n)].heap_index = i;
}

// Takes node `k` out of the heap and puts it on the free list.
void free_node(RouteSearch& s, i32 k) {
    const i32 hi = s.pool[static_cast<std::size_t>(k)].heap_index;
    s.pool[static_cast<std::size_t>(k)].heap_index = s.free_head;
    s.free_head = k;
    --s.open_count;
    if (hi < s.open_count) {
        s.heap[static_cast<std::size_t>(hi)] = s.heap[static_cast<std::size_t>(s.open_count)];
        s.pool[static_cast<std::size_t>(s.heap[static_cast<std::size_t>(hi)])].heap_index = hi;
        sift_down(s, hi);
    }
}

// A new node at the end of the heap (from the free list, else a fresh one).
i32 push_node(RouteSearch& s, const RouteNode& v) {
    const i32 i = s.open_count++;
    i32 k = s.free_head;
    if (k == -1) {
        k = s.pool_count++;
        if (s.pool.size() < static_cast<std::size_t>(s.pool_count)) s.pool.resize(static_cast<std::size_t>(s.pool_count) * 2);
    } else {
        s.free_head = s.pool[static_cast<std::size_t>(k)].heap_index;
    }
    if (s.heap.size() < static_cast<std::size_t>(s.open_count)) s.heap.resize(static_cast<std::size_t>(s.open_count) * 2);
    RouteNode& n = s.pool[static_cast<std::size_t>(k)];
    n = v;
    n.heap_index = i;
    s.heap[static_cast<std::size_t>(i)] = k;
    sift_up(s, i);
    return k;
}

void release_class_lock(RouteContext& c, MoveClass& m, Unit& u);

// Drops the unit being served (after its answer, or when it has none).
void finish(RouteContext& c) {
    RouteSearch& s = c.search;
    if (s.move_class >= 0 && s.active) release_class_lock(c, c.classes[static_cast<std::size_t>(s.move_class)], *s.active);
    s.active = nullptr;
    s.move_class = -1;
}

// Before the search: the class counts units that have not claimed cells for 30
// steps as obstacles (0x440AF0). The served unit's own footprint is refreshed
// as free if it was counted (its step is the game tick while this runs), and
// so are the footprints of units that became obstacles since the last time.
void acquire_class_lock(RouteContext& c, MoveClass& m, Unit& u) {
    const u32 tick = c.env.game_tick;
    const u32 old_lock = m.lock_tick;
    const u32 lock = (tick < 31 ? 30u : tick) - 30u;
    m.lock_tick = lock;
    MotionState& mo = c.ticks[u.id].motion;
    const u32 own = mo.claim_tick;
    mo.claim_tick = tick;
    if (own < old_lock) update_class_passability(c, m, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
    if (lock != old_lock) {
        for (std::size_t id = 1; id < c.units.size(); ++id) {
            Unit& o = c.units[id];
            const UnitTickState& t = c.ticks[id];
            if (!(o.flags & unit_flags::kAlive) || !t.has_motion) continue;
            const u32 stamp = t.motion.claim_tick;
            if (old_lock <= stamp && stamp < lock) update_class_passability(c, m, o.cell_x, o.cell_z, o.footprint_x, o.footprint_z);
        }
    }
    mo.claim_tick = own;
}

// After the search: the served unit's footprint is an obstacle again if it
// counts as one (0x440BE0).
void release_class_lock(RouteContext& c, MoveClass& m, Unit& u) {
    if (c.ticks[u.id].motion.claim_tick < m.lock_tick) update_class_passability(c, m, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
}

bool on_probe_line(const RouteSearch& s, i32 cx, i32 cz, i32 px, i32 pz) {
    i32 gdx = s.nearest_x - cx, gdz = s.nearest_z - cz, dx = px - cx, dz = pz - cz;
    if (gdx < 0) {
        gdx = -gdx;
        dx = -dx;
    }
    if (gdz < 0) {
        gdz = -gdz;
        dz = -dz;
    }
    return (dz == 0 && dx > 0 && dx <= gdx) || (dx == gdx && dz > 0 && dz <= gdz);
}

// Marks a cell of the probe line; true when it is a goal cell.
bool probe_mark(RouteContext& c, i32 x, i32 z, u8 dir) {
    RouteSearch& s = c.search;
    const i32 index = s.width * z + x;
    touch(s, index);
    s.cell_dir[static_cast<std::size_t>(index)] = dir;
    const u8 old = s.cell_flags[static_cast<std::size_t>(index)];
    s.cell_flags[static_cast<std::size_t>(index)] = static_cast<u8>(old | kProbe);
    return (old & kGoal) != 0;
}

// Before the search, a probe walks from the start toward the nearest goal cell,
// first along x, then along z; at an obstacle two walkers follow its outline in
// opposite directions until one gets back onto that line (0x40E160). Returns 0
// when the probe reaches a goal cell or an estimate of 0, else the smallest
// estimate it saw (the start's if the start itself is blocked).
i32 probe(RouteContext& c) {
    RouteSearch& s = c.search;
    i32 cx = cell_x(s.start_cell), cz = cell_z(s.start_cell);
    i32 best = estimate(c, cx, cz);
    if (passability(c, cx, cz) < 1) return best;
    while (true) {
        ++s.work;
        if (best == 0) return 0;
        const i32 gdx = s.nearest_x - cx;
        u8 dir;
        if (gdx < 0) dir = 2;
        else if (gdx > 0) dir = 6;
        else dir = (s.nearest_z - cz <= 0) ? 0 : 4;
        i32 nx = cx + kDirX[dir], nz = cz + kDirZ[dir];
        if (passability(c, nx, nz) >= 1) {
            cx = nx;
            cz = nz;
            if (probe_mark(c, nx, nz, dir)) return 0;
            const i32 h = estimate(c, nx, nz);
            if (h < best) best = h;
            continue;
        }
        // Around the obstacle: walker a turns one way, walker b the other.
        i32 ax = cx, az = cz, bx = cx, bz = cz;
        u8 bdir = static_cast<u8>((dir + 2) & 7);
        u8 adir = bdir;
        bool moved = false;
        bool back_on_line = false;
        while (!back_on_line) {
            ++s.work;
            const u8 astop = static_cast<u8>((adir - 3) & 7);
            adir = static_cast<u8>((adir - 2) & 7);
            nx = ax + kDirX[adir];
            nz = az + kDirZ[adir];
            while (passability(c, nx, nz) < 1) {
                if (adir == astop) return best;
                adir = static_cast<u8>((adir + 1) & 7);
                nx = ax + kDirX[adir];
                nz = az + kDirZ[adir];
            }
            if (ax == bx && az == bz && adir == bdir && moved) return best;
            ax = nx;
            az = nz;
            moved = true;
            if (probe_mark(c, ax, az, adir)) return 0;
            if (on_probe_line(s, cx, cz, ax, az)) {
                cx = ax;
                cz = az;
                back_on_line = true;
                break;
            }
            i32 h = estimate(c, ax, az);
            if (h < best) best = h;
            const u8 bstop = static_cast<u8>((bdir + 3) & 7);
            bdir = static_cast<u8>((bdir + 2) & 7);
            nx = bx - kDirX[bdir];
            nz = bz - kDirZ[bdir];
            while (passability(c, nx, nz) < 1) {
                if (bdir == bstop) return best;
                bdir = static_cast<u8>((bdir - 1) & 7);
                nx = bx - kDirX[bdir];
                nz = bz - kDirZ[bdir];
            }
            if (ax == bx && az == bz && adir == bdir) return best;
            bx = nx;
            bz = nz;
            if (probe_mark(c, bx, bz, bdir)) return 0;
            if (on_probe_line(s, cx, cz, bx, bz)) {
                cx = bx;
                cz = bz;
                back_on_line = true;
                break;
            }
            h = estimate(c, bx, bz);
            if (h < best) best = h;
        }
    }
}

void answer(RouteContext& c, const u32* points, i32 n) {
    if (!c.search.goal) {
        set_fault(c, "route answer without a route goal");
        return;
    }
    c.search.goal->set_route(c.motion, points, n);
}

// Starts serving the unit (0x40E630).
void begin(RouteContext& c, OrderGoal* goal) {
    RouteSearch& s = c.search;
    Unit& u = *s.active;
    s.move_class = c.types[c.ticks[u.id].type_index].move_class;
    s.order_goal = goal;
    s.start_cell = pack(u.cell_x, u.cell_z);
    if (s.move_class < 0 || s.move_class >= kMoveClasses) {
        set_fault(c, "route search for a unit without a movement class");
        s.active = nullptr;
        s.move_class = -1;
        return;
    }
    if (!goal) {
        set_fault(c, "route search for a goal without an order goal");
        s.active = nullptr;
        s.move_class = -1;
        return;
    }
    MoveClass& m = c.classes[static_cast<std::size_t>(s.move_class)];
    acquire_class_lock(c, m, u);
    for (int k = 0; k < 8; ++k) {
        s.turn_cost[static_cast<std::size_t>(k)] = kTurnCost[k];
        s.dir_cost[static_cast<std::size_t>(k)] = (k & 1) ? 0x16 : 0x10;
    }
    clear_touched(s);
    // Goal cells, and the one nearest the start (the probe heads for it).
    std::vector<u32> cells;
    goal->goal_cells(cells);
    i32 nearest = 0x7FFFFFFF;
    const i32 sx = cell_x(s.start_cell), sz = cell_z(s.start_cell);
    for (u32 p : cells) {
        const i32 x = cell_x(p), z = cell_z(p);
        if (static_cast<u32>(x) < static_cast<u32>(s.width) && static_cast<u32>(z) < static_cast<u32>(s.height)) {
            const i32 index = s.width * z + x;
            s.cell_flags[static_cast<std::size_t>(index)] = kGoal;
            touch(s, index);
        }
        const i32 dz = sz - z, dx = sx - x;
        const i32 d = static_cast<i32>(static_cast<u32>(dx) * static_cast<u32>(dx) + static_cast<u32>(dz) * static_cast<u32>(dz));
        if (d < nearest) {
            s.nearest_x = x;
            s.nearest_z = z;
            nearest = d;
        }
    }
    auto no_route = [&]() {
        answer(c, nullptr, 0);
        finish(c);
    };
    if (goal->contains_cell(sx, sz)) {
        goal->report(0x100);
        no_route();
        return;
    }
    const i32 h0 = estimate(c, sx, sz);
    if (static_cast<u32>(sx) >= static_cast<u32>(s.width) || static_cast<u32>(sz) >= static_cast<u32>(s.height)) {
        goal->report(0x200);
        no_route();
        return;
    }
    s.probe_best = probe(c);
    if (s.probe_best == 0) {
        goal->report(0x100);
    } else {
        goal->report(0x200);
        if (s.probe_best >= h0) {  // the probe got no closer than the start
            no_route();
            return;
        }
    }
    s.open_count = 0;
    s.free_head = -1;
    s.pool_count = 0;
    s.hold = 0;
    const i32 index = s.width * sz + sx;
    touch(s, index);
    s.cell_flags[static_cast<std::size_t>(index)] |= kOpen;
    s.cell_dir[static_cast<std::size_t>(index)] = static_cast<u8>(((static_cast<u32>(u.heading) + 0x1000) >> 13) & 7);
    RouteNode start;
    start.cell = s.start_cell;
    start.g = 0;
    start.f = h0;
    start.step = 0;  // uninitialised in the original (never read)
    start.depth = 100;
    s.cell_node[static_cast<std::size_t>(index)] = static_cast<u16>(push_node(s, start));
    s.turn_range = 4;
}

// Tries the neighbour of the popped cell in direction (its entry direction +
// turn) (0x40DA70).
void expand(RouteContext& c, const RouteNode& parent, i32 parent_index, i32 turn) {
    RouteSearch& s = c.search;
    const u8 dir = static_cast<u8>((s.cell_dir[static_cast<std::size_t>(parent_index)] + turn) & 7);
    const i32 nx = kDirX[dir] + cell_x(parent.cell), nz = kDirZ[dir] + cell_z(parent.cell);
    if (static_cast<u32>(nx) >= static_cast<u32>(s.width) || static_cast<u32>(nz) >= static_cast<u32>(s.height)) return;
    const i32 index = s.width * nz + nx;
    u8& flags = s.cell_flags[static_cast<std::size_t>(index)];
    const u8 state = flags & kStateMask;
    if (state == 0) {
        touch(s, index);
        const u32 w = passability(c, nx, nz);
        if (w == 0 && !(flags & kProbe)) {
            flags |= kBlocked;
            return;
        }
        const i32 h = estimate(c, nx, nz);
        // Cells estimated as close as the probe came count as goals.
        flags |= (h > s.probe_best) ? kOpen : static_cast<u8>(kOpen | kGoal);
        s.cell_dir[static_cast<std::size_t>(index)] = dir;
        RouteNode v;
        v.cell = pack(nx, nz);
        v.depth = turn != 0 ? i16{1} : static_cast<i16>(parent.depth + 1);
        v.step = w > 1 ? i16{0} : i16{30};
        v.g = s.turn_cost[static_cast<std::size_t>(turn)] + s.dir_cost[dir] + parent.g + v.step;
        if (turn != 0 && parent.depth < 5) v.g += 75;
        v.f = v.g + h;
        if (s.hold) {
            // The held (popped) node becomes this one.
            const i32 k = s.heap[0];
            RouteNode& n = s.pool[static_cast<std::size_t>(k)];
            const i32 hi = n.heap_index;
            n = v;
            n.heap_index = hi;
            sift_down(s, 0);
            s.hold = 0;
            s.cell_node[static_cast<std::size_t>(index)] = static_cast<u16>(k);
            return;
        }
        s.cell_node[static_cast<std::size_t>(index)] = static_cast<u16>(push_node(s, v));
        return;
    }
    if (state != kOpen) return;
    const i32 k = s.cell_node[static_cast<std::size_t>(index)];
    RouteNode& n = s.pool[static_cast<std::size_t>(k)];
    i32 g = s.turn_cost[static_cast<std::size_t>(turn)] + s.dir_cost[dir] + parent.g + n.step;
    if (turn != 0 && parent.depth < 5) g += 75;
    if (g >= n.g) return;
    s.cell_dir[static_cast<std::size_t>(index)] = dir;
    n.f += g - n.g;
    n.g = g;
    n.depth = turn != 0 ? i16{1} : static_cast<i16>(parent.depth + 1);
    if (!s.hold) {
        sift_up(s, n.heap_index);
        return;
    }
    const i32 held = s.heap[0];
    sift_up(s, n.heap_index);
    if (s.pool[static_cast<std::size_t>(held)].heap_index != 0) {
        // The held node was pushed down: it goes for good.
        s.hold = 0;
        free_node(s, held);
    }
}

// The route from the start to the goal cell found, as its corners in whole
// world units (the footprint's centre), at most 64 (0x40E050).
void reconstruct(RouteContext& c) {
    RouteSearch& s = c.search;
    std::array<u32, 64> ring{};
    ring[0] = s.found_cell;
    i16 x = cell_x(s.found_cell), z = cell_z(s.found_cell);
    i32 dir = static_cast<std::int8_t>(s.cell_dir[static_cast<std::size_t>(s.width * z + x)]);
    u32 n = 1;
    i32 guard = 0;
    while (x != cell_x(s.start_cell) || z != cell_z(s.start_cell)) {
        const i32 d = static_cast<std::int8_t>(s.cell_dir[static_cast<std::size_t>(s.width * z + x)]);
        if (d != dir) {
            ring[n & 63] = pack(x, z);
            ++n;
            dir = d;
        }
        if (dir < 0 || dir > 7 || ++guard > s.cell_count + 1) {
            set_fault(c, "route reconstruction does not lead back to the start");
            return;
        }
        x = static_cast<i16>(x - kDirX[dir]);
        z = static_cast<i16>(z - kDirZ[dir]);
        if (static_cast<u32>(x) >= static_cast<u32>(s.width) || static_cast<u32>(z) >= static_cast<u32>(s.height)) {
            set_fault(c, "route reconstruction leaves the map");
            return;
        }
    }
    ring[n & 63] = s.start_cell;
    ++n;
    const i32 count = n >= 64 ? 64 : static_cast<i32>(n);
    const MoveClass& m = c.classes[static_cast<std::size_t>(s.move_class)];
    std::array<u32, 64> points{};
    for (i32 i = 0; i < count; ++i) {
        const u32 p = ring[(n - 1 - static_cast<u32>(i)) & 63];
        const u16 px = static_cast<u16>((static_cast<u16>(p * 2) + static_cast<u16>(m.footprint_x)) << 3);
        const u16 pz = static_cast<u16>((static_cast<u16>((p >> 16) * 2) + static_cast<u16>(m.footprint_z)) << 3);
        points[static_cast<std::size_t>(i)] = static_cast<u32>(px) | (static_cast<u32>(pz) << 16);
    }
    answer(c, points.data(), count);
}

// Pops and expands cells until this round's work is spent (part of 0x40EB70).
void search_round(RouteContext& c) {
    RouteSearch& s = c.search;
    do {
        if (s.open_count == s.hold) break;
        ++s.work;
        if (s.hold) {
            s.hold = 0;
            free_node(s, s.heap[0]);
        }
        const RouteNode top = s.pool[static_cast<std::size_t>(s.heap[0])];
        s.hold = 1;
        const i32 index = s.width * cell_z(top.cell) + cell_x(top.cell);
        if (s.cell_flags[static_cast<std::size_t>(index)] & kGoal) {
            s.found_cell = top.cell;
            reconstruct(c);
            finish(c);
            return;
        }
        s.cell_flags[static_cast<std::size_t>(index)] = kClosed;
        for (i32 t = -s.turn_range; t <= s.turn_range; ++t) expand(c, top, index, t & 7);
        s.turn_range = 2;
    } while (s.work < 100);
}

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2 || p.controller == 3) && p.state != 10; }

}  // namespace

void build_passability(RouteContext& c) {
    for (MoveClass& m : c.classes)
        if (m.named) build_class(c, m);
}

void update_class_passability(RouteContext& c, MoveClass& m, std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) {
    i32 x0 = x - m.footprint_x, z0 = z - m.footprint_z;
    i32 x1 = w + 1 + x, z1 = h + 1 + z;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (static_cast<u32>(m.width) < static_cast<u32>(x1)) x1 = m.width;
    if (static_cast<u32>(m.height) < static_cast<u32>(z1)) z1 = m.height;
    if (x0 >= x1) return;
    for (i32 zz = z0; zz < z1; ++zz)
        for (i32 xx = x0; xx < x1; ++xx) m.set_grade(xx, zz, footprint_grade(c, m, xx, zz));
}

void update_passability(RouteContext& c, std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) {
    for (MoveClass& m : c.classes)
        if (m.named) update_class_passability(c, m, x, z, w, h);
}

void passability_after_release(RouteContext& c, Unit& u) {
    UnitTickState& t = c.ticks[u.id];
    if (t.has_motion) {
        const u32 old = t.motion.claim_tick;
        t.motion.claim_tick = c.env.game_tick;
        for (MoveClass& m : c.classes)
            if (old < m.lock_tick) update_class_passability(c, m, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
        return;
    }
    update_passability(c, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
}

bool can_place_footprint(RouteContext& c, std::uint16_t type, std::uint16_t exclude, std::int16_t x, std::int16_t z,
                         std::uint32_t mode) {
    if (type >= c.types.size()) {
        set_fault(c, "placement check for an unknown type");
        return false;
    }
    const RouteType& t = c.types[type];
    const i32 fx = t.footprint_x, fz = t.footprint_z;
    if (x < 0 || z < 0 || fx + x >= c.map.width || fz + z >= c.map.height) return mode == 2;
    if (t.movement == 0) return c.systems.structure_fits(type, x, z);
    if (mode != 1) return true;
    const i32 sea = c.env.sea_level;
    for (i32 row = 0; row < fz; ++row) {
        for (i32 col = 0; col < fx; ++col) {
            const std::size_t index = static_cast<std::size_t>((z + row) * c.map.width + x + col);
            if (feature_blocks(c, index)) return false;
            const MapCell& cell = c.map.cells[index];
            if (cell.unit != 0 && cell.unit != exclude) return false;
            const i32 lo = cell.height_min, hi = cell.height_max;
            if (lo < sea - t.max_water_depth) return false;
            if (sea - t.min_water_depth < hi) return false;
            const i32 d = hi - lo;
            if (d > t.max_slope) {
                if (lo >= sea) return false;
                if (d > t.max_water_slope) return false;
            }
        }
    }
    return true;
}

void init_route_search(RouteContext& c) {
    RouteSearch& s = c.search;
    s.pool.clear();
    s.heap.clear();
    s.free_head = -1;
    s.pool_count = 0;
    s.open_count = 0;
    s.hold = 0;
    s.width = c.map.width;
    s.height = c.map.height;
    s.cell_count = static_cast<i32>((static_cast<u32>(s.width * s.height) + 7u) & ~7u);
    s.cell_flags.assign(static_cast<std::size_t>(s.cell_count), 0);
    s.cell_dir.assign(static_cast<std::size_t>(s.cell_count), 0);
    s.cell_node.assign(static_cast<std::size_t>(s.cell_count), 0);
    s.touched.assign((static_cast<std::size_t>(s.cell_count) + 0xFF) >> 8, 0);
    s.active = nullptr;
    s.goal = nullptr;
    s.order_goal = nullptr;
    s.move_class = -1;
    s.budget_divisor = 0x535;
    s.work = 0;
    s.player = 0;
    s.scale = 0;
    s.base_budget = 0x18000;
    for (int p = 0; p < 10; ++p) {
        s.estimate_scale[static_cast<std::size_t>(p)] = s.base_budget;
        s.credit[static_cast<std::size_t>(p)] = 0;
        s.unit_cursor[static_cast<std::size_t>(p)] = static_cast<std::size_t>(p) < c.players.size() ? c.players[static_cast<std::size_t>(p)].first_unit : 0;
    }
}

void route_search_forget(RouteSearch& s, const MotionGoal& goal) {
    if (&goal != s.goal) return;
    s.active = nullptr;
    s.goal = nullptr;
    s.order_goal = nullptr;
    s.move_class = -1;
}

void run_route_search(RouteContext& c) {
    RouteSearch& s = c.search;
    if (c.env.player_count == 0) return;
    const i32 share = s.budget_divisor / static_cast<i32>(c.env.player_count);
    if (++s.refresh_counter >= 150) {
        s.refresh_counter = 0;
        for (int p = 0; p < 10; ++p) {
            if (c.env.units_per_player == 0) {
                set_fault(c, "route budget refresh divides by a zero unit range");
                return;
            }
            const i32 load = s.load[static_cast<std::size_t>(p)] / static_cast<i32>(c.env.units_per_player);
            i32 scale = s.base_budget;
            if (load < 1) scale = s.base_budget * 6;
            else if (load < 2) scale = s.base_budget * 3;
            s.estimate_scale[static_cast<std::size_t>(p)] = scale;
            s.load[static_cast<std::size_t>(p)] = 0;
        }
    }
    i32 total = 0;
    for (int p = 0; p < 10; ++p) {
        if (static_cast<std::size_t>(p) >= c.players.size() || !plays(c.players[static_cast<std::size_t>(p)])) continue;
        s.credit[static_cast<std::size_t>(p)] += share;
        total += s.credit[static_cast<std::size_t>(p)];
    }
    while (total > 0) {
        s.work = 0;
        if (!s.active) {
            s.work = 1;
            while (s.credit[s.player] <= 0) s.player = static_cast<u8>(s.player + 1 >= 10 ? 0 : s.player + 1);
            ++s.load[s.player];
            const PlayerSlot& pl = c.players[s.player];
            i32& cursor = s.unit_cursor[s.player];
            cursor = cursor == pl.last_unit ? pl.first_unit : cursor + 1;
            if (cursor < 0 || static_cast<std::size_t>(cursor) >= c.units.size()) {
                set_fault(c, "route search unit cursor outside the unit table");
                return;
            }
            Unit& u = c.units[static_cast<std::size_t>(cursor)];
            UnitTickState& t = c.ticks[static_cast<std::size_t>(cursor)];
            if (t.type_index != 0 && t.has_motion && t.motion.has_move_class && t.motion.goal) {
                MotionGoal* g = t.motion.goal.get();
                s.goal = g->wants_route(c.motion) ? dynamic_cast<RouteGoal*>(g) : nullptr;
                if (s.goal) {
                    s.active = &u;
                    s.work += 100;
                    s.scale = s.estimate_scale[s.player];
                    begin(c, s.goal->attached);
                }
            }
        } else if (s.open_count - s.hold == 0) {
            answer(c, nullptr, 0);
            finish(c);
        } else {
            search_round(c);
        }
        total -= s.work;
        s.credit[s.player] -= s.work;
        if (!c.fault.empty()) return;
    }
}

} // namespace sim
