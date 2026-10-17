#include "net/dplay_serial.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "net/dplay_tcpip.h"

// The serial provider's frame (the original's serial and modem providers, observed in the
// provider's receive state machine): an 8-byte head in front of each session message
//   u16 0x2BAD, u16 the frame's whole length (head included, at most 0xFFFF),
//   u16 check of the message bytes, u16 check of the head's first six bytes
// The check is the 16-bit cyclic code with the reflected polynomial 0xA001 (0x8005 reversed),
// starting from 0, without a final inversion. A receiver whose head does not check out drops
// one byte and looks again; a message whose check fails is dropped. The message itself is the
// session message as it is (a system message from its signature, or a game message: from, to,
// bytes); the link has no addresses, so the players' provider data is empty.

namespace net::dplay {
namespace {

std::uint16_t crc16(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? (c >> 1) ^ 0xA001u : c >> 1;
    }
    return static_cast<std::uint16_t>(c);
}

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- byte streams: a COM port, or a TCP connection standing in for the cable (tests)
class Stream {
public:
    virtual ~Stream() = default;
    virtual int read(std::uint8_t* p, int n) = 0;   // >= 0 bytes, -1 broken
    virtual int write(const std::uint8_t* p, int n) = 0;
    virtual bool open() const = 0;
};

#ifdef _WIN32
class ComPort final : public Stream {
public:
    ComPort(const SerialOptions& o, std::string& error) {
        const std::string path = o.port.rfind("\\\\.\\", 0) == 0 ? o.port : "\\\\.\\" + o.port;
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            error = "cannot open " + o.port;
            return;
        }
        SetupComm(h_, 4096, 4096);
        DCB d{};
        d.DCBlength = sizeof d;
        GetCommState(h_, &d);
        d.BaudRate = o.baud;
        d.ByteSize = 8;
        d.fBinary = TRUE;
        d.Parity = o.parity == 1 ? ODDPARITY : o.parity == 2 ? EVENPARITY : o.parity == 3 ? MARKPARITY : NOPARITY;
        d.fParity = o.parity != 0;
        d.StopBits = o.stop_bits == 1 ? ONE5STOPBITS : o.stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
        d.fOutX = d.fInX = o.flow == 1;
        d.fOutxCtsFlow = o.flow >= 2;
        d.fRtsControl = o.flow >= 2 ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
        d.fOutxDsrFlow = o.flow == 3;
        d.fDtrControl = o.flow == 3 ? DTR_CONTROL_HANDSHAKE : DTR_CONTROL_ENABLE;
        SetCommState(h_, &d);
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;  // reads return at once with what is there
        t.WriteTotalTimeoutConstant = 2000;
        SetCommTimeouts(h_, &t);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        EscapeCommFunction(h_, SETDTR);
    }
    ~ComPort() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    int read(std::uint8_t* p, int n) override {
        DWORD got = 0;
        if (!ReadFile(h_, p, static_cast<DWORD>(n), &got, nullptr)) return -1;
        return static_cast<int>(got);
    }
    int write(const std::uint8_t* p, int n) override {
        DWORD put = 0;
        if (!WriteFile(h_, p, static_cast<DWORD>(n), &put, nullptr)) return -1;
        return static_cast<int>(put);
    }
    bool open() const override { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};
using sock_t = SOCKET;
const sock_t kBadSock = INVALID_SOCKET;
void close_sock(sock_t s) { closesocket(s); }
bool sock_would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void sock_nonblocking(sock_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
bool sock_startup() {
    static bool done = false;
    if (!done) {
        WSADATA w;
        done = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }
    return done;
}
#else
class ComPort final : public Stream {
public:
    ComPort(const SerialOptions& o, std::string& error) {
        fd_ = ::open(o.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            error = "cannot open " + o.port;
            return;
        }
        termios t{};
        tcgetattr(fd_, &t);
        cfmakeraw(&t);
        tcsetattr(fd_, TCSANOW, &t);
    }
    ~ComPort() override {
        if (fd_ >= 0) ::close(fd_);
    }
    int read(std::uint8_t* p, int n) override {
        const ssize_t r = ::read(fd_, p, static_cast<std::size_t>(n));
        if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        return static_cast<int>(r);
    }
    int write(const std::uint8_t* p, int n) override { return static_cast<int>(::write(fd_, p, static_cast<std::size_t>(n))); }
    bool open() const override { return fd_ >= 0; }

private:
    int fd_ = -1;
};
using sock_t = int;
const sock_t kBadSock = -1;
void close_sock(sock_t s) { ::close(s); }
bool sock_would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
void sock_nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool sock_startup() { return true; }
#endif

// "tcp:PORT" waits on 127.0.0.1:PORT for the other end; "tcp:HOST:PORT" connects to it.
class TcpCable final : public Stream {
public:
    TcpCable(const std::string& spec, std::string& error) {
        if (!sock_startup()) {
            error = "socket start-up failed";
            return;
        }
        const std::string rest = spec.substr(4);
        const std::size_t colon = rest.rfind(':');
        sockaddr_in a{};
        a.sin_family = AF_INET;
        if (colon == std::string::npos) {
            listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            a.sin_port = htons(static_cast<std::uint16_t>(std::atoi(rest.c_str())));
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int on = 1;
            setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
            if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_, 1) != 0) {
                error = "cannot wait on " + spec;
                return;
            }
            sock_nonblocking(listen_);
        } else {
            s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            a.sin_port = htons(static_cast<std::uint16_t>(std::atoi(rest.c_str() + colon + 1)));
            const std::uint32_t ip = resolve_ip(rest.substr(0, colon));
            std::memcpy(&a.sin_addr, &ip, 4);
            if (::connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                error = "cannot reach " + spec;
                close_sock(s_);
                s_ = kBadSock;
                return;
            }
            sock_nonblocking(s_);
        }
        ok_ = true;
    }
    ~TcpCable() override {
        if (s_ != kBadSock) close_sock(s_);
        if (listen_ != kBadSock) close_sock(listen_);
    }
    int read(std::uint8_t* p, int n) override {
        if (s_ == kBadSock) {
            if (listen_ == kBadSock) return -1;
            sockaddr_in a{};
#ifdef _WIN32
            int len = sizeof a;
#else
            socklen_t len = sizeof a;
#endif
            s_ = ::accept(listen_, reinterpret_cast<sockaddr*>(&a), &len);
            if (s_ == kBadSock) return 0;
            sock_nonblocking(s_);
        }
        const int r = static_cast<int>(::recv(s_, reinterpret_cast<char*>(p), n, 0));
        if (r > 0) return r;
        if (r < 0 && sock_would_block()) return 0;
        return -1;
    }
    int write(const std::uint8_t* p, int n) override {
        if (s_ == kBadSock) return 0;
        const int r = static_cast<int>(::send(s_, reinterpret_cast<const char*>(p), n, 0));
        if (r < 0) return sock_would_block() ? 0 : -1;
        return r;
    }
    bool open() const override { return ok_; }

private:
    sock_t s_ = kBadSock, listen_ = kBadSock;
    bool ok_ = false;
};

#ifdef _WIN32
// "pipe:NAME": a named pipe \\.\pipe\NAME this program serves (tests: a program whose serial
// port is redirected to the pipe sits at the other end).
class PipeCable final : public Stream {
public:
    PipeCable(const std::string& spec, std::string& error) {
        const std::string name = "\\\\.\\pipe\\" + spec.substr(5);
        h_ = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) error = "cannot make the pipe " + name;
    }
    ~PipeCable() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    int read(std::uint8_t* p, int n) override {
        if (!connected_) {
            ConnectNamedPipe(h_, nullptr);  // non-blocking: ERROR_PIPE_CONNECTED once the other end opened it
            if (GetLastError() != ERROR_PIPE_CONNECTED) return 0;
            connected_ = true;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (!avail) return 0;
        DWORD got = 0;
        if (!ReadFile(h_, p, std::min<DWORD>(avail, static_cast<DWORD>(n)), &got, nullptr)) return -1;
        return static_cast<int>(got);
    }
    int write(const std::uint8_t* p, int n) override {
        if (!connected_) return 0;
        DWORD put = 0;
        if (!WriteFile(h_, p, static_cast<DWORD>(n), &put, nullptr)) return GetLastError() == ERROR_NO_DATA ? -1 : 0;
        return static_cast<int>(put);
    }
    bool open() const override { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    bool connected_ = false;
};
#endif

class Serial final : public Provider {
public:
    Serial(const SerialOptions& o, std::unique_ptr<Stream> s) : opt_(o), s_(std::move(s)) {
        if (opt_.modem) {
            modem_ = Modem::Init;
            command(opt_.init);
        }
    }
    const char* name() const override { return opt_.modem ? "Modem" : "Serial"; }
    Bytes local_sp_data() const override { return {}; }
    bool same_machine(const Bytes&, const Bytes&) const override { return true; }  // one other end
    std::string describe(const Bytes&) const override { return opt_.port; }

    void send(const Bytes&, const Bytes& payload, bool) override {
        const Bytes f = serial_frame(payload);
        if (modem_ != Modem::Online) {
            waiting_.push_back(f);  // until the call is up
            return;
        }
        out_.insert(out_.end(), f.begin(), f.end());
        flush();
    }
    bool receive(Inbound& out) override {
        if (in_.empty()) return false;
        out = std::move(in_.front());
        in_.pop_front();
        return true;
    }
    void pump() override {
        if (broken_) return;
        std::uint8_t buf[4096];
        for (;;) {
            const int n = s_->read(buf, sizeof buf);
            if (n < 0) {
                broken_ = true;
                lost_ = true;
                return;
            }
            if (n == 0) break;
            stream_.insert(stream_.end(), buf, buf + n);
        }
        if (modem_ != Modem::Online) modem_step();
        if (modem_ == Modem::Online) {
            Bytes payload;
            while (serial_unframe(stream_, payload)) {
                Inbound in;
                in.payload = std::move(payload);
                in_.push_back(std::move(in));
            }
            while (!waiting_.empty()) {
                out_.insert(out_.end(), waiting_.front().begin(), waiting_.front().end());
                waiting_.pop_front();
            }
        }
        flush();
    }
    void enumerate(const Bytes& payload) override { send({}, payload, true); }
    bool listen(std::string&) override { return true; }
    bool lost(Bytes& who) override {
        if (!lost_) return false;
        lost_ = false;
        who.clear();
        return true;
    }
    bool ready() const override { return modem_ == Modem::Online; }
    std::string error() const override { return error_; }

private:
    // Hayes commands: reset, then dial or wait for a ring and answer; "CONNECT" starts the
    // frames (the original's modem provider goes through the system's telephony service, which
    // speaks the same commands to the modem; the far end only ever sees the connection).
    enum class Modem : std::uint8_t { Online, Init, Dialing, WaitRing, Answering, Failed };
    void command(const std::string& c) {
        const std::string line = c + "\r";
        out_.insert(out_.end(), line.begin(), line.end());
        flush();
        sent_at_ = now_ms();
    }
    bool line(std::string& l) {
        for (std::size_t i = 0; i < stream_.size(); ++i)
            if (stream_[i] == '\n' || stream_[i] == '\r') {
                l.assign(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(i));
                stream_.erase(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                return true;
            }
        return false;
    }
    void modem_step() {
        std::string l;
        while (modem_ != Modem::Online && line(l)) {
            if (l.empty()) continue;
            if (modem_ == Modem::Init && l.find("OK") != std::string::npos) {
                if (opt_.answer) modem_ = Modem::WaitRing;
                else {
                    modem_ = Modem::Dialing;
                    command("ATDT" + opt_.phone);
                }
            } else if (modem_ == Modem::WaitRing && l.find("RING") != std::string::npos) {
                modem_ = Modem::Answering;
                command("ATA");
            } else if ((modem_ == Modem::Dialing || modem_ == Modem::Answering) && l.find("CONNECT") != std::string::npos) {
                modem_ = Modem::Online;
            } else if (l.find("NO CARRIER") != std::string::npos || l.find("BUSY") != std::string::npos || l.find("NO DIALTONE") != std::string::npos ||
                       l.find("ERROR") != std::string::npos) {
                modem_ = Modem::Failed;
                error_ = "modem: " + l;
                lost_ = true;
            }
        }
        if (modem_ == Modem::Init && now_ms() - sent_at_ > 3000) command(opt_.init);  // no answer yet: again
    }
    void flush() {
        while (!out_.empty()) {
            const int n = s_->write(out_.data(), static_cast<int>(out_.size()));
            if (n < 0) {
                broken_ = true;
                lost_ = true;
                return;
            }
            if (n == 0) return;
            out_.erase(out_.begin(), out_.begin() + n);
        }
    }

    SerialOptions opt_;
    std::unique_ptr<Stream> s_;
    Bytes stream_, out_;
    std::deque<Bytes> waiting_;
    std::deque<Inbound> in_;
    Modem modem_ = Modem::Online;
    std::uint32_t sent_at_ = 0;
    bool broken_ = false, lost_ = false;
    std::string error_;
};

}  // namespace

std::vector<std::uint8_t> serial_frame(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> f(8 + payload.size());
    const std::size_t len = f.size();
    f[0] = 0xAD;
    f[1] = 0x2B;
    f[2] = static_cast<std::uint8_t>(len);
    f[3] = static_cast<std::uint8_t>(len >> 8);
    std::memcpy(f.data() + 8, payload.data(), payload.size());
    const std::uint16_t body = crc16(f.data() + 8, payload.size());
    f[4] = static_cast<std::uint8_t>(body);
    f[5] = static_cast<std::uint8_t>(body >> 8);
    const std::uint16_t head = crc16(f.data(), 6);
    f[6] = static_cast<std::uint8_t>(head);
    f[7] = static_cast<std::uint8_t>(head >> 8);
    return f;
}

bool serial_unframe(std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& payload) {
    for (;;) {
        if (stream.size() < 8) return false;
        const std::uint8_t* h = stream.data();
        const std::size_t len = static_cast<std::size_t>(h[2] | (h[3] << 8));
        const bool head_ok = h[0] == 0xAD && h[1] == 0x2B && crc16(h, 6) == static_cast<std::uint16_t>(h[6] | (h[7] << 8)) && len > 8;
        if (!head_ok) {
            stream.erase(stream.begin());  // look again one byte later
            continue;
        }
        if (stream.size() < len) return false;
        const bool body_ok = crc16(h + 8, len - 8) == static_cast<std::uint16_t>(h[4] | (h[5] << 8));
        if (body_ok) payload.assign(stream.begin() + 8, stream.begin() + static_cast<std::ptrdiff_t>(len));
        stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(len));
        if (body_ok) return true;
    }
}

std::unique_ptr<Provider> serial_provider(const SerialOptions& options, std::string& error) {
    std::unique_ptr<Stream> s;
    if (options.port.rfind("tcp:", 0) == 0) s = std::make_unique<TcpCable>(options.port, error);
#ifdef _WIN32
    else if (options.port.rfind("pipe:", 0) == 0) s = std::make_unique<PipeCable>(options.port, error);
#endif
    else s = std::make_unique<ComPort>(options, error);
    if (!s->open()) return nullptr;
    return std::make_unique<Serial>(options, std::move(s));
}

}  // namespace net::dplay
