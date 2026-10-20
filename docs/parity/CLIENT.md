# Client parity checklist (battle drawing, battle interface, battle sound)

Area: everything the player sees, hears and does during a battle. Sources: the original's
functions reachable from the battle pass `0x496790`, the frame `0x468CF0`, the key handler
`0x495E90`, the clicks `0x498F70` / `0x499100`, scrolling `0x41CE90`, the panel handlers
(`0x41AA00`, `0x419BE0`, `0x41A490`), the tab bar `0x494740` / `0x495010`, the share and unit
info screens (`0x4936F0`, `0x4942E0`) — a depth-4 call walk gave 655 functions, 396 of them
already named in RE_MEMORY or the sources; the strings the original uses here; the side data,
GUI and sound keys; and the open items of RE_MEMORY's "Rendering", "Client" and "Console
commands" sections.

Status words: **BUILT+VERIFIED** (evidence named), **BUILT-UNVERIFIED** (read and built, not
compared with the original), **PARTIAL**, **MISSING**, **NOT-APPLICABLE** (reason).
Front-end screens (menus before and after a battle, the battle room, the loading screen, the
statistics) belong to the FRONTEND checklist; packet semantics to NETWORK.

Counts at the start of this round (2026-10-20): 98 items — BUILT+VERIFIED 26, BUILT-UNVERIFIED 17,
PARTIAL 18, MISSING 33, NOT-APPLICABLE 4. The "Now" column is kept up to date below; the
summary at the end gives the counts after this round.

## 1. Battle drawing

| # | Item | Original | Status | Evidence / note |
|---|------|----------|--------|-----------------|
| 1.1 | Frame order (terrain, row buckets, low features, ground units + tall features, shots, effects, air units, overlays) | `0x468CF0` | BUILT+VERIFIED | frame_harness scenes; RE_MEMORY "Rendering" |
| 1.2 | Terrain tiles | `0x483FA0` | BUILT+VERIFIED | frame_harness |
| 1.3 | Unit model sprites, piece order, textures, team colours | `0x45AC20`, `0x458810`, `0x459830`, `0x42A140` | BUILT+VERIFIED | raster_harness (scan conversion), frame_harness |
| 1.4 | Uncached pieces drawn direct | `0x4584D0` | BUILT+VERIFIED | rules assumed equal to the tested routines. *2026-10-20*: render_harness (the original's whole unit draw `0x45AC20` in a suspended process vs the engine's: random real unit types, poses, cached / visible / shaded piece flags, structure, z-buffer, under construction, cargo, cloak, digger / hover / floater / no shadow, sea level, shadows / unit shadows / anti-aliasing / shading options): seeds 5 and 77, 699 scenarios identical (101 + structures, 234 + z-buffer, 69 + under construction, 77 + with cargo, aircraft among them), 1 scenario where the original's own draw faulted |
| 1.5 | Ground shadows (shadows option, `noshadow`) | `0x459200`, `0x45A470` | BUILT+VERIFIED | frame_harness |
| 1.6 | Air units' projected shadows | `0x45A790` | BUILT+VERIFIED | re-checked 2026-10-20: `0x45A790` is the structure shadow (units with the static flag), built as `structure_shadow`; aircraft take the sprite copy (`Gaf_CopyFrameToShadow`) 5 pixels right at the ground under them with VehicleShadows and type flags 0x81000 clear, as the client's mobile shadow — the row's 'approximation' note was wrong. *2026-10-20*: render_harness (the original's whole unit draw `0x45AC20` in a suspended process vs the engine's: random real unit types, poses, cached / visible / shaded piece flags, structure, z-buffer, under construction, cargo, cloak, digger / hover / floater / no shadow, sea level, shadows / unit shadows / anti-aliasing / shading options): seeds 5 and 77, 699 scenarios identical (101 + structures, 234 + z-buffer, 69 + under construction, 77 + with cargo, aircraft among them), 1 scenario where the original's own draw faulted |
| 1.7 | Depth-plane path (`zbuffer` units, under construction, 3-D features' proxy unit) | `MemCache_AllocMaskBufferDual` | BUILT+VERIFIED | re-checked 2026-10-20: the depth sprite's cut / tint below the sea (compose_depth_look), its shadow cut at below + 50 (125 for diggers, `0x4BA1B0`) and the 3-D features' stand-in unit (`0x421F20`, client_scene.cpp) are built; the row was stale. *2026-10-20*: render_harness (the original's whole unit draw `0x45AC20` in a suspended process vs the engine's: random real unit types, poses, cached / visible / shaded piece flags, structure, z-buffer, under construction, cargo, cloak, digger / hover / floater / no shadow, sea level, shadows / unit shadows / anti-aliasing / shading options): seeds 5 and 77, 699 scenarios identical (101 + structures, 234 + z-buffer, 69 + under construction, 77 + with cargo, aircraft among them), 1 scenario where the original's own draw faulted |
| 1.8 | Shaded path (Shading option), anti-aliased structures | `0x459C70` | BUILT+VERIFIED | `unit_look.cpp` (lit / twice-size structures); not compared. *2026-10-20*: render_harness (the original's whole unit draw `0x45AC20` in a suspended process vs the engine's: random real unit types, poses, cached / visible / shaded piece flags, structure, z-buffer, under construction, cargo, cloak, digger / hover / floater / no shadow, sea level, shadows / unit shadows / anti-aliasing / shading options): seeds 5 and 77, 699 scenarios identical (101 + structures, 234 + z-buffer, 69 + under construction, 77 + with cargo, aircraft among them), 1 scenario where the original's own draw faulted |
| 1.9 | Nanoframes (units under construction) | shaded / depth path | BUILT+VERIFIED | `model.cpp apply_nanoframe`; not compared. *2026-10-20*: render_harness (the original's whole unit draw `0x45AC20` in a suspended process vs the engine's: random real unit types, poses, cached / visible / shaded piece flags, structure, z-buffer, under construction, cargo, cloak, digger / hover / floater / no shadow, sea level, shadows / unit shadows / anti-aliasing / shading options): seeds 5 and 77, 699 scenarios identical (101 + structures, 234 + z-buffer, 69 + under construction, 77 + with cargo, aircraft among them), 1 scenario where the original's own draw faulted |
| 1.10 | Features (2-D, shadows, `shadtrans`/`animtrans`, hidden `nodrawundergray`) | `0x46A610`, `0x4658E0` | BUILT+VERIFIED | re-checked 2026-10-20: the hidden-feature sight test is built (client_scene.cpp, kNoDrawUnderGray against the cell's viewer bits); the row was stale. *2026-10-20*: frame_harness writes the battle's feature types, cells and animation slots: with the fog grid fixed (1.11), 30 fog-on scenes (Moon Quartet, Great Divide, dithered fog) and 35 no-fog scenes identical, the maps' trees, rocks and wrecks included |
| 1.11 | Projectiles: lines, sprite shells, flames, models | `0x49BE60` | BUILT+VERIFIED (render type 2: PARTIAL) | re-checked 2026-10-20: render type 4 picks its shell sequence by the weapon's colour byte (+0x10D, 0..4 → GS+0x147BB..), not by damage — as built; models are drawn as models (types 1, 3, 6). The "plasma sizes by damage" note was wrong. *2026-10-20*: frame_harness `--no-fog --shots 60` (random shots of every stock weapon around the view, all render types but 2): 25 scenes (Moon Quartet 640 x 480, Great Divide 1024 x 768) identical. Render type 2 (the lens, `0x4B9360`; only MINDGUN, which no stock unit carries) is still not drawn: the original copies the 22 x 22 frame rect under the shot unclipped (side panel / bottom bar pixels of the back buffer included), remaps it through the offset table of `0x4B91B0` (radius < 5, source index can leave the capture into the table or the output) and blits it keyed by the frame's +8 byte, which `0x4B91B0` never sets (heap garbage); RE_MEMORY "Rendering". frame_harness fix: the fog grid's cell count (+0xC, the number of cells `0x4843C0` clears before it ORs the edge bits in) was left 0, so the original's grid kept every earlier scene's bits and turned black after a few scenes; with it set, fog-on runs with shots: 30 more scenes (Moon Quartet, Great Divide, dithered fog) identical |
| 1.12 | Lightning shots | `0x49BE60` type 7 | BUILT+VERIFIED | *2026-10-20*: the note was stale — the lightning draws from a copy of the battle's C runtime rand() state and hands the state back (SIM 7.4); compared within 1.11's frame_harness runs (lightning weapons among the random shots, the rand state written into the original) |
| 1.13 | World effects, glows through the light table, shards, debris | `0x420B00`, `0x420D20` | BUILT+VERIFIED | feature_harness / fx_harness, sample frames CPU = GPU. *2026-10-20*: shards were drawn as a model sprite with their angles reversed (the old "approximation of 0x46BAE0"); now as the original: the shard object of `0x420620` / `0x421700` (8 points, 6 faces from `0x502BF8`, no selection primitive) through `0x46BAE0` (points turned by the effect's angles +0x4C as stored, projected around the effect's point straight onto the frame), each face's copied animation cursor frozen at the shattering (the client notes it after the step, before the texture cursors move), team textures in the owner's **player colour** (debris too, `0x4211D0`; both took the player index before). frame_harness `--shards N`: 46 scenes (640 x 480 and 1024 x 768, 4 / 6 players, 40-150 shards each, with and without fog and shots) identical; reversing the angles makes every scene differ |
| 1.14 | LOS fog, black edges | `0x4848E0` | BUILT+VERIFIED | frame_harness |
| 1.15 | Dithered fog | `0x4BFF20`, `0x4B88D0` | BUILT+VERIFIED | frame_harness `--dither` (11/14 identical, 3 harness-limited) |
| 1.16 | Selection outlines | `0x46A530` | BUILT+VERIFIED | frame_harness `--hud` (45 outlines) |
| 1.17 | Damage bars and group digits | `0x46A430` | BUILT+VERIFIED | frame_harness `--hud` |
| 1.18 | Drag box, placement outline | frame, `0x4197D0` | BUILT+VERIFIED | frame_harness `--hud` |
| 1.19 | Shift-held queued orders view | `0x48CC30` | BUILT+VERIFIED | frame_harness `--hud` |
| 1.20 | Camera follow (tracking eases half the distance a step, 320 at most; F3 / `n` / view jumps through the eased target) | `0x41CA10`, `0x41C7C0`, `0x41C8E0` | BUILT+VERIFIED | frame_harness `--camera`: 600 random states, two maps and screen sizes, identical (2026-10-20) |
| 1.21 | Screen shake | `0x41C6F0` (in the camera stage) | BUILT+VERIFIED | the simulation's offsets (flow_harness) added in the camera stage's order (eased, shaken, clamped). *2026-10-20*: frame_harness `--camera` now shakes in half the stages (random shake state and rand() state written into the original; the engine runs the simulation's decay, then the client's camera stage): 4,000 stages (Great Divide 640 x 480, Moon Quartet 1024 x 768) identical, view, target, following, rand() state and shake state |
| 1.22 | Gamma (option slider, `+gamma`) | display palette | BUILT-UNVERIFIED | display palette x factor (min 255, truncated) at presentation only (frames, screenshots, films keep the palette); factor 0.5 + n/24 from the setting at battle load and after the options screens, n x 0.1 from +gamma; as read in 0x4BA200 / 0x45BCC0 / 0x417290, not compared (the original's output goes to DirectDraw) |
| 1.23 | `+contour` lines | `0x418310` → `0x4181D0` / `0x417F60` | BUILT-UNVERIFIED | as read (RE_MEMORY "Console commands, the rest"); smoke: ta_play headless, +contour 1024 on Anteer Strait: lines on the hills, none on flat ground |
| 1.24 | `+light` (light direction for the shaded path) | `0x4166C0` → `0x4597F0`, read by `0x459C70` | BUILT-UNVERIFIED | the lit path's vector (render::LookTables::light, default -0.8, 1, 0.25), n x 0.01 each; the look cache is made again |
| 1.25 | ShowRanges (`+showranges`, GS+0x391BF) | `0x4390A0`, `0x439740`, rings `0x438EA0` | BUILT-UNVERIFIED | the queued-order view's labelled rings (every range of the type, weapon ranges, and at attack targets area of effect / coverage / attack length); smoke screenshot with Shift held |
| 1.26 | Debug grid views (F11 debug mode, GS+0x14280 1–4), frame-rate line | `0x418310` | NOT-APPLICABLE | debug-only (need the "+Now Film…" switch); not player features. The `+profile` bars (`0x46B900`) are built (BUILT-UNVERIFIED, RE_MEMORY "Console commands, the rest") |
| 1.27 | BPS display (`+bps`) | `0x468380` | BUILT+VERIFIED | frame_harness `--hud` bps check: 12 scenes (640 x 480, 1024 x 768), rates none / small / past 5600 / huge, 0 differing pixels; byte totals from NetBattle (messages to and from the transport) |
| 1.28 | Paused sign, victory / defeat signs | frame | BUILT-UNVERIFIED | `hud.cpp`. *2026-10-20*: re-read in `0x468CF0`: igpaused (GS+0x1481B) when GS+0x38A51 bit 0; igvictory / igdefeat (GS+0x14813 / 0x14817) on the end flags 0x20 / 0x40 **only when the local player's side record has no option 0x40 (watching)**, all at ((screen w + 128) / 2, screen h / 2). The client drew the signs for a watching player too; fixed. Not pixel-compared |
| 1.29 | Network "waiting" sign (GS+0x38A51 bit 1 frame at (w − 16, h − 80)) | frame | BUILT+VERIFIED | the step budget's lag throttle (a remote player 900+ steps behind) shows the hourglass pointer's first frame; frame_harness `--hud` waiting check: 5 scenes, two sizes, 0 differing pixels |
| 1.30 | CPU and GPU paths identical | — | BUILT+VERIFIED | `battle_view --gpu`, `--gpu-check` on every client screenshot |

## 2. Battle interface

| # | Item | Original | Status | Evidence / note |
|---|------|----------|--------|-----------------|
| 2.1 | Side panel, top and bottom bar art | frame | BUILT+VERIFIED | frame_harness `--hud` |
| 2.2 | Top bar (amounts, rates, storage marks) | frame | BUILT+VERIFIED | frame_harness `--hud` (17 scenes) |
| 2.3 | Bottom bar (unit / build button / feature info, status line, second line) | `0x46A860` | BUILT+VERIFIED | frame_harness `--hud` (28 scenes) *2026-10-20* (language): the texts the original translates go through the table (`client::tr`): a feature's description (`0x46B869` / `0x46B887`), "kill" / "kills" / "Veteran" (`0x46B2F3` .. `0x46B32C`, "%d %s" / "%d %s - %s"), the status line (`0x46B3C0`), "Weapon" (`0x46B47C`), "Unidentified object" (`0x46B710`) — which the original prints after "S: " (unit flag 0x200) or "R: " ("%s%s"), now drawn so; that prefix is read from the code, not yet in a frame_harness scene (unseen units) |
| 2.4 | RELOAD1–3, TOTALUNITS, TOTALTIME side-data regions | `0x431A60` reads them | NOT-APPLICABLE | read by the side loader; no drawing routine of the original reads them (none found in the frame, bars, Space panels) |
| 2.5 | Orders panel, build menus, page arrows, switches, counts | `0x41B2E0`, `0x41AA00`, `0x4A6AE0` | BUILT+VERIFIED | gui_harness (244 pages, pointer events) |
| 2.6 | Minimap (picture, fog, marks, circles, shots, view box, clicks) | `0x4669B0` … | BUILT+VERIFIED | frame_harness `--minimap`, `--minimap-steps` |
| 2.7 | Console (ring, expiry, drawing, logos) | `0x463CA0`, `0x464060`, `0x463EF0` | BUILT+VERIFIED | frame_harness `--hud` |
| 2.8 | Chat line (TALK.GUI) | `0x494050`, `0x493BF0` | BUILT+VERIFIED | gui_harness `talk` |
| 2.9 | Chat recipient panel (TALK2.GUI, SENDTO) | `0x493AE0` | BUILT-UNVERIFIED | *2026-10-20*: network games open the chat line as TALK.GUI with its SENDTO switch (flags 0x880), or with the switch on as TALK2.GUI (flags 0x800, stock panel): SENDTYPE (All / Allies / Enemies / Custom → GS+0x2BF0), a row per other player (`0x447380(1)`: name, colour logo); SENDTO and SENDTYPE "SmallButton"; the PLAYER switches only change look (the handler looks for LIVEPLYR gadgets the file lacks); a line without a prefix goes to the chosen recipients, a prefix only for that line, and sending clears the switch (`0x493BF0`). Smoke: loopback battle — TALK with SENDTO, TALK2 with Bob / AI:Alice rows, SENDTYPE All → Allies. Not compared with gui_harness |
| 2.10 | Paste into the chat line (Ctrl+V / Insert) | `0x4AB720` | BUILT-UNVERIFIED | Ctrl+V (0xBF) and Insert (0xEE) in the chat line and every menu text field: the line replaced by the clipboard text (max - 1 characters, then shortened to the field width), the insertion point kept; smoke: ta_play headless with a scripted clipboard (`clip TEXT`), pasted line shown, a key typed after it goes in at the kept point |
| 2.11 | Pointer looks, animation, every mode | `0x48D220`, `0x43E490`, `0x4AB5D0` | BUILT+VERIFIED | animation of all 20 numbered looks (frame_harness, 0 differ); the per-unit rules `sim::pointer_look` against `0x43E490` (targeting_harness op 11: 620 looks, 0 differ); the client draws the looks by number |
| 2.12 | Space-held game info panel (Game Time, Total Units / Max, Game Speed) sliding up from the bottom bar | `0x4689C0` | BUILT+VERIFIED | frame_harness `--hud` panels check: 11 scenes, 0 differing pixels *2026-10-20* (language): "Game Time", "Total Units", "Normal", "Game Speed" translated (`0x468B74` .. `0x468C57`); German not compared |
| 2.13 | Space-held (or F4) kills / losses scoreboard sliding in from the left (skirmish, network) | `0x4948E0` | BUILT+VERIFIED | same check (both game types, F4, flashes, ranks with gaps: ranks equal after drawing); the kill ranking and lead message from the battle's kill hook *2026-10-20* (language): "Kills" / "Losses" translated (`0x494AC5` / `0x494B10`), the leader line's format too (`0x486C4B`) |
| 2.14 | F1 unit info screen (UNITINFOX.GUI: picture, name, costs, build time, statistics, speed, acceleration, turn rate, description lines) | `0x4942E0`, `0x489280` | BUILT+VERIFIED | gui_harness `battle unitinfo:` against ta_play frames: 7 types, 0 of 61,750 pixels each *2026-10-20* (language): the eight headings (`0x49445F` .. `0x494651`) and "m/s", "m/s/s", "deg/s", "N/A" (`0x48938F` .. `0x4894CC`) translated |
| 2.15 | Game menu Help / Briefing pages (mission text) | `0x4609B0` | BUILT-UNVERIFIED | Help and Settings verified earlier; *2026-10-20*: in a campaign the Mission button opens BRIEFING.GUI (`0x4609B0` → handler `0x45F770`): at its own place, "igmbrief" in the box, the mission's text pages as the briefing's (`0x476D80`/`0x476EF0`; the blink table is made but nothing draws it here), a click on the text or MOREBAR ("Options") the next page, OK ("Options") closes. Smoke: Arm mission 1 — pages 1 and 2 in the box. Not compared with gui_harness |
| 2.16 | Game clock option | frame | BUILT-UNVERIFIED | *2026-10-20*: the text as `Render_DrawBattleFrame` (`0x46A1D0`) makes it: "%s : %02d:%02d:%02d" of "Game Time" with the game steps split as the information panel does (was "Game Time: hh:mm:ss"); at x 130, y = height − 34 − font height *2026-10-20* (language): "Game Time" translated (`0x46A246`) |
| 2.17 | Selection: click, Shift toggle, double click (same type on screen), drag box | `0x48C7F0`, `0x48C390` | BUILT-UNVERIFIED | sounds checked by sound_harness |
| 2.18 | Pick test (model base rectangle, smallest rank) | `0x48CD80`, `0x48C6A0` | BUILT-UNVERIFIED | |
| 2.19 | Group keys: select (CTRL_F rule: an armed member drops the group's CTRL_F units), Shift adds; the order mode returns to default | `0x48D9A0`, `0x495860` | BUILT-UNVERIFIED | CTRL_F rule and the mode reset built as read |
| 2.20 | Ctrl+digit makes a group | `0x48D920` | BUILT-UNVERIFIED | |
| 2.21 | Digit keys and the SwitchAlt option: plain digit = build page of the focus unit, Alt+digit = group (SwitchAlt swaps) | `0x495E90`, `0x41C060` | BUILT-UNVERIFIED | fixed (was swapped); plain digits are typed characters |
| 2.22 | Ctrl+A (all selectable, mode reset), Ctrl+S, Ctrl+Z, Ctrl+letter categories | `0x48BD50`, `0x48C030`, `0x48BE00`, `0x48BF30` | BUILT-UNVERIFIED | mode reset added |
| 2.23 | Ctrl+C (CTRL_C category, track the last "Commander" category unit) | `0x41C310` | BUILT-UNVERIFIED | |
| 2.24 | Ctrl+D self-destruct: cancels pending ones, else orders it | `0x495E90` case 0xAD | BUILT-UNVERIFIED | smoke: Ctrl+D twice takes the order back, three times gives it again |
| 2.25 | View keys: Ctrl+F5–F8 store, F5–F8 recall (SelectSquad) | `0x41D3B0`, `0x41D3F0` | BUILT-UNVERIFIED | |
| 2.26 | Tracking t / T, n next unit | `0x41C2E0`, `0x48C190`, `0x48D4D0` | BUILT+VERIFIED | following through the camera stage (`--camera`); 'n' eases and keeps the followed unit |
| 2.27 | F3 jump to the next noted unit of the console (marks the line) | `0x464000`, `0x463F60` | BUILT-UNVERIFIED | built as read |
| 2.28 | F4 toggles the scoreboard (visual bit 0x80) | `0x495E90` | BUILT+VERIFIED | part of the panels check |
| 2.29 | F12 clears the console | `0x463C80` | BUILT-UNVERIFIED | built as read |
| 2.30 | `~ ! # * damage bars toggle (saved to the settings) | `0x495E90`, `0x430F00` | BUILT-UNVERIFIED | saved through the front end's settings |
| 2.31 | + / − game speed (not in F11 debug mode, not for watchers) | `0x490DF0` | BUILT-UNVERIFIED | watcher rule added; *2026-10-20*: the console line only when the speed changes (not at 1 or 20 again), "Game Speed Normal" or "%s  %c%d\n" ("Game Speed", '+' above normal and ' ' below, the speed less 10 with its own minus, the newline part of the line; was "Game Speed  -3") *2026-10-20* (language): "Game Speed Normal" / "Game Speed" translated (`0x490E2D` / `0x490E6D`) |
| 2.32 | Pause key (packet 0x19) | `0x495E90` | BUILT-UNVERIFIED | |
| 2.33 | Esc: order mode → default, else deselect; closes the game menu | `0x495E90` | BUILT+VERIFIED | battle_menus_check |
| 2.34 | Enter: chat line (SmallButton) | `0x495E90` | BUILT+VERIFIED | gui_harness `talk` |
| 2.35 | Tab / F2: game menu, network tab bar | `0x495010`, `0x460CC0` | BUILT+VERIFIED | battle_menus_check |
| 2.36 | 'h' opens SHARE.GUI (network games, not watching) | `0x4936F0` | BUILT-UNVERIFIED | the key opens the share screen (2.48) |
| 2.37 | ',' '.' step menus | `0x41BF10`, `0x41BDE0` | BUILT-UNVERIFIED | |
| 2.38 | '\' repeats the last command (debug switch) | `0x495E90` | BUILT-UNVERIFIED | |
| 2.39 | Shift+F1 / Shift+F2 debug unit picks (GS+0x391B3 / 0x391B9) | `0x495E90` | NOT-APPLICABLE | debug values nothing in a normal game reads |
| 2.40 | Ctrl+F10 film (debug switch) and `+film` / `+filmspeed` | `0x495E90`, `0x496790`, `0x4CB170` | BUILT-UNVERIFIED | as read (start / stop, MOVIEnnn numbering, first frame without the console, a frame each 30 / speed ticks, the budget clock restarted); settings saved; smoke: ta_play headless, +filmspeed 15, 60 ticks filmed: 32 frames (FRAM0001..0032); pictures by the verified PCX writer |
| 2.41 | Ctrl+F9 screenshot: `<game dir>\screenshots\SHOTnnnn.pcx` (next number after the highest present), 8-bit RLE PCX with the display palette, in every state of the program | `0x499890`, `0x4CB170`, `0x4CAC40` | BUILT+VERIFIED | writer: frame_harness `--pcx 30`, 30 of 30 identical; folder and numbering as read; smoke: SHOT0001/0002 |
| 2.42 | `+makeposter` | `0x417600` → `0x495A30` | BUILT-UNVERIFIED | size / corner rules as read; tiles of a whole battle frame each (sight and mapping off, sight rebuilt clearing before and after); smoke: BIGSHOT0001.pcx 800 x 600; the tile loop's exact steps not compared |
| 2.43 | Mouse: left/right in interface types 0 and 1, minimap orders, Shift-kept modes | `0x498F70`, `0x499100`, `0x498D00` | BUILT+VERIFIED | gui_harness pointer events, frame_harness |
| 2.44 | Middle drag and Ctrl+right drag scroll | `0x41CC60`, `0x41CD50` | BUILT-UNVERIFIED | re-read: only Ctrl+right over the view in interface type 0 drags (the client's middle-button drag removed: the original has none); pointer hidden, put in the screen's centre, a pass's travel / 4 cells then back to the centre (rest dropped), ends when the right button is up with the pointer back where it was; smoke: ta_play headless, the view moved 7 / 5 then 2 / 2 cells as computed, pointer restored |
| 2.45 | Edge and arrow scrolling (arrows not while the chat line is open) | `0x41CE90` | BUILT-UNVERIFIED | arrows ignored under the chat line |
| 2.46 | Key repeat for every held key | platform | BUILT-UNVERIFIED | every key repeats (menus and battles) |
| 2.47 | Allies screen (ALLIES.GUI) | `0x4478B0` | BUILT-UNVERIFIED | ta_compat `AlliesScreen` (rows, logos, team icons, alliance switches with the answer stage, allied victory); smoke: two headless loopback clients, alliance announced on both; not pixel-compared |
| 2.48 | Share screen (SHARE.GUI: METAL / ENERGY sliders, PLYRLIST, SHARUNIT, MAPINFO) | `0x4936F0`, `0x4934B0` | BUILT-UNVERIFIED | ta_compat `ShareScreen` (sliders, player list, units, explored map); smoke: two loopback clients, metal and energy arrive; not pixel-compared |
| 2.49 | Control screen (CONTROL.GUI) | `0x4466B0` | BUILT-UNVERIFIED | ta_compat `ControlScreen` + `RejectQuestion` (YESORNO); smoke: the host rejects the joiner, both consoles report it; not pixel-compared |
| 2.50 | Time-out dialog of network games (TIMEOUT.GUI: name, console lines, chat, seconds left, Reject Now) | `0x453A50`, `0x453C20`, `0x4538F0` | BUILT-UNVERIFIED | added this round (was not listed); smoke: the joiner gone silent, the dialog after 30 s, the countdown, rejected at 0 |
| 2.50 | One-frame present lag | `0x426E80` / `0x496BB0` | NOT-APPLICABLE | the client presents the frame it drew; the lag of the original's DirectDraw flip is a timing artefact, not a picture difference |

## 3. Sound in battle

| # | Item | Original | Status | Evidence / note |
|---|------|----------|--------|-----------------|
| 3.1 | Sound engine (32-entry table, eviction, duplicates, sweep) | `0x4CF570` … | BUILT+VERIFIED | sound_harness `mixer` |
| 3.2 | World sounds (sight, mono / 3-D) | `0x47F300` | BUILT+VERIFIED | sound_harness `world` |
| 3.3 | Unit notices (queue, priorities, texts) | `0x47F850` … | BUILT+VERIFIED | sound_harness `notices` *2026-10-20* (language): a notice's text is translated when it is queued (`0x47F7C5` / `0x47F89E`) |
| 3.4 | Interface sounds of panels, switches, placement, selection, keys | handlers | BUILT-UNVERIFIED | read from the handlers; ta_play sound log |
| 3.5 | Space panels' Panel / Options sounds | `0x4689C0`, `0x4948E0` | BUILT-UNVERIFIED | Panel / Options as read (headless sound log) |
| 3.6 | Tab bar / Share / Allies sounds (BigButton, Options, Previous, Ally) | `0x494740`, `0x4934B0`, `0x453D40` | BUILT-UNVERIFIED | BigButton / Options / Previous / Ally / SmallButton as read in the handlers (headless sound log) |
| 3.7 | Music in battle (moods, battle / peace judge) | `0x4CDB40`, `0x494E70` | BUILT-UNVERIFIED | |
| 3.8 | Music console commands (CDPlay, CDStop, MusicMode), Sound3D | option table | BUILT-UNVERIFIED | +cdplay n (0: the mood picks; the track already playing goes on), +cdstop, +musicmode n on the front end's music player; smoke: ta_play headless, +cdplay 5 starts track 5. The mode change's cross-fade timer (0x4CE690 with the fade thread) is the player's own mode switch here; Sound3D kept |

## 4. Console commands — client parts

| # | Command | Status | Note |
|---|---------|--------|------|
| 4.1 | +contour | BUILT-UNVERIFIED | see 1.23 |
| 4.2 | +gamma | BUILT-UNVERIFIED | see 1.22 |
| 4.3 | +light | BUILT-UNVERIFIED | see 1.24 |
| 4.4 | +makeposter | BUILT-UNVERIFIED | see 2.42 |
| 4.5 | +film / +filmspeed | BUILT-UNVERIFIED | see 2.40 (+film sets the image output folder, +filmspeed the rate, both saved) |
| 4.6 | +cdplay / +cdstop / +musicmode | BUILT-UNVERIFIED | see 3.8 |
| 4.7 | +showranges | BUILT-UNVERIFIED | see 1.25 |
| 4.8 | +bps | BUILT+VERIFIED | see 1.27 (the command sets the client option) |
| 4.9 | +screenchat, +clock, +noshake, +dither, +shading, +antialias, +shadow, +tshadow, +fshadow | BUILT-UNVERIFIED | switches the client reads (*2026-10-20*: the "noshake waits for 1.21" note was stale: +noshake toggles the battle's shake switch, which the shake add reads as flow_harness checks; 1.21 is built and compared) |
| 4.10 | Settings the original saves after a command (DitheredFog, Shading, …) | BUILT-UNVERIFIED | the commands that call `0x430F00` (scrollspeed, iface, sound3d, shading, antialias, shadow, dither, switchalt toggle, screenchat, gamma, clock, los, mapping, film, filmspeed) write the battle's settings by their registry names |

## 5. Work done this round

(Filled in as items are built; each entry names its evidence.)

*2026-10-20, open-items pass:*
- Shards (1.13): drawn as the original's `0x46BAE0` from the shard object of `0x420620` / `0x421700` (was a model sprite with reversed angles); the copied animation cursor stays at its frame; debris and shard team textures by the player's colour (were by player index). frame_harness `--shards N` (new): 46 scenes identical.
- frame_harness: fog grid cell count set (the original's view went black after a few fog-on scenes); render type 2 shots left out (the lens frame needs the battle-start routine); `--camera` shakes. Shots of every other render type (1.11, 1.12), features (1.10), screen shake (1.21) now compared: identical.
- render_harness run: 699 unit-draw scenarios identical (1.4, 1.6-1.9).
- Victory / defeat signs not drawn for a watching local player (1.28, `0x468CF0`).
- Stale notes corrected: 1.12 (lightning rand), 4.9 (+noshake), the summary below.

## 6. Summary

Counts after the open-items pass (*2026-10-20*): 99 items — BUILT+VERIFIED 46, BUILT-UNVERIFIED 49, PARTIAL 0 (1.11 keeps one
PARTIAL part: render type 2, the lens), MISSING 0, NOT-APPLICABLE 4 (earlier this round: 36 / 52 / 6 / 1 / 4; start: 26 / 17 /
18 / 33 / 4 of 98). The previous summary's open list was stale: TALK2.GUI (2.9) and the briefing page (2.15) are built (not
pixel-compared), air shadows, depth-plane clipping and hidden features are built and now compared (1.6, 1.7, 1.10), and the
"plasma sizes" item was a misreading (1.11). Left open: the lens shot (1.11, MINDGUN only; its transparent index is heap
garbage); the BUILT-UNVERIFIED interface keys, panels and sounds (2.9-2.50, 3.4-3.8, 4.x), which have no harness against the
original yet; the music mode change's cross-fade (3.8).
