macOS build mappings
====================

The harness names game code as the original Windows project does: by the
RVAs of the game's Windows Steam build in `src/dll/game.h`, and by the
serialiser sites in `sites.h` and `site_members.c`. A mapping translates
them for one macOS build of the game. See
[Build tables](../../docs/MACOS.md#build-tables) in the macOS guide for how
the harness uses it.

| File | Purpose |
| --- | --- |
| `steam-24.4.2.json` | The mapping for the Steam build 24.4.2 (2081827), both slices |
| `gen_tables.py` | Generates `src/dll/game_macos_tables.c` from a mapping |
| `check_mapping.py` | Checks a mapping against the game executable |

```sh
python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c
python3 -I tools/macos/check_mapping.py tools/macos/steam-24.4.2.json
```

`check_mapping.py` reads the game in the default Steam library,
`~/Library/Application Support/Steam`, unless given the path of an `fm.app`
or its executable after the mapping. Unlike `fm24ctl`, it does not search
Steam's other library folders, so a game installed in another library needs
that path. It checks each slice's UUID, that every function is a function
start, that vtables and globals (the RVAs and IDs that `src/dll/game.h`
declares in `GameData` and `MacosGameData`) lie in data, that the signature
bytes match, and that every frame address is a return address (it follows a
call). It cannot check that an entry means the right thing; the harness's
tests and a run against a config exported with the original Windows project
do that.

A new build needs a new mapping, a `macos_builds.c` entry for each slice's
UUID, and regenerated tables. The layouts in `src/dll/game.h` may change too.

## Format

All macOS addresses are unslid (the image header is at `0x100000000`);
Windows values are RVAs of the game's Windows Steam build.

```json
{
  "build": "Football Manager 2024 24.4.2 (2081827), Steam",
  "slices": {
    "arm64": {
      "uuid": "D710F6FA-6ACA-32BB-99FE-451CEE8DFA3D",
      "addresses": [{"name": "RVA_GAME_MATCH_CREATE", "windows": "0x1A8FAF0", "mac": "0x104493a38"}],
      "signatures": [{"name": "RVA_GAME_MATCH_CREATE", "mac": "0x104493a38", "bytes": "fd7bbfa9..."}],
      "frames": [{"mac": "0x10429b778", "win": ["0x1939ED4"], "note": "..."}]
    },
    "x86_64": {"uuid": "...", "addresses": [], "signatures": [], "frames": []}
  }
}
```

- `addresses`: the macOS counterpart of each RVA the harness calls or reads,
  and of the macOS-only IDs (`RVA_MACOS_*` in `game.h`: code in
  `MacosGameFunction`, data in `MacosGameData`). The generator rejects two
  entries for one RVA, and a mapping without one the harness uses.
- `signatures`: up to 32 bytes at the start of each function the harness
  calls. The harness compares them with the loaded game before serving jobs.
- `frames`: return addresses in the macOS serialisers and the Windows return
  addresses (up to four) they stand for. An entry with a `parent` applies
  only when the next frame up the stack is that return address; the
  translation prefers it to the entry without one. An empty `win` list drops
  the frame. `note` is for people.

## How the 24.4.2 mapping was made

The game's Windows and macOS builds come from the same source, so the same
functions make the same calls in the same order, with three differences:
clang and MSVC inline different calls, clang turns more calls into tail
jumps, and the Itanium ABI lays out vtables and objects differently.

- **Functions, vtables and globals.** Each Windows reference function in
  `src/runner/probe_patterns.c` was located in the arm64 and x86_64 slices
  by the Itanium RTTI of its class (for example the vtable of
  `GAME_MATCH`), the strings and constants it uses, and its callers and
  callees. The two slices were matched with each other function by
  function, so each address is confirmed in two independent compilations.
- **Frames.** For each serialiser on the Windows sites' call paths, the
  ordered calls of the Windows function were aligned with those of its macOS
  counterpart: stream writes of the same width in the same order, the same
  helper calls and virtual slots (the macOS slot is usually one higher: the
  Itanium ABI has two destructor entries). A call clang inlined gives one
  macOS frame several Windows frames; a helper MSVC inlined, or a call it
  made through a jump, has no macOS frame of its own. A macOS frame without
  a Windows counterpart keeps its own identity in the trace.
- **Layouts.** The macOS offsets in `game.h` were read from the accessors and
  serialisers that use each field, in both slices.
- **The game's state.** The `MacosGameData` globals, which the harness only
  reads (`src/dll/game_state.c`), were found from the code that uses them
  and checked in the disassembly of both slices of the installed game. The
  `GAME` and `SAVE_SYSTEM` pointers are what their getters load (arm64
  `0x101aaf0b0` and `0x10007057c`, x86_64 `0x101c31fe0` and `0x10006ed30`).
  The game options object is the one whose `+0x5d` the core loader sets
  before it posts `LGOK` (arm64 `0x101af97d8`, x86_64 `0x101c7ec84`), and
  that `end_processing` reads first. The start screen's pointer is the one
  its first entry stores once the auto setup has returned (arm64
  `0x100c44698`, x86_64 `0x100cbe4fe`); the only other store to it, a null,
  is in the application's shutdown (arm64 `0x100221690`, x86_64
  `0x10022f12e`). The `GAME` offsets come from
  `start_processing` and `end_processing` (`+0xb8`, `+0xbc`) and the
  auto-play reader (`+0x64`, `+0x80`; arm64 `0x101abfd74`, x86_64
  `0x101c439e0`), and the save name's `+0x28` from the loads that set it
  (load from path: arm64 `0x101b08b10`, x86_64 `0x101c8dc40`). None of
  them has been read in a running game yet.

Every site and member in `sites.h` and `site_members.c` was checked for
frames in both slices; the `note` fields record the exceptions (the staff
block's legacy branch, which the macOS build places at the end of its
function, and match officials whose writes MSVC inlined into one site).
`gen_tables.py` refuses a mapping that leaves out an RVA the harness uses or
a write site in `sites.h`; the tests check the same for the generated
tables, and that they are sorted; and the harness refuses tables that lack
an address, or a signature for a function, it needs. Confirming the labels
on a live game, against a config exported with the original Windows project,
is the check that remains open for each new mapping. For 24.4.2 it has not
been done: the labels have only been compared, offline, with a Windows
export of a different fixture, and no config exported with the original
Windows project has been played with this version, or the reverse. Moving
fixtures between the two is unchecked.
