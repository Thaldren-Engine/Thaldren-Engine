#include "sim/projectile.h"

#include <cmath>

#include "sim/trig.h"
#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

namespace pf = projectile_flags;
namespace wf = weapon_flags;

i32 add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 sub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 neg(i32 a) { return static_cast<i32>(0u - static_cast<u32>(a)); }
i16 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }

void add3(Vec3i& a, const Vec3i& b) {
    for (int k = 0; k < 3; ++k) a[static_cast<std::size_t>(k)] = add(a[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(k)]);
}

// High dword of the 64-bit square: about (d / 65536)^2.
i32 square_hi(i32 d) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(d) * d) >> 32)); }

// Squared distance in whole world units, each axis squared as above, summed with wraparound.
i32 dist2_hi(const Vec3i& a, const Vec3i& b) {
    return add(add(square_hi(sub(a[0], b[0])), square_hi(sub(a[1], b[1]))), square_hi(sub(a[2], b[2])));
}

// sqrt(dx^2 + dy^2 + dz^2) at double precision, truncated, low 32 bits.
i32 length_ftol(i32 dx, i32 dy, i32 dz) {
    const double x = dx, y = dy, z = dz;
    return x87::ftol32(std::sqrt((x * x + y * y) + z * z));
}

// Signed division by 16 rounding toward zero.
i32 div16(i32 v) { return v / 16; }

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }

std::int32_t first_human_net_id(const std::vector<PlayerSlot>& players) {
    for (std::size_t s = 0; s < players.size() && s < 10; ++s)
        if (plays(players[s])) return players[s].net_id;
    return -1;
}

// The C runtime's stricmp in the "C" locale: only A-Z fold, bytes compare unsigned.
int stricmp_c(const std::string& a, const std::string& b) {
    for (std::size_t i = 0;; ++i) {
        auto fold = [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : static_cast<int>(ch); };
        const int ca = i < a.size() ? fold(static_cast<unsigned char>(a[i])) : 0;
        const int cb = i < b.size() ? fold(static_cast<unsigned char>(b[i])) : 0;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

i32 slot_index(const ProjectileTable& t, const Projectile& p) {
    const Projectile* base = t.slots.data();
    return &p >= base && &p < base + kMaxProjectiles ? static_cast<i32>(&p - base) : -1;
}

UnitTickState& state_of(ProjectileWorld& w, const Unit& u) { return w.combat.ticks[u.id]; }
u8 owner_slot_of(ProjectileWorld& w, const Unit& u) { return static_cast<u8>(state_of(w, u).owner); }

// The camera stops following a shot that ends; it rests where the shot was.
void release_camera(ProjectileWorld& w, Projectile& p) {
    ProjectileTable& t = w.table;
    if (t.camera_slot >= 0 && t.camera_slot == slot_index(t, p)) {
        t.camera_rest = p.pos;
        t.camera_rest_ticks = p.weapon->hold_time;
        t.camera_slot = -1;
    }
}

void end_shot(ProjectileWorld& w, Projectile& p) {
    release_camera(w, p);
    p.flags = static_cast<u16>(p.flags | pf::kDeleted);
}

// Which of the owner's three weapon slots holds `weapon` (3 if none).
int weapon_slot_of(ProjectileWorld& w, const Unit& owner, const WeaponType* weapon) {
    int k = 0;
    while (k < 3 && w.systems.slot_weapon(owner, k) != weapon) ++k;
    return k;
}

const char* const kQueryNames[3] = {"QueryPrimary", "QuerySecondary", "QueryTertiary"};
const char* const kFireNames[3] = {"FirePrimary", "FireSecondary", "FireTertiary"};

// The piece a weapon slot fires from, asked from the unit's script at once. A slot
// past the third has no query name (the original reads a name that matches no
// script function), so it gets piece 0 like a script without the query.
i32 query_weapon_piece(Unit& owner, int slot) {
    i32 piece = 0;
    if (slot < 3 && owner.script)
        owner.script->call_in_out(owner.script->program().find_function(kQueryNames[slot]), &piece, nullptr, nullptr,
                                  nullptr);
    return piece;
}

Vec3i piece_world_pos(const Unit& u, i32 piece) {
    const auto o = piece_local_offset(u, piece);
    return {add(u.pos[0], o[0]), add(u.pos[1], o[1]), add(u.pos[2], o[2])};
}

void set_velocity_from_angles(Projectile& p, i32 speed) {
    p.vel[1] = trig::mul_sin(p.elevation, speed);
    const i32 h = trig::mul_cos(p.elevation, speed);
    p.vel[0] = neg(trig::mul_sin(p.yaw, h));
    p.vel[2] = neg(trig::mul_cos(p.yaw, h));
}

// Fire and recoil scripts of a launch (queued, they run in the owner's next script step).
void launch_scripts(ProjectileWorld& w, const FireSlot& slot, Unit& owner) {
    if (!owner.script) return;  // the original requires a script
    owner.script->call(kFireNames[slot.index & 3], nullptr, false);
    const u16 rel = static_cast<u16>(w.systems.slot_fire_heading(owner, slot.index & 3) - owner.heading);
    const i32 s = neg(trig::mul_sin(rel, 800));
    const i32 c = neg(trig::mul_cos(rel, 800));
    owner.script->call_with_args(owner.script->program().find_function("RockUnit"), nullptr, false, 2, c, s, 0, 0);
}

u32 udiv(ProjectileTable& t, u32 a, u32 b, const char* what) {
    if (b == 0) {
        t.fault = what;
        return 0;
    }
    return a / b;
}

void mark_owner_damage(Unit& owner, i32 enemy, i32 friendly) {
    if (enemy > static_cast<i32>(static_cast<u32>(friendly) * 2u)) owner.net_dirty = static_cast<u16>(owner.net_dirty | 0x4000);
    else owner.net_dirty = static_cast<u16>(owner.net_dirty | 0x2000);
}

// Damage of one shot to one unit, scaled (edge falloff), raised by the shooter's
// kills, doubled/halved by the game options; dealt as weapon or paralysis damage.
i32 deal_damage(ProjectileWorld& w, const Projectile& p, Unit& target, float scale) {
    const WeaponType& wt = *p.weapon;
    i32 base = wt.default_damage;
    if (!wt.damage.empty()) {
        const UnitTickState& ts = state_of(w, target);
        static const std::string kNone;
        const std::string& name = ts.tick_type ? ts.tick_type->name : kNone;
        // Lower bound by case-insensitive name, then an exact (case-insensitive) match.
        std::size_t lo = 0, hi = wt.damage.size();
        while (lo != hi) {
            const std::size_t mid = lo + ((hi - lo) >> 1);
            if (stricmp_c(wt.damage[mid].first, name) < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo != wt.damage.size() && stricmp_c(name, wt.damage[lo].first) >= 0) base = wt.damage[lo].second;
    }
    i32 dmg = x87::ftol32(static_cast<double>(base) * static_cast<double>(scale));
    const u16 heading = static_cast<u16>(x87::atan2_scaled(sub(p.pos[0], target.pos[0]), sub(p.pos[2], target.pos[2])) - target.heading);
    if (p.owner) {
        u32 tier = state_of(w, *p.owner).kill_count / 5u;
        if (tier > 5) tier = 5;
        dmg = static_cast<i32>((tier * 6u + 100u) * static_cast<u32>(dmg)) / 100;
    }
    if (w.env.options & 0x80) dmg = add(dmg, dmg);
    if (w.env.options & 0x100) dmg /= 2;
    apply_damage(w.combat, p.owner, target, dmg, (wt.flags & wf::kParalyzer) ? damage_kind::kParalyze : damage_kind::kWeapon,
                 heading);
    return dmg;
}

// Distance along one axis from a point to a box [lo, hi] (0 inside).
i32 box_gap(i32 v, i32 lo, i32 hi) {
    if (v < lo) return sub(lo, v);
    if (v > hi) return sub(v, hi);
    return 0;
}

void area_damage(ProjectileWorld& w, Projectile& p, const Vec3i& at) {
    const WeaponType& wt = *p.weapon;
    MapGrid& map = w.map;
    const i32 radius = wt.area_of_effect >> 1;
    const i32 reach = radius / 16 + 1;
    const i32 cx = div16(hi16(at[0])), cz = div16(hi16(at[2]));
    i32 x0 = cx - reach, x1 = cx + reach, z0 = cz - reach, z1 = cz + reach;
    if (x0 < 0) x0 = 0;
    if (x1 > map.width) x1 = map.width;
    if (z0 < 0) z0 = 0;
    if (z1 > map.height) z1 = map.height;
    i32 enemy = 0, friendly = 0;
    std::vector<const Unit*> seen_units;      // at most 20 remembered
    std::vector<const MapCell*> seen_features;  // at most 64 remembered
    for (i32 z = z0; z < z1; ++z) {
        for (i32 x = x0; x < x1; ++x) {
            MapCell* cell = map.cell(x, z);
            if (!cell) continue;  // not reached: the range is clamped to the map
            for (int k = 0; k < 2; ++k) {
                const u16 id = k == 0 ? cell->unit : cell->unit2;
                if (id == 0 || id >= w.combat.units.size()) continue;
                Unit& u = w.combat.units[id];
                if (&u == p.owner) continue;
                bool seen = false;
                for (const Unit* s : seen_units) seen = seen || s == &u;
                if (seen) continue;
                if (seen_units.size() < 20) seen_units.push_back(&u);
                const UnitTypeInfo empty{};
                const UnitTypeInfo& ty = u.type ? *u.type : empty;
                const i32 gx = box_gap(at[0], add(u.pos[0], ty.model_min_x), add(u.pos[0], ty.model_max_x));
                const i32 gy = box_gap(at[1], add(u.pos[1], ty.model_min_y), add(u.pos[1], ty.model_max_y));
                const i32 gz = box_gap(at[2], add(u.pos[2], ty.model_min_z), add(u.pos[2], ty.model_max_z));
                const i32 d = hi16(length_ftol(gx, gy, gz));
                if (d >= radius) continue;
                float scale = 1.0f;
                if (d != 0) {
                    const double e = wt.edge_effectiveness;
                    const double t = static_cast<double>(d) / static_cast<double>(radius) - 1.0;
                    scale = static_cast<float>(t * t * (1.0 - e) + e);
                }
                const i32 dealt = deal_damage(w, p, u, scale);
                if (p.owner_slot == owner_slot_of(w, u)) friendly = add(friendly, dealt);
                else enemy = add(enemy, dealt);
            }
            if (wt.flags & wf::kUnitsOnly) continue;
            MapCell* fcell = cell;
            i32 fx = x, fz = z;
            if (cell->feature == 0xFFFE) {
                fx = x - (cell->feature_aux >> 8);
                fz = z - (cell->feature_aux & 0xFF);
                fcell = map.cell(fx, fz);
                if (!fcell) {
                    w.table.fault = "feature cell outside the map";
                    continue;
                }
            }
            const u16 fid = fcell->feature;
            if (fid >= 0xFFFB) continue;
            i32 d;
            if (cell->flags & 1) {
                const Vec3i a = w.systems.feature_anim_pos(cell->feature_aux);
                d = hi16(length_ftol(sub(at[0], a[0]), sub(at[1], a[1]), sub(at[2], a[2])));
            } else {
                // The feature's centre: its footprint (in half cells) from this cell's corner, on the ground.
                const FeatureTypeInfo none{};
                const FeatureTypeInfo& ft = fid < w.features.size() ? w.features[fid] : none;
                if (fid >= w.features.size()) w.table.fault = "feature type outside the table";
                const i32 wx = static_cast<i32>(static_cast<u32>(ft.footprint_x + x * 2) << 19);
                const i32 wz = static_cast<i32>(static_cast<u32>(ft.footprint_z + z * 2) << 19);
                const i32 wy = static_cast<i32>(static_cast<u32>(w.heights.sample(hi16(wx), hi16(wz))) << 16);
                d = hi16(length_ftol(sub(at[0], wx), sub(at[1], wy), sub(at[2], wz)));
            }
            if (d >= radius) continue;
            bool seen = false;
            for (const MapCell* s : seen_features) seen = seen || s == fcell;
            if (seen) continue;
            if (seen_features.size() < 64) seen_features.push_back(fcell);
            w.systems.damage_feature(*fcell, fx, fz, wt);
        }
    }
    if (wt.flags & wf::kInterceptor) {
        ProjectileTable& t = w.table;
        for (i32 i = 0; i < t.count; ++i) {
            Projectile& o = t.slots[static_cast<std::size_t>(i)];
            if ((o.flags & pf::kDeleted) || &o == &p) continue;
            const u32 aoe = p.weapon->area_of_effect;
            if (dist2_hi(p.pos, o.pos) >= static_cast<i32>(aoe * aoe)) continue;
            detonate_projectile(w, o, nullptr);
            if (!p.owner) {  // the original dereferences the missing owner
                t.fault = "interceptor without an owner";
                continue;
            }
            const i32 net = w.combat.players[static_cast<std::size_t>(state_of(w, *p.owner).owner)].net_id;
            for (const Projectile* s : {static_cast<const Projectile*>(&o), static_cast<const Projectile*>(&p)}) {
                std::uint8_t pkt[14];
                pkt[0] = 0x0E;
                for (int k = 0; k < 3; ++k)
                    for (int b = 0; b < 4; ++b)
                        pkt[1 + 4 * k + b] = static_cast<std::uint8_t>(static_cast<u32>(s->aim[static_cast<std::size_t>(k)]) >> (8 * b));
                pkt[13] = s->weapon->index;
                w.systems.network_send(net, pkt, 14);
            }
        }
    }
    if (p.owner) mark_owner_damage(*p.owner, enemy, friendly);
}

// Steers a seeking shot: where it heads now.
Vec3i seek_point(ProjectileWorld& w, Projectile& p) {
    const bool cruise = (p.weapon->flags & wf::kCruise) != 0;
    if (cruise) {
        // Far from the aim point, cruise at height 700 above it (the fraction of the aim height is kept).
        if (hi16(length_ftol(sub(p.pos[0], p.aim[0]), sub(p.pos[1], p.aim[1]), sub(p.pos[2], p.aim[2]))) > 0x400) {
            p.trail = p.aim;
            p.trail[1] = static_cast<i32>((static_cast<u32>(p.trail[1]) & 0xFFFFu) | (700u << 16));
            return p.trail;
        }
        // Close: dive to the ground (or sea) below the aim point.
        p.trail = p.aim;
        const i32 sea = w.env.sea_level;
        const i32 ground = w.heights.sample(hi16(p.aim[0]), hi16(p.aim[2]));
        p.trail[1] = static_cast<i32>(static_cast<u32>(ground > sea ? ground : sea) << 16);
        return p.trail;
    }
    if (p.intercepted >= 0) return w.table.slots[static_cast<std::size_t>(p.intercepted)].pos;
    if (p.target && (p.target->flags & unit_flags::kAlive)) return p.target->pos;
    return p.aim;
}

// Turns yaw and elevation toward `to` by at most the weapon's turn rate. False
// (a burn-blow weapon overshot: more than 27000 off) ends the turn early.
bool turn_toward(Projectile& p, const Vec3i& to) {
    const WeaponType& wt = *p.weapon;
    const i32 dx = sub(p.pos[0], to[0]), dy = sub(p.pos[1], to[1]), dz = sub(p.pos[2], to[2]);
    const u16 want_yaw = static_cast<u16>(x87::atan2_scaled(dx, dz));
    const i32 len = x87::ftol32(x87::crt_hypot(dx, dz));
    const u16 want_elev = static_cast<u16>(x87::atan2_scaled(neg(hi16(dy)), hi16(len)));
    auto step = [&](u16& angle, u16 want) {
        const i16 d = static_cast<i16>(want - angle);
        const i16 mag = static_cast<i16>(d < 0 ? -d : d);  // |-32768| stays -32768
        if (mag > 27000 && (wt.flags & wf::kBurnBlow)) return false;
        if (mag < static_cast<i32>(wt.turn_rate)) angle = want;
        else if (d < 0) angle = static_cast<u16>(angle - wt.turn_rate);
        else angle = static_cast<u16>(angle + wt.turn_rate);
        return true;
    };
    if (!step(p.yaw, want_yaw)) return false;
    return step(p.elevation, want_elev);
}

// Hit tests after a move: map edge, chased shot, units, features, ground, water.
void collide(ProjectileWorld& w, const WeaponType& wt, Projectile& p) {
    MapCell* cell = w.map.cell_at(p.pos);
    if (!cell) {
        end_shot(w, p);
        return;
    }
    if (p.intercepted >= 0) {
        const Projectile& o = w.table.slots[static_cast<std::size_t>(p.intercepted)];
        const u32 aoe = p.weapon->area_of_effect;
        if (dist2_hi(p.pos, o.pos) < static_cast<i32>(aoe * aoe)) detonate_projectile(w, p, nullptr);
    }
    p.ground_height = static_cast<i16>((cell->height_max + cell->height_min) / 2);
    for (int k = 0; k < 2; ++k) {
        const u16 id = k == 0 ? cell->unit : cell->unit2;
        if (id == 0 || id >= w.combat.units.size()) continue;
        Unit& u = w.combat.units[id];
        if (owner_slot_of(w, u) == p.owner_slot) continue;
        const UnitTypeInfo empty{};
        const UnitTypeInfo& ty = u.type ? *u.type : empty;
        const bool below_top = p.pos[1] < add(ty.model_max_y, u.pos[1]);
        // The first unit is hit anywhere below its top, the second only within its height.
        if (k == 0 ? below_top : (p.pos[1] >= add(ty.model_min_y, u.pos[1]) && p.pos[1] <= add(ty.model_max_y, u.pos[1]))) {
            detonate_projectile(w, p, &u);
            return;
        }
    }
    if (wt.flags & wf::kUnitsOnly) return;
    const i32 tx = div16(hi16(p.pos[0])), tz = div16(hi16(p.pos[2]));
    const FeatureTypeInfo* ft = nullptr;
    if (cell->feature < 0xFFFB) {
        if (cell->feature < w.features.size()) ft = &w.features[cell->feature];
    } else if (cell->feature == 0xFFFE) {
        const i32 back = w.map.width * (cell->feature_aux & 0xFF) + (cell->feature_aux >> 8);
        const std::ptrdiff_t at = (cell - w.map.cells.data()) - back;
        if (at < 0) w.table.fault = "feature cell outside the map";
        else {
            const u16 fid = w.map.cells[static_cast<std::size_t>(at)].feature;
            if (fid < 0xFFFB) {
                if (fid < w.features.size()) ft = &w.features[fid];
                else w.table.fault = "feature type outside the table";
            }
        }
    }
    const i16 y = hi16(p.pos[1]);
    if (ft) {
        // A feature is hit once per cell crossed (the last hit cell is remembered).
        if (static_cast<i32>(ft->height) + cell->height_min > y) {
            if (p.last_feature_x == static_cast<i16>(tx) && p.last_feature_z == static_cast<i16>(tz)) ft = nullptr;
            else {
                p.last_feature_x = static_cast<i16>(tx);
                p.last_feature_z = static_cast<i16>(tz);
            }
        } else ft = nullptr;
        if (ft) {
            detonate_projectile(w, p, nullptr);
            return;
        }
    }
    if (static_cast<i16>(cell->height_min) > y) {
        if (wt.flags & wf::kGroundBounce) {
            p.vel[1] = neg(p.vel[1] >> 2);
            return;
        }
    } else {
        if (wt.flags & wf::kWaterWeapon) return;
        if (y >= static_cast<i16>(w.env.sea_level)) return;
        if (w.env.no_sea_level_trigger) return;
    }
    detonate_projectile(w, p, nullptr);
}

// A burst launcher: every burst_rate steps it clones itself into a new shot.
void burst_step(ProjectileWorld& w, Projectile& p) {
    const WeaponType& wt = *p.weapon;
    const u32 tick = w.env.game_tick;
    if (tick < p.spawn_tick + wt.burst_rate) return;
    if (wt.burst_rate >= 5 || (p.burst_left & 1)) {
        // Slow bursts (and every other round of fast ones) leave from the firing piece's current place.
        if (p.owner) {
            (void)weapon_slot_of(w, *p.owner, p.weapon);  // computed by the original, unused with a piece
            p.pos = piece_world_pos(*p.owner, static_cast<u16>(p.piece));
        } else {
            w.table.fault = "burst without an owner";
        }
    }
    p.burst_left = static_cast<u16>(p.burst_left - 1);
    p.spawn_tick += wt.burst_rate;
    Projectile* c = alloc_projectile(w.table);
    if (c) {
        *c = p;
        c->spawn_tick = tick;
        if (wt.flags & wf::kSoundTrigger) w.systems.play_sound(wt.sound_start, p.pos);
        if (wt.weapon_timer != 0) c->expire_tick = tick + wt.weapon_timer;
        else c->expire_tick = udiv(w.table, static_cast<u32>(p.distance) + 0x100000u, static_cast<u32>(p.speed), "burst speed 0") + tick;
        if (wt.random_decay != 0) {
            const u32 r = w.rng.next_mod(wt.random_decay);
            c->expire_tick += r - (wt.random_decay >> 1);
        }
        c->burst_left = 0;
        if (wt.spray_angle != 0) {
            // The launcher's own direction is sprayed; the next round inherits it.
            const u32 r = w.rng.next_mod(wt.spray_angle);
            const u16 a = static_cast<u16>(r + static_cast<u16>(p.yaw - (wt.spray_angle >> 1)));
            const i32 h = trig::mul_cos(p.elevation, wt.velocity);
            p.vel[0] = neg(trig::mul_sin(a, h));
            p.vel[2] = neg(trig::mul_cos(a, h));
        }
    }
    if (p.burst_left == 0) end_shot(w, p);
}

} // namespace

MapCell* MapGrid::cell(std::int32_t x, std::int32_t z) {
    if (x < 0 || x >= width || z < 0 || z >= height) return nullptr;
    return &cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

MapCell* MapGrid::cell_at(const Vec3i& world) { return cell(world[0] >> 20, world[2] >> 20); }

Projectile* alloc_projectile(ProjectileTable& table) {
    if (table.count >= kMaxProjectiles) return nullptr;
    Projectile& p = table.slots[static_cast<std::size_t>(table.count++)];
    p.flags = static_cast<u16>(p.flags & ~pf::kDeleted);
    p.target = nullptr;
    return &p;
}

void init_projectile(ProjectileWorld& w, Projectile& p, const WeaponType& weapon, const Vec3i& start, const Vec3i* aim,
                     std::uint32_t spawn_tick, Unit* owner) {
    p.weapon = &weapon;
    p.pos = start;
    p.trail = start;
    if (aim) p.aim = *aim;
    p.flags = static_cast<u16>(p.flags & ~pf::kBeamFull);
    p.spawn_tick = spawn_tick;
    p.burst_left = 0;
    p.intercepted = -1;
    p.next_smoke_tick = w.env.game_tick;
    p.target = nullptr;
    p.flags = static_cast<u16>(p.flags & ~pf::kPhaseMask);
    if (!owner) {
        p.owner_slot = 10;
        p.owner = nullptr;
    } else {
        p.owner = owner;
        p.owner_slot = owner_slot_of(w, *owner);
        if ((owner->flags & 0x20000000u) && w.table.camera_unit == owner) w.table.camera_slot = slot_index(w.table, p);
        p.piece = static_cast<i16>(query_weapon_piece(*owner, weapon_slot_of(w, *owner, &weapon)));
        owner->cloak_holdoff_tick = w.env.game_tick + 600;
    }
    w.systems.play_sound(weapon.sound_start, start);
}

bool fire_directed(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& muzzle, const Vec3i& aim, Unit* target) {
    Projectile* p = alloc_projectile(w.table);
    if (!p) return false;
    init_projectile(w, *p, *slot.weapon, muzzle, &aim, w.env.game_tick, &owner);
    const i32 dx = sub(muzzle[0], aim[0]), dy = sub(muzzle[1], aim[1]), dz = sub(muzzle[2], aim[2]);
    const i32 yaw = x87::atan2_scaled(dx, dz);
    const i32 len = x87::ftol32(x87::crt_hypot(dx, dz));
    p->distance = len;
    const i32 elev = x87::atan2_scaled(neg(hi16(dy)), hi16(len));
    p->yaw = static_cast<u16>(yaw);
    p->elevation = static_cast<u16>(elev);
    const WeaponType& wt = *slot.weapon;
    p->speed = wt.start_velocity != 0 ? wt.start_velocity : (wt.acceleration == 0 ? wt.velocity : 0);
    set_velocity_from_angles(*p, p->speed);
    const WeaponType& pw = *p->weapon;
    if (pw.velocity != 0 && !(pw.flags & wf::kNoAutoRange))
        p->expire_tick = (static_cast<u32>(pw.range) << 16) / static_cast<u32>(pw.velocity) + w.env.game_tick;
    else
        p->expire_tick = w.env.game_tick + pw.weapon_timer;
    p->target = target;
    p->burst_left = slot.weapon->burst;
    launch_scripts(w, slot, owner);
    if (slot.weapon->flags & wf::kStartSmoke) w.systems.spawn_muzzle_smoke(muzzle);
    return true;
}

bool fire_ballistic(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& muzzle, const Vec3i& aim, Unit* target) {
    Projectile* p = alloc_projectile(w.table);
    if (!p) return false;
    init_projectile(w, *p, *slot.weapon, muzzle, nullptr, w.env.game_tick, &owner);
    const WeaponType& wt = *slot.weapon;
    p->yaw = slot.fire_heading;
    p->elevation = slot.fire_pitch;
    const u32 steps = udiv(w.table, slot.aim_drop, static_cast<u32>(wt.velocity), "ballistic velocity 0");
    const i32 drop = static_cast<i32>(steps * static_cast<u32>(w.env.gravity));
    p->vel[1] = sub(trig::mul_sin(slot.fire_pitch, wt.velocity), drop);
    const i32 h = trig::mul_cos(slot.fire_pitch, wt.velocity);
    p->vel[0] = neg(trig::mul_sin(slot.fire_heading, h));
    p->vel[2] = neg(trig::mul_cos(slot.fire_heading, h));
    if (wt.flags & wf::kBurnBlow) {
        // Burn-blow shells end over the aim point: horizontal distance over horizontal speed.
        const i32 len = x87::ftol32(x87::crt_hypot(sub(muzzle[0], aim[0]), sub(muzzle[2], aim[2])));
        i32 steps_left = 0;
        if (h == 0 || (h == -1 && len == INT32_MIN)) w.table.fault = "burn-blow shell without horizontal speed";
        else steps_left = len / h;
        p->expire_tick = static_cast<u32>(steps_left) + w.env.game_tick;
    } else {
        p->expire_tick = w.env.game_tick + wt.weapon_timer;
    }
    p->target = target;
    p->burst_left = wt.burst;
    launch_scripts(w, slot, owner);
    if (wt.flags & wf::kStartSmoke) w.systems.spawn_muzzle_smoke(muzzle);
    return true;
}

bool spawn_from_motion(ProjectileWorld& w, const FireSlot& slot, Unit& owner, const Vec3i& pos) {
    Projectile* p = alloc_projectile(w.table);
    if (!p) return false;
    init_projectile(w, *p, *slot.weapon, pos, nullptr, w.env.game_tick, &owner);
    p->speed = 0;
    p->yaw = owner.heading;
    p->vel[1] = 0;
    const i32 v = w.systems.motion_speed(owner);
    p->vel[0] = neg(trig::mul_sin(p->yaw, v));
    p->vel[2] = neg(trig::mul_cos(p->yaw, v));
    return true;
}

bool spawn_free_projectile(ProjectileWorld& w, const WeaponType& weapon, const Vec3i& pos, const Vec3i& vel, bool broadcast) {
    Projectile* p = alloc_projectile(w.table);
    if (!p) return false;
    init_projectile(w, *p, weapon, pos, nullptr, w.env.game_tick, nullptr);
    p->vel = vel;
    if (broadcast && w.env.net_game) {
        // 0x0D: position, velocity, weapon; the original sends 36 bytes, the last 10 uninitialised.
        std::uint8_t pkt[36] = {};
        pkt[0] = 0x0D;
        for (int k = 0; k < 3; ++k)
            for (int b = 0; b < 4; ++b) {
                pkt[1 + 4 * k + b] = static_cast<std::uint8_t>(static_cast<u32>(pos[static_cast<std::size_t>(k)]) >> (8 * b));
                pkt[13 + 4 * k + b] = static_cast<std::uint8_t>(static_cast<u32>(vel[static_cast<std::size_t>(k)]) >> (8 * b));
            }
        pkt[25] = weapon.index;
        w.systems.network_send(first_human_net_id(w.combat.players), pkt, 36);
    }
    return true;
}

void detonate_projectile(ProjectileWorld& w, Projectile& p, Unit* hit) {
    // Interceptors detonate the shots around them, which may be interceptors too. Shots
    // that do not explode stay live, so two of them can set each other off forever: the
    // original recurses until its stack overflows. The engine gives up the whole chain.
    ProjectileTable& t = w.table;
    if (t.runaway_chain) return;
    if (t.detonation_depth >= kMaxDetonationDepth) {
        t.fault = "endless chain of interceptor detonations";
        t.runaway_chain = true;
        return;
    }
    struct Nesting {
        ProjectileTable& t;
        explicit Nesting(ProjectileTable& table) : t(table) { ++t.detonation_depth; }
        ~Nesting() {
            if (--t.detonation_depth == 0) t.runaway_chain = false;
        }
    } nesting(t);
    const WeaponType& wt = *p.weapon;
    const MapCell* cell = w.map.cell_at(p.pos);
    const bool sea = cell && cell->height_max < w.env.sea_level;
    if (!(wt.flags & wf::kNoExplode)) end_shot(w, p);
    if (w.env.no_sea_level_trigger && sea && !hit) {
        end_shot(w, p);
        return;
    }
    w.systems.camera_shake(wt.shake_magnitude, wt.shake_magnitude, wt.shake_duration);
    if (sea && !hit) {
        w.systems.play_sound(wt.sound_water, p.pos);
        w.systems.spawn_explosion(p.pos, wt, true, true);
    } else {
        w.systems.play_sound(wt.sound_hit, p.pos);
        if (wt.flags & wf::kEndSmoke) w.systems.spawn_smoke(p.pos);
        else w.systems.spawn_explosion(p.pos, wt, false, sea);
    }
    // Watched players' shots deal no damage here (their own machine reports it).
    const PlayerSlot* owner = p.owner_slot < w.combat.players.size() ? &w.combat.players[p.owner_slot] : nullptr;
    if (owner && owner->active && owner->controller == 3) return;
    if (wt.area_of_effect <= 16 && hit) {
        const i32 dealt = deal_damage(w, p, *hit, 1.0f);
        if (p.owner) {
            const i32 low = static_cast<i32>(static_cast<u32>(dealt) & 0xFFFFu);
            if (p.owner_slot != owner_slot_of(w, *hit)) mark_owner_damage(*p.owner, low, 0);
            else mark_owner_damage(*p.owner, 0, low);
        }
        return;
    }
    const Vec3i at = p.pos;
    area_damage(w, p, at);
}

void detonate_matching(ProjectileWorld& w, const std::uint8_t* packet) {
    auto get32 = [&](int at) {
        return static_cast<i32>(packet[at] | packet[at + 1] << 8 | packet[at + 2] << 16 | static_cast<u32>(packet[at + 3]) << 24);
    };
    const Vec3i aim{get32(1), get32(5), get32(9)};
    for (i32 i = 0; i < w.table.count; ++i) {
        Projectile& p = w.table.slots[static_cast<std::size_t>(i)];
        if (p.aim == aim && p.weapon->index == packet[13]) {
            detonate_projectile(w, p, nullptr);
            return;
        }
    }
}

void remove_owner_bursts(ProjectileWorld& w, Unit& owner) {
    // After each removal the table is compacted at once and the scan goes on at the
    // next slot, so the launcher that slid into this slot is not looked at.
    for (i32 i = 0; i < w.table.count; ++i) {
        Projectile& p = w.table.slots[static_cast<std::size_t>(i)];
        if (p.burst_left != 0 && p.owner == &owner) {
            end_shot(w, p);
            compact_projectiles(w.table);
        }
    }
}

void detonate_unit_death(ProjectileWorld& w, Unit& unit, bool self_destruct) {
    if (!unit.type) return;
    const WeaponType* weapon = self_destruct ? unit.type->self_destruct_as : unit.type->explode_as;
    if (!weapon) return;
    // A temporary shot outside the table, without owner or target.
    Projectile p;
    p.weapon = weapon;
    p.owner_slot = owner_slot_of(w, unit);
    p.target = nullptr;
    p.owner = nullptr;
    p.pos = unit.pos;
    p.trail = unit.pos;
    detonate_projectile(w, p, nullptr);
}

void compact_projectiles(ProjectileTable& t) {
    // Moved shots that chase another shot get their chased slot renumbered, found by
    // the chased shot's pre-compaction index; other references are left as they are.
    std::vector<std::pair<i32, i16>> remap;  // (new slot of the chaser, old index of the chased slot)
    i32 live = t.count;
    i32 dest = -1;
    for (i32 i = 0; i < t.count; ++i) {
        Projectile& p = t.slots[static_cast<std::size_t>(i)];
        p.compact_index = static_cast<i16>(i);
        if (p.flags & pf::kDeleted) {
            if (dest < 0) dest = i;
            --live;
        } else if (dest >= 0) {
            if (t.camera_slot == i) t.camera_slot = dest;
            if (p.intercepted >= 0) remap.emplace_back(dest, static_cast<i16>(p.intercepted));
            t.slots[static_cast<std::size_t>(dest)] = p;
            ++dest;
        }
    }
    if (!remap.empty()) {
        std::size_t done = 0;
        for (i32 j = 0; j < live && done < remap.size(); ++j) {
            for (std::size_t m = 0; m < remap.size(); ++m) {
                if (t.slots[static_cast<std::size_t>(j)].compact_index != remap[m].second) continue;
                t.slots[static_cast<std::size_t>(remap[m].first)].intercepted = j;
                if (++done == remap.size()) break;
            }
        }
    }
    t.count = live;
}

void update_projectiles(ProjectileWorld& w) {
    ProjectileTable& t = w.table;
    const ProjectileEnvironment& env = w.env;
    const i32 n = t.count;  // shots added during the step wait for the next one
    for (i32 i = 0; i < n; ++i) {
        Projectile& p = t.slots[static_cast<std::size_t>(i)];
        const i16 old_y = hi16(p.pos[1]);
        const WeaponType& wt = *p.weapon;
        const u32 tick = env.game_tick;
        if (p.burst_left != 0) {
            burst_step(w, p);
            continue;
        }
        if (wt.flags & wf::kPropeller) p.propeller = static_cast<i16>(p.propeller + 0x400);
        const u32 f = wt.flags;
        bool moved = true;
        if (f & wf::kSelfPropelled) {
            if (p.expire_tick > tick) {
                if ((f & wf::kWaterWeapon) && old_y >= static_cast<i16>(env.sea_level)) {
                    // A torpedo out of the water falls.
                    p.vel[1] = sub(p.vel[1], env.gravity);
                    p.elevation = 0;
                } else {
                    if (static_cast<u32>(p.speed) < static_cast<u32>(wt.velocity)) {
                        p.speed = add(p.speed, wt.acceleration);
                        if (static_cast<u32>(p.speed) > static_cast<u32>(wt.velocity)) p.speed = wt.velocity;
                    }
                    const bool seek = (f & wf::kTwoPhase) ? (p.flags & pf::kPhaseMask) != 0 : (f & wf::kGuidance) != 0;
                    if (seek) {
                        const Vec3i to = seek_point(w, p);
                        if (!turn_toward(p, to)) detonate_projectile(w, p, nullptr);
                    }
                    set_velocity_from_angles(p, p.speed);
                }
            } else if (f & wf::kBurnBlow) {
                detonate_projectile(w, p, nullptr);
            } else {
                // Out of fuel: falls. A two-phase weapon starts its second (seeking) phase.
                p.vel[1] = sub(p.vel[1], env.gravity);
                if ((f & wf::kTwoPhase) && !(p.flags & pf::kPhaseMask)) {
                    p.expire_tick = tick + p.weapon->flight_time;
                    p.flags = static_cast<u16>(p.flags | pf::kPhase2);
                    if (!(p.weapon->flags & wf::kTracks)) {
                        p.intercepted = -1;
                        p.target = nullptr;
                    }
                }
            }
            add3(p.pos, p.vel);
        } else if (f & wf::kLineOfSight) {
            if (p.expire_tick > tick) {
                add3(p.pos, p.vel);
                if (f & wf::kBeam) {
                    if (p.flags & pf::kBeamFull) add3(p.trail, p.vel);
                    else if (tick > p.spawn_tick + wt.duration) p.flags = static_cast<u16>(p.flags | pf::kBeamFull);
                }
            } else {
                end_shot(w, p);
                moved = false;
            }
        } else if ((f & wf::kBallistic) && wt.weapon_timer != 0) {
            if (p.expire_tick > tick) {
                add3(p.pos, p.vel);
                add3(p.pos, env.wind);
                p.vel[1] = sub(p.vel[1], env.gravity);
            } else if (f & wf::kBurnBlow) {
                detonate_projectile(w, p, nullptr);
                moved = false;
            } else {
                w.systems.spawn_smoke(p.pos);
                end_shot(w, p);
                moved = false;
            }
        } else if (f & (wf::kBallistic | wf::kDropped)) {
            add3(p.pos, p.vel);
            add3(p.pos, env.wind);
            p.vel[1] = sub(p.vel[1], env.gravity);
        } else if (f & wf::kMeteor) {
            add3(p.pos, p.vel);
            p.tumble = static_cast<i16>(p.tumble + static_cast<i16>(static_cast<u16>(hi16(p.vel[0])) << 8));
            p.elevation = static_cast<u16>(p.elevation + static_cast<u16>(static_cast<u16>(hi16(p.vel[2])) << 8));
        } else {
            moved = false;
        }
        if (moved) collide(w, wt, p);
        if (p.flags & pf::kDeleted) continue;
        if ((wt.flags & wf::kSmokeTrail) && p.expire_tick > tick && p.next_smoke_tick < tick) {
            w.systems.spawn_smoke(p.pos);
            p.next_smoke_tick += wt.smoke_delay;
        }
        // Entering the water: a splash where the sea is deep enough.
        if (static_cast<i32>(old_y) > static_cast<i32>(env.sea_level) && hi16(p.pos[1]) <= static_cast<i16>(env.sea_level)) {
            const MapCell* cell = w.map.cell_at(p.pos);
            if (cell && cell->height_max < env.sea_level && !env.no_sea_level_trigger)
                w.systems.spawn_explosion(p.pos, wt, true, true);
        }
    }
    compact_projectiles(t);
}

} // namespace sim
