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
    : inner_(inner), own_(std::make_unique<NetSession>(transport, timing)), session_(*own_), transport_(session_.transport),
      sender_(session_.sender), receiver_(session_.receiver) {}

NetBattle::NetBattle(sim::BattleOutside& inner, NetSession& session)
    : inner_(inner), session_(session), transport_(session.transport), sender_(session.sender), receiver_(session.receiver) {}

void NetBattle::attach(sim::Battle& battle) {
    battle_ = &battle;
    std::vector<std::int32_t> ids;
    for (int s = 0; s < 10; ++s)
        if (plays(s)) ids.push_back(battle.players[static_cast<std::size_t>(s)].net_id);
    transport_.set_local_players(ids);
}

SendFn NetBattle::send_fn() {
    return [this](std::int32_t from, std::int32_t to, std::vector<std::uint8_t> m) {
        counters_.bytes_sent += static_cast<std::uint32_t>(m.size());
        transport_.send(from, to, std::move(m));
    };
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
    if (!per_group_) {
        sender_.queue(from, kEverybody, p, n, clock_, send_fn());
        return true;
    }
    // Two players share a group (0x450980): one addressed copy per group of remote players.
    std::array<bool, 11> sent{};
    for (int k = 0; k < 10; ++k) {
        const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(k)];
        if (!q.active || q.controller != 3) continue;
        const std::int32_t g = room_[static_cast<std::size_t>(k)].group;
        if (g >= 0 && g < 10 && sent[static_cast<std::size_t>(g)]) continue;
        sender_.queue(from, q.net_id, p, n, clock_, send_fn());
        if (g >= 0 && g < 10) sent[static_cast<std::size_t>(g)] = true;
    }
    return true;
}

void NetBattle::set_room(const std::array<RoomPlayer, 10>& players) {
    room_ = players;
    have_room_ = true;
    if (!battle_) return;
    for (int s = 0; s < 10; ++s)
        if (room_[static_cast<std::size_t>(s)].net_id && !plays(s)) apply_side_record(s, room_[static_cast<std::size_t>(s)]);
}

void NetBattle::apply_side_record(int s, const RoomPlayer& r) {
    // A remote player's side record (0x20): its watch flag and kind reach the victory checks.
    sim::VictorySide& side = battle_->victory.sides[static_cast<std::size_t>(s)];
    side.watching = r.watching();
    side.human = r.kind == 1;
    side.host = (r.flags & room::kHost) != 0;  // the whole side record is copied (+0x97 too)
}

void NetBattle::announce_records() {
    // 0x450F90: the local players' side records (0x20) and ally teams (0x24).
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& r = room_[static_cast<std::size_t>(s)];
        if (!r.net_id || !plays(s)) continue;
        u8 pk[186] = {packet::kSideRecord};
        const auto bytes = encode_side_record(r);
        std::memcpy(pk + 1, bytes.data(), bytes.size());
        broadcast(r.net_id, pk, 186);
        u8 at[6] = {packet::kAllyTeam};
        put32(at + 1, static_cast<std::uint32_t>(r.net_id));
        at[5] = r.ally_team;
        broadcast(r.net_id, at, 6);
    }
    sender_.flush(clock_, true, send_fn());
}

void NetBattle::check_watch() {
    // The players' stage put the local player in watch mode (0x464F80): its record gets the
    // watch bit, mapping and line of sight off, and everybody hears it.
    if (!battle_ || !have_room_ || watch_sent_) return;
    const int l = battle_->settings.local_player;
    if (l < 0 || l >= 10 || !battle_->victory.sides[static_cast<std::size_t>(l)].watching) return;
    RoomPlayer& r = room_[static_cast<std::size_t>(l)];
    r.options = static_cast<std::uint16_t>(r.options | room::kWatch);
    r.map_flags = static_cast<std::uint8_t>(r.map_flags & ~(room::kMapping | room::kLineOfSight));
    watch_sent_ = true;
    announce_records();
}

void NetBattle::check_timeouts() {
    // 0x453C20.
    if (!battle_ || timeout_seconds_ == 0 || timeout_disabled_) return;
    if (paused_) {
        pause_clock_ = clock_;
        return;
    }
    int group = -1, first = -1;
    bool several = false;
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(s)];
        if (!q.active || q.controller != 3) continue;
        const std::uint32_t since = std::max(last_heard_[static_cast<std::size_t>(s)], pause_clock_);
        if (clock_ - since <= timeout_seconds_ * 30u) continue;
        const int g = room_[static_cast<std::size_t>(s)].group;
        if (first < 0) first = s;
        if (group < 0) group = g;
        else if (group != g) several = true;
    }
    if (on_timeout) on_timeout(first >= 0 && !several ? first : -1);
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
    // Feature changes go out from the first playing player's net id. A machine that is not the
    // host asks the host alone (0x4244B0 -> 0x451BC0 to the net id of 0x450030: the first record
    // with the host flag, -1 when that record has no controller); every other change goes to
    // everybody. (Sent to everybody, a request reached the other non-host machines too, and
    // each of them asked the host again: damage counted once more per extra machine.)
    if (!to_host) {
        broadcast(first_local_net_id(), packet, 6);
        return;
    }
    if (!battle_) return;
    for (int s = 0; s < 10; ++s) {
        if (!battle_->victory.sides[static_cast<std::size_t>(s)].host) continue;
        const sim::PlayerSlot& h = battle_->players[static_cast<std::size_t>(s)];
        if (h.controller != 0) send_to(first_local_net_id(), h.net_id, packet, 6);
        return;
    }
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
    send_view_state(player, -1, false);
}

bool NetBattle::remote_human(int s) const {
    // A remote record whose side record says human (+0x94 = 1).
    const sim::PlayerSlot& o = battle_->players[static_cast<std::size_t>(s)];
    if (!o.active || o.controller != 3) return false;
    return have_room_ ? room_[static_cast<std::size_t>(s)].kind == 1 : battle_->victory.sides[static_cast<std::size_t>(s)].human;
}

void NetBattle::send_view_state(int player, int to, bool ask) {
    // 0x4573D0(record, addressee or none, ask): a playing, present record's totals (its record's
    // kill and loss words, storage and stored resources, and the six income/use figures).
    if (!battle_ || !plays(player) || leaving_[static_cast<std::size_t>(player)]) return;
    const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(player)];
    const sim::PlayerEconomy& e = battle_->player_economy[static_cast<std::size_t>(player)];
    u8 pk[58] = {packet::kViewState, static_cast<u8>(ask ? 1 : 0)};
    put32(pk + 2, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.units_killed)));
    put32(pk + 6, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.units_lost)));
    put32(pk + 10, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.commanders_killed)));
    put32(pk + 14, static_cast<std::uint32_t>(static_cast<std::int32_t>(p.commanders_lost)));
    putf(pk + 18, e.metal_storage);
    putf(pk + 22, e.energy_storage);
    putf(pk + 26, p.metal_stored);
    putf(pk + 30, p.energy_stored);
    if (to >= 0) {
        if (!leaving_[static_cast<std::size_t>(to)]) send_to(p.net_id, battle_->players[static_cast<std::size_t>(to)].net_id, pk, 58);
        return;
    }
    // To every remote human player that is present (0x451BC0 to each).
    for (int s = 0; s < 10; ++s)
        if (remote_human(s) && !leaving_[static_cast<std::size_t>(s)]) send_to(p.net_id, battle_->players[static_cast<std::size_t>(s)].net_id, pk, 58);
}

bool NetBattle::end_exchange() {
    // 0x4572A0.
    if (!battle_) return true;
    bool done = true;
    for (int l = 0; l < 10; ++l) {
        if (!plays(l) || leaving_[static_cast<std::size_t>(l)]) continue;
        for (int r = 0; r < 10; ++r) {
            const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(r)];
            if (!q.active || q.controller != 3) continue;
            const std::size_t L = static_cast<std::size_t>(l), R = static_cast<std::size_t>(r);
            const bool want = remote_human(r) ? (!stats_acked_[L][R] || !stats_told_[L][R] || !stats_got_[L][R]) : !stats_got_[L][R];
            if (!want) continue;
            send_view_state(l, r, true);
            done = false;
        }
    }
    flush_now();
    return done;
}

void NetBattle::network_flush() { sender_.flush(clock_, false, send_fn()); }

void NetBattle::flush_all() { sender_.flush(clock_, true, send_fn()); }

void NetBattle::send_chat(const std::string& text) { send_chat(text, 0); }

void NetBattle::send_chat(const std::string& text, int recipients, const std::array<bool, 10>& chosen) {
    u8 pk[65] = {packet::kChat};
    std::memcpy(pk + 1, text.data(), std::min<std::size_t>(text.size(), 64));
    ++counters_.chat;
    const std::int32_t me = first_local_net_id();
    if (!battle_ || (!text.empty() && text[0] == '+') || recipients == 0) {
        broadcast(me, pk, 65);
        return;
    }
    const int local = battle_->settings.local_player;
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(s)];
        if (!q.active || q.controller != 3) continue;
        const bool allied = battle_->alliances[static_cast<std::size_t>(local)][static_cast<std::size_t>(s)] != 0;  // by record index (0x453360)
        const bool to = recipients == 3 ? chosen[static_cast<std::size_t>(s)] : recipients == 1 ? allied : !allied;
        if (to) send_to(me, q.net_id, pk, 65);
    }
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

void NetBattle::reject(int slot, std::uint8_t reason) {
    if (!battle_ || slot < 0 || slot > 9) return;
    const sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(slot)];
    if (!p.active || p.controller != 3 || leaving_[static_cast<std::size_t>(slot)]) return;
    send_away(slot, reason);
}

void NetBattle::remove_player(int slot) {
    // 0x452CC0 in the battle (GS+0x2A44 bit 2 is set from the game start on).
    if (!battle_ || slot < 0 || slot > 9) return;
    sim::Battle& b = *battle_;
    sim::PlayerSlot& p = b.players[static_cast<std::size_t>(slot)];
    if (!p.active || p.controller < 1 || p.controller > 3) return;
    const u8 state = p.state;
    if (state == 10) return;
    const std::size_t s = static_cast<std::size_t>(slot);
    const bool was_host = b.victory.sides[s].host;
    // Every local record forgets its alliance toward it (+0x108 and +0x113 at its index).
    for (std::size_t k = 0; k < 10 && k < b.players.size(); ++k) {
        const sim::PlayerSlot& q = b.players[k];
        if (!q.active || (q.controller != 1 && q.controller != 2)) continue;
        if (k < b.alliances.size() && state < 16) b.alliances[k][state] = 0;
        allied_by_[k][s] = 0;
    }
    b.kill_team(slot);  // 0x486F10
    if (p.controller == 3) {
        // Another machine's record is freed (0x463C60(record, 0): controller and side kind 0;
        // net id -1, group +0xC 0). One of this machine's stays as it is in the battle: no
        // session player is destroyed here.
        p.active = false;
        p.controller = 0;
        p.net_id = -1;
        room_[s].kind = 0;
        room_[s].group = 0;
    }
    // (GS+0x2A3C, the session's player count, goes down by one: only the menus read it.)
    room_[s].cd = static_cast<std::uint16_t>(room_[s].cd & ~4u);  // side +0x9D bit 2
    if (s < b.alliances.size())
        for (std::size_t k = 0; k < 11; ++k) b.alliances[s][k] = 0;  // its own +0x108..+0x112
    if (!was_host) return;
    // Its side record had the host flag: the human record (local 1 or remote 3) with the highest
    // net id (unsigned, above 0) gets it too; a local record removed here still counts, and the
    // old record keeps its flag (0x450030 still finds the first flagged record).
    std::uint32_t best = 0;
    for (std::size_t k = 0; k < 10 && k < b.players.size(); ++k) {
        const sim::PlayerSlot& q = b.players[k];
        if (q.active && (q.controller == 3 || q.controller == 1) && best < static_cast<std::uint32_t>(q.net_id))
            best = static_cast<std::uint32_t>(q.net_id);
    }
    const int h = slot_of(static_cast<std::int32_t>(best));
    if (h == 10) return;
    b.victory.sides[static_cast<std::size_t>(h)].host = true;
    room_[static_cast<std::size_t>(h)].flags |= room::kHost;
}

void NetBattle::send_away(int slot, std::uint8_t reason) {
    // 0x453010.
    if (!battle_ || slot < 0 || slot > 9) return;
    sim::PlayerSlot& p = battle_->players[static_cast<std::size_t>(slot)];
    const std::size_t s = static_cast<std::size_t>(slot);
    auto status = [&](std::int32_t id) {
        u8 pk[6] = {packet::kStatus};
        put32(pk + 1, static_cast<std::uint32_t>(id));
        pk[5] = reason;
        broadcast(first_local_net_id(), pk, 6);
    };
    auto gone = [&](int k, bool tell) {
        leaving_[static_cast<std::size_t>(k)] = reason;
        if (tell && on_player_left) on_player_left(k, reason);
    };
    if (p.active && !leaving_[s]) {
        if (p.controller == 1) {
            // This machine's human: a status for each player this machine runs, all from the
            // first of them (refused once that one is leaving: only the first goes out), and
            // 0x452CC0 for the human each time; its computer players' teams are not killed.
            const std::int32_t human = p.net_id;
            for (int k = 0; k < 10 && static_cast<std::size_t>(k) < battle_->players.size(); ++k) {
                const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(k)];
                if (!q.active || (q.controller != 1 && q.controller != 2)) continue;
                status(q.net_id);
                remove_player(slot_of(human));
                gone(k, true);
            }
        } else if (p.controller == 2) {
            status(p.net_id);
            remove_player(slot);
            gone(slot, true);
        } else if (p.controller == 3) {
            status(p.net_id);
            if (room_[s].kind != 1) {
                remove_player(slot);
                gone(slot, true);
            } else {
                // A remote human: every record of its machine group (+0xC), active or not
                // (freed records have group 0); each gets the leaving byte.
                const std::int32_t group = room_[s].group;
                for (int k = 0; k < 10 && static_cast<std::size_t>(k) < battle_->players.size(); ++k) {
                    if (room_[static_cast<std::size_t>(k)].group != group) continue;
                    const bool was = battle_->players[static_cast<std::size_t>(k)].active;
                    remove_player(k);
                    gone(k, was);
                }
            }
        }
    }
    leaving_[s] = reason;
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
    // The session's own messages first: a destroyed player of the game (system message 5, a
    // player: 0x453D40's system branch) is sent away with reason 1 (0x453010: its status 0x1B
    // to everybody, 0x452CC0 by the rules there) and its record gets controller 0 (0x463C60).
    // (The original sends no 0x1C for it; the host also refreshes the session's description.)
    SessionEvent ev;
    while (transport_.next_event(ev)) {
        if (ev.kind != SessionEvent::Kind::PlayerDestroyed) continue;
        const int s = slot_of(ev.id);
        if (s == 10) continue;
        sim::PlayerSlot& lp = battle_->players[static_cast<std::size_t>(s)];
        if (!lp.active || lp.controller < 1 || lp.controller > 3 || lp.state == 10) continue;
        send_away(s, room::kLeft);
        lp.controller = 0;
        room_[static_cast<std::size_t>(s)].kind = 0;
    }
    const FrameReceiver::ReadFn read = [this](std::int32_t& f, std::int32_t& t, std::vector<std::uint8_t>& m) {
        Message msg;
        if (!transport_.receive(msg)) return false;
        counters_.bytes_received += static_cast<std::uint32_t>(msg.bytes.size());
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
        if (from == 10) {
            // (The original reads a "record 10" past the table here, GS+0x2851; 0x453010 on the
            // unknown id returns before sending anything.)
            ++counters_.refused;
            continue;
        }
        const sim::PlayerSlot& fp = battle_->players[static_cast<std::size_t>(from)];
        if (!(fp.active && fp.controller == 3 && fp.state != 10)) {
            // A known sender that is not a present remote player goes to 0x453010(sender, 6)
            // (a local one is only ignored): an inactive record just gets the leaving byte; one
            // out of the game (state 10) is sent away (status 6 to everybody, its group if human).
            if (fp.controller != 1 && fp.controller != 2) send_away(from, 6);
            ++counters_.refused;
            continue;
        }
        if (to == 10 || !battle_->players[static_cast<std::size_t>(to)].active || battle_->players[static_cast<std::size_t>(to)].state == 10) {
            ++counters_.refused;  // the addressee is no record in the game: dropped, nothing said
            continue;
        }
        last_heard_[static_cast<std::size_t>(from)] = clock_;
        ++counters_.packets_received;
        if (type < counters_.by_type.size()) ++counters_.by_type[type];
        dispatch(from, to, d.packet);
        if (type == packet::kGameStart) break;
    }
    // 0x450980: two playing players in one group switch broadcasts to one copy per group.
    if (have_room_) {
        std::array<int, 11> count{};
        per_group_ = false;
        for (int s = 0; s < 10; ++s) {
            const sim::PlayerSlot& q = battle_->players[static_cast<std::size_t>(s)];
            const std::int32_t g = room_[static_cast<std::size_t>(s)].group;
            if (!q.active || q.controller < 1 || q.controller > 3 || q.state == 10 || g < 1 || g > 10) continue;
            if (++count[static_cast<std::size_t>(g)] >= 2) per_group_ = true;
        }
    }
    check_timeouts();
    check_watch();
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
    case packet::kProbeReply:
        // Case 5: the sender record's byte +0x21 gets bit 0 (never read by the original).
        record21_[static_cast<std::size_t>(from)] |= 1;
        break;
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
        // (0x453D40 -> 0x453010: the named player is sent away here too, the status passed on)
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s != 10) send_away(s, p[5]);
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
        // 0x457540: the totals are kept unless a local player already has this sender's final
        // ones (+0x129); an ask (byte 1) is answered by each local player with 0x29 {1, whether
        // the sender acknowledged ours} and, while this machine has not won, its own final
        // totals until the sender acknowledged them.
        bool frozen = false;
        for (int l = 0; l < 10; ++l)
            if (plays(l) && stats_got_[static_cast<std::size_t>(l)][static_cast<std::size_t>(from)]) frozen = true;
        if (!frozen) {
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
        }
        if (p[1] != 0) {
            const bool won = b.victory_met();
            for (int l = 0; l < 10; ++l) {
                if (!plays(l) || leaving_[static_cast<std::size_t>(l)]) continue;
                const std::size_t L = static_cast<std::size_t>(l), F = static_cast<std::size_t>(from);
                stats_got_[L][F] = 1;
                u8 ack[3] = {packet::kViewAck, 1, static_cast<u8>(stats_acked_[L][F] ? 1 : 0)};
                send_to(b.players[L].net_id, b.players[F].net_id, ack, 3);
                if (!won && !stats_acked_[L][F]) send_view_state(l, from, true);
            }
        }
        break;
    }
    case packet::kViewAck:
        // 0x29: the addressee's record notes that the sender has its final totals (+0x11E) and,
        // with byte 2, that the sender had its acknowledgement (+0x134).
        if (p[1] != 0) {
            stats_acked_[static_cast<std::size_t>(to)][static_cast<std::size_t>(from)] = 1;
            if (p[2] != 0) stats_told_[static_cast<std::size_t>(to)][static_cast<std::size_t>(from)] = 1;
        }
        break;
    case packet::kUnitStates:
        last_state_tick_[static_cast<std::size_t>(from)] = b.apply_unit_state_packet(from, p, need);
        break;
    case packet::kSideRecord: {
        // A remote record during the battle (its watch mode, for one): kept and applied.
        if (plays(from)) break;
        RoomPlayer r = decode_side_record(p + 1);
        RoomPlayer& q = room_[static_cast<std::size_t>(from)];
        r.net_id = b.players[static_cast<std::size_t>(from)].net_id;
        r.ally_team = q.ally_team;
        r.group = q.group;
        r.start_position = q.start_position;
        q = r;
        apply_side_record(from, r);
        break;
    }
    case packet::kAllyTeam: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s != 10 && !plays(s)) room_[static_cast<std::size_t>(s)].ally_team = p[5];
        break;
    }
    case packet::kModifiedExe: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s != 10 && on_modified_executable) on_modified_executable(s);
        break;
    }
    case packet::kDisconnect: {
        // Case 0x1A: a player this machine knows (controller not 0) is shown as "Player %s has
        // disconnected"; it goes through 0x452CC0 when it is another machine's or the loading
        // is not under way (GS+0x38D75 bit 0 clear or bit 1 set), and this machine passes the
        // 0x1C on from its own player (GS+0x2A42) once (afterwards the record is not found).
        // Naming this machine's human sets GS+0x3923B bit 2. No leaving byte is written.
        const std::int32_t id = static_cast<std::int32_t>(get32(p + 1));
        const int s = slot_of(id);
        if (s != 10) {
            if (on_disconnected) on_disconnected(s, from);
            const sim::PlayerSlot& q = b.players[static_cast<std::size_t>(s)];
            if ((q.active && q.controller == 3) || phase_ != Phase::Loading) remove_player(s);
            u8 pk[5] = {packet::kDisconnect};
            put32(pk + 1, static_cast<std::uint32_t>(id));
            const int l = b.settings.local_player;
            if (l >= 0 && l < 10) broadcast(b.players[static_cast<std::size_t>(l)].net_id, pk, 5);
            for (int k = 0; k < 10; ++k)
                if (b.players[static_cast<std::size_t>(k)].controller == 1) {
                    if (b.players[static_cast<std::size_t>(k)].net_id == id) disconnected_ = true;
                    break;
                }
        }
        break;
    }
    default:
        break;  // battle room and loading packets (net/lobby.h) and the ignored ones
    }
}

} // namespace net
