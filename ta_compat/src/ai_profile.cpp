#include "ta/ai_profile.h"

#include <cctype>

#include "ta/crt_strtod.h"
#include "ta/tdf.h"

namespace ta {
namespace {

// One command line split into words (the game's tokenizer): words are separated by C-locale
// white space, '#' ends the line, at most 20 words count and the words share a 128-byte
// buffer (126 characters of text).
struct TokenLine {
    std::vector<std::string> args;
    bool endless = false;  // a word overran the buffer: the original loops forever writing past it
};

bool c_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

TokenLine tokenize(std::string_view line) {
    TokenLine t;
    std::size_t at = 0, used = 0;
    const std::size_t n = line.size();
    for (;;) {
        while (at < n && c_space(line[at])) ++at;
        if (at == n || line[at] == '#') return t;
        std::string word;
        while (at < n && !c_space(line[at]) && line[at] != '#') {
            if (used >= 0x7E) {
                // The copy stops without moving on: every later word starts at the same
                // character, copies nothing and is terminated past the buffer's end.
                t.endless = true;
                return t;
            }
            word.push_back(line[at]);
            ++at;
            ++used;
        }
        ++used;  // the terminating zero
        if (t.args.size() < 20) t.args.push_back(word);
    }
}

std::string arg(const TokenLine& t, std::size_t i) { return i < t.args.size() ? t.args[i] : std::string(); }

// atol() of the 1998 runtime: white space, a sign, decimal digits, 32-bit wrap.
std::int32_t crt_atol(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && c_space(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    std::uint32_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10u + static_cast<std::uint32_t>(s[i++] - '0');
    return static_cast<std::int32_t>(neg ? 0u - v : v);
}

// The game's wildcard match of a unit name against a pattern ('?' one character, '*' any run;
// letters compared upper-cased), as the default command handler uses it: a list of pattern
// positions, where a '*' stays and adds the position after it (tried on the same character).
bool wildcard_match(const std::string& name, const std::string& pattern) {
    auto up = [](char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); };
    auto pat = [&](int i) { return static_cast<std::size_t>(i) < pattern.size() ? up(pattern[static_cast<std::size_t>(i)]) : '\0'; };
    std::vector<int> states{0};
    for (char raw : name) {
        const char c = up(raw);
        if (!c) break;
        for (std::size_t i = 0; i < states.size(); ++i) {
            const int s = states[i];
            const char p = pat(s);
            if (p == '?' || p == c) {
                states[i] = s + 1;
            } else if (p == '*') {
                if (states.size() < 100) states.push_back(s + 1);
            } else {
                states[i] = states.back();
                states.pop_back();
                if (states.empty()) return false;
                --i;
            }
        }
    }
    for (int s : states) {
        const char p = pat(s);
        if (p == '\0' || (p == '*' && pat(s + 1) == '\0')) return true;
    }
    return false;
}

// A unit name or a category name as a set of types (and whether it named a unit).
sim::CategoryMask name_mask(const UnitDefTable& defs, const std::string& name, bool& unit) {
    sim::CategoryMask m{};
    // Binary search of the sorted definitions (entry 0, "None", excluded).
    std::size_t lo = 1, count = defs.units.size() > 1 ? defs.units.size() - 1 : 0;
    while (count > 0) {
        const std::size_t step = count / 2;
        if (stricmp_c(defs.units[lo + step].unit_name, name) < 0) {
            lo += step + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    if (lo < defs.units.size() && stricmp_c(name, defs.units[lo].unit_name) == 0 && defs.units[lo].index != 0) {
        const std::uint16_t i = defs.units[lo].index;
        m[(i >> 5) & 15] |= 1u << (i & 31);
        unit = true;
        return m;
    }
    unit = false;
    if (const auto* c = defs.categories.find(name)) m = *c;
    return m;
}

void dispatch(const TokenLine& t, Vfs& vfs, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& r, int depth) {
    if (t.endless) {
        if (ai.fault.empty()) ai.fault = "profile line longer than the command buffer (the original never finishes it)";
        return;
    }
    if (t.args.empty()) return;
    ++r.lines;
    const std::string cmd = t.args[0];
    if (stricmp_c(cmd, "plan") == 0) {
        ++r.plans;
        sim::ai_profile_plan(ai, t.args);
        return;
    }
    if (stricmp_c(cmd, "weight") == 0) {
        ++r.weights;
        if (!ai.state.profile_gate) return;
        bool unit = false;
        const sim::CategoryMask m = name_mask(defs, arg(t, 1), unit);
        const float factor = t.args.size() > 2 ? static_cast<float>(crt_atof(t.args[2])) : 0.0f;
        sim::ai_profile_weight(ai, m, factor, unit);
        return;
    }
    if (stricmp_c(cmd, "limit") == 0) {
        ++r.limits;
        if (!ai.state.profile_gate) return;
        bool unit = false;
        const sim::CategoryMask m = name_mask(defs, arg(t, 1), unit);
        const std::int32_t limit = t.args.size() > 2 ? crt_atol(t.args[2]) : 0;
        sim::ai_profile_limit(ai, m, limit, unit);
        return;
    }
    // Anything else goes to the default handler: unit names matching the word are created at
    // the cursor (not carried out here), else debugdat\<word>.txt is run when it exists.
    ++r.other;
    if (r.defer_other) {
        std::string line;
        for (std::size_t k = 0; k < t.args.size(); ++k) line += (k ? " " : "") + t.args[k];
        r.deferred.push_back(line);
        return;
    }
    for (std::size_t i = 1; i < defs.units.size(); ++i)
        if (wildcard_match(defs.units[i].unit_name, cmd)) {
            r.notes.push_back("\"" + cmd + "\" would create " + defs.units[i].unit_name + " units at the cursor");
            return;
        }
    std::vector<std::uint8_t> inc;
    if (depth < 8 && vfs.read("debugdat\\" + cmd + ".txt", inc)) {
        ++r.includes;
        run_ai_commands(std::string(inc.begin(), inc.end()), vfs, defs, ai, r);
    }
}

void run_text(const std::string& text, Vfs& vfs, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& r, int depth) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        dispatch(tokenize(std::string_view(text).substr(at, end - at)), vfs, defs, ai, r, depth);
        at = end + 1;
    }
}

}  // namespace

void to_ai_type(const UnitDefRecord& d, const std::vector<sim::WeaponType>& weapons, sim::AiTypeInfo& out) {
    auto weapon = [&](int slot) -> const sim::WeaponType* {
        return slot >= 0 && static_cast<std::size_t>(slot) < weapons.size() ? &weapons[static_cast<std::size_t>(slot)] : nullptr;
    };
    out.type_flags = d.type_flags;
    out.abilities = d.can_do_flags;
    out.weapons = {weapon(d.weapon1), weapon(d.weapon2), weapon(d.weapon3)};
    out.build_cost_energy = d.build_cost_energy;
    out.build_cost_metal = d.build_cost_metal;
    out.energy_make = d.energy_make;
    out.energy_use = d.energy_use;
    out.extracts_metal = d.extracts_metal;
    out.wind_generator = d.wind_generator;
    out.tidal_generator = d.tidal_generator;
    out.makes_metal = d.makes_metal;
    out.structure = d.bm_code == 0;
    out.radar_distance = d.radar_distance;
    out.sonar_distance = d.sonar_distance;
    out.min_water_depth = d.min_water_depth;
    out.footprint_x = d.footprint_x;
    out.footprint_z = d.footprint_z;
    out.has_build_options = d.has_build_options;
    out.build_options.clear();
    const std::int32_t n = d.build_options_count < 0 ? 0 : (d.build_options_count > 30 ? 30 : d.build_options_count);
    for (std::int32_t k = 0; k < n; ++k) out.build_options.push_back(d.build_options[static_cast<std::size_t>(k)]);
    out.side = d.side;
    out.weapon_menu_name = d.unit_name.find("MAKENUKE") != std::string::npos || d.unit_name.find("MAKEANTI") != std::string::npos;
}

std::string ai_profile_path(const std::vector<std::uint8_t>& ota, int schema) {
    std::string stem;
    const TdfParseResult parsed = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
    if (parsed.root)
        if (const TdfSection* header = parsed.root->child("GlobalHeader"))
            if (const TdfSection* s = header->child("Schema " + std::to_string(schema))) stem = s->get_string("aiprofile", 0x100, "");
    if (stem.empty()) stem = "default";
    // The stem's own extension is dropped.
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && stem.find_first_of("\\/", dot) == std::string::npos) stem.erase(dot);
    return "ai\\" + stem + ".txt";
}

void run_ai_commands(const std::string& text, Vfs& vfs, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& report) {
    run_text(text, vfs, defs, ai, report, 1);
}

bool load_ai_profile(Vfs& vfs, const std::string& path, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& report) {
    std::vector<std::uint8_t> file;
    bool found = vfs.read(path, file) || vfs.read("ai\\default.txt", file);
    if (found) run_text(std::string(file.begin(), file.end()), vfs, defs, ai, report, 0);
    // The downloadable units' own lines, per active computer player: once for types whose
    // weight a unit-name line has not fixed, then again (the same weight text) for types
    // whose limit is not fixed.
    for (int s = 0; s < 10; ++s) {
        const sim::PlayerSlot& p = ai.players[static_cast<std::size_t>(s)];
        if (!p.active || p.controller != 2) continue;
        for (int pass = 0; pass < 2; ++pass) {
            sim::ai_profile_force(ai);
            for (std::size_t i = 1; i < defs.units.size(); ++i) {
                const UnitDefRecord& d = defs.units[i];
                if (!(d.type_flags & 0x20u)) continue;
                const sim::AiTable& tb = ai.state.tables[static_cast<std::size_t>(s)];
                const std::vector<std::int32_t>& fixed = pass == 0 ? tb.weight_fixed : tb.limit_fixed;
                if (i < fixed.size() && fixed[i] == 1) continue;
                if (d.ai_weight.empty()) continue;
                run_text(d.ai_weight, vfs, defs, ai, report, 0);
            }
        }
    }
    return found;
}

}  // namespace ta
