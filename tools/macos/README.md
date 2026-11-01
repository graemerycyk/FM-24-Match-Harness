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
| `steam-24.4.2.json` | The mapping for FM24's Steam build 24.4.2 (2081827), both slices |
| `steam-27.0.0.2486716.json` | The mapping for FM27's Steam build 27.0.0.2486716, the free preview, both slices of its engine plugin |
| `gen_tables.py` | Generates an edition's tables (`src/dll/game_macos_tables.c` for FM24, `game_macos_tables_fm27.c` and its synthetic sites, `sites_fm27.h`, for FM27) from its mappings |
| `check_mapping.py` | Checks a mapping against the game's image |
| `remap.py` | Carries an FM27 mapping over to a new build of the game, through the mapped build's fingerprints, and makes those fingerprints (see [Mapping a new FM27 build](#mapping-a-new-fm27-build)) |
| `align/` | Builds an FM27 mapping's serialiser frames from FM24's (`frames.py`, with its alignment file `align/steam-27.0.0.2486716.json`); each mapped FM27 build's fingerprints (`fingerprints-<version>.json`, made by `fingerprint.py` for `remap.py`); and the tests of these and of `gen_tables.py` |

```sh
python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c
python3 -I tools/macos/check_mapping.py tools/macos/steam-24.4.2.json
python3 -I tools/macos/gen_tables.py tools/macos/steam-27.*.json src/dll/game_macos_tables_fm27.c
python3 -I tools/macos/check_mapping.py tools/macos/steam-27.0.0.2486716.json
python3 -I tools/macos/align/frames.py tools/macos/align/steam-27.0.0.2486716.json \
    <FM24's fm.app or its fm> <FM27's fm.app or its game_plugin> [--write]
python3 -I tools/macos/remap.py --fingerprint tools/macos/steam-27.0.0.2486716.json \
    <FM27's fm.app or its game_plugin> [--write tools/macos/align/fingerprints-27.0.0.2486716.json]
python3 -I tools/macos/remap.py tools/macos/steam-27.0.0.2486716.json \
    tools/macos/align/fingerprints-27.0.0.2486716.json <the new build's fm.app> [--out <mapping>]
python3 -I -m unittest discover -s tools/macos/align -p 'test_*.py'
```

`check_mapping.py` reads the game in the default Steam library,
`~/Library/Application Support/Steam`, unless given the path of an `fm.app`
or of the image after the mapping. The image is the app's executable `fm`
for FM24, and for FM27 its engine plugin,
`Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin` (the
mapping's `image`). Unlike `fm24ctl`, it does not search Steam's other
library folders, so a game installed in another library needs that path. It
checks each slice's UUID, that every function is a function start, that
vtables and globals (the RVAs and IDs that `src/dll/game.h` declares in
`GameData`, `MacosGameData` and `PluginGameData`) lie in data, that the
signature bytes match, that every frame address (and its parent and
grandparent) is a return address (it follows a call), that the evidence of
each vtable and global still shows its address, and for a mapping with
`layouts` (below), that every vtable and global has evidence and each
layout's evidence still shows its value. It cannot check that an entry
means the right thing; the harness's tests and a run against a config
exported with the original Windows project do that.

A new FM24 build would need a new mapping, a `macos_builds.c` entry for each
slice's UUID, and regenerated tables; the layouts in `src/dll/game.h` may
change too. FM27's harness carries the tables of every FM27 build that has
a mapping, and uses those of the build it finds (by the slice's UUID), so a
new FM27 build adds a mapping and `macos_builds.c` entries, and the tables
are generated from all the mappings together. Its frames keep the
synthetic IDs the mappings give FM27's own writes the same across builds:
`gen_tables.py` refuses mappings that give a named one different IDs. Its layouts are in
`src/dll/game_layout_fm27.h`, which every build's mapping must match:
`gen_tables.py` refuses one that differs, so a build whose objects moved
needs the harness to carry layouts per build first. `remap.py` makes a new
FM27 build's mapping from the last one (see
[Mapping a new FM27 build](#mapping-a-new-fm27-build)).

## Format

All macOS addresses are unslid: FM24's image header is at `0x100000000`, and
FM27's engine plugin's at `0` (its `image_base`). Windows values are RVAs of
the game's Windows Steam build.

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
  translation prefers it to the entry without one. In an edition whose
  engine is a plugin, an entry with a `parent` may also have a
  `grandparent`, the frame after the parent, and is preferred again. An
  empty `win` list drops the frame. `note` is for people.

### FM27's mappings

An FM27 mapping has these keys as well:

```json
{
  "edition": 27,
  "image": "Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin",
  "image_base": "0x0",
  "version_text": "27.0.0.2486716",
  "game_build": 2486716,
  "format_version": "0x004c",
  "absent": [{"name": "RVA_ALLOC_FROM_POOL", "reason": "..."},
             {"name": "SITE_TEAM_STAFF_1", "reason": "..."},
             {"site": "0x1A84A14", "member": "MATCH_STAFF+0x1ac", "reason": "..."}],
  "sites": [{"name": "SITE27_TACTIC_SLOT_IP", "id": "0x7E000010", "count": 11, "note": "..."},
            {"id": "0x7E001000", "slice": "arm64", "mac": "0x4a4da00", "note": "..."}],
  "notes": {"RVA_MACOS_START_SCREEN": "..."},
  "slices": {
    "arm64": {"addresses": [
      {"name": "RVA_TEAM_VTABLE", "windows": "0x5A78848", "mac": "0x55c2e40",
       "evidence": [{"rtti": "N2db4TEAME", "offset_to_top": "0x0"}]},
      {"name": "RVA_MACOS_MATCH_CONTEXT", "windows": "0x7F000008", "mac": "0x5721a08",
       "evidence": [{"at": "0x2941768"}, {"at": "0x2e81cb4"}]}, ...], ...}, ...},
  "layouts": [
    {"name": "MATCH_CLOCK", "value": "0x154dc", "fm24": "0x9d8", "confidence": "high",
     "relation": "sum", "note": "...",
     "evidence": {"arm64": [{"at": "0x448a08c", "imm": "0x15000"}, ...],
                  "x86_64": [{"at": "0x498adf9", "imm": "0x154dc"}]}}
  ]
}
```

- `edition`, `image` and `image_base`: the edition (FM24's mapping has
  none), the engine's image in the app, and the unslid address of its
  header.
- `version_text`, `game_build` and `format_version`: the app's version, the
  build number configs record as their `game_build` (the last part of the
  version), and the match stream's format version, which the generator
  puts in the build's tables: the harness reads a serialised match's seed
  only if its header has that version.
- `absent`: IDs of `game.h` and sites of `sites.h` the build has no
  counterpart for, each with its reason; the generator refuses a mapping
  that leaves one out without listing it here, or lists one it has. An
  entry with a `site` (and its `member`) is a member of `site_members.c` or
  another site the labels use, which `align/frames.py` checks. `notes`
  explains entries that need it.
- `sites`: the synthetic IDs (0x7E000000 to 0x7EFFFFFF) that the frames give
  the build's writes and calls FM24 has no counterpart for. A named one (or
  a range of `count`, named `_FIRST` and `_LAST`) is one the labels use,
  which the generator writes into `src/dll/sites_fm27.h`; the others are
  the IDs `align/frames.py` gave the frames it found no translation for,
  each with its `slice` and frame (`mac`). The generator refuses frames
  with an ID the list does not give.
- `sites_pending`: present while the serialiser frames, and with them the
  labels, are not mapped yet; the generator then leaves out the check that
  every write site has a frame.
- `evidence` of the vtables and globals in `addresses`, which every one has:
  for a vtable, its class's RTTI name and the offset to top, which
  `check_mapping.py` reads before the address point; for a global, the
  instructions (`at`) that reach its address, as for layouts below.
  `remap.py` finds the instructions again in a new build.
- `layouts`: every value in `src/dll/game_layout_fm27.h`, with FM24's
  (`fm24`), how sure it is (`high`, `medium`, `unverified` for a value
  carried over unchecked, or `unresolved` for one not located, which the
  header leaves out) and the instructions that show it in each slice. An
  evidence entry is an instruction (`at`) that encodes an immediate (`imm`;
  an offset subtracted is negative) or reaches an address (`target`, through
  an `adrp` on arm64 or RIP-relative on x86_64), or a vtable slot
  (`vtable`, `slot_offset`) that points to a function (`points_to`). By
  default each of them shows the value itself; `relation` says otherwise:
  `sum` (the slice's immediates add up to it), `target-minus:<ID>` (the
  address less that ID's), `minus:<n>`, `plus:<n>` (for a member inside a
  block a serialiser writes whole, from the block's start), `slot-index` or
  `slot-offset`.
  `check_mapping.py` decodes each one again, so a remapped build can be
  checked the same way.

## Mapping a new FM27 build

Steam updates FM27 in place, and the harness refuses a build it has no
tables for. `remap.py` finds what a mapping names in the new build, using
the fingerprints of the mapped build, so the old build's binary is not
needed once they are made. Each mapped build keeps its fingerprints in
`align/fingerprints-<version>.json`: for every function the mapping uses
(its addresses and signatures, the functions holding its frames and its
layouts' evidence, and the smallest functions that reach its globals), the
function's size and the sizes of the eight functions on each side, a hash
of its instructions with their registers, immediates and addresses masked,
a sketch of their 4-grams (64 hashes, for how alike two versions are), its
calls and stream writes in order (with each callee's size and the masked
hash of the instructions before each call), the hashes of the strings it
uses and the vtable slots that point to it; for every frame, its function
and the index of its call; for every instruction the mapping relies on, its
function, its index and the masked hashes of the instructions around it;
each vtable by its class's RTTI name; and the instruction that reaches the
format version's constant. They hold no code: about 0.7 MB for both slices.

To map a new build, before the game updates again:

1. Copy the new build's `game_plugin` (and the app's `Info.plist`) out of
   the app, outside the repository: the next update replaces it.
2. Remap the newest mapping onto it. The version comes from the app's
   `Info.plist`, or `--version` for the plugin on its own, and the new
   mapping goes to `tools/macos/steam-<version>.json` unless `--out` says
   otherwise:

   ```sh
   python3 -I tools/macos/remap.py tools/macos/steam-27.0.0.2486716.json \
       tools/macos/align/fingerprints-27.0.0.2486716.json \
       "$HOME/Library/Application Support/Steam/steamapps/common/Football Manager 27/fm.app"
   ```

   It matches, for each slice: vtables by their classes' RTTI names;
   functions by the vtable slots that point to them, by the sizes of the
   functions around them, through the aligned calls of functions already
   matched, and by the strings they use, each checked against its
   fingerprint (the same masked code, or one at least 60% alike); globals
   through the instructions that reach them; frames by aligning each
   function's calls and writes with its fingerprint's; each layout by
   reading its evidence instruction again; the signatures from the new
   build; and the format version from the constant that the match header's
   writer writes.
3. Read the report it prints. Everything not matched one to one is under
   "needs review", and the exit code is then 1: functions not found, or
   found but changed (with how alike they are and what the mapping uses
   them for), frames, sites and addresses left out, layouts whose value
   changed, and a format version that changed. A changed function's frames
   and evidence were placed by alignment and need checking by hand, as in
   [How the 27.0.0.2486716 frames were made](#how-the-27002486716-frames-were-made);
   a changed layout needs `src/dll/game_layout_fm27.h` to carry layouts per
   build first.
4. Check the new mapping against the new build, add its slices' UUIDs and
   version to `src/common/macos_builds.c`, regenerate the tables from every
   mapping and run the tests:

   ```sh
   python3 -I tools/macos/check_mapping.py tools/macos/steam-<version>.json <the new fm.app>
   python3 -I tools/macos/gen_tables.py tools/macos/steam-27.*.json src/dll/game_macos_tables_fm27.c
   ```

5. Make the new build's fingerprints from the reviewed mapping, so the next
   build can be remapped from it:

   ```sh
   python3 -I tools/macos/remap.py --fingerprint tools/macos/steam-<version>.json <the new fm.app> \
       --write tools/macos/align/fingerprints-<version>.json
   ```

   Without `--write`, it checks that the build's fingerprints file is what
   it would write.
6. Run the first checks with the game on the new build
   ([checks](../../docs/MACOS-FM27.md#checks-with-the-game) 1, 4 and 6),
   then release the harness. Older builds keep their tables, so one harness
   supports every mapped build.

Remapping a mapping onto its own build gives the mapping back, which the
tests check for 27.0.0.2486716 when `FM27_GAME_PLUGIN` names its archived
plugin. The tests also build two versions of a small bundle with clang,
the second with code inserted, fields and a global moved, a vtable slot
added, a serialiser's write added and the format version changed, and
check that `remap.py` finds everything in the second and reports what
changed. A real update has not been remapped yet: the first will be the
full release of 10 November 2026.

## How the 27.0.0.2486716 mapping was made

FM27's match engine and database live in `game_plugin`, a bundle the
Unity-based game loads once it has started. It comes from the same code
base as FM24, built by the same compiler, so its functions were found by
comparing it with FM24's macOS build rather than with Windows:

- **Functions, vtables and globals:** by the RTTI of their classes, the
  strings and constants they use, their callers and callees, and
  instruction-by-instruction comparison with their FM24 counterparts, in
  the arm64 slice first and then the x86_64 slice. `ALLOC_FROM_POOL` is
  inlined everywhere, and the `GAME_MATCH` factory (`RVA_MACOS_MATCH_FACTORY`)
  makes a match in one call instead. The serialisation context
  (`RVA_MACOS_MATCH_CONTEXT`) is the global that create, serialise and
  deserialise now take as an argument. The main menu moved into Unity, so
  there is no start screen: `RVA_MACOS_START_SCREEN` is mapped to the `GAME`
  singleton, which the plugin's startup creates, as the signal that the
  game has started. The startup creates it before it reads the auto-play
  file and loads the save, so it does not mark a load's end: the harness
  waits for a game in progress to settle instead (see
  [the game's state](../../docs/TECHNICAL-FM27.md#the-games-state)).
- **Layouts:** mostly from the getters in the classes' vtables, aligned slot
  by slot with FM24's (a getter that read FM24's offset sits in the same
  place and reads FM27's), and from the database readers, which store each
  field after reading its four-letter tag (`Pdob`, `Pnti` and so on). The
  player record changed most: its ratings moved before its other values, and
  it holds a body type (1-5) where FM24's held a weight. The match builder
  now gives a player the weight `BMI(body type) * height^2` (with BMIs 19.0,
  20.5, 23.5, 26.5 and 29.5), which the harness copies
  (`game_playerWeight`).
- **Not located:** the `GAME` processing flag and kind, the loaded save's
  name and the clubs' short names (`GAME_UNLOCATED_*` in the layout header):
  the code that used them in FM24 moved into Unity or changed. The harness
  does without them: the game is never read as busy, `status` reports no
  save, and teams are found by unique ID only. The manager flags' values are
  FM24's; the field is found, but the values have not been checked.
- **The match calls:** read from the game's own callers. Create takes no
  argument and makes a `GAME_MATCH` through the factory the game registers
  (a plain match without one); serialise and deserialise take the
  serialisation context, read from its global, after the stream; the
  builder prepares the teams, makes the match with the factory and passes
  `InitArgs` of 0x38 bytes (the fixture, the scratch block, the seed at
  +0x20 and the mode at +0x24) and a 16-byte flags block. The builder's
  clock seed is unchanged in kind: `mach_absolute_time` in milliseconds at
  the timebase it works out once, folded into 32 bits. A serialised match
  starts with the game's version, the format version, a zero, a tag (0x02)
  and a u32, then the label, a u32 and the seed: 20 bytes plus the label's
  length before the seed.
- **The detailed statistics** (`MatchStatsLayout`): the result object, the
  players and their statistics objects, the minutes and the person, from the
  routine that adds up a played match's minutes and ratings, which matches
  FM24's instruction for instruction in both slices; the statistics
  themselves from `MATCH_PLAYER_STATS`'s serialiser, which writes the same
  blocks as FM24's in the same order, 4 to 22 bytes further on (the
  floating-point values before `+0x60`, 20 bytes lower), and the counts of
  the game's own reads of each member agree. They are `medium`: the sprints count, which
  FM24 writes on its own and FM27 with the block before it, is the least
  sure.

None of the mapping has been checked with the game running.

## How the 27.0.0.2486716 frames were made

`align/frames.py` builds both slices' frames, and rebuilds them
identically from the same inputs (without `--write` it checks that the
mapping holds what it builds). It reads the archived FM24 arm64 slice, the
FM27 plugin and the alignment file, which lists:

- `roots`: the serialisers to start from in FM24's arm64 slice and in
  each FM27 slice: `GAME_MATCH::serialize` and the serialisers it reaches
  only through virtual calls (the officials', a player's, the managers',
  the database helper's and the set-piece routines'), found by their
  classes' vtables;
- `pairs`: FM24 and FM27 functions that the tool cannot pair itself;
- `fixes`: frames set by hand, each with its reason.

For each root, both builds' serialisers are flattened: every stream write
and virtual call reached through direct calls and tail jumps, with the
return addresses on the stack (`align/derive.py`, from `llvm-objdump`'s
disassembly). The two sequences are aligned by kind and write length; the
FM24 stack of each aligned write translates through FM24's frames into a
chain of Windows frames, and that chain is split over the FM27 stack's
frames: functions are paired by the aligned stacks they share, a frame
of an FM27 function carries the part of its FM24 counterpart, and the
helpers FM27 calls where FM24 wrote directly carry what tells their
callers apart, with a parent or grandparent where one frame stands for
several (FM27 writes each end of the pitch through a helper, which writes
each point through another, which writes each coordinate, where FM24's
pitch serialiser wrote all 39 coordinates itself). Aligned writes
whose stacks call functions paired with others are left out. The result
is replayed: every aligned write's FM27 stack must translate into its
FM24 chain, unless a fix changes it. The x86_64 slice's frames are made
the same way from the arm64 slice's.

The fixes cover what changed:

- the match has a new part at `+0x6f0` (`SITE27_PART_EXTRA_1776`), whose
  objects the game writes through virtual calls the mapping does not
  follow;
- the team's optional staff member is the analyst, FM24's `staff[2]`:
  FM27's team has no doctor (FM24's `staff[1]`);
- a tactic has three plans, and a plan writes each formation slot in two
  halves, from arrays at `+0x120` and `+0x440` (presumably in and out of
  possession), each with a position, player instructions and a role
  written last, and a byte for each slot (`SITE27_TACTIC_SLOT_*`); the
  halves' fields keep FM24's slot sites;
- the familiarity's six setting categories are separate calls, not FM24's
  loop, and its entries' count is written by their list's serialiser.

Every other frame on an FM27 write path that gets no translation is given
a synthetic ID of its own, kept in `sites` with the frame, so no FM27 path
holds an untranslated frame, and every frame that gives a label site lies
on a traced path. Of the 1,180 sites and members FM24's labels use, the
frames give 1,061 in both slices; the other 119 are listed in `absent`.
The labels for FM27's own structure (its new part, plans, slot halves and
familiarity) are in `src/dll/labels.c`; [Editing an FM27 fixture
file](../../docs/FIXTURE-FM27.md) lists them. Aligned writes of the same
length in the same order are not proof of the same field: each label
still needs checking against an export and the game.

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
