#include <cstring>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>

#include "net/protocol.h"
#include "net/transport.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
static const sock_t kBad = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static void nonblocking(sock_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
static bool startup() {
    static bool done = false;
    if (done) return true;
    WSADATA d;
    done = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    return done;
}
using socklen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBad = -1;
static void close_sock(sock_t s) { close(s); }
static void nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static bool startup() { return true; }
using socklen = socklen_t;
#endif

namespace net {
namespace {

constexpr std::uint8_t kGuaranteed = 1, kPlain = 2, kAck = 3, kRegister = 4;
constexpr std::uint32_t kResendMs = 100, kMaxTries = 100;

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool same(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

class Udp final : public Transport {
public:
    Udp(bool host, UdpOptions o) : host_(host), opt_(o), loss_state_(o.seed ? o.seed : 1) {}
    ~Udp() override {
        if (s_ != kBad) close_sock(s_);
    }
    bool open(const std::string& address, std::uint16_t port, std::string& error) {
        if (!startup()) {
            error = "socket start-up failed";
            return false;
        }
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == kBad) {
            error = "no socket";
            return false;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = host_ ? htons(port) : 0;
        if (::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            error = "cannot bind the port";
            return false;
        }
        nonblocking(s_);
        if (!host_) {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
                error = "unknown address";
                return false;
            }
            Link l;
            std::memcpy(&l.addr, res->ai_addr, sizeof l.addr);
            freeaddrinfo(res);
            links_.push_back(l);
        }
        return true;
    }
    void send(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override { route(nullptr, from, to, bytes, true); }
    void send_unguaranteed(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> bytes) override {
        route(nullptr, from, to, bytes, false);
    }
    bool receive(Message& out) override {
        pump();
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }
    void set_local_players(const std::vector<std::int32_t>& ids) override {
        ids_ = ids;
        if (!host_ && !links_.empty()) {
            std::vector<std::uint8_t> p(ids.size() * 4);
            for (std::size_t i = 0; i < ids.size(); ++i) put32(p.data() + 4 * i, static_cast<std::uint32_t>(ids[i]));
            transmit(links_[0], kRegister, 0, 0, p, true);
        }
    }
    void pump() override {
        // Receive everything waiting.
        std::uint8_t buf[65536];
        for (;;) {
            sockaddr_in from{};
            socklen len = sizeof from;
            const int n = static_cast<int>(::recvfrom(s_, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n < 13) {
                if (n < 0) break;
                continue;
            }
            Link* l = link(from);
            if (!l) {
                if (!host_) continue;  // a client hears only its host
                links_.push_back(Link{});
                links_.back().addr = from;
                l = &links_.back();
            }
            const std::uint8_t kind = buf[0];
            const std::uint32_t seq = get32(buf + 1);
            const std::int32_t f = static_cast<std::int32_t>(get32(buf + 5)), t = static_cast<std::int32_t>(get32(buf + 9));
            std::vector<std::uint8_t> payload(buf + 13, buf + n);
            if (kind == kAck) {
                l->unacked.erase(seq);
                continue;
            }
            if (kind == kGuaranteed || kind == kRegister) {
                std::vector<std::uint8_t> ack;
                transmit_raw(*l, kAck, seq, 0, 0, ack);
                if (!l->seen.insert(seq).second) {
                    ++counters_.duplicates;
                    continue;
                }
                while (l->seen.size() > 8192) l->seen.erase(l->seen.begin());
            }
            if (kind == kRegister) {
                l->ids.clear();
                for (std::size_t i = 0; i + 4 <= payload.size(); i += 4) l->ids.push_back(static_cast<std::int32_t>(get32(payload.data() + i)));
                continue;
            }
            if (host_) route(l, f, t, payload, kind == kGuaranteed);
            else inbox_.push_back(Message{f, t, std::move(payload)});
            ++counters_.delivered;
        }
        // Send again what is not acknowledged.
        const std::uint32_t now = now_ms();
        for (Link& l : links_) {
            if (l.dead) continue;
            for (auto& [seq, p] : l.unacked) {
                if (now - p.sent < kResendMs) continue;
                if (++p.tries > kMaxTries) {
                    l.dead = true;
                    break;
                }
                p.sent = now;
                ++counters_.resent;
                send_datagram(l, p.bytes);
            }
        }
    }
    bool connected() const override { return !(!host_ && !links_.empty() && links_[0].dead); }
    UdpCounters counters_;

private:
    struct Pending {
        std::vector<std::uint8_t> bytes;
        std::uint32_t sent = 0, tries = 0;
    };
    struct Link {
        sockaddr_in addr{};
        std::vector<std::int32_t> ids;
        std::uint32_t next_seq = 1;
        std::map<std::uint32_t, Pending> unacked;
        std::set<std::uint32_t> seen;
        bool dead = false;
    };
    Link* link(const sockaddr_in& a) {
        for (Link& l : links_)
            if (same(l.addr, a)) return &l;
        return nullptr;
    }
    void route(Link* origin, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes, bool guaranteed) {
        if (!host_) {
            if (!links_.empty()) transmit(links_[0], guaranteed ? kGuaranteed : kPlain, from, to, bytes, guaranteed);
            return;
        }
        const bool mine = std::find(ids_.begin(), ids_.end(), to) != ids_.end();
        if (origin && (to == 0 || mine)) {
            inbox_.push_back(Message{from, to, bytes});
            ++counters_.delivered;
        }
        for (Link& l : links_) {
            if (&l == origin || l.dead) continue;
            if (to == 0 || std::find(l.ids.begin(), l.ids.end(), to) != l.ids.end())
                transmit(l, guaranteed ? kGuaranteed : kPlain, from, to, bytes, guaranteed);
        }
    }
    void transmit(Link& l, std::uint8_t kind, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes, bool guaranteed) {
        const std::uint32_t seq = l.next_seq++;
        std::vector<std::uint8_t> d = datagram(kind, seq, from, to, bytes);
        if (guaranteed) l.unacked[seq] = Pending{d, now_ms(), 0};
        send_datagram(l, d);
    }
    void transmit_raw(Link& l, std::uint8_t kind, std::uint32_t seq, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes) {
        send_datagram(l, datagram(kind, seq, from, to, bytes));
    }
    static std::vector<std::uint8_t> datagram(std::uint8_t kind, std::uint32_t seq, std::int32_t from, std::int32_t to, const std::vector<std::uint8_t>& bytes) {
        std::vector<std::uint8_t> d(13 + bytes.size());
        d[0] = kind;
        put32(d.data() + 1, seq);
        put32(d.data() + 5, static_cast<std::uint32_t>(from));
        put32(d.data() + 9, static_cast<std::uint32_t>(to));
        std::copy(bytes.begin(), bytes.end(), d.begin() + 13);
        return d;
    }
    void send_datagram(const Link& l, const std::vector<std::uint8_t>& d) {
        if (opt_.loss > 0.0) {
            loss_state_ = loss_state_ * 1103515245u + 12345u;
            if (static_cast<double>((loss_state_ >> 8) & 0xFFFF) / 65536.0 < opt_.loss) {
                ++counters_.dropped_by_loss;
                return;
            }
        }
        ::sendto(s_, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0, reinterpret_cast<const sockaddr*>(&l.addr), sizeof l.addr);
    }

    bool host_;
    UdpOptions opt_;
    std::uint32_t loss_state_;
    sock_t s_ = kBad;
    std::deque<Link> links_;
    std::deque<Message> inbox_;
    std::vector<std::int32_t> ids_;
};

}  // namespace

std::unique_ptr<Transport> udp_host(std::uint16_t port, std::string& error, UdpOptions o) {
    auto t = std::make_unique<Udp>(true, o);
    if (!t->open("", port, error)) return nullptr;
    return t;
}

std::unique_ptr<Transport> udp_join(const std::string& address, std::uint16_t port, std::string& error, UdpOptions o) {
    auto t = std::make_unique<Udp>(false, o);
    if (!t->open(address, port, error)) return nullptr;
    return t;
}

UdpCounters udp_counters(const Transport& t) {
    if (const auto* u = dynamic_cast<const Udp*>(&t)) return u->counters_;
    return {};
}

}  // namespace net
