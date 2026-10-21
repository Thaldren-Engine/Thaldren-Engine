#pragma once
// Saved games in the original's format (docs/RE_MEMORY.md, "Saved games").
//
// The original keeps a saved game in a "bank" file: a 0x22-byte header ("HAPIBANK", the bank
// name's offset in the string directory, the directory's file offset, the first account's
// offset, version 1, directory-squashed flag), then one account per section (a 0x20-byte
// header: size, name offset, counts of integer, double, string and blob entries, squashed
// flag; the entries; the blob bytes), then the string directory (all names and string
// values, zero-terminated). Saved games use the bank name "Total Annihilation 3.0", squash
// their accounts and directory when that is smaller, and hold:
//   Summary        the keys the load menu lists, the radar image
//   Players        the game time block, per player its resources, totals, timers, alliances
//   Camera         the view position
//   Mapping        the explored map
//   Features       the feature type names and every feature on the map
//   PlayerFeatures the cells' owner nibbles;  Metal  the metal map
//   Units          per unit a 0xB8 record, its script, orders (and their goals), motion and
//                  resource slot
//   Meteor         the meteor shower's schedule
//   VictoryCondition_* / DefeatCondition_*   a mission's goal flags (campaigns)
// This is a partial state: what is not written (both random generators, projectiles,
// effects, the route search, motion goals, computer players, wind, ...) starts afresh when
// the original loads it, and the game does not continue as it would have. The engine writes
// and reads exactly this, so its saves and the original's are interchangeable and a loaded
// game goes on as the original's would.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ta/battle_loader.h"
#include "ta/campaign.h"
#include "ta/hpi.h"

namespace ta {

// --- The bank container.
struct BankSection {
    std::string name;
    std::vector<std::pair<std::string, std::int32_t>> ints;   // in insertion order
    std::vector<std::pair<std::string, double>> doubles;
    std::vector<std::pair<std::string, std::string>> strings;
    struct Blob {
        bool named = true;
        std::string name;
        std::int32_t index = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<Blob> blobs;  // in creation order (named and numbered mixed, as the original keeps them)

    void set_int(const std::string& key, std::int32_t v);
    void set_string(const std::string& key, const std::string& v);
    void set_double(const std::string& key, double v);
    bool has(const std::string& key) const;
    std::int32_t get_int(const std::string& key, std::int32_t def) const;
    double get_double(const std::string& key, double def) const;
    std::string get_string(const std::string& key, const std::string& def) const;
    const std::string* find_string(const std::string& key) const;
    Blob& blob(const std::string& name);        // found or added (case-insensitive)
    Blob& numbered(std::int32_t index);          // found or added
    const Blob* find_blob(const std::string& name) const;
    const Blob* find_numbered(std::int32_t index) const;
};

struct Bank {
    std::string name;  // the bank name ("Total Annihilation 3.0" for saved games)
    std::vector<BankSection> sections;
    BankSection& section(const std::string& name);  // found or added (case-insensitive)
    const BankSection* find(const std::string& name) const;
};

// Parses a bank (squashed accounts and directory included). `only`: read just that section.
bool read_bank(const std::vector<std::uint8_t>& bytes, Bank& out, std::string& error, const char* only = nullptr);

// How a bank is written. The original's writer leaves stack words in two places: a named
// blob's descriptor index (the previous numbered blob's index in that account, else
// `named_blob_word`), and the byte after a squashed chunk's end marker (the dictionary
// coder's item slot, `squash_stale_byte`). Both are inputs.
struct BankWriteOptions {
    bool squash = false;                 // squash accounts and the directory when smaller (saved games do)
    std::uint32_t named_blob_word = 0;
    std::uint8_t squash_stale_byte = 0;
};
// Writes a bank. `stale` (optional) receives the file offsets of the named-blob words this
// writer took from `named_blob_word` (in stored accounts).
void write_bank(const Bank& bank, std::vector<std::uint8_t>& out, std::vector<std::size_t>* stale = nullptr,
                const BankWriteOptions& options = {});

// --- The Summary section (GameSave_SaveAll 0x4326B0 / the load menu 0x492360).
struct SaveSummary {
    std::int32_t max_units = 0;
    bool has_campaign = false;     // the key is absent for skirmishes (no campaign name)
    std::string campaign, mission, map, thumbs, description;
    bool has_description = false;
    std::int32_t difficulty = 0, side = 0, players = 0, game_type = 2;
    bool skirmish_keys = false;    // CommanderDeath .. LineOfSightType present (game type 2)
    std::int32_t commander_death = 1, location = 1, mapping = 1, line_of_sight = 1, line_of_sight_type = 1;
    bool between_missions = false;
    std::int32_t game_id = 0;
    std::uint32_t game_time = 0;   // steps
    bool has_battle = false;       // the battle sections are present (saved during a battle)
};

bool read_save_summary(const std::vector<std::uint8_t>& bytes, SaveSummary& out, std::string& error);
void read_summary(const BankSection& s, SaveSummary& out);

// --- Writing a running battle.

// What the original's writer reads outside the simulation, or from its own uninitialised
// stack (inputs; the client passes what it has, documented per field).
struct SaveContext {
    // Summary.
    std::string description;            // the name typed in the save menu
    std::uint16_t max_units = 0;        // GS+0x37EEC (the skirmish unit limit setting)
    std::int32_t game_id = 0;           // time(NULL) at the save
    std::int32_t game_type = 2;         // the map info's provider: 1 campaign, 2 skirmish
    std::string campaign;               // campaign name ("" for skirmishes: no key)
    std::string mission;                // the map info's mission name (campaign: the localized
                                        // missionname; skirmish: the map name); also "Map"
    std::string thumbs;                 // GS+0x391CF (the map thumbnail name of the load menu)
    std::int32_t difficulty = 0, side = 0, players = 0;
    std::int32_t commander_death = 0, location = 0, mapping = 0, line_of_sight = 0, line_of_sight_type = 0;
    // Saved between missions (the front end's state is not "in battle"): only the Summary,
    // with BetweenMissions and the next mission's name.
    bool between_missions = false;
    // The radar image (the minimap composite of the viewed player): width, height, rows.
    std::int32_t radar_width = 0, radar_height = 0;
    std::vector<std::uint8_t> radar_pixels;
    // Camera (the view's scroll position, world units).
    std::int32_t camera_x = 0, camera_z = 0;
    // Per player record: the side record's side and logo (colour) bytes (+0x95 / +0x96).
    std::array<std::uint8_t, 10> player_side{}, player_logo{};
    // The game time block (GS+0x38A37, 0x1C bytes: the step timing and speed fields of the
    // front end around the game tick at +0x10, which is taken from the battle).
    std::array<std::uint8_t, 0x1C> time_block{};
    // Stale stack words of the unit writer (they persist from unit to unit within one save):
    // the record buffer's bytes before the first unit (the type name's tail beyond its
    // terminator, the weapon entries' top three flag bits, the flag word's bits 17-19).
    std::array<std::uint8_t, 0xB8> unit_record_stack{};
    // The order goals' first stack words (8 bytes: point and manoeuvre goals; 4: the others).
    std::array<std::uint8_t, 8> goal_record_stack{};
    BankWriteOptions bank{true, 0, 0};
};

// The sections the original writes, from a battle (LoadedBattle gives the type and feature
// names). `notes` receives where the original would crash (a unit without a script) or
// writes something this writer cannot know.
Bank make_save_bank(const LoadedBattle& battle, const SaveContext& ctx, std::vector<std::string>* notes = nullptr);
// The whole file.
bool write_save_file(const LoadedBattle& battle, const SaveContext& ctx, std::vector<std::uint8_t>& out);
// Between missions: the Summary alone.
Bank make_between_missions_bank(const SaveContext& ctx);

// --- Loading.

// The game a saved game starts: the request the original's load menu sets up (map or mission,
// players and their controllers and sides, options), seeds included (inputs).
struct LoadInputs {
    std::uint32_t battle_rng_seed = 0;    // QueryPerformanceCounter at the battle start
    std::uint32_t loading_rand_seed = 0;  // time() of the loading thread
    std::uint32_t crt_rand_state = 0;     // the simulation thread's rand() at the first step
    std::uint16_t unit_limit = 250;       // skirmish: the UnitLimit setting (the save's maxunits only
                                          // reaches GS+0x37EEC after the tables are made)
    // A skirmish save's schema: the load (0x492360) loads the map info (0x435A20, kind 2) before
    // the save's players reach the set-up table, so 0x436860 counts the table as the session
    // left it (MenuArt::setup_players). 0: not given (the save's highest player slot + 1).
    int setup_players = 0;
};

struct SavedGame {
    Bank bank;
    SaveSummary summary;
};
bool read_saved_game(const std::vector<std::uint8_t>& bytes, SavedGame& out, std::string& error);

// What the load restored outside the simulation (for the client).
struct LoadedView {
    std::int32_t camera_x = 0, camera_z = 0;
    std::array<std::uint8_t, 0x1C> time_block{};
    std::array<std::uint8_t, 10> side{}, logo{};  // per player record (side record +0x95 / +0x96)
    std::vector<std::string> notes;  // where the original reads uninitialised memory or would crash
};

// Starts the saved game's battle the way the original's battle start does with a bank (the
// map's features are not placed, no start positions or commanders, no starting resources;
// after the line-of-sight rebuild the sections are loaded). Missions need the campaign data
// and the CD archives mounted on `vfs`. Between-missions saves have no battle (use the
// summary's campaign and mission).
std::unique_ptr<LoadedBattle> load_saved_game(Vfs& vfs, const SavedGame& save, const LoadInputs& inputs, sim::BattleOutside& outside,
                                              LoadedView& view, std::string& error);

// The battle request a save starts from (also for front ends that load battles themselves);
// `restore` must then run inside the start (BattleRequest::start.restore is set to it).
bool make_saved_game_request(Vfs& vfs, const SavedGame& save, const LoadInputs& inputs, BattleRequest& out, LoadedView& view,
                             std::string& error);

// The sections' loaders on a battle being started (GameSave_LoadAll 0x432610's order:
// Players, Camera, Features, Metal, PlayerFeatures, Mapping, Units, Meteor, victory).
bool load_save_sections(LoadedBattle& lb, const Bank& bank, LoadedView& view, std::string& error);

// Parts, for harnesses: the Units section's writer and loader on their own.
void write_units_section(const LoadedBattle& lb, BankSection& units, const SaveContext& ctx, std::vector<std::string>* notes);
void load_units_section(LoadedBattle& lb, const BankSection& units, LoadedView& view);

}  // namespace ta
