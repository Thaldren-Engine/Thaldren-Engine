#pragma once
// Feature definitions (trees, rocks, wrecks, ...) from features\**\*.tdf,
// built in the same order and with the same quirks as TotalA.exe
// (see docs/RE_MEMORY.md, "Feature definitions").
//
// The table is per battle: it starts with the map's TNT feature list, then
// grows as other content asks for features by name (mission features, unit
// corpses), and finally resolve_chains() appends every dead / burnt /
// reclamate feature that is still missing.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ta/gaf.h"
#include "ta/hpi.h"
#include "ta/tdf.h"

namespace ta {

namespace feature_flags {
constexpr std::uint16_t kGaf = 1u << 0;  // 2D (GAF) feature; clear = 3D object
constexpr std::uint16_t kAnimating = 1u << 1;
constexpr std::uint16_t kAnimTrans = 1u << 2;
constexpr std::uint16_t kShadTrans = 1u << 3;
constexpr std::uint16_t kFlamable = 1u << 4;
constexpr std::uint16_t kGeothermal = 1u << 5;
constexpr std::uint16_t kBlocking = 1u << 6;
constexpr std::uint16_t kReclaimable = 1u << 7;
constexpr std::uint16_t kAutoReclaimable = 1u << 8;
constexpr std::uint16_t kIndestructible = 1u << 9;
constexpr std::uint16_t kNoDisplayInfo = 1u << 10;
constexpr std::uint16_t kNoDrawUnderGray = 1u << 11;
constexpr std::uint16_t kDefinedMask = 0x0FFF;  // bits 12-15 are never written by the game
}  // namespace feature_flags

enum FeatureSeq : int { kSeq, kSeqShadow, kSeqBurn, kSeqBurnShadow, kSeqDie, kSeqDieShadow, kSeqReclamate,
                        kSeqReclamateShadow, kSeqCount };

struct FeatureDefRecord {
    std::string name;          // [128], strncpy (no terminator at 128 characters)
    bool section_found = true; // false: the game shows "Record ... missing" and then crashes
    std::string description;   // [20]
    std::int16_t footprint_x = 0, footprint_z = 0;
    std::uint8_t height = 0;
    std::uint16_t flags = 0;   // feature_flags (bits 12-15 undefined)
    // 3D features
    std::string object;        // objects3d\<object>.3DO
    // 2D features
    std::string gaf_file;      // [16] file name, or "REUSE" when an earlier feature loaded the same file
    int gaf_owner = -1;        // index of the feature whose loaded GAF is used (-1 = none / load failed)
    std::array<std::string, kSeqCount> sequences;  // sequence names as stored in the GAF ("" = none)
    std::uint8_t spread_chance = 0, reproduce = 0, reproduce_area = 0;
    float metal = 0, energy = 0;
    std::int16_t damage = 0;
    std::int16_t spark_time = 0;  // ticks
    int burn_weapon = -1;         // weapon slot, -1 = none
    std::int16_t dead = -1, burnt = -1, reclamate = -1;  // feature indices, -1 = none
};

class FeatureDefTable {
public:
    // Parses every features\**\*.tdf (the game's per-battle feature library).
    // weapon_names: slot names of the weapon table, for burnweapon.
    void load_library(Vfs& vfs, const std::array<std::string, 256>& weapon_names);

    // Appends a definition unconditionally (the TNT list does this, duplicates included). Returns its index.
    int load(const std::string& name);
    // Index of the first case-insensitive name match; otherwise appends it.
    int find_or_load(const std::string& name);
    // Index of the first case-insensitive name match, or -1.
    int find(const std::string& name) const;

    // Map features in TNT order (each entry appended).
    void load_map_features(const std::vector<std::string>& tnt_names);
    // Features placed by the map's OTA schema ([GlobalHeader] > [<schema>] > [features] >
    // children in file order: Featurename, XPos, ZPos; a negative position drops the entry).
    void load_schema_features(const TdfSection& ota_root, const std::string& schema);
    // Resolves featuredead / featurereclamate / featureburnt for every entry,
    // including the ones appended while resolving.
    void resolve_chains();

    const std::vector<FeatureDefRecord>& records() const { return records_; }
    const std::vector<std::string>& notes() const { return notes_; }
    const std::vector<std::string>& library_files() const { return files_; }

private:
    const TdfSection* find_section(const std::string& name) const;
    Vfs* vfs_ = nullptr;
    const std::array<std::string, 256>* weapon_names_ = nullptr;
    std::vector<std::string> files_;
    std::vector<std::unique_ptr<TdfSection>> roots_;
    std::vector<FeatureDefRecord> records_;
    std::vector<std::unique_ptr<GafDirectory>> gafs_;  // per record: the GAF it loaded itself
    std::vector<std::string> notes_;
};

// Feature names listed in a TNT map file (version 0x1020 or 0x2000), in file order.
bool tnt_feature_names(const std::vector<std::uint8_t>& tnt, std::vector<std::string>& out);

} // namespace ta
