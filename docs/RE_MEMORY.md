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
| `0x004CB560` | `Model3d_Load` | — | Relocate the 3DO tree in place, then flip X/Z; see 3DO models | Verified: live model dump |
| `0x004CB4C0` | `Model3d_RelocatePointers` | — | File offsets → pointers; sibling, then child, then sort | Verified: live model dump |
| `0x004CB370` | `Model3d_SortPrimitivesByDepth` | — | Selection primitive to [0]; bubble sort by average Y | Verified: live model dump |
| `0x004CB590` | `Model3d_NegateAxesXZ` | — | Negate X/Z of vertices and offsets | Verified: live model dump |
| `0x004CB5F0` | `Model3d_GetHeight` | — | Max Y over the tree, ≥ 0 | Verified: unit table Y extents |
| `0x0045AE80` | `Model3d_CountTreeNodes` | — | Piece count | Disasm |

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
| 2026-10-13 | 3DO model loader + unit model height — **exact vs live game: 278 unit + 74 weapon model trees; unit table now 25,617 fields** | `0x004CB560`, `0x004CB4C0`, `0x004CB370`, `0x004CB590`, `0x004CB5F0`, `0x0045AE80` | `ta_compat/src/model3do.cpp`, `tools/model_check.cpp` |
| 2026-10-13 | Weapon definition table — **exact vs live game: all 256 slots, 9,141 fields** | `0x0042E310`, `0x0042E440`, `0x0049E010`, `0x00429470`, `0x0042F7E0` | `ta_compat/src/weapondefs.cpp`, `tools/weapondef_check.cpp` |
| 2026-10-13 | Unit definition table (all fields except 3DO height / COB) — **exact vs live game: 24,780 fields, 279 units, 56 category masks** | `0x0042A8D0`, `0x0042D2E0`, `0x0042BF40`, `0x0042DCF0`, `0x0042BE30`, `0x00440340`, `0x004402E0`, `0x00440420`, `0x0049E5B0`, `0x00488B10`, `0x00488C50`, `0x00488E70`, `0x00438760`, `0x004290F0`, `0x004BCA30`, `0x004BC4B0`, `0x004BC640`, `0x004BC370`, `0x004C58A0`, `0x004BB0F0`, `0x0042F580`, `0x0042E310` | `ta_compat/src/unitdefs.cpp`, `tools/unitdef_check.cpp`, capture `--dump-defs` |
| 2026-10-13 | Bit-exact atof/atoi | `0x004E4560`, `0x004EAF00`, `0x004F3D10`, `0x004F8C40`, `0x004F9390`, `0x004F90D0`, `0x004F37C0`, `0x004F3610`, tables `0x005116F0`/`0x00511850` | `ta_compat/src/crt_strtod.cpp`, `tools/gen_pow10_table.py`, `ta_compat/tools/orig_harness` |
| 2026-10-13 | TDF parser | `0x004C2F60`, `0x004C3120`, `0x004C33A0`, `0x004C3410`, `0x004C3E40`, `0x004C4340`, `0x004C4470`, `0x004C4630`–`0x004C48C0`, `0x004E4560`, `0x004E4ED0`, `0x004F8A70`, `0x004B6BA0` | `ta_compat/src/tdf.cpp`, `tools/tdf_check.cpp`, `tests/compat_tests.cpp` |
| 2026-10-13 | HPI archives + game VFS | `0x0041D4C0`, `0x004BB2E0`, `0x004BB4E0`, `0x004BB7C0`, `0x004BDD70`, `0x004BE010`, `0x004BE0B0`, `0x004BE400`, `0x004D1480`, `0x004D1970`, `0x004D1D20` | `ta_compat/src/hpi.cpp`, `inflate.cpp`, `tools/hpi_check.cpp` |
| 2026-10-13 | Step scheduler + frame budget | `0x00495230`, `0x00495490`, `0x004E5050` | `engine/sim/src/world.cpp`, `step_budget.cpp` |
| 2026-10-13 | Numeric core: trig, RNGs, ftol, fixed point | `0x004B6C30`–`0x004B7159`, `0x004E4860`, `0x004E4870`, `0x004E43A0`, `0x00509F00`, `0x0051FC88` | `engine/sim/*`, `tests/sim_tests.cpp` (1316 checks pass, MSVC) |
