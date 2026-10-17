#pragma once
// The TCP/IP service provider of the session protocol (net/dplay_session.h), as the original's
// machines speak it:
//  - each machine listens for TCP connections on a "stream" port (the first free one from 2300)
//    and receives datagrams on a UDP port (the first free one from 2350); together they are its
//    provider data (two 16-byte socket addresses, the address left zero: the receiver fills in
//    the address the machine's messages come from);
//  - a host also answers session enumeration on UDP and TCP port 47624; a client sends its
//    request to both (to the given address, or as a broadcast when none is given);
//  - every system message and every guaranteed game message goes over TCP, framed by a 20-byte
//    header (size and token 0xFAB, then the sender's stream address for the answer); a machine
//    sends over connections it opened itself to the other's stream port and reads whatever
//    arrives on the connections others opened to it;
//  - game messages without delivery guarantee are bare UDP datagrams (from, to, bytes) to the
//    other machine's datagram port.

#include <cstdint>
#include <memory>
#include <string>

#include "net/dplay_session.h"

namespace net::dplay {

struct TcpipOptions {
    // The address the sockets are bound to: 0 = every address of the machine; kLoopback keeps
    // a game on this machine (tests).
    std::uint32_t bind_ip = 0;
    std::uint16_t stream_port = 2300;    // the first stream port tried (up to 100 more)
    std::uint16_t datagram_port = 2350;  // the first datagram port tried
    std::uint16_t enum_port = kEnumPort;
    // Client: the address sessions are asked of (a name or a dotted address; empty: broadcast).
    std::string address;
};

std::unique_ptr<Provider> tcpip_provider(const TcpipOptions& options, std::string& error);

// "a.b.c.d" or a host name -> the address (kLoopback form: first byte lowest); 0 on failure.
std::uint32_t resolve_ip(const std::string& name);

}  // namespace net::dplay
