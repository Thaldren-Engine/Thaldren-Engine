// The original's totala.ini (spec: docs/RE_MEMORY.md, "Preferences file"): read beside the
// program with GetPrivateProfileIntA, section [Preferences] (0x49F5A0).
#pragma once

#include <cstdint>
#include <string>

namespace ta {

// An integer from Windows profile text as GetPrivateProfileIntA reads it: the first section
// named `section` (case-insensitive, blanks inside the brackets ignored; a later section of
// the same name is not read), the first `key` in it (case-insensitive, blanks around the name
// and the value ignored, lines starting with ';' skipped); a value in matching quotes loses
// them; then an optional sign and "0x" hex digits or decimal digits, kept modulo 2^32 (the
// rest of the text ignored; no digits: 0). No such key, or an empty value: `fallback`.
std::int32_t profile_int(const std::string& text, const std::string& section, const std::string& key, std::int32_t fallback);

// [Preferences] `key` of <game_dir>/totala.ini (0x49F5A0; no file: `fallback`).
std::int32_t preferences_int(const std::string& game_dir, const std::string& key, std::int32_t fallback);

// The session's unit limit at start-up (0x491200): [Preferences] UnitLimit of
// <game_dir>\totala.ini, default 250, more than 500 → 500, less than 20 → 20.
int preferences_unit_limit(const std::string& game_dir);

}  // namespace ta
