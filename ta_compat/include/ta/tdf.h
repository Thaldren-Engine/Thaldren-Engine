#pragma once
// TA's text definition format (.tdf, .fbi, .ota, .gui, ...), parsed with the
// same rules as the game (see docs/RE_MEMORY.md, "TDF text format").

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ta {

// String helpers with the original runtime's semantics.
int stricmp_c(std::string_view a, std::string_view b);  // ASCII-only case fold, unsigned byte order
std::int32_t msvc_atoi(std::string_view s);            // wraps on overflow
double msvc_atof(std::string_view s);                  // bit-exact original runtime atof (see crt_strtod.h)

struct TdfKey {
    std::string key;
    std::string value;
};

class TdfSection {
public:
    const std::string& name() const { return name_; }
    const std::vector<TdfKey>& keys() const { return keys_; }
    const std::vector<std::unique_ptr<TdfSection>>& children() const { return children_; }
    std::uint32_t checksum() const { return checksum_; }

    // First child whose name matches case-insensitively.
    const TdfSection* child(std::string_view name) const;

    // Raw value lookup (binary search over the case-insensitively sorted keys).
    const std::string* raw(std::string_view key) const;

    std::int32_t get_int(std::string_view key, std::int32_t def) const;
    double get_float(std::string_view key, double def) const;
    std::int32_t get_fixed(std::string_view key, std::int32_t def) const;  // 16.16
    // Copies at most buf_size-1 characters, as the original strncpy-based getter does.
    std::string get_string(std::string_view key, std::size_t buf_size, std::string_view def, bool* found = nullptr) const;

private:
    friend struct TdfParser;
    std::string name_;
    std::vector<TdfKey> keys_;
    std::vector<std::unique_ptr<TdfSection>> children_;
    std::uint32_t checksum_ = 0;
};

struct TdfParseResult {
    std::unique_ptr<TdfSection> root;  // named "root"
    std::string error;                 // empty on success; the game aborts on any error
};

// Blank out // line comments and /* */ block comments in place (replaced with spaces).
void tdf_blank_comments(std::string& text);

TdfParseResult tdf_parse(std::string_view bytes);

// 32-bit content mix used for TDF sections and other sync checksums.
std::uint32_t simple_checksum(const std::uint8_t* data, std::int32_t len);

} // namespace ta
