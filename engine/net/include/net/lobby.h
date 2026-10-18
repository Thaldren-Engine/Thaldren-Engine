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
    // Shown by the battle room: the player's screen size (+0x8B / +0x8D, the RES column), its CD
    // word (+0x9D, bit 2 the game CD), the host's best ping (+0x9F, the game list's ping column)
    // and the game's name (the session's name in the original; carried at +0x60, a place the
    // original was not seen to read).
    std::uint16_t screen_w = 0, screen_h = 0;
    std::uint16_t cd = 4;
    std::uint16_t best_ping = 0;
    std::string game_name;
    // While loading: the player's progress (the record byte +0x20: the average of the loading
    // screen's six rows, packet 0x2A).
    std::uint8_t load_percent = 0;
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

    // ---- the battle room screen's controls (0x447B10)
    // Host: the whole option word of the game (commander 0x1800, cheat codes 0x2000, fixed
    // locations 0x4000, closed 0x8000, watching allowed 0x80; bits 8-10 the map flags).
    void set_option_word(std::uint16_t options);
    // This machine's screen size (the RES column).
    void set_screen(std::uint16_t w, std::uint16_t h);
    // The game's name (the session's name: the game list shows it).
    void set_game_name(const std::string& name);
    // Host: a computer player in the given free slot (-1 when taken), its logo the first free one.
    int add_computer_at(int slot, std::uint8_t side);
    // Host: a computer player's slot freed (status 1, "left").
    bool remove_computer(int slot, std::uint8_t reason = room::kLeft);
    // Host: an empty slot closed to newcomers ("[BLOCKED]"; slot table id -1) or opened again.
    bool set_blocked(int slot, bool blocked);
    bool blocked(int slot) const { return slot >= 0 && slot < 10 && blocked_[static_cast<std::size_t>(slot)]; }
    // Host: a player sent away (status with the reason; 0 "rejected").
    bool kick(int slot, std::uint8_t reason = 0);
    // This machine leaves the room (status 2 for each of its players).
    void leave();
    // The local player's alliance with another slot (packet 0x23; a computer player answers at once).
    void set_allied(int slot, bool allied);
    // The local player's alliance bytes (+0x108) and the others' (+0x113, "allied by").
    const std::array<std::uint8_t, 10>& allied() const { return allied_; }
    const std::array<std::uint8_t, 10>& allied_by() const { return allied_by_; }
    // The ally team of any of this machine's players (the host for its computer players).
    void set_ally_team_of(int slot, std::uint8_t team);
    // A chat line as it came (the original's room shows the text a player sent as it is).
    std::function<void(int slot, const std::string& line)> on_chat_line;
    // Why this machine is out of the room (the status reason; -1 still in): 0 rejected, 3 closed,
    // 10 the creator left, ...
    int out_reason() const { return out_reason_; }
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
    const UnitChecks& unit_checks() const { return checks_; }
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

    // ---- the room screen and the loading screen (0x44A680, 0x497F40)
    std::int32_t net_id() const { return net_id_; }
    std::uint32_t clock() const { return clock_; }
    // Changes one of this machine's records (its human or a computer player it runs); the change
    // goes out with the next records. False when the slot is not this machine's.
    bool edit_own(int slot, const std::function<void(RoomPlayer&)>& change);
    // Host: the start resources (hundreds) and the unit limit of the game (the room's sliders).
    void set_resources(std::uint16_t energy_hundreds, std::uint16_t metal_hundreds, std::uint16_t unit_limit);
    // Host, after a new map: the other machines' players count as not ready until their next
    // records say otherwise (0x444CB0 clears the ready bit of every record not of this machine).
    void unready_others();
    // ---- unit restrictions (the definition exchange's entries, 0x46D040 / 0x46E550 / 0x46E160)
    // Each definition key's count: the most units of it one player may build, -1 no limit. A
    // machine starts from its own definitions (0 for "wacky" ones, else -1); the host's counts
    // go to every peer in the exchange's answers (0x1A sub 3, bytes 12-13) and replace the
    // peers' own.
    void set_unit_limits(std::map<std::uint32_t, std::int32_t> counts) { limits_ = std::move(counts); }
    // Host: a count changed (the restrictions); the answer for that definition goes to every peer.
    void set_unit_limit(std::uint32_t key, std::int32_t count);
    std::int32_t unit_limit(std::uint32_t key) const {
        const auto it = limits_.find(key);
        return it == limits_.end() ? -1 : it->second;
    }
    // A definition enabled or not (0x46E4D0 / 0x46E450: the restrictions' closing); the host
    // answers every peer.
    void set_unit_enabled(std::uint32_t key, bool on);
    // A definition's entry: "allowed" (+0x18, 1 unless restricted away), its verdict (+0x1A: the
    // host's comparison with every peer; 0 until then), and both (0x46E160: whether this machine
    // may build it in the game).
    bool unit_allowed(std::uint32_t key) const;
    bool unit_confirmed(std::uint32_t key) const;
    bool unit_enabled(std::uint32_t key) const;
    // Host: the unit definition exchange with this player still runs (the room shows "Synching").
    bool synching(int slot) const;
    // Loading: whether this machine heard "loaded" (0x15) from the player (GS+0x29A4).
    bool heard_loaded(int slot) const { return slot >= 0 && slot < 10 && heard_loaded_[static_cast<std::size_t>(slot)]; }
    // The original's order of a network start: the host's 0x08, then every machine loads its
    // data and only then runs the loading handshake (start positions 0x1E / 0x1F, "loaded" 0x15;
    // 0x4568C0 is called once the loading thread is done). With `defer` the handshake waits for
    // data_loaded(); without (the default) it follows the start at once.
    void defer_loading_handshake(bool defer) { defer_handshake_ = defer; }
    void data_loaded() { defer_handshake_ = false; }
    // One frame of the loading screen (0x497F40): this machine's players' progress (the average
    // of the six rows) goes to everybody (0x2A) with a probe (0x06), then the network is read.
    void send_load_progress(std::uint8_t percent);

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
    // Sessions with their own player directory (the original's session service).
    void session_events();
    std::int32_t computer_id(int slot);
    void publish_session_info();
    std::uint32_t published_user_[4] = {0, 0, 0, 0};
    std::string published_name_;
    bool logo_asked_ = false;  // a joining machine asks the host for logo 0 (0x4517B0 -> 0x4526C0(0))
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
    std::array<bool, 10> blocked_{};
    std::uint16_t pending_screen_w_ = 0, pending_screen_h_ = 0;
    std::array<std::uint8_t, 10> allied_{}, allied_by_{};
    int out_reason_ = -1;
    bool defer_handshake_ = false;
    bool handshake_done() const;
    std::map<std::uint32_t, std::int32_t> limits_;
    std::map<std::uint32_t, bool> allowed_;
    void send_answer(std::uint32_t key);
};

} // namespace net
