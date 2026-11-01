Batches
===

A batch plays thousands to millions of matches of one or more fixtures
unattended, keeps every match's results and per-match statistics, and can
be stopped and resumed at any point without playing a match twice or
leaving one out. A *season batch* plays a league's whole season many times
instead, and keeps each season's final table and players' totals (see
[Season runs](#season-runs)). This guide goes from a plan to the analysis.
The commands, files and settings are in the [CLI reference](CLI.md#batches-batch),
and how they work in the [technical notes](TECHNICAL.md#batches-fm24-batch-plan1).

## From a plan to a batch

Export the fixture, and any variants of it, with a copy of the save loaded
(see [A copy of the save](MACOS.md#running-a-batch-unattended) and
[Working headless](MACOS.md#working-headless)). Each fixture is an *arm*:
every arm plays the same indices, each index the same seed, so arms differ
only in what you changed.

An arm whose line-up you changed with
[`lineup`](CLI.md#change-a-sides-line-up-lineup) needs `keep_build=1`: a
rebuild of each seed would put the game's own line-up back, so `create`
refuses the plan without it. Derive every arm from one export, so that all
of them keep the same pre-match values. `create` also refuses
`human_control=1` with such an arm when you manage the changed side, since
FM24 may then play the manager's own selection instead.

```sh
mkdir -p fixtures
./fm24ctl get-next-fixture fixtures/next.json --save LIV_COPY
```

Then write a plan, `plan.txt`:

```text
format=fm24-batch-plan/1
name=liv-ars-2026-10
arm=baseline fixtures/next.json
arm=gegenpress fixtures/next-gegenpress.json
seeds=scatter:20261025:0-999999   # a million scattered seeds
save=/Users/you/fm24-sandbox/LIV_COPY.fm
```

Scattered seeds (`scatter:<key>:<from>-<to>`) are better than a range of
consecutive seeds: the engine's random numbers for neighbouring seeds may
not be independent, and `summary.py` tests for that (below). Another
batch with the same key and other indices, or another key, plays other
seeds, so batches can be added together. Leave the other settings at their
defaults to start with; [The plan](CLI.md#the-plan) lists them.

```sh
./fm24ctl batch create plan.txt ~/fm24-batches/liv-ars
./fm24ctl batch run ~/fm24-batches/liv-ars
```

`create` checks the plan and freezes it and the fixtures in the folder; it
refuses a folder in Desktop, Documents, Downloads or under `/Volumes`,
where macOS would ask before the game writes. `run` starts FM24 with the
save if it is not running, warms each new game session up, checks that it
repeats the first session's results, and plays chunk after chunk, printing
a line for each:

```text
chunk baseline/0000000000 ok 1000 in 555.0 s (1.80/s); 1,000/2,000,000 (0.1%); 1.80/s over 1 chunk; ETA 2026-11-07 14:02 UTC
```

With `./fm24ctl run` and six to eight workers (seven is the default
there), an M1 MacBook Air with nothing else to do played about 1.8 to 1.95
matches a second, so a million matches of one arm would take about six days, and
by `fm24ctl`'s own estimate about 1.5 GB once compressed. FM24's own
file-list error logs would take far more: each match the game builds adds
about 0.2 MB to them, and FM24 never deletes them, so a million matches
rebuilt per seed, as a batch's are unless its plan has `keep_build=1`,
would leave about 200 GB. So `batch run` trims them: before its first chunk
and after each one, it empties FM24's current file-list error log and
deletes the older ones, leaving about one chunk's worth (about 0.2 GB for a
chunk of 1,000), and touches nothing else in FM24's folders; a run that
plays no chunk, such as one of a complete batch, leaves them. The batch's
`events.log` says what it freed (`game_logs_trimmed`). To keep the logs,
put `trim_game_logs=0` in the plan before `batch create`, and delete them
yourself as [Tidying up](MACOS.md#tidying-up) says.
[FM24's logs](CLI.md#fm24s-logs) has the details; trimming has run only on
folders of test files, not with the game.

`batch run` has not run with the game yet, only against a fake game and a
stand-in harness, and the per-match rows it keeps only with synthetic
matches (see [What has been checked](MACOS.md#what-has-been-checked)), so
it has not been timed either. Play a chunk first, and look at its files,
at FM24's `Logs` folder and at the `game_logs_trimmed` lines in the
batch's `events.log`, before leaving a batch for days:
`./fm24ctl batch run ~/fm24-batches/liv-ars --max-chunks 1` stops once it
has committed one chunk, and running it again without the option goes on.

While it runs, from another Terminal window:

```sh
./fm24ctl batch status ~/fm24-batches/liv-ars   # progress, ETA, incidents
./fm24ctl batch stop ~/fm24-batches/liv-ars     # stop once the current chunk is committed
./fm24ctl batch stop ~/fm24-batches/liv-ars --now
```

Ctrl-C in `run`'s window does the same: once to stop after the chunk, twice
to stop now (the matches under way finish and are kept), and a third time to
exit at once. A stop never starts FM24 again: if FM24 exits or crashes once
a stop is asked, `run` stops too. Nor does `run` wait for a job that has
hung once a stop is asked: it exits, and with the default `kill_game=0`
leaves the job running in FM24, saying so with FM24's process ID; the next
`run` waits for an attempt at a chunk left this way and keeps what it
played, while a warm-up or canary keeps nothing; quitting FM24 ends either
([Stop a batch](CLI.md#stop-a-batch-batch-stop) has the details). Whatever
stopped it, a crash, a restart of the Mac or a power cut included, run it
again to go on. If it stopped by itself, `batch status` says why (see
[When things go wrong](CLI.md#when-things-go-wrong)), and `run`'s exit code
tells the kind: `1` a failure, `3` the game not in the state the batch
needs, `4` stopped as asked.

`./fm24ctl batch verify ~/fm24-batches/liv-ars --deep` checks every
committed chunk's files, decompressing them.

## Running it unattended

[Running a batch unattended](MACOS.md#running-a-batch-unattended) lists
what keeps a Mac playing for days: power, sleep, Steam's updates, a copy of
the save, App Nap and starting the batch again after a restart.
[Files and removal](MACOS.md#files-and-removal) lists the files a batch and
the tools below leave, and how to remove them and undo those settings.

## The tools

`tools/batch/` has four scripts, standard library only, for Python 3.9 or
later (macOS's own `/usr/bin/python3` will do). They come with the source,
not the release: with a release, clone the repository for them (see
[Building from source](../README.md#building-from-source)). Run them with
`-I`, which keeps Python from loading code from the current folder or your
settings. From the clone's `bin/`, where a build's `./fm24ctl` runs (see
[Setting up](MACOS.md#setting-up)), they are in `../tools/batch/`:

```sh
python3 -I ../tools/batch/summary.py ~/fm24-batches/liv-ars
python3 -I ../tools/batch/merge.py ~/fm24-batches/liv-ars --out ~/fm24-merged
python3 -I ../tools/batch/fm24seeds.py scatter:20261025:0-999999 0 9
```

They read only committed chunks, so they can run while the batch does, and
check each file against its chunk's `chunk.json` as they read it. Suspect
chunks, committed in a session whose results stopped repeating, are left
out unless given `--include-suspect`: those in `suspect.csv`, and a chunk
0 whose `chunk.json` has `"canary_reference_match": false`.

**`summary.py <dir> [--arm A] [--include-suspect] [--chunks] [--results-only] [--out D]`**
prints, for each arm:

- home wins, draws and away wins with 95% confidence intervals, overall and
  for each game session, and with `--chunks` for each chunk;
- the lag-1 autocorrelation of draws in index order, with the range that
  95% of independent series fall in. Scattered seeds should land inside it;
  consecutive seeds outside it would show that neighbouring seeds play
  related matches;
- each side's team means.

```text
arm baseline: 1000 of 1000 chunks; 996,012 matches; home 55.9% (55.8-56.0), draw 28.7% (28.6-28.8), away 15.4% (15.3-15.5); goals 1.688-1.041
  session s0001: 412,331 matches; home 55.9% (55.7-56.0), ...
  lag-1 draw autocorrelation in index order: 0.0004 over 996,012 matches (95% of independent series within 0.0020 of -0.0000)
  home Liverpool: goals 1.688, possession 54.012, xg 1.873, shots 15.204, average_rating 7.041
```

With `--out D` it also writes the tables as CSV (`arms.csv`,
`sessions.csv`, `chunks.csv`) and, from the per-match rows,
`<arm>-players.csv` and `<arm>-teams.csv`: the averages that
`run --detailed` writes, in its columns, within 0.0005. `--results-only`
skips the per-match rows, which take a few minutes to read per million
matches.

**`merge.py <dir> [--arm A] [--out D] [--include-suspect]`** writes each
arm's committed chunks, in index order and with one header, to
`<arm>-results.csv.gz`, `<arm>-match-players.csv.gz`,
`<arm>-match-teams.csv.gz` and `<arm>-failed.csv.gz` (seeds that failed
to rebuild), and `<arm>-merge.json`, which names the chunks merged and left
out with their files' SHA-256s, and each output's rows and SHA-256s. The
same chunks always merge to the same bytes. Without `--out`, it writes them
to `<name>-merged` (the plan's `name`) in the current folder, `bin/` in the
example above. Each file is written as a `.tmp` file beside it, then
renamed; a merge that stops at a damaged chunk leaves that `.tmp` file,
which the next merge replaces.

**`fm24seeds.py <seeds> [first last]`** prints the seed of each index of a
seed set (`range:`, `scatter:` or `list:`, or a season fixture's
`season:<key>:<fixtures>:<fixture>:<first>-<last>`), as `fm24ctl` makes
them.

**`season.py`** reads a [season batch](#season-runs) into its tables.

## Reading the data

Each committed chunk is a folder, `chunks/arm=<arm>/chunk=<first>/`, of
gzip-compressed CSV files, each with its header: `results.csv.gz` (`index`
and the results' columns), `match-players.csv.gz` and
`match-teams.csv.gz` (the [per-match rows](CLI.md#per-match-rows)) and
`failed.csv.gz`. `index` joins them, and with `arm` it is unique across a
batch; across batches, the fixture's `input_sha256` (in `chunk.json`), its
team overrides and the seed are.

The `arm=` and `chunk=` folder names let DuckDB read the whole batch as
tables with those two columns:

```sql
SELECT arm, count(*) AS matches,
       avg((home_goals > away_goals)::INTEGER) AS home_wins,
       avg((home_goals = away_goals)::INTEGER) AS draws
FROM read_csv('chunks/arm=*/chunk=??????????/results.csv.gz', hive_partitioning = true)
GROUP BY arm;

SELECT p.arm, p.person_uid, count(*) AS appearances, avg(p.rating) AS rating,
       avg(p.xg) AS xg
FROM read_csv('chunks/arm=*/chunk=??????????/match-players.csv.gz', hive_partitioning = true) AS p
JOIN read_csv('chunks/arm=*/chunk=??????????/results.csv.gz', hive_partitioning = true) AS r
  ON p.arm = r.arm AND p."index" = r."index"
WHERE p.appeared = 1 AND r.home_goals > r.away_goals
GROUP BY ALL;
```

`chunk=??????????`, ten characters, matches only committed chunks: a commit
builds its chunk in `chunk=<first>.tmp/`, compressing files there, and
renames it once it is complete, and a stopped commit leaves that folder
until the next `batch run` removes it. A wider pattern such as `chunks/*/*/`
would read it too: files `gzip` is still writing, or a chunk that is not
committed. These queries read suspect chunks too; leave out those in
`suspect.csv` and a chunk 0 whose `chunk.json` has
`"canary_reference_match": false`, or query `merge.py`'s files instead.

With pandas, read the merged files and only the columns needed, with
narrow types: a million matches have about 40 million player rows, which
with pandas' default 64-bit columns do not fit in 16 GB. Statistics are
empty for players who did not appear, so read them as `float32` (or
pandas' nullable `UInt8` for counts):

```python
import pandas as pd

players = pd.read_csv(
    'baseline-match-players.csv.gz',
    usecols=['index', 'side', 'person_uid', 'appeared', 'minutes', 'rating', 'goals', 'xg'],
    dtype={'index': 'uint32', 'side': 'category', 'person_uid': 'uint32', 'appeared': 'uint8',
           'minutes': 'uint8', 'rating': 'float32', 'goals': 'float32', 'xg': 'float32'},
)
```

The DuckDB and pandas examples have not been run against a batch yet;
`summary.py` and `merge.py` have, in the repository's tests.

Names are not in the rows: `lookups/teams.txt` and `lookups/players.txt`,
written by the batch's first session, map team and player unique IDs to
names (as `list-teams` and `list-players` write them).

## Season runs

A season batch plays one league season again and again: every fixture of a
double round-robin of the teams you list, home and away, once a season,
for as many seasons as you ask. It keeps the same per-match files as any
batch, and `season.py` makes each season's final table and each player's
season totals from them.

FM24's career does not advance in a batch. Every match a batch plays is a
standalone simulation of one fixture, and nothing carries over from one
match to the next: no fatigue, injuries, suspensions, form, morale or
transfers, and no table the teams play towards. A simulated season is each
fixture played once with a seed of its own, and the results added up.

### The plan

Export the manager's next fixture with a copy of the save loaded, as for
any batch, and look up the clubs' unique IDs:

```sh
./fm24ctl get-next-fixture fixtures/next.json --save LIV_COPY
./fm24ctl list-teams teams.txt --save LIV_COPY
```

```text
format=fm24-batch-plan/1
name=epl-2026-seasons
mode=season
base=fixtures/next.json            # the fixture every other is built from
team=1131                          # a club's unique ID (list-teams); 2 to 40 teams
team=1150
team=1153
...
seeds=season:20261027:0-999        # seasons 0 to 999
save=/Users/you/fm24-sandbox/LIV_COPY.fm
```

Every ordered pair of teams is a fixture: 380 for 20 teams, named `f0001`
on in the teams' order: `f0001` is the first team at home to the second,
`f0020` the second at home to the first and `f0380` the last at home to
the one before it. Each fixture is an arm and each season an index: season
*s* of fixture *k* (from 0) of *F* plays the scattered seed of *s* × *F* +
*k*, so no two matches of a batch play the same seed, and a batch with the
same key and teams and other seasons plays other seeds, so season batches
add together. A chunk is `chunk` seasons of one fixture, 100 by default,
and the chunks run block by block: chunk 0 of every fixture, then chunk 1,
so seasons complete a block at a time. A season's other defaults are
`canary=1` and `max_seed_errors=5`, a count of each fixture's seeds;
`order=sequential`, `keep_build=1`, `human_control=1` and `warmup=0` are
refused, and so is a base whose line-up
[`lineup`](CLI.md#change-a-sides-line-up-lineup) changed. [The
plan](CLI.md#the-plan) has the rest.

### The build

`batch create` checks the plan and freezes it and the base; it builds
nothing, as it never contacts the game. `batch run` builds the fixtures
before its first chunk, in game sessions it starts and checks as it does
for chunks:

1. Each session warms the base up and writes the lookups.
2. For every team, its *home intermediate*: the base with that team at
   home ([`replace-home`](CLI.md#replace-a-team-replace-home-replace-away)),
   rebuilt in every session, which also warms each club up.
3. Each fixture not yet built: its home team's intermediate with the away
   team replaced (`replace-away`).

That is 401 replacement jobs for 20 teams. Each reply must name the team
asked for and keep no edits of the base (`0 edits kept`), and each fixture
must be of the plan's save, game build and in-game date, encode, and carry
the team overrides asked for. Anything else stops the batch with `1`,
naming the fixture and its teams in `batch status` and in `events.log`
(`build_failed`). Once every fixture is built, `inputs/fixtures.txt`
freezes them (`fixtures_frozen` in `events.log`), the session warms up the
*check fixtures* and runs their canaries, and the chunks begin. The check
fixtures cover every team once (the first team at home to the second, the
third to the fourth, and so on), so that a session's warm-up and canary
are 10 one-worker matches for 20 teams, not 380.

A stop between replacement jobs (`batch stop`, Ctrl-C, `--until`) keeps
the fixtures built so far, and a crash mid-replacement starts the game
again as it would mid-chunk; `batch run` then goes on, building what is
missing after the session's home intermediates. `batch status` says how
far it has got:

```text
batch epl-2026-seasons: season run of 20 teams, 380 fixtures, seasons 0-999 in blocks of 100
fixtures: 137 of 380 built; batch run builds the others before the first chunk
```

A fixture is a rebuild of its own seed, so every match it plays should
rebuild it with no edits. A canary or chunk whose report says otherwise
means that what plays is not the fixture that was built: its rows are
discarded and the batch stops with `1` (`edits` in `events.log`).

### Reading the seasons

```sh
python3 -I ../tools/batch/season.py ~/fm24-batches/epl --out ~/fm24-epl-seasons
```

```text
epl-2026-seasons: 400 of 1000 seasons complete; standings.csv.gz 8000 rows, players.csv.gz 223104 rows, ...
```

**`season.py <dir> [--out D] [--seasons A-B] [--include-suspect] [--matches]`**
reads the batch's committed chunks a block at a time, each chunk once and
checked against its `chunk.json`. A season is *complete* when every
fixture has a results row for it in a chunk that is not suspect; only
complete seasons, of `A-B` if given, go into the tables. It writes, to `D`
(by default `<name>-seasons` in the current folder):

| File | Rows |
|---|---|
| `standings.csv.gz` | `season,position,team_uid,team,played,won,drawn,lost,goals_for,goals_against,goal_difference,points` |
| `players.csv.gz` | `season,team_uid,team,person_uid,player,squad,appearances,starts,sub_appearances,minutes,average_rating,rating_total`, then the 46 statistics of the [per-match rows](CLI.md#per-match-rows) from `goals` to `distance_km`, summed, then `injuries`; one row per player in a matchday squad, by team in the plan's order, then unique ID (`per_match=1` only) |
| `teams.csv` | Each team's complete seasons, points (mean, sample standard deviation, least, most), mean position, goals for and against, and how often it finished in each position |
| `fixtures.csv` | `fixture,home_team_uid,home_team,away_team_uid,away_team,json_sha256,bin_sha256` |
| `matches.csv.gz` | With `--matches`: `season,fixture,seed,home_team_uid,away_team_uid,home_goals,away_goals` |
| `incomplete.csv` | `season,fixture,reason` for each match an incomplete season lacks: `not_played`, `failed` (it failed to rebuild), `excluded` (a seed that crashes the game) or `suspect`, in the blocks that have begun |
| `seasons.json` | The seasons included and left out, each chunk read with its files' SHA-256s, and each output's rows and SHA-256s |

The table gives 3 points for a win and 1 for a draw, and ranks by points,
goal difference, goals for, then the team's name (from
`lookups/teams.txt`) and unique ID: not FM24's own rules, with no
head-to-head. A player's `rating_total` adds the ratings exactly, in
hundredths, and `average_rating` is it over `appearances`, empty without
one; `sub_appearances` are the appearances without a start; `xg`, `xa` and
`distance_km` have four decimals; `injuries` counts the matches with an
`injured_minute`. Names come from `lookups/`, which the batch's first
session writes, and `season.py` refuses a batch without them, as names
break ties. Every team and player row must name its fixture's team. The
same chunks give the same bytes; each file is written as a `.tmp` file
beside its place, and all are renamed into place once every one is
written.

A seed that fails to rebuild, or crashes the game, costs its season, which
`incomplete.csv` names; `batch status` counts them
(`seasons: 400 of 1,000 in complete blocks (0-399); block 4: 0 of 380 fixtures; 2 matches missing`).

The chunks are those of any batch, with `arm` the fixture and `index` the
season, so `summary.py`, `merge.py` and DuckDB read them too, and
`fixtures.csv` joins them to the teams:

```sql
SELECT p."index" AS season, p.person_uid, count(*) AS appearances, avg(p.rating) AS rating
FROM read_csv('chunks/arm=*/chunk=??????????/match-players.csv.gz', hive_partitioning = true) AS p
JOIN read_csv('fm24-epl-seasons/fixtures.csv') AS f ON f.fixture = p.arm
WHERE p.appeared = 1 AND p.team_uid = 1131
GROUP BY ALL;
```

This query has not been run against a batch; `season.py` has, in the
repository's tests.

### Time and space

These are estimates: no season batch has run with the game yet. 1,000
seasons of 20 teams are 380,000 matches, about 59 hours at 1.8 matches a
second, plus each of its 3,800 jobs' overhead (opening its fixture,
building its own seed, the last workers' tail and the commit), which is
not measured: at 2 to 4 seconds a job, 2 to 4 hours more. The build is 401
replacement jobs, their time not measured either. About 2.6 days in all,
and about 0.8 GB: 0.57 GB of compressed per-match rows, 0.19 GB of
fixtures (each about 2.7 MB of JSON, gzipped with `compress=gzip`, and
0.3 MB encoded), and `season.py`'s files. A larger `chunk` pays less
overhead but completes seasons less often: 10,000 seasons would take about
25 days in chunks of 200 to 500 seasons. `batch create` warns above 20,000
chunks, as every commit and `batch status` read them all.

### Limits

- Every fixture is the base with its teams replaced, so it keeps the
  base's competition, venue and date: every club plays at the base's
  ground (see [Changing teams](FIXTURE.md#changing-teams)).
- Your managed club is not built as the others are. Before you submit the
  team, the game picks a line-up and a plain tactic for it; after
  **Simulate Match**, it keeps your own selection. Exporting the base
  before match day reaches the match is the plan here, so that its other
  side, and every replacement, is built as an AI side; whether the club in
  your slot is treated differently has not been checked (see [What has
  been checked](MACOS.md#what-has-been-checked)).
- Nothing carries over between matches, and the table's tie-breaks are not
  FM24's.
- `batch run` builds and plays seasons only against a fake game and a
  stand-in harness so far, and `season.py` only on folders of synthetic
  rows.
