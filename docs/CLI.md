# CLI reference

`fm24ctl` exports, edits, views and simulates FM24 fixtures on macOS. It
also installs the harness, starts FM24 with it and reports what the game is
doing. For Windows, use the
[original project](https://github.com/PhilipArmstead/FM-24-Match-Harness)
by Philip Armstead.

Examples run in Terminal, in the folder that holds `fm24ctl`: the release's
folder, or `bin/` in a build. Quote paths and names containing spaces.
`<...>` means a required argument; `[...]` means optional. The
[macOS guide](MACOS.md) covers setting up, working headless and the viewer.

## Requirements and conventions

Game-connected commands need FM24 24.4.2 (bundle version `2081827`) from
Steam, started with the harness, which refuses them while the game is
loading or processing (see [`status`](#check-the-games-state-status)).
Commands whose job is for one save first make sure the game has it loaded,
and can start the game with it (see [Commands that check the
save](#commands-that-check-the-save)). Exporting or rebuilding needs the
human manager's upcoming fixture.

| Invocation                     | Needs FM24?                 | Purpose                                 |
|--------------------------------|-----------------------------|-----------------------------------------|
| No arguments                   | Yes                         | Export and open the next fixture        |
| `<fixture.json>`               | No, to view                 | Open an existing JSON fixture           |
| `<fixture.bin>`                | Yes                         | Decode and open a binary fixture        |
| `get-next-fixture`             | Yes                         | Export the next fixture                 |
| `run`                          | Yes                         | Simulate one or more matches            |
| `encode`                       | No                          | Convert fixture JSON to binary          |
| `decode`                       | Yes                         | Convert a binary fixture to JSON        |
| `replace-home`, `replace-away` | Yes                         | Rebuild one side with another team      |
| `list-teams`                   | Yes                         | Export team IDs and names               |
| `list-players`                 | Yes                         | Export player IDs and names             |
| `get-player`                   | Yes                         | Export a player's database values       |
| `inject-player`                | Only when given a player ID | Copy player values into a fixture       |
| `probe`                        | No                          | Check whether a game build is supported |
| `install`                      | No                          | Set up the harness for Steam            |
| `launch`                       | Starts it                   | Start FM24 with the harness             |
| `status`                       | Yes                         | Report what FM24 is doing               |

Relative paths use the current directory. Create destination directories
first: commands whose output the game writes, and `inject-player`, return
`1` before they contact or start the game if an output is a folder, if its
folder does not exist or is a file, or if you have no permission to open
that folder or one above it, saying which. This covers `run`'s default
results and its `--detailed` reports too. They also return `1` if you may
not write the output: for a new file, in its folder; for an existing one,
the file itself; or if that folder or file is locked (Finder's **Get
Info**). A folder that macOS reports as closed to `fm24ctl` ("operation not
permitted"), as its privacy protection may for the app `fm24ctl` runs in
(such as Terminal), stops only `run`'s results, which `fm24ctl` reads back,
and `inject-player`'s output, which it writes: the game writes the others,
and macOS decides its access separately. Which error macOS's own privacy
protection gives `fm24ctl` has not been checked (see
[Known limits](MACOS.md#known-limits)). Explicit output files and
simulation results are overwritten without prompting; do not use an input
file as a results/profile path. JSON simulation and team replacement, and
player injection by ID, write temporary job inputs to your temporary
directory.

There is no global configuration file or global flag. Use command arguments
and editable fixture JSON; see [Editing a fixture file](FIXTURE.md) for its
fields and constraints. Flags take separate values (`--threads 4`, not
`--threads=4`); there are no short aliases or dedicated help/version flags.
An unknown command, including `--help`, prints usage and returns exit code 2.

## Export the next fixture: `get-next-fixture`

```text
fm24ctl get-next-fixture <output.json|output.bin> [--seed N] [--full]
    [--save <name>] [--no-launch] [--any-save]
```

Build the human manager's next fixture and write editable JSON or binary match
data. The seed controls match randomness and pre-match values such as attendance,
officials, weather and per-player random values.

| Flag            | Default            | Meaning                                                                                                                           |
|-----------------|--------------------|-----------------------------------------------------------------------------------------------------------------------------------|
| `--seed N`      | Clock-derived seed | Decimal integer from `0` to `4294967295`                                                                                          |
| `--full`        | Off                | Add research annotations to JSON: header `bytes`/`paths` and field `site`/`p`/`member`; no effect on binary data or match results |
| `--save <name>` | None               | First make sure FM24 has this save loaded and idle, starting the game with it if it is not running                                |
| `--no-launch`   | Off                | With `--save`, do not start the game                                                                                              |
| `--any-save`    | Off                | With `--save`, export from another save if FM24 has one loaded                                                                    |

The JSON's header also records the loaded save's name (`save`) and the
in-game date (`game_date`), as information; see
[Finding your way around](FIXTURE.md#finding-your-way-around).

Options follow the output path, in any order; see
[Commands that check the save](#commands-that-check-the-save) for the last
three. For a reproducible comparison
with the real match, export after **Simulate Match**/**Submit Team** and before
**Continue**/**Go to Match**, then run before advancing the game. Before team
submission, the game may choose a different human line-up for each seed.

Until match day reaches the match, the game hasn't yet scheduled you to attend
it, so it builds your side as it would an AI side: the AI picks your line-up
and tactic. That includes the morning of match day, even after you have
confirmed your team: the game schedules you once **Continue** has taken the
day to the match and the button shows **Go to Match**. The export still
succeeds, but the result line ends with a `warning:` saying so. Export again at
that point to get your own line-up and tactic. Do not press **Continue** in
a session with harness work in it: FM24 then autosaves the loaded save with
the changes the export left in the game's memory (see
[Loading a save](MACOS.md#loading-a-save)). Quit without saving, load the
save again, press **Continue** until the button shows **Go to Match**, and
only then export.

```sh
./fm24ctl get-next-fixture next-fixture.json
./fm24ctl get-next-fixture next-fixture.json --seed 42 --full
./fm24ctl get-next-fixture next-fixture.bin --seed 42
```

## Simulate fixtures: `run`

```text
fm24ctl run <input.json|input.bin> [seeds] [output.csv]
    [--threads N] [--profile <samples.txt>] [--detailed]
    [--human-control] [--keep-build] [--no-launch] [--any-save]
```

JSON is encoded automatically. The default output replaces the input's
extension with `.csv`; for example, `next-fixture.json` produces
`next-fixture.csv`. An explicit output path may be given with or without seeds.
An input that is not there is reported (`cannot read`) before the default
output's folder is checked.
Flags can appear anywhere after `run`.

An exported JSON input names its save (`save`), and `run` plays it only
once FM24 has that save loaded and idle: it waits while the game is busy,
starts the game with the save if it is not running, and stops if another
save is loaded. It warns if the save is at another in-game date than the
input's `game_date`. See
[Commands that check the save](#commands-that-check-the-save).

### Seeds

| Pattern           | Meaning                                                            |
|-------------------|--------------------------------------------------------------------|
| Omitted or `keep` | Play the stored fixture once with its own seed, without rebuilding |
| `42`              | Play seed 42                                                       |
| `1-1000`          | Play every seed in the inclusive range                             |
| `1,5,10-20`       | Combine individual seeds and ranges, in that order                 |
| `42,42,42`        | Repeat a seed; duplicates are retained                             |

Numeric seeds are decimal unsigned 32-bit integers (`0`-`4294967295`).
Do not include spaces or mix `keep` with numeric seeds. The pattern must fit
in 255 characters. Ranges must not descend and must satisfy `last - first <= 10000000`
(at most 10000001 matches per range). `run` checks these rules before it
checks the save or contacts the game, and returns `2` with the reason for a
pattern that breaks them. Large batches can take hours.

With explicit numeric seeds, the game rebuilds the fixture for each seed and
re-applies your edits. This reproduces its pre-match randomness, but requires
the input to match the human manager's next fixture in the current game state.
Edits to a player follow that player to wherever the game's build lists the
player. A seed fails if the rebuilt structure cannot accommodate an edit, such
as an edited player no longer in the line-up. See `harness.log` for the field.

Use `--keep-build` for older or otherwise non-rebuildable inputs. It changes
only the seed, leaving the stored attendance, weather and other pre-match
values unchanged; other seeds can therefore differ from the game's results.

A positional argument made solely of digits, commas and hyphens is treated
as seeds, not an output path. Use a filename such as `2024.csv`, not `2024`.

### Flags

| Flag                      | Default                                          | Meaning                                                                                                              |
|---------------------------|--------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `--threads N`             | Logical CPU count minus one, clamped to `1`-`16` | Set `1`-`16` workers; at most one worker per requested match                                                         |
| `--profile <samples.txt>` | Off                                              | Sample worker 0's stack about every 1 ms into a text file, with phase/timing comments; not a profile of every worker |
| `--detailed`              | Off                                              | Also write averaged player and team statistics                                                                       |
| `--human-control`         | Off                                              | Preserve the input's human-control and match-plan flags instead of handing human-managed teams to the AI             |
| `--keep-build`            | Off                                              | Skip per-seed rebuilding; patch only the seed into the input                                                         |
| `--no-launch`             | Off                                              | Do not start FM24 with the input's save if it is not running                                                         |
| `--any-save`              | Off                                              | Play the input even if FM24 has another save loaded                                                                  |

By default, the AI handles in-match changes for both teams. With
`--human-control`, a human-controlled side has nobody making decisions and
may not substitute even an injured player. AI-managed teams are unaffected.
Thread count does not change results (see the observers in the
[technical notes](TECHNICAL.md#known-limits)); CSV rows remain in seed-list
order.

```sh
./fm24ctl run next-fixture.json
./fm24ctl run next-fixture.json results.csv
./fm24ctl run next-fixture.json keep results.csv
./fm24ctl run next-fixture.json 1
./fm24ctl run next-fixture.json 1-10 results.csv --threads 4
./fm24ctl run next-fixture.json 42,99,300-400 lots-of-results.csv --detailed
./fm24ctl run next-fixture.bin 1-100 --keep-build --human-control
./fm24ctl run next-fixture.json 42 --threads 1 --profile samples.txt
```

### Results

The console prints the first ten results, win/draw/loss totals and average
goals. The main CSV has one row per completed match:

| Column                     | Meaning                                                   |
|----------------------------|-----------------------------------------------------------|
| `seed`                     | Match seed                                                |
| `home_goals`, `away_goals` | Final score                                               |
| `clock_ticks`              | Match clock, four ticks per second                        |
| `finished`                 | Whether the engine reports the match finished (`1` = yes) |
| `rng_final`                | Final random-number state, useful for exact comparisons   |
| `ms`                       | Match load/play elapsed time in milliseconds              |

For `results.csv`, `--detailed` adds `results-players.csv` and
`results-teams.csv`. A trailing `.csv` is removed before adding these suffixes;
otherwise they are appended to the output name. Reports include the engine's
player and team statistics, averaged per match: goals, assists, possession,
shots, xG/xA, passing, defending, cards, goalkeeping, distance and more.
Player average ratings are averaged over appearances; substitutes are included
and rows are grouped by unique ID even if squad order changes.

Detailed reports are written only after all seeds succeed. A failed run may
leave a partial main CSV; output-write failures return an error.

## Convert JSON to binary: `encode`

```text
fm24ctl encode <input.json> <output.bin>
```

Encode a fixture offline; no flags. Invalid field values produce an error
with the field number. The console reports the byte count on success.
`run` already performs this conversion for JSON inputs. `decode` converts
the other way.

Team replacement metadata (`home_team`/`away_team`) is not stored in the
binary data. Keep the JSON for later per-seed rebuilds of replaced teams;
use `keep` or `--keep-build` to play their stored binary fixture.

```sh
./fm24ctl encode next-fixture.json next-fixture.bin
```

## Convert binary to JSON: `decode`

```text
fm24ctl decode <input.bin> <output.json> [--full]
```

Have FM24 decode a binary fixture and write it as fixture JSON, in the same
format as a `get-next-fixture` export. The game reads the stored match
without building another fixture or playing it, as the viewer does to open
a `.bin`. `--full`, placed last, adds the same research annotations as
`get-next-fixture --full`; it defaults to off. `decode` never starts the
game or checks which save it has loaded.

The input must not be JSON, and the output must end in `.json`; otherwise
the command says why and returns exit code `2`. Input that is not FM24
binary match data returns `1`.

Binary data keeps neither the export's description nor the team overrides
(see `encode`). The JSON's `source` only says that it was decoded, so the
viewer shows no date, and it has no `home_team`/`away_team`, `save` or
`game_date`. Keep the JSON of a replaced team for per-seed rebuilds.

The game writes the decoded match again rather than copying its bytes, so a
value it calculates while writing can differ from an export of the same match:
the `(computed)` field at site `1a7bd17` (the tenth field) is `32` after
`decode` and `0` in exports. A fixture taken through `decode` and `encode`
plays exactly as the game's own `.bin` does.

`decode` has been run against the game on the arm64 slice only.

```sh
./fm24ctl decode next-fixture.bin next-fixture.json
./fm24ctl decode next-fixture.bin next-fixture-full.json --full
```

## Replace a team: `replace-home`, `replace-away`

```text
fm24ctl replace-home <input.json> <team ID or club name> <output.json> [--full]
    [--no-launch] [--any-save]
fm24ctl replace-away <input.json> <team ID or club name> <output.json> [--full]
    [--no-launch] [--any-save]
```

Have the game choose a new squad and tactic for one side, using the input's
seed. Requires the next fixture in the same game state, as with per-seed
rebuilding. Both input and output must be JSON.

Use the team's unique ID or the club's in-game short name (for example,
`"Man City"`). Ambiguous names report candidate IDs; use `list-teams` to find
an exact ID. The replaced side's edits are discarded; other edits are kept.
Both team overrides remain in the JSON for subsequent replacements and runs.
The output's `save` and `game_date` are those of the game it was rebuilt
in.

`--full`, after the output, adds the same research annotations as
`get-next-fixture --full`; it defaults to off. The input's `save` is
checked first, as for `run`, and `--no-launch` and `--any-save` change
that check (see
[Commands that check the save](#commands-that-check-the-save)). The flags
come in any order.
See [Changing teams](FIXTURE.md#changing-teams) for details.

```sh
./fm24ctl replace-away next-fixture.json "Luton Town" luton.json
./fm24ctl replace-away next-fixture.json 677 luton.json --full
./fm24ctl replace-home luton.json "Norwich City" norwich-v-luton.json
./fm24ctl run norwich-v-luton.json 1-100
```

## Export team IDs: `list-teams`

```text
fm24ctl list-teams <output.txt> [--save <name>] [--no-launch] [--any-save]
```

Write a UTF-8 text file of every club's first team in the loaded database,
one `uniqueId<TAB>short name` line per team. `--save` first makes sure FM24
has that save loaded and idle (see
[Commands that check the save](#commands-that-check-the-save)); there are no
other flags.

```sh
./fm24ctl list-teams teams.txt
```

## Export player IDs: `list-players`

```text
fm24ctl list-players <output.txt> [--save <name>] [--no-launch] [--any-save]
```

Write a UTF-8 text file of every player in the loaded database, one
`uniqueId<TAB>forename<TAB>surname<TAB>common name<TAB>club short name`
line per player. The common name and club are empty when the player has
none. The flags are those of `list-teams`.

The unique ID is the one `get-player` and `inject-player` take. The
viewer's swap-player prompt searches this list by name; from the command
line, search the file.

```sh
./fm24ctl list-players players.txt
grep -i "haaland" players.txt
```

## Export player values: `get-player`

```text
fm24ctl get-player <person unique ID> <output.json>
    [--save <name>] [--no-launch] [--any-save]
```

Export a player's database values as an editable `fm24-player/1` JSON file
for `inject-player`. The ID is a decimal integer from `1` to `4294967295`.
The player may belong to any club in the loaded database. Age is calculated
on the human manager's next match day, so an upcoming fixture is required.
The flags are those of `list-teams`.

```sh
./fm24ctl get-player 2000040347 torre-values.json
```

## Copy a player's values into a slot: `inject-player`

```text
fm24ctl inject-player <input.json> <home|away>.player[N]
    <person unique ID|player.json> <output.json> [--allow-duplicate]
    [--no-launch] [--any-save]
```

Patch an existing player block without changing squad order or fixture
structure. `N` is the block's zero-based index, including substitutes; it
must exist in the fixture. A numeric ID is fetched from FM24 first; a
`get-player` JSON file works offline. Input and output must be JSON. A
player is fetched by ID only once FM24 has the input's `save` loaded, as
for `run`; `--no-launch` and `--any-save` change that check (see
[Commands that check the save](#commands-that-check-the-save)).

Copies database values such as ratings, condition, sharpness, fatigue,
personality, traits, age, height, weight and identity. It does not copy
derived ability/role ratings, caps or squad number. See
[Putting another player in a slot](FIXTURE.md#putting-another-player-in-a-slot)
for the exact scope.

By default, all three unique-ID copies are replaced and duplicate players
elsewhere in the fixture are rejected. `--allow-duplicate`, after the
output, injects the player even if they are already in the fixture, so the same
person can play twice (for example on both sides). Detailed statistics list
them once per side, under their own name. The flag defaults to off.

A rebuild (a `run` with numeric seeds and without `--keep-build`, or
`replace-home`/`replace-away` of the other side) writes the replaced
player's unique ID back into `.person_uid` and the copy at site `19ba773`.
The injected values and the copy at site `1a7fe22` are kept. This was seen
with `replace-away` on one Apple silicon Mac (arm64 slice).

```sh
./fm24ctl inject-player next-fixture.json "home.player[10]" 2000040347 torre.json
./fm24ctl run torre.json 1-100
./fm24ctl inject-player next-fixture.json "away.player[0]" torre-values.json edited.json --allow-duplicate
```

## Open the viewer

```text
fm24ctl
fm24ctl <fixture.json|fixture.bin>
```

The viewer is `FM24Viewer.app`; see the
[macOS guide](MACOS.md#the-fixture-viewer) for its controls. `fm24ctl`
hands fixtures to it, returning as soon as the app has them rather than
when its window closes:

- With no arguments, it exports the next fixture to `temp-fixture.json` in
  the user's temporary directory, which the game can always write to, and
  opens it in the app as a fixture not yet saved. A failed export prints
  an error and returns its exit code without opening the app.
- With exactly one JSON or binary path, it opens that fixture in the app
  without running a match. A file that does not exist returns `1`. JSON
  viewing is offline. Binary viewing uses FM24 to decode the stored input,
  without building or playing a new fixture.

Neither mode accepts flags; use the app's **Simulation** tab or `run` to
simulate.

```sh
./fm24ctl
./fm24ctl next-fixture.json
./fm24ctl next-fixture.bin
```

`fm24ctl` uses the app it is inside (the app's own copy of `fm24ctl`), else
`FM24Viewer.app` beside it, as the build puts both in `bin/`, else the app
macOS knows by its bundle identifier. If there is none, it says how to
build one and returns `2`. The summary's **Click here** link saves a
fixture not yet saved into the app's export folder, and the results of
simulating such a fixture go there too, under its export name (see the
macOS guide).

To go from a closed game to a fixture on show in one step, start the app
itself with `--launch-save`, which takes a save's name or `.fm` file as
[`launch --save`](#load-a-save-launch---save) does, and optionally
`--rosetta` (Apple silicon only): it runs its `fm24ctl launch --save`
(showing the progress as it comes), then exports the next fixture and
shows it. A `.fm` file's path must be absolute, as `open` starts the app in
`/`, not your shell's folder; the viewer refuses another. `open` passes
arguments only to an app it starts, so quit the viewer first. The viewer's
**File > Launch FM24…** and **Install Harness…** run `launch` and
`install` the same way (see the
[macOS guide](MACOS.md#the-fixture-viewer)).

```sh
open -a ./FM24Viewer.app --args --launch-save LIV_MAIN_FM24
```

Give `open -a` the app's path, as here: by name, as in
`open -a "FM24 Viewer"`, it opens the copy macOS knows by that name, which
may be another build. `--launch-save` has not been run from a closed game
yet. Like `launch`, it starts the game directly, which has not been
verified, until Steam has saved the launch option: check with `fm24ctl
install` first.

## Inspect another game build: `probe`

```text
fm24ctl probe [path to fm.app]
```

Check whether this harness supports a build of the game. `probe` only reads
the file: it prints each architecture slice's UUID and whether this harness
supports it, writes no report file and does not search for the harness's
code. Without a path it uses the running game or the Steam install, so the
game need not be running, and it returns failure if it finds neither. It
returns success only if this harness supports at least one slice, so an
unsupported build returns failure. A new build needs a new mapping; see
[`tools/macos`](../tools/macos/README.md). No flags.

```sh
./fm24ctl probe
./fm24ctl probe "$HOME/Library/Application Support/Steam/steamapps/common/Football Manager 2024/fm.app"
```

## Check the game's state: `status`

```text
fm24ctl status
```

Ask the harness what FM24 is doing and print its one-line answer. The
harness only reads the game's memory; the game is not touched. No flags.

| Exit code | Meaning                                                    |
|-----------|------------------------------------------------------------|
| `0`       | A game is loaded and idle, with the human manager's team   |
| `1`       | No harness answered, or it could not read the game's state |
| `3`       | The harness answered, but the game is not in that state    |

```text
ok: ready in_progress=1 processing=0 kind=-1 people=152340 human_team=1 menu_seen=1 autoplay=0 autoplay_read=0 date=2024 day 123 15:00 save=LIV_MAIN_FM24
```

The word after `ok:` is the game's state:

| State      | Meaning                                                                  |
|------------|--------------------------------------------------------------------------|
| `starting` | The game's start screen has not finished its first setup yet             |
| `menu`     | No game is loaded                                                        |
| `loading`  | A game is being loaded or created                                        |
| `busy`     | The game is processing, for example loading or saving                    |
| `ready`    | A game is loaded and idle, and the human manager's team was found        |
| `no_team`  | A game is loaded and idle, but the harness found no human manager's team |

The keys always come in this order. `date` and `save` are left out when the
harness cannot read them, and `save` runs to the end of the line:

| Key                  | Meaning                                                                                 |
|----------------------|-----------------------------------------------------------------------------------------|
| `in_progress`        | `1` once a game has been loaded or created, until it is left                            |
| `processing`, `kind` | `1` while the game processes; `kind` is the game's number for the task, else -1         |
| `people`             | People in the game's database, `0` with no game loaded                                  |
| `human_team`         | `1` if the human manager's team was found; looked for only when idle                    |
| `menu_seen`          | `1` once the start screen's first setup, and any auto-play load in it, has finished     |
| `autoplay`           | `1` while the game's auto play is on; it goes off when the game finds no auto-play file |
| `autoplay_read`      | `1` once the game has read an auto-play file                                            |
| `date`               | The in-game date, written as in an export's `source`                                    |
| `save`               | The loaded save's name, without `.fm`                                                   |

While the game is `loading` or `busy`, the harness refuses every other job
with `error: FM24 is busy (loading); wait until it is idle`, or with
`processing kind N` in the brackets. The check happens when a job starts,
so a game that starts processing just after it (you press **Continue**) runs
alongside the job: keep the game idle while commands run.

`status` waits at most 30 seconds for another runner's job, then fails.
This has been run against the game on the arm64 slice only (see
[What has been checked](MACOS.md#what-has-been-checked)).

```sh
./fm24ctl status
```

## Set up and start the game: `install`, `launch`

```text
fm24ctl install
fm24ctl launch [--save <name|path.fm>] [--timeout <s>] [--user-data <dir>] [--rosetta]
    [path to fm.app]
```

`install` writes the harness dylib and the launcher script `launch-fm24.sh`
to `~/Library/Application Support/fm24-harness/`, finds FM24 in your Steam
libraries (or says where it looked) and prints the Steam launch option that
loads the harness, after the Steam account's current option if it has
another:

```text
/bin/sh "/Users/you/Library/Application Support/fm24-harness/launch-fm24.sh" %command%
```

If the option is already set, it says so. Steam may save a newly set option
only when it quits, so run `install` again after setting it, quitting Steam
if it does not yet say so. Run `install` again after updating `fm24ctl`;
the new harness loads the next time the game starts.
[Files and removal](MACOS.md#files-and-removal) lists every file the harness
adds and how to remove it.

`launch` installs the harness the same way, then starts the game and waits
for the harness: up to ten minutes, two minutes once the game is running
without it (ten with `--rosetta`), or until the game exits. It reports the
harness ready once the game has kept it running for ten seconds. With the
launch option set for the Steam account that signed in last (and no path
given), it starts the game through Steam; otherwise it opens `fm.app` (the
Steam install's, or the given one, which may be the bundle or its
`Contents/MacOS/fm`) with the harness loaded. It returns failure if the game
is already running without this harness version. If the game is already
running with this harness version, `launch` says so and returns success. The
direct start has not been verified (the game crashed in its one attempt),
and `launch` says so before it opens the game that way; starting through
Steam is the tested route. Its progress, and that of the commands that check
the save, goes to standard output a line at a time, also into a pipe, so a
script or the viewer sees each line as it is printed.

```sh
./fm24ctl install
./fm24ctl launch
```

### Run under Rosetta: `launch --rosetta`

On Apple silicon, `launch` starts the game's arm64 slice through Steam.
With `--rosetta` it starts the x86_64 slice under Rosetta instead, with or
without the other options:

```sh
./fm24ctl launch --rosetta
./fm24ctl launch --rosetta --save LIV_MAIN_FM24
```

- Through Steam, `launch` leaves a request next to the launcher,
  `rosetta-request-<pid>`, named after its own process. The launcher takes
  it as it takes a save's request (see
  [`launch --save`](#load-a-save-launch---save)): once, if it is under five
  minutes old and that `fm24ctl` is still running. It deletes it and starts
  the game with `arch -arch x86_64`. Without a request it starts the arm64
  slice as before, so the next start from Steam runs natively again.
  `launch` deletes the request once the game has started or the launch has
  failed; `install` and every `launch` also delete any that an interrupted
  `fm24ctl` left.
- The direct start opens `fm.app` with `open --arch x86_64`, whatever its
  "Open using Rosetta" setting.
- On its first start after an update, Rosetta translates the game before it
  runs, which can take minutes, so `launch` waits up to ten minutes for a
  running game to load the harness rather than two. The wait for the save
  keeps its own `--timeout`, by default 900 seconds.
- Once the harness is ready, `launch` checks that the game runs under
  Rosetta, and returns `1` if it does not: Steam may have started it some
  other way. Quit it and run `launch` again.
- If FM24 is already running, `--rosetta` starts nothing and changes
  nothing: a game running natively stops `launch` with exit code `3`, and
  one under Rosetta is used as without `--rosetta`. A game without this
  harness version returns `1` first, as without `--rosetta`.
- On an Intel Mac, which runs the x86_64 slice natively, `--rosetta` is an
  invalid argument (exit code `2`). On Apple silicon it needs Rosetta
  installed.
- Commands that start the game for their save (see
  [Commands that check the save](#commands-that-check-the-save)) start it
  without `--rosetta`, so through Steam on its arm64 slice: to work on the
  x86_64 slice, start the game with `launch --rosetta` first. They use a
  game running under Rosetta like any other.

None of this has been run against the game yet (see
[What has been checked](MACOS.md#what-has-been-checked)).

### Load a save: `launch --save`

With `--save`, `launch` also has FM24 load a save, then waits until the game
has it loaded and idle:

```sh
./fm24ctl launch --save LIV_MAIN_FM24
./fm24ctl launch --save "$HOME/fm24-sandbox/LIV_COPY.fm"
```

- `--save` takes a save's name or its `.fm` file. A name must match a file
  name exactly, without `.fm`: `LIV_MAIN_FM24` loads `LIV_MAIN_FM24.fm`,
  never `LIV_MAIN_FM24 (v02).fm`, `LIV_MAIN_FM24_FINAL.fm` or
  `liv_main_fm24.fm`. It is looked for in the `cloud/games` folder of FM24's
  data folder, where the game saves by default, then in `games`. If both
  have it, `launch` stops and asks for the path; if neither has it, it lists
  the saves whose names start with it, in any case. A value with a `/` or
  ending in `.fm` is a path, which must name an existing `.fm` file. As
  `fm24ctl` tells saves apart by name only, a file elsewhere with the name
  of a save in those folders is refused: give a copy a name of its own, as
  `LIV_COPY.fm` above.
- `--user-data <dir>` names FM24's data folder if it is not
  `~/Library/Application Support/Sports Interactive/Football Manager 2024`.
  It only changes where the save is looked for. Without it, a start
  through Steam looks in the folder that the Steam launch option gives the
  game with `--user_data_location=<dir>` (or `-user_data_location=`). A
  launch option that sets the folder in any other way, without `=` or with
  a folder that is not absolute, stops `launch --save` until `--user-data`
  names the folder; `install` warns about such an option.
- `--timeout <s>` limits the wait for the save once the harness has started,
  1-86400 seconds, by default 900 to allow for a slow start under Rosetta.
  The wait for the harness keeps the limits above. `--timeout` and
  `--user-data` need `--save`.

FM24 has an auto-play file for unattended runs: started with
`--auto_play_game_file=<file>`, its start screen loads the save that the
file's `LOAD_GAME` names. `launch` writes the file itself, with only that
key, `{"LOAD_GAME": "<path of the save>"}`, so it asks the game to create,
save or move nothing. Steam's start passes no arguments, so `launch` leaves
the file next to the launcher as a request named after its own process,
`autoplay-request-<pid>.json`. The launcher hands it to the game it starts
next, once, while that `fm24ctl` is still running: it renames the file
`autoplay-<pid>.json` after the game's process and adds the argument. A
request more than five minutes old, or one whose `fm24ctl` has exited (as
after Ctrl-C), is ignored, so a later start from Steam loads no save. The
direct start passes the argument to `fm.app` itself. `launch` deletes these
files once the game is ready or the launch has failed (a game still loading
at the timeout keeps its copy); `install` and every `launch` also delete
any that processes no longer running left behind, except that while the
game runs they keep the auto-play files (not the requests): an `fm24ctl`
that started it directly and was then ended may have left the file the
game has yet to read.

`launch` then asks the harness for the game's state every second (see
[`status`](#check-the-games-state-status)). Once the game has been ready
with the save three times in a row, it prints
`ready: LIV_MAIN_FM24 loaded (pid N, 2024 day 123 15:00)` and returns
success. It fails if the game exits, or if the game is at its start screen
15 seconds after reaching it, saying whether the game read the auto-play
file at all. If the harness cannot read the save's name, `launch` warns and
takes the loaded game to be the save asked for.

If FM24 is already running, `launch --save` starts nothing, and compares a
name with the loaded save's without looking it up in the save folders:

| The running game                    | `launch --save`                                 | Exit code |
|-------------------------------------|-------------------------------------------------|-----------|
| Has the save loaded and idle        | Says it is already loaded                       | `0`       |
| Is loading or busy                  | Waits until it is idle, then checks its save    |           |
| Has another save loaded             | Will not switch saves in a running game         | `3`       |
| Is at its start screen              | Asks you to load the save, or quit and rerun    | `3`       |
| Has no harness of this version      | Fails, as without `--save`                      | `1`       |

`fm24ctl` never quits, saves or switches the save of a running game, so no
unsaved progress is lost. `launch --save` also returns `3` if a game with no
human manager's team stays loaded for 30 seconds, or if the timeout runs
out, and `1` if the save cannot be found or the game does not load it.
Without `--save`, `launch` works as before. This has been run against the
game through Steam on the arm64 slice only, not with the direct start (see
[What has been checked](MACOS.md#what-has-been-checked)).

## Commands that check the save

Commands whose job is for one save make sure FM24 has that save loaded and
idle before they submit the job:

- `run`, `replace-home`, `replace-away` and `inject-player` with a person
  ID use the input's `save`, which exports have (see
  [Finding your way around](FIXTURE.md#finding-your-way-around)).
- `get-next-fixture`, `list-teams`, `list-players` and `get-player` use
  `--save <name>`, a save's name as `launch --save` takes it: its file name
  exactly, without `.fm`. A path is not accepted here. Without `--save`
  they check nothing.
- `decode`, `encode`, `status` and `inject-player` with a player file never
  start the game or check its save.

| The game                           | The command                                                                  | Exit code            |
|------------------------------------|------------------------------------------------------------------------------|----------------------|
| Has the save loaded and idle       | Submits its job                                                              |                      |
| Is loading or busy                 | Waits until it is idle, then checks its save                                 | `3` after 15 minutes |
| Has another save loaded            | Stops; with `--any-save`, submits the job for the loaded save                | `3`                  |
| Is at its start screen             | Stops: load the save in FM24, or quit FM24 and run the command again         | `3`                  |
| Is not running                     | Starts the game with the save, as `launch --save` does, then submits the job |                      |
| Is not running, with `--no-launch` | Says so, then submits the job as before, which fails without the harness     | `1`                  |
| Runs without this harness version  | Submits the job as before, which says why it fails                           | `1`                  |

When FM24 is not running, the command prints
`Football Manager 2024 is not running; starting it to load LIV_MAIN_FM24 (--no-launch turns this off)`
and starts it as `launch --save` does: it looks the save up by name in
`cloud/games`, then `games`, of FM24's data folder (the usual one, or the
one Steam's launch option gives the game, as for `launch --save`), starts
the game through Steam if the launch option is set (otherwise directly,
which has not been verified), waits for the harness and then up to 15
minutes for the save. A save that is in neither folder, or in both, stops
the command with exit code `1`, as does a launch option that sets the data
folder in a way `fm24ctl` does not follow: load the save with
`launch --save` and `--user-data` first. The game keeps running afterwards:
`fm24ctl` never quits, saves or switches the save of a running game. If
another save is loaded, the command asks you to quit FM24, saving first
only if you loaded that save yourself and have done no harness work since:
do not save a session that `launch --save` or a command started, or one
with harness work in it (see [Loading a save](MACOS.md#loading-a-save)).

A game without the human manager's team for 30 seconds also stops the
command with `3`. If the harness cannot read the loaded save's name, the
command warns and takes it to be the right one. If the harness answers the
status job with an error, such as an unsupported build, the job is
submitted as before and reports it. A `save` that cannot be a save's name
stops the command with `1`; delete it to use any save. Without a `save`, or
with a `.bin` input, nothing is checked.

`run` also compares the save's in-game date with the input's `game_date`,
and warns before playing if they differ:

```text
warning: the save is at 2024 day 130 09:00, the fixture was exported at 2024 day 123 15:00; the results reflect the save
```

While another runner's job holds the harness, the check waits for it as long
as the command's own job would. The check comes before the job, so a game
that starts processing in between still runs alongside it. This has been
run against the game on the arm64 slice only (see
[What has been checked](MACOS.md#what-has-been-checked)).

```sh
./fm24ctl run next-fixture.json 1-100
./fm24ctl run next-fixture.json 1-100 --any-save
./fm24ctl get-next-fixture next-fixture.json --save LIV_MAIN_FM24
./fm24ctl list-players players.txt --save LIV_MAIN_FM24 --no-launch
```

## Errors, logs and timeouts

| Exit code | Meaning                                                                                                                   |
|-----------|---------------------------------------------------------------------------------------------------------------------------|
| `0`       | Success                                                                                                                   |
| `1`       | Operation failed                                                                                                          |
| `2`       | Invalid command or arguments                                                                                              |
| `3`       | Game not in the state needed (`status`, `launch --save` and `--rosetta`, [the save check](#commands-that-check-the-save)) |

Some invalid values are detected by the game-side job parser and return
`1` rather than `2`. Check the console message as well as the exit code.

The harness cannot be loaded into a running game: it is loaded when the
game starts (see [Set up and start the
game](#set-up-and-start-the-game-install-launch)). A command that finds no
harness first installs this `fm24ctl`'s harness and launcher, as `install`
does, then explains how to start the game with it: through Steam's launch
option, or, once Steam has the option, with `launch`. A development build's
`fm24-harness-<version>.dylib` beside `fm24ctl` takes precedence over the
embedded copy when installing. `harness.log` is in `~/Library/Application
Support/fm24-harness/`.

The harness handles one job at a time; other runners wait. `run` waits up
to six hours, `status` 30 seconds, and other game jobs up to ten minutes,
including queue time. A command's save check waits for another runner's
job as long as its own job would, and for a busy game up to 15 minutes.
These limits are not command-line configurable. A timeout stops the runner
waiting, **not** the harness's work; later jobs still wait for it.

For build scripts, regression-test commands and toolchain configuration,
see [Development](DEVELOPMENT.md).
