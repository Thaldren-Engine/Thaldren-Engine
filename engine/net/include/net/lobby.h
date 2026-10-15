#pragma once
// The battle room and the loading handshake of a network game (docs/RE_MEMORY.md, "Network
// and sync", "Battle room"):
//  - every two seconds each machine pings (0x02), probes (0x06) and sends its players' side
//    records (0x20, 185 bytes) and ally teams (0x24); the host also sends the slot table (0x26);
//  - a player's logo (team colour) is asked of the host (0x17) and granted (0x18): the first
//    free logo from the asked one upward;
//  - the ally team (0..4, 5 none) goes with 0x24; the host numbers players' groups (0x21 ask,
//    0x22 set);
//  - the unit definitions are compared (0x1A) on the host;
//  - the host starts (0x08); during loading the host assigns start positions (0x4568C0: in slot
//    order, or shuffled on the C runtime rand() unless locations are fixed) and sends each
//    remote player its position (0x1E, answered by 0x1F); every machine reports "loaded"
//    (0x15); the game begins when every remote player has reported (the host: and answered).
// Then every machine loads with provider 3: its players local, the others remote.
//
// Side record fields the original's code reads: map name at 0, map ready +0x8F, kind +0x94,
// side +0x95, logo +0x96, flags +0x97, memory (MB) +0x99, option word +0x9B (map flags in its
// high byte +0x9C), CD flag +0x9D, energy and metal in hundreds +0xA1 / +0xA3, unit limit
// +0xA5, version +0xA7/+0xA8, map checksum +0xA9. The player's name (the session's player name
// in the original) travels at +0x40, a place the original was not seen to read.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net/frames.h"
#include "net/session.h"
#include "net/transport.h"
#include "sim/battle_start.h"
#include "sim/rng.h"

namespace net {

namespace room {
// Side record option word bits (+0x9B).
constexpr std::uint16_t kReady = 0x20, kWatch = 0x40, kCheats = 0x80, kCommanderMask = 0x1800, kNonCampaign = 0x2000,
                        kFixedLocations = 0x4000, kClosed = 0x8000;
// Side record flags (+0x97).
constexpr std::uint8_t kHost = 1, kShareMetal = 2, kShareEnergy = 4, kShareMap = 0x20;
// Map flags (+0x9C: the option word's high byte, bits 0-2).
constexpr std::uint8_t kMapping = 1, kLineOfSight = 2, kTrueSight = 4;
constexpr std::size_t kRecordSize = 185;
constexpr std::uint8_t kNoTeam = 5;
// Status reasons (packet 0x1B) used here.
constexpr std::uint8_t kLeft = 1, kRefused = 6;
}  // namespace room

struct RoomPlayer {
    std::int32_t net_id = 0;      // 0: slot free
    std::string name;
    std::uint8_t kind = 0;        // 1 human, 2 computer
    std::uint8_t side = 0;        // 0 ARM, 1 CORE
    std::uint8_t logo = 0xFF;     // team colour / logo (0..9; 0xFF not granted yet)
    std::uint8_t flags = 0;       // room::kHost / share flags
    std::uint16_t options = room::kNonCampaign;
    std::uint8_t map_flags = 0;
    std::uint16_t memory_mb = 64;
    std::uint16_t unit_limit = 250;
    std::uint16_t energy = 10, metal = 10;  // start resources in hundreds
    std::uint8_t version_major = 3, version_minor = 1;
    std::uint32_t map_checksum = 0;
    std::string map;
    bool map_ready = false;
    // Player record fields carried by their own packets.
    std::uint8_t ally_team = room::kNoTeam;  // +0x13F (0x24)
    std::int32_t group = 0;                  // +0xC (0x22)
    std::int8_t start_position = -1;         // +0x147 (0x1E during loading)
    std::uint32_t last_heard = 0;
    std::uint32_t latency_ms = 0;
    bool ready() const { return (options & room::kReady) != 0; }
    bool watching() const { return (options & room::kWatch) != 0; }
};

std::vector<std::uint8_t> encode_side_record(const RoomPlayer& p);
RoomPlayer decode_side_record(const std::uint8_t* record);  // 185 bytes

// Unit definitions compared before a game: each machine's (unit key, checksum) pairs.
using UnitChecks = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

class BattleRoom {
public:
    // `net_id`: this machine's player (the session's player id; any distinct nonzero value).
    BattleRoom(Transport& transport, bool host, std::int32_t net_id, const std::string& name);
    // On a shared session (the battle continues on it: net::NetBattle(outside, session)).
    BattleRoom(NetSession& session, bool host, std::int32_t net_id, const std::string& name);
    NetSession& session() { return session_; }

    // Local player's choices (sent with the next record).
    void set_side(std::uint8_t side);
    void set_logo(std::uint8_t logo);  // asks the host (0x17); the host answers with the first free one
    void next_logo();                  // the LOGO button: the next logo after the current one
    void set_ready(bool ready);
    void set_ally_team(std::uint8_t team);  // 0..4, 5 = none (0x24)
    void next_ally_team();                  // the team icon button: (team + 1) % 6
    void set_watch(bool watch);
    void set_memory(std::uint16_t megabytes);
    // Host: the game's settings.
    void set_map(const std::string& map, std::uint32_t map_checksum = 0);
    void set_options(std::uint8_t commander_mode, std::uint8_t map_flags, std::uint16_t unit_limit, std::uint16_t energy_hundreds,
                     std::uint16_t metal_hundreds, bool fixed_locations);
    void set_closed(bool closed);  // no more players may join
    // Host: a computer player in a free slot (its records are the host's to send); returns the slot or -1.
    int add_computer(std::uint8_t side);
    void say(const std::string& text);
    // Whether this machine has the map (the original's map-ready byte).
    void set_map_ready(bool ready) {
        if (local_slot_ >= 0) me().map_ready = ready;
        pending_map_ready_ = ready;
        dirty_ = true;
    }
    // This machine's checksum of a map (0: it does not have it). A client compares it with the
    // host's (+0xA9) when the host is of version 3.1 or later and sets its map-ready byte.
    std::function<std::uint32_t(const std::string& map)> map_checksum;
    // This machine's unit definitions (the checksum exchange).
    void set_unit_checks(UnitChecks checks) { checks_ = std::move(checks); }
    // The host's verdict per unit key (true: buildable) once the exchange ran.
    const std::map<std::uint32_t, bool>& unit_verdicts() const { return verdicts_; }
    std::vector<std::uint32_t> disabled_units() const;

    // One pass of the room's loop: receive, answer, and send what is due (clock in 1/30 s).
    // `crt_rand`: this machine's C runtime rand() (the host's start position shuffle draws it).
    void frame(std::uint32_t clock, sim::Lcg15* crt_rand = nullptr);

    // Host: everybody human is ready, has the map and finished the definition exchange.
    bool can_start() const;
    // Host: starts the game (packet 8); the loading handshake follows in frame().
    bool start();
    bool started() const { return started_; }
    // The loading handshake is over: battle_start() is final.
    bool loaded() const { return loaded_; }

    // The battle description for this machine (after loading): its players local, the others remote.
    sim::BattleStart battle_start() const;
    std::array<std::string, 10> sides() const;
    std::string map() const;

    const std::array<RoomPlayer, 10>& players() const { return players_; }
    int local_slot() const { return local_slot_; }
    bool is_host() const { return host_; }
    bool refused() const { return refused_; }
    std::function<void(const std::string& line)> on_chat;
    std::function<void(int slot)> on_player_left;
    std::string error() const { return error_; }
    FrameSender& sender() { return sender_; }

private:
    void init(const std::string& name);
    RoomPlayer& me() { return players_[static_cast<std::size_t>(local_slot_ < 0 ? 0 : local_slot_)]; }
    int slot_of(std::int32_t net_id) const;
    bool mine(const RoomPlayer& p) const { return p.net_id == net_id_ || (host_ && p.kind == 2); }
    std::int32_t first_human_id() const;
    void send_all(std::int32_t from, const std::uint8_t* p, std::size_t n);
    void send_one(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n);
    void broadcast_records();
    void handle(std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& p);
    void grant_logo(std::int32_t asker, std::uint8_t wanted);
    void sync_tick();
    void judge_unit(std::uint32_t key);
    void loading_tick(sim::Lcg15* crt_rand);
    SendFn send_fn();

    std::unique_ptr<NetSession> own_;
    NetSession& session_;
    Transport& transport_;
    bool host_;
    std::int32_t net_id_;
    std::array<RoomPlayer, 10> players_{};
    int local_slot_ = -1;
    std::int32_t host_id_ = 0;
    FrameSender& sender_;
    FrameReceiver& receiver_;
    std::uint32_t clock_ = 0, next_periodic_ = 0;
    bool dirty_ = true, started_ = false, loaded_ = false, refused_ = false;
    std::string error_;
    std::string pending_name_, checked_map_;
    std::uint32_t checked_sum_ = 0;
    std::uint8_t pending_side_ = 0;
    bool pending_map_ready_ = false;
    // Loading handshake (GS+0x29A4 loaded, +0x29D0 position answered, +0x29FC positions).
    std::array<bool, 10> heard_loaded_{}, position_acked_{};
    std::array<std::int32_t, 10> positions_{};
    bool positions_made_ = false;
    // Unit definition exchange.
    UnitChecks checks_;
    std::size_t sync_sent_ = 0;
    std::uint32_t sync_heard_ = 0;
    bool sync_hello_ = false;
    struct PeerChecks {
        std::int32_t id = 0;
        std::uint32_t expected = 0, got = 0, acked = 0;
        bool counted = false;  // the definition count arrived
        std::map<std::uint32_t, std::uint32_t> list;
    };
    std::vector<PeerChecks> peers_;
    std::map<std::uint32_t, bool> verdicts_;
};

} // namespace net
