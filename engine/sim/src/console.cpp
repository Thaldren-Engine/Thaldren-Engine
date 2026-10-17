// Typed commands: words, the registry, and the simulation parts commands share
// (docs/RE_MEMORY.md, "Console commands").

#include "sim/console.h"

#include <algorithm>

#include "sim/rng.h"
#include "sim/sky.h"

namespace sim {
namespace console {
namespace {

bool c_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

// r * range / 0x8000 with a signed 64-bit product, truncating toward zero.
std::int32_t scaled(std::int64_t r, std::int64_t range) { return static_cast<std::int32_t>(r * range / 0x8000); }

}  // namespace

std::string Words::text(int i, const std::string& fallback) const {
    return i >= 0 && i < count() ? words[static_cast<std::size_t>(i)] : fallback;
}

std::int32_t Words::number(int i, std::int32_t fallback) const {
    return i >= 0 && i < count() ? c_atol(words[static_cast<std::size_t>(i)]) : fallback;
}

Words split(std::string_view line) {
    Words t;
    std::size_t at = 0, used = 0;
    const std::size_t n = line.size();
    for (;;) {
        while (at < n && c_space(line[at])) ++at;
        if (at == n || line[at] == '#') return t;
        std::string word;
        while (at < n && !c_space(line[at]) && line[at] != '#') {
            if (used >= 0x7E) {
                t.endless = true;
                return t;
            }
            word.push_back(line[at]);
            ++at;
            ++used;
        }
        ++used;  // the terminator
        if (t.words.size() < 20) t.words.push_back(word);
    }
}

void substitute(Words& words, const Words& parent) {
    for (std::string& w : words.words) {
        if (w.empty() || w[0] != '%') continue;
        const std::int32_t k = c_atol(std::string_view(w).substr(1));
        if (k >= 0 && k < parent.count()) w = parent.words[static_cast<std::size_t>(k)];
    }
}

std::int32_t c_atol(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && c_space(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    std::uint32_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10u + static_cast<std::uint32_t>(s[i++] - '0');
    return static_cast<std::int32_t>(neg ? 0u - v : v);
}

int c_stricmp(std::string_view a, std::string_view b) {
    for (std::size_t i = 0;; ++i) {
        const unsigned char ca = static_cast<unsigned char>(i < a.size() ? lower(a[i]) : '\0');
        const unsigned char cb = static_cast<unsigned char>(i < b.size() ? lower(b[i]) : '\0');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
}

bool wildcard_match(std::string_view name, std::string_view pattern) {
    auto pat = [&](int i) { return static_cast<std::size_t>(i) < pattern.size() ? upper(pattern[static_cast<std::size_t>(i)]) : '\0'; };
    std::vector<int> states{0};
    for (char raw : name) {
        const char c = upper(raw);
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

void Registry::add(const std::string& name, Handler handler, std::uint32_t bits) {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), name,
                               [](const Entry& e, const std::string& n) { return c_stricmp(e.name, n) < 0; });
    if (it != entries_.end() && c_stricmp(it->name, name) == 0) {
        it->handler = std::move(handler);
        it->bits = bits;
        return;
    }
    entries_.insert(it, Entry{name, std::move(handler), bits});
}

void Registry::set_default(Handler handler, std::uint32_t bits) {
    default_ = std::move(handler);
    default_bits_ = bits;
}

std::uint32_t Registry::dispatch(const Words& words, std::uint32_t allowed) const {
    if (words.count() < 1) return 0;
    const std::string& first = words.words[0];
    auto it = std::lower_bound(entries_.begin(), entries_.end(), first,
                               [](const Entry& e, const std::string& n) { return c_stricmp(e.name, n) < 0; });
    const Entry* found = it != entries_.end() && c_stricmp(first, it->name) >= 0 ? &*it : nullptr;
    if (found && (found->bits & allowed) != 0) {
        if (found->handler) found->handler(words);
        return found->bits;
    }
    if (default_ && (default_bits_ & allowed) != 0) {
        default_(words);
        return default_bits_;
    }
    return 0;
}

std::uint32_t Registry::run_text(std::string_view text, const Words& parent, std::uint32_t allowed) const {
    std::uint32_t ran = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        Words w = split(text.substr(at, end - at));
        if (w.endless) return ran;  // the original never finishes such a line
        substitute(w, parent);
        ran |= dispatch(w, allowed);
        at = end + 1;
    }
    return ran;
}

int place_unit_row(std::array<std::int32_t, 3>& pos, std::int32_t row_end_x, int type_count, const UnitRowHooks& hooks) {
    int matched = 0;
    auto add = [](std::int32_t a, std::int32_t b) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); };
    for (int k = 1; k < type_count; ++k) {
        const auto type = static_cast<std::uint16_t>(k);
        if (!hooks.matches(type)) continue;
        const std::array<std::int32_t, 2> ext = hooks.extents_x(type);
        if (matched != 0) pos[0] = add(pos[0], -ext[0]);
        hooks.snap(type, pos);
        hooks.create(type, pos);
        pos[0] = add(add(pos[0], 0x200000), ext[1]);
        if (row_end_x <= pos[0]) {
            pos[2] = add(pos[2], 0xA00000);
            pos[0] = 0xA00000;
        }
        ++matched;
    }
    return matched;
}

void start_meteor_shower(MeteorShower& m, std::uint32_t tick, std::int32_t map_width, std::int32_t map_height, Lcg15& rand) {
    m.active = true;
    m.end_tick = static_cast<std::uint32_t>(m.duration) + tick;
    m.next_schedule = static_cast<std::uint32_t>(m.schedule_gap) + m.end_tick;
    m.next_spawn = tick;
    m.origin_z = static_cast<std::int16_t>(scaled(rand.next(), map_height));
    m.origin_x = static_cast<std::int16_t>(scaled(rand.next(), map_width));
    const std::int32_t dz = scaled(rand.next(), 10) - 15;
    const std::int32_t dx = scaled(rand.next(), 30) - 15;
    m.aim_z = static_cast<std::int16_t>(dz + m.origin_z);
    m.aim_x = static_cast<std::int16_t>(dx + m.origin_x);
}

}  // namespace console
}  // namespace sim
