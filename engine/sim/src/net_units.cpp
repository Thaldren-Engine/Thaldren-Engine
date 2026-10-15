// Network play inside the battle: the unit packets each machine applies to the units of
// players on other machines, and the unit state packets (docs/RE_MEMORY.md, "Network and
// sync"). Written from the original's routines named at each member.

#include "sim/battle.h"
#include "sim/net_units.h"
#include "sim/x87.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
void wr16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void wr32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

constexpr u32 kDying = 0x4000u, kFractionChanged = 0x2000u, kResettle = 0x10000u;

// Whether a motion goal has a state to send (vtable +0x1C).
bool has_net_state(const MotionGoal& g, const MotionState& m) {
    if (auto* r = dynamic_cast<const RouteGoal*>(&g)) return (r->flags & route_flags::kChanged) || ((m.flags ^ r->flags) & 4);
    if (auto* f = dynamic_cast<const FlightGoal*>(&g)) return f->local && (f->net_flags & 1);
    return false;
}

void write_point_goal(BitWriter& w, const PointGoal& p) {
    namespace pg = point_goal;
    w.put(p.flags, 8);
    if (p.flags & pg::kFollow) {
        w.put(static_cast<u32>(static_cast<i32>(p.piece)), 16);
        w.put(p.link.unit ? p.link.unit->id : 0u, 16);
    }
    if (p.flags & pg::kRadius) w.put(static_cast<u32>(static_cast<i32>(p.radius)), 16);
    if (p.flags & pg::kAltitude) w.put(static_cast<u32>(static_cast<i32>(p.altitude)), 16);
    if (p.flags & pg::kHeading) w.put(static_cast<u32>(static_cast<i32>(p.goal_heading)), 16);
    if (p.flags & pg::kGround)
        for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(p.pos[static_cast<std::size_t>(k)]), 32);
}

void write_manoeuvre_goal(BitWriter& w, const ManoeuvreGoal& m) {
    w.put(m.flags, 1);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(m.pos[static_cast<std::size_t>(k)]), 32);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(m.vel[static_cast<std::size_t>(k)]), 32);
    if (m.flags & 1) w.put(m.target_heading, 16);
}

}  // namespace

int unit_type_bits(std::size_t type_count) {
    int bits = 0;
    for (std::size_t n = type_count; n; n >>= 1) ++bits;
    return bits;
}

std::uint32_t encode_build_fraction(float f) {
    if (f == 0.0f) return 0;
    return static_cast<u32>(1 - x87::ftol32(static_cast<double>(f) * -254.0)) & 0xFFu;
}

double decode_build_fraction(std::uint32_t byte) { return static_cast<double>(byte) * static_cast<double>(0.003921569f); }

// Packet 9 received (0x4861D0): the unit at the packet's id becomes a new, unfinished unit of
// the packet's type at its position, for the sending player. A live unit there is killed first.
Unit* Battle::apply_unit_packet(int slot, const std::uint8_t* pk) {
    sync_settings();
    const u16 id = rd16(pk + 3), type = rd16(pk + 1);
    const PlayerSlot& p = players[static_cast<std::size_t>(slot & 0xFF)];
    if (p.first_unit > p.last_unit) return nullptr;  // the record has no unit table
    if (id == 0 || id >= units.size()) {
        net_->fault = "unit packet for unit 0 or beyond the table (the original reads through a bad pointer)";
        return nullptr;
    }
    if (type >= types.size()) {
        net_->fault = "unit packet with an unknown type";
        return nullptr;
    }
    Unit& u = units[id];
    UnitTickState& t = ticks[id];
    if (t.type_index != 0) {
        std::uint32_t word = 0;
        kill_unit(combat_, u, 0, word);
    }
    const i32 x = static_cast<i32>(rd32(pk + 5)), y = static_cast<i32>(rd32(pk + 9)), z = static_cast<i32>(rd32(pk + 13));
    const BattleUnitType& bt = types[type];
    t.type_index = type;
    init_unit_fields(u, t, bt, x, y, z, false);
    build_script_and_pose(u, bt.create);
    setup_weapon_slots(u, bt.create.weapons);
    update_metal_extraction(u, map, bt.create.extracts_metal);
    if (bt.create.movement == 1) {
        t.has_motion = true;
        sim::create_motion(motion_, u);
        u.heading = bt.create.build_angle;
    }
    std::uint32_t word = 0;
    settle_height(u, word);
    claim_footprint(footprint_, u);
    stamp_line_of_sight(u);
    PlayerSlot& owner = players[static_cast<std::size_t>(t.owner)];
    ++owner.alive_count;
    ++owner.units_created;
    outside_.victory_unit_created(u);
    ++net_->units_made;
    return &u;
}

void Battle::write_full_state(BitWriter& w, const Unit& u) const {
    // 0x48B200: one unit's whole state.
    const UnitTickState& t = ticks[u.id];
    const int bits = unit_type_bits(types.size());
    w.put(t.type_index, bits);
    if (t.type_index == 0) return;
    w.put(static_cast<u32>(static_cast<i32>(u.hit_points)), 16);
    w.put(encode_build_fraction(u.build_fraction_left), 8);
    w.put(u.activate_flags, 8);
    w.put(u.flags & 3u, 2);
    if (u.carrier) {
        w.put_bit(true);
        w.put(u.carrier->id, 15);
        w.put(static_cast<u32>(static_cast<i32>(static_cast<std::int8_t>(u.transport_piece))), 8);
        return;
    }
    w.put_bit(false);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(u.pos[static_cast<std::size_t>(k)]), 32);
    w.put(u.heading, 16);
    w.put(static_cast<u16>(u.bank), 16);
    w.put(static_cast<u16>(u.pitch), 16);
    if (t.has_motion) w.put(static_cast<u32>(t.motion.speed), 32);
}

std::vector<std::uint8_t> Battle::unit_state_packet(int slot) {
    // 0x48B710: the player's units whose motion goal has news, then one unit's whole state
    // (the unit slot step mod units per player of the player's range).
    sync_settings();
    const PlayerSlot& p = players[static_cast<std::size_t>(slot)];
    const int bits = unit_type_bits(types.size());
    BitWriter w;
    w.put(0x2C, 8);
    w.put(0, 16);
    w.put(unit_env_.game_tick, 32);
    for (int id = p.first_unit; id <= p.last_unit && id >= 0 && static_cast<std::size_t>(id) < units.size(); ++id) {
        Unit& u = units[static_cast<std::size_t>(id)];
        UnitTickState& t = ticks[static_cast<std::size_t>(id)];
        if (!(u.flags & unit_flags::kAlive) || !t.has_motion || !t.motion.goal) continue;
        MotionGoal& g = *t.motion.goal;
        if (!has_net_state(g, t.motion)) continue;
        w.put(static_cast<u32>(id - p.first_unit), 16);
        w.put(t.type_index, bits);
        if (auto* r = dynamic_cast<RouteGoal*>(&g)) {
            // 0x44F4A0: blocked bit, up to three waypoints of the route.
            u32 n = 0;
            if (r->flags & route_flags::kHasRoute) n = r->count > 2 ? 3u : static_cast<u32>(r->count);
            w.put_bit((t.motion.flags & 4) != 0);
            w.put(n, 2);
            for (u32 k = 0; k < n; ++k) {
                w.put(r->points[k] & 0xFFFFu, 16);
                w.put(r->points[k] >> 16, 16);
            }
            r->flags = static_cast<u8>((r->flags & 0xF3) ^ (t.motion.flags & 4));
        } else if (auto* f = dynamic_cast<FlightGoal*>(&g)) {
            // 0x4908C0: the order goal (none, point, manoeuvre) and the layer.
            if (!f->attached) {
                w.put(0, 2);
            } else if (auto* pg = dynamic_cast<PointGoal*>(f->attached)) {
                w.put(1, 2);
                write_point_goal(w, *pg);
            } else if (auto* mg = dynamic_cast<ManoeuvreGoal*>(f->attached)) {
                w.put(2, 2);
                write_manoeuvre_goal(w, *mg);
            }  // other kinds write nothing (aircraft never hold them)
            w.put(t.motion.flags & 3u, 2);
            f->net_flags = static_cast<u8>(((t.motion.flags & 3) << 1) | (f->net_flags & 0xF8));
        }
        ++net_->states_written;
        if (w.size() > 0x1FF) break;
    }
    w.put(0xFFFF, 16);
    w.put_bit(true);
    if (settings.units_per_player == 0) {
        net_->fault = "unit state packet with no units per player (the original divides by zero)";
    } else {
        const i32 idx = static_cast<i32>(unit_env_.game_tick % settings.units_per_player);
        const std::size_t at = static_cast<std::size_t>(p.first_unit + idx);
        if (at < units.size()) write_full_state(w, units[at]);
        ++net_->full_states;
    }
    const std::size_t size = w.size();
    w.set_byte(1, static_cast<u8>(size));
    w.set_byte(2, static_cast<u8>(size >> 8));
    return w.bytes();
}

void Battle::apply_full_state(BitReader& r, Unit& u) {
    // 0x48B3F0.
    const int bits = unit_type_bits(types.size());
    UnitTickState& t = ticks[u.id];
    const i16 type = static_cast<i16>(r.get(bits));
    if (type == 0) {
        if (t.type_index != 0) u.flags |= kDying;
        return;
    }
    if (static_cast<i16>(t.type_index) != type) {
        u8 pk[23] = {0x09};
        wr16(pk + 1, static_cast<u16>(type));
        wr16(pk + 3, u.id);
        for (int k = 0; k < 3; ++k) wr32(pk + 5 + 4 * k, static_cast<u32>(u.pos[static_cast<std::size_t>(k)]));
        apply_unit_packet(t.owner, pk);
    }
    u.hit_points = static_cast<i16>(r.get(16));
    const double f = decode_build_fraction(r.get(8));
    if (f != static_cast<double>(u.build_fraction_left)) {
        u.build_fraction_left = static_cast<float>(f);
        u.flags |= kFractionChanged;
    }
    const u8 act = static_cast<u8>(r.get(8));
    unit_set_activate(u, *this, act, true);
    unit_set_activate(u, *this, static_cast<u8>(~act), false);
    const u32 layer = r.get(2);
    if (r.get_bit()) {
        const u16 carrier = static_cast<u16>(r.get(15));
        const u8 piece = static_cast<u8>(r.get_signed(8));
        apply_transport_attach(missions_, construction_hooks_, u, carrier ? &units[carrier] : nullptr, piece, static_cast<u8>(layer));
        return;
    }
    if (u.carrier) apply_transport_attach(missions_, construction_hooks_, u, nullptr, 0xFF, static_cast<u8>(layer));
    const i32 x = static_cast<i32>(r.get(32)), y = static_cast<i32>(r.get(32)), z = static_cast<i32>(r.get(32));
    const u16 heading = static_cast<u16>(r.get(16)), bank = static_cast<u16>(r.get(16)), pitch = static_cast<u16>(r.get(16));
    const i16 cx = footprint_cell(x, u.footprint_x), cz = footprint_cell(z, u.footprint_z);
    if (cx == u.cell_x && cz == u.cell_z && layer == (u.flags & 3u)) {
        u.pos = {x, y, z};
    } else {
        release_cells(footprint_, u);
        u.pos = {x, y, z};
        u.cell_x = cx;
        u.cell_z = cz;
        u.flags = (u.flags & ~3u) | (layer & 3u);
        claim_footprint(footprint_, u);
        update_sight(u);
    }
    u.flags |= kResettle;
    u.pitch = static_cast<i16>(pitch);
    u.heading = heading;
    u.bank = static_cast<i16>(bank);
    if (t.has_motion) t.motion.speed = static_cast<i32>(r.get(32));
}

std::uint32_t Battle::apply_unit_state_packet(int slot, const std::uint8_t* packet, std::size_t size) {
    // 0x48B920.
    sync_settings();
    BitReader r(packet, size);
    r.get(8);
    r.get(16);
    const u32 tick = r.get(32);
    const PlayerSlot& p = players[static_cast<std::size_t>(slot)];
    if (p.first_unit > p.last_unit) return tick;
    const int bits = unit_type_bits(types.size());
    for (i16 off = static_cast<i16>(r.get(16)); off != -1; off = static_cast<i16>(r.get(16))) {
        const i32 id = p.first_unit + off;
        if (id < 0 || static_cast<std::size_t>(id) >= units.size()) {
            net_->fault = "unit state for a unit outside the table";
            return tick;
        }
        Unit& u = units[static_cast<std::size_t>(id)];
        UnitTickState& t = ticks[static_cast<std::size_t>(id)];
        const i16 type = static_cast<i16>(r.get(bits));
        if (static_cast<i16>(t.type_index) != type) {
            u8 pk[23] = {0x09};
            wr16(pk + 1, static_cast<u16>(type));
            wr16(pk + 3, u.id);
            for (int k = 0; k < 3; ++k) wr32(pk + 5 + 4 * k, static_cast<u32>(u.pos[static_cast<std::size_t>(k)]));
            apply_unit_packet(t.owner, pk);
        }
        ++net_->states_read;
        if (!t.has_motion || !t.motion.goal) {
            net_->fault = "unit state for a unit without motion (the original calls through a null pointer)";
            return tick;
        }
        MotionGoal& g = *t.motion.goal;
        if (auto* rr = dynamic_cast<RemoteRouteGoal*>(&g)) {
            // 0x44F5C0: blocked bit into the motion flags, up to three waypoints.
            t.motion.flags = static_cast<u8>((t.motion.flags & 0xFB) | (r.get_bit() ? 4 : 0));
            rr->count = static_cast<i32>(r.get(2));
            for (i32 k = 0; k < rr->count; ++k) {
                const u32 px = r.get(16), pz = r.get(16);
                rr->points[static_cast<std::size_t>(k)] = (px & 0xFFFFu) | (pz << 16);
            }
        } else if (auto* f = dynamic_cast<FlightGoal*>(&g); f && !f->local) {
            // 0x490A10: a new order goal (the old one is deleted), then the layer.
            if (net_->remote_goals.size() < units.size()) net_->remote_goals.resize(units.size());
            auto& slot_goal = net_->remote_goals[static_cast<std::size_t>(id)];
            if (f->attached) {
                f->attached->release();
                f->attached = nullptr;
            }
            slot_goal.reset();
            const u32 kind = r.get(2);
            if (kind == 1) {
                namespace pgf = point_goal;
                auto pg = std::make_shared<PointGoal>();
                pg->owner = &u;
                pg->pos = net_inputs.fresh_goal_pos;
                pg->offset = net_inputs.fresh_goal_offset;
                pg->flags = static_cast<u16>(r.get(8));
                if (pg->flags & pgf::kFollow) {
                    pg->piece = static_cast<i16>(r.get(16));
                    const u16 tid = static_cast<u16>(r.get(16));
                    relink_unit(orders_, pg->link, tid ? &units[tid < units.size() ? tid : 0] : nullptr);
                    pg->link.goal = pg.get();
                }
                pg->radius = (pg->flags & pgf::kRadius) ? static_cast<i16>(r.get(16)) : i16{0};
                pg->altitude = (pg->flags & pgf::kAltitude) ? static_cast<i16>(r.get(16)) : i16{0};
                pg->goal_heading = (pg->flags & pgf::kHeading) ? static_cast<i16>(r.get(16)) : i16{0};
                if (pg->flags & pgf::kGround)
                    for (int k = 0; k < 3; ++k) pg->pos[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
                slot_goal = pg;
            } else if (kind == 2) {
                auto mg = std::make_shared<ManoeuvreGoal>();
                mg->owner = &u;
                mg->flags = static_cast<u16>(r.get(1));
                for (int k = 0; k < 3; ++k) mg->pos[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
                for (int k = 0; k < 3; ++k) mg->vel[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
                if (mg->flags & 1) mg->target_heading = static_cast<u16>(r.get(16));
                slot_goal = mg;
            }
            f->attached = slot_goal.get();
            set_motion_layer(motion_, u, r.get(2));
        }
        // Other goals read nothing (the stream then goes out of step, as in the original).
    }
    // Every unit of the sender with motion moves one step.
    for (int id = p.first_unit; id <= p.last_unit && static_cast<std::size_t>(id) < units.size(); ++id) {
        Unit& u = units[static_cast<std::size_t>(id)];
        UnitTickState& t = ticks[static_cast<std::size_t>(id)];
        if (!(u.flags & unit_flags::kAlive) || !t.has_motion) continue;
        run_motion(motion_, u);
        std::uint32_t word = 0;
        settle_height(u, word);
    }
    if (r.get_bit()) {
        if (settings.units_per_player == 0) {
            net_->fault = "unit state packet with no units per player (the original divides by zero)";
        } else {
            const i32 idx = static_cast<i32>(tick % settings.units_per_player);
            const std::size_t at = static_cast<std::size_t>(p.first_unit + idx);
            if (at < units.size()) apply_full_state(r, units[at]);
        }
    }
    return tick;
}

void Battle::apply_transport_packet(const std::uint8_t* pk) {
    sync_settings();
    const u16 cargo = rd16(pk + 1), transport = rd16(pk + 3);
    if (cargo == 0 || cargo >= units.size() || transport >= units.size()) {
        net_->fault = "transport packet with a bad unit";
        return;
    }
    apply_transport_attach(missions_, construction_hooks_, units[cargo], transport ? &units[transport] : nullptr, pk[5], pk[6]);
}

void Battle::apply_fire_packet(const std::uint8_t* pk) {
    // 0x49D270: a shot fired on another machine.
    sync_settings();
    const u8 index = pk[25];
    const WeaponType* w = index < net_->weapons.size() ? net_->weapons[index] : nullptr;
    if (!w) {
        net_->fault = "fire packet for a weapon this battle does not know";
        return;
    }
    Vec3i start{static_cast<i32>(rd32(pk + 1)), static_cast<i32>(rd32(pk + 5)), static_cast<i32>(rd32(pk + 9))};
    Vec3i aim{static_cast<i32>(rd32(pk + 13)), static_cast<i32>(rd32(pk + 17)), static_cast<i32>(rd32(pk + 21))};
    if (w->flags & weapon_flags::kMeteor) {
        // Ownerless: the second vector is the velocity.
        Projectile* pr = alloc_projectile(projectiles);
        if (!pr) return;
        init_projectile(projectile_world_, *pr, *w, start, nullptr, unit_env_.game_tick, nullptr);
        pr->vel = aim;
        return;
    }
    const u16 owner_id = rd16(pk + 33);
    if (owner_id == 0 || owner_id >= units.size()) return;
    Unit& owner = units[owner_id];
    if (!(owner.flags & unit_flags::kAlive)) return;
    const int si = pk[35];
    if (si < 0 || si > 2) {
        net_->fault = "fire packet for weapon slot beyond 2";
        return;
    }
    WeaponSlot& slot = owner.weapons[static_cast<std::size_t>(si)];
    slot.aim_pitch = rd16(pk + 29);
    slot.aim_heading = rd16(pk + 27);
    const u16 target_id = rd16(pk + 31);
    Unit* target = target_id && target_id < units.size() ? &units[target_id] : nullptr;
    i32 intercept = -1;
    if (pk[26] & 1) {
        // The shot it chases: one not of this machine's player aimed at the same point whose
        // owner is the packet's target.
        for (i32 k = 0; k < projectiles.count; ++k) {
            const Projectile& q = projectiles.slots[static_cast<std::size_t>(k)];
            if (q.owner_slot != settings.local_player && q.aim == aim && q.owner && q.owner->id == target_id) {
                intercept = k;
                break;
            }
        }
    }
    if (!slot.type) {
        net_->fault = "fire packet for an empty weapon slot (the original reads through a null pointer)";
        return;
    }
    const FireSlot fs{slot.type, si, slot.aim_heading, slot.aim_pitch, static_cast<u32>(slot.aux)};
    const u32 f = w->flags;  // the packet's weapon picks the routine, the slot's weapon is fired
    if (f & weapon_flags::kBallistic) {
        fire_ballistic(projectile_world_, fs, owner, start, aim, target);
    } else if (f & weapon_flags::kVerticalLaunch) {
        fire_vertical(projectile_world_, fs, owner, start, aim, target, intercept);
    } else if (!(f & weapon_flags::kLineOfSight) && !(f & weapon_flags::kSelfPropelled)) {
        if (f & weapon_flags::kDropped) spawn_from_motion(projectile_world_, fs, owner, start);
    } else {
        fire_directed(projectile_world_, fs, owner, start, aim, target);
    }
}

void Battle::apply_script_packet(const std::uint8_t* pk) {
    const u16 id = rd16(pk + 1);
    if (id == 0 || id >= units.size()) {
        net_->fault = "script packet for unit 0 (the original reads through a null pointer)";
        return;
    }
    Unit& u = units[id];
    if (!(u.flags & unit_flags::kAlive) || !u.script) return;
    u.script->call_with_args(static_cast<i16>(rd16(pk + 3)), nullptr, false, pk[5], static_cast<i32>(rd32(pk + 6)),
                             static_cast<i32>(rd32(pk + 10)), static_cast<i32>(rd32(pk + 14)), static_cast<i32>(rd32(pk + 18)));
}

void Battle::apply_activate_packet(const std::uint8_t* pk) {
    const u16 id = rd16(pk + 1);
    if (id == 0 || id >= units.size()) {
        net_->fault = "activation packet for unit 0 (the original reads through a null pointer)";
        return;
    }
    Unit& u = units[id];
    if (!(u.flags & unit_flags::kAlive)) return;
    unit_set_activate(u, *this, pk[3], true);
    unit_set_activate(u, *this, static_cast<u8>(~pk[3]), false);
}

void Battle::apply_finished_packet(const std::uint8_t* pk) {
    sync_settings();
    const u16 id = rd16(pk + 1), builder = rd16(pk + 3);
    Unit* u = id && id < units.size() ? &units[id] : nullptr;
    Unit* b = builder && builder < units.size() ? &units[builder] : nullptr;
    finalize_built_unit(missions_, b, u);
}

void Battle::apply_ownership_packet(int new_owner, const std::uint8_t* pk) {
    // 0x488570 called with the packet: the new (local) owner gets a finished copy with the
    // packet's hit points, build fraction (sent as an integer), attitude and stockpiles. The old
    // unit is not touched here (its machine destroys it).
    sync_settings();
    const u16 id = rd16(pk + 1);
    if (id == 0 || id >= units.size()) return;
    Unit& old = units[id];
    const PlayerSlot& np = players[static_cast<std::size_t>(new_owner)];
    if (old.id < ticks.size() && ticks[old.id].owner == new_owner) return;
    if (!(old.flags & unit_flags::kAlive) || (old.flags & kDying)) return;
    outside_.victory_unit_transferred(old);
    const PlayerSlot& op = players[static_cast<std::size_t>(ticks[old.id].owner)];
    if (op.active && (op.controller == 1 || op.controller == 2)) return;  // a local unit is never transferred by a packet
    if (!np.active || !(np.controller == 1 || np.controller == 2)) return;
    Unit* c = create_unit(np.state, ticks[old.id].type_index, old.pos[0], old.pos[1], old.pos[2], true, old.flags & 3u, 0);
    if (!c) return;
    c->flags &= 0xFFC3FFFFu;
    c->hit_points = static_cast<i16>(rd16(pk + 11));
    c->build_fraction_left = static_cast<float>(static_cast<i32>(rd32(pk + 7)));
    const u32 att = rd32(pk + 15);
    c->pitch = static_cast<i16>(att & 0xFFFF);
    c->heading = static_cast<u16>(att >> 16);
    c->bank = static_cast<i16>(rd16(pk + 19));
    for (int k = 0; k < 3; ++k)
        if (c->weapons[static_cast<std::size_t>(k)].flags & slot_flags::kEnabled) c->weapons[static_cast<std::size_t>(k)].stockpile = pk[21 + k];
    unit_set_activate(*c, *this, old.activate_flags, true);
    unit_set_activate(*c, *this, static_cast<u8>(~old.activate_flags), false);
}

void Battle::share_explored_map(int from, int to) {
    if (!sight_ready_ || from < 0 || to < 0 || from > 10 || to > 10) return;
    sync_sight();
    share_explored(sight_world_, static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(to));
}

}  // namespace sim
