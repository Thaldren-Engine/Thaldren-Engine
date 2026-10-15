#include "net/lobby.h"

#include <algorithm>
#include <cstring>

#include "net/protocol.h"

namespace net {

using u8 = std::uint8_t;

namespace {
// Placements of the room fields the original's code was not seen to read (see lobby.h).
constexpr std::size_t kName = 0x40, kNameLen = 0x20, kColour = 0x99, kTeam = 0xA0, kEnergy = 0xA1, kMetal = 0xA3;
constexpr std::size_t kMapLen = 0x40;
}  // namespace

std::vector<std::uint8_t> encode_side_record(const RoomPlayer& p) {
    std::vector<u8> r(room::kRecordSize, 0);
    std::memcpy(r.data(), p.map.data(), std::min(p.map.size(), kMapLen - 1));
    std::memcpy(r.data() + kName, p.name.data(), std::min(p.name.size(), kNameLen - 1));
    r[0x8F] = p.map_ready ? 1 : 0;
    r[0x94] = p.kind;
    r[0x95] = p.side;
    r[0x96] = static_cast<u8>(p.start_position);
    r[0x97] = p.flags;
    r[kColour] = p.colour;
    // The map flags are the option word's bits 8-10 (+0x9C, its high byte).
    put16(&r[0x9B], static_cast<std::uint16_t>((p.options & ~0x0700u) | ((p.map_flags & 7u) << 8)));
    r[0x9D] = 4;  // has the game CD
    r[kTeam] = p.team;
    put16(&r[kEnergy], p.energy);
    put16(&r[kMetal], p.metal);
    put16(&r[0xA5], p.unit_limit);
    return r;
}

RoomPlayer decode_side_record(const std::uint8_t* r) {
    RoomPlayer p;
    p.map.assign(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), kMapLen));
    p.name.assign(reinterpret_cast<const char*>(r + kName), strnlen(reinterpret_cast<const char*>(r + kName), kNameLen));
    p.map_ready = r[0x8F] != 0;
    p.kind = r[0x94];
    p.side = r[0x95];
    p.start_position = static_cast<std::int8_t>(r[0x96]);
    p.flags = r[0x97];
    p.colour = r[kColour];
    p.options = get16(r + 0x9B);
    p.map_flags = static_cast<std::uint8_t>(r[0x9C] & 7);
    p.team = r[kTeam];
    p.energy = get16(r + kEnergy);
    p.metal = get16(r + kMetal);
    p.unit_limit = get16(r + 0xA5);
    return p;
}

BattleRoom::BattleRoom(Transport& transport, bool host, std::int32_t net_id, const std::string& name)
    : transport_(transport), host_(host), net_id_(net_id) {
    transport_.set_local_players({net_id});
    if (host) {
        local_slot_ = 0;
        host_id_ = net_id;
        RoomPlayer& p = players_[0];
        p.net_id = net_id;
        p.name = name;
        p.kind = 1;
        p.flags = room::kHost;
        p.map = "Moon Quartet";
    } else {
        pending_name_ = name;
    }
}

SendFn BattleRoom::send_fn() {
    return [this](std::int32_t from, std::int32_t to, std::vector<std::uint8_t> m) { transport_.send(from, to, std::move(m)); };
}

int BattleRoom::slot_of(std::int32_t id) const {
    for (int s = 0; s < 10; ++s)
        if (players_[static_cast<std::size_t>(s)].net_id == id && id != 0) return s;
    return -1;
}

void BattleRoom::send_all(std::int32_t from, const std::uint8_t* p, std::size_t n) { sender_.queue(from, kEverybody, p, n, clock_, send_fn()); }
void BattleRoom::send_one(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n) { sender_.queue(from, to, p, n, clock_, send_fn()); }

void BattleRoom::set_side(std::uint8_t side) {
    if (local_slot_ >= 0) me().side = side;
    pending_side_ = side;
    dirty_ = true;
}
void BattleRoom::set_colour(std::uint8_t colour) {
    if (local_slot_ >= 0) me().colour = colour;
    pending_colour_ = colour;
    dirty_ = true;
}
void BattleRoom::set_ready(bool ready) {
    if (local_slot_ < 0) return;
    me().options = static_cast<std::uint16_t>((me().options & ~room::kReady) | (ready ? room::kReady : 0));
    dirty_ = true;
}
void BattleRoom::set_team(std::uint8_t team) {
    if (local_slot_ >= 0) me().team = team;
    dirty_ = true;
}
void BattleRoom::set_start_position(std::int8_t position) {
    if (local_slot_ >= 0) me().start_position = position;
    dirty_ = true;
}
void BattleRoom::set_map(const std::string& map) {
    if (!host_) return;
    for (RoomPlayer& p : players_)
        if (p.net_id && (p.flags & room::kHost || p.kind == 2)) p.map = map;
    dirty_ = true;
}
void BattleRoom::set_options(std::uint8_t commander_mode, std::uint8_t map_flags, std::uint16_t unit_limit, std::uint16_t energy,
                             std::uint16_t metal, bool fixed) {
    if (!host_) return;
    RoomPlayer& h = players_[0];
    h.options = static_cast<std::uint16_t>((h.options & ~(room::kCommanderMask | room::kFixedLocations)) | ((commander_mode & 3) << 11) |
                                           (fixed ? room::kFixedLocations : 0));
    h.map_flags = map_flags;
    h.unit_limit = unit_limit;
    h.energy = energy;
    h.metal = metal;
    dirty_ = true;
}

int BattleRoom::add_computer(std::uint8_t side) {
    if (!host_) return -1;
    for (int s = 0; s < 10; ++s) {
        RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (p.net_id) continue;
        p = RoomPlayer{};
        p.net_id = net_id_ + 0x1000 + s;  // the host creates the computer players' ids
        p.name = "Computer " + std::to_string(s + 1);
        p.kind = 2;
        p.side = side;
        p.colour = static_cast<u8>(s);
        p.options = room::kNonCampaign | room::kReady;
        p.map = players_[0].map;
        p.map_ready = true;
        std::vector<std::int32_t> ids;
        for (const RoomPlayer& q : players_)
            if (q.net_id && (q.net_id == net_id_ || q.kind == 2)) ids.push_back(q.net_id);
        transport_.set_local_players(ids);
        dirty_ = true;
        return s;
    }
    return -1;
}

void BattleRoom::say(const std::string& text) {
    u8 pk[65] = {packet::kChat};
    std::memcpy(pk + 1, text.data(), std::min<std::size_t>(text.size(), 63));
    send_all(net_id_, pk, 65);
}

void BattleRoom::broadcast_records() {
    // Each machine sends its own player's record (the host also its computer players').
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (!p.net_id) continue;
        const bool mine = p.net_id == net_id_ || (host_ && p.kind == 2);
        if (!mine) continue;
        u8 pk[186] = {packet::kSideRecord};
        const auto r = encode_side_record(p);
        std::memcpy(pk + 1, r.data(), r.size());
        send_all(p.net_id, pk, 186);
    }
    if (local_slot_ < 0) {
        // Not seated yet: the join request is our record.
        RoomPlayer p;
        p.net_id = net_id_;
        p.name = pending_name_;
        p.kind = 1;
        p.side = pending_side_;
        p.colour = pending_colour_;
        u8 pk[186] = {packet::kSideRecord};
        const auto r = encode_side_record(p);
        std::memcpy(pk + 1, r.data(), r.size());
        send_all(net_id_, pk, 186);
    }
    if (host_) {
        u8 pk[41] = {packet::kSlotTable};
        for (int s = 0; s < 10; ++s) put32(pk + 1 + 4 * s, static_cast<std::uint32_t>(players_[static_cast<std::size_t>(s)].net_id));
        send_all(net_id_, pk, 41);
    }
}

void BattleRoom::frame(std::uint32_t clock) {
    clock_ = clock;
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
    while (receiver_.next(0, sender_.timing().frame_timing, read, d))
        if (!d.packet.empty()) handle(d.from, d.packet);
    // Every two seconds (60 ticks): ping, keep-alive and the records (0x456310); at once on a change.
    if (clock_ >= next_periodic_ || dirty_) {
        if (clock_ >= next_periodic_) {
            next_periodic_ = clock_ + 60;
            u8 ping[13] = {packet::kPing};
            put32(ping + 1, clock_ * 1000u / 30u + 1u);
            put32(ping + 9, static_cast<std::uint32_t>(net_id_));
            send_all(net_id_, ping, 13);
            u8 probe[1] = {packet::kProbe};
            send_all(net_id_, probe, 1);
        }
        dirty_ = false;
        broadcast_records();
    }
    sync_tick();
    sender_.flush(clock_, true, send_fn());
}

void BattleRoom::sync_tick() {
    // 0x46DAD0. Clients: once the host spoke, the definition count, then four definitions per
    // pass, then the count of the host's answers. The host greets new human players.
    if (host_) {
        for (const RoomPlayer& p : players_) {
            if (!p.net_id || p.kind != 1 || p.net_id == net_id_) continue;
            if (std::none_of(peers_.begin(), peers_.end(), [&](const PeerChecks& c) { return c.id == p.net_id; })) {
                peers_.push_back(PeerChecks{p.net_id});
                u8 pk[14] = {packet::kSync, 0};
                send_one(net_id_, p.net_id, pk, 14);
            }
        }
        return;
    }
    if (sync_heard_ == 0 || host_id_ == 0) return;
    u8 pk[14] = {packet::kSync};
    if (!sync_hello_) {
        pk[1] = 1;
        put32(pk + 10, static_cast<std::uint32_t>(checks_.size()));
        send_one(net_id_, host_id_, pk, 14);
        sync_hello_ = true;
        return;
    }
    for (int k = 0; k < 4 && sync_sent_ < checks_.size(); ++k, ++sync_sent_) {
        pk[1] = 2;
        put32(pk + 6, checks_[sync_sent_].first);
        put32(pk + 10, checks_[sync_sent_].second);
        send_one(net_id_, host_id_, pk, 14);
    }
    if (sync_sent_ >= checks_.size()) {
        pk[1] = 4;
        put32(pk + 6, 0);
        put32(pk + 10, sync_heard_);
        send_one(net_id_, host_id_, pk, 14);
    }
}

void BattleRoom::handle(std::int32_t from, const std::vector<std::uint8_t>& v) {
    const u8* p = v.data();
    const std::size_t need = packet_size_at(p, v.size());
    if (need == 0 || v.size() < need) return;
    if (!(packet_phases(p[0]) & static_cast<u8>(Phase::Room))) return;
    const int fs = slot_of(from);
    if (fs >= 0) players_[static_cast<std::size_t>(fs)].last_heard = clock_;
    switch (p[0]) {
    case packet::kPing:
        if (get32(p + 5) == 0) {
            std::vector<u8> echo(v.begin(), v.begin() + 13);
            put32(echo.data() + 5, clock_ * 1000u / 30u + 1u);
            send_one(net_id_, static_cast<std::int32_t>(get32(p + 9)), echo.data(), 13);
        } else if (fs >= 0) {
            players_[static_cast<std::size_t>(fs)].latency_ms = clock_ * 1000u / 30u + 1u - get32(p + 1);
        }
        break;
    case packet::kChat:
        if (on_chat) {
            const char* s = reinterpret_cast<const char*>(p + 1);
            const std::string who = fs >= 0 ? players_[static_cast<std::size_t>(fs)].name : std::string("?");
            on_chat(who + ": " + std::string(s, strnlen(s, 64)));
        }
        break;
    case packet::kProbe: {
        u8 r[1] = {packet::kProbeReply};
        send_one(net_id_, from, r, 1);
        break;
    }
    case packet::kSideRecord: {
        RoomPlayer r = decode_side_record(p + 1);
        r.net_id = from;
        if (fs < 0) {
            if (!host_) break;  // the host seats newcomers; others wait for the slot table
            for (int s = 0; s < 10; ++s) {
                RoomPlayer& q = players_[static_cast<std::size_t>(s)];
                if (q.net_id) continue;
                r.last_heard = clock_;
                q = r;
                q.map = players_[0].map;
                dirty_ = true;
                break;
            }
            break;
        }
        RoomPlayer& q = players_[static_cast<std::size_t>(fs)];
        const std::uint32_t heard = q.last_heard, lat = q.latency_ms;
        const std::string keep_map = q.map;
        q = r;
        q.last_heard = heard;
        q.latency_ms = lat;
        if (host_) q.map = keep_map;  // the host's map is the game's
        break;
    }
    case packet::kSlotTable: {
        if (host_) break;
        host_id_ = from;
        std::array<RoomPlayer, 10> next{};
        for (int s = 0; s < 10; ++s) {
            const std::int32_t id = static_cast<std::int32_t>(get32(p + 1 + 4 * s));
            if (!id) continue;
            const int old = slot_of(id);
            next[static_cast<std::size_t>(s)] = old >= 0 ? players_[static_cast<std::size_t>(old)] : RoomPlayer{};
            next[static_cast<std::size_t>(s)].net_id = id;
            if (id == net_id_ && local_slot_ < 0) {
                RoomPlayer& m = next[static_cast<std::size_t>(s)];
                m.name = pending_name_;
                m.kind = 1;
                m.side = pending_side_;
                m.colour = pending_colour_;
                m.options = room::kNonCampaign;
                dirty_ = true;
            }
        }
        for (int s = 0; s < 10; ++s) {
            const std::int32_t id = players_[static_cast<std::size_t>(s)].net_id;
            if (id && std::none_of(next.begin(), next.end(), [&](const RoomPlayer& q) { return q.net_id == id; }) && on_player_left) on_player_left(s);
        }
        players_ = next;
        local_slot_ = slot_of(net_id_);
        break;
    }
    case packet::kStatus: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s >= 0) {
            players_[static_cast<std::size_t>(s)] = RoomPlayer{};
            if (on_player_left) on_player_left(s);
            dirty_ = true;
        }
        break;
    }
    case packet::kSync: {
        const u8 sub = p[1];
        if (sub >= 100) break;
        if (!host_) {
            ++sync_heard_;
            if (sub == 3 && p[10] == 0) disabled_.push_back(get32(p + 6));
            break;
        }
        auto it = std::find_if(peers_.begin(), peers_.end(), [&](const PeerChecks& c) { return c.id == from; });
        if (it == peers_.end()) break;
        if (sub == 1) {
            it->expected = get32(p + 10);
            it->counted = true;
        }
        else if (sub == 4) it->acked = std::max(it->acked, get32(p + 10));
        else if (sub == 2) {
            // 0x46D970: the definition is enabled when the host has it with the same checksum.
            const std::uint32_t key = get32(p + 6), sum = get32(p + 10);
            it->list.push_back({key, sum});
            ++it->got;
            const auto own = std::find_if(checks_.begin(), checks_.end(), [&](const auto& c) { return c.first == key; });
            const bool enabled = own != checks_.end() && own->second == sum;
            if (!enabled) disabled_.push_back(key);
            u8 pk[14] = {packet::kSync, 3};
            put32(pk + 6, key);
            pk[10] = enabled ? 1 : 0;
            pk[11] = 1;
            put16(pk + 12, 0xFFFF);
            for (const PeerChecks& c : peers_) send_one(net_id_, c.id, pk, 14);
        }
        break;
    }
    case packet::kStartPosition:
        if (!host_ && from == host_id_ && local_slot_ >= 0) {
            me().start_position = static_cast<std::int8_t>(p[1]);
            dirty_ = true;
        }
        break;
    case packet::kGameStart:
        if (!host_ && from == host_id_) started_ = true;
        break;
    default:
        break;
    }
}

bool BattleRoom::can_start() const {
    if (!host_) return false;
    int humans = 0;
    for (const RoomPlayer& p : players_) {
        if (!p.net_id) continue;
        if (p.kind == 1) ++humans;
        if (p.kind == 1 && p.net_id != net_id_ && (!p.ready() || !p.map_ready)) return false;
    }
    for (const PeerChecks& c : peers_)
        if (!c.counted || c.got < c.expected) return false;
    return humans >= 1;
}

bool BattleRoom::start() {
    if (!can_start()) {
        error_ = "not everybody is ready";
        return false;
    }
    // Start positions: the chosen ones, else in slot order (fixed) as the host assigns them.
    int next = 0;
    for (RoomPlayer& p : players_)
        if (p.net_id && p.start_position < 0) p.start_position = static_cast<std::int8_t>(next++);
        else if (p.net_id) next = std::max(next, p.start_position + 1);
    for (RoomPlayer& p : players_)
        if (p.net_id && p.net_id != net_id_ && p.kind == 1) {
            // A human's start position travels in the host's packet 0x17 (slot, position).
            u8 pk[2] = {packet::kStartPosition, static_cast<u8>(p.start_position)};
            send_one(net_id_, p.net_id, pk, 2);
        }
    broadcast_records();
    u8 go[1] = {packet::kGameStart};
    send_all(net_id_, go, 1);
    sender_.flush(clock_, true, send_fn());
    started_ = true;
    return true;
}

std::string BattleRoom::map() const {
    for (const RoomPlayer& p : players_)
        if (p.net_id && (p.flags & room::kHost)) return p.map;
    return players_[0].map;
}

std::array<std::string, 10> BattleRoom::sides() const {
    std::array<std::string, 10> s{};
    for (int k = 0; k < 10; ++k)
        if (players_[static_cast<std::size_t>(k)].net_id) s[static_cast<std::size_t>(k)] = players_[static_cast<std::size_t>(k)].side ? "CORE" : "ARM";
    return s;
}

sim::BattleStart BattleRoom::battle_start() const {
    sim::BattleStart b;
    const RoomPlayer* hostp = nullptr;
    for (const RoomPlayer& p : players_)
        if (p.net_id && (p.flags & room::kHost)) hostp = &p;
    const RoomPlayer& h = hostp ? *hostp : players_[0];
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (!p.net_id) continue;
        sim::StartPlayer& sp = b.players[static_cast<std::size_t>(s)];
        const bool mine = p.net_id == net_id_ || (host_ && p.kind == 2);
        sp.kind = !mine ? sim::PlayerKind::Remote : (p.kind == 2 ? sim::PlayerKind::Computer : sim::PlayerKind::Local);
        sp.net_id = p.net_id;
        sp.start_position = p.start_position;
        sp.colour = p.colour;
        sp.ally_group = p.team;
        sp.name = p.name;
        sp.watcher = (p.options & room::kWatch) != 0;
    }
    b.provider = 3;
    b.net_game = true;
    b.local_player = static_cast<std::uint8_t>(local_slot_ < 0 ? 0 : local_slot_);
    b.unit_limit = h.unit_limit;
    b.commander_mode = (h.options & room::kCommanderMask) >> 11;
    b.explore = (h.map_flags & room::kMapping) != 0;
    b.fog_of_war = (h.map_flags & room::kLineOfSight) != 0;
    b.true_sight = (h.map_flags & room::kTrueSight) != 0;
    b.fixed_locations = (h.options & room::kFixedLocations) != 0;
    b.start_energy = static_cast<float>(h.energy) * 100.0f;
    b.start_metal = static_cast<float>(h.metal) * 100.0f;
    return b;
}

} // namespace net
