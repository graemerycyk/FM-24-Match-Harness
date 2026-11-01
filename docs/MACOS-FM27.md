FM27 guide
===

The FM27 edition of the harness is made for **Football Manager 27 from
Steam**, 27.0.0.2486716 (the free preview of October 2026). Its binaries
are built for Apple silicon and Intel Macs, but Intel Macs are not yet
tested, and the edition has not been run with the game at all yet (see
below). It is built from the same sources as the FM24 version (see
[Editions](DEVELOPMENT.md#editions)) and is meant to do for
FM27 what that version does for FM24: `fm27ctl` has `fm24ctl`'s commands,
and FM27 Viewer has FM24 Viewer's controls. So the
[macOS guide](MACOS.md), the [CLI reference](CLI.md) and
[Editing a fixture file](FIXTURE.md) apply to FM27 too, with the names in
the table below. This guide covers what is
different, how to set the harness up and check it with the game, and what
has and has not been checked.

**None of the FM27 edition has run with the game yet.** Its tests pass
against stand-ins for the game, Steam and the engine, and its tables have
been checked against the game's files, but every address, object layout
and field name it uses comes from reading FM27's code, not from running it.
Until the [checks with the game](#checks-with-the-game) have passed, use it
on a copy of a save, with your career backed up, and expect it to refuse
jobs, report wrong values or crash the game.

| | FM24 | FM27 |
|---|---|---|
| Game | Football Manager 2024 24.4.2, Steam app 2252570 | Football Manager 27 27.0.0.2486716, Steam app 4530370 |
| Command line | `fm24ctl` | `fm27ctl` |
| Viewer | `FM24Viewer.app`, FM24 Viewer | `FM27Viewer.app`, FM27 Viewer |
| Harness folder | `~/Library/Application Support/fm24-harness/` | `~/Library/Application Support/fm27-harness/` |
| Harness, socket | `fm24-harness-<version>.dylib`, `.sock` | `fm27-harness-<version>.dylib`, `.sock` |
| Launcher | `launch-fm24.sh` | `launch-fm27.sh` |
| Game's data folder | `Sports Interactive/Football Manager 2024` | `Sports Interactive/Football Manager 27` |
| Fixture and player formats | `fm24-match-config/1`, `fm24-player/1` | `fm27-match-config/1`, `fm27-player/1` |
| Viewer's export folder | `~/Documents/FM24 Match Harness` | `~/Documents/FM27 Match Harness` |
| Releases | `FM24-Harness-macOS-<version>.zip` | `FM27-Harness-macOS-<version>.zip` (none yet) |

The two editions share no file, folder, socket, launch option or setting.
Even the files they keep in your temporary folder have names of their own:
the next fixture that `fm27ctl` with no arguments and FM27 Viewer export
is `fm27-temp-fixture.json` (FM24's is `temp-fixture.json`), and a job's
input is `fm27-job-input-<pid>-<n>.bin` (FM24's is
`_job_input-<pid>-<n>.bin`). Only one game runs under a Steam account
at a time, so they do not get in each other's way.

## What differs from FM24

- **The match engine is a plugin.** FM27 is a Unity app, `fm.app`, whose
  executable is `Football Manager 27`; the match engine and database are in
  a plugin, `Contents/PlugIns/game_plugin.bundle`, which the game loads
  about two seconds after it starts. The harness, loaded as the game starts,
  serves jobs at once, but until the game has loaded the plugin and the
  harness has checked it, `status` reports the game as `starting` and other
  jobs are refused (`FM27 is busy (starting)`). When the game unloads the
  plugin as it quits, a job in progress stops at its next seed. See
  [How it works](#how-it-works).
- **Builds are told apart by the plugin.** The harness and `fm27ctl probe`
  read the UUIDs of `game_plugin`'s slices, not of the game's executable,
  which is Unity's and the same in every build. `probe` takes the app, its
  executable, the plugin's bundle or the plugin itself, and prints the
  app's version too (see [`probe`](CLI-FM27.md#probe)).
- **A first check that only logs.** `fm27ctl install --log-only` installs a
  harness that logs how the game loads, identifies and unloads its engine,
  and serves no jobs (see [The first check](#the-first-check-the-harness-only-logs)).
- **What the harness cannot read yet.** Three things the FM24 harness reads
  have not been found in FM27's code:
  - whether the game is processing (pressing **Continue**), so the harness
    never reports FM27 as `busy`: keep the game idle yourself while
    commands run. A day whose processing adds or removes people shows as
    `loading` for a few seconds (see [The game's state](#the-games-state));
    how often days do that has not been checked (check 3 of the
    [checks with the game](#checks-with-the-game));
  - the loaded save's name, so `status` and exports give no save; the
    commands for an export wait while FM27 loads or works, then take the
    save it has loaded to be the export's (they stop if FM27 is at its
    start screen or not running), and the commands that check a
    fixture's save take a loaded game to have the save they want, saying
    so either way (see [Loading a save](#loading-a-save));
  - the clubs' short names, so `list-teams` writes `?` for each team's name
    and `list-players` for each player's club, an export's `source` and
    `--detailed`'s team statistics name the teams `?` (the teams' own
    blocks in the fixture still hold their names), and `replace-home`,
    `replace-away` and the viewer's team picker take a team's unique ID
    only: the harness refuses a club's name, saying so.
- **Tactics have two phases.** Each formation slot has a position and a role
  in possession and others out of possession, and roles have no duties.
  Fixtures name the halves `slot[i].ip` and `slot[i].oop`; which of them is
  in possession is a guess. Roles and the six tactical familiarity
  categories are numbered, not named (see
  [Editing an FM27 fixture file](FIXTURE-FM27.md)).
- **A player's weight** is worked out as FM27's match builder does, from the
  player's body type and height, as FM27's database keeps no weight.
  `get-player` writes it as `weight`.
- **Fixtures play in builds with their format version.** A serialised match
  starts with its format version (0x004c in 27.0.0.2486716). The harness
  finds the seed only in a match of the running build's format version, so
  a fixture exported from another build with another format version is
  refused.

Everything else should work as in FM24, but none of it has been checked
with the game: the commands and their flags, outputs and exit codes,
per-seed rebuilds and `--keep-build`, detailed statistics, team and player
replacement, the viewer's controls, and the launcher and `launch
--rosetta`. Per-seed rebuilds, `replace-home` and `replace-away` have not
even run against the stand-in engine, only against fakes of the engine's
match builder and serialiser (see [What has been checked](#what-has-been-checked)).

## Setting up

1. Build `fm27ctl` and `FM27Viewer.app` in a clone outside Desktop,
   Documents and Downloads (see the README's
   [Building from source](../README.md#building-from-source)), then run
   `cd bin` in it. There is no FM27 release yet.
2. Back up your career: copy its save from `games` or `cloud/games` in
   `~/Library/Application Support/Sports Interactive/Football Manager 27`
   to a folder of your own, and keep that backup as it is. FM27's saves
   have not been looked at yet: `fm27ctl` looks for them as `.fm` files in
   those folders, as `fm24ctl` does in FM24's.
3. Run `./fm27ctl install --log-only` for the first check, or
   `./fm27ctl install` once it has passed. Either writes the harness and
   `launch-fm27.sh` to `~/Library/Application Support/fm27-harness/`,
   finds FM27 in your Steam libraries and prints the launch option to use:

   ```text
   /bin/sh "/Users/you/Library/Application Support/fm27-harness/launch-fm27.sh" %command%
   ```

4. In Steam, open **Football Manager 27 > Properties > General > Launch
   Options**, paste the line, keeping any arguments of your own after
   `%command%`, and run `./fm27ctl install` again until it says that
   Steam's launch option already uses this launcher, quitting Steam if it
   does not, as for FM24 (see [Setting up](MACOS.md#setting-up)). The
   game's block in `localconfig.vdf` is `"4530370"`.
5. Start FM27 from Steam. With `--log-only`, quit it once it has reached its
   main menu and read `harness.log` (see below). Otherwise load a copy of
   your save, press **Continue** until your next match is due, as in FM24
   (see [`get-next-fixture`](CLI.md#export-the-next-fixture-get-next-fixture)),
   leave the game idle, and use the commands:

   ```sh
   ./fm27ctl status
   ./fm27ctl get-next-fixture next-fixture.json
   ./fm27ctl next-fixture.json
   ./fm27ctl run next-fixture.json 1-10 results.csv --detailed
   ```

6. Quit FM27 without saving when you are done, and do not press
   **Continue** or play the match in that session: harness work leaves
   changes in the game's memory, and FM24 autosaves the save it loaded
   when **Continue** moves the game on, which FM27 may do as well.

`fm27ctl launch` starts the game through Steam once Steam has the launch
option, and otherwise opens `fm.app` directly, which, as for FM24, has not
been verified. The launcher starts the executable that the app's
`Info.plist` names (`CFBundleExecutable`), which has spaces in its name.

## The first check: the harness only logs

`./fm27ctl install --log-only` writes a file, `log-only`, next to the
harness, with a line saying which command wrote it. While it is there, the
harness only logs to `harness.log`: it never opens its socket, so every
command finds no harness serving. Start FM27 from
Steam, wait for its main menu and quit it. The log should read like this
(with times before each line, your folders, and on an Intel Mac or under
Rosetta the x86_64 slice):

```text
harness 1.3.0 loaded (pid 4321)
starting: waiting for Football Manager 27 to load its engine, game_plugin.bundle/Contents/MacOS/game_plugin
log-only mode (/Users/you/Library/Application Support/fm27-harness/log-only): logging the engine's loading and unloading, and serving no jobs
found Football Manager 27's engine (the game's version 27.0.0.2486716): /Users/you/Library/Application Support/Steam/steamapps/common/Football Manager 27/fm.app/Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin
game build: Football Manager 27 27.0.0.2486716, arm64 (UUID 9E8E47DE-F704-314C-917D-C32D12637678)
accepted (log-only mode: serving no jobs)
Football Manager 27 unloaded its engine
gone (log-only mode: serving no jobs)
```

- `accepted` means the build is supported and the code the harness calls is
  where its tables say. `refusing: ...` lines before `refused` say why not:
  an unsupported build's UUID, or code that differs from the tables.
- The game may quit without unloading its engine, so the last two lines may
  be missing. That is fine as long as the game quits normally; a crash or a
  hang at quit is not.
- No `found` line means the harness did not see the plugin load: send the
  log and `Player.log`.

Any other `install` (also `launch`, and a command that finds no harness)
removes `log-only`, so the next start serves jobs.

## Loading a save

`fm27ctl launch --save <save>`, and the commands that start the game for a
fixture that names its save (FM27's exports do not, as below), work as in
FM24 (see [Working headless](MACOS.md#working-headless)
and [`launch --save`](CLI.md#load-a-save-launch---save)): the launcher
passes the game `--auto_play_game_file=<file>`, whose `LOAD_GAME` names the
save. FM27's engine reads that file as it starts, but it gets its command
line from the game's Unity code, which has not been checked to pass the
argument on. So if the game reaches its menu without loading the save,
`fm27ctl` says why (it did not read the auto-play file, or it read it and
did not load the save) and waits, within `--timeout`, for you to load the
save in the game yourself; the command then goes on. Load the copy that
`fm27ctl` names, as it cannot check which save the game loaded.

As the harness cannot read the loaded save's name in FM27, exports have no
`save`, so `fm27ctl` cannot tell which save an export is for, or start
FM27 with it. Before its job for a JSON fixture without a `save`, as
FM27's exports are, `run`, `replace-home`, `replace-away` and
`inject-player` with a person ID check that FM27 is running with the
harness and has a save loaded and idle, whichever it is, which they take
to be the fixture's. So do FM27 Viewer's simulation, team replacement and
player swap, which run them:

| FM27 | The command | Exit code |
|---|---|---|
| Has a save loaded and idle | Says it takes that save to be the fixture's, and submits its job | |
| Is loading | Waits until it is idle, as for a fixture's save, then submits its job | `3` after 15 minutes |
| Has no save loaded | Stops: load the fixture's save in FM27, then run the command again | `3` |
| Is not running | Stops, saying to start FM27 and load the fixture's save first | `1` |
| Runs without this harness version | Submits the job as before, which says why it fails | `1` |

While another runner's job holds the harness, the check waits for it as
long as the command's own job would, as in FM24. When FM27 is not running,
the command prints:

```text
Football Manager 27 is not running: start it with the harness, load the fixture's save and leave the game idle, then run this again. FM27 exports do not record their save, so fm27ctl cannot start the game with it.
```

A command on such a fixture never starts FM27, so `--no-launch` has
nothing to stop, and as any loaded save is taken, `--any-save` has nothing
to allow. `run` warns, as in FM24, if the loaded save is at another
in-game date than the fixture's `game_date`; that is the only sign of the
wrong save, so make sure FM27 has the fixture's save loaded. A `.bin`
input is not checked, as in FM24, and a fixture with a `save` you have
added is checked for that save, as above.

## The game's state

`fm27ctl status` prints the same line as `fm24ctl status` (see
[`status`](CLI.md#check-the-games-state-status)), with these differences:

- `starting` until the game has loaded its engine and the harness has
  accepted it; with an unsupported build, `status` fails, saying so.
- FM27 has no start screen in its engine to say when a load has ended. The
  engine creates its game object as it starts, before it reads the
  auto-play file, and may, as FM24 does, mark a game in progress before it
  has finished loading it. So `menu_seen` is `1` once the game object
  exists, and while a game is in progress, only once the game has
  settled: until the in-progress flag and the number of people in the
  game's database have held for three seconds, the state is `loading`,
  `menu_seen` is `0` and jobs are refused (`FM27 is busy (loading)`). The
  harness samples them every second between jobs, so a game that has
  settled is `ready` at once. As nothing is sampled while a job runs or
  the Mac sleeps, a sample more than two seconds after the last starts the
  three seconds again, so the game is `loading` for a few seconds after a
  long job; and a game in progress with no people is `loading` however
  long it stays so. Whether three seconds is long enough is one of the
  [checks with the game](#checks-with-the-game).
- `processing` is always `0` and `kind` `-1`, and the state is never
  `busy`: the processing flag has not been found. A day's processing that
  changes the number of people shows as `loading`.
- There is no `save`.

## The viewer

FM27 Viewer works as FM24 Viewer does (see
[The fixture viewer](MACOS.md#the-fixture-viewer)), under FM27's names
(**File > Launch FM27…**). For FM27's tactics:

- each side has two pitches, **In possession** and **Out of possession**,
  from `plan[0]`'s `ip` and `oop` halves;
- the squad lists show **IP Position**, **IP Role**, **OOP Position** and
  **OOP Role** where FM24's show the position, role and duty, a role by its
  number, as `Role 5`, and positions by FM24's position names, which have
  not been checked for FM27;
- the headings give both formations, as `[IP 4-2-3-1, OOP 4-4-2]`;
- the six familiarity columns, **Familiarity 0** to **5**, show each
  category's best entry, not the game's bar, and **Max familiarity** sets
  every entry's amount to 10000 (see
  [Tactical familiarity](FIXTURE-FM27.md#tactical-familiarity)).

The team picker lists teams by unique ID, each named `?`, as the clubs'
names cannot be read yet. It starts empty, listing every team: type or
pick the team's ID; anything else is refused with "Enter a team's unique
ID."

## Files and removal

The harness adds the files that [Files and removal](MACOS.md#files-and-removal)
lists for FM24, under FM27's names:

- in the harness folder, `~/Library/Application Support/fm27-harness/`:
  `fm27-harness-<version>.dylib`, `launch-fm27.sh`, `harness.log`, the
  requests of `launch --save` and `--rosetta`, and `log-only` after
  `install --log-only`;
- in your temporary folder: `fm27-harness-<version>.sock` and its
  `.sock.lock`, `fm27-job-input-<pid>-<n>.bin` and `.json`, the viewer's
  `fm27-viewer-<pid>/` and `fm27-temp-fixture.json`, the next fixture that
  `fm27ctl` with no arguments and FM27 Viewer export;
- the viewer's settings,
  `~/Library/Preferences/io.github.graemerycyk.fm27-match-harness.viewer.plist`,
  and its caches in
  `$(getconf DARWIN_USER_CACHE_DIR)io.github.graemerycyk.fm27-match-harness.viewer/`;
- Steam's launch option for FM27, and FM27's own records of the save it
  loaded, as for FM24. Whether FM27 also keeps file-list error logs for
  the harness's match builds, as FM24 does, has not been checked: look in
  `Logs` of its data folder.

To remove it all, follow [Removing the harness](MACOS.md#removing-the-harness)
with FM27's names: clear the launch option in **Football Manager 27 >
Properties** (or in the `"4530370"` block of `localconfig.vdf`, checked
with `grep -l launch-fm27.sh`), then delete the harness folder, the
viewer's settings (`defaults delete io.github.graemerycyk.fm27-match-harness.viewer`)
and the temporary files, with this command, which matches none of FM24's
(with `-print` in place of `-exec rm -rf {} +`, it lists them instead):

```sh
find "$(getconf DARWIN_USER_TEMP_DIR)" -maxdepth 1 \
  \( -name 'fm27-harness-*.sock*' -o -name 'fm27-job-input-*' \
  -o -name fm27-temp-fixture.json -o -name 'fm27-viewer-[0-9]*' \) \
  -exec rm -rf {} +
```

Removing the FM27 edition leaves FM24's harness, launch option and files
as they are, and the reverse: FM24's command matches none of FM27's files
either.

## How it works

The harness is loaded as for FM24: Steam runs the launch option, and the
launcher sets `DYLD_INSERT_LIBRARIES` and starts the game's executable,
which accepts the library. The harness then registers with dyld to hear of
every image the game loads. When an image ending in
`game_plugin.bundle/Contents/MacOS/game_plugin` is loaded, the harness's
thread identifies its build by the slice's `LC_UUID`, picks that build's
tables, checks the first bytes of each function it calls, and from then on
serves jobs with that build's addresses, which are offsets from the
plugin's header. When the plugin is unloaded, the harness waits up to ten
seconds for a job that uses it to reach a seed boundary, then refuses every
job. One harness carries the tables of every FM27 build that has been
mapped. The [technical notes on FM27](TECHNICAL-FM27.md) have the details.

## New game builds

Steam updates FM27 in place. A build the harness has no tables for is
refused when the game loads its engine: `harness.log` names the build's
UUID, every job is refused, and `fm27ctl probe` prints the UUIDs and the
game's version. The game itself plays as usual, with the harness idle. A
new build needs a new mapping, which
[`tools/macos/remap.py`](../tools/macos/README.md#mapping-a-new-fm27-build)
makes from the last one, and then a new version of the harness that
carries it; until then, the harness does nothing in that build. Please
report a build that is refused, with `probe`'s output.

Exported fixtures may play in a later build if its format version is the
same; `fm27ctl run` refuses those whose format version differs.

## Checks with the game

These are the checks that the FM27 edition still needs, in order. Run them
on a copy of a save, with your career backed up. Stop at the first that
fails, and send `harness.log`, `Player.log`
(`~/Library/Logs/Sports Interactive/Football Manager 27/`), what `fm27ctl`
printed and any crash report. Record here each one that has been run.

1. **The harness only logs** (see [The first check](#the-first-check-the-harness-only-logs)):
   the constructor ran in `Football Manager 27`, the engine was found with
   the build's UUID and version, the build was accepted, and the game quit
   normally.
2. **Loading a save:** `./fm27ctl install`, then
   `./fm27ctl launch --save <copy>`. Note whether the save loaded by itself
   or `fm27ctl` asked for it to be loaded by hand, and why; then `status`
   should reach `ready`, with the game's date.
3. **The game's state:** `status` through the start (`starting`, `menu`,
   `loading`, `ready`), and what it reports while **Continue** processes a
   day (FM27 is never `busy` yet). Check that the game's own screens show
   the save loaded, that `status` never reports `ready` while the game is
   still loading, and how long it stays `loading` after the game shows
   the save.
4. **Export:** `./fm27ctl get-next-fixture next.json --full`. The file
   should name the human team, both squads and the players as the game
   shows them, and its `match` block should start with the build's version
   and format version, with the seed after the label.
5. **Round trip:** `./fm27ctl get-next-fixture next.bin --seed 42`,
   `./fm27ctl decode next.bin decoded.json`,
   `./fm27ctl encode decoded.json again.bin`, then `cmp next.bin again.bin`
   (they may differ in the one header byte that FM24's exports do).
6. **Simulation:** `run` over seeds 1-10, then with `--keep-build` and with
   `--human-control`. The same seed should give the same result twice, the
   matches should finish with plausible scores and a clock past 90 minutes.
7. **Detailed statistics:** `run --detailed`: each team's players' goals add
   up to its score, and minutes and ratings are plausible.
8. **The database:** `list-teams`, `list-players` and `get-player` against
   the players' profiles in the game: age, ratings, condition, contract
   team, height and weight.
9. **Replacing teams:** `replace-home` and `replace-away` with a team's
   unique ID, then `run`.
10. **The viewer:** open an export, use **Max condition** and **Max
    familiarity**, edit the venue, simulate, and compare both pitches and
    the squad lists with the game's tactics screen: which half is in
    possession, the positions, and what the role numbers and familiarity
    categories are.
11. **Rosetta:** checks 1, 4 and 6 again with `./fm27ctl launch --rosetta`,
    on the x86_64 slice.
12. **Both editions:** with FM24 and FM27 both set up, `fm24ctl status` and
    `fm24ctl launch` still work for FM24.

After the release of 10 November, and after each later build is mapped,
checks 1, 4 and 6 need running again on that build.

## What has been checked

Only without the game, on one Apple silicon Mac:

- The build, with no warnings, and every test, on both slices (the x86_64
  slice under Rosetta), with stand-ins in place of the game:
  - a stand-in game, an executable named `Football Manager 27` in a folder
    with spaces, that loads the harness and loads and unloads a stand-in
    engine plugin, another build of it and an unrelated bundle; the
    harness's log, `starting`, refusals, log-only mode and the wait for a
    job when the plugin unloads; a save that the stand-in engine loads in
    steps, game in progress first, which the harness reports as `loading`
    until it has settled;
  - `fm27ctl` driving that stand-in game from start to finish through a
    stand-in Steam and the launcher: `install`, `launch --save`, `status`,
    `list-teams`, `list-players`, `get-player`, `inject-player`, `run`
    (keeping the build, with detailed statistics and human control),
    `encode`, `run` on a fixture without a `save` (with the game idle,
    while it loads, and not running), `replace-away` with a club's name,
    which the harness refuses, and the load by hand when the command line
    does not reach the engine;
  - the match calls against fakes of the engine's (building, loading,
    playing, the seed after FM27's header, the export's clock seed), and
    the labels of FM27's fixtures on synthetic serialiser traces;
  - FM27 Viewer's models and C core with synthetic FM27 fixtures, and
    the release script's packaging of an FM27 build.
- The mapping of 27.0.0.2486716 against the archived plugin (both slices):
  every function, vtable and global, every signature and frame, and the
  instructions that show each object layout (`check_mapping.py`); the
  frames rebuilt identically from FM24's (`align/frames.py`); and
  `remap.py` finding everything again in the same build.
- `fm27ctl probe` on the archived plugin and an app assembled from its
  `Info.plist` files: both slices supported.

Not checked at all: anything with FM27 or Steam running, including
everything in [Checks with the game](#checks-with-the-game), Steam's form
of `%command%` for FM27, FM27's saves and folders, and an Intel Mac.

## Known limits

- Only 27.0.0.2486716, the free preview, is supported; the full release
  and later builds need mappings first (see [New game builds](#new-game-builds)).
- The harness cannot tell when FM27 is processing, read the loaded save's
  name or read clubs' names (see [What differs from FM24](#what-differs-from-fm24)).
- Every field name in FM27's fixtures is unconfirmed, and the tactics'
  phases, roles and familiarity categories are guesses or numbers (see
  [Editing an FM27 fixture file](FIXTURE-FM27.md#not-known-yet)).
- Whether the engine's calls work on the harness's thread, as they do in
  FM24, has not been checked: the harness checks that a match it makes has
  the engine's own vtable and refuses the job if not.
- The x86_64 slice's serialiser frames were made from the arm64 slice's,
  without running either.
- FM24's [known limits](MACOS.md#known-limits) apply as well, such as the
  unverified direct start without Steam's launch option.
