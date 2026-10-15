// The battle sections of a saved game: what the original's writer (GameSave_SaveAll 0x4326B0)
// writes and what its loader (GameSave_LoadAll 0x432610) restores (docs/RE_MEMORY.md,
// "Saved games").

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sim/ai.h"
#include "sim/footprint.h"
#include "sim/motion.h"
#include "ta/battle_features.h"
#include "ta/saved_game.h"
#include "ta/tdf.h"

namespace ta {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

template <class T> void put(std::vector<u8>& b, std::size_t at, T v) {
    if (b.size() < at + sizeof v) b.resize(at + sizeof v, 0);
    std::memcpy(b.data() + at, &v, sizeof v);
}
template <class T> T get(const std::vector<u8>& b, std::size_t at) {
    T v{};
    if (at + sizeof v <= b.size()) std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
u32 fbits(float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    return v;
}
float from_bits(u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
std::string hex4(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04x", v);
    return buf;
}
std::string order_blob_name(u16 id, u32 k) { return "u" + hex4(id) + "m" + hex4(k); }
std::string utype_key(u32 type) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "UTYPENAME%4d", static_cast<int>(type));
    return buf;
}
bool alive(const sim::Unit* u) { return u && (u->flags & sim::unit_flags::kAlive); }

// The order kinds whose parameter is a unit type (their type's name is kept by name).
bool names_unit_type(const std::string& mission) {
    return mission == "MobileBuild" || mission == "VTOL_MobileBuild" || mission == "BuildingBuild";  // case-sensitive
}

// Section names of the mission goals (one section per kind: goals of the same kind share it).
const char* goal_section(sim::GoalKind k) {
    using G = sim::GoalKind;
    switch (k) {
    case G::KillEnemyCommander: return "VictoryCondition_KillEnemyCommander";
    case G::DestroyAllUnits: return "VictoryCondition_DestroyAllUnits";
    case G::KillAllMobileUnits: return "VictoryCondition_KillAllMobileUnits";
    case G::BuildUnitType: return "VictoryCondition_BuildUnitType";
    case G::CaptureUnitType: return "VictoryCondition_CaptureUnitType";
    case G::KillAllOfType: return "VictoryCondition_KillAllOfType";
    case G::KillUnitType: return "VictoryCondition_KillUnitType";
    case G::MoveUnitToRadius: return "VictoryCondition_MoveUnitToRadius";
    case G::UnitTypeNearX: return "VictoryCondition_UnitTypePassesX";
    case G::UnitTypeNearZ: return "VictoryCondition_UnitTypePassesZ";
    case G::VictoryTimer: return "VictoryCondition_VictoryTimerRunsOut";
    case G::CommanderKilled: return "DefeatCondition_CommanderKilled";
    case G::AllUnitsKilled: return "DefeatCondition_AllUnitsKilled";
    case G::AllUnitsKilledOfType: return "DefeatCondition_AllUnitsKilledOfType";
    case G::UnitTypeKilled: return "DefeatCondition_UnitTypeKilled";
    case G::DeathTimer: return "DefeatCondition_DeathTimerRunsOut";
    case G::AnyUnitNearX: return "DefeatCondition_AnyUnitPassesX";
    case G::AnyUnitNearZ: return "DefeatCondition_AnyUnitPassesZ";
    }
    return "";
}
// The count key of a goal kind ("" none).
const char* goal_count_key(sim::GoalKind k) {
    if (k == sim::GoalKind::KillAllMobileUnits) return "NumUnits";
    if (k == sim::GoalKind::KillUnitType || k == sim::GoalKind::UnitTypeKilled) return "NumLeftToKill";
    return "";
}

void note(std::vector<std::string>* notes, const std::string& s) {
    if (notes) notes->push_back(s);
}

// ---------------------------------------------------------------------------------------
// Writer.

// A unit's script (0x4B1EC0) into the selected blob: the 0x528-byte header (the file's
// checksum, the eight threads with their sink word zeroed, the live-thread count), the
// statics, then per piece 27 words: its animation words 1-18, the piece's move and turn per
// axis (turns zero-extended) and its visible, cached and shaded bits.
void write_script(sim::Unit& u, std::vector<u8>& out) {
    sim::ScriptInstance& s = *u.script;
    out.clear();
    put<u32>(out, 0, s.program().checksum);
    for (int t = 0; t < sim::ScriptInstance::kThreads; ++t)
        for (int w = 0; w < sim::ScriptInstance::kThreadWords; ++w)
            put<u32>(out, 4 + static_cast<std::size_t>(t * sim::ScriptInstance::kThreadWords + w) * 4,
                     w == sim::ScriptInstance::kSink ? 0u : s.thread_word(t, w));
    put<u32>(out, 0x524, s.count_word());
    for (std::int32_t v : s.statics()) put<i32>(out, out.size(), v);
    const std::size_t pieces = s.program().piece_names.size();
    const std::vector<i32>& anims = s.piece_anims();
    for (std::size_t p = 0; p < pieces; ++p) {
        const std::size_t at = out.size();
        for (int k = 0; k < 18; ++k) {
            const std::size_t a = p * sim::ScriptInstance::kPieceWords + 1 + static_cast<std::size_t>(k);
            put<i32>(out, at + static_cast<std::size_t>(k) * 4, a < anims.size() ? anims[a] : 0);
        }
        const i32 pi = static_cast<i32>(p);
        for (int k = 0; k < 3; ++k) {
            put<i32>(out, at + (18 + static_cast<std::size_t>(k)) * 4, u.pose.get_move(pi, k));
            put<u32>(out, at + (21 + static_cast<std::size_t>(k)) * 4, u.pose.get_turn(pi, k));
        }
        const u16 f = p < u.pose.pieces.size() ? u.pose.pieces[p].flags : 0;
        put<u32>(out, at + 24 * 4, f & 1u);
        put<u32>(out, at + 25 * 4, (f >> 1) & 1u);
        put<u32>(out, at + 26 * 4, (f >> 2) & 1u);
    }
}

// An order goal's record (the goal kinds' save methods). `stack`: the record's first words,
// which the original leaves from its stack.
void write_goal(const sim::OrderGoal& g, const std::array<u8, 8>& stack, std::vector<u8>& out) {
    out.clear();
    auto id_of = [](const sim::Unit* u) -> u16 { return u ? u->id : u16{0}; };
    switch (g.kind()) {
    case 2: {
        const auto& p = static_cast<const sim::PointGoal&>(g);
        out.assign(stack.begin(), stack.end());
        put<u16>(out, 8, id_of(p.owner));
        // A unit link the save routine builds on its stack (no unit).
        put<u32>(out, 0x0A, 0x004FD754u);
        put<u32>(out, 0x0E, 0);
        put<u32>(out, 0x12, 0);
        put<u32>(out, 0x16, 0);
        put<u16>(out, 0x1A, id_of(p.link.unit));
        put<u16>(out, 0x1C, p.flags);
        put<i16>(out, 0x1E, p.radius);
        put<i16>(out, 0x20, p.altitude);
        put<i16>(out, 0x22, p.goal_heading);
        put<i16>(out, 0x24, p.piece);
        for (int k = 0; k < 3; ++k) put<i32>(out, 0x26 + static_cast<std::size_t>(k) * 4, p.pos[static_cast<std::size_t>(k)]);
        put<i32>(out, 0x32, p.offset);
        break;
    }
    case 3: {
        const auto& m = static_cast<const sim::ManoeuvreGoal&>(g);
        out.assign(stack.begin(), stack.end());
        put<u16>(out, 8, id_of(m.owner));
        put<u16>(out, 0x0A, m.flags);
        for (int k = 0; k < 3; ++k) put<i32>(out, 0x0C + static_cast<std::size_t>(k) * 4, m.pos[static_cast<std::size_t>(k)]);
        for (int k = 0; k < 3; ++k) put<i32>(out, 0x18 + static_cast<std::size_t>(k) * 4, m.vel[static_cast<std::size_t>(k)]);
        put<i16>(out, 0x24, m.unused22);
        put<u16>(out, 0x26, m.target_heading);
        put<i16>(out, 0x28, m.unused26);
        break;
    }
    case 4: {
        const auto& c = static_cast<const sim::CircleGoal&>(g);
        out.assign(stack.begin(), stack.begin() + 4);
        put<i16>(out, 4, c.cell_x);
        put<i16>(out, 6, c.cell_z);
        put<i32>(out, 8, c.radius);
        put<i32>(out, 0x0C, c.radius_cells_sq);
        break;
    }
    case 5: {
        const auto& r = static_cast<const sim::RingGoal&>(g);
        out.assign(stack.begin(), stack.begin() + 4);
        put<i16>(out, 4, r.cell_x);
        put<i16>(out, 6, r.cell_z);
        put<i32>(out, 8, r.inner);
        put<i32>(out, 0x0C, r.outer);
        put<i32>(out, 0x10, r.inner_cells_sq);
        put<i32>(out, 0x14, r.outer_cells_sq);
        break;
    }
    case 6: {
        const auto& e = static_cast<const sim::EdgeGoal&>(g);
        out.assign(stack.begin(), stack.begin() + 4);
        put<i32>(out, 4, e.min_x);
        put<i32>(out, 8, e.max_x);
        put<i32>(out, 0x0C, e.min_z);
        put<i32>(out, 0x10, e.max_z);
        break;
    }
    default: break;
    }
}

// One order (0x43A970) into blob `name` of the Units section: the 0x3A-byte record, the
// "<name>_name" key with the order kind's lookup name, for the build kinds the type's name
// as "UTYPENAME%4d" (once per type, while the section has no such key), and its goal.
bool write_order(const LoadedBattle& lb, const sim::MissionOrder& o, const sim::Unit& unit, BankSection& s, const std::string& name,
                 const SaveContext& ctx) {
    if (!o.unit || o.unit->id != unit.id || name.size() > 0x1F) return false;
    std::vector<u8> r(0x3A, 0);
    put<u16>(r, 0, o.unit ? o.unit->id : u16{0});
    put<u16>(r, 2, alive(o.target.unit) ? o.target.unit->id : u16{0});
    put<i32>(r, 4, o.attached && o.goal ? o.goal->kind() : 0);
    r[8] = o.kind;
    r[9] = o.step;
    put<u32>(r, 0x0A, o.wait);
    put<u32>(r, 0x0E, o.wake_tick);
    for (int k = 0; k < 3; ++k) put<i32>(r, 0x12 + static_cast<std::size_t>(k) * 4, o.pos[static_cast<std::size_t>(k)]);
    put<u32>(r, 0x1E, o.scratch0);
    put<u32>(r, 0x22, o.scratch1);
    put<i32>(r, 0x26, o.param0);
    put<i32>(r, 0x2A, o.param1);
    put<i32>(r, 0x2E, o.param2);
    put<u32>(r, 0x32, o.flags);
    put<u32>(r, 0x36, o.events);
    s.blob(name).data = r;
    const std::vector<std::string>& kinds = lb.unit_context.mission_names;
    const std::string mission = o.kind < kinds.size() ? kinds[o.kind] : std::string();
    s.set_string(name + "_name", mission);
    if (names_unit_type(mission)) {
        const u32 t = static_cast<u16>(o.param0);
        const std::string key = utype_key(t);
        if (!s.has(key) && t != 0 && t < lb.type_defs.size() && lb.type_defs[t]) s.set_string(key, lb.type_defs[t]->unit_name);
    }
    if (o.attached && o.goal) {
        std::vector<u8> g;
        write_goal(*o.goal, ctx.goal_record_stack, g);
        s.blob(name + "g").data = g;
    }
    return true;
}

}  // namespace

void write_units_section(const LoadedBattle& lb, BankSection& s, const SaveContext& ctx, std::vector<std::string>* notes) {
    sim::Battle& b = *lb.battle;
    // The record buffer is one stack frame for the whole loop: what one unit does not write
    // stays from the previous unit (or the stack before the first).
    std::vector<u8> rec(ctx.unit_record_stack.begin(), ctx.unit_record_stack.end());
    i32 saved = 0;
    for (std::size_t id = 0; id < b.units.size(); ++id) {
        sim::Unit& u = b.units[id];
        if (!(u.flags & sim::unit_flags::kAlive)) continue;
        const sim::UnitTickState& t = b.ticks[id];
        // Script.
        BankSection::Blob& sb = s.blob("Script" + std::to_string(saved));
        if (u.script) {
            write_script(u, sb.data);
        } else {
            note(notes, "unit " + std::to_string(id) + " has no script: the original's writer crashes");
        }
        // Orders: the primary list then the secondary list, numbered on.
        u32 count = 0;
        for (const sim::MissionOrder* o = u.orders; o; o = o->next) write_order(lb, *o, u, s, order_blob_name(u.id, count++), ctx);
        for (const sim::MissionOrder* o = u.secondary_orders; o; o = o->next) write_order(lb, *o, u, s, order_blob_name(u.id, count++), ctx);
        // Motion (0x43DD70): velocity, tilt, speed, turn, the claim step, the layer and blocked bits.
        if (t.has_motion) {
            std::vector<u8>& m = s.blob("u" + hex4(u.id) + "mob").data;
            const sim::MotionState& ms = t.motion;
            const u8 stale_flags = m.size() > 0x22 ? m[0x22] : 0;  // never written: a fresh blob
            m.assign(0x23, 0);
            for (int k = 0; k < 3; ++k) put<i32>(m, static_cast<std::size_t>(k) * 4, t.velocity[static_cast<std::size_t>(k)]);
            for (int k = 0; k < 3; ++k) put<i32>(m, 0x0C + static_cast<std::size_t>(k) * 4, ms.tilt[static_cast<std::size_t>(k)]);
            put<i32>(m, 0x18, ms.speed);
            put<i16>(m, 0x1C, ms.turn);
            put<u32>(m, 0x1E, ms.claim_tick);
            m[0x22] = static_cast<u8>((stale_flags & 0xF8) | (ms.flags & 7));
        }
        // Resource slot (0x4010B0): its 12 figures.
        {
            std::vector<u8>& a = s.blob("u" + hex4(u.id) + "acc").data;
            const float f[12] = {t.energy_make, t.energy_use, t.energy_use_paid, t.energy_stall, t.energy_make_prev, t.energy_use_prev,
                                 t.metal_make,  t.metal_use,  t.metal_use_paid,  t.metal_stall,  t.metal_make_prev,  t.metal_use_prev};
            a.assign(0x30, 0);
            std::memcpy(a.data(), f, 0x30);
        }
        // The record.
        const std::string& tn = t.type_index < lb.type_defs.size() && lb.type_defs[t.type_index] ? lb.type_defs[t.type_index]->unit_name : std::string();
        const std::size_t nlen = std::min<std::size_t>(tn.size(), 31);
        std::memcpy(rec.data(), tn.data(), nlen);
        rec[nlen] = 0;  // the name and its terminator; the rest of the 32 bytes stays
        rec[0x20] = static_cast<u8>(t.owner);
        put<u16>(rec, 0x21, u.id);
        put<i32>(rec, 0x23, static_cast<i32>(count));
        put<u32>(rec, 0x27, t.has_motion ? 1u : 0u);
        for (int k = 0; k < 3; ++k) put<i32>(rec, 0x2B + static_cast<std::size_t>(k) * 4, u.pos[static_cast<std::size_t>(k)]);
        put<i16>(rec, 0x37, u.pitch);
        put<u16>(rec, 0x39, u.heading);
        put<i16>(rec, 0x3B, u.bank);
        put<i16>(rec, 0x3D, u.hit_points);
        put<u16>(rec, 0x3F, t.kill_count);
        for (int w = 0; w < 3; ++w) {
            const sim::WeaponSlot& sl = u.weapons[static_cast<std::size_t>(w)];
            const std::size_t e = 0x41 + static_cast<std::size_t>(w) * 0x18;
            put<i16>(rec, e, sl.target_x);
            put<i16>(rec, e + 2, sl.target_z);
            put<i32>(rec, e + 4, sl.aim.result);
            if (!sl.type) note(notes, "unit " + std::to_string(id) + " weapon slot without a weapon: the original's writer crashes");
            put<u32>(rec, e + 8, sl.type ? sl.type->index : 0u);
            put<i32>(rec, e + 0x0C, sl.aux);
            put<u16>(rec, e + 0x10, sl.reload);
            put<u16>(rec, e + 0x12, sl.aim_heading);
            put<u16>(rec, e + 0x14, sl.aim_pitch);
            rec[e + 0x16] = sl.stockpile;
            rec[e + 0x17] = static_cast<u8>((rec[e + 0x17] & 0xE0) | (sl.flags & 0x1F));
        }
        const bool carried = alive(u.carrier);
        put<u16>(rec, 0x89, carried ? u.carrier->id : u16{0});
        put<u16>(rec, 0x8B, alive(t.last_attacker) ? t.last_attacker->id : u16{0});
        rec[0x8D] = carried ? u.transport_piece : u8{0xFF};
        rec[0x8E] = t.last_attacker_slot;
        put<float>(rec, 0x8F, u.metal_extraction);
        put<i16>(rec, 0x93, u.cell_x);
        put<i16>(rec, 0x95, u.cell_z);
        put<i16>(rec, 0x97, u.los_cell_x);
        put<i16>(rec, 0x99, u.los_cell_z);
        put<i16>(rec, 0x9B, u.footprint_x);
        put<i16>(rec, 0x9D, u.footprint_z);
        put<u32>(rec, 0x9F, u.group);
        put<u32>(rec, 0xA3, u.cloak_holdoff_tick);
        put<float>(rec, 0xA7, u.build_fraction_left);
        rec[0xAB] = t.last_damage_kind;
        rec[0xAC] = t.health_percent;
        rec[0xAD] = t.prev_health_percent;
        put<u16>(rec, 0xAE, u.net_dirty);
        rec[0xB0] = u.los_frame;
        rec[0xB1] = t.damaged_timer;
        put<u16>(rec, 0xB2, u.activate_flags);
        // Flags packed: unit flags 0-11 to bits 4-15, 13 to 16, 14-25 to 20-31; the script
        // flags' low four bits in 0-3; bits 17-19 are never written.
        const u32 f = u.flags;
        const u32 a = ((f & 0xFFFFC000u) << 2) | (f & 0xFFFu);
        const u32 bb = (a << 1) | (f & 0x2000u);
        const u32 w = (bb << 3) | (u.script_flags & 0xFu) | (get<u32>(rec, 0xB4) & 0xE0000u);
        put<u32>(rec, 0xB4, w);
        s.numbered(saved).data = rec;
        ++saved;
    }
    if (saved > 0) {
        s.set_int("Number of Units", saved);
        s.set_int("Version", 0x11);
    }
}

namespace {

// Features (0x424890): the type names (0x80 bytes each), then every feature's own cell in
// row order as a 3D feature (its slot's damage, position and attitude), a 2D one (its cell
// word), or an animating 2D one (its slot's cursor frame byte, damage word and fire byte's
// high half, and which animation it runs: burning, dying or reclaimed; others are dropped).
void write_features(const LoadedBattle& lb, Bank& bank) {
    sim::Battle& b = *lb.battle;
    BankSection& s = bank.section("Features");
    const auto& recs = lb.feature_defs.records();
    const std::size_t types = b.features.size();
    std::vector<u8> names(types * 0x80, 0);
    for (std::size_t k = 0; k < types && k < recs.size(); ++k) {
        const std::string& n = recs[k].name;
        std::memcpy(names.data() + k * 0x80, n.data(), std::min<std::size_t>(n.size(), 0x80));
    }
    s.blob("Feature Type Names").data = names;
    i32 normal = 0, three_d = 0, animating = 0;
    const i32 W = b.map.width;
    for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
        const sim::MapCell& c = b.map.cells[i];
        if (c.feature >= 0xFFFB) continue;
        const u16 x = static_cast<u16>(static_cast<i32>(i) % W), z = static_cast<u16>(static_cast<i32>(i) / W);
        const sim::FeatureTypeInfo* t = c.feature < types ? &b.features[c.feature] : nullptr;
        const u16 tflags = t ? t->flags : 0;
        auto slot = [&](u16 k) -> const sim::FeatureSlot& {
            static const sim::FeatureSlot none{};
            return k < b.feature_state.slots.size() ? b.feature_state.slots[k] : none;
        };
        if (!(tflags & sim::feature_type_flags::kFlat)) {
            const sim::FeatureSlot& sl = slot(c.feature_aux);
            std::vector<u8> r(0x1A, 0);
            put<u16>(r, 0, x);
            put<u16>(r, 2, z);
            put<u16>(r, 4, c.feature);
            put<u16>(r, 6, sl.get<u16>(0x26));
            std::memcpy(r.data() + 8, sl.b.data() + 8, 12);
            std::memcpy(r.data() + 0x14, sl.b.data() + 0x20, 6);
            auto& d = s.blob("3D Features").data;
            d.insert(d.end(), r.begin(), r.end());
            ++three_d;
        } else if (!(c.flags & 1)) {
            std::vector<u8> r(8, 0);
            put<u16>(r, 0, x);
            put<u16>(r, 2, z);
            put<u16>(r, 4, c.feature);
            put<u16>(r, 6, c.feature_aux);
            auto& d = s.blob("Normal Features").data;
            d.insert(d.end(), r.begin(), r.end());
            ++normal;
        } else {
            const sim::FeatureSlot& sl = slot(c.feature_aux);
            const u32 seq = sl.get<u32>(0x0C);
            u8 state;
            if (seq == t->sequences[sim::kSeqBurn]) state = 0;
            else if (seq == t->sequences[sim::kSeqDie]) state = 1;
            else if (seq == t->sequences[sim::kSeqReclaim]) state = 2;
            else continue;
            std::vector<u8> r(10, 0);
            put<u16>(r, 0, x);
            put<u16>(r, 2, z);
            put<u16>(r, 4, c.feature);
            put<u16>(r, 6, sl.get<u16>(0x26));
            r[8] = sl.b[4];
            r[9] = static_cast<u8>((sl.b[0x2E] & 0xF0) | state);
            auto& d = s.blob("Animating Features").data;
            d.insert(d.end(), r.begin(), r.end());
            ++animating;
        }
    }
    s.set_int("Number of Normal Features", normal);
    s.set_int("Number of 3D Features", three_d);
    s.set_int("Number of Animating Features", animating);
}

}  // namespace

void write_battle_sections(const LoadedBattle& lb, Bank& bank, const SaveContext& ctx, std::vector<std::string>* notes) {
    sim::Battle& b = *lb.battle;
    // The radar image (a blob of the Summary): width, height, then the rows.
    {
        std::vector<u8>& d = bank.section("Summary").blob("Radar Image").data;
        d.clear();
        put<i32>(d, 0, ctx.radar_width);
        put<i32>(d, 4, ctx.radar_height);
        if (ctx.radar_height > 0 && ctx.radar_width > 0)
            d.insert(d.end(), ctx.radar_pixels.begin(),
                     ctx.radar_pixels.begin() + std::min<std::size_t>(ctx.radar_pixels.size(),
                                                                       static_cast<std::size_t>(ctx.radar_width) * static_cast<std::size_t>(ctx.radar_height)));
    }
    {
        BankSection& c = bank.section("Camera");
        c.set_int("X Position", ctx.camera_x);
        c.set_int("Z Position", ctx.camera_z);
    }
    // Players (0x4662F0).
    {
        BankSection& p = bank.section("Players");
        p.set_int("Human Player", b.settings.local_player);
        std::vector<u8> tb(ctx.time_block.begin(), ctx.time_block.end());
        put<u32>(tb, 0x10, b.game_tick());
        p.blob("GameTime").data = tb;
        for (int k = 0; k < 10; ++k) {
            const sim::PlayerSlot& ps = b.players[static_cast<std::size_t>(k)];
            if (ps.controller == 0) continue;
            const sim::PlayerEconomy& e = b.player_economy[static_cast<std::size_t>(k)];
            BankSection& s = bank.section("Player " + std::to_string(k));
            s.set_double("Energy", static_cast<double>(ps.energy_stored));
            s.set_double("Metal", static_cast<double>(ps.metal_stored));
            s.set_double("TotalEnergyProduced", e.energy_produced);
            s.set_double("TotalMetalProduced", e.metal_produced);
            s.set_double("TotalEnergyConsumed", e.energy_consumed);
            s.set_double("TotalMetalConsumed", e.metal_consumed);
            s.set_double("EnergyWasted", e.energy_wasted);
            s.set_double("MetalWasted", e.metal_wasted);
            s.set_double("PlayerEnergyStorage", static_cast<double>(e.energy_storage_bonus));
            s.set_double("PlayerMetalStorage", static_cast<double>(e.metal_storage_bonus));
            s.set_int("AddPlayerStorage", ps.flags & 1);
            s.set_int("Kills", static_cast<i16>(ps.units_killed));
            s.set_int("Losses", static_cast<i16>(ps.units_lost));
            s.set_int("UpdateTime", static_cast<i32>(e.next_update_tick));
            s.set_int("WinLoseTime", static_cast<i32>(e.win_lose_tick));
            s.set_int("DisplayTimer", static_cast<i32>(e.display_tick));
            s.set_int("Controller", ps.controller);
            s.set_int("Logo", ctx.player_logo[static_cast<std::size_t>(k)]);
            s.set_int("Side", ctx.player_side[static_cast<std::size_t>(k)]);
            std::vector<u8> al(11, 0);
            for (std::size_t o = 0; o < 11; ++o) al[o] = b.alliances[static_cast<std::size_t>(k)][o];
            s.blob("Alliances").data = al;
        }
    }
    write_units_section(lb, bank.section("Units"), ctx, notes);
    // Mapping (0x484F50): the explored map.
    {
        const std::size_t n = static_cast<std::size_t>((static_cast<u32>(b.map.width * b.map.height) & 0x7FFFFFFFu) >> 1);
        std::vector<u8> d(n, 0);
        std::memcpy(d.data(), b.explored.data(), std::min(n, b.explored.size() * 2));
        bank.section("Mapping").blob("Data").data = d;
    }
    write_features(lb, bank);
    // PlayerFeatures (0x484DF0): two cells' owner nibbles per byte (the first one high).
    {
        const std::size_t n = static_cast<std::size_t>(b.map.width * b.map.height / 2);
        std::vector<u8> d(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const u8 c0 = b.map.cells[2 * i].flags, c1 = b.map.cells[2 * i + 1].flags;
            d[i] = static_cast<u8>(((c1 >> 3) & 0xF) | ((c0 & 0xF8) << 1));
        }
        bank.section("PlayerFeatures").blob("Plotmap").data = d;
    }
    // Metal (0x484CE0): the metal map.
    {
        std::vector<u8> d(b.map.cells.size());
        for (std::size_t i = 0; i < d.size(); ++i) d[i] = b.map.cells[i].metal;
        bank.section("Metal").blob("Plotmap").data = d;
    }
    // Meteor (0x438180).
    {
        BankSection& m = bank.section("Meteor");
        const sim::MeteorShower& ms = b.meteors;
        m.set_int("Enabled", ms.enabled ? 1 : 0);
        m.set_int("Active", ms.active ? 1 : 0);
        m.set_int("Next Strike Time", static_cast<i32>(ms.next_schedule));
        m.set_int("Time Strike Ends", static_cast<i32>(ms.end_tick));
        m.set_int("Next Hit Time", static_cast<i32>(ms.next_spawn));
        m.set_int("Origin X", ms.aim_x);
        m.set_int("Origin Z", ms.aim_z);
        m.set_int("Target X", ms.origin_x);
        m.set_int("Target Z", ms.origin_z);
    }
    // A mission's goals (0x48FDF0; campaigns only): win goals then loss goals, each into the
    // section of its kind.
    if (ctx.game_type == 1) {
        auto put_goal = [&](const sim::MissionGoal& g) {
            BankSection& s = bank.section(goal_section(g.kind));
            const char* ck = goal_count_key(g.kind);
            if (*ck) s.set_int(ck, g.count);
            s.set_int("Satisfied", g.satisfied ? 1 : 0);
            s.set_int("Celebrated", g.celebrated ? 1 : 0);
        };
        for (const sim::MissionGoal& g : b.mission_goals.win) put_goal(g);
        for (const sim::MissionGoal& g : b.mission_goals.loss) put_goal(g);
    }
}

// ---------------------------------------------------------------------------------------
// Loader.

namespace {

struct UnitLoader {
    LoadedBattle& lb;
    sim::Battle& b;
    const BankSection& s;
    LoadedView& view;
    i32 count = 0;

    void fault(const std::string& what) { view.notes.push_back(what); }

    // The record with id `id` (the first numbered blob whose id matches), scanning from the
    // first; a missing or short blob ends the search.
    const std::vector<u8>* find_record(u16 id, i32& index) {
        for (i32 i = 0; i < count; ++i) {
            const BankSection::Blob* r = s.find_numbered(i);
            if (!r || r->data.empty()) return nullptr;
            if (r->data.size() < 0xB8) return nullptr;
            if (get<u16>(r->data, 0x21) == id) {
                index = i;
                return &r->data;
            }
        }
        return nullptr;
    }

    // 0x487080: the unit with this id, created from its record when it is not alive yet.
    sim::Unit* load(u16 id) {
        if (id == 0) return nullptr;
        if (id >= b.units.size()) {
            fault("a saved unit id " + std::to_string(id) + " beyond the unit table: the original reads past it");
            return nullptr;
        }
        sim::Unit& existing = b.units[id];
        if (existing.flags & sim::unit_flags::kAlive) return &existing;
        i32 index = 0;
        const std::vector<u8>* rp = find_record(id, index);
        if (!rp) return nullptr;
        const std::vector<u8> r = *rp;  // (blobs do not move, but keep a copy)
        const u32 w = get<u32>(r, 0xB4);
        std::string name(reinterpret_cast<const char*>(r.data()), 32);
        name.resize(std::strlen(name.c_str()));
        const u16 type = lb.type_index(name);
        sim::Unit* u = b.create_unit(r[0x20], type, get<i32>(r, 0x2B), get<i32>(r, 0x2F), get<i32>(r, 0x33), true, (w >> 4) & 3, id);
        if (!u) return nullptr;
        sim::UnitTickState& t = b.ticks[u->id];
        u->pitch = get<i16>(r, 0x37);
        u->heading = get<u16>(r, 0x39);
        u->bank = get<i16>(r, 0x3B);
        u->hit_points = get<i16>(r, 0x3D);
        t.kill_count = get<u16>(r, 0x3F);
        u->pos[1] = get<i32>(r, 0x2F);
        // The carrier first (loaded if needed), then the last attacker.
        const u16 carrier = get<u16>(r, 0x89);
        if (carrier != 0)
            if (sim::Unit* c = load(carrier)) b.transport_attach(*u, c, r[0x8D], static_cast<u8>((w >> 4) & 3));
        t.last_attacker = load(get<u16>(r, 0x8B));
        u->transport_piece = r[0x8D];
        t.last_attacker_slot = r[0x8E];
        u->metal_extraction = get<float>(r, 0x8F);
        u->cell_x = get<i16>(r, 0x93);
        u->cell_z = get<i16>(r, 0x95);
        u->los_cell_x = get<i16>(r, 0x97);
        u->los_cell_z = get<i16>(r, 0x99);
        u->footprint_x = get<i16>(r, 0x9B);
        u->footprint_z = get<i16>(r, 0x9D);
        // The group is stored first, then the group change runs (it leaves the group the unit
        // now names, where it is not, and joins it again: a unit of another group stays in
        // the group list creation put it in).
        const i32 group = get<i32>(r, 0x9F);
        u->group = static_cast<u32>(group);
        sim::set_unit_group(b.ai(), *u, group);
        u->build_fraction_left = get<float>(r, 0xA7);
        t.last_damage_kind = r[0xAB];
        t.health_percent = r[0xAC];
        t.prev_health_percent = r[0xAD];
        u->net_dirty = get<u16>(r, 0xAE);
        u->los_frame = r[0xB0];
        t.damaged_timer = r[0xB1];
        u->activate_flags = r[0xB2];
        u->script_flags = static_cast<u8>((u->script_flags & 0xF0) | (w & 0xF));
        u->flags = (u->flags & ~(0xFFCu | 0x2000u | 0x3FFC000u)) | ((w >> 4) & 0xFFCu) | ((w >> 3) & 0x2000u) | ((w >> 6) & 0x3FFC000u);
        u->cloak_holdoff_tick = get<u32>(r, 0xA3);
        // Resource slot.
        if (const BankSection::Blob* a = s.find_blob("u" + hex4(u->id) + "acc"); a && !a->data.empty()) {
            float f[12] = {t.energy_make, t.energy_use, t.energy_use_paid, t.energy_stall, t.energy_make_prev, t.energy_use_prev,
                           t.metal_make,  t.metal_use,  t.metal_use_paid,  t.metal_stall,  t.metal_make_prev,  t.metal_use_prev};
            std::memcpy(f, a->data.data(), std::min<std::size_t>(a->data.size(), 0x30));
            t.energy_make = f[0], t.energy_use = f[1], t.energy_use_paid = f[2], t.energy_stall = f[3], t.energy_make_prev = f[4],
            t.energy_use_prev = f[5], t.metal_make = f[6], t.metal_use = f[7], t.metal_use_paid = f[8], t.metal_stall = f[9],
            t.metal_make_prev = f[10], t.metal_use_prev = f[11];
        }
        // Motion (0x43DE30), when the record says there was a motion object.
        if (get<u32>(r, 0x27) != 0) {
            if (!t.has_motion) {
                fault("unit " + std::to_string(u->id) + ": a saved motion object for a unit without one (the original writes through a null pointer)");
            } else {
                std::vector<u8> m(0x23, 0);
                if (const BankSection::Blob* mb = s.find_blob("u" + hex4(u->id) + "mob"))
                    std::memcpy(m.data(), mb->data.data(), std::min<std::size_t>(mb->data.size(), 0x23));
                else
                    fault("unit " + std::to_string(u->id) + ": no motion blob (the original reads stack contents)");
                for (int k = 0; k < 3; ++k) t.velocity[static_cast<std::size_t>(k)] = get<i32>(m, static_cast<std::size_t>(k) * 4);
                for (int k = 0; k < 3; ++k) t.motion.tilt[static_cast<std::size_t>(k)] = get<i32>(m, 0x0C + static_cast<std::size_t>(k) * 4);
                t.motion.speed = get<i32>(m, 0x18);
                t.motion.turn = get<i16>(m, 0x1C);
                t.motion.claim_tick = get<u32>(m, 0x1E);
                t.motion.flags = static_cast<u8>((t.motion.flags & 0xF8) | (m[0x22] & 7));
            }
        }
        // Orders, in record order; each goes to the list its secondary flag names.
        const i32 orders = get<i32>(r, 0x23);
        sim::MissionOrder** primary_tail = &u->orders;
        sim::MissionOrder** secondary_tail = &u->secondary_orders;
        while (*primary_tail) primary_tail = &(*primary_tail)->next;
        while (*secondary_tail) secondary_tail = &(*secondary_tail)->next;
        for (i32 k = 0; k < orders; ++k) {
            sim::MissionOrder* o = load_order(*u, order_blob_name(u->id, static_cast<u32>(k)));
            if (!o) continue;
            if (!(o->flags & sim::order_flags::kSecondary)) {
                *primary_tail = o;
                primary_tail = &o->next;
            } else {
                *secondary_tail = o;
                secondary_tail = &o->next;
            }
        }
        // The first primary order's goal goes to the motion goal (0x4388B0).
        if (u->orders && u->orders->attached) {
            if (!t.has_motion || !t.motion.goal) fault("unit " + std::to_string(u->id) + ": an order goal without a motion object (the original crashes)");
            else t.motion.goal->attach(b.motion_context(), u->orders->goal.get());
        }
        // Script (0x4B2040).
        if (const BankSection::Blob* sc = s.find_blob("Script" + std::to_string(index))) load_script(*u, sc->data);
        // Weapon slots: the weapon's index byte goes into the weapon type the slot has now.
        for (int sl = 0; sl < 3; ++sl) {
            sim::WeaponSlot& ws = u->weapons[static_cast<std::size_t>(sl)];
            const std::size_t e = 0x41 + static_cast<std::size_t>(sl) * 0x18;
            ws.target_x = get<i16>(r, e);
            ws.target_z = get<i16>(r, e + 2);
            ws.aim.result = get<i32>(r, e + 4);
            if (ws.type) const_cast<sim::WeaponType*>(ws.type)->index = r[e + 8];
            else fault("unit " + std::to_string(u->id) + ": weapon slot without a weapon (the original writes through a null pointer)");
            ws.aux = get<i32>(r, e + 0x0C);
            ws.reload = get<u16>(r, e + 0x10);
            ws.aim_heading = get<u16>(r, e + 0x12);
            ws.aim_pitch = get<u16>(r, e + 0x14);
            ws.stockpile = r[e + 0x16];
            ws.flags = static_cast<u8>((ws.flags & 0xE0) | (r[e + 0x17] & 0x1F));
        }
        if (u->script_flags & sim::unit_flags::kYardOpen) sim::force_reclaim_around(b.footprint_world(), *u);
        return u;
    }

    // An order goal from blob "<order>g" (the goal kinds' loading constructors).
    std::shared_ptr<sim::OrderGoal> load_goal(sim::MissionOrder& o, i32 kind, const std::string& name) {
        const BankSection::Blob* gb = s.find_blob(name);
        const std::vector<u8> none;
        const std::vector<u8>& g = gb ? gb->data : none;
        sim::MotionContext& mc = b.motion_context();
        auto common = [&](sim::OrderGoal& x, u32 size) {
            x.order = &o;
            x.owner = o.unit;
            x.address = mc.systems.allocate_object(size);
        };
        switch (kind) {
        case 2: {
            auto p = std::make_shared<sim::PointGoal>();
            common(*p, 0x36);
            p->link.goal = p.get();
            if (g.size() >= 0x36) {
                p->owner = load(get<u16>(g, 8));
                sim::relink_unit(b.order_context(), p->link, load(get<u16>(g, 0x1A)));
                p->flags = get<u16>(g, 0x1C);
                p->radius = get<i16>(g, 0x1E);
                p->altitude = get<i16>(g, 0x20);
                p->goal_heading = get<i16>(g, 0x22);
                p->piece = get<i16>(g, 0x24);
                for (int k = 0; k < 3; ++k) p->pos[static_cast<std::size_t>(k)] = get<i32>(g, 0x26 + static_cast<std::size_t>(k) * 4);
                p->offset = get<i32>(g, 0x32);
            } else {
                fault(name + ": a short point goal (the original keeps uninitialised fields)");
            }
            return p;
        }
        case 3: {
            auto m = std::make_shared<sim::ManoeuvreGoal>();
            common(*m, 0x2C);
            if (g.size() >= 0x2A) {
                m->owner = load(get<u16>(g, 8));
                m->flags = get<u16>(g, 0x0A);
                for (int k = 0; k < 3; ++k) m->pos[static_cast<std::size_t>(k)] = get<i32>(g, 0x0C + static_cast<std::size_t>(k) * 4);
                for (int k = 0; k < 3; ++k) m->vel[static_cast<std::size_t>(k)] = get<i32>(g, 0x18 + static_cast<std::size_t>(k) * 4);
                m->unused22 = get<i16>(g, 0x24);
                m->target_heading = get<u16>(g, 0x26);
                m->unused26 = get<i16>(g, 0x28);
            } else {
                fault(name + ": a short manoeuvre goal (the original keeps uninitialised fields)");
            }
            return m;
        }
        case 4: {
            auto c = std::make_shared<sim::CircleGoal>();
            common(*c, 0x14);
            if (g.size() >= 0x10) {
                c->cell_x = get<i16>(g, 4);
                c->cell_z = get<i16>(g, 6);
                c->radius = get<i32>(g, 8);
                c->radius_cells_sq = get<i32>(g, 0x0C);
            } else {
                fault(name + ": a short circle goal (the original keeps uninitialised fields)");
            }
            return c;
        }
        case 5: {
            auto rg = std::make_shared<sim::RingGoal>();
            common(*rg, 0x1C);
            if (g.size() >= 0x18) {
                rg->cell_x = get<i16>(g, 4);
                rg->cell_z = get<i16>(g, 6);
                rg->inner = get<i32>(g, 8);
                rg->outer = get<i32>(g, 0x0C);
                rg->inner_cells_sq = get<i32>(g, 0x10);
                rg->outer_cells_sq = get<i32>(g, 0x14);
            } else {
                fault(name + ": a short ring goal (the original keeps uninitialised fields)");
            }
            return rg;
        }
        case 6: {
            auto e = std::make_shared<sim::EdgeGoal>();
            common(*e, 0x18);
            if (g.size() >= 0x14) {
                e->min_x = get<i32>(g, 4);
                e->max_x = get<i32>(g, 8);
                e->min_z = get<i32>(g, 0x0C);
                e->max_z = get<i32>(g, 0x10);
            } else {
                fault(name + ": a short edge goal (the original keeps uninitialised fields)");
            }
            return e;
        }
        default: return nullptr;
        }
    }

    // One order (0x43A420): its record, its kind by name (a binary search of the sorted
    // lookup names), the type of a build order by its "UTYPENAME" key, its target and goal.
    sim::MissionOrder* load_order(sim::Unit& unit, const std::string& name) {
        sim::OrderContext& oc = b.order_context();
        sim::MissionOrder* o = oc.systems.allocate_order();
        o->kind = 0;
        o->target.owner = o;
        o->created_tick = b.game_tick();
        o->next = nullptr;
        o->attached = 0;
        const BankSection::Blob* rb = s.find_blob(name);
        if (!rb || rb->data.size() < 0x3A) {
            fault(name + ": a missing order record (the original keeps an order with uninitialised fields)");
            oc.systems.free_order(o);
            return nullptr;
        }
        const std::vector<u8>& r = rb->data;
        const std::vector<std::string>& kinds = lb.unit_context.mission_names;
        u8 kind = 0;
        if (const std::string* nm = s.find_string(name + "_name")) {
            for (std::size_t k = 0; k < kinds.size(); ++k)
                if (stricmp_c(kinds[k], *nm) == 0) {
                    kind = static_cast<u8>(k);
                    break;
                }
        } else {
            // No name: the saved kind counts the table's entries without flag 0x100 (byte +0x14 bit 0).
            const auto& df = sim::mission::default_flags();
            u32 n = 0;
            const u8 want = r[8];
            for (std::size_t k = 0; k < kinds.size(); ++k) {
                kind = static_cast<u8>(k);
                if (!((df[k] >> 24) & 1)) {
                    if (n == want) break;
                    ++n;
                }
                if (k + 1 == kinds.size()) kind = static_cast<u8>(k + 1);
            }
        }
        if (get<u16>(r, 0) != unit.id || kind == 0) {
            fault(name + ": an order of another unit or of an unknown kind (the original keeps an order with uninitialised fields)");
            oc.systems.free_order(o);
            return nullptr;
        }
        o->unit = &unit;
        sim::relink_unit(oc, o->target, load(get<u16>(r, 2)));
        o->target.owner = o;
        o->step = r[9];
        o->kind = kind;
        o->wait = get<u32>(r, 0x0A);
        o->wake_tick = get<u32>(r, 0x0E);
        for (int k = 0; k < 3; ++k) o->pos[static_cast<std::size_t>(k)] = get<i32>(r, 0x12 + static_cast<std::size_t>(k) * 4);
        o->scratch0 = get<u32>(r, 0x1E);
        o->scratch1 = get<u32>(r, 0x22);
        o->param0 = get<i32>(r, 0x26);
        o->param1 = get<i32>(r, 0x2A);
        o->param2 = get<i32>(r, 0x2E);
        o->flags = get<u32>(r, 0x32);
        o->events = get<u32>(r, 0x36);
        const std::string mission = kind < kinds.size() ? kinds[kind] : std::string();
        if (names_unit_type(mission)) {
            const u32 want = static_cast<u16>(o->param0);
            u32 type = 0;
            if (const std::string* tn = s.find_string(utype_key(want))) {
                type = lb.type_index(*tn);
            } else {
                // Without the key: the n-th type (from 1) that is not a feature... counted as the original does.
                u32 n = 0;
                for (std::size_t k = 1; k < lb.type_defs.size(); ++k) {
                    const UnitDefRecord* d = lb.type_defs[k];
                    if (d && (d->type_flags & 0x20u) == 0) {
                        if (n == want) {
                            type = static_cast<u32>(k - 1);
                            break;
                        }
                        ++n;
                    }
                }
            }
            o->param0 = static_cast<i32>(type & 0xFFFF);
        }
        const i32 goal_kind = get<i32>(r, 4);
        if (std::shared_ptr<sim::OrderGoal> g = load_goal(*o, goal_kind, name + "g")) {
            o->attached = g->address;
            o->goal = std::move(g);
        }
        return o;
    }

    void load_script(sim::Unit& u, const std::vector<u8>& d) {
        if (!u.script) return;
        sim::ScriptInstance& sc = *u.script;
        const std::size_t statics = static_cast<std::size_t>(sc.program().static_count) * 4;
        const std::size_t pieces = sc.program().piece_names.size();
        if (d.size() != 0x528 + statics + pieces * 0x6C) return;
        if (get<u32>(d, 0) != sc.program().checksum) return;
        for (int t = 0; t < sim::ScriptInstance::kThreads; ++t) {
            for (int w = 0; w < sim::ScriptInstance::kThreadWords; ++w)
                sc.thread_word(t, w) = get<u32>(d, 4 + static_cast<std::size_t>(t * sim::ScriptInstance::kThreadWords + w) * 4);
            sc.drop_sink(t);
        }
        sc.count_word() = get<u32>(d, 0x524);
        for (std::size_t k = 0; k < sc.statics().size(); ++k) sc.statics()[k] = get<i32>(d, 0x528 + k * 4);
        std::vector<i32>& anims = sc.piece_anims();
        for (std::size_t p = 0; p < pieces; ++p) {
            const std::size_t at = 0x528 + statics + p * 0x6C;
            const std::size_t base = p * sim::ScriptInstance::kPieceWords;
            if (base < anims.size()) anims[base] = 1;
            const i32 pi = static_cast<i32>(p);
            u.pose.set_visible(pi, get<u32>(d, at + 24 * 4));
            u.pose.set_cached(pi, get<u32>(d, at + 25 * 4));
            u.pose.set_shaded(pi, get<u32>(d, at + 26 * 4));
            for (int k = 0; k < 3; ++k) {
                for (int f = 0; f < 6; ++f) {
                    const std::size_t a = base + 1 + static_cast<std::size_t>(f * 3 + k);
                    if (a < anims.size()) anims[a] = get<i32>(d, at + static_cast<std::size_t>(f * 3 + k) * 4);
                }
                u.pose.set_move(pi, k, get<i32>(d, at + (18 + static_cast<std::size_t>(k)) * 4));
                u.pose.set_turn(pi, k, static_cast<i16>(get<u32>(d, at + (21 + static_cast<std::size_t>(k)) * 4)));
            }
        }
        sc.set_anim_dirty(true);
    }
};

// Features (0x424C00): the saved type names map onto the battle's types (by index when the
// name matches there, else the first match, else the type is loaded and appended); then the
// normal features, the animating ones (each started again: burning, dying or reclaimed) and
// the 3D ones are placed.
void load_features(LoadedBattle& lb, const Bank& bank, LoadedView& view) {
    sim::Battle& b = *lb.battle;
    const BankSection* s = bank.find("Features");
    static const BankSection kEmpty;
    if (!s) s = &kEmpty;
    std::vector<u16> map;
    const std::size_t before = lb.feature_defs.records().size();
    if (const BankSection::Blob* names = s->find_blob("Feature Type Names"); names && !names->data.empty()) {
        const std::size_t n = names->data.size() >> 7;
        map.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            std::string nm(reinterpret_cast<const char*>(names->data.data() + i * 0x80), 0x80);
            nm.resize(std::strlen(nm.c_str()));
            const auto& recs = lb.feature_defs.records();
            if (i < recs.size() && stricmp_c(nm, recs[i].name) == 0) {
                map[i] = static_cast<u16>(i);
                continue;
            }
            const int f = lb.feature_defs.find(nm);
            map[i] = static_cast<u16>(f >= 0 ? f : lb.feature_defs.load(nm));
        }
    } else {
        map.resize(b.features.size());
        for (std::size_t i = 0; i < map.size(); ++i) map[i] = static_cast<u16>(i);
    }
    lb.feature_defs.resolve_chains();
    if (lb.feature_defs.records().size() != before) {
        // New feature types: the battle's table grows (the loaded ones keep their indices).
        b.features = to_feature_types(lb.feature_defs, lb.weapons, *lb.anims, &lb.feature_models);
        bind_feature_blocks(b);
        std::vector<std::string> type_names(lb.type_defs.size());
        for (std::size_t k = 1; k < lb.type_defs.size(); ++k)
            if (lb.type_defs[k]) type_names[k] = lb.type_defs[k]->unit_name;
        bind_resurrect_types(b.features, lb.feature_defs, type_names);
        sim::init_feature_type_cursors(b.feature_world());
    }
    sim::FeatureWorld& fw = b.feature_world();
    auto type_of = [&](u16 saved) -> u16 {
        if (saved >= map.size()) {
            view.notes.push_back("a saved feature type beyond the saved names (the original reads past its table)");
            return 0xFFFF;
        }
        return map[saved];
    };
    auto cell_index = [&](u16 x, u16 z) -> i32 {
        if (x >= b.map.width || z >= b.map.height) {
            view.notes.push_back("a saved feature off the map (the original crashes)");
            return -1;
        }
        return static_cast<i32>(z) * b.map.width + x;
    };
    {
        const i32 n = s->get_int("Number of Normal Features", 0);
        const BankSection::Blob* d = s->find_blob("Normal Features");
        for (i32 k = 0; k < n && d; ++k) {
            const std::size_t at = static_cast<std::size_t>(k) * 8;
            if (at + 8 > d->data.size()) continue;
            const i32 c = cell_index(get<u16>(d->data, at), get<u16>(d->data, at + 2));
            if (c < 0) continue;
            sim::place_feature(fw, c, type_of(get<u16>(d->data, at + 4)), nullptr, nullptr, 10);
            b.map.cells[static_cast<std::size_t>(c)].feature_aux = get<u16>(d->data, at + 6);
        }
    }
    {
        const i32 n = s->get_int("Number of Animating Features", 0);
        const BankSection::Blob* d = s->find_blob("Animating Features");
        for (i32 k = 0; k < n && d; ++k) {
            const std::size_t at = static_cast<std::size_t>(k) * 10;
            if (at + 10 > d->data.size()) continue;
            const u16 x = get<u16>(d->data, at), z = get<u16>(d->data, at + 2);
            const i32 c = cell_index(x, z);
            if (c < 0) continue;
            sim::place_feature(fw, c, type_of(get<u16>(d->data, at + 4)), nullptr, nullptr, 10);
            const u8 st = d->data[at + 9];
            if ((st & 0xF) == 0) sim::ignite_feature(fw, x, z, false);
            else if ((st & 0xF) == 1) sim::kill_feature(fw, x, z, false);
            else if ((st & 0xF) == 2) sim::kill_feature(fw, x, z, true);
            const u16 slot = b.map.cells[static_cast<std::size_t>(c)].feature_aux;
            if (slot >= b.feature_state.slots.size()) {
                view.notes.push_back("an animating feature's slot beyond the table (the original writes past it)");
                continue;
            }
            sim::FeatureSlot& sl = b.feature_state.slots[slot];
            sl.set<u16>(0x26, get<u16>(d->data, at + 6));
            sl.set<u16>(4, d->data[at + 8]);
            sl.b[0x2E] = static_cast<u8>(st & 0xF0);
        }
    }
    {
        const i32 n = s->get_int("Number of 3D Features", 0);
        const BankSection::Blob* d = s->find_blob("3D Features");
        for (i32 k = 0; k < n && d; ++k) {
            const std::size_t at = static_cast<std::size_t>(k) * 0x1A;
            if (at + 0x1A > d->data.size()) continue;
            const i32 c = cell_index(get<u16>(d->data, at), get<u16>(d->data, at + 2));
            if (c < 0) continue;
            const sim::Vec3i pos{get<i32>(d->data, at + 8), get<i32>(d->data, at + 0x0C), get<i32>(d->data, at + 0x10)};
            const std::array<i16, 3> ang{get<i16>(d->data, at + 0x14), get<i16>(d->data, at + 0x16), get<i16>(d->data, at + 0x18)};
            sim::place_feature(fw, c, type_of(get<u16>(d->data, at + 4)), &pos, &ang, 10);
            const u16 slot = b.map.cells[static_cast<std::size_t>(c)].feature_aux;
            if (slot < b.feature_state.slots.size()) b.feature_state.slots[slot].set<u16>(0x26, get<u16>(d->data, at + 6));
        }
    }
}

}  // namespace

void load_units_section(LoadedBattle& lb, const BankSection& s, LoadedView& view) {
    UnitLoader l{lb, *lb.battle, s, view};
    if (s.get_int("Version", 0) != 0x11) return;
    l.count = s.get_int("Number of Units", 0);
    for (i32 i = 0; i < l.count; ++i) {
        const BankSection::Blob* r = s.find_numbered(i);
        if (!r || r->data.empty()) continue;
        u16 id = get<u16>(r->data, 0x21);
        if (r->data.size() < 0xB8) {
            if (r->data.size() != 0xB6) continue;
            // A record two bytes short (an older version): its id word is read as zero-extended.
        }
        l.load(id);
    }
}

bool load_save_sections(LoadedBattle& lb, const Bank& bank, LoadedView& view, std::string& error) {
    sim::Battle& b = *lb.battle;
    static const BankSection kEmpty;
    auto sec = [&](const char* n) -> const BankSection& {
        const BankSection* s = bank.find(n);
        return s ? *s : kEmpty;
    };
    // Players (0x466050).
    {
        const BankSection& p = sec("Players");
        const u8 human = static_cast<u8>(p.get_int("Human Player", 10));
        b.settings.local_player = human;
        b.settings.local_view = human;
        const BankSection::Blob* tb = p.find_blob("GameTime");
        if (tb && tb->data.size() >= 0x1C) {
            std::memcpy(view.time_block.data(), tb->data.data(), 0x1C);
            b.set_game_tick(get<u32>(tb->data, 0x10));
            for (int k = 0; k < 10; ++k) {
                sim::PlayerSlot& ps = b.players[static_cast<std::size_t>(k)];
                const BankSection* s = bank.find("Player " + std::to_string(k));
                if (!s) {
                    ps.controller = 0;
                    continue;
                }
                sim::PlayerEconomy& e = b.player_economy[static_cast<std::size_t>(k)];
                ps.energy_stored = static_cast<float>(s->get_double("Energy", 0.0));
                ps.metal_stored = static_cast<float>(s->get_double("Metal", 0.0));
                e.energy_produced = s->get_double("TotalEnergyProduced", 0.0);
                e.metal_produced = s->get_double("TotalMetalProduced", 0.0);
                e.energy_consumed = s->get_double("TotalEnergyConsumed", 0.0);
                e.metal_consumed = s->get_double("TotalMetalConsumed", 0.0);
                e.energy_wasted = s->get_double("EnergyWasted", 0.0);
                e.metal_wasted = s->get_double("MetalWasted", 0.0);
                e.energy_storage_bonus = static_cast<float>(s->get_double("PlayerEnergyStorage", 0.0));
                e.metal_storage_bonus = static_cast<float>(s->get_double("PlayerMetalStorage", 0.0));
                ps.flags = static_cast<u16>((ps.flags & ~1u) | (static_cast<u32>(s->get_int("AddPlayerStorage", 0)) & 1u));
                ps.units_killed = static_cast<u16>(s->get_int("Kills", 0));
                ps.units_lost = static_cast<u16>(s->get_int("Losses", 0));
                e.next_update_tick = static_cast<u32>(s->get_int("UpdateTime", 0));
                e.win_lose_tick = static_cast<u32>(s->get_int("WinLoseTime", 0));
                e.display_tick = static_cast<u32>(s->get_int("DisplayTimer", 0));
                view.logo[static_cast<std::size_t>(k)] = static_cast<u8>(s->get_int("Logo", 0));
                view.side[static_cast<std::size_t>(k)] = static_cast<u8>(s->get_int("Side", 0));
                if (const BankSection::Blob* al = s->find_blob("Alliances"); al && al->data.size() == 0xB)
                    for (std::size_t o = 0; o < 11; ++o) b.alliances[static_cast<std::size_t>(k)][o] = al->data[o];
                b.alliances[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)] = 1;
            }
        }
    }
    // Camera (0x41D2B0).
    {
        const BankSection& c = sec("Camera");
        view.camera_z = c.get_int("Z Position", view.camera_z);
        view.camera_x = c.get_int("X Position", view.camera_x);
    }
    load_features(lb, bank, view);
    // Metal (0x484D60), PlayerFeatures (0x484E80), Mapping (0x484FA0).
    {
        const std::size_t n = b.map.cells.size();
        if (const BankSection* m = bank.find("Metal"))
            if (const BankSection::Blob* d = m->find_blob("Plotmap"); d && d->data.size() == n)
                for (std::size_t i = 0; i < n; ++i) b.map.cells[i].metal = d->data[i];
        const std::size_t half = static_cast<std::size_t>(b.map.width * b.map.height / 2);
        if (const BankSection* pf = bank.find("PlayerFeatures"))
            if (const BankSection::Blob* d = pf->find_blob("Plotmap"); d && d->data.size() == half && half)
                for (std::size_t i = 0; i < half; ++i) {
                    u8& c0 = b.map.cells[2 * i].flags;
                    u8& c1 = b.map.cells[2 * i + 1].flags;
                    c0 = static_cast<u8>(((d->data[i] >> 1) & 0x78) | (c0 & 0x87));
                    c1 = static_cast<u8>(((d->data[i] & 0xF) << 3) | (c1 & 0x87));
                }
        const std::size_t en = static_cast<std::size_t>((static_cast<u32>(b.map.width * b.map.height) & 0x7FFFFFFFu) >> 1);
        if (const BankSection* mp = bank.find("Mapping"))
            if (const BankSection::Blob* d = mp->find_blob("Data"); d && d->data.size() == en)
                std::memcpy(b.explored.data(), d->data.data(), std::min(en, b.explored.size() * 2));
    }
    if (const BankSection* u = bank.find("Units")) load_units_section(lb, *u, view);
    // Meteor (0x438250).
    {
        const BankSection& m = sec("Meteor");
        sim::MeteorShower& ms = b.meteors;
        ms.enabled = m.get_int("Enabled", 0) != 0;
        ms.active = m.get_int("Active", 0) != 0;
        ms.next_schedule = static_cast<u32>(m.get_int("Next Strike Time", 0));
        ms.end_tick = static_cast<u32>(m.get_int("Time Strike Ends", 0));
        ms.next_spawn = static_cast<u32>(m.get_int("Next Hit Time", 0));
        ms.aim_x = static_cast<i16>(m.get_int("Origin X", 0));
        ms.aim_z = static_cast<i16>(m.get_int("Origin Z", 0));
        ms.origin_x = static_cast<i16>(m.get_int("Target X", 0));
        ms.origin_z = static_cast<i16>(m.get_int("Target Z", 0));
    }
    // A mission's goals (0x48FE60).
    if (b.settings.provider == 1) {
        auto get_goal = [&](sim::MissionGoal& g) {
            const BankSection& s = sec(goal_section(g.kind));
            const char* ck = goal_count_key(g.kind);
            if (*ck) g.count = s.get_int(ck, 0);
            g.satisfied = s.get_int("Satisfied", 0) != 0;
            g.celebrated = s.get_int("Celebrated", 0) != 0;
        };
        for (sim::MissionGoal& g : b.mission_goals.win) get_goal(g);
        for (sim::MissionGoal& g : b.mission_goals.loss) get_goal(g);
    }
    (void)error;
    return true;
}

}  // namespace ta
