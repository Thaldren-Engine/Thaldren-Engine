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

NetPointGoal plain(const PointGoal& p) {
    NetPointGoal g;
    g.flags = p.flags;
    g.piece = p.piece;
    g.radius = p.radius;
    g.altitude = p.altitude;
    g.heading = p.goal_heading;
    g.link_id = p.link.unit ? p.link.unit->id : 0;
    g.pos = p.pos;
    return g;
}

NetManoeuvreGoal plain(const ManoeuvreGoal& m) {
    NetManoeuvreGoal g;
    g.flags = m.flags;
    g.pos = m.pos;
    g.vel = m.vel;
    g.target_heading = m.target_heading;
    return g;
}

}  // namespace

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
    NetWholeState st;
    st.type = t.type_index;
    st.hit_points = u.hit_points;
    st.fraction_left = u.build_fraction_left;
    st.activate = u.activate_flags;
    st.layer = u.flags & 3u;
    st.carried = u.carrier != nullptr;
    st.carrier_id = u.carrier ? u.carrier->id : 0;
    st.piece = static_cast<std::int8_t>(u.transport_piece);
    st.pos = u.pos;
    st.heading = u.heading;
    st.bank = static_cast<u16>(u.bank);
    st.pitch = static_cast<u16>(u.pitch);
    st.has_motion = t.has_motion;
    st.speed = t.motion.speed;
    write_whole_state(w, unit_type_bits(types.size()), st);
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
            write_route_state(w, (t.motion.flags & 4) != 0, r->flags, r->count, r->points.data());
            r->flags = route_flags_after_send(r->flags, t.motion.flags);
        } else if (auto* f = dynamic_cast<FlightGoal*>(&g)) {
            // 0x4908C0: the order goal (none, point, manoeuvre) and the layer.
            if (!f->attached) {
                w.put(0, 2);
            } else if (auto* pg = dynamic_cast<PointGoal*>(f->attached)) {
                w.put(1, 2);
                write_point_goal_state(w, plain(*pg));
            } else if (auto* mg = dynamic_cast<ManoeuvreGoal*>(f->attached)) {
                w.put(2, 2);
                write_manoeuvre_goal_state(w, plain(*mg));
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
    // 0x48B3F0 on this battle's unit.
    struct Target final : WholeStateTarget {
        Battle& b;
        Unit& u;
        Target(Battle& battle, Unit& unit) : b(battle), u(unit) {}
        UnitTickState& t() { return b.ticks[u.id]; }
        std::uint16_t type() override { return t().type_index; }
        void make(std::uint16_t type) override {
            u8 pk[23] = {0x09};
            wr16(pk + 1, type);
            wr16(pk + 3, u.id);
            for (int k = 0; k < 3; ++k) wr32(pk + 5 + 4 * k, static_cast<u32>(u.pos[static_cast<std::size_t>(k)]));
            b.apply_unit_packet(t().owner, pk);
        }
        void add_flags(std::uint32_t bits) override { u.flags |= bits; }
        void set_hit_points(std::int16_t hp) override { u.hit_points = hp; }
        float fraction_left() override { return u.build_fraction_left; }
        void set_fraction_left(float f) override { u.build_fraction_left = f; }
        void activate(std::uint8_t mask, bool on) override { unit_set_activate(u, b, mask, on); }
        bool carried() override { return u.carrier != nullptr; }
        void transport(std::uint16_t carrier, std::uint8_t piece, std::uint8_t layer) override {
            apply_transport_attach(b.missions_, b.construction_hooks_, u, carrier ? &b.units[carrier] : nullptr, piece, layer);
        }
        bool same_cells(std::int32_t x, std::int32_t z, std::uint32_t layer) override {
            return footprint_cell(x, u.footprint_x) == u.cell_x && footprint_cell(z, u.footprint_z) == u.cell_z && layer == (u.flags & 3u);
        }
        void set_position(const std::array<std::int32_t, 3>& pos) override { u.pos = pos; }
        void move_cells(const std::array<std::int32_t, 3>& pos, std::uint32_t layer) override {
            release_cells(b.footprint_, u);
            u.pos = pos;
            u.cell_x = footprint_cell(pos[0], u.footprint_x);
            u.cell_z = footprint_cell(pos[2], u.footprint_z);
            u.flags = (u.flags & ~3u) | (layer & 3u);
            claim_footprint(b.footprint_, u);
            b.update_sight(u);
        }
        void set_angles(std::uint16_t pitch, std::uint16_t heading, std::uint16_t bank) override {
            u.pitch = static_cast<i16>(pitch);
            u.heading = heading;
            u.bank = static_cast<i16>(bank);
        }
        bool has_motion() override { return t().has_motion; }
        void set_speed(std::int32_t speed) override { t().motion.speed = speed; }
    } target(*this, u);
    apply_whole_state(r, unit_type_bits(types.size()), target);
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
            bool blocked = false;
            read_route_state(r, blocked, rr->count, rr->points);
            t.motion.flags = static_cast<u8>((t.motion.flags & 0xFB) | (blocked ? 4 : 0));
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
                NetPointGoal ng;
                ng.pos = pg->pos;
                read_point_goal_state(r, ng);
                pg->flags = ng.flags;
                if (ng.flags & pgf::kFollow) {
                    pg->piece = ng.piece;
                    relink_unit(orders_, pg->link, ng.link_id ? &units[ng.link_id < units.size() ? ng.link_id : 0] : nullptr);
                    pg->link.goal = pg.get();
                }
                pg->radius = ng.radius;
                pg->altitude = ng.altitude;
                pg->goal_heading = ng.heading;
                pg->pos = ng.pos;
                slot_goal = pg;
            } else if (kind == 2) {
                auto mg = std::make_shared<ManoeuvreGoal>();
                mg->owner = &u;
                NetManoeuvreGoal ng;
                read_manoeuvre_goal_state(r, ng);
                mg->flags = ng.flags;
                mg->pos = ng.pos;
                mg->vel = ng.vel;
                if (ng.flags & 1) mg->target_heading = ng.target_heading;
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
