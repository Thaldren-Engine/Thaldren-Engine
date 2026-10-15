#pragma once
// One machine's side of a network battle (docs/RE_MEMORY.md, "Network and sync").
//
// The original is not lockstep: every machine runs the whole step for its own players'
// units (controller 1 human, 2 computer) and announces what happens to them as packets;
// the units of players on other machines (controller 3) only run their scripts here and
// follow those packets: creation, the motion goals and a rolling full state (0x2C), damage,
// deaths, shots, script calls, activation, construction, ownership, transport. Each machine
// draws its own random numbers, so effects only the owner decides (hits, damage, deaths) are
// decided once, on the owner's machine, and everyone else copies them.
//
// NetBattle sits between the battle and the rest of the client: it is the battle's
// BattleOutside, forwarding everything that is not network to the client's own outside,
// and turns the battle's network hooks into packets, frames and messages.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/frames.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/battle.h"

namespace net {

struct NetCounters {
    std::uint64_t packets_sent = 0, packets_received = 0, refused = 0, state_packets = 0, chat = 0;
    std::array<std::uint64_t, 0x2D> by_type{};
};

class NetBattle final : public sim::BattleOutside {
public:
    NetBattle(sim::BattleOutside& inner, Transport& transport, NetTiming timing = {});
    // The battle this machine runs (after it is built; before its first step).
    void attach(sim::Battle& battle);
    sim::Battle* battle() { return battle_; }

    // The original's real-time clock in 1/30 s (an input: the client's frame clock).
    void set_clock(std::uint32_t ticks) { clock_ = ticks; }
    std::uint32_t clock() const { return clock_; }
    void set_timing(const NetTiming& t) { sender_.set_timing(t); }

    // The game phase packets are accepted in (Phase::Battle while the battle runs).
    void set_phase(Phase p) { phase_ = p; }

    // Chat (packet 5): a line from the local player to everybody; received lines go to on_chat.
    void send_chat(const std::string& text);
    std::function<void(int slot, const std::string& text)> on_chat;
    // Pause and game speed (packet 0x19).
    void send_pause(bool paused);
    void send_speed(int speed);
    bool paused() const { return paused_; }
    int speed() const { return speed_; }
    std::function<void(bool)> on_pause;
    std::function<void(int)> on_speed;
    // Alliance change (packet 0x23) from local player `from` to player `to`.
    void send_alliance(int from, int to, bool allied);
    // A player left (packet 0x1B); `reason` as the original's status codes.
    std::function<void(int slot, int reason)> on_player_left;

    // Each remote player's last unit state step (the record's +0x18) and how far the slowest
    // is behind this machine (the step budget slows down when it is 900 steps or more).
    std::uint32_t last_state_tick(int slot) const { return last_state_tick_[static_cast<std::size_t>(slot)]; }
    std::int32_t remote_lag() const;
    // A remote player's alliance answers (the local record's "allied by" bytes, +0x113).
    std::uint8_t allied_by(int local, int remote) const { return allied_by_[static_cast<std::size_t>(local)][static_cast<std::size_t>(remote)]; }
    // Totals other machines announced for their players (packet 0x28).
    struct ViewState {
        std::int16_t kills = 0, losses = 0, commanders_killed = 0, commanders_lost = 0;
        float metal_storage = 0, energy_storage = 0, metal = 0, energy = 0;
        float values[6]{};
        bool valid = false;
    };
    const ViewState& view_state(int slot) const { return view_[static_cast<std::size_t>(slot)]; }

    const NetCounters& counters() const { return counters_; }
    const FrameReceiver& receiver() const { return receiver_; }
    const FrameSender& sender() const { return sender_; }
    std::string fault() const { return fault_; }

    // Sends waiting frames now (the menus' and the start's forced flushes).
    void flush_now() { sender_.flush(clock_, true, send_fn()); }
    // Network input outside a step (paused game): the frame loop's poll.
    void poll() { network_input(); }

    // ---- sim::BattleOutside: the network part.
    void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) override;
    void broadcast_player_units(int player) override;
    void broadcast_script_call(sim::Unit& unit, std::int16_t function) override;
    void broadcast_script_call_args(sim::Unit& unit, std::int16_t function, std::int32_t argc, std::int32_t a) override;
    void broadcast_script_call_named(sim::Unit& unit, std::int16_t function, std::int32_t argc, std::int32_t a, std::int32_t b,
                                     std::int32_t c, std::int32_t d) override;
    void feature_packet(const std::uint8_t* packet, bool to_host) override;
    void resources_shared(int kind, int from, int to, float amount) override;
    void explored_map_shared(int from, int to) override;
    void player_sensor_turn(int player) override;
    void network_input() override;
    void network_flush() override;

    // ---- the rest goes to the client's outside.
    std::uint8_t bucket_height(const sim::Unit& u) override { return inner_.bucket_height(u); }
    void update_sight(sim::Unit& u) override { inner_.update_sight(u); }
    bool structure_fits(std::uint16_t t, std::int16_t x, std::int16_t z) override { return inner_.structure_fits(t, x, z); }
    void unfinished_decay(sim::Unit& u, std::int32_t t) override { inner_.unfinished_decay(u, t); }
    bool unit_visible(int p, sim::Unit& u) override { return inner_.unit_visible(p, u); }
    bool position_explored(int p, const std::array<std::int32_t, 3>& pos) override { return inner_.position_explored(p, pos); }
    std::uint8_t feature_flags(std::uint16_t f) override { return inner_.feature_flags(f); }
    void ai_own_unit(int t, sim::Unit& u) override { inner_.ai_own_unit(t, u); }
    void ai_rescore(int t) override { inner_.ai_rescore(t); }
    void ai_controller_step(int p) override { inner_.ai_controller_step(p); }
    void player_stage_rest(int p) override { inner_.player_stage_rest(p); }
    void players_stage_end() override { inner_.players_stage_end(); }
    void player_victory_turn(int p) override { inner_.player_victory_turn(p); }
    void join_group(sim::Unit& u, std::int32_t g) override { inner_.join_group(u, g); }
    void stamp_line_of_sight(sim::Unit& u) override { inner_.stamp_line_of_sight(u); }
    void victory_unit_created(sim::Unit& u) override { inner_.victory_unit_created(u); }
    std::int32_t run_mission(sim::Unit& u, sim::MissionOrder& o, std::uint32_t& e) override { return inner_.run_mission(u, o, e); }
    void attach_unit(sim::Unit& u, std::int32_t a, std::int32_t p, std::int32_t m) override { inner_.attach_unit(u, a, p, m); }
    void drop_unit(sim::Unit& u, std::int32_t a) override { inner_.drop_unit(u, a); }
    void detach_from_carrier(sim::Unit& u) override { inner_.detach_from_carrier(u); }
    void release_unit(sim::Unit& u) override { inner_.release_unit(u); }
    void emit_sfx(sim::Unit& u, std::int32_t p, std::int32_t t) override { inner_.emit_sfx(u, p, t); }
    void reveal_dead_unit(sim::Unit& u) override { inner_.reveal_dead_unit(u); }
    void forget_fog(sim::Unit& u) override { inner_.forget_fog(u); }
    void victory_unit_destroyed(sim::Unit& u) override { inner_.victory_unit_destroyed(u); }
    void player_out_of_units(int p) override { inner_.player_out_of_units(p); }
    void commander_died(int p) override { inner_.commander_died(p); }
    std::uint32_t clock_ticks() override { return inner_.clock_ticks(); }
    void on_activate_flags_changed(sim::Unit& u, std::uint8_t g, std::uint8_t l) override;
    void speech(sim::Unit& u, std::uint32_t id, sim::SpeechText t) override { inner_.speech(u, id, t); }
    void under_attack_notice(sim::Unit& u) override { inner_.under_attack_notice(u); }
    void display_changed(sim::Unit& u) override { inner_.display_changed(u); }
    void build_counts_changed(sim::Unit& u) override { inner_.build_counts_changed(u); }
    void nano_beam(const std::array<std::int32_t, 3>& f, const std::array<std::int32_t, 6>& b) override { inner_.nano_beam(f, b); }
    void forget_speech(sim::Unit& u) override { inner_.forget_speech(u); }
    void leave_group(sim::Unit& u) override { inner_.leave_group(u); }
    void stats_activity(std::int32_t k) override { inner_.stats_activity(k); }
    void kill_counted(std::uint8_t s, sim::Unit& v) override { inner_.kill_counted(s, v); }
    void stockpile_used(sim::Unit& u) override { inner_.stockpile_used(u); }
    void camera_follow_step() override { inner_.camera_follow_step(); }
    void play_sound(std::uint16_t s, const sim::Vec3i& p) override { inner_.play_sound(s, p); }
    void spawn_smoke(const sim::Vec3i& p) override { inner_.spawn_smoke(p); }
    void spawn_muzzle_smoke(const sim::Vec3i& p) override { inner_.spawn_muzzle_smoke(p); }
    void camera_shake(std::int32_t x, std::int32_t y, std::int32_t d) override { inner_.camera_shake(x, y, d); }
    void feature_smoke(const sim::Vec3i& p) override { inner_.feature_smoke(p); }
    void feature_steam(const sim::Vec3i& p) override { inner_.feature_steam(p); }
    void feature_fire_sound(std::int16_t x, std::int16_t z) override { inner_.feature_fire_sound(x, z); }
    void wreck_fire(const sim::Vec3i& p) override { inner_.wreck_fire(p); }
    void explosion_smoke(const sim::Vec3i& p) override { inner_.explosion_smoke(p); }
    void victory_unit_transferred(sim::Unit& u) override { inner_.victory_unit_transferred(u); }
    void builder_menu(sim::Unit& u) override { inner_.builder_menu(u); }
    void build_menu_refresh(sim::Unit& u) override { inner_.build_menu_refresh(u); }
    void orders_panel_changed() override { inner_.orders_panel_changed(); }
    void selection_changed() override { inner_.selection_changed(); }
    void teleport_trail(const std::array<std::int32_t, 3>& a, const std::array<std::int32_t, 3>& b) override { inner_.teleport_trail(a, b); }
    void ai_player_setup(int p) override { inner_.ai_player_setup(p); }
    void ai_battle_ready() override { inner_.ai_battle_ready(); }

private:
    SendFn send_fn();
    // Net_BroadcastGuaranteed: from a playing, present player to everybody.
    bool broadcast(std::int32_t from, const std::uint8_t* p, std::size_t n);
    // Net_SendPacketToPlayer: from a playing player to a remote one.
    bool send_to(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n);
    int slot_of(std::int32_t net_id) const;  // 10 when none
    std::int32_t first_local_net_id() const;
    bool plays(int slot) const;
    void dispatch(int from, int to, const std::vector<std::uint8_t>& p);

    sim::BattleOutside& inner_;
    Transport& transport_;
    sim::Battle* battle_ = nullptr;
    FrameSender sender_;
    FrameReceiver receiver_;
    std::uint32_t clock_ = 0;
    Phase phase_ = Phase::Battle;
    bool paused_ = false;
    int speed_ = 10;
    std::uint32_t view_turns_ = 0;
    std::array<std::uint32_t, 11> last_state_tick_{};
    std::array<std::uint32_t, 11> last_heard_{};
    std::array<std::array<std::uint8_t, 11>, 11> allied_by_{};
    std::array<ViewState, 11> view_{};
    std::array<std::uint8_t, 11> leaving_{};
    NetCounters counters_;
    std::string fault_;
};

} // namespace net
