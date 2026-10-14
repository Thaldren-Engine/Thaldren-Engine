#pragma once
// A BattleOutside that does nothing: no motion, no mission behaviours (they
// report "continue"), energy always paid, no presentation, network or user
// interface. Tools and tests derive from it and override what they need.

#include "sim/battle.h"

namespace sim {

class QuietBattleOutside : public BattleOutside {
public:
    // Moves are always allowed (the placement check is not built yet); flat buckets.
    bool can_place(Unit&, std::int16_t, std::int16_t, std::uint32_t) override { return true; }
    std::uint8_t bucket_height(const Unit&) override { return 0; }
    void route_search_forget(MotionGoal&) override {}
    void update_sight(Unit&) override {}
    void enemies_in_radius(int, const std::array<std::int32_t, 3>&, std::int32_t, std::vector<Unit*>&) override {}
    Unit* fire_at_will_target(Unit&) override { return nullptr; }
    bool issue_attack_order(Unit&, Unit&, bool) override { return false; }
    std::uint8_t resolve_mission(OrderIntent, Unit&, Unit*, const std::array<std::int32_t, 3>*) override { return 0; }
    void unfinished_decay(Unit&, std::int32_t) override {}
    bool use_resources_if_not_stalled(Unit&, float, float) override { return true; }
    void speech(Unit&, std::uint32_t, SpeechText) override {}
    void display_changed(Unit&) override {}
    void build_counts_changed(Unit&) override {}
    void nano_beam(const std::array<std::int32_t, 3>&, const std::array<std::int32_t, 6>&) override {}
    std::int32_t run_mission(Unit&, MissionOrder&, std::uint32_t&) override { return mission_result::kContinue; }
    void retarget(Unit*, Unit&, std::uint16_t) override {}
    void attach_unit(Unit&, std::int32_t, std::int32_t, std::int32_t) override {}
    void drop_unit(Unit&, std::int32_t) override {}
    void detach_from_carrier(Unit&) override {}
    void update_passability(std::int16_t, std::int16_t, std::int16_t, std::int16_t) override {}
    void passability_after_release(Unit&) override {}
    void join_group(Unit&, std::int32_t) override {}
    void stamp_line_of_sight(Unit&) override {}
    void victory_unit_created(Unit&) override {}
    void release_unit(Unit&) override {}
    bool pay_energy(Unit&, float) override { return true; }
    void damage_feature(MapCell&, std::int32_t, std::int32_t, const WeaponType&) override {}
    Vec3i feature_anim_pos(std::uint16_t) override { return {}; }
    void spawn_wreck(Unit&, std::int32_t, bool) override {}
    void explode_piece(Unit&, std::int32_t, std::int32_t) override {}
    void emit_sfx(Unit&, std::int32_t, std::int32_t) override {}
    void reveal_dead_unit(Unit&) override {}
    void forget_fog(Unit&) override {}
    void victory_unit_destroyed(Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int) override {}
    std::uint32_t clock_ticks() override { return 0; }
    void network_send(std::int32_t, const std::uint8_t*, std::int32_t) override {}
    void broadcast_player_units(int) override {}
    void broadcast_script_call(Unit&, std::int16_t) override {}
    void on_activate_flags_changed(Unit&, std::uint8_t, std::uint8_t) override {}
    void forget_speech(Unit&) override {}
    void leave_group(Unit&) override {}
    void stats_activity(std::int32_t) override {}
    void kill_counted(std::uint8_t, Unit&) override {}
    void stockpile_used(Unit&) override {}
    void camera_follow_step() override {}
    void play_sound(std::uint16_t, const Vec3i&) override {}
    void spawn_smoke(const Vec3i&) override {}
    void spawn_muzzle_smoke(const Vec3i&) override {}
    void spawn_explosion(const Vec3i&, const WeaponType&, bool, bool) override {}
    void camera_shake(std::int32_t, std::int32_t, std::int32_t) override {}
};

} // namespace sim
