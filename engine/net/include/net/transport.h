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
