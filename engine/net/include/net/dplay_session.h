#pragma once
// A session of the original game's peer session protocol (net/dplay_wire.h), so this engine
// and the original program can meet in one network game: our own implementation of the
// protocol (no library of the original's platform is used).
//
// Roles: the host (the "name server") gives out player ids and keeps the player table; every
// machine has a system player plus the game's players; a game message goes from a player to a
// player or to everybody, sent by its machine directly to the machines concerned.
//
// Joining (observed between two original programs and against them):
//   client -> host  EnumSessions (the application's id)            host -> client  EnumSessionsReply (description, name)
//   client -> host  RequestPlayerId (flags 9: system, local)       host -> client  RequestPlayerReply (id)
//   client -> host  AddForwardRequest (its system player, password, the session key)
//                                                                  host -> client  SuperEnumPlayersReply (description, every player)
// then each game player of a machine: RequestPlayerId (flags 8) to the host, the id back, and
// CreatePlayer (the player, its names, data and machine) from that machine to every machine.
//
// Service providers carry the messages: TCP/IP (net/dplay_tcpip.h) and the serial / modem
// link (net/dplay_serial.h).

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "net/dplay_wire.h"
#include "net/transport.h"

namespace net::dplay {

// The original game's application id (its sessions are found by it).
Guid game_application();

// What a provider received: a payload (a system or a game message) and where it came from.
struct Inbound {
    Bytes payload;
    Bytes reply_to;  // provider specific address of the sender (TCP/IP: 32 bytes of provider data)
    bool datagram = false;
};

// A service provider: moves payloads between machines.
class Provider {
public:
    virtual ~Provider() = default;
    virtual const char* name() const = 0;
    // This machine's provider data (part of its system player in every table).
    virtual Bytes local_sp_data() const = 0;
    // A remote machine's provider data with what the provider learns from the messages it sent
    // (TCP/IP: a zero address is the address its messages came from).
    virtual Bytes complete_sp_data(const Bytes& sp, const Bytes& reply_to) const {
        (void)reply_to;
        return sp;
    }
    // `to`: a reply address (Inbound::reply_to) or a machine's provider data.
    virtual void send(const Bytes& to, const Bytes& payload, bool guaranteed) = 0;
    virtual bool receive(Inbound& out) = 0;
    virtual void pump() = 0;
    // Client: asks for sessions (to the configured address; providers without addresses ask the
    // other end of their link).
    virtual void enumerate(const Bytes& enum_payload) = 0;
    // Host: answers enumeration requests.
    virtual bool listen(std::string& error) = 0;
    // A machine whose link broke (its provider data or reply address).
    virtual bool lost(Bytes& who) {
        (void)who;
        return false;
    }
    // Whether two addresses name the same machine.
    virtual bool same_machine(const Bytes& a, const Bytes& b) const { return a == b; }
    virtual bool ready() const { return true; }
    virtual std::string error() const { return {}; }
    virtual std::string describe(const Bytes& address) const { return hex(address.data(), address.size()); }
};

struct HostSettings {
    std::string session_name;
    std::string password;
    Guid application = game_application();
    std::uint32_t max_players = 10;
    std::uint32_t user[4] = {0, 0, 0, 0};
};

struct FoundSession {
    SessionDesc desc;
    Bytes address;  // the host's reply address
    std::uint32_t seen_ms = 0;
};

class Session final : public Transport {
public:
    enum class State : std::uint8_t { Idle, Hosting, Finding, Joining, Joined, Failed, Closed };
    // `log`: one line per message sent and received (empty: no log).
    explicit Session(std::unique_ptr<Provider> provider, std::function<void(const std::string&)> log = {});
    ~Session() override;

    // ---- host
    bool host(const HostSettings& settings, std::string& error);
    // ---- client
    void find(const Guid& application = game_application(), const std::string& password = {});
    const std::vector<FoundSession>& sessions() const { return found_; }
    bool join(const FoundSession& session, const std::string& password);
    State state() const { return state_; }
    std::uint32_t result() const { return result_; }  // the host's refusal code, when Failed
    // Leaves the session (each local player and the machine's system player are deleted).
    void close();

    const SessionDesc& desc() const { return desc_; }
    std::uint32_t system_id() const { return system_id_; }
    std::vector<Player> players() const;
    const Player* player(std::uint32_t id) const;
    Provider& provider() { return *provider_; }

    // ---- Transport
    void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override;
    void send_unguaranteed(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override;
    bool receive(net::Message& out) override;
    void set_local_players(const std::vector<std::int32_t>& ids) override { (void)ids; }
    void pump() override;
    bool connected() const override { return state_ == State::Hosting || state_ == State::Joined; }
    std::string error() const override { return error_; }
    bool next_event(SessionEvent& e) override;
    std::int32_t create_player(const std::string& name, const std::vector<std::uint8_t>& data) override;
    void destroy_player(std::int32_t id) override;
    std::string player_name(std::int32_t id) const override;
    void set_session_info(const std::string& name, const std::uint32_t user[4], bool closed) override;

    struct Counters {
        std::uint64_t sent = 0, received = 0, system_sent = 0, system_received = 0;
    };
    const Counters& counters() const { return counters_; }

private:
    struct Entry {
        Player p;
        bool local = false;
    };
    void handle(const Inbound& in);
    void handle_system(const Message& m, const Inbound& in);
    void send_system(const Bytes& to, const Bytes& payload);
    void to_every_machine(const Bytes& payload);
    std::uint32_t next_id();
    void add_player(const Player& p, bool local, const Bytes& reply_to);
    void remove_player(std::uint32_t id, bool tell_others);
    const Entry* machine_of(std::uint32_t player) const;
    void log_msg(const char* dir, const Bytes& who, const Bytes& payload);
    void emit(SessionEvent e) { events_.push_back(std::move(e)); }
    void send_desc_change();
    void count_players();

    std::unique_ptr<Provider> provider_;
    std::function<void(const std::string&)> log_;
    State state_ = State::Idle;
    std::string error_;
    std::uint32_t result_ = 0;
    SessionDesc desc_;
    Guid find_application_;
    std::string password_;
    std::vector<FoundSession> found_;
    Bytes host_address_;
    std::uint32_t system_id_ = 0;
    std::map<std::uint32_t, Entry> players_;
    std::uint32_t next_index_ = 0;
    // Pending id requests of this machine: the names and data of the players waiting for ids.
    struct Pending {
        std::uint32_t flags = 0;
        std::string name;
        Bytes data;
    };
    std::deque<Pending> pending_;
    std::deque<net::Message> inbox_;
    std::deque<SessionEvent> events_;
    Counters counters_;
};

}  // namespace net::dplay
