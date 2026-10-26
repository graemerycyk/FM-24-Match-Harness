FM24 match-engine harness for macOS: technical notes
===

These notes describe how the macOS version works: `fm24ctl`, the launcher
that loads the harness into Football Manager 2024 24.4.2 from Steam, the
harness dylib and its job protocol, the build tables, the build and
FM24Viewer.app. For setting up and using it, see the [README](../README.md),
the [macOS guide](MACOS.md) and the [CLI reference](CLI.md).

The harness's job, match, fixture and seed-building code comes from the
original Windows project,
[PhilipArmstead/FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead. Its Windows-specific parts (injecting a DLL into
`fm.exe`, the Windows memory, thread and profiler code, and the Win32
viewer) remain in the source, so the shared code stays mergeable with that
project, but they are not described or supported here. For Windows, use the
original project.

# Layout

```
src/common/  protocol (the job socket, the dylib's name), byte buffer, files,
             shared tactic role/position names, platform (threads, locks,
             clocks, files and UTF-8; platform_posix on macOS), channel (the
             socket path and its lock), macos_builds (supported macOS slices)
src/dll/     the harness: game.h (addresses, layouts, signatures), memory,
             stream, match, rng, database, fixture, trace, seed_build
             (per-seed rebuilds), sites/site_lookup/site_members, labels
             (rating names), config_writer, output, profiler, log, job,
             job_run/build/inspect/replace/teams/players/player/execute/
             status, game_state (what the game is doing, read only),
             match_stats, tactic_flags (generated); macOS: game_macos
             (+ generated tables), main_macos (socket server), memory_macos,
             profiler_macos
src/runner/  fm24ctl: main, commands, json_reader,
             match_config (encoder, streaming field visitor and the
               export's save and date),
             player_inject (copies a player's values into a block),
             fixture_data (platform-independent read model),
             fixture_edit (a save's edits and encoding, in memory),
             saves (finding a save, the auto-play file, the status line),
             session (waiting for a save to be loaded and idle, and
               what a command does first for its job's save);
             macOS: client_posix (the job socket), paths_posix,
             embedded_macos (the built-in dylib), inject_macos (installs
               the harness and says how to start the game with it),
             probe_macos, macos_game (install, launch, launch --save, a
               command's save check, Steam discovery), macos_viewer
               (opening fixtures in FM24Viewer.app)
src/ui/      fixture_export (portable export filename generation),
             fixture_view (the viewer's rules as portable C;
             module.modulemap imports it into Swift), save_list (the saves
             the Launch FM24 sheet lists); macos/: FM24Viewer.app (App:
             AppKit shell and menus, Model: Foundation models, Views:
             SwiftUI views and the squad tables)
tests/       custom test runner, fixture reader, job, runner startup and viewer
             checks; fixture_samples (synthetic fixtures); macos_test (tables,
             socket round trip, Steam discovery, launcher, launch --save,
             the save check before a job), with macos_stand_in_game and
             macos_stand_in_library for the launcher tests; viewer/: the
             viewer's Swift tests, with viewer_stand_in_fm24ctl in place of
             fm24ctl
cmake/       macos_viewer.cmake (FM24Viewer.app), viewer_probe.swift
scripts/     build.sh, format.sh, tidy.sh (env.sh: tool locations),
             release_macos.sh (the release zip)
tools/       macos/: the build mapping (steam-24.4.2.json), its generator
               gen_tables.py -> src/dll/game_macos_tables.c, check_mapping.py
             experiments/: team instruction analysis and ability experiments
               (those that run the harness look for fm24ctl in the working
               directory or ../bin/; running them on macOS has not been
               checked)
             memscan.py: the original project's memory scanner for fm.exe
               (Windows only: on macOS the game's hardened runtime denies
               other processes access to it)
```

`CMakeLists.txt` lists the sources each platform builds. The sources only
the Windows build compiles (such as `src/dll/main.c`, `src/runner/inject.c`
and `src/ui/fixture_window.c`) are not described here. Source comments cite
`docs/PLAN.md` by phase, and `tactic_flags.c` and `site_members.c` name
generators in `tools/fields/`; neither is in this repository.

# How it works

## Loading the harness

The game is signed with the hardened runtime, which denies other processes
access to it, so nothing can be injected into a running game. The game is,
however, entitled to accept `DYLD_INSERT_LIBRARIES` and libraries signed by
others, so the harness is loaded when the game starts.

`fm24ctl install` (also `launch`, and any command that finds no harness)
writes the dylib, `fm24-harness-<version>.dylib`, and the launcher,
`launch-fm24.sh`, into `~/Library/Application Support/fm24-harness/`.
`install` prints the Steam launch option that runs the launcher,
`/bin/sh "<launcher>" %command%`. It writes a development build's dylib
from beside `fm24ctl` if there is one (as in `build/`), and otherwise the
copy built into `fm24ctl` (`src/runner/embedded_macos.c`). An identical file
is left alone. Otherwise a new file is renamed over the old one, so a game
that has the old one loaded keeps its own copy: overwriting it in place
would invalidate the code signature of its mapped pages.

Steam runs the launch option through `/bin/sh`, which drops `DYLD_*`
variables, so the launcher sets `DYLD_INSERT_LIBRARIES` itself and starts
the game executable, keeping the libraries Steam inserts for its overlay
(`STEAM_DYLD_INSERT_LIBRARIES`). On Apple silicon, Steam starts the launch
option as an Intel process, and a game started from it would inherit that
and run under Rosetta. The launcher therefore starts the game through
`/usr/bin/arch` in the same process, so Steam still tracks the game: with
the arm64 slice where `sysctl` reports Apple silicon, and the x86_64 slice
on Intel Macs, or under Rosetta when a waiting `launch --rosetta` has left
a request for it. The launcher also takes a waiting `launch --save`
request and passes the game its auto-play file (see
[Loading a save](MACOS.md#loading-a-save)). Without Steam's launch option,
`fm24ctl launch` starts the game directly with `open` (`open --arch x86_64`
for `--rosetta`), which has not been verified (see
[Known limits](MACOS.md#known-limits)).

## The harness

The dylib's initialiser runs before the game's own. In the game's process
(an executable ending in `/Contents/MacOS/fm`; helper processes that inherit
`DYLD_INSERT_LIBRARIES` are left alone) it only starts the job thread, with
a 16 MB stack, since jobs run engine code on it. The thread opens
`harness.log` next to the dylib, identifies the game by the `LC_UUID` of
the running slice (`src/common/macos_builds.c`), checks that the build
tables have every address it needs and that the first bytes of each
function it calls match (`src/dll/game_macos.c`), and opens the job socket.
It then logs `ready; waiting for jobs`, or `refusing jobs: unsupported game
build` and answers every job with an error. A `refusing: ...` line before
it gives the reason, such as an unsupported build's UUID or unexpected code
at a called function (a software breakpoint, or another build). The dylib
exports nothing.

## The job protocol

`src/common/protocol.h` is what the harness and `fm24ctl` agree on. The
channel is a Unix socket in your temporary directory (the one
`getconf DARWIN_USER_TEMP_DIR` prints), named after the protocol version:
`fm24-harness-<version>.sock`. The harness holds an exclusive lock on
`fm24-harness-<version>.sock.lock` while it serves, so `fm24ctl` can tell a
running harness from a socket left behind by a game that crashed, and a
second game process with the same version stays idle. It binds the socket
under a temporary name, open only to your user, renames it into place, and
removes it when the game exits normally.

The harness runs one job at a time:

1. `fm24ctl` connects. Both sides check that the other end belongs to the
   same user (`getpeereid`). Runners that connect while a job runs wait in
   the socket's backlog; while the backlog is full, `fm24ctl` retries as
   long as the harness holds its lock.
2. The harness sends one byte, `+`, when it takes the connection.
3. `fm24ctl` writes the job, `key=value` lines such as `command=status` (at
   most 16384 bytes; `src/dll/job.h` lists the commands), and shuts down
   its sending side. The harness waits up to 60 seconds for the whole job.
4. The harness runs the job (`job_execute`), replies with one result line
   starting with `ok` or `error` (at most 512 bytes), and closes the
   connection.

Jobs carry full paths, so a runner works with a harness loaded from any
folder. `fm24ctl` writes job inputs into the same temporary directory,
which the game can always read. A `fm24ctl` of another protocol version
finds no socket of its own: it reports the version the game has loaded and
says to restart the game (unit tests cover finding the other version's
socket; the message has not been checked with the game).

## Build tables

The harness names game functions, data and serialiser call sites by the
RVAs of the Windows Steam build, as the original project does
(`src/dll/game.h`, `sites.h`, `site_members.c`), and labels every field of
a serialised match from the call stack of the write that produced it. The
macOS build is compiled by clang with the Itanium C++ ABI: its code is laid
out, inlined and tail-called differently, and many objects are laid out
differently (libc++ containers are smaller than MSVC's). The harness keeps
all its logic and adds per-architecture tables
(`src/dll/game_macos_tables.c`, see `src/dll/game_macos.h`):

- `addresses`: the macOS counterpart of each RVA the harness calls or
  reads (functions, vtables, globals), and the game state objects that
  `status` reads (`MacosGameData`).
- `frames`: for every return address in the macOS serialisers, the Windows
  frames it stands for. One macOS frame may stand for several Windows
  frames (a call clang inlined) or for none (a call MSVC inlined or turned
  into a jump). The write trace translates each macOS stack this way, so
  the labels, member names, per-seed alignment and team replacement work
  unchanged. A frame without a Windows counterpart keeps its own identity
  (`0x80000000 | macOS offset`).
- `signatures`: the first bytes of each function the harness calls,
  checked at startup.
- The thread-local PCG32 the pre-match setup draws from.

The macOS object layouts are in `src/dll/game.h`. The tables are generated
from `tools/macos/steam-24.4.2.json`;
[`tools/macos/README.md`](../tools/macos/README.md) describes the mapping,
how it was made and how to check it against the installed game.

# Building

Requirements: macOS 11 or newer with the Xcode Command Line Tools (Apple
clang and Swift), CMake 3.20 or newer, and Ninja if it is installed
(otherwise Makefiles). `format.sh` and `tidy.sh` need clang-format and
clang-tidy 22 or newer from Homebrew's LLVM (`brew install llvm`). See
[Development](DEVELOPMENT.md#macos) and `scripts/env.sh`.

```sh
scripts/build.sh     # build into build/, install fm24ctl, the viewer and tests into bin/
scripts/format.sh    # apply .clang-format (--check to only verify)
scripts/tidy.sh      # clang-tidy with .clang-tidy per architecture (warnings are errors)
```

The build script always configures Release mode and C17, including all
regression checks in `fm24_tests`. It builds universal (arm64 and x86_64)
binaries for macOS 11 and newer with Apple clang, and the viewer for macOS
13 and newer (see [The fixture viewer](DEVELOPMENT.md#the-fixture-viewer)).
CMake defaults Release builds to `-O2 -DNDEBUG`, with link-time
optimisation across the shared code, dylib, runner and tests. Configuration
fails if the compiler does not support LTO. Release links discard unused
sections (`-dead_strip`). The dylib is compiled with hidden visibility and
exports nothing. Frame pointers are kept (`-fno-omit-frame-pointer`),
because the trace and the profiler walk frame-pointer chains. Debug builds
retain their normal flags without LTO. Fast-math and
build-machine-specific CPU instructions are not enabled.

Unstripped binaries stay in `build/` for diagnosis. `fm24ctl` embeds
`build/harness-embedded.dylib`, a stripped (`strip -x`), ad-hoc signed copy
of the universal dylib, in its `__TEXT,__fm24_harness` section, so `bin/`
holds no dylib (the script deletes any left there, since a loose dylib
beside `fm24ctl` would override the built-in one). The script installs
copies into `bin/`, then strips `fm24ctl` and `fm24_tests` and signs them
again ad hoc: stripping invalidates the linker's signature, which arm64
requires. Installation, stripping or signing errors fail the script instead
of silently leaving old binaries in `bin/`. For a harness the game has
loaded, see the protocol-version notes below.

Run `bin/fm24_tests` for the application regression checks, or pass a suite
name (`job`, `dll`, `fixture_data`, `runner`, `runner_main`,
`fixture_export`, `fixture_view`, `player_inject` or `macos`) to run only
that suite. On Apple silicon with Rosetta, `arch -x86_64 bin/fm24_tests`
runs the x86_64 slice's. `fm24_tests` needs its two stand-ins next to it
(`fm24_stand_in_game` and `libfm24_stand_in_library.dylib`). Each named
test prints `[PASS]` or `[FAIL]`, followed by a test/failure count; failures
return a nonzero exit code. Assertions report the source location,
expression and expected and actual values without stopping the remaining
checks.

The `dll` suite uses synthetic engine objects to check serialisation traces,
edit alignment, player identity across rebuilt line-ups, instruction labels
and UTF-8 exports without loading FM24. It also reads a synthetic game's
state: the status line, the save name's checks, the refusal of jobs while
the game is busy, and the save and date in exports but not in decoded
fixtures. The `job` suite also covers repeated seed keys, malformed or
overflowing seed values and status jobs.

The `runner` suite checks single-read config preparation, CLI and CSV numbers,
result-file failures, `status`'s exit codes, configs with and without `save`
and `game_date`, the check of a job's save (what happens in each state of
the game, the arguments before and after the save options, and the commands
that read a fixture's save), the replacement of an installed dylib and
`probe` on synthetic universal and single-slice files, without connecting
to FM24. `runner_main` checks the conversion of arguments, the commands'
dispatch and opening fixtures in the viewer app, and `macos` which images
of the game the harness accepts, its refusal of jobs elsewhere, finding
another harness version's socket, the direct start's `open` command, how
`launch` judges the starting game, and a command's save check against a
stand-in harness.
Config preparation reads replacement-team metadata while encoding, rather than
loading and parsing the same JSON twice. Field visitors reuse one field buffer
instead of retaining the entire encoded fixture. Strings and hex values are
copied in batches, and discarded annotations allocate no decoding buffers.
Malformed UTF-8, Unicode escapes, hex digits and replacement IDs are rejected.

Per-seed rebuilding omits write tracing and edit alignment when the input
has no edits. Edited inputs with unchanged write sites skip call-chain
parsing and alignment allocations. When tracing is needed, call chains are
interned as numeric RVAs (after the [build tables](#build-tables)'
translation); their text is formatted only once per distinct chain. CPU
timing calls are limited to the profiled worker, and workers only copy the
input when patching seeds rather than rebuilding it.

Use `ctest --test-dir build --output-on-failure` for individual results and
the release checks. Building does not run the tests. The `release` check
(`tests/release_macos_test.cmake`) verifies both slices, the signatures,
the embedded dylib and that nothing is exported; on Apple silicon with
Rosetta, `x86_64` runs the tests' x86_64 slice, and the viewer has its own
checks (see [The fixture viewer](DEVELOPMENT.md#the-fixture-viewer)). To
stage only the distributable runner, without the test executable:

```sh
cmake --install build --config Release --prefix dist --component Runtime --strip
```

Sign the staged runner again, as `scripts/build.sh` and the release check
do: the linker signs only the arm64 slice, and `codesign --verify` fails on
a universal binary whose x86_64 slice is unsigned.

```sh
codesign --force --sign - dist/fm24ctl
```

The build uses `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and friends
and is warning-free; `format.sh --check` and `tidy.sh` are clean.

When configuring CMake directly, use `-DFM24_BUILD_TESTS=ON` to include the
test executable. Its portable reader, filename and player-injection checks
can also be built on their own, without CMake:

```sh
cc -std=c17 -O2 -Isrc/common -Isrc/runner -Isrc/ui \
  tests/test.c tests/fixture_data_test.c tests/fixture_export_test.c \
  tests/player_inject_test.c src/ui/fixture_export.c \
  src/runner/fixture_data.c src/runner/player_inject.c \
  src/runner/match_config.c src/runner/json_reader.c \
  src/common/buffer.c src/common/tactic_names.c -o /tmp/fm24_tests
/tmp/fm24_tests
# Optional integration check against the local Aston Villa/Bristol City baseline:
/tmp/fm24_tests fixture_data tools/experiments/baseline.json
```

Tests use the same dependency-free framework as the Triple Triad project:
include `test.h`, define cases with `TEST(describes_the_behaviour)`, and
use `ASSERT_INT_EQ(actual, expected)` or `ASSERT_BOOL_EQ(actual, expected)`.
Register each case with a descriptive `test_run` call in its suite's
`register...Tests` function. Guard failed setup before dereferencing pointers
or inspecting buffers, since assertions do not abort the test. Keep
pointer-sized and 64-bit comparisons in `ASSERT_BOOL_EQ` to avoid narrowing
them to the integer assertion's `int` parameters.

**Rebuilding while the game runs:** the game loads the harness only when it
starts, so a rebuilt harness loads when the game next starts; until then,
commands use the harness the game loaded without saying so, unless its
protocol version differs. `build/fm24ctl install` (or `launch`) installs
the build's unstripped `fm24-harness-<version>.dylib` from `build/` rather
than its embedded copy. To give a rebuilt harness its own socket and file
name, bump `HARNESS_PROTOCOL_TEXT` in `src/common/protocol.h` and build;
CMake re-configures by itself.

**Generated code:** after changing an address in `src/dll/game.h`,
`sites.h` or `site_members.c`, update its macOS counterpart in
`tools/macos/steam-24.4.2.json` and run
`python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c`
(see [tools/macos](../tools/macos/README.md)).

# The viewer

FM24Viewer.app is an AppKit shell with SwiftUI views (`src/ui/macos/`) over
the viewer's rules in portable C (`src/ui/fixture_view.h`, which Swift
imports through `src/ui/module.modulemap`). Those are the rules of the
original Windows project's viewer, so the summary, squad lists, edits and
venue values follow the same rules. It uses only the system's AppKit and
SwiftUI, with no third-party UI or parsing dependencies. It reaches the game
by running its own copy of `fm24ctl` (`get-next-fixture`, `decode`, `run`,
`replace-home`, `replace-away`, `inject-player`, `list-teams`,
`list-players`, `launch` and `install`), with every path the game reads or
writes in your temporary directory, and takes the first line of `fm24ctl`'s
explanation when no harness serves the job socket.

Opening a fixture does not change its source;
`fm24ctl <fixture.json|fixture.bin>` opens it in the `FM24Viewer.app`
beside `fm24ctl` and leaves the other commands unchanged. Viewing a JSON
file does not connect to the game. A `.bin` fixture is decoded by
`fm24ctl decode`, through the `inspect` job, from a copy in the viewer's
own temporary folder (`fm24-viewer-<pid>`). Inspection deserialises and
traces the existing match without running preparation, building another
fixture or invoking the engine's match runner. Started without a fixture,
the viewer exports the next fixture to `temp-fixture.json` in the
temporary directory and shows it as not yet saved; if the export fails,
the window stays open and says why. The export link in the summary, or
**File > Export**, writes an unsaved fixture with its edits to the export
folder, never replacing an existing file.

The Venue tab edits labelled venue values. Float values are bit-copied
to/from their serialized `u32` representation. The config visitor exposes
numeric source spans so venue saves can patch only the values, retaining
unrelated fields, annotations and team replacement IDs. Shared validation
rejects invalid dimensions, coordinates, surfaces, condition and capacity
relationships. The same writer derives pitch geometry and every attendance
copy for saving, exporting, team replacement and simulation. Saves replace
the destination only once the whole file is written. Unstored edits are
marked in the summary, and opening another fixture, closing or quitting
asks before discarding them.

The Simulation tab runs `fm24ctl run`. Thread bounds reuse the CLI default
(logical CPUs minus one, clamped to 1-16). Seed patterns are limited to 255
characters, checked for job-safe characters before submission, and fully
parsed by the harness. The viewer always plays a copy in its temporary
folder, with venue values and pending player edits written into it and the
results written there too, so the source is never overwritten by
simulation. The harness does not report progress, so the viewer reads the
results CSV every 200 ms: the run flushes each row as the rows complete in
seed order, so the count trails by at most one match per thread less one,
and a line that is not a result leaves the bar indeterminate. The Dock
icon's badge shows the percentage, and an `NSProcessInfo` activity keeps
the Mac from idle sleep and the viewer out of App Nap while the run lasts.
Afterwards the viewer moves the results (and detailed reports) beside the
fixture, following the CLI's fixture-based naming and replacing earlier
ones, or into the export folder for a fixture not yet saved. A run that
fails part way has its rows so far saved, as the game leaves them.

The team picker (a popover from the pencil) and the swap prompt (a sheet)
follow the rules of the original Windows project's viewer
(`Model/Choices.swift` and `Model/Replacement.swift`), whose tests mirror
that viewer's. The viewer loads the team list (`fm24ctl list-teams`) in the
background the first time either team is edited, to fill its filterable
picker; a picked team is
submitted by ID, so clubs that share a short name resolve. It loads the
player list (`list-players`) the first time its swap prompt opens. "Swap
player" swaps out the side's selected row, and is enabled only while
exactly one player is selected. The prompt's label names that player
("Swap <name> for..."). The new player field lists matches once 3
characters have been typed and the typing has paused for 100 ms. Matching
ignores case and accents: the folding decomposes the text, spells out the
letters Æ, Ð, Đ, Ø, Þ, ß, Ħ, ı, Ł and Œ, drops the marks that are not
letters and lowercases it, so "odegaard" finds "Ødegaard", and a name that
starts with the text comes first. Lists are sorted by that folded label.
Labels show the common name first, e.g. "Pelé (Edson Arantes do
Nascimento)". A picked label is injected by the ID at its end. Digits are
taken as a unique ID, which also works while the list is loading or if it
failed. The prompt's "Allow duplicates?" box passes `--allow-duplicate`, so
a player already in the fixture can be swapped into another block. Without
it, the refusal's hint points at the box. A replacement or swap writes the
fixture's JSON with the edits it carries (a `.bin` fixture's JSON decoded
when it was opened) into the viewer's temporary folder, has `fm24ctl` work
there and writes the venue values into the result. The lists are dropped
when the game quits, since another save may list others.

# Use

Run `fm24ctl` from `bin/` (or the release folder): it opens the
`FM24Viewer.app` beside it. `harness.log` lives next to the dylib the game
loaded, in `~/Library/Application Support/fm24-harness/`. A runner sends
its job over the socket of its protocol version, so a `fm24ctl` in another
folder (e.g. `build/`) with the same protocol version uses the harness the
game loaded, and its log.

The harness runs one job at a time. Runners (the viewer's included) that
submit while a job is running wait for it (`the harness is running another
job; waiting for it to finish`), and their time limit includes that wait.
A runner that times out stops waiting but does not cancel the harness's
work; later jobs still wait for it.

1. Start FM24 with the harness (from Steam, with the launch option
   `fm24ctl install` prints, or with `fm24ctl launch`) and load the save,
   or let `fm24ctl launch --save <save>` load it. Wait until the game is
   **idle** (not processing). The engine calls aren't synchronised with the
   game's own threads. The harness refuses jobs while the game loads or
   processes a game (see step 7).
2. Every command that needs the game connects to the harness's socket
   first. If no harness of this version serves it, the command installs
   this version's dylib and launcher (see
   [Loading the harness](#loading-the-harness)) and says whether the game
   is not running, has another harness version loaded or runs without the
   harness, and how to start it with the harness: a running game cannot
   load it. `harness.log` should say `ready; waiting for jobs`.
   `refusing: ...` means a different game build, or a software breakpoint
   on a called function. Old versions collect in the harness folder (see
   [Files and removal](MACOS.md#files-and-removal))
3. `fm24ctl run <input> [seeds] [out.csv]`,
   where `<input>` is a `.bin` input or a `.json` config; e.g.
   `fm24ctl run next.json` plays the config's own seed into `next.csv`.
   It prints a result line (`ok: N/N runs...` or `error: ...`) and a
   summary
	- seeds: `keep` (the input's own seed; the default), `42`, `1-1000`, or
	  `1,5,10-20`. A seed plays exactly as the game would with that seed
	  (`GAME_MATCH__init_from_fixture`'s seed): the seed also draws pre-match
	  values (attendance, the officials' match-day ratings, a random number
	  per player, weather), so for each seed `run` has the game rebuild the
	  input's fixture with that seed and re-applies the input's edits. An
	  unedited dump then matches the game; edited fields keep your values.
	  This needs the input to be the human manager's next fixture in the
	  running game (built in the current game state); otherwise `run` stops
	  with an error. To compare with the game, dump the fixture after clicking
	  "Simulate Match" and before "Continue", and run it before "Continue":
	  until the team is submitted, a build picks the human side's line-up
	  itself, differently for each seed. If a seed's build has a different
	  structure (another line-up or AI tactic) where the input was edited,
	  that seed fails (see `harness.log`). The input and the build are
	  aligned part by part along the serialiser's call chains (`align()` in
	  `seed_build.c`), so a difference only stops edits inside the parts that
	  differ. Each team's players are paired by person, not by place
	  (`comparePlayers` and `alignPlayers`), because the game can list them in
	  another order (see [Known limits](#known-limits))
	- `--keep-build`: play the input as it is, changing only its seed. The
	  pre-match values stay those of the seed it was built with, so results
	  differ from the game's for other seeds, but it works for any input
	- output: the input's name with a `.csv` extension unless given. An
	  argument of only digits, `,` and `-` is read as seeds, so name the output
	  e.g. `2024.csv`, not `2024`
	- matches run in parallel: by default on one worker thread per logical
	  CPU less one, at most 16. Results are identical whatever the thread
	  count (see the observers in [Known limits](#known-limits)), and rows
	  stay in seed order. In the original Windows project one match took
	  about 3 s, ~0.4 s per match on average with 11 threads on a
	  6-core/12-thread CPU, and each match's `ms` rose to ~4-5 s under load
	- `--threads N` (1-16): set the worker count (1 = one match at a time)
	- `--profile <samples.txt>`: sample the engine's stack about every 1 ms
	  (worker 0 only; see `src/dll/profiler.h` for the format). The first
	  line, `# fm <base>`, is the address of the running slice's Mach-O
	  header: an address in the game minus the base is its offset in that
	  slice, and adding `0x100000000` to the offset gives the unslid address
	  that `tools/macos/steam-24.4.2.json` uses. That file covers only the
	  functions the harness calls and the return addresses in the game's
	  serialisers, so most samples are not in it
	- `--detailed`: also write `<output>-players.csv` and `<output>-teams.csv`,
	  with each statistic averaged per match over all the seeds. Players (both
	  squads, substitutes included): appearances, starts, minutes, average
	  rating (over appearances), then goals, assists, shots, on target,
	  blocked and long shots, xG, xA, key passes, clear-cut and half chances,
	  passes (and progressive passes), crosses, dribbles, tackles,
	  interceptions, clearances, blocks and shots blocked, pressures
	  (attempted, inferred; completed), headers, fouls
	  committed and suffered, offsides, cards, set pieces taken, goalkeeper
	  saves, goals conceded, possession won and lost, high-intensity sprints
	  and distance (km). Teams: goals, possession, average rating (as the
	  game's match statistics screen shows it) and the team totals. The data
	  is the engine's own per-player stats objects. The values match the
	  game's statistics screens, team and player (checked on one match, in
	  the original Windows project). The members are listed in
	  `src/dll/match_stats.c`. Player rows are ordered by unique ID and
	  retain each person's totals when a rebuilt line-up changes order or
	  introduces another player.
	  Reports are written only after every seed succeeds; a CSV or detailed-report
	  write failure makes the job fail rather than reporting success.
	- both teams' in-match changes (substitutions etc.) are made by the game's
	  AI, as when the game simulates a match nobody watches. Before kick-off
	  the harness resets the human manager's control flags to "no match plan,
	  not in control", so the Match Plan of whoever built the input doesn't
	  carry over. AI-managed teams are left as they are
	- `--human-control`: keep the input's flags instead. A team built with a
	  human in control then makes no tactical changes (nobody is there to make
	  them) and may not even replace injured players
4. `fm24ctl get-next-fixture <out.json|out.bin> [--seed N] [--full]` builds
   the human manager's next fixture (~0.2 s). The seed defaults to a clock
   value, like the game. The same seed gives a byte-identical file, because
   the seed also seeds the thread-local RNG the pre-match setup uses. Only
   the match's observers can differ, because they also depend on earlier
   builds (see [Known limits](#known-limits)); with other observers, a
   `--full` export's `paths` and `p` numbers differ too.
	- `--full` (JSON only): also write the research keys: `bytes` and `paths`
	  in the header, and `site`, `p` and `member` on each field. Without it
	  the file keeps only the values and the labels (`name`, `player`,
	  `flags`, `role`), about 1.5 MB instead of 5.4 MB. Both encode to the
	  same `.bin`
5. `fm24ctl encode <in.json> <out.bin>` converts a config to a `.bin` input
   (no game needed).
   `fm24ctl decode <in.bin> <out.json> [--full]` converts back through the
   `inspect` job the viewer opens a `.bin` with (`src/dll/job_inspect.c`):
   the game deserialises the input and the harness traces its serialisation,
   as for an export, but nothing is prepared, built or played. The config's
   `source` notes that it was decoded, and `--full` adds the research keys as
   for an export.
   `fm24ctl list-teams <out.txt>` writes `uniqueId<TAB>short name` lines for
   every club's first team, the teams `database_findTeam` resolves names to
   (`database_listTeams`, ~1 s).
   `fm24ctl replace-home|replace-away <in.json> <team> <out.json> [--full]`
   rebuilds either side (`src/dll/job_replace.c`). `<team>` is a
   `db::TEAM` unique ID or a club short name (`database_findTeam`). The
   input's edits are re-applied as in `run`, except those to the replaced
   team. The output's `home_team`/`away_team` headers preserve both sides'
   overrides for later replacements and per-seed builds. For each build the
   seed builder swaps the teams in three places and restores them afterwards
   (`swapTeams` in `seed_build.c`):
	- the fixture's home (`+0x08`) and away (`+0x10`) pointers;
	- the copies of the fixture among the human's matches
	  (`[RVA_HUMAN_MATCHES_HOLDER]+0x130`). Without these the builder doesn't
	  give the human manager control and picks the human's side itself;
	- each original team's next-match opponent (`TEAM+0xA0` -> `+0x38`).
	  All required pointers are collected before mutation; exceeding the fixed
	  swap capacity fails without changing the game.
	  `fm24ctl get-player <person uid> <out.json>` writes a player's database
	  values as an `fm24-player/1` file (`src/dll/job_player.c`): the fields a
	  player block copies from the database, named by their block suffix, with
	  the age on the human manager's next match day. The player record is found
	  from the person through the Itanium RTTI (`database_playerRecord`): the
	  person's vtable gives the offset to the complete object and the mangled
	  class name, `N2db13ACTUAL_PLAYERE` or
	  `N2db28ACTUAL_PLAYER_AND_NON_PLAYERE`. For all 47 players of one
	  fixture, every value matched the fixture's (checked in the original
	  Windows project).
	  `fm24ctl inject-player <in.json> <home|away>.player[N] <uid|player.json>
		<out.json> [--allow-duplicate]` patches those values into one block offline
	  (`src/runner/player_inject.c`), fetching them first when given a unique
	  ID. Like a venue save, it replaces only value tokens (and the block's
	  `player` note) through the visitor's source spans, so the structure and
	  every other byte stay as they were.
	  `fm24ctl list-players <out.txt>` writes `uniqueId<TAB>forename<TAB>
	  surname<TAB>common name<TAB>club short name` lines for every person with
	  a player record (`database_listPlayers`; the common name and the club
	  are empty when the player has none). A person's names are relative to
	  its `ACTUAL_PERSON_BASE` part, which `database_personFields` finds
	  through the vtable; the common name is at `+0x38` there. The viewer's
	  use of these lists is under [The viewer](#the-viewer).
6. `fm24ctl probe [path to fm.app]` reads only the `LC_UUID` of each slice
   of the game's executable and reports whether this harness supports it.
   It reads the file only: the game isn't needed (without a path it probes
   the running game's file, or the one in your Steam libraries). It prints
   each slice's UUID, writes no file, and exits with 1 for an unsupported
   build. A new build needs a new mapping (see [Build tables](#build-tables)).
7. `fm24ctl status` submits the `status` job of protocol 1.3.0
   (`src/dll/job_status.c`). It reads the game's state without writing
   anything (`src/dll/game_state.c`, through the fault-tolerant reads of
   `memory.h`) and answers with one line whose keys keep this order:
   `ok: <state> in_progress=0|1 processing=0|1 kind=N people=N
   human_team=0|1 menu_seen=0|1 autoplay=0|1 autoplay_read=0|1
   date=YYYY day D HH:MM save=<name>`. A date or save that cannot be read
   is left out, and the save runs to the end of the line. The runner exits
   with 0 for `ready`, 3 for another state and 1 without an answer. The
   objects it reads are `MacosGameData` in `src/dll/game.h`, with each
   slice's address in the mapping:
	- `in_progress`: u8 `game_options+0x5d`. The core loader sets it when a
	  load succeeds, just before it posts `LGOK`; a new game sets it too, and
	  leaving the game clears it.
	- `processing` and `kind`: `GAME+0xb8` (u8) and `+0xbc` (i32).
	  `GAME::start_processing` sets them to 1 and its argument, and
	  `end_processing` to 0 and -1. The game's code on disk suggested that
	  the start screen's auto-play load (`--auto_play_game_file`) does not
	  call `start_processing`, but in the game (arm64) that load read as
	  processing, kind -1, from "in progress" until it was idle. Each day of
	  **Continue** is processing of kind 1.
	- `people`: the length of the people list (`RVA_PEOPLE_LIST`, `+0x88`),
	  which the game's own auto setup checks. `human_team`:
	  `database_findHumanTeam`, a pass over the people, so it is looked for
	  only while a game is in progress and idle.
	- `menu_seen`: the pointer the start screen's first entry sets once the
	  game's auto setup, and so any auto-play load, has returned (arm64
	  `0x100c44698`, x86_64 `0x100cbe4fe`). Only `fm::APPLICATION`'s
	  shutdown clears it again (arm64 `0x100221690`, x86_64 `0x10022f12e`).
	- `autoplay`: `GAME+0x64`, set by the constructor and cleared when there
	  is no auto-play file; `autoplay_read`: `GAME+0x80`, the file's parsed
	  JSON.
	- `date`: `RVA_CURRENT_DATE` while a game is in progress, written as a
	  fixture's date (`gameState_formatDate`).
	- `save`: the string object at `[SAVE_SYSTEM]+0x28` (a u32 length, then
	  the bytes), which the game's loads set: kept only as valid UTF-8 text
	  of 1-255 bytes (`configWriter_isText`), without a trailing `.fm`.
	  In the game (arm64) it was the save's file name, for saves loaded by
	  `launch --save` from `cloud/games` and from another folder; a load from
	  the game's menu has not been checked.

	 The state is `busy` while processing, `loading` while people are loaded
	 but no game is in progress, `starting` or `menu` (before and after the
	 start screen) with neither, and `ready` or `no_team` once a game is in
	 progress, with or without the human manager's team. A game in progress
	 without `menu_seen` is `loading` too: the auto-play load sets
	 `in_progress` before its last steps, which the start screen's first
	 entry finishes on the main thread. Before any other job, `job_execute`
	 reads the state (without the team search) and refuses the job while the
	 game is `loading` or `busy`:
	 `error: FM24 is busy (loading); wait until it is idle`, or
	 `processing kind N`. A game that starts processing just after the check
	 still races the job.

CSV columns: `seed, home_goals, away_goals, clock_ticks (4/s), finished,
rng_final, ms`.

## Match configs (`fm24-match-config/1`)

`fields` lists every write the game's serialiser makes, in order, so encoding
is lossless. The encoder reads only `format` and each field's value; every
other key is an annotation, except `home_team` and `away_team` (unique IDs of
teams substituted for the fixture's original sides), which `run` passes
to the harness. `site`, `p`, `member` and the header's `paths`
and `bytes` are written only with `get-next-fixture --full`. Exports and
team replacements also write `save` and `game_date` after `source`: the
save name and in-game date of step 7, each only when it can be read.
`decode` never writes them. `fm24ctl` reads them back
(`matchConfig_readOrigin`, a pass over the top-level keys only): `run`,
`replace-home`/`replace-away` and `inject-player` with an ID first wait for
the game to have `save` loaded and idle, or start it with that save, and
`run` warns when the save's date is not `game_date` (`session_plan` and
`macosGame_ensureSave`; see `docs/CLI.md`). User-facing field meanings are
in `docs/FIXTURE.md`.

- **Value:** each field has exactly one: `u8`, `u16`, `u32`, `u64`, `str`
  (u32 length + UTF-8), `hex`, or `u8map`.
- **Location** (`--full`): `site` gives the `<call site>/<caller>` of the
  write as Windows Steam build RVAs, after the
  [build tables](#build-tables)' translation (named parts are in
  `src/dll/sites.h`); a frame without a Windows counterpart appears as
  `0x80000000 | macOS offset`. `p` indexes the `paths` table of serialiser
  call chains.
- **Seed:** the seed field has `"role": "seed"`.
- **Labels:** blocks carry a `"name"` (`match`, `referee`, `venue`, `fixture`,
  `home.player[3]`, `away.manager`, `home.backroom[2]`, ...), and player
  blocks also carry `"player"`. Fields whose meaning is known are named too:
  `.person_uid`, `.ratings`, `.ability` (caps every role rating),
  `.leadership_value`, `.role_rating[winger@AMR]` with its `.role` and
  `.position` bytes, and the team counts and flags. The weather
  (`match.weather.*`), the venue's fields (`venue.pitch.*`, `.stadium.*`,
  `.attendance`, `.stand[i].*`, `.section[i].*`, ...) and the attendance's
  copies in `fixture` and `fixture_extra` are named by write site or member
  (`labelVenueField` in `src/dll/labels.c`, sites in `src/dll/sites.h`).
- **Tactics:** each manager's tactic is under `<side>.manager.tactic`. It
  holds `takers`, the active `plan[0]` and a second `plan[1]`,
  `set_pieces`, and `formation[i]`. A plan names `.mentality` (1-7),
  `.team_instructions[0..2]` and each formation slot's `.slot[i].position`,
  `.slot[i].role` (role bit plus a duty bit) and
  `.slot[i].player_instructions[role].bits[0..1]`.
- **Members** (`--full`): most fields also carry `"member"`, the game object
  member the serialiser wrote, named as in the Windows build, where the
  original project found them in IDA: `MATCH_PLAYER_DATA+0x60`,
  `FIXTURE_RESULT+0x46`, ... Unnamed serialisers appear as `fn_<rva>+0xNN`;
  `STRUCT[element]` is an array element, `(computed)` a value the serialiser
  calculated (a count or flag) and `(constant)` a fixed byte. All of these are
  annotations; the encoder ignores them.
- **Ratings:** each player's ratings are a `u8map`: 15 positions (1-20),
  then 54 attributes (1-100 = in-game value x 5), named in
  `src/dll/labels.c`.

Values and strings (including their length) can be edited freely. Adding or
removing entries changes the structure and is not safe yet. Edited ratings
can be played as they are: the engine plays from the raw attributes, and the
values the game derives from them (ability, role ratings) don't affect
results.

# Known limits

- Only the Steam build 24.4.2 (2081827), both slices, is supported; the
  harness refuses any other ([macOS limits](MACOS.md#known-limits)). A new
  build needs a new mapping (see [tools/macos](../tools/macos/README.md)).
- With `--keep-build`, patching the seed in an input is not the game
  choosing that seed: pre-match data (attendance etc.) also depends on the
  build-time RNG.
- Rebuilding per seed needs the input to be the human manager's next
  fixture, in the same game state; once the match is played in the game,
  use `--keep-build`.
- One input = one fixture with fixed line-ups and tactics.
- A game-produced .bin and its JSON export differ in one header byte (offset
  21, a flag the deserialiser sets). Match results are identical.
- `get-next-fixture` (and every per-seed rebuild) sets one byte (`+0x46`) of the real
  fixture in the save and leave it set, as the game's own match setup
  probably does. The first build after loading a save therefore differs in
  that byte from later builds with the same seed. The byte before it,
  `FIXTURE_RESULT+0x45`, can change too.
- Building a match has the game pick the match's observers, the people
  attending to watch players (mostly national-team managers;
  `GAME_MATCH+0x720`, written by `GAME_MATCH__serialize_base`,
  `ObserverPart` in `src/dll/sites.h`), when its match-day options and the
  fixture allow. `GAME_MATCH__init_from_fixture` books most of them in the
  game's matchday manager (`[RVA_HUMAN_MATCHES_HOLDER]`, a list at
  `+0x170`) and passes over booked people in later builds until the game
  resets that list. The harness doesn't undo the bookings. So the first
  build of a pairing can list an observer that later builds don't, and
  which observers a seed's build lists depends on the builds before it,
  including the order in which worker threads reach them. Whether the
  engine reads the observers is not known. Per-seed rebuilds and team
  replacement leave the observers out when they compare an input with the
  game's build (`findEdits` in `seed_build.c`) and when they split a build
  into parts to align it with the input; they are never edits, so each
  build keeps its own.
- The game doesn't always list a team's players in the same order. A team's
  26 player slots (`MATCH_TEAM+0x180`; the count at `+0x3d2`) are filled on
  every build from the line-up the AI picks for an AI side, and for the
  human side before the team is submitted. Players it rates alike can then
  come out in either order: with another seed or opponent, and between a
  club's first build after the game starts and later builds, which agree
  with each other (4 of 13 clubs tried on one save, alike in both sessions
  checked). What the first build changes is not known; it is not the
  seeded RNG, nor a re-sort of the previous build's order. The
  order changes the match: two files that differ only in the order of two
  substitutes gave other results for each of 100 seeds, with the same
  spread overall. Per-seed rebuilds and team replacement therefore pair
  players by person: when the input is opened, its players are paired with
  those of the build of its own seed by their writes and then by how many
  records they share (`comparePlayers`), and each rebuild's players are
  paired with them by unique ID (`alignPlayers`). Edits, injected players
  included, follow each player, and a build that leaves out an edited
  player fails. A file exported from a club's first build keeps its order
  when played as it is (`keep` or `--keep-build`). References to a place in
  a list, such as the marked opponent's index, are not adjusted.
