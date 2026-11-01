FM27 match-engine harness for macOS
===

Run Football Manager 27's own match engine headlessly inside the running
game on your Mac. Export a fixture, edit it, and simulate it across many
random seeds to compare tactics, players and match conditions.

**This edition has not been run with the game yet.** It builds, and its
tests pass against stand-ins for the game, but every address, object
layout and field name it uses in FM27 comes from reading the game's code,
not from running it. Until the [checks with the
game](docs/MACOS-FM27.md#checks-with-the-game) have passed, treat it as a
development version: back up your career first, and expect the harness to
refuse jobs, report wrong values or crash the game. There is no FM27
release yet; build it from source.

This is the FM27 edition of the macOS port of
[FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead, licensed under GPL-3.0-or-later. The same sources also
build the FM24 version, which is released and has been used with the game:
see [the FM24 README](README-FM24.md). On Windows, use the original project
instead: see
[its README](https://github.com/PhilipArmstead/FM-24-Match-Harness#readme).

It is made for FM27 from Steam. Its binaries are built for Apple silicon
and Intel Macs, but Intel Macs are not yet tested, and it has not been run
with the game at all yet. It has a command line, `fm27ctl`, and a native
viewer, FM27 Viewer, which exports, shows, edits and simulates fixtures,
and replaces teams and players. They do for FM27 what `fm24ctl` and FM24
Viewer do for FM24, under their own names, so both editions can be
installed side by side. The [FM27 guide](docs/MACOS-FM27.md) says what
differs. Written in C17, with the viewer in Swift.

## Features

As in the FM24 version:

- Export the human manager's next fixture as editable JSON or binary data,
  and turn binary data back into JSON.
- Inspect squads, stored tactics, player condition and venue details in
  FM27 Viewer, which draws each side's formation in possession and out of
  possession.
- Maximise selected players' morale, condition, sharpness and
  familiarity, and clear their fatigue, in the viewer.
- Replace players or teams, in the viewer or from the command line.
- Edit pitch, stadium and attendance values in the viewer.
- Run a single seed, a range or repeated seeds, with parallel simulation.
- Check whether the game is ready with `fm27ctl status`.
- Automate experiments through the CLI and annotated fixture JSON.

What FM27 changes, and so what differs from FM24 (see the
[FM27 guide](docs/MACOS-FM27.md#what-differs-from-fm24)):

- FM27 keeps its match engine in a plugin that it loads a few seconds after
  it starts. Until then `fm27ctl status` reports the game as `starting`.
  `fm27ctl install --log-only` sets up a harness that only logs how the
  game loads its engine, for a first check of a game build.
- The harness cannot yet tell when FM27 is processing, nor read the loaded
  save's name or the clubs' short names. So keep the game idle yourself,
  name teams by their unique ID, and check that the game has the save you
  want: exports record no save, so the commands for one use the save FM27
  has loaded (waiting while it loads), and do not start the game.
- FM27's tactics have a formation in possession and one out of possession,
  and roles without duties. Roles show by their number, and the six
  familiarity categories are not named yet (see
  [Editing an FM27 fixture file](docs/FIXTURE-FM27.md)).

## Requirements

- A Mac with macOS 11 or newer (macOS 13 or newer for FM27 Viewer). The
  binaries are built for Apple silicon and Intel, and their tests have
  passed on Apple silicon only, against stand-ins for the game: Intel Macs
  are not yet tested, and FM27 has not been run with the harness at all
  yet.
- Football Manager 27 from Steam, 27.0.0.2486716 (the free preview of
  October 2026). Compatibility is build-specific: the harness carries the
  tables of each FM27 build that has been mapped, and refuses others. A
  Steam update to a build that has not been mapped stops it until a new
  version supports that build (see [New game
  builds](docs/MACOS-FM27.md#new-game-builds)). The full release, due on 10
  November 2026, has not been mapped yet.
- For exporting and simulating: FM27 running with the harness and a save
  loaded, an upcoming human-managed fixture, and the game **idle**, not
  processing.
- To build it: the Xcode Command Line Tools and CMake (see
  [Building from source](#building-from-source)).

Existing JSON fixtures can be viewed and edited offline. Simulation, team
replacement and fetching players from the game require FM27.

## Quick start

These steps are also the first checks with the game: stop at the first
one that does not do what it says, and
[report it](#troubleshooting) with `harness.log`.

1. Build `fm27ctl` and `FM27Viewer.app` (see
   [Building from source](#building-from-source)), and open Terminal in
   `bin/` of the clone.
2. Back up your career: copy its save from
   `~/Library/Application Support/Sports Interactive/Football Manager 27`
   (its `games` or `cloud/games` folder) to a folder of your own, and keep
   that backup as it is.
3. Run `./fm27ctl install --log-only` and paste the launch option it prints
   into Steam: **Football Manager 27 > Properties > General > Launch
   Options**. Run it again until it says that Steam's launch option already
   uses this launcher, quitting Steam if it does not (see
   [Setting up](docs/MACOS-FM27.md#setting-up)). Start FM27 from Steam,
   wait for its main menu and quit it. `harness.log`, in
   `~/Library/Application Support/fm27-harness/`, should show the game
   loading its engine and the harness accepting the build, and the game
   should quit normally (see
   [the first check](docs/MACOS-FM27.md#the-first-check-the-harness-only-logs)).
4. Run `./fm27ctl install` without `--log-only`, which turns the harness's
   jobs on. Start FM27 from Steam, load your save and leave the game
   idle: `./fm27ctl status` prints `ok: ready …` once the harness has
   found the human manager's team, a few seconds after the save has
   loaded (until then it prints `ok: loading …`; see
   [The game's state](docs/MACOS-FM27.md#the-games-state)). For your own
   line-up and tactic, press **Continue** until the game is about to play
   your next match, as in FM24 (see
   [`get-next-fixture`](docs/CLI.md#export-the-next-fixture-get-next-fixture));
   do that before any harness work.
5. Export the next fixture and view it in FM27 Viewer:

   ```sh
   ./fm27ctl get-next-fixture next-fixture.json
   ./fm27ctl next-fixture.json
   ```

6. Simulate it with the viewer's **Simulation** tab, or from Terminal:

   ```sh
   ./fm27ctl run next-fixture.json 1-10 results.csv --detailed
   ```

7. When you are done, quit FM27 without saving, as harness work leaves
   changes in the game's memory. Do not press **Continue** or play the
   match in that session either, in case FM27 autosaves the loaded save as
   FM24 does.

`./fm27ctl launch --save <save>` is meant to start the game and load a save
for you, as in FM24, but whether FM27 passes its command line on to its
engine has not been checked: if the game reaches its menu without the save,
`fm27ctl` says so and waits for you to load the save yourself (see
[Loading a save](docs/MACOS-FM27.md#loading-a-save)).

### Building from source

You need the Xcode Command Line Tools (`xcode-select --install`, which
also installs Git) and CMake 3.20 or newer, for example with
[Homebrew](https://brew.sh): `brew install cmake`. Clone and build in a
folder outside Desktop, Documents and Downloads, on the branch with the
FM27 edition, then go on from step 2 above in `bin/`, where the build puts
`fm27ctl` and `FM27Viewer.app`. The first build takes one to three minutes.
See [Development](docs/DEVELOPMENT.md#macos) for the details, and
[Editions](docs/DEVELOPMENT.md#editions) for building FM24's instead.

```sh
git clone -b fm27 https://github.com/graemerycyk/FM-24-Match-Harness.git
cd FM-24-Match-Harness
scripts/build.sh
cd bin
```

### View a fixture

FM27 Viewer works as FM24 Viewer does (see the
[macOS guide](docs/MACOS.md#the-fixture-viewer)), with FM27's names: it
opens `.json` fixtures offline and has the game decode `.bin` ones,
exports the next fixture when opened without one, and has the
**Simulation** tab, the team picker, **Swap player**, **File > Launch
FM27…** and **File > Install Harness…**. Each side has two pitches, **In
possession** and **Out of possession**, and the squad lists show each
phase's position and role. Which half of FM27's tactic is in possession,
its roles, and its familiarity categories are the labels' guesses until an
export has been compared with the game's screens (see
[The viewer](docs/MACOS-FM27.md#the-viewer)).

### Command line

From Terminal in `bin/` of a build:

```sh
./fm27ctl get-next-fixture next-fixture.json
./fm27ctl run next-fixture.json 1-100 results.csv --detailed
```

`fm27ctl` has `fm24ctl`'s commands, flags, outputs and exit codes: see the
[CLI reference](docs/CLI.md), and [FM27's
differences](docs/CLI-FM27.md) from it.

## Troubleshooting

The harness loads only when the game starts, so start FM27 from Steam with
the launch option, or with `./fm27ctl launch`, after each `install`. Keep
the game idle while commands run: the harness cannot yet tell when FM27 is
processing. `./fm27ctl probe` says whether this harness supports the
installed build; an unsupported build is refused at startup, and every job
then says so.

For failures, check `fm27ctl`'s output (in FM27 Viewer, **View > Show
Log**) and `harness.log` in
`~/Library/Application Support/fm27-harness/`, and FM27's own log,
`~/Library/Logs/Sports Interactive/Football Manager 27/Player.log`.

[Report a bug or suggest a feature](https://github.com/graemerycyk/FM-24-Match-Harness/issues)
in this repository, saying it is about FM27. Include the harness version
(`./fm27ctl install` prints it), the game's version and `./fm27ctl probe`'s
output, the steps you took, `harness.log` and `Player.log`, and for a crash
the report in `~/Library/Logs/DiagnosticReports` if there is one.

## Development and contributions

See the [development guide](docs/DEVELOPMENT.md) for prerequisites,
cloning, building, tests and code-style guidelines, the
[technical notes on FM27](docs/TECHNICAL-FM27.md), and
[`tools/macos`](tools/macos/README.md) for the build mappings and how a new
FM27 build is mapped. Keep contributions focused and add regression tests
for changed behaviour.

The repository keeps the original project's Windows source, so that the
shared code stays mergeable with it. The Windows version is not supported
here: for it, use the
[original project](https://github.com/PhilipArmstead/FM-24-Match-Harness).

## License

The source code is licensed under
[GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html), as indicated
by its SPDX headers. It is a port of
[FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead, under the same licence.
