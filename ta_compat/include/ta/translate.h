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
// The C runtime's _strlwr in the "C" locale (0x4E8960): 'A'..'Z' lowered, other bytes kept.
std::string lower_ascii(std::string text);
// A name the original looks up in lower case (map names 0x434BF0 / 0x435A20, a map's
// description 0x435DA0): the translation of the lowered name, or the name as given when that
// equals the lowered name in any case (nothing found).
std::string translate_lowered(const std::string& name);
// The reverse lookup (0x4C5840): the base text of the first entry (in base-text byte order)
// whose translation equals `text` in any case; "" when there is none.
std::string untranslate(const std::string& text);
// The map info's map name (0x435DA0, providers 2 and 3): the name when Maps\<name>.ota opens,
// else the base text whose translation the name is (a translated map name), else the name.
// The map info carries that name (+0xA14) and the battle loads its files by it.
std::string map_file_name(Vfs& vfs, const std::string& name);

}  // namespace ta
