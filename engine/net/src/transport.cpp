#include "net/transport.h"

#include <algorithm>
#include <cstring>

#include "net/protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void set_nonblocking(socket_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
static bool net_startup() {
    static bool done = false;
    if (done) return true;
    WSADATA d;
    done = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    return done;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kNoSocket = -1;
static void close_socket(socket_t s) { close(s); }
static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
static void set_nonblocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static bool net_startup() { return true; }
#endif

namespace net {

// ------------------------------------------------------------------ loopback

class LoopbackHub::End final : public Transport {
public:
    explicit End(LoopbackHub& hub) : hub_(hub) {}
    void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override {
        hub_.deliver(*this, from, to, std::move(bytes));
    }
    bool receive(Message& out) override {
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    void set_local_players(const std::vector<std::int32_t>& ids) override { ids_ = ids; }
    bool owns(std::int32_t id) const { return std::find(ids_.begin(), ids_.end(), id) != ids_.end(); }
    std::deque<Message> inbox_;

private:
    LoopbackHub& hub_;
    std::vector<std::int32_t> ids_;
};

LoopbackHub::LoopbackHub() = default;
LoopbackHub::~LoopbackHub() = default;

Transport& LoopbackHub::add_machine() {
    ends_.push_back(std::make_unique<End>(*this));
    return *ends_.back();
}

void LoopbackHub::deliver(End& from_end, std::int32_t from, std::int32_t to, std::vector<std::uint8_t>&& bytes) {
    ++messages_;
    bytes_ += bytes.size();
    for (auto& e : ends_) {
        if (e.get() == &from_end) continue;
        if (to == 0 || e->owns(to)) e->inbox_.push_back(Message{from, to, bytes});
    }
}

// ------------------------------------------------------------------ TCP

namespace {

struct Conn {
    socket_t s = kNoSocket;
    std::vector<std::uint8_t> in, out;
    std::vector<std::int32_t> ids;
    bool dead = false;
};

void frame_into(std::vector<std::uint8_t>& out, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes) {
    std::uint8_t head[12];
    put32(head, static_cast<std::uint32_t>(bytes.size()));
    put32(head + 4, static_cast<std::uint32_t>(from));
    put32(head + 8, static_cast<std::uint32_t>(to));
    out.insert(out.end(), head, head + 12);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// Moves bytes both ways; false when the connection closed.
bool pump_conn(Conn& c) {
    while (!c.out.empty()) {
        const int n = ::send(c.s, reinterpret_cast<const char*>(c.out.data()), static_cast<int>(c.out.size()), 0);
        if (n > 0) {
            c.out.erase(c.out.begin(), c.out.begin() + n);
            continue;
        }
        if (n < 0 && would_block()) break;
        return false;
    }
    char buf[8192];
    for (;;) {
        const int n = ::recv(c.s, buf, sizeof buf, 0);
        if (n > 0) {
            c.in.insert(c.in.end(), buf, buf + n);
            continue;
        }
        if (n < 0 && would_block()) break;
        return false;
    }
    return true;
}

bool take_message(Conn& c, Message& m) {
    if (c.in.size() < 12) return false;
    const std::uint32_t len = get32(c.in.data());
    if (c.in.size() < 12u + len) return false;
    m.from = static_cast<std::int32_t>(get32(c.in.data() + 4));
    m.to = static_cast<std::int32_t>(get32(c.in.data() + 8));
    m.bytes.assign(c.in.begin() + 12, c.in.begin() + 12 + len);
    c.in.erase(c.in.begin(), c.in.begin() + 12 + len);
    return true;
}

std::vector<std::uint8_t> ids_payload(const std::vector<std::int32_t>& ids) {
    std::vector<std::uint8_t> p(ids.size() * 4);
    for (std::size_t i = 0; i < ids.size(); ++i) put32(p.data() + 4 * i, static_cast<std::uint32_t>(ids[i]));
    return p;
}

std::vector<std::int32_t> payload_ids(const std::vector<std::uint8_t>& p) {
    std::vector<std::int32_t> ids;
    for (std::size_t i = 0; i + 4 <= p.size(); i += 4) ids.push_back(static_cast<std::int32_t>(get32(p.data() + i)));
    return ids;
}

constexpr std::int32_t kRegister = -2;

class TcpHost final : public Transport {
public:
    ~TcpHost() override {
        for (Conn& c : conns_) close_socket(c.s);
        if (listen_ != kNoSocket) close_socket(listen_);
    }
    bool open(std::uint16_t port, std::string& error) {
        if (!net_startup()) {
            error = "socket start-up failed";
            return false;
        }
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == kNoSocket) {
            error = "no socket";
            return false;
        }
        int yes = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_, 8) != 0) {
            error = "cannot listen on the port";
            return false;
        }
        set_nonblocking(listen_);
        return true;
    }
    void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override { route(nullptr, from, to, bytes); }
    bool receive(Message& out) override {
        pump();
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    void set_local_players(const std::vector<std::int32_t>& ids) override { ids_ = ids; }
    void pump() override {
        for (;;) {
            socket_t s = ::accept(listen_, nullptr, nullptr);
            if (s == kNoSocket) break;
            set_nonblocking(s);
            int yes = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
            conns_.push_back(Conn{s});
        }
        for (Conn& c : conns_) {
            if (c.dead) continue;
            if (!pump_conn(c)) {
                c.dead = true;
                continue;
            }
            Message m;
            while (take_message(c, m)) {
                if (m.from == kRegister) c.ids = payload_ids(m.bytes);
                else route(&c, m.from, m.to, m.bytes);
            }
            pump_conn(c);
        }
    }

private:
    void route(Conn* origin, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes) {
        const bool mine = std::find(ids_.begin(), ids_.end(), to) != ids_.end();
        if (origin && (to == 0 || mine)) inbox_.push_back(Message{from, to, bytes});
        for (Conn& c : conns_) {
            if (&c == origin || c.dead) continue;
            if (to == 0 || std::find(c.ids.begin(), c.ids.end(), to) != c.ids.end()) {
                frame_into(c.out, from, to, bytes);
                pump_conn(c);
            }
        }
    }
    socket_t listen_ = kNoSocket;
    std::vector<Conn> conns_;
    std::deque<Message> inbox_;
    std::vector<std::int32_t> ids_;
};

class TcpClient final : public Transport {
public:
    ~TcpClient() override {
        if (c_.s != kNoSocket) close_socket(c_.s);
    }
    bool open(const std::string& host, std::uint16_t port, std::string& error) {
        if (!net_startup()) {
            error = "socket start-up failed";
            return false;
        }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
            error = "unknown address";
            return false;
        }
        c_.s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        const bool ok = c_.s != kNoSocket && ::connect(c_.s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
        freeaddrinfo(res);
        if (!ok) {
            error = "cannot connect";
            return false;
        }
        set_nonblocking(c_.s);
        int yes = 1;
        setsockopt(c_.s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
        return true;
    }
    void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override {
        frame_into(c_.out, from, to, bytes);
        pump();
    }
    bool receive(Message& out) override {
        pump();
        return take_message(c_, out);
    }
    void set_local_players(const std::vector<std::int32_t>& ids) override {
        frame_into(c_.out, kRegister, 0, ids_payload(ids));
        pump();
    }
    void pump() override {
        if (!c_.dead && !pump_conn(c_)) c_.dead = true;
    }
    bool connected() const override { return !c_.dead; }

private:
    Conn c_;
};

}  // namespace

std::unique_ptr<Transport> tcp_host(std::uint16_t port, std::string& error) {
    auto t = std::make_unique<TcpHost>();
    if (!t->open(port, error)) return nullptr;
    return t;
}

std::unique_ptr<Transport> tcp_join(const std::string& address, std::uint16_t port, std::string& error) {
    auto t = std::make_unique<TcpClient>();
    if (!t->open(address, port, error)) return nullptr;
    return t;
}

} // namespace net
