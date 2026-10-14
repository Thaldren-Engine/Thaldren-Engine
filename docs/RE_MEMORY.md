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
_(HPI, TNT, 3DO, COB, GAF, PCX, TDF/FBI, OTA, ... record only what has been verified)_

## Feature log
| Date | Feature | RE'd addresses | Engine files |
|---|---|---|---|
| 2026-10-13 | Numeric core: trig, RNGs, ftol, fixed point | `0x004B6C30`–`0x004B7159`, `0x004E4860`, `0x004E4870`, `0x004E43A0`, `0x00509F00`, `0x0051FC88` | `engine/sim/*`, `tests/sim_tests.cpp` (1316 checks pass, MSVC) |
