# Thaldren Engine

A from-scratch reimplementation of the Total Annihilation 3.1 game engine, written in C++20.

The goal is a 1:1 match with the original program:

- **Simulation:** identical tick for tick, including its fixed-point and x87 floating-point
  arithmetic, its random number streams and its quirks.
- **Client:** menus, rendering, sound and music that look and behave like the original's.
- **Multiplayer:** interoperable with real Total Annihilation 3.1 clients over TCP/IP, IPX,
  serial and modem, by speaking DirectPlay's wire protocol in the engine's own code.

> **Not affiliated with or endorsed by** Cavedog Entertainment, GT Interactive, Atari or
> Wargaming. *Total Annihilation* is a trademark of its owner. This repository contains **no
> game data**: to play, you need your own installed copy of Total Annihilation 3.1 (the Steam
> release works).

## Status

Every routine of the original program has been accounted for: ported, or recorded as not
applicable (C runtime, DirectPlay, developer-only runtime internals, code the original never
runs). See [`docs/parity/COVERAGE.md`](docs/parity/COVERAGE.md) for the full sweep.

The parity documents track each feature against the original and how it was verified:

| Area | Document |
|---|---|
| Simulation | [`docs/parity/SIM.md`](docs/parity/SIM.md) |
| Battle client (interface, rendering, sound) | [`docs/parity/CLIENT.md`](docs/parity/CLIENT.md) |
| Menus and program flow | [`docs/parity/FRONTEND.md`](docs/parity/FRONTEND.md) |
| Multiplayer | [`docs/parity/NETWORK.md`](docs/parity/NETWORK.md) |
| Floating-point fidelity | [`docs/parity/NUMERICS_FLOAT.md`](docs/parity/NUMERICS_FLOAT.md) |
| Fixed-point and integer fidelity | [`docs/parity/NUMERICS_FIXED.md`](docs/parity/NUMERICS_FIXED.md) |

Rows marked BUILT+VERIFIED were compared against the original program (recorded games replayed
step by step, pixel comparisons, or the original's own routines run side by side).
BUILT-UNVERIFIED rows are implemented from the original's code but not yet compared.

Only Windows x64 is built and tested so far. The code is portable C++20 with SDL3, and Linux
and macOS are intended targets.

## Playing

1. Download the release zip and unpack it.
2. Either copy `ta_play.exe` into your Total Annihilation folder (the one containing
   `TotalA.exe`) and run it there, or edit `run.bat` so `TA_DIR` points at that folder and run
   `run.bat`.

Useful options:

| Option | Effect |
|---|---|
| `--cpu` | Use the software renderer. |
| `--no-sound` | No sound or music. |
| `--direct --map "Moon Quartet"` | Skip the menus and start a skirmish. |

The original's own command-line switches (`-d`, `-s`, `-w`, `-t`, a language word, and so on)
work as they do in `TotalA.exe`.

## Building

Requirements:

- CMake 3.24 or later.
- A C++20 compiler. The project is developed with MSVC (Visual Studio 2026, toolset 14.51) on
  Windows x64.
- The SDL 3.4.18 source tree, for the window, sound and GPU renderer. It is built statically.

```
git clone --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL.git SDL3
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_BUILD_TYPE=Release" "-DSDL3_SOURCE_DIR=SDL3"
cmake --build build
```

Run these from a Visual Studio x64 developer prompt. Any CMake generator works; NMake is what
the project uses. Without `SDL3_SOURCE_DIR` the simulation, tools and tests still build, but not
the playable client.

The client is `build/client/ta_play.exe`.

## Tests

```
ctest --test-dir build
```

- `sim_tests`, `net_tests`, `compat_tests` and `render_tests` need nothing else.
- With `-DTA_GAME_DIR=<your Total Annihilation folder>`, more tests check the importers against
  every archive, unit, weapon, model and feature of your installed game.
- `game_flow` and `game_replay` compare the engine with recordings of the original program
  (`ta_compat/tests/fixtures/golden_skirmish_moonquartet_300.jsonl`, and whole-game captures
  made with `ta_compat/tools/capture`).
- `ta_compat/tools/orig_harness` holds 32-bit harnesses that load your own `TotalA.exe` and run
  its routines next to the engine's. They need a 32-bit build.

## Making a release

Releases are built by GitHub Actions (`.github/workflows/release.yml`):

1. Optionally write the release text in `docs/releases/<tag>.md` (for example
   `docs/releases/v0.2.0.md`) and push it. Without it, GitHub generates notes from the commits.
2. Tag the commit and push the tag: `git tag -a v0.2.0 -m "Thaldren Engine v0.2.0"`, then
   `git push origin v0.2.0`.

The workflow builds the client, runs the tests, packages
`thaldren-engine-<tag>-windows-x64.zip` with `tools/package_release.ps1` and publishes it with
its SHA-256 as a GitHub Release. To build the release of a tag that already exists, run the
workflow by hand (Actions → release → Run workflow) and enter the tag.

## Repository layout

| Folder | Contents |
|---|---|
| `engine/sim` | The simulation. Portable C++, no game-specific names or file formats. |
| `engine/net` | Lockstep networking and the DirectPlay-compatible session and wire protocol. |
| `render` | The 8-bit palettised renderer and the optional SDL3 GPU backend. |
| `client` | Window, input, interface, sound and music. |
| `ta_compat` | Readers for the original's file formats (HPI, TDF, FBI, COB, 3DO, GAF, GUI, saves…), the original's screens, the `ta_play` program, and the comparison tools. |
| `tests` | Unit tests. |
| `tools` | Scripts: table generators, menu checks, packaging. |
| `docs` | The reverse-engineering notes and the parity documents. |

## How it was made

The original program was studied with a disassembler and by running its routines in test
harnesses. Each finding is written down as a specification in our own words in
[`docs/RE_MEMORY.md`](docs/RE_MEMORY.md), with the original's addresses for reference. The
engine is written from those specifications; no decompiled code is copied into it.

Some test fixtures were recorded from the original program running with the developer's own
copy of the game. They are recordings for comparison, not game assets.

## License

The engine is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE.md). You may
use, copy, modify and share it for any noncommercial purpose, provided every copy keeps the
license terms and this line:

```
Required Notice: Copyright (c) 2026 Thaldren
```

Commercial use is not permitted.

Third-party components and their licenses are listed in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
