#pragma once
// Game packets: one type byte followed by a fixed number of bytes per type (the unit state
// packet 0x2C carries its own length in bytes 1-2). Several packets travel together in one
// frame (net/frames.h). See docs/RE_MEMORY.md, "Network and sync".

#include <cstdint>
#include <cstring>
#include <vector>

namespace net {

namespace packet {
constexpr std::uint8_t kPing = 0x02;           // 13: echo request/answer with the sender's clock
constexpr std::uint8_t kChat = 0x05;           // 65: text (64 bytes, zero padded)
constexpr std::uint8_t kProbe = 0x06;          // 1: "are you there", answered with kProbeReply
constexpr std::uint8_t kProbeReply = 0x07;     // 1
constexpr std::uint8_t kGameStart = 0x08;      // 1: the host starts loading
constexpr std::uint8_t kUnitCreated = 0x09;    // 23
constexpr std::uint8_t kTransport = 0x0A;      // 7
constexpr std::uint8_t kDamage = 0x0B;         // 9
constexpr std::uint8_t kKilled = 0x0C;         // 11
constexpr std::uint8_t kFire = 0x0D;           // 36
constexpr std::uint8_t kDetonate = 0x0E;       // 14
constexpr std::uint8_t kFeature = 0x0F;        // 6
constexpr std::uint8_t kScriptCall = 0x10;     // 22
constexpr std::uint8_t kActivate = 0x11;       // 4
constexpr std::uint8_t kFinished = 0x12;       // 5
constexpr std::uint8_t kSound = 0x13;          // 18
constexpr std::uint8_t kOwnership = 0x14;      // 24
constexpr std::uint8_t kLoaded = 0x15;         // 1
constexpr std::uint8_t kShare = 0x16;          // 17
constexpr std::uint8_t kStartPosition = 0x17;  // 2
constexpr std::uint8_t kReadyByte = 0x18;      // 2: the sender's side record ready byte (+0x96)
constexpr std::uint8_t kSpeed = 0x19;          // 3: [1] 0 = pause flag in [2], else game speed [2]
constexpr std::uint8_t kSync = 0x1A;           // 14: unit definition checksums
constexpr std::uint8_t kStatus = 0x1B;         // 6: net id, reason (player leaving)
constexpr std::uint8_t kDisconnect = 0x1C;     // 5
constexpr std::uint8_t kFrameTiming = 0x1D;    // 9 (ignored by the receiver)
constexpr std::uint8_t kRecordByte = 0x1E;     // 2: sets a byte of the receiver's record (+0x147), answered by 0x1F
constexpr std::uint8_t kRecordAck = 0x1F;      // 5: net id whose answer arrived
constexpr std::uint8_t kSideRecord = 0x20;     // 186: a player's battle room record
constexpr std::uint8_t kTeam = 0x21;           // 10: team request (host assigns)
constexpr std::uint8_t kTeamSet = 0x22;        // 6: net id, team
constexpr std::uint8_t kAlliance = 0x23;       // 14
constexpr std::uint8_t kRecordFlag = 0x24;     // 6: net id, record byte +0x13F
constexpr std::uint8_t kModified = 0x25;       // 5
constexpr std::uint8_t kSlotTable = 0x26;      // 41: the host's net id per slot
constexpr std::uint8_t kCheatWarning = 0x27;   // 17
constexpr std::uint8_t kViewState = 0x28;      // 58: a player's totals for allies' displays
constexpr std::uint8_t kViewAck = 0x29;        // 3
constexpr std::uint8_t kViewFlag = 0x2A;       // 2
constexpr std::uint8_t kUnitStates = 0x2C;     // variable (bytes 1-2), at least 3
}  // namespace packet

// Size of a packet of `type` (0 for types with no size: 0, 1, 4, 0x2B and above 0x2C).
std::uint16_t packet_size(std::uint8_t type);
// Size of the packet starting at `p` (the unit state packet reads its length field).
std::uint16_t packet_size_at(const std::uint8_t* p, std::size_t available);

// Phases of the game a packet is accepted in: bit 0 menus/battle room, bit 1 loading,
// bit 2 battle.
std::uint8_t packet_phases(std::uint8_t type);
enum class Phase : std::uint8_t { Room = 1, Loading = 2, Battle = 4 };

// Little-endian field helpers.
inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline float getf(const std::uint8_t* p) {
    const std::uint32_t u = get32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline void putf(std::uint8_t* p, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    put32(p, u);
}

} // namespace net
