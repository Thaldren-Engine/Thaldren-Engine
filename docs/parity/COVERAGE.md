# Coverage sweep of the original program (TotalA.exe 3.1)

*2026-10-21.* A sweep over the whole original program, not over the parity documents: every
function of the executable, every string, every key its readers ask for, every imported system
call and every file type it opens. The question for each one: is it in the rewrite, is it not
applicable (library, platform glue, debugging, dead code), or is it missing? The parity
documents (SIM, CLIENT, FRONTEND, NETWORK) keep their own open items; this file adds what they
did not list and ranks what is still missing by what a player or a peer could see.

## How it was made

- **Functions.** Ghidra's function list of `/TotalA.exe` (3,009) plus every `call` target of the
  disassembly that Ghidra did not list as a function start (115): **3,124 functions**. The call
  graph (10,697 direct calls), the strings each function references, the imports it calls and
  the function pointers in the data sections (66 tables, 399 slots: vtables, the console tables,
  the mission table) come from the disassembly (`dumpbin /disasm`, 355,521 instructions).
- **Ported?** A function counts as ported when an address inside it is cited by the rewrite's
  code, tests or harnesses (1,125; a harness line that only *stubs* the function does not count),
  when its Ghidra name or its documented address is in the docs with an implementation (381), and
  "ported (inferred)" when every caller and every table that refers to it is ported (a helper of
  ported code: 773). Library ranges by their code and strings: the MSVC C runtime and its
  `std::string` (0x4E3A90..0x4FB940), the Cavedog runtime library (heap manager, crash log,
  performance dialogs, registry and window helpers: 0x4D80D0..0x4E3A90), zlib 1.0.4 inflate
  (0x4D1C80..0x4D80D0, reimplemented in `ta_compat/src/inflate.cpp`), the HPI LZ77 code
  (0x4D06C0..0x4D1C80, `ta_compat/src/hpi.cpp`).
- **Every function not covered that way (about 430) was read in Ghidra** and decided by hand:
  ported under another name (the console command handlers, the motion and victory goal objects,
  the save sections), N/A with the reason, debug-only, dead (no call and no pointer to it), or not
  ported. The subsystem of a function is that of the rewrite file that cites it, else of its
  nearest classified neighbour in address order (the original's link order keeps a source file
  together).
- **Other axes**: all 2,861 strings of the executable against the rewrite's sources and docs
  (461 do not occur there; each was looked at, below); the 452 keys passed to the TDF readers
  (`Tdf_GetInt` / `GetFloat` / `GetString` / `GetStringLocalized` / `EnterSection`), the registry
  wrappers and the profile reader; the 254 imports by call site; the file names and patterns in
  the strings; the three console tables (`0x501D3C`, `0x501F4C`, `0x501FD4`).
- Limits: function ends are not known (a function's range runs to the next start), so a cited
  address inside a large routine marks all of it; "ported" says the routine is built, not that
  every branch is. Pointers computed at run time are not followed (41 functions have no caller and
  no pointer to them: listed as unreferenced).

## Per-subsystem table

| Subsystem | Functions | ported | ported (inferred) | N/A | not ported |
|---|---|---|---|---|---|
| Client (battle UI, input, sound) | 380 | 241 | 105 | 34 | 0 |
| Console commands | 128 | 109 | 18 | 1 | 0 |
| Data files and formats | 19 | 11 | 1 | 6 | 1 |
| Front end (menus, campaign, movies) | 462 | 188 | 239 | 33 | 2 |
| Network | 326 | 162 | 107 | 53 | 4 |
| Rendering | 114 | 68 | 41 | 3 | 2 |
| Saves | 38 | 34 | 4 | 0 | 0 |
| Simulation | 965 | 691 | 258 | 13 | 3 |
| Library: Cavedog runtime (heap, crash log, perf dialogs, registry/window helpers) | 206 | 0 | 0 | 206 | 0 |
| Library: HPI LZ77 (ported as ta_compat hpi) | 11 | 11 | 0 | 0 | 0 |
| Library: MSVC C runtime | 417 | 2 | 0 | 415 | 0 |
| Library: MSVC std::string | 15 | 0 | 0 | 15 | 0 |
| Library: zlib 1.0.4 inflate (ported as ta_compat inflate) | 43 | 43 | 0 | 0 | 0 |
| **Total** | 3124 | 1560 | 773 | 779 | 12 |

(The "Data files" row is small because most format readers are cited from RE_MEMORY's format
sections and land with the subsystem that uses them. The 12 not-ported functions are the three
items ranked below; two of them, `0x444580` and `0x4B5980`, are ported except for that part.)

## Ported in this sweep

| Item | Original | Rewrite | Evidence |
|---|---|---|---|
| Unit notices show the order behaviour's own text. `Unit_QueueSpeech` uses the table text only when the caller gives none; the behaviours pass 43 texts ("Unable to create any more units", "I can't reach the construction site", "Capture failed", "Landing aborted: all pads are occupied", "Ressurection failed" (sic) …) and the acknowledgements their mission word ("Guarding", "Repairing", "Loading unit" …). At the default unitchattext 5 a failed build showed "ARMCOM: Cannot Comply" where the original shows "ARMCOM: Unable to create any more units" | `0x47F780`, `0x438880`, strings `0x5010CC`..`0x501CFC` | `ta_compat/src/notice_texts.cpp` (`ta::speech_text`), `GameClient::on_speech(unit, id, text)`, `NoticeQueue::queue(…, text)`, `ta_compat/src/client_battle_setup.cpp` | compat_tests `test_notice_texts`; each string's push site checked in the disassembly (the two resurrection texts: `0x404DEF` / `0x404F6F`) |
| The unit limit is the session value GS+0x37EEC: at start-up `[Preferences] UnitLimit` of `totala.ini` beside the program (default 250, more than 500 → 500, less than 20 → 20), replaced by a loaded save's `maxunits`; it is not a registry setting (the client read a `UnitLimit` setting). `NoDirectSound` from the same file starts without sound as `-s` does | `0x491200`, `0x49F5A0`, `0x432610` (from `0x497180`), readers `0x4971F1`, `0x449C3B`; `0x47ED40` | `ta_compat/src/preferences.cpp` (`ta::profile_int`, `preferences_unit_limit`, `preferences_int`), `FrontEnd::unit_limit()`, ta_play | compat_tests `test_preferences`: 42 cases of GetPrivateProfileIntA's parsing measured on Windows 11 (sections, keys, quotes, signs, hex, wrap-around) and the clamp |

## Not ported, ranked by what a player or peer sees

1. **The online-service buttons on the multiplayer connection screen** (Network front end;
   medium). The Steam installation ships `online.dll` (version 3) and six service extension
   DLLs (`tadwngox`, `taheatx`, `takalix`, `tamplayx`, `tatenx`, `tawirepx`). When SELPROV.GUI
   opens (`0x444580`) the original asks `online.dll` for its links (`0x45B490`:
   `ONLGetVersion` > 2, then `ONLGetLinkInfo`, entries of 0x24 bytes: id and a 32-byte name,
   each name translated) and adds one button per entry (`0x44475D`): a copy of the hidden
   template gadget `SERVICEX` (81, 221, 321 × 20), named `SERVICE<n>`, made active, each next
   one at y + height + 1. online.dll always gives two links, id 0 "www.cavedog.com" and id 1
   "Unit Download", then one entry per `ta*.dll` in the program's folder that loads as an
   extension (`TotalAExtVersion`, button text from `TotalAExtGetButtonText`; at most 10; the
   Mplayer one moved first). The extension texts depend on whether the service is installed
   ("Play on Kali" / "Get Kali", "Play on HEAT" / "Get HEAT", "Play on Mplayer" / "Get Mplayer",
   "Play on Wireplay" / "Get Wireplay (UK)", "Play Free on TEN", DWANGO). Pressing one runs
   `ONLProcessButtonCommand(id)` (`0x45B250`, through the screen's handler `0x4441A0`): the links
   open their web pages (ShellExecute), an extension its service program or download page. So
   on a Steam installation the original's connection screen shows up to eight more buttons
   than the rewrite's. gui_harness's `selprov` scene stubs `0x45B490` (no links), which is why
   the screen compared identical. *Implementation note:* the rewrite cannot load these 32-bit
   DLLs; the faithful route is to reproduce online.dll's answer (its two fixed links, the
   extension DLLs found by `ta*.dll`, the Mplayer reordering, the limit) and each known
   extension's button-text rule (its registry probe) as data, read the button texts from that,
   clone `SERVICEX` as `0x44475D` does, and open the link with the platform's URL opener on a
   press. Check the drawn screen with gui_harness's `selprov` once the stub returns the real
   links (the harness can call the DLL's exports in the suspended original).
2. **Mouse trails switched off while the program runs** (platform; small). At start-up
   (`0x4B5980`) the original saves the user's mouse-trail setting
   (`SystemParametersInfoA(SPI_GETMOUSETRAILS)`) and sets it to 0 with `SPIF_UPDATEINIFILE`;
   it restores the saved value at exit (`0x4B6110`). Seen only by users who have mouse trails
   on (Windows' default is off), and the rewrite hides the system pointer over its window. A
   Windows-only call pair at the window's creation and destruction would match it (the original
   writes the user profile, so a crash leaves the trails off; the same would hold).
3. **Palette tables built when the files are missing** (data; small to medium, invisible with
   the shipped data). The original loads `palettes\PALETTE.ALP` / `.SHD` / `.LHT` and, when a
   file is absent, computes the table (alpha `0x4BA750` — the rule is in RE_MEMORY — shade
   `0x4BADF0`, light `0x4BABD0`) and writes it out (`0x4BC290`); `0x429330` likewise builds
   `palettes\<name>.PAL` from `<name>.PCX`'s palette and caches it (`0x42E140`, `0x42E1D0`,
   `0x42E260`). The 3.1 archives ship all four files, so only a mod that leaves them out sees a
   difference (the rewrite then has no translucency / shading tables). *Implementation note:*
   port the three builders (float formulas over the palette: keep the x87 rounding of the
   `__ftol` calls) and use them when the read fails; the cache write is a side effect on the
   game folder that has no player-visible consequence.

Related but already tracked by the parity documents (not repeated here): the movie states and
CD prompts (FRONTEND), the lens shot and the music cross-fade (CLIENT), the network items
against two or more originals (NETWORK).

## N/A (with the reason)

| What | Where | Why |
|---|---|---|
| DirectPlay wrapper "HAPINET" (33 functions; DPERR texts) | `0x4C9530`..`0x4CAA10` | rule: no DirectPlay; engine/net speaks its wire protocol itself (NETWORK §3) |
| CD audio through MCI (`open/status/play cdaudio`, CD player window class) | `0x4CDA00`..`0x4CED40` (the parts not already cited by the music player) | the client plays the music tracks from files (CLIENT §3) |
| Scores reporting (`reporter.dll`: `_RIInitializeEx` …, REPORT.GUI, "Unable to initialize scores reporting.") | `0x46BF30`, `0x46BCE0`, `0x46C2A0`, `0x46C620`, `0x4CAA20`, `0x4435A0` | reporter.dll is not shipped (Steam or CD): LoadLibrary fails and nothing is shown |
| `-c` lobby file through online.dll (`ONLLoadConfigFile`) | `0x45B670` from `0x49EE30` | FRONTEND 9.1: lobby launches are N/A |
| Code-segment checksum on every front-end state change | `0x4256D0`, `0x4257E0`, `0x425860`, `0x4259B0`, `0x425A90`, `0x428BC0` | the check is a stub returning 0 in 3.1 (FRONTEND 1.5) |
| CD probes and prompts (TOTALA.ID, volume serial, "Please insert …", "Close Windows CD Player?") | `0x41D6A0`, `0x4BB260`, `0x41F680`, `0x426200` | the Steam flag answers at once (FRONTEND 4.6, 3.5) |
| Smacker DirectDraw glue (pixel format, palette, paint) | `0x47C150`, `0x47C230`, `0x47C2A0` | the movie player is reimplemented (FRONTEND §2) |
| Windows wave-out sound path (`-w`, UseWindowsSound) | `0x47EFE0`, `0x49F610` | one audio output (FRONTEND 9.2) |
| Out-of-memory handler and ErrorLog.txt | `0x49E680`, `0x49E6F0` | system error reporting (FRONTEND 10.5) |
| Cavedog runtime library (heap with `-memfussy` … switches, crash log, performance and memory dialogs on hot keys, `-debughelper`) | `0x4D80D0`..`0x4E3A90` | development runtime, nothing in play |
| Debug-only: `+NetStats` numbers line (`pS=… pR=…`, drawn only in the debug overlay), Unit State Probe overlay, `+Include` debugdat files, HapiBank audit dump, DebugHelper.dll | `0x415FA0`, `0x467D70`, `0x4177E0`, `0x4B4D70`, `0x4CBAB0` | reachable only with developer mode (RE_MEMORY "Developer mode") |
| Dead: the file requester (FILEREQ.GUI; its opener `0x4AFA30` has no reference), an old AI-profile tokenizer ("parse error reading AI profile"), the ArmCamp / CoreCamp registry writers | `0x4AF5B0`, `0x4AF670`, `0x4BC2E0`..`0x4BC360`, `0x428CD0`, `0x428D10`, `0x432A80` | never called |
| Unreferenced (no call, no data pointer found; dead or reached through computed pointers whose targets are covered elsewhere) | `0x41D8C0`, `0x4335F0`, `0x434F70`, `0x437280`, `0x4467F0`, `0x448BF0`, `0x461860`, `0x462BD0`, `0x462D30`, `0x462ED0`, `0x463610`..`0x463730`, `0x46E3D0`, `0x47FCA0`, `0x47FFA0`, `0x4896F0`, `0x48DC30`, `0x49D1E0`, `0x4A4890`, `0x4B9A50`, `0x4BCE60`, `0x4BE270`, `0x4C38A0`..`0x4C3C60`, `0x4C42A0`, `0x4CCB65`, `0x4CCD1C`, `0x4CCD85`, `0x4CD010`, `0x4CF800`, exception funclets | 41 functions; none references a player-visible string except `0x448BF0` ("SIDE%d" button stages) and `0x48DC30` (the "CTRL_F" category: the Ctrl+F selection is built in client/src/game_client.cpp) |

## The other axes

- **Strings.** 461 of the 2,861 strings do not occur in the rewrite's sources or docs. By kind:
  memory-block tags of the Cavedog allocator ("UNIT MEMORY", "SORT INDICES", "PCX BODY" … about
  120), debug and trace texts (the packet pool's trace, netCondenser, Smacker statistics,
  "Unit Builder Probe", "FRATE" … about 110), DirectPlay error texts and HAPINET traces (about
  70), the unit-notice texts (ported above), MCI command strings (14), the online.dll / reporter
  names (item 1, N/A), CD prompts and checksum messages (N/A), assertion file names
  (`c:\cavedog\wargame\*.cpp`). No other player-visible text was found missing.
- **Keys the readers ask for** (452). Not in the rewrite's text: the GUI slider fields `thick`,
  `knobpos`, `knobsize`, `itemheight`, `hotornot`, `nuttin`, `texturenumber`, `totalgadgets`
  and VERSION `major` / `minor` / `revision` (read by the GUI loader `0x4AD350` / `0x4AD890` /
  `0x4AE170` into the gadget records; the rewrite's GUI reader skips them and sizes its knobs and
  lists from the gadget's own size and contents; no difference is known, but this sweep did not
  check each reader of those record fields); `ackfx` (FRONTEND 8.6,
  N/A); `GPFVersion` (Revision.GPF warning, N/A); `NoDirectSound` / `UseWindowsSound` (ported /
  N/A above); two mission-file error texts. Every FBI / TDF / OTA / sound / campaign key the
  readers look up is otherwise present.
- **Imports.** Registry (`Software\Cavedog Entertainment\Total Annihilation`) — the settings file
  (FRONTEND §8); `GetPrivateProfileIntA` — totala.ini (ported above) and Warp.ini (FRONTEND 3.6);
  `GetUserNameA` — the default network name (ported, `os_user_name`); clipboard — the paste keys
  (ported); `GetAsyncKeyState` / `GetKeyState` / `SetCursorPos` / `GetCursorPos` — input
  (CLIENT); `CreateSemaphoreA` — the single-instance check (FRONTEND 1.6); `GlobalMemoryStatus` —
  the room's memory field (NETWORK); `SystemParametersInfoA` — mouse trails (item 2); `RegisterHotKey`,
  `DeviceIoControl`, `VirtualAlloc` … — the Cavedog runtime (N/A); `PlaySoundA` / `aux*` /
  `waveOut*Volume` — the Windows sound path and the 1998 mixer restore (N/A); `ShellExecuteA` —
  online.dll's links (item 1) and the runtime's help viewer; DirectDraw / DirectSound /
  DirectPlay / smackw32 — replaced by the rewrite's own platform, sound, network and movie code.
- **Files.** Every pattern and name in the strings is read by the rewrite or listed N/A:
  `*.HPI/*.UFO/*.CCX` + `rev%s.GP3`, `Maps\*.ota`, `Weapons\*.tdf`, the GUI files (SELVMODE,
  CDCHECK, the campaign GUIs FRONTEND 5.14 lists: never opened), `glamour\Arm01.PCX`,
  `sounds\explode.wav`, `ai\default.txt`, `gamedata\translate.tdf`, `savegame\%s.sav`,
  `%s%s%s%04i.pcx` (screenshots), `\Warp.ini`, `%s\totala.ini` (ported above), `stats.txt`,
  `memdump.txt`, `debugdat\%s.txt`, `DEBUG.FIL`, `ErrorLog.txt` (debug / error, N/A).
- **Console.** All 83 commands of the three tables (43 options, 10 cheats, 30 debug; plus the
  three AI-profile words) are registered and handled in `ta_compat/src/console_commands.cpp`.
- **Packets and key bindings** were not re-swept here: NETWORK §2 lists every packet type of the
  size table `0x451FD0`, CLIENT §2 the interface keys.
