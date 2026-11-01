FM27 match-engine harness for macOS: technical notes
===

These notes describe how the FM27 edition differs inside from the FM24
version, which the [technical notes](TECHNICAL.md) describe: the same
sources, built with `FM_EDITION=27` (see [Editions](DEVELOPMENT.md#editions)),
for Football Manager 27 from Steam. For setting it up and using it, see the
[README](../README.md) and the [FM27 guide](MACOS-FM27.md). Nothing here
has been checked with the game running: it rests on reading the game's
code and on tests with stand-ins.

The job, match, fixture and seed-building code is the original Windows
project's,
[PhilipArmstead/FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead, as in the FM24 version. FM27's differences are in
blocks under `EDITION_ENGINE_PLUGIN` and `EDITION_TACTIC_PHASES`
(`edition.h`), and in the edition's own files:

```
src/common/  info_plist (an app's names, from its Info.plist), macos_builds
             (FM27's builds, under FM_EDITION 27)
src/dll/     game_layout_fm27.h (FM27's object layouts, and what has not been
               located), game_macos_tables_fm27.c and sites_fm27.h
               (generated from the mappings)
tests/       macos_stand_in_plugin, macos_stand_in_harness and
               macos_stand_in_offsets.cmake (a stand-in engine plugin, and
               the harness built with its tables)
tools/macos/ steam-27.<build>.json (one mapping per build), remap.py (maps a
               new build from the last), align/ (FM27's frames from FM24's,
               and each build's fingerprints-<version>.json)
```

# Loading the harness

As for FM24, Steam runs the launch option, and the launcher sets
`DYLD_INSERT_LIBRARIES` and starts the game's executable through
`/usr/bin/arch`. FM27's executable is the one the app's `Info.plist` names
(`CFBundleExecutable`, `Football Manager 27` in the preview), which the
launcher reads with `plutil` and `fm27ctl` and the harness with
`src/common/info_plist.c`, in an app whose `CFBundleIdentifier` is
`com.SportsInteractive.FootballManager27`. The executable is Unity's; it
is entitled to accept `DYLD_INSERT_LIBRARIES` and libraries signed by
others, as FM24's is.

# The harness

The dylib's initialiser runs in the game's process (its executable, by the
name above) and registers dyld's add-image and remove-image callbacks, then
starts the job thread. The add-image callback runs under dyld's lock, so it
only counts the bundles loaded and wakes the thread; the thread finds the
engine among the loaded images by its path, ending in
`game_plugin.bundle/Contents/MacOS/game_plugin` (`src/dll/game_macos.c`).

The thread opens `harness.log` and the job socket at once. Until the engine
is found and checked, `status` answers that the game is `starting` and every
other job is refused. Then, as for FM24's executable, the harness reads the
slice's `LC_UUID`, finds the build in `src/common/macos_builds.c`, picks
that build's tables, checks that they have every address it needs, and
compares the first bytes of each function it calls. The engine's header is
the base its addresses are offsets from (FM27's mappings have
`image_base` 0). The log then says `accepted: ready; waiting for jobs`, or
`refusing ...` and `refusing jobs: unsupported game build`.

A job runs holding the engine. When the game unloads it, the remove-image
callback marks it as unloading, a run stops at its next seed, and the
callback waits up to ten seconds for the job to let go; from then on every
job is refused. (A bundle with thread-local variables, as `game_plugin`
has, may never be unmapped; the game may then exit without unloading it.)
With a `log-only` file next to the dylib, which `fm27ctl install --log-only`
writes, the harness logs all this and opens no socket.

# The game's state

`src/dll/game_state.c` reads, for FM27, the game options' "game in
progress" flag (`+0x67`), the `GAME` singleton and its auto-play fields
(`+0x80`, `+0xd0`), the people list and the in-game date. FM27's main menu
is Unity's, so the engine has no start screen: `RVA_MACOS_START_SCREEN` is
mapped to the `GAME` singleton, which the engine's startup creates. That
says only that the engine has started: the plugin's startup gets `GAME`
(arm64 `0x2b3dc`) before it calls the auto-play reader (`0x2b5d4`), which
hands the save named by `LOAD_GAME` to the save system's load
(`0x258241c`), and no store the startup makes after that call was found to
mark the load's end. In FM24 a load sets "in progress" before it has
ended, and FM27's may too. So a game in progress counts as loaded only
once it has settled
(`gameState_settled`): once it has people, and the in-progress flag and
the number of people have held for `GAME_STATE_SETTLE_MS` (three seconds,
as long as `fm27ctl`'s three ready samples take). Until then `menu_seen`
is `0`, the state is `loading`, jobs are refused and the people are not
searched for the human manager. The job thread samples the game every
`GAME_STATE_SAMPLE_MS` (a second) while it waits for jobs
(`src/dll/main_macos.c`), so a game that has settled is ready when asked.
Nothing is sampled while a job runs, while the Mac sleeps or while the
engine is not loaded, and the game may load another save meanwhile with
as many people, so a sample taken more than `GAME_STATE_GAP_MS` (two
seconds) after the last counts as a change, and the three seconds start
again; the samples are also forgotten whenever the engine is not
accepted. So after a job of more than a second or two, the game is
`loading` for three seconds or so.
The `GAME` processing flag and kind, the save system's save name and the
clubs' short names have not been located (`GAME_UNLOCATED_*` in
`src/dll/game_layout_fm27.h`), so the game is never read as busy, `status`
gives no save, and teams are found by unique ID only: `replace-home` and
`replace-away` refuse a club's name (`database_findTeam`).

# The match calls

`src/dll/match.c` calls FM27's engine as its own callers do:

- **Create** takes no argument: it makes a `GAME_MATCH` through the factory
  that the game registers in its context, or a plain match without one. The
  harness checks that the object carries the `GAME_MATCH` vtable, and
  otherwise deletes it through its own vtable and refuses the job.
- **Serialise and deserialise** take `(match, stream**, context, 0)`; the
  context is a global (`RVA_MACOS_MATCH_CONTEXT`), which the harness reads
  and refuses jobs without.
- **Build** prepares the teams, makes the match with the factory
  (`RVA_MACOS_MATCH_FACTORY`; the pool allocator FM24's builder called is
  inlined in FM27), and passes `InitArgs` of 0x38 bytes (the fixture, the
  scratch block, the seed at `+0x20` and the mode at `+0x24`) and a
  16-byte flags block. `static_assert`s check the sizes and offsets.
- **Prepare, run and destroy** are FM24's calls.

A serialised match starts with the game's version (4 bytes), the format
version (`u16`), a zero, a tag (`0x02`) and a `u32`, then the label (a `u32`
length and its bytes), a `u32` and the seed: the seed is at 20 plus the
label's length. `match_seedOffset` checks the format version against the
running build's (`MacosBuildTables.formatVersion`, from the mapping) and
refuses another. The export's clock seed, when no `--seed` is given,
copies FM27's builder: `mach_absolute_time` in milliseconds at the
timebase it works out, folded into 32 bits.

FM27's player record holds a body type where FM24's held a weight; the
match builder gives a player the weight `BMI(body type) * height^2`, with
BMIs of 19.0, 20.5, 23.5, 26.5 and 29.5, which `game_playerWeight` copies.

The detailed statistics (`MatchStatsLayout` in the layout header) come from
the routine that adds up a played match's minutes and ratings, which is
FM24's instruction for instruction, and from `MATCH_PLAYER_STATS`'s
serialiser, which writes FM24's blocks in FM24's order at other offsets.

# Build tables

FM27's tables (`src/dll/game_macos_tables_fm27.c`) hold one set per build
and slice, which the harness picks by the engine's UUID, so one harness
supports every FM27 build that has a mapping. They are generated from all
the mappings (`tools/macos/steam-27.*.json`) by `tools/macos/gen_tables.py`,
which also checks that each mapping's layouts are those of
`src/dll/game_layout_fm27.h`: the layouts are shared by every build, and a
build whose objects moved needs per-build layouts first.

The frames translate FM27's serialiser stacks into FM24's Windows frames
wherever FM27 writes a field as FM24 does, so the labels, member names,
per-seed rebuilds and team replacement work unchanged. An entry may depend
on the next two frames up the stack, not only the next. FM27's own writes
and calls translate into synthetic IDs from `0x7E000000`, which each
mapping lists in `sites` and keeps from build to build; the named ones,
which the labels use, are generated into `src/dll/sites_fm27.h`.
`tools/macos/align/frames.py` made 27.0.0.2486716's frames from FM24's, and
[tools/macos](../tools/macos/README.md) describes the mapping, how it was
made, and how [`remap.py`](../tools/macos/README.md#mapping-a-new-fm27-build)
carries it over to a later build through that build's fingerprints.

# Configs

FM27's fixtures have `"format": "fm27-match-config/1"` and
`"game_build": 2486716`, the last part of the game's version (its
`CFBundleVersion` is `0`). Their fields are labelled as FM24's are, with
FM27's own structure named where it differs: [Editing an FM27 fixture
file](FIXTURE-FM27.md) lists it.

# Tests

The FM27 edition's suites (`bin/fm27_tests`) are FM24's, with FM27's
values, plus tests of what only FM27 does (see
[Editions](DEVELOPMENT.md#editions)). In the `macos` suite, a stand-in
game, named as FM27's and in a folder with spaces, loads the harness and
loads and unloads a stand-in engine plugin, another build of it and an
unrelated bundle; the stand-in plugin also stands in for the engine's
calls in a run that keeps the build, and its startup reads the auto-play
file from its command line, so `fm27ctl` is tested from `install` to `run`
against it. `ctest`'s `align` test runs the Python tests of the mapping
tools, `remap.py` included, which builds two versions of a small bundle
for both slices to remap one onto the other.

# Known limits

- Every FM27 value comes from the game's code, read without running it;
  [Checks with the game](MACOS-FM27.md#checks-with-the-game) lists what
  needs checking, in order.
- Only 27.0.0.2486716 has a mapping.
- The processing state, the save's name and the clubs' names are not read.
- Whether the engine's calls work on the harness's thread, without what
  the game's own threads set up, is not known; the vtable check refuses a
  job whose match is not a `GAME_MATCH`.
- The x86_64 slice's frames were made from the arm64 slice's.
- FM24's [known limits](TECHNICAL.md#known-limits) on per-seed rebuilds,
  observers and player order presumably apply too.
