#pragma once
// The battle room of a network game (docs/RE_MEMORY.md, "Network and sync", "Battle room"):
// players join the host's session, each machine announces its player's side record (packet
// 0x20, every two seconds and on change), the host announces which net id holds which slot
// (0x26), everybody pings (0x02) and keeps the others awake (0x06), chat goes as packet 5,
// the unit definitions are compared (0x1A) and the host starts the game (0x08). Then every
// machine loads the battle with provider 3 (sim::start_battle), its own players as local
// (controller 1, computer players of the host as 2) and the others as remote (3).
//
// The side record is the original's 185-byte block. Fields whose place the original's code
// shows are written there (map name at 0, map ready +0x8F, kind +0x94, side +0x95, start
// position +0x96, flags +0x97, options +0x9B, map flags +0x9C, CD flag +0x9D, unit limit
// +0xA5); the player's name, colour, team and start resources use bytes the original's code
// was not seen to read (listed in RE_MEMORY) and may not match the original.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "net/frames.h"
#include "net/transport.h"
#include "sim/battle_start.h"

namespace net {

namespace room {
// Side record option word bits (+0x9B).
constexpr std::uint16_t kReady = 0x20, kWatch = 0x40, kCommanderMask = 0x1800, kNonCampaign = 0x2000, kFixedLocations = 0x4000,
                        kClosed = 0x8000;
// Side record flags (+0x97).
constexpr std::uint8_t kHost = 1, kShareMetal = 2, kShareEnergy = 4, kShareMap = 0x20;
// Map flags (+0x9C: the option word's high byte, bits 0-2).
constexpr std::uint8_t kMapping = 1, kLineOfSight = 2, kTrueSight = 4;
constexpr std::size_t kRecordSize = 185;
}  // namespace room

struct RoomPlayer {
    std::int32_t net_id = 0;      // 0: slot free
    std::string name;
    std::uint8_t kind = 0;        // 1 human, 2 computer
    std::uint8_t side = 0;        // 0 ARM, 1 CORE
    std::uint8_t colour = 0;
    std::int8_t start_position = -1;
    std::uint8_t team = 0xFF;     // players of the same team are allied (0xFF none)
    std::uint8_t flags = 0;       // room::kHost / share flags
    std::uint16_t options = room::kNonCampaign;
    std::uint8_t map_flags = 0;
    std::uint16_t unit_limit = 250;
    std::uint16_t energy = 10, metal = 10;  // start resources in hundreds
    std::string map;
    bool map_ready = false;
    bool loaded = false;
    std::uint32_t last_heard = 0;
    std::uint32_t latency_ms = 0;
    bool ready() const { return (options & room::kReady) != 0; }
};

std::vector<std::uint8_t> encode_side_record(const RoomPlayer& p);
RoomPlayer decode_side_record(const std::uint8_t* record);  // 185 bytes

// Unit definitions compared before a game: each machine's (unit key, checksum) pairs.
using UnitChecks = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

class BattleRoom {
public:
    // `net_id`: this machine's player (the session's player id; any distinct nonzero value).
    BattleRoom(Transport& transport, bool host, std::int32_t net_id, const std::string& name);

    // Local player's choices (sent with the next record).
    void set_side(std::uint8_t side);
    void set_colour(std::uint8_t colour);
    void set_ready(bool ready);
    void set_team(std::uint8_t team);
    // The start position chosen in the room (-1: the host assigns one).
    void set_start_position(std::int8_t position);
    // Host: the game's settings.
    void set_map(const std::string& map);
    void set_options(std::uint8_t commander_mode, std::uint8_t map_flags, std::uint16_t unit_limit, std::uint16_t energy_hundreds,
                     std::uint16_t metal_hundreds, bool fixed_locations);
    // Host: a computer player in a free slot (its records are the host's to send); returns the slot or -1.
    int add_computer(std::uint8_t side);
    void say(const std::string& text);
    // Whether this machine has the map (the original's map-ready byte).
    void set_map_ready(bool ready) { me().map_ready = ready; dirty_ = true; }
    // This machine's unit definitions (the checksum exchange).
    void set_unit_checks(UnitChecks checks) { checks_ = std::move(checks); }
    // Units disabled because a machine's definition differs or is missing (after the exchange).
    const std::vector<std::uint32_t>& disabled_units() const { return disabled_; }

    // One pass of the room's loop: receive, answer, and send what is due (clock in 1/30 s).
    void frame(std::uint32_t clock);

    // Host: everybody human is ready, has the map and finished the definition exchange.
    bool can_start() const;
    // Host: starts the game (start positions assigned, records sent, packet 8).
    bool start();
    bool started() const { return started_; }

    // The battle description for this machine (after start): its players local, the others remote.
    sim::BattleStart battle_start() const;
    std::array<std::string, 10> sides() const;
    std::string map() const;

    const std::array<RoomPlayer, 10>& players() const { return players_; }
    int local_slot() const { return local_slot_; }
    bool is_host() const { return host_; }
    std::function<void(const std::string& line)> on_chat;
    std::function<void(int slot)> on_player_left;
    std::string error() const { return error_; }
    FrameSender& sender() { return sender_; }

private:
    RoomPlayer& me() { return players_[static_cast<std::size_t>(local_slot_ < 0 ? 0 : local_slot_)]; }
    int slot_of(std::int32_t net_id) const;
    void send_all(std::int32_t from, const std::uint8_t* p, std::size_t n);
    void send_one(std::int32_t from, std::int32_t to, const std::uint8_t* p, std::size_t n);
    void broadcast_records();
    void handle(std::int32_t from, const std::vector<std::uint8_t>& p);
    void sync_tick();
    SendFn send_fn();

    Transport& transport_;
    bool host_;
    std::int32_t net_id_;
    std::array<RoomPlayer, 10> players_{};
    int local_slot_ = -1;
    std::int32_t host_id_ = 0;
    FrameSender sender_;
    FrameReceiver receiver_;
    std::uint32_t clock_ = 0, next_periodic_ = 0;
    bool dirty_ = true, started_ = false;
    std::string error_;
    std::string pending_name_;
    std::uint8_t pending_side_ = 0, pending_colour_ = 0;
    // Unit definition exchange.
    UnitChecks checks_;
    std::size_t sync_sent_ = 0;
    std::uint32_t sync_heard_ = 0;
    bool sync_hello_ = false;
    struct PeerChecks {
        std::int32_t id = 0;
        std::uint32_t expected = 0, got = 0, acked = 0;
        bool counted = false;  // the definition count arrived
        UnitChecks list;
    };
    std::vector<PeerChecks> peers_;
    std::vector<std::uint32_t> disabled_;
};

} // namespace net
