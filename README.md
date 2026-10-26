FM24 match-engine harness for macOS
===

Run Football Manager 24's own match engine headlessly inside the running
game on your Mac. Export a fixture, edit it, and simulate it across many
random seeds to compare tactics, players and match conditions.

This is the macOS port of
[FM-24-Match-Harness](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead, licensed under GPL-3.0-or-later. On Windows, use the
original project instead: see
[its README](https://github.com/PhilipArmstead/FM-24-Match-Harness#readme)
and download it from
[its releases](https://github.com/PhilipArmstead/FM-24-Match-Harness/releases/latest).

It works with FM24 from Steam and has been tested on Apple silicon; Intel
Macs are not yet tested. It has a command line, `fm24ctl`, and a native
viewer, FM24 Viewer, which exports, shows, edits and simulates fixtures,
and replaces teams and players (see the [macOS guide](docs/MACOS.md)).
Results are saved as CSV files, with optional detailed player and team
statistics. Written in C17, with the viewer in Swift.

## Features

- Export the human manager's next fixture as editable JSON or binary data,
  and turn binary data back into JSON.
- Inspect squads, stored tactics, player condition and venue details in
  FM24 Viewer.
- Maximise selected players' morale, condition and sharpness, and clear
  their fatigue, in the viewer.
- Replace players or teams, in the viewer or from the command line.
- Edit pitch, stadium and attendance values in the viewer.
- Run a single seed, a range or repeated seeds, with parallel simulation.
- Check whether the game is ready with `fm24ctl status`.
- Automate experiments through the CLI and annotated fixture JSON.

## Requirements

- A Mac with macOS 11 or newer (macOS 13 or newer for FM24 Viewer). The
  binaries are built for Apple silicon and Intel, and have been tested on
  Apple silicon only: one M1 Mac with macOS 27. Intel Macs are not yet
  tested.
- FM24 24.4.2 (build 2081827) from Steam. Compatibility is build-specific:
  only this build is supported for now, and others are refused. Epic Games
  support is coming soon, and GOG support will follow if FM24 is released
  there.
- For exporting and simulating: FM24 running with the harness and a save
  loaded, an upcoming human-managed fixture, and the game **idle**, not
  processing.

Existing JSON fixtures can be viewed and edited offline. Simulation, team
replacement and fetching players from the game require FM24.

## Quick start

You need FM24 24.4.2 from Steam and a Mac with macOS 11 or newer (macOS 13
or newer for FM24 Viewer).

1. Download `FM24-Harness-macOS-<version>.zip` from the
   [latest release](https://github.com/graemerycyk/FM-24-Match-Harness/releases/latest).
   It is signed and notarized, so macOS opens it without a security
   warning. Unzip it, move the `FM24-Harness-macOS-<version>` folder out
   of Downloads to your home folder (keep it outside Desktop, Documents and
   Downloads), and open Terminal in it:

   ```sh
   cd ~/FM24-Harness-macOS-<version>
   ```

   The folder holds `fm24ctl`, the command line, and `FM24Viewer.app`, the
   fixture viewer. To build them from source instead, see
   [Building from source](#building-from-source) below.
2. Run `./fm24ctl install` and paste the launch option it prints into
   Steam: **Football Manager 2024 > Properties > General > Launch
   Options**. Then run `./fm24ctl install` again: once Steam has saved the
   option, it says that Steam's launch option already uses this launcher.
   If it does not, quit Steam, which saves its settings when it quits, and
   run it again (see [Setting up](docs/MACOS.md#setting-up)).
3. Before your first session, back up your career: copy its `.fm` file
   from `~/Library/Application Support/Sports Interactive/Football Manager 2024/cloud/games`
   (or `games` beside it) to a folder of your own, and keep that backup as
   it is. Start FM24 from Steam and load your save. For your own line-up
   and tactic, press **Continue** until the button shows **Go to Match**
   for your next match: until then the game picks your side as the AI
   would (see
   [`get-next-fixture`](docs/CLI.md#export-the-next-fixture-get-next-fixture)).
   Do this, and save if you like, before any harness work: it leaves
   changes in the game's memory, so after it you will quit without saving.
   Then leave the game idle: `./fm24ctl status` prints `ok: ready …` once
   it is.
4. Export the next fixture and view it in FM24 Viewer:

   ```sh
   ./fm24ctl get-next-fixture next-fixture.json
   ./fm24ctl next-fixture.json
   ```

   Opening `FM24Viewer.app` on its own also exports and shows the next
   fixture. If the export ends with a `warning:` that the AI picked your
   line-up, quit FM24 without saving and start again from loading your save
   in step 3.
5. Simulate it with the viewer's **Simulation** tab, or from Terminal:

   ```sh
   ./fm24ctl run next-fixture.json 1-100 results.csv --detailed
   ```

6. When you are done, quit FM24 without saving, as harness work leaves
   changes in the game's memory. Do not press **Continue** or play the
   match in that session either: FM24 then autosaves the loaded save, with
   those changes. Load your save again to play on.

The tested way to start the game is through the launch option. Once the
option is set, `./fm24ctl launch` starts the game through Steam. Without
it, or given a path to `fm.app`, `./fm24ctl launch` opens the game
directly, which has not been verified: in its one attempt the game crashed
about four seconds after loading the harness. To have `fm24ctl` start the
game and load a save for you, see
[Working headless](docs/MACOS.md#working-headless). The
[macOS guide](docs/MACOS.md) has the details, and
[Files and removal](docs/MACOS.md#files-and-removal) lists every file the
harness adds and how to remove it.

### Building from source

You need the Xcode Command Line Tools (`xcode-select --install`, which
also installs Git) and CMake 3.20 or newer, for example with
[Homebrew](https://brew.sh): `brew install cmake`. Clone and build in a
folder outside Desktop, Documents and Downloads, then go on from step 2
above in `bin/`, where the build puts `fm24ctl` and `FM24Viewer.app`. The
first build takes one to three minutes. See
[Development](docs/DEVELOPMENT.md#macos) for the details.

```sh
git clone https://github.com/graemerycyk/FM-24-Match-Harness.git
cd FM-24-Match-Harness
scripts/build.sh
cd bin
```

### View a fixture

FM24 Viewer, opened without a fixture or through `./fm24ctl` with no
arguments, exports the next fixture and shows it. It also opens `.json`
and `.bin` fixtures: run `./fm24ctl next-fixture.json`, drop one on the
app or its window, or use **File > Open**. JSON opens without the game;
binary fixtures need FM24 running and idle. Opening a file does not run a
simulation or overwrite it.

The viewer shows the fixture's summary, squads, pitch and venue values,
with player buttons, venue editing and an export link, and **Save** writes
the fixture with its edits as JSON or binary. Its **Simulation** tab runs
the fixture over the seeds you choose, showing progress on the Dock icon,
and saves the results beside the fixture; results of a fixture not yet
saved go to its export folder. The blue pencil after a team's heading
opens a team picker to replace that team, and **Swap player** finds
another player by name or unique ID for the selected one; both need FM24
running and idle. **File > Launch FM24…** starts the game with the harness
and a save you choose and, with no fixture open, shows the next fixture,
and **File > Install Harness…** installs the harness and gives you the
Steam launch option to copy. Until Steam has saved that option, the viewer
says to set it first, as **Launch FM24…** would then open the game
directly, which has not been verified. Once `./fm24ctl install` says that
Steam has the option, `open -a ./FM24Viewer.app --args --launch-save <your
save's name>`, run in the release folder (or `bin/` in a build), is meant
to do the launch and export in one step, but has not been tried with the
game yet; before then, it would open the game directly without asking.
Quit a session that either of them started without saving. Press
**Continue** in it only with a second copy of your save under a new name,
such as `~/fm24-sandbox/LIV_COPY.fm`, not your backup: FM24 autosaves the
save it loaded (see [Loading a save](docs/MACOS.md#loading-a-save)), and
`fm24ctl` refuses a copy with the name of a save in FM24's folders. Choose
the copy with **Other…** in **Launch FM24…**, or give `--launch-save` its
absolute path.
See the [macOS guide](docs/MACOS.md#the-fixture-viewer) for all its
controls, and [Development](docs/DEVELOPMENT.md#the-fixture-viewer) for
building it.

The viewer shows stored fixture data, not a live match or tactical editor.
Use the [fixture editing guide](docs/FIXTURE.md) for manual JSON edits;
change values without adding, removing or reordering fields.

### Command line

From Terminal in the release folder, or in `bin/` of a build:

```sh
./fm24ctl get-next-fixture next-fixture.json
./fm24ctl run next-fixture.json 1-100 results.csv --detailed
```

This runs 100 matches and writes `results.csv`, `results-players.csv` and
`results-teams.csv`. By default, the AI handles in-match changes for both
teams.

Explicit numeric seeds rebuild the fixture in the current game state and
reapply your edits. Keep the exported fixture as the manager's upcoming
match. For older fixtures, `--keep-build` skips rebuilding, but leaves
pre-match values such as attendance and weather unchanged.

See the [CLI reference](docs/CLI.md) for commands, flags, output formats and
more examples.

## Troubleshooting

Keep FM24 idle and avoid clicking around in the game during simulations.

The harness loads only when the game starts. If a command reports that the
game is running without the harness, or has another harness version
loaded, quit the game and start it again through the Steam launch option
or with `./fm24ctl launch`. A rebuilt harness with the same version number
is not picked up until the game restarts either; until then, commands use
the copy the game loaded. Run `./fm24ctl install`, then quit the game and
start it again. Start Steam before opening the game directly with
`./fm24ctl launch`. If macOS asks whether Football Manager 2024 (or Steam)
may access a folder, the game is writing a command's output there: allow
it, or run the commands from a folder outside Desktop, Documents and
Downloads.

For failures, check `fm24ctl`'s output (in FM24 Viewer, **View > Show
Log**) and `harness.log`, normally at
`~/Library/Application Support/fm24-harness/harness.log`. Unsupported game
builds are refused.

[Report a bug or suggest a feature](https://github.com/graemerycyk/FM-24-Match-Harness/issues)
in this repository. For the Windows version, use the original project's
[issues](https://github.com/PhilipArmstead/FM-24-Match-Harness/issues).
For bugs, include the harness version (`./fm24ctl install` prints it), the
game version, reproduction steps, and relevant `fm24ctl` or log output.
For crashes, include the game's crash report from
`~/Library/Logs/DiagnosticReports` if there is one.

## Development and contributions

See the [development guide](docs/DEVELOPMENT.md) for prerequisites, cloning,
building, tests and code-style guidelines. Keep contributions focused and
add regression tests for changed behaviour.

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
