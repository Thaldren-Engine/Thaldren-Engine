// Command input (docs/RE_MEMORY.md, "Commands").

#include "sim/commands.h"

#include <cstring>

#include "sim/battle.h"

namespace sim::command {

void set_selected(Battle& battle, Unit& unit, bool on) {
    (void)battle;
    if (on) unit.flags |= kSelectedFlag;
    else unit.flags &= ~kSelectedFlag;
}

void clear_selection(Battle& battle, int player) {
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) battle.units[static_cast<std::size_t>(id)].flags &= ~kSelectedFlag;
}

bool order_unit(Battle& battle, Unit& unit, std::uint8_t mission, bool queued, Unit* target, const std::array<std::int32_t, 3>* pos,
                std::int32_t param0, std::int32_t param1) {
    return battle.order(unit, mission, queued, target, pos, param0, param1);
}

void issue_to_selected(Battle& battle, int player, std::uint8_t intent, std::uint8_t mission, bool queued, Unit* hover,
                       const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1) {
    // Provisional: refined to the original's routine in a later commit.
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        Unit& u = battle.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kSelectedFlag) || &u == hover) continue;
        std::uint8_t kind = mission;
        if (intent) kind = resolve_mission(battle.targeting(), intent, u, hover, pos);
        battle.order(u, kind, queued, hover, pos, param0, param1);
    }
}

void issue_to_group(Battle& battle, int player, std::int32_t group, std::uint8_t intent, bool queued, Unit* target,
                    const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1) {
    const PlayerSlot& p = battle.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
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
        if (!c.queued()) clear_selection(battle, c.player);
        for (std::uint16_t id : c.units)
            if (Unit* u = owned(id)) set_selected(battle, *u, true);
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
    case Kind::SelectGroup:
    case Kind::TransferEnergy:
    case Kind::TransferMetal:
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
