# Parity checklist: SIM (the simulation and the game data it uses)

Audit of everything the original TotalA.exe (3.1) does in the simulation area against the engine,
2026-10-20, base master 14456bf. Statuses:

- **VERIFIED**: built and checked against the original (harness calling the original's own routine in a
  suspended process, live-table dump, or whole-game replay). The evidence column names it; details in
  `docs/RE_MEMORY.md` under the section given.
- **BUILT**: built from the disassembly, not differentially tested.
- **PARTIAL**: built with a known difference.
- **MISSING**: the original has it, the engine does not.
- **N/A**: not simulation, dead code in the original, or an input by design (reason given).

## How the audit was made
1. **Call graph sweep.** Every function reachable from the battle step (`0x495490`), the 68 mission
   handlers and unit creation (`0x485F50`) through Ghidra's callee lists: 915 functions. 581 are named
   by address in the engine, the importer or RE_MEMORY. The 334 others were each looked at: helpers
   inlined into verified routines (vector/allocator code, the mission table's sort, Tdf/Hapi file
   access, the bit-stream and DirectPlay library above `0x4B8000`), interface code reached through
   shared routines (selection cycling `0x48D630`/`0x48D790`, the game-menu gadgets `0x4A....`, the GUI
   stat bars `0x4B0...`), the lobby and network code (`0x446FB0`, `0x461xxx`-`0x462xxx`, `0x46C620`
   onwards: NETWORK area) and the effect pool's helpers (`0x470EB0`, `0x4732E0`). None is a
   simulation routine the engine lacks.
2. **Every random-number consumer.** All callers of the battle generator `Math_RandMod` `0x4B6C30`
   (55 functions, 121 call sites) and of the C runtime `rand()` `0x4E4870` (26 functions, 59 call
   sites) checked by address: every one is built or classified (RE_MEMORY "Simulation completeness")
   except `0x40AD20`, which has **no callers** (dead code; the live copy is `0x40B2C0`).
3. **Every data key** read by the unit (`0x42A8D0`, `0x42BF40`), weapon (`0x42E440`), feature
   (`0x4224B0`), map-info (`0x435DA0`, `0x436860`, `0x436C30`), goal (`0x48E010`), movement-class,
   sidedata and AI-profile loaders, against the importers.
4. **Every unit-script entry point** the original calls by name (strings near `0x5050xx`, `0x508Dxx`,
   `0x5012xx`) and every opcode / unit value.
5. **Every open item** in RE_MEMORY and SIM_INVENTORY in this area, and every console command with a
   simulation effect (the command table at `0x501D38` / `0x501F48` / `0x501FD0`).

## Summary
| Status | Before (2026-10-20 audit) | After this round |
|---|---|---|
| VERIFIED | 118 | 118 |
| BUILT | 14 | 14 |
| PARTIAL | 3 | 3 |
| MISSING | 7 | 7 |
| N/A | 16 | 16 |

(Counts are rows of the tables below. "After" is updated as items land; see the change log at the end.)

---

## 1. Game data keys

### 1.1 Unit definitions (`units\*.FBI`, `[UNITINFO]`)
Importer `ta_compat/src/unitdefs.cpp`; `unitdef_check` compares every UnitDef field of all 279 stock
units with a dump of the live game's table (24,780 fields, 0 differences, RE_MEMORY "Unit
definitions"). The simulation receives the whole flag words and the numeric fields
(`ta_compat/src/battle_types.cpp`), and reads each bit where the original routine reads it; those
routines are differential-tested with real definitions (column "consumer").

| Keys | Status | Consumer / evidence |
|---|---|---|
| unitname, name, description, side, objectname, version, copyright, norestrict, wacky, ai_weight, ai_limit, buildcostenergy, buildcostmetal | VERIFIED | enumeration `0x42A8D0` (unitdef_check); ai_weight/ai_limit run by the profile loader (ai_harness) |
| maxdamage, healtime, damagemodifier, hidedamage | VERIFIED | combat_harness (damage, healing); hidedamage is interface only |
| maxvelocity, acceleration, brakerate, turnrate, moverate1, moverate2, maxslope, maxwaterdepth, minwaterdepth, maxwaterslope, footprintx, footprintz, movementclass, upright, floater, canhover, amphibious, waterline, canfly, cruisealt, bankscale, pitchscale, attackrunlength, maneuverleashlength, hoverattack, isairbase | VERIFIED | motion_harness, route_harness, orders_harness (aircraft), unitloop_harness |
| builder, workertime, builddistance, buildtime, canresurrect, canreclamate, cancapture, canrepair (via canresurrect), canguard, canpatrol, canstop, canmove, canattack, canload, candgun, bmcode, yardmap, buildangle, activatewhenbuilt, init_cloaked, onoffable, cloakcost, cloakcostmoving, mincloakdistance, stealth | VERIFIED | orders_harness (construction), create_harness (yards, activation at creation), economy_harness (cloak), los_harness (stealth); candgun is the interface's button only |
| energymake, energyuse, metalmake, makesmetal, extractsmetal, energystorage, metalstorage, windgenerator, tidalgenerator | VERIFIED | economy_harness |
| sightdistance, radardistance, sonardistance, radardistancejam, sonardistancejam, istargetingupgrade | VERIFIED | los_harness, targeting_harness (targeting facility flag) |
| weapon1-3, explodeas, selfdestructas, selfdestructcountdown, kamikaze, kamikazedistance, shootme, wpri/wsec/wspe_badtargetcategory, nochasecategory, category, firestandorders, standingfireorder, mobilestandorders, standingmoveorder, defaultmissiontype, antiweapons, commander, immunetoparalyzer, cantbetransported, transportsize, transportcapacity, teleporter, isfeature, corpse, soundcategory | VERIFIED | weapons_harness, targeting_harness, combat_harness (death explosions, self-destruct), orders_harness (kamikaze, transports, teleport), create_harness (standing orders), feature_harness (corpse/wrecks) |
| showplayername, sortbias, noshadow, zbuffer, digger, downloadable (menu), armoredstate (script value 20 path verified) | N/A (interface / drawing; the simulation-relevant halves verified above) | CLIENT / RENDERER areas |
| Build options (sidedata `[CANBUILD]`, download menus `0x42DCF0`) | VERIFIED | unitdef_check (shared scratch list quirk) |
| Sort order of the table (`0x42D2E0`) with **duplicate unit names** | PARTIAL → see 7.2 | the original sorts with the 1998 STL sort (unstable); the engine used a stable sort (identical for the stock data, which has no duplicates) |

### 1.2 Weapon definitions (`weapons\*.tdf`)
Importer `ta_compat/src/weapondefs.cpp`; `weapondef_check`: all 256 slots identical to the live game
(198 loaded, 9,141 fields).

| Keys | Status | Consumer / evidence |
|---|---|---|
| ID, name, range, reloadtime, weaponvelocity, startvelocity, weaponacceleration, turnrate, weapontimer, flighttime, burst, burstrate, sprayangle, accuracy, tolerance, pitchtolerance, minbarrelangle, energypershot, metalpershot, areaofeffect, edgeeffectiveness, `[DAMAGE]` default and per-unit keys | VERIFIED | weapons_harness (slot pass, aim), ballistic_check (pitch solver, 1.4 M calls), projectile_harness, combat_harness (area damage, edge effectiveness) |
| lineofsight, ballistic, shellweapon, beamweapon, vlaunch, meteor, noradar, paralyzer, dropped, tracks, guidance, unitsonly, groundbounce, waterweapon, toairweapon, turret, selfprop, noexplode, burnblow, twophase, cruise, commandfire, noautorange, stockpile, targetable, interceptor, coverage, propeller | VERIFIED | weapons_harness, projectile_harness, targeting_harness (interceptor search 2,248 calls), the fire routines (fire-routine smoke) |
| firestarter, duration, randomdecay, holdtime | VERIFIED | feature_harness (fires), projectile_harness |
| shakemagnitude, shakeduration | VERIFIED | flow_harness (shake add/decay draws rand()) |
| smoketrail, startsmoke, endsmoke, smokedelay, soundtrigger | VERIFIED | fx_harness / effect lists (smoke emitters draw rand()) |
| rendertype, color, color2, model, explosiongaf/explosionart, water*/lava* explosion keys, soundstart, soundhit, soundwater | N/A for the simulation (drawing and sound); loaded and checked by weapondef_check | RENDERER / CLIENT |

### 1.3 Feature definitions (`features\**\*.tdf`)
Importer `ta_compat/src/featuredefs.cpp`; `feature_check`: 434 features, 8,307 fields identical.

| Keys | Status | Evidence |
|---|---|---|
| footprintx/z, height, blocking, flamable, burnweapon, sparktime, spreadchance, damage, indestructible, reclaimable, autoreclaimable, metal, energy, reproduce, reproducearea, geothermal, featuredead, featureburnt, featurereclamate, object, filename, seq* (animation lengths drive burn/death timing), animating, nodrawundergray | VERIFIED | feature_harness (1,128 feature stages, 206 ignites, 399 weapon damages, fires spreading), whole-game replay |
| description, nodisplayinfo, animtrans, shadtrans | N/A (interface / drawing) | |

### 1.4 Map info (`maps\*.ota`), movement classes, sides, AI profiles
| Item | Status | Evidence |
|---|---|---|
| GlobalHeader wind, tidalstrength, gravity, lavaworld, nosealeveltrigger, waterdoesdamage, waterdamage, SurfaceMetal, schema choice (`0x436860`) | VERIFIED (gravity, wind, schema with whole-game replay s2/g8/g9); BUILT (lava, water damage: read, used by verified routines, no captured map uses them) | RE_MEMORY "Simulation completeness", "Whole-game replay" |
| Schema start positions, meteor keys, features of the schema, aiprofile | VERIFIED (start positions, meteors: flow_harness; schema features: feature_check) | |
| Campaign header and goal keys (`0x435DA0`, `0x48E010`), placed units (`0x436C30`) | BUILT (read; goal behaviour and initial orders VERIFIED by goal_harness / order_harness) | RE_MEMORY "Campaign" |
| Placed units' BuildPriority, CreationCountdown, MissionCriticalUnit, AiIgnore, AiPriorityTarget, InitialGroup | N/A: the original stores them in the schema record (`0x436C30`) but its placement `0x488310` copies only Immunity, health, facing; no other reader | decompile of `0x488310` |
| killmul, timemul | N/A for the simulation (score table: CLIENT, built in `client_menus.cpp`) | |
| `gamedata\moveinfo.tdf` CLASS0-31 | VERIFIED | route_harness uses the real 32 classes |
| `gamedata\sidedata.tdf` commander, CANBUILD | VERIFIED | unitdef_check, whole-game replay |
| AI profiles (`ai\*.txt`, plan/weight/limit) | VERIFIED | ai_harness (858 profile command lines through the original dispatcher) |
| `gamedata\sound.tdf` (soundcategory sections) | N/A (sound: CLIENT) | |

## 2. Unit scripts (COB)
| Item | Status | Evidence |
|---|---|---|
| Every opcode (move, turn, spin, stop-spin, show/hide, cache, explode (legacy and new), play-sound, move/turn-now, shade, emit-sfx, waits, sleep, stack ops, arithmetic, rand, get/set value, get transporting/carrier, comparisons, logic, start/call script, jumps, return, signal, set-signal-mask, attach/drop) | VERIFIED | cob_harness: 278 scripts × 3 seeds (580,539 operations) + 3,000 random programs, 0 differences |
| Unit values GET 1-20 and SET 1, 5, 6, 18, 19, 20 | VERIFIED | unitvalue_harness: 116,867 GET, 33,341 SET |
| Piece pose, piece positions | VERIFIED | pose_harness |
| Callbacks the original calls: Create, Killed, Activate, Deactivate, StartBuilding, StopBuilding, QueryBuildInfo, QueryNanoPiece, QueryLandingPad, QueryTransport, BeginTransport, EndTransport, TransportPickup, TransportDrop, StartMoving, StopMoving, MoveRate1/2/3, setSFXoccupy, SetSpeed, SetDirection, SetMaxReloadTime, QueryPrimary/Secondary/Tertiary, AimFromPrimary/…, AimPrimary/…, FirePrimary/…, SweetSpot, TargetCleared, HitByWeapon, TakeDamage, RockUnit | VERIFIED | unitloop_harness, weapons_harness, orders_harness, combat_harness (each call site with real scripts) |
| MoveRate0, Go, Stop (as script names), Demo, SmokeUnit | N/A: not called by 3.1 (no such strings in the executable; SmokeUnit is a script-internal function) | strings sweep |
| EMIT_SFX gate (only units the viewed player sees) | VERIFIED | whole-game replay (follows `local_view`) |

## 3. The battle step
| Stage (`0x495490`) | Status | Evidence |
|---|---|---|
| 1 network input `0x453D40` | N/A here (NETWORK area) | |
| 2 unit pass `0x48AD30` (wind notice, weapon slots, scripts, timers, health %, water damage, healing, orders, motion, ground height) | VERIFIED | unitloop_harness 1,000 scenarios; whole-game replay |
| 3 projectiles `0x49B720` | VERIFIED | projectile_harness 809,096 updates |
| 4 world effects and debris `0x420F30` | VERIFIED | feature_harness / fx runs |
| 5 players' stage `0x464F80` (route search, AI, sensors, victory, share) | VERIFIED | route_harness, ai_harness, los_harness, whole-game replay |
| 6 features `0x424050` | VERIFIED | feature_harness |
| 7 texture cursors `0x415B30` | N/A (drawing) | |
| 8 wind `0x490C40` | VERIFIED | economy_harness (198 wind steps) |
| 9 meteors `0x437DE0` | VERIFIED | flow_harness |
| 10 camera shake decay `0x41CA10` (draws rand()) | VERIFIED | flow_harness |
| 11 effect lists `0x471EB0` | VERIFIED | effect-list harness run (3,226 operations) |
| 12 minimap blink `0x466580` | N/A (interface) | |
| 13 auto-share `0x457D30`, frame flush | VERIFIED (share: economy_harness); flush N/A (NETWORK) | |
| Frame tail: lingering sight `0x482130` | VERIFIED | los_harness |
| Step budget `0x495230` (speed, 5-step cap) | VERIFIED | whole-game replay frames |

## 4. Orders, missions and unit states
| Item | Status | Evidence |
|---|---|---|
| All 68 mission kinds (ground, building, VTOL, air) | VERIFIED (smoke) | orders_harness, motion_harness (RE_MEMORY "Mission behaviours", "Aircraft and transports") |
| Order lists, queueing, cancel, intent resolution (16 intents) | VERIFIED | orders_harness 366,066 operations; cmd_harness 9,957 orders |
| Fire / move standing orders, defaults from the FBI | VERIFIED | orders_harness, create_harness |
| Activation, on/off, cloak (economy pass decides), armoured state | VERIFIED | unitvalue_harness, economy_harness |
| Self-destruct countdown, kamikaze | VERIFIED | orders_harness / combat_harness |
| Transports (load, unload, packets) | VERIFIED | orders_harness (31 transport packets) |
| Construction, help, repair, reclaim, capture, resurrect | VERIFIED | orders_harness |
| Selection orders `0x48CF30` | VERIFIED | cmd_harness |
| Unit creation `0x485F50`, footprints, yards | VERIFIED | create_harness |
| Route search | VERIFIED | route_harness |
| Ground settle, motion | VERIFIED | orders_harness, motion_harness |

## 5. Combat, economy, sight, AI, victory
| Item | Status | Evidence |
|---|---|---|
| Damage, healing, death, wrecks, death explosions | VERIFIED | combat_harness 656,956 combat operations |
| Targeting, retargeting, interceptors | VERIFIED | targeting_harness |
| Weapon slot pass, aim, fire routines (turret, vlaunch, line of sight, dropped), stockpile, energy/metal per shot | VERIFIED | weapons_harness, fire smoke |
| Economy settlement, stalls, transfers, sharing, wind/tide, metal extraction | VERIFIED | economy_harness |
| Line of sight, radar, sonar, jammers, stealth, lingering sight, sight rebuilds | VERIFIED | los_harness |
| Computer player (roles, groups, scores, sites, profiles) | VERIFIED | ai_harness; whole-game replay with 3 hard computer players, 3,000 steps identical |
| Victory/defeat (skirmish, network) `0x464F80`, `0x490360` | BUILT | read and implemented; the defeat check's rand() draw included (RE_MEMORY "Victory") |
| Campaign goals | VERIFIED | goal_harness 4,800 operations |
| Campaign placed units and their first orders | VERIFIED (orders) / BUILT (placement) | order_harness |
| Features: damage, fire, spread, reproduce, wrecks, reclaim | VERIFIED | feature_harness |
| Features in network games (host decides damage) | BUILT on branch `worktree-agent-ac56a4e808c5568aa` (see 7.3) | feature_harness both roles smoke; net_play features |
| Debris, shards, world effects, nano streams | VERIFIED | feature_harness, effect-list harness |
| Meteors, wind, camera shake rand() | VERIFIED | flow_harness, economy_harness |
| Battle start (seeds, start positions, commanders, explosion tables) | VERIFIED | whole-game replay (battle generator identical at step 1) |

## 6. Random numbers
| Item | Status | Evidence |
|---|---|---|
| Battle generator `0x4B6C30` / seed `0x4B6CA0` | VERIFIED | cob_harness and every harness compare its state |
| 55 battle-generator callers | VERIFIED / BUILT (all built; `0x40AD20` is dead code) | sweep above |
| 26 C runtime rand() callers | VERIFIED for the simulation ones; frame-driven ones (lightning drawing, speech queue, CD audio) are inputs by design | RE_MEMORY "Simulation completeness" |

## 7. Items that were open in this round

### 7.1 Console commands with a simulation effect (debug list `0x501FD0`, bits 4)
Only reachable after the "+Now Film Chris Include Reload Assert" switch; each was registered but did
nothing ("not built" in `ta_compat/src/console_commands.cpp`).

| Command | Original | Status before | Status after |
|---|---|---|---|
| BurnAll | `0x416390` → `0x424840`: every map cell holding a feature id below 0xFFFB or a feature part (0xFFFE) has its feature removed as by `Feature_RemoveFromCell(cell, 1)` | MISSING | pending |
| BurnOne | `0x4163A0`: when the feature id under the pointer (GS+0x2CBC) is below 0xFFFB, the feature of the pointer's cell (GS+0x2C8E/0x2C90) is removed the same way | MISSING | pending |
| Feature `<name>` | `0x4163D0`: the first loaded feature type whose name matches (stricmp, `0x422DD0`; only types this battle loaded) is placed at the pointer's cell with owner nibble 10 | MISSING | pending |
| Assign `<mission> <a> <b>` | `0x416310`: mission kind by name (`MissionOrder_FindByName`); if found, the selection routine `0x48CF30` with no intent, that kind, no position, parameters atol(word 1) (the mission's own name, so normally 0) and atol(word 2) | MISSING | pending |
| Search `<n> [f]` | `0x416780`: route search object +0x48 (budget divisor) = n when n ≠ 0; with exactly three words +0x54 (base) = ftol(atof(word 2)) | MISSING | pending |
| Reload `<unit>` | `0x417490`: with a known type: every unit of it is killed through the kill packet (`0x486E80`, kind 8), then its FBI and script are read again (`0x42D1F0`, only when the type is "can be made") | MISSING | pending |
| ReloadAIProfiles | `0x4174D0` → `0x40A100`: for every active computer player (controller 2), the profile defaults of its table (`Ai_ResetProfile` `0x409470`, by slot), then the battle's whole profile load again (`0x4648E0`) | MISSING | pending |
| SeaLevel, TreeDeath, Kill, AI, Control, IWin, ILose, Move, default command | | VERIFIED / BUILT | unchanged |
| Edge, Film, PrintWeights, Save, MemDump, Mem, DPrint, Assert, DebugBreak | | N/A (view, files, saves: other areas) | |

### 7.2 Unit table order with duplicate unit names
`GameData_LoadAll` `0x42D2E0` compacts the compatible units (stable) and sorts records 1.. with the
1998 STL sort: quicksort loop `0x432D40` (partitions of more than 16, median of first / middle /
last, unguarded partition, recursion on the smaller side), then insertion sort `0x432FB0` of the
first 16 and unguarded linear insertion of the rest, all by `stricmp(unitname) < 0`. Equal names
therefore end in an order that depends on the whole table. Status before: PARTIAL (stable sort).
After: pending.

### 7.3 Feature consistency in network games (branch `worktree-agent-ac56a4e808c5568aa`)
Reviewed the simulation half of commit 66daf6b and the uncommitted test in its worktree:
`Battle::sync_settings` gives the feature system its role in network games from the viewed player's
side record (host flag), as `0x4244B0` reads it on every call (sync_settings runs at the start of
every battle entry point, so a change of viewed player is seen as in the original);
`sim::damage_feature` / `apply_feature_packet` (already on master) follow the original's host /
other-machine rules. No simulation change needed. The network half (addressing requests to the
host, `net_play features`, `dplay_play`, `ta_capture dump_features`) is the NETWORK agent's to
merge; status of this row: BUILT on that branch, merge owned by NETWORK.

### 7.4 Remaining PARTIAL / inputs (by design)
| Item | Status | Reason |
|---|---|---|
| Base-angle sync when a unit is drawn (`0x45AC20`, up to 7 angle units of difference later visible to the simulation) | PARTIAL | the original's drawing changes simulation state; the engine syncs only at simulation query points. Needs the camera as an input (which units were drawn each frame); not taken in yet |
| Lightning drawing's rand() draws, speech queue, CD audio | N/A (input) | frame/camera dependent in the original itself; whole-game replay feeds them |
| Stale stack words (AI site height, reclaim site y, air repair patrol site, kill word of "+kill") | N/A (input) | run-varying; replay feeds them |
| Wreck level of units without a Killed function | VERIFIED | "Stale stack word" |

## Change log
- 2026-10-20: audit written.
