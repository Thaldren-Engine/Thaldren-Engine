// totala.ini [Preferences] (0x49F5A0 → GetPrivateProfileIntA) and the start-up unit limit
// (0x491200). The profile rules are those of the system call the original makes (measured on
// the current Windows: docs/RE_MEMORY.md, "Preferences file").

#include "ta/preferences.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace ta {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k)
        if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k]))) return false;
    return true;
}

std::int32_t parse_value(std::string v) {
    // Matching quotes around the value go ("\"77\"", "'9'"); an unmatched one stays (and
    // stops the number).
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    std::size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
    bool negative = false;
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) negative = v[i++] == '-';
    std::uint32_t n = 0;
    if (i + 1 < v.size() && v[i] == '0' && v[i + 1] == 'x') {  // lower-case x only
        for (i += 2; i < v.size() && std::isxdigit(static_cast<unsigned char>(v[i])); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[i])));
            n = n * 16u + static_cast<std::uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        }
    } else {
        for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) n = n * 10u + static_cast<std::uint32_t>(v[i] - '0');
    }
    if (negative) n = 0u - n;
    return static_cast<std::int32_t>(n);
}

}  // namespace

std::int32_t profile_int(const std::string& text, const std::string& section, const std::string& key, std::int32_t fallback) {
    std::istringstream in(text);
    std::string line;
    enum { Before, Inside, After } state = Before;
    while (state != After && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            const std::size_t close = t.find(']');
            if (close == std::string::npos) continue;
            if (state == Inside) state = After;  // only the first section of the name is read
            else if (same_name(trim(t.substr(1, close - 1)), section)) state = Inside;
            continue;
        }
        if (state != Inside || t.empty() || t[0] == ';') continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos || !same_name(trim(t.substr(0, eq)), key)) continue;
        const std::string value = trim(t.substr(eq + 1));
        return value.empty() ? fallback : parse_value(value);
    }
    return fallback;
}

std::int32_t preferences_int(const std::string& game_dir, const std::string& key, std::int32_t fallback) {
    std::ifstream f(game_dir + "/totala.ini", std::ios::binary);
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    return profile_int(ss.str(), "Preferences", key, fallback);
}

int preferences_unit_limit(const std::string& game_dir) {
    const std::int32_t v = preferences_int(game_dir, "UnitLimit", 250);
    // (0x491653: signed compares against 500, then 20)
    if (v > 500) return 500;
    return v < 20 ? 20 : v;
}

}  // namespace ta
