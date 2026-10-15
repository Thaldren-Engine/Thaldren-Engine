// Persistent settings as a "key = value" text file (one per line, '#' comments).

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "client/front_end.h"

namespace client {

int Settings::get_int(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    return std::atoi(it->second.c_str());
}

std::string Settings::get_text(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Settings::set_int(const std::string& key, int value) { values_[key] = std::to_string(value); }
void Settings::set_text(const std::string& key, const std::string& value) { values_[key] = value; }

bool Settings::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t");
            const auto b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        values_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "# settings\n";
    for (const auto& [k, v] : values_) out << k << " = " << v << "\n";
    return static_cast<bool>(out);
}

}  // namespace client
