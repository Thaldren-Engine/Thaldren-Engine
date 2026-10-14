#pragma once
// Compiled unit scripts (scripts\*.cob) -> engine script programs
// (see docs/RE_MEMORY.md, "Unit script VM").

#include <cstdint>
#include <string>
#include <vector>

#include "sim/script_vm.h"

namespace ta {

struct CobFile {
    sim::ScriptProgram program;
    std::vector<std::string> sound_names;  // optional sound table
    std::uint32_t version = 0;
};

// Parses a COB image. Fails only on a truncated or inconsistent file.
bool load_cob(const std::vector<std::uint8_t>& file, CobFile& out, std::string& error);

// How the original decodes one code word when execution reaches it.
sim::ScriptOp cob_decode(std::uint32_t word);

} // namespace ta
