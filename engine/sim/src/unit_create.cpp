#include "sim/unit_create.h"

#include "sim/battle.h"
#include "sim/weapons.h"
#include "sim/x87.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
namespace cf = create_flags;

void put16(std::uint8_t* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}
void put32(std::uint8_t* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

void run_named(Unit& unit, const char* name, i32 arg) {
    if (unit.script) unit.script->call_with_args(unit.script->program().find_function(name), nullptr, false, 1, arg, 0, 0, 0);
}

} // namespace

void setup_weapon_slots(Unit& unit, const std::array<const WeaponType*, 3>& weapons) {
    u32 longest = 0;
    for (int i = 0; i < 3; ++i) {
        WeaponSlot& s = unit.weapons[static_cast<std::size_t>(i)];
        s.reload = 0;
        s.type = weapons[static_cast<std::size_t>(i)];
        s.stockpile = 0;
        const bool enabled = s.type && s.type->index != 0;
        s.flags = static_cast<u8>((s.flags & 0xF0) | (i << 2) | slot_flags::kReleased | (enabled ? slot_flags::kEnabled : 0));
        // Height of the Query piece above the AimFrom piece, scaled by 1.25 (x87, truncated).
        const WorldPos query = weapon_piece_position(unit, i, -1);
        const WorldPos from = aim_from_position(unit, i);
        const i32 dy = static_cast<i32>(static_cast<u32>(query[2]) - static_cast<u32>(from[2]));
        s.aux = x87::ftol32(static_cast<double>(dy) * 1.25);
        const u32 reload = s.type ? s.type->reload_time : 0u;  // the original has no null weapon
        if (reload > longest) longest = reload;
    }
    // Milliseconds at 30 steps per second (signed division of a value that is never negative).
    run_named(unit, "SetMaxReloadTime", static_cast<i32>(longest * 1000u) / 30);
}

void update_metal_extraction(Unit& unit, const MapGrid& map, float extracts_metal) {
    if (!(extracts_metal > 0.0f)) return;
    const i16 fx = unit.footprint_x, fz = unit.footprint_z;
    u16 sum = 0;
    for (i32 z = unit.cell_z; z < unit.cell_z + fz; ++z)
        for (i32 x = unit.cell_x; x < unit.cell_x + fx; ++x)
            if (x >= 0 && x < map.width && z >= 0 && z < map.height)
                sum = static_cast<u16>(sum + map.cells[static_cast<std::size_t>(z * map.width + x)].metal + 1);
    // The sum sits in the high half of a 32-bit word that is loaded as a signed integer
    // and scaled back by 1/65536: a signed 16-bit count.
    const double count = static_cast<double>(static_cast<i32>(static_cast<u32>(sum) << 16));
    unit.metal_extraction = static_cast<float>(static_cast<double>(extracts_metal) * count * (1.0 / 65536.0));
    run_named(unit, "SetSpeed", static_cast<i16>(sum));
}

void Battle::init_unit_table(std::size_t count) {
    units.clear();
    units.resize(count);
    ticks.assign(count, UnitTickState{});
    script_hosts_.clear();
    script_hosts_.resize(count);
    for (std::size_t i = 0; i < count; ++i) units[i].id = static_cast<u16>(i);
}

void Battle::assign_unit_range(int player, int first, int last) {
    PlayerSlot& p = players[static_cast<std::size_t>(player)];
    p.first_unit = first;
    p.last_unit = last;
    for (int i = first; i <= last && i >= 0 && static_cast<std::size_t>(i) < ticks.size(); ++i) ticks[static_cast<std::size_t>(i)].owner = player;
}

void Battle::init_unit_fields(Unit& u, UnitTickState& t, const BattleUnitType& type, i32 x, i32 y, i32 z, bool fully_built) {
    const UnitCreateType& ct = type.create;
    const u32 tf = ct.type_flags;
    u.type = &type.info;
    t.tick_type = &type.tick;
    u32 f = u.flags | unit_flags::kAlive;
    f = (f & 0xDFFFBFFFu) | (ct.movement == 0 ? footprint_flags::kStructure : 0u);
    u.footprint_x = ct.footprint_x;
    u.footprint_z = ct.footprint_z;
    f = (f & 0x7FFFFFFFu) | ((tf & 0xFFFF0000u) << 15);  // bit 31 = type bit 16 (has weapons)
    u.draw_flags = (u.draw_flags & ~1u) | ((tf >> 7) & 1u);
    f = (f & 0xBFFFFFFFu) | ((tf & cf::kAirBase) << 21);
    if (fully_built) {
        u.build_fraction_left = 0.0f;
        u.hit_points = static_cast<i16>(type.info.max_damage);
    } else {
        u.build_fraction_left = 1.0f;
        u.unbuilt_word = 0;
        u.hit_points = 0;
    }
    u.script_flags = static_cast<u8>(u.script_flags & 0xF0);
    t.health_percent = 0;
    f = (f & 0xFFFDF3E1u) | 0x10021u;  // selectable, resettle, ground layer
    u.pos = {x, y, z};
    t.prev_health_percent = 0;
    u.activate_flags = 0;
    u.cloak_holdoff_tick = 0;
    u.bank = 0;
    footprint_origin(x, z, u.footprint_x, u.footprint_z, u.cell_x, u.cell_z);
    // Facing: the build angle's spread around south.
    const u32 r = rng_.next_mod(ct.build_angle);
    u.pitch = 0;
    u.heading = static_cast<u16>(r + 0x8000u - (ct.build_angle >> 1));
    u.los_cell_x = 0;
    u.los_cell_z = 0;
    t.damaged_timer = 0;
    t.transfer_holdoff = 0;
    const PlayerSlot& owner = players[static_cast<std::size_t>(t.owner)];
    f = (f & ~0x300u) | (owner.state == settings.local_view ? 0x200u : 0u);
    u.flags = f;
    for (int i = 0; i < 3; ++i) {
        u.weapons[static_cast<std::size_t>(i)].target_x = 0;
        u.weapons[static_cast<std::size_t>(i)].target_z = kUnitTarget;
        sim::release_weapon_targets(u, i);
    }
    u.net_dirty = 0;
    t.kill_count = 0;
    t.last_attacker = nullptr;
    t.last_attacker_slot = 10;
    f = u.flags;
    f = (f & 0xFFF3FFFFu) | ((tf & cf::kStandingMoveMask) << 18);
    f = (f & 0xFFCFFFFFu) | ((tf & cf::kStandingFireMask) << 18);
    f = (f & 0xF3FFF7FFu) | ((tf & cf::kInitCloaked) << 7);
    f = ct.build_menu_pages > 1 ? ((f & 0xFCFFFFFFu) | 0x00C00000u) : (f & 0xFC3FFFFFu);
    u.flags = f;
    u.los_frame = 0;
    // The resource slot starts empty (its player is the entry's owner).
    t.metal_make = 0.0f;
    t.energy_use = 0.0f;
    t.metal_use = 0.0f;
    t.energy_make = t.energy_use_paid = t.energy_stall = t.energy_make_prev = t.energy_use_prev = 0.0f;
    t.metal_use_paid = t.metal_stall = t.metal_make_prev = t.metal_use_prev = 0.0f;
    u.transport_piece = 0xFF;
    u.hover_phase = static_cast<u16>(rng_.next_mod(0x10000));
    outside_.join_group(u, 0);
}

void Battle::build_script_and_pose(Unit& u, const UnitCreateType& ct) {
    static const std::vector<std::string> kNoPieces;
    if (ct.model) u.pose.build(*ct.model, ct.script ? ct.script->piece_names : kNoPieces);
    else u.pose = PoseState{};
    if (!ct.script) {
        u.script.reset();
        return;
    }
    if (script_hosts_.size() < units.size()) script_hosts_.resize(units.size());
    auto& host = script_hosts_[u.id];
    UnitWorld& world = *this;
    if (!host) host = std::make_unique<UnitScriptHost>(u, world);
    u.script = std::make_unique<ScriptInstance>(*ct.script, *host, settings.script_tick_rate);
    u.script->call("Create", nullptr, true);
}

Unit* Battle::create_unit(int player, std::uint16_t type, std::int32_t x, std::int32_t y, std::int32_t z, bool fully_built,
                          std::uint32_t layer, std::uint16_t wanted_id) {
    sync_settings();
    player &= 0xFF;
    if (type == 0 || type >= types.size() || static_cast<std::size_t>(player) >= players.size()) return nullptr;
    const BattleUnitType& bt = types[type];
    const UnitCreateType& ct = bt.create;
    if (!(ct.type_flags & cf::kLoadable)) return nullptr;
    PlayerSlot& p = players[static_cast<std::size_t>(player)];
    const i32 first = p.first_unit, last = p.last_unit;
    auto in_table = [&](i32 id) { return id >= 0 && static_cast<std::size_t>(id) < units.size(); };
    if (ct.max_per_player != -1) {
        i32 count = 0;
        for (i32 id = first; id <= last && in_table(id); ++id)
            if (ticks[static_cast<std::size_t>(id)].type_index == type) ++count;
        if (count >= ct.max_per_player) return nullptr;
    }
    if (first > last) return nullptr;
    i32 id = first;
    for (;;) {
        if (wanted_id != 0) {
            id = wanted_id;
            if (id < first || id > last) return nullptr;
        }
        if (!in_table(id)) return nullptr;
        if (ticks[static_cast<std::size_t>(id)].type_index == 0) break;
        if (wanted_id != 0) return nullptr;
        if (++id > last) return nullptr;
    }
    Unit& u = units[static_cast<std::size_t>(id)];
    UnitTickState& t = ticks[static_cast<std::size_t>(id)];
    u.id = static_cast<u16>(id);
    t.type_index = type;
    init_unit_fields(u, t, bt, x, y, z, fully_built);
    build_script_and_pose(u, ct);
    setup_weapon_slots(u, ct.weapons);
    update_metal_extraction(u, map, ct.extracts_metal);
    if (ct.movement == 1) {
        t.has_motion = true;
        sim::create_motion(motion_, u);
        u.heading = ct.build_angle;  // the random facing is replaced (its draw still happened)
    }
    u.flags = (u.flags & ~3u) | (layer & 3u);
    std::uint32_t settle_word = 0;  // creation runs deeper than the unit pass's stale word
    settle_height(u, settle_word);
    claim_footprint(footprint_, u);
    const std::int32_t net = players[static_cast<std::size_t>(t.owner)].net_id;
    {
        std::uint8_t pk[kUnitCreatedPacketSize] = {0x09};
        put16(pk + 1, type);
        put16(pk + 3, u.id);
        put32(pk + 5, static_cast<u32>(u.pos[0]));
        put32(pk + 9, static_cast<u32>(u.pos[1]));
        put32(pk + 13, static_cast<u32>(u.pos[2]));
        put16(pk + 17, static_cast<u16>(u.pitch));
        put16(pk + 19, u.heading);
        put16(pk + 21, static_cast<u16>(u.bank));
        outside_.network_send(net, pk, kUnitCreatedPacketSize);
    }
    if (fully_built) {
        if (ct.movement == 0) {
            std::uint8_t pk[kUnitFinishedPacketSize] = {0x12};
            put16(pk + 1, u.id);
            put16(pk + 3, u.id);
            outside_.network_send(net, pk, kUnitFinishedPacketSize);
        }
        // The flags of the unit's own type (the same as ct's).
        if (ct.type_flags & cf::kActivateWhenBuilt) unit_set_activate(u, *this, unit_flags::kActivated, true);
        if (ct.type_flags & cf::kIsFeature) {
            t.last_damage_kind = 7;
            u.flags |= unit_tick_flags::kDying;
        }
    }
    stamp_line_of_sight(u);
    ++p.alive_count;
    ++p.units_created;
    outside_.victory_unit_created(u);
    return &u;
}

} // namespace sim
