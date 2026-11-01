FM27 CLI differences
===

`fm27ctl` is the FM27 edition's command line (see the
[FM27 guide](MACOS-FM27.md)). It has `fm24ctl`'s commands, flags, outputs
and exit codes, which the [CLI reference](CLI.md) describes: read
`fm27ctl` for `fm24ctl`, FM27 for FM24, `fm27-harness` for `fm24-harness`,
`launch-fm27.sh` for `launch-fm24.sh` and `fm27-match-config/1` for
`fm24-match-config/1`. This page lists what is different. **None of it has
been run with the game yet** (see
[What has been checked](MACOS-FM27.md#what-has-been-checked)).

Game-connected commands need Football Manager 27 27.0.0.2486716 from Steam,
started with the harness. The harness refuses them until the game has
loaded its match engine, a few seconds after it starts:

```text
error: FM27 is busy (starting); wait until it is idle
```

It never refuses them for processing, which it cannot see in FM27 yet:
keep the game idle while commands run. Once the game has unloaded its
engine as it quits, every job is refused with
`error: FM27 is quitting: it has unloaded its match engine`, and in a build
the harness does not support, with
`error: harness <version> does not support this game build; see harness.log`.

## Exports and runs

- **`get-next-fixture`** writes `game_date` but no `save`, as the loaded
  save's name cannot be read, so the commands for an export wait while
  FM27 loads or works, then use the save it has loaded; they stop if FM27
  is at its start screen or not running, and never start it (see
  [The save check](#launch---save-and-the-save-check)). The `source`
  annotation names both teams `?`, as the clubs' names cannot be read
  either; the teams' own blocks still hold their names. The file's `game_build` is the last part of the
  game's version, `2486716`, and its `engine_version` the game's version
  as the match block starts with it.
- **`fm27ctl`** with no arguments exports the next fixture to
  `fm27-temp-fixture.json` in your temporary folder, not FM24's
  `temp-fixture.json`, and opens it in FM27 Viewer. A job's encoded input,
  and a player fetched for `inject-player`, are
  `fm27-job-input-<pid>-<n>.bin` and `.json` there, not FM24's
  `_job_input-<pid>-<n>.bin` and `.json`.
- **`run`** plays a fixture only in a build with the format version it was
  exported in (0x004c in 27.0.0.2486716), with or without `--keep-build`.
  Before kick-off it sets the human manager's flags to "no match plan, not
  in control" with FM24's values, which have not been checked for FM27;
  `--human-control` keeps the input's flags instead. In `--detailed`'s
  `-teams.csv`, each team is named `?`.
- **`decode`** and **`encode`** work as in FM24; a fixture's research keys
  (`--full`) may hold FM27's synthetic IDs (see
  [Research keys](FIXTURE-FM27.md#research-keys)).

## Teams and players

- **`replace-home`** and **`replace-away`** take a team's unique ID only,
  as club names cannot be looked up yet:

  ```text
  fm27ctl replace-away <input.json> <team unique ID> <output.json> [--full] [--no-launch] [--any-save]
  fm27ctl replace-home <input.json> <team unique ID> <output.json> [--full] [--no-launch] [--any-save]
  ```

  A club's name is refused with
  `error: the FM27 harness cannot look teams up by club name yet: give the team's unique ID (list-teams lists them)`;
  a blank team, or one with a line break, with `Enter a team's unique ID.`
- **`list-teams`** writes `uniqueId<TAB>?` for each club's first team.
- **`list-players`** writes `?` in each player's club column.
- **`get-player`** writes an `fm27-player/1` file. Its `weight` is worked
  out as FM27's match builder does it, from the player's body type (1 to 5)
  and height, as FM27's database keeps no weight.
- **`inject-player`** is FM24's, with `fm27-player/1` files.

## `probe`

```text
fm27ctl probe [path to fm.app, its executable, its engine plugin or the plugin's bundle]
```

`probe` reads the engine plugin,
`Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin`, whose
slices' UUIDs tell FM27's builds apart, and prints the app's version
(`CFBundleShortVersionString`) when it has the app:

```text
Football Manager 27 27.0.0.2486716: /Users/you/Library/Application Support/Steam/steamapps/common/Football Manager 27/fm.app
/Users/you/Library/Application Support/Steam/steamapps/common/Football Manager 27/fm.app/Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin
  x86_64  22CFE5C3-B5CF-3C00-8043-1EA190A69B89  supported: 27.0.0.2486716
  arm64   9E8E47DE-F704-314C-917D-C32D12637678  supported: 27.0.0.2486716
harness 1.3.0 supports 2 slice(s) of this build
```

For a build it does not support, it asks for the UUIDs and the game's
version to be reported, and returns `1`. A slice it calls supported is one
the harness has build tables for.

## `status`

`status` prints `fm24ctl status`'s line, with these differences:

- the state is `starting` until the game has loaded its engine and the
  harness has accepted it, and never `busy`;
- a game in progress is `loading`, with `menu_seen` `0`, until it has
  settled: until the in-progress flag and the number of people have held
  for three seconds (see [The game's state](MACOS-FM27.md#the-games-state));
- `processing` is always `0`, and `kind` `-1`;
- `menu_seen` is otherwise `1` once the engine has created its game object;
- `save` is always left out.

## `install`

```text
fm27ctl install [--log-only]
```

`install` sets up the harness and `launch-fm27.sh` for Steam's app 4530370.
With `--log-only`, it also writes a file, `log-only`, next to the harness,
which makes the harness only log how the game loads and unloads its engine,
and serve no jobs (see
[The first check](MACOS-FM27.md#the-first-check-the-harness-only-logs)).
Every other `install`, and every `launch` and command that installs the
harness first, removes that file.

## `launch --save` and the save check

`launch --save` hands the game the auto-play file as in FM24. If the game
it started reaches its menu without loading the save, `fm27ctl` says why
and waits, within `--timeout`, for the save to be loaded by hand:

```text
FM27 reached its menu without loading LIV_COPY: it did not read the auto-play file, so its command line may not reach its match engine. Load it in FM27 yourself (/Users/you/fm27-sandbox/LIV_COPY.fm), and leave the game idle; fm27ctl waits until it is loaded, for at most 840 s more, then goes on.
```

The commands that start the game for a fixture's save do the same. As the
harness cannot read the loaded save's name, `launch --save` and the save
check take a loaded game to be the save asked for, and say so: load the
right one. `fm27ctl` looks for saves as `.fm` files in `cloud/games` and
`games` of `~/Library/Application Support/Sports Interactive/Football
Manager 27`, which has not been checked with FM27's saves.

FM27's exports have no `save`, so `fm27ctl` cannot start FM27 for them.
Before its job for a JSON fixture without a `save`, `run`, `replace-home`,
`replace-away` and `inject-player` with a person ID check the game as for
a save, but take whatever save FM27 has loaded:

| FM27 | The command | Exit code |
|---|---|---|
| Has a save loaded and idle | Says it takes that save to be the fixture's, and submits its job | |
| Is loading | Waits until it is idle, then submits its job | `3` after 15 minutes |
| Has no save loaded | Stops: load the fixture's save in FM27, then run the command again | `3` |
| Is not running | Stops, saying to start FM27 and load the fixture's save first | `1` |
| Runs without this harness version | Submits the job as before, which says why it fails | `1` |

With FM27 idle, a run prints, before its results:

```text
the harness cannot read which save FM27 has loaded; taking it to be the fixture's save
```

after a wait also `ready: the fixture's save is already loaded (pid N, <date>)`,
and with FM27 not running:

```text
Football Manager 27 is not running: start it with the harness, load the fixture's save and leave the game idle, then run this again. FM27 exports do not record their save, so fm27ctl cannot start the game with it.
```

`--no-launch` and `--any-save` have nothing to act on for such a fixture:
the command never starts FM27 for it, and takes any loaded save. `run`
warns if that save is at another in-game date than the fixture's
`game_date`, as in FM24; nothing else checks that it is the fixture's
save. A `.bin` input is not checked, as in FM24.
