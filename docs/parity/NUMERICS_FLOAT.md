# Floating-point fidelity audit (x87 vs. the rewrite)

Audit of every floating-point computation in sim-affecting code against the original's
instructions (2026-10-21). Spec: [RE_MEMORY.md, "Numeric model"](../RE_MEMORY.md).

Verdicts: **OK** (bit-identical by construction or by an original-code comparison),
**FIXED** (was different; fixed here, with a test or a harness run), **OPEN** (known
difference left as is, with the reason), **N/A** (not sim-affecting, or exact whatever the
precision).

## 1. Method

1. **Rewrite inventory.** clang-tidy 22 query checks (`--experimental-custom-checks`) over
   `engine/sim`, `engine/net` and the sim-affecting `ta_compat` sources (with their headers),
   compiled with the project's own flags:
   - `float`-typed arithmetic (`binaryOperator(hasType(float))`, compound assignments included): **3 sites**, all exact (see 3.1).
   - comparisons of `float` arithmetic: 0.
   - `float` literals: 179. Double arithmetic: 303. Rounding to `float` (explicit and implicit): 64. Integer→floating: 193.
   - float/double comparisons: 133. Float→integer casts: 10. Math library calls: 41.

   Every site in those lists was read against the original routine it implements.
2. **Original inventory.** Every x87 instruction of the game code (below the C runtime,
   `0x401000`–`0x4E4000`): 3,325 instructions (plus `__ftol`, `_hypot`, `floor`, `acos`,
   `atof` and `pow` calls) in 233 functions. Each function was classified (simulation,
   loading, network, client/render, C runtime) and the simulation ones were compared
   instruction by instruction with the rewrite (store points, operand order, constant
   widths, compare masks).
3. **Constants.** All 171 memory operands of x87 instructions were decoded (address, width,
   value) and matched with the rewrite's literals (3.4).
4. **Compare masks.** All 236 `FCOM*`/`FNSTSW` sites were listed with their `test ah,…` mask and
   branch (`test ah,40h` = equal *or unordered*; `test ah,01h` = below *or unordered*;
   `test ah,41h` = not above, i.e. below, equal or unordered) and matched with the
   rewrite's C++ comparison, NaN included.
5. **Precision control.** Every `FLDCW` and every caller of `_controlfp` / `_ctrlfp`.
6. **Original-code runs** (orig_harness, suspended process): `atof`, `atoi`, and newly
   `_atoflt` (sscanf `%f` into a float) on 4,022,360 strings: identical (3.3).
7. Checks after every fix: sim_tests (5,848 → 5,869 checks), net_tests (514 → 522),
   compat_tests (137 → 145), game_flow golden, replays g8 / s2 / g9 (3,000 / 4,500 / 2,759
   steps) and move_bench checksums: all identical to before.

## 2. The build and the x87 model

- The original sets **PC53, round to nearest** at start (`__setdefaultprecision` `0x4EA5B0`
  → `_controlfp(0x10000, 0x30000)` through `0x4E8CE0`). The other callers of `0x4E8CE0` are
  the `-fpufussy` exception masks (`0x4DF160`) and a sound routine (`0x4CF370`) that
  restores them: no effect on results.
- Game code never spills to `tbyte`: no `fld/fstp tbyte` exists below `0x4E4000`. Every
  spill is `fstp qword` (= a double rounding, the same as C++ `double`) or `fstp dword`
  (= a float store). A value kept in an x87 register at PC53 has a double's 53-bit
  significand; only its exponent range is wider.
- x64 MSVC `/fp:precise` uses SSE (`FLT_EVAL_METHOD` 0). The rewrite widens every
  `float` operand to `double` before arithmetic and rounds with `static_cast<float>` exactly
  where the original has `fst/fstp dword`. The clang query found no `float`-typed
  arithmetic left except three exact products (3.1). No `long double`, no `std::min/max`
  on floats, no `float` math functions (`sqrtf` etc.) in sim code.
- **Exponent range (task item 2).** At PC53 the x87 keeps a 15-bit exponent, so an
  intermediate can exceed `DBL_MAX` or fall below `DBL_MIN` without overflowing or
  denormalising. Whether this can occur, per kind of site:
  - Products, sums and quotients of `float` fields: |float| < 3.4e38, so at most three
    factors (the longest chains here) stay below 1e116 and above 1e-135. Never near the double range.
  - Integer squares and sums under `FSQRT` (16.16 world offsets): below 3·2⁶², fine.
  - `Weapon_SolveBallisticPitch`: `h⁴·g²·s` stays near 2¹⁹³. Checked by `ballistic_check`.
  - `atof` results (up to ±1.8e308, or ±inf for "1e999") multiplied by constants before
    `__ftol` (`Tdf_GetFixed` × 65536, weapon rates × 2184.5, console arguments). An
    overflowing product is finite in the FPU but infinite in C++. Either way it is out of
    `__ftol`'s int64 range, giving `0x8000000000000000` (low dword 0). Same result.
  - Double denormals (< 2.2e-308, where x87 double rounding differs): unreachable, since no
    sim intermediate gets below about 1e-135.

  No site needs the soft-float path.
- **NaN and infinity.** SSE and the x87 agree on IEEE special values. The default NaN is
  negative on both (`0xFFC00000` / `0xFFF8…`). Loading a float or double quiets an sNaN on
  both. MSVC's x64 MXCSR has FTZ and DAZ off. The difference that remained was in
  *comparisons*: see 3.2.

## 3. Findings

### 3.1 `float`-typed arithmetic (clang `fparith`)

| Site | Original | Operation | Verdict |
|---|---|---|---|
| `engine/sim/src/victory.cpp:275` | `0x464F80` (respawn credit) | `(float)(int)start * 100.0f`, start ≤ 65535 | OK: exact (≤ 24 bits) |
| `engine/net/src/lobby.cpp:1154-1155` | room side record → `BattleStart` | `(float)u16 * 100.0f` | OK: exact |

### 3.2 Compare conditions (NaN). FIXED, commits `60bdf43`, `edb7580`

An unordered x87 compare sets C3, C2 and C0 together, so `test ah,40h` takes a NaN as
**equal** and `test ah,01h` takes it as **below**. C++ `==`, `!=` and `<` disagree with that
on NaN. New helpers: `sim::x87::equal(a, b)` = `!(a < b || a > b)` and
`sim::x87::less(a, b)` = `!(a >= b)`. Most sites already used this style (`is_zero`,
`!(x >= y)`). These were the exceptions:

| Site | Original | Test | Before | Verdict |
|---|---|---|---|---|
| `engine/sim/src/combat.cpp:182` (kill: wreck cleared while unbuilt) | `0x4865D8` | 40h jne | `!= 0.0f` | FIXED |
| `engine/sim/src/combat.cpp:226` (kill counted only when built) | `0x4868D9` | 40h je | `== 0.0f` | FIXED |
| `engine/sim/src/commands.cpp:20` (selectable) | `0x48C853` family | 40h je | `!= 0.0f` | FIXED |
| `engine/sim/src/mission_goals.cpp:52,81` (usable unit, BuildUnitType) | `0x48ED6A`, `0x48F292`, `0x48F7A3` | 40h je | `== 0.0f` | FIXED |
| `engine/sim/src/unit.cpp:109` (BUILD_PERCENT_LEFT) | `0x480A4A` | 40h je | `== 0.0f` | FIXED |
| `engine/sim/src/unit_tick.cpp:42` (selection kept) | `0x48AE29` | 40h je | `== 0.0f` | FIXED |
| `engine/sim/src/net_states.cpp:31` (`encode_build_fraction`) | `0x48B24C` | 40h jne | `== 0.0f` (NaN sent 1) | FIXED (test) |
| `engine/sim/src/net_states.cpp:138` (whole-state fraction replaced) | `0x48B4D0` | 40h jne | `!=` | FIXED (test) |
| `engine/sim/src/ai.cpp:384` (site heap sift-down) | `0x40D68D` | 01h je | `<` | FIXED |
| `engine/sim/src/ai.cpp:394` (site heap sift-up) | `0x40D761` | 01h je | `<` | FIXED |
| `engine/sim/src/step_budget.cpp:21` (lag throttle ≥ 0.01) | `0x495347` | 01h je | `<` | FIXED (unreachable: integer input) |
| `engine/sim/src/targeting.cpp:530` (special attack energy) | `0x43E874` | 01h jne | `<` | FIXED |
| `ta_compat/src/battle_loader.cpp:108` (meteor values set) | `0x4367C1`… | 40h jne | `!= 0.0f` | FIXED |
| `ta_compat/src/battle_economy.cpp:45` (tidal default) | `0x483854` | 01h jne | `< 0.0f` | FIXED |
| `client/src/game_client.cpp:232,1769` (selection gates → orders) | `0x48D790` family | 40h je | `== 0.0f` | FIXED |

A NaN build fraction, score or amount cannot come from game data, because `atof` never
returns NaN. It can come from a saved game or network packet with arbitrary bits. Tests:
`test_x87_compare_conditions` (sim_tests) covers the helpers, `encode_build_fraction(NaN)`
and `apply_whole_state` with a NaN fraction.

All other 133 rewrite comparisons were matched with their mask. They are listed with
their functions in 3.5.

### 3.3 Number parsing. FIXED, commit `99394c8`

| Path | Original | Rewrite | Verdict |
|---|---|---|---|
| TDF `float` fields: `Tdf_GetFloat` → `atof` → `fstp dword` | `0x4C4760` → `0x4E4560` (`_fltin` → `__strgtold12` `0x4F3D10` → `_ld12tod` `0x4F3990`) | `crt_atof` → `static_cast<float>` | OK (atof: harness, 4,022,360 strings) |
| TDF fixed: `Tdf_GetFixed` = `__ftol(atof × 65536.0)` | `0x4C4800`, qword `0x4FDC20` | `tdf.cpp:222` | OK |
| TDF ints: `atoi` | `0x4E4ED0` | `msvc_atoi` | OK (harness) |
| Weapon rates: `__ftol(atof × k)`, k = qwords `0x4FD240` (65536/30), `0x4FD248` (65536/900), `0x4FD250` (30), `0x4FD258` (1/30); `minbarrelangle` × qword `0x4FD260` → `fstp dword` | `0x42E440` | `weapondefs.cpp:17-65` (constants as bit patterns) | OK |
| Feature `sparktime` × qword 30 → `__ftol` → int16 | `0x4224B0` | `featuredefs.cpp:142` | OK |
| FBI `Version`: `floor`, `(v − major) × 10.0` (qword `0x4FD208`), `floor`, `__ftol` | `0x42B024` | `unitdefs.cpp:169-172` | OK (`Crt_floor` is exact: RC down, PC64) |
| Console float args: `0x4B7410` (atof, or the float default in ST0) | `0x4B7410` | `crt_atof` | OK |
| **Mission initial orders: `sscanf " %f %f"` into floats** | `0x487BF0` (formats `0x508CD4`, `0x508CA8`, `0x508C80`, `0x508C74`) → `_input` `0x4EF910` → `_fassign` `0x4EA740` → `_atoflt` `0x4F3AD0` → `_ld12tof` `0x4F39B0` | was `static_cast<float>(atof(s))` | **FIXED** |

The `%f` conversion rounds the 12-byte intermediate's 64-bit significand directly to 24
bits. It uses `_ld12cvt`'s float descriptor at `0x510CF8`: {128, −127, 24, 8, 32, 127}.
The double descriptor at `0x510CE0` is {1024, −1023, 53, 11, 64, 1023}. The rounding is
the runtime's own: up when the first dropped bit is set and some bit after the next one
is. Rounding atof's double to a float is a second rounding, and differs on about 0.7% of
inputs. Examples: `"700.8E8"` gives `0x518288BF` in the original and `0x518288C0` through
a double; `"-203784012"` gives `0xCD4257F4` against `0xCD4257F5`.

- New: `crt_ld12_to_float_bits` and `crt_scanf_float`. Both share `_ld12cvt` with the
  double path. A digitless string keeps its sign in the intermediate (`"-"` gives `-0.0f`;
  atof still returns +0).
- `orig_harness` now also runs the original `_atoflt`: **0 mismatches on 4,022,360
  strings**. The old model differed on 28,038 of them, 3 of them game-data values.
- compat_tests checks four of those values.

### 3.4 Constants

Every x87 memory constant used by simulation code, with the rewrite's literal. Widths
matter only where the value is not exactly representable in a float.

| Original constant | Used by | Rewrite | Verdict |
|---|---|---|---|
| qword −0.7 `0xBFE6666666666666`, qword −0.5 (`0x4FC480/88`, `0x4FC6D8/E0`, `0x4FCC90/98`, `0x4FD040/48`, `0x4FD540/48`, `0x4FD700/08`) | AI easy/medium credit: settlement, factory refund, build progress, reclaim yield, transfers, killed-packet refund, respawn | `-0.7` / `-0.5` doubles (economy.cpp:130-134, construction.cpp:138-139, combat.cpp:265-267, victory.cpp:276-277) | OK |
| dword 0.0005 `0x3A03126F`, dword −1/140 `0xBBEA0EA1`, dword 30, dword −150 | Capture time `0x404359` | `float_bits(...)`, `30.0`, `-150.0` (construction.cpp:695-697) | OK |
| dword −0.5, dword 15 / 30 | Reclaim feature `0x404BBA`, VTOL reclaim | `-0.5`, `15.0`, `30.0` | OK (exact) |
| qword 0.3 | Resurrect `0x404FC8` | `0.3` | OK |
| qword 8, −8 | Mobile build, repair, capture reach | `8.0`, `-8.0` | OK |
| qword 16 | Help build ring | `16.0` | OK |
| qword 0.2 | Repair patrol (ground/air) | `0.2` | OK |
| qword 1.5 / 1.25 | Ground unload / weapon slot set-up | `1.5` / `1.25` | OK |
| qword 65536 | AI hunter, unit table anchor, steer air, orders, initial orders, TNT gravity, Tdf_GetFixed, +search | `65536.0` | OK |
| dword −0.01, −0.002, −0.0025, −0.02, 30, 100, 5 | `Ai_Rescore` `0x409730` | `static_cast<double>(-0.01f)` etc. (ai.cpp:752-796) | OK |
| dword 50, 25, 0.125, 0.25, 1, 200, 3, 5 | `Ai_BuildScore` `0x40BB00` | literals (exact) | OK |
| dword −1/65536 `0xB7800000` | Unit table anchor `0x40AC43` | `-1.52587890625e-05` (exact) | OK |
| dword 1/65535⁺ `0x37800080`, dword 512, dword 65535 | `Fx_ShatterPiece` `0x421700` | `to_float_units` bits, `512.0`, `65535.0` | OK |
| qword 1/65536 `0x3EF0000000000000` | Metal extraction `0x4378E0` | `1.0 / 65536.0` | OK |
| dword 10, dword 300 | Reclaim pulse `0x438650` | `10.0f`, `300.0` | OK |
| dword 1/65536, qword 65536, dword 8 | `Motion_SteerAir` `0x43D290` | `kInv65536`, `65536.0`, `8.0` | OK |
| dword 0.5, dword 48 | Point / manoeuvre goal reached | `0.5`, `48.0` | OK |
| dword 1/3 `0x3EAAAAAB`, dword 0.5 | `Share_AutoShareResources` | `0.33333334f`, `0.5f` | OK |
| dword −254, dword 1/255 `0x3B808081` | Net build fraction | `-254.0`, `0.003921569f` | OK |
| dword −99 | GetUnitValue BUILD_PERCENT_LEFT | `-99.0` | OK |
| qword 1.0 | `Wind_Step` clamp | `1.0f` (exact) | OK |
| qword 0.1, `0x3F3845C8A0CE5129`, 0.01 | `SpeedCtrl_BudgetSimSteps` | `0.1`, `0.00037037037037037035` (same bits), `0.01` | OK |
| dword 1.0 | Area damage falloff | `1.0` | OK |
| qwords −2, ½π⁺ `0x3FF921FB54442D11`, ¼π⁺, 32768, 1/π⁺ | Ballistic pitch | bit patterns (weapons.cpp:19-21) | OK (`ballistic_check`) |
| qword 1/900 `0x3F523456789ABCDF` | TNT gravity `0x483610` | `65536.0 * (1.0 / 900.0)`; the compiler's 1/900 has the same bits | OK |
| dword 30 | Sky `0x437D60`, AirStrike, mission order times | `30.0` | OK (exact) |
| dword 256 `0x4FCC78` | `+contour` `0x416DB0` | missing | FIXED (3.6) |
| qword 65536 `0x4FCC70` | `+search` `0x416780` | missing | FIXED (3.6) |

### 3.5 Site table (simulation)

Every rewrite function with floating-point operations. "Store" means where the original
writes a dword (a float rounding); everything else stays in a register or a qword at
PC53, as a C++ `double`.

| Rewrite site | Original | Operation | Verdict |
|---|---|---|---|
| economy.cpp:24 `book_energy_use` | `0x4013D2`/`0x401628` | use ≥ 0 (01h): `use + slot` → dword; paid unless stall > 0 (41h); production `-use` credited | OK |
| economy.cpp:37 `pay_from_pool` | `0x401A4D` loop | stall > pool (41h) → frac = pool/stall (dword); rest kept double; pool = dword(rest) then dword(rest − rest / − paid) | OK |
| economy.cpp:60 `close_slot` | `0x401B3D`, `0x401BAE` | `paid − frac·paid + stall − frac·stall` → dword | OK |
| economy.cpp:88-123 add-use / try-consume | `0x4011C0`, `0x401180`, `0x401220`, `0x401260` | float adds stored; stalls 41h; stored ≥ amount (01h) | OK |
| economy.cpp:127 `credit_make` | settlement credits | `make − amount·k` (qword k) or `amount + make` → dword | OK |
| economy.cpp:170 `settle_player` | `0x401360` | per unit: extract/makes/wind/tidal order, the 41h tests, storage sums stored; cloak `__ftol(rate)` vs stored (41h); 8 running sums each stored; own slot energy make **kept double**; totals qword; pools dword; waste qword | OK (all 302 FP instructions traced) |
| economy.cpp:277,292 transfers | `0x464B30`, `0x464C60` | clamp (41h), zero/NaN skip (40h), credit | OK |
| economy.cpp:318 `share_amount` | `0x457E26`/`0x457F44` | `(have − above)·k`, `FST dword` copy, compare double (01h), dword(space) else | OK |
| economy.h:199-224 `auto_share` | `0x457D30` | receiver pick `!(other ≥ own)` (01h) | OK |
| economy.cpp:326 `step_wind` | `0x490D40` | `FILD speed / FIDIV denom` → dword, then compare the dword with qword 1 (41h) | OK |
| economy.cpp:343 `stamp_feature_metal` | `0x422040` | zero/NaN skip (40h), `__ftol` → byte | OK |
| construction.cpp:134 `with_income` | AI credits | as `credit_make` | OK |
| construction.cpp:216 `beyond_reach` | `0x403A20`, `0x404270`, `0x405300` | `__ftol(_hypot)`, `_hypot(fx,fz)·±8.0` | OK |
| construction.cpp:250 factory cancel refund | `0x402666` | `__ftol((1.0f − left)·cost)`, uint32 `FILD qword` | OK |
| construction.cpp:441 help-build ring | `0x404011` | `√((x·x + z) + z)·16` | OK |
| construction.cpp:582, 955 reclaim time | `0x404BBA`, `0x414770` | `k − (e + m)·−0.5` (dwords) | OK |
| construction.cpp:695 capture time | `0x404359` | dword constants, products in register | OK |
| construction.cpp:1136 resurrect time | `0x404FB0` | `(bt·0.3)/rate` | OK |
| construction.cpp:1248 `apply_build_progress` | `0x41BA60` | `left − work/bt`; clamp 41h/01h; next, slice, energy and metal stored as dwords; `__ftol(left·max) − __ftol(next·max)` | OK |
| construction.cpp:1290 `unfinished_decay` | `0x41BCD0` | `−(product/energy)` → dword argument | OK |
| construction.cpp:1327 `reclaim_pulse` | `0x438650` | cost > 10f (41h), `FILD qword` uint32 product / (m·300f) | OK |
| construction.cpp:1360 ownership packet | `0x488614` | `__ftol(fraction)` | OK |
| construction.cpp:1519 `reclaim_feature` | `0x4237D0` | yield credits | OK |
| combat.cpp:146 `repair_heal` | `0x41BD10` | `(max·rate − 1f)/bt − −1f`, `__ftol` | OK |
| combat.cpp:182,226 kill paths | `0x4865D8`, `0x4868D9` | fraction zero tests | FIXED (3.2) |
| combat.cpp:260 killed-packet reclaim refund | `0x486CB7` | `(1f − left)·metal`, AI credit | OK |
| missions.cpp:106 work rate | `0x402A00` and others | `(float)(wt/30)` | OK |
| missions.cpp:320 stockpile payment | `0x402BF3` | `__ftol(n·m/r) − __ftol(p·m/r)` (×2), int→float arguments | OK |
| ground_missions.cpp:330, 360-362 | `0x406A42`; `0x4059EA`, `0x405C83` | `size·1.5`; `cap·0.2` vs stored (41h); `stored + more` vs cap (41h) | OK |
| air_missions.cpp:690 | VTOL missions | `__ftol(_hypot)` | OK |
| air_missions.cpp:731 bomb lead | `0x4123D6` | `√((alt + alt)/g)·30f·speed` | OK |
| air_missions.cpp:1075-1079 | `0x415495`… | as ground | OK |
| unit_missions_common.cpp:181, 335 | `0x47EC1C` | `__ftol(_hypot)`; best metal `!(best ≥ v)` (01h) | OK |
| unit_create.cpp:44 | `0x49E070` | `dy·1.25` | OK |
| unit_create.cpp:53-63 metal extraction | `0x437840` | extracts > 0 (41h); `(e·count)·2⁻¹⁶` → dword | OK |
| unit.cpp:98-111 GetUnitValue | `0x480770` | `__ftol(_hypot)`; build percent `1 − __ftol(f·−99f)` | OK, zero test FIXED |
| unit_tick.cpp:11 wind notify | `0x437921` | gen > 0 (41h) | OK |
| motion.cpp:160 `PointGoal::reached` | `0x44E5B0` | `_hypot·2⁻¹⁶`; radius `!(d ≥ r)` (01h); `d > 0.5` (41h) | OK |
| motion.cpp:279 `ManoeuvreGoal::reached` | `0x44EB60` | `!(d ≥ 48)` | OK |
| motion.cpp:705 `steer_air` | `0x43D290` | accel, horizontal and brake as dwords; keep/forward `__ftol`; `−√(2a/d)`; ax/az: one stored (FST dword), the other kept double when scaled; `h > accel` (41h); speed √((x²+y²)+z²) | OK (traced) |
| motion.cpp:32 and others: `hypot_trunc` | `0x43CD20`, `0x44F2A0`, `0x490690` | `__ftol(_hypot(int, int))` | OK |
| weapons.cpp:25 `ballistic_pitch` | `0x49A890` | verified by `ballistic_check` (incl. NaN orders, extended `acos` result) | OK |
| weapons.cpp:217 veteran lead | `0x48A1E0` | `√((x²+y²)+z²)` | OK |
| weapons.cpp:256 `direct_aim` | `0x49D910` | `__ftol(_hypot)`, `atan2_scaled` | OK |
| weapons.cpp:280 `use_resources` | `0x4012A0` | stored ≥ cost (01h) both, then metal again | OK |
| weapons.cpp:360 can fire | `0x49E3F9` | stored ≥ per-shot (01h) | OK |
| projectile.cpp:40 `length_ftol` | `0x49A850` | `(x²+y²)+z²` | OK |
| projectile.cpp:164 damage scale | `0x499CD0` | `__ftol(base·scale)` | OK |
| projectile.cpp:219 area falloff | `0x49A3B6` | `t·t·(1f − e) + e`, t = d/r − 1f → dword | OK |
| projectile.cpp:320, 496, 531; fire.cpp:31 | `0x49B520`, `0x49C9C0`, `0x49CDE0`, `0x49D9C0`, `0x49DB70` | `__ftol(_hypot)` | OK |
| effects.cpp:79-98 vectors | `0x4B6EB0`, `0x4B6F70`, `0x4B6FF0`, `0x421700` | differences, cross products, normalise, each component stored as a dword | OK |
| effects.cpp:165-171 shard normal use | `0x421700` | `__ftol(n·512f)`, `__ftol(n·65535f)` | OK |
| effect_lists.cpp:24; nano_spray.cpp:48 | `0x4736E0`, `0x473D50`, `0x474760` | `√((x²+y²)+z²)` | OK |
| ai.cpp:30 `length3` | `0x408100`, `0x40BDB0`, `0x40BFE0` | `√((x²+y²)+z²)` | OK |
| ai.cpp:379 `heap_adjust` | `0x40D670`, `0x40D740` | score order | FIXED (3.2) |
| ai.cpp:417 metal site score | `0x40A3E3` | `FILD` → dword | OK |
| ai.cpp:497-723 unit table anchor | `0x40AA40` | `sum − (v·w)·k` → dword, `w + sum_w` → dword; ÷ unless 0/NaN (40h); ×65536 `__ftol` | OK |
| ai.cpp:582 hunter centre | `0x407D40` | int × 65536 | OK |
| ai.cpp:623 metal sites | `0x40A84D` | metal ≠ 0 (40h) | OK |
| ai.cpp:653 profile weight | `0x409DC0`, factor `0x406DFD` dword | `__ftol(w·f)` | OK |
| ai.cpp:727 `ai_rescore` | `0x409730`, `0x488F30` | threat `__ftol` chains; make clamp; energy score dword `ex` then min/max with NaN rules; metal score dword `mq` | OK (traced) |
| ai.cpp:876 `ai_switch_by_economy` | `0x408670` | `m + m` vs energy (01h, NaN goes on); net > 0 (41h) | OK |
| ai.cpp:906 `ai_build_score` | `0x40BB00` | stored ≥ 50/25 (01h); room `(cap − stored)·k`, max 0 (41h) | OK |
| targeting.cpp:530 special attack | `0x43E874` | energy, metal | FIXED (3.2) |
| step_budget.cpp:17-33 | `0x495230` | rate, throttle (clamp FIXED), `elapsed·rate + frac` stored qword, `floor`, `__ftol`, frac → dword | OK |
| sky.cpp:17-19 | `0x437D60` | `30f/density`, `secs·30f` | OK |
| net_states.cpp:31-35, 138 | `0x48B200`, `0x48B3F0` | fraction encode/decode | FIXED (3.2) |
| net_units.cpp:458 | `0x48873A` | `FILD` → dword fraction | OK |
| battle_start.cpp:56 `init_resources` | `0x465E30` | network `u16·100` and skirmish ints `FILD` → dword; **missions: `__ftol(float)` for the storage bonus** | FIXED (3.6) |
| victory.cpp:275 respawn | `0x464F80` | credit | OK |
| lobby.cpp:1154 | room record | exact | OK |
| net_battle.cpp `send_view_state` / receive | `0x4573D0`, `0x457540` | totals → floats, record update | FIXED (3.6) |
| battle_economy.cpp:26, 45 | `0x483854` | tidal float, default rule | FIXED (3.2) |
| battle_loader.cpp:103-109, 217 | `0x4367BD`, `0x483610` | meteor floats (40h); gravity `g·65536·(1/900)` | OK, meteor test FIXED |
| unitdefs.cpp:161-259 | `0x42BF40` | int → dword costs, float fields `fstp dword`; cloak default `__ftol` | OK |
| weapondefs.cpp, featuredefs.cpp, tdf.cpp | `0x42E440`, `0x4224B0`, `0x4C4800` | see 3.3 | OK |
| mission_text.cpp:72 `Scanner::real` | `0x487BF0` | sscanf `%f` | FIXED (3.3) |
| mission_text.cpp:104 `ftol` | `0x487BF0` | `__ftol(x·65536)`, `__ftol(t·30f)` | OK (same as `ftol32`) |
| console_commands.cpp `+atm`, `+nometal/+noenergy`, `+give`, set share | `0x4170C0`, `0x417150`, `0x416BD0`, `0x419340` | `stored − −1000f`; int → dword; set share compares the int (exact) with the float storage, same result as comparing the rounded float | OK |
| console_commands.cpp `+search`, `+contour` | `0x416780`, `0x416DB0` | scaling | FIXED (3.6) |
| ai_profile.cpp:133 | `0x406DB0` | `fstp dword` of atof | OK |
| save_sections.cpp (players) | `0x4662F0`, `0x466050` | floats ↔ doubles in the bank (exact widening, rounding on load) | OK |

### 3.6 Other fixes found on the way

- **Mission storage bonus** (`battle_start.cpp:69`, `0x465F2E`). The original converts the
  mission's float amounts with `__ftol` (low dword of the int64). The rewrite used a plain
  `static_cast<int32_t>`, which is undefined out of range: x64 saturates to `0x80000000`.
  Now `x87::ftol32`. Example: 5e9 → 705032704 (was −2³¹, so the 200 minimum). Test in
  `test_x87_compare_conditions`. Commit `60bdf43`.
- **`+search`** (`0x416780`). The route search's base estimate scale (+0x54) is
  `__ftol(atof(word 2) × 65536.0)`, a 16.16 value like its default 0x18000. The rewrite
  stored the bare integer part. Sim-affecting: route search budget. Commit `2b59f95`.
- **`+contour`** (`0x416DB0`). Both arguments are multiplied by 256.0f before `__ftol`,
  matching the ×256 heights of `0x418310`. Display only. Commit `2b59f95`. No unit test:
  `ConsoleCommands` needs a full `GameClient`. Checked against the disassembly.
- **Packet 0x28** (`0x4573D0` / `0x457540`), commit `310aeba`, net_tests
  `test_view_state_packet`:
  - The sender writes the stored amounts (+0x98, +0x8C) *before* the storage (+0xA8,
    +0xA4). Then come the six battle totals, each double stored as a float (FSTP dword).
    The rewrite had the two pairs swapped and sent zeros for the totals.
  - The receiver writes all of it into the **sender's player record** (counters, stored,
    storage, totals as FLD dword / FSTP qword). The rewrite kept them only in a side table,
    so a remote record's stored amounts never followed its owner. Auto-share picks the
    poorest ally by those amounts (`0x457DF4`).

### 3.7 Precision control and the C runtime

| `FLDCW` / `_controlfp` site | What | Reached by sim | Verdict |
|---|---|---|---|
| `0x4EA5B0` `__setdefaultprecision` | PC53 at start-up | yes | OK (the model) |
| `0x4E43A0` `__ftol` | RC = truncate around `FISTP qword` | yes | OK (`x87::ftol64/ftol32`) |
| `0x4FB440` `_hypot` (`0x4FB480` via `_ctrlfp` `0x4EC4C0`) | PC64, sqrt helper at 0x37F | yes (33 callers) | OK (`x87::crt_hypot`, 1,000,000 calls) |
| `0x4E5050` `floor` (`_ctrlfp` → CW 0x173F) | RC down, PC64, `FRNDINT` | FBI version, step budget | OK (exact for any input) |
| `0x4E67F0` `acos` (no CW change; FPATAN at PC53) | | ballistic pitch | OK (`x87::crt_acos`, ballistic_check) |
| `0x4E5370` `_CIpow` (`FYL2X`/`F2XM1`, CW check 0x27F) | | `0x443480` only: `2^i`, i = 0..15 (a dialog's option bits) | N/A (exact powers of two) |
| `0x4F4C30`… FDIV-bug workaround (`_adj_fdiv*`) and `_adj_fprem` | | not called by game code | N/A |
| `0x4EC782`, `0x4EC97E`, `0x4ECA36`, … (`_87except` and math error paths) | | error paths of the above | N/A |
| `0x4CF370` (sound), `0x4DF160` (`-fpufussy`) | exception masks only | | N/A |

Transcendentals reached by simulation code: `FSQRT` (26 sites: `std::sqrt` is IEEE at
53 bits; operands are always doubles or exact integers), `FPATAN` `0x4B7163`
(`x87::atan2_scaled`), `FSINCOS` `0x4B7173` (`x87::rotate_point_2d`), CRT `_hypot`,
`acos`, `floor`. No `fsin`, `fcos`, `fptan`, `fprem`, `fyl2x`, `f2xm1`, `fscale` or
`frndint` in game code. The `fisttp` lines in the listing are data decoded as code.

### 3.8 Integer ↔ float conversions

- `FILD dword` (402 sites) and `FILD word` (1): exact, as C++ `double(int)`.
- `FILD qword` (8 sites): all zero-extended uint32. Checked: `0x402697`, `0x41BB46`,
  `0x4386CC`, `0x48B4C6`; the rest are client code. Exact, as `double(uint32_t)`.
- `FIMUL` / `FIDIV` / `FIADD` dword: exact operand, one rounding. Same as C++.
- `__ftol` (270 calls): truncation, with the low dword of `0x8000000000000000` out of range.
  The rewrite uses `x87::ftol32/64` everywhere; the clang query found no raw float→int
  cast left in sim code.
- `FISTP` with nearest rounding: only `0x4B716B` (`atan2_scaled`) and `0x4B71A0/A2`
  (`rotate_point_2d`), both emulated.
- Float to int16 or byte: `__ftol`, then the low bits (`static_cast<i16>(ftol32(...))`). OK.

## 4. OPEN

None in sim-affecting code.

Not audited for bit-exactness (out of scope, display only): render and HUD arithmetic
(`0x420D20`, `0x458xxx`–`0x459xxx`, `0x468CF0`, `0x46A860`, palettes `0x4BABD0`/`0x4BADF0`),
GUI layout (`0x45xxxx`, `0x4A2xxx`), the music volume (`0x4CDB40`).
`client_loading.cpp:87` and `client_net_screens.cpp` use plain casts where the original uses
`__ftol`. They agree for every in-range value.

## 5. Summary

- Original FP code classified: 233 functions / 3,325 FP instructions. Every simulation
  and loading function among them was compared with the rewrite (3.3, 3.5, 3.7). The
  rest are client, render, GUI or C runtime.
- Rewrite sites inventoried: 926 (clang query, 9 checks). `float`-typed arithmetic: 3,
  all exact.
- **FIXED: 24 sites in 6 groups**:
  - 18 NaN compare conditions, in 12 files.
  - sscanf `%f` float rounding (mission orders).
  - Mission storage bonus `__ftol`.
  - `+search` ×65536.
  - `+contour` ×256.
  - Packet 0x28, send and receive: layout, float totals, record update.
- OPEN: 0.
- Golden and replays (game_flow, g8, s2, g9) and move_bench checksums are unchanged. None
  of the fixed paths occurs in those captures: no NaN, no missions, no console, no network
  view packets.
