#pragma once
// Reader for TA's HAPI archives (.hpi, .ufo, .ccx, .gp3) and the layered
// virtual file system the game builds from them.
//
// Format (see docs/RE_MEMORY.md, "HPI archives"):
//   header (20 bytes): "HAPI", u32 version 0x00010000, u32 directory_size,
//                      u8 key (+3 pad), u32 root_node_offset
//   bytes [20, directory_size) hold the directory tree, XOR-obscured when key != 0.
//   node:  u32 entry_count, u32 entries_offset
//   entry (9 bytes): u32 name_offset, u32 data_offset, u8 flags (bit0 = directory)
//   file record (9 bytes): u32 data_offset, u32 size, u8 compression
//   compressed files: u32 chunk sizes for each 64 KiB block, then SQSH chunks.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ta {

enum class HpiError {
    None,
    OpenFailed,
    BadHeader,
    BadTrailer,
    BadDirectory,
    ReadFailed,
    BadChunk,
    BadChunkChecksum,
    BadChunkSize,
    BadCompression,
};

const char* to_string(HpiError e);

// Decode one SQSH chunk into `out` (appended). Exposed for tests.
HpiError unpack_sqsh(std::vector<std::uint8_t>& chunk, std::vector<std::uint8_t>& out);

// Number of SQSH chunks decoded so far, per compression method (0-3). Diagnostics only.
const std::uint64_t* sqsh_method_counts();

// LZ77 variant with a 4 KiB window. Exposed for tests.
void decompress_lz77(const std::uint8_t* in, std::size_t in_size, std::vector<std::uint8_t>& out);

struct HpiEntry {
    std::string name;
    bool is_dir = false;
    // Directory
    std::vector<HpiEntry> children;
    // File
    std::uint32_t data_offset = 0;
    std::uint32_t size = 0;
    std::uint8_t compression = 0;
};

class HpiArchive {
public:
    static std::unique_ptr<HpiArchive> open(const std::filesystem::path& path, HpiError& err);

    const std::filesystem::path& path() const { return path_; }
    const HpiEntry& root() const { return root_; }

    // Path lookup using the game's rules: '\\' or '/' separators, case-insensitive,
    // entries searched from last to first within a directory.
    const HpiEntry* find(std::string_view path) const;

    HpiError read(const HpiEntry& file, std::vector<std::uint8_t>& out);

private:
    HpiArchive() = default;
    bool read_raw(std::uint32_t offset, std::uint8_t* dst, std::size_t size);

    std::filesystem::path path_;
    std::ifstream file_;
    std::uint8_t key_ = 0;
    HpiEntry root_;
};

// The game's layered file system: loose files under the root directory first,
// then archives in mount order (earlier mounts take precedence).
class Vfs {
public:
    // Mounts archives from `game_dir` exactly as TA 3.1 does:
    //   rev31.gp3, *.ccx, *.ufo, then *.hpi (at most 10 newly mounted).
    // Globs are matched in case-insensitive name order.
    void mount_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* log = nullptr);

    bool mount(const std::filesystem::path& archive, HpiError& err);

    const std::vector<std::unique_ptr<HpiArchive>>& archives() const { return archives_; }

    // Read a file by game path (e.g. "units\\armcom.fbi"). Returns false if absent.
    bool read(std::string_view path, std::vector<std::uint8_t>& out, std::string* source = nullptr);

private:
    std::filesystem::path root_;
    std::vector<std::unique_ptr<HpiArchive>> archives_;
};

} // namespace ta
