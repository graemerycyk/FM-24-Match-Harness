# Development

This repository, [graemerycyk/FM-24-Match-Harness](https://github.com/graemerycyk/FM-24-Match-Harness),
is a fork of
[PhilipArmstead/FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness),
the original Windows project by Philip Armstead, and adds the macOS port,
FM24 Viewer and macOS releases, and an edition for Football Manager 27 (see
[Editions](#editions)). It supports macOS only; for Windows, use
the [original project](https://github.com/PhilipArmstead/FM-24-Match-Harness).
Work on `main`, and report problems and open pull requests in
[this repository](https://github.com/graemerycyk/FM-24-Match-Harness/issues).

## macOS

Build on macOS 11 or newer with the Xcode Command Line Tools, which include
Apple clang, Swift and Git, and CMake 3.20 or newer, for example from
[Homebrew](https://brew.sh). Ninja is used if it is installed, otherwise
Makefiles. The Command Line Tools also build the fixture viewer, which is
written in Swift; full Xcode works too (see
[The fixture viewer](#the-fixture-viewer)). A newly installed Xcode needs
its license accepted first, by opening it once or with
`sudo xcodebuild -license accept`.

```sh
xcode-select --install
brew install cmake
```

For contributions, also install Homebrew's LLVM for clang-format and
clang-tidy (22 or newer): `brew install llvm`. To use tools installed
elsewhere, set `CMAKE`, `NINJA`, `CLANG_FORMAT` or `CLANG_TIDY` (see
[`scripts/env.sh`](../scripts/env.sh)).

Then, in Terminal, clone the repository and build it, in a folder outside
Desktop, Documents and Downloads (see [Known limits](MACOS.md#known-limits)).
Only the clone and the installs above need the network:

```sh
git clone https://github.com/graemerycyk/FM-24-Match-Harness.git
cd FM-24-Match-Harness
FM_EDITION=24 scripts/build.sh
```

This builds the C17 project in Release mode, with its regression tests, as
universal (arm64 and x86_64) binaries for macOS 11 and newer, for FM24
(see [Editions](#editions)). `bin/`
holds `fm24ctl`, with the harness dylib embedded, `fm24_tests` and
`FM24Viewer.app`, the fixture viewer (macOS 13 and newer); all are stripped
and ad-hoc signed. Use `bin/fm24ctl`, from `bin/` (`cd bin`): it opens the
`FM24Viewer.app` beside it. `fm24_tests` needs the two stand-ins
next to it (`fm24_stand_in_game` and `libfm24_stand_in_library.dylib`),
which its launcher test starts in place of the game, the harness and Steam's
overlay; `fm27_tests` also needs those of its plugin test
(`fm27_stand_in_plugin`, `fm27_stand_in_other_plugin` and
`libfm27_stand_in_harness.dylib`; see [Editions](#editions)). `build/`
also holds the unstripped `fm24-harness-<version>.dylib`, which
`build/fm24ctl install` prefers to the embedded copy, for working on the
harness; unlike the embedded copy, its x86_64 slice is not signed.

The first build takes one to three minutes, most of them in the configure
step: it checks that Swift can build the viewer for each slice, which first
builds Swift's module cache (about a minute per slice), and on Apple silicon
it checks that Rosetta runs an x86_64 program, for the x86_64 tests (see
below). Later builds take seconds. Signing prints `replacing existing
signature` lines, and with the Command Line Tools alone the viewer's x86_64
link prints `ld: warning: ignoring file '.../libswiftCompatibilityPacks.a':
fat file missing arch 'x86_64'`; both are expected.

The tests are for working on the harness; using it needs none of them. Run
them natively and, on Apple silicon with Rosetta installed
(`softwareupdate --install-rosetta`), as x86_64:

```sh
bin/fm24_tests
arch -x86_64 bin/fm24_tests  # Apple silicon with Rosetta only
ctest --test-dir build --output-on-failure
```

`ctest` also runs the x86_64 tests (`x86_64` and `viewer_x86_64`) if the
configure step found that Rosetta runs an x86_64 program within ten
seconds, and says when it leaves them out. `FM24_ROSETTA_TESTS=OFF
scripts/build.sh` skips that check and those tests. Without them, `ctest`
has the `macos` suite run nothing under Rosetta either, as
`FM24_ROSETTA_TESTS=OFF bin/fm24_tests` does by hand. `ctest -E` leaves
out tests by name, for example `ctest --test-dir build -E '^x86_64$'`.

The generator (Ninja or Makefiles) is chosen when `build/` is first
configured; delete `build/` to switch.

### Editions

The same sources build the harness for two editions of the game, chosen
by `FM_EDITION` when `build/` is first configured:

- `24`, the FM24 version this guide describes, which the
  [releases](https://github.com/graemerycyk/FM-24-Match-Harness/releases)
  are built from: `FM_EDITION=24 scripts/build.sh`;
- `27`, for Football Manager 27 from Steam, the default on macOS on the
  `fm27` branch, where it is being developed. Its build names everything
  after FM27 (`fm27ctl`, `FM27Viewer.app`, `fm27_tests`, the
  `fm27-harness` folder, `launch-fm27.sh` and the `fm27-match-config/1`
  format), so it can be installed beside FM24's. Its harness finds the
  game's match engine as the game loads it, carries build tables for both
  slices of 27.0.0.2486716, the free preview, builds, loads and plays
  matches with that engine's calls, and names the fields of its exports
  ([Editing an FM27 fixture file](FIXTURE-FM27.md)). `fm27ctl` installs
  the harness and launcher for Steam's app 4530370, starts the game and
  loads saves with them as `fm24ctl` does for FM24, its `probe` reads
  the engine, and `FM27Viewer.app` shows FM27's tactics (see below).
  Nothing of this has run with the game yet. The [README](../README.md),
  the [FM27 guide](MACOS-FM27.md), [FM27 CLI differences](CLI-FM27.md),
  [Editing an FM27 fixture file](FIXTURE-FM27.md) and the
  [technical notes on FM27](TECHNICAL-FM27.md) describe it; the
  [FM24 README](../README-FM24.md) and the other guides describe FM24's.

FM27 keeps its match engine in a plugin bundle, `game_plugin.bundle` in
the app's `Contents/PlugIns`, which the game loads a few seconds after it
starts and unloads as it quits (`EDITION_ENGINE_PLUGIN` in `edition.h`).
So the FM27 harness serves jobs at once, reporting the game as `starting`
and refusing other jobs until it has found and checked the plugin, and
identifies the build by the plugin's `LC_UUID`. When the game unloads the
plugin, a job in progress is told to stop at its next seed, and the
unloading waits for it, for ten seconds at most; jobs are refused from
then on (`src/dll/game_macos.c`). `fm27ctl install --log-only` installs a
harness that only logs to `harness.log` how the game loads, identifies and
unloads its engine, and serves no jobs, for a first check with a game
build; any other install of the harness turns that off. Nothing of this
has run with the game yet.

FM27 is a Unity app whose executable, `Football Manager 27` in the
preview, has spaces in its name. `fm27ctl`, the launcher and the harness
take the game's executable to be the one its `Info.plist` names
(`CFBundleExecutable`, in a bundle with the game's identifier;
`src/common/info_plist.c`), and the edition's name where it names none, so
a build that renames it is refused for its build rather than left without
the harness. `fm27ctl probe` reads the engine plugin in the app's
`Contents/PlugIns`, given the app, its executable, the plugin's bundle or
the plugin itself, and prints the app's version, which a report of a build
it does not support needs too; every build `src/common/macos_builds.c`
lists has build tables, as the `macos` suite checks, so a slice it calls
supported is one the harness has tables for. The game's own code passes
its command line to the engine, which reads the auto-play file
`launch --save` gives it as the engine starts. If the started game reaches
its menu without the save, because the engine did not get the file or did
not load the save, `fm27ctl` says so and waits, within `--timeout`, for
the save to be loaded by hand; the commands that start the game for a
fixture's save do the same. The harness cannot read the loaded save's name
in FM27 (`GAME_UNLOCATED_SAVE_NAME`), so `fm27ctl` takes a loaded game to
have the save it asked for, and says so; and as FM27's exports have no
`save`, a command for a fixture without one waits while the running game
loads or works, then uses the save it has loaded, stopping if the game is
at its start screen or not running; it never starts the game for it
(`SessionNeed.unrecorded`). It looks for saves as `fm24ctl`
does, as `.fm` files in `cloud/games` and `games` of
`~/Library/Application Support/Sports Interactive/Football Manager 27`,
which has not been checked with the game either.

FM27's build tables (`src/dll/game_macos_tables_fm27.c`) hold a set per
mapped build and slice, which the harness picks by the plugin's UUID, and
its object layouts are in `src/dll/game_layout_fm27.h`;
[tools/macos](../tools/macos/README.md) describes the mapping they come
from, with the instructions that show each layout, what has not been
located in it, and how `tools/macos/remap.py` maps a new build from the last
one through that build's fingerprints. Where the edition's layouts lack something FM24's have
(`GAME_UNLOCATED_*`, `GAME_WEIGHT_FROM_BODY_TYPE`), the harness's code
does without it. The `macos` suite checks it with stand-ins: a
stand-in game (an executable named as FM27's, in a folder with spaces)
loads the harness, built with tables for a stand-in engine plugin, and
loads and unloads that plugin, another build of it and an unrelated
bundle as the test tells it. The stand-in plugin also stands in for the
engine's calls a run with `keep_build` makes, and notes how it is called,
so the test plays matches through the harness's own code: the seed
patched after FM27's header, the serialisation context, the manager flags
handed to the assistant, and the results and detailed statistics read in
FM27's layout. (A bundle with thread-local variables is never unloaded, so
the stand-in has none.) The stand-in engine loads a save in steps, as
FM24's load does and FM27's may: a game in progress at once, then the
people, on a thread of its own; and as in the game's builds, the stand-in
harness's start screen is the engine's `GAME` object, which exists before
any load. So the suite checks that the harness reports a load as
`loading`, and refuses jobs, until the load has settled, also when nothing
asks it meanwhile (`gameState_settled` in `src/dll/game_state.c`); the
`dll` suite checks that a gap between samples, or the engine's unloading,
starts the wait again, and that a game without people never settles. The
`match` suite checks the same calls, and the builder's, against fakes for
each edition.

The `macos` suite also drives `fm27ctl`'s own code against the stand-in
game from start to finish. In a home folder of its own, with the game in a
Steam library and the launcher in Steam's launch option: `install`; then
`launch --save`, whose start through Steam runs the launcher on the
stand-in game's app, so that the launcher inserts the stand-in harness
`install` put in place and hands the game the auto-play file, and the
stand-in engine, which the stand-in game loads as it starts, loads the
save the file names; then `status`, `list-teams`, `list-players`,
`get-player`, `inject-player`, `get-next-fixture` (the stand-in save has no
fixture, which the harness says), `encode` and `run`, keeping the build;
`run` on a fixture without a `save`, as FM27's exports are, once the game
is idle and again while it loads the save afresh, which `run` waits for;
and `replace-away` with a club's name, which the harness refuses, and with
an unknown team ID. Started again with its command line kept from its
engine, the game stays at its menu, `fm27ctl` asks for the save to be
loaded by hand, and the test has the stand-in game load it. Once the game
has quit, `run` on that fixture stops, saying to start FM27 and load the
save, and starts nothing. The stand-in engine stands in for neither
the match builder nor the serialiser, so exports, team replacement and
`decode` are checked against fakes only (the `runner`, `dll` and `match`
suites).

FM27's serialiser frames translate its write stacks into FM24's Windows
frames wherever it writes a field as FM24 does, so the labels, members,
per-seed rebuilds and team replacement work as in FM24; its own writes and
calls get synthetic IDs (`src/dll/sites_fm27.h` names the ones the labels
use), and a frame may depend on its two outer frames, not only the next.
`tools/macos/align/frames.py` makes the frames from FM24's by aligning the
two builds' serialisers ([tools/macos](../tools/macos/README.md#how-the-27002486716-frames-were-made)),
and needs both games' binaries, so it is not part of the tests. The `dll`
suite checks FM27's own labels (its new match part, its plans' slot
halves and its familiarity) on synthetic traces, `macos` checks that the
tables give every site of `sites.h` but those FM27 has not, and its own
named ones, and
`ctest`'s `align` test runs the Python tests of the frame derivation, of
what `gen_tables.py` refuses and of `remap.py`, which build two versions of
a small bundle with `clang++` for both slices and remap a mapping of the
first onto the second, and check that every FM27 mapping has fingerprints
that cover it.

FM27's tactics have two phases (`EDITION_TACTIC_PHASES` in `edition.h`):
each formation slot has a position and a role in possession and others out
of possession, and roles have no duties. So `FM27Viewer.app` draws two
pitches for each side, **In possession** and **Out of possession**, its
squad lists show each phase's position and role (**IP Position**, **IP
Role**, **OOP Position** and **OOP Role**) where FM24's show the position,
role and duty, and its headings give both formations, as `[IP 4-2-3-1, OOP
4-4-2]`. Which half of a slot is in possession is the labels' guess, and
FM27's role bits are not named yet, so a role shows as its bit (`Role 5`)
and positions as FM24's bits name them (see [Editing an FM27 fixture
file](FIXTURE-FM27.md#tactics)). Its six familiarity categories are not
named either, and how the game works out a tactic's setting for each is
not known, so the six familiarity columns (**Familiarity 0** to **5**)
show each category's best entry, how well the player knows the setting
they know best (`None` for a category with no entries), not the game's bar
against the tactic, and **Max familiarity** sets every entry's amount to
10000. Everything else in the viewer is FM24's, on the same labels: the
other Max buttons, the venue, the summary, simulation and replacements.
The C core holds these rules (`src/ui/fixture_view.c` and
`src/runner/fixture_data.c`, under `EDITION_TACTIC_PHASES`), and the
viewer's Swift asks it how many phases there are, so the same Swift builds
both viewers. `tests/fixture_samples.c` makes FM27's sample fixtures in its
label schema, with two phases and made-up familiarity, for the C suites and
the Swift tests. None of this has been compared with the game's screens.

The Windows build is FM24's only. `build/` keeps the edition it was first
configured for: to switch, delete it or configure another folder (`cmake
-S . -B build-24 -DFM_EDITION=24 -DFM24_BUILD_TESTS=ON`). The `FM24_*`
build settings keep their names in both editions.

Every name that tells the editions apart is in
[`cmake/edition.cmake`](../cmake/edition.cmake), which writes them into
`build/generated/edition.h` (from `src/common/edition.h.in`). The C
sources use its `EDITION_*` macros, the viewer's Swift imports the same
header as the module `Edition`, and CMake and the tests use the same
names. Sources never spell the names out, and their comments write
`fmNNctl`, `FMNN` and so on: `ctest`'s `edition` test fails on an edition
name anywhere under `src/`, `cmake/`, `scripts/` or `.github/` outside
the files that need one (it lists them), as well as on two editions
sharing a name. The tests themselves use the macros wherever the edition's
names appear in what they check, and the stand-ins' environment variables
carry the edition's prefix (`FM24_` or `FM27_`, as in
`FM27_ROSETTA_TESTS=OFF bin/fm27_tests`). The FM24 edition built from
these sources is the same, byte for byte, as before the editions existed.

`scripts/format.sh` formats the C sources under `src/` (`--check` only
reports), and `scripts/tidy.sh`, which needs the build's compilation
database, lints each architecture. It lints the `src/` sources the build
compiles, wherever the clone is, and never the tests. See the
[macOS guide](MACOS.md) for how the harness finds its way around the macOS
game, and [`tools/macos`](../tools/macos/README.md) for its build tables.

### The fixture viewer

`scripts/build.sh` builds the viewer, `bin/FM24Viewer.app`, alongside
`fm24ctl`. It runs on macOS 13 or newer and is built with Swift 5 language
mode. The Command Line Tools build both of its slices, under the rule
below, as full Xcode does with any SDK: it has been built with the Command
Line Tools 27.0 and with Xcode 27.1, and CI builds it with the runner's
Xcode. The build finds `swiftc` through `xcrun`, which uses the developer
folder that `xcode-select -p` prints, or the one `DEVELOPER_DIR` names:
with both installed, `DEVELOPER_DIR=/Library/Developer/CommandLineTools
scripts/build.sh` builds a new `build/` with the Command Line Tools (and
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` with Xcode)
without changing the selection; delete `build/` to switch. If `bin/` has
no `FM24Viewer.app` after a build, the configure step's warning says why.

`FM24Viewer.app` is an AppKit shell with SwiftUI views
(`src/ui/macos/`) over the viewer's rules in portable C
(`src/ui/fixture_view.h`, which Swift imports through
`src/ui/module.modulemap`) and the saves its Launch FM24 sheet lists
(`src/ui/save_list.h`, which follows `fm24ctl`'s rules for finding saves;
a test in the `macos` suite checks that both find the same ones in the
folders `tests/save_samples.c` makes, which the Swift tests list too).
`cmake/macos_viewer.cmake` runs `swiftc` once per slice and joins the
slices with `lipo`, so it works with Makefiles, Ninja and Xcode's generator
(`cmake -G Xcode`), which with Xcode 27.1 needs
`-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0`, as Xcode no longer accepts 11.0, and
`ctest -C Release`. The bundle
carries the same build's `fm24ctl`. The `FM24_VIEWER` setting (also read
by `scripts/build.sh`) chooses:

- `AUTO` (the default): build the viewer for the slices this toolchain can
  build, which a small Swift link per slice
  (`cmake/viewer_probe.swift`) finds when `build/` is configured, and warn
  about the rest;
- `ON`: fail unless every slice builds (CI does this);
- `OFF`: leave the viewer out.

Whenever the viewer is not built (`OFF`, or `AUTO` with no slice), a
`FM24Viewer.app` that an earlier build left in `build/` or `bin/` is
removed, so `fm24ctl` never opens an older copy.

Full Xcode builds every slice. The Command Line Tools also build both
slices for macOS 13, but only because the viewer uses none of the SwiftUI
and Foundation features that are macros in newer SDKs, whose plugins only
Xcode has: `@State`, `@Entry`, `@Animatable`, `@AnimatableIgnored`,
`@Previewable`, `#Preview`, `#Predicate`, `#Expression` and `#bundle`.
That is the rule for viewer sources: keep view state in small
`ObservableObject` models (`@StateObject`, or the model's own objects, as
the team editor and swap prompt are). `@Observable`, whose plugin the
Command Line Tools do have, is left out as well, because Observation needs
macOS 14. The `viewer_sources` test fails on all of these. The configure
step checks the toolchain again whenever the compiler's version or the SDK
changes, including an update that moves the Command Line Tools'
`MacOSX.sdk` link. With an older
toolchain the configure step's probe may build only some slices (it warns
and builds those) or none (it leaves the viewer out); install Xcode then.
Swift is formatted with `swift-format` and `.swift-format` (tabs, 100
columns), which `format.sh` does not run because its output differs between
Xcode releases:

```sh
xcrun swift-format format -i -r --configuration .swift-format \
  src/ui/macos tests/viewer
```

`ctest` runs the viewer's Swift tests (`viewer`, and `viewer_x86_64` under
Rosetta; `build/fm24viewer_tests`), which use its models without a window,
and `viewer_release`, which installs the bundle and checks its slices,
minimum macOS versions, signatures, libraries and document types. The Swift
tests run `build/fm24_stand_in_fm24ctl` (`tests/viewer_stand_in_cli.c`)
in place of `fm24ctl`, which never contacts the game (its `run` writes a
results CSV row by row, as slowly as a test asks; its `replace-home`,
`replace-away` and `inject-player` write a sample fixture, and its
`list-teams` and `list-players` a few made-up teams and players, with
lines the viewer skips; it plays `fm24ctl`'s check of a fixture's save,
refusing an input for another save or printing what `fm24ctl` prints as
it starts the game with one or waits for a busy game to be idle, and the
harness refusing other jobs while the game is busy; its `launch` prints
what `fm24ctl launch` prints as it starts the game through Steam, as
slowly as a test asks, or finds it running with a save, and its
`install` what `install` prints, writing nothing; all in `fm24ctl`'s own
words, which the `viewer_stand_in_texts` test finds in `fm24ctl`'s
sources, and line-buffered, as that test checks `fm24ctl`'s output is
too); `ctest` names it in `FM24CTL`, and the tests refuse to run any other
program. To run them by hand:

```sh
FM24CTL="$PWD/build/fm24_stand_in_fm24ctl" build/fm24viewer_tests
```

The app runs the `fm24ctl` that `FM24CTL` names in place of its own copy,
and exports the next fixture from the game when it starts without a
fixture, so when trying a build without the game, set `FM24CTL` to the
stand-in and pass a fixture. To check the window's layout without showing
it, render it offscreen to a PNG:

```sh
build/FM24Viewer.app/Contents/MacOS/fm24viewer --render-snapshot \
  next-fixture.json squads squads.png [light|dark] [1480x980] [edited] \
  [temporary] [busy] [failed] [simulating] [simulated] [replacing] \
  [filtered] [unclear]
```

The views are `squads`, `venue`, `simulation`, `team` (the home team's
picker, as its popover shows it), `swap` (the swap prompt for the first
away player, as its sheet shows it), `start` (no fixture: pass `-`),
`log`, `launch` (the Launch FM24 sheet over a few made-up saves in a home
folder of its own; pass `-`) and `install` (the Install Harness sheet
after a made-up install; pass `-`); `team`, `swap`, `launch` and `install`
are rendered at their own size unless one is given.
`edited` first selects the first three players of each side that can be
selected, maximises their happiness and condition where it can, and changes
the venue's capacity, to show edited cells and the unsaved-edit marker.
`temporary` shows the fixture as an export not yet saved; `busy` shows a
job of the game's running (on `start`, the startup export; over a fixture,
a decode), and `failed` the start panel after the startup export failed.
`simulating` shows a simulation of seeds 1-100 with 37 results in, and
`simulated` one that has finished; the snapshot copies the fixture into a
folder of its own first, so its results never land beside the fixture.
`replacing` shows the away team being replaced. `filtered` types into the
picker of `team` ("u") or `swap` ("ode"), and on `team`, `failed` shows a
refusal in red; their lists are a few made-up teams and players. On
`launch`, `busy` shows a launch under way, `failed` one that another
loaded save stopped and `unclear` the sheet when Steam's launch option sets
FM24's data folder in a way `fm24ctl` does not follow; on `install`,
`failed` shows a failed install.
Snapshots never run `fm24ctl` or look at the game. The window's background
is drawn behind the content, as on screen, so translucent controls such as
the tab bar come out as the window shows them.

### Making a macOS release

`scripts/release_macos.sh` makes a release of the edition that
`FM_EDITION` names, as for `scripts/build.sh` (27 by default, or 24):
`dist/FM27-Harness-macOS-<version>.zip`, with `fm27ctl`, `FM27Viewer.app`
and a short `READ ME.txt`, or for FM24 `dist/FM24-Harness-macOS-<version>.zip`
with `fm24ctl` and `FM24Viewer.app`, and a SHA-256 file beside it. It builds
afresh in `build/release-macos` (both slices, the viewer for both), runs the
`release` and `viewer_release` checks, signs the viewer's own CLI, the app
and the loose CLI with your Developer ID and the hardened runtime, has
Apple notarize them, and staples the ticket to the app. A loose executable
cannot hold a ticket, so Gatekeeper checks the CLI's online the first time
it runs. The harness that the CLI carries keeps its ad-hoc signature: it is
data in the CLI, which writes it to disk itself, so it is never
quarantined. `READ ME.txt` points to the quick start of the edition's
README: `README-<product>.md` if there is one, as `README-FM24.md` is for
FM24, otherwise `README.md`.

Do not release the FM27 edition until the [checks with the
game](MACOS-FM27.md#checks-with-the-game) have passed on the build it is
for.

You need a Developer ID Application certificate in your keychain
(`security find-identity -v -p codesigning` lists it) and, once, a notary
profile, which keeps your Apple ID's app-specific password in the keychain:

```sh
xcrun notarytool store-credentials fm24-notary --apple-id <your Apple ID> --team-id <TEAMID>
```

Then, from a clean checkout of the commit you release:

```sh
scripts/release_macos.sh --version v1.0.0 \
  --identity "Developer ID Application: <Your Name> (<TEAMID>)" --notary-profile fm24-notary
FM_EDITION=24 scripts/release_macos.sh --version v1.1.0-macos.2 \
  --identity "Developer ID Application: <Your Name> (<TEAMID>)" --notary-profile fm24-notary
```

`--identity -` signs ad hoc and skips notarizing, for a dry run of the
packaging. A dry run can also package a build you already have, as it is,
instead of building afresh: `--build build` names it (configured with
`FM24_VIEWER=ON`), and `--dist` another folder for the zip.
`ctest`'s `release_package` test makes such a dry run of its own build and
checks the zip: its name and contents, both slices, the signatures and the
`READ ME.txt`.

Before publishing, download the zip as a user would (for example from a
draft release), unzip it, and check that the viewer opens and `./fm27ctl
status` (or `./fm24ctl status`) runs without a Gatekeeper warning. Then
publish it with the zip and its `.sha256`, tagged `fm27-v<version>` for
FM27 and `v<version>-macos.<n>` for FM24:

```sh
gh release create fm27-v1.0.0 dist/FM27-Harness-macOS-v1.0.0.zip* \
  --target <the released commit> --title "FM27 macOS v1.0.0"
gh release create v1.1.0-macos.2 dist/FM24-Harness-macOS-v1.1.0-macos.2.zip* \
  --target main --title "macOS v1.1.0-macos.2"
```

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push and pull request,
without the game. On macOS it runs, for each [edition](#editions),
`scripts/build.sh` with `FM24_VIEWER=ON`, both slices' tests, `ctest`,
`format.sh --check` and `tidy.sh`.

The repository still contains the original project's Windows code, which
keeps the shared code mergeable with that project. It is not supported
here, but CI still builds and tests it so that the shared code keeps
compiling for Windows: with MSYS2 MinGW-w64 on Windows, and as a MinGW-w64
cross build on Linux, where only its release check runs. A change to the
shared code goes into the Windows build too, so keep its Windows behaviour
unchanged and check CI's Windows jobs on your push or pull request.

## Contribution guidelines

- Keep changes focused and add regression tests for changed behaviour.
- Use C17, existing C APIs and, in macOS code, POSIX and Mach; avoid
  unnecessary dependencies. Keep platform differences in
  `src/common/platform.h`, in `#ifdef __APPLE__` or `#ifdef _WIN32`
  blocks, or in the per-platform sources that `CMakeLists.txt` lists under
  `else()` for macOS and `if(WIN32)` for the Windows code. Check those
  lists, as file names do not always show the platform: macOS-only files
  have `_macos`, `_posix` or `macos_` in their names, apart from
  `channel.c`, and most Windows-only files have no suffix (`inject.c`,
  with `inject_macos.c` on macOS). Code outside them is shared with the
  Windows build (see [Continuous integration](#continuous-integration)).
- Follow `.clang-format`, `.clang-tidy` and `.editorconfig`: tabs for
  indentation, spaces for alignment, a 100-column code limit, attached
  braces and braces around control-flow bodies.
- Use `camelCase` for functions, variables and parameters, `CamelCase` for
  types, and `UPPER_CASE` for macros and enum constants. Follow existing
  `module_function` names for module APIs.
- Include the source file's own header first, then C library headers, then
  other project headers. Keep internal functions `static`.
- Prioritise runtime performance, then binary size. Reuse existing helpers
  and keep comments brief, explaining only non-obvious code.
- Add tests using `TEST(...)` and the assertions in `tests/test.h`, then
  register them in the suite's `register...Tests` function. Assertions do
  not stop execution, so guard failed setup before using its results.
- Keep compiler warnings and clang-tidy checks clean. Update relevant
  documentation when behaviour changes.

Before you open a pull request, run the checks, then the tests for both
slices, for each [edition](#editions) your change affects (shown for FM24;
for FM27, build in a clone of its own, or another build folder, and run
`bin/fm27_tests`):

```sh
scripts/format.sh            # Apply formatting; use --check to verify only
FM_EDITION=24 scripts/build.sh
scripts/tidy.sh              # Requires the build's compilation database
bin/fm24_tests
arch -x86_64 bin/fm24_tests  # Apple silicon with Rosetta: the x86_64 slice
ctest --test-dir build --output-on-failure
```

`ctest` also runs the release checks (`release`, and with the viewer
`viewer_release` and `release_package`), the `edition` check and, with
Python 3, the `tables` and `align` checks.

To run a single test suite, pass its name, for example:

```sh
bin/fm24_tests fixture_data
```

The suites are `job`, `dll`, `fixture_data`, `runner`, `runner_main`,
`fixture_export`, `fixture_view`, `player_inject` and `macos`.
