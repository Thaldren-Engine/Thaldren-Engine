#pragma once
// GAF animation archives (anims\*.gaf): only the sequence directory is read here.

#include <cstdint>
#include <string>
#include <vector>

namespace ta {

struct GafDirectory {
    std::vector<std::string> sequence_names;  // in stored order (name field holds at most 31 characters)
};

// Parses the header and sequence entries. Returns false on a truncated or malformed file.
bool gaf_read_directory(const std::vector<std::uint8_t>& file, GafDirectory& out);

// The game's lookup: first sequence whose name matches case-insensitively, or -1.
int gaf_find_sequence(const GafDirectory& dir, const std::string& name);

} // namespace ta
