#pragma once
// The build restrictions' list files (the multiplayer room's Load List / Save List,
// SAVEGAME\<name>.LST) and the file listing the original's load / save menus share.
//
// File (0x44B230 writes it, 0x44B140 reads it; little-endian int32s, no header text):
//   int32 n            the unit definition table's size less one (every definition, the
//                      "norestrict" ones too)
//   then for each definition index 1 .. size-1 that the restrictions list (not "norestrict"),
//   in definition index order:
//   int32 key          the definition's key (its FBI file's simple checksum, def +0x13E)
//   int32 count        its count (0..100, -1 no limit)
// Reading takes n pairs whatever the file holds: a short read leaves the last pair's words as
// they were (the C library's fread copies what it got), so the pairs past the end repeat the
// last one. Each pair's key picks the first definition (index 1 up) with that key; the listed
// row of that definition takes the count; a key no definition has, or a definition not listed,
// changes nothing.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta {

// One row of the restrictions' list: its definition's index (1..) and its count.
struct RestrictListRow {
    std::int32_t def_index = 0;
    std::int32_t count = 0;
};

// `def_keys`: every definition's key by index (index 0, the blank definition, included).
void write_restrict_list(const std::vector<std::uint32_t>& def_keys, const std::vector<RestrictListRow>& rows, std::vector<std::uint8_t>& out);
// Applies a list file to the rows (their counts). Returns the number of pairs read in whole.
int read_restrict_list(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint32_t>& def_keys, std::vector<RestrictListRow>& rows);

// The files <dir>\*.<ext> (case-insensitive; not directories, not names starting with '.') in
// the order the original's load / save menus list them (0x4AF320 with sorting, 0x4AEFA0): the
// directory's order (NTFS: by upper-case name), then stably by last write time (whole seconds),
// newest first.
std::vector<std::filesystem::path> list_files_newest_first(const std::string& dir, const std::string& ext);

// The name a list shows for a file: up to its last '.'.
std::string name_without_extension(const std::string& file_name);

}  // namespace ta
