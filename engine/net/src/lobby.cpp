#include "net/lobby.h"

#include <algorithm>
#include <cstring>

#include "net/protocol.h"

namespace net {

using u8 = std::uint8_t;

namespace {
constexpr std::size_t kMapLen = 0x40, kName = 0x40, kNameLen = 0x20;  // name: a place the original was not seen to read
}  // namespace

std::vector<std::uint8_t> encode_side_record(const RoomPlayer& p) {
    std::vector<u8> r(room::kRecordSize, 0);
    std::memcpy(r.data(), p.map.data(), std::min(p.map.size(), kMapLen - 1));
    std::memcpy(r.data() + kName, p.name.data(), std::min(p.name.size(), kNameLen - 1));
    r[0x8F] = p.map_ready ? 1 : 0;
    r[0x94] = p.kind;
    r[0x95] = p.side;
    r[0x96] = p.logo;
    r[0x97] = p.flags;
    put16(&r[0x99], p.memory_mb);
    // The map flags are the option word's bits 8-10 (+0x9C, its high byte).
    put16(&r[0x9B], static_cast<std::uint16_t>((p.options & ~0x0700u) | ((p.map_flags & 7u) << 8)));
    r[0x9D] = 4;  // has the game CD
    put16(&r[0xA1], p.energy);
    put16(&r[0xA3], p.metal);
    put16(&r[0xA5], p.unit_limit);
    r[0xA7] = p.version_major;
    r[0xA8] = p.version_minor;
    put32(&r[0xA9], p.map_checksum);
    return r;
}

RoomPlayer decode_side_record(const std::uint8_t* r) {
    RoomPlayer p;
    p.map.assign(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), kMapLen));
    p.name.assign(reinterpret_cast<const char*>(r + kName), strnlen(reinterpret_cast<const char*>(r + kName), kNameLen));
    p.map_ready = r[0x8F] != 0;
    p.kind = r[0x94];
    p.side = r[0x95];
    p.logo = r[0x96];
    p.flags = r[0x97];
    p.memory_mb = get16(r + 0x99);
    p.options = get16(r + 0x9B);
    p.map_flags = static_cast<std::uint8_t>(r[0x9C] & 7);
    p.energy = get16(r + 0xA1);
    p.metal = get16(r + 0xA3);
    p.unit_limit = get16(r + 0xA5);
    p.version_major = r[0xA7];
    p.version_minor = r[0xA8];
    p.map_checksum = get32(r + 0xA9);
    return p;
}

BattleRoom::BattleRoom(Transport& transport, bool host, std::int32_t net_id, const std::string& name)
    : transport_(transport), host_(host), net_id_(net_id) {
    transport_.set_local_players({net_id});
    positions_.fill(-1);
    if (host) {
        local_slot_ = 0;
        host_id_ = net_id;
        RoomPlayer& p = players_[0];
        p.net_id = net_id;
        p.name = name;
        p.kind = 1;
        p.flags = room::kHost;
        p.map = "Moon Quartet";
        p.logo = 0;
        p.group = 1;  // the host's machine is group 1 (0x450530)
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

std::int32_t BattleRoom::first_human_id() const {
    for (const RoomPlayer& p : players_)
        if (p.net_id && p.kind == 1 && mine(p)) return p.net_id;
    return net_id_;
}

void BattleRoom::send_all(std::int32_t from, const std::uint8_t* p, std::size_t n) { sender_.queue(from, kEverybody, p, n, clock_, send_fn()); }
void BattleRoom::send_one(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n) { sender_.queue(from, to, p, n, clock_, send_fn()); }

std::vector<std::uint32_t> BattleRoom::disabled_units() const {
    std::vector<std::uint32_t> out;
    for (const auto& [k, on] : verdicts_)
        if (!on) out.push_back(k);
    return out;
}

void BattleRoom::set_side(std::uint8_t side) {
    if (local_slot_ >= 0) me().side = side;
    pending_side_ = side;
    dirty_ = true;
}

void BattleRoom::set_logo(std::uint8_t logo) {
    // 0x4526C0: the host grants its own; others ask the host (0x17).
    if (local_slot_ < 0) return;
    if (host_) {
        grant_logo(net_id_, logo);
        return;
    }
    u8 pk[2] = {packet::kLogoAsk, logo};
    send_one(net_id_, host_id_, pk, 2);
    sender_.flush(clock_, true, send_fn());
}

void BattleRoom::next_logo() {
    if (local_slot_ < 0) return;
    set_logo(static_cast<u8>(me().logo == 0xFF ? 0 : me().logo + 1));
}

void BattleRoom::grant_logo(std::int32_t asker, std::uint8_t wanted) {
    // 0x452570 / 0x4523E0: the asked logo when nobody else (present) holds it, else the first
    // free one from it upward (wrapping at 10); granted with 0x18.
    std::uint8_t logo = wanted;
    for (int tries = 0; tries < 10; ++tries) {
        if (logo > 9) logo = 0;
        bool taken = false;
        for (const RoomPlayer& p : players_)
            if (p.net_id && p.net_id != asker && p.logo == logo) taken = true;
        if (!taken) break;
        ++logo;
    }
    if (logo > 9) logo = 0;
    const int s = slot_of(asker);
    if (s >= 0) players_[static_cast<std::size_t>(s)].logo = logo;
    if (s >= 0 && mine(players_[static_cast<std::size_t>(s)])) {
        dirty_ = true;
        return;
    }
    u8 pk[2] = {packet::kLogoGrant, logo};
    send_one(first_human_id(), asker, pk, 2);
}

void BattleRoom::set_ready(bool ready) {
    if (local_slot_ < 0) return;
    me().options = static_cast<std::uint16_t>((me().options & ~room::kReady) | (ready ? room::kReady : 0));
    dirty_ = true;
}

void BattleRoom::set_ally_team(std::uint8_t team) {
    if (local_slot_ < 0) return;
    me().ally_team = team > room::kNoTeam ? room::kNoTeam : team;
    dirty_ = true;
}

void BattleRoom::next_ally_team() {
    if (local_slot_ >= 0) set_ally_team(static_cast<u8>((me().ally_team + 1) % 6));
}

void BattleRoom::set_watch(bool watch) {
    if (local_slot_ < 0) return;
    me().options = static_cast<std::uint16_t>((me().options & ~room::kWatch) | (watch ? room::kWatch : 0));
    dirty_ = true;
}

void BattleRoom::set_memory(std::uint16_t mb) {
    if (local_slot_ >= 0) me().memory_mb = mb;
    dirty_ = true;
}

void BattleRoom::set_map(const std::string& map, std::uint32_t checksum) {
    if (!host_) return;
    for (RoomPlayer& p : players_)
        if (p.net_id && mine(p)) {
            p.map = map;
            p.map_checksum = checksum;
        }
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

void BattleRoom::set_closed(bool closed) {
    if (!host_) return;
    players_[0].options = static_cast<std::uint16_t>((players_[0].options & ~room::kClosed) | (closed ? room::kClosed : 0));
    dirty_ = true;
}

int BattleRoom::add_computer(std::uint8_t side) {
    if (!host_) return -1;
    for (int s = 0; s < 10; ++s) {
        RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (p.net_id) continue;
        p = RoomPlayer{};
        p.net_id = net_id_ + 0x1000 + s;  // the host's session creates the computer players
        p.name = "Computer " + std::to_string(s + 1);
        p.kind = 2;
        p.side = side;
        p.options = room::kNonCampaign | room::kReady;
        p.map = players_[0].map;
        p.map_checksum = players_[0].map_checksum;
        p.map_ready = true;
        p.group = players_[0].group;  // a computer player joins its machine's group
        p.logo = 0xFF;
        std::vector<std::int32_t> ids;
        for (const RoomPlayer& q : players_)
            if (q.net_id && mine(q)) ids.push_back(q.net_id);
        transport_.set_local_players(ids);
        grant_logo(p.net_id, static_cast<u8>(s));
        dirty_ = true;
        return s;
    }
    return -1;
}

void BattleRoom::say(const std::string& text) {
    u8 pk[65] = {packet::kChat};
    std::memcpy(pk + 1, text.data(), std::min<std::size_t>(text.size(), 63));
    send_all(first_human_id(), pk, 65);
}

void BattleRoom::broadcast_records() {
    // 0x456310 / 0x450F90: each machine sends its own players' side records and ally teams; the
    // host also the slot table.
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (!p.net_id || !mine(p)) continue;
        u8 pk[186] = {packet::kSideRecord};
        const auto r = encode_side_record(p);
        std::memcpy(pk + 1, r.data(), r.size());
        send_all(p.net_id, pk, 186);
        u8 at[6] = {packet::kAllyTeam};
        put32(at + 1, static_cast<std::uint32_t>(p.net_id));
        at[5] = p.ally_team;
        send_all(p.net_id, at, 6);
    }
    if (local_slot_ < 0 && !refused_) {
        // Not seated yet: the join request is our record.
        RoomPlayer p;
        p.net_id = net_id_;
        p.name = pending_name_;
        p.kind = 1;
        p.side = pending_side_;
        p.map_ready = pending_map_ready_;
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
    // 0x450530 (outside the battle): players without a group ask the host for one.
    if (!host_ && host_id_ != 0)
        for (const RoomPlayer& p : players_) {
            if (!p.net_id || p.group != 0) continue;
            u8 pk[10] = {packet::kGroupAsk};
            put32(pk + 2, static_cast<std::uint32_t>(p.net_id));
            put32(pk + 6, 0xFFFFFFFFu);
            if (p.net_id == net_id_) pk[1] = 1;        // a new group for this machine
            else if (mine(p)) {                         // (computer players join this machine's)
                pk[1] = 1;
                put32(pk + 6, static_cast<std::uint32_t>(net_id_));
            } else pk[1] = 0;                           // ask the host which group it has
            send_one(first_human_id(), host_id_, pk, 10);
        }
}

void BattleRoom::frame(std::uint32_t clock, sim::Lcg15* crt_rand) {
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
        if (!d.packet.empty()) handle(d.from, d.to, d.packet);
    if (!started_ && (clock_ >= next_periodic_ || dirty_)) {
        if (clock_ >= next_periodic_) {
            next_periodic_ = clock_ + 60;  // every two seconds (0x456310)
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
    if (!started_) sync_tick();
    if (started_ && !loaded_) loading_tick(crt_rand);
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

void BattleRoom::judge_unit(std::uint32_t key) {
    // 0x46D970: a definition stays buildable only when every human that takes part has it, and
    // (for players of version 3.1, +0xA7/+0xA8) with the host's checksum. The verdict goes to
    // every human (sub 3).
    const auto own = std::find_if(checks_.begin(), checks_.end(), [&](const auto& c) { return c.first == key; });
    bool enabled = own != checks_.end();
    for (const PeerChecks& c : peers_) {
        const auto it = c.list.find(key);
        if (it == c.list.end()) {
            enabled = false;
            break;
        }
        const int s = slot_of(c.id);
        const RoomPlayer* p = s >= 0 ? &players_[static_cast<std::size_t>(s)] : nullptr;
        const bool compare = p && (p->version_major >= 2 || (p->version_major == 1 && p->version_minor >= 2));
        if (compare && own != checks_.end() && it->second != own->second) {
            enabled = false;
            break;
        }
    }
    verdicts_[key] = enabled;
    u8 pk[14] = {packet::kSync, 3};
    put32(pk + 6, key);
    pk[10] = enabled ? 1 : 0;
    pk[11] = 1;
    put16(pk + 12, players_[0].unit_limit);
    for (const PeerChecks& c : peers_) send_one(net_id_, c.id, pk, 14);
}

void BattleRoom::loading_tick(sim::Lcg15* crt_rand) {
    // 0x4568C0, once per pass while loading.
    if (host_ && !positions_made_) {
        std::vector<std::int32_t> ids;
        std::vector<int> slots;
        for (int s = 0; s < 10; ++s) {
            const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
            if (p.net_id && !p.watching()) {
                slots.push_back(s);
                ids.push_back(static_cast<std::int32_t>(ids.size()));
            }
        }
        if (!(players_[0].options & room::kFixedLocations)) {
            // The shuffle draws this machine's C runtime rand() (the simulation thread's).
            std::uint32_t state = crt_rand ? crt_rand->state() : 0;
            sim::shuffle_start_positions(ids, state);
            if (crt_rand) crt_rand->set_state(state);
        }
        for (std::size_t k = 0; k < slots.size(); ++k) positions_[static_cast<std::size_t>(slots[k])] = ids[k];
        positions_made_ = true;
    }
    bool done = true;
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (!p.net_id || mine(p)) continue;
        if (!heard_loaded_[static_cast<std::size_t>(s)] || (host_ && !position_acked_[static_cast<std::size_t>(s)])) done = false;
    }
    if (host_)
        for (int s = 0; s < 10; ++s) {
            RoomPlayer& p = players_[static_cast<std::size_t>(s)];
            if (!p.net_id || position_acked_[static_cast<std::size_t>(s)]) continue;
            if (mine(p)) {
                p.start_position = static_cast<std::int8_t>(positions_[static_cast<std::size_t>(s)]);
                position_acked_[static_cast<std::size_t>(s)] = true;
            } else {
                u8 pk[2] = {packet::kStartPositionSet, static_cast<u8>(positions_[static_cast<std::size_t>(s)])};
                send_one(first_human_id(), p.net_id, pk, 2);
            }
        }
    // "Loaded": clients every pass, the host once everything is done.
    if (!host_ || done)
        for (const RoomPlayer& p : players_)
            if (p.net_id && mine(p)) {
                u8 pk[1] = {packet::kLoaded};
                send_all(p.net_id, pk, 1);
            }
    if (done) loaded_ = true;
}

void BattleRoom::handle(std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& v) {
    const u8* p = v.data();
    const std::size_t need = packet_size_at(p, v.size());
    if (need == 0 || v.size() < need) return;
    const Phase phase = started_ ? Phase::Loading : Phase::Room;
    if (!(packet_phases(p[0]) & static_cast<u8>(phase))) return;
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
            // Join gate: a closed or started game takes nobody new ("Can't add another player").
            if ((players_[0].options & room::kClosed) || started_) {
                u8 st[6] = {packet::kStatus};
                put32(st + 1, static_cast<std::uint32_t>(from));
                st[5] = room::kRefused;
                send_one(net_id_, from, st, 6);
                break;
            }
            for (int s = 0; s < 10; ++s) {
                RoomPlayer& q = players_[static_cast<std::size_t>(s)];
                if (q.net_id) continue;
                r.last_heard = clock_;
                r.logo = 0xFF;
                q = r;
                q.map = players_[0].map;
                grant_logo(from, static_cast<u8>(s));
                dirty_ = true;
                break;
            }
            break;
        }
        RoomPlayer& q = players_[static_cast<std::size_t>(fs)];
        if (mine(q)) break;
        if (!host_ && (r.flags & room::kHost) && local_slot_ >= 0) {
            // The host's settings for the game reach this machine's own record.
            me().map = r.map;
            me().map_checksum = r.map_checksum;
            me().energy = r.energy;
            me().metal = r.metal;
            me().unit_limit = r.unit_limit;
        }
        const RoomPlayer keep = q;
        q = r;
        q.last_heard = keep.last_heard;
        q.latency_ms = keep.latency_ms;
        q.ally_team = keep.ally_team;
        q.group = keep.group;
        q.start_position = keep.start_position;
        if (host_) q.logo = keep.logo;  // the host keeps the logos it granted
        break;
    }
    case packet::kAllyTeam: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s >= 0 && !mine(players_[static_cast<std::size_t>(s)])) players_[static_cast<std::size_t>(s)].ally_team = p[5];
        break;
    }
    case packet::kLogoAsk:
        // Only the host answers (its record has the host flag).
        if (host_ && fs >= 0) grant_logo(from, p[1]);
        break;
    case packet::kLogoGrant:
        if (!host_ && local_slot_ >= 0) {
            me().logo = p[1];
            dirty_ = true;
        }
        break;
    case packet::kGroupAsk: {
        if (!host_) break;
        const int a = slot_of(static_cast<std::int32_t>(get32(p + 2)));
        if (a < 0) break;
        RoomPlayer& asker = players_[static_cast<std::size_t>(a)];
        if (p[1] != 0) {
            const int t = slot_of(static_cast<std::int32_t>(get32(p + 6)));
            if (t >= 0) asker.group = players_[static_cast<std::size_t>(t)].group;
            else if (asker.group == 0)
                for (std::int32_t g = 1; g <= 10; ++g)
                    if (std::none_of(players_.begin(), players_.end(), [&](const RoomPlayer& q) { return q.net_id && q.group == g; })) {
                        asker.group = g;
                        break;
                    }
        }
        if (asker.group == 0) break;
        u8 pk[6] = {packet::kGroupSet};
        put32(pk + 1, static_cast<std::uint32_t>(asker.net_id));
        pk[5] = static_cast<u8>(asker.group);
        send_all(net_id_, pk, 6);
        break;
    }
    case packet::kGroupSet: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s >= 0) players_[static_cast<std::size_t>(s)].group = p[5];
        break;
    }
    case packet::kSlotTable: {
        if (host_) break;
        host_id_ = from;
        std::array<RoomPlayer, 10> next{};
        for (int s = 0; s < 10; ++s) {
            const std::int32_t id = static_cast<std::int32_t>(get32(p + 1 + 4 * s));
            if (!id || id == -1) continue;
            const int old = slot_of(id);
            next[static_cast<std::size_t>(s)] = old >= 0 ? players_[static_cast<std::size_t>(old)] : RoomPlayer{};
            next[static_cast<std::size_t>(s)].net_id = id;
            if (id == net_id_ && local_slot_ < 0) {
                RoomPlayer& m = next[static_cast<std::size_t>(s)];
                m.name = pending_name_;
                m.kind = 1;
                m.side = pending_side_;
                m.map_ready = pending_map_ready_;
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
        const std::int32_t id = static_cast<std::int32_t>(get32(p + 1));
        if (id == net_id_ && p[5] == room::kRefused) {
            refused_ = true;
            error_ = "the host refused the join (the game is closed or running)";
            break;
        }
        const int s = slot_of(id);
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
            if (sub == 3) verdicts_[get32(p + 6)] = p[10] != 0 && p[11] != 0;
            break;
        }
        auto it = std::find_if(peers_.begin(), peers_.end(), [&](const PeerChecks& c) { return c.id == from; });
        if (it == peers_.end()) break;
        if (sub == 1) {
            it->expected = get32(p + 10);
            it->counted = true;
        } else if (sub == 4) {
            it->acked = std::max(it->acked, get32(p + 10));
        } else if (sub == 2) {
            const std::uint32_t key = get32(p + 6);
            if (it->list.count(key)) break;
            it->list[key] = get32(p + 10);
            ++it->got;
            judge_unit(key);
        }
        break;
    }
    case packet::kGameStart:
        if (!host_ && from == host_id_) started_ = true;
        break;
    case packet::kLoaded:
        if (started_ && fs >= 0) heard_loaded_[static_cast<std::size_t>(fs)] = true;
        break;
    case packet::kStartPositionSet: {
        const int ts = slot_of(to);
        if (ts < 0 || !mine(players_[static_cast<std::size_t>(ts)])) break;
        players_[static_cast<std::size_t>(ts)].start_position = static_cast<std::int8_t>(p[1]);
        u8 pk[5] = {packet::kStartPositionAck};
        put32(pk + 1, static_cast<std::uint32_t>(to));
        send_one(to, from, pk, 5);
        break;
    }
    case packet::kStartPositionAck: {
        const int s = slot_of(static_cast<std::int32_t>(get32(p + 1)));
        if (s >= 0) position_acked_[static_cast<std::size_t>(s)] = true;
        break;
    }
    default:
        break;
    }
}

bool BattleRoom::can_start() const {
    if (!host_ || started_) return false;
    int humans = 0;
    for (const RoomPlayer& p : players_) {
        if (!p.net_id) continue;
        if (p.kind == 1) ++humans;
        if (p.kind == 1 && p.net_id != net_id_ && (!p.ready() || !p.map_ready)) return false;
        if (p.logo == 0xFF) return false;  // the battle start waits for every logo (0x497180)
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
    // The definitions no peer was asked about yet (the host's own) are judged too.
    for (const auto& c : checks_)
        if (!verdicts_.count(c.first)) judge_unit(c.first);
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
    const RoomPlayer& self = players_[static_cast<std::size_t>(local_slot_ < 0 ? 0 : local_slot_)];
    for (int s = 0; s < 10; ++s) {
        const RoomPlayer& p = players_[static_cast<std::size_t>(s)];
        if (!p.net_id) continue;
        sim::StartPlayer& sp = b.players[static_cast<std::size_t>(s)];
        sp.kind = !mine(p) ? sim::PlayerKind::Remote : (p.kind == 2 ? sim::PlayerKind::Computer : sim::PlayerKind::Local);
        sp.net_id = p.net_id;
        sp.start_position = p.start_position;
        sp.colour = p.logo == 0xFF ? static_cast<std::uint8_t>(s) : p.logo;
        sp.ally_group = p.ally_team == room::kNoTeam ? 0xFF : p.ally_team;
        sp.name = p.name;
        sp.watcher = p.watching();
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
    // Start resources come from this machine's own side record (the original reads the local one).
    b.start_energy = static_cast<float>(self.energy) * 100.0f;
    b.start_metal = static_cast<float>(self.metal) * 100.0f;
    return b;
}

} // namespace net
