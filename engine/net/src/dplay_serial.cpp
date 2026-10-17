#include "net/dplay_serial.h"

namespace net::dplay {

std::vector<std::uint8_t> serial_frame(const std::vector<std::uint8_t>& payload) { return payload; }

bool serial_unframe(std::vector<std::uint8_t>& stream, std::vector<std::uint8_t>& payload) {
    (void)stream;
    (void)payload;
    return false;
}

std::unique_ptr<Provider> serial_provider(const SerialOptions& options, std::string& error) {
    (void)options;
    error = "the serial provider is not built yet";
    return nullptr;
}

}  // namespace net::dplay
