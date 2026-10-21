# Fixed-point and integer arithmetic: audit against `TotalA.exe`

Audit date 2026-10-21. Scope: `engine/sim` (src, include, trig, rng, x87 helpers), `engine/net`
(bitstream, codec) and the simulation-facing parts of `ta_compat/src` (unit and weapon
definitions, TDF numbers, battle loader, saved game, campaign, COB loading). Evidence comes from
the disassembly (`dumpbin /disasm` of `TotalA.exe`, addresses below) and the existing
differential harnesses (`ta_compat/tools/orig_harness/*`) that the code was built against.

Verdicts: **OK** (matches the original's instruction semantics), **FIXED** (changed in this
audit, with a test), **OPEN** (a known, deliberate difference, with the reason).

## 1. Formats

The table also appears in `docs/RE_MEMORY.md` ("Fixed-point formats").

| Quantity | Original storage (where checked) | Rewrite type | Notes |
|---|---|---|---|
| World position x, y, z | int32 16.16, unit +0x6A/+0x6E/+0x72; whole part read as `movsx word [+0x6C/+0x70/+0x74]` | `std::array<std::int32_t,3>` / `Vec3i`; `hi16()` = `(int16)(v >> 16)` | 1 map cell = 16 whole units = 2^20. Cell = whole >> 4 (unsigned compare where the original uses `jae`), or pos >> 20 (arithmetic) depending on the site. |
| Velocities, speed, max velocity, acceleration, brake rate | int32 16.16 per step (def +0x192 max velocity etc.) | `std::int32_t` | FBI values converted with `ftol(value * 65536 / 30)`-style constants (`get_fixed`, `ftol32`). |
| Gravity | int32 16.16 per step² | `std::int32_t` | `ftol(g * 65536 / 900)` (battle_loader). |
| Angles (heading, yaw, elevation, turn targets) | uint16, 65536 per turn; pitch/bank int16 (unit +0x64..+0x69) | `std::uint16_t` (`Angle`) / `std::int16_t` | Differences taken as `(int16)(a - b)`; wraps by 16-bit truncation (C++20: modular). |
| Turn rate | uint16 (def +0x1BA, `mov dx,word; and 0xFFFF` at 0x43CBCA, 0x43D014) | `std::uint16_t` | Turn per step int16. |
| Trig | int16 Q13 table, 640 entries at 0x509F00 | `kTable` int16 | sin = tbl[a>>7], cos = tbl[(a>>7)+128]; rounded multiply `(tbl*mag + 0x1000) >> 13` low 32 bits of a signed 64-bit product (0x4B70EF / 0x4B7123). |
| atan2 result | int (65536/2π scale, `FISTP`) | `int32` | `x87::atan2_scaled`. |
| Map heights | uint8 whole units per cell (+ max/min uint8) | `std::uint8_t` | Bilinear sample with C (truncating) /16; sea level uint8. |
| Hit points | int16 signed, unit +0x108 (every load `movsx`, 11 sites) | `std::int16_t` | Damage packet amount uint16; `hp -= amount` is a 16-bit `sub word`. |
| Max damage | uint32 (def +0x1FA) | `std::uint32_t` | Percent = `(u32)(hp*100) / max` unsigned divide. |
| Damage modifier, bank/pitch scale, tilt | int32 16.16 | `std::int32_t` | Products via signed 64-bit (`imul` + `__allshr`, 0x489BEA). |
| Kill count (veterancy) | uint16, +0xB8 (`xor ecx; mov cx,word`) | `std::uint16_t` | `/5` signed magic division. |
| Build progress | float (x87 single), `build_fraction_left` | `float` | Not fixed point: x87 model (RE_MEMORY "Numeric model"). |
| Energy, metal, storage, income | float | `float` | Not fixed point; integers enter through `__ftol` (truncate, low 32 bits). |
| Worker time, heal time, build distance, sight/radar/sonar (+jammers) | int16 (def +0x202 sight etc.) | `std::int16_t` | Whole units; sight ring index `s/32 - 5` (C division). |
| Build time | int32 | `std::int32_t` | |
| Weapon range / velocity / reload / area | range int32 whole units, velocity int32 16.16 per step, reload/timers int16 steps, area int16 | same | Expire = `(u32)(range << 16) / (u32)velocity` (unsigned `div`, 0x49CB3A). |
| Footprint, cell coordinates | int16 (`movsx` at 0x40A448), packed cells uint32 (x | z << 16) | `std::int16_t` / `std::uint32_t` | Footprint centre `(footprint + 2*cell) << 19`. |
| Wind | speed int32, heading uint16, drift int32 16.16 | same | |
| COB values | uint32 stack words; linear = 16.16 (×65536 per unit), angular = uint16 (masked `& 0xFFFF`), speeds = value / tick rate (signed `idiv` by 30) | `std::uint32_t` words | See section 5. |
| RNG states | uint32 | `std::uint32_t` | Battle RNG unsigned divide/modulo; CRT LCG. |
| Game tick | uint32 | `std::uint32_t` | |

## 2. Undefined and implementation-defined behaviour

Language baseline: C++20. That makes `<<` of negative values, `>>` of negative values
(arithmetic) and narrowing to `int16_t`/`int8_t` well defined and modular, i.e. exactly `shl`,
`sar` and `mov word/byte`. What stays undefined is **signed overflow of `+ - *`**,
**division by zero**, **`INT_MIN / -1`**, **shift counts >= width** and **float-to-int
conversion out of range**.

**MSVC does exploit signed-overflow UB.** A probe built with this toolchain (`cl /O2`, VS 18)
folded `(x * 2) / 2 == x` to `true` for `x = 0x40000001` (the original's `imul` wraps, giving
false). There is no MSVC switch to turn this off (`/d2UndefIntOverflow-` is rejected by this
compiler). The rewrite therefore routes every add/sub/mul that can wrap through unsigned helpers
(`wadd`/`wsub`/`wmul`/`wshl`/`lo32`, `sim/fixed.h` `wrap_*`), and products of 16.16 values
through `int64` exactly where the original uses `imul` 32x32->64 + `shrd`/`__allshr`. GCC and Clang
builds now also get `-fwrapv` (CMakeLists.txt) as a safety net.

**MSVC also miscompiles a well-defined pattern.** `static_cast<int32_t>(0u - x) / k` (and the
same with a signed `-v` that cannot overflow, then `(uint32_t)v << n`, then `/ k`) is compiled
at `/O2` as `-((int32_t)x / k)`: `movsx; shl; cdq; sub; sar; neg`. That is wrong when the
negated value is 0x80000000 (the original gives −0x40000000 for a halving, the rewrite built
with MSVC gave +0x40000000). Reproduced in isolation for k = 2, 3, 16 (`/Od` is correct; `%`
is not affected). It was hit by the unit-definition footprint box for footprint 2048 (FIXED
below). Remedy: write such divisions as the original's own idiom, `(int32_t)(w + (w >> 31)) >> 1`
for a halving (cdq; sub; sar), which the optimizer leaves alone. Other places that divide a
negated value were searched (`neg(`, `0u -`, `-(`, `* -k` followed by `/`): none besides the
footprint box; a negation and a division in different statements after inlining cannot be
excluded by grep (OPEN, needs the value to be exactly 0x80000000).

Scan (scripted, every `*`, `/`, `%`, `>>`, `+=`/`-=` in the scope above): about 320 raw
multiplications, 860 division/modulo tokens, 280 right shifts. Raw signed `*`/`+`/`-` that remain
operate on promoted 8/16-bit fields (cannot overflow `int`), on loop/index arithmetic bounded by
table sizes, or on constants. The sites below are the ones where overflow was possible with
some input and the original wraps.

Shift counts: every variable shift is masked (`& 31`, matching x86) or bounded (bitstream
`mask(bits)` handles 32). Float to int: every simulation conversion goes through
`x87::ftol32/ftol64` (truncate, NaN/out of range -> integer indefinite, low 32 bits kept),
except the two FIXED below.

## 3. Division by zero, `INT_MIN / -1`, out-of-bounds

The original faults (#DE, access violation) or reads arbitrary memory in these cases. The
rewrite cannot reproduce a crash or another process's memory, so its policy is: detect the
condition, record a fault string on the subsystem (`fault`/`set_fault`), and continue with a
defined value (usually 0 or the scratch word). Divisions by zero guarded this way: script VM
`Div` (also `INT_MIN / -1`), motion turn/brake/max velocity, flame/wake effects (6 sites),
nano spray (`INT_MIN / -1` too), AI site grid and site scaling, route budget refresh, unit
state packets, mission goal row division, burst speed (`udiv`), capture of a 0-hp type,
health percent (`hp_percent_raw`, `unit_get_value`), burn-blow shells. Out-of-bounds table
reads guarded the same way: about 60 sites (type/feature/unit/order tables, map cells,
COB code/stack/statics/pieces, weapon slots, alliance bytes). **OPEN by policy:** these are
detected, not emulated; a run that hits one has already left the original's defined
behaviour (the harness runs showed the original crashing at the same points, e.g. 239
projectile scenarios with burst speed 0).

Where the original's own code makes an out-of-range read benign the rewrite reproduces it
exactly: the trig table's 128-entry tail (cos of `a >> 7` up to index 639), the COB stack
overrun of thread 7 into the live-thread count, stale stack words (`stale_word` paths),
uninitialised locals in `CallInOut`.

## 4. Site table

### FIXED

| Rewrite site | Original | Operation | Verdict |
|---|---|---|---|
| `engine/sim/src/battle_start.cpp:70` (mission start amounts to the storage bonus) | 0x465F2E `fld dword; call __ftol` (x2) | float -> int | **FIXED.** Was `static_cast<int32_t>(float)`: UB for NaN / |v| >= 2^31 (MSVC gives 0x80000000). Now `mission_storage_amount()` = `ftol32` (low 32 bits of the int64 truncation). Test: sim_tests (3e9 -> 0xB2D05E00, NaN -> 0, 1e20 -> 0). |
| `engine/sim/src/battle_start.cpp:47,51` (skirmish defaults) | rewrite-only fallback (the original's skirmish path is `fild dword`, 0x465F00) | float -> int | **FIXED** (UB removed; same values for every finite in-range default). |
| `engine/sim/src/orders.cpp:177` `add_paralysis` | 0x489E69 `add dword [eax+36h], ebx` (ebx zero-extended amount, `xor ebx,ebx; mov bx,word`) | int32 add | **FIXED.** Was `param0 += amount` (signed overflow UB). Now an unsigned wrapping add. Test: sim_tests (0x7FFFFFF0 + 0x20 -> 0x80000010). |
| `engine/sim/src/sky.cpp:73-76` camera shake add | 0x41C67B..0x41C6CB: `add; cdq; sub; sar 1`, `add [accum_x]`, `add [accum_y]` | int32 add, C halving | **FIXED.** Accumulator adds and the magnitude sum were raw signed adds. Now wrapping; halving stays C division (truncation). Test: sim_tests (0x7FFFFFFF + 2 wraps; (-5 + 2) / 2 = -1, not -2). |
| `engine/sim/src/sky.cpp:92-93` shake offsets | 0x41C74E `cdq; sub; sar 1; sub esi,eax` | int32 sub | **FIXED** (wrapping sub; values unchanged in range). |
| `engine/sim/src/sky.cpp:58,60` meteor drop point | meteor spawn: `(aim << 20) - mul_sin(...)` | int32 sub | **FIXED** (wrapping sub). |
| `ta_compat/src/unitdefs.cpp` `footprint_model_box` | 0x42D079..0x42D11F: `movsx; neg; shl 14h; cdq; sub; sar 1`, `sub` for sizes, `add` then magic /3 | int32 shift/sub/add | **FIXED.** Was `footprint * -0x100000` (UB for |footprint| >= 2048, any FBI int16 is accepted) and raw `-`/`+`. Now `(u32)(-f) << 20` halved with the add-sign-bit-and-`sar` idiom (a plain `/ 2` here is miscompiled by MSVC, section 2), wrapping sub/add. Test: compat_tests (footprint 2048 gives min x = max x = −0x40000000 as in the original; 4095). |
| `CMakeLists.txt` | - | build flags | **FIXED** (`-fwrapv` for GCC/Clang). |

### OK (checked against the instructions)

| Rewrite site | Original | Operation | Verdict |
|---|---|---|---|
| `engine/sim/src/script_vm.cpp:346-348` Add/Sub/Mul | 0x4B0DA0 interpreter, `add`, `sub`, `imul ecx,edx` (0x4B14F1) | 32-bit wrap | OK (all on `uint32_t`). |
| `script_vm.cpp:349-356` Div | `cdq; idiv ebx` (0x4B150F) | signed, truncating | OK; 0 and `INT_MIN/-1` fault (the original raises #DE) -> section 3. |
| `script_vm.cpp:360-363` Lt/Le/Gt/Ge | `setl/setle/setg/setge` (0x4B16F1, 0x4B1729, 0x4B174B, 0x4B1783) | signed compares | OK. |
| `script_vm.cpp:310` Sleep | 0x4B1366 `imul edx,ebx` then magic `0x10624DD3`, `sar 6`, `+ sign` (signed /1000) | wrap multiply, trunc divide | OK (`cdiv(s(u(tick_rate)*ms), 1000)`). |
| `script_vm.cpp:215,227,245,246,257` speeds / tick rate | `cdq; idiv [edi+4]` (0x4B0EDE, 0x4B0F7B, 0x4B1031, 0x4B1047, 0x4B10AB) | signed trunc divide | OK. |
| `script_vm.cpp:225,292` turn targets `& 0xFFFF`; `:229-235` direction choice | interpreter | uint16 angle | OK (differential: cob_harness). |
| `script_vm.cpp:390` Rand | `BattleRng_NextMod(hi - lo + 1) + lo` | uint32 wrap | OK. |
| `script_vm.cpp:515,529,540-562` animate | interpreter animation pass | wrapping adds/mults, sign-kept 0x10000 reduction | OK (all unsigned). |
| `engine/sim/src/trig.cpp` | 0x4B70A0 / 0x4B70C0 (`movzx; shr 6; and 0FFFEh; movsx`), 0x4B70EF / 0x4B7123 (`imul; add 1000h; adc 0; shrd 0Dh`) | Q13 table, rounded multiply | OK. The sin-rounded routine sign-extends its angle (`movsx` at 0x4B70FB) and cos zero-extends; after `+bias, >> 6, & 0x3FE` both give the same index, so the rewrite's `uint16` is exact. |
| `engine/sim/src/rng.cpp` | 0x4B6C30 | unsigned div/mod, signed `n < 2` and `<= 0` tests | OK. |
| `engine/sim/src/combat.cpp:72-79` | 0x489BB0: `imul edi` + `__allshr` 16 (armour), `mov cx,word [+0B8h]` + magic /5, `imul ecx,edi; shl 2`, magic /100 signed | 64-bit product, wrap, trunc | OK. |
| `combat.cpp:106,127` hit points | 0x489D5F `movsx [+108h]`; 0x489EB5 `sub word [+108h]`; `cmp word [+108h],0` | int16 | OK. |
| `combat.cpp:53` health percent | unsigned `div` by max damage | uint32 | OK (0 -> fault). |
| `engine/sim/src/effects.cpp:248-251` debris bounce | 0x421486 `sar ebx,1; neg; sar edx,1; sar ecx,1; cmp ebx,20000h` | floor halving (plain `sar`) | OK (`>> 1`, not `/ 2`). |
| `effects.cpp:357-358` shard bounce | 0x421073 `cdq; sub; sar 1; neg`; `cmp word [+3Ah],0; jg` | trunc halving | OK (`/ 2`, then `hi16 < 1`). |
| `engine/sim/src/ai.cpp:430-431` metal site offset | 0x40A448 `movsx; sub 3; cdq; sub; sar 1; neg` | trunc halving of int16 | OK. |
| `engine/sim/src/motion.cpp:84-91` turn clamp | 0x43CBCA `mov dx,word [+1BAh]; and 0FFFFh` | uint16 rate, signed compare | OK. |
| `motion.cpp:684` turn time | 0x43D014 `mov ax,word [+1BAh]` (zero-ext) then `__alldiv` | signed 64-bit divide, low 32 | OK (0 -> fault). |
| `motion.cpp:661-662, 686, 726` 16.16 divides | `__alldiv` pattern | int64 / int32, low 32 kept | OK (no #DE path: `__alldiv` never overflows). |
| `engine/sim/src/projectile.cpp:505-506, 571` shot lifetime | 0x49CB1F `test ecx,ecx; jz`, `test [+111h],8000000h`, `div ecx` (unsigned) | unsigned divide guarded by the original | OK. |
| `engine/sim/src/sky.cpp:86-87` shake amplitudes | 0x41C721 `imul eax,ebx; cdq; idiv esi` | wrap multiply, signed divide | OK. |
| `sky.cpp:88-89` random scaling | 0x41C737 `call rand; imul edi; __alldiv 8000h` | signed 64-bit, trunc | OK. |
| `engine/sim/src/battle_start.cpp:46-51` skirmish amounts | 0x465F00 `fild dword [ecx+ebp+10h]` | int32 | OK (values are ints in the original). |
| `ta_compat/src/tdf.cpp:42` `msvc_atoi` | 0x4E4ED0 CRT `atol`: `lea eax,[ebx+ebx*4]; lea ebx,[esi+eax*2-30h]`, `neg` at the end | wrapping accumulate | OK (accumulates in `uint32_t`). |
| `ta_compat/src/weapondefs.cpp:23` `scaled`, `tdf.cpp:220` `get_fixed` | `__ftol` | truncate, low 32 bits | OK. |
| `engine/sim/src/heightmap.cpp:22`, `ground_settle.cpp:45-54` bilinear | RE_MEMORY "C-truncating /16" (slope fit and heightmap sampler; harness-verified) | trunc division | OK. |
| `engine/sim/src/construction.cpp:1522` reclaim cell | RE_MEMORY "x / 2^20 rounded towards zero" | trunc division | OK. |
| `engine/sim/include/sim/x87.h` `ftol64/ftol32` | 0x4E43A0 `__ftol` | truncate, indefinite | OK. |

The rest of the simulation's integer code (motion, route search, targeting, construction,
features, sight, AI, missions, networking states) was built from the same disassembly with
explicit wrapping helpers and is covered by the differential harnesses listed in
`docs/RE_MEMORY.md` (projectile, combat, orders, unit value, create, feature, cob, sight,
route, AI) plus the whole-game replays g8/s2/g9 and the golden skirmish.

### OPEN

| Item | Reason |
|---|---|
| Division by zero, `INT_MIN / -1`, out-of-bounds reads (section 3) | The original crashes or reads unrelated memory; the rewrite detects and reports a fault instead. Emulation would need the original's whole address space. |
| Signed-overflow proof is by scan + review, not by a sanitizer | No UBSan-capable compiler is installed (MSVC only). With `-fwrapv` on GCC/Clang and the helpers on MSVC, an overflow missed by the review would only matter on MSVC, and only if the optimizer happened to fold that expression. |
| MSVC `(int)(0u - x) / k` miscompile across statements | Only wrong for a negated value of exactly 0x80000000; the grep found no same-expression site besides the fixed one. |
| `Math_MulHigh32` (0x4B70E0) | Not called as such; the rewrite's `square_hi` / `(int64)a*b >> 32` give the same high half. |

## 5. Script VM summary

Every opcode's integer arithmetic was compared with `UnitScript_ExecSlot` (0x4B0DA0): stack
words are `uint32_t`; add, sub, mul wrap; div is signed `idiv` (truncating); comparisons are
signed (`setl/setle/setg/setge`), equality and the bit operations are on the raw word; the
logical operators produce 0/1; `Rand` is `RandMod(hi - lo + 1) + lo` with the unsigned wrap;
`Sleep` is `(tick_rate * ms) / 1000` signed after a wrapping multiply; move/turn/spin speeds are
signed `value / tick_rate`; turn targets are masked to 16 bits; the animation pass uses
wrapping unsigned arithmetic. GetValue/SetValue go to `unit_get_value`/`unit_set_value`, which
are covered by `unitvalue_harness`.

## Counts

- Formats catalogued: 22.
- Sites checked against instructions in this audit: 36 table rows (28 OK, 8 FIXED), plus the script VM opcode set (section 5).
- Fault-reported original crash / OOB sites: about 75 (OPEN by policy).
- Tests added: sim_tests +11 checks (paralysis wrap, camera shake wrap and C halving, mission storage amounts; 5,848 -> 5,859), compat_tests +9 checks (footprint box; 137 -> 146).
