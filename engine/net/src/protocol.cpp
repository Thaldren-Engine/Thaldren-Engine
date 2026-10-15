#include "net/protocol.h"

#include <array>

namespace net {
namespace {

struct Entry {
    std::uint16_t size;
    std::uint8_t phases;
};

constexpr std::array<Entry, 0x2D> kTable = {{
    {0, 0},      {0, 0},      {13, 7},     {3, 7},      {0, 0},     {65, 7},     {1, 7},    {1, 7},     // 0x00-0x07
    {1, 7},      {23, 4},     {7, 4},      {9, 4},      {11, 4},    {36, 4},     {14, 4},   {6, 4},     // 0x08-0x0F
    {22, 4},     {4, 4},      {5, 4},      {18, 7},     {24, 4},    {1, 6},      {17, 4},   {2, 7},     // 0x10-0x17
    {2, 7},      {3, 7},      {14, 1},     {6, 7},      {5, 7},     {9, 0},      {2, 6},    {5, 6},     // 0x18-0x1F
    {186, 7},    {10, 1},     {6, 1},      {14, 7},     {6, 7},     {5, 1},      {41, 7},   {17, 7},    // 0x20-0x27
    {58, 7},     {3, 7},      {2, 7},      {0, 0},      {3, 4},                                          // 0x28-0x2C
}};

}  // namespace

std::uint16_t packet_size(std::uint8_t type) { return type < kTable.size() ? kTable[type].size : 0; }

std::uint16_t packet_size_at(const std::uint8_t* p, std::size_t available) {
    if (available == 0) return 0;
    if (p[0] == packet::kUnitStates) return available >= 3 ? get16(p + 1) : 0;
    return packet_size(p[0]);
}

std::uint8_t packet_phases(std::uint8_t type) { return type < kTable.size() ? kTable[type].phases : 0; }

} // namespace net
