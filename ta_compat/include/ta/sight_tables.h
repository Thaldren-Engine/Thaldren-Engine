#pragma once
// The original's line-of-sight shapes (see docs/RE_MEMORY.md, "Line of sight and radar"):
// the ring table of gamedata\los.tdf (true sight) and the masks of sequence
// "vismask" in anims\vismasks.gaf (circular sight).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sim/sight.h"
#include "ta/hpi.h"

namespace ta {

// [TABLEINFO] numtables; [TABLE<n>] numlines; line<i> = count, a1, b1, a2, b2, ...
// Each line gives four slices (one per quarter turn): (a, -b), (b, a), (-a, b), (-b, -a),
// stored as slices i, numlines + i, 2 numlines + i and 3 numlines + i. A missing line leaves
// its slices empty. False (with a reason) where the original would crash.
bool parse_sight_rings(std::string_view tdf_text, std::vector<sim::SightRing>& out, std::string& error);

// The frames of a GAF sequence as sight masks: a block is covered where the frame's byte
// differs from its transparency index. The original reads the frame data as raw pixels,
// so a compressed frame's bytes are taken as they are stored.
bool sight_masks_from_gaf(const std::vector<std::uint8_t>& gaf, std::string_view sequence, std::vector<sim::SightMask>& out,
                          std::string& error);

// Both, from the game's files.
bool load_sight_tables(Vfs& vfs, sim::SightTables& out, std::string& error);

} // namespace ta
