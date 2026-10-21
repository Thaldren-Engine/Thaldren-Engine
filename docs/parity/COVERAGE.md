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
| Data files and formats | 19 | 12 | 1 | 6 | 0 |
| Front end (menus, campaign, movies) | 462 | 190 | 239 | 33 | 0 |
| Network | 326 | 166 | 107 | 53 | 0 |
| Rendering | 114 | 70 | 41 | 3 | 0 |
| Saves | 38 | 34 | 4 | 0 | 0 |
| Simulation | 965 | 694 | 258 | 13 | 0 |
| Library: Cavedog runtime (heap, crash log, perf dialogs, registry/window helpers) | 206 | 0 | 0 | 206 | 0 |
| Library: HPI LZ77 (ported as ta_compat hpi) | 11 | 11 | 0 | 0 | 0 |
| Library: MSVC C runtime | 417 | 2 | 0 | 415 | 0 |
| Library: MSVC std::string | 15 | 0 | 0 | 15 | 0 |
| Library: zlib 1.0.4 inflate (ported as ta_compat inflate) | 43 | 43 | 0 | 0 | 0 |
| **Total** | 3124 | 1572 | 773 | 779 | 0 |

(The "Data files" row is small because most format readers are cited from RE_MEMORY's format
sections and land with the subsystem that uses them. The 8 functions that were not ported were
the two items of the ranked list, mouse trails and the palette tables, ported since (2026-10-21,
"Ported in this sweep"); each row's not-ported count moved to its ported column. The online-service
buttons, item 1 of the first version of this list (`0x444580`'s links, `0x44475D`, `0x45B490`,
`0x45B250`), are ported: see below.)

## Ported in this sweep

| Item | Original | Rewrite | Evidence |
|---|---|---|---|
| Unit notices show the order behaviour's own text. `Unit_QueueSpeech` uses the table text only when the caller gives none; the behaviours pass 43 texts ("Unable to create any more units", "I can't reach the construction site", "Capture failed", "Landing aborted: all pads are occupied", "Ressurection failed" (sic) …) and the acknowledgements their mission word ("Guarding", "Repairing", "Loading unit" …). At the default unitchattext 5 a failed build showed "ARMCOM: Cannot Comply" where the original shows "ARMCOM: Unable to create any more units" | `0x47F780`, `0x438880`, strings `0x5010CC`..`0x501CFC` | `ta_compat/src/notice_texts.cpp` (`ta::speech_text`), `GameClient::on_speech(unit, id, text)`, `NoticeQueue::queue(…, text)`, `ta_compat/src/client_battle_setup.cpp` | compat_tests `test_notice_texts`; each string's push site checked in the disassembly (the two resurrection texts: `0x404DEF` / `0x404F6F`) |
| The unit limit is the session value GS+0x37EEC: at start-up `[Preferences] UnitLimit` of `totala.ini` beside the program (default 250, more than 500 → 500, less than 20 → 20), replaced by a loaded save's `maxunits`; it is not a registry setting (the client read a `UnitLimit` setting). `NoDirectSound` from the same file starts without sound as `-s` does | `0x491200`, `0x49F5A0`, `0x432610` (from `0x497180`), readers `0x4971F1`, `0x449C3B`; `0x47ED40` | `ta_compat/src/preferences.cpp` (`ta::profile_int`, `preferences_unit_limit`, `preferences_int`), `FrontEnd::unit_limit()`, ta_play | compat_tests `test_preferences`: 42 cases of GetPrivateProfileIntA's parsing measured on Windows 11 (sections, keys, quotes, signs, hex, wrap-around) and the clamp |
| The online-service buttons of the connection screen: "www.cavedog.com", "Unit Download" and one per service extension DLL beside the program (`ta*.dll` in the system's listing order, at most 10, the Mplayer one moved first), copies of the hidden SERVICEX 21 pixels apart, captions from each extension's probe ("Get Mplayer" / "Play on Mplayer", "Get HEAT", "Get Kali", "DWANGO", "Play Free on TEN", "Get Wireplay (UK)" …: registry keys in the 32-bit view, ta.mpi); a press plays BigButton and opens the web page or service program (then the program ends, as the original's exit(0)), or shows the installation-CD question / the failure text in a 320-wide box | `0x444580` → `0x45B490` (online.dll 3: `ONLGetLinkInfo` 0x10001BB0, scan 0x10001750), `0x44475D`; handler `0x4441A0` → `0x45B250` (`ONLProcessButtonCommand` 0x100011E0), `0x45B640`; the six extensions' `TotalAExtGetButtonText` / `TotalAExtAction` | `ta_compat/src/online_services.cpp` (`ta::OnlineServices`, `real_online_system`: the DLLs' answers as rules, nothing loaded), `SelProv::add_service_buttons` / `press_service` in `ta_compat/src/client_net_screens.cpp`, the client hook `FrontEndHooks::open_external` (ta_play: ShellExecuteA / CreateProcessA; `--open-log FILE` for tests, `--online-links`) | compat_tests `test_online_services` (35 checks: listing order, exclusions, the ten limit and the Mplayer move, every probe, every press outcome and message); gui_harness `links:… selprov:…` against ta_play `--net-screen selprov` on this machine (8 buttons): 0 differing pixels; the press of "Get Mplayer" by its quick key G (gui_harness `onlpress:1:TEXT press:g frame`, the call into online.dll answered with its CD text) against ta_play: 0 differing pixels in four frames |
| Mouse trails off while the program runs: at start-up (before the archives are mounted) `SystemParametersInfoA(SPI_GETMOUSETRAILS 0x5E, 0, &saved, 1)` and `(SPI_SETMOUSETRAILS 0x5D, 0, NULL, 1)` (fWinIni = SPIF_UPDATEINIFILE only; a failed query saves 0); after the message loop ends (window closed, Exit to Windows, Alt+F4 in a movie) `(0x5D, saved, NULL, 1)`. The fatal errors (`0x4B6290` → exit(1)), the online-service start (exit(0)) and a failed window set-up never reach the restore: the trails stay off | `0x4B5980`, `0x4B6110` (only from WinMain `0x49ED6F`) | `client/include/client/mouse_trails.h` (`client::MouseTrails`, the system call injected), ta_play (`system_parameters()`; window runs only: off after the semaphore, back when `FrontEndSetup::loop_ended`; `FrontEnd::exit_at_once()` for the online-service start) | compat_tests `test_mouse_trails` (the call sequence, parameters and flags, a failed query); headless runs and tests never call the system |
| Palette tables built when missing: `palettes\PALETTE.PAL` from `PALETTE.PCX`'s colour map (manufacturer 0x0A, version 5, the last 768 bytes) and cached, the loose ALP / LHT / SHD then deleted; `PALETTE.ALP` (alpha: the nearest entry to the channel halves), `.SHD` (32 levels, factor 0 + n × 0.06875) and `.LHT` (32 levels, 1 + n / 30) built over the palette sorted by channel sum (exchange sort, nearest within ±40) when the file is missing or empty, and written with fopen "wb" into the game folder (only when its `palettes` folder exists); a missing PCX is the fatal error with its path | `0x42A400`, `0x429330`, `0x4CB080`, `0x42E140`, `0x42E1D0`, `0x42E260`, `0x4BA920`, `0x4BA9D0`, `0x4BA750`, `0x4BADF0`, `0x4BABD0`, `0x4BC290`, `0x4BBC30` | `ta_compat/src/palette_tables.cpp` (`ta::load_palette_tables`, `build_alpha_table`, `build_shade_table`, `build_light_table`, `pcx_palette`), `MenuArt::load` (start-up, with the cache writes), `ClientWorld::load` | orig_harness `palette_harness`: the original's builders on the shipped palette, 4 generated and 240 random palettes = the port, 0 differing bytes; the port's (and the original's) tables of the shipped palette = the shipped files (65,536 + 8,192 + 8,192 bytes, 0 differ); compat_tests (the original's tables of 4 palettes by hash, the start-up sequence over a temporary folder); a mod-like headless ta_play over a copy of the install whose archives lack the three tables: tables built, written byte-identical to the shipped files when `palettes` exists, nothing written when not, menu and battle screenshots identical to the real install's |
| CD audio music (was N/A: "the client plays the music tracks from files"). The original plays CD audio through `mciSendStringA` ("open / status / set / play / pause / stop cdaudio", the track-end notification, WM_DEVICECHANGE), the CD's aux volume, the 30-a-second tick timers for the mood change's fade (−volume / 18 every 2 ticks, then the pick, after 120 ticks for building), the disc's track types by volume serial (CDLISTS), the Windows CD player's window and question. The shipped program sends those commands to the replacement library beside it (`win32.dll`, the WINMM import renamed), which answers 0 with no reply, plays music/N.mp3 for "play cdaudio from N" on its own thread and ignores "stop" — the file-based music is the shipped program's, not an invention, but with its rules: a data track first (CD track N = file N + 1, 16 tracks by the 3.1 "17 tracks" patch), the music plays on in the menus, the fade is inaudible, the game menu holds and resumes at the position | `0x4CDA00`..`0x4CED40`, `0x4D00D0`, `0x4D0040`, `0x4CFF30`, `0x4B63F0` / `0x4B6370` / `0x4B64D0`, `0x490FE0`, `0x490F80`, `0x494E70`, `0x45D280` / `0x45D7C0` / `0x45C3F0` / `0x45D130` / `0x45D0C0`, `0x426200` / `0x426190`, `0x4CE030`, `0x49E830`'s activation; win32.dll `0x10002140`, `0x10001330` | `client/cd_audio.h` (`CdMusic`, `TickTimers`, `CdTrackLists`, `BattleMusicJudge`), `client/track_file_device.h`, `client/cd_audio_system.h` (system MCI, aux, drives, the window tap), the front end's call sites, the music page and the CD player question (`ta_compat/src/client_screens.cpp`), ta_play `--cd-audio tracks / system` | compat_tests `test_cd_music`, `test_cd_timers`, `test_cd_drives`, `test_cd_track_lists`, `test_track_file_device`, `test_battle_music_judge` (routines as read; the library's thread on a test clock); headless skirmish runs (music/14.mp3 after 1 s, held / resumed at its position); CLIENT 3.7–3.9 |
| CD probes and prompts (was N/A: "the Steam flag answers at once"): re-read — the check is not patched out, the flag `0x50289C` (1, no writer) short-circuits `0x41D6A0` to '.', so every caller passes and no prompt (or CDCHECK.GUI) is reachable; the CD-ROM walk and the volume serial stay reachable (the music's disc key) | `0x41D6A0`, `0x4BB190`, `0x4BB260`, `0x41F680`, the callers listed in RE_MEMORY "CD checks" | `ta/cd_check.h` (`cd_probe` with the TOTALA.ID walk and -f, `cd_present`), `client::next_cdrom_drive` / `cd_volume_serial` | compat_tests `test_cd_check`, `test_cd_drives`; FRONTEND 3.5, 4.6, 5.13 |
| The sound paths (was N/A: "one audio output"): -s / NoDirectSound (no effects, narration or movie sound — `SmackSoundOnOff(DirectSound ≠ 0)` — the music plays) and -w / UseWindowsSound (`0x49F610` always 1: PlaySoundA, one sound at a time at the file's level, RIFF only, no volume / mode / sight / view rules, the loop restarted at a pass with nothing playing). The effect volume (`waveOutSetVolume`, fxvol × 1024) is the program's whole session volume on today's Windows (measured), so it scales the music too | `0x47ED40`, `0x49EE30`, `0x47EFC0` / `0x47EFD0`, `0x47EFE0`, `0x47F0C0`, `0x47F300`, `0x47F750`, `0x47F680`, `0x49F620` / `0x49F640` / `0x49F680` / `0x49F6A0` / `0x49F6C0`, `0x4D0070` | `FrontOptions::sound_path`, `client::WaveOutChannel`, `ClientOptions::wave_out`, `AudioOut::set_fx_volume` | compat_tests `test_wave_out_channel`; headless `-W` run; CLIENT 3.10, 3.11, FRONTEND 9.2 |
| Scores reporting (was N/A): the reachable path on TCP/IP and IPX games — the reporting flag set, `reporter.dll` not found (not shipped; a 64-bit program cannot load the 32-bit one), no services, REPORT.GUI not opened, nothing shown (serial / modem / no provider: not tried) | `0x4436E0`, `0x46BF30`, `0x46BCE0`, `0x46C190`, `0x46C620`, `0x4435A0` | `ta::start_scores_reporting` / `scores_reporting_shown` (`ta/cd_check.h`), run where the original runs it (`client_net_screens.cpp`, a joined and a hosted session) | compat_tests `test_scores_reporting` |

After both: sim_tests 5848 checks / 0 failures, net_tests 514 / 0, compat_tests 240 / 0, battle_menus_check ok (skirmish, net host, net join), game_flow all identical, whole-game replays g8 (3,000 steps), s2 (4,500), g9 (2,759) identical. After the online-service buttons (2026-10-21): sim_tests 5859 / 0, net_tests 514 / 0, compat_tests 284 / 0, battle_menus_check ok. After the audio paths (CD audio, CD checks, sound paths, scores reporting; 2026-10-21): sim_tests 5880 / 0, net_tests 522 / 0, compat_tests 436 / 0, battle_menus_check ok (skirmish, net host, net join), game_flow all identical (checksum cf546f1c, as before), replays g8 / s2 / g9 identical. The per-subsystem table above is the sweep's; the functions of the four N/A rows ported here (Client and Front end rows) now count as ported.

## Not ported, ranked by what a player or peer sees

Nothing left (2026-10-21): the two items of this list, the mouse trails (`0x4B5980` /
`0x4B6110`) and the palette tables built when their files are missing (`0x429330`, `0x42E140`,
`0x42E1D0`, `0x42E260`, `0x4BA750`, `0x4BADF0`, `0x4BABD0`, `0x4BC290`), are ported: see "Ported
in this sweep".

Related but already tracked by the parity documents (not repeated here): the movie states and
CD prompts (FRONTEND; the prompts are unreachable on the shipped program), the lens shot (CLIENT; the music cross-fade is ported), the network items
against two or more originals (NETWORK).

## N/A (with the reason)

| What | Where | Why |
|---|---|---|
| DirectPlay wrapper "HAPINET" (33 functions; DPERR texts) | `0x4C9530`..`0x4CAA10` | rule: no DirectPlay; engine/net speaks its wire protocol itself (NETWORK §3) |
| `-c` lobby file through online.dll (`ONLLoadConfigFile`) | `0x45B670` from `0x49EE30` | FRONTEND 9.1: lobby launches are N/A |
| Code-segment checksum on every front-end state change | `0x4256D0`, `0x4257E0`, `0x425860`, `0x4259B0`, `0x425A90`, `0x428BC0` | the check is a stub returning 0 in 3.1 (FRONTEND 1.5) |
| Smacker DirectDraw glue (pixel format, palette, paint) | `0x47C150`, `0x47C230`, `0x47C2A0` | the movie player is reimplemented (FRONTEND §2) |
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
  70), the unit-notice texts (ported above), MCI command strings (14; ported with the CD audio, below), the online.dll / reporter
  names (reporter and online.dll's texts: ported), CD prompts (ported: unreachable on the shipped program) and checksum messages (N/A), assertion file names
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
  the room's memory field (NETWORK); `SystemParametersInfoA` — mouse trails (ported); `RegisterHotKey`,
  `DeviceIoControl`, `VirtualAlloc` … — the Cavedog runtime (N/A); `PlaySoundA` / `aux*` /
  `waveOut*Volume` — the Windows sound path, the CD's aux volume and the effect volume (ported below) and the 1998 mixer restore (N/A); `ShellExecuteA` —
  online.dll's links (ported: the client's `open_external` hook) and the runtime's help viewer; DirectDraw / DirectSound /
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
