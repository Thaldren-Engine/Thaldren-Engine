#include "ta/translate.h"

#include <map>
#include <vector>

#include "ta/hpi.h"
#include "ta/tdf.h"

namespace ta {

namespace {

struct Table {
    std::string language;  // the language loaded (0x51FDC0)
    bool loaded = false;
    std::map<std::string, std::string> entries;  // byte order, as the original's sorted list
};

Table& table() {
    static Table t;
    return t;
}

}  // namespace

void load_translations(const std::string& tdf_text, const std::string& language) {
    Table& t = table();
    if (t.loaded && stricmp_c(language, t.language) == 0) return;
    t.loaded = true;
    t.language = language;
    t.entries.clear();
    const TdfParseResult r = tdf_parse(tdf_text);
    if (!r.root) return;
    for (const auto& sec : r.root->children()) {
        // Section names and translations read into 255-byte buffers.
        std::string base = sec->name().substr(0, 254);
        std::string text = sec->get_string(language, 0xFF, "");
        if (text.empty()) continue;
        t.entries[base] = std::move(text);
    }
}

void load_translations(Vfs& vfs, const std::string& language) {
    std::vector<std::uint8_t> f;
    std::string text;
    if (vfs.read("gamedata\\translate.tdf", f)) text.assign(f.begin(), f.end());
    load_translations(text, language);
}

const std::string& translate(const std::string& text) {
    const Table& t = table();
    const auto it = t.entries.find(text);
    return it == t.entries.end() ? text : it->second;
}

}  // namespace ta
