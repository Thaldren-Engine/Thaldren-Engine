#pragma once
// Saved games (docs/RE_MEMORY.md, "Saved games").
//
// The original keeps a saved game in a "bank" file: a 0x22-byte header ("HAPIBANK", the bank
// name's offset in the string directory, the directory's file offset, the first account's
// offset, version 1, directory-compressed flag), then one account per section (a 0x20-byte
// header: size, name offset, counts of integer, double, string and blob entries, compression;
// the entries; the blob bytes), then the string directory (all names and string values,
// zero-terminated, optionally squashed). Saved games use the bank name "Total Annihilation
// 3.0" and a "Summary" section the load menu lists.
//
// The engine writes the same container with the original's Summary keys, so its saves look
// like the original's in a listing, and adds its own sections: the start description of the
// game and the battle's input journal (sim/journal.h). Loading replays the journal from the
// start: the state at the save is restored exactly (the original's own sections are a
// partial state snapshot whose loading does not continue the game identically, see
// RE_MEMORY). Banks written by the original can be read (sections, keys, blobs, squashed
// accounts) and their summaries shown; restoring a game from the original's sections is not
// done.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/journal.h"
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
    std::vector<Blob> blobs;

    void set_int(const std::string& key, std::int32_t v);
    void set_string(const std::string& key, const std::string& v);
    void set_double(const std::string& key, double v);
    bool has(const std::string& key) const;
    std::int32_t get_int(const std::string& key, std::int32_t def) const;
    std::string get_string(const std::string& key, const std::string& def) const;
    Blob& blob(const std::string& name);
    const Blob* find_blob(const std::string& name) const;
};

struct Bank {
    std::string name;  // the bank name ("Total Annihilation 3.0" for saved games)
    std::vector<BankSection> sections;
    BankSection& section(const std::string& name);  // found or added (case-insensitive)
    const BankSection* find(const std::string& name) const;
};

// Parses a bank (squashed accounts and directory included). `only`: read just that section.
bool read_bank(const std::vector<std::uint8_t>& bytes, Bank& out, std::string& error, const char* only = nullptr);
// Writes a bank (accounts and directory stored, not squashed, which the original also reads).
void write_bank(const Bank& bank, std::vector<std::uint8_t>& out);

// --- What a game was started from.
struct GameStart {
    enum class Kind : std::uint8_t { Battle = 0, Mission = 1 } kind = Kind::Battle;
    BattleRequest battle;      // Kind::Battle (map, schema, sides, players, options, seeds)
    std::string campaign;      // Kind::Mission: campaign file name, mission index and file
    std::int32_t mission_index = -1;
    std::string mission_file;
    MissionRequest mission;
};

// Builds the battle a GameStart describes (missions: the campaign data and the CD archives
// must be mounted on `vfs`).
std::unique_ptr<LoadedBattle> start_game(Vfs& vfs, const GameStart& start, sim::BattleOutside& outside, std::string& error);

// --- Saved games.
struct SaveSummary {
    std::int32_t max_units = 0;
    std::string campaign, mission, map, description;
    std::int32_t difficulty = 0, side = 0, players = 0, game_type = 2;
    std::int32_t commander_death = 0, location = 0, mapping = 0, line_of_sight = 0, line_of_sight_type = 0;
    bool between_missions = false;
    std::int32_t game_id = 0;
    std::uint32_t game_time = 0;  // steps
    bool engine = false;          // carries the engine's sections (exact restore possible)
};

struct SavedGame {
    SaveSummary summary;
    GameStart start;
    sim::Journal journal;
    std::uint32_t tick = 0;      // the step the save was made after
    std::uint32_t checksum = 0;  // sim::state_checksum then
    std::uint32_t battle_rng = 0, crt_rand = 0;  // both generators then
};

// The summary of a battle and its start (game type, map, players, options) for a save.
SaveSummary make_summary(const sim::Battle& battle, const GameStart& start, const std::string& description);

bool write_saved_game(const SavedGame& save, std::vector<std::uint8_t>& out);
bool read_saved_game(const std::vector<std::uint8_t>& bytes, SavedGame& out, std::string& error);
// The summary alone (also of the original's saves).
bool read_save_summary(const std::vector<std::uint8_t>& bytes, SaveSummary& out, std::string& error);

// Rebuilds the battle of a save and replays it to the save's step; checks the checksum and
// both generators. `player` (optional) is left positioned after the save, to replay the
// clock values and further journal parts if needed.
std::unique_ptr<LoadedBattle> load_saved_game(Vfs& vfs, const SavedGame& save, sim::BattleOutside& outside, std::string& error);

}  // namespace ta
