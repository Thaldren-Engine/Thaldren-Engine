// The game CD checks and the scores reporting start (spec: docs/RE_MEMORY.md, "CD checks").

#include "ta/cd_check.h"

#include <cstdio>

#include "ta/tdf.h"

namespace ta {

char cd_probe(int kind, bool flag, const CdCheckEnv& env) {
    if (flag) return '.';
    const char* key = nullptr;
    if (kind == 0) key = "Campaign";
    else if (kind == 1) key = "Multiplayer";
    else return 0;
    char c = 0;
    while (true) {
        if (!env.h_only) c = env.next_drive ? env.next_drive(c) : 0;
        else c = static_cast<char>((c != 0 ? 0x98 : 0) + 0x68);  // "h", then 0 (0x100 cut to a byte)
        if (c == 0) return 0;
        char path[32];
        std::snprintf(path, sizeof path, "%c:\\TOTALA.ID", c);
        if (env.read_file) {
            if (std::optional<std::string> bytes = env.read_file(path)) {
                TdfParseResult r = tdf_parse(*bytes);
                if (r.error.empty() && r.root)
                    if (const TdfSection* s = r.root->child("Contents"))
                        if (s->get_int(key, 0) != 0) return c;
            }
        }
    }
}

ScoresReporting start_scores_reporting(const ScoresReportingEnv& env, ScoresReportingState& state) {
    // 0x46C190 first: nothing reporting, the library let go.
    state.reporting = false;
    state.library = false;
    if (!env.lobby_override) {
        static const std::array<std::uint8_t, 16> kZero{};
        // DPSPGUID_SERIAL {0F1D6860-88D9-11CF-9C4E-00A0C905425E} (0x4FCDB8) and DPSPGUID_MODEM
        // {44EAA760-CB68-11CF-9C4E-00A0C905425E} (0x4FCDC8).
        static const std::array<std::uint8_t, 16> kSerial{0x60, 0x68, 0x1D, 0x0F, 0xD9, 0x88, 0xCF, 0x11, 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E};
        static const std::array<std::uint8_t, 16> kModem{0x60, 0xA7, 0xEA, 0x44, 0x68, 0xCB, 0xCF, 0x11, 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E};
        if (env.provider == kZero || env.provider == kSerial || env.provider == kModem) return ScoresReporting::NotTried;
    }
    // 0x46BCE0: the score tables (their allocation never fails here).
    state.reporting = true;
    const int lib = env.load_library ? env.load_library() : 0;
    if (lib == 0) {
        state.services = 0;  // (*param_1 = 0)
        return ScoresReporting::Started;
    }
    if (lib == 2) return ScoresReporting::LibraryBroken;  // (FreeLibrary; the flag stays)
    const int n = env.services ? env.services() : -1;
    if (n >= 0) {
        state.library = true;
        state.services = n;
    }
    // (a refusing library is let go; the reporting flag stays set either way: 0)
    return ScoresReporting::Started;
}

ScoresReportingShown scores_reporting_shown(ScoresReporting r, const ScoresReportingState& state) {
    if (r == ScoresReporting::Started) return state.services != 0 ? ScoresReportingShown::ReportScreen : ScoresReportingShown::Nothing;
    if (r == ScoresReporting::NotTried) return ScoresReportingShown::Nothing;
    return ScoresReportingShown::ErrorBox;
}

}  // namespace ta
