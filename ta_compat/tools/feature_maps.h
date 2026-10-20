#pragma once
// Feature maps of network games, for comparing machines (net_play, dplay_play): what each
// machine's map holds per cell (feature, damage taken, burning), dumps in one text format the
// capture tool also writes for the original program, and the scripted orders that make units
// shoot at features. See docs/RE_MEMORY.md "Network and sync" and "Features and wrecks".
//
// Dump format: a line "step <n>", then one line per cell "x z type damage state" (type and
// damage as the map holds them: type hexadecimal; state '-' no animation slot, 'B' burning,
// 'D' dying or reclaim animation, 'S' a 3D feature with its slot damage).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "sim/commands.h"

namespace feature_maps {

struct FeatureCell {
    std::uint16_t type = sim::kNoFeature;
    std::uint16_t damage = 0;
    char state = '-';
    bool operator==(const FeatureCell& o) const { return type == o.type && damage == o.damage && state == o.state; }
};

inline std::vector<FeatureCell> feature_map(sim::Battle& b) {
    std::vector<FeatureCell> out(b.map.cells.size());
    const sim::FeatureState& st = b.feature_world().state;
    for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
        const sim::MapCell& c = b.map.cells[i];
        FeatureCell& f = out[i];
        f.type = c.feature;
        if (c.feature >= sim::kFirstSpecialFeature) continue;
        if (!(c.flags & 1)) {
            f.damage = c.feature_aux;
            continue;
        }
        const sim::FeatureSlot& s = st.slots[c.feature_aux % sim::kFeatureSlots];
        const bool flat = c.feature < b.features.size() && (b.features[c.feature].flags & sim::feature_type_flags::kFlat);
        if (!flat) {
            f.state = 'S';
            f.damage = s.get<std::uint16_t>(sim::FeatureSlot::kDamage);
        } else {
            f.state = (s.get<std::uint8_t>(sim::FeatureSlot::kFlags) & sim::feature_slot_flags::kBurning) ? 'B' : 'D';
        }
    }
    return out;
}

struct FeatureDiff {
    std::size_t cells = 0, type = 0, state = 0, damage = 0, damage_host_only = 0;
    std::vector<std::string> first;  // a few examples
};

// The first map against each other one. A damage counter only the host keeps (the other
// machine's cell reads 0 with the same type and state) is the original's own rule and is
// counted apart.
inline FeatureDiff compare_features(const std::vector<std::vector<FeatureCell>>& maps, const std::vector<bool>& host, int width) {
    FeatureDiff d;
    for (std::size_t m = 1; m < maps.size(); ++m)
        for (std::size_t i = 0; i < maps[0].size() && i < maps[m].size(); ++i) {
            const FeatureCell& a = maps[0][i];
            const FeatureCell& c = maps[m][i];
            if (a == c) continue;
            ++d.cells;
            const char* what = "";
            if (a.type != c.type) {
                ++d.type;
                what = "type";
            } else if (a.state != c.state) {
                ++d.state;
                what = "state";
            } else {
                if ((host[0] && c.damage == 0) || (host[m] && a.damage == 0)) {
                    ++d.damage_host_only;
                    continue;
                }
                ++d.damage;
                what = "damage";
            }
            if (d.first.size() < 12) {
                char line[160];
                std::snprintf(line, sizeof line, "%s at (%d,%d): machine 0 %04x %u %c, machine %zu %04x %u %c", what, static_cast<int>(i % width),
                              static_cast<int>(i / width), a.type, a.damage, a.state, m, c.type, c.damage, c.state);
                d.first.push_back(line);
            }
        }
    return d;
}

// Cells that differ from `start` (or have damage or a slot); with `start` empty every cell that
// holds anything.
inline void dump_features(std::FILE* f, std::uint32_t tick, const std::vector<FeatureCell>& now, const std::vector<FeatureCell>& start, int width) {
    if (!f) return;
    std::fprintf(f, "step %u\n", tick);
    for (std::size_t i = 0; i < now.size(); ++i) {
        const bool show = start.empty() ? now[i].type != sim::kNoFeature : (!(now[i] == start[i]) || now[i].damage || now[i].state != '-');
        if (show)
            std::fprintf(f, "%d %d %04x %u %c\n", static_cast<int>(i % width), static_cast<int>(i / width), now[i].type, now[i].damage, now[i].state);
    }
    std::fflush(f);
}

// Feature cells (their own cells) within `r` world units of a point.
inline std::vector<int> features_near(sim::Battle& b, const sim::Vec3i& at, int r, bool flammable_only) {
    std::vector<int> out;
    const int cx = (at[0] >> 16) / 16, cz = (at[2] >> 16) / 16, rc = r / 16;
    for (int z = std::max(0, cz - rc); z < std::min(b.map.height, cz + rc + 1); ++z)
        for (int x = std::max(0, cx - rc); x < std::min(b.map.width, cx + rc + 1); ++x) {
            const sim::MapCell& c = b.map.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(b.map.width) + static_cast<std::size_t>(x)];
            if (c.feature >= sim::kFirstSpecialFeature || c.feature >= b.features.size()) continue;
            if (flammable_only && !(b.features[c.feature].flags & sim::feature_type_flags::kFlammable)) continue;
            out.push_back(z * b.map.width + x);
        }
    return out;
}

// One machine's orders in the features scenario: its armed units shoot at the ground under
// features near them (trees, rocks, metal, wrecks), now and then at an enemy unit; the
// commander reclaims a feature near it every 150 steps.
template <class Rng>
void feature_orders(sim::Battle& b, int slot, Rng& rng, int step, bool attack_units = true) {
    const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(slot)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (id <= 0) continue;
        const auto i = static_cast<std::size_t>(id);
        if (!b.ticks[i].type_index || !(b.units[i].flags & sim::unit_flags::kAlive)) continue;
        const sim::Unit& u = b.units[i];
        const bool commander = id == p.first_unit;
        if (commander ? step % 150 != 0 : (rng() % 25) != 0) continue;
        sim::command::Command sel;
        sel.kind = sim::command::Kind::Select;
        sel.player = static_cast<std::uint8_t>(slot);
        sel.units.push_back(static_cast<std::uint16_t>(id));
        sim::command::apply(b, sel);
        sim::command::Command c;
        c.player = static_cast<std::uint8_t>(slot);
        c.kind = sim::command::Kind::IssueIntent;
        c.has_pos = true;
        const std::vector<int> near = features_near(b, u.pos, commander ? 160 : 260, !commander && (rng() % 2) == 0);
        if (near.empty()) continue;
        const int cell = near[rng() % near.size()];
        c.pos = {((cell % b.map.width) * 16 + 8) << 16, u.pos[1], ((cell / b.map.width) * 16 + 8) << 16};
        c.intent = commander ? sim::intent::kReclaim : sim::intent::kAttack;
        if (!commander && (rng() % 4) == 0 && attack_units) {
            for (std::size_t k = 1; k < b.units.size(); ++k)
                if (b.ticks[k].type_index && b.ticks[k].owner != slot && (b.units[k].flags & sim::unit_flags::kAlive)) {
                    c.target = static_cast<std::uint16_t>(k);
                    c.pos = b.units[k].pos;
                    break;
                }
        }
        sim::command::apply(b, c);
    }
}

// Armed units for the scenario: six of the side's (cannons, plasma, rockets or flames; the last
// start fires) in a row `away` world units south of a point.
template <class TypeIndex>
void place_feature_shooters(sim::Battle& b, int slot, const sim::Vec3i& near, TypeIndex type_index, int away = 200) {
    static const char* const names[2][3] = {{"ARMSTUMP", "ARMHAM", "ARMROCK"}, {"CORRAID", "CORPYRO", "CORSTORM"}};
    const bool core = b.players[static_cast<std::size_t>(slot)].commander_name.rfind("COR", 0) == 0;
    for (int k = 0; k < 6; ++k) {
        const std::uint16_t type = type_index(names[core ? 1 : 0][k % 3]);
        if (type) b.create_unit(slot, type, near[0] + ((k * 40 - 100) << 16), near[1], near[2] + (away << 16), true, 1);
    }
}

}  // namespace feature_maps
