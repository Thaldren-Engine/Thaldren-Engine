#pragma once
// The original game's campaigns and mission files (docs/RE_MEMORY.md, "Campaign"):
//   camps\<name>.tdf      [HEADER] campaignside; [MISSIONn] missionfile, missionname (+ translations)
//   maps\<mission>.ota    the mission's map info: briefing names, options, win/loss goal keys,
//                         and per difficulty a schema (Easy / Medium / Hard) with the start
//                         amounts, AI profile, meteors, and the placed units and features
//   camps\briefs\*.txt / *.wav   briefing text and narration
//   camps\useonly\*.tdf   the unit types a mission allows (one section per unit name)
// and the conversion into a battle start (sim::MissionStart) the way the original's mission
// loading does it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sim/battle_start.h"
#include "sim/mission_goals.h"
#include "ta/battle_loader.h"
#include "ta/hpi.h"

namespace ta {

// The archive rescan the original runs before a campaign starts (Vfs::scan): each scan mounts
// at most ten new .hpi of the game directory, then the CD drives' archives. On the Steam
// install the second scan adds totala3.hpi, totala4.hpi (the campaign) and worlds.hpi.
void mount_cd_archives(Vfs& vfs, const std::filesystem::path& game_dir);

struct CampaignMission {
    std::string file;  // map info file name ("AC01.ota")
    std::string name;  // missionname
};

struct Campaign {
    std::string name;  // file name without ".tdf"
    std::string side;  // campaignside
    bool has_header = false;  // the file has a [HEADER] (campaign lists show only those)
    std::vector<CampaignMission> missions;
};

// The campaign files in the game's enumeration order (camps\*.tdf), names without extension.
std::vector<std::string> list_campaigns(Vfs& vfs);
bool load_campaign(Vfs& vfs, const std::string& name, Campaign& out, std::string& error);

struct MissionFeature {
    std::string name;
    std::int32_t x = -1, z = -1;  // cells
};

struct MissionSpecial {
    bool start_pos = false;
    std::int32_t id = 0;
    std::int16_t x = 0, z = 0;
};

// Inputs the original's parsers take from uninitialised stack slots (a malformed goal or order
// string leaves them unread); defaults zero.
struct MissionParseState {
    std::array<std::int32_t, 4> goal_words{};  // the goal parser's integer slots
    std::string goal_name;                      // its type-name buffer
    float order_x = 0.0f, order_z = 0.0f;       // the order parser's two position floats
    std::uint32_t goal_record_address = 0;      // heap address of a goal record (run-varying)
};

struct MissionInfo {
    std::string file;   // "AC01.ota"
    std::string map;    // TNT base name (the mission file's base name)
    // Briefing and presentation.
    std::string name, description, brief, narration, hint, glamour, glamour_sound, planet, memory, numplayers;
    std::string brief_text;  // camps\briefs\<brief>.txt (empty when missing)
    std::string use_only;    // camps\useonly file name (empty: no restriction)
    bool no_movie = false;
    // Options.
    std::int32_t mapping = 0, line_of_sight = 0;
    std::int32_t max_units = 200;
    float kill_mul = 0.0f, time_mul = 0.0f;
    // The schema picked for the difficulty.
    int schema = -1;
    std::string schema_type;
    float human_metal = 0, human_energy = 0, computer_metal = 0, computer_energy = 0;
    std::int32_t surface_metal = 0;
    std::string ai_profile;
    std::vector<sim::MissionUnit> units;  // types unresolved (type = 0) until bound to a battle
    std::vector<MissionFeature> features;
    std::vector<MissionSpecial> specials;
    sim::MissionGoals goals;              // win and loss goals (types resolved at run time by name)
};

// Reads a mission's map info for a difficulty (0 easy, 1 medium, 2 hard) as the original's
// mission loading does (schema preference: easy: E, M, H; medium: M, E, H; hard: H, M, E; the
// first schema of the first type found).
bool load_mission(Vfs& vfs, const std::string& file, int difficulty, MissionInfo& out, std::string& error,
                  MissionParseState* parse = nullptr);

// Parses one placed unit's InitialMission string into orders (unit and type names resolved by
// `type_index`, which returns 0 for an unknown name).
std::vector<sim::MissionUnitOrder> parse_initial_orders(const std::string& text, const std::function<std::uint16_t(const std::string&)>& type_index,
                                                        MissionParseState& state);

// The win and loss goal keys of a mission's [GlobalHeader] (0x48E010).
class TdfSection;
void read_mission_goals(const TdfSection& global_header, MissionParseState& state, sim::MissionGoals& out);

// A battle request for a mission: two players (0: the human on the campaign's side, 1: the
// computer on the other side), the mission's options and its start (units, goals, amounts).
struct MissionRequest {
    std::string campaign_side = "ARM";
    std::string other_side;       // empty: the side that is not campaign_side ("ARM"/"CORE")
    int difficulty = 1;
    std::uint32_t battle_rng_seed = 0, loading_rand_seed = 0, crt_rand_state = 0;
};

// The battle request alone (for front ends that load battles themselves): the mission's
// start, its unit list read from `vfs`, and `vfs`'s archives as extra archives (so a front end
// with its own file system finds the CD's maps too).
BattleRequest make_mission_request(Vfs& vfs, const MissionInfo& mission, const MissionRequest& request);

std::unique_ptr<LoadedBattle> load_mission_battle(Vfs& vfs, const MissionInfo& mission, const MissionRequest& request,
                                                  sim::BattleOutside& outside, std::string& error);

}  // namespace ta
