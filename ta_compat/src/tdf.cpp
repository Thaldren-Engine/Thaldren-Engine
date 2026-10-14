#include "ta/tdf.h"

#include <cstring>

#include "sim/x87.h"
#include "ta/crt_strtod.h"

namespace ta {
namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// C-locale isspace: space, \t, \n, \v, \f, \r.
bool is_c_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::uint8_t fold(std::uint8_t c) { return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c + 0x20) : c; }

// Trim space/tab/CR/LF from both ends of [b, e).
std::string trim_copy(const char* b, const char* e) {
    while (b < e && *b && is_ws(*b)) ++b;
    if (e <= b) return {};
    while (e > b && is_ws(e[-1])) --e;
    return std::string(b, e);
}

} // namespace

int stricmp_c(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    for (;; ++i) {
        const std::uint8_t ca = i < a.size() ? static_cast<std::uint8_t>(a[i]) : 0;
        const std::uint8_t cb = i < b.size() ? static_cast<std::uint8_t>(b[i]) : 0;
        if (ca != cb) {
            const std::uint8_t la = fold(ca), lb = fold(cb);
            if (la != lb) return la < lb ? -1 : 1;
        }
        if (ca == 0) return 0;
    }
}

std::int32_t msvc_atoi(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && is_c_space(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    std::uint32_t v = 0;
    while (i < s.size() && is_digit(s[i])) v = v * 10u + static_cast<std::uint32_t>(s[i++] - '0');
    if (neg) v = 0u - v;
    return static_cast<std::int32_t>(v);
}

double msvc_atof(std::string_view s) { return crt_atof(s); }

std::uint32_t simple_checksum(const std::uint8_t* data, std::int32_t len) {
    std::uint8_t sum = 0, x = 0, sum_ix = 0, x_ix = 0;
    for (std::int32_t i = 0; i < len; ++i) {
        const std::uint8_t b = data[i];
        const auto ib = static_cast<std::uint8_t>(i);
        sum = static_cast<std::uint8_t>(sum + b);
        x ^= b;
        sum_ix = static_cast<std::uint8_t>(sum_ix + (ib ^ b));
        x_ix ^= static_cast<std::uint8_t>(ib + b);
    }
    return static_cast<std::uint32_t>(sum) | (static_cast<std::uint32_t>(x) << 8) |
           (static_cast<std::uint32_t>(sum_ix) << 16) | (static_cast<std::uint32_t>(x_ix) << 24);
}

void tdf_blank_comments(std::string& t) {
    std::size_t p = 0;
    const std::size_t n = t.size();
    while (p < n && t[p] != '\0') {
        if (t[p] == '/' && p + 1 < n && t[p + 1] == '/') {
            // Blank up to (not including) the newline.
            while (p < n && t[p] != '\0' && t[p] != '\n') t[p++] = ' ';
            continue;
        }
        if (t[p] == '/' && p + 1 < n && t[p + 1] == '*') {
            t[p] = ' ';
            t[p + 1] = ' ';
            p += 2;
            while (p < n && t[p] != '\0') {
                if (t[p - 1] == '*' && t[p] == '/') {
                    t[p] = ' ';
                    t[p - 1] = ' ';
                    break;
                }
                t[p - 1] = ' ';
                ++p;
            }
            if (p < n && t[p] != '\0') ++p;  // step past the closing '/'
            continue;
        }
        ++p;
    }
}

struct TdfParser {
    const char* text;  // NUL-terminated
    std::string error;

    // Parses section contents starting at `p`. On '}' sets *end past it (child
    // sections); the root must end at NUL.
    std::unique_ptr<TdfSection> parse(std::string name, const char* p, const char** end) {
        auto sec = std::make_unique<TdfSection>();
        sec->name_ = std::move(name);
        const char* start = p;
        auto set_checksum = [&](const char* stop) {
            // The game sums [start, stop - 1) minus one more byte: length = stop - start - 2.
            const std::ptrdiff_t len = (stop - 1) - start - 1;
            sec->checksum_ = (stop - 1 >= start) ? simple_checksum(reinterpret_cast<const std::uint8_t*>(start),
                                                                   static_cast<std::int32_t>(len))
                                                 : 0;
        };
        for (;;) {
            while (*p && is_ws(*p)) ++p;
            if (*p == '\0') {
                if (end) {
                    error = "End of file, nextblock not zero";
                    return sec;
                }
                set_checksum(p);
                return sec;
            }
            if (*p == '[') {
                const char* close = std::strchr(p, ']');
                if (!close) {
                    error = "Sub-record closing ']' not found";
                    return sec;
                }
                std::string child_name = trim_copy(p + 1, close);
                p = close + 1;
                while (*p && is_ws(*p)) ++p;
                if (*p != '{') {
                    error = "Sub-record opening '{' not found";
                    return sec;
                }
                const char* after = nullptr;
                auto child = parse(std::move(child_name), p + 1, &after);
                sec->children_.push_back(std::move(child));
                if (!error.empty()) return sec;
                p = after;
                continue;
            }
            if (*p == '}') {
                if (end) *end = p + 1;
                set_checksum(p);
                return sec;
            }
            const char* eq = std::strchr(p, '=');
            if (!eq) {
                error = "Data field '=' not found";
                return sec;
            }
            std::string key = trim_copy(p, eq);
            p = eq + 1;
            const char* semi = std::strchr(p, ';');
            if (!semi) {
                error = "Data field ';' not found";
                return sec;
            }
            std::string value = trim_copy(p, semi);
            p = semi + 1;
            // lower_bound by case-insensitive compare; replace only on exact match.
            auto& keys = sec->keys_;
            std::size_t lo = 0, hi = keys.size();
            while (lo != hi) {
                const std::size_t mid = lo + (hi - lo) / 2;
                if (stricmp_c(keys[mid].key, key) < 0) lo = mid + 1;
                else hi = mid;
            }
            if (lo != keys.size() && keys[lo].key == key) {
                keys[lo].value = std::move(value);
            } else {
                keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(lo), TdfKey{std::move(key), std::move(value)});
            }
        }
    }
};

TdfParseResult tdf_parse(std::string_view bytes) {
    std::string text(bytes);
    // The game copies the file into a NUL-terminated buffer; an embedded NUL ends it.
    tdf_blank_comments(text);
    TdfParser parser{text.c_str(), {}};
    TdfParseResult r;
    r.root = parser.parse("root", text.c_str(), nullptr);
    r.error = std::move(parser.error);
    return r;
}

const TdfSection* TdfSection::child(std::string_view name) const {
    for (const auto& c : children_) {
        if (stricmp_c(c->name_, name) == 0) return c.get();
    }
    return nullptr;
}

const std::string* TdfSection::raw(std::string_view key) const {
    std::size_t lo = 0, hi = keys_.size();
    while (lo != hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (stricmp_c(keys_[mid].key, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == keys_.size() || stricmp_c(key, keys_[lo].key) < 0) return nullptr;
    return &keys_[lo].value;
}

std::int32_t TdfSection::get_int(std::string_view key, std::int32_t def) const {
    const std::string* v = raw(key);
    return v ? msvc_atoi(*v) : def;
}

double TdfSection::get_float(std::string_view key, double def) const {
    const std::string* v = raw(key);
    return v ? msvc_atof(*v) : def;
}

std::int32_t TdfSection::get_fixed(std::string_view key, std::int32_t def) const {
    const std::string* v = raw(key);
    return v ? sim::x87::ftol32(msvc_atof(*v) * 65536.0) : def;
}

std::string TdfSection::get_string(std::string_view key, std::size_t buf_size, std::string_view def, bool* found) const {
    const std::string* v = raw(key);
    if (found) *found = v != nullptr;
    if (v) {
        std::string s = *v;
        const std::size_t nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        if (buf_size > 0 && s.size() > buf_size - 1) s.resize(buf_size - 1);
        return s;
    }
    return std::string(def);
}

} // namespace ta
