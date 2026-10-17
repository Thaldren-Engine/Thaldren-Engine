#pragma once
// The original's typed commands (a chat line starting with '+'): its command table with the
// names, permission bits and effects of every entry, and the default command that creates
// units by name (docs/RE_MEMORY.md, "Console commands"). The table is registered into the
// engine's command registry (sim/console.h) and runs on the client's chat line.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/console.h"

namespace client {
class GameClient;
}

namespace ta {

class ClientWorld;

// Permission bits of the table's entries.
namespace console_bits {
constexpr std::uint32_t kOption = 1, kCheat = 2, kDebug = 4, kProfile = 8;
}

// The table: every name with its permission bits (the computer player's profile commands
// plan / weight / limit share the registry with bits 8; the default command has bits 4).
struct ConsoleTableEntry {
    const char* name;
    std::uint32_t bits;
};
const std::vector<ConsoleTableEntry>& console_table();
std::uint32_t console_table_bits(const std::string& name);  // 0: not in the table

// What the commands keep between lines (the original's switches no other part reads yet).
struct ConsoleState {
    std::uint32_t kill_word = 0;          // the stack word the kill routine reads as its wreck level (an input)
    bool net_drop_off = false;            // "+drop": a silent player is not dropped
    bool screen_chat = true;              // chat shown on the screen (setting screenchat)
    bool sing = false;                    // units' notices replaced by the two "sing" sounds
    bool big_brother = false;             // the view follows the tracked unit
    bool bps = false, compression = true; // network statistics / outgoing compression
    bool profile = false;                 // the debug profiler display
    std::int32_t contour[2] = {0, 0};     // contour line display (interval, shade)
    std::int32_t gamma = 12;
    std::int32_t light[3] = {0, 0, 0};    // light direction
    std::int32_t senderror = 0;           // simulated send errors (percent)
    std::int32_t film_speed = 0;
    std::string film_dir;
    std::vector<std::string> not_built;   // commands that ran but have no effect here yet
};

class ConsoleCommands {
public:
    ConsoleCommands(client::GameClient& client, ClientWorld& world);
    ~ConsoleCommands();
    // Runs one command line (the text after the '+') with the allowed permission bits;
    // returns the bits of the command that ran (0: none).
    std::uint32_t run(const std::string& line, std::uint32_t allowed);
    ConsoleState& state();
    const sim::console::Registry& registry() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Puts the original's command table on the client's chat line (the returned object must
// outlive the client's use of it; the client's on_command calls it).
std::shared_ptr<ConsoleCommands> install_console_commands(client::GameClient& client, ClientWorld& world);

}  // namespace ta
