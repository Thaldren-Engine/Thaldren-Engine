#include "ta/featuredefs.h"

#include <algorithm>
#include <cstring>

#include "sim/x87.h"

namespace ta {
namespace {

std::string upto(const std::string& s, std::size_t n) { return s.size() > n ? s.substr(0, n) : s; }

// base + "\" + name with the extension (after the last '.') replaced.
std::string virtual_path(const std::string& base, const std::string& name, const std::string& ext) {
    std::string p = base + "\\" + name;
    const auto dot = p.rfind('.');
    if (dot != std::string::npos) p.resize(dot);
    return p + "." + ext;
}

// strncpy(dst, src, 16) followed by strncmp(dst, other, 16) == 0.
bool same_16(const std::string& stored, const std::string& other) { return stored == upto(other, 16); }

}  // namespace

void FeatureDefTable::load_library(Vfs& vfs, const std::array<std::string, 256>& weapon_names) {
    vfs_ = &vfs;
    weapon_names_ = &weapon_names;
    for (const auto& path : vfs.enumerate_tree("features", "*.tdf")) {
        std::vector<std::uint8_t> data;
        // An empty or unreadable file is dropped; parse errors are fatal in the game.
        if (!vfs.read(path, data) || data.empty()) continue;
        auto r = tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        if (!r.root) {
            notes_.push_back("parse error (fatal in game): " + path + ": " + r.error);
            continue;
        }
        files_.push_back(path);
        roots_.push_back(std::move(r.root));
    }
}

const TdfSection* FeatureDefTable::find_section(const std::string& name) const {
    for (const auto& root : roots_)
        if (const TdfSection* s = root->child(name)) return s;
    return nullptr;
}

int FeatureDefTable::find(const std::string& name) const {
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (stricmp_c(name, records_[i].name) == 0) return static_cast<int>(i);
    return -1;
}

int FeatureDefTable::find_or_load(const std::string& name) {
    const int i = find(name);
    return i >= 0 ? i : load(name);
}

int FeatureDefTable::load(const std::string& name) {
    const int index = static_cast<int>(records_.size());
    FeatureDefRecord f;
    f.name = upto(name, 0x80);
    gafs_.emplace_back();
    const TdfSection* s = find_section(name);
    if (!s) {
        f.section_found = false;
        notes_.push_back("Record " + name + " missing from feature files (the game crashes)");
        records_.push_back(std::move(f));
        return index;
    }
    f.description = s->get_string("Description", 0x14, "");
    f.footprint_x = static_cast<std::int16_t>(s->get_int("footprintx", 0));
    f.footprint_z = static_cast<std::int16_t>(s->get_int("footprintz", 0));
    f.height = static_cast<std::uint8_t>(s->get_int("height", 0));

    bool is_3d = false;
    const std::string object = s->get_string("object", 0x100, "", &is_3d);
    if (is_3d) {
        f.object = object;
        std::vector<std::uint8_t> data;
        if (!vfs_->read(virtual_path("objects3d", object, "3DO"), data) || data.empty())
            notes_.push_back("missing model (fatal in game): objects3d\\" + object);
        // The GAF fields stay uninitialised for 3D features.
    } else {
        f.flags |= feature_flags::kGaf;
        const std::string file = s->get_string("filename", 0x100, "");
        // An earlier 2D feature that loaded the same file (first 16 characters, case-sensitive)
        // lends its GAF; this one stores "REUSE".
        const GafDirectory* gaf = nullptr;
        int lender = -1;
        for (int i = 0; i < index; ++i) {
            const auto& r = records_[static_cast<std::size_t>(i)];
            if ((r.flags & feature_flags::kGaf) && same_16(r.gaf_file, file)) {
                lender = i;
                break;
            }
        }
        if (lender >= 0) {
            f.gaf_file = "REUSE";
            gaf = gafs_[static_cast<std::size_t>(lender)].get();
            f.gaf_owner = gaf ? lender : -1;
        } else {
            f.gaf_file = upto(file, 16);
            std::vector<std::uint8_t> data;
            auto dir = std::make_unique<GafDirectory>();
            if (vfs_->read(virtual_path("anims", file, "GAF"), data) && !data.empty() && gaf_read_directory(data, *dir)) {
                gafs_.back() = std::move(dir);
                gaf = gafs_.back().get();
                f.gaf_owner = index;
            } else {
                notes_.push_back("GAF not loaded: anims\\" + file);
            }
        }
        static const char* const kKeys[kSeqCount] = {"seqname",    "seqnameshad",    "seqnameburn",      "seqnameburnshad",
                                                      "seqnamedie", "seqnamedieshad", "seqnamereclamate", "seqnamereclamateshad"};
        for (int k = 0; k < kSeqCount; ++k) {
            const std::string seq = s->get_string(kKeys[k], 0x100, "");
            if (seq.empty() || !gaf) continue;
            const int at = gaf_find_sequence(*gaf, seq);
            if (at >= 0) f.sequences[static_cast<std::size_t>(k)] = gaf->sequence_names[static_cast<std::size_t>(at)];
        }
    }

    f.spread_chance = static_cast<std::uint8_t>(s->get_int("spreadchance", 0));
    f.reproduce = static_cast<std::uint8_t>(s->get_int("reproduce", 0));
    f.reproduce_area = static_cast<std::uint8_t>(s->get_int("reproducearea", 0));
    f.metal = static_cast<float>(static_cast<std::uint32_t>(s->get_int("metal", 0)) & 0xFFFFu);
    f.energy = static_cast<float>(static_cast<std::uint32_t>(s->get_int("energy", 0)) & 0xFFFFu);
    f.damage = static_cast<std::int16_t>(s->get_int("damage", 0));
    auto bit = [&](const char* key, std::int32_t def, std::uint16_t mask) {
        if (s->get_int(key, def) & 1) f.flags |= mask;
    };
    bit("animating", 0, feature_flags::kAnimating);
    bit("animtrans", 0, feature_flags::kAnimTrans);
    bit("shadtrans", 0, feature_flags::kShadTrans);
    bit("flamable", 0, feature_flags::kFlamable);
    bit("geothermal", 0, feature_flags::kGeothermal);
    bit("blocking", 0, feature_flags::kBlocking);
    bit("reclaimable", 0, feature_flags::kReclaimable);
    bit("autoreclaimable", 1, feature_flags::kAutoReclaimable);
    bit("indestructible", 0, feature_flags::kIndestructible);
    bit("nodisplayinfo", 0, feature_flags::kNoDisplayInfo);
    bit("nodrawundergray", 0, feature_flags::kNoDrawUnderGray);
    // Hard-coded by the feature's requested name.
    for (const char* n : {"DragonsTeeth", "DragonsTeeth_Core", "Fortification", "Fortification_Core"})
        if (stricmp_c(name, n) == 0) f.flags |= feature_flags::kNoDrawUnderGray;

    // Seconds to ticks: x87 product with 30.0, truncated, low 16 bits kept.
    f.spark_time = static_cast<std::int16_t>(sim::x87::ftol64(s->get_float("sparktime", 0.0) * 30.0));

    const std::string weapon = s->get_string("burnweapon", 0x100, "");
    if (!weapon.empty() && weapon_names_) {
        for (int i = 0; i < 256; ++i) {
            if (stricmp_c((*weapon_names_)[static_cast<std::size_t>(i)], weapon) == 0) {
                f.burn_weapon = i;
                break;
            }
        }
    }
    records_.push_back(std::move(f));
    return index;
}

void FeatureDefTable::load_map_features(const std::vector<std::string>& tnt_names) {
    for (const auto& n : tnt_names) load(n);
}

void FeatureDefTable::load_schema_features(const TdfSection& ota_root, const std::string& schema) {
    const TdfSection* gh = ota_root.child("globalheader");
    const TdfSection* sc = gh ? gh->child(schema) : nullptr;
    const TdfSection* list = sc ? sc->child("features") : nullptr;
    if (!list) return;
    for (const auto& e : list->children()) {
        const std::string name = e->get_string("Featurename", 0x80, "");
        if (e->get_int("XPos", -1) < 0 || e->get_int("ZPos", -1) < 0 || name.empty()) continue;
        find_or_load(name);
    }
}

void FeatureDefTable::resolve_chains() {
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const TdfSection* s = find_section(records_[i].name);
        if (!s) continue;  // the game crashes here
        auto link = [&](const char* key) -> std::int16_t {
            bool found = false;
            const std::string target = s->get_string(key, 0x100, "", &found);
            if (!found) return -1;
            // A miss loads the target, which is then appended (and resolved later in this loop).
            return static_cast<std::int16_t>(find_or_load(target));
        };
        // Resolution order matters: each miss appends a new entry.
        const std::int16_t dead = link("featuredead");
        records_[i].dead = dead;
        const std::int16_t reclamate = link("featurereclamate");
        records_[i].reclamate = reclamate;
        const std::int16_t burnt = link("featureburnt");
        records_[i].burnt = burnt;
    }
}

bool tnt_feature_names(const std::vector<std::uint8_t>& tnt, std::vector<std::string>& out) {
    out.clear();
    if (tnt.size() < 0x24) return false;
    std::uint32_t h[9];
    std::memcpy(h, tnt.data(), sizeof h);
    if (h[0] != 0x1020 && h[0] != 0x2000) return false;
    const std::uint32_t count = h[7], offset = h[8];
    // Entry: u32 index, char name[128].
    if (static_cast<std::size_t>(offset) + static_cast<std::size_t>(count) * 0x84 > tnt.size()) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* p = reinterpret_cast<const char*>(&tnt[offset + i * 0x84u + 4]);
        out.emplace_back(p, std::find(p, p + 128, char(0)));
    }
    return true;
}

} // namespace ta
