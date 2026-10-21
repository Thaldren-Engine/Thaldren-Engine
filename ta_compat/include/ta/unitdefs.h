#pragma once
// Unit definition table built from units\*.FBI exactly as TotalA.exe does
// (see docs/RE_MEMORY.md, "Unit definitions").

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ta/hpi.h"

namespace ta {

// UnitTypeFlags / UnitCanDoFlags bits (subset referenced by the importer).
namespace unit_flags {
constexpr std::uint32_t kHasWeapons = 1u << 16;
constexpr std::uint32_t kVersionCompatible = 1u << 23;
constexpr std::uint32_t kHasGui0 = 1u << 31;
constexpr std::uint32_t kCanCloakFromCost = 1u << 13;  // can-do
}  // namespace unit_flags

struct UnitDefRecord {
    // Fixed-size text fields hold at most size-1 characters.
    std::string name;         // [32]
    std::string unit_name;    // [32]
    std::string description;  // [64]
    std::string object_name;  // [32]
    std::string side;         // [30]
    std::string ai_weight;    // [64]
    std::string ai_limit;     // [64]
    std::uint32_t fbi_checksum = 0;
    std::uint32_t script_checksum = 0;
    std::uint32_t weapon_hash_xor = 0;
    std::int16_t footprint_x = 0, footprint_z = 0;
    bool has_yard_map = false;
    std::vector<std::uint8_t> yard_map;
    std::int32_t build_options_count = 0;
    bool has_build_options = false;
    std::array<std::uint16_t, 30> build_options{};
    std::int32_t max_this_unit = 0;
    std::int32_t model_min_x = 0, model_min_y = 0, model_min_z = 0;
    std::int32_t model_max_x = 0, model_max_y = 0, model_max_z = 0;
    std::int32_t model_size_x = 0, model_size_y = 0, model_size_z = 0;
    std::int32_t model_avg_size = 0;
    float build_cost_energy = 0, build_cost_metal = 0;
    std::int32_t max_velocity = 0, scaled_max_velocity = 0, brake_rate = 0, acceleration = 0;
    std::int32_t bank_scale = 0, pitch_scale = 0, damage_modifier = 0, move_rate1 = 0, move_rate2 = 0;
    int movement_class = -1;  // CLASS index, -1 = none
    std::uint16_t turn_rate = 0;
    std::string corpse;       // feature name ("" = none); the id depends on the map's feature list
    bool has_corpse = false;
    std::int16_t corpse_feature_id = -1;  // feature index (-1 = none; set only with a feature table; "None" has 0)
    std::int16_t max_water_depth = 0, min_water_depth = 0;
    float energy_make = 0, energy_use = 0, metal_make = 0, extracts_metal = 0, wind_generator = 0;
    float tidal_generator = 0, cloak_cost = 0, cloak_cost_moving = 0, energy_storage = 0, metal_storage = 0;
    std::int32_t build_time = 0;
    int weapon1 = 0, weapon2 = 0, weapon3 = 0;  // weapon slot (0 when not found; -1 = null pointer)
    std::uint32_t max_damage = 0;
    std::int16_t worker_time = 0, heal_time = 0, sight_distance = 0, radar_distance = 0, sonar_distance = 0;
    std::int16_t min_cloak_distance = 0, radar_distance_jam = 0, sonar_distance_jam = 0, sound_category = 0;
    std::int16_t build_angle = 0, build_distance = 0, maneuver_leash_length = 0, attack_run_length = 0;
    std::int16_t kamikaze_distance = 0, sort_bias = 0, cruise_alt = 0;
    std::uint16_t index = 0;
    int explode_as = 0, self_destruct_as = 0;
    std::uint8_t max_slope = 0, max_water_slope = 0, transport_size = 0, transport_capacity = 0;
    std::uint8_t waterline = 0, makes_metal = 0, build_menu_page_count = 0, bm_code = 0, default_mission = 0;
    // Category names as looked up (whole value strings; matched case-insensitively).
    std::string wpri_bad_target, wsec_bad_target, wspe_bad_target, no_chase;
    std::uint32_t type_flags = 0;
    std::uint32_t can_do_flags = 0;
    std::string source_file;  // FBI file name found during enumeration
    int model_pieces = 0;     // piece count of objects3d\<objectname>.3DO
};

// Named 512-bit unit masks keyed case-insensitively; keeps the first spelling.
class CategoryRegistry {
public:
    std::array<std::uint32_t, 16>& lookup_or_create(const std::string& name);
    const std::array<std::uint32_t, 16>* find(const std::string& name) const;
    const std::map<std::string, std::pair<std::string, std::array<std::uint32_t, 16>>>& all() const { return masks_; }

private:
    std::map<std::string, std::pair<std::string, std::array<std::uint32_t, 16>>> masks_;  // key: lower-case
};

struct UnitDefContext {
    std::array<std::string, 256> weapon_names;        // by slot ("" = empty)
    std::array<std::string, 32> movement_class_names; // by CLASS index ("" = empty)
    struct MoveInfo {
        std::int16_t footprint_x = 0, footprint_z = 0, max_water_depth = 10000, min_water_depth = -10000;
        std::uint8_t max_slope = 0xFF, bad_slope = 0xFF, max_water_slope = 0xFF, bad_water_slope = 0xFF;
    };
    std::array<MoveInfo, 32> movement_classes{};
    std::vector<std::string> sound_categories;         // section names of gamedata\sound.tdf
    std::vector<std::string> mission_names = default_mission_names();  // sorted lookup names (index = mission type)
    std::string language = Vfs::game_language();       // key prefix for localized strings

    // The game's mission table sorted by lookup name (verified against a live dump).
    static std::vector<std::string> default_mission_names();
    bool cd_check_enabled = true;                      // loose (non-archive) FBIs/weapons rejected
    // The game enumerates the unit files (and the weapon files it checksums with them) once,
    // at its first main menu, when only the start-up scan has run; battles are loaded after
    // later rescans. When set, these lists ("units\*.FBI", "Weapons\*.tdf" names from a Vfs
    // after mount_game_dir only) are used instead of enumerating `vfs` again.
    std::optional<std::vector<std::string>> enumerated_unit_files, enumerated_weapon_files;
    std::uint8_t game_version_major = 3, game_version_minor = 1;
};

// One download menu entry (download\*.TDF [MENUENTRYn]) as the game keeps it: the builder's
// unit index, MENU (the build page is MENU - 1), BUTTON (the page's gadget record BUTTON + 4
// takes the unit) and the unit's name as written (not checked). In table order: files in
// enumeration order, then their sections.
struct DownloadMenuEntry {
    std::uint16_t builder = 0;
    std::uint8_t menu = 0, button = 0;
    std::string unit_name;
};

struct UnitDefTable {
    std::vector<UnitDefRecord> units;  // [0] is the "None" record
    CategoryRegistry categories;
    std::vector<std::string> notes;    // diagnostics (e.g. duplicate unit names)
    std::vector<DownloadMenuEntry> download_menus;
    // 0x42A8D0's end: some definitions were dropped and none for a failed copyright line or a
    // loose file under the CD check (those set its "tampered" flag): the program then shows
    // "Incompatible units found. ..." (a system message box, 0x4B6B80).
    bool incompatible_notice = false;
};

// The enumeration's verdict alone (the stubs of every units\*.FBI; no properties loaded): whether
// 0x42A8D0 would show "Incompatible units found. ..." (UnitDefTable::incompatible_notice).
bool unit_files_incompatible_notice(Vfs& vfs, const UnitDefContext& ctx);

// Loads weapon slot names, movement classes and sound categories into ctx
// (mission names and language must be supplied by the caller).
void load_unit_context(Vfs& vfs, UnitDefContext& ctx);

class FeatureDefTable;

// The model box from the footprint (0x42D079): min = ((-f) << 20) / 2, max = (f << 20) / 2 per
// axis (C division), size = max - min (y too), average size = (size z + size x) / 3; 32-bit wrapping.
void footprint_model_box(UnitDefRecord& u);

// Enumerate, filter, sort and load every unit definition. With a feature table,
// each corpse is looked up there (and appended if missing), as the game does
// during battle loading; corpse_feature_id then holds the index.
UnitDefTable load_unit_defs(Vfs& vfs, const UnitDefContext& ctx, FeatureDefTable* features = nullptr);

// The debug command Reload (0x42D1F0): units\<unitname>.FBI read again over the existing
// record of a running battle (0x42BF40 in place: keys write their fields, nothing is reset
// first). The corpse is looked up by name in the battle's feature table; a corpse the battle
// did not load cannot be loaded any more (the loading parsers are gone: the original shows
// "Record %s missing from feature" and then crashes), which returns false with `error`.
// Nothing happens (true) when the file is missing or empty. The YardMap scratch buffer is
// the caller's (`yard_buffer`, the original's stack buffer: an input).
bool reload_unit_properties(Vfs& vfs, const UnitDefContext& ctx, UnitDefTable& table, std::size_t index,
                            const FeatureDefTable& features, std::string& yard_buffer, std::string& error);

} // namespace ta
