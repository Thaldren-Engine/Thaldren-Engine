// Post-link step for fpu_capture.exe: marks the image as a Windows 4.0
// program (OS and subsystem version 4.0) and clears DllCharacteristics, which
// current linkers refuse to emit but Windows 95's loader requires.
//
// Usage: pe_win95 <file.exe>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pe_win95 <file.exe>\n");
        return 2;
    }
    std::vector<char> b;
    {
        std::ifstream f(argv[1], std::ios::binary);
        b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (b.size() < 0x200 || b[0] != 'M' || b[1] != 'Z') {
        std::fprintf(stderr, "not an MZ image\n");
        return 1;
    }
    std::uint32_t pe;
    std::memcpy(&pe, &b[0x3C], 4);
    if (pe + 0x60 > b.size() || std::memcmp(&b[pe], "PE\0\0", 4) != 0) {
        std::fprintf(stderr, "no PE header\n");
        return 1;
    }
    const std::size_t opt = pe + 24;  // optional header (PE32)
    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(&b[at], &v, 2); };
    put16(opt + 40, 4);   // MajorOperatingSystemVersion
    put16(opt + 42, 0);   // MinorOperatingSystemVersion
    put16(opt + 48, 4);   // MajorSubsystemVersion
    put16(opt + 50, 0);   // MinorSubsystemVersion
    put16(opt + 70, 0);   // DllCharacteristics
    std::ofstream o(argv[1], std::ios::binary | std::ios::trunc);
    o.write(b.data(), static_cast<std::streamsize>(b.size()));
    std::printf("pe_win95: %s marked as a Windows 4.0 image\n", argv[1]);
    return 0;
}
