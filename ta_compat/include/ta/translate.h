#pragma once
// The game's translation table (gamedata\translate.tdf; docs/RE_MEMORY.md, "Language").
//
// Loading (0x4C54F0, at start-up with the program's language): every top-level section's
// name is a base text (case-sensitive), its field named after the language (field names
// case-insensitive) the translation; empty translations are not taken, a later section of
// the same name replaces an earlier one. Nothing is reloaded for the language already loaded
// (compared case-insensitively). Lookup (0x4C5740): the exact base text's translation, else the
// text itself.

#include <string>

namespace ta {

class Vfs;

// Loads the table for `language` from `vfs` (a missing or unreadable file leaves it empty).
void load_translations(Vfs& vfs, const std::string& language);
// Builds the table from a translate.tdf's bytes (tests).
void load_translations(const std::string& tdf_text, const std::string& language);
// The translation of `text`, or `text` itself.
const std::string& translate(const std::string& text);

}  // namespace ta
