#pragma once
// The original game's multiplayer screens on the client's front end (spec: docs/RE_MEMORY.md,
// "Client", "Multiplayer screens"): the connection screen (SELPROV.GUI), the TCP/IP address
// question (TCP.GUI), the game list (SELGAME.GUI), the new game question (NEWMULTI.GUI), the
// battle room (LOUNGE2.GUI) and their message boxes (MSGBOX.GUI), run over the engine's network
// room (net::BattleRoom) and transports.
//
// The original lists the session service's providers (TCP/IP, IPX, modem, serial); here the list
// holds the engine's own transports in the same list.

#include <memory>
#include <string>
#include <vector>

#include "ta/client_menus.h"

#ifdef TA_CLIENT_HAVE_NET
namespace ta {

// The main menu's multiplayer button opens the connection screen.
void install_net_screens(client::FrontEndHooks& hooks, MenuArt& art, const client::NetMenuConfig& config);

// Tests: multiplayer screens on their own, stacked in the order given, each with the state its
// argument names (the inputs gui_harness's commands of the same names read):
//   "selprov"           the connection screen with the engine's transports
//   "tcp[:ADDRESS]"     the address question (the saved address given)
//   "selgame:FILE"      the game list (FILE: "game ..." lines, "nick", "password")
//   "newmulti"          the new game question
//   "lounge:FILE"       the battle room from a room file (players, chat, map)
//   "message:WIDTH:TEXT" a message box
std::unique_ptr<client::Screen> make_net_test_screens(MenuArt& art, const client::NetMenuConfig& config, const std::vector<std::string>& specs);

}  // namespace ta
#endif
