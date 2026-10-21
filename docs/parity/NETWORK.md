# Network parity checklist (multiplayer end to end)

Status per item: **BUILT+VERIFIED** (evidence named), **BUILT-UNVERIFIED** (read in the original and
implemented, not checked against it), **PARTIAL**, **MISSING**, **NOT-APPLICABLE** (reason).
Sources: Ghidra `/TotalA.exe` (the dispatch `0x453D40` read case by case, its callees, the room
`0x447B10` / `0x448C70`, the end of the game `0x499200`), the original's strings, `docs/RE_MEMORY.md`
("Network and sync" and its sub-sections, "Features and wrecks"), the engine (`engine/net`,
`engine/sim/src/net_units.cpp`, `client/src/net_menu.cpp`, `ta_compat/src/client_net_screens.cpp`).
Engine smoke tools: `net_tests`, `net_play` (loop / duel / features / tcp / udp), `dplay_play`
(DirectPlay wire protocol, also against a real `TotalA.exe` under the capture tool's offscreen
mode), harnesses `net_harness`, `netstate_harness`, `feature_harness`.

Counts over the 85 rows below (2026-10-20). On master 14456bf: 35 BUILT+VERIFIED, 24
BUILT-UNVERIFIED, 9 PARTIAL, 13 MISSING, 3 NOT-APPLICABLE. Now (recounted 2026-10-20 after the
leave-path round; 5 rows added by the message round: 90 rows, 41 BUILT+VERIFIED, 41 BUILT-UNVERIFIED): 40 BUILT+VERIFIED, 37 BUILT-UNVERIFIED, 4 PARTIAL, 1 MISSING, 3 NOT-APPLICABLE (rows changed this round are marked
*2026-10-20*; the feature rows of section 1 count as changed: master had no host role, broadcast
requests, no comparison tool and no regression test).

## 1. User bug: "some explosions exploded differently and caused features to desync"

| Item | Status | Evidence / notes |
|---|---|---|
| Feature damage decided by the host only (`0x4244B0`: the viewed player's side record +0x97 bit 0; a non-host sends {0x0F, weapon, x, z} to the host and changes nothing) | BUILT+VERIFIED *2026-10-20* | cause of the bug: the battle never got its network role, so each machine applied only its own shots' damage. `sim_tests` `test_feature_damage_network_roles`; `net_play features` 900 steps (final maps identical, 14 of 16 comparisons identical, the others a destruction in flight), 3 machines identical; with the original hosting and our client shooting (offscreen capture run, 474 0x0F packets): 18,921 cells identical |
| Request addressed to the host only (`0x450030` first record with the host flag, −1 none; `0x451BC0` checks) | BUILT+VERIFIED | same runs; before, requests went to everybody and 3+ machines forwarded them again |
| Host announces destroy 0xFD / ignite 0xFE; receivers destroy / ignite remote (no spread, no countdown) | BUILT+VERIFIED | `feature_harness` (both roles), runs above |
| Reclaim / resurrection 0xFF from the owner | BUILT+VERIFIED | `feature_harness`; net_play features (commanders reclaim) |
| Explosions where the original's model puts them: a remote shot is the receiver's own copy launched from the fire packet's start and aim (`0x49D270`), flown and hit-tested locally; detonate packet 0x0E (`0x49AF90`) only for interceptors; a dying unit explodes at its local copy (`0x49B000`); damage only on the owner's machine | BUILT+VERIFIED | `projectile_harness` (handlers); `0x49D270` re-read 2026-10-20 against `Battle::apply_fire_packet` (routine picked by the packet's weapon, the slot's weapon fired, interceptor search). Explosions are not synchronised in the original either; with the host deciding features they cannot change features differently |
| Two-machine feature-map comparison (every N steps, dumps per machine) | BUILT+VERIFIED | `net_play features [steps] [seed] [map] [every]`, `NET_PLAY_FEATURE_DUMP`, `NET_PLAY_MACHINES`; `dplay_play --features --dump-features`; capture `dump_features` (the original's map) |
| Regression tests | BUILT+VERIFIED | ctest `net_features_match`; `sim_tests` |
| Races the original allows (late requests, destroy meeting a still-burning copy, wreck cells from trailing copies, reproduction drawing each machine's generator, console commands) | BUILT-UNVERIFIED | kept as the original has them (RE_MEMORY "Features across the machines") |
| Our host + original joining with shots fired | PARTIAL | the run made had its shooters out of range (no shots): to redo |

## 2. Packets (type, size table `0x451FD0`, phases `0x512BC0`; dispatch `0x453D40`)

| Type | Original handler | Status | Notes |
|---|---|---|---|
| table of sizes/phases, frame split, size-0 hang | `0x451FD0`, `0x463790` | BUILT+VERIFIED | net_harness, net_tests |
| sender checks (known, present remote; addressee in the game) | `0x453D40` top | BUILT-UNVERIFIED *2026-10-20* | a known sender that is not a present remote player goes to `0x453010(sender, 6)` (a local one is ignored): an inactive record only gets the leaving byte, one with state 10 is sent away (status 6 to everybody); an unknown id sends nothing (the "unknown" status never reaches the sender); the addressee check comes after. net_tests (both cases). Not reproduced: for an unknown sender the original reads a "record 10" past the table (GS+0x2851), an input |
| 0x02 ping (echo with time, latency) | `0x4565A0` | BUILT-UNVERIFIED | |
| 0x03, 0x04, 0x25, 0x2B | no case | NOT-APPLICABLE | the original ignores them too |
| 0x05 chat (shown when the addressee is human) | `0x463CA0` | BUILT+VERIFIED | with the original both ways (room and battle) |
| 0x06 probe → 0x07 from the first local playing record to the session's player | case 4 | BUILT-UNVERIFIED | |
| 0x07 probe answer: sender record +0x21 bit 0 | case 5 | BUILT-UNVERIFIED *2026-10-20* | was MISSING. The bit is set and never read in the original (no reader at GS+0x1B81..+0x1B87 or through a record pointer); the room and the battle keep it per record, not cleared on free (`probe_answered`). net_tests: the room's periodic probes, a battle probe |
| 0x08 game start (sets GS+0x2A44 bit 2, ends the loop) | case 6 | BUILT+VERIFIED | with the original |
| 0x09 unit create | `0x4861D0` | BUILT+VERIFIED | with the original; netstate_harness |
| 0x0A transport | `0x4876..` | BUILT-UNVERIFIED | orders_harness for the routine |
| 0x0B damage, 0x0C killed | `Unit_ApplyDamagePacket`, `Unit_HandleKilledPacket` | BUILT+VERIFIED | with the original (net_play duel, dplay runs) |
| 0x0D fire | `0x49D270` | BUILT+VERIFIED | projectile_harness for the fire routines; handler re-read |
| 0x0E detonate | `0x49AF90` | BUILT+VERIFIED | projectile_harness |
| 0x0F feature | case 0x0D | BUILT+VERIFIED | section 1 |
| 0x10 script call, 0x11 activation, 0x12 finished | | BUILT+VERIFIED | with the original (dplay runs) |
| 0x13 sound | | BUILT-UNVERIFIED | |
| 0x14 ownership | `Unit_TransferOwnership` | BUILT-UNVERIFIED | |
| 0x15 loaded (counted only after the own loading) | case 0x13 | BUILT+VERIFIED | with the original |
| 0x16 share energy / metal / explored map | `0x456EE0` / `0x457050` / `0x4571C0` | BUILT-UNVERIFIED | economy harness covers the transfer routines |
| 0x17 / 0x18 logos | `0x4526C0` / `0x452570` / `0x4523E0` | BUILT+VERIFIED | with the original (logo 0xFF crash found) |
| 0x19 pause / speed | case 0x17 | BUILT-UNVERIFIED | |
| 0x1A unit definition exchange | `Sync_*` | BUILT+VERIFIED | with the original; gui_harness |
| 0x1B status (player leaving, `0x453010`) | case 0x19 | BUILT-UNVERIFIED *2026-10-20* | received or sent (time-out Reject, sender check, session "player destroyed"): `NetBattle::send_away` — the status from the first local record (refused once it is leaving); this machine's human: a status per local player (only the first goes out) and `0x452CC0` for the human each time (its computer players' teams are not killed; was: all of them killed and their session players destroyed); a remote human: every record of its group; a computer player: itself. Status 7 ("You need a unit you don't have"): the original never sends it (no caller passes 7, no other writer of +0x22), nor does the engine; a received 7 shows the text (`out_text`). net_tests |
| 0x1C disconnect ("Player %s has disconnected") | case 0x1A | BUILT-UNVERIFIED *2026-10-20* | the line (session name, kind 4, `on_disconnected`); `0x452CC0` for it when remote or not loading; passed on once from the local player; naming the local human sets GS+0x3923B bit 2 (`local_disconnected`; its use by the client not read). Was: only marked leaving. Nothing in the original starts a 0x1C (`0x452800`, the same body, has no caller); the engine sent one on a session "player destroyed": removed. net_tests *2026-10-20* (language): the format goes through the translation first (`0x454FD6`) |
| 0x1D frame timing (`0x461620`) | phase mask 0 | NOT-APPLICABLE | never accepted by the phase table |
| 0x1E / 0x1F start positions | cases 0x1C / 0x1D | BUILT+VERIFIED | with the original |
| 0x20 side record (found by owner id at +0x91) | case 0x1E | BUILT+VERIFIED | with the original |
| 0x21 / 0x22 groups, per-group sends `0x450980` | | BUILT-UNVERIFIED | net_tests |
| 0x23 alliance (`0x452960`; +0x113, +0x108 for computer players or forced) | | BUILT-UNVERIFIED *2026-10-20* | the forced word (bytes 10-13) now sent and honoured in the room |
| 0x24 ally team | case 0x22 | BUILT-UNVERIFIED | |
| 0x26 slot table | case 0x24 | BUILT+VERIFIED | with the original |
| 0x27 modified executable (12 lines) | case 0x25 | BUILT-UNVERIFIED | |
| 0x28 view state; freeze once the final totals arrived (+0x129); the ask byte answered | `0x457540` | BUILT-UNVERIFIED *2026-10-20* | the freeze was described as a shared-view test before (wrong); the ask path was missing |
| 0x29 final-totals acknowledgement (+0x11E, +0x134) | case 0x27 | BUILT-UNVERIFIED *2026-10-20* | was MISSING |
| 0x2A loading progress | case 0x28 | BUILT+VERIFIED | with the original |
| 0x2C unit states | `0x48B920` | BUILT+VERIFIED | netstate_harness, with the original |

## 3. DirectPlay (our own implementation of its wire protocols)

| Item | Status | Notes |
|---|---|---|
| TCP/IP provider: enumeration (UDP/TCP 47624), framing, guaranteed / datagram messages | BUILT+VERIFIED | games with the original both ways |
| system messages the game handles: create player (3: closed 3, bad data 8, wrong password 4), delete (5), player data (0x102), names (0x103), session description (0x104) | BUILT-UNVERIFIED | create/delete seen with the original; password path not observed |
| session messages: enumerate/answer, player id, add-forward, super-enum player table, create/delete player, description change, ping | BUILT+VERIFIED | net_tests decode/encode the original's messages byte for byte |
| password-protected sessions against the original | PARTIAL | engine refuses with 0x24; the original's own handling not observed |
| 3+ machines with original programs: how the original's host announces a newcomer to the other machines | PARTIAL | engine sends a create of the newcomer's system player; the original's way not observed (needs two originals or an original host with two of ours: next) |
| serial provider (frame 0x2BAD, CRC-16/ARC) | BUILT+VERIFIED | frames checked against the system provider both ways |
| serial with the original: stayed at "Connecting" | PARTIAL *2026-10-20* | cause found (capture tool) and fixed; listed, joined, room, start; the battle not reached (the original exits while loading under the tool) |
| reliable packets (system messages 0x30 / 0x31) over serial | BUILT+VERIFIED *2026-10-20* | the original's join request reassembled and acknowledged; net_tests with its bytes |
| modem (Hayes driven by us; the original dials through TAPI) | BUILT-UNVERIFIED | emulated modems only |
| IPX | NOT-APPLICABLE | no provider on current Windows |

### Serial stall (2026-10-20): cause found and fixed; the original now lists, joins and starts over serial
- The game's enumeration (`0x4C9E50`) loops only while `EnumSessions` answers DPERR_CONNECTING;
  the "Connecting (ESC to abort)" box (`0x441460`) is up for the whole call for every provider but
  TCP/IP. The system's serial provider never answers CONNECTING (only its TAPI modem path does)
  and every port wait of it times out within 5 s.
- **Cause: the capture tool's port emulation, not the protocol.** The provider's reader thread
  waits in `WaitCommEvent` (mask EV_RXCHAR); our breakpoint stub answered at once and slept 3 ms
  *inside the debug loop* when nothing was waiting, which holds every thread of the game, at every
  pass of that reader: the game hardly ran (one enumeration in 20 s, ESC unread). Now the stub
  turns an empty wait into `Sleep(20)` inside the game (stack rewritten to return to the caller).
  Result: our serial session listed by the original (name, map, settings), as with TCP/IP.
- **Then found: reliable packets.** Over serial the library sends guaranteed system messages as
  reliable pieces (system message 0x30: message id, piece index/size/offset/count, message size;
  each acknowledged by 0x31 with the id and index, resent until then). The original's join (its
  player id request) came so and our host ignored it; the engine now reassembles, acknowledges
  and handles them (`net_tests`: the original's own bytes). Result: the original joined our serial
  host, its room showed both, ready, start; it then exited (code −1, no exception) while loading —
  as 2 of 5 runs of the capture tool did at start-up this session, so taken as the tool's
  environment, not resolved. Log: `fixtures/dplay/serial_original_join.log`.
- Not done: the original hosting over serial (our client joining), our own guaranteed messages
  sent as reliable pieces (the original took them plain so far).

## 4. Room (LOUNGE2), session and lobby rules

| Item | Status | Notes |
|---|---|---|
| room records, ready, map, options, start condition `0x456760` | BUILT+VERIFIED | with the original both ways |
| room drawn 1:1 | BUILT+VERIFIED (CLIENT/FRONTEND) | gui_harness comparisons |
| connection screen (SELPROV): the online.dll link buttons — "www.cavedog.com", "Unit Download" and one per service extension DLL (`ta*.dll`: Mplayer, DWANGO, HEAT, Kali, TEN, Wireplay), clones of the hidden SERVICEX gadget (`0x444580` → `0x45B490`, `0x44475D`; pressed: `0x45B250`) | MISSING *2026-10-21* | found by the coverage sweep (COVERAGE.md item 1, with the implementation note); gui_harness `selprov` stubs `0x45B490`, so the compared screen had no links |
| row order: each machine shows its own player in row 0 | BUILT-UNVERIFIED *2026-10-20* | built in b0e1351 (`display_row`: the host its slot table, a joiner its own player first, then the others in slot order); the row was stale. No net_tests case (screen code) |
| a joining machine adding a computer player (PLAYER on a free or blocked row, `0x447B10`) | BUILT-UNVERIFIED *2026-10-20* | any machine adds one unless the game is closed ("Can't add another player when game is closed.", 500 wide), the commander mode is 0x1000, or `0x457B90` finds one this machine already runs (controller 2 only; was: any computer player anywhere, and the host only). A joiner asks the session for its player ("AI:" + its name, 16 characters, data byte 0x13 = 0x50); its side record goes out until the host seats it; it is the joiner's to run and to remove. net_tests loopback (seated at the same slot on both machines, removed again); not tried against the original |
| Exit to Windows in a network battle (`0x451B60`): `0x452CC0` for every player this machine runs (team killed; in the battle the record and session player stay), then the session closed (`0x46C190`), which destroys them for the others; the other machines, on the session's "player destroyed" (system message 5), send it away with reason 1 (`0x453010`: status 0x1B, `0x452CC0` for its whole machine group: teams killed, alliance bytes toward it cleared, records freed) and set its controller to 0 | BUILT-UNVERIFIED *2026-10-20* | loopback host + join: the host exits to Windows from the exit menu, the joiner frees both host-side records (human and computer player) with their units. Corrected 2026-10-20: no 0x1C is sent for it (`0x4550D5` is in the 0x1C receiver), the other leave paths (0x1B) now kill teams and free records as the original does (the "Open" note is closed); net_tests: session destroy. Not done: GS+0x2BEE bit 0 and the host's session-description refresh after it |
| alliances of the room carried into the battle (team rule `0x446C70` every room frame, forced breaks `0x446E90`, `0x452960`) | BUILT-UNVERIFIED *2026-10-20* | was MISSING (the battle derived alliances from teams only). net_tests: team rule, a hand-made alliance, a forced break, the battle start's table |
| ALLY toggle on a teammate leaves the team | BUILT-UNVERIFIED *2026-10-20* | `0x447B10` "ALLY%d" re-read: the leaving record is the local one (`0x2A42`); after the toggle (`0x452960`) a shared team other than 5 runs `0x446E90` (every alliance in it broken, forced), team 5, `0x452BD0` (0x24); the sound and line read the byte afterwards, so "broke alliance with" and Multi. Was: alliances broken unforced, the team kept. Same fix for TEAMICONS (no unforced pass before the room's forced one). Room tests and battle_menus_check unchanged; the lounge press itself not driven |
| restrictions dialog | BUILT+VERIFIED | gui_harness pixel comparison |
| restrictions Load List / Save List files | BUILT-UNVERIFIED *2026-10-20* | `SAVEGAME\<name>.LST` (int32 table size − 1, then key + count per listed definition in index order; `0x44B230` / `0x44B140`), LOADLIST.GUI / SAVELIST.GUI dialogs (`0x44BC10` / `0x44B990`, handlers `0x44B3C0` / `0x44B690`), "There are no saved lists to choose from". compat_tests format round trip; headless ta_play save → load (2116-byte file for 279 definitions, counts back in the rows). Dialog pixels not compared with the original (gui_harness has no LOADLIST / SAVELIST command); see RE_MEMORY "Restriction list files" |
| join gate (closed / started → status 6) | BUILT-UNVERIFIED | |
| machine groups and per-group sends | BUILT-UNVERIFIED | |
| unit definition agreement | BUILT+VERIFIED | |
| computer players' session names ("AI:" + host name) | BUILT+VERIFIED | |
| host's session or player not made (New game screen → `0x426E80` state 0x10 sub-state 0x11) | BUILT-UNVERIFIED *2026-10-20* | `0x451540` sets the host flag (side record +0x97 bit 0) and the player count 0 and opens the session without looking at the result; `0x451220` fails, sets the record back to controller 0 and pushes MSGBOX (500 wide, not modal); `0x450A10` is skipped and the state goes to 0x11: LOUNGE2 opens over the message box (`0x449BB0`) with no record in use (no player rows), this machine holding the host's controls, the list's first map. The text: the first record 0-9 with a controller and the host flag (this machine's no longer counts after the reset), else record 10 (cleared at `0x41D920`, never seated) — re-create only when that record is in use with controller 1/2, so with no records left from an earlier session it is "... re-join the game session." also on the host (was: the re-create text and no room). The client runs that room without a session (`enter_broken_room`, `make_broken_lounge`); a joining machine's failure still ends with the message (`0x4517B0` → the list again) |
| game list: "Updating..." (TCP/IP) / "Connecting  (ESC to abort)" box while the games are asked for (`0x441460`: 150, no OK, fitted); a failed enumeration (unknown address, Escape) → the list closed and "Invalid TCP/IP Address" (`0x443CB0`, 200), seen two passes before the new connection screen covers it; UPDATE's failure leaves the rows | BUILT+VERIFIED *2026-10-20* | was not shown (the client blocked silently). Boxes: gui_harness `msgbox:150:0:1:Updating...` / `msgbox:200:Invalid TCP/IP Address` vs ta_play: the boxes identical (Updating: the whole frame differs only in rows 100–144; Invalid: 0 of 14,365 box pixels; the frames differ only in the provider rows the harness has none of). ta_play TCP/IP to 999.1.1.1: Updating box 2 frames, Invalid box frames 47–48, provider list at 49 |
| room MAP with no map (`0x444EA0`: map name empty or none listed) → "There are no multiplayer maps to choose from" (320) | BUILT-UNVERIFIED *2026-10-20* | was missing; the box is the verified NetMessage |
| menu-loop message width (`0x425730` buffer, shown by `0x426E80`) | BUILT-UNVERIFIED *2026-10-20* | text width + 20 by `0x4A5030`: the GUI's picture font (GUI +0x14 = hattfont12, set at start-up `0x491200`; only `0x4689C0` in battles changes it), the sum of its frame widths over every byte (control characters too); the client measured with the same font but stopped at a line break and skipped control characters |
| the client library's own connection / room screens (`client/src/net_menu.cpp` ConnectScreen, RoomScreen, `make_multiplayer_menu`, `make_room_screen`, `install_multiplayer`) | REMOVED *2026-10-20* | never installed (the main menu's hook is `ta_compat` `install_net_screens`); they held invented texts and side names |

## 5. Battle network behaviour

| Item | Status | Notes |
|---|---|---|
| who simulates what (own players; remote scripts only) | BUILT+VERIFIED | |
| unit-state scheduling (rolling whole state, goal states) | BUILT+VERIFIED | netstate_harness |
| frames, channels, spreading, out-of-order frames | BUILT+VERIFIED | net_harness, net_tests |
| envelope, compression | BUILT+VERIFIED | net_harness |
| damage, kills, wrecks | BUILT+VERIFIED | duel runs, with the original |
| feature changes | BUILT+VERIFIED | section 1 |
| resource sharing (Share screen's network side) | BUILT-UNVERIFIED | |
| alliances in battle (Allies screen's network side) | BUILT-UNVERIFIED | |
| watch mode | BUILT-UNVERIFIED | *2026-10-20*: the question "You're out!  Continue Watching?" (no computer player fighting) and the 500-wide box for a host of live computer players (`0x464F80`, `0x465803`; CLIENT 2.53) |
| out-of-units line of a network game (`0x450380`: name + rand() & 7 text) | BUILT-UNVERIFIED *2026-10-20* | CLIENT 2.52 |
| sent away during the battle: the reason's box at the end (`0x41F7F0`) | BUILT-UNVERIFIED *2026-10-20* | CLIENT 2.54 |
| packet 0x27: "<name> has modified his executable.  Game integrity breached." 12 console lines (`0x45530C`) | BUILT-UNVERIFIED *2026-10-20* | received only (TotalA.exe never sends 0x27; neither does the client) |
| pause / speed | BUILT-UNVERIFIED | |
| chat recipients (`0x453360`) | BUILT+VERIFIED | two-machine menus run |
| players leaving / host leaving: a leaving host's flag passes to the record (controller 1 or 3) with the highest net id (`0x452CC0`) | BUILT-UNVERIFIED *2026-10-20* | was "no one decides features after": the new flag makes that machine decide feature damage itself; others still address the old host where it comes first (`0x450030`), as the original; machines can disagree (each sees its own computer players as controller 2). Side records (0x20) now carry the host flag into the battle. net_tests. Open: the kill routine's stale word for a remote team killed here (`Battle::kill_team` records a fault; the word is the network code's stack, an input) |
| time-outs (`0x453C20`) | BUILT-UNVERIFIED | |
| in-battle time-out dialog (TIMEOUT.GUI) | BUILT-UNVERIFIED | re-checked 2026-10-20: built (hooks.timeout_dialog, client_screens.cpp); the row was stale |
| end-of-game statistics exchange (`0x4572A0`) | BUILT-UNVERIFIED *2026-10-20* | was MISSING; net_play smoke: settles in 5 passes |
| view states to remote humans only (`0x4573D0`) | BUILT-UNVERIFIED *2026-10-20* | went to every remote record |
| Control screen (ownership) network side | BUILT-UNVERIFIED | |

## Remaining, in order
1. (done 2026-10-20: row order, a joiner's computer player.)
2. Serial with the original into the battle (the original exits while loading under the capture tool); the original hosting over serial.
3. (done 2026-10-20: Restrictions Load/Save List files; status 7 — the original never sends it.)
4. Our host + the original joining with shots fired (feature maps); 3+ machines with the original; passwords against the original.
5. Leave paths (*2026-10-20*, the rest): the kill routine's stale word when a remote team is killed by `0x452CC0` (network code stack, an input: `kill_team` records a fault); the "record 10" read for an unknown sender (GS+0x2851); after a session "player destroyed" GS+0x2BEE bit 0 and the host's session-description refresh (`0x451090` / `0x4C9890`); what the client does with GS+0x3923B bit 2 (`local_disconnected`).
