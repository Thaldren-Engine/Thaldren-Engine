// The original's typed command table: names and permission bits, in the order its three
// lists register them (docs/RE_MEMORY.md, "Console commands").

#include "ta/console_commands.h"

namespace ta {

const std::vector<ConsoleTableEntry>& console_table() {
    namespace cb = console_bits;
    static const std::vector<ConsoleTableEntry> table = {
        // Options: always allowed.
        {"NoShake", cb::kOption}, {"Contour", cb::kOption}, {"ScrollSpeed", cb::kOption}, {"IFace", cb::kOption},
        {"Give", cb::kOption}, {"CDPlay", cb::kOption}, {"CDStop", cb::kOption}, {"Sound3D", cb::kOption},
        {"Shading", cb::kOption}, {"AntiAlias", cb::kOption}, {"Shadow", cb::kOption}, {"Dither", cb::kOption},
        {"SwitchAlt", cb::kOption}, {"TShadow", cb::kOption}, {"FShadow", cb::kOption}, {"LOSType", cb::kOption},
        {"Light", cb::kOption}, {"RCache", cb::kOption}, {"Selectable", cb::kOption}, {"MusicMode", cb::kOption},
        {"Logo", cb::kOption}, {"ScreenChat", cb::kOption}, {"Gamma", cb::kOption}, {"Clock", cb::kOption},
        {"NetStats", cb::kOption}, {"Sing", cb::kOption}, {"NoMetal", cb::kOption}, {"NoEnergy", cb::kOption},
        {"BigBrother", cb::kOption}, {"Now", cb::kOption}, {"Drop", cb::kOption}, {"ShootAll", cb::kOption},
        {"ShareMetal", cb::kOption}, {"ShareEnergy", cb::kOption}, {"ShareMapping", cb::kOption}, {"ShareRadar", cb::kOption},
        {"ShareAll", cb::kOption}, {"ShowRanges", cb::kOption}, {"SetShareMetal", cb::kOption}, {"SetShareEnergy", cb::kOption},
        {"Compression", cb::kOption}, {"BPS", cb::kOption}, {"SFX", cb::kOption},
        // Cheats.
        {"Radar", cb::kCheat}, {"ATM", cb::kCheat}, {"View", cb::kCheat}, {"LOS", cb::kCheat}, {"Mapping", cb::kCheat},
        {"DoubleShot", cb::kCheat}, {"HalfShot", cb::kCheat}, {"NowISee", cb::kCheat}, {"Meteor", cb::kCheat},
        {"MakePoster", cb::kCheat},
        // Debug commands.
        {"AI", cb::kDebug}, {"Control", cb::kDebug}, {"Kill", cb::kDebug}, {"IWin", cb::kDebug}, {"ILose", cb::kDebug},
        {"Film", cb::kDebug}, {"FilmSpeed", cb::kDebug}, {"Assert", cb::kDebug}, {"Assign", cb::kDebug}, {"BurnAll", cb::kDebug},
        {"BurnOne", cb::kDebug}, {"DebugBreak", cb::kDebug}, {"DPrint", cb::kDebug}, {"Edge", cb::kDebug}, {"Include", cb::kDebug},
        {"Mem", cb::kDebug}, {"MemDump", cb::kDebug}, {"Move", cb::kDebug}, {"PrintWeights", cb::kDebug}, {"Profile", cb::kDebug},
        {"Reload", cb::kDebug}, {"ReloadAIProfiles", cb::kDebug}, {"Save", cb::kDebug}, {"SeaLevel", cb::kDebug},
        {"Search", cb::kDebug}, {"SelBoxes", cb::kDebug}, {"Senderror", cb::kDebug}, {"TreeDeath", cb::kDebug},
        {"Feature", cb::kDebug}, {"ZBuffer", cb::kDebug},
        // The computer player's profile commands (registered by the profile loader).
        {"plan", cb::kProfile}, {"weight", cb::kProfile}, {"limit", cb::kProfile},
    };
    return table;
}

std::uint32_t console_table_bits(const std::string& name) {
    for (const ConsoleTableEntry& e : console_table())
        if (name == e.name) return e.bits;
    return 0;
}

}  // namespace ta
