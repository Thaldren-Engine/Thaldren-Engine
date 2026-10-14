#include "ta/battle_types.h"

#include "ta/model3do.h"

namespace ta {
namespace {

using u16 = std::uint16_t;

const sim::WeaponType* weapon_at(const std::vector<sim::WeaponType>& weapons, int slot) {
    return slot >= 0 && static_cast<std::size_t>(slot) < weapons.size() ? &weapons[static_cast<std::size_t>(slot)] : nullptr;
}

} // namespace

sim::WeaponType to_weapon_type(const WeaponDefRecord& d) {
    sim::WeaponType w;
    w.index = d.index;
    w.flags = d.flags;
    switch (d.fire) {
    case WeaponFireFn::Turret: w.fire = sim::WeaponFireRoutine::Turret; break;
    case WeaponFireFn::VLaunch: w.fire = sim::WeaponFireRoutine::VerticalLaunch; break;
    case WeaponFireFn::LineOfSight: w.fire = sim::WeaponFireRoutine::LineOfSight; break;
    case WeaponFireFn::Dropped: w.fire = sim::WeaponFireRoutine::Dropped; break;
    default: w.fire = sim::WeaponFireRoutine::None; break;
    }
    w.velocity = d.weapon_velocity;
    w.start_velocity = d.start_velocity;
    w.acceleration = d.weapon_acceleration;
    w.range = d.range;
    w.energy_per_shot = d.energy_per_shot;
    w.metal_per_shot = d.metal_per_shot;
    w.min_barrel_angle = d.min_barrel_angle;
    w.reload_time = static_cast<u16>(d.reload_time);
    w.tolerance = static_cast<u16>(d.tolerance);
    w.pitch_tolerance = static_cast<u16>(d.pitch_tolerance);
    w.accuracy = static_cast<u16>(d.accuracy);
    w.coverage = d.coverage;
    w.weapon_timer = static_cast<u16>(d.weapon_timer);
    w.turn_rate = static_cast<u16>(d.turn_rate);
    w.burst = static_cast<u16>(d.burst);
    w.burst_rate = static_cast<u16>(d.burst_rate);
    w.spray_angle = static_cast<u16>(d.spray_angle);
    w.duration = static_cast<u16>(d.duration);
    w.random_decay = static_cast<u16>(d.random_decay);
    w.smoke_delay = static_cast<u16>(d.smoke_delay);
    w.flight_time = static_cast<u16>(d.flight_time);
    w.hold_time = static_cast<u16>(d.hold_time);
    w.area_of_effect = static_cast<u16>(d.area_of_effect);
    w.default_damage = static_cast<u16>(d.default_damage);
    w.edge_effectiveness = d.edge_effectiveness;
    w.shake_magnitude = d.shake_magnitude;
    w.shake_duration = d.shake_duration;
    if (d.has_damage_map) w.damage = d.damage_overrides;
    return w;
}

std::vector<sim::WeaponType> to_weapon_types(const WeaponDefTable& defs) {
    std::vector<sim::WeaponType> out;
    out.reserve(defs.slots.size());
    for (const WeaponDefRecord& d : defs.slots) out.push_back(to_weapon_type(d));
    return out;
}

bool load_unit_assets(Vfs& vfs, const UnitDefRecord& d, UnitTypeAssets& out, std::string& error) {
    out = UnitTypeAssets{};
    if (!vfs.read("objects3d\\" + d.object_name + ".3DO", out.model_file)) {
        error = "no model objects3d\\" + d.object_name + ".3DO";
        return false;
    }
    const Model3doLoadResult m = load_3do(out.model_file, true);
    if (!m.root) {
        error = "model: " + m.error;
        return false;
    }
    out.has_model = true;
    out.pose_model = to_pose_model(m.root.get());
    out.has_footprint = selection_footprint(out.model_file, m.root.get(), out.footprint);
    if (vfs.read("scripts\\" + d.unit_name + ".COB", out.cob_file)) {
        std::string cob_error;
        out.has_script = load_cob(out.cob_file, out.cob, cob_error);
        if (!out.has_script) {
            error = "script: " + cob_error;
            return false;
        }
    }
    return true;
}

std::array<sim::MoveClass, sim::kMoveClasses> to_move_classes(const UnitDefContext& ctx) {
    std::array<sim::MoveClass, sim::kMoveClasses> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const UnitDefContext::MoveInfo& m = ctx.movement_classes[i];
        sim::MoveClass& c = out[i];
        c.named = !ctx.movement_class_names[i].empty();
        c.footprint_x = m.footprint_x;
        c.footprint_z = m.footprint_z;
        c.max_water_depth = m.max_water_depth;
        c.min_water_depth = m.min_water_depth;
        c.max_slope = m.max_slope;
        c.bad_slope = m.bad_slope;
        c.max_water_slope = m.max_water_slope;
        c.bad_water_slope = m.bad_water_slope;
    }
    return out;
}

void to_battle_unit_type(const UnitDefRecord& d, const UnitTypeAssets& assets, const std::vector<sim::WeaponType>& weapons,
                         sim::BattleUnitType& out) {
    sim::UnitTypeInfo& info = out.info;
    info.max_damage = d.max_damage;
    info.model_min_x = d.model_min_x;
    info.model_min_y = d.model_min_y;
    info.model_min_z = d.model_min_z;
    info.model_max_x = d.model_max_x;
    info.model_max_y = d.model_max_y;
    info.model_max_z = d.model_max_z;
    info.explode_as = weapon_at(weapons, d.explode_as);
    info.self_destruct_as = weapon_at(weapons, d.self_destruct_as);
    info.yard_map = d.has_yard_map ? d.yard_map : std::vector<std::uint8_t>{};
    info.corpse = static_cast<std::uint16_t>(d.corpse_feature_id);
    info.is_feature = (d.type_flags & (1u << 24)) != 0;

    sim::UnitTickTypeInfo& tick = out.tick;
    tick.wind_generator = d.wind_generator;
    tick.heal_time = static_cast<u16>(d.heal_time);
    tick.type_flags = d.type_flags;
    tick.damage_modifier = d.damage_modifier;
    tick.build_time = d.build_time;
    tick.build_cost_energy = d.build_cost_energy;
    tick.build_cost_metal = d.build_cost_metal;
    tick.name = d.unit_name;
    tick.default_mission = d.default_mission;

    sim::MotionType& m = out.motion;
    m.type_flags = d.type_flags;
    m.max_velocity = d.max_velocity;
    m.brake_rate = d.brake_rate;
    m.acceleration = d.acceleration;
    m.bank_scale = d.bank_scale;
    m.pitch_scale = d.pitch_scale;
    m.move_rate1 = d.move_rate1;
    m.move_rate2 = d.move_rate2;
    m.turn_rate = d.turn_rate;
    m.cruise_altitude = d.cruise_alt;
    m.waterline = d.waterline;
    m.model_height = static_cast<std::int16_t>(static_cast<std::uint32_t>(d.model_max_y) >> 16);
    m.has_move_class = d.movement_class >= 0;

    sim::RouteType& route = out.route;
    route.move_class = static_cast<std::int8_t>(d.movement_class);
    route.footprint_x = d.footprint_x;
    route.footprint_z = d.footprint_z;
    route.movement = d.bm_code;
    route.max_slope = d.max_slope;
    route.max_water_slope = d.max_water_slope;
    route.max_water_depth = d.max_water_depth;
    route.min_water_depth = d.min_water_depth;

    sim::EconomyType& eco = out.economy;
    eco.energy_make = d.energy_make;
    eco.energy_use = d.energy_use;
    eco.metal_make = d.metal_make;
    eco.extracts_metal = d.extracts_metal;
    eco.makes_metal = d.makes_metal;
    eco.wind_generator = d.wind_generator;
    eco.tidal_generator = d.tidal_generator;
    eco.cloak_cost = d.cloak_cost;
    eco.cloak_cost_moving = d.cloak_cost_moving;
    eco.energy_storage = d.energy_storage;
    eco.metal_storage = d.metal_storage;

    out.mission.abilities = d.can_do_flags;
    out.mission.worker_time = static_cast<std::uint16_t>(d.worker_time);

    sim::GroundSettleType& settle = out.settle;
    settle.type_flags = d.type_flags;
    settle.waterline = d.waterline;
    settle.max_velocity = d.max_velocity;
    settle.has_footprint = assets.has_footprint;
    settle.footprint = assets.footprint;

    sim::UnitCreateType& c = out.create;
    c.type_flags = d.type_flags;
    c.max_per_player = d.max_this_unit;
    c.movement = d.bm_code;
    c.build_menu_pages = d.build_menu_page_count;
    c.footprint_x = d.footprint_x;
    c.footprint_z = d.footprint_z;
    c.build_angle = static_cast<u16>(d.build_angle);
    c.extracts_metal = d.extracts_metal;
    c.weapons = {weapon_at(weapons, d.weapon1), weapon_at(weapons, d.weapon2), weapon_at(weapons, d.weapon3)};
    c.model = assets.has_model ? &assets.pose_model : nullptr;
    c.script = assets.has_script ? &assets.cob.program : nullptr;
}

void to_targeting_type(const UnitDefRecord& d, const CategoryRegistry& categories, const std::vector<sim::WeaponType>& weapons,
                       sim::TargetingTypeInfo& out) {
    auto mask = [&](const std::string& name) {
        const auto* m = categories.find(name);
        return m ? *m : sim::CategoryMask{};
    };
    out.category_bit = d.index;
    out.bad_target = {mask(d.wpri_bad_target), mask(d.wsec_bad_target), mask(d.wspe_bad_target)};
    out.no_chase = mask(d.no_chase);
    out.sight_distance = d.sight_distance;
    out.maneuver_leash = d.maneuver_leash_length;
    out.max_water_depth = d.max_water_depth;
    out.min_water_depth = d.min_water_depth;
    out.footprint_x = d.footprint_x;
    out.transport_size = d.transport_size;
    out.transport_capacity = d.transport_capacity;
    out.has_build_options = d.has_build_options;
    out.first_weapon = weapon_at(weapons, d.weapon1);
}

} // namespace ta
