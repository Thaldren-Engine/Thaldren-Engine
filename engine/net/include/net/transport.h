#pragma once
// Session transports carrying the game's messages between machines. The original used a
// peer-to-peer session service (TCP/IP, IPX, modem, serial) with reliable, ordered delivery
// ("guaranteed" messages); every message names its sending player and its addressee (a
// player, or 0 for everybody else). These transports give the same: an in-process loopback
// for tests and a TCP star (the host relays) for real games.

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct Message {
    std::int32_t from = 0, to = 0;
    std::vector<std::uint8_t> bytes;
};

// What a session with its own player directory reports (the original's session service gave
// the game "system messages": a player was created or destroyed, its name or data changed, the
// session description changed, the session was lost).
struct SessionEvent {
    enum class Kind : std::uint8_t { PlayerCreated, PlayerDestroyed, PlayerName, PlayerData, SessionDesc, Lost };
    Kind kind = Kind::PlayerCreated;
    std::int32_t id = 0;
    bool local = false;  // the player is this machine's
    std::string short_name, long_name;
    std::vector<std::uint8_t> data;  // the player's data (created, data changed)
};

class Transport {
public:
    virtual ~Transport() = default;
    // `to` = a player's net id, or 0 for every player on the other machines.
    virtual void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) = 0;
    // A message without guaranteed delivery (the original turns guarantees off for its pings).
    virtual void send_unguaranteed(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) { send(from, to, std::move(bytes)); }
    virtual bool receive(Message& out) = 0;
    // The players this machine hosts (messages addressed to them come here).
    virtual void set_local_players(const std::vector<std::int32_t>& ids) = 0;
    // Moves bytes on (sockets); no-op for the loopback.
    virtual void pump() {}
    virtual bool connected() const { return true; }
    virtual std::string error() const { return {}; }

    // ---- sessions with their own player directory (net/dplay_session.h); the defaults are
    // those of the transports above, whose players' ids are chosen by the room.
    // The next directory event, if any.
    virtual bool next_event(SessionEvent&) { return false; }
    // Creates a player of this machine with the session's next id: the id when known at once
    // (the host), 0 when it follows as a local PlayerCreated event, -1 when not supported.
    virtual std::int32_t create_player(const std::string& /*name*/, const std::vector<std::uint8_t>& /*data*/) { return -1; }
    virtual void destroy_player(std::int32_t /*id*/) {}
    // A player's name in the session's directory ("" when unknown or not supported).
    virtual std::string player_name(std::int32_t /*id*/) const { return {}; }
    // The host's description of its game for the game lists (the session's name and its four
    // application words); `closed` refuses newcomers.
    virtual void set_session_info(const std::string& /*name*/, const std::uint32_t /*user*/[4], bool /*closed*/) {}
};

// In-process machines; messages arrive in send order.
class LoopbackHub {
public:
    LoopbackHub();
    ~LoopbackHub();
    // A new machine's transport (owned by the hub).
    Transport& add_machine();
    std::size_t machines() const { return ends_.size(); }
    std::uint64_t messages() const { return messages_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    class End;
    friend class End;
    void deliver(End& from_end, std::int32_t from, std::int32_t to, std::vector<std::uint8_t>&& bytes);
    std::vector<std::unique_ptr<End>> ends_;
    std::uint64_t messages_ = 0, bytes_ = 0;
};

// TCP: the host listens, others connect to it; the host relays messages between them.
// Stream framing: u32 length, i32 from, i32 to, bytes (little-endian). A message from -2
// lists the sender machine's players (registration).
std::unique_ptr<Transport> tcp_host(std::uint16_t port, std::string& error);
std::unique_ptr<Transport> tcp_join(const std::string& address, std::uint16_t port, std::string& error);

// UDP: datagrams with the session's own delivery guarantee, as the original's session service
// gives "guaranteed" messages over UDP: every guaranteed datagram is acknowledged and sent again
// until it is (every 100 ms, up to 100 times); duplicates are dropped; datagrams are handed on
// in arrival order, so messages can arrive out of order (the frame receiver puts frames back in
// order as the original does). The host relays between the others. Datagram: u8 kind (1
// guaranteed, 2 not guaranteed, 3 acknowledgement, 4 registration), u32 number, i32 from,
// i32 to, bytes. `loss` drops that share of outgoing datagrams (tests), drawn from `seed`.
struct UdpOptions {
    double loss = 0.0;
    std::uint32_t seed = 1;
};
std::unique_ptr<Transport> udp_host(std::uint16_t port, std::string& error, UdpOptions options = {});
std::unique_ptr<Transport> udp_join(const std::string& address, std::uint16_t port, std::string& error, UdpOptions options = {});
// Counters of a UDP transport (0 for others): datagrams sent again, duplicates dropped.
struct UdpCounters {
    std::uint64_t resent = 0, duplicates = 0, dropped_by_loss = 0, delivered = 0;
};
UdpCounters udp_counters(const Transport& t);

} // namespace net
