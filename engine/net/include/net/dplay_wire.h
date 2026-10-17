#pragma once
// The wire format of the peer session protocol the original game's network layer used
// (Microsoft DirectPlay, the 1997-2000 "DirectPlay 4" protocol, message version 14), written
// from observing the messages a running original exchanged and from the protocol's public
// description; no code of that library is used or linked. See docs/RE_MEMORY.md, "Network and
// sync" / "DirectPlay wire protocol".
//
// A session message ("payload") is either
//  - a system message: the signature "play", a 16-bit command, a 16-bit version, then the
//    command's fields; offsets inside a message count from the signature; or
//  - a game message: the sending player's id, the addressee's id (0: everybody), the bytes.
// The service provider carrying it adds its own header (TCP/IP: a 32-bit word with the
// message size in its low 20 bits and the token 0xFAB above, and the sender's return address;
// game messages sent without delivery guarantee travel as bare datagrams without it).
//
// Strings are UTF-16 (little-endian) with a terminating zero; the game's 8-bit names are
// widened byte by byte (Latin-1).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace net::dplay {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kSignature = 0x79616C70;  // "play"
constexpr std::uint16_t kVersion = 14;            // the message version every observed message had
constexpr std::uint32_t kToken = 0xFAB;           // provider header token of an ordinary message
constexpr std::uint16_t kEnumPort = 47624;        // session enumeration (UDP and TCP)

// System message commands (the ones this implementation sends or understands are marked).
namespace cmd {
constexpr std::uint16_t kEnumSessionsReply = 0x01;   // * the host's session description
constexpr std::uint16_t kEnumSessions = 0x02;        // * "who hosts this application"
constexpr std::uint16_t kRequestPlayerId = 0x05;     // * a new player id, asked of the host
constexpr std::uint16_t kRequestPlayerReply = 0x07;  // * the id
constexpr std::uint16_t kCreatePlayer = 0x08;        // * a player joined (sent by its machine to every machine)
constexpr std::uint16_t kDeletePlayer = 0x0B;        // * a player left
constexpr std::uint16_t kPlayerDataChanged = 0x0F;   // * a player's data
constexpr std::uint16_t kPlayerNameChanged = 0x10;   // * a player's names
constexpr std::uint16_t kAddForwardRequest = 0x13;   // * a machine asks the host to be added to the session
constexpr std::uint16_t kPing = 0x16;                // * round trip measurement
constexpr std::uint16_t kPingReply = 0x17;           // *
constexpr std::uint16_t kSessionDescChanged = 0x1A;  // * the host changed the session description
constexpr std::uint16_t kAddForwardReply = 0x24;     // * the host refuses a machine (error code)
constexpr std::uint16_t kSuperEnumPlayersReply = 0x29;  // * the host's player table for a machine that joined
constexpr std::uint16_t kAddForward = 0x2E;          // the host announces a machine to the others
constexpr std::uint16_t kAddForwardAck = 0x2F;
}  // namespace cmd

// Player flags.
constexpr std::uint32_t kSystemPlayer = 1;  // a machine's own player (one per machine)
constexpr std::uint32_t kNameServer = 2;    // the host's machine
constexpr std::uint32_t kInSession = 4;     // set in the host's table for every player
constexpr std::uint32_t kSenderLocal = 8;   // the player lives on the machine sending the message

// Session flags (the session description's first flags word).
constexpr std::uint32_t kNewPlayersDisabled = 0x1, kJoinDisabled = 0x20, kPasswordRequired = 0x400;
// Enumeration flags.
constexpr std::uint32_t kEnumAvailable = 0x1;

// Error codes carried in replies.
constexpr std::uint32_t kOk = 0;
constexpr std::uint32_t kErrInvalidPassword = 0x88770154u;
constexpr std::uint32_t kErrCantCreatePlayer = 0x8877003Cu;
constexpr std::uint32_t kErrAccessDenied = 0x8877000Au;  // joining refused (closed or full)

struct Guid {
    std::array<std::uint8_t, 16> b{};
    bool operator==(const Guid& o) const { return b == o.b; }
    bool operator!=(const Guid& o) const { return b != o.b; }
    bool null() const;
    std::string str() const;
    static Guid parse(const std::string& s);  // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
};

// An IPv4 socket address as it appears inside messages (16 bytes: family 2, port in network
// order, the address bytes, eight zero bytes). `ip` holds the four address bytes a.b.c.d in
// that order from the lowest byte (so 127.0.0.1 is 0x0100007F).
struct SockAddr {
    std::uint16_t port = 0;
    std::uint32_t ip = 0;
    bool operator==(const SockAddr& o) const { return port == o.port && ip == o.ip; }
    bool operator<(const SockAddr& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
};
constexpr std::uint32_t kLoopback = 0x0100007Fu;
void put_sockaddr(Bytes& out, const SockAddr& a);
SockAddr get_sockaddr(const std::uint8_t* p);
std::string ip_text(std::uint32_t ip);

// The session description (80 bytes on the wire; the name and password travel as strings).
struct SessionDesc {
    std::uint32_t flags = 0;
    Guid instance, application;
    std::uint32_t max_players = 0, current_players = 0;
    std::uint32_t reserved1 = 0, reserved2 = 0;  // reserved1: the key player ids are XORed with
    std::array<std::uint32_t, 4> user{};
    std::string name, password;
};

// A player as the session's tables describe it.
struct Player {
    std::uint32_t flags = 0;
    std::uint32_t id = 0;
    std::uint32_t system_id = 0;  // the machine's system player (for a system player: its own id)
    std::uint32_t version = kVersion;
    std::string short_name, long_name;
    Bytes sp_data;      // the provider's data: TCP/IP: stream and datagram addresses (32 bytes)
    Bytes player_data;  // the application's data
    std::uint32_t parent = 0;
    bool system() const { return (flags & kSystemPlayer) != 0; }
};

// ---- encoding (payloads: from the signature on)
Bytes enum_sessions(const Guid& application, std::uint32_t flags, const std::string& password);
Bytes enum_sessions_reply(const SessionDesc& d);
Bytes request_player_id(std::uint32_t flags);
Bytes request_player_reply(std::uint32_t id, std::uint32_t result);
Bytes add_forward_request(std::uint32_t to, const Player& p, const std::string& password, std::uint32_t key);
Bytes add_forward_reply(std::uint32_t result);
Bytes super_enum_players_reply(const SessionDesc& d, const std::vector<Player>& players);
Bytes create_player(std::uint32_t to, const Player& p);
Bytes delete_player(std::uint32_t to, std::uint32_t id);
Bytes player_name_changed(std::uint32_t to, std::uint32_t id, const std::string& short_name, const std::string& long_name);
Bytes player_data_changed(std::uint32_t to, std::uint32_t id, const Bytes& data);
Bytes session_desc_changed(std::uint32_t to, const SessionDesc& d);
Bytes ping(std::uint32_t from, std::uint32_t tick);
Bytes ping_reply(std::uint32_t from, std::uint32_t tick);
Bytes game_message(std::uint32_t from, std::uint32_t to, const std::uint8_t* data, std::size_t size);

// Packed (full) and super-packed (name table) player forms.
Bytes packed_player(const Player& p);
Bytes super_packed_player(const Player& p);

// ---- decoding
bool is_system(const std::uint8_t* p, std::size_t n);
struct Message {
    std::uint16_t command = 0, version = 0;
    // fields, filled by command
    Guid application;               // enum sessions
    std::uint32_t flags = 0;        // enum sessions, request player id
    std::string password;           // enum sessions, add forward request
    SessionDesc desc;               // enum sessions reply, super enum players reply, session desc changed
    std::uint32_t id = 0;           // request player reply (the id), create/delete/name/data (the player), ping (from)
    std::uint32_t to = 0;           // the addressee field of create/delete/add-forward/changes
    std::uint32_t result = 0;       // request player reply, add forward reply
    std::uint32_t key = 0;          // add forward request: the session key echoed
    std::uint32_t tick = 0;         // ping
    Player player;                  // add forward request, create player
    std::vector<Player> players;    // super enum players reply
    std::string short_name, long_name;  // player name changed
    Bytes data;                     // player data changed
};
// Decodes a system message (from the signature on); nullopt when malformed.
std::optional<Message> decode(const std::uint8_t* p, std::size_t n);
const char* command_name(std::uint16_t command);

// UTF-16 helpers (8-bit text widened byte by byte; narrowed by dropping the high byte).
void put_wide(Bytes& out, const std::string& s);
std::string get_wide(const std::uint8_t* p, std::size_t avail, std::size_t* used = nullptr);

// Player id scheme of the host: (uniqueness << 16 | index) XOR the session key.
inline std::uint32_t make_player_id(std::uint32_t key, std::uint32_t index, std::uint32_t uniqueness = 1) {
    return ((uniqueness << 16) | (index & 0xFFFF)) ^ key;
}

// TCP/IP provider data of a machine: its stream (TCP) and datagram (UDP) addresses.
Bytes tcpip_sp_data(const SockAddr& stream, const SockAddr& datagram);
bool tcpip_from_sp_data(const Bytes& sp, SockAddr& stream, SockAddr& datagram);

// The TCP/IP provider header (20 bytes) in front of a payload.
Bytes tcpip_frame(const SockAddr& return_address, const Bytes& payload);
inline std::uint32_t frame_size(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
            (static_cast<std::uint32_t>(p[3]) << 24)) &
           0xFFFFFu;
}
constexpr std::size_t kTcpipHeader = 20;

std::string hex(const std::uint8_t* p, std::size_t n);

}  // namespace net::dplay
