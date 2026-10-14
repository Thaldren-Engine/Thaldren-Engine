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
    - On success, flag bit2 = value&1 (so 2 tests "open" but stores closed), flags |= 0x08000000, claim cells, update passability. Occupancy is not built yet, so this is an engine hook.
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

## Feature log
| Date | Feature | RE'd addresses | Engine files |
|---|---|---|---|
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
