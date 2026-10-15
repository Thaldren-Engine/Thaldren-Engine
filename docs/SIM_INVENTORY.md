# Simulation Inventory

Everything the engine simulation needs, gathered from `/Thaldren_TotalA.exe` on 2026-10-13.
**Status: reference, not yet verified.** Each item is re-checked when it is implemented. It only
goes into `/TotalA.exe` and the RE_MEMORY "Functions" table after the engine tests pass.
Addresses are identical in both Ghidra programs.

---

## 1. Main loop and timing

| Item | Address | Notes |
|---|---|---|
| `Game_MainLoopTick` | `0x00496790` | Per-frame driver. Calls budget, then step. |
| `SpeedCtrl_BudgetSimSteps` | `0x00495230` | steps = round(wallDtTicks × gameSpeed × 0.1 + fracAccum). Caps at **5 steps per frame**. Speed hysteresis: −1 speed after 10 overloaded frames, +1 after 100 light frames. AI-lag throttle when a player is 900–3600 ticks behind. Pause flag forces 0. Uses x87 + `ftol`. |
| `Game_RunSimulationStep` | `0x00495490` | One loop iteration per pending step; `dwGameTick++` at the start. |
| `Timer_GetScaledTickCount` | `0x004B6340` | `GetTickCount() × nTickRateHz / 1000`. **Open:** confirm `nTickRateHz` = 30. |
| Periodic work | — | Several systems run every 30 ticks (`tick % 30`) or every 8 ticks (`tick & 7`). |

**Fixed order inside one simulation step** (this order *is* the spec):
1. `Net_ProcessIncomingPackets` `0x00453D40` (networked only): apply the packets of other machines' units (built: `net::NetBattle::network_input`, RE_MEMORY "Network and sync")
2. `Unit_TickAllPlayersScriptsAndWeapons` `0x0048AD30`
3. `Projectile_UpdateAllActive` `0x0049B720`
4. `Fx_TickWorldEffectsAndDebris` `0x00420F30`
5. `Game_TickPlayersAndWatchMode` `0x00464F80`
6. `Feature_TickAnimsReproduceAndFire` `0x00424050`
7. `Gaf_AdvanceAllFrameCursors` `0x00415B30` (model texture animations: presentation only)
8. `Game_UpdateRandomWindState` `0x00490C40` (built: `Wind_Step`, `Battle::step_wind`)
9. `Meteor_TickShower` `0x00437DE0` (built: `sim/sky.h`, `Battle::step_meteor_shower`)
10. `Camera_TickFollowEaseShakeClamp` `0x0041CA10` (the view is presentation, but its shake decay draws `rand()`: built, `sim/sky.h`)
11. `Fx_SweepAndPruneListenerSlots` `0x00471EB0` (built: `sim/effect_lists.h` lists 0-5 and 7-9, nano list 6)
12. `Minimap_TickBlinkDirtyFlag` `0x00466580` (not sim)
The whole step is `Battle::step()`; battle start `sim::start_battle` / `ta::load_battle` (RE_MEMORY "Battle start"; campaign missions: "Campaign").
13. Networked: `Share_AutoAllyResourcesAndMap` (built: `Share_AutoShareResources`, `Battle::auto_share_resources`) + `Net_FlushPendingFrames` (built: `net::NetBattle::network_flush`, frames `net/frames.h`)

After all steps in a frame: `Los_ExpireLingeringSight` (`0x00482130`, built: `Battle::expire_lingering_sight`), chat HUD index.

---

## 2. Numeric primitives (see RE_MEMORY "Numeric model")

| Item | Address | Notes |
|---|---|---|
| World position | — | `int` **16.16 fixed** (`nPosX/Y/Z`). World pixel = `>>16`. Map cell = `>>20` (16 world px). |
| Angles | — | `ushort`, full circle = `0x10000` (heading, pitch, bank). |
| Sin/cos table | `g_awSinCosTable` | 512 × `short`, **Q13** (1.0 = `0x2000`). **Open:** static data or built at startup? |
| `Trig_LookupSinHalf` / `CosHalf` | `0x004B70A0` / `0x004B70C0` | Plain table index `angle>>6`. |
| `Math_MulSinFixed` / `MulCosFixed` | `0x004B70EF` / `0x004B7123` | `(tbl[(a+0x20)>>6] × mag + 0x1000) >> 13`, with rounding. |
| `Math_Atan2Scaled` | `0x004B715A` | x87 `FPATAN`. |
| `Math_MulHigh32`, `Math_MulDiv` | `0x004B70E0`, `0x004B7381` | 64-bit intermediate fixed math. |
| `Trig_BuildRotationMatrix`, `Math_MulVec3ByMatrix3x3`, `Math_Rotate*` | `0x004B71A7`, `0x004B72E2`, `0x004B6CC0`.. | Piece/weapon transforms. |
| Float vector ops | `0x004B6E80`–`0x004B7094` | `Vec3f_*`, `Math_NormalizeVec3` (x87 `FSQRT`). |
| `__ftol`, `Math_RoundDoubleChecked` | `0x004E43A0` area, `0x004E5050` | Float→int conversions. |
| **Mixed model** | — | Economy and build progress are `float` (`flEnergyMake`, `flBuildPercentLeft`); hit points are `short`. |

---

## 3. Randomness (two generators, both used by the simulation)

| Item | Address | Notes |
|---|---|---|
| `Math_RandMod(n)` | `0x004B6C30` | "Battle RNG": Park–Miller style (×16807, mod 2³¹−1) on `g_dwBattleRngState`; returns `state % n`; `n<2` → 0, with no state advance. **Re-check exact arithmetic in the disassembly.** |
| `Math_SeedBattleRng(seed)` | `0x004B6CA0` | `state = seed ^ 0x66E29572 | 1`. **Open:** where the seed comes from. |
| `Math_Rand15()` | `0x004E4870` | The C runtime's `rand()`: `s = s×214013 + 2531011; return (s>>16)&0x7FFF`. Per-thread state. Used by sim (wind timer). **Open:** `srand` seed. |

---

### RNG consumers (critical for identical behaviour)
**Battle RNG (`Math_RandMod`), 57 callers, all simulation-side.** They include:
- AI
- missions (most `MissionTick_*`)
- targeting (`Unit_FindBestTargetInRange`, `Unit_RetargetAttack`, `WeaponFire_Turret`)
- `Projectile_UpdateAllActive`, the COB VM (`UnitScript_ExecSlot`: the `rand` opcode), `UnitScript_ExplodePiece`, `Unit_InitInstanceFields`
- features and fire, wind, battle init
- **debris spawning** (`Fx_SpawnDebrisFromUnitPiece`, `Unit_SpawnDebrisFromAllPieces`)

Debris therefore must be emulated even though it looks visual: it advances the battle RNG. **Open:** whether debris spawning depends on camera or visibility. If it does, the original sim is view-dependent.

**CRT `rand()` (`Math_Rand15`), 29 callers, mixed simulation + rendering/UI.**
- Simulation: wind, meteors, features, `Explosion_Frame`, `VictoryConditions_PollDefeat`, battle init, start positions.
- Rendering/UI: `Gfx_DrawProjectiles`, camera shake, FX particles, GUI sparks, speech, CD audio, net chat.

Because draw code advances the same stream, **the original's `rand()` sequence depends on frame rate and view, so it is not deterministic in the original itself.**

Plan:
- The engine keeps `rand()` as its own sim-only stream.
- Golden tests capture `rand()` state from the debugger at each step boundary and inject it, so every other part of the sim can be compared exactly.

### Multiplayer model (verified by reading, see RE_MEMORY "Network and sync")
The unit tick runs missions and motion only for units whose owner is controller type 1 or 2 (local human or local AI). It then calls `Net_BroadcastPlayerUnitStates`. Remote players' units (type 3) appear to be updated from received state, not simulated locally. `SpeedCtrl` throttles on remote players' `dwUnitStateSyncTick`.
**TA is owner-authoritative state broadcast, not lockstep.** Built (2026-10-14, multiplayer): `engine/net` (wire format, frames, transports, `NetBattle`, battle room) and the Battle's network members (`engine/sim/src/net_units.cpp`); the engine's `sim/lockstep.h` stays a test tool. See RE_MEMORY "Network and sync". Completed 2026-10-15: loading handshake and start positions, logos, ally teams, machine groups, side record and map checksum, definition agreement among all humans, frames out of order, UDP transport with resend, time-out, watch notice, chat recipients; the unit state packets are harness-verified against the original (`netstate_harness`).

## 4. State structures (reference-program layouts, packed / alignment 1)

| Struct | Size | Role |
|---|---|---|
| `GameState` | 234 061 | Global game: players[10], tick, speed control, wind, map info, sea level… |
| `PlayerState` | 331 (0x14B) | Per player: controller type, unit range, resources, AI, LOS. |
| `UnitInstance` | 280 (0x118) | Per unit: position, angles, weapons[3], orders, motion, script, HP, flags. |
| `UnitWeaponSlot` | 28 | ×3 per unit. |
| `UnitResourceSlot` | 52 | Per-unit make/use rates. |
| `UnitMotion` | 47 | Movement state. |
| `MissionOrder` | 86 | Order queue node (primary and secondary lists). |
| `UnitScriptContext` | 1348 | COB VM instance. |
| `UnitDef` | 585 | From FBI. |
| `WeaponDef` | 277 | From weapon TDF. |
| `Projectile` | 107 | Live projectile. |
| `MapInfo` | 3780 | From OTA. |
| `MoveInfo` | 32 | Movement class. |

The engine defines its **own** structs. These layouts are only for understanding and for reading TA state in tests.

---

## 5. Simulation subsystems (reference-program function counts)

**Per-unit tick** (`0x0048AD30`). Loops over players 0–9, then each player's unit range:
1. `Wind_NotifyUnitScript` `0x00437910`: COB SetDirection/SetSpeed when wind changed (sees *previous* step's wind)
2. `UnitScript_ProcessWeaponSlots` `0x0049E1A0`: aim and fire
3. `UnitScript_TickSlotsAndAnim` `0x004B0D60`: run COB threads and piece animation
4. Timers: recently-damaged, post-transfer hold-off; selection flag gate
5. Every 30 ticks: health %
6. Water damage (every 30 ticks, below sea level, not hover); heal (every 8 ticks, `healtime`)
7. `Unit_ProcessPrimaryMissionOrders` `0x0043B7C0`, `…Secondary…` `0x0043BAD0`
8. `UnitMotion_Tick` + `Unit_UpdateGroundHeight` `0x0048A870`
9. Kill broadcast if flag bit 14

| Subsystem | Functions | Key entry points |
|---|---|---|
| **COB script VM** | UnitScript 68, Cob 18 | `UnitScript_ExecSlot` `0x004B0DA0`, `AdvancePieceAnims` `0x004B1C00`, `LoadCob` `0x004B2450`, Get/SetUnitValue `0x00480770`/`0x00480B20`, FireWeapon* `0x0049C9C0`.. |
| **Missions / orders** | MissionTick 66, MissionOrder 32, MissionOrderTable 20, AirManeuverOrder 14 | Ground, building, VTOL and air handlers `0x00401C20`–`0x004152F0`. **All 68 kinds built** (smoke-verified): see RE_MEMORY "Mission behaviours", "Construction and reclaim", "Movement behaviours (ground)" and "Aircraft and transports". Inputs: the reclaim-site y and the air repair patrol's stack address (uninitialised / run-varying). The teleport trail's flame dots are simulated (rand() in the effect sweep). |
| **Movement** | UnitMotion 13, MoveInfo 16 | `UnitMotion_Tick` `0x0043DD20`, fly physics `0x0043D290` |
| **Pathfinding** | PathGoal 44, PathOrder 22, PathOrderAttach 3 | The route search `RouteSearch_Step` `0x0040EB70` (the reference program's "AiSearch": a pathfinder, not AI) and the movement classes' passability: built, see RE_MEMORY "Route search" |
| **Weapons / projectiles** | Weapon 23, WeaponDef 9, WeaponFire 4, Projectile 12, Explosion 4 | Ballistic aim `0x0049A9xx`, area damage `0x0049A388` |
| **Damage / death** | (Unit) | `Unit_ApplyDamageAndBroadcast` `0x00489BB0`, `Unit_CreateFromDefId` `0x00485F50` |
| **Economy** | Player 39, UnitResourceSlot 7 | Built, see RE_MEMORY "Economy": settlement `0x00401360` (the reference program's `Player_AccumulateUnitResourceRates`), slot bookings and spends `0x00401180`-`0x00401260`, transfers `0x00464B30`/`0x00464C60`, sharing `0x00457D30`, wind `0x00490C40`; extractor rate `0x00437840` with unit creation. `0x00401320` is dead code. Player getters (`0x00464AB0`..) are UI |
| **Line of sight / radar / cloak** | Los 28, Sensor 3 | Built: see RE_MEMORY "Line of sight and radar" (`sim/sight.h`; sight updates `0x004827B0`, sensor pass `0x00467440`, rebuild `0x004816A0`, lingering sight `0x00482910`/`0x00482130`). The cloak itself is applied by the economy pass `0x00401360`. |
| **AI** | Ai 63, AiSearch 23, AiPlan 6 | Built (smoke-verified): see RE_MEMORY "Computer player" (`sim/ai.h`; controller step `0x00408C40`, roles `0x004077E0`-`0x00408100`, group assignment `0x00408830`, groups `0x00480250`, scores `0x00409730`, pick/score/site `0x0040BDB0`/`0x0040BB00`/`0x0040BFE0`, profile `0x00406C90`-`0x00406E40` and `ta/ai_profile.h`). The reference program's "AiSearch" is the route search (built). Dead: `0x00408920`, `0x00408BF0`, `0x00408620`, `0x0040C250` |
| **Features** | Feature 13, FeatureDef 4 | Animation, reproduce, tree fire, reclaim, wreckage |
| **Map / terrain** | Map 35, MapInfo 33, HeightMap 10, SpatialBucket 2 | Heights, footprints, passability |
| **Environment** | Wind 1, Meteor 7 | Wind every 150–420 ticks; meteor showers |
| **Victory / defeat** | VictoryCond 44, DefeatCond 25, VictoryConditions 13 | Built for skirmish/network (`engine/sim/src/victory.cpp`, RE_MEMORY "Victory") and campaigns (`sim/mission_goals.h`, RE_MEMORY "Campaign"; smoke-verified) |
| **Commands (input)** | Cmd 93, Orders 4 | How player orders enter the sim |
| **Net lockstep / sync** | Net 66, Sync 59 | The original replicates unit states (not lockstep); Sync = unit-definition checksums. Built: the original's model (`engine/net`, `net_units.cpp`; wire format differential-tested in `net_harness`), the battle room with the checksum exchange (`net/lobby.h`), TCP and loopback transports; the engine lockstep tool (`sim/lockstep.h`) remains for tests. Open: DirectPlay interoperability, the side record's unread bytes, resend/out-of-order frame paths |
| **Debris / FX** | Fx 55, Debris 3 | Built: world effects, shards, debris, the effect particle lists and the nano streams with their shared 1000-object pool (RE_MEMORY "Debris and effects", "Effect particle lists", "Simulation completeness"); drawing only: model texture cursors `0x415B30`, explosion art frames |
| **Construction / reclaim / capture** | MissionTick 9, Unit 12 | Built (smoke-verified): see RE_MEMORY "Construction and reclaim" — build slices `0x41BA60`, factory `0x402640`, mobile build `0x403A20`, help `0x403F70`, repair `0x405300`, reclaim `0x404AD0`/`0x404730`, capture `0x404270`, yard placement `0x47D2E0`, transport packet `0x48AB70`. **Nano streams draw the C runtime rand() in the unit pass and stage 9 (not at draw time).** RepairPatrol, Resurrect and the aircraft builders are built ("Aircraft and transports") |

---

## 6. Data the simulation consumes (provided by `ta_compat` importers)

| Source | Gives the sim |
|---|---|
| Unit FBI files | Unit definitions: stats, speeds, costs, categories, weapons |
| Weapon TDF files | Weapon definitions: range, velocity, damage, ballistics, area of effect |
| COB scripts | Unit behaviour bytecode (aim, fire, build, animation) |
| 3DO models | **Piece hierarchy and offsets.** The sim needs them for weapon fire points, aim-from pieces and nano pieces. |
| Feature TDF files | Trees, rocks, wrecks: reclaim value, blocking, burnable |
| TNT maps | Height map, tile/feature placement, metal map, sea level |
| OTA (map info) | Wind min/max, gravity, tidal strength, water damage, lava, start positions |
| `moveinfo.tdf` | Movement classes (slope, water depth, footprint) |
| `sidedata.tdf` | Sides, commanders, start resources |

---

## 7. Setup and testing

| Item | Address | Use |
|---|---|---|
| `Game_InitializeBattle` | `0x00497180` | Sim initialization order (map, players, RNG seed, start units) |
| `Game_LoadMapTnt` | `0x004917D0` | Map plus environment init (wind denominator 5000) |
| GameSave (22 functions) | — | Read (RE_MEMORY "Saved games"): a partial snapshot (no generators, projectiles, effects, route search, AI); the engine saves the start plus an input journal (`sim/journal.h`, `ta/saved_game.h`) in the original's bank container |
| **Ghidra debugger** (ghidra-mcp `debugger_*`) | — | Run the original TotalA.exe, break per sim step, dump RNG/unit state → **golden traces** to compare against the engine tick-for-tick |

**Out of scope for the sim:** rendering, GUI, sound, lobby, CD audio, camera, minimap.

---

## 8. Open questions (resolve when the relevant step is reached)
1. ~~`nTickRateHz`~~ resolved: 30 Hz (runtime capture #1).
2. ~~Sin/cos table~~ resolved: static, 640 entries, formula-generated (see RE_MEMORY).
3. ~~RNG seeds~~ resolved: QPC for the battle RNG, `time(NULL)` for `rand()` (see RE_MEMORY).
4. ~~`Math_RandMod` arithmetic~~ resolved from the disassembly (see RE_MEMORY).
5. ~~Which FX/debris work mutates sim state~~ resolved for features, world effects and debris (see RE_MEMORY "Features and wrecks", "Debris and effects", "Draw-time side effects"); the GAF texture cursors (`0x00415B30`) only advance model texture animation frames (drawing).
6. ~~Command packet format~~ resolved: there are no command packets (RE_MEMORY "Commands").

## 9. What is left outside the simulation (2026-10-15)
Every `BattleOutside` virtual and every C runtime `rand()` caller is classified in RE_MEMORY "Simulation completeness". Nothing in the simulation is left on a hook. What stays outside, and why: run-varying inputs (the wall clock for the hover bob, stale stack words of the AI site height, the reclaim sites and the network handlers `0x452CC0` / `0x4861D0`, heap addresses); frame- or camera-driven `rand()` draws of the original (the speech queue, modelled by the client; lightning drawing; CD audio); presentation and the network.
