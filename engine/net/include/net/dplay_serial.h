#pragma once
// The serial (null-modem cable) and modem service providers of the session protocol
// (net/dplay_session.h): two machines on one byte stream. The same system and game messages as
// over TCP/IP travel in the provider's frames (see dplay_serial.cpp and docs/RE_MEMORY.md,
// "DirectPlay wire protocol"); a modem first dials or answers with the modem's AT commands and
// then carries the same stream.
//
// The byte stream is a COM port ("COM1".."COM256"), or for tests a local stand-in: "tcp:PORT"
// listens on 127.0.0.1:PORT for the other end, "tcp:HOST:PORT" connects to it (a null-modem
// cable between two programs on one machine).

#include <cstdint>
#include <memory>
#include <string>

#include "net/dplay_session.h"

namespace net::dplay {

struct SerialOptions {
    std::string port = "COM1";
    // The original's serial settings dialog: speed, stop bits, parity, flow control.
    std::uint32_t baud = 57600;
    std::uint8_t stop_bits = 0;  // 0: one, 1: one and a half, 2: two
    std::uint8_t parity = 0;     // 0 none, 1 odd, 2 even, 3 mark
    std::uint8_t flow = 3;       // 0 none, 1 XON/XOFF, 2 RTS, 3 RTS/DTR (hardware)
    // Modem: dial `phone`, or wait for a call and answer it.
    bool modem = false;
    bool answer = false;
    std::string phone;
    std::string init = "ATZ";
};

std::unique_ptr<Provider> serial_provider(const SerialOptions& options, std::string& error);

// The provider's framing on its own (tests): wraps a payload in a frame / takes the complete
// frames off the front of a received byte stream.
std::vector<std::uint8_t> serial_frame(const std::vector<std::uint8_t>& payload);
bool serial_unframe(std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& payload);

}  // namespace net::dplay
