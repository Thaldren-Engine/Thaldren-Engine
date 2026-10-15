#include "net/net_battle.h"

#include <algorithm>
#include <cstring>

#include "net/protocol.h"
#include "sim/combat.h"
#include "sim/economy.h"
#include "sim/features.h"
#include "sim/projectile.h"
#include "sim/sight.h"

namespace net {

using u8 = std::uint8_t;

NetBattle::NetBattle(sim::BattleOutside& inner, Transport& transport, NetTiming timing)
    : inner_(inner), transport_(transport), sender_(timing) {}

void NetBattle::attach(sim::Battle& battle) {
    battle_ = &battle;
    std::vector<std::int32_t> ids;
    for (int s = 0; s < 10; ++s)
        if (plays(s)) ids.push_back(battle.players[static_cast<std::size_t>(s)].net_id);
    transport_.set_local_players(ids);
}

SendFn NetBattle::send_fn() {
    return [this](std::int32_t from, std::int32_t to, std::vector<std::uint8_t> m) { transport_.send(from, to, std::move(m)); };
}

bool NetBattle::plays(int s) const {
    if (!battle_ || s < 0 || s >= 10) return false;
    const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(s)];
    return p.active && (p.controller == 1 || p.controller == 2);
}

int NetBattle::slot_of(std::int32_t id) const {
    if (!battle_ || id == -1) return 10;
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(s)];
        if (p.controller != 0 && p.net_id == id) return s;
    }
    return 10;
}

std::int32_t NetBattle::first_local_net_id() const {
    for (int s = 0; s < 10; ++s)
        if (plays(s)) return battle_->players[static_cast<std::size_t>(s)].net_id;
    return -1;
}

bool NetBattle::broadcast(std::int32_t from, const std::uint8_t* p, std::size_t n) {
    const int s = slot_of(from);
    if (s == 10 || !plays(s) || leaving_[static_cast<std::size_t>(s)]) return false;
    ++counters_.packets_sent;
    if (p[0] < counters_.by_type.size()) ++counters_.by_type[p[0]];
    sender_.queue(from, kEverybody, p, n, clock_, send_fn());
    return true;
}

bool NetBattle::send_to(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n) {
    const int s = slot_of(from), d = slot_of(to);
    if (s == 10 || !plays(s) || leaving_[static_cast<std::size_t>(s)]) return false;
    if (d == 10 || battle_->players[static_cast<std::size_t>(d)].controller != 3 || leaving_[static_cast<std::size_t>(d)]) return false;
    ++counters_.packets_sent;
    if (p[0] < counters_.by_type.size()) ++counters_.by_type[p[0]];
    sender_.queue(from, to, p, n, clock_, send_fn());
    return true;
}

std::int32_t NetBattle::remote_lag() const {
    if (!battle_) return 0;
    std::int32_t lag = 0;
    std::uint32_t lowest = battle_->game_tick();
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(s)];
        if (p.active && p.controller == 3 && p.alive_count != 0 && last_state_tick_[static_cast<std::size_t>(s)] < lowest)
            lowest = last_state_tick_[static_cast<std::size_t>(s)];
    }
    lag = static_cast<std::int32_t>(battle_->game_tick() - lowest);
    return lag;
}

// ------------------------------------------------------------------ sending

void NetBattle::network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) {
    broadcast(net_id, packet, static_cast<std::size_t>(size));
}

void NetBattle::broadcast_player_units(int player) {
    if (!battle_) return;
    const std::vector<std::uint8_t> pk = battle_->unit_state_packet(player);
    ++counters_.state_packets;
    broadcast(battle_->players[static_cast<std::size_t>(player)].net_id, pk.data(), pk.size());
}

namespace {
std::array<u8, 22> script_packet(const sim::Unit& u, std::int16_t fn, std::int32_t argc, std::int32_t a, std::int32_t b,
                                 std::int32_t c, std::int32_t d) {
    std::array<u8, 22> pk{};
    pk[0] = packet::kScriptCall;
    put16(&pk[1], u.id);
    put16(&pk[3], static_cast<std::uint16_t>(fn));
    pk[5] = static_cast<u8>(argc);
    put32(&pk[6], static_cast<std::uint32_t>(a));
    put32(&pk[10], static_cast<std::uint32_t>(b));
    put32(&pk[14], static_cast<std::uint32_t>(c));
    put32(&pk[18], static_cast<std::uint32_t>(d));
    return pk;
}
}  // namespace

void NetBattle::broadcast_script_call(sim::Unit& u, std::int16_t fn) {
    if (!battle_) return;
    const auto pk = script_packet(u, fn, 0, 0, 0, 0, 0);
    broadcast(battle_->players[static_cast<std::size_t>(battle_->ticks[u.id].owner)].net_id, pk.data(), pk.size());
}

void NetBattle::broadcast_script_call_args(sim::Unit& u, std::int16_t fn, std::int32_t argc, std::int32_t a) {
    if (!battle_) return;
    // 0x456290 with the remaining three arguments as its callers leave them (zero).
    const auto pk = script_packet(u, fn, argc, a, 0, 0, 0);
    broadcast(battle_->players[static_cast<std::size_t>(battle_->ticks[u.id].owner)].net_id, pk.data(), pk.size());
}

void NetBattle::broadcast_script_call_named(sim::Unit& u, std::int16_t fn, std::int32_t argc, std::int32_t a, std::int32_t b,
                                            std::int32_t c, std::int32_t d) {
    if (!battle_) return;
    const auto pk = script_packet(u, fn, argc, a, b, c, d);
    broadcast(battle_->players[static_cast<std::size_t>(battle_->ticks[u.id].owner)].net_id, pk.data(), pk.size());
}

void NetBattle::on_activate_flags_changed(sim::Unit& u, std::uint8_t gained, std::uint8_t lost) {
    inner_.on_activate_flags_changed(u, gained, lost);
    if (!battle_) return;
    const int owner = battle_->ticks[u.id].owner;
    if (!plays(owner)) return;
    u8 pk[4] = {packet::kActivate};
    put16(pk + 1, u.id);
    pk[3] = u.activate_flags;
    broadcast(battle_->players[static_cast<std::size_t>(owner)].net_id, pk, 4);
}

void NetBattle::feature_packet(const std::uint8_t* packet, bool to_host) {
    inner_.feature_packet(packet, to_host);
    // Feature changes go out from the first human player's net id (to the host or to everybody;
    // with the star transport both reach every machine).
    broadcast(first_local_net_id(), packet, 6);
}

void NetBattle::resources_shared(int kind, int from, int to, float amount) {
    inner_.resources_shared(kind, from, to, amount);
    if (!battle_ || from == 10 || to == 10) return;
    u8 pk[17] = {packet::kShare};
    put32(pk + 1, static_cast<std::uint32_t>(kind));
    const std::int32_t f = battle_->players[static_cast<std::size_t>(from)].net_id, t = battle_->players[static_cast<std::size_t>(to)].net_id;
    put32(pk + 5, static_cast<std::uint32_t>(f));
    put32(pk + 9, static_cast<std::uint32_t>(t));
    putf(pk + 13, amount);
    send_to(f, t, pk, 17);
}

void NetBattle::explored_map_shared(int from, int to) {
    inner_.explored_map_shared(from, to);
    if (!battle_ || from == 10 || to == 10) return;
    u8 pk[17] = {packet::kShare, 3};
    const std::int32_t f = battle_->players[static_cast<std::size_t>(from)].net_id, t = battle_->players[static_cast<std::size_t>(to)].net_id;
    put32(pk + 5, static_cast<std::uint32_t>(f));
    put32(pk + 9, static_cast<std::uint32_t>(t));
    send_to(f, t, pk, 17);
}

void NetBattle::player_sensor_turn(int player) {
    inner_.player_sensor_turn(player);
    // Every fourth turn of the viewed player (network games): its totals for the other human
    // players (0x4573D0).
    if (!battle_ || battle_->settings.provider != 3) return;
    if ((++view_turns_ & 3) != 0) return;
    if (!plays(player) || leaving_[static_cast<std::size_t>(player)]) return;
    const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(player)];
    const sim::PlayerEconomy& e = battle_->player_economy[static_cast<std::size_t>(player)];
    u8 pk[58] = {packet::kViewState, 0};
    put32(pk + 2, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.units_killed)));
    put32(pk + 6, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.units_lost)));
    put32(pk + 10, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.commanders_killed)));
    put32(pk + 14, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.commanders_lost)));
    putf(pk + 18, e.metal_storage);
    putf(pk + 22, e.energy_storage);
    putf(pk + 26, p.metal_stored);
    putf(pk + 30, p.energy_stored);
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& o = battle_->players[static_cast<std::size_t>(s)];
        if (o.active && o.controller == 3 && !leaving_[static_cast<std::size_t>(s)]) send_to(p.net_id, o.net_id, pk, 58);
    }
}

void NetBattle::network_flush() { sender_.flush(clock_, false, send_fn()); }

void NetBattle::send_chat(const std::string& text) {
    u8 pk[65] = {packet::kChat};
    std::memcpy(pk + 1, text.data(), std::min<std::size_t>(text.size(), 63));
    ++counters_.chat;
    broadcast(first_local_net_id(), pk, 65);
}

void NetBattle::send_pause(bool paused) {
    paused_ = paused;
    u8 pk[3] = {packet::kSpeed, 0, static_cast<u8>(paused ? 1 : 0)};
    broadcast(first_local_net_id(), pk, 3);
}

void NetBattle::send_speed(int speed) {
    speed_ = std::clamp(speed, 1, 20);
    u8 pk[3] = {packet::kSpeed, 1, static_cast<u8>(speed_)};
    broadcast(first_local_net_id(), pk, 3);
}

void NetBattle::send_alliance(int from, int to, bool allied) {
    if (!battle_) return;
    // 0x452960 for a local player: its own alliance byte, and the other's answer byte when the
    // other is a computer player; a remote player hears it (packet 0x23).
    const sim::PlayerSlot& f = battle_->players[static_cast<std::size_t>(from)];
    const sim::PlayerSlot& t = battle_->players[static_cast<std::size_t>(to)];
    if (!plays(from) || !t.active) return;
    battle_->alliances[static_cast<std::size_t>(from)][t.state] = allied ? 1 : 0;
    if (t.controller == 2) allied_by_[static_cast<std::size_t>(from)][static_cast<std::size_t>(to)] = allied ? 1 : 0;
    if (t.controller == 1 || t.controller == 2) {
        allied_by_[static_cast<std::size_t>(to)][static_cast<std::size_t>(from)] = allied ? 1 : 0;
        if (t.controller == 2) battle_->alliances[static_cast<std::size_t>(to)][f.state] = allied ? 1 : 0;
    } else if (t.controller == 3) {
        u8 pk[14] = {packet::kAlliance};
        put32(pk + 1, static_cast<std::uint32_t>(f.net_id));
        put32(pk + 5, static_cast<std::uint32_t>(t.net_id));
        pk[9] = allied ? 1 : 0;
        send_to(f.net_id, t.net_id, pk, 14);
    }
}

// ------------------------------------------------------------------ receiving

void NetBattle::network_input() {
    // 0x453D40: every packet that is due, then the session's bookkeeping.
    if (!battle_) return;
    transport_.pump();
    const FrameReceiver::ReadFn read = [this](std::int32_t& f, std::int32_t& t, std::vector<std::uint8_t>& m) {
        Message msg;
        if (!transport_.receive(msg)) return false;
        f = msg.from;
        t = msg.to;
        m = std::move(msg.bytes);
        return true;
    };
    Delivered d;
    while (receiver_.next(battle_->game_tick(), sender_.timing().frame_timing, read, d)) {
        if (d.packet.empty()) continue;
        const int from = slot_of(d.from);
        int to = slot_of(d.to);
        if (to == 10) to = slot_of(first_local_net_id());  // addressed to everybody
        const u8 type = d.packet[0];
        if (!(packet_phases(type) & static_cast<u8>(phase_))) {
            ++counters_.refused;
            continue;
        }
        if (from == 10 || to == 10) {
            ++counters_.refused;  // the original answers "unknown player" (status 6)
            continue;
        }
        const sim::PlayerSlot& fp = battle_->players[static_cast<std::size_t>(from)];
        if (!(fp.active && fp.controller == 3 && fp.state != 10)) {
            ++counters_.refused;
            continue;
        }
        last_heard_[static_cast<std::size_t>(from)] = clock_;
        ++counters_.packets_received;
        if (type < counters_.by_type.size()) ++counters_.by_type[type];
        dispatch(from, to, d.packet);
        if (type == packet::kGameStart) break;
    }
    if (receiver_.stuck()) fault_ = "a frame held a packet type without a size (the original hangs)";
    if (!battle_->net_state().fault.empty() && fault_.empty()) fault_ = battle_->net_state().fault;
}

void NetBattle::dispatch(int from, int to, const std::vector<std::uint8_t>& v) {
    sim::Battle& b = *battle_;
    const u8* p = v.data();
    const std::size_t need = packet_size_at(p, v.size());
    if (need == 0 || v.size() < need) {
        ++counters_.refused;
        return;
    }
    switch (p[0]) {
    case packet::kPing: {
        if (get32(p + 5) == 0) {
            std::vector<u8> echo(v);
            put32(echo.data() + 5, clock_ * 1000u / 30u + 1u);  // GetTickCount (never 0)
            send_to(b.players[static_cast<std::size_t>(to)].net_id, static_cast<std::int32_t>(get32(p + 9)), echo.data(), 13);
        }
        break;
    }
    case packet::kChat:
        if (b.players[static_cast<std::size_t>(to)].controller == 1 && on_chat) {
            const char* s = reinterpret_cast<const char*>(p + 1);
            on_chat(from, std::string(s, strnlen(s, 64)));
        }
        break;
    case packet::kProbe: {
        u8 r[1] = {packet::kProbeReply};
        send_to(first_local_net_id(), b.players[static_cast<std::size_t>(from)].net_id, r, 1);
        break;
    }
    case packet::kUnitCreated:
        b.apply_unit_packet(from, p);
        break;
    case packet::kTransport:
        b.apply_transport_packet(p);
        break;
    case packet::kDamage: {
        sim::DamagePacket pk{};
        std::copy(p, p + 9, pk.begin());
        sim::apply_damage_packet(b.combat(), pk);
        break;
    }
    case packet::kKilled: {
        sim::KilledPacket pk{};
        std::copy(p, p + 11, pk.begin());
        sim::apply_killed_packet(b.combat(), pk, false);
        break;
    }
    case packet::kFire:
        b.apply_fire_packet(p);
        break;
    case packet::kDetonate:
        sim::detonate_matching(b.projectile_world(), p);
        break;
    case packet::kFeature:
        sim::apply_feature_packet(b.feature_world(), p, b.net_state().weapons);
        break;
    case packet::kScriptCall:
        b.apply_script_packet(p);
        break;
    case packet::kActivate:
        b.apply_activate_packet(p);
        break;
    case packet::kFinished:
        b.apply_finished_packet(p);
        break;
    case packet::kSound:
        if (p[1] == 0)
            inner_.play_sound(static_cast<std::uint16_t>(get32(p + 2)),
                              sim::Vec3i{static_cast<std::int32_t>(get32(p + 6)), static_cast<std::int32_t>(get32(p + 10)),
                                         static_cast<std::int32_t>(get32(p + 14))});
        break;
    case packet::kOwnership: {
        const int ns = slot_of(static_cast<std::int32_t>(get32(p + 3)));
        if (ns != 10 && plays(ns)) b.apply_ownership_packet(ns, p);
        break;
    }
    case packet::kShare: {
        const int fs = slot_of(static_cast<std::int32_t>(get32(p + 5))), ts = slot_of(static_cast<std::int32_t>(get32(p + 9)));
        if (fs == 10 || ts == 10) break;
        const std::uint32_t kind = get32(p + 1);
        if (kind == 1) sim::transfer_energy(b.economy(), fs, ts, getf(p + 13), false);
        else if (kind == 2) sim::transfer_metal(b.economy(), fs, ts, getf(p + 13), false);
        else if (kind == 3) b.share_explored_map(fs, ts);
        break;
    }
    case packet::kSpeed:
        if (p[1] == 0) {
            paused_ = (p[2] & 1) != 0;
            if (on_pause) on_pause(paused_);
        } else {
            speed_ = std::clamp(static_cast<int>(p[2]), 1, 20);
            if (on_speed) on_speed(speed_);
        }
        break;
    case packet::kStatus: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s != 10) {
            leaving_[static_cast<std::size_t>(s)] = p[5];
            if (on_player_left) on_player_left(s, p[5]);
        }
        break;
    }
    case packet::kAlliance: {
        const int fs = slot_of(static_cast<std::int32_t>(get32(p + 1))), ts = slot_of(static_cast<std::int32_t>(get32(p + 5)));
        if (fs == 10 || ts == 10 || !plays(ts)) break;
        allied_by_[static_cast<std::size_t>(ts)][static_cast<std::size_t>(fs)] = p[9];
        if (b.players[static_cast<std::size_t>(ts)].controller == 2 || get32(p + 10) != 0)
            b.alliances[static_cast<std::size_t>(ts)][b.players[static_cast<std::size_t>(fs)].state] = p[9];
        break;
    }
    case packet::kViewState: {
        ViewState& vs = view_[static_cast<std::size_t>(from)];
        vs.kills = static_cast<std::int16_t>(get16(p + 2));
        vs.losses = static_cast<std::int16_t>(get16(p + 6));
        vs.commanders_killed = static_cast<std::int16_t>(get16(p + 10));
        vs.commanders_lost = static_cast<std::int16_t>(get16(p + 14));
        vs.metal_storage = getf(p + 18);
        vs.energy_storage = getf(p + 22);
        vs.metal = getf(p + 26);
        vs.energy = getf(p + 30);
        for (int k = 0; k < 6; ++k) vs.values[k] = getf(p + 34 + 4 * k);
        vs.valid = true;
        break;
    }
    case packet::kUnitStates:
        last_state_tick_[static_cast<std::size_t>(from)] = b.apply_unit_state_packet(from, p, need);
        break;
    default:
        break;  // battle room and loading packets (net/lobby.h) and the ignored ones
    }
}

} // namespace net
