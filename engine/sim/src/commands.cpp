// Command input (docs/RE_MEMORY.md, "Commands").

#include "sim/commands.h"

#include <cstring>

#include "sim/battle.h"

namespace sim::command {

namespace {

// The selection gate of the user interface's selecting routines (0x48C7F0 and family): the
// player's own live unit, selectable (flags 0x20), complete, not just transferred, and not
// carried unless its carrier lets carried units be selected.
bool selectable(Battle& b, int player, const Unit& u) {
    const UnitTickState& t = b.ticks[u.id];
    if (t.type_index == 0 || t.owner != player || !(u.flags & unit_flags::kAlive)) return false;
    if (!(u.flags & unit_tick_flags::kSelectable) || u.build_fraction_left != 0.0f || t.transfer_holdoff != 0) return false;
    return !u.carrier || (u.carrier->flags & unit_tick_flags::kCarrierSelectable);
}

// The mission table entry's flags (also a new order's starting flags).
std::uint32_t table_flags(std::uint8_t kind) { return kind < mission::kCount ? mission::default_flags()[kind] : 0; }

}  // namespace

void set_selected(Battle& battle, Unit& unit, bool on) {
    (void)battle;
    if (on) unit.flags |= kSelectedFlag;
    else unit.flags &= ~kSelectedFlag;
}

void clear_selection(Battle& battle, int player) {
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit && id < static_cast<int>(battle.units.size()); ++id)
        if (id > 0) battle.units[static_cast<std::size_t>(id)].flags &= ~kSelectedFlag;
}

bool order_unit(Battle& battle, Unit& unit, std::uint8_t mission, bool queued, Unit* target, const std::array<std::int32_t, 3>* pos,
                std::int32_t param0, std::int32_t param1) {
    return battle.order(unit, mission, queued, target, pos, param0, param1);
}

// 0x43AFC0: a queued order that matches one in the unit's primary list (same kind, the same
// target when one is given, the position within 16 units on x and z when one is given) is
// cancelled instead: unlinked from the list its secondary flag names (if it is there), marked
// "not current" unless it was the primary head, destroyed. Otherwise the order is given.
void issue_or_cancel(Battle& battle, std::uint8_t mission, bool queued, Unit& unit, Unit* target, const std::array<std::int32_t, 3>* pos,
                     std::int32_t param0, std::int32_t param1) {
    if (queued) {
        MissionOrder* head = unit.orders;
        for (MissionOrder* o = head; o; o = o->next) {
            if (o->kind != mission) continue;
            if (target && target != o->target.unit) continue;
            if (pos && (static_cast<std::uint32_t>((*pos)[0] - o->pos[0]) + 0x100000u > 0x200000u ||
                        static_cast<std::uint32_t>((*pos)[2] - o->pos[2]) + 0x100000u > 0x200000u))
                continue;
            MissionOrder** link = (o->flags & order_flags::kSecondary) ? &unit.secondary_orders : &unit.orders;
            while (*link && *link != o) link = &(*link)->next;
            if (!*link) return;
            *link = o->next;
            if (o != head) o->flags |= order_flags::kNotCurrent;
            destroy_order(battle.order_context(), o);
            return;
        }
    }
    battle.order(unit, mission, queued, target, pos, param0, param1);
}

void issue_to_selected(Battle& battle, int player, std::uint8_t intent, std::uint8_t mission, bool queued, Unit* hover,
                       const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1) {
    // 0x48CF30. The unit under the cursor is the target (and is left out) when the intent
    // takes one (all but unload, stop and build) or the mission's table flags say so.
    bool uses_hover;
    if (intent) uses_hover = intent != intent::kUnload && intent != intent::kStop && intent != intent::kBuild;
    else uses_hover = (table_flags(mission) >> 9) & 1u;
    Unit* target = uses_hover ? hover : nullptr;
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    auto in_range = [&](int id) { return id > 0 && id < static_cast<int>(battle.units.size()); };
    // The selection's centre in whole units.
    std::int32_t count = 0, sx = 0, sz = 0;
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (!in_range(id)) continue;
        const Unit& u = battle.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kSelectedFlag) || &u == target) continue;
        ++count;
        sx += static_cast<std::int16_t>(u.pos[0] >> 16);
        sz += static_cast<std::int16_t>(u.pos[2] >> 16);
    }
    if (count == 0) return;
    const std::int32_t cx = static_cast<std::int32_t>(static_cast<std::uint32_t>(sx / count) << 16);
    const std::int32_t cz = static_cast<std::int32_t>(static_cast<std::uint32_t>(sz / count) << 16);
    const std::int32_t radius = count * 3000;
    const std::array<std::int32_t, 3> cursor = pos ? *pos : std::array<std::int32_t, 3>{};
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (!in_range(id)) continue;
        Unit& u = battle.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kSelectedFlag) || &u == target) continue;
        std::uint8_t kind = mission;
        if (intent) kind = resolve_mission(battle.targeting(), intent, u, target, &cursor);
        if (kind == 0) continue;
        const std::uint32_t can_do = battle.types[battle.ticks[static_cast<std::size_t>(id)].type_index].mission.abilities;
        if (kind == mission::kStandingFireOrder && !(can_do & 0x2u)) continue;
        if (kind == mission::kStandingMoveOrder && !(can_do & 0x1u)) continue;
        // Point orders: units close to the centre keep their place in the group.
        std::array<std::int32_t, 3> spread;
        const std::array<std::int32_t, 3>* at = pos;
        if (pos && (table_flags(kind) & 2u)) {
            const std::int64_t dx = static_cast<std::int64_t>(u.pos[0] - cx), dz = static_cast<std::int64_t>(u.pos[2] - cz);
            const std::int32_t d = static_cast<std::int32_t>((dx * dx) >> 32) + static_cast<std::int32_t>((dz * dz) >> 32);
            if (d <= radius) {
                spread = {(*pos)[0] + u.pos[0] - cx, (*pos)[1], (*pos)[2] + u.pos[2] - cz};
                at = &spread;
            }
        }
        issue_or_cancel(battle, kind, queued, u, target, at, param0, param1);
    }
}

void issue_to_group(Battle& battle, int player, std::int32_t group, std::uint8_t intent, bool queued, Unit* target,
                    const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1) {
    // 0x480460: every unit of the player (not an empty entry) in group `group`.
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (id <= 0 || id >= static_cast<int>(battle.units.size())) continue;
        Unit& u = battle.units[static_cast<std::size_t>(id)];
        if (battle.ticks[static_cast<std::size_t>(id)].type_index == 0 || static_cast<std::int32_t>(u.group) != group) continue;
        const std::uint8_t kind = resolve_mission(battle.targeting(), intent, u, target, pos);
        battle.order(u, kind, queued, target, pos, param0, param1);
    }
}

namespace {
Unit* unit_or_null(Battle& b, std::uint16_t id) { return id && id < b.units.size() ? &b.units[id] : nullptr; }
}  // namespace

bool apply(Battle& battle, const Command& c) {
    if (c.player >= 10) return false;
    const std::array<std::int32_t, 3>* pos = c.has_pos ? &c.pos : nullptr;
    auto owned = [&](std::uint16_t id) -> Unit* {
        Unit* u = unit_or_null(battle, id);
        if (!u) return nullptr;
        const PlayerSlot& p = battle.players[c.player];
        return id >= p.first_unit && id <= p.last_unit ? u : nullptr;
    };
    switch (c.kind) {
    case Kind::Select:
        // A plain selection clears every unit's selection and camera-visit bits first; with
        // shift each unit's selection toggles (0x48C7F0, 0x48C390).
        if (!c.queued())
            for (std::size_t i = 1; i < battle.units.size(); ++i) battle.units[i].flags &= ~0xD0u;
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id); u && selectable(battle, c.player, *u)) {
                if (c.queued()) u->flags ^= kSelectedFlag;
                else u->flags |= kSelectedFlag;
            }
        return true;
    case Kind::Deselect:
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id)) set_selected(battle, *u, false);
        return true;
    case Kind::ClearSelection: clear_selection(battle, c.player); return true;
    case Kind::IssueIntent:
        issue_to_selected(battle, c.player, c.intent, 0, c.queued(), unit_or_null(battle, c.target), pos, c.param0, c.param1);
        return true;
    case Kind::IssueMission:
        issue_to_selected(battle, c.player, 0, c.mission, c.queued(), unit_or_null(battle, c.target), pos, c.param0, c.param1);
        return true;
    case Kind::OrderUnits: {
        bool any = false;
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id)) any |= order_unit(battle, *u, c.mission, c.queued(), unit_or_null(battle, c.target), pos, c.param0, c.param1);
        return any;
    }
    case Kind::BuildCount:
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id)) battle.order_build_count(*u, c.unit_type, c.param0);
        return true;
    case Kind::MobileBuild: {
        bool any = false;
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id)) any |= battle.order_mobile_build(*u, c.unit_type, c.pos, c.queued());
        return any;
    }
    case Kind::AssignGroup:
        for (int id = battle.players[c.player].first_unit; id <= battle.players[c.player].last_unit; ++id)
            if (id > 0 && id < static_cast<int>(battle.units.size()) && (battle.units[static_cast<std::size_t>(id)].flags & kSelectedFlag))
                battle.units[static_cast<std::size_t>(id)].group = static_cast<std::uint32_t>(c.param0);
        return true;
    case Kind::SelectGroup:
        for (std::size_t i = 1; i < battle.units.size(); ++i) battle.units[i].flags &= ~0xD0u;
        for (int id = battle.players[c.player].first_unit; id <= battle.players[c.player].last_unit; ++id)
            if (id > 0 && id < static_cast<int>(battle.units.size())) {
                Unit& u = battle.units[static_cast<std::size_t>(id)];
                if (static_cast<std::int32_t>(u.group) == c.param0 && selectable(battle, c.player, u)) u.flags |= kSelectedFlag;
            }
        return true;
    case Kind::TransferEnergy:
        if (c.param0 < 0 || c.param0 >= 10) return false;
        return transfer_energy(battle.economy(), c.player, c.param0, c.amount, true) != 0.0f;
    case Kind::TransferMetal:
        if (c.param0 < 0 || c.param0 >= 10) return false;
        return transfer_metal(battle.economy(), c.player, c.param0, c.amount, true) != 0.0f;
    case Kind::None: break;
    }
    return false;
}

namespace {
void put8(std::vector<std::uint8_t>& o, std::uint32_t v) { o.push_back(static_cast<std::uint8_t>(v)); }
void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
    put8(o, v);
    put8(o, v >> 8);
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    put16(o, v);
    put16(o, v >> 16);
}
struct Reader {
    const std::vector<std::uint8_t>& in;
    std::size_t& at;
    bool ok = true;
    std::uint32_t get(int bytes) {
        std::uint32_t v = 0;
        if (at + static_cast<std::size_t>(bytes) > in.size()) {
            ok = false;
            return 0;
        }
        for (int k = 0; k < bytes; ++k) v |= static_cast<std::uint32_t>(in[at++]) << (8 * k);
        return v;
    }
};
}  // namespace

void encode(const Command& c, std::vector<std::uint8_t>& out) {
    put8(out, static_cast<std::uint8_t>(c.kind));
    put8(out, c.player);
    put8(out, c.flags | (c.has_pos ? 0x80 : 0));
    put8(out, c.intent);
    put8(out, c.mission);
    put16(out, c.target);
    put16(out, c.unit_type);
    for (std::int32_t v : c.pos) put32(out, static_cast<std::uint32_t>(v));
    put32(out, static_cast<std::uint32_t>(c.param0));
    put32(out, static_cast<std::uint32_t>(c.param1));
    std::uint32_t bits = 0;
    static_assert(sizeof(float) == 4);
    std::memcpy(&bits, &c.amount, 4);
    put32(out, bits);
    put16(out, static_cast<std::uint32_t>(c.units.size()));
    for (std::uint16_t id : c.units) put16(out, id);
}

bool decode(const std::vector<std::uint8_t>& in, std::size_t& at, Command& c) {
    Reader r{in, at};
    c = Command{};
    c.kind = static_cast<Kind>(r.get(1));
    c.player = static_cast<std::uint8_t>(r.get(1));
    const std::uint8_t f = static_cast<std::uint8_t>(r.get(1));
    c.flags = f & 0x7F;
    c.has_pos = (f & 0x80) != 0;
    c.intent = static_cast<std::uint8_t>(r.get(1));
    c.mission = static_cast<std::uint8_t>(r.get(1));
    c.target = static_cast<std::uint16_t>(r.get(2));
    c.unit_type = static_cast<std::uint16_t>(r.get(2));
    for (std::int32_t& v : c.pos) v = static_cast<std::int32_t>(r.get(4));
    c.param0 = static_cast<std::int32_t>(r.get(4));
    c.param1 = static_cast<std::int32_t>(r.get(4));
    const std::uint32_t bits = r.get(4);
    std::memcpy(&c.amount, &bits, 4);
    const std::uint32_t n = r.get(2);
    for (std::uint32_t k = 0; k < n && r.ok; ++k) c.units.push_back(static_cast<std::uint16_t>(r.get(2)));
    return r.ok;
}

}  // namespace sim::command
