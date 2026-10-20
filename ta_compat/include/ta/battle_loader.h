#pragma once
// Builds a battle from the original game's files the way the original's battle loading does:
// every unit, weapon and feature definition, the map (TNT tiles and heights, OTA schema:
// start positions, metal, wind, tide), animations, sight tables, movement classes, then the
// engine's set-up (sim/battle_start.h) with the players' sides and commanders.
//
// LoadedBattle owns everything the battle points into; keep it alive as long as the battle.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "sim/battle_start.h"
#include "ta/battle_features.h"
#include "ta/battle_types.h"
#include "ta/featuredefs.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace ta {

struct LoadedBattle;

// The rand() draws of the explosion tables the original computes while a battle loads
// (0x420620, presentation only; fixed parameters, so a fixed count: measured on 10 captures).
constexpr std::uint32_t kExplosionTableDraws = 391606;

struct BattleRequest {
    std::string map = "Moon Quartet";  // maps\<map>.tnt / .ota
    int schema = -1;                   // the OTA schema (-1: chosen as the original does)
    // Per player slot: the side name ("ARM", "CORE"), used for the commander; the rest of the
    // player description (kind, colour, alliances, start position, options, seeds) is
    // start.players / start.*. A player's commander_type is filled from its side.
    std::array<std::string, 10> sides{};
    sim::BattleStart start;
    // The loading thread's srand(time()) value. When set, the loading thread's rand() runs from
    // it the way the original's battle loading does: the map's features are placed (geothermal
    // vents draw for their steam), then the explosion tables draw kExplosionTableDraws times,
    // and start.loading_rand_seed becomes the state at the start-position shuffle.
    bool loading_srand_given = false;
    std::uint32_t loading_srand = 0;
    // Optional: sees the loaded data and the start description just before the set-up runs
    // (missions resolve their unit names to type indices here).
    std::function<void(LoadedBattle&, sim::BattleStart&)> prepare_start;
    // Archives to mount before loading when the file system has not mounted them yet (a
    // mission on the game CD's archives).
    std::vector<std::filesystem::path> extra_archives;
    // A saved game (the original's battle start with a bank): the map's own features are not
    // placed and the schema's features are neither loaded nor placed (Map_LoadTnt); the saved
    // game's Features section places them (start.restore).
    bool saved_game = false;
};

struct LoadedBattle {
    // Static data (the battle points into these).
    sim::HeightMap heights;
    sim::BattleRng rng;
    UnitDefContext unit_context;
    UnitDefTable unit_defs;
    WeaponDefTable weapon_defs;
    FeatureDefTable feature_defs;
    std::vector<sim::WeaponType> weapons;
    std::vector<std::unique_ptr<UnitTypeAssets>> type_assets;  // by battle type index ([0] empty)
    // Assets replaced while the battle runs (the debug command Reload); units made before
    // keep pointing into them.
    std::vector<std::unique_ptr<UnitTypeAssets>> retired_assets;
    std::vector<const UnitDefRecord*> type_defs;                // by battle type index ([0] null)
    std::vector<std::string> feature_models;                    // 3D feature model names
    std::vector<std::uint8_t> tnt, ota;
    int schema = -1;  // the map info schema the battle uses (-1: none)
    std::unique_ptr<AnimImporter> anims;
    std::unique_ptr<sim::Battle> battle;

    // Battle type index of a unit by its unit name (0: none).
    std::uint16_t type_index(const std::string& unit_name) const;
};

// Loads the game data and the map and starts the battle. `outside` must outlive the result.
// `before_start` (optional) sees the battle once its data is loaded, just before the set-up
// runs (for hooks that need the battle during the set-up).
std::unique_ptr<LoadedBattle> load_battle(Vfs& vfs, const BattleRequest& request, sim::BattleOutside& outside,
                                          std::string& error,
                                          const std::function<void(sim::Battle&)>& before_start = {});

}  // namespace ta
