#pragma once
// Typed commands (docs/RE_MEMORY.md, "Console commands"): a line is split into words, the
// first word names a command in a registry, and the command runs only when one of its
// permission bits is allowed by the caller. A default command takes words no entry names.
// The simulation parts some commands share live here too: placing a row of new units at a
// point and starting a meteor shower at once.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sim {

struct MeteorShower;
class Lcg15;

namespace console {

// One command line split into words: words are separated by C-locale white space, '#' ends
// the line, at most 20 words are kept and all words share a 128-byte buffer (126 characters
// of text with their terminators). A word that does not fit stops the copy without moving
// on, and the original then writes terminators past the buffer forever (`endless`).
struct Words {
    std::vector<std::string> words;
    bool endless = false;

    int count() const { return static_cast<int>(words.size()); }
    // Word i, or `fallback` when there is no such word.
    std::string text(int i, const std::string& fallback = std::string()) const;
    // Word i read as a C runtime atol(), or `fallback` when there is no such word.
    std::int32_t number(int i, std::int32_t fallback) const;
};
Words split(std::string_view line);
// "%N" words (N a number below the parent's word count) take the parent's word N.
void substitute(Words& words, const Words& parent);

// The C runtime's atol(): white space, a sign, decimal digits, wrapping at 32 bits.
std::int32_t c_atol(std::string_view s);
// Case-insensitive ordering of the C runtime's _stricmp (letters folded to lower case).
int c_stricmp(std::string_view a, std::string_view b);
// The wildcard match the default command uses ('?' one character, '*' any run, letters
// compared upper-cased): a set of up to 100 pattern positions advanced per name character.
bool wildcard_match(std::string_view name, std::string_view pattern);

using Handler = std::function<void(const Words&)>;

// The command registry: entries sorted by name (case-insensitive), each with a handler and
// permission bits; registering a name again replaces its handler and bits.
class Registry {
public:
    void add(const std::string& name, Handler handler, std::uint32_t bits);
    void set_default(Handler handler, std::uint32_t bits);
    // Runs the command the first word names when its bits meet `allowed`, else the default
    // command when its bits do. Returns the bits of the command that ran (0: none ran).
    std::uint32_t dispatch(const Words& words, std::uint32_t allowed) const;
    // Text of several lines (split at '\n'), each with "%N" words taken from `parent`.
    std::uint32_t run_text(std::string_view text, const Words& parent, std::uint32_t allowed) const;
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        Handler handler;
        std::uint32_t bits = 0;
    };
    std::vector<Entry> entries_;
    Handler default_;
    std::uint32_t default_bits_ = 0;
};

// The default command's row of new units (0x417890): for every type in 1..type_count-1 that
// `matches` accepts, in type order: every unit after the first moves the point right by the
// type's left extent (subtracting its model box's minimum x), structures snap to their
// footprint (`snap`), the unit is made (`create`), and the point moves right by 32 units plus
// the type's right extent; once it reaches `row_end_x` (16.16) the row starts again at
// x = 160 units, 160 units further down. `pos` (16.16) is left where the next unit would go.
// Returns how many types matched (made or not).
struct UnitRowHooks {
    std::function<bool(std::uint16_t type)> matches;
    std::function<std::array<std::int32_t, 2>(std::uint16_t type)> extents_x;  // model box min x, max x
    std::function<void(std::uint16_t type, std::array<std::int32_t, 3>& pos)> snap;
    std::function<void(std::uint16_t type, const std::array<std::int32_t, 3>& pos)> create;
};
int place_unit_row(std::array<std::int32_t, 3>& pos, std::int32_t row_end_x, int type_count, const UnitRowHooks& hooks);

// Starts a meteor shower now, whatever the schedule and the map's switch say: active, its end
// and next schedule from now, the next meteor at once, and origin and aim from four draws.
void start_meteor_shower(MeteorShower& m, std::uint32_t tick, std::int32_t map_width, std::int32_t map_height, Lcg15& rand);

}  // namespace console
}  // namespace sim
