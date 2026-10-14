#include "sim/unit.h"

#include "sim/x87.h"
#include "sim/x87_math.h"
#include "sim/x87_trig.h"

namespace sim {
namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

i32 add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }

// Inverse of pack_xz: X as 16.16 (integer part in the high half), Z as integer << 16.
void unpack_xz(i32 packed, i32& x, i32& z) {
    u32 hi = static_cast<u32>(packed) & 0xFFFF0000u;
    const u32 lo = static_cast<u32>(packed) << 16;
    if (static_cast<i32>(lo) < 0) hi += 0x10000u;
    x = static_cast<i32>(hi);
    z = static_cast<i32>(lo);
}

Unit* lookup(UnitWorld& world, i32 a) {
    if (static_cast<std::int16_t>(a) == 0) return nullptr;
    Unit* u = world.unit_by_id(static_cast<std::uint16_t>(a));
    return (u && (u->flags & unit_flags::kAlive)) ? u : nullptr;
}

} // namespace

std::array<i32, 3> piece_local_offset(const Unit& unit, i32 piece) {
    const auto& pieces = unit.pose.pieces;
    if (piece < 0 || static_cast<std::size_t>(piece) >= pieces.size() || !unit.pose.model()) return {0, 0, 0};
    const auto& nodes = unit.pose.model()->nodes;
    const PosePiece* p = &pieces[static_cast<std::size_t>(piece)];
    const auto& n = nodes[static_cast<std::size_t>(p->node)];
    i32 v[3] = {add(n.offset[0], p->move[0]), add(n.offset[1], p->move[1]), add(n.offset[2], p->move[2])};
    // Each ancestor rotates the accumulated offset by its own turn (the top
    // ancestor also by the unit's attitude) and then adds its own offset.
    for (int parent = p->parent; parent >= 0; parent = pieces[static_cast<std::size_t>(parent)].parent) {
        const PosePiece& a = pieces[static_cast<std::size_t>(parent)];
        std::int16_t angles[3] = {a.turn[2], a.turn[1], a.turn[0]};  // z, y, x
        if (a.parent < 0) {
            angles[0] = static_cast<std::int16_t>(angles[0] + unit.pitch);
            angles[1] = static_cast<std::int16_t>(angles[1] + unit.heading);
            angles[2] = static_cast<std::int16_t>(angles[2] + unit.bank);
        }
        x87::rotate_point_3d(v, v, angles);
        const auto& an = nodes[static_cast<std::size_t>(a.node)];
        for (int k = 0; k < 3; ++k) v[k] = add(v[k], add(an.offset[static_cast<std::size_t>(k)], a.move[static_cast<std::size_t>(k)]));
    }
    return {v[0], v[1], static_cast<i32>(0u - static_cast<u32>(v[2]))};
}

i32 pack_xz(i32 x, i32 z) {
    return static_cast<i32>((static_cast<u32>(x) & 0xFFFF0000u) + static_cast<u32>(z >> 16));
}

i32 unit_get_value(Unit& self, UnitWorld& world, i32 id, i32 a, i32 b) {
    switch (static_cast<UnitValue>(id)) {
    case UnitValue::Activation: return self.activate_flags & 1;
    case UnitValue::StandingMoveOrders: return static_cast<i32>((self.flags >> 18) & 3);
    case UnitValue::StandingFireOrders: return static_cast<i32>((self.flags >> 20) & 3);
    case UnitValue::Health: {
        // Signed product, unsigned division (a negative hit point count gives a huge value).
        const u32 num = static_cast<u32>(static_cast<i32>(self.hit_points) * 100);
        const u32 den = self.type ? self.type->max_damage : 0;
        return den ? static_cast<i32>(num / den) : 0;  // the original faults on 0
    }
    case UnitValue::InBuildStance: return self.script_flags & 1;
    case UnitValue::Busy: return (self.script_flags >> 1) & 1;
    case UnitValue::PieceXZ: {
        const auto o = piece_local_offset(self, a);
        return pack_xz(add(self.pos[0], o[0]), add(self.pos[2], o[2]));
    }
    case UnitValue::PieceY: return add(self.pos[1], piece_local_offset(self, a)[1]);
    case UnitValue::UnitXZ: {
        const Unit* u = lookup(world, a);
        return u ? pack_xz(u->pos[0], u->pos[2]) : 0;
    }
    case UnitValue::UnitY: {
        const Unit* u = lookup(world, a);
        return u ? u->pos[1] : 0;
    }
    case UnitValue::UnitHeight: {
        const Unit* u = lookup(world, a);
        return u && u->type ? u->type->model_max_y : 0;
    }
    case UnitValue::XZAtan: {
        i32 x, z;
        unpack_xz(a, x, z);
        return static_cast<i32>(static_cast<std::uint16_t>(x87::atan2_scaled(x, z) - self.heading));
    }
    case UnitValue::XZHypot: {
        i32 x, z;
        unpack_xz(a, x, z);
        return x87::ftol32(x87::crt_hypot(static_cast<double>(x), static_cast<double>(z)));
    }
    case UnitValue::Atan: return x87::atan2_scaled(a, b) & 0xFFFF;
    case UnitValue::Hypot: return x87::ftol32(x87::crt_hypot(static_cast<double>(a), static_cast<double>(b)));
    case UnitValue::GroundHeight: {
        i32 x, z;
        unpack_xz(a, x, z);
        const i32 h = world.heightmap().sample(static_cast<std::int16_t>(x >> 16), static_cast<std::int16_t>(z >> 16));
        return static_cast<i32>(static_cast<u32>(h) << 16);
    }
    case UnitValue::BuildPercentLeft:
        if (self.build_fraction_left == 0.0f) return 0;
        // float * -99.0f is exact in double precision; truncated.
        return static_cast<i32>(1u - static_cast<u32>(x87::ftol32(static_cast<double>(self.build_fraction_left) * -99.0)));
    case UnitValue::YardOpen: return (self.script_flags >> 2) & 1;
    case UnitValue::BuggerOff: return (self.script_flags >> 3) & 1;
    case UnitValue::Armored: return (self.activate_flags >> 1) & 1;
    default: return 0;
    }
}

void unit_set_activate(Unit& self, UnitWorld& world, std::uint8_t mask, bool on) {
    const std::uint8_t before = self.activate_flags;
    const std::uint8_t after = on ? static_cast<std::uint8_t>(before | mask) : static_cast<std::uint8_t>(before & ~mask);
    self.activate_flags = after;
    if (after == before) return;
    const std::uint8_t gained = static_cast<std::uint8_t>(after & ~before), lost = static_cast<std::uint8_t>(before & ~after);
    // Scripts start in this order (not run now; they run on the unit's next script pass).
    if (self.script) {
        if (gained & unit_flags::kActivated) self.script->call("Activate", nullptr, false);
        if (lost & unit_flags::kActivated) self.script->call("Deactivate", nullptr, false);
        if (gained & unit_flags::kBuilding) self.script->call("StartBuilding", nullptr, false);
        if (lost & unit_flags::kBuilding) self.script->call("StopBuilding", nullptr, false);
    }
    world.on_activate_flags_changed(self, gained, lost);
}

void unit_set_value(Unit& self, UnitWorld& world, i32 id, i32 value) {
    switch (static_cast<UnitValue>(id)) {
    case UnitValue::Activation: unit_set_activate(self, world, unit_flags::kActivated, value != 0); break;
    case UnitValue::InBuildStance:
        self.script_flags = static_cast<std::uint8_t>((self.script_flags & ~unit_flags::kInBuildStance) | (value & 1));
        break;
    case UnitValue::Busy:
        self.script_flags = static_cast<std::uint8_t>((self.script_flags & ~unit_flags::kBusy) | ((value & 1) << 1));
        break;
    case UnitValue::YardOpen:
        // The occupancy test uses value != 0, the stored flag value & 1 (2 tests "open" but stores closed).
        if (world.yard_cells_free(self, value)) {
            self.script_flags = static_cast<std::uint8_t>((self.script_flags & ~unit_flags::kYardOpen) | ((value & 1) << 2));
            self.flags |= unit_flags::kYardClaimPending;
            world.claim_yard_cells(self);
        }
        break;
    case UnitValue::BuggerOff:
        self.script_flags = static_cast<std::uint8_t>((self.script_flags & ~unit_flags::kBuggerOff) | ((value & 1) << 3));
        break;
    case UnitValue::Armored: unit_set_activate(self, world, unit_flags::kArmored, value != 0); break;
    default: break;  // read-only and unknown ids only mark the unit dirty
    }
    self.net_dirty |= unit_flags::kNetScriptState;
}

void UnitScriptHost::set_piece_move(i32 piece, i32 axis, i32 value) { unit_.pose.set_move(piece, axis, value); }
void UnitScriptHost::set_piece_turn(i32 piece, i32 axis, i32 value) {
    unit_.pose.set_turn(piece, axis, static_cast<std::int16_t>(value));
}
void UnitScriptHost::set_piece_visible(i32 piece, i32 on) { unit_.pose.set_visible(piece, static_cast<u32>(on)); }
void UnitScriptHost::set_piece_cached(i32 piece, i32 on) { unit_.pose.set_cached(piece, static_cast<u32>(on)); }
void UnitScriptHost::set_piece_shaded(i32 piece, i32 on) { unit_.pose.set_shaded(piece, static_cast<u32>(on)); }
i32 UnitScriptHost::get_piece_move(i32 piece, i32 axis) { return unit_.pose.get_move(piece, axis); }
i32 UnitScriptHost::get_piece_turn(i32 piece, i32 axis) { return unit_.pose.get_turn(piece, axis); }
void UnitScriptHost::emit_sfx(i32 piece, i32 type) { world_.emit_sfx(unit_, piece, type); }
void UnitScriptHost::explode_piece(i32 piece, i32 type) { world_.explode_piece(unit_, piece, type); }
void UnitScriptHost::attach_unit(i32 passenger, i32 piece, i32 mode) { world_.attach_unit(unit_, passenger, piece, mode); }
void UnitScriptHost::drop_unit(i32 passenger) { world_.drop_unit(unit_, passenger); }
void UnitScriptHost::set_value(i32 id, i32 value) { unit_set_value(unit_, world_, id, value); }
i32 UnitScriptHost::get_value(i32 id, i32 a, i32 b, i32, i32) { return unit_get_value(unit_, world_, id, a, b); }
i32 UnitScriptHost::get_transporting(i32 unit_id) {
    // 1 if a unit with this id (compared as a full 32-bit value) is in the cargo list.
    for (const Unit* c = unit_.cargo_head; c; c = c->cargo_next)
        if (static_cast<u32>(c->id) == static_cast<u32>(unit_id)) return 1;
    return 0;
}
i32 UnitScriptHost::get_carrier() { return unit_.carrier ? unit_.carrier->id : 0; }
std::uint32_t UnitScriptHost::random_mod(std::uint32_t n) { return world_.random_mod(n); }

} // namespace sim
