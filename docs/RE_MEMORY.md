# TA 3.1 Reverse-Engineering Memory

Single source of truth for everything learned from `TotalA.exe` (OTA 3.1, Steam build).
Read this before touching Ghidra; append to it after every RE session. Never re-derive
something already recorded here.

## Workflow rules
1. Reverse engineer **only** what the current request needs. No speculative sweeps, no bulk transfers.
2. Check this file first. If the answer is here, use it.
3. Use `/Thaldren_TotalA.exe` (prior, partly unverified RE work) as the reference/starting point for the
   functions involved. Re-check its names/types against the decompile; correct anything wrong.
4. Implement + test the feature in this repo, citing the addresses it was derived from.
5. Only after it is verified by testing, write the final corrected names/types/comments into
   `/TotalA.exe` (the clean, authoritative program) and record them below.

## Project rules (commercial path)
The goal is a sellable original game, so these rules apply to all work.

- **Three layers, kept separate:**
  1. `engine/`: original code, its own data formats, no TA files/names.
  2. `ta_compat/`: TA importer and compatibility layer. Separate, free/open source, never bundled with the paid game. Doubles as the test harness.
  3. The game: original assets, factions, name and balance.
- **Ghidra is for understanding, not copying.** Record behaviour and formats here as a *spec in our own words*. Engine code is written from this spec, never transcribed from decompiled code.
- **Fidelity target: the simulation behaves the same as TA's** (user rule, 2026-10-13). Given the same starting state and inputs, the engine's sim state must match TA's tick for tick. That covers:
  - iteration order, slot and ID allocation, and every RNG draw (count and order);
  - integer wraparound, float rounding points and TA's bugs/quirks.

  Verified against golden traces captured from the original `TotalA.exe` under the debugger. Numerics: see "Numeric model". Code is still written from our spec, never transcribed.
- **Libraries:** commercial-friendly only (MIT/BSD/zlib/Apache). No GPL.
- Git history and this file serve as the independent-design record.

## Platform target: current Windows, Linux, macOS
(The earlier Windows 95 target was dropped on 2026-10-13.)
- **Language/build:** C++20 with CMake. Compilers: MSVC (Windows), GCC/Clang (Linux) and AppleClang (macOS).
- **CPUs:** x86-64 and arm64 (Apple Silicon).
- **Platform layer:** SDL3 (zlib licence) for windows, input, audio and file I/O.
- **Rendering:** SDL3 GPU API, which maps to D3D12, Vulkan or Metal. Not OpenGL, which is deprecated on macOS.
- **Simulation numerics follow TA's own model** (user rule, 2026-10-13). Wherever TA uses fixed-point, the engine uses the same fixed-point format. Wherever TA uses x87 floating point, the engine reproduces x87 results, emulating them where native hardware would differ. Spec: see "Numeric model" below.
- **Compiler float flags (mandatory):** no `-ffast-math`. Use `-ffp-contract=off`, or `/fp:precise` with no FMA contraction on MSVC. Without these, x86-64 and arm64 results diverge.
- **Allowed libraries:** miniz/zlib (HPI decompression), doctest (tests).
- **CI:** build and test on all three OSes.

## Ghidra programs (project `OTA 31 Steam Reverse/TA_Reverse.gpr`)
| Path | Role |
|---|---|
| `/TotalA.exe` | Authoritative. Only verified, corrected RE goes here. |
| `/Thaldren_TotalA.exe` | Reference. Earlier RE pass (4140 functions, many named); may contain errors. |

Both are the **same binary** (PE timestamp `0x35C0B9E5`, identical sections/bytes), so addresses map 1:1.
Both report the program name `TotalA.exe`, so always pass the **path** (`/TotalA.exe` or
`/Thaldren_TotalA.exe`) as the ghidra-mcp `program` argument. Inline scripts are disabled
(`GHIDRA_MCP_ALLOW_SCRIPTS` not set).

## Target
| Item | Value |
|---|---|
| Binary | `OTA 31 Steam Root/TotalA.exe` |
| Arch | x86 32-bit LE, image base `0x00400000` |
| Functions | 2892 (auto-analysis complete, mostly unnamed as of 2026-10-13) |
| Ghidra project | `OTA 31 Steam Reverse/TA_Reverse.gpr` |

## Numeric model (spec)
Sources: `/Thaldren_TotalA.exe`, checked 2026-10-13. Not yet written to `/TotalA.exe`; that happens after the engine tests pass.

**x87 configuration**
- At CRT startup, `__setdefaultprecision` (`0x004EA5B0`) calls `_controlfp(_PC_53, _MCW_PC)`. That sets x87 precision to **53-bit (double)**, with rounding left at the default **round-to-nearest-even**.
- `_controlfp`-equivalent at `0x004E8CB0`, called through `0x004E8CE0`, which clears bit `0x80000`.
- The `-fpufussy` command-line flag (`0x004DF160`) unmasks the invalid-operation and divide-by-zero exceptions (mask `0x18`). By default they stay masked. The engine always behaves as if masked.
- A sound routine (`0x004CF370`) changes the same exception masks and then restores them; it doesn't affect the simulation.

**x87 instructions the game uses**
- `FSQRT`: 28 sites, all in simulation or effects code (ballistic aim, area damage, flight physics, help-build, AI scouting, vector normalize/length, projectile seek).
- `FPATAN`: 2 sites: `Math_Atan2Scaled` (`0x004B7163`) and the CRT `asin` (`0x004E6836`).
- `FSIN`/`FCOS`: none. Sine/cosine must come from tables or the CRT. **Unverified; RE this when angles are needed.**
- `__ftol` (`0x004E43A0` area): converts float to int by switching to truncate mode (`FLDCW`), storing the 64-bit integer, then restoring the control word.

**How the engine reproduces this**
1. Basic operations (+, −, ×, ÷, sqrt) at 53-bit precision produce the same results as IEEE double, except in exponent-range cases (overflow or underflow of intermediates, and double rounding of denormals). The engine computes in native `double` and rounds to `float` at exactly the points where the original stores a 32-bit value. Rounding double→float once is equivalent to a correctly rounded single-precision operation, so those stores match.
2. Exponent-range edge cases: a soft-float x87 (80-bit) reference path is used in tests to compare against the native path. It is only used at runtime if the tests show it's needed. Candidate library: Berkeley SoftFloat 3 (BSD).
3. `ftol` (`0x004E43A0`, checked against the disassembly): saves the control word, sets RC = truncate, `FISTP qword`, restores, returns **int64 in EDX:EAX**.
   - NaN, or input outside the int64 range, gives `0x8000000000000000`.
   - Callers that keep only EAX get the **low 32 bits** (wraps; it does *not* saturate to `0x80000000`).
   - The engine has `ftol64()` plus `ftol32() = (uint32)ftol64()`. A plain C++ cast is undefined behaviour here.
4. `atan2`/`asin` use a correctly rounded implementation (for example CORE-MATH, MIT), which matches `FPATAN` rounded to double in all but vanishingly rare cases. Each such site is recorded with a test.
5. **FSINCOS (the only site): `Math_Rotate2DPoint` `0x004B7173`** (cdecl `(short angle, int* xy)`), verified 2026-10-13 by `tools/orig_harness/x87_check`.
   - Angle 0 is skipped.
   - The argument is `a = (double)angle × 0x3F1921FB54442C5A`, a slightly-off 2π/65536, with the product rounded to 53 bits.
   - Then `FSINCOS`, giving 64-bit mantissas.
   - `x' = x·cos − y·sin`, `y' = y·cos + x·sin`. Each product and sum is rounded to 53 bits (PC53).
   - The result is stored with round-to-nearest-even `FISTP`.
   - **FSINCOS is not correctly rounded.** On the reference CPU (Intel i7-13700K), 11,093 of 131,070 values differ from correct rounding, some by many units near the zeros.
   - Model: reduce the argument exactly by the FPU's **66-bit π** (`0xC90FDAA22168C234C·2⁻⁶⁶`), then round correctly. That leaves 3,620 one-unit exceptions, stored in `engine/sim/src/x87_sincos_exceptions.inc`. With the table, the emulation equals the hardware for all 65,535 angles.
   - Our `sim::x87::rotate_point_2d` equals the original routine on 524,288 calls (every angle, 8 points each).
   - Other CPUs (e.g. AMD) may compute different last bits, as the original game itself would.
   - **FPU profiles (`sim::x87::FpuProfile`, part of the sync contract):**
     - `Model66Pi` (**default**, user decision 2026-10-14: "use the more accurate one"): the documented algorithm only. Exact 66-bit-π reduction, then correct rounding.
     - `IntelP6`: the model plus the measured table. Reported identical across Intel from the Pentium III to Core i7, i.e. the P6 line that began with the Pentium Pro (1995).
     - `Captured`: a table recorded on real hardware with `tools/fpu_capture`. That is a C-runtime-free Win32 executable stamped as Windows 4.0, importing only 5 kernel32 functions and using no CMOV/SSE. It runs on Win95 with a 486DX/Pentium and writes `FSINCOS.CAP`. Check a capture with `fpu_profile_check`.
   - **1995 hardware (researched 2026-10-14):**
     - Every Intel x87 since the 387 reduces with the same 66/68-bit π (Intel SDM / Intel forum). Intel improved the algorithms between the 387/486 and the Pentium and specifies an error bound, not bit-exact results.
     - No public data exists for the original P5 Pentium's last bits. The capture tool is the way to get them.
   - **Emulators (source-checked):** none reproduces 1995 silicon.
     - DOSBox-X normal core: host double `sin`/`cos` (53 bits).
     - DOSBox / dosbox-staging x86 FPU core: the *host* CPU's x87 instructions via inline asm.
     - 86Box / Bochs / MAME SoftFloat: 128-bit-π reduction plus float128 Taylor series, i.e. nearly correctly rounded, *not* the 66-bit-π behaviour.
   - **Impact measured:** on 1,048,560 rotations (all angles; model-scale and full-range int32 points), the captured i7 table, `Model66Pi`, a SoftFloat-style true-π computation and a host-double computation all give **identical** results. The 53-bit products and integer rounding absorb last-bit differences: a flip needs a product within about 1e-7 of a .5 boundary.
   - FPATAN and hypot behave the same way. hypot uses only IEEE-exact operations, so it is identical on any IEEE x87, except an original P5 with the 1994 FDIV flaw, which was not emulated.
   - `Math_RotatePointByAngles` (`0x004B6CC0`): (x,y) rotated by angles[0], then (y,z) by angles[2], then (x,z) by angles[1].
6. **Scaled atan2: `Math_Atan2Scaled` `0x004B715A`** (cdecl `(int y, int x) → int`), with 22 callers in weapons, movement, AI and scripts.
   - `FPATAN(y, x)` at 64-bit precision, then × `0x40C45F306DC9C78A` (≈65536/2π, slightly off), rounded to 53 bits (PC53), then `FISTP` nearest-even.
   - Engine: `sim::x87::atan2_scaled`. A double fast path is used unless the scaled value is within 1e-7 of a .5 tie. Otherwise it computes FPATAN exactly (Newton iterations in 320-bit fixed point, rounded to 64 bits) and does the exact 53-bit product.
   - Verified against the original on 4,000,150 calls, including 150 near-tie inputs: identical.
7. **C runtime `_hypot`: `Math_Hypot` `0x004FB440`** (cdecl `(double, double) → ST0`), with 33 callers.
   - It runs at **precision control 64** (`_controlfp(0x133F)`; its sqrt helper switches to 0x37F), so every step is double-rounded through a double store:
     - m = max(|x|,|y|), with m = 0 giving 0.
     - x' = rnd53(rnd64(x/m)), y' likewise.
     - s = rnd53(rnd64(rnd64(x'²) + rnd64(y'²))).
     - q = rnd53(rnd64(√s)).
     - The result is rnd53(rnd64(mant(m)·mant(q)))·2^(e_m+e_q), which equals rnd53(rnd64(m·q)).
   - Engine: `sim::x87::crt_hypot`, built on exact soft-float `div64/mul64/add64/sqrt64/to_double`.
   - Verified on 1,000,000 calls (Pythagorean integer pairs, random ints, wide-exponent doubles, float-derived values, one-zero cases): identical.
   - Callers usually `__ftol` (truncate) the result, so perfect squares that come out 1 ulp low would truncate down. The emulation reproduces that exactly.

**Trig table** (static checks done 2026-10-13; awaiting engine test before writing to `/TotalA.exe`)
- `0x00509F00`: static `.data`, never written. **640** `int16` entries (the reference program typed it as 512; that is wrong). Entries 0–511 cover one full circle and 512–639 repeat the first quarter, so cosine can read `index + 128` without wrapping.
- The value of entry *i* is exactly `round(8192 × sin(2π·i/512))`, Q13 format. All 640 match; plain truncation does not. The engine generates the table from this formula and tests it against the captured bytes.
- Sine lookup: `tbl[(angle >> 7)]`. Cosine: `tbl[(angle >> 7) + 128]`. The multiply variants round the angle first: the byte offset is `((angle + bias) >> 6) & 0x3FE`, with bias `0x20` for sin (`0x004B70EF`) and `0x4020` for cos (`0x004B7123`, so it does **not** use the +128 tail and wraps within 512). Then they compute the low 32 bits of `(tbl × mag + 0x1000) >> 13` with a signed 64-bit intermediate. `Math_MulHigh32` (`0x004B70E0`) returns the high 32 bits of a signed 32×32 multiply.

**RNGs** (static checks done 2026-10-13)
- Battle RNG `RandMod(n)` (`0x004B6C30`, state at `0x0051FC88`). Exact arithmetic from the disassembly, all `uint32` and wrapping:
  - `q = s / 127773` (unsigned)
  - `s' = 16807·s − q·0x7FFFFFFF` (mod 2³²)
  - if `(int32)s' <= 0`: `s' += 0x7FFFFFFF`
  - return `s' % n` (unsigned division)

  The `n < 2` check is **signed**, so n ≤ 1 or n ≥ 2³¹ returns 0 without advancing the state. This is **not** true Park–Miller, because `16807·s` overflows 32 bits; reproduce it as-is.
- Seed (`Game_InitializeBattle` `0x00497180`, first lines): `SeedBattleRng(QPC.LowPart + QPC.HighPart)` → `s = seed ^ 0x66E29572 | 1`. It's **clock-based and per machine**: every run and every multiplayer peer gets a different battle RNG (consistent with owner-authoritative MP).
- CRT `rand()` (`0x004E4870`): MSVC LCG `s = s·214013 + 2531011`, returns `(s >> 16) & 0x7FFF`. State lives in per-thread data slot 5.
- `srand` is `0x004E4860`, which writes the same per-thread slot 5. **The reference program mislabels it `Crt_SetTerminate`. Correct this in `/TotalA.exe`.** Seeded with `srand(time(NULL))` right after the battle RNG.
- Consequence for testing: golden captures must record (or overwrite) both seeds at battle init.

**Runtime capture #1** (2026-10-13, `ta_compat/tools/capture`, fixture `golden_skirmish_moonquartet_300.jsonl`)
- Setup: Moon Quartet skirmish, you + 3 hard AI, random start positions, battle seed overridden to `0x12345678`, `srand` seed overridden to `0xC0FFEE`; ran 300 ticks.
- **Tick rate: 30 Hz confirmed** (`displayContext+0xE8` = 30). The first 5 frames ran 5 catch-up steps each (budget cap); after that, 1 step per frame.
- **Threads:** the simulation runs on the **main thread**. Battle init (`Game_InitializeBattle`) runs on a separate **loading thread**.
- **`srand` is called twice:**
  - At program start on the main thread (`App_WinMain`, return address `0x0049E8C0`), with `time()`.
  - In battle init on the **loading thread** (`0x004971B3`).

  Since `rand()` state is per thread, **the simulation's `rand()` state is the main thread's**. That's the startup seed advanced by every menu/UI draw before the battle, so it can't be reproduced and must be captured at tick 1 and injected. Battle init's `srand` and its ~400 k `rand()` calls (explosion setup) only affect the loading thread.
- The battle RNG is seeded and drawn 46 times on the loading thread during init (start placement etc.); all later draws are on the main thread.
- RNG consumers seen in 300 ticks:
  - Battle RNG (471 draws): `Ai_TryRandomFootprintPlacement`, `Ai_PeriodicRefreshUnitTblById`, `MissionTick_Standby`, `Ai_PickWeightedUnitTypeCandidate`, `Ai_TickSpatialBehaviorSlot`, `Ai_SeekRandomPlacementNearTarget`, `Ai_TickSquadScoutBehaviorSlot`, `Game_UpdateRandomWindState`.
  - `rand()` on the sim thread (6 draws): `Game_UpdateRandomWindState`, `Meteor_TickShower` ×4, **and `CdAudio_SyncPlaybackToMode` (`0x004CDDE8`)**. That last one confirms that non-sim code advances the sim's `rand()` stream. The engine will treat `rand()` draws as inputs recorded per tick.

**Fixed-point formats:** unverified. Record each format here as it is found (position, velocity, angle units, resource amounts).

**Simulation inventory:** see [SIM_INVENTORY.md](SIM_INVENTORY.md) (unverified reference list from `/Thaldren_TotalA.exe`).

## Functions
Written to `/TotalA.exe` (name + prototype + plate comment) once verified.

| Address | Name | Signature | Behaviour | Confidence |
|---|---|---|---|---|
| `0x004B6C30` | `BattleRng_NextMod` | `uint __stdcall (uint n)` | Battle RNG draw; see Numeric model | Verified: emulator vs engine tests |
| `0x004B6CA0` | `BattleRng_Seed` | `void __stdcall (uint seed)` | `s = seed ^ 0x66E29572 \| 1` | Verified |
| `0x004B70A0` | `Trig_SinQ13` | `int __cdecl (ushort angle)` | `tbl[angle >> 7]` | Verified (disasm + table test) |
| `0x004B70C0` | `Trig_CosQ13` | `int __cdecl (ushort angle)` | `tbl[(angle >> 7) + 128]` | Verified (disasm + table test) |
| `0x004B70E0` | `Math_MulHigh32` | `int __cdecl (int a, int b)` | high 32 bits of signed product | Disasm only (not yet used by the engine) |
| `0x004B70EF` | `Trig_MulSinRounded` | `int __cdecl (ushort angle, int mag)` | rounded sin × mag | Verified: emulator vs engine tests |
| `0x004B7123` | `Trig_MulCosRounded` | `int __cdecl (ushort angle, int mag)` | rounded cos × mag | Verified: emulator vs engine tests |
| `0x004E4860` | `Crt_srand` | `void __cdecl (uint seed)` | CRT `srand` (the reference program's `Crt_SetTerminate` was wrong) | Verified |
| `0x004E4870` | `Crt_rand` | `int __cdecl (void)` | MSVC LCG, 15-bit output | Verified (disasm + known sequence) |
| `0x004E43A0` | `__ftol` | `int64 (st0)` | truncating float→int64 | Verified (disasm) |
| `0x00495230` | `SpeedCtrl_BudgetSimSteps` | `void (void)` | Frame step budget; full spec in the plate comment and `engine/sim/src/step_budget.cpp` | Verified: disasm + tests + capture (cap 5) |
| `0x00495490` | `Game_RunSimulationStep` | `void (int networked)` | Step loop + stage order; Los deferred expiry runs once per frame **after** the loop (even with 0 steps) | Verified: runtime |
| `0x004E5050` | `Crt_floor` | `double (double)` | CRT floor (x87 CW `0x173F`) | Verified (disasm) |
| `0x004CB560` | `Model3d_Load` | — | Relocate the 3DO tree in place, then flip X/Z; see 3DO models | Verified: live model dump |
| `0x004CB4C0` | `Model3d_RelocatePointers` | — | File offsets → pointers; sibling, then child, then sort | Verified: live model dump |
| `0x004CB370` | `Model3d_SortPrimitivesByDepth` | — | Selection primitive to [0]; bubble sort by average Y | Verified: live model dump |
| `0x004CB590` | `Model3d_NegateAxesXZ` | — | Negate X/Z of vertices and offsets | Verified: live model dump |
| `0x004CB5F0` | `Model3d_GetHeight` | — | Max Y over the tree, ≥ 0 | Verified: unit table Y extents |
| `0x0045AE80` | `Model3d_CountTreeNodes` | — | Piece count | Disasm |
| `0x004917D0` | `Loading_LoadBattleData` | `void (void)` | Battle data load order (features → map → weapons → units → chains) | Verified: feature table order |
| `0x004222E0` | `Feature_EnumTdfFiles` | `void (void)` | Parse features\**\*.tdf into the parser list | Verified |
| `0x00421F20` | `Map_ProcessTntFeatures` | `void (TntInfo*)` | Reset table, append every TNT feature | Verified |
| `0x00483610` | `Map_LoadTnt` | `void (void)` | TNT header, tiles, feature placement | Partial (feature part verified) |
| `0x004224B0` | `FeatureDef_LoadTdfProperties` | `int (char* name)` | Append one feature def; returns its index | Verified: live dump |
| `0x00422E40` | `FeatureDef_FindOrLoad` | `int (char* name)` | Index by stricmp, else load | Verified: corpse indices |
| `0x00422EA0` | `FeatureDef_ResolveChains` | `void (void)` | dead → reclamate → burnt links, appending misses | Verified: live dump |
| `0x00423160` | `Mission_SpawnMapFeatures` | `void (void)` | OTA schema features: find-or-load + place | Disasm (no schema features on the test map) |
| `0x004BCB50` | `Hapi_EnumerateTree` | `void (char* dir, char* pattern, vector*, int source, bool all)` | Recursive file enumeration | Verified: feature library |
| `0x004B8C60` | `Gaf_LoadAndRelocate` | `GafFile* (char* path)` | Load a GAF and relocate offsets | Verified (sequence names) |
| `0x004B8D40` | `Gaf_FindSequenceByName` | `GafSeq* (GafFile*, char*)` | Linear stricmp lookup | Verified |
| `0x004B2450` | `UnitScript_LoadCob` | `Cob* (char* path)` | Load + relocate a COB, cache its checksum | Verified (harness layout) |
| `0x004B08C0` | `UnitScript_StartThread` | `int __thiscall (int fn)` | First free thread; returns it or −1 | Verified: differential |
| `0x004B0940` | `UnitScript_CallNamed` | `int __thiscall (char*, Sink*, int run_now)` | Start by name | Verified: differential |
| `0x004B0B00` | `UnitScript_InvokeByIndex` | `int __thiscall (int, Sink*, int run_now, int argc, int a, int b, int c, int d)` | Start with 4 args | Verified: differential |
| `0x004B0C40` | `UnitScript_CallInOut` | `int __thiscall (int, int*, int*, int*, int*)` | Run now, copy locals 0–3 back | Verified: differential |
| `0x004B0D60` | `UnitScript_Tick` | `void __thiscall (int ticks)` | Run threads, then animate | Verified: differential |
| `0x004B0DA0` | `UnitScript_ExecSlot` | `void __thiscall (int thread, int ticks)` | The interpreter | Verified: differential |
| `0x004B1C00` | `UnitScript_AdvancePieceAnims` | `void __thiscall (int ticks)` | Piece move/turn/spin stepping | Verified: differential |
| `0x0042A2C0` | `Model3d_LoadObjectByName` | `Model* (char*)` | objects3d\<name>.3DO, flip, register | Verified (feature models) |
| `0x0049B6E0` | `Projectile_Alloc` | `void* (void)` | Next table slot (count < 300); never called, inlined everywhere | Verified (inlined copies): projectile_harness |
| `0x0049C740` | `Projectile_InitLaunch` | `void __stdcall (proj, weapon, int* start, int* aim, uint tick, owner)` | Launch set-up; see Projectiles | Verified: projectile_harness |
| `0x0049C9C0` | `Projectile_FireDirected` | `int __stdcall (slot, owner, int* muzzle, int* aim, target)` | LOS/self-propelled launch | Verified: projectile_harness |
| `0x0049CDE0` | `Projectile_FireBallistic` | `int __stdcall (slot, owner, int* muzzle, int* aim, target)` | Ballistic launch | Verified: projectile_harness |
| `0x0049D000` | `Projectile_SpawnFromUnitMotion` | `int __stdcall (slot, owner, int* pos)` | Unused launch along the owner's motion | Verified: projectile_harness |
| `0x0049DF10` | `Projectile_SpawnFree` | `int __stdcall (weapon, int* pos, int* vel, int broadcast)` | Ownerless shot (meteors), packet 0x0D | Verified: projectile_harness |
| `0x0049B720` | `Projectile_UpdateAll` | `void (void)` | Per-step flight, hit tests, compaction | Verified: projectile_harness |
| `0x0049B090` | `Projectile_HitTest` | `void __stdcall (weapon, proj)` | Map edge, chased shot, units, features, ground, water | Verified: projectile_harness |
| `0x0049B3E0` | `Projectile_SeekPoint` | `int* __stdcall (proj)` | Cruise waypoint / chased shot / target / aim | Verified: projectile_harness |
| `0x0049B520` | `Projectile_TurnToward` | `int __stdcall (proj, int* point)` | Turn-rate-limited steering | Verified: projectile_harness |
| `0x0049AE20` | `Projectile_Compact` | `void (void)` | Remove deleted shots, partial chased-pointer remap | Verified: projectile_harness |
| `0x00499EB0` | `Projectile_Detonate` | `void __stdcall (proj, hitUnit)` | Impact effects + direct/area damage | Verified: projectile_harness |
| `0x00499CD0` | `Projectile_DamageUnit` | `int __stdcall (proj, unit, float scale)` | Damage table, veterancy, options → apply damage | Verified: projectile_harness |
| `0x0049A120` | `Projectile_AreaDamage` | `void __stdcall (proj, int* pos)` | Units, features, interceptor chain | Verified: projectile_harness |
| `0x00406F50` | `Projectile_MarkOwnerDamageSplit` | `void __stdcall (proj, int enemy, int own)` | Owner net-dirty 0x4000/0x2000 | Verified: projectile_harness |
| `0x0049AF90` | `Projectile_HandleDetonatePacket` | `void __stdcall (ctx, packet)` | Packet 0x0E | Verified: projectile_harness |
| `0x0049C880` | `Projectile_RemoveOwnerBursts` | `void __stdcall (unit)` | Dying unit's burst launchers | Verified: projectile_harness |
| `0x0049B000` | `Unit_DetonateDeathWeapon` | `void __stdcall (unit, int selfDestruct)` | Death explosion | Verified: projectile_harness |
| `0x00421EB0` | `Map_GridToWorldPosWithHeight` | `int* __stdcall (int* out, uint* cellXZ, featureDef)` | Feature spot for area damage | Verified: projectile_harness |
| `0x0049A850` | `Math_Vec3LengthFtol` | `int64 __stdcall (int* v)` | ftol(sqrt((x²+y²)+z²)) | Verified: projectile_harness |
| `0x00468CF0` | `Render_DrawBattleFrame` | `void (int drawDynamic, int present)` | Battle view frame and draw order; see Rendering | Spec + viewer |
| `0x004C1000` | `Gfx_RasterizeDepthPoly` | `stdcall (frame*, int* xyz, int n, int colour)` | Flat polygon scan conversion | Verified: raster_harness |
| `0x004C8760` | `Gfx_DrawTexturedQuadDepthTested` | `stdcall (frame*, texFrame*, int* xyz, int* uv)` | Textured quad scan conversion | Verified: raster_harness |
| `0x00459830` | `Model_RasterizePiecesTextured` | `thiscall (frame, state, slot, cachedOnly)` | Model sprite pieces | Spec + viewer |
| `0x0042A140` | `Model3d_BindPrimitiveTextures` | `void (Model3do*, char*)` | Primitive texture binding | Spec + viewer |

## Globals
| Address | Name | Type | Meaning |
|---|---|---|---|
| `0x0051FC88` | `g_dwBattleRngState` | `uint` | Battle RNG state |
| `0x00509F00` | `g_awSinTableQ13` | `short[640]` | Q13 sine table (512 + 128 tail) |
| `0x00511DE8` | `g_pGameState` | `void *` | Game state: `+0x38A3B` steps pending, `+0x38A47` game tick (runtime verified) |
| `0x0051FBD0` | `g_pDisplayContext` | `void *` | `+0xE8` tick rate Hz = 30 (runtime verified) |

Also applied: `0x00495490` `Game_RunSimulationStep` (runtime verified).

## Structs
_(name, size, field offsets)_

## File formats
_(record only what has been verified)_

### HPI archives (.hpi / .ufo / .ccx / .gp3)
Verified 2026-10-13: `hpi_check` decodes all 7,890 files in all 30 archives of the Steam install with 0 failures (9,630 LZ77 + 26,175 zlib chunks; every chunk checksum and size matches). Implementation: `ta_compat/src/hpi.cpp`, `ta_compat/src/inflate.cpp`.
- **Header (20 bytes):** `"HAPI"`, u32 `0x00010000`, u32 directory size, u8 key (+3 pad), u32 root node offset.
- **Trailer:** the file must end with `Copyright 0000 Cavedog Entertainment` (the 4 year digits are not compared).
- **Scrambling:** if key ≠ 0, key' = `~((key >> 6) | (key << 2))` (8-bit). Every byte read from the archive after the 20-byte header becomes `(u8)filePos ^ key' ^ ~b`, where filePos is its absolute offset. This covers the directory, chunk tables and chunks.
- **Directory (offsets relative to file start):**
  - node = u32 count, u32 entries offset;
  - entry (9 bytes) = u32 name offset (NUL-terminated), u32 data offset, u8 flags (bit 0 = directory, so data offset points to a child node; bit 1 = "shadowed", set at runtime);
  - file record (9 bytes) = u32 data offset, u32 size, u8 compression (0 = stored).
- **Compressed files:** u32 chunk sizes for each 64 KiB block (`ceil(size/65536)` entries) at the data offset, followed by the chunks back to back.
- **SQSH chunk (19-byte header):**
  - `"SQSH"`, u8 (unused), u8 method (1 = LZ77, 2 = zlib; > 3 is an error), u8 scrambled flag;
  - u32 packed size @7, u32 unpacked size @11, u32 byte sum of the packed payload @15.
  - If scrambled: `b[i] = (b[i] - i) ^ i`.
  - It's an error if the output length ≠ unpacked size.
- **LZ77:** 4 KiB zeroed window, write position starts at 1. Each flag byte is read LSB first:
  - 0 = copy 1 literal byte;
  - 1 = u16 LE `v`: offset = `v >> 4` (0 ends the stream), count = `(v & 15) + 2`. Copy `window[(offset + i) & 0xFFF]` one byte at a time; each output byte is also written to the window.
- **zlib:** a standard zlib stream (header + DEFLATE + Adler-32).
- **Lookup:** path split on `\`; within a node, entries are scanned **from last to first**, case-insensitively, and the first hit wins.
- **Mount order** (`Hapi_ScanAndMountArchives`, verified at runtime): `rev31.GP3`, `*.CCX`, `*.UFO`, `*.HPI` **(at most 10 newly mounted)**, then CD drives.
  - Globs come back in NTFS order (names compared upper-cased).
  - Opening a file: a loose file on disk wins; otherwise the first mounted archive containing it.
  - **On the Steam install, `totala3.hpi`, `totala4.hpi` and `worlds.hpi` are never mounted.**
- **Finder** (`Hapi_FindFirstFile` `0x004BC4B0` / `Hapi_FindNextFile` `0x004BC640`): a source index of −1 means disk first, then every archive in mount order. A source index ≥ 0 restricts the search to one source. Archive entries flagged "shadowed" (bit 1) are skipped. `*.*` is treated as `*`.
- **Recursive enumeration** (`Hapi_EnumerateTree` `0x004BCB50`, used for `features\**\*.tdf`):
  - Lists `<dir>\*.*` over all sources, skipping `.` and `..`.
  - A directory is recursed **within the source it came from only**. This is why archive subtrees are walked archive by archive.
  - A file is kept when its name matches the pattern, and is stored as `dir\sub\name`.
  - Implemented as `Vfs::enumerate_tree`; verified for features (177 files, giving an identical feature table).

### TDF text format (.tdf / .fbi / .ota / .gui)
Verified 2026-10-13: `tdf_check` parses 2,311 files (55k keys in FBIs, 573k in OTAs). The only 2 failures are files the game never loads: `gamedata\UNITVIEW.TDF` (Unit Viewer data, stray `};`) and `guis\SCORE.GUI` (unbalanced braces, no string reference in the exe). Implementation: `ta_compat/src/tdf.cpp`.
- **Load** (`Tdf_Load` `0x004C2F60`): read the whole file, NUL-terminate (an embedded NUL ends the text), blank comments, parse into a root section named `root`.
- **Comments** (`0x004C33A0`): `//` up to (not including) `\n`, and `/* … */`. Blanked characters become spaces.
  - The close check uses the raw previous character, so `/*/` does not close the comment.
  - In an unterminated block, the last character before NUL is not blanked.
- **Parse** (`Tdf_Parse` `0x004C3E40`):
  - Skip space/tab/CR/LF.
  - NUL: ends the root; inside a child section it's an error.
  - `[name]` (name trimmed), optional whitespace, then `{` starts a child section. Children keep file order.
  - `}` ends a section.
  - Otherwise: key = trimmed text up to the **next `=` anywhere ahead**; value = trimmed text up to the next `;`.
- **Key storage:** a vector kept sorted by `stricmp`; each key is inserted at the lower bound. A case-sensitive exact match replaces the value. A case variant is inserted **before** the existing key, so lookups return it.
- **Errors** are fatal message boxes in the game.
- **Lookups:**
  - Key: binary-search lower bound by `stricmp`; a hit means the key does not sort below the found entry.
  - Child section: linear search, first case-insensitive match.
- **Getters:**
  - `GetInt` = `atoi` (C-locale space skip, sign, digits; wraps on overflow).
  - `GetFloat` = `atof`.
  - `GetFixed` = `ftol(atof(v) × 65536.0)`, low 32 bits.
  - `GetString` = `strncpy` to the buffer size with forced NUL (keeps at most size−1 chars); falls back to the default value.
- **`stricmp`** (`0x004F8A70`, C locale): on a mismatch, fold only `A`–`Z` to lowercase, then compare unsigned.
- **Section checksum:** `Hash_ComputeSimpleChecksum` (`0x004B6BA0`) over `[sectionStart, end − 2)`. The mix is four 8-bit accumulators: Σb, ⊕b, Σ(i⊕b), ⊕(i+b).
- `atof` is reproduced bit-exactly; see the next section.

### Unit definitions (units\*.FBI → UnitDef[], 0x249 bytes each)
Implementation: `ta_compat/src/unitdefs.cpp`. Checked with `ta_compat/tools/unitdef_check`, which compares field by field against a dump of the live game's tables (`ta_capture --dump-defs`). Status: see Feature log.
- **Enumerate** (`UnitDef_EnumerateFbiAndCheckCompat` `0x0042A8D0`):
  - Source: `units\*.FBI` in VFS enumeration order (see `Vfs::enumerate`).
  - Record 0 = `"None"`, zero-filled with only VERSION_COMPATIBLE set.
  - Per file: `fbi_checksum` = simple checksum of the bytes. Stub keys: name (localized), unitname, side, ai_weight, ai_limit, objectname (default unitname), buildcost*, norestrict, wacky.
  - `weapon_hash_xor` ^= checksum of the first weapon TDF section named weapon1/2/3/explodeas/selfdestructas.
  - Version: `floor(v)` vs 3, `floor((v - major) × 10)` vs 1.
  - Copyright must equal `Copyright 0000 Humongous Entertainment. All rights reserved.` (year ignored).
  - Loose (non-archive) FBIs are rejected while the CD check is enabled.
  - `max_this_unit` = −1.
  - An FBI without `[UNITINFO]` aborts the whole enumeration.
- **Sort** (`GameData_LoadAll` `0x0042D2E0`): drop incompatible units, then sort [1..] by `stricmp(unitname)`. The quicksort is unstable; duplicate names are not yet reproduced, and there are none in the stock data.
- **Properties** (`UnitDef_LoadFbiProperties` `0x0042BF40`) re-open `units\<unitname>.FBI` (by **unitname**, not by the enumerated filename) if its size > 0. Notable defaults and quirks:
  - moverate1/2 default to maxvelocity × 2; bankscale and damagemodifier default to 1.0 (16.16).
  - cloakcostmoving defaults to `ftol(cloakcost)`.
  - canresurrect copies CANRECLAMATE into CANREPAIR (bit 9).
  - CANCLOAK when cloakcost > 0; then a mincloakdistance of 0 becomes 80.
  - selfdestructcountdown missing → 5.
  - The 4 bad-target/no-chase category keys use the **whole** string as one name (default `none`). `category` is split into tokens, plus `ALL`.
  - soundcategory = section index in sound.tdf, else `atoi`.
  - movementclass → `CLASS<n>` index. Otherwise, footprint/slope/depth come from UNITINFO keys with defaults 10000 / −10000 / 255 and clamping.
  - `scaled_max_velocity` = (int64)(maxvel << 16) / ((maxslope + 1) << 16).
  - Weapons are found by linear name search over 256 slots; not found → slot 0. HASWEAPONS is set if any weapon1-3 ≠ 0.
  - YardMap is read only when bmcode = 0. Unknown characters are skipped, and the last character repeats when the string runs out. A **missing** YardMap reads stale stack data (modelled; no stock unit hits it).
  - Model X/Z extents come from the footprint; Y: min 0, max = 3DO height of `objects3d\<objectname>.3DO`, size = max (see 3DO models).
- **Build menus:**
  - `HAS_GUI0` if `guis\<name>0.GUI` exists with nonzero size. Pages = 1 + count of consecutive `<name>1..N.GUI`; if none, 1 when HAS_GUI0, else 0.
- **Build options** (sidedata.tdf `[CANBUILD]\[<unit>]` canbuild1..N, counter increments even when a name is unknown): written into **one shared, never-cleared 30-entry scratch list**, copied whole to each builder, so stale entries from earlier builders persist.
- **Download menus** (`UnitDef_LoadDownloadMenus` `0x0042DCF0`, `UnitDef_AppendDownloadBuildOptions` `0x0042BE30`):
  - Each file gets a 0xBD block = u32 count + 5 slots of 0x25 bytes {u16 unit, u8 MENU, u8 BUTTON, char[32] UNITNAME}. More than 5 sections overflow into the next file's block.
  - A slot whose UNITMENU is unknown still counts but stays uninitialised.
  - Page count = max(MENU).
  - DOWNLOADABLE is checked against **slot 0 of each file only**.
  - Up to 31 build options are appended (the buffer holds 30).

### Weapon definitions (weapons\*.tdf → WeaponDef[256], 0x115 bytes, inline at GameState+0x2CF3)
Implementation: `ta_compat/src/weapondefs.cpp`. Checked with `tools/weapondef_check`: all 256 slots identical to the live game (198 loaded, 9,141 fields).
- **Loading:** `WeaponDef_LoadAllTdfFiles` (`0x0042E310`, called from `Game_LoadMapTnt`, i.e. every map load).
  - Pre-clear only `aName[0]` and `bIndex = slot` in all 256 slots. Every other field keeps its previous value, so reloading a map leaves stale data in unused slots (a first battle starts zeroed).
  - Then enumerate `Weapons\*.tdf`; loose files are ignored while the CD check is on. Every root section goes through `WeaponDef_LoadTdfProperties` (`0x0042E440`).
- **Slot assignment:** slot = key `ID`. A missing ID is −1, which writes the record **before** the table (memory corruption). Later sections with the same ID overwrite earlier ones. The name is `strcpy`'d with no length limit (32-byte field).
- **Scaled values** are `ftol(GetFloat × k)`, with the constants copied bit-exact from the exe:
  - weaponvelocity and startvelocity: × 65536/30;
  - weaponacceleration: × 65536/900;
  - reloadtime, weapontimer, burstrate, duration, randomdecay, smokedelay, flighttime, holdtime, shakeduration: × 30;
  - turnrate: × 1/30;
  - minbarrelangle: default −11.25, multiplied by `0x3F91DF46A2529D34`, which is **not exactly π/180**, then stored as a float.
- **Plain values:**
  - Integers: range (default 0x7FFF), coverage, areaofeffect, burst, sprayangle, accuracy, tolerance, pitchtolerance, shakemagnitude, plus byte fields.
  - Floats: energypershot, metalpershot, edgeeffectiveness.
- **Flag bits:**

  | Bit | Flag | Bit | Flag | Bit | Flag |
  |---|---|---|---|---|---|
  | 0 | lineofsight | 11 | soundtrigger | 21 | propeller |
  | 1 | ballistic | 12 | guidance | 22 | noexplode |
  | 2 | shellweapon | 13 | tracks | 23 | burnblow |
  | 3 | beamweapon | 14 | unitsonly | 24 | twophase |
  | 4 | vlaunch | 15 | groundbounce | 25 | cruise |
  | 5 | meteor | 16 | waterweapon | 26 | commandfire |
  | 6 | noradar | 17 | toairweapon | 27 | noautorange |
  | 7 | paralyzer | 18 | smoketrail | 28 | stockpile |
  | 8 | dropped | 19 | turret | 29 | targetable |
  | 9 | startsmoke | 20 | selfprop | 30 | interceptor |
  | 10 | endsmoke | | | | |

- **Fire function** (`0x0049E010`): first match wins.
  1. turret → `0x0049D580`
  2. vlaunch → `0x0049DB70`
  3. lineofsight or selfprop → `0x0049D9C0`
  4. dropped → `0x0049DD60`

  Otherwise unchanged.
- **Model:**
  - If an earlier slot (j < ID) has a stored model name equal to this one (stricmp), share that slot's model and set our own `aModelName[0]` = 0.
  - Otherwise load `objects3d\<model>.3DO` and store the name.
  - No `model` key → model pointer NULL, name unchanged.
- **Explosions:**
  - `explosiongaf` + `explosionart` gives the GAF sequence.
  - The second sequence uses the water keys, or the lava keys if `MapInfo.nLavaWorld`.
- **Sounds:** soundstart, soundhit and soundwater register their paths in the global sound bank (`SoundBank_EnsureNamed` `0x00429470`, appended after `allsound.tdf`). No key → −1.
- **Damage:** `[DAMAGE]` `default` gives the default damage. The other keys are walked in the section's sorted key order and inserted into a stricmp-sorted vector of {name, damage}. The value is looked up by name, so the first case variant wins.

### Feature definitions (FeatureDef[], 0x100 bytes, GameState+0x1426F, count +0x14253) — exact vs live game: 434 features, 8,307 fields, 279 unit corpse indices
The table is built per battle and grows by appending (realloc). Implementation: `ta_compat/src/featuredefs.cpp`; checked by `tools/feature_check`.
- **Order inside battle loading** (`Loading_LoadBattleData` `0x004917D0`):
  1. `Feature_EnumTdfFiles` (`0x004222E0`) parses every `features\**\*.tdf` (recursive enumeration) into a list of parsers, in enumeration order. A file that fails to load is dropped.
  2. `Map_LoadTnt` (`0x00483610`) → `Map_ProcessTntFeatures` (`0x00421F20`): the count is reset to 0, then **every** TNT feature entry is appended, in TNT order, duplicates included.
     - TNT header u32[7] = feature count, u32[8] = offset of 0x84-byte entries {u32 index, char name[128]}.
     - Then, unless a saved game is loading, `Mission_SpawnMapFeatures` (`0x00423160`) runs on the OTA schema's `[features]`, using find-or-load. The schema list is built by `0x00436C30`: children in file order with Featurename[0x80], XPos, ZPos (default −1). A negative position blanks the name, and blank names are skipped.
  3. Units (`UnitDef_LoadFbiProperties`): `corpse` present → `FeatureDef_FindOrLoad` (`0x00422E40`). Its return value is the unit's corpse index (UnitDef+0x1BC; −1 when there is no corpse key). Units are processed in sorted unit order.
  4. `FeatureDef_ResolveChains` (`0x00422EA0`): for i = 0 .. count−1, with the count re-read every iteration, resolve **featuredead (0xF4), then featurereclamate (0xF8), then featureburnt (0xF6)**.
     - Each link: key absent → −1. Otherwise take the first case-insensitive name match, or append the feature by loading it.
     - Because the count is re-read, appended entries get resolved too.
  5. The parsers are destroyed (`0x004223E0`).
- **Section lookup:** the first parser (in file order) with a top-level section whose name matches case-insensitively. If none: message "Record %s missing from feature" and then a null dereference (crash).
- **Load** (`FeatureDef_LoadTdfProperties` `0x004224B0`, returns the new index):
  - Name: strncpy of the requested name (not the section's spelling), 0x80 bytes.
  - `Description` [20]. `footprintx`/`footprintz` → i16; `height` → u8.
  - **`object` present → 3D:** flag bit 0 is cleared and objects3d\<object>.3DO is loaded (load + X/Z flip + register). The GAF fields 0xA8–0xCB are **not written** (uninitialised).
  - **Otherwise 2D:** bit 0 is set; read `filename`. Scan existing entries 0..n−1 with a **case-sensitive `strncmp(entry+0x98, filename, 16)`**. For 3D entries that compares pointer bytes.
    - Hit: this entry's GAF pointer is 0 and 0x98 receives `"REUSE"`. Its sequences are looked up in the hit entry's GAF. Later features can never match a REUSE entry by file name.
    - Miss: load `anims\<filename>.GAF`, relocated in place (0 if missing), and strncpy the name into 0x98 (16 bytes).
  - **Sequences** 0xAC..0xC8: seqname, seqnameshad, seqnameburn, seqnameburnshad, seqnamedie, seqnamedieshad, seqnamereclamate, seqnamereclamateshad.
    - Missing or empty key → 0. Otherwise the first GAF entry whose name matches stricmp (0 if none).
    - For the six burn/die/reclamate pointers, sequence byte +2 is cleared.
  - Fields:
    - `spreadchance` 0xFB, `reproduce` 0xFC, `reproducearea` 0xFD: u8.
    - `metal` 0xF0 and `energy` 0xEC: float of `(uint)atoi & 0xFFFF`.
    - `damage` 0xEA: i16.
  - **Flags** u16 at 0xFE: bits 1–11 = `(atoi & 1)` of animating, animtrans, shadtrans, flamable, geothermal, blocking, reclaimable, autoreclaimable (**default 1**), indestructible, nodisplayinfo, nodrawundergray.
    - Bits 12–15 are never written: they come from realloc memory (0xB in the live dump).
    - Bit 11 is forced on when the requested name is DragonsTeeth, DragonsTeeth_Core, Fortification or Fortification_Core (stricmp).
  - `sparktime` 0xE8 = (i16) ftol(atof × 30.0).
  - `burnweapon` 0xE4 = pointer to the first weapon slot whose name matches stricmp; an empty string gives 0.
  - Per-def animation state at 0xCC/0xD8 is zeroed. If animating, it is initialised from seq / seqshad (`0x004B8B30`).
- **GAF in memory** (`Gaf_LoadAndRelocate` `0x004B8C60`):
  - Header {u32 version, i16 count @4, …, u32 entry offsets @12}.
  - Sequence entry {u16 frames, u8 @2, …, char name[32] @8, frame table}.
  - `Gaf_FindSequenceByName` `0x004B8D40` does a linear stricmp search.

### 3DO models (objects3d\*.3DO) — exact vs live game: 278 unit + 74 weapon model trees
- **Layout:** the file is loaded whole and used in place; every stored offset is file-relative and becomes a pointer (`Model3d_RelocatePointers` `0x004CB4C0`).
  - Piece header, 0x34 bytes: version, vertex count, primitive count, selection primitive (−1 = none), offset x/y/z, then file offsets of name, (unused), vertices, primitives, sibling, child.
  - Vertex = 3 × int32 (16.16). Primitive, 0x20 bytes: colour index, index count, (unused), file offset of u16 indices, file offset of texture name, then 12 more bytes.
- **Per-piece order:** relocate the sibling first, then the child, then sort this piece's primitives (`Model3d_SortPrimitivesByDepth` `0x004CB370`).
  - If a selection primitive exists, swap it into slot 0 and set the selection index to 0.
  - Bubble-sort primitives 1..n−1, ascending by average vertex Y. The average is the int32 sum divided by the index count, with C truncation. Only runs when n ≥ 3; a primitive with no indices would divide by zero.
- **Axis flip** (`Model3d_NegateAxesXZ` `0x004CB590`): negate X and Z of every vertex and every piece offset, across the whole tree. Applied to all unit and weapon models.
- **Height** (`Model3d_GetHeight` `0x004CB5F0`): over the piece and its siblings, take the max of (vertex Y + own Y offset) and (child-tree height + own Y offset). Starts at 0, so it is never negative. Wrapping int32 arithmetic.
- **Piece count** (`Model3d_CountTreeNodes` `0x0045AE80`): 1 + child count + sibling count.
- **Model table entry 0** ("None") is never written. The game leaves its pointer as uninitialised heap memory.

### Unit script VM (scripts\*.cob) — differential-tested against the original interpreter
Verified 2026-10-13 with `ta_compat/tools/orig_harness/cob_harness`:
- The tool runs the original routines inside a suspended TotalA.exe with logging callbacks; our `sim::ScriptInstance` gets identical callback return values.
- Real scripts: all 278 stock scripts × 3 seeds, 580,539 operations.
- Synthetic: 3,000 random programs that use every opcode, 164,145 operations.
- After every operation it compares the thread block, the animation table, statics, the callback log and the RNG state: **0 differences**.
- Implementation: engine VM `engine/sim/src/script_vm.cpp` (own op enum); importer `ta_compat/src/cob.cpp` (every code word decoded, so jumps into operands behave the same).
- **COB file** (`UnitScript_LoadCob` `0x004B2450`, loaded in place, offsets relocated):
  - u32 header: version (4), function count, piece count, code length (words), static count, sound count.
  - Then file offsets of: entry table (u32 code-word index per function), function name offsets, piece name offsets, code, and the sound table (8 bytes per entry, name offset in the second u32).
  - The game caches a simple checksum of the file.
- **Script object** (0x544): vtable, tick rate (display context +0xE8 = 30), COB, checksum, statics (u32 × count), piece anim table (19 u32 per piece), dirty flag, 8 threads × 41 u32 at +0x1C, then the live-thread count at +0x53C, **directly after the threads**. A stack overrun of thread 7 therefore writes the count.
- **Thread** (41 u32):
  - Header: state, pc, sp (−1 = empty), sleep ticks, wait piece, wait axis, thread being waited for, signal mask, return-sink pointer.
  - Then 32 stack words. The stack top is word 9+sp; locals are words 9+i, sharing the stack.
  - **States:** 0 free; 0x01000000 running; 0x021/0x022/0x024/0x028xxxxx waiting for turn / move / sleep / a called thread.
- **Start** (`0x004B08C0`): the first free thread gets state running, pc = entry, sp = −1, sink 0, signal mask 1, and the count goes up by one.
  - **Locals are not cleared** (stale values from the previous occupant).
  - Function index bounds-checked; −1 if no thread is free.
  - Name lookups are case-sensitive `strcmp`.
- **Entry points:**
  - `CallNamed` (`0x004B0940`, name, sink, run_now): run_now runs all 8 threads once with 0 elapsed ticks.
  - `InvokeByIndex` (`0x004B0B00`): a failed start returns 0 to the sink. Pushes 4 args, then sp = argc−1.
  - `CallInOut` (`0x004B0C40`): pushes 4 args, sp = 3, runs only that thread, then copies locals 0–3 back. It copies them even if the thread has ended.
  - `Tick` (`0x004B0D60`, ticks): if the count ≠ 0, run threads 0..7, then animate.
- **Run** (`UnitScript_ExecSlot` `0x004B0DA0`):
  - Waiting threads wake when the turn speed (or move speed) of their piece/axis reaches 0, or when sleep − ticks ≤ 0.
  - A woken thread runs in the same call. It runs until it sleeps, waits, returns or is killed.
  - Opcodes are matched on `word & 0x100FF000`. Push/pop kind = `word & 7`: 1 const, 2 local, 4 static. Other push kinds push a stale local of the interpreter; other pop kinds pop nothing.
  - **Opcodes:** 01 move, 02 turn, 03 spin, 04 stop-spin, 05/06 show/hide, 07/08 cache/don't, 09 legacy explode (piece + 2 pops), 0A play sound, 0B move-now, 0C turn-now, 0D/0E shade/don't, 0F emit sfx, 11/12 wait turn/move, 13 sleep, 21 push, 22 create local (sp+1, no init), 23 pop, 24 drop top.
  - 31–38 add, sub, mul, signed div (div by 0 crashes), and, or, xor, not.
  - 41 rand → `BattleRng_NextMod(hi−lo+1)+lo`. 42 get value(x,0,0,0,0); 43 get value with 4 args; 44 get transporting(x); 45 get carrier.
  - 51–56 `< <= > >= == !=` (signed). 57/58 logical and/or. **59 = bitwise xor (not logical).** 5A logical not.
  - 61 start-script (fn, argc). 62 call-script. 63 drop argc args. 64 jump. 65 return. 66 jump-if-zero. 67 signal. 68 set signal mask. 71 explode piece. 82 set value. 83 attach (3 pops). 84 drop.
  - Anything else: the thread is freed; waiters are **not** woken.
  - **Start/call args:** popped into the callee's locals argc−1..0, and the callee inherits the caller's signal mask. If no thread is free, **the args are not popped**, and a call-script waits on thread −1 forever.
  - **Return:** if the sink is nonzero, pop the value and pass it to the sink. Free the thread and wake every thread waiting on it.
  - **Signal:** frees every live thread whose mask & signal ≠ 0, and wakes their waiters. The caller may kill itself.
  - **Speeds and timing:**
    - Speeds and accelerations are divided by the tick rate (C truncation).
    - Sleep ticks = (int)(rate × ms) / 1000.
  - **Turn:** target &= 0xFFFF and accel is cleared. The speed sign follows the shortest path: d = target − current; negate when (|d| > 0x8000) ≠ (d < 0); speed 0 when d = 0.
  - **Move:** speed negated when target < current.
  - **Spin:** turn target = −1. Speed set immediately if accel = 0.
  - **Stop-spin:** spin target 0, accel = −(decel/rate). It does **not** mark the piece active or the table dirty.
  - **Program counter after an instruction:** most instructions re-read the pc after their stack traffic and add their length. Wait, sleep, create-local, drop-top, drop-args, set-signal-mask and jump-if-zero use the pc they started at. This matters when a stack underrun overwrites header words.
- **Animate** (`UnitScript_AdvancePieceAnims` `0x004B1C00`): only with ticks ≠ 0 and the dirty flag set. Clear dirty, then for each active piece, clear its active flag and, per axis 0–2:
  - **move:** new = cur + ticks×speed. Keep moving (active) while new is short of the target in the speed's direction, else snap to the target and set speed 0.
  - **spin accel:** speed += accel, clamped at the spin target (then accel = 0).
  - **turn:** if target = −1, spin forever (active). Otherwise r = remaining distance (target−cur, or cur−target, +0x10000, magnitude & 0xFFFF with its sign). If r > ticks×|speed|, keep going (active), else snap and set speed 0. The value is passed & 0xFFFF.
  - A piece still active sets dirty again.
- **Unit callback table** (vtable `0x004FD698`): +00 set move, +04 set turn, +08 visible, +0C cached, +10 shaded, +14 get move, +18 get turn, +28 legacy explode (no-op), +2C play sound (no-op), +30 emit sfx, +34 explode, +38 attach, +3C drop, +40 set value, +44 get value, +48 transporting, +4C carrier.

### Piece pose (per-unit object state) — differential-tested: 278 units, 110,000 operations, 0 differences
Verified 2026-10-13 with `tools/orig_harness/pose_harness`. It runs the original builder, the script piece callbacks and the sync inside the suspended exe, with `Mem_MallocSafe_IgnoreTag` patched to a bump allocator. It compares every piece field and every transformed vertex. Implementation: `engine/sim/src/pose.cpp`, plus `ta::to_pose_model`.
- **State** (0x22 header + 54 bytes per piece):
  - Header: piece count, cache-draw count, anim-dirty flag, owner, 2 draw-frame pointers, base angles **Z (pitch), Y (heading), X (bank)** at +0x18/+0x1A/+0x1C, root piece.
  - Piece: model node, move xyz (int), turn xyz (short), xform xyz (int), point list, skip flag (u16), flags (bit0 visible, bit1 cached, bit2 shaded), sibling, child, parent.
- **Build** (`Object_AllocCloneAndReorderByCob` `0x0045A950`):
  - Clone the model depth-first: node, its child subtree, then its sibling (`0x0045AEC0`). Each piece gets a copy of its vertices. Flags = cached|shaded, plus visible iff ≥3 vertices. Anim dirty = 1.
  - Then for each script piece i: take the first piece at index ≥ i whose model name matches (stricmp) and **swap** it into slot i. If no piece matches, slot i keeps whatever piece is there, so script index i addresses that piece.
  - Relink the tree (`0x0045AF90`): each model node's piece is found by searching from the **last** index down.
- **Callbacks** (unit script vtable):
  - set move/turn: if the value changed, store it, clear skip, set dirty, and reset the cache-draw count if the piece is cached.
  - set visible: if (flags&1) ≠ value, set bit0 = value&1, clear skip, and reset the cache count if cached.
  - set cached/shaded: set the bit and drop the draw frame (render only).
  - get move: int.
  - **get turn is zero-extended (0..65535)**. The reference program typed it as `short`, which is wrong.
- **Sync** (`Unit_SyncObjectStateAnimTree` `0x0045AB10`):
  - If any of |(short)(baseX−bank)|, |(short)(baseY−heading)|, |(short)(baseZ−pitch)| is > 7, store the new base angles and mark dirty.
  - When dirty: reset the vertices/xforms of the whole tree from the model (`0x0045B030`). Then transform child-first (`0x0045B0A0`).
    - Each piece uses angles (turnZ, turnY, turnX), plus the base angles **for the root only**, and translation move + model offset.
    - `0x0045B150` rotates the piece's xform and its vertices (last to first) with `Math_RotatePointByAngles` and adds the translation. It does the same for the whole subtree of descendants.
  - Clear dirty.
- **Skip flag:** never set nonzero anywhere in the exe. Every recompute is full.
- **Render coupling (open question):** `Unit_UpdateAndDrawModel` (`0x0045AC20`) performs the same base-angle check and recompute when a unit is *drawn*. The base angles a later simulation query sees can therefore depend on whether the unit was on screen in between (difference of at most 7 angle units). The engine syncs only at simulation query points.

### Heightmap (TNT terrain heights)
Implementation: `engine/sim/src/heightmap.cpp`, `ta_compat/src/tnt.cpp`. Verified through `GROUND_HEIGHT` in `unitvalue_harness`, using Moon Quartet's real heights.
- **TNT header** (u32): version (**0x2000** for TA maps; 0x1020 is also accepted), width and height in **height cells** (16 world units each; Moon Quartet is 610×616), map-data offset, attribute offset, tile-graphics offset, tile count, feature count, feature-table offset. For 0x2000 the header continues with sea level, minimap offset and flags.
- **Attributes:** 4 bytes per cell in 0x2000 maps (height byte, u16 feature index, pad) and 8 bytes in 0x1020 maps (height first).
- **Loading** (`Map_LoadTnt`): copies each height byte into the 13-byte map cell, at +4 (cells at GameState+0x14287, width +0x14233, height +0x14237).
- **Sampling** (`HeightMap_SampleAtWorldPos` `0x00485070`): takes the world X/Z integer parts as shorts.
  - cell = v>>4 (arithmetic), frac = v&15.
  - The cell and its +1 neighbours must be inside the map, else −1.
  - top = (h10−h00)·fx/16 + h00, bottom = (h11−h01)·fx/16 + h01, result = (bottom−top)·fz/16 + top, using C-truncating division.

### Unit values (script GET/SET, vtable +0x40/+0x44/+0x48/+0x4C) — differential-tested: 116,867 GET, 33,341 SET, 16,592 transport calls on all 278 units, 0 differences
Implementation: `engine/sim/src/unit.cpp` (`unit_get_value`, `unit_set_value`, `unit_set_activate`, `piece_local_offset`, `UnitScriptHost`). Checked by `tools/orig_harness/unitvalue_harness`, which uses a fake game state, real models and scripts, and the original builder.
- **Packed XZ** (ids 7, 9, 12, 13, 16): the X integer part goes in the high 16 bits and the Z integer part (`z>>16`, arithmetic) is added to the low half, so a negative Z borrows from X. Unpack: X = v & 0xFFFF0000, plus 0x10000 if (v<<16) < 0; Z = v<<16.
- **GET by id:**
  - **1** activation bit0.
  - **2/3** flags bits 18-19 / 20-21.
  - **4** health: (u32)(hp·100 as int32) / max_damage, an *unsigned* divide; division by 0 crashes.
  - **5/6** script flags bit0/bit1.
  - **7/8** piece X/Z packed, and piece Y.
  - **9/10/11** another unit's XZ, Y, and model height. Lookup: (short)arg = 0 → none; the table is indexed by arg&0xFFFF without a bound; the unit must have flags bit28 (alive); else 0.
  - **12** (u16)(atan2_scaled(X, Z) − heading).
  - **13** hypot(X, Z), truncated.
  - **14** atan2_scaled(a, b) & 0xFFFF.
  - **15** hypot(a, b), truncated.
  - **16** sample(X, Z) << 16 (−1 << 16 when outside the map).
  - **17** 0 if the build fraction == 0.0f, else 1 − trunc(fraction × −99.0f): 100 when fully unbuilt.
  - **18/19** script flags bit2/bit3.
  - **20** activate flags bit1.
  - Any other id gives 0.
- **Piece position** (`Unit_GetPieceLocalOffset` `0x0043DEF0` + `Unit_GetPieceWorldPos` `0x0043E060`):
  - Computed from the parent chain, **not** from the cached transforms.
  - Start with the piece's offset + move. For each ancestor: rotate by the ancestor's turn (the top ancestor also adds unit pitch/heading/bank to Z/Y/X), then add the ancestor's offset + move.
  - The piece's own turn is ignored, and a root piece is never rotated.
  - Z is negated on output, then the unit position is added.
  - An out-of-range piece gives (0,0,0), so only the unit position remains.
- **SET by id** (`0x00480B20`); every path sets net-dirty bit 4:
  - **1** activation (mask 1) and **20** armored (mask 2) go through `Unit_SetActivateState` (`0x0048B090`). It sets or clears the bits, then starts (without running) `Activate`/`Deactivate`/`StartBuilding`/`StopBuilding` for gained/lost bit0/bit3 in that order. It also queues speech, notifies path orders on gaining cloak, marks the HUD and broadcasts packet 0x11. Not harness-tested, because it needs the full game; written from the disassembly.
  - **5/6/19** set bit0/bit1/bit3 of the script flags.
  - **18** yard open (`Unit_SetYardOpen` `0x0047DAC0`):
    - Occupancy test with value≠0.
    - On success, flag bit2 = value&1 (so 2 tests "open" but stores closed), flags |= 0x08000000, claim cells, update passability. Implemented by the footprint system (see "Footprints and occupancy").
  - Other ids only mark the unit dirty.
- **Transport queries:**
  - +0x48 `0x00481430`: 1 if any cargo-list unit's u16 id equals the full 32-bit argument.
  - +0x4C `0x00481470`: carrier id (zero-extended), or 0.
- Sound (+0x2C) and the legacy explode (+0x28) are `RET` no-ops.

### Unit tick (per-step unit pass `Unit_TickAllPlayersScriptsAndWeapons` `0x0048AD30`) — differential-tested: 1,000 scenarios, 253,024 passes with real scripts, 0 differences
Implementation: `engine/sim/src/unit_tick.cpp` (`run_unit_pass`). Checked by `tools/orig_harness/unitloop_harness`, which builds in the suspended exe:
- three players and up to 16 units of random real types (models, scripts, object states, the original unit callback table);
- logging stubs for every subsystem not built yet;
- random wind/environment/tick values and random script calls.

After every step it compares the stub call log, all touched unit fields, every unit's script threads/animations/statics, piece poses, the RNG and the live count. 285 of the 1,000 scenarios ended early on endless script loops caused by random arguments; the original would hang the same way.

**Structure:**
- Live count (GameState+0x14353) is reset to 0.
- Player slots 0..9 (0x14B bytes at GameState+0x1B63) are visited in order. A slot is skipped unless active (+0) **and** its controller (+0x73) is 1/2/3 **and** its state (+0x146) ≠ 10.
- Its units are walked by address, from +0x67 to +0x6B inclusive (stride 0x118). Units with type index (+0xA6) = 0 are skipped.

**Per unit, in this order (every callee is callee-cleaned):**
1. Count it.
2. **Wind** (`Wind_NotifyUnitScript` `0x00437910`): if the type's wind generator > 0.0f and wind changed (GS+0x37EE2), queue `SetDirection(heading u16)` then `SetSpeed(speed << 4)`. These are by-name calls with 1 argument; they run in this step's script pass.
3. Weapons (`0x0049E1A0`), only if the *slot* player is active with controller 1|2.
4. **Script step** `UnitScript_Tick(1)`, if the unit has a script.
5. Damaged timer (+0xFA, u8) and transfer hold-off (+0xFB, u32) count down to 0.
6. **Selection** bit 0x10 is kept only if bit 0x20 is set, the build fraction == 0.0f, the hold-off (after its decrement) is 0, and there is either no carrier or the carrier has flag 0x40000000. Otherwise it is cleared.
7. **Health %**, if tick % 30 == 0: (u64)(u32)(hp·100) / max damage (unsigned). A result negative as signed becomes 0; above 100 becomes 100. Previous (+0xF7) = old value; current goes to +0xF6.
8. If the *owner* player (+0x96) is active with controller 1|2:
   - **Water damage** (kind 0xB, from no attacker): applies when the schema's water-does-damage (+0xD4C) and water damage (+0xD50) are set, tick % 30 == 0, (short)(posY >> 16) ≤ the sea-level byte (signed word compare), and type flags bit 12 is clear.
   - **Self-heal**: when heal time (+0x200) ≠ 0, (u32)hp < max damage (an *unsigned* compare, so negative hp never heals) and tick & 7 == 0: heal(unit, unit, (float)(heal·8/30)) with C division.
   - Primary orders `0x0043B7C0`, then secondary orders `0x0043BAD0`.
   - If the unit has a motion object: motion tick `0x0043DD20` (thiscall on it), then ground settle `0x0048A870`.
9. If flags bit 14 (dying): `0x004864B0`(unit, last damage kind +0xF5).

**After each slot's units:** if GS+0x2A44 bit0 is set and the slot player is active with controller 1|2, broadcast its units (`0x0048B710`).

**After all slots:** camera auto-follow, every 90 steps while GS+0x14373 bit1 is set (UI; it reads the keyboard for Shift).

### Damage, healing and death — differential-tested: 3,000 scenarios, 600,000 unit passes, 656,956 combat operations, 26,878 kills, all 278 unit types, 0 differences
Implementation: `engine/sim/src/combat.cpp` (`apply_damage`, `apply_damage_packet`, `repair_heal`, `kill_unit`, `apply_killed_packet`). Checked by `tools/orig_harness/combat_harness`. It works like `unitloop_harness`, but the original damage, heal and kill routines run for real. Random direct damage, heals, kills and received damage/kill packets are mixed with unit passes. After each operation it compares:
- the stub log, including network packet bytes;
- unit fields (hp, flags, type index, kill count, last attacker and slot, last damage kind, metal income, timers);
- player counters;
- scripts and poses.

All 278 unit types are used, including the 22 whose script has no `Killed` function. The harness presets the stale stack word (below) before each unit pass and direct kill, reads it back afterwards, and compares it too.

**Unit fields used:** +0xB8 kill count (u16), +0xBC the unit's resource slot (metal income float at +0xD4, its player at +0xEC), +0xF0 last attacker, +0xF4 last attacker's player slot (10 = none), +0xF5 last damage kind, +0xFF owner slot.
**Player fields used** (0x14B record):
- +0x04 net id; +0x27 lobby info (side byte at +0x95);
- u16 counters: +0xFC units killed, +0xFE units lost, +0x104 commanders killed, +0x106 commanders lost;
- +0x129 shares-vision bytes (indexed by the other player's +0x146);
- +0x144 alive units (u16); +0x149 flags (bit 0 = still has its commander).
**Side commander names:** GS+0x37F5F + side × 0x232.

**Damage packet** (9 bytes): `0x0B`, target id u16, attacker id u16 (0 = none), amount u16, heading high byte, kind byte.
**Kill packet** (11 bytes): `0x0C`, unit id u16, attacker's net id i32, attacker id u16, severity byte, (kind << 4) | wreck level.

**Apply damage** (`Unit_ApplyDamageAndBroadcast` `0x00489BB0`, stdcall attacker, target, amount, kind, heading):
- **Heal (kind 10):** the amount passes through unchanged.
- **Other kinds:**
  - If the *target* is armored and the amount < 30000, the amount becomes (int64 product of the type's damage modifier at +0x1AA and the amount) >> 16, arithmetic.
  - The *target's* kills give a tier = min(kills / 5, 5).
  - The amount sent is ((25 − tier) × amount × 4, wrapping in 32 bits) / 100, truncated, stored as 16 bits. Veterans take up to 20% less.
- **Sending:** it builds the packet and applies it locally. Then, if the target's owner is active with controller 3 and kind ≠ 11, it sends the packet with a net id: the attacker owner's (+4), or with no attacker the first active player with controller 1|2 (`0x0044FDB0`, −1 if none).

**Apply damage packet** (`0x00489CE0`): it looks units up by id (0 = none). It does nothing unless the target is alive and not dying (0x4000).
- **Kind 10:** hp + (u16)amount. If the sum ≥ max damage, compared *unsigned*, hp = max. A negative heal therefore fills the unit.
- **Otherwise**, in this order:
  1. Damaged timer (+0xFA) = 0xF0 (`0x00467950`).
  2. If kind ≠ 11, the retarget hook (`0x00406F80`, attacker, victim, amount) runs.
  3. +0xF5 = kind.
  4. With an attacker: +0xF0 = attacker, +0xF4 = attacker's owner slot. If either owner is the local player (GS+0x2A42), the statistics hook (`0x00494FF0`, 1) runs.
- **Kind 2 (paralysis):** no hp change. If the target is still alive and not dying, its owner plays (1|2) and its type flags lack bit 26, the "paralyze" mission order gets the amount: it is added to the head order (+0x5C) if that order already is one, otherwise a new one is prepended. Engine: `add_paralysis` hook.
- **Other kinds:** hp −= amount (16 bits).
  - If hp ≤ 0 and the owner plays (1|2), the unit becomes dying (flags |= 0x4000) and the routine stops; the unit pass kills it.
  - If hp ≤ 0 otherwise, hp = 0.
  - **Kind 1 (weapon):** queue `HitByWeapon(cos·400, sin·400)` from the heading byte << 8, using the rounded multiply variants. Then queue `TakeDamage(pct)` with pct = (u32)(hp·100) / max, clamped 0..100.

**Repair / self-heal step** (`Unit_ApplyRepairHealProgress` `0x0041BD10`, stdcall worker, target, float rate). It returns 0 if (int)hp ≥ (int)max. All type values are the *target's*:
- Hit points: trunc((max × rate − 1) / build time (+0x1EA) + 1). Energy: the same with the build energy cost (+0x186) for max.
  - Each operation rounds to double (x87 at 53 bits). Truncation keeps the low 32 bits of the int64 result; build time 0 gives 0.
  - Each result is then **capped at 1**, so a step heals at most 1 hp.
- The energy goes to the economy hook (`0x00401180`, &worker+0xBC, (float)energy). Only if that succeeds: apply damage (worker, target, hp, kind 10, 0), and return 1.

**Kill a dying unit** (`Unit_BroadcastKilledPacket` `0x004864B0`, unit, kind). It does nothing unless the unit is alive.
- **Commander:** the side's commander name is matched case-insensitively against the type name (def+0x20). For the commander, owner flags bit 0 is cleared.
- **Severity and wreck:**
  - Kind 7: wreck 1, severity 0.
  - Kinds 4, 5, 9, or hp > 0: both 0.
  - Otherwise: severity = ((u32)(−hp·100) / max + previous health % (+0xF7)) / 2 as signed, clamped 1..100. Then the script runs `Killed(severity, wreck)` in/out at once (CallInOut). Both values may change.
  - **Quirk:** the wreck variable is uninitialised before that call, and the in/out call leaves it alone when the script has no `Killed` function. Its value is then the caller's stale stack word (next section). The engine takes that word as a parameter, and after the routine the word holds the final wreck level.
- If the build fraction ≠ 0, wreck = 0.
- It builds the kill packet: the net id is from the last attacker slot via `0x0044FFD0` (−1 for slot 10 or a slot with controller 0).
- If the owner plays (1|2), it sends 11 bytes with the owner's net id.
- Then it applies the packet with the death script skipped (the script already ran).
- Afterwards: if it was the commander, the game mode (GS+0x37EF6) ≠ 0 and the owner plays, the commander-death hooks run (`0x00491D70`(1), then `0x00486F10`(owner slot)).

**Apply kill packet** (`Unit_HandleKilledPacket` `0x004866D0`, packet, skip_script). It does nothing unless the unit is alive. In order:
1. If the owner's +0x146 equals GS+0x2A43, the reveal hook runs (`0x00482910`, &pos, def+0x202, def+0x170, 60).
2. Last attacker = unit by id. Last attacker slot = the first slot whose net id (as `0x0044FFD0` gives it) equals the packet's (`0x0044FE40`; −1 or no match gives 10).
3. Hooks: victory (`0x004904C0`), clear orders (`0x00439EB0`, 1), speech (`0x0047F8C0`), group −1 (`0x00480250`), projectiles (`0x0049C880`).
4. If carried: detach (`0x0048AAC0`, unit, 0, −1, 1).
5. Each cargo unit takes damage (last attacker, 30000, kind 3 if the kill type is 3 else 6), then is detached. This loops while the cargo head is set.
6. Footprint hook (`0x0047CBD0`). If map flags (GS+0x14281) bit 1 is set, the fog hook (`0x00482090`).
7. If !skip_script and the signed severity > 0: queue `Killed(severity)` with run-now.
8. **Statistics by kill type** (the high nibble):
   - Types 1 and 6, and type 5 when the attacker slot is neither 10 nor the owner:
     - owner units lost +1;
     - if the attacker slot ≠ 10, the unit is fully built and the slots differ: attacker player units killed +1;
     - for a commander: attacker commanders killed +1 (slot ≠ 10), owner commanders lost +1;
     - if there is a last attacker, the unit is fully built and the slots differ: **attacker unit kills +1**;
     - if the attacker slot is the local player: statistics hook (5).
   - Type 3 (self-destruct): owner units lost +1 (and commanders lost for a commander), unless the local player's shares-vision byte for the owner's +0x146 is set.
   - If counted and the attacker slot ≠ 10: kill-lead messages (network games only, provider type 2/3), then the kill flash if GS+0x37F06 bit 7 is set (`0x004948B0`, slot, owner +0x146). Engine: `kill_counted` hook.
9. **Reclaim (type 5) with a last attacker:** m = (1 − build fraction) × metal cost (+0x18A), in doubles. The attacker's metal income (+0xD4) becomes, rounded to float:
   - income − m × −0.5 for an AI (controller 2) on difficulty (GS+0x37EEE) 0;
   - income − m × −0.7 for an AI on difficulty 1;
   - income + m otherwise.
10. If severity > 0 and the unit is fully built: detonate (`0x0049B000`, unit, kill type == 3).
11. If wreck ≠ 0: spawn the wreck (`0x00486360`, unit, wreck, kill type ≠ 7).
12. **Teardown:**
    - release hook (`0x00489740`);
    - delete the script, the object state and the motion object;
    - type index = 0; clear alive and bits 0x30; the def becomes the game state's blank def (GS+0x1439B);
    - owner alive count −1. At 0, the network/AI announcements run (provider types 3/2).

### Unit orders (mission order lists) — differential-tested: 3,000 scenarios, 300,000 unit passes, 366,066 order operations, 734,124 behaviour steps, 0 differences
Implementation: `engine/sim/src/orders.cpp` (`sim/orders.h`). Checked by `tools/orig_harness/orders_harness`. It builds the mission table with the original registration code, then a battle like `combat_harness` (real types, scripts, models, Moon Quartet terrain). The original order processing, constructor, insertion, removal, clearing, paralysis path, ground settle and slope fit run for real. Every table entry's behaviour is replaced by one logging stub that plays back a scripted list of results, wait bits, wake ticks and event-slot writes; the engine's behaviour hook plays back the same list. Each step applies random order operations and direct calls (create/insert, primary, secondary, settle, clear, remove, paralysis damage, kill, state changes), then one unit pass. After each one it compares:
- the stub log (behaviour calls with unit, order address, event and kind; weapon-target clears/releases; goal/attached-object calls; frees of orders; network notices);
- every field of every order of both lists, by address (the harness engine allocator hands out the same addresses as the original's bump heap);
- each unit's target-link list, unit flags, pending events, position, pitch/heading/bank, scripts, the battle RNG, the clock reads and the stale word.

**Mission table** (`MissionTable_RegisterBuiltins` `0x0043C050`, a vector at `0x00512344`/`0x00512348`, entries 0x19 bytes): the Ready entry, then three static blocks (22 Standby-family, 22 air, 23 Stop-family entries), each append followed by an introsort by the lookup name (+0x15, case-insensitive). The names are unique, so the result is simply sorted: 68 entries, index = mission kind (`sim::mission::Kind`; `ta::UnitDefContext::default_mission_names` has the names). Entry fields: +0 display name, +4 behaviour (stdcall `(unit, order, event)` → result), +8 a drawing callback, +0xC draw mask, +0x10 a byte, **+0x11 the default order flags** (`sim::mission::default_flags`), +0x15 lookup name. Verified entry by entry against the engine table (names, flags, behaviour addresses).

**Order object** (0x56 bytes, from `0x004B4F10`, freed with `0x004B4F20`):
| Offset | Field |
|---|---|
| +0 | vtable (not state) |
| +4 | kind (u8) |
| +5 | step counter (u8) |
| +6 | wait mask: bit 0 timer, bit 1 run the behaviour once more with event 2 when destroyed; any bit can be woken by an event (primary list) |
| +0xA | wake tick (u32, −1 = none) |
| +0xE | the unit whose list holds it |
| +0x12 | target link {vtable, unit, next link, owner order} (16 bytes) |
| +0x22 | position (3 × 16.16) |
| +0x2E, +0x32 | behaviour state, zeroed by the constructor |
| +0x36, +0x3A, +0x3E | parameters (+0x36 = amount for paralysis) |
| +0x42 | flags: 0x4 kept by a partial clear, 0x200 has target, 0x400 has position, 0x4000 idle (made because the unit had nothing to do; inherited from the order it is put in front of), 0x10000 destroyed while not the primary head, 0x40000 lives in the secondary list, 0x400000 building (stop the building script when destroyed), 0x800000 set by result 9 |
| +0x46 | creation tick |
| +0x4A | next order |
| +0x4E | events received (bit 0 = timer fired) |
| +0x52 | movement goal object attached by the behaviour (owned; deleted on destroy) |

**Unit fields:** +0x5C primary list, +0x60 secondary list, +0xA2 head of the list of target links pointing at the unit (newest first), +0xBA pending event bits (u16, the same word as the script SET "dirty" bits), +0xAA hover bob phase.

**Target links** (`UnitLink_Init` `0x004895C0`, `UnitLink_Release` `0x00489650`, `UnitLink_Set` `0x00489690`): a link to a unit is pushed on the unit's +0xA2 list only if the unit's table slot is occupied (type index ≠ 0); release walks that list to unlink it.

**Construct** (`MissionOrder_Construct` `0x0043A0C0`, thiscall `(kind, target, pos*, p0, p1, p2)`): link the target; zero +0x2E/+0x32; params; creation tick = game tick; link owner = the order; step, wait, events = 0; wake = −1; position = *pos or 0; unit = 0; flags = table default; next = 0; attached = 0. No target argument → clear 0x200; no position → clear 0x400; then without 0x200 the target link is dropped again (so kinds without 0x200 never keep a target).

**Insert** (`Unit_InsertMissionOrder` `0x0043AC60`, `(unit, order, before)`): in the list chosen by the order's 0x40000 flag, walk to the link pointing at `before` (null = end), link the order there, set its unit and next, and OR in `before`'s 0x4000. **Prepend** (`0x0043ACB0`) inserts before the current head.

**Destroy** (`MissionOrder_Destroy` `0x0043A1F0`, then free), in order:
1. wait bit 1 → behaviour(unit, order, 2), result ignored;
2. flag 0x400000 → start the unit script's `StopBuilding` (by index, not run now, no arguments), network notice `0x00456190`(unit, function index), clear the flag;
3. if the unit has a motion object: its goal object (motion +0) is asked; if the goal's current attachment (goal +4) is the order's +0x52, the goal's method +4 is called with 0 first; then the attached object is deleted (virtual destructor, 1) and +0x52 = 0. Engine hook `release_attached`;
4. without flag 0x10000: release the weapon targets the order gave (`Unit_ReleaseWeaponTargets` `0x00489800`(unit, 3): slots 0, 1 then 2; an enabled slot without flag 0x10 gets flag 0x10 and forgets its target like `0x0048A0F0`; `sim::release_weapon_targets`, verified in `orders_harness`);
5. unlink the target link.

**Remove** (`Unit_RemoveMissionOrder` `0x00439F80`): search the list named by the order's own 0x40000 flag; if found, unlink, flag 0x10000 unless it is the current primary head, destroy. Not found → nothing.
**Clear** (`Unit_ClearMissionOrders` `0x00439EB0`, `(unit, all)`): every primary order (or only those without 0x4 when `all` = 0) is unlinked, flagged 0x10000 unless it was the first, destroyed; with `all`, then remove the secondary head until the list is empty (an order whose 0x40000 flag is clear is never found there, which would loop forever, as in the original). Called with 1 on death and for unknown behaviour results.

**Primary orders** (`Unit_ProcessPrimaryMissionOrders` `0x0043B7C0`), looping on the current head:
1. if game tick ≥ wake (unsigned): wake = −1, events |= 1;
2. woken = (events | pending u16) & wait; if wait ≠ 0 and woken = 0, return;
3. pending &= ~woken (16 bits), wait = 0, events &= ~woken;
4. woken bit 0x10000 → clear the targets of weapon slots 0, 1, 2 (`Weapon_ClearTarget` `0x0048A0F0`: if the slot has a target, set it to none (id 0, z = −0x8000), look up `StartBuilding` without running it, and queue `TargetCleared(slot)`; `sim::clear_weapon_target`, verified in `orders_harness`);
5. result = behaviour(order's unit, order, woken), then:
   - 0: step = 0; 1: step + 1; 2, 4: nothing;
   - 3: sleep: r = random(15) (battle RNG), wait |= 1, wake = tick + r + 30;
   - 5, 8: remove (as Remove, but the head check uses the head read before unlinking). An order whose 0x40000 flag sends the search to the other list stays and runs again;
   - 6: move the order to the back of the primary list;
   - 7: destroy every primary order (0x10000 for all but the first), remove every secondary order, return;
   - 9: flag 0x800000; if it is the last order: step = 0 and sleep 30 + random(30); else remove;
   - anything else (unsigned > 9): clear all and return.

   The loop continues with the (new) head, so a behaviour that neither sleeps nor finishes runs again at once.
6. Empty list: if the owner (unit +0x96 player) is active with controller 1|2 and the type's default mission (def +0x230) ≠ 0, a new order of that kind (no target, no position, params 0) gets flag 0x4000 and goes in front of the head of its list (the secondary head if its default flags say so).

**Secondary orders** (`Unit_ProcessSecondaryMissionOrders` `0x0043BAD0`): walk the list; an order with wait ≠ 0 and tick < wake is skipped. Otherwise wait = 0, result = behaviour(order's unit, order, 0), and after handling the result the walk **restarts from the head**. Results: 0/1/2/3/4 as above (3 = sleep 30 + random(15)); 5, 8, 9 and unknown: remove if found; 6, 7: remove if found, then return.

**Paralysis order** (from the damage packet, kind 2): kind = `MissionOrder_FindByName("PARALYZE")` = 27; if the primary head is that kind, its +0x36 += amount; else construct (27, no target, no position, amount, 0, 0) and prepend. Engine: `sim::add_paralysis` (the combat `add_paralysis` hook calls it). Verified through the real damage routine.

**Behaviours (hooked; `OrderSystems::run_mission`).** Handler addresses by kind: 0 Ready `0x439EA0` (returns 5), 1 Activate `0x403010`, 2 AirStrike `0x411F50`, 3 AirToAir `0x412D40`, 4 AirToGround `0x412710`, 5 AirToGroundHover `0x413470`, 6 Attack_Chase `0x4034A0`, 7 Attack_Kamikaze `0x403260`, 8 Attack_NoMove `0x402160`, 9 AttackSpecial `0x403190`, 10 AttackUType `0x401E00`, 11 BeCarried `0x402FC0`, 12 BuildingBuild `0x402640`, 13 BuildWeapon `0x402B70`, 14 Capture `0x404270`, 15 Cloak_Off `0x4030A0`, 16 Cloak_On `0x403070`, 17 Deactivate `0x403040`, 18 Follow_Ground `0x406300`, 19 GetBuilt `0x402DA0`, 20 Ground_Pickup `0x406780`, 21 Ground_Unload `0x406900`, 22 Guard_NoMove `0x4021F0`, 23 HelpBuild `0x403F70`, 24 MakeSelectable `0x401CC0`, 25 MobileBuild `0x403A20`, 26 Move_Ground `0x4031D0`, 27 Paralyze `0x402D10`, 28 Park `0x4061A0`, 29 Patrol `0x4033A0`, 30 QMove and 31 QPatrol `0x403160`, 32 Reclaim `0x404AD0`, 33 ReclaimUnit `0x404730`, 34 RepairPatrol `0x405980`, 35 RepairUnit `0x405300`, 36 RepairUnitNoMove `0x405740`, 37 Resurrect `0x404DB0`, 38 SelfDestruct and 39 SelfDestructFG `0x402010`, 40 SelfRepair `0x402430`, 41 Standby `0x405FE0`, 42 Standby_Mine `0x406090`, 43 Standing_FireOrder `0x403100`, 44 Standing_MoveOrder `0x4030D0`, 45 Stop `0x401C20`, 46 Suppress `0x4038A0`, 47 Teleport `0x406AA0`, 48 VTOL_Evade `0x413BC0`, 49 VTOL_Follow `0x40FBE0`, 50 VTOL_GetRepaired `0x415250`, 51 VTOL_HelpBuild `0x414380`, 52 VTOL_LandIfCan `0x40F2A0`, 53 VTOL_Landing `0x4118E0`, 54 VTOL_MobileBuild `0x413D80`, 55 VTOL_Move `0x40FA20`, 56 VTOL_Patrol `0x410E70`, 57 VTOL_Pickup `0x4111B0`, 58 VTOL_Reclaim `0x414770`, 59 VTOL_ReclaimUnit `0x414A80`, 60 VTOL_RepairPatrol `0x4152F0`, 61 VTOL_RepairUnit `0x414E70`, 62 VTOL_SeekAttack `0x4103E0`, 63 VTOL_SeekGuard `0x410850`, 64 VTOL_Standby `0x40F7D0`, 65 VTOL_Unload `0x411560`, 66 Wait `0x401CE0`, 67 WaitForAttack `0x401FD0`. None is implemented yet. Common helpers seen in them: set a deadline (`0x00439E80`: wait |= 1, wake = tick + n), attach a goal object (`0x004388D0`).
- **Event slot reuse (stale word):** a static scan of the decompiled behaviours shows these reuse their third argument as a local and so change what the secondary list leaves behind: BuildingBuild, GetBuilt, MobileBuild, Park, VTOL_Follow, VTOL_MobileBuild and VTOL_RepairPatrol (a stack address there; run-varying). Teleport's decompile was not checked. Their implementations must write the slot like the original.
- Real-time inputs: none in the list code itself.

### Ground settle — differential-tested within the orders harness: 52,577 direct settles (11,169 slope fits, all paths) plus every unit pass, 0 differences
Implementation: `engine/sim/src/ground_settle.cpp` (`sim::settle_on_ground`, `sim::fit_to_slope`), footprint import `ta::selection_footprint` (`ta_compat/src/model3do.cpp`).

**Settle** (`Unit_UpdateGroundHeight` `0x0048A870`, called after the motion step):
- Runs only if unit flags bit 16 (resettle) is set **or** type flags bit 12 (hover). It clears flags bit 16, then continues only with a motion object and (flags & 3) = 1 (ground layer, using the flags before the clear).
- Type bit 20 (terrain only): without bit 12, Y = sample(pos) << 16 (fraction cleared; −1 << 16 off the map). With bit 12: surface = sea level − waterline (def +0x22C, both as unsigned bytes, signed result); if sample > surface, Y = (second sample) << 16, else Y = surface << 16.
- Else type bit 19 (waterline): Y = (waterline × 0xFFFF + sea level) << 16, which wraps to (sea − waterline) << 16.
- Else the slope fit.

**Slope fit** (`Unit_FitToSlope` `0x0048A490`):
- Model = GS+0x14377[type index]; its selection primitive index (+0xC) < 0 → nothing. Its first four vertex index slots (read even if the primitive has fewer) give four corners; their X and Z (after the load-time negation) are the footprint.
- For each corner i: rotate (x, z) by the heading (`Math_Rotate2DPoint`); world X = (short)((x + posX) >> 16), Z = (short)((posZ − z) >> 16) (32-bit wrapping). Cell = coordinate >> 4 as unsigned; if cell X ≥ width − 1 or cell Z ≥ height − 1, **stop without changing anything**. Bilinear height as in the heightmap sampler (C-truncating /16).
- **Hover** (type bit 12, unit alive bit 28 and not dying): height = max(height, sea level) (signed); then a bob: clock = `0x004B6340` (wall-clock ms × tick rate / 1000 — **not deterministic**, an engine input), angle = (((clock & 31) + 8i) << 11) + hover phase (unit +0xAA) in 16 bits; half = max velocity (def +0x192) / 2 (C division), speed = min(motion +0x20, half); ratio = (int32)(((int64)speed << 16) / half) (the original faults if half = 0); amplitude = 2 − (int32)(((int64)ratio × 2) >> 16); t = min(tick − motion +0x2A, 60) (unsigned); height += mul_cos(angle, amplitude − (u32)(amplitude × t) / 60) (rounded multiply).
- Result: front = (h0 + h1) / 2, back = (h2 + h3) / 2 (C division, 32-bit wrapping sums); the **integer half only** of Y (word +0x70) = (front + back) / 2; bank (+0x68) = atan2_scaled(back − front, |z0 − z3| >> 16); pitch (+0x64) = atan2_scaled(h0 − h1, |x0 − x1| >> 16), with the raw (unrotated) footprint coordinates; |INT_MIN| stays negative.

**Motion object (unit +0; not built yet, learned on the way):** the pass calls `0x0043DD20` as thiscall on it with the unit pushed. +0 = goal object (vtable method +4 = attach/detach an order's goal object; +4 = the current attachment), +0x20 = current speed (16.16), +0x2A = step the current movement started. The clock `0x004B6340` calls the import at `0x004FC0DC` (wall-clock milliseconds) × display tick rate (`0x0051FBD0`+0xE8) / 1000.

### Stale stack word (the wreck level of units without a `Killed` function)
The kill routine's wreck variable is the dword 0x10 below its entry stack pointer. Called from the unit pass, that is **0x1C below the pass's frame** (after its four register pushes). Its value is whatever the routines the pass called last left at that depth. The engine carries it as `UnitTickEnvironment::stale_word` across passes. Every hook the pass calls gets it by reference and must update it like the original routine does.

**Verified effects** (compared after every pass and direct kill in `combat_harness`):
- **Wind notification** (when it fires): the last pushed argument, `wind speed << 4`.
- **Script step:** if the live-thread count was ≠ 0 at its start, 7 (the last thread index pushed). Then, if the animation step ran a rotation (turn speed ≠ 0, including endless spins), the turn target of the last rotation processed, as a u32 (a local of `0x004B1C00` at its entry − 4).
- **Water damage:** the damage packet's kind byte (0x0B) replaces the low byte. The other three bytes stay.
- **Self-heal step:** a saved register, **the unit's address** in the original's unit table (`table + id × 0x118`).
- **Kill:** the final wreck level (1 for kind 7; 0 for kinds 4, 5, 9 or hp > 0, or when not fully built; else whatever `Killed` returned or the stale value). If the unit is not alive, unchanged.
- **Weapons `0x0049E1A0`:** the y of the last target position it resolved (or the fire routine left), else unchanged; see "Weapons: slot pass and aiming" (`weapons_harness`).
- **Stubbed / not built yet:** motion, broadcast and camera. The harness stubs never reach that depth.

**Verified in `orders_harness`** (word compared after every pass and every direct call; 300,000 passes, 0 differences). For a one-argument routine the word is its entry − 0x14:
- **Primary orders `0x0043B7C0`:** always the saved EDI, which in the pass is **the slot player's record address** (GS + 0x1B63 + slot × 0x14B, the slot whose unit range is being walked, not the unit's owner). Engine: the hook sets `addresses.player(env.pass_slot)`.
- **Secondary orders `0x0043BAD0`:** unchanged with nothing due. Otherwise the last of: the behaviour's third argument slot after it returns (pushed as 0; behaviours that reuse that slot as a local leave their value, see Unit orders), 15 after a random snooze, **the order's heap address** after destroying one (the argument of the free call). Heap addresses vary between runs; the engine gives each order an `address` from its allocator hook.
- **Ground settle `0x0048A870`:** the return address of the last call it made: `0x0048A8FF` (water-level sample, sample ≤ surface), `0x0048A909` (second sample), `0x0048A92C` (terrain-only sample) or `0x0048A96F` (slope fit). Unchanged on paths with no call. So a dying mobile unit without `Killed` usually gets wreck level 15, 9 or 12.
- **Motion `0x0043DD20`, weapons `0x0049E1A0`:** not traced.

**The original is not deterministic here.** The game state block is a heap allocation, and its address varies between runs on the same machine: two skirmishes on Windows 11 measured 0x025F818E and 0x025E86AB (unit table 0x0E7C9020 and 0x0E6FD020; its low 12 bits look stable). The value left by primary orders depends on that address.
- The engine keeps both addresses in `OriginalAddresses`; the defaults are the first measurement.
- `ta_capture` writes a `layout` event with the live addresses at the first simulation step.

**Other callers of the kill routine** each have their own stale word: `0x00486F10` (kill a player's units), `0x004861D0`, `0x00485980`. They are not built yet.

### Weapon ballistics (pitch solver `0x0049A890`) — differential-tested: 1,428,570 calls (519,193 solvable), 0 differences
Implementation: `sim::ballistic_pitch` (`engine/sim/src/weapons.cpp`), `sim::x87::crt_acos` / `fpatan(double, double, bits)` / `compare` (`x87_math.cpp`). Checked by `tools/orig_harness/ballistic_check`, which batches the original solver and the C runtime acos inside the suspended exe. Gravities tested: 1, 50, 112, 300 and 4000.

**Inputs:** stdcall (dx, dy, dz, speed, float min_angle). Gravity g (int) is read from GS+0x14263. Returns a pitch in half-turns of 32768, or 0x8000 in AX when there is no solution (the upper half of EAX is garbage).

**Evaluation** (every operation rounded to a double, PC53):
- h = C-runtime hypot(dx, dz). Then:
  - H2 = h², V2 = speed², D2 = dy², H4 = H2²;
  - G2 = (int32)(g·g) with 32-bit wrap;
  - S = D2 + H2.
- disc = ((V2 − (g·dy)·(−2))·V2 + D2·G2)·H4 − (H4·G2)·S.
  - Fails if not ≥ 0 (NaN fails too).
- R = √disc, B = (V2 + g·dy)·H2.
- Two candidates: Q_flat = (R + B)/(S + S) and Q_steep = (B − R)/(S + S).
- Each angle = acos(√Q / speed). Q ≤ 0 (or NaN) gives the constant `0x3FF921FB54442D11` (π/2, stored slightly low).
- The flat angle is stored as a double. The steep one stays on the FPU stack; at PC53 it is a double anyway.
- **Choice:**
  - Take the flat angle if min_angle < flat (NaN counts as true) and !(flat > `0x3FE921FB54442D11`) (π/4, stored low).
  - Otherwise the steep angle, if steep > min_angle and steep ≤ π/4.
  - Otherwise fail.
- **Result:** truncate(angle × 32768 × `0x3FD45F306DC9C889`) (1/π, stored slightly high).
  - **A NaN angle yields pitch 0** (integer indefinite truncated to 16 bits), not a failure.
  - Speed 0 gives an infinite cosine, so acos is NaN, so pitch 0.

**C runtime acos (`0x004E67F0` → `0x004E680D`):**
- For |c| < 1: FPATAN(√((1 + c)(1 − c)), c), with each step rounded to a double.
- c = 1 gives 0; c = −1 gives FLDPI.
- |c| > 1, infinity and NaN give NaN, plus the CRT error path (errno/matherr).
  - That path crashes in the suspended harness, where the C runtime is uninitialised, so gravity 0 and inputs beyond ±1 are not tested.
- **Under precision control 53, FPATAN (and FLDPI) results are rounded to 53 bits.**
  - The best model is double rounding: the correctly rounded 64-bit value is rounded again to 53 bits. Single rounding: 271 per million differ; double rounding: 10 per million.
  - All 10 remaining are knife-edge cases. The exact angle lies within ⅛ unit of a 64-bit halfway point that also decides the 53-bit result. There the CPU's own FPATAN rounding decides (Intel guarantees < 1 ulp, not correct rounding), as with FSINCOS. The harness classifies them separately.
  - A last-bit difference in the angle cannot change the pitch unless angle × 32768/π is also within ~10⁻¹² of an integer.
### Weapons: slot pass and aiming — differential-tested: 3,000 scenarios, 300,000 unit passes + 69,483 direct slot passes, 242,937 helper calls, 15,025 shots, 16,789 broadcast aim starts, 0 differences
Implementation: `engine/sim/src/weapons.cpp` (`process_weapon_slots`, `weapon_target_position`, `sweet_spot_position`, `aim_from_position`, `weapon_piece_position`, `direct_aim`, `weapon_in_range`, `aim_within_tolerance`, `weapon_target_unit`, `use_resources`); slot and type data in `engine/sim/include/sim/weapon_slot.h`. Checked by `tools/orig_harness/weapons_harness`: like `combat_harness`, but every unit gets three slots with weapon types from the real weapon table (a third of them with flipped flag bits, other ranges, costs, reload times, tolerances or speeds), random targets (none, ground points inside and outside the map, live units, empty table entries), kill counts, motion velocities and player resources. The original slot pass runs inside the unit pass and is also called directly; the helpers are called on their own (target position, tolerance with boundary values, query-piece position, range check at the range limit, direct aim), and poses are re-synced at random as drawing would. Fire routines, the stockpile display and network sends are logging stubs (the fire stub also logs the target position it receives). After every operation it compares the stub log, all three slots of every unit, unit and player fields (net dirty bits, resource use, stored resources), scripts (including the sink word of waiting aim threads), poses, the RNG and the stale stack word. Mutation check: each of 18 deliberate one-line errors in the engine (lead factor, reload, resources, stale word, flags, packets, boundaries) is caught.

**Slot layout** (3 slots of 0x1C bytes at unit+4):
- +0/+2 i16 target: if +2 ≠ −0x8000 it is a ground point (x, z in whole world units); otherwise +0 is a target unit id (0 = none).
- +4 the aim result sink (vtable `0x004FD6F0`), its result at +8. The sink's return handler (`0x00481490`) sets the result to 1 for any nonzero return value and leaves it alone for 0.
- +0xC weapon type; +0x10 a value set at slot setup (projectiles use it); +0x14 u16 reload countdown; +0x16/+0x18 last aim heading/pitch sent to the script; +0x1A u8 stockpile count.
- +0x1B flags: bit0 aim started, bit1 enabled (slot setup: weapon id ≠ 0), bits 2–3 script index (0..2 select Primary/Secondary/Tertiary names), bit4 set at slot setup.
- **Weapon type fields used:** +0x60 fire routine (null: the slot never aims or fires), +0x68 speed, +0xC0/+0xC4 energy/metal per shot (floats), +0xC8 minimum barrel angle, +0xDC range, +0xE4 reload time (u16), +0x106/+0x108 tolerance/pitch tolerance, +0x111 flags (bits: 0 line of sight, 1 ballistic, 4 vertical launch, 16 water weapon, 19 turret, 25 cruise, 26 command fire, 28 stockpile).

**Slot pass** (`UnitScript_ProcessWeaponSlots` `0x0049E1A0`, stdcall unit). For slots 0, 1, 2 in order, skipping slots without the enabled bit:
1. If the reload countdown is nonzero, decrement it.
2. Resolve the target position (below). No target: clear the aim-started bit; next slot.
3. No fire routine: next slot.
4. **Turret** weapons, unless aiming already started:
   - **Ballistic:** from = aim-from position; d = from − target (wrapping). Heading = atan2_scaled(dx, dz) − unit heading (16 bits); pitch = the ballistic solver on (dx, dy, dz), the type's speed and minimum angle, and the game's gravity (GS+0x14263). Aim only if the pitch ≠ −32768.
   - **Else line of sight:** direct aim (below), always succeeds.
   - **Else:** no aim.
   - To aim: store pitch and heading in the slot, then start the aim script.
   - **Vertical launch** (non-turret) weapons, if not a stockpile weapon or the stockpile is nonzero, and aiming has not started: start the aim script with heading 0 and pitch 0.
   - **Starting the aim script:** the sink's result = 0; start `Aim<n>` (n from the script index) with 2 arguments (heading, pitch, zero-extended), the slot's sink, not run now; then the broadcast (below); set the aim-started bit.
5. If the reload countdown is nonzero: next slot.
6. **Range check** from the unit's position (below). Out of range: unit net-dirty |= 0x1000 (byte +0xBB bit 0x10); next slot.
7. **Can fire:** a stockpile weapon needs a nonzero stockpile; any other needs the owner player's stored energy (+0x8C) ≥ energy per shot and stored metal (+0x98) ≥ metal per shot (float compares; NaN fails).
8. **Fire:** the fire routine (stdcall unit, slot, target unit or null (`0x0048A190`), &target position). Zero: next slot.
9. **After a shot:**
   - Stockpile weapons: stockpile −1 (8 bits), then the stockpile display hook (`0x0041C150`, UI only).
   - Others: tier = min(kills / 5, 5); hurt = (u32)(hp·20, signed 32-bit) / max damage, **unsigned**; base = (100 − 6·tier) × reload time / 100 (truncated); reload = (120 − hurt) × base / 100 with 32-bit wrapping and signed truncating division, stored as 16 bits. So veterans reload up to 30% faster and a damaged unit up to 20% slower (negative hp gives a wrapped value).
   - Net dirty |= 0x800 for command-fire weapons, else 0x400.
   - Non-stockpile weapons then pay (below); its result is ignored.

**Target position** (`0x0048A1E0`, stdcall unit, &pos, slot index; returns 0/1):
- **Ground point:** x = target x << 16, z = target z << 16, y = terrain sample there (`0x00485070`), raised to the sea-level byte (GS+0x1427F) when not above it (signed compare; off the map the sample is −1, so sea level). Returns 1.
- **Unit id 0:** returns 0 with pos untouched.
- **Dead target** (table entry, indexed by the sign-extended id, with type index 0): set the target to (0, −0x8000), look up `StartBuilding` (a no-op lookup; nothing runs), queue `TargetCleared(slot index)` (1 argument, not run now), return 0 with pos untouched.
- **Live target:** its sweet spot (below). Then, **lead for veterans**: if the slot is enabled, its type has no cruise flag, the target has a motion object, the shooter has more than 5 kills and the type's speed ≠ 0:
  - d = shooter position − target position (32-bit wrapping, per axis); dist = truncate(√(dx² + dy² + dz²)) evaluated in doubles, keeping the low 32 bits;
  - t = low 32 bits of ((int64)dist << 16) / speed (signed 64-bit division), then lead = low 32 bits of (t · 0xCCCC) >> 16 (about 5/6 of the flight time);
  - each axis += low 32 bits of (velocity · lead) >> 16, with the motion object's velocity at +8/+0xC/+0x10.
- Returns 1.

**Sweet spot** (`0x0043E3C0` → `0x0043E0B0`): piece = 0, run `SweetSpot` in/out on the target's script now (a missing function leaves 0). Take the piece's current transformed vertices from the object state (**as last synced**, no sync here). The box starts at (0,0,0) in every bound, so it always contains the unit origin. The centre per axis is (max + min) / 2 (wrapping sum, truncating division), plus the unit position. The piece index is not range-checked by the original.

**Aim-from position** (`0x0043E2E0`, stdcall unit, &out, script index): piece = −1, run `AimFrom<n>` in/out now; if it is still −1, piece = 0 and run `Query<n>` in/out now. Out = unit position + piece offset from its parent chain (`0x0043DEF0`; out-of-range pieces give 0). Script index 3 reads past the name table (undefined).
**Query-piece position** (`0x0043E240`, stdcall unit, &out, script index, piece): a negative piece is replaced by `Query<n>` (starting at 0); then the same offset sum.

**Direct aim** (`0x0049D910`, stdcall unit, type (unused), &heading, &pitch, script index, &target; returns 1): from = aim-from position; d = from − target. Heading = atan2_scaled(dx, dz) − unit heading (16 bits). flat = low 32 bits of truncate(hypot(dx, dz)) (C runtime `_hypot`). Pitch = atan2_scaled(−(i16)(dy >> 16), (i16)(flat >> 16)), i.e. only whole world units of the rise and of the distance.

**Range check** (`0x0049AA80`, stdcall unit, &from, &target, slot as a byte — the caller's upper three bytes are stale and ignored; returns 0/1):
- dist² = hi32((int64)dx²) + hi32((int64)dz²) with dx, dz the 16.16 offsets (32-bit wrapping sum); out of range if dist² > range² (32-bit product), signed compare.
- Unless a water weapon: fails if (i16)(from.y >> 16) + (i16)(model height >> 16) ≤ the sea-level byte; then, for ballistic weapons, fails when the ballistic solver on (from − target) finds no pitch.

**Aim tolerance** (`0x0049D880`, stdcall unit, slot, heading, pitch; used by fire routines): tolerances = the type's (pitch tolerance falls back to the tolerance when 0); a type tolerance of 0 gives 2000 for both when unit flags & 0xC, else 150. Within when |(i16)(slot heading − heading)| ≤ heading tolerance and |(i16)(slot pitch − pitch)| ≤ pitch tolerance.

**Paying a shot** (`0x004012A0`, thiscall on the unit's resource slot +0xBC, float energy, float metal; player = slot+0x30, the unit's +0xEC): fails unless stored energy ≥ energy and stored metal ≥ metal (NaN fails). Then stored energy −= energy and slot+4 (unit+0xC0, energy used) += energy, each rounded to float; then, if stored metal ≥ metal still holds, stored metal −= metal and slot+0x1C (unit+0xD8, metal used) += metal. Returns 1.

**Aim broadcast** (`0x00456200`, stdcall unit, name, argc, 4 args): looks the function up (index, −1 if missing); if GS+0x2A44 bit0 is set, sends a 22-byte packet: `0x10`, unit id u16, function index u16, argc byte, 4 dwords of arguments, to the owner's net id (`0x00451DF0`).

**Stale stack word:** the unit pass's word lies inside the slot pass's frame, on the target position's **y** word. So after the slot pass it holds the y of the last target position written: the last slot whose target resolved (ground height or sweet spot plus lead), or whatever the fire routine left there (it receives the pointer). With no resolved target, it is unchanged. The engine starts the position's y from the word and stores it back.

**Functions written to `/TotalA.exe`** (name, prototype, plate comment): `0x0049E1A0` `UnitScript_ProcessWeaponSlots`, `0x0048A1E0` `Weapon_ResolveTargetPos`, `0x0048A190` `Weapon_GetTargetUnit`, `0x0043E3C0` `Unit_GetSweetSpotPos`, `0x0043E0B0` `Unit_GetPieceBoxCentre`, `0x0043E2E0` `Weapon_GetAimFromPos`, `0x0043E240` `Weapon_GetQueryPiecePos`, `0x0049E570` `Vec3_Subtract`, `0x0049D910` `Weapon_ComputeDirectAim`, `0x0049AA80` `Weapon_IsTargetPosInRange`, `0x0049D880` `Weapon_IsAimWithinTolerance`, `0x00456200` `UnitScript_BroadcastScriptStart`, `0x004012A0` `ResourceSlot_TrySpend`, `0x004B07C0` `UnitScript_FindFunction`, `0x004B0A70` `UnitScript_InvokeNamed`, `0x004B0BC0` `UnitScript_CallInOutNamed`, `0x00481490` `WeaponAimSink_OnReturn`.

**Open:** the stockpile display hook (`0x0041C150`) only touches the HUD (it calls `0x004199B0` when the unit is the one shown at GS+0x37E9C); not modelled. The fire routines (+0x60) are not built yet.
### Projectiles — differential-tested: 2,950 scenarios, 809,096 projectile updates (+ the same number of unit passes), 0 differences
Implementation: `engine/sim/src/projectile.cpp` (`sim/projectile.h`). Checked by `tools/orig_harness/projectile_harness`. Inside the suspended exe it builds a battle like `combat_harness` (three players, real unit types with scripts and object states, real damage and kill routines) plus the projectile table, Moon Quartet map cells with unit occupancy, features, feature types and animation slots, and all 256 real weapon definitions (some varied per scenario in flags, burst, spray, decay, timers, area, turn rate, ...). Each step it launches shots with the original creation routines (directed, ballistic, ownerless, from motion), assigns chase targets, feeds detonate packets, kills units (their death explosions and burst removal run for real), runs the unit pass and then the projectile update. After every operation it compares all 300 table slots field by field (stale slots included), the count, the camera follow state, the stub log (sounds, effects with positions, camera shake, feature damage, network packets with contents), units (hp, flags, kills, net dirty bits, decloak timer, ...), players, scripts, poses and the battle RNG.
**Run 2026-10-14** (seeds 0–2949, 300 steps each, stopped just before the end at the user's request to postpone long runs): 0 mismatches; 278 scenarios ended where the engine predicted an original crash (239 burst division by speed 0 on mutated weapons, 39 runaway interceptor chains) and the original did crash there. A 40-scenario sample covered: full table (1,004 steps at 300 shots), 9,269 directed / 7,087 ballistic / 11,574 ownerless / 1,091 motion launches, 34,057 explosions (9,809 water), 3,271 unit damage events, 20,995 feature hits, seeking 116,680 shot-steps, cruise 3,548, beams 42,782, meteors 23,784, chasing 24,674, camera following 663, packets 0x0B/0x0C/0x0D/0x0E.
**Place in the step** (`Game_RunSimulationStep` `0x00495490`): after the unit pass (`0x0048AD30`), before the effects tick (`Fx_TickWorldEffectsAndDebris`). Shots fired during the unit pass therefore fly in the same step. Burst rounds created during the update wait for the next step (the loop count is taken at its start).
**Table** (GameState+0x141F7 → 300 slots of 0x6B bytes, live count at +0x141F3, never cleared after battle start):
- Slot: +0x00 weapon def; +0x04 position xyz; +0x10 trail xyz (beam tail / cruise waypoint); +0x1C velocity xyz; +0x28 aim point xyz; +0x34 meteor tumble (i16); +0x36 yaw (u16, around the vertical); +0x38 elevation (u16; meteors: second tumble); +0x3A speed; +0x3E launch distance; +0x42 spawn tick (bursts: tick of the last round); +0x46 expiry tick; +0x4A next smoke tick; +0x4E target unit; +0x52 owner unit; +0x56 chased shot (pointer into the table); +0x5A/+0x5C last feature cell hit (i16 x, z); +0x5E ground height below (avg of the cell's max and min); +0x60 burst rounds left; +0x62 firing piece; +0x64 propeller spin; +0x66 owner's player slot (10 = none); +0x67 index before the last compaction; +0x69 flags (bit0 beam stopped growing, bit1 deleted, bits 4-5 two-phase phase).
- **Allocation** takes slot `count` if count < 300, clears only flag bit1 and the target. Everything else is the previous occupant's: the debounce cell, speed and distance of shots that never set them (ballistic and ownerless shots), elevation of shots from motion, the propeller spin, ...
- **Compaction** (`0x0049AE20`, end of every update): walk the slots, write each slot's own index into +0x67, drop deleted ones and copy later live ones down (order kept). For every *moved* shot that chases another, the chased pointer is renumbered afterwards: the survivor whose +0x67 equals the chased slot's old index becomes the target. Unmoved chasers (before the first hole) keep their old slot address, and a chased shot that was deleted leaves the pointer on its old slot: both then point at whatever moved in. The camera's followed shot moves with its slot.
**Launch set-up** (`0x0049C740`, weapon, start, aim or none, tick, owner or none): weapon, position = trail = start, aim (if given), flag bit0 and the phase bits cleared, spawn tick, burst 0, chased shot none, next smoke = game tick, target none. Without owner: player slot 10. With owner: owner and its player slot; if the owner has flag 0x20000000 and is the unit the camera follows (GS+0x142F3), the camera now follows this slot (GS+0x142F7). The weapon slot is the first of the owner's three slots whose weapon is this one (3 if none); the piece is asked from the script at once (`QueryPrimary/Secondary/Tertiary` in/out, `0x0043E1E0`; slot 3 reads a code address as the name, matches nothing, piece 0). Owner +0xB0 = tick + 600 (decloak hold-off). Then the start sound (weapon +0xF4) at the start.
**Launches** (the fire handlers call these; they return 0 when the table is full):
- **Directed** (`0x0049C9C0`, slot, owner, muzzle, aim, target): set-up with the aim. d = muzzle − aim. Yaw = atan2_scaled(dx, dz); distance = ftol(hypot(dx, dz)) (low 32 bits, stored); elevation = atan2_scaled(−(short)(dy>>16), (short)(distance>>16)). Speed = start velocity if nonzero, else velocity if there is no acceleration, else 0. Velocity from the angles (below). Expiry = tick + (range<<16)/velocity (unsigned) unless velocity is 0 or the weapon has `noautorange`, then tick + weapon timer. Target, burst = weapon burst. Then the owner's script is queued (not run) `Fire<Primary|Secondary|Tertiary>` by the slot's index (slot flags bits 2-3) and `RockUnit(−cos·800, −sin·800)` of (that slot's fire heading − unit heading) with the rounded multiplies; `startsmoke` gives a launch puff at the muzzle.
- **Ballistic** (`0x0049CDE0`): set-up without aim. Yaw/elevation = the slot's fire heading/pitch. vy = sin(pitch)·velocity − (slot aim drop (+0x10) / velocity, unsigned) · gravity (32-bit product); h = cos(pitch)·velocity; vx = −sin(yaw)·h, vz = −cos(yaw)·h. Expiry: `burnblow` → tick + ftol(hypot(muzzle−aim in x, z)) / h (signed; h = 0 crashes); else tick + weapon timer. Speed and distance are not set. Scripts, target, burst and smoke as above.
- **From motion** (`0x0049D000`, never called): set-up, speed 0, yaw = owner heading, vy 0, vx/vz from the owner's motion speed (motion +0x20); elevation and expiry stale.
- **Ownerless** (`0x0049DF10`, meteors): set-up without owner, velocity given. If asked and the net flag (GS+0x2A44 bit0) is set: packet 0x0D (36 bytes: position, velocity, weapon index at 25; the last 10 bytes are stack garbage) from the first playing player's net id.
- Velocity from angles (also every step of self-propelled shots): vy = sin(elev)·speed, h = cos(elev)·speed, vx = −sin(yaw)·h, vz = −cos(yaw)·h, all `Trig_MulSin/CosRounded`.
**Update** (`0x0049B720`), per slot of the count taken at the start; old height = (short)(y>>16) first:
- **Burst launchers** (burst ≠ 0): nothing until tick ≥ spawn + burst rate. Then, if the rate ≥ 5 or the remaining count is odd, the launcher moves to the owner's firing piece (`0x0043E240` with the stored piece, no script call). Rounds left −1, spawn += rate, and if a slot is free a full copy becomes the round: spawn = tick; `soundtrigger` plays the start sound at the launcher; expiry = tick + weapon timer, or with no timer tick + (distance + 0x100000)/speed (unsigned, of the launcher; speed 0 crashes the original); `randomdecay` adds rand(decay) − decay/2; burst 0. `sprayangle`: the *launcher's* vx/vz are re-aimed at yaw + rand(spray) − spray/2 with h = cos(elev)·weapon velocity (vy kept), so the next round inherits the spray. When no rounds are left the launcher ends (camera release, deleted).
- `propeller`: spin += 0x400.
- **Self-propelled**: alive while expiry > tick. A water weapon at or above the sea (old height ≥ sea level) just falls (vy −= gravity, elevation 0). Otherwise speed += acceleration up to the velocity (unsigned compares), then if seeking (two-phase: phase ≠ 0, else `guidance`) it steers (below); a failed turn detonates the shot but the step goes on; velocity from the angles. Expired: `burnblow` detonates; otherwise vy −= gravity and a two-phase weapon in phase 0 enters phase 1 with expiry = tick + flight time and, without `tracks`, drops target and chased shot. Then position += velocity and the hit test.
- **Line of sight**: alive while expiry > tick: position += velocity; beams: once tick > spawn + duration the head stops growing (flag bit0) and from then on the trail moves too. Expired: ends (camera release, deleted), no hit test.
- **Ballistic with a weapon timer**: alive: position += velocity + wind (GS+0x37ECC xyz), vy −= gravity; expired: `burnblow` detonates, else an end puff and it ends. **Ballistic without timer or dropped**: always fly like that. **Meteor**: position += velocity, tumble += (vx>>16)<<8, elevation += (vz>>16)<<8. Other weapons do not move and are not hit-tested.
- After the move (unless deleted): `smoketrail` puffs while alive whenever next smoke < tick (next += smoke delay); crossing down through the sea level (old height > sea, new ≤ sea) over a cell whose max height is below the sea, on maps with water impacts, gives a water splash.
**Steering**: the seek point (`0x0049B3E0`): `cruise` → farther than 0x400 from the aim (3D length, ftol, high word) the trail becomes the aim with its height's integer part replaced by 700; closer, the trail becomes the aim at the ground height there (sampled) or the sea level if higher. Otherwise the chased shot's position, else a live target's position, else the aim. Turning (`0x0049B520`): wanted yaw = atan2_scaled(dx, dz), wanted elevation = atan2_scaled(−(short)(dy>>16), (short)(ftol(hypot(dx, dz))>>16)) with d = shot − point. For yaw, then elevation: d = (short)(want − cur); |d| as a short (−32768 stays negative); > 27000 with `burnblow` → fail at once (yaw may already be turned); < turn rate → snap; else ± turn rate by the sign of d.
**Hit test** (`0x0049B090`):
1. Off the map (cell = (x>>20, z>>20) outside) → ends.
2. Chasing a shot: if the sum of the high dwords of the 64-bit squares of dx, dy, dz < aoe² (own weapon), detonate (and go on).
3. Ground height = (max + min)/2 of the cell.
4. The cell's first unit, if its player slot differs from the shot's and y < unit y + model max y → detonate on it, stop. Then the second unit, within [min y, max y] → detonate on it, stop. No alive check.
5. `unitsonly` stops here.
6. Feature: cell feature < 0xFFFB (and < the feature type count), or 0xFFFE → the anchor cell (back by the low byte in z and the high byte in x of +0xA) with a feature < 0xFFFB (no count check). It is hit if (short)(y>>16) < feature height + cell min height and the cell (trunc(x/16), trunc(z/16) of the integer parts) differs from the last feature cell hit, which is then remembered → detonate.
7. Below the cell's min height: `groundbounce` → vy = −(vy>>2), stop; else detonate. At or above: water weapons stop; below the sea level on a map with water impacts → detonate.
**Detonation** (`0x00499EB0`, shot, hit unit or none): sea = cell max < sea level. Unless `noexplode`: ends (camera release, deleted). No-water-impact map + sea + no unit: ends, nothing else. Camera shake (magnitude, magnitude, duration). Sea without unit: water sound + water explosion art; else hit sound and `endsmoke` puff or the explosion art (with the sea flag). The owner slot's player record (record 10 for ownerless shots: the 11th record) active with controller 3 → no damage here. Area ≤ 16 and a unit hit → direct damage at scale 1, then the owner (if any) gets net-dirty bit 0x4000 when the unit is an enemy (other player slot) and the damage's low 16 bits are nonzero, else 0x2000. Otherwise area damage.
**Damage to a unit** (`0x00499CD0`, scale): base = default damage (u16), or the weapon's damage-table entry for the unit's name (lower bound by stricmp, then exact). dmg = ftol(base × scale) (double). Heading = atan2_scaled(shot−unit in x, z) − unit heading. Owner kills: dmg = ((min(kills/5, 5)·6 + 100)·dmg, 32-bit)/100 (signed). Game options (GS+0x37F2F): bit 7 doubles, bit 8 halves (truncating). Then `Unit_ApplyDamageAndBroadcast` (attacker = owner, kind 2 for `paralyzer` else 1). Returns dmg.
**Area damage** (`0x0049A120`): radius = aoe/2; cells within ±(radius/16 + 1) of the impact cell (trunc of the integer parts / 16), clamped to the map; z outer, x inner. Per cell, first then second unit, not the owner, not seen before (a list of 20; when full, units are still damaged): distance = ftol(√(gx² + gy² + gz²)) high word, where g is the gap from the impact to the unit's model box (0 inside), all at double precision with (x²+y²)+z². If < radius: scale = 1 at 0, else (d/r − 1)²·(1 − edge) + edge (double, stored as float); damage, summed into enemy/own totals by player slot. Then, unless `unitsonly`, the cell's feature (0xFFFE → the anchor cell): distance to the feature animation's position if *this* cell has flag bit0, else to the feature's spot `((footprint + 2·cell) << 19, ground << 16)` from *this* cell's coordinates; if < radius and the anchor was not hit yet (list of 64), feature damage (anchor cell, its coordinates, weapon). Interceptors then detonate every other live shot whose squared distance (as in the hit test) < aoe², each followed by two 0x0E packets (that shot's aim and weapon index, then its own) from the owner's net id. Finally the owner's net-dirty split from the full totals (enemy > 2 × own → 0x4000, else 0x2000; `0x00406F50`).
**Other entry points**: received 0x0E (`0x0049AF90`): the first shot with that aim and weapon index detonates. A dying unit (`0x0049C880`, from the kill packet handler): every burst launcher it owns ends and the table is compacted at once; the scan continues at the next slot, so a launcher that slid into the current slot is skipped. Death explosion (`0x0049B000`): a temporary shot (weapon = explode-as or self-destruct-as, unit position, unit's player slot, no owner/target) detonates with no unit.
**Original crashes the engine records** (`ProjectileTable::fault`, harness-confirmed): burst round with no weapon timer and speed 0 (a stale 0 in a fresh slot; no stock weapon is a timer-less burst), burn-blow shell with horizontal speed 0, and two `noexplode` interceptors setting each other off (endless recursion, stack overflow). Not modelled: feature ids ≥ the type count in area damage (reads past the table), a burst launcher whose dead owner survived the removal quirk (freed object state).
**Hooks** (presentation or other systems): sounds (`0x0047F300`), puffs (`0x00472810`, `0x004729D0`), explosion art (`0x00420A30`), camera shake (`0x0041C640`), feature damage (`0x004244B0`: burning, accumulated damage, death anims, packet 0x0F), network (`0x00451DF0`), feature animation positions, weapon slots and motion speed.

### Weapons: fire routines — smoke-tested: 50 scenarios, 2,820 fire-routine calls, 0 differences (large runs pending)
Implementation: `sim::fire_weapon` (`engine/sim/src/fire.cpp`), plus `fire_vertical` and `find_intercept_target` (`projectile.cpp`). Checked in `tools/orig_harness/projectile_harness`, which calls the original routines directly on random slot states: aim flag, aim result, stockpile, aim angles and targets. It compares the result, the weapon slots, the projectile table, the packets and the RNG.

The weapon type's routine is picked at load time (see "Weapon definitions"): turret, vertical launch, line of sight / self-propelled, or dropped. All four are stdcall (unit, slot, target unit or null, &target position) and return 1 when a shot went off. When the network flag (GS+0x2A44 bit 0) is set, a successful shot sends packet 0x0D (36 bytes):
- start position (12), aim point (12), weapon index (1);
- a byte whose bit 0 is the interceptor flag; **bits 1–7 are uninitialised stack** (the engine sends 0);
- slot aim heading and pitch (2 + 2);
- target id and shooter id (2 + 2);
- slot index (1).

**Turret** (`Weapon_FireTurret` `0x0049D580`):
- **Preconditions:** the slot must be aiming (flag bit 0) and its aim result (+8) nonzero; otherwise return 0.
- **Re-solve the aim:**
  - Ballistic: from the aim-from piece, heading = atan2(from − target) − unit heading, pitch = ballistic solver.
  - Line of sight: direct aim (`0x0049D910`).
  - Otherwise: no solution.
  - No solution: clear the aiming bit, set net flag 0x1000 (+0xBB bit 4), and return 0.
- **Tolerance:** the script's aim (+0x16/+0x18) must be within tolerance of the fresh solution (`0x0049D880`). Otherwise clear the aiming bit and return 0.
- **Muzzle** = the Query piece position. The shot uses **the script's aim, not the fresh one**: heading += unit heading (made absolute).
- **Spread:**
  - spread = (u16)(accuracy (+0x104) − (u16)((u32)(hp << 11) / max damage) + 0x800).
  - If kills/12 > 1, spread /= kills/12.
  - If spread ≠ 0: heading += NextMod(spread) − spread/2, then pitch += NextMod(spread) − spread/2 (16-bit, battle RNG).
  - The register holding spread carries the weapon definition pointer's upper half; only the low 16 bits are used.
- **Fire:** line-of-sight or self-propelled → directed shot (`0x0049C9C0`); ballistic → shell (`0x0049CDE0`); otherwise nothing.
- **On success:** clear the aim result and the aiming bit, then send the packet with the muzzle as start.

**Vertical launch** (`Weapon_FireVerticalLaunch` `0x0049DB70`):
- Needs the aim result (not the aiming bit).
- **Aim:** muzzle = Query piece. The slot's absolute heading = atan2_scaled(muzzle − target in x, z); pitch = atan2_scaled(−hi16(dy), hi16(trunc(hypot))). Both are stored in the slot.
- **Interceptors** (flag bit 30) need a shot to chase (`Weapon_FindInterceptTarget` `0x0049D120`), else return 0:
  - the slot must hold a stockpiled round;
  - for each table entry (deleted ones too): another owner slot, weapon targetable (bit 29), and its aim point inside the square of half-width `coverage` (+0xE0, ×65536) around the unit. This is one unsigned compare per axis.
  - The first such shot no other entry already chases wins.
- **Missile** (`Projectile_FireVertical` `0x0049CC20`):
  - like the directed shot, but yaw 0, elevation 0x4000 and velocity 0;
  - speed from start velocity / velocity as usual; expiry from the range or the timer;
  - the chased shot is stored; the slot's aim result is cleared as soon as a table slot is taken;
  - fire scripts and RockUnit, then the start smoke.
- **On success:** packet (muzzle start), clear the aim result and the aiming bit.

**Line of sight / self-propelled** (`Weapon_FireLineOfSight` `0x0049D9C0`):
- No aim checks. Muzzle and slot heading/pitch as for vertical launch.
- **The unit itself must face the target:** the tolerance check compares the slot angles with the unit's heading and **its bank word** (+0x68), not its pitch.
- Directed shot. The packet start is **the unit's position**, not the muzzle. The slot flags are left alone.

**Dropped** (`Weapon_FireDropped` `0x0049DD60`):
- Muzzle = Query piece. Shot set up as `Projectile_SpawnFromUnitMotion`:
  - speed 0, yaw = unit heading;
  - horizontal velocity = −(sin, cos)(heading) × the motion object's speed (+0x20).
- No lifetime, target, scripts or smoke of its own. Packet with the muzzle as start.
- **The original requires a motion object** (it reads +0x20 through it). Only aircraft carry dropped weapons.

### Draw-time side effects on the simulation
**Drawing advances the simulation's C-runtime random sequence.** `Gfx_DrawProjectiles` (`0x0049BE60`) calls the C runtime `rand()` while drawing lightning shots (render type 7). It runs on the same thread as the simulation. The simulation also draws from `rand()`, for wind, meteors, features, explosion frames, victory polling and battle start (see SIM_INVENTORY "Randomness"). So in the original, that sequence depends on which lightning shots were on screen and on how many frames were drawn. This is a view- and frame-rate-dependent input, like the heap-address cases. The engine keeps drawing free of side effects; whether to feed these draws in is open.

### Battle step (integration) — `sim::Battle`
`engine/sim/src/battle.cpp` holds the state of one game:
- the unit table (index = id, entry 0 unused), per-unit states, 11 player records (ten slots plus "no owner"), unit types (entry 0 = the blank type of empty slots);
- the projectile table, the map grid, feature types and settings.

It wires the systems built so far to each other. Everything else goes through one `BattleOutside` interface:
- systems not built yet: motion, mission behaviours, attack retargeting, transport, economy, features, line of sight, debris;
- the network, presentation and user interface.

**`Game_RunSimulationStep` (`0x00495490`), per pending step:**
1. Increment the game tick (GS+0x38A47).
2. Network input `0x00453D40` (when called for a network game).
3. Unit pass `0x0048AD30`.
4. Projectile update `0x0049B720`.
5. Effects and debris `0x00420F30`.
6. Players `0x00464F80`.
7. Features `0x00424050`.
8. Animation cursors `0x00415B30`, wind `0x00490C40`, meteors `0x00437DE0`, camera `0x0041CA10`.
9. `0x00471EB0`, then `0x00466580`.
10. Network flush, for a network game with `0x00506DBC` set.

Between stages it adds GetTickCount deltas to profiling counters, which are not part of the simulation. After the frame's steps it runs `0x00428BD0`, `0x00428BE0`, `0x00428BF0`, `0x00463EF0` and `0x00482130`; see SIM_INVENTORY.

`Battle::advance_tick()` and `Battle::step_units_and_projectiles()` cover stages 1, 3 and 4. Other stages run in the caller. Hook mapping:
- weapons → slot pass; fire → fire routines;
- damage, heal and kill → combat;
- primary orders → order processing, then the stale word = the visited slot's player record;
- secondary orders → order processing; settle → ground settle;
- paralysis, clear orders, weapon-target release → orders;
- projectile removal and death explosions → projectiles.

Orders get stand-in heap addresses (a sequence from 0x01000000 in steps of 0x58), since the original's vary between runs.

**Behaviour seen while testing:** shots pass through units of their own player (`owner_slot` equality in the hit test). Units are found only through the map cells' occupancy, which the footprint system fills (see "Footprints and occupancy"); units enter the battle through `Battle::create_unit` (see "Unit creation").

### Unit creation — smoke-tested: 24 scenarios, 1,675 operations, 440 units created (147 structures, 247 mobile, 129 activated, 86 with contested cells), 309 refused, 0 differences (large runs pending)
Implementation: `sim::Battle::create_unit` (`engine/sim/src/unit_create.cpp`, `sim/unit_create.h`), type data `sim::UnitCreateType` (in `sim::BattleUnitType::create`), filled from imported definitions by `ta::to_battle_unit_type` (`ta_compat/src/battle_types.cpp`). Checked by `tools/orig_harness/create_harness` (below, with the footprints).

**Entry** (`Unit_CreateFromDefId` `0x00485F50`, stdcall (player, type u16, x, y, z, fully_built, layer, wanted id u16) → unit or 0). The player is the low byte; the definition record is GS+0x1439B + type × 0x249 (record 0 is the blank type).
1. Fails for type 0 and for a type whose flag word (def+0x241) lacks bit 23 (the importer's "version compatible" bit).
2. **Limit:** if def+0x15A (`maxthisunit`, −1 = none) is not −1, every entry of the player's range (+0x67..+0x6B inclusive, dead or alive) with this type index is counted; count ≥ limit (signed) fails.
3. **Entry:** the range must not be empty. Without a wanted id: the first entry with type index 0. With one: exactly that entry, which must lie in the range and be free; otherwise fail.
4. The three weapon slots' aim sinks get their vtable (no state); type index = type.
5. Field set-up (below), script and object state, weapon slots, metal extraction (below).
6. **bmcode** (def+0x22F) = 1: a motion object (`0x43DC00`, a hook for the motion system) and the heading becomes def+0x210 (the build angle) — the random heading drawn in step 5 is thrown away, but its RNG draw stays.
7. Unit flags bits 0-1 = layer bits 0-1. Ground settle (`0x48A870`, see "Ground settle"; the new unit has resettle set). Footprint claim (`0x47CC30`, see "Footprints and occupancy").
8. **Packet 9** (23 bytes, to the owner's net id through `0x451DF0`, always): 0x09, type u16, id u16, x, y, z, pitch, heading, bank.
9. **Only when fully built:** bmcode 0 → packet 0x12 (5 bytes: 0x12, id, id); type bit 18 (`activatewhenbuilt`) → `Unit_SetActivateState(1, on)` (verified here through the real routine: Activate queued, speech, HUD and packet 0x11 hooks); type bit 24 (`isfeature`) → last damage kind 7 and the dying flag 0x4000 (the next unit pass kills it).
10. Line of sight (`0x482AC0`, hook), the player's alive count (+0x144, u16) and units-ever-created (+0x140, u32) + 1, and the victory conditions hear about the unit (`0x490580`, hook).

**Field set-up** (`Unit_InitInstanceFields` `0x00485A40`), in order. "Type bit n" means bit n of def+0x241.
- def pointer; flags |= alive (bit 28); flags bit 14 (dying) cleared; bit 29 = (bmcode == 0) (a structure: claims by yard map); footprint (+0x7E) = def+0x14A.
- flags bit 31 = type bit 16 (has weapons); +0x114 bit 0 = type bit 7 (z-buffer); flags bit 30 = type bit 9.
- Fully built: build fraction left 0.0, hp = low 16 bits of max damage (def+0x1FA). Otherwise: build fraction 1.0, +0x100 = 0, hp 0.
- Script flags &= 0xF0; health % 0; flags: clear bits 1-4, 10, 11, 17, set bits 0, 5, 16 (ground layer, selectable, resettle); position; previous health % 0; **activate flags 0**; cloak hold-off 0; bank 0.
- **Cell origin** (+0x76/+0x78, shorts): ((x − fx·2¹⁹ + 2¹⁹) >> 20) and the same for z (32-bit wrapping, arithmetic shift): the footprint centred on the position, rounded to the nearest cell.
- **Heading** = rand(build angle) + 0x8000 − build angle / 2 (16 bits; battle RNG, a build angle < 2 draws nothing). Pitch 0; line-of-sight cell (+0x7A/+0x7C) 0; damaged timer 0; transfer hold-off 0.
- Flags bit 9 = (owner's +0x146 == the local view GS+0x2A43), bit 8 cleared.
- For slots 0, 1, 2: target = (0, −0x8000) (`0x48A160`), then `Unit_ReleaseWeaponTargets(slot)` (only sets bit 0x10 on enabled, unreleased slots — the target is already gone).
- Net dirty 0; kills 0; last attacker none, its slot 10. **The last damage kind is not reset.**
- Flags bits 18-19 = type bits 0-1, bits 20-21 = type bits 2-3, bit 11 = type bit 4 (init cloaked), bits 26-27 cleared; build menu pages (def+0x22E) > 1 → bits 22-23 set, 24-25 cleared; else bits 22-25 cleared.
- +0xF8 = 0; resource slot (+0xBC, 52 bytes) zeroed and its player = the entry's owner slot (+0xFF) (`0x401070`); transport piece +0xF9 = 0xFF; hover phase = rand(0x10000); group list 0 (`0x480250`, hook).
- **Not touched** (whatever the previous occupant left): the owner (+0x96/+0xFF, fixed per range), the motion pointer of non-mobile types, orders, target links, aim result/heading/pitch of the slots, slot flag bits 5-7, last damage kind, metal extraction of non-extractors.

**Script and object state** (`Unit_CreateScriptAndObjectState` `0x00485D40`): with a COB (def+0x18E): a script object (tick rate 30), the object state reordered by the script's piece names, bound, then `Create` started and all threads run once. Without one: no script, object state without reordering. State +0x10 = 0. The script's statics come from `realloc` **without clearing** (heap contents; the engine starts them at 0, the harness clears its heap to match).

**Weapon slots** (`Unit_SetupWeaponSlots` `0x0049E070`), slot i: reload 0, weapon = def+0x1EE[i], stockpile 0, flags = (old & 0xF0) | i << 2 | 0x10 | (weapon's index byte +0x10A ≠ 0) << 1. Slot +0x10 = ftol((Query-piece z − AimFrom-piece z) × 1.25) (`0x43E240` with piece −1, then `0x43E2E0`; x87 FILD × double, truncated): the z difference only, not a distance. Then the script gets `SetMaxReloadTime(longest reload × 1000 / 30)` (queued). A null weapon pointer or a missing script crashes the original.

**Metal extraction** (`Unit_UpdateMetalExtraction` `0x00437840`): only if def+0x1CE (`extractsmetal`) > 0.0 (NaN and ≤ 0 skip). Sum = Σ (cell metal byte +7, + 1) over the footprint cells inside the map, as 16 bits. Extraction (+0x58) = (float)(extracts × (short)sum) (the sum sits in the high half of a dword loaded with FILD, then × 2⁻¹⁶). Script: `SetSpeed(sum)` queued.

**Engine API:** `Battle::init_unit_table(n)`, `assign_unit_range(player, first, last)` (sets the owner of those entries), `init_buckets()` after the map is set, `types[k]` filled with `ta::to_battle_unit_type`, then `create_unit(player, type, x, y, z, fully_built, layer, wanted_id)`. Hooks in `BattleOutside`: `create_motion`, `join_group`, `stamp_line_of_sight`, `victory_unit_created` (plus the footprint hooks below). **Callers** (names from the reference program, not verified): battle start `0x497180`, the commander spawn `0x496EE0`, map-schema units `0x488310`, saved games `0x487080`, ownership transfer `0x488570`, the players' tick `0x464F80`, the building/mobile-build/resurrect behaviours and a console command. The commander spawn (read, not tested) looks up the start position for the player's start index in the schema (`0x437320`), the side's commander type by name, and calls the routine with (player, that type, start x, y, z, fully built 1, layer 1, any entry); it also sets the player's side bytes and starting resources (at least 200). A tool spawns a commander the same way: `create_unit(player, commander_type, x, y, z, true, 1)`.

### Footprints and occupancy — smoke-tested within create_harness (same run): 160 moves, 57 wanted-cell claims, 79 area re-claims, 81 forced re-claims, 47 yard checks, 46 yard openings, 60 height-range updates, 0 differences
Implementation: `engine/sim/src/footprint.cpp` (`sim/footprint.h`): `claim_footprint`, `release_cells`, `clear_footprint`, `claim_wanted_cells`, `reclaim_area`, `force_reclaim_around`, `yard_cells_free`, `refresh_height_range`, `footprint_origin`; wired into `sim::Battle` (yard opening, death). `create_harness` runs the original routines below against them after random moves, layer and yard-state changes and random cell occupants, and compares all cells, buckets and units after every operation.

**Map cell** (13 bytes, GS+0x14287, row-major, width GS+0x14233, height GS+0x14237): +0 unit (u16), +2 unit2 (u16), +4 height, +5 max and +6 min of the cell's and its +x/+z/+xz neighbours' heights, +7 metal, +8 feature, +0xA feature extra, +0xC flags (bit 0 feature animation, **bit 1 marked by a yard map**). `Map_GetCell` `0x481550` (x, z) gives the cell or 0 outside.

**Spatial buckets:** GS+0x1429F points at a grid of 10-byte records (head unit pointer at +6), GS+0x142A3/+0x142A7 = width/height = map world size rounded up to 128 units (`0x482C20`), GS+0x142B7 = one more record for units off the map. A unit's +0x82 is its bucket (0 none); lists link through **+0x8E, the same field as the cargo list** (carried units are never filed). Newest first.

**Occupation contest** (every claim): a unit U wants a cell field that holds H ≠ 0 (H may be U itself). If H's owner record is active (+0 ≠ 0) and has controller 3: H gets flags bit 27 (wants cells), U gets bit 26 (holds wanted cells) and U takes the field. Otherwise H gets bit 26, U gets bit 27 and the field stays H's. Bit 27 marks a unit to claim again; bit 26 makes a unit's release trigger re-claims around it.

**Claim** (`Unit_ClaimFootprint` `0x0047CC30`):
1. With a motion object: motion +0x26 = game tick (hook `motion_cells_claimed`).
2. Off the map (cell x or z < 0, or x + fx ≥ width, or z + fz ≥ height, int sums): file the unit in the off-map list; done.
3. File it in bucket (z >> 23) × bucket width + (x >> 23) (arithmetic shifts of the 16.16 position). Moving between lists: carried units only record the new bucket; others unlink (walk) and push at the head.
4. **Structures** (flags bit 29): yard byte k per footprint cell (row by row): if byte & (yard open (script flags bit 2) ? 2 : 4): contest `unit`; then if byte & 1: cell flags |= 2. Afterwards the height range of the rectangle (x−1, z−1, fx+2, fz+2) (16-bit sums) and passability for (x, z, fx, fz) (`0x440A40`, hook `update_passability`).
5. Others: layer 1 contests `unit`, layer 2 `unit2`, layers 0 and 3 claim nothing.

**Release** (`Unit_ReleaseFootprintCells` `0x0047D0E0`): unless in the off-map list (a unit never filed goes ahead with its origin): structures clear `unit` where it is theirs and clear cell flag 2 for yard bit 0, then the height range as above (no passability call); layer 1/2 clear their own field. Then clear bit 27; if bit 26 was set: clear it and re-claim the area of the footprint (below). Finally passability after release (`0x440A70`, hook: with a motion object it recomputes the movement classes changed since motion +0x26 and restamps it; without, all classes).
**Clear** (`Unit_ClearFootprint` `0x0047CBD0`, on death): release, then leave the bucket list unless carried; bucket = 0.
**Wanted-cell claim** (`Unit_ClaimWantedCells` `0x0047C790`): only with bit 27, which it clears. The first cell comes from `Map_GetCell` (0 off the map: **the original crashes** when the footprint is not empty); the walk is not bounded (a footprint past the bottom edge writes beyond the grid). Structures: per yard byte, occupied in the current yard state → contest `unit`, else clear `unit` if it is theirs (no cell marks, heights or passability). Others: **layer 1 contests `unit`, every other layer (0, 2, 3) `unit2`.**
**Area re-claim** (`Map_ForEachUnitInCellRect` `0x0047E5C0` with the callback `0x47ED30`, also `Map_ReclaimCellRect` `0x47CAC0`): buckets x from (x0 >> 3) − 1 to (x1 >> 3) + 1 (outer), z likewise (inner), skipping those outside the grid (unsigned compare); every unit of the bucket and every unit of its cargo list whose footprint overlaps [x0, x1) × [z0, z1) gets a wanted-cell claim.
**Forced re-claim** (`Unit_ForceReclaimAround` `0x0047DB20`): bit 27 on, area re-claim of its footprint, passability.
**Yard check** (`Unit_YardCellsFree` `0x0047D970`, value): x1 = (short)(x + fx), z1 = (short)(z + fz); false unless x ≥ 1, z ≥ 1, x1 < width, z1 < height; then false if any yard byte & (value ≠ 0 ? 2 : 4) covers a cell whose `unit` is another unit. **Yard open** (`Unit_SetYardOpen` `0x0047DAC0`, the script value 18): if free: script flag bit 2 = value & 1, bit 27 on, wanted-cell claim, passability.
**Height range** (`Map_RefreshCellHeightRange` `0x00483210`, origin, size as shorts): x0 = max(x, 0), z0 = max(z, 0), x1 = x + w and z1 = z + h, each clamped to width − 1 / height − 1 when beyond; each cell of [x0, x1) × [z0, z1) gets max/min over itself and its +x (if x < width − 1), +z (if z < height − 1) and +xz neighbours.

**Undefined behaviour recorded by the engine** (`FootprintWorld::fault`, the access is skipped): cells beyond the grid, yard map reads past the end, occupants outside the table, buckets outside the grid, a unit missing from its bucket list. In the smoke run the original crashed every time the engine predicted a crash (4 wanted-cell claims with the first cell off the map) and wrote past the grid once without crashing (scenario stopped).

**Not reverse engineered here:** the movement classes' passability (`0x440830`, `0x47E1F0`; motion system), placement checks for building (`0x47D2E0`, `0x47E2D0`, `0x47DB70`), the start-position code that calls the creation routine.

**Functions written to `/TotalA.exe`** (name, prototype, plate comment): `0x00485F50` `Unit_CreateFromDefId`, `0x00485A40` `Unit_InitInstanceFields`, `0x00485D40` `Unit_CreateScriptAndObjectState`, `0x0049E070` `Unit_SetupWeaponSlots`, `0x00437840` `Unit_UpdateMetalExtraction`, `0x0047CC30` `Unit_ClaimFootprint`, `0x0047D0E0` `Unit_ReleaseFootprintCells`, `0x0047CBD0` `Unit_ClearFootprint`, `0x0047C790` `Unit_ClaimWantedCells`, `0x0047D970` `Unit_YardCellsFree`, `0x0047DAC0` `Unit_SetYardOpen`, `0x0047E5C0` `Map_ForEachUnitInCellRect`, `0x0047CAC0` `Map_ReclaimCellRect`, `0x0047ED30` `FootprintCb_ClaimWanted` (function created), `0x0047DB20` `Unit_ForceReclaimAround`, `0x00483210` `Map_RefreshCellHeightRange`, `0x00481550` `Map_GetCell`, `0x0048A160` `Weapon_ClearSlotTarget`, `0x00401070` `ResourceSlot_Init`, `0x00456050` `Net_SendUnitCreated`, `0x004560C0` `Net_SendStructureFinished`.

**create_harness:** `create_harness <TotalA.exe> <game_dir> [scenarios] [operations]`. Per scenario: three players with ranges 1–8, 9–16, 17–24 (random controllers incl. 3, random view), a fourth with an empty range, four random real types (model, script, all 256 weapon definitions) with random limits, bmcode, build pages, build angles, extraction, footprints and yard maps, type flag flips, settle variants; Moon Quartet heights with random metal. Stubs: motion constructor, line of sight, victory, group list, passability (both), speech, HUD, network (packet bytes logged), script effect requests, the COB checksum cache. A crash the engine predicted (fault set) restarts the process without counting as a failure.

### atof (1998 C runtime string → double) — bit-exact, user rule: keep all quirks
Verified 2026-10-13: `ta::crt_atof` (`ta_compat/src/crt_strtod.cpp`) matches the original `atof` (`0x004E4560`) on **20,022,360 strings**, with **0 mismatches**. That covers all 22,360 distinct value strings in the game's TDF/FBI/OTA/GUI files plus 20M random edge-heavy strings. `atoi` was also checked with 0 mismatches. A modern `strtod` would differ on 3.25M of them, including 27 real game values.
- **Pipeline:** skip C-locale isspace → `Crt_StrToLd12` (`0x004F3D10`) → `Crt_Ld12ToIeee` (`0x004F37C0`, double format descriptor at `0x00510CE0` = {maxexp 1024, minexp −1023, prec 53, expwidth 11, 64 bits, bias 1023}).
- **Scanner:**
  - Skips space/tab/LF/CR, optional sign, digits, `.` (locale decimal point), digits.
  - Exponent letters: `e` `E` `d` `D`.
  - Stores at most 25 significant digits, then keeps 24; **extra digits are truncated**.
  - Explicit exponent clamps at 5201, which gives inf or zero.
  - No digits gives +0. The sign is OR'd in at the end, so `-0` and `-.0` give −0.0.
  - No `inf`/`nan`/hex parsing.
- **Mantissa:** decimal digits → 96-bit integer → normalized 80-bit mantissa (`Crt_DigitsToLd12` `0x004F8C40`, start exponent `0x404E`).
- **Scaling** (`Crt_Ld12MulPow10` `0x004F9390`):
  - Clear the low 16-bit guard word.
  - For each octal digit of |exp| (least significant first), multiply by a table power.
  - The table entry is "un-rounded" (its dword@2 decremented) when its low word > `0x7FFF`.
  - Tables at `0x005116F0` / `0x00511850`: 29 × 12 bytes each, entry = low16(round80(10^p)) | round64(10^p) | exponent. Generated bit-exact by `tools/gen_pow10_table.py`.
- **Multiply** (`Crt_Ld12Mul` `0x004F90D0`): 16-bit word partial products, only those with index sum ≥ 4. Carries bump one word only. Round half-even on the 16-bit guard word.
- **To double** (`Crt_RoundMantissa` `0x004F3610`): round up only if bit 53 is set **and** some bit in 55..95 is set. **Bit 54 is never examined, and exact halves round down.** Denormals re-round after shifting.
- **Test technique (reusable):** `ta_compat/tools/orig_harness` (32-bit) starts `TotalA.exe` **suspended**, injects an x86 stub, and calls the original routine in batches via `CreateRemoteThread`. Use it for any self-contained original routine (no OS calls, static data only).

### Rendering (battle view) — scan conversion differential-tested: 7,000 random polygons, 0 differences
Implementation: `render/` (engine side, no game names), importers in `ta_compat/src/render_assets.cpp`, viewer `render/tools/battle_view.cpp`. The original draws everything in software into an 8-bit offscreen surface; we build a **draw list** (projection, culling, order, CPU-rasterised model sprites) and execute it either on the CPU (reference) or on the GPU (SDL3 GPU API, `render/src/gpu_renderer.cpp`). Both give identical palette indices (checked every frame by `battle_view --gpu`; D3D12 and Vulkan: 0 differing pixels).

- **Frame** (`Render_DrawBattleFrame` `0x00468CF0`, called once per main-loop pass from `0x00496790` with (draw dynamic = 1, present = 1)). Order:
  1. Terrain tiles, clipped to the view rectangle.
  2. (HUD, minimap: not part of the battle view renderer.)
  3. Visible units are bucketed into **rows of 16 world units of z**: row = trunc((z − scroll y) / 16) + 16 (C truncation), kept when 0 ≤ row < row count; within a row, in visible-unit-list order.
  4. Feature cells from cell row trunc(scroll y / 16) − 16 and cell column trunc(scroll x / 16) − 10, clamped to the map minus one cell: every feature with height < 10 is drawn (row by row, column by column); taller ones are marked deferred (cell flag 0x04).
  5. Row by row: the row's **ground units** (unit flags & 3 == 1), then the deferred tall features of the matching cell row.
  6. Projectiles (`0x0049BE60`), then world effects, then the **other (air) units** in row order.
  7. Overlays (order lines, health bars, LOS fog, selection boxes, HUD text) follow; not modelled.
  - Hidden features (`nodrawundergray`) are drawn only when the cell's owner nibble is the viewing player or the LOS footprint test passes (not modelled; we draw all).
- **Projection.** World (x, y, z) in 16.16; screen x = int(x) − scroll x + 128, screen y = int(z) − (int(y) >> 1) − scroll y + 32, where int() is the high 16 bits as a signed short and >> is arithmetic. (128, 32) is the view's top-left corner on the 640×480 screen. **Height only moves things up by half; the terrain image itself is not displaced by height** (the tile art is pre-drawn in perspective). View size assumed 512×416 (left panel 128, top and bottom bars 32): not yet confirmed from the binary.
- **Terrain** (`Map_BlitVisibleTerrainTiles` `0x00483FA0`): the TNT tile map (u16 per 32×32-pixel tile, (width/2) × (height/2) tiles, at header offset 3) indexes 1024-byte tiles (header offset 5, count at offset 6). The tiles covering the view are copied opaque at tile × 32 − scroll + view origin; partial edge tiles go through a clipped copy, inner ones through a fast 32×32 copy. Exact.
- **Palette and tables.** `palettes\PALETTE.PAL` (256 × RGBA, A unused). Translucency and shadows use `palettes\PALETTE.ALP`, a 256×256 table: entry [src·256 + dst] = palette index nearest to the average of the two colours (diagonal = itself); built by `Palette_BuildAlphaBlendTable` `0x004BA750` and cached as the file (ccdata.ccx ships one, which we load). PALETTE.LHT / .SHD (8 KiB each) are light and shade tables (32 levels) used by the shaded model path (not modelled yet).
- **Model sprite** (`Unit_UpdateAndDrawModel` `0x0045AC20` → `Object_DrawPieces` `0x00458810` → `Model_AllocAndRasterizeSprite` `0x004586A0`):
  - Draw first **syncs the unit's pose** (base angle check and transform recompute, as `Unit_SyncObjectStateAnimTree`): see "Render coupling" below.
  - A unit's model is rasterised into a cached 8-bit sprite and blitted at the unit's screen position. The sprite is rebuilt when the cache-draw count is 0 (reset by pose changes to cached pieces), when it has none, or after cache eviction; otherwise the old sprite is reused. Rebuilding every frame gives the same pixels.
  - **Bounds** (`0x004581E0`): over every *visible* piece's transformed points, project x = int(px), y = int(−pz) − (int(py) >> 1); min/max start at 0 (the unit origin is always inside). Sprite = (max x − min x + 4) × (max y − min y + 4), hotspot (2 − min x, 2 − min y), filled with 1, transparency key 1.
  - **Pieces** (`Model_RasterizePiecesTextured` `0x00459830`, called with "cached only"): pieces from the last state slot to the first; a piece is drawn when visible and its cached flag is set (all visible pieces while the unit is under construction). Points are projected as above plus the hotspot; depth = int(py) + 50 (+75 for `digger` units). Primitives in stored order (the load-time sort by average Y is the painter's order), skipping primitive 0 when the piece has a selection primitive. **No depth buffer** in the normal path (1-byte-per-pixel sprite).
  - Primitive kinds (flags at primitive +0x1C, from the file's word there and the texture binder): bit 0 → **flat polygon** in palette index (colour & 0xFF), any vertex count; else only **4-index primitives are drawn, as textured quads**; others are skipped.
  - Texture frame: bit 1 (animated): bit 2 (team colour) → frame = owner player's colour index; otherwise frame 0 in the sprite path, the animation cursor in the direct path. Not animated → the bound frame.
  - **Texture binding** (`Model3d_BindPrimitiveTextures` `0x0042A140`, per model, sibling then child then own primitives): if the primitive has a texture name pointer: clear bit 2; search the texture GAFs (`textures\*.GAF` in enumeration order without `logos.GAF`, `Texture_LoadAllGafs` `0x0042A440`) by stricmp; else `logos.GAF` (a sequence with exactly 10 frames sets bit 2); found → bit 1 = (frames ≥ 2); not found anywhere → colour 0xD1 and bit 0 set. Bit 0 from the file is never cleared, so a coloured primitive stays flat even with a texture.
  - **Blit**: at (int(x − scroll x·65536) + 128, int(z − scroll y·65536) − (int(y) >> 1) + 32) minus the hotspot, transparency key 1 (or translucent when the unit's +0x10E bit 2 / crosshair mode).
  - **Uncached pieces** (visible, cached flag clear) are drawn every frame straight onto the frame after the sprite (`Model_DrawPiecePrimitivesDirect` `0x004584D0`): point x = int(px + unit x − scroll x·65536) + 128, y = int(unit z − scroll y·65536 − pz) − (int(py + unit y) >> 1) + 32 (note the different rounding: unit and point fractions are added before truncation), no depth, clipped to the view. The non-depth textured quad / polygon routines used there (`0x004C7580`, `0x004C0330`) are assumed to follow the same scan rules (not yet differential-tested).
  - Units with the `zbuffer` flag or under construction use a second path with a depth plane (`MemCache_AllocMaskBufferDual`, composite with attached units, water clipping by sea level): not modelled. The "shaded" path (`0x00459C70`, under construction with a visual option) is not modelled.
- **Polygon scan conversion** (`Gfx_RasterizeDepthPoly` `0x004C1000`, `Gfx_DrawTexturedQuadDepthTested` `0x004C8760` + span `0x004C7A20`, flat span `0x004C06E0`) — **differential-tested** (`tools/orig_harness/raster_harness`):
  - Top vertex = first with the smallest y, bottom = first with the largest y. Reject if max x < clip x0 (textured only), min x > clip x1, max y < clip y0, min y > clip y1. Clamp min y up to clip y0 and max y down to **clip y1** (so the last row is never drawn); nothing if they are equal.
  - Left chain: from the top vertex backwards (index − 1, wrapping) to the bottom; right chain: forwards. Each edge with y_a < y_b: x step = ((x_b − x_a)·65536) / (y_b − y_a) (32-bit, C division), x starts at x_a·65536 + 0xFFFF; texture u/v and depth z start at value·65536 with steps ((b·65536) − (a·65536)) / dy. Rows above the clip are skipped by stepping. Rows y_a .. min(y_b, max y) − 1 are written **through a moving row pointer** (not indexed by y), integer x = x >> 16.
  - Rows min y .. max y − 1: span when right − left > 0. Span steps = (right − left attribute) / width (C division); a left end before clip x0 is stepped forward; the right end is clamped to **clip x1** (so the last column is never drawn); pixels left .. right − 1.
  - Texel = texture[(u >> 16) + (v >> 16) · width] (power-of-two widths use shifts with the same result); quads map vertex 0..3 to (0,0), (w−1,0), (w−1,h−1), (0,h−1); texel colours are copied as-is (no key).
  - With a depth plane: write when depth byte ≤ (z >> 16) & 0xFF, and store it.
- **Shadows** (composite `0x00459200`, 1-byte path): when shadows are on (visual flags bit 2) and the type lacks `noshadow`: ground units (unit +0x110 bit 29 clear, or `digger`), with visual bit 3 and not `canhover`/`floater` → copy the sprite, set every non-key pixel to 0 (`Gaf_CopyFrameToShadow` `0x0045A470`) and blend it through the ALP table at (x + 5, y computed with the **terrain height** at the unit instead of the unit's height), before the sprite. Airborne units get a ground-projected shadow built from the model (`UnitTable_RasterizeAndCompressShadow` `0x0045A790`); we use the ground-unit shadow for them (approximation).
- **Features** (`Map_BlitFeatureGaf` `0x0046A610`): x = footprint x·16/2 + cell x·16 + 128 − scroll x; y = footprint z·16/2 − (sum of the 4 corner cell heights >> 3) + cell z·16 + 32 − scroll y. Shadow sequence frame 0 first (when visual flags bit 4), blended if `shadtrans`; then the sequence frame 0, blended if `animtrans`. Animating features use their cursors; 3D features are drawn through a proxy unit with `Unit_UpdateAndDrawModel` (not modelled yet).
- **GAF frames**: header {u16 w, u16 h, i16 x offset, i16 y offset, u8 transparency index, u8 compressed, u16 layer count, u32, u32 data, u32}; layered frames hold a table of layer frame offsets (drawn in order; blended layers per their flag). Compressed rows: u16 byte count, then codes: bit 0 → skip code >> 1 pixels; bit 1 → repeat the next byte (code >> 2) + 1 times; else copy (code >> 2) + 1 bytes. A frame drawn at (x, y) lands at (x − x offset, y − y offset). Blend blits mix through the ALP table only when the display context's flag 0x20 is set.
- **Projectiles** (`Gfx_DrawProjectiles` `0x0049BE60`), by the weapon's render type (+0x10C): 0 line from the shot to its tail in HUD palette colour [colour] (with a second, offset line in [colour2] when set); 1/3/6 3DO model with a blended shadow blob at the shot's ground height (`Gfx_DrawModel3doProjected` `0x0046BAE0`, not yet specified); 2 explosion-lens frame; 4 shell GAF (cannon / plasma sizes) at frame (tick − spawn) mod frames; 5 flame stream frame by remaining life; 7 lightning: two passes of jittered segments every 5 world units using **CRT `rand()`**. Shots hidden by the viewer's fog/LOS are skipped. Implemented: lines, sprite shots and model shots (as a sprite with all pieces); colours and frames are supplied by the caller.
- **Lines** (`Gfx_BlitClipped` `0x004BE950`): clipped one-colour line; we use Bresenham (tie pixels not verified).
- **Render coupling — the original's draw mutates simulation-visible state:**
  1. Drawing a unit syncs its pose (base angles within 7 units; transforms) — the sim can later observe base angles that depend on whether the unit was drawn (already noted under "Piece pose"). The renderer draws from a synced **copy** and never writes the unit.
  2. Lightning projectiles (render type 7) call CRT `rand()` while drawing, on the main thread that also runs the simulation, so the CRT random sequence the simulation sees depends on which lightning shots were on screen (and on the frame rate). The renderer does not draw lightning yet; when it does, it must use its own random source.
  3. Feature drawing writes the deferred-tall-feature bit (0x04) into the map cells each frame; cell flags are otherwise simulation data.
  4. Texture animation cursors advance in the simulation step (`Gaf_AdvanceAllFrameCursors`), not in the draw.
- **Exact vs approximate (current state):** exact by construction or test — projection, tile terrain, polygon scan conversion (tested), sprite bounds/hotspot/key, piece/primitive order, texture binding, flat colours, team-colour frames, blend table, feature placement, draw order buckets. Approximate — view size and row-bucket counts (assumed), line pixels, air-unit shadows, uncached-piece path (rules assumed equal to the tested ones), animated texture frame selection timing, model projectiles, depth-plane/zbuffer and shaded paths, carried units, LOS/fog, overlays.

## Feature log
| Date | Feature | RE'd addresses | Engine files |
|---|---|---|---|
| 2026-10-14 | Battle view renderer: projection, tile terrain, model sprites, uncached pieces, shadows, features, projectiles (lines/sprites/models), draw order; palette + ALP blend table, GAF frames, TNT tiles, texture binding; CPU reference + SDL3 GPU backend (D3D12/Vulkan/Metal) — **scan conversion: 7,000 random polygons identical; GPU = CPU reference (0 pixels, D3D12 and Vulkan)** | `0x00468CF0`, `0x00483FA0`, `0x0045AC20`, `0x00458810`, `0x004586A0`, `0x004581E0`, `0x00459830`, `0x004584D0`, `0x00459200`, `0x004C1000`, `0x004C06E0`, `0x004C8760`, `0x004C7A20`, `0x0046A610`, `0x004B7F90`, `0x004B8500`, `0x0042A140`, `0x0042A440`, `0x004BA750`, `0x0049BE60`, `0x0045A470`, `0x00437B50`, `0x004CC51D` | `render/*`, `ta_compat/src/render_assets.cpp`, `render/tools/battle_view.cpp`, `tools/orig_harness/raster_harness.cpp` |
| 2026-10-14 | Unit creation (limits, entry choice, field set-up, script/object state, weapon slots, metal extraction, motion hook, settle, claim, packets 9/0x12, activation, feature types, counters) — **smoke: 24 scenarios / 1,675 operations / 440 creations identical; large runs pending** | `0x00485F50`, `0x00485A40`, `0x00485D40`, `0x0049E070`, `0x00437840`, `0x0048A160`, `0x00401070`, `0x00456050`, `0x004560C0` | `engine/sim/src/unit_create.cpp`, `engine/sim/include/sim/unit_create.h`, `ta_compat/src/battle_types.cpp`, `tools/orig_harness/create_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-14 | Footprints and occupancy (claim with contests, release, clear on death, wanted-cell claims, area and forced re-claims, yard check and opening, cell height ranges, spatial buckets) — **smoke: same run, 0 differences; 4 predicted original crashes; large runs pending** | `0x0047CC30`, `0x0047D0E0`, `0x0047CBD0`, `0x0047C790`, `0x0047D970`, `0x0047DAC0`, `0x0047E5C0`, `0x0047CAC0`, `0x0047ED30`, `0x0047DB20`, `0x00483210`, `0x00481550`, `0x00482C20` (bucket sizes) | `engine/sim/src/footprint.cpp`, `engine/sim/include/sim/footprint.h`, `engine/sim/src/battle.cpp` |
| 2026-10-14 | Weapon fire routines (turret spread, vertical launch + interceptor search, line of sight, dropped) — **smoke: 2,820 fire-routine calls identical; large runs pending** | `0x0049D580`, `0x0049DB70`, `0x0049D9C0`, `0x0049DD60`, `0x0049CC20`, `0x0049D120` | `engine/sim/src/{fire,projectile}.cpp`, `tools/orig_harness/projectile_harness.cpp` |
| 2026-10-14 | Weapon ballistic pitch solver + C runtime acos (FPATAN at PC53, double rounded) — **1,428,570 solver calls identical; acos 1,000,009 calls, 10 CPU knife-edge cases** | `0x0049A890`, `0x004E67F0`, `0x004E680D` | `engine/sim/src/{weapons,x87_math}.cpp`, `tools/orig_harness/ballistic_check.cpp` |
| 2026-10-14 | Weapon slot pass + aiming (target position, veteran lead, sweet spot, aim-from/query pieces, direct and ballistic aim, aim scripts and sink, broadcast, range check, tolerance, reload, stockpile, shot payment, stale word) — **3,000 scenarios / 300,000 unit passes + 69,483 direct slot passes / 242,937 helper calls / 15,025 shots identical** | `0x0049E1A0`, `0x0048A1E0`, `0x0048A190`, `0x0043E3C0`, `0x0043E0B0`, `0x0043E2E0`, `0x0043E240`, `0x0049E570`, `0x0049D910`, `0x0049AA80`, `0x0049D880`, `0x00456200`, `0x004012A0`, `0x004B07C0`, `0x004B0A70`, `0x004B0BC0`, `0x00481490`, `0x0049E070` | `engine/sim/src/weapons.cpp`, `engine/sim/include/sim/weapon_slot.h`, `tools/orig_harness/weapons_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-14 | Unit order lists: mission table, order construct/insert/prepend/remove/clear/destroy, target links, primary and secondary processing, default missions, paralysis orders, stale-word effects (behaviours hooked) — **3,000 scenarios / 300,000 unit passes / 366,066 operations / 734,124 behaviour steps / 248,743 orders freed identical** | `0x0043B7C0`, `0x0043BAD0`, `0x0043A0C0`, `0x0043A1F0`, `0x0043AC60`, `0x0043ACB0`, `0x00439F80`, `0x00439EB0`, `0x0043C050`, `0x00438760`, `0x004895C0`, `0x00489650`, `0x00489690`, table `0x00512344` | `engine/sim/src/orders.cpp`, `tools/orig_harness/orders_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-14 | Ground settle + slope fit (terrain/water/waterline heights, footprint slope pitch/bank, hover bob) — **52,577 direct settles (11,169 slope fits) + every pass identical** | `0x0048A870`, `0x0048A490`, `0x004B6340` (clock, input) | `engine/sim/src/ground_settle.cpp`, `ta_compat/src/model3do.cpp` (`selection_footprint`) |
| 2026-10-14 | Projectiles: table/allocation with stale slots, launch set-up, directed/ballistic/ownerless/motion launches, flight by weapon kind, bursts/spray/decay, seeking/cruise/two-phase, hit tests (units, features, ground, water, map edge), direct and area damage, interceptor chains, compaction, detonate packets, death explosions — **2,950 scenarios / 809,096 updates identical** | `0x0049B6E0`, `0x0049C740`, `0x0049C9C0`, `0x0049CDE0`, `0x0049D000`, `0x0049DF10`, `0x0049B720`, `0x0049B090`, `0x0049B3E0`, `0x0049B520`, `0x0049AE20`, `0x00499EB0`, `0x00499CD0`, `0x0049A120`, `0x00406F50`, `0x0049AF90`, `0x0049C880`, `0x0049B000`, `0x00421EB0`, `0x0049A850` | `engine/sim/src/projectile.cpp`, `tools/orig_harness/projectile_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-14 | Stale stack word as the wreck level of units without `Killed` (wind, script step, water damage, heal, kill effects; original heap addresses) — **3,000 scenarios incl. all 22 Killed-less types, word compared after every pass/kill: 0 differences** | `0x004864B0`, `0x0048AD30`, `0x00437910`, `0x004B0D60`, `0x004B1C00`, `0x00489BB0`, `0x0041BD10`, `0x0043B7C0`, `0x0048A870` | `engine/sim/src/{combat,unit_tick,script_vm}.cpp`, `tools/orig_harness/combat_harness.cpp`, `tools/capture/capture.cpp` |
| 2026-10-14 | Damage, repair/self-heal, kills (packets, veterancy, severity/wreck, statistics, reclaim, teardown) — **3,000 scenarios / 600,000 unit passes / 656,853 combat operations / 26,934 kills identical** | `0x00489BB0`, `0x00489CE0`, `0x0041BD10`, `0x004864B0`, `0x004866D0`, `0x00467950`, `0x0044FDB0`, `0x0044FE40`, `0x0044FFD0` | `engine/sim/src/combat.cpp`, `tools/orig_harness/combat_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-14 | Per-step unit pass (wind, script step, timers, selection, health %, water damage, self-heal gates; subsystems as ordered hooks) — **1,000 scenarios / 253,024 passes / 65,843 script calls with real scripts identical** | `0x0048AD30`, `0x00437910`, `0x0048B090` (activation side calls `0x0047F780`, `0x0041C110`, `0x00451DF0`) | `engine/sim/src/unit_tick.cpp`, `tools/orig_harness/unitloop_harness.cpp` |
| 2026-10-14 | Unit state, script unit values (GET/SET/transport), heightmap + TNT heights — **116,867 GET / 33,341 SET / 16,592 transport calls on all 278 units identical** | `0x00480770`, `0x00480B20`, `0x00481430`, `0x00481470`, `0x00485070`, `0x0043DEF0`, `0x0043E060`, `0x0048B090`, `0x0047DAC0`, `0x00483610` | `engine/sim/src/{unit,heightmap}.cpp`, `ta_compat/src/tnt.cpp`, `tools/orig_harness/unitvalue_harness.cpp` |
| 2026-10-13 | Piece pose (object state build, script piece callbacks, transform sync) + bit-exact x87 FSINCOS rotation — **pose: 278 units / 110,000 ops identical; rotation: 524,288 calls identical; FSINCOS emulation = hardware on all 65,535 angles** | `0x0045A950`, `0x0045AEC0`, `0x0045AF90`, `0x0045AB10`, `0x0045B030`, `0x0045B0A0`, `0x0045B150`, `0x004B6CC0`, `0x004B7173`, `0x00480C30`–`0x00480DF0`, `0x0045AC20` | `engine/sim/src/pose.cpp`, `x87_trig.cpp`, `ta_compat/src/model3do.cpp` (`to_pose_model`), `tools/orig_harness/{pose_harness,x87_check,remote.h}` |
| 2026-10-13 | Unit script VM + COB importer — **differential vs original interpreter: 278 stock scripts (580,539 ops) + 3,000 synthetic programs (164,145 ops, every opcode), 0 differences** | `0x004B2450`, `0x004B0610`, `0x004B0720`, `0x004B08C0`, `0x004B0940`, `0x004B0B00`, `0x004B0C40`, `0x004B0D60`, `0x004B0DA0`, `0x004B1C00`, `0x00485D40`, vtable `0x004FD698` | `engine/sim/src/script_vm.cpp`, `ta_compat/src/cob.cpp`, `tools/orig_harness/cob_harness.cpp`, `tests/sim_tests.cpp` |
| 2026-10-13 | Feature definitions + unit corpse indices + recursive VFS enumeration — **exact vs live game: 434 features, 8,307 fields (incl. model trees and GAF sequence names), 279 corpse indices** | `0x004917D0`, `0x004222E0`, `0x00483610`, `0x00421F20`, `0x00423160`, `0x00436C30`, `0x00422E40`, `0x004224B0`, `0x00422EA0`, `0x004BCB50`, `0x004BC4B0`, `0x004BC640`, `0x004B8C60`, `0x004B8D40`, `0x0042A2C0`, `0x004C4760` | `ta_compat/src/featuredefs.cpp`, `gaf.cpp`, `hpi.cpp` (`enumerate_tree`), `tools/feature_check.cpp` |
| 2026-10-13 | 3DO model loader + unit model height — **exact vs live game: 278 unit + 74 weapon model trees; unit table now 25,617 fields** | `0x004CB560`, `0x004CB4C0`, `0x004CB370`, `0x004CB590`, `0x004CB5F0`, `0x0045AE80` | `ta_compat/src/model3do.cpp`, `tools/model_check.cpp` |
| 2026-10-13 | Weapon definition table — **exact vs live game: all 256 slots, 9,141 fields** | `0x0042E310`, `0x0042E440`, `0x0049E010`, `0x00429470`, `0x0042F7E0` | `ta_compat/src/weapondefs.cpp`, `tools/weapondef_check.cpp` |
| 2026-10-13 | Unit definition table (all fields except 3DO height / COB) — **exact vs live game: 24,780 fields, 279 units, 56 category masks** | `0x0042A8D0`, `0x0042D2E0`, `0x0042BF40`, `0x0042DCF0`, `0x0042BE30`, `0x00440340`, `0x004402E0`, `0x00440420`, `0x0049E5B0`, `0x00488B10`, `0x00488C50`, `0x00488E70`, `0x00438760`, `0x004290F0`, `0x004BCA30`, `0x004BC4B0`, `0x004BC640`, `0x004BC370`, `0x004C58A0`, `0x004BB0F0`, `0x0042F580`, `0x0042E310` | `ta_compat/src/unitdefs.cpp`, `tools/unitdef_check.cpp`, capture `--dump-defs` |
| 2026-10-13 | Bit-exact atof/atoi | `0x004E4560`, `0x004EAF00`, `0x004F3D10`, `0x004F8C40`, `0x004F9390`, `0x004F90D0`, `0x004F37C0`, `0x004F3610`, tables `0x005116F0`/`0x00511850` | `ta_compat/src/crt_strtod.cpp`, `tools/gen_pow10_table.py`, `ta_compat/tools/orig_harness` |
| 2026-10-13 | TDF parser | `0x004C2F60`, `0x004C3120`, `0x004C33A0`, `0x004C3410`, `0x004C3E40`, `0x004C4340`, `0x004C4470`, `0x004C4630`–`0x004C48C0`, `0x004E4560`, `0x004E4ED0`, `0x004F8A70`, `0x004B6BA0` | `ta_compat/src/tdf.cpp`, `tools/tdf_check.cpp`, `tests/compat_tests.cpp` |
| 2026-10-13 | HPI archives + game VFS | `0x0041D4C0`, `0x004BB2E0`, `0x004BB4E0`, `0x004BB7C0`, `0x004BDD70`, `0x004BE010`, `0x004BE0B0`, `0x004BE400`, `0x004D1480`, `0x004D1970`, `0x004D1D20` | `ta_compat/src/hpi.cpp`, `inflate.cpp`, `tools/hpi_check.cpp` |
| 2026-10-13 | Step scheduler + frame budget | `0x00495230`, `0x00495490`, `0x004E5050` | `engine/sim/src/world.cpp`, `step_budget.cpp` |
| 2026-10-13 | Numeric core: trig, RNGs, ftol, fixed point | `0x004B6C30`–`0x004B7159`, `0x004E4860`, `0x004E4870`, `0x004E43A0`, `0x00509F00`, `0x0051FC88` | `engine/sim/*`, `tests/sim_tests.cpp` (1316 checks pass, MSVC) |
