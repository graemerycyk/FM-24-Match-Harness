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

| Invocation                     | Needs FM24?                 | Purpose                                    |
|--------------------------------|-----------------------------|--------------------------------------------|
| No arguments                   | Yes                         | Export and open the next fixture           |
| `<fixture.json>`               | No, to view                 | Open an existing JSON fixture              |
| `<fixture.bin>`                | Yes                         | Decode and open a binary fixture           |
| `get-next-fixture`             | Yes                         | Export the next fixture                    |
| `run`                          | Yes                         | Simulate one or more matches               |
| `encode`                       | No                          | Convert fixture JSON to binary             |
| `decode`                       | Yes                         | Convert a binary fixture to JSON           |
| `replace-home`, `replace-away` | Yes                         | Rebuild one side with another team         |
| `list-teams`                   | Yes                         | Export team IDs and names                  |
| `list-players`                 | Yes                         | Export player IDs and names                |
| `get-player`                   | Yes                         | Export a player's database values          |
| `inject-player`                | Only when given a player ID | Copy player values into a fixture          |
| `lineup`                       | No                          | Show or reorder a side's players           |
| `probe`                        | No                          | Check whether a game build is supported    |
| `install`                      | No                          | Set up the harness for Steam               |
| `launch`                       | Starts it                   | Start FM24 with the harness                |
| `status`                       | Yes                         | Report what FM24 is doing                  |
| `version`                      | No                          | Print the protocol, commit and slice       |
| `batch`                        | Only `batch run`            | Plan, play, check and stop a batch of runs |

Relative paths use the current directory. Create destination directories
first: commands whose output the game writes, and `inject-player` and
`lineup`, return `1` before they contact or start the game if an output is
a folder, if its folder does not exist or is a file, or if you have no
permission to open that folder or one above it, saying which. This covers
`run`'s default results and the files it writes beside its results too:
its `--detailed` reports, `--per-match` rows, `--keep-going` failed seeds
and `--manifest`. They also return `1` if you may not write the output: for
a new file, in its folder; for an existing one, the file itself, except
`run`'s manifest, which replaces an earlier one with a new file and so
always needs its folder, which must not be append-only (`chflags uappnd`)
either; or if that folder or file is locked (Finder's **Get Info**). A
folder that macOS reports as closed to `fm24ctl` ("operation not
permitted"), as its privacy protection may for the app `fm24ctl` runs in
(such as Terminal), stops only the files `fm24ctl` opens itself: `run`'s
results, which it reads back, its manifest and the outputs of
`inject-player` and `lineup`, which it writes, and the output of
`replace-home` or `replace-away` when it keeps the other side's changed
line-up, which it reads back and writes after the job. That check comes
after the input is read, since the input's header says whether there is a
line-up to keep, but still before the game is contacted. The game writes
the others, and macOS decides its access separately. Which error macOS's
own privacy protection gives `fm24ctl` has not been checked (see
[Known limits](MACOS.md#known-limits)). Explicit output files and
simulation results are overwritten without prompting; do not use an input
file as a results/profile path. JSON simulation and team replacement, and
player injection by ID, write temporary job inputs to your temporary
directory.

There is no global configuration file or global flag. Use command arguments
and editable fixture JSON; see [Editing a fixture file](FIXTURE.md) for its
fields and constraints. Flags take separate values (`--threads 4`, not
`--threads=4`); there are no short aliases or help flags, and the build is
printed by the [`version`](#print-the-build-version) command, not a flag.
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
    [--human-control] [--keep-build] [--per-match] [--keep-going]
    [--manifest] [--no-launch] [--any-save]
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

Each seed the game rebuilds adds about 0.2 MB to FM24's file-list error
log, which `run` leaves alone, unlike `batch run`: delete those logs
yourself, as [Tidying up](MACOS.md#tidying-up) says.

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

A line-up that [`lineup`](#change-a-sides-line-up-lineup) changed (the
input's `home_lineup` or `away_lineup` header key) is played only as the
file has it: a rebuild lists the players as the game's build does. `run`
still plays such an input, but first warns, with numeric seeds and without
`--keep-build`:

```text
warning: home line-up was edited (home_lineup); rebuilding each seed puts FM24's own line-up back; add --keep-build to play it
```

and with `--human-control`, which may have FM24 play the manager's own
selection instead (see [Changing the
line-up](FIXTURE.md#changing-the-line-up)):

```text
warning: home line-up was edited (home_lineup); with --human-control FM24 may play the manager's own selection instead
```

A `.bin` input has no header keys, so `run` cannot warn about one.

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
| `--per-match`             | Off                                              | Also write each match's player and team rows                                                                         |
| `--keep-going`            | Off                                              | Record seeds whose rebuild fails in a file and play the rest, instead of stopping at the first                       |
| `--manifest`              | Off                                              | Also write a JSON record of the run: what ran, on what, how it ended and what it wrote                               |
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
./fm24ctl run next-fixture.json 1-1000 results.csv --per-match --keep-going
./fm24ctl run next-fixture.json 1-1000 results.csv --per-match --manifest
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

#### Per-match rows

`--per-match` writes every match's statistics, unaveraged, to
`results-match-players.csv` and `results-match-teams.csv` (named like the
detailed reports), and `--keep-going` lists skipped seeds in
`results-failed.csv`. The harness writes their headers when the run starts,
even if no rows follow. They do not change the main CSV, and work with or
without `--detailed`. The console names the files after the result line,
with how many seeds were skipped:

```text
ok: 1000/1000 runs, 3 skipped (rebuild failed), each seed rebuilt by the game
per-match player rows: /Users/you/fm24/results-match-players.csv
per-match team rows: /Users/you/fm24/results-match-teams.csv
3 seeds skipped; see /Users/you/fm24/results-failed.csv
```

Each row starts with `index` and `seed`. `index` is the seed's place in the
seed list, counting from 0, so repeated seeds stay apart and rows from
different files join on it. The player file has one row for each squad
slot, home then away in the game's order, and the team file one row per
side:

| Player columns | Meaning |
|---|---|
| `index`, `seed`, `side` | The match, and `home` or `away` |
| `team_uid` | The team's unique ID, after any team override |
| `slot`, `person_uid` | The place in the squad list, and the player's unique ID |
| `in_squad`, `started`, `appeared` | `1` or `0`: listed with a person, in the starting line-up, and played (from the start or brought on) |
| `minutes` | Minutes played, as the game counts them for ratings |
| `on_minute`, `off_minute`, `injured_minute`, `sent_off_minute` | When the player came on, was substituted, went off injured or was sent off; `0` for never |
| `stats_flags` | The engine's flags for the player, in decimal (`32`: a substitute) |
| `rating` | The match rating to two decimals, such as `7.12` or `-0.05` |
| `goals` ... `distance_km` | The same 46 statistics as the detailed report, for this match |

A player who did not appear has an empty `rating` and empty statistics,
since the averages leave such players out. Counts are whole numbers; `xg`,
`xa` and `distance_km` have nine significant digits, enough to recover the
engine's own values exactly.

The team file has `index`, `seed`, `side`, `team_uid`, `squad` (the
players listed) and `used` (those who appeared), then the 38 statistics of
the detailed team report for this match: sums over the players who
appeared, goals including own goals, possession as a whole percent, and
the minutes-weighted average rating. Averaging these rows gives the
detailed report's values to its three decimals.

With `--keep-going`, a seed whose rebuild fails (see [Seeds](#seeds)) gets
a row `index,seed,rebuild` in `results-failed.csv` and no other rows, and
the run goes on. Any other failure still stops the run. Without the flag,
the first failed seed stops it, as before.

The harness writes a match's per-match rows and any earlier failed seeds'
rows before its row in the main CSV, so if the game stops mid-run, every
match in the main CSV also has its other rows.

Per-match rows and `--keep-going` have been checked only with synthetic
matches, not yet in the game (see
[What has been checked](MACOS.md#what-has-been-checked)).

#### Run manifests

`--manifest` writes `results-manifest.json` beside the results (named like
the detailed reports), and the console names it last:

```text
manifest: /Users/you/fm24/results-manifest.json
```

It is written however the run ends once its paths are known: after a
reply, after a lost connection or a timeout, and when the job never reached
the harness (another save loaded, a fixture that does not encode). An
output that `run` refuses before the job (see
[Requirements and conventions](#requirements-and-conventions)), the
manifest among them, stops it without a manifest. It
replaces the file in one step, through a temporary file synced to disk, so
it is never left half written. If it cannot be written, `run` fails with
exit code `1`. With `--manifest` the harness also reports on the job (see
the [technical notes](TECHNICAL.md#run-manifests-fm24-run-manifest1)).
`--manifest` has been checked only with a stand-in for the client, not yet
in the game (see [What has been checked](MACOS.md#what-has-been-checked)).

The manifest is one JSON object, `"format": "fm24-run-manifest/1"`; a
value that is not known is `null`:

| Key | Contents |
|---|---|
| `outcome` | `ok`; `stopped` (only a stop file stopped it); `error`; or `not_submitted` (the harness never took the job) |
| `exit_code` | `run`'s exit code |
| `client_outcome` | How the wait for the result ended: `replied`, `no_harness`, `queue_timeout` (behind another job), `run_timeout`, `lost` (the game closed the connection without a reply), `abandoned` or `failed`; `null` if the job was not submitted |
| `result_line` | The harness's reply |
| `created_at` | When the manifest was written, in UTC |
| `command` | The arguments from `run` on |
| `fm24ctl`, `harness` | Each build's protocol, commit, `dirty` (uncommitted changes) and slice; the harness's also has the game's build and process, when it loaded, how many jobs it has had, and the job's threads, edits, time, places written, seeds skipped, and any failure |
| `harness_matches_fm24ctl` | Whether the game's harness was built from the same commit as `fm24ctl`; `false` warns that the game still has an older harness, which only a restart replaces |
| `game` | The game's process, whether it runs under Rosetta, the save checked and its in-game date, and its memory (`rss_bytes`) when the manifest was written |
| `host` | The computer: model, processor, cores, memory and system |
| `fixture` | The input's path, size and SHA-256; the size and SHA-256 of what the harness plays (its encoding, for JSON); and a JSON fixture's header (`format`, `game_build`, `engine_version`, `source`, `save`, `game_date`, `home_team`, `away_team`) |
| `seeds` | The seed list as given, how many seeds it plays, the first and last, and the SHA-256 of the list's text |
| `flags` | The threads, flags and options of the run |
| `timing` | When the job was submitted, taken and finished; milliseconds queued, running and asleep; matches per second; and the mean of the results' `ms` |
| `outputs` | Each file the job wrote: its role (`results`, `match_players`, `match_teams`, `failed`, `detailed_players`, `detailed_teams`), name, size, rows (lines after the header) and SHA-256 |
| `summary` | The results' matches, home wins, draws and away wins, goals, unfinished matches and skipped seeds |

The results also have a `content_sha256`, the SHA-256 of the file with each
line's last field and the comma before it removed: the same seeds played
again give the same value, although their `ms` differ. `outputs` and
`summary` are filled only when the reply carries the harness's report,
which it gives once it has opened the files for this job; otherwise files
at those paths may be an earlier run's. `input_sha256` and the header's
team overrides identify the fixture played, whatever its JSON's spacing or
annotations.

## Convert JSON to binary: `encode`

```text
fm24ctl encode <input.json> <output.bin>
```

Encode a fixture offline; no flags. Invalid field values produce an error
with the field number. The console reports the byte count on success.
`run` already performs this conversion for JSON inputs. `decode` converts
the other way.

Team replacement metadata (`home_team`/`away_team`) is not stored in the
binary data, nor are [`lineup`](#change-a-sides-line-up-lineup)'s
`home_lineup` and `away_lineup` markers: a changed order is kept, but `run`
cannot warn when a rebuild would put it back. Keep the JSON for later
per-seed rebuilds of replaced teams; use `keep` or `--keep-build` to play
their stored binary fixture.

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
viewer shows no date, and it has no `home_team`/`away_team`, `save`,
`game_date` or `lineup`'s markers (a changed order stays changed). Keep the
JSON of a replaced team for per-seed rebuilds.

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

The game's build lists the other side's players in its own order. When the
input marks that side's line-up as changed (the `home_lineup` or
`away_lineup` that [`lineup`](#change-a-sides-line-up-lineup) adds), the
command then puts each of its players back in their place in the input, by
unique ID, writes the marker again and says what it moved as `lineup` does
(`ok: kept the input's home line-up: ...`). A player the rebuilt side does
not have is missing, and so is one whose unique ID the rebuild wrote back
(see [`inject-player`](#copy-a-players-values-into-a-slot-inject-player)): a
warning names them and the player who takes their place, the first of the
rebuilt side's players whom the input does not name, in file order, so the
others keep theirs. A side managed by FM24's AI is kept too, if marked (by
`lineup --allow-ai-side`). A side it cannot reorder, for one of `lineup`'s
reasons or for a player without a unique ID, is warned of and left as the
game wrote it. The replaced side's order and marker are dropped with its
other edits, and an unmarked line-up stays the game's. An output it cannot
read back or write again returns `1`. Since `fm24ctl` then opens the output
itself, an output folder that macOS closes to `fm24ctl` stops such a
replacement before the game is contacted, as it stops `lineup`.

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
with `replace-away` on one Apple silicon Mac (arm64 slice). A replacement
that keeps a changed line-up (see
[`replace-home`](#replace-a-team-replace-home-replace-away)) therefore
counts the injected player as missing, and their block, whose unique ID the
input does not name, usually takes their place.

```sh
./fm24ctl inject-player next-fixture.json "home.player[10]" 2000040347 torre.json
./fm24ctl run torre.json 1-100
./fm24ctl inject-player next-fixture.json "away.player[0]" torre-values.json edited.json --allow-duplicate
```

## Change a side's line-up: `lineup`

```text
fm24ctl lineup <input.json> <home|away>
fm24ctl lineup <input.json> <home|away> <player> <player> <output.json> [--allow-ai-side]
fm24ctl lineup <input.json> <home|away> <player,player,...> <output.json> [--allow-ai-side]
```

Show a side's players, or move them to other places in the line-up,
offline. The first form prints the side as tab-separated text, with a
header line: each player's `place` (the `N` of `home.player[N]`), `slot`
(`0`-`10`, `sub` for the substitutes, `reserve` for the places after them),
the slot's `position`, the `player` and the `person_uid`. The second form
exchanges two players' places. The third gives the XI in slot order
(`player[0]` plays `slot[0]`), optionally followed by the first
substitutes: at least 11 players. The players it does not list stay where
they are, except that those it displaces from the first places take the
places the listed players left, so a list that replaces one starter with a
substitute is the exchange of the two.

A `<player>` is `player[N]` or `<side>.player[N]`, the zero-based place on
that side; a unique ID (`.person_uid`, as `inject-player` matches it); or a
name, the block's `player` note, whole or, failing that, a part only one
player's name has. Case is ignored but accents are not: give part of the
name or the ID instead. Digits alone are always an ID, never a place. Spaces
around a player, or a list item, are ignored. `--allow-ai-side` applies only
to the two forms that write an output, and may come anywhere after
`lineup`.

`lineup` moves each player's whole block, values and all, renames its fields
to the new place, points the other side's marking of a moved player at its
new place, and adds `"home_lineup": true` (or `away_lineup`) to the header.
It then checks that the result encodes to as many bytes as the input, with
each place holding its player, before it writes the output. See [Changing the
line-up](FIXTURE.md#changing-the-line-up) for what moves with a player and
what stays with the place.

```text
ok: home line-up: Florian Wirtz to player[10] (slot 10, ST), Ryan Gravenberch to player[19] (sub); wrote /Users/you/fm24/arm-wirtz.json
XI: Alisson Becker, ..., Florian Wirtz
bench: Harvey Elliott, ..., Ryan Gravenberch
note: play it with keep or --keep-build; rebuilding each seed puts FM24's own line-up back
```

A list that moves more than two players says how many it moved. `left
out:` names those it moved out of the XI and substitutes, and `also
re-pointed away's marking of` those the other side's marking followed.
Warnings go to the error output as `warning:` lines: a player in goal who is
not a goalkeeper (`pos_gk` below 10), a goalkeeper in another slot, a bench
that no longer has a goalkeeper, and the other side's opposition
instructions (`part_4240`) when they are not all alike, which stay as they
were. With `--allow-ai-side` it warns that FM24's AI picks that side's
XI.

The new order is played only as the file has it: play it with `keep` or
`--keep-build` (see [Seeds](#seeds), where `run`'s warnings are). Each
seed's rebuild lists the players as the game's build does. `replace-home`
and `replace-away` put the other side's changed line-up back, marker and
all, and drop the replaced side's (see [Replace a
team](#replace-a-team-replace-home-replace-away)). A `.bin` made with
`encode` keeps the order but not the marker.

FM24's AI picks the XI of a side it manages at kick-off, whatever the
file's order. `lineup` refuses such a side unless given `--allow-ai-side`,
which is for research; the first form notes it. In every form it refuses a
file whose blocks were moved without being renamed (`home.player[19] is in
place 10`) and a side with more player blocks than its `player_count`
(`home has 22 player blocks, but its player_count is 21`). The other two
forms also refuse blocks whose first 15 values differ and a block with
another block's field in it.

It returns `2`, with the usage, for a wrong number of arguments, a side
other than `home` or `away`, an input or output that does not end in
`.json`, a malformed `player[...]` or one of the other side, an empty player
or list item, a list of fewer than 11 players, an unknown option, and
`--allow-ai-side` given twice or with the first form. It returns `1` for an
input it cannot read or that is not a fixture, a place the side does not
have, a player it cannot find or that several match (it lists them), the
same player twice, the refusals above and an output it cannot write, which
it checks before it reads the input. It never starts or contacts the game.

To compare line-ups over many seeds, derive each arm of a
[batch](#batches-batch) from one export and play them all with `keep_build=1`
and the same seeds. Replacing the other team keeps each arm's line-up;
replacing the side you changed drops it:

```sh
./fm24ctl get-next-fixture base.json --seed 42
./fm24ctl lineup base.json home > base-home.tsv
./fm24ctl lineup base.json home Gravenberch Wirtz arm-wirtz.json
./fm24ctl lineup base.json home 19058734,28104124,...,91193048 arm-xi2.json
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

## Print the build: `version`

```text
fm24ctl version
```

Print the protocol, the commit that `fm24ctl` and its embedded harness were
built from, whether tracked files had uncommitted changes then (`clean` or
`dirty`), and the slice that runs (`arm64` or `x86_64`). A build from a
folder that is not the top of a Git checkout, or without Git, prints
`unknown unknown` for the commit and its state. The game is not contacted.
No flags.

```sh
./fm24ctl version
```

```text
fm24ctl 1.4.0 ff79e5b1a6cea96602d84c28e1543bdb5c2f75b8 clean arm64
```

## Batches: `batch`

```text
fm24ctl batch create <plan.txt> <dir>
fm24ctl batch run <dir> [--threads N] [--until HH:MM] [--max-chunks N] [--allow-harness-change]
fm24ctl batch status <dir>
fm24ctl batch verify <dir> [--deep]
fm24ctl batch stop <dir> [--now]
```

A batch plays thousands to millions of matches of one or more fixtures,
its *arms*, for later analysis. Each arm plays the same set of *indices*,
each index one seed, in chunks of contiguous indices. A chunk is committed
once, by renaming a finished folder into place, so a batch stopped at any
point, even by a crash or a power cut, carries on without playing an index
twice or leaving one out. `batch run` plays a batch in the game; `create`,
`status`, `verify` and `stop` read and write only the batch's folder and
never contact the game. [BATCH.md](BATCH.md) goes from a plan to the
analysis, with the tools that merge and summarise a batch. A *season
batch* (`mode=season`) builds every fixture of a league's double
round-robin from one exported fixture and plays them all once a season,
for many seasons ([Season runs](BATCH.md#season-runs)).

`batch run` has not run with the game yet, only against a fake game and a
stand-in harness, and the per-match rows a batch keeps only with synthetic
matches (see [What has been checked](MACOS.md#what-has-been-checked)).
Before a batch of days, try a chunk first: `batch run <dir> --max-chunks 1`
stops once it has committed one, and running it again goes on.

### The plan

A plan is a text file of `key=value` lines. A `#` at the start of a line,
or after a space or tab, starts a comment. Relative paths are taken from
the plan's folder. (In the batch's frozen `plan.txt`, only a `#` at the
start of a line does, so a save's name or a folder with ` #` in it is
kept whole.)

```text
format=fm24-batch-plan/1
name=liv-ars-2026-10
arm=baseline fixtures/next.json          # an ID, a space, then a JSON fixture
arm=gegenpress fixtures/next-gegenpress.json
seeds=scatter:20261025:0-999999
chunk=1000
save=/Users/you/fm24-sandbox/LIV_COPY.fm
```

`format`, `name`, at least one `arm` and `seeds` are required. A name is
1-64 letters, digits, `_`, `.` and `-`. A batch has 1-16 arms; an arm's ID
is 1-32 letters, digits, `_` and `-`, and two may not differ only in case.
The other keys, each at most once:

| Key | Default | Meaning |
|---|---|---|
| `chunk` | `1000` | Indices per chunk, 1-1,000,000; at most what fits in one job (see below) |
| `threads` | One per logical processor, less one (as `run`) | Workers per job, 1-16 |
| `per_match` | `1` | Write [per-match rows](#per-match-rows) |
| `keep_build` | `0` | As `run --keep-build` |
| `human_control` | `0` | As `run --human-control` |
| `seed_errors` | `skip` | `skip` records a seed that fails to rebuild and goes on (`run --keep-going`); `stop` stops the job there |
| `max_seed_errors` | `1%` | How many seeds of an arm may fail to rebuild: a count, or a percentage with up to three decimals |
| `order` | `interleave` | `interleave` runs chunk *c* of every arm before chunk *c + 1*, so a stopped batch is balanced; `sequential` runs each arm in turn |
| `compress` | `gzip` | Keep committed files compressed with `gzip -n -6`, or not (`none`) |
| `save` | The fixtures' `save` | A save's name, or its `.fm` file to start the game with (which must exist and have that name) |
| `warmup`, `canary`, `canary_every`, `on_canary_mismatch` | `1`, `3`, `50`, `stop` | How each new game session is warmed up and checked against the first one |
| `slice`, `launch`, `any_save`, `kill_game` | `arm64`, `1`, `0`, `0` | Which slice of the game to require, whether to start it with the save, whether another save will do, and whether the batch may quit the game |
| `hang_minutes`, `queue_wait_minutes`, `busy_wait_minutes` | `10`, `360`, `15` | How long a job may make no progress, wait behind another runner's job, or wait for a busy game |
| `max_crashes_per_hour`, `max_poison` | `4`, `3` | How many game crashes an hour (besides those isolating a seed), and seeds of an arm that crash the game, stop the batch |
| `pause_on_battery`, `min_free_gb`, `notify` | `1`, `20`, `1` | Pause on battery power, the free space to keep, and notifications |
| `trim_game_logs` | `1` | Empty FM24's current file-list error log and delete the older ones before `batch run`'s first chunk and after each chunk (see [FM24's logs](#fm24s-logs)); `0` leaves them |

The settings from `warmup` on are for `batch run`; `create` checks and
keeps them. `detailed` is refused: per-match rows give the same averages.

**Seasons.** `mode=season` (the default is `mode=match`) makes a season
batch, which has no `arm` lines:

```text
format=fm24-batch-plan/1
name=epl-2026-seasons
mode=season
base=fixtures/next.json          # the fixture each fixture is built from
team=1131                        # 2-40 unique IDs (list-teams), each once
team=1150
seeds=season:20261027:0-999      # seasons 0-999
```

| Key | Meaning |
|---|---|
| `base` | A JSON fixture, once: the manager's next fixture, as exported. Every fixture is built from it by replacing its teams |
| `team` | A club's unique ID, 1-4294967295, from [`list-teams`](#export-team-ids-list-teams); 2 to 40 of them, each once, in the order that numbers the fixtures |
| `seeds` | `season:<key>:<first>-<last>`, the only kind a season takes: its seasons are the indices |

Every ordered pair of teams is a fixture, an arm of its own: team *h* at
home to team *a* (by their places in the plan, from 0) is fixture *h* (*n*
- 1) + *a*, less one if *a* > *h*, named `f` and that number from 1 as four
digits: 380 fixtures, `f0001` to `f0380`, for 20 teams. Season *s* of
fixture *k* of *F* plays the scattered seed of *s* × *F* + *k*, which must
be below 2^32 (with 20 teams, seasons up to 11,302,544). A season's
defaults differ: `chunk=100` (seasons), `canary=1` and
`max_seed_errors=5`. It refuses `arm`, `order=sequential`, `keep_build=1`,
`human_control=1` and `warmup=0`; a match plan refuses `base`, `team` and
`season:` seeds.

**Seeds.** Indices and seeds are 0-4294967295, and no two indices of a set
play the same seed:

| `seeds=` | Indices | Index *i* plays |
|---|---|---|
| `range:<first>-<last>` | *first* to *last* | Seed *i* |
| `scatter:<key>:<from>-<to>` | *from* to *to* | A seed that a fixed bijection makes of *i* and *key*; with key `20261025`, indices 0, 1 and 2 play `4210832071`, `3117509974` and `3856835949` |
| `list:<file>` | 0 to the lines less one | The file's line *i*: one decimal seed per line, each once |
| `season:<key>:<first>-<last>` | Seasons *first* to *last* | In fixture *k* of *F*, the `scatter` seed of *s* × *F* + *k* (`mode=season` only) |

Scattered seeds avoid neighbouring seeds; a batch with the same key and
other indices, or another key, plays other seeds. An index is the join key
of every file a batch writes.

**Chunks.** Chunk *k* of an arm covers `chunk` indices, starting *k* ×
`chunk` after the first index; the last chunk ends at the last index. A
chunk's ID is its first index, as ten digits (`0000037000`).

### Create a batch: `batch create`

```sh
./fm24ctl batch create plan.txt ~/fm24-batches/liv-ars
```

```text
created batch liv-ars-2026-10 in /Users/you/fm24-batches/liv-ars: 2 arms of 1,000,000 indices, in 2,000 chunks of up to 1,000
plan_sha256 4f38b86c78fc030cfc5d44e93e850e63ce73fdb882a97bb16ed10d1d3f0720a8
```

`create` checks the plan and everything it names before writing anything.
It refuses a plan unless:

- every arm is a JSON fixture (`fm24-match-config/1`) that encodes;
- an arm whose line-up [`lineup`](#change-a-sides-line-up-lineup) changed
  (its `home_lineup` or `away_lineup`) has `keep_build=1`, as each seed's
  rebuild would put the game's line-up back, and not `human_control=1` if
  you manage that side, as FM24 may then play the manager's own selection;
- the arms are from one save and one game build, and from one in-game date
  unless `keep_build=1`;
- `save=`, if given, is that save or its `.fm` file; a batch with no save
  at all needs `launch=0`;
- a list has a seed on every line, each once;
- `warmup` and `canary` are at most `chunk` and the number of seeds;
- the longest job a chunk can need, with every seed of ten digits and this
  folder's paths, fits in one job (16 KB). The message names the largest
  chunk that fits: about 1,430 scattered or listed seeds with a folder path
  of about 100 characters, and any range;
- the folder is new or empty (the Finder's `.DS_Store` aside) and not in
  Desktop, Documents, Downloads or under `/Volumes`: the game writes the
  batch's files, and macOS asks before it writes there, which would stop a
  batch nobody is watching.

A season batch's `base` must be a JSON fixture that encodes, without an
edited line-up, as each fixture is built from it by replacement, which
would drop it; its save, game build and in-game date are the batch's, and
it freezes only the base (`inputs/base.json` and `inputs/base.bin`):
`batch run` builds the fixtures. It warns when a batch has more than
20,000 chunks, which every commit and `status` read.

It warns when the folder's disk has less free space than the batch needs,
compressed or not, and `min_free_gb`. Then it freezes the plan: it copies
each arm's fixture to `inputs/<arm>.json`, encodes it to `inputs/<arm>.bin`,
copies a list to `inputs/seeds.txt` and writes `plan.txt`, with absolute
paths, every setting spelled out, the inputs' SHA-256s and team overrides,
and the fixtures' save, game build and in-game date. Its last line,
`plan_sha256=`, is the SHA-256 of the lines above it, which chunks and
manifests record. A batch whose `plan.txt` or inputs have changed since is
refused with exit code `2`.

### Run a batch: `batch run`

```sh
./fm24ctl batch run ~/fm24-batches/liv-ars
```

```text
session s0001: game pid 81234 (batch run started)
chunk baseline/0000000000 ok 1000 in 555.0 s (1.80/s); 1,000/2,000,000 (0.1%); 1.80/s over 1 chunk; ETA 2026-11-07 14:02 UTC
chunk gegenpress/0000000000 ok 1000 in 552.1 s (1.81/s); 2,000/2,000,000 (0.1%); 1.81/s over 2 chunks; ETA 2026-11-07 13:58 UTC
```

`run` plays the batch's chunks in the plan's order until each is committed
or something stops it. It also resumes a batch: run it again after any
stop, crash or power cut, and it carries on from the folder's files without
playing an index twice. One `run` at a time holds a batch (`batch.lock`); a
second exits with `1`, naming the first's process ID.

| Option | Meaning |
|---|---|
| `--threads N` | Workers per job, 1-16, in place of the plan's `threads` for this run |
| `--until HH:MM` | Stop at the first chunk boundary from that local time on, today or tomorrow |
| `--max-chunks N` | Stop once this run has committed `N` chunks |
| `--allow-harness-change` | Go on with another harness commit, game build or slice than the first session's, recording the change |

**Starting.** `run` checks `plan.txt` and the inputs against their hashes
(exit code `2` if they have changed), and keeps the computer from sleeping
on AC power while it runs (`caffeinate -m -s`). macOS still sleeps with the
lid closed, and on battery power, where `run` pauses
(`pause_on_battery=1`); with `pause_on_battery=0`, which plays on, it holds
idle sleep off on battery power too (`caffeinate -i -m -s`). It removes
`control/stop`, which asked an earlier `run` to stop. If an earlier `run`
stopped while its job was in the game, it waits for the job, watching it as
below, and keeps what it played; a `control/stop-now` the earlier `run` left
stays for that job, which stops at it, and asks this one nothing (see
[Stop a batch](#stop-a-batch-batch-stop)). It then removes that
`control/stop-now`, and what the stopped run left half done, records each
attempt's part, shortens a part whose files have lost rows its `part.json`
counted (as a power cut can leave them), and commits each chunk its parts
now cover. An arm's chunk 0 it commits with the check against the canary
reference below, and one a stopped `run` committed but could not act on is
acted on now.

**Sessions.** Each game process the batch plays in is a session
(`sessions/s0001/` on). Before a chunk, `run` makes sure the game has the
save loaded and idle, runs the plan's `slice`, and is at the fixtures'
in-game date unless `keep_build=1`, starting the game with the save if it
is not running (`launch=1`; from `save=` when that is a `.fm` file). With
`launch=0`, a game that is not running stops it (`1`). A new session
then:

1. plays each arm's first `warmup` seeds with one worker, as the first job
   after the game starts or loads builds matches differently from later
   ones (see the [technical notes](TECHNICAL.md#batch-run));
2. writes `lookups/teams.txt` and `lookups/players.txt` once for the batch,
   as `list-teams` and `list-players` do: the names of the rows' IDs;
3. plays each arm's first `canary` seeds with one worker. The first
   session's become the reference in `canary/`, which each later session's
   must match: the results without `ms`, and the per-match rows byte for
   byte.

The canary runs again every `canary_every` chunks of a session, and before
the next chunk after another runner's jobs have run in the game. When an
arm's chunk 0 commits, its first `canary` indices are compared with the
reference too (`chunk.json` `checks.canary_reference_match`). The first
session's harness commit, game build and slice are pinned (`batch.json`
`pinned.harness`); see [When things go wrong](#when-things-go-wrong) for a
change.

**A season's fixtures.** A season batch builds its fixtures before its
first chunk, a job at a time, with stops, `--until` and the checks of the
game and the computer between jobs, as between chunks. Each session,
whose warm-up plays the base, builds every team's home intermediate
first, `inputs/build/home-<team>.json` and `.bin` (`replace_home` of the
base), then each fixture whose `inputs/<fixture>.json` (`.json.gz` with
`compress=gzip`) is not there yet (`replace_away` of its home team's
intermediate), the job's output written as `.tmp.json`, checked, encoded
to `.bin` and renamed into place. A reply must be a success naming the
team asked for and keeping no edits (`0 edits kept`), and the fixture it
wrote must have the plan's save, game build and in-game date, encode, have
the asked team's override on the replaced side, or none if that is the
side's own team, and keep the other side's. Once every fixture is built,
`inputs/fixtures.txt` freezes their hashes and team overrides and
`inputs/build/` goes. The session then warms up and runs the canary of
the *check fixtures* only, which cover every team once (team 1 at home to
team 2, 3 to 4, and so on, and the last to the first when there are an odd
number), and plays the chunks, block by block. A crash, hang or busy game
during the build is handled as during a warm-up. `--max-chunks` counts
chunks, not the build's jobs.

**Chunks.** Each job at a chunk, an attempt, plays the indices left from
where its parts stop, with the plan's `per_match`, `seed_errors`,
`keep_build`, `human_control` and team overrides, `index_base`, the stop
file `control/stop-now` and a report. When it ends, its files are verified
and its part recorded; a chunk its parts cover is committed, its progress
line printed and FM24's logs trimmed (see [FM24's logs](#fm24s-logs)).
Before each attempt, `run` checks the free space (`min_free_gb` and three
times the latest chunk's files) and, with `pause_on_battery=1`, waits while
the computer runs on battery power, checking each minute.

**The watchdog.** Every five seconds while a job runs, `run` checks that the
game is running and its harness serving, and that the job's results and
failed seeds grow. No growth for `hang_minutes` of time awake is a hang.
Time asleep does not count: the clock that runs through sleep getting ahead
of the one that does not is logged as `slept`, kept as the part's
`slept_ms` and left out of rates. Waiting behind another runner's job is
not a hang either; `queue_wait_minutes` of it stops the batch.

**Stopping.** `batch stop`, or the first Ctrl-C or `kill` (SIGINT or
SIGTERM), asks for `control/stop`: `run` stops once the chunk under way is
committed. `batch stop --now`, or the second, asks for `control/stop-now`,
the jobs' stop file: the job starts no more matches, the matches under way
finish and are kept, and `run` stops. The third exits at once, leaving the
job in the game for the next `run` to adopt; `batch status` then still says
`running`. `--until` and `--max-chunks` stop at a chunk boundary. A stop
asked while no chunk is under way, as the game is checked or a session
starts, stops `run` before a chunk begins. A stop never starts the game
again, and `run` does not wait for a job that has hung once a stop is
asked: [Stop a batch](#stop-a-batch-batch-stop) says what it does instead.
Ctrl-C reaches only `run` itself: the programs it
starts (`caffeinate`, `gzip`, `pmset`, `osascript` and the `open` that
starts the game) run in process groups of their own, so the computer is
kept awake until `run` exits, a commit under way finishes, and a game being
started starts.

**Exit codes.**

| Code | Meaning |
|---|---|
| `0` | Every chunk is committed |
| `1` | Stopped by a failure: the reason is in `batch status`, `events.log` and `batch.json`, and `run` resumes it |
| `2` | Invalid arguments, or `plan.txt` or an input changed since `create` |
| `3` | The game is not in the state the batch needs: another save, the start screen, no human manager's team, the other slice, or busy for `busy_wait_minutes` |
| `4` | Stopped as asked: `batch stop`, a signal, `--until` or `--max-chunks` |

With `notify=1`, `run` posts a notification when the batch completes, stops
for a reason other than being asked, a job hangs, results stop repeating, or
it stops as asked with a job left running in FM24.
It posts them with `osascript` (see
[Running a batch unattended](MACOS.md#running-a-batch-unattended)).

#### FM24's logs

Each match the game builds adds about 0.2 MB to FM24's file-list error
log, `Logs/file llist errors_<date>_<time>.txt` in its data folder, which
FM24 never deletes (see
[What it changes elsewhere](MACOS.md#what-it-changes-elsewhere)). A batch
builds a match for every index it plays unless `keep_build=1`, so a
million matches would leave about 200 GB. With `trim_game_logs=1`, the
default, `run` trims them once before it plays its first chunk and again
after each chunk it commits, while no job of the batch's runs, and a run
that plays no chunk (a complete batch's, or one stopped before its first)
leaves them alone. Of the regular files directly in that `Logs` folder whose
names are `file llist errors_`, anything, then `.txt`, it empties the newest
by modification time in place, as FM24 may still have it open, and deletes
the others. It never follows a link, looks in no subfolder and touches
nothing else, and a missing `Logs` folder is nothing to trim. The logs then
hold about one chunk's matches, about 0.2 GB for a chunk of 1,000.

The data folder is the one `run` looks the save up in: FM24's usual one,
`~/Library/Application Support/Sports Interactive/Football Manager 2024`,
or the folder Steam's launch option gives the game with
`--user_data_location=<dir>` when the option runs the harness's launcher.
`events.log` gets a `game_logs_trimmed` line, with `emptied`, `deleted` and
`freed_bytes` (the space the logs took on the disk), whenever `run` emptied
or deleted one. If FM24 then writes at its old place in the emptied log
rather than at its end, the log keeps its old size in Finder and `ls -l`
but takes almost no space, which `du -h` shows; which of the two FM24 does
has not been checked. Nothing that goes wrong here stops the batch (see
[When things go wrong](#when-things-go-wrong)).

`trim_game_logs=0` leaves the logs alone; delete them yourself then, as
[Tidying up](MACOS.md#tidying-up) says. `fm24ctl run` and the viewer
never trim them. Trimming has run only on folders of test files, not with
the game.

### When things go wrong

Every attempt keeps what it played, as a part, however it ends.

| What happens | How `run` sees it | What it does |
|---|---|---|
| FM24 crashes or quits mid-chunk | The connection closes, or the game has gone (one whose harness went has 10 seconds to exit) | Starts the game with the save (`launch=1`), and goes on from the part in a new session; more than `max_crashes_per_hour` crashes in an hour stop it (`1`). With a stop asked, keeps the part and stops (`4`, with `launch=0` too) without starting the game; the next `run` finishes the chunk, once you have started the game with `launch=0` |
| FM24 is not running, with `launch=0` | The save check before a chunk, or a job's end | Stops (`1`), at the save check without a new session: start the game with the save, then run the batch again |
| A seed crashes the game | Two crashes in a row at a chunk, the second with fewer indices played than workers | Plays the seeds then under way with one worker; a seed that crashes the game twice with one worker is *poison*, excluded from its chunk (`chunk.json` `excluded`). An attempt at it that plays nothing without a crash, such as one the busy game refused, does not count, and the seed is tried again. The crashes of the attempts that isolate the seed (one worker over the seeds under way, then the seed alone) do not count toward `max_crashes_per_hour`. More than `max_poison` in an arm stop it (`1`) |
| The engine hangs | No growth for `hang_minutes` while the game runs | With `kill_game=1`, quits the game (SIGTERM, then SIGKILL a minute later) and goes on as after a crash. Otherwise it notifies and watches the job, keeping what it played once it ends; quitting FM24 yourself then counts as a crash. A job that has not ended after `queue_wait_minutes` more stops it (`1`), saying the job may still run in FM24. A stop asked meanwhile stops it within five seconds (`4`; see [Stop a batch](#stop-a-batch-batch-stop)) |
| The game runs without its harness serving | The harness no longer holds its lock, and the game has not exited 10 seconds later | With `kill_game=1`, quits and starts the game again; otherwise stops (`1`) |
| `run` was interrupted, or the Mac restarted or lost power | The next `run` | Waits for a job still running, then goes on: only matches under way are lost. A part whose files lost rows (`part_shortened` in `events.log`) plays again from where they end |
| Another runner's job holds the harness | The job waits to be taken | Waits; after `queue_wait_minutes`, stops (`1`). A stop asked meanwhile stops it at once (`4`), as it does while the save check before a chunk waits behind that job |
| FM24 is loading or busy | `FM24 is busy`, or the save check waiting | Tries again each minute; after `busy_wait_minutes`, stops (`3`). A stop asked meanwhile stops it within five seconds (`4`) |
| Another save is loaded | The save check | Stops (`3`) and leaves everything as it was: it never switches saves. With `any_save=1` it plays the loaded save |
| The game is at its start screen | The save check | With `kill_game=1`, quits it (nothing is lost without a save loaded) and starts it with the save; otherwise stops (`3`) |
| The save's date has moved (**Continue** was pressed) | The in-game date is not the fixtures' | Stops (`1`) |
| Results no longer repeat | A canary, or chunk 0, differs from the reference | `on_canary_mismatch=stop` stops (`1`); `continue` marks the session `nonrepeatable` and goes on. Chunks committed since the session's last matching canary are listed in `suspect.csv` |
| A Steam update, or another harness | A report's commit, build or slice is not the pinned one | Stops (`1`), unless `--allow-harness-change`, which records the change in `session.json` and `batch.json` |
| An unsupported build, or a harness of another protocol | The harness refuses jobs, or none answers | Stops (`1`) |
| The game runs the other slice than `slice` | The running game's slice | Stops (`3`); with `kill_game=1`, quits it and starts the plan's slice |
| Steam is not running, or asks you to log in | The game does not start | Tries again after 5 and then 15 minutes, then stops (`1`) |
| A seed fails to rebuild (`seed_errors=skip`) | A row in `failed.csv` | Recorded; an arm whose failures pass `max_seed_errors` (a percentage from 1,000 indices on) stops it (`1`) |
| A seed fails to rebuild (`seed_errors=stop`), to deserialise, or its statistics run out of memory | The job's report | Tries again from that index; the same index failing again stops it (`1`) |
| The fixture can no longer be rebuilt per seed | `cannot rebuild per seed` | Stops (`1`) |
| The disk is full, or a file cannot be written | The free-space check, or the report | Stops (`1`), saying how much is free |
| Files torn or cut short | The verifier | Keeps the rows that agree in every file; a job whose files hold less than it reported is tried once more |
| On battery power | `pmset -g ps` before each attempt | Waits for AC power (`pause_on_battery=1`) |
| FM24's logs cannot be trimmed: a log cannot be emptied or deleted, the `Logs` folder cannot be read, or Steam's launch option sets the data folder in a way `fm24ctl` does not follow | Trimming before `run`'s first chunk and after each chunk (`trim_game_logs=1`) | Goes on. Logs each kind once a run (`game_logs_failed`, with `kind` and `error`) and prints a warning, naming the folder; what it could trim it still trims |
| The Mac slept | The clocks | Logs it and starts the hang timer again |
| `plan.txt` or an input changed, or another `run` holds the batch | Their hashes, or `batch.lock` | Exits with `2`, or `1` |
| The harness cannot read a job | `cannot read the job`, or `the job is too long` | Stops (`2`): a bug |
| A season's replacement fails, or builds what was not asked (another team, kept edits, another override, save, build or date, a fixture that does not encode) | The reply, and the fixture it wrote | Stops (`1`), naming the fixture and its teams (`build_failed` in `events.log`); the next `run` builds it again |
| A season's fixture plays with edits | A canary's or chunk's report with `edits` other than 0: as frozen, it does not rebuild to itself | Discards the rows and stops (`1`; `edits` in `events.log`) |

A stopped batch is resumed by running `batch run` again, which accepts what
stopped it as it then stands: an arm that has passed `max_poison` goes on
with its poison seeds excluded, for example.

### The folder

```text
<dir>/plan.txt  batch.lock  batch.json  index.csv  events.log  [suspect.csv]
<dir>/control/stop  stop-now
<dir>/inputs/<arm>.json  <arm>.bin  [seeds.txt]
                         a season's: base.json  base.bin  fixtures.txt  <fixture>.json.gz  <fixture>.bin
                                     [build/home-<team>.json  home-<team>.bin]
<dir>/lookups/teams.txt  players.txt
<dir>/canary/<arm>.csv  <arm>-match-players.csv  <arm>-match-teams.csv  [<arm>-failed.csv]
<dir>/sessions/s<n>/  session.json  warmup-<arm>*.csv  canary-<arm>*.csv
<dir>/attempts/<arm>/<chunk>/a<k>/  attempt.json  part.json  results.csv  results-match-players.csv
                                    results-match-teams.csv  results-failed.csv
<dir>/chunks/arm=<arm>/chunk=<chunk>/  results.csv.gz  match-players.csv.gz  match-teams.csv.gz
                                       [failed.csv.gz]  chunk.json
```

A season's fixtures are its arms: `inputs/<fixture>.json.gz` (`.json`
with `compress=none`) and `.bin` once built, and `inputs/build/` while
they are. **`inputs/fixtures.txt`** (`format=fm24-season-fixtures/1`)
freezes them once all are built: the plan's `plan_sha256`, a
`fixture=<fixture> json_sha256=… bin_sha256=… home_team=… away_team=…`
line for each (the SHA-256 of its JSON uncompressed, and its team
overrides, `-` for none), and `fixtures_sha256=`, the SHA-256 of the lines
above it. A season batch with chunks but no `fixtures.txt`, or whose
`fixtures.txt` or a fixture's `.bin` has changed, is refused with `2`;
`verify` checks the JSONs. Its `canary/` has only the check fixtures'.

A chunk is committed once `chunks/arm=<arm>/chunk=<chunk>/chunk.json`
exists, and is not changed after. Each job at a chunk is an *attempt*, in a
folder of its own; once its job has ended, its *part* is the run of indices
from its first whose rows are complete and agree in every file (see the
[technical notes](TECHNICAL.md#batches-fm24-batch-plan1)). A chunk whose
parts cover it is merged, compressed, written to the disk and renamed into
place, and then its attempts are removed. `index.csv` and `batch.json` are
made again from the chunks, and `events.log` is for people. There is no
other state. A process stopped while it writes can also leave a file or
folder ending in `.tmp`: `batch run` removes those of the chunks,
`batch.json` and `index.csv` as it starts, and any other can be deleted
while no `batch run` runs. Nothing removes the batch's folder;
[What runs and batches write](MACOS.md#what-runs-and-batches-write) says
when it can go.

The `arm=` and `chunk=` folder names let DuckDB read the files with
`hive_partitioning`. Every committed file has one header:

| File | Rows |
|---|---|
| `results.csv` | `index` and then the results' columns (`fm24-batch-results/1`) |
| `match-players.csv`, `match-teams.csv` | The [per-match rows](#per-match-rows), `per_match=1` only |
| `failed.csv` | `index,seed,stage` for seeds that failed to rebuild, `seed_errors=skip` only |

Every index of a chunk is in exactly one of `results.csv`, `failed.csv`
and `chunk.json`'s `excluded` (a seed that crashes the game every time).

**`chunk.json`** (`"format": "fm24-chunk-manifest/1"`) records the batch's
name and `plan_sha256`; the arm, its fixture's and input's SHA-256 and team
overrides; the seeds (`spec`, first and last index, count, and the SHA-256
of the chunk's seeds, one per line); `excluded`; the `parts` that make the
chunk, each with its attempt, first index, `count` given, `verified`,
`outcome`, `poison`, session, threads, times, `wall_ms`, `slept_ms`,
`client_outcome` and the harness's report as in a [run
manifest](#run-manifests) (`null` without one); a season's chunk, its
`fixtures_sha256` and `season_fixture` (its `number`, the `fixtures` and
the `home_team_uid` and `away_team_uid`), with the seeds' `spec`
`season:<key>:<fixtures>:<fixture>:<first>-<last>`; the `unused_attempts`
that did not advance it; `rows` of each file; `files`, each with its
`rows`, `raw_bytes`, `raw_sha256`, the results' `content_sha256`, and the
stored `bytes` and `sha256`; the `summary` of results; `host` (power, free
bytes, the game's memory); `checks`; and `committed_at`.

**`index.csv`** has a line for each committed chunk, in the plan's order:

```text
arm,first_index,count,results,failed,excluded,player_rows,team_rows,home,draw,away,home_goals,away_goals,unfinished,sessions,attempts,wall_ms,matches_per_s,committed_at,results_sha256,players_sha256,teams_sha256,path
```

`sessions` are the chunk's sessions, separated by `;`; `attempts` counts
every attempt at it; `matches_per_s` is its indices per second awake; the
SHA-256s are of the uncompressed files.

**`events.log`** has a UTC line for each event, with its details as
`key=value`: `created`, `run_started`, `game_launched`, `session_started`,
`warmup`, `lookup`, `canary`, a season's `build_failed`, `fixtures_frozen`
(the fixtures, the build's jobs and its time), `seasons_complete` (a
block's first and last season) and `edits`, `part_finished`,
`part_shortened`,
`seed_errors`, `poison`, `chunk_committed`, `crash` (`isolating=1` for one
of an attempt isolating a seed, which is not counted), `hang`, `game_killed`,
`identity_changed`, `foreign_jobs`, `slept`, `disk`, `paused`, `resumed`,
`game_logs_trimmed`, `game_logs_failed` (see [FM24's logs](#fm24s-logs)),
`stopped` and `complete`. A `chunk_committed` that a stopped process could
not log is logged when the batch next starts, with `recovered=1`.

**`session.json`** (`"format": "fm24-batch-session/1"`) records a
session: why it started, the game's process ID and when its harness loaded,
whether the batch started the game, the slice, the harness's protocol,
commit and build, whether they changed from the pinned ones, the save and
in-game date, the warm-up, the canary (`reference`, `match`, `mismatch` or
`off`), `nonrepeatable`, and the jobs the harness had run before it and
other runners' jobs since. **`suspect.csv`** (`arm,chunk,session,reason`)
lists chunks committed before a canary that differed (`canary_mismatch`),
and chunks 0 that differed from the reference (`reference_mismatch`), each
written through to the disk once `run` has acted on it. A chunk 0 that
differs is also recorded in its `chunk.json`
(`"canary_reference_match": false`) as it commits, and `summary.py` and
`merge.py` count it as suspect from then on.

**`batch.json`** (`"format": "fm24-batch-manifest/1"`) is rewritten after
every commit and when `run` starts and stops: the plan and its SHA-256,
`fm24ctl`'s build, the computer, `pinned` (the first session's harness,
then the save, in-game date, build and slice), each arm's fixture, chunks,
indices covered, results, skipped and excluded seeds, wins, draws and
goals, the `sessions` with their records, `progress` (planned, done, in
parts, percent, recent rate, ETA), `incidents` (attempts, retries and how
they ended, poison seeds, time asleep) and `outcome` (`created`,
`running`, `stopped` or `complete`, with a reason, exit code and time). A
season's also has `seasons`: its `teams`, `fixtures`, `first` and `last`
seasons, `block` (seasons a chunk), `fixtures_built`, `frozen`,
`blocks_complete`, `seasons_complete` and `matches_missing` (failed and
excluded seeds of complete blocks, each costing its season).

### Show a batch: `batch status`

```text
batch liv-ars-2026-10: 2 arms, chunks of 1,000, interleaved order
progress: 74,000 of 2,000,000 indices (3.7%) in 74 of 2,000 chunks, and 412 in parts of chunks not yet committed
last: chunk baseline/0000036000 ok 1000 in 555.0 s (1.80/s); 74,000/2,000,000 (3.7%); 1.79/s over 10 chunks; ETA 2026-11-07 14:02 UTC
next: chunk gegenpress/0000037000, 412 of 1000 indices done in 1 attempt; the last, a1, ended crashed
arm baseline: 37 of 1000 chunks; 36,996 results, 3 skipped, 1 excluded; home 557, draw 286, away 153
arm gegenpress: 37 of 1000 chunks; 37,000 results, 0 skipped, 0 excluded; home 552, draw 290, away 158
attempts: 76, 74 ok, 2 crashed; 1 poison seed; 0 s asleep
state: running at 2026-10-25T09:23:18Z
```

A season batch shows its teams, fixtures and seasons, how many fixtures
are built, and the seasons of complete blocks in place of the arms:

```text
batch epl-2026-seasons: season run of 20 teams, 380 fixtures, seasons 0-999 in blocks of 100
fixtures: 380 of 380 built, frozen in inputs/fixtures.txt
progress: 152,000 of 380,000 matches (40.0%) in 1520 of 3,800 chunks
last: chunk f0380/0000000300 ok 100 in 56.0 s (1.79/s); 152,000/380,000 (40.0%); 1.79/s over 10 chunks; ETA 2026-10-31 02:10 UTC
next: chunk f0001/0000000400, not started
seasons: 400 of 1,000 in complete blocks (0-399); block 4: 0 of 380 fixtures; 2 matches missing
```

The rate is of indices per second awake over the latest ten committed
chunks, and the ETA divides what is left by it. `status` also names any
folders a stopped process left, which the next `batch run` removes, and a
`chunk.json` that cannot be read. It needs no lock and changes nothing,
so it can run while the batch does.

### Check a batch: `batch verify`

`verify` checks each committed chunk's files against its `chunk.json`:
that each file it lists is there with its size and SHA-256, that there is
no other file, and that its rows, failed seeds and exclusions add up to its
indices. With `--deep` it also decompresses each file and checks every row:
that the chunk's indices are each covered once with their own seeds, that
each played index has its two team rows and each side's players, and that
the files' sizes, rows and SHA-256s (and the results' `content_sha256`) are
those recorded. For a season batch it first checks each fixture's `.bin`
and JSON (decompressed) against `inputs/fixtures.txt`, and with `--deep`
also that each match's team rows name its fixture's teams. It prints each
problem and returns exit code `1` if there is one:

```text
chunk gegenpress/0000000110: match-teams.csv.gz does not match its size and SHA-256 in chunk.json
checked 74 committed chunks (74,000 indices): problems found
```

### Stop a batch: `batch stop`

`stop` creates `control/stop`, which asks a running batch to stop once its
current chunk is committed, or with `--now`, `control/stop-now`, its jobs'
[stop file](TECHNICAL.md#per-match-rows-and-run-reports-protocol-140): no
more matches start, those under way finish, and what they played is kept.
Ctrl-C or `kill` in `batch run` asks the same, once for `stop` and twice for
`stop-now` (see [Stopping](#run-a-batch-batch-run)). While a job runs, or
`batch run` waits, for a job, for the save check or in a pause, it checks
for a stop at least every five seconds (but not while a game it has just
started has yet to load the harness), and it stops with exit code `4`
before the chunk boundary in these cases:

- **A stop never starts the game.** If FM24 exits mid-chunk while a stop is
  asked (quit by hand, crashed, or quit by `batch run` with `kill_game=1`),
  `batch run` keeps the part as crashed and stops, with `launch=0` too,
  rather than start the game again; the next `batch run` starts it (with
  `launch=0`, start it yourself first) and finishes the chunk. Nor does
  `batch run` start a game it finds not running before a job.
- **Nothing of the batch's has started.** A job not yet taken, waiting
  behind another runner's job, is abandoned: the harness never runs it.
  The save check before a chunk, waiting behind such a job or for a busy
  game, ends; so do the pauses on battery power, for a busy game and after
  a failed start.
- **A job that has hung.** Once a hang is noted (`hang_minutes` without
  growth), or at once if it already was, any stop ends the wait for the
  job within five seconds. A stop asked before then takes effect when the
  hang is noted.
- **A job asked to stop now that makes no progress for 30 seconds**, as a
  match that never finishes would, is not waited for any longer.

In the last two, with the default `kill_game=0`, `batch run` leaves the
job and FM24 running, without the attempt's `part.json`. It prints FM24's
process ID and the file the job writes; its reason in `batch status`,
`events.log` and `batch.json` gives the process ID; and it notifies
(`notify=1`). The next `batch run` waits for an attempt left this way as it
starts, and keeps what it played; quitting FM24 ends the job, and its rows
are kept. A warm-up, canary or lookup left this way keeps nothing, and the
next `batch run` waits behind it as behind another runner's job, so quit
FM24 to end it; `batch run` says which it left. With `kill_game=1`,
`batch run` quits FM24, keeps the part, and stops.

While `batch run` waits for a job an earlier run left (see
[Starting](#run-a-batch-batch-run)), `stop` takes effect once that job has
ended and its chunk is committed, or once it has hung, and `stop --now` as
above; the job stops at the new `control/stop-now` too. Each `stop` writes
a line of its own into the file (`pid=` and `ticks=`), by which `batch run`
tells a new `control/stop-now` from one an earlier run left for its job.
`batch run` removes `control/stop` as it starts, so a `stop` asked while no
`batch run` runs asks nothing.

**Exit codes.** `0` on success; `1` if a command failed, `create` refused
the plan or `verify` found a problem; `2` for invalid arguments, or when a
batch's frozen files no longer match their hashes. `batch run` adds `3` and
`4` (see [Run a batch](#run-a-batch-batch-run)).

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
- `decode`, `encode`, `status`, `lineup` and `inject-player` with a player
  file never start the game or check its save.

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

| Exit code | Meaning                                                                                                                                |
|-----------|----------------------------------------------------------------------------------------------------------------------------------------|
| `0`       | Success                                                                                                                                |
| `1`       | Operation failed                                                                                                                       |
| `2`       | Invalid command or arguments                                                                                                           |
| `3`       | Game not in the state needed (`status`, `launch --save` and `--rosetta`, [the save check](#commands-that-check-the-save), `batch run`) |
| `4`       | Stopped as asked ([`batch run`](#run-a-batch-batch-run))                                                                               |

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
