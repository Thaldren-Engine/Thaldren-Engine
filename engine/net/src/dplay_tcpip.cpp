#include "net/dplay_tcpip.h"

#include <cstring>
#include <deque>
#include <map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
namespace {
using sock_t = SOCKET;
const sock_t kBad = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
void nonblocking(sock_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
bool would_block() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
bool startup() {
    static bool done = false;
    if (done) return true;
    WSADATA d;
    done = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    return done;
}
void no_udp_reset(sock_t s) {
    // A datagram to a closed port must not make the next receive fail.
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
}
using socklen = int;
}  // namespace
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
using sock_t = int;
const sock_t kBad = -1;
void close_sock(sock_t s) { close(s); }
void nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY; }
bool startup() { return true; }
void no_udp_reset(sock_t) {}
using socklen = socklen_t;
}  // namespace
#endif

namespace net::dplay {
namespace {

sockaddr_in to_os(const SockAddr& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_port = htons(a.port);
    std::memcpy(&s.sin_addr, &a.ip, 4);
    return s;
}

SockAddr from_os(const sockaddr_in& s) {
    SockAddr a;
    a.port = ntohs(s.sin_port);
    std::memcpy(&a.ip, &s.sin_addr, 4);
    return a;
}

Bytes address_bytes(const SockAddr& stream, const SockAddr& datagram) { return tcpip_sp_data(stream, datagram); }

struct Conn {
    sock_t s = kBad;
    Bytes in, out;
    bool outgoing = false, connecting = false, dead = false, enum_conn = false, close_when_sent = false;
    SockAddr peer;       // outgoing: the target; incoming: the sender's stream address once known
    bool peer_known = false;
    std::uint32_t source_ip = 0;
};

class Tcpip final : public Provider {
public:
    explicit Tcpip(const TcpipOptions& o) : opt_(o) {}
    ~Tcpip() override {
        for (auto& c : conns_)
            if (c->s != kBad) close_sock(c->s);
        for (sock_t s : {listen_, dgram_, enum_udp_, enum_tcp_})
            if (s != kBad) close_sock(s);
    }

    bool open(std::string& error) {
        if (!startup()) {
            error = "socket start-up failed";
            return false;
        }
        listen_ = bind_first(SOCK_STREAM, opt_.stream_port, stream_);
        if (listen_ == kBad) {
            error = "no free stream port from " + std::to_string(opt_.stream_port);
            return false;
        }
        ::listen(listen_, 16);
        dgram_ = bind_first(SOCK_DGRAM, opt_.datagram_port, datagram_);
        if (dgram_ == kBad) {
            error = "no free datagram port from " + std::to_string(opt_.datagram_port);
            return false;
        }
        int on = 1;
        setsockopt(dgram_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
        no_udp_reset(dgram_);
        return true;
    }

    const char* name() const override { return "TCP/IP"; }

    Bytes local_sp_data() const override { return address_bytes(stream_, datagram_); }

    Bytes complete_sp_data(const Bytes& sp, const Bytes& reply_to) const override {
        SockAddr s, d, rs, rd;
        if (!tcpip_from_sp_data(sp, s, d)) return sp;
        if (!tcpip_from_sp_data(reply_to, rs, rd)) return sp;
        const std::uint32_t ip = rs.ip ? rs.ip : rd.ip;
        if (!s.ip) s.ip = ip;
        if (!d.ip) d.ip = ip;
        return address_bytes(s, d);
    }

    bool same_machine(const Bytes& a, const Bytes& b) const override {
        SockAddr as, ad, bs, bd;
        if (!tcpip_from_sp_data(a, as, ad) || !tcpip_from_sp_data(b, bs, bd)) return false;
        return as == bs;
    }

    std::string describe(const Bytes& address) const override {
        SockAddr s, d;
        if (!tcpip_from_sp_data(address, s, d)) return "-";
        return ip_text(s.ip) + ":" + std::to_string(s.port);
    }

    void send(const Bytes& to, const Bytes& payload, bool guaranteed) override {
        SockAddr s, d;
        if (!tcpip_from_sp_data(to, s, d)) return;
        if (!guaranteed && d.port) {
            const sockaddr_in a = to_os(d);
            ::sendto(dgram_, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()), 0,
                     reinterpret_cast<const sockaddr*>(&a), sizeof a);
            return;
        }
        Conn* c = outgoing(s);
        if (!c) return;
        const Bytes f = tcpip_frame(return_address(), payload);
        c->out.insert(c->out.end(), f.begin(), f.end());
        flush(*c);
    }

    bool receive(Inbound& out) override {
        if (in_.empty()) return false;
        out = std::move(in_.front());
        in_.pop_front();
        return true;
    }

    void pump() override {
        accept_all(listen_, false);
        accept_all(enum_tcp_, true);
        for (auto& c : conns_) {
            if (c->dead) continue;
            if (c->connecting) check_connected(*c);
            if (!c->connecting) {
                flush(*c);
                read(*c);
            }
        }
        read_datagrams(dgram_);
        read_datagrams(enum_udp_);
        // Broken links.
        for (auto it = conns_.begin(); it != conns_.end();) {
            Conn& c = **it;
            if (!c.dead) {
                ++it;
                continue;
            }
            if (c.s != kBad) close_sock(c.s);
            if (c.outgoing) {
                auto o = out_.find(c.peer);
                if (o != out_.end() && o->second == &c) out_.erase(o);
            }
            if (!c.enum_conn && !c.close_when_sent && c.peer_known) lost_.push_back(address_bytes(c.peer, SockAddr{}));
            it = conns_.erase(it);
        }
    }

    void enumerate(const Bytes& payload) override {
        const Bytes f = tcpip_frame(return_address(), payload);
        const std::uint32_t target = opt_.address.empty() ? 0xFFFFFFFFu : resolve_ip(opt_.address);
        if (!target) {
            error_ = "unknown address " + opt_.address;
            return;
        }
        const SockAddr to{opt_.enum_port, target};
        const sockaddr_in a = to_os(to);
        ::sendto(dgram_, reinterpret_cast<const char*>(f.data()), static_cast<int>(f.size()), 0, reinterpret_cast<const sockaddr*>(&a),
                 sizeof a);
        if (target != 0xFFFFFFFFu) {
            // The request also goes over TCP (as the original's machines send it).
            auto c = std::make_unique<Conn>();
            c->outgoing = true;
            c->close_when_sent = true;
            c->peer = to;
            if (start_connect(*c)) {
                c->out = f;
                conns_.push_back(std::move(c));
            }
        }
    }

    bool listen(std::string& error) override {
        SockAddr got;
        enum_udp_ = bind_port(SOCK_DGRAM, opt_.enum_port, got);
        enum_tcp_ = bind_port(SOCK_STREAM, opt_.enum_port, got);
        if (enum_udp_ == kBad || enum_tcp_ == kBad) {
            error = "the session port " + std::to_string(opt_.enum_port) + " is taken";
            return false;
        }
        no_udp_reset(enum_udp_);
        ::listen(enum_tcp_, 16);
        return true;
    }

    bool lost(Bytes& who) override {
        if (lost_.empty()) return false;
        who = std::move(lost_.front());
        lost_.pop_front();
        return true;
    }

    std::string error() const override { return error_; }

private:
    SockAddr return_address() const { return SockAddr{stream_.port, opt_.bind_ip}; }

    sock_t bind_port(int type, std::uint16_t port, SockAddr& got) {
        sock_t s = ::socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
        if (s == kBad) return kBad;
        const sockaddr_in a = to_os(SockAddr{port, opt_.bind_ip});
        if (::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
            close_sock(s);
            return kBad;
        }
        nonblocking(s);
        got = SockAddr{port, opt_.bind_ip};
        return s;
    }

    sock_t bind_first(int type, std::uint16_t first, SockAddr& got) {
        for (std::uint16_t p = first; p < first + 100; ++p) {
            sock_t s = bind_port(type, p, got);
            if (s != kBad) return s;
        }
        return kBad;
    }

    bool start_connect(Conn& c) {
        c.s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c.s == kBad) return false;
        nonblocking(c.s);
        int on = 1;
        setsockopt(c.s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
        const sockaddr_in a = to_os(c.peer);
        if (::connect(c.s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 && !would_block()) {
            close_sock(c.s);
            c.s = kBad;
            return false;
        }
        c.connecting = true;
        c.peer_known = true;
        return true;
    }

    Conn* outgoing(const SockAddr& to) {
        auto it = out_.find(to);
        if (it != out_.end()) return it->second;
        auto c = std::make_unique<Conn>();
        c->outgoing = true;
        c->peer = to;
        if (!start_connect(*c)) {
            lost_.push_back(address_bytes(to, SockAddr{}));
            return nullptr;
        }
        Conn* raw = c.get();
        conns_.push_back(std::move(c));
        out_[to] = raw;
        return raw;
    }

    void check_connected(Conn& c) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(c.s, &w);
        FD_SET(c.s, &e);
        timeval tv{0, 0};
        if (::select(static_cast<int>(c.s + 1), nullptr, &w, &e, &tv) <= 0) return;
        if (FD_ISSET(c.s, &e)) {
            c.dead = true;
            return;
        }
        int err = 0;
        socklen len = sizeof err;
        getsockopt(c.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err) {
            c.dead = true;
            return;
        }
        c.connecting = false;
    }

    void flush(Conn& c) {
        if (c.connecting || c.dead) return;
        while (!c.out.empty()) {
            const int n = ::send(c.s, reinterpret_cast<const char*>(c.out.data()), static_cast<int>(c.out.size()), 0);
            if (n > 0) {
                c.out.erase(c.out.begin(), c.out.begin() + n);
                continue;
            }
            if (n < 0 && would_block()) return;
            c.dead = true;
            return;
        }
        if (c.close_when_sent) c.dead = true;
    }

    void read(Conn& c) {
        std::uint8_t buf[16384];
        for (;;) {
            const int n = ::recv(c.s, reinterpret_cast<char*>(buf), sizeof buf, 0);
            if (n > 0) {
                c.in.insert(c.in.end(), buf, buf + n);
                continue;
            }
            if (n < 0 && would_block()) break;
            c.dead = true;
            break;
        }
        std::size_t at = 0;
        while (c.in.size() - at >= 4) {
            const std::uint32_t size = frame_size(c.in.data() + at);
            if (size < kTcpipHeader) {
                c.dead = true;
                break;
            }
            if (c.in.size() - at < size) break;
            const std::uint8_t* f = c.in.data() + at;
            SockAddr ret = get_sockaddr(f + 4);
            if (!ret.ip) ret.ip = c.source_ip;
            if (!c.outgoing && !c.enum_conn) {
                c.peer = ret;
                c.peer_known = true;
            }
            Inbound in;
            in.payload.assign(f + kTcpipHeader, f + size);
            in.reply_to = address_bytes(ret, SockAddr{});
            in_.push_back(std::move(in));
            at += size;
        }
        c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(at));
    }

    void accept_all(sock_t l, bool enum_conn) {
        if (l == kBad) return;
        for (;;) {
            sockaddr_in a{};
            socklen len = sizeof a;
            sock_t s = ::accept(l, reinterpret_cast<sockaddr*>(&a), &len);
            if (s == kBad) return;
            nonblocking(s);
            auto c = std::make_unique<Conn>();
            c->s = s;
            c->enum_conn = enum_conn;
            c->source_ip = from_os(a).ip;
            conns_.push_back(std::move(c));
        }
    }

    void read_datagrams(sock_t s) {
        if (s == kBad) return;
        std::uint8_t buf[65536];
        for (;;) {
            sockaddr_in a{};
            socklen len = sizeof a;
            const int n = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&a), &len);
            if (n <= 0) {
                if (n < 0 && !would_block()) continue;  // a stale error of an earlier datagram
                return;
            }
            const SockAddr src = from_os(a);
            Inbound in;
            const auto un = static_cast<std::size_t>(n);
            if (un >= kTcpipHeader + 8 && frame_size(buf) == un && is_system(buf + kTcpipHeader, un - kTcpipHeader)) {
                SockAddr ret = get_sockaddr(buf + 4);
                if (!ret.ip) ret.ip = src.ip;
                in.payload.assign(buf + kTcpipHeader, buf + un);
                in.reply_to = address_bytes(ret, SockAddr{});
            } else {
                if (s != dgram_) continue;
                in.payload.assign(buf, buf + un);
                in.reply_to = address_bytes(SockAddr{0, src.ip}, src);
                in.datagram = true;
            }
            in_.push_back(std::move(in));
        }
    }

    TcpipOptions opt_;
    sock_t listen_ = kBad, dgram_ = kBad, enum_udp_ = kBad, enum_tcp_ = kBad;
    SockAddr stream_, datagram_;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::map<SockAddr, Conn*> out_;
    std::deque<Inbound> in_;
    std::deque<Bytes> lost_;
    std::string error_;
};

}  // namespace

std::uint32_t resolve_ip(const std::string& name) {
    if (!startup()) return 0;
    in_addr a{};
    if (inet_pton(AF_INET, name.c_str(), &a) == 1) {
        std::uint32_t ip = 0;
        std::memcpy(&ip, &a, 4);
        return ip;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return 0;
    std::uint32_t ip = 0;
    std::memcpy(&ip, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return ip;
}

std::unique_ptr<Provider> tcpip_provider(const TcpipOptions& options, std::string& error) {
    auto p = std::make_unique<Tcpip>(options);
    if (!p->open(error)) return nullptr;
    return p;
}

}  // namespace net::dplay
