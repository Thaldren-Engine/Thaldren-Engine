#include "sim/construction.h"

#include <cmath>
#include <cstring>

#include "sim/features.h"
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

i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }
i32 square_hi(i32 v) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(v) * v) >> 32)); }

// Unit and type flag bits used here.
constexpr u32 kTypeBuilder = 0x40, kTypeCanFly = 0x800, kTypeAirBase = 0x200, kTypeActivateWhenBuilt = 0x40000,
              kTypeIsFeature = 0x1000000;
constexpr u32 kAbilityReclaim = 0x400, kAbilityCapture = 0x1000;
constexpr u32 kAlive = unit_flags::kAlive, kStructure = 0x20000000u, kDying = unit_tick_flags::kDying;
constexpr u32 kSelected = unit_tick_flags::kSelected;

Construction& con(Missions& m) {
    static MapGrid no_map;
    static const std::vector<FeatureTypeInfo> no_features;
    static const std::vector<ConstructionTypeInfo> no_types;
    static const HeightMap no_heights;
    if (!m.construction) {
        m.fault = "construction behaviour without the construction systems";
        static ConstructionEnvironment env;
        struct Null final : ConstructionSystems {
            Unit* create_unit(int, u16, const std::array<i32, 3>&, bool, u32) override { return nullptr; }
            bool can_place(u16, u16, i16, i16, u32) override { return false; }
            void transport_attach(Unit&, Unit*, u8, u8) override {}
            void attach_edge_goal(Unit&, MissionOrder&, u32, u32) override {}
            void attach_ring_goal(Unit&, MissionOrder&, const std::array<i32, 3>&, i32, i32) override {}
            void nano_beam_reverse(const std::array<i32, 6>&, const std::array<i32, 3>&) override {}
            void kill_feature(i32, i32, bool) override {}
            void victory_unit_transferred(Unit&) override {}
            void network_send(i32, const u8*, i32) override {}
            void broadcast_script_call_args(Unit&, i16, i32, i32) override {}
            void builder_menu(Unit&) override {}
            void build_menu_refresh(Unit&) override {}
            void orders_panel_changed() override {}
        };
        static Null null;
        static Construction none{no_map, no_features, no_types, no_heights, null, env};
        return none;
    }
    return *m.construction;
}

UnitTickState& tick_of(Missions& m, const Unit& u) { return m.orders.ticks[u.id]; }
u32 type_flags(Missions& m, const Unit& u) {
    const UnitTickTypeInfo* t = tick_of(m, u).tick_type;
    return t ? t->type_flags : 0;
}
const MissionTypeInfo& mission_type(Missions& m, const Unit& u) {
    static const MissionTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    return i < m.types.size() ? m.types[i] : none;
}
const ConstructionTypeInfo& con_type(Missions& m, std::size_t type) {
    static const ConstructionTypeInfo none;
    const auto& v = con(m).types;
    if (type >= v.size()) {
        m.fault = "unit type outside the type table";
        return none;
    }
    return v[type];
}
const ConstructionTypeInfo& con_type(Missions& m, const Unit& u) { return con_type(m, tick_of(m, u).type_index); }
const UnitTickTypeInfo& tick_type(Missions& m, const Unit& u) {
    static const UnitTickTypeInfo none;
    const UnitTickTypeInfo* t = tick_of(m, u).tick_type;
    return t ? *t : none;
}
u32 game_tick(Missions& m) { return m.orders.game_tick; }
bool has_motion(Missions& m, const Unit& u) { return tick_of(m, u).has_motion; }
bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }
PlayerSlot& owner_of(Missions& m, const Unit& u) { return m.orders.players[static_cast<std::size_t>(tick_of(m, u).owner)]; }
// A float compared with 0.0f the way FCOMP + "equal" does: NaN counts as equal.
bool is_zero(float f) { return !(f < 0.0f || f > 0.0f); }
Unit* target_of(const MissionOrder& o) { return o.target.unit; }

void set_deadline(Missions& m, MissionOrder& o, u32 ticks) {
    o.wait |= order_wait::kTimer;
    o.wake_tick = game_tick(m) + ticks;
}

void speech(Missions& m, Unit& u, u32 id, SpeechText t = SpeechText::None) { m.systems.speech(u, id, t); }

// A newly issued order announces itself once (order flag 0x2000), with a status text.
void announce(Missions& m, MissionOrder& o, SpeechText text) {
    if (!(o.flags & 0x2000)) return;
    o.flags &= ~0x2000u;
    m.systems.speech(*o.unit, 5, text);
}

// Build rate of a worker: its worker time per second, per step.
float work_rate(Missions& m, const Unit& u) { return static_cast<float>(static_cast<i32>(mission_type(m, u).worker_time) / 30); }

// A refund (or yield) into a resource slot's income: an AI player gets half on the easiest
// level and 70% on the next, as the original subtracts the amount times -0.5 / -0.7.
float with_income(Missions& m, const Unit& slot_unit, float income, double amount) {
    const PlayerSlot& p = owner_of(m, slot_unit);
    const double make = income;
    if (p.active && p.controller == 2) {
        if (con(m).env.ai_difficulty == 0) return static_cast<float>(make - amount * -0.5);
        if (con(m).env.ai_difficulty == 1) return static_cast<float>(make - amount * -0.7);
    }
    return static_cast<float>(amount + make);
}

// World position of the unit's nano piece (QueryNanoPiece, starting at 0).
std::array<i32, 3> nano_point(Missions& m, Unit& u) {
    i32 piece = 0;
    if (!u.script) {
        m.fault = "nano piece of a unit without a script";
        return u.pos;
    }
    u.script->call_in_out(u.script->program().find_function("QueryNanoPiece"), &piece, nullptr, nullptr, nullptr);
    const auto off = piece_local_offset(u, piece);
    return {wadd(u.pos[0], off[0]), wadd(u.pos[1], off[1]), wadd(u.pos[2], off[2])};
}

// The target's model box around its position (`min_y`: with the model's lowest point, else from its feet).
std::array<i32, 6> box_of(const Unit& t, bool min_y) {
    const UnitTypeInfo none;
    const UnitTypeInfo& ti = t.type ? *t.type : none;
    return {wadd(t.pos[0], ti.model_min_x), min_y ? wadd(t.pos[1], ti.model_min_y) : t.pos[1], wadd(t.pos[2], ti.model_min_z),
            wadd(t.pos[0], ti.model_max_x), wadd(t.pos[1], ti.model_max_y), wadd(t.pos[2], ti.model_max_z)};
}

// The nano lathe sprays from the worker's nano piece onto the target.
void spray_onto(Missions& m, Unit& worker, const Unit& target, bool min_y) {
    const auto from = nano_point(m, worker);
    m.systems.nano_beam(from, box_of(target, min_y));
}
// The other way round: from the target back to the worker (reclaiming, capturing).
void spray_from(Missions& m, Unit& worker, const Unit& target) {
    const auto to = nano_point(m, worker);
    con(m).systems.nano_beam_reverse(box_of(target, false), to);
}

// Heading from the unit to a point, relative to its own heading (the StartBuilding argument).
u32 relative_heading(const Unit& u, const std::array<i32, 3>& to) {
    const i32 a = x87::atan2_scaled(wsub(u.pos[0], to[0]), wsub(u.pos[2], to[2]));
    return static_cast<u16>(static_cast<u16>(a) - u.heading);
}

// StartBuilding(heading) is started (not run now) and announced; destroying the order stops it.
void start_building(Missions& m, Unit& u, MissionOrder& o, u32 heading) {
    if (!u.script) {
        m.fault = "StartBuilding of a unit without a script";
        return;
    }
    const int fn = u.script->program().find_function("StartBuilding");
    u.script->call_with_args(fn, nullptr, false, 1, static_cast<i32>(heading & 0xFFFF), 0, 0, 0);
    con(m).systems.broadcast_script_call_args(u, static_cast<i16>(fn), 1, static_cast<i32>(heading & 0xFFFF));
    o.flags |= order_flags::kBuilding;
}

void stop_building(Missions& m, Unit& u, MissionOrder& o) {
    if (!(o.flags & order_flags::kBuilding)) return;
    if (!u.script) {
        m.fault = "StopBuilding of a unit without a script";
        return;
    }
    const int fn = u.script->program().find_function("StopBuilding");
    u.script->call_with_args(fn, nullptr, false, 0, 0, 0, 0, 0);
    m.orders.systems.broadcast_script_call(u, static_cast<i16>(fn));
    o.flags &= ~order_flags::kBuilding;
}

// Waits (event 4, the script's state change) until the script says the unit stands ready to build.
std::int32_t wait_build_stance(Unit& u, MissionOrder& o, u32 wait) {
    if (u.script_flags & unit_flags::kInBuildStance) return mission_result::kAdvance;
    o.wait = wait | 4;
    return mission_result::kContinue;
}

// Whether a site at `pos` with footprint `site_x/z` is beyond the builder's reach: the
// distance minus the builder's half-diagonal plus the site's (each eight times the
// footprint diagonal in cells), compared with the build distance. `site_factor` is -8.0.
// `whole_distance` receives the distance's low 32 bits (MobileBuild keeps it in a slot).
bool beyond_reach(Missions& m, Unit& u, const std::array<i32, 3>& pos, i16 site_x, i16 site_z, i32* whole_distance = nullptr) {
    const i32 d = x87::ftol32(x87::crt_hypot(static_cast<double>(wsub(u.pos[0], pos[0])), static_cast<double>(wsub(u.pos[2], pos[2]))));
    if (whole_distance) *whole_distance = d;
    const i32 own = x87::ftol32(x87::crt_hypot(u.footprint_x, u.footprint_z) * 8.0);
    const i32 site = x87::ftol32(x87::crt_hypot(site_x, site_z) * -8.0);
    const i32 reach = wadd(wsub(hi16(d), own), site);
    return reach > static_cast<i32>(con_type(m, u).build_distance);
}

float float_bits(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

u32 pack_cell(i32 x, i32 z) { return (static_cast<u32>(x) & 0xFFFF) | (static_cast<u32>(z) << 16); }
u32 pack_footprint(i16 x, i16 z) { return pack_cell(x, z); }

// Cell origin of a footprint centred on a position (rounded to the nearest cell).
std::array<i16, 2> site_cell(const std::array<i32, 3>& pos, i16 fx, i16 fz) {
    // Centre minus half the footprint, rounded to the nearest cell (32-bit wrapping).
    const u32 hx = static_cast<u32>(static_cast<i32>(fx)) << 19, hz = static_cast<u32>(static_cast<i32>(fz)) << 19;
    return {static_cast<i16>(static_cast<i32>(static_cast<u32>(pos[0]) - hx + 0x80000u) >> 20),
            static_cast<i16>(static_cast<i32>(static_cast<u32>(pos[2]) - hz + 0x80000u) >> 20)};
}

// --- The behaviours -------------------------------------------------------

// A factory's build order (0x402640): param0 the unit type, param1 how many; the order's
// target is the unit on the pad.
std::int32_t building_build(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    Construction& c = con(m);
    if (event & 2) {  // cancelled: the unfinished unit is scrapped, its unbuilt metal refunded
        if (Unit* a = target_of(o)) {
            const double refund = static_cast<double>(static_cast<u32>(x87::ftol32(
                (1.0 - static_cast<double>(a->build_fraction_left)) * static_cast<double>(tick_type(m, *a).build_cost_metal))));
            UnitTickState& t = tick_of(m, u);
            t.metal_make = with_income(m, u, t.metal_make, refund);
            finalize_built_unit(m, &u, a);
            apply_damage(m.combat, &u, *target_of(o), 30000, 9, 0);
        }
        unit_set_activate(u, m.world, unit_flags::kActivated | unit_flags::kBuilding, false);
        m.systems.build_counts_changed(u);
        return mission_result::kFinished;
    }
    if (event & 8) {
        speech(m, u, 7, SpeechText::ConstructionStopped);
        o.param1 = wsub(o.param1, 1);
        m.systems.build_counts_changed(u);
        return mission_result::kRestart;
    }
    switch (o.step) {
    case 0:
        relink_unit(m.orders, o.target, nullptr);
        if (!(u.flags & kStructure)) return mission_result::kClearAll;
        if (o.param1 > 0) {
            unit_set_activate(u, m.world, unit_flags::kActivated, true);
            return mission_result::kAdvance;
        }
        unit_set_activate(u, m.world, unit_flags::kActivated, false);
        return mission_result::kFinished;
    case 1: return wait_build_stance(u, o, 2);
    case 2: {
        i32 piece = -1;
        if (!u.script) {
            m.fault = "QueryBuildInfo of a unit without a script";
            return mission_result::kContinue;
        }
        u.script->call_in_out(u.script->program().find_function("QueryBuildInfo"), &piece, nullptr, nullptr, nullptr);
        const auto off = piece_local_offset(u, piece);
        o.pos = {wadd(u.pos[0], off[0]), wadd(u.pos[1], off[1]), wadd(u.pos[2], off[2])};
        if (static_cast<u32>(o.param0) >= c.types.size()) {
            m.fault = "factory order for a unit type outside the type table";
            return mission_result::kContinue;
        }
        const ConstructionTypeInfo& bt = con_type(m, static_cast<u32>(o.param0));
        const auto cell = site_cell(o.pos, bt.footprint_x, bt.footprint_z);
        event = pack_cell(cell[0], cell[1]);  // the behaviour keeps the cell in its event argument
        if (!c.systems.can_place(static_cast<u16>(o.param0), 0, cell[0], cell[1], u.flags & 3u)) {
            set_deadline(m, o, 15);
            o.wait |= order_wait::kNotifyDestroy;
            return mission_result::kContinue;
        }
        Unit* made = c.systems.create_unit(tick_of(m, u).owner, static_cast<u16>(o.param0), o.pos, false, 1);
        relink_unit(m.orders, o.target, made);
        Unit* a = target_of(o);
        if (!a) {
            speech(m, u, 7, SpeechText::UnableToCreate);
            set_deadline(m, o, 300);
            o.wait |= order_wait::kNotifyDestroy;
            return mission_result::kContinue;
        }
        speech(m, u, 9, SpeechText::StartingConstruction);
        c.systems.transport_attach(*a, &u, static_cast<u8>(piece), 1);
        a = target_of(o);
        a->flags = ((u.flags ^ a->flags) & 0xC0000u) ^ a->flags;
        a->flags = ((u.flags ^ a->flags) & 0x300000u) ^ a->flags;
        if (!enqueue_order(m.orders, *a, mission::kGetBuilt, true, &u, nullptr, 0, 0)) m.fault = "endless idle-order search";
        unit_set_activate(u, m.world, unit_flags::kBuilding, true);
        m.systems.build_counts_changed(u);
        return mission_result::kAdvance;
    }
    case 3: {
        Unit* a = target_of(o);
        if (!a) break;
        if (apply_build_progress(m, u, *a, work_rate(m, u))) spray_onto(m, u, *target_of(o), false);
        if (is_zero(target_of(o)->build_fraction_left)) return mission_result::kAdvance;
        set_deadline(m, o, 1);
        o.wait |= 0xA;
        return mission_result::kContinue;
    }
    case 4:
        speech(m, u, 8);
        unit_set_activate(u, m.world, unit_flags::kBuilding, false);
        finalize_built_unit(m, &u, target_of(o));
        relink_unit(m.orders, o.target, nullptr);
        o.param1 = wsub(o.param1, 1);
        m.systems.build_counts_changed(u);
        return mission_result::kRestart;
    default: break;
    }
    return mission_result::kClearAll;
}

// A mobile builder's order to build param0 at the order's position (0x403A20); param2
// counts the waits for a blocked site; the target is the unit being built.
std::int32_t mobile_build(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    Construction& c = con(m);
    if (event & 8) {
        speech(m, u, 7, SpeechText::ConstructionTerminated);
        m.systems.display_changed(u);
        return 8;
    }
    if (event & 2) {
        m.systems.display_changed(u);
        return mission_result::kFinished;
    }
    switch (o.step) {
    case 0: {
        const ConstructionTypeInfo& bt = con_type(m, static_cast<u32>(o.param0));
        const auto cell = site_cell(o.pos, bt.footprint_x, bt.footprint_z);
        event = pack_cell(cell[0], cell[1]);
        o.pos[0] = static_cast<i32>(static_cast<u32>(bt.footprint_x + cell[0] * 2) << 19);
        o.pos[2] = static_cast<i32>(static_cast<u32>(bt.footprint_z + cell[1] * 2) << 19);
        o.param2 = 0;
        c.systems.attach_edge_goal(u, o, event, pack_footprint(bt.footprint_x, bt.footprint_z));
        o.wait = 0xE0;
        return mission_result::kAdvance;
    }
    case 1: {
        const ConstructionTypeInfo& bt = con_type(m, static_cast<u32>(o.param0));
        if (event & 0x40) {
            i32 d = 0;
            const bool far = beyond_reach(m, u, o.pos, bt.footprint_x, bt.footprint_z, &d);
            event = static_cast<u32>(d);
            if (far) {
                speech(m, u, 7, SpeechText::CantReachSite);
                return 8;
            }
        }
        const auto cell = site_cell(o.pos, bt.footprint_x, bt.footprint_z);
        event = pack_cell(cell[0], cell[1]);
        if (!c.systems.can_place(static_cast<u16>(o.param0), 0, cell[0], cell[1], 1)) {
            if (o.param2 == 0) {
                speech(m, u, 7, SpeechText::WaitingForArea);
            } else if (o.param2 > 10) {
                speech(m, u, 7, SpeechText::AreaBlocked);
                return 8;
            }
            o.param2 = wadd(o.param2, 1);
            set_deadline(m, o, 30);
            return mission_result::kContinue;
        }
        unrelease_weapon_slot(u, 3);
        snap_to_footprint(c, static_cast<u16>(o.param0), o.pos);
        Unit* made = c.systems.create_unit(tick_of(m, u).owner, static_cast<u16>(o.param0), o.pos, false, 1);
        relink_unit(m.orders, o.target, made);
        if (!target_of(o)) {
            speech(m, u, 7, SpeechText::UnableToCreate);
            set_deadline(m, o, 300);
            return mission_result::kContinue;
        }
        speech(m, u, 9, SpeechText::StartingConstruction);
        m.systems.display_changed(u);
        if (!enqueue_order(m.orders, *target_of(o), mission::kGetBuilt, true, &u, nullptr, 0, 0)) m.fault = "endless idle-order search";
        start_building(m, u, o, relative_heading(u, target_of(o)->pos));
        return mission_result::kAdvance;
    }
    case 2: return wait_build_stance(u, o, 10);
    case 3: {
        Unit* a = target_of(o);
        if (!a) {
            m.fault = "mobile build step without the unit being built";
            return mission_result::kContinue;
        }
        if (apply_build_progress(m, u, *a, work_rate(m, u))) spray_onto(m, u, *target_of(o), true);
        u.cloak_holdoff_tick = game_tick(m) + 300;
        if (is_zero(target_of(o)->build_fraction_left)) return mission_result::kAdvance;
        set_deadline(m, o, 1);
        o.wait |= 0xA;
        return mission_result::kContinue;
    }
    case 4: speech(m, u, 8, SpeechText::BuildingComplete); return mission_result::kFinished;
    default: return mission_result::kClearAll;
    }
}

// Helping to build the order's target (0x403F70).
std::int32_t help_build(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Construction& c = con(m);
    if (event & 2) {
        m.systems.display_changed(u);
        return mission_result::kFinished;
    }
    Unit* t = target_of(o);
    if (!t) {
        speech(m, u, 7, SpeechText::ConstructionTerminated);
        return 8;
    }
    switch (o.step) {
    case 0: {
        if (!has_motion(m, u) || !(type_flags(m, u) & kTypeBuilder)) return mission_result::kClearAll;
        const ConstructionTypeInfo& tt = con_type(m, *t);
        // Half of sixteen times sqrt(x * x + 2 * z) (the z footprint is not squared).
        const double fx = tt.footprint_x, fz = tt.footprint_z;
        const i32 v = x87::ftol32(std::sqrt((fx * fx + fz) + fz) * 16.0);
        const i32 half = v / 2;
        c.systems.attach_ring_goal(u, o, t->pos, wadd(static_cast<i32>(con_type(m, u).build_distance), half), half);
        o.wait = 0xE8;
        return mission_result::kAdvance;
    }
    case 1:
        if (event & 0x40) {
            speech(m, u, 7, SpeechText::CantGetThere);
            return 8;
        }
        if (is_zero(t->build_fraction_left)) return mission_result::kFinished;
        unrelease_weapon_slot(u, 3);
        start_building(m, u, o, relative_heading(u, target_of(o)->pos));
        m.systems.display_changed(u);
        return mission_result::kAdvance;
    case 2: return wait_build_stance(u, o, 10);
    case 3:
        if (apply_build_progress(m, u, *t, work_rate(m, u))) spray_onto(m, u, *target_of(o), true);
        u.cloak_holdoff_tick = game_tick(m) + 300;
        if (!is_zero(target_of(o)->build_fraction_left)) {
            set_deadline(m, o, 1);
            o.wait |= 0xA;
            return mission_result::kContinue;
        }
        return mission_result::kAdvance;
    case 4:
        speech(m, u, 8, SpeechText::BuildingComplete);
        o.wait |= order_wait::kNotifyDestroy;
        return mission_result::kFinished;
    default: return mission_result::kClearAll;
    }
}

// Repairing the order's target, moving next to it first (0x405300). param2 = leash: the
// order ends once the unit is that far from where the order started (+0x2E/+0x30).
std::int32_t repair_unit(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Construction& c = con(m);
    if (!target_of(o)) {
        speech(m, u, 7, SpeechText::RepairsUnsuccessful);
        return mission_result::kFinished;
    }
    if (o.param2 != 0) {
        const i32 ox = static_cast<i16>(o.scratch0 & 0xFFFF), oz = static_cast<i16>(o.scratch0 >> 16);
        const i32 d = x87::ftol32(x87::crt_hypot(static_cast<double>(hi16(u.pos[0]) - ox), static_cast<double>(hi16(u.pos[2]) - oz)));
        if (d >= o.param2) return mission_result::kFinished;
    }
    Unit* t = target_of(o);
    const u32 tflags = t->flags;
    if ((tflags & 3u) != 1) {
        speech(m, u, 7, SpeechText::RepairsUnsuccessful);
        return mission_result::kFinished;
    }
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !(type_flags(m, u) & kTypeBuilder) || !is_zero(t->build_fraction_left)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Repairing);
        return mission_result::kAdvance;
    case 1:
        if (event & 0x40) return 8;
        if (beyond_reach(m, u, t->pos, t->footprint_x, t->footprint_z)) {
            c.systems.attach_edge_goal(u, o, pack_cell(t->cell_x, t->cell_z), pack_footprint(t->footprint_x, t->footprint_z));
            set_deadline(m, o, m.orders.rng.next_mod(30) + 30);
            o.wait |= 0xE8;
            return mission_result::kContinue;
        }
        unrelease_weapon_slot(u, 3);
        start_building(m, u, o, relative_heading(u, target_of(o)->pos));
        return mission_result::kAdvance;
    case 2: return wait_build_stance(u, o, 8);
    case 3: {
        const u32 max = t->type ? t->type->max_damage : 0;
        if (static_cast<u32>(static_cast<i32>(t->hit_points)) >= max) return mission_result::kAdvance;
        if (tflags & 0xCu) {
            stop_building(m, u, o);
            set_deadline(m, o, 15);
            return mission_result::kRestart;
        }
        u.cloak_holdoff_tick = game_tick(m) + 150;
        if (repair_heal(m.combat, u, *target_of(o), work_rate(m, u))) spray_onto(m, *o.unit, *target_of(o), false);
        set_deadline(m, o, 1);
        o.wait |= 8;
        return mission_result::kContinue;
    }
    case 4: speech(m, u, 10, SpeechText::UnitRepaired); return mission_result::kFinished;
    default: return mission_result::kClearAll;
    }
}

// The feature at a world position: its type (0xFFFF none), its own cell and its footprint.
u16 feature_at(Missions& m, const std::array<i32, 3>& pos, u32* cell_out, u32* footprint_out) {
    Construction& c = con(m);
    i16 x = static_cast<i16>(pos[0] >> 20), z = static_cast<i16>(pos[2] >> 20);
    const MapCell* cell = c.map.cell(x, z);
    if (!cell) return 0xFFFF;
    if (cell->feature == kFeaturePart) {
        z = static_cast<i16>(z - (cell->feature_aux & 0xFF));
        x = static_cast<i16>(x - (cell->feature_aux >> 8));
        cell = c.map.cell(x, z);
        if (!cell) {
            m.fault = "feature part pointing outside the map";
            return 0xFFFF;
        }
    }
    if (cell->feature >= kFirstSpecialFeature) return 0xFFFF;
    if (cell_out) *cell_out = pack_cell(x, z);
    if (footprint_out) {
        if (cell->feature >= c.features.size()) {
            m.fault = "feature type outside the feature table";
            return 0xFFFF;
        }
        const FeatureTypeInfo& f = c.features[cell->feature];
        *footprint_out = pack_footprint(f.footprint_x, f.footprint_z);
    }
    return cell->feature;
}

// Reclaiming the feature at the order's position (0x404AD0); param0 counts down the work left.
std::int32_t reclaim(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Construction& c = con(m);
    u32 cell = 0, footprint = 0;
    const u16 id = feature_at(m, o.pos, &cell, &footprint);
    if (id == 0xFFFF) {
        speech(m, u, 7, SpeechText::ReclamationFailed);
        return 8;
    }
    if (id >= c.features.size()) {
        m.fault = "feature type outside the feature table";
        return mission_result::kContinue;
    }
    const FeatureTypeInfo& f = c.features[id];
    if (!(f.flags & 0x80)) return 8;  // not reclaimable
    const i16 cx = static_cast<i16>(cell & 0xFFFF), cz = static_cast<i16>(cell >> 16);
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !(mission_type(m, u).abilities & kAbilityReclaim)) return mission_result::kClearAll;
        c.systems.attach_edge_goal(u, o, cell, footprint);
        o.wait = 0xE0;
        return mission_result::kAdvance;
    case 1: {
        if (event & 0x40) return 8;
        o.param0 = x87::ftol32(15.0 - (static_cast<double>(f.energy) + static_cast<double>(f.metal)) * -0.5);
        const i16 fx = static_cast<i16>(footprint & 0xFFFF), fz = static_cast<i16>(footprint >> 16);
        std::array<i32, 3> at = {static_cast<i32>(static_cast<u32>(fx + cx * 2) << 19), 0, static_cast<i32>(static_cast<u32>(fz + cz * 2) << 19)};
        const u32 r = m.orders.rng.next_mod(f.height);
        const i32 ground = c.heights.sample(static_cast<i16>(hi16(at[0])), static_cast<i16>(hi16(at[2])));
        at[1] = static_cast<i32>((r + static_cast<u32>(ground)) << 16);
        start_building(m, u, o, relative_heading(u, at));
        return mission_result::kAdvance;
    }
    case 2: return wait_build_stance(u, o, 0);
    case 3: speech(m, u, 0xB); [[fallthrough]];
    case 4: {
        set_deadline(m, o, 2);
        o.param0 = wsub(o.param0, 2);
        if (o.param0 <= 0) return mission_result::kAdvance;
        u.cloak_holdoff_tick = game_tick(m) + 300;
        if (o.param0 > 15) {
            const auto to = nano_point(m, u);
            const i32 x = static_cast<i32>(static_cast<u32>(static_cast<i32>(cx)) << 20);
            const i32 z = static_cast<i32>(static_cast<u32>(static_cast<i32>(cz)) << 20);
            const i32 ground = c.heights.sample(static_cast<i16>(hi16(x)), static_cast<i16>(hi16(z)));
            const i32 y = static_cast<i32>(static_cast<u32>(ground) << 16);
            const std::array<i32, 6> box = {x, y, z, wadd(x, static_cast<i32>(static_cast<u32>(static_cast<i32>(f.footprint_x)) << 20)),
                                            wadd(y, static_cast<i32>(static_cast<u32>(f.height) << 16)),
                                            wadd(z, static_cast<i32>(static_cast<u32>(static_cast<i32>(f.footprint_z)) << 20))};
            c.systems.nano_beam_reverse(box, to);
            c.systems.nano_beam_reverse(box, to);
        }
        return mission_result::kContinue;
    }
    case 5: reclaim_feature(m, u, o.pos); return mission_result::kFinished;
    default: return mission_result::kClearAll;
    }
}

// Reclaiming the order's target unit (0x404730): pulses of damage of kind 5 every 15 steps.
std::int32_t reclaim_unit(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Construction& c = con(m);
    Unit* t = target_of(o);
    if (!t || (event & 0x10008u)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (has_motion(m, u) && (mission_type(m, u).abilities & kAbilityReclaim)) {
            if (can_reclaim(m, u, *t)) {
                announce(m, o, SpeechText::Reclaiming);
                unrelease_weapon_slot(u, 3);
                return mission_result::kAdvance;
            }
            speech(m, u, 7, SpeechText::CannotBeReclaimed);
            speech(m, u, 7, SpeechText::ReclamationFailed);
            return 8;
        }
        speech(m, u, 7, SpeechText::ReclamationFailed);
        return mission_result::kClearAll;
    case 1:
        if (event & 0x20) return mission_result::kAdvance;
        c.systems.attach_edge_goal(u, o, pack_cell(t->cell_x, t->cell_z), pack_footprint(t->footprint_x, t->footprint_z));
        o.wait |= 0x100E8;
        set_deadline(m, o, 15);
        o.param0 = reclaim_pulse(m, u, *target_of(o), 15);
        o.param1 = 0;
        return mission_result::kContinue;
    case 2:
        if (event & 0x40) return mission_result::kFinishOrIdle;
        start_building(m, u, o, relative_heading(u, t->pos));
        return mission_result::kAdvance;
    case 3: return wait_build_stance(u, o, 0x10008);
    case 4: speech(m, u, 0xB); return mission_result::kAdvance;
    case 5: {
        const i32 dx = wsub(u.pos[0], t->pos[0]), dz = wsub(u.pos[2], t->pos[2]);
        const i32 reach = wadd(static_cast<i32>(con_type(m, u).build_distance), con_type(m, *t).reclaim_reach);
        if (wadd(square_hi(dx), square_hi(dz)) <= static_cast<i32>(static_cast<u32>(reach) * static_cast<u32>(reach)) && can_reclaim(m, u, *target_of(o))) {
            if (o.param1 > 14) {
                apply_damage(m.combat, &u, *target_of(o), o.param0, 5, 0);
                o.param1 = 0;
            }
            u.cloak_holdoff_tick = game_tick(m) + 900;
            spray_from(m, *o.unit, *target_of(o));
            set_deadline(m, o, 2);
            o.param1 = wadd(o.param1, 2);
            return mission_result::kContinue;
        }
        set_deadline(m, o, 15);
        stop_building(m, u, o);
        return mission_result::kRestart;
    }
    default: return mission_result::kClearAll;
    }
}

// Capturing the order's target (0x404270): param1 is the work needed, param0 the work done.
std::int32_t capture(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Construction& c = con(m);
    Unit* t = target_of(o);
    if (!t || (event & 0x10008u)) {
        speech(m, u, 7, SpeechText::CaptureFailed);
        return 8;
    }
    switch (o.step) {
    case 0: {
        if (!has_motion(m, u)) return mission_result::kClearAll;
        if (!(mission_type(m, u).abilities & kAbilityCapture)) return mission_result::kClearAll;
        if (mission_type(m, *t).abilities & kAbilityCapture) {
            speech(m, u, 7, SpeechText::CannotBeCaptured);
            return 8;
        }
        if (!is_zero(t->build_fraction_left)) {
            speech(m, u, 7, SpeechText::CloudOfVapor);
            return 8;
        }
        announce(m, o, SpeechText::Capturing);
        const UnitTickTypeInfo& tt = tick_type(m, *target_of(o));
        // Base time from the target's costs (each product rounded to double, constants as floats).
        const double e = static_cast<double>(tt.build_cost_energy) * 30.0 * static_cast<double>(float_bits(0x3A03126Fu));
        const double mt = static_cast<double>(tt.build_cost_metal) * 30.0 * static_cast<double>(float_bits(0xBBEA0EA1u));
        i32 work = x87::ftol32((e - mt) - -150.0);
        if (work >= 0x708) work = 0x708;
        o.param1 = work;
        Unit* tg = target_of(o);
        const u32 max = tg->type ? tg->type->max_damage : 0;
        if (max == 0) {
            m.fault = "capture of a unit type without hit points";
            return mission_result::kContinue;
        }
        const u32 scaled = static_cast<u32>(wadd(tg->hit_points, static_cast<i32>(max)) * work) / (max << 1);
        o.param1 = static_cast<i32>(scaled);
        const i32 kills = tick_of(m, *tg).kill_count;
        o.param1 = static_cast<i32>(static_cast<u32>(kills / 5 + 10) * scaled * 10u) / 100;
        unrelease_weapon_slot(u, 3);
        Unit* tg2 = target_of(o);
        c.systems.attach_edge_goal(u, o, pack_cell(tg2->cell_x, tg2->cell_z), pack_footprint(tg2->footprint_x, tg2->footprint_z));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 1:
        if (event & 0x40) return 8;
        if (!beyond_reach(m, u, t->pos, t->footprint_x, t->footprint_z)) {
            start_building(m, u, o, relative_heading(u, target_of(o)->pos));
            return mission_result::kAdvance;
        }
        return mission_result::kRestart;
    case 2: return wait_build_stance(u, o, 0x10008);
    case 3: speech(m, u, 0xB); return mission_result::kAdvance;
    case 4:
        if (has_motion(m, *t) && (t->flags & 0xCu)) {
            stop_building(m, u, o);
            set_deadline(m, o, 30);
            return mission_result::kRestart;
        }
        if (o.param0 < o.param1) {
            spray_from(m, *o.unit, *target_of(o));
            u.cloak_holdoff_tick = game_tick(m) + 900;
            o.param0 = wadd(o.param0, 2);
            set_deadline(m, o, 2);
            return mission_result::kContinue;
        }
        return mission_result::kAdvance;
    case 5:
        transfer_ownership(m, *t, tick_of(m, u).owner);
        speech(m, u, 0x10);
        return mission_result::kFinished;
    default: return mission_result::kClearAll;
    }
}

// Being carried (0x402FC0): weapons are free while the carrier holds the unit.
std::int32_t be_carried(Missions& m, Unit& u, MissionOrder& o) {
    if (!u.carrier) return mission_result::kFinished;
    if (o.step == 0) {
        unrelease_weapon_slot(u, 3);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    set_deadline(m, o, 10);
    return mission_result::kContinue;
}

// Parking (0x4061A0): a new unit drives off its factory pad to the edge of a box around it.
std::int32_t park(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    if (o.step == 0) {
        if (!has_motion(m, u)) return mission_result::kClearAll;
        if (type_flags(m, u) & kTypeCanFly) {
            o.pos = u.pos;
            const u8 kind = mission::kAirMove;
            const u32 d = mission::default_flags()[kind];
            o.kind = kind;
            o.flags = ((d ^ o.flags) & 0x600u) ^ d;
            return mission_result::kRestart;
        }
        i16 f = u.footprint_x;
        if (con_type(m, u).min_water_depth >= 0) f = static_cast<i16>(f + 3);
        const u16 cx = static_cast<u16>(static_cast<u32>(u.pos[0]) >> 20), cz = static_cast<u16>(static_cast<u32>(u.pos[2]) >> 20);
        const u32 corner = pack_cell(static_cast<u16>(cx - f * 4), static_cast<u16>(cz - f * 3));
        event = corner;
        con(m).systems.attach_edge_goal(u, o, corner, pack_cell(static_cast<u16>(f * 8), static_cast<u16>(f * 6)));
        o.wait = 0xE0;
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    if (event & 0x20) return mission_result::kFinished;
    if (o.next) return mission_result::kFinished;
    set_deadline(m, o, 30);
    return mission_result::kRestart;
}

} // namespace

bool construction_kind(std::uint8_t kind) {
    switch (kind) {
    case mission::kBuildingBuild: case mission::kMobileBuild: case mission::kHelpBuild: case mission::kRepairUnit:
    case mission::kReclaim: case mission::kReclaimUnit: case mission::kCapture: case mission::kBeCarried: case mission::kPark:
        return true;
    default: return false;
    }
}

std::int32_t run_construction_behaviour(Missions& m, Unit& u, MissionOrder& o, std::uint32_t& event) {
    switch (o.kind) {
    case mission::kBuildingBuild: return building_build(m, u, o, event);
    case mission::kMobileBuild: return mobile_build(m, u, o, event);
    case mission::kHelpBuild: return help_build(m, u, o, event);
    case mission::kRepairUnit: return repair_unit(m, u, o, event);
    case mission::kReclaim: return reclaim(m, u, o, event);
    case mission::kReclaimUnit: return reclaim_unit(m, u, o, event);
    case mission::kCapture: return capture(m, u, o, event);
    case mission::kBeCarried: return be_carried(m, u, o);
    case mission::kPark: return park(m, u, o, event);
    default: return m.systems.run_other(u, o, event);
    }
}

bool apply_build_progress(Missions& m, Unit& builder, Unit& target, float work) {
    if (is_zero(target.build_fraction_left)) return false;
    if (work >= 0.0f) target.net_dirty |= 0x8000;
    if (is_zero(work)) return false;
    const UnitTickTypeInfo& tt = tick_type(m, target);
    const u32 max = target.type ? target.type->max_damage : 0;
    // The new fraction at 53-bit precision, clamped to [0, 1] and stored as a float.
    const double left = target.build_fraction_left;
    const double next = left - static_cast<double>(work) / static_cast<double>(tt.build_time);
    const double clamped = next > 0.0 ? next : 0.0;
    float next_f;
    if (!(clamped < 1.0)) next_f = 1.0f;
    else if (next > 0.0) next_f = static_cast<float>(next);
    else next_f = 0.0f;
    const float slice = static_cast<float>(left - static_cast<double>(next_f));
    const float energy = static_cast<float>(static_cast<double>(tt.build_cost_energy) * static_cast<double>(slice));
    const float metal = static_cast<float>(static_cast<double>(tt.build_cost_metal) * static_cast<double>(slice));
    const i32 gain = wsub(x87::ftol32(left * static_cast<double>(max)), x87::ftol32(static_cast<double>(next_f) * static_cast<double>(max)));
    bool paid = false;
    if (work >= 0.0f) {
        if (m.systems.use_resources_if_not_stalled(builder, energy, metal)) {
            u32 hp = static_cast<u32>(wadd(gain, target.hit_points));
            if (hp >= max) hp = max;
            target.hit_points = static_cast<i16>(hp);
            target.build_fraction_left = next_f;
            target.flags |= 0x2000;
            paid = true;
        }
    } else {
        // Unbuilding: the unbuilt metal goes back to the target's own slot.
        UnitTickState& t = tick_of(m, target);
        t.metal_make = with_income(m, target, t.metal_make, -static_cast<double>(metal));
        const i32 hp = wadd(gain, target.hit_points);
        target.build_fraction_left = next_f;
        target.hit_points = static_cast<i16>(hp <= 0 ? 0 : hp);
        target.flags |= 0x2000;
        if (!(next_f < 1.0f)) apply_damage(m.combat, &target, target, 30000, 9, 0);
    }
    if (is_zero(target.build_fraction_left)) finalize_built_unit(m, &builder, &target);
    return paid;
}

void unfinished_decay(Missions& m, Unit& unit, std::int32_t steps) {
    const UnitTickTypeInfo& tt = tick_type(m, unit);
    const i32 product = static_cast<i32>(static_cast<u32>(tt.build_time) * static_cast<u32>(steps));
    const float work = static_cast<float>(-(static_cast<double>(product) / static_cast<double>(tt.build_cost_energy)));
    apply_build_progress(m, unit, unit, work);
}

void finalize_built_unit(Missions& m, Unit* builder, Unit* built) {
    if (!builder || !(builder->flags & kAlive) || !con_type(m, *builder).has_build_options) return;
    if (!built || !(built->flags & kAlive)) return;
    Construction& c = con(m);
    // (The unit's drawing state forgets its frame: presentation.)
    const u32 before = built->flags;
    built->build_fraction_left = 0.0f;
    built->flags = before | 0x2000;
    const PlayerSlot& owner = owner_of(m, *built);
    if (plays(owner)) {
        if (before & kStructure) {
            c.systems.builder_menu(*built);
        } else if (built->carrier) {
            c.systems.transport_attach(*built, nullptr, 0xFF, 1);
        }
    }
    if (type_flags(m, *built) & kTypeActivateWhenBuilt) unit_set_activate(*built, m.world, unit_flags::kActivated, true);
    if (c.env.build_menu_focus == builder->id) c.systems.build_menu_refresh(*builder);
    if (type_flags(m, *built) & kTypeIsFeature) {
        tick_of(m, *built).last_damage_kind = 7;
        built->flags |= kDying;
    }
    if (plays(owner_of(m, *built))) {
        u8 pk[5] = {0x12, static_cast<u8>(built->id), static_cast<u8>(built->id >> 8), static_cast<u8>(builder->id),
                    static_cast<u8>(builder->id >> 8)};
        c.systems.network_send(owner_of(m, *builder).net_id, pk, 5);
    }
    if ((builder->flags & kSelected) || (built->flags & kSelected)) c.systems.orders_panel_changed();
}

std::int32_t reclaim_pulse(Missions& m, Unit& worker, Unit& target, std::int32_t steps) {
    const float cost = tick_type(m, target).build_cost_metal;
    const double metal = cost > 10.0f ? static_cast<double>(cost) : 10.0;
    const i32 factor = (static_cast<i32>(tick_of(m, worker).kill_count) + 5) / 5;
    const u32 max = target.type ? target.type->max_damage : 0;
    const u32 product = static_cast<u32>(mission_type(m, worker).worker_time) * static_cast<u32>(factor) * max * static_cast<u32>(steps);
    const i32 hp = x87::ftol32(static_cast<double>(product) / (metal * 300.0));
    return hp > 1 ? hp : 1;
}

bool can_reclaim(Missions& m, Unit& worker, Unit& target) {
    return (mission_type(m, worker).abilities & kAbilityReclaim) && (target.flags & 3u) != 2 &&
           !(mission_type(m, target).abilities & kAbilityCapture);
}

void transfer_ownership(Missions& m, Unit& unit, int new_owner) {
    UnitTickState& t = tick_of(m, unit);
    if (t.owner == new_owner || !(unit.flags & kAlive) || (unit.flags & kDying)) return;
    Construction& c = con(m);
    c.systems.victory_unit_transferred(unit);
    const PlayerSlot& from = m.orders.players[static_cast<std::size_t>(t.owner)];
    const PlayerSlot& to = m.orders.players[static_cast<std::size_t>(new_owner)];
    if (plays(from)) {
        if (!to.active) return;
        if (to.controller == 3) {
            // A watched player's machine takes the unit over: announce it, then it is gone here.
            t.transfer_holdoff = 0x96;
            unit.flags &= ~kSelected;
            u8 pk[0x18] = {0x14};
            auto put16 = [&](int at, u32 v) { pk[at] = static_cast<u8>(v); pk[at + 1] = static_cast<u8>(v >> 8); };
            auto put32 = [&](int at, u32 v) { put16(at, v); put16(at + 2, v >> 16); };
            put16(1, unit.id);
            put32(3, static_cast<u32>(to.controller != 0 ? to.net_id : -1));
            put32(7, static_cast<u32>(x87::ftol32(unit.build_fraction_left)));
            put32(0xB, static_cast<u32>(static_cast<i32>(unit.hit_points)));
            put32(0xF, static_cast<u16>(unit.pitch) | static_cast<u32>(unit.heading) << 16);
            put16(0x13, static_cast<u16>(unit.bank));
            if (unit.weapons[0].flags & 2) {
                pk[0x15] = unit.weapons[0].stockpile;
                pk[0x16] = unit.weapons[1].stockpile;
                pk[0x17] = unit.weapons[2].stockpile;
            }
            c.systems.network_send(from.net_id, pk, 0x18);
            apply_damage(m.combat, nullptr, unit, 30000, 4, 0);
            return;
        }
    }
    if (!plays(to)) return;
    Unit* copy = c.systems.create_unit(new_owner, t.type_index, unit.pos, true, unit.flags & 3u);
    if (!copy) return;
    copy->flags &= 0xFFC3FFFFu;
    copy->hit_points = unit.hit_points;
    copy->build_fraction_left = unit.build_fraction_left;
    copy->pitch = unit.pitch;
    copy->heading = unit.heading;
    copy->bank = unit.bank;
    for (int s = 0; s < 3; ++s)
        if (copy->weapons[static_cast<std::size_t>(s)].flags & 2) copy->weapons[static_cast<std::size_t>(s)].stockpile = unit.weapons[static_cast<std::size_t>(s)].stockpile;
    apply_damage(m.combat, nullptr, unit, 30000, 4, 0);
    unit_set_activate(*copy, m.world, unit.activate_flags, true);
    unit_set_activate(*copy, m.world, static_cast<u8>(~unit.activate_flags), false);
}

namespace {
// The feature cell a part of a bigger feature refers back to (rows back in the low byte,
// columns back in the high byte), by index arithmetic on the cell table as the original.
const MapCell* own_cell(const MapGrid& map, i32 x, i32 z, u16 aux, std::string* fault) {
    const i64 index = static_cast<i64>(z) * map.width + x - (static_cast<i64>(aux & 0xFF) * map.width + (aux >> 8));
    if (index < 0 || index >= static_cast<i64>(map.cells.size())) {
        if (fault) *fault = "feature part pointing outside the map";
        return nullptr;
    }
    return &map.cells[static_cast<std::size_t>(index)];
}

// Feature type of a cell for the yard map tests: special ids and ids past the table give
// none; a part takes its feature's type (whose id the original does not check).
const FeatureTypeInfo* placement_feature(const Construction& c, const MapCell& cell, i32 x, i32 z) {
    u16 id = cell.feature;
    if (id == kFeaturePart) {
        const MapCell* own = own_cell(c.map, x, z, cell.feature_aux, nullptr);
        if (!own || own->feature >= kFirstSpecialFeature) return nullptr;
        id = own->feature;
    } else if (id >= kFirstSpecialFeature || id >= c.features.size()) {
        return nullptr;
    }
    return id < c.features.size() ? &c.features[id] : nullptr;
}

// Whether a cell's feature blocks a structure: none no; a type by its blocking flag (ids
// past the table block); a part by its feature's flag (special there: no); other special
// ids block.
bool placement_blocked(const Construction& c, const MapCell& cell, i32 x, i32 z) {
    const u16 id = cell.feature;
    if (id == kNoFeature) return false;
    if (id < kFirstSpecialFeature) return id >= c.features.size() || (c.features[id].flags & 0x40);
    if (id != kFeaturePart) return true;
    const MapCell* own = own_cell(c.map, x, z, cell.feature_aux, nullptr);
    if (!own || own->feature >= kFirstSpecialFeature) return false;
    return own->feature >= c.features.size() || (c.features[own->feature].flags & 0x40);
}
} // namespace

bool structure_fits(const Construction& c, std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) {
    if (type >= c.types.size()) return false;
    const ConstructionTypeInfo& t = c.types[type];
    const i32 fx = t.footprint_x, fz = t.footprint_z;
    if (cell_x < 1 || cell_z < 1 || fx + cell_x >= c.map.width || fz + cell_z >= c.map.height) return false;
    u8 lo = 0xFF, hi = 0, water = 0;
    bool needs_vent = false, has_vent = false;
    std::size_t k = 0;
    for (i32 row = 0; row < fz; ++row)
        for (i32 col = 0; col < fx; ++col) {
            const i32 x = cell_x + col, z = cell_z + row;
            const MapCell* cell = c.map.cell(x, z);
            const u8 y = k < t.yard_map.size() ? t.yard_map[k] : 0;  // (past the end the original reads on)
            ++k;
            if (!cell) return false;
            if (y & 8) {
                if (cell->height_min < lo) lo = cell->height_min;
                if (cell->height_max > hi) hi = cell->height_max;
            }
            if ((y & 0x10) && cell->height_max > water) water = cell->height_max;
            if ((y & 1) && (cell->flags & 2)) return false;
            if ((y & 6) && cell->unit != 0) return false;
            if ((y & 0x20) && placement_blocked(c, *cell, x, z)) return false;
            if (y & 0x40) {
                const FeatureTypeInfo* f = placement_feature(c, *cell, x, z);
                if (f && (f->flags & 0x200)) return false;
            }
            if (y & 0x80) {
                needs_vent = true;
                const FeatureTypeInfo* f = placement_feature(c, *cell, x, z);
                if (f && (f->flags & 0x20)) has_vent = true;
            }
        }
    if (needs_vent && !has_vent) return false;
    const u8 sea = c.env.sea_level;
    u8 h;
    if (hi < lo) {
        h = static_cast<u8>(sea - t.waterline);
    } else {
        if (static_cast<i32>(hi) - static_cast<i32>(lo) > static_cast<i32>(t.max_slope)) return false;
        h = lo;
    }
    if (water > h) return false;
    if (static_cast<i32>(lo) < static_cast<i32>(sea) - t.max_water_depth) return false;
    const u8 top = hi > water ? hi : water;
    if (static_cast<i32>(top) > static_cast<i32>(sea) - t.min_water_depth) return false;
    return true;
}

std::uint8_t structure_place_height(const Construction& c, std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) {
    if (type >= c.types.size()) return 0;
    const ConstructionTypeInfo& t = c.types[type];
    const i32 fx = t.footprint_x, fz = t.footprint_z;
    if (cell_x < 1 || cell_z < 1 || cell_x + fx >= c.map.width || cell_z + fz >= c.map.height) return 0;
    u8 lo = 0xFF, hi = 0;
    std::size_t k = 0;
    for (i32 row = 0; row < fz; ++row)
        for (i32 col = 0; col < fx; ++col) {
            const MapCell* cell = c.map.cell(cell_x + col, cell_z + row);
            const u8 y = k < t.yard_map.size() ? t.yard_map[k] : 0;
            ++k;
            if (cell && (y & 8)) {
                if (cell->height_min < lo) lo = cell->height_min;
                if (cell->height_max > hi) hi = cell->height_max;
            }
        }
    if (hi < lo) return static_cast<u8>(c.env.sea_level - t.waterline);
    return lo;
}

void snap_to_footprint(const Construction& c, std::uint16_t type, std::array<std::int32_t, 3>& pos) {
    if (type >= c.types.size() || c.types[type].movement != 0) return;
    const ConstructionTypeInfo& t = c.types[type];
    const auto cell = site_cell(pos, t.footprint_x, t.footprint_z);
    pos[0] = static_cast<i32>(static_cast<u32>(t.footprint_x + cell[0] * 2) << 19);
    pos[2] = static_cast<i32>(static_cast<u32>(t.footprint_z + cell[1] * 2) << 19);
    pos[1] = static_cast<i32>(static_cast<u32>(structure_place_height(c, type, cell[0], cell[1])) << 16);
}

bool reclaim_feature(Missions& m, Unit& unit, const std::array<std::int32_t, 3>& pos) {
    Construction& c = con(m);
    // C division by 2^20 (toward zero), unlike the shifts elsewhere.
    const i32 x = pos[0] / 0x100000, z = pos[2] / 0x100000;
    const MapCell* cell = c.map.cell(x, z);
    if (!cell) return false;
    const FeatureTypeInfo* f = nullptr;
    if (cell->feature < kFirstSpecialFeature) {
        if (cell->feature < c.features.size()) f = &c.features[cell->feature];
    } else if (cell->feature == kFeaturePart) {
        const MapCell* own = own_cell(c.map, x, z, cell->feature_aux, &m.fault);
        if (!own) return false;
        if (own->feature < kFirstSpecialFeature) {
            if (own->feature >= c.features.size()) {
                m.fault = "feature type outside the feature table";
                return false;
            }
            f = &c.features[own->feature];
        }
    }
    if (!f) return false;
    if ((cell->flags & 1) && (f->flags & 1)) return false;  // a 2D feature busy animating (burning, dying)
    UnitTickState& t = tick_of(m, unit);
    t.energy_make = with_income(m, unit, t.energy_make, static_cast<double>(f->energy));
    t.metal_make = with_income(m, unit, t.metal_make, static_cast<double>(f->metal));
    c.systems.kill_feature(x, z, true);
    if (c.env.network_provider == 3) {
        const u8 pk[6] = {0x0F, 0xFF, static_cast<u8>(x), static_cast<u8>(x >> 8), static_cast<u8>(z), static_cast<u8>(z >> 8)};
        c.systems.network_send(owner_of(m, unit).net_id, pk, 6);
    }
    return true;
}

void enqueue_be_carried(Missions& m, Unit& cargo) {
    if (!cargo.carrier) return;
    clear_orders(m.orders, cargo, false);
    MissionOrder* o = m.orders.systems.allocate_order();
    construct_order(m.orders, *o, mission::kBeCarried, cargo.carrier, nullptr, 0, 0, 0);
    prepend_order(cargo, *o);
}

void apply_transport_attach(Missions& m, TransportLinks& links, Unit& cargo, Unit* transport, std::uint8_t piece, std::uint8_t layer) {
    if (!(cargo.flags & kAlive) || (cargo.flags & kStructure) || cargo.cargo_head) return;
    if (transport && (!(transport->flags & kAlive) || transport == &cargo || transport->carrier)) return;
    if (Unit* prior = cargo.carrier) {
        Unit** link = &prior->cargo_head;
        while (*link && *link != &cargo) link = &(*link)->cargo_next;
        if (!*link) {
            m.fault = "carried unit missing from its carrier's cargo list";
            return;
        }
        *link = cargo.cargo_next;
    } else {
        links.bucket_unlink(cargo);
    }
    cargo.transport_piece = piece;
    if (!transport) {
        cargo.cargo_next = nullptr;
        cargo.flags &= ~0x20000u;
        cargo.carrier = nullptr;
        links.bucket_prepend(cargo);
    } else {
        cargo.carrier = transport;
        cargo.cargo_next = transport->cargo_head;
        transport->cargo_head = &cargo;
        cargo.flags = (cargo.flags & ~0x20000u) | (piece == 0xFF ? 0x20000u : 0u);
    }
    UnitTickState& t = tick_of(m, cargo);
    if (!t.has_motion) m.fault = "transport attach of a unit without a motion object";
    else t.motion.flags = static_cast<u8>(((layer ^ t.motion.flags) & 3u) ^ t.motion.flags);
    if (plays(owner_of(m, cargo)) && transport && !(links.type_flags(*transport) & kTypeAirBase)) enqueue_be_carried(m, cargo);
    // A selected unit that may no longer be selected loses the mark.
    if ((cargo.flags & kSelected) &&
        (!(cargo.flags & 0x20u) || !is_zero(cargo.build_fraction_left) || t.transfer_holdoff != 0 ||
         (cargo.carrier && !(cargo.carrier->flags & unit_tick_flags::kCarrierSelectable)))) {
        cargo.flags &= ~kSelected;
        links.selection_changed();
    }
}

void adjust_build_count(Missions& m, Unit& unit, std::uint8_t kind, std::int32_t type, std::int32_t delta) {
    const bool secondary = (mission::default_flags()[kind] & order_flags::kSecondary) != 0;
    if (delta < 1) {
        for (;;) {
            MissionOrder* last = nullptr;
            for (MissionOrder* p = secondary ? unit.secondary_orders : unit.orders; p; p = p->next)
                if (p->kind == kind && p->param0 == type) last = p;
            if (!last) return;
            if (-delta < last->param1) {
                last->param1 = wadd(last->param1, delta);
                return;
            }
            delta = wadd(delta, last->param1);
            MissionOrder* const head = unit.orders;
            MissionOrder** link = (last->flags & order_flags::kSecondary) ? &unit.secondary_orders : &unit.orders;
            for (MissionOrder* p = *link; p; p = p->next) {
                if (p == last) {
                    *link = last->next;
                    if (last != head) last->flags |= order_flags::kNotCurrent;
                    destroy_order(m.orders, last);
                    break;
                }
                link = &p->next;
            }
        }
    }
    MissionOrder* tail = secondary ? unit.secondary_orders : unit.orders;
    while (tail && tail->next) tail = tail->next;
    if (tail && tail->kind == kind && tail->param0 == type) {
        tail->param1 = wadd(tail->param1, delta);
        return;
    }
    if (!enqueue_order(m.orders, unit, kind, true, nullptr, nullptr, type, delta)) m.fault = "endless idle-order search";
}

} // namespace sim
