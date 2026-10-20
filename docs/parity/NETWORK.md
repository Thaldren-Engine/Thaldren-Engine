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

Counts over the 84 rows below (2026-10-20). On master 14456bf: 35 BUILT+VERIFIED, 24
BUILT-UNVERIFIED, 9 PARTIAL, 13 MISSING, 3 NOT-APPLICABLE. Now: 40 BUILT+VERIFIED, 30
BUILT-UNVERIFIED, 5 PARTIAL, 6 MISSING, 3 NOT-APPLICABLE (rows changed this round are marked
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
| sender checks (known, present remote, addressee playing; "unknown" status 6) | `0x453D40` top | BUILT-UNVERIFIED | engine counts it refused; status 6 to the sender not sent in battle |
| 0x02 ping (echo with time, latency) | `0x4565A0` | BUILT-UNVERIFIED | |
| 0x03, 0x04, 0x25, 0x2B | no case | NOT-APPLICABLE | the original ignores them too |
| 0x05 chat (shown when the addressee is human) | `0x463CA0` | BUILT+VERIFIED | with the original both ways (room and battle) |
| 0x06 probe → 0x07 from the first local playing record to the session's player | case 4 | BUILT-UNVERIFIED | |
| 0x07 probe answer: sender record +0x21 bit 0 | case 5 | MISSING | nothing reads it in the engine; the byte's reader not found yet |
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
| 0x1B status (player leaving, `0x453010`) | case 0x19 | BUILT-UNVERIFIED *2026-10-20* | received or sent (time-out Reject): `NetBattle::send_away` — once per player: status relayed to everybody, this machine's human takes its machine's players with it, a remote human its machine group, a computer player itself; each removed by `0x452CC0` (team killed, alliance bytes toward it cleared, record freed; this machine's session players destroyed). Open: the per-reason texts (status 7 "You need a unit you don't have") |
| 0x1C disconnect ("Player %s has disconnected") | | BUILT-UNVERIFIED | |
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
| row order: each machine shows its own player in row 0 | MISSING | engine rooms use the host's slot order |
| a joining machine adding a computer player (PLAYER on a free or blocked row, `0x447B10`) | BUILT-UNVERIFIED *2026-10-20* | any machine adds one unless the game is closed ("Can't add another player when game is closed.", 500 wide), the commander mode is 0x1000, or `0x457B90` finds one this machine already runs (controller 2 only; was: any computer player anywhere, and the host only). A joiner asks the session for its player ("AI:" + its name, 16 characters, data byte 0x13 = 0x50); its side record goes out until the host seats it; it is the joiner's to run and to remove. net_tests loopback (seated at the same slot on both machines, removed again); not tried against the original |
| Exit to Windows in a network battle (`0x451B60`): every player this machine runs has its team killed and its session player destroyed; the other machines, on the session's "player destroyed" (system message 5, `0x4550D5`), run `0x452CC0` for it (team killed there, their alliance bytes toward it cleared, the record freed) and send "has disconnected" (0x1C) | BUILT-UNVERIFIED *2026-10-20* | loopback host + join: the host exits to Windows from the exit menu, the joiner frees both host-side records (human and computer player) with their units; was: ignored until the time-out. Open: the other leave paths of the original (status 0x1B, `0x453010`) also kill the leaver's team, the engine still only marks those players as leaving |
| alliances of the room carried into the battle (team rule `0x446C70` every room frame, forced breaks `0x446E90`, `0x452960`) | BUILT-UNVERIFIED *2026-10-20* | was MISSING (the battle derived alliances from teams only). net_tests: team rule, a hand-made alliance, a forced break, the battle start's table |
| ALLY toggle on a teammate leaves the team | BUILT-UNVERIFIED *2026-10-20* | `0x447B10` "ALLY%d" re-read: the leaving record is the local one (`0x2A42`); after the toggle (`0x452960`) a shared team other than 5 runs `0x446E90` (every alliance in it broken, forced), team 5, `0x452BD0` (0x24); the sound and line read the byte afterwards, so "broke alliance with" and Multi. Was: alliances broken unforced, the team kept. Same fix for TEAMICONS (no unforced pass before the room's forced one). Room tests and battle_menus_check unchanged; the lounge press itself not driven |
| restrictions dialog | BUILT+VERIFIED | gui_harness pixel comparison |
| restrictions Load List / Save List files | MISSING | file format not read yet |
| join gate (closed / started → status 6) | BUILT-UNVERIFIED | |
| machine groups and per-group sends | BUILT-UNVERIFIED | |
| unit definition agreement | BUILT+VERIFIED | |
| computer players' session names ("AI:" + host name) | BUILT+VERIFIED | |

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
| watch mode | BUILT-UNVERIFIED | |
| pause / speed | BUILT-UNVERIFIED | |
| chat recipients (`0x453360`) | BUILT+VERIFIED | two-machine menus run |
| players leaving / host leaving (no one decides features after) | BUILT-UNVERIFIED | |
| time-outs (`0x453C20`) | BUILT-UNVERIFIED | |
| in-battle time-out dialog (TIMEOUT.GUI) | BUILT-UNVERIFIED | re-checked 2026-10-20: built (hooks.timeout_dialog, client_screens.cpp); the row was stale |
| end-of-game statistics exchange (`0x4572A0`) | BUILT-UNVERIFIED *2026-10-20* | was MISSING; net_play smoke: settles in 5 passes |
| view states to remote humans only (`0x4573D0`) | BUILT-UNVERIFIED *2026-10-20* | went to every remote record |
| Control screen (ownership) network side | BUILT-UNVERIFIED | |

## Remaining, in order
1. (done 2026-10-20: row order, a joiner's computer player.)
2. Serial with the original into the battle (the original exits while loading under the capture tool); the original hosting over serial.
3. Restrictions Load/Save List files; status 7.
4. Our host + the original joining with shots fired (feature maps); 3+ machines with the original; passwords against the original.
