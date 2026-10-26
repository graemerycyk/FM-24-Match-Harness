# Development

This repository, [graemerycyk/FM-24-Match-Harness](https://github.com/graemerycyk/FM-24-Match-Harness),
is a fork of
[PhilipArmstead/FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness),
the original Windows project by Philip Armstead, and adds the macOS port,
FM24 Viewer and macOS releases. It supports macOS only; for Windows, use
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
scripts/build.sh
```

This builds the C17 project in Release mode, with its regression tests, as
universal (arm64 and x86_64) binaries for macOS 11 and newer. `bin/`
holds `fm24ctl`, with the harness dylib embedded, `fm24_tests` and
`FM24Viewer.app`, the fixture viewer (macOS 13 and newer); all are stripped
and ad-hoc signed. Use `bin/fm24ctl`, from `bin/` (`cd bin`): it opens the
`FM24Viewer.app` beside it. `fm24_tests` needs the two stand-ins
next to it (`fm24_stand_in_game` and `libfm24_stand_in_library.dylib`),
which its launcher test starts in place of the game, the harness and Steam's
overlay. `build/` also holds the unstripped
`fm24-harness-<version>.dylib`, which `build/fm24ctl install` prefers to the
embedded copy, for working on the harness; unlike the embedded copy, its
x86_64 slice is not signed.

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
tests run `build/fm24_stand_in_fm24ctl` (`tests/viewer_stand_in_fm24ctl.c`)
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

`scripts/release_macos.sh` makes `dist/FM24-Harness-macOS-<version>.zip`,
with `fm24ctl`, `FM24Viewer.app` and a short `READ ME.txt`, and a SHA-256
file beside it. It builds afresh in `build/release-macos` (both slices, the
viewer for both), runs the `release` and `viewer_release` checks, signs the
viewer's own `fm24ctl`, the app and the loose `fm24ctl` with your Developer
ID and the hardened runtime, has Apple notarize them, and staples the
ticket to the app. A loose executable cannot hold a ticket, so Gatekeeper
checks `fm24ctl`'s online the first time it runs. The harness that
`fm24ctl` carries keeps its ad-hoc signature: it is data in `fm24ctl`, and
`fm24ctl` writes it to disk itself, so it is never quarantined.

You need a Developer ID Application certificate in your keychain
(`security find-identity -v -p codesigning` lists it) and, once, a notary
profile, which keeps your Apple ID's app-specific password in the keychain:

```sh
xcrun notarytool store-credentials fm24-notary --apple-id <your Apple ID> --team-id <TEAMID>
```

Then, from a clean checkout of the commit you release:

```sh
scripts/release_macos.sh --version v1.1.0-macos.1 \
  --identity "Developer ID Application: <Your Name> (<TEAMID>)" --notary-profile fm24-notary
```

`--identity -` signs ad hoc and skips notarizing, for a dry run of the
packaging. Before publishing, download the zip as a user would (for
example from a draft release), unzip it, and check that `FM24Viewer.app`
opens and `./fm24ctl status` runs without a Gatekeeper warning. Then
publish it with the zip and its `.sha256`:

```sh
gh release create v1.1.0-macos.1 dist/FM24-Harness-macOS-v1.1.0-macos.1.zip* \
  --target main --title "macOS v1.1.0-macos.1"
```

## Continuous integration

`.github/workflows/ci.yml` builds and tests on every push and pull request,
without the game. On macOS it runs `scripts/build.sh` with
`FM24_VIEWER=ON`, both slices' tests, `ctest`, `format.sh --check` and
`tidy.sh`.

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
slices:

```sh
scripts/format.sh            # Apply formatting; use --check to verify only
scripts/build.sh
scripts/tidy.sh              # Requires the build's compilation database
bin/fm24_tests
arch -x86_64 bin/fm24_tests  # Apple silicon with Rosetta: the x86_64 slice
ctest --test-dir build --output-on-failure
```

`ctest` also runs the release check.

To run a single test suite, pass its name, for example:

```sh
bin/fm24_tests fixture_data
```

The suites are `job`, `dll`, `fixture_data`, `runner`, `runner_main`,
`fixture_export`, `fixture_view`, `player_inject` and `macos`.
