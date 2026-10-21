#pragma once
// The game CD checks (spec: docs/RE_MEMORY.md, "CD checks"). `0x41D6A0` looks for the game CD
// in the CD-ROM drives: "X:\TOTALA.ID" whose [Contents] has a nonzero Campaign (kind 0) or
// Multiplayer (kind 1) value; it answers the drive's letter, 0 when none. Its callers show
// "Please insert the Campaign CD (Disc 2) and try again" (and the Multiplayer / "a Total
// Annihilation CD" texts, CDCHECK.GUI) on 0, and the network room sets a player's CD bit from
// it. The 3.1 program on sale has the flag at 0x50289C set (initialised data, no writer): the
// check answers '.' at once, so every caller passes and no prompt is reachable; the path
// builder then makes ".\<dir>\<name>" (the game folder is the CD).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ta {

// 0x50289C in the shipped program.
constexpr bool kCdCheckFlag = true;

struct CdCheckEnv {
    // The next CD-ROM drive after the one given (0: from A), 0 when none (0x4BB190).
    std::function<char(char)> next_drive;
    // A file's bytes (nothing when it cannot be read).
    std::function<std::optional<std::string>(const std::string& path)> read_file;
    bool h_only = false;  // the -f switch (0x511DE0): only drive "h:" is looked at
};

// 0x41D6A0: '.' with the flag set; otherwise the drive letter of the first CD whose TOTALA.ID
// says it holds `kind` (0 campaign, 1 multiplayer), 0 when none (or another kind).
char cd_probe(int kind, bool flag, const CdCheckEnv& env);
// The check's answer the way its callers take it (nonzero: the CD is there).
inline bool cd_present(int kind, const CdCheckEnv& env) { return cd_probe(kind, kCdCheckFlag, env) != 0; }

// Scores reporting (0x4436E0 -> 0x46BF30, network games only): what the program does when a
// network game's session starts. The provider's GUID zero, serial or modem: nothing (4).
// Otherwise the score tables are made, the reporting flag set (0x51E590) and reporter.dll
// loaded: missing (as shipped: neither the Steam nor the CD version has it) -> no services, the
// report screen (REPORT.GUI) is not opened and nothing is shown (0); a library without its eight
// entry points -> "Unable to initialize scores reporting." in a 400-wide box over the
// ReportError picture (2); a complete one -> its services (REPORT.GUI when there are any). The
// hourglass pointer is set and put back around it (no frame is shown in between).
enum class ScoresReporting { NotTried = 4, Started = 0, LibraryBroken = 2 };
struct ScoresReportingEnv {
    std::array<std::uint8_t, 16> provider{};  // the session provider's GUID (GS+0x39201)
    bool lobby_override = false;              // 0x512C98 (skips the provider test)
    // LoadLibraryA("reporter.dll") and its exports: 0 missing, 1 complete, 2 an export missing.
    std::function<int()> load_library;
    // A complete library's _RIGetVersion / _RIInitializeEx: the service count, -1 when it
    // refuses (the library is let go).
    std::function<int()> services;
};
struct ScoresReportingState {
    bool reporting = false;  // 0x51E590 (the report routine 0x46C620 builds the score tables)
    bool library = false;    // 0x51E58C
    int services = 0;        // 0x5054A8: REPORT.GUI lists these
};
ScoresReporting start_scores_reporting(const ScoresReportingEnv& env, ScoresReportingState& state);
// What the session start shows: the report screen with `services` rows (0x4435A0), the error
// box, or nothing.
enum class ScoresReportingShown { Nothing, ReportScreen, ErrorBox };
ScoresReportingShown scores_reporting_shown(ScoresReporting r, const ScoresReportingState& state);
// This 64-bit program can never load the 32-bit library: the load fails as when it is missing.
inline int reporter_library_not_loadable() { return 0; }

}  // namespace ta
