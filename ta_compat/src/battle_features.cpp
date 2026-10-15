#include "ta/battle_features.h"

#include <cstring>

#include "ta/gaf.h"
#include "ta/tdf.h"

namespace ta {
namespace {

template <class T> bool rd(const std::vector<std::uint8_t>& b, std::size_t at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, &b[at], sizeof(T));
    return true;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

bool gaf_sequence_timing(const std::vector<std::uint8_t>& gaf, int seq, sim::AnimSequence& out) {
    std::uint32_t entry = 0;
    if (seq < 0 || !rd(gaf, 12 + static_cast<std::size_t>(seq) * 4, entry)) return false;
    std::uint16_t frames = 0;
    std::uint8_t loop = 0;
    if (!rd(gaf, entry, frames) || !rd(gaf, entry + 2, loop)) return false;
    out.loop = loop;
    out.frames.clear();
    for (std::uint16_t i = 0; i < frames; ++i) {
        std::uint32_t fp = 0, duration = 0;
        if (!rd(gaf, entry + 40 + i * 8u, fp) || !rd(gaf, entry + 44 + i * 8u, duration)) return false;
        sim::AnimFrame f;
        f.duration = static_cast<std::uint16_t>(duration);
        rd(gaf, fp, f.width);
        rd(gaf, fp + 2, f.height);
        rd(gaf, fp + 4, f.x_offset);
        rd(gaf, fp + 6, f.y_offset);
        out.frames.push_back(f);
    }
    return true;
}

const std::vector<std::uint8_t>* AnimImporter::file(const std::string& name) {
    const std::string key = lower(name);
    auto it = files_.find(key);
    if (it == files_.end()) {
        std::vector<std::uint8_t> data;
        if (!vfs_.read("anims\\" + name + ".GAF", data)) data.clear();
        it = files_.emplace(key, std::move(data)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

std::uint32_t AnimImporter::sequence(const std::string& domain, const std::string& file_name, const std::string& seq, bool clear_loop) {
    if (seq.empty()) return 0;
    const std::vector<std::uint8_t>* data = file(file_name);
    if (!data) return 0;
    GafDirectory dir;
    if (!gaf_read_directory(*data, dir)) return 0;
    const int index = gaf_find_sequence(dir, seq);
    if (index < 0) return 0;
    const std::string key = domain + "|" + lower(file_name) + "|" + std::to_string(index);
    std::uint32_t address = 0;
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        address = it->second;
    } else {
        sim::AnimSequence s;
        if (!gaf_sequence_timing(*data, index, s)) return 0;
        address = lib_.add(std::move(s));
        loaded_[key] = address;
        origins_[address] = {lower(file_name), index};
    }
    if (clear_loop)
        if (sim::AnimSequence* s = lib_.find(address)) s->loop = 0;
    return address;
}

std::vector<sim::FeatureTypeInfo> to_feature_types(const FeatureDefTable& defs, const std::vector<sim::WeaponType>& weapons,
                                                   AnimImporter& anims, std::vector<std::string>* model_names) {
    const auto& recs = defs.records();
    std::vector<sim::FeatureTypeInfo> out(recs.size());
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const FeatureDefRecord& r = recs[i];
        sim::FeatureTypeInfo& t = out[i];
        t.footprint_x = r.footprint_x;
        t.footprint_z = r.footprint_z;
        t.height = r.height;
        t.flags = r.flags;
        t.damage = static_cast<std::uint16_t>(r.damage);
        t.spark_time = static_cast<std::uint16_t>(r.spark_time);
        t.spread_chance = r.spread_chance;
        t.reproduce = r.reproduce;
        t.reproduce_area = r.reproduce_area;
        t.burn_weapon = r.burn_weapon >= 0 && static_cast<std::size_t>(r.burn_weapon) < weapons.size() ? &weapons[static_cast<std::size_t>(r.burn_weapon)] : nullptr;
        t.dead = static_cast<std::uint16_t>(r.dead);
        t.burnt = static_cast<std::uint16_t>(r.burnt);
        t.reclamate = static_cast<std::uint16_t>(r.reclamate);
        t.metal = r.metal;
        t.energy = r.energy;
        if (r.flags & feature_flags::kGaf) {
            if (r.gaf_owner >= 0) {
                const std::string& file = recs[static_cast<std::size_t>(r.gaf_owner)].gaf_file;
                const std::string domain = "feature" + std::to_string(r.gaf_owner);
                for (int k = 0; k < kSeqCount; ++k)
                    t.sequences[static_cast<std::size_t>(k)] =
                        anims.sequence(domain, file, r.sequences[static_cast<std::size_t>(k)], k >= kSeqBurn);
            }
        } else if (model_names) {
            model_names->push_back(r.object);
            t.model = static_cast<std::uint32_t>(model_names->size());
        } else {
            t.model = 1;
        }
    }
    return out;
}

void bind_weapon_explosions(const WeaponDefTable& defs, std::vector<sim::WeaponType>& weapons, AnimImporter& anims) {
    for (std::size_t i = 0; i < defs.slots.size() && i < weapons.size(); ++i) {
        const WeaponDefRecord& d = defs.slots[i];
        if (!d.loaded) continue;
        weapons[i].fire_starter = d.fire_starter;
        weapons[i].explosion_seq = anims.sequence("shared", d.explosion_gaf, d.explosion_seq, true);
        weapons[i].water_explosion_seq = anims.sequence("shared", d.water_explosion_gaf, d.water_explosion_seq, true);
    }
}

void load_effect_art(AnimImporter& anims, sim::EffectsEnvironment& env) {
    // The generated glows: square noise frames shown 2 steps each, not looping.
    static const int kGlowFrames[3] = {12, 15, 15};
    for (int k = 0; k < 3; ++k) {
        sim::AnimSequence s;
        s.loop = 0;
        for (int f = 0; f < kGlowFrames[k]; ++f) s.frames.push_back({2, 0, 0, 0, 0});
        env.glow[static_cast<std::size_t>(k)] = anims.library().add(std::move(s));
    }
    static const char* const kBitmaps[6] = {"explosion", "explode2", "explode3", "explode4", "explode5", "nuke1"};
    for (int k = 0; k < 6; ++k) env.bitmap[static_cast<std::size_t>(k)] = anims.sequence("shared", "fx", kBitmaps[k], true);
    env.water_splash = anims.sequence("shared", "fx", "h2oboom2", true);
    env.lava_splash = anims.sequence("shared", "fx", "lavasplash", true);
}

void bind_feature_blocks(sim::Battle& b) {
    b.feature_blocks.resize(b.features.size());
    for (std::size_t k = 0; k < b.features.size(); ++k) b.feature_blocks[k] = (b.features[k].flags & 0x40u) ? 1 : 0;
}

void place_tnt_features(sim::Battle& b, const std::vector<std::uint8_t>& tnt, bool place) {
    std::uint32_t h[8] = {};
    for (int i = 0; i < 8; ++i)
        if (!rd(tnt, static_cast<std::size_t>(i) * 4, h[i])) return;
    if (h[0] != 0x2000) return;
    const std::size_t cells = static_cast<std::size_t>(h[1]) * h[2];
    if (cells != b.map.cells.size() || static_cast<std::size_t>(h[4]) + cells * 4 > tnt.size()) return;
    sim::FeatureWorld& w = b.feature_world();
    for (std::size_t i = 0; i < cells; ++i) {
        sim::MapCell& c = b.map.cells[i];
        c.flags = static_cast<std::uint8_t>((c.flags & 0xD7) | 0x50);  // owner nibble 10: nobody
        std::uint16_t f = 0;
        rd(tnt, h[4] + i * 4 + 1, f);
        if (f == sim::kFeatureVoid) sim::place_feature(w, static_cast<std::int32_t>(i), sim::kFeatureVoid, nullptr, nullptr, 10);
    }
    for (std::size_t i = 0; i < cells && place; ++i) {
        std::uint16_t f = 0;
        rd(tnt, h[4] + i * 4 + 1, f);
        if (f < h[7]) sim::place_feature(w, static_cast<std::int32_t>(i), f, nullptr, nullptr, 10);
    }
}

void bind_resurrect_types(std::vector<sim::FeatureTypeInfo>& features, const FeatureDefTable& defs,
                          const std::vector<std::string>& type_names) {
    const auto& recs = defs.records();
    auto lower = [](std::string v) {
        for (char& c : v)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return v;
    };
    for (std::size_t i = 0; i < recs.size() && i < features.size(); ++i) {
        std::string name = recs[i].name.substr(0, 63);
        const std::size_t cut = name.find('_');
        if (cut != std::string::npos) name.resize(cut);
        const std::string want = lower(name);
        features[i].resurrect_type = 0;
        for (std::size_t k = 1; k < type_names.size(); ++k)
            if (lower(type_names[k]) == want) {
                features[i].resurrect_type = static_cast<std::uint16_t>(k);
                break;
            }
    }
}

} // namespace ta
