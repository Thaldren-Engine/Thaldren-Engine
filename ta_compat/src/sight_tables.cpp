#include "ta/sight_tables.h"

#include <cstring>

#include "ta/gaf.h"
#include "ta/tdf.h"

namespace ta {
namespace {

// strtok with the delimiters ", ".
struct Tokens {
    std::string text;
    std::size_t at = 0;
    bool next(std::string& out) {
        auto delim = [](char c) { return c == ',' || c == ' '; };
        while (at < text.size() && delim(text[at])) ++at;
        if (at >= text.size()) return false;
        const std::size_t start = at;
        while (at < text.size() && !delim(text[at])) ++at;
        out = text.substr(start, at - start);
        if (at < text.size()) ++at;
        return true;
    }
};

template <class T> bool rd(const std::vector<std::uint8_t>& b, std::size_t at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, &b[at], sizeof(T));
    return true;
}

} // namespace

bool parse_sight_rings(std::string_view tdf_text, std::vector<sim::SightRing>& out, std::string& error) {
    out.clear();
    TdfParseResult parsed = tdf_parse(tdf_text);
    if (!parsed.root) {
        error = "los.tdf: " + parsed.error;
        return false;
    }
    const TdfSection* info = parsed.root->child("TABLEINFO");
    if (!info) return true;  // the table stays as it was (empty)
    const std::int16_t tables = static_cast<std::int16_t>(info->get_int("numtables", 0));
    out.resize(tables > 0 ? static_cast<std::size_t>(tables) : 0);
    for (std::int16_t t = 0; t < tables; ++t) {
        const TdfSection* sec = parsed.root->child("TABLE" + std::to_string(t + 1));
        if (!sec) continue;
        const std::int16_t lines = static_cast<std::int16_t>(sec->get_int("numlines", 0));
        if (lines < 0) {
            error = "los.tdf: negative numlines";
            return false;
        }
        sim::SightRing& ring = out[static_cast<std::size_t>(t)];
        ring.assign(static_cast<std::size_t>(lines) * 4, {});
        for (std::int16_t i = 0; i < lines; ++i) {
            bool found = false;
            const std::string value = sec->get_string("line" + std::to_string(i + 1), 0x200, "", &found);
            if (!found) continue;  // its four slices stay empty
            Tokens tok{value};
            std::string s;
            if (!tok.next(s)) continue;
            const std::int16_t count = static_cast<std::int16_t>(msvc_atoi(s));
            if (count < 0) {
                error = "los.tdf: negative point count";
                return false;
            }
            for (int q = 0; q < 4; ++q) ring[static_cast<std::size_t>(q * lines + i)].assign(static_cast<std::size_t>(count), {});
            for (std::int16_t k = 0; k < count; ++k) {
                std::string a, b;
                if (!tok.next(a) || !tok.next(b)) {
                    error = "los.tdf: line ends early (the original reads a null token)";
                    return false;
                }
                const auto av = static_cast<std::int16_t>(msvc_atoi(a));
                const auto bv = static_cast<std::int16_t>(msvc_atoi(b));
                const auto neg = [](std::int16_t v) { return static_cast<std::int16_t>(-v); };
                ring[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] = {av, neg(bv)};
                ring[static_cast<std::size_t>(lines + i)][static_cast<std::size_t>(k)] = {bv, av};
                ring[static_cast<std::size_t>(2 * lines + i)][static_cast<std::size_t>(k)] = {neg(av), bv};
                ring[static_cast<std::size_t>(3 * lines + i)][static_cast<std::size_t>(k)] = {neg(bv), neg(av)};
            }
        }
    }
    return true;
}

bool sight_masks_from_gaf(const std::vector<std::uint8_t>& gaf, std::string_view sequence, std::vector<sim::SightMask>& out,
                          std::string& error) {
    out.clear();
    GafDirectory dir;
    if (!gaf_read_directory(gaf, dir)) {
        error = "sight masks: bad GAF";
        return false;
    }
    const int seq = gaf_find_sequence(dir, std::string(sequence));
    if (seq < 0) {
        error = "sight masks: no sequence " + std::string(sequence);
        return false;
    }
    std::uint32_t entry = 0;
    std::uint16_t frames = 0;
    if (!rd(gaf, 12 + static_cast<std::size_t>(seq) * 4, entry) || !rd(gaf, entry, frames)) {
        error = "sight masks: truncated GAF";
        return false;
    }
    for (std::uint16_t i = 0; i < frames; ++i) {
        std::uint32_t fp = 0, data = 0;
        std::uint16_t w = 0, h = 0;
        std::uint8_t layers = 0;
        std::int16_t xo = 0, yo = 0;
        std::uint8_t key = 0;
        if (!rd(gaf, entry + 40 + i * 8u, fp) || !rd(gaf, fp, w) || !rd(gaf, fp + 2, h) || !rd(gaf, fp + 4, xo) || !rd(gaf, fp + 6, yo) ||
            !rd(gaf, fp + 8, key) || !rd(gaf, fp + 10, layers) || !rd(gaf, fp + 16, data)) {
            error = "sight masks: truncated frame";
            return false;
        }
        if (layers != 0) {
            error = "sight masks: layered frame (the original would read its layer table as pixels)";
            return false;
        }
        const std::size_t n = static_cast<std::size_t>(w) * h;
        if (data + n > gaf.size()) {
            error = "sight masks: frame data past the file";
            return false;
        }
        sim::SightMask m;
        m.width = w;
        m.height = h;
        m.x_offset = xo;
        m.y_offset = yo;
        m.covered.resize(n);
        for (std::size_t k = 0; k < n; ++k) m.covered[k] = gaf[data + k] != key ? 1 : 0;
        out.push_back(std::move(m));
    }
    return true;
}

bool load_sight_tables(Vfs& vfs, sim::SightTables& out, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!vfs.read("gamedata\\los.tdf", bytes)) {
        error = "gamedata\\los.tdf missing";
        return false;
    }
    if (!parse_sight_rings(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), out.rings, error)) return false;
    if (!vfs.read("anims\\vismasks.gaf", bytes)) {
        error = "anims\\vismasks.gaf missing";
        return false;
    }
    return sight_masks_from_gaf(bytes, "vismask", out.masks, error);
}

} // namespace ta
