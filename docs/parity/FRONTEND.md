# Front-end parity checklist

Area: everything outside a battle and the program around it (the front-end state machine
`0x00426E80`, every GUI the original opens outside battles, movies, the campaign flow,
saved games from the menus, the settings, the command line, the window).

Status words: **BUILT+VERIFIED** (evidence named), **BUILT-UNVERIFIED**, **PARTIAL**, **MISSING**,
**N/A** (with the reason). Sources: Ghidra `/TotalA.exe` (state machine, handlers, strings),
`docs/RE_MEMORY.md` ("Client" / "Front end", "Campaign", "Saved games"), the engine
(`client/`, `ta_compat/src/client_*.cpp`, `ta_compat/tools/ta_play.cpp`).

The audit was taken at base master `14456bf`; the "after" column is kept up to date as items land.

## 1. Program start-up and the state machine

| # | Item (original) | Before | After / evidence |
|---|---|---|---|
| 1.1 | State machine `0x426E80`: GS+0x2BBE top state, +0x2BBF/+0x2BC0 sub-state; a sub-state change runs `0x4256D0` first | PARTIAL (screens are a stack; transitions per screen) | |
| 1.2 | State 0 start: when the display is full screen, `PlayMovie` (registry, default 1) set → `1.zrb`, then state 1 (`2.zrb`), `PlayMovie` = 0 saved; else (no lobby switch `-c`/`-n`) `1.zrb` only; then the main menu | MISSING (no movies) | BUILT-UNVERIFIED (main menu's first entry; headless ta_play --movies: 1.zrb, a key, 2.zrb, Enter, main menu; PlayMovie written 0) |
| 1.3 | State 1 (`2.zrb`), state 3 (`5.zrb`, credits), state 4 (`3.zrb` + `5.zrb`, Arm campaign won), state 5 (`4.zrb` + `5.zrb`, Core campaign won) → main menu | MISSING | BUILT-UNVERIFIED (first start 1.zrb + 2.zrb, INTRO 2.zrb, credits 5.zrb, campaign ends 3.zrb / 4.zrb + 5.zrb through client::MovieScreen; headless runs checked the order, skipping and the return to the main menu) |
| 1.4 | Main menu sub-states: 1 run, 5 → single player, 6 → multiplayer, 7 → state 0 (intro again), 8 quit, 9 → credits | PARTIAL (no movie states) | BUILT-UNVERIFIED (the movie sub-states added: Intro, Credits, the first-start movies) |
| 1.5 | "Code segment checksum error found when switching FE states" message (`0x4256D0`, `0x428BC0`) | N/A — the check of the original's own code bytes; nothing to check in the engine | |
| 1.6 | Program run once: semaphore "Total Annihilation" — a second start exits at once | MISSING | |
| 1.7 | `srand(time)` at start (the menus' rand() state) | BUILT-UNVERIFIED (the client seeds its menu rand from the clock — an input) | |
| 1.8 | Language: registry `language` (default "english"), `gamedata\translate.tdf` | BUILT-UNVERIFIED (English only; translate.tdf not read) | |
| 1.9 | `SOFTWARE\Classes\AudioCD\shell` default value replaced by "cdshell" while running, restored at exit | N/A — CD audio autoplay of 1998 Windows; the engine plays `music\N.mp3` and never touches the machine's settings | |

## 2. Movies (`*.zrb`, Smacker; `0x426780`, player `0x47BDF0`…`0x47C6C0`)

| # | Item | Before | After / evidence |
|---|---|---|---|
| 2.1 | Smacker decoder: header, frame sizes/types, the four Huffman trees with their 3-value caches, palette records, video blocks (mono, full, skip, fill), ring frame | MISSING | BUILT+VERIFIED (movie_harness vs smackw32.dll: 1-5.zrb, 11,407 frames, 0 pixel / palette differences) |
| 2.2 | Smacker audio: DPCM tracks (8/16-bit, mono/stereo), all 7 tracks opened (`SmackOpen` flags 0xFE100) | MISSING | BUILT+VERIFIED (movie_harness: SmackGetTrackData of every frame, 16.8 MB, 0 differences; 16-bit / mono paths unexercised by the game's files) |
| 2.3 | Playback: files `Data\<n>.zrb` (loose files); sounds stopped and the screen cleared first; the pointer hidden; frame at x 0, y (480 − height) / 2; Y-interlaced files leave every other line black; palette taken on frames that carry one; the movie's own frame rate | MISSING | BUILT+VERIFIED placement (movie_harness compares the 640x480 placement); pacing / sound start BUILT-UNVERIFIED (client::MovieScreen) |
| 2.4 | Keys: any typed character ends the movie (and the Shift-started loop); Alt+F4 ends the program; mouse buttons do nothing | MISSING | BUILT-UNVERIFIED (MovieScreen::typed_character; Alt+F4 = the window's close, which quits) |
| 2.5 | INTRO button (`0x425D80`): "smlButton", full-screen and CD checks, Shift held → the intro repeats until a key | MISSING (button does nothing) | BUILT-UNVERIFIED (headless: INTRO click -> 2.zrb; Shift -> repeat) |
| 2.6 | Credits button → `5.zrb` (same checks) | MISSING | BUILT-UNVERIFIED (headless: Credits hot spot -> 5.zrb at y 88) |
| 2.7 | "Debug: You must be in full-screen mode to play a movie." when windowed | MISSING | BUILT-UNVERIFIED (headless without --movies shows the message box) |
| 2.8 | Error texts "Could not open movie file, please check filename in INI.", "Could not setup Direct Draw to play movie.", "Smacker Error", stats.txt | N/A — DirectDraw / SDK diagnostics (a missing file is skipped as the original's existence check does) | |

## 3. Main menu (MAINMENU.GUI, `0x4263B0`, handler `0x425D80`)

| # | Item | Before | After / evidence |
|---|---|---|---|
| 3.1 | Layout, FrontendX, version label "v3.1", sparks | BUILT+VERIFIED (gui_harness passes; sparks exact) | |
| 3.2 | SINGLE ("BigButton"), MULTI ("BigButton", multiplayer data check), EXIT | BUILT-UNVERIFIED | |
| 3.3 | INTRO / Credits | MISSING → see 2.5, 2.6 | BUILT-UNVERIFIED (see 2.5, 2.6) |
| 3.4 | Music mood 4 (silent), BGM loop | BUILT-UNVERIFIED | |
| 3.5 | "Close Windows CD Player?" (YESORNO over the menu, `0x426200`) | N/A — the Windows 95 CD player owning the drive | |
| 3.6 | Warp hot keys / WARPLEVELS (`0x4269D0`, `\Warp.ini`) | N/A — needs a `Warp.ini` the game does not ship (developer start) — to be confirmed | |

## 4. Single player (SINGLE.GUI, `0x4777A0`, handler `0x4775A0`)

| # | Item | Before | After / evidence |
|---|---|---|---|
| 4.1 | New Campaign ("BigButton" → NEWGAME), Load Game ("BigButton"), Skirmish ("skirmish"), Options ("options"), Previous Menu ("Previous") | BUILT-UNVERIFIED (New Campaign goes to the engine's own text screen) | |
| 4.2 | AnyMsn shown only with `AllMissions`; "bigButton" → NEWGAME in any-mission mode | PARTIAL (button not hidden by the setting; goes to the text screen) | BUILT-UNVERIFIED (AnyMsn shown by AllMissions; opens the any-mission screen, verified above) |
| 4.3 | Typing DRDEATH toggles AnyMsn and writes `AllMissions` (`0x477510`) | MISSING | BUILT-UNVERIFIED (typed-key tail of the interface pass; DRDEATH toggles AllMissions) |
| 4.4 | Registry `side` sets the local player's side (record +0x95) | MISSING | |
| 4.5 | Spanish: the Skirmish button's quick key becomes 's' | N/A — English data only | |
| 4.6 | CD checks ("Please insert the Campaign CD (Disc 2) and try again", Multiplayer CD) | N/A — the Steam version's check always passes (`0x50289C` = 1) | |

## 5. Campaign

| # | Item | Before | After / evidence |
|---|---|---|---|
| 5.1 | New campaign screen NEWGAME.GUI (`0x478240`, handler `0x477AB0`): background newcampaign4 (more than 2 campaigns) / newcampaign4x / playanygame4 (any mission); Side0/Side1 pictures and Arm/Core buttons ("SideSelect" / "SideSelect2"), Campaign list (+knob) by side, Missions list (+knob) in any-mission mode (Campaign list moved to y 308, height 48, Missions height 62), Difficulty switch ("SmlButton", registry `Difficulty`), Start ("bigButton"), Previous Menu ("Previous") | PARTIAL (a text screen of the engine's own: wrong art, keys, no sides) | BUILT+VERIFIED (gui_harness 0x478240(0) after main/single/rescan vs ta_play headless: new campaign, after Core, after Arm, any mission: 0 of 307,200 pixels differ each; newcampaign4x untestable: 14 campaigns installed) |
| 5.2 | Campaign list = `camps\*.tdf` whose [HEADER] campaignside matches the chosen side (`0x476A60`) | PARTIAL (all campaigns listed) | BUILT+VERIFIED (same comparisons: ARM list incl. EXAMPLE/Krogoth, CORE list) |
| 5.3 | Start: mission status string reset (`0x41DA30`), the campaign loaded, the mission (0 or the chosen one), records +0x96 set, prefs saved, → briefing | PARTIAL | BUILT-UNVERIFIED (Start: rescan, results reset, the mission, settings saved, MSNBRIEF; back from the briefing the screen is set up anew) |
| 5.4 | Mission briefing MSNBRIEF.GUI (`0x478E80`, handler `0x478CB0`): the planet's rotating picture (`<planet>Rotate`), the panorama (`<planet>Pan`, `0x478790`), the planet's name and gravity / wind texts, the brief text in pages with colour mark-up (`0x476CD0`…`0x476EF0`), MORE / BACK TO START, narration, SHUTUP, Start, Previous | PARTIAL (a text screen: no art, no planet, no panorama, own keys) | BUILT+VERIFIED first page (gui_harness campaign:'Arm Campaign':0 + srand + msnbrief vs ta_play TA_BRIEF_RAND: 0 of 307,200 pixels differ; pages 2 and 3 after clicks: text and MOREBAR identical, planet / panorama differ only by the harness's wall clock); animation timing BUILT-UNVERIFIED |
| 5.5 | Briefing music by locale (`0x476C70`) and glamour sound (`0x476CA0`) | MISSING | BUILT-UNVERIFIED (narration at set-up and by the switch; the briefing's own music: none in 3.1 — 0x476C70 is never called) |
| 5.6 | Rescan of the archives at New Campaign / Start / briefing (the Steam `totala4.hpi` missions) | BUILT-UNVERIFIED (`mount_cd_archives` at the campaign menu) | |
| 5.7 | End of a won mission: glamour picture `bitmaps\glamour\<glamour>.pcx` (fallback `glamour\Arm01.PCX`) faded in (`0x41DFC0`/`0x41E270`), glamour sound, "Click to continue." after 5 s, a key or click → statistics | MISSING | BUILT-UNVERIFIED (headless TA_TEST_MISSION_END=win: AC01's glamour faded in one step a tick by fifths, its sound, 'Click to continue.' after 1 + 5 s, a click goes on) |
| 5.8 | Campaign statistics screen ENDMSN (`0x41F0A0`/`0x41F400`/`0x41EC50`): outcome1, Start / Load Game / Save Game / Missions list with ✓/✗ marks / Difficulty / AdjustDiff / MainMenu; the next mission selected after a win | PARTIAL (an engine text screen) | BUILT+VERIFIED (gui_harness campaign + endmsn type 1 + endrun:0,70 vs ta_play --end-screen with campaign keys: 0 of 307,200 pixels; Start -> the chosen mission's briefing, its Previous back to this screen; Save Game writes a between-missions save; Load Game; Difficulty) |
| 5.9 | Final mission won → `3.zrb`/`4.zrb` + `5.zrb` → main menu (not windowed) | MISSING | BUILT-UNVERIFIED (movies 3/4 + 5 then the main menu; windowed: straight to the main menu) |
| 5.10 | Campaign Restart (exit menu RESTART in a mission) | MISSING (the dialog closes) | BUILT+VERIFIED headless (ta_play: Arm mission 1, F2 > Exit > Restart > Difficulty Hard->Easy > Restart: the mission reloaded from step 0 with the Easy schema's units; the exit menu offers Restart in missions; the difficulty kept in the campaign's progress) |
| 5.11 | Progress: the 25-character status string (GS+0x391CF) per session and in saves; registry `ArmCamp`/`CoreCamp` writers exist but are never called in 3.1 | BUILT-UNVERIFIED (string kept in the session and saves) | |
| 5.12 | Mission keys AiIgnore, AiPriorityTarget, InitialGroup, BuildPriority, CreationCountdown, MissionCriticalUnit | PARTIAL (read by the importer, not used; simulation effect is the SIM agent's) | |
| 5.13 | CDCHECK.GUI at the end of a campaign battle | N/A — the Steam check passes | |
| 5.14 | SELCAMP / SELCAMPX / NEWCAMP / SELSIDE / SIDESEL / SSIDESEL / ssidesel1 / MISSION / MISSIONX / BRIEF / BRIEFX / CREDITS / DEMOMSG GUIs | N/A — present in the data but never opened by the 3.1 program (no string or reference) | |

## 6. Skirmish, map selection, load / save

| # | Item | Before | After / evidence |
|---|---|---|---|
| 6.1 | Skirmish set-up (SKIRMISH.GUI) rows, options, Start checks and messages | BUILT-UNVERIFIED (gui_harness renders it; rules as read) | |
| 6.2 | Roman numeral player-count cheat (III…X) | BUILT-UNVERIFIED | BUILT-UNVERIFIED (was missing: '*III'..'*X' typed in the skirmish set-up, 0x47B9F0) |
| 6.3 | Select map (SELMAP.GUI) | BUILT-UNVERIFIED | |
| 6.4 | Load / save menus (LOADGAME.GUI) | BUILT+VERIFIED (commits 47505c4, 386c140) | |
| 6.5 | Saves in the original's format; re-save byte difference | BUILT-UNVERIFIED (save_check reports re-save differences) | |

## 7. Options (STARTOPT.GUI and pages)

| # | Item | Before | After / evidence |
|---|---|---|---|
| 7.1 | Pages SOUND / MUSIC / INTERFACE / VISUALS, OK / Cancel / Restore / Undo | BUILT-UNVERIFIED | |
| 7.2 | SELVMODE.GUI (screen size dialog, `0x45E5E0(1)`) | PARTIAL (the slider is on the visuals page; the separate dialog not built) | |
| 7.3 | Anti-alias registry name "Anti-Alias" | PARTIAL (engine key "Anti Alias") | BUILT-UNVERIFIED (key renamed 'Anti-Alias', the string at 0x504618 of the original) |

## 8. Settings (registry `0x42F9A0`, writer `0x430F00`) → the settings file

| # | Item | Before | After / evidence |
|---|---|---|---|
| 8.1 | Interface Type 0 (clamped ≤ 1), DisplaymodeWidth/Height 640/480, side 0, Difficulty 1, scrollspeed 32 | PARTIAL (side missing) | |
| 8.2 | SingleCommanderDeath / SingleMapping / SingleLineOfSight / SingleLOSType 1; MultiCommanderDeath / MultiMapping / MultiLineOfSight / MultiLOSType 1 | MISSING | |
| 8.3 | screenchat 1, damagebars 0, Sound Mode 1, MixingBuffers 8, RestoreVolume / WaveOutVolume / CDAudioVolume | PARTIAL (screenchat missing; the volume restore is the 1998 mixer: N/A) | BUILT-UNVERIFIED (screenchat 1 read into the client's options; the volume restore stays N/A) |
| 8.4 | Anti-Alias 1, Shadows 1, FeatureShadows 1, VehicleShadows 1, Shading 1, DitheredFog 0, Gamma 12 (10 reads as 12), SwitchAlt 0 | PARTIAL (Shadows, VehicleShadows, DitheredFog, the 10→12 rule missing) | BUILT-UNVERIFIED (Anti-Alias, Shadows, FeatureShadows, VehicleShadows, Shading 1 / DitheredFog 0 read into the client's options as bit 0 of each value; Gamma 10 shows as 12 on the visuals page) |
| 8.5 | Password, Nickname, Game Name, Image Output Directory (`<user>_images`-like default), Movie Output Rate 10 | PARTIAL (Nickname only) | |
| 8.6 | textlines 10, textscroll 10, mousespeed 10, gamespeed 10, unitchat 10, unitchattext 5, musicmode 1, cdmode 4, ackfx 1, buildfx 1, speechfx 1, fxvol 27, musicvol 32, clock 0 | PARTIAL (mousespeed, ackfx, buildfx missing) | |
| 8.7 | NumSkirmishPlayers 4, Skirmish… values, SkirmishMap (first map), `Total Annihilation\Skirmish` Player%d… | BUILT-UNVERIFIED | |
| 8.8 | PlayMovie 1, DisplaymodeDepth / Games (option bit 2), AllMissions 0 | PARTIAL (AllMissions only) | PARTIAL (PlayMovie 1 read and cleared after the first start's movies; AllMissions; DisplaymodeDepth / Games not used: one colour depth, no DirectPlay option) |
| 8.9 | CDLISTS track types | BUILT-UNVERIFIED | |

## 9. Command line (`0x49EE30`)

| # | Switch | Before | After / evidence |
|---|---|---|---|
| 9.1 | `-c <file>` lobby / connection file, `-n <1-4>[:addr]` provider, `-h <name>` host game name, `-p <n>` packet rate, `-t <30-300>` time-out, `-e <0-100>` | MISSING (network start-ups; the NETWORK agent owns their behaviour) | |
| 9.2 | `-s` no sound (NoDirectSound), `-w` Windows sound | MISSING | PARTIAL: -s / /s (any case) starts without sound (ta_play: front.sound off; BUILT-UNVERIFIED, the headless log lists sounds either way); -w (Windows wave-out instead of DirectSound) N/A — the client has one audio output |
| 9.3 | `-d` / `-df` display (debug window / flag) | MISSING | |
| 9.4 | `-f` CD drive H: only, `-l` (no effect), `-r` DirectPlay registration and exit, `-b` (battle switches) | N/A for -f/-r/-l (CD drives, DirectPlay registry); -b to be read | |
| 9.5 | A bare word = the language | MISSING | |
| 9.6 | Cavedog library switches (-memfussy …, -dprint…) | N/A — the 1998 debug library | |

## 10. Window, close and errors

| # | Item | Before | After / evidence |
|---|---|---|---|
| 10.1 | Close / Alt+F4 in a battle → "Surrender this battle and exit to Windows?" (YESORNO) | MISSING (the engine quits at once) | BUILT+VERIFIED headless (ta_play script 'quit' in a campaign battle: YESORNO 'Surrender this battle and exit to Windows?' over the battle, Yes quits, No closes; the original's battle state sets close handler 0x4609A0 -> 0x460680 with intent 2; menus and end screens still quit at once) |
| 10.2 | Close in the menus → network cleanup, end flag 4, quit (with the lobby's status text when set) | BUILT-UNVERIFIED (quits) | |
| 10.3 | Alt+F4 during a movie → quit | MISSING (no movies) | BUILT-UNVERIFIED (a close during a movie quits: the movie runs outside a battle) |
| 10.4 | Window can't be moved (SC_MOVE refused) | N/A — a full-screen DirectDraw window; the engine's window is a platform window | |
| 10.5 | Error texts: "Error: Environment Initialization Failed! Check your DirectX setup", "Error: Sound system initialization failed.", "No sound driver is available for use.", "Out of memory! Your hard disk may be full", "Unable to start the loading thread!", "Can't load GAMEDATA.TDF", "Can't load MOVEINFO.TDF", "No [%s] in GAMEDATA/SIDEDATA.TDF for side:%s", the mission file errors | PARTIAL (the engine reports its own texts) | |

## 11. Battle room drawing (FRONTEND draws, NETWORK behaves)

| # | Item | Before | After / evidence |
|---|---|---|---|
| 11.1 | Start button glow difference in the battle room | PARTIAL (see RE_MEMORY "Battle room, live") | PARTIAL, rule confirmed (0x4B8310 read: START's BUTTONS0 frames are compressed and one layer, so dst = T[level*256 + src], the client's rule; uncompressed and layered frames follow other rules, now in RE_MEMORY). The remaining difference is not resolved: a re-run of gui_harness on the NETWORK agent's room dump differs elsewhere too (records / chat), so no clean comparison of the light row was possible here |

## 12. Not in this area (noted)

In-battle menus and pages (ARMOPT, PREFS, HELP, BRIEFING, EXITMENU, RESTART, YESORNO, TABMENU,
the unit info screen) belong to the CLIENT agent except where the campaign flow needs them
(5.10). Multiplayer screens (SELPROV, TCP, SERIAL, MODEM, SELGAME, NEWMULTI, LOUNGE2, ALLIES,
SHARE, CONTROL, REPORT, TIMEOUT, LOGOSEL, ENDMULTI, SAVELIST, LOADLIST, RESTRICT2) belong to the
NETWORK agent.

## Counts

Before: BUILT+VERIFIED 3, BUILT-UNVERIFIED 17, PARTIAL 20, MISSING 22, N/A 15.

Recounted row by row over the 72 numbered rows (status word of the "Before" cell, and of the
"After" cell where set): before BUILT+VERIFIED 2, BUILT-UNVERIFIED 15, PARTIAL 19, MISSING 24,
N/A 12; after BUILT+VERIFIED 11, BUILT-UNVERIFIED 33, PARTIAL 10, MISSING 6, N/A 12.

Still missing / partial, and why: 1.6 single-instance semaphore and 9.3 / 9.5 (-d display flag,
bare-word language): program-level switches with no front-end screen, not built. 4.4 / 8.1 the
`side` setting and 8.2 Single* / Multi* options: the values feed the battle room and the
battle's options (NETWORK / CLIENT areas), not read yet. 8.5 / 8.6 Password, Game Name, image
output, mousespeed, ackfx, buildfx: no screen of this area uses them. 7.2 the SELVMODE dialog.
10.5 the original's error texts. 11.1 the battle room's START glow (rule confirmed, the light
row difference not resolved). 1.1 the state machine is a screen stack by design. 5.12 the
mission keys' effects belong to SIM.
