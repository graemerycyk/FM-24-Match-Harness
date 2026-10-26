macOS guide
===

The harness runs on macOS with **Football Manager 2024 from Steam**, version
24.4.2 (build 2081827): the macOS release of the same update as the Windows
Steam build that the
[original Windows project](https://github.com/PhilipArmstead/FM-24-Match-Harness)
supports. `fm24ctl`, the harness and the viewer are universal binaries, and
the harness maps both architecture slices of the game: arm64 (Apple
silicon) and x86_64 (Intel Macs, and Apple silicon under Rosetta). The port
has been tested on one Apple silicon Mac only, almost entirely on the arm64
slice; the x86_64 slice has had only a few early checks under Rosetta, and
Intel Macs and older macOS versions are untested (see
[What has been checked](#what-has-been-checked)).

This guide goes through the README's [quick start](../README.md#quick-start)
in more detail, then covers the [fixture viewer](#the-fixture-viewer), the
[files the harness adds](#files-and-removal) and how to remove them, and
[how it works](#how-it-works). The [CLI reference](CLI.md) has the commands.

Some things to know first:

- The fixture viewer is a separate app, `FM24Viewer.app` (see
  [The fixture viewer](#the-fixture-viewer)), which `fm24ctl` with no
  arguments or with a fixture opens. It exports, shows, edits and simulates
  fixtures and replaces teams and players, and can also start the game with
  a save and install the harness.
- The harness can read what the game is doing: `fm24ctl status` reports it
  (see [`status`](CLI.md#check-the-games-state-status)), the harness refuses
  other jobs while the game is loading or processing, and exports record the
  loaded save and the in-game date (`save` and `game_date`). The commands
  that play or rebuild an export make sure the game has its save loaded
  first, and can start the game with it (see
  [Working headless](#working-headless)).
- Only the Steam build 24.4.2 is supported for now, not older Steam
  builds. Epic Games support is coming soon, and GOG support will follow
  if FM24 is released there.
- `probe` only reads a build's slice UUIDs and says whether this harness
  supports them. It writes no report file and exits 1 for an unsupported
  build. A new build needs a new mapping (see
  [`tools/macos`](../tools/macos/README.md)).
- `run --profile` writes the header `# fm <base>`, and its addresses are in
  the running slice: subtracting the base gives offsets in that slice, not
  the Windows build's RVAs (see [Build tables](#build-tables)).

Apart from `probe`, the commands that come from the original Windows project
use its command and job code, and fixtures use its JSON format. A fixture
exported on another machine, or with the original Windows project, can only
be played with `keep` or `--keep-build`; per-seed rebuilds and team
replacement need the next fixture in the same game state (see
[Seeds](CLI.md#seeds)). Moving fixtures between the original Windows project
and this version has not been checked yet.

## Setting up

1. Download the signed release, unzip it and move its folder outside
   Desktop, Documents and Downloads (see the README's
   [quick start](../README.md#quick-start) and [Known limits](#known-limits)),
   then `cd` into that folder. Or build `fm24ctl` (see
   [Development](DEVELOPMENT.md#macos)) in a clone outside those folders,
   then run `cd bin` in it; use this `bin/fm24ctl`, not the copy in
   `build/`. Either folder holds `fm24ctl` and `FM24Viewer.app`, and the
   commands on this page run from it.
2. Run `./fm24ctl install` once (or **File > Install Harness…** in the
   [fixture viewer](#the-fixture-viewer)). It writes the harness and a
   launcher script to `~/Library/Application Support/fm24-harness/`, finds
   the game in your Steam libraries and prints the launch option to use,
   after the account's current one if it has another. If it cannot find the
   game, it says where it looked. [Files and removal](#files-and-removal)
   lists everything the harness adds and how to remove it.
3. In Steam, open **Football Manager 2024 > Properties > General > Launch
   Options** and paste the printed line, which looks like:

   ```text
   /bin/sh "/Users/you/Library/Application Support/fm24-harness/launch-fm24.sh" %command%
   ```

   Keep any arguments of your own after `%command%`. Text pasted into this
   field has not always been kept, so reopen Properties to check it, and
   set it again if it is empty. Then run `./fm24ctl install` again: once
   Steam has saved the option to disk, it prints that Steam's launch option
   already uses this launcher. Steam may save it only when it quits, so if
   `install` does not say so, quit Steam and run it again. Until then,
   `fm24ctl launch` and the commands that start the game open it directly,
   which has not been verified (see below). If the field will not keep the
   line, quit Steam (it rewrites its settings when it quits), back up
   `~/Library/Application Support/Steam/userdata/<account>/config/localconfig.vdf`,
   where `<account>` is the folder of the Steam account that signed in last,
   and add the option to the game's block (`"2252570"` under `"apps"`) with
   its inner quotes escaped:

   ```text
   "LaunchOptions"		"/bin/sh \"/Users/you/Library/Application Support/fm24-harness/launch-fm24.sh\" %command%"
   ```

4. Back up your career first: copy its `.fm` file from `cloud/games` (see
   [Working headless](#working-headless)) to a folder of your own, and keep
   that backup as it is. Start the game from Steam as usual and load your
   save. For your own line-up and tactic in the export, press **Continue**
   until the button shows **Go to Match** for an upcoming fixture (see
   [`get-next-fixture`](CLI.md#export-the-next-fixture-get-next-fixture)),
   and save then if you like: after harness work you will quit without
   saving. Then leave the game idle. `./fm24ctl status` succeeds once the
   game has a save loaded and idle. Or let `./fm24ctl launch --save`, or a
   command for an exported fixture, start the game and load the save:
   [Working headless](#working-headless) says how to do that with a copy of
   the save under a new name, which keeps your career safe.
5. Export and view the next fixture with `./fm24ctl get-next-fixture
   next-fixture.json` and `./fm24ctl next-fixture.json`, or open
   `FM24Viewer.app` (see [The fixture viewer](#the-fixture-viewer)). Then
   simulate it with the viewer's **Simulation** tab or with `run`, as
   below.
6. When you are done, quit FM24 without saving, as harness work leaves
   changes in the game's memory. Do not press **Continue** or play the
   match in that session either: FM24 then autosaves the loaded save, with
   those changes (see [Loading a save](#loading-a-save)). Load the save
   again to play on.

Then use the commands from the [CLI reference](CLI.md), for example:

```sh
./fm24ctl get-next-fixture next-fixture.json
./fm24ctl run next-fixture.json 1-100 results.csv --detailed
```

A command that finds no harness serving installs this `fm24ctl`'s harness
and launcher first, as `install` does, then says how to start the game with
it: through Steam's launch option, or, once the option is set, with
`fm24ctl launch`.

`fm24ctl launch` also starts the game with the harness. With the launch
option set (and no path given), it starts the game through Steam, which is
the tested route. Without the option, or with a path, it starts the game
directly: it opens `fm.app` with the harness loaded and tells the game's
Steam library which app it is, so the game connects to the running Steam
client. The direct start has not been verified: in its one attempt the game
crashed about four seconds after loading the harness, and `launch` says so
before it opens the game this way. Either way `launch` waits until the
harness is ready. `fm24ctl` reads the launch option from the settings of
the Steam account that signed in last; until Steam has saved a newly set
option to disk, `launch` starts the game directly, so run `fm24ctl install`
first to check that it has.

`fm24ctl launch --save LIV_MAIN_FM24` also has the game load that save, and
waits until it is loaded and idle (see
[`launch --save`](CLI.md#load-a-save-launch---save)). If the game is already
running, it only checks that the save is the one loaded: `fm24ctl` never
quits, saves or switches the save of a running game.

On Apple silicon, `fm24ctl launch --rosetta` starts the game's x86_64 slice
under Rosetta instead of arm64, through Steam or directly, with or without
`--save` (see [`launch --rosetta`](CLI.md#run-under-rosetta-launch---rosetta)).
A game that is already running keeps its slice: `launch --rosetta` stops
with exit code 3 if it runs natively.

Run `fm24ctl install` again after updating `fm24ctl`. This version's
launcher also takes the requests of `launch --save` and `launch --rosetta`,
so an installation from an older version needs `install` again; the
launch option stays the same, and `launch` rewrites the launcher itself
before it starts the game. The running game keeps the harness it loaded
until it restarts, and the next start loads the new one. If the new harness
has the same protocol version
(`HARNESS_PROTOCOL_TEXT` in `src/common/protocol.h`), commands use the
loaded harness without saying so. Only a different protocol version is
reported: commands then fail, saying that the game has another harness
version loaded, until you quit the game and start it again.

## Working headless

With the launch option set, the commands can start the game and load a
save without you clicking through it (the game still opens its window as
usual). `launch --save` starts the game with a save, an export records its
save, and the commands that play or rebuild the export make sure the game
has that save loaded and idle first, starting the game with it if it is not
running:

```sh
./fm24ctl install                                         # once, and after updating fm24ctl
./fm24ctl launch --save LIV_MAIN_FM24                     # start FM24 with the save
./fm24ctl get-next-fixture next.json --save LIV_MAIN_FM24 # export, checking the save
./fm24ctl run next.json 1-100 results.csv                 # checks next.json's save first
./fm24ctl replace-away next.json "Luton Town" luton.json
./fm24ctl run luton.json 1-100 luton.csv
```

`LIV_MAIN_FM24` stands for your save's name: its file name without `.fm` in
`~/Library/Application Support/Sports Interactive/Football Manager 2024/cloud/games`
(where FM24 saves by default) or `games` next to it:

```sh
ls ~/Library/Application\ Support/Sports\ Interactive/Football\ Manager\ 2024/cloud/games
```

If no save has that name, `launch --save` lists the saves whose names start
with it. A session started this way must not be saved, and **Continue**
autosaves (see [Loading a save](#loading-a-save)). To keep your career
safe, copy the `.fm` file to a folder of your own under a new name and
start with `./fm24ctl launch --save "$HOME/fm24-sandbox/LIV_COPY.fm"`.
Commands start the game themselves only for saves in FM24's own folders, so
for a copy kept elsewhere, run that `launch --save` first.

Later, with FM24 closed, `./fm24ctl run next.json 1-100 results.csv` starts
the game with `LIV_MAIN_FM24` itself and plays once the save is loaded. A
save loaded from disk is at the date it was saved: `run` warns if that is not
the export's `game_date`, and per-seed rebuilds then need the fixture to be
the human manager's next one at that date, or `keep` or `--keep-build` (see
[Seeds](CLI.md#seeds)). If FM24 has another save loaded, the commands stop
with exit code 3 rather than switch saves; `--any-save` uses the loaded save
anyway, and `--no-launch` keeps a command from starting the game. The game
stays open afterwards: quit it without saving (see
[Loading a save](#loading-a-save)). The details are under
[Commands that check the save](CLI.md#commands-that-check-the-save).

## The fixture viewer

FM24Viewer.app (FM24 Viewer), in the release beside `fm24ctl` and built by
`scripts/build.sh` into `bin/`, is a native viewer for macOS 13 or newer
(see [Development](DEVELOPMENT.md#the-fixture-viewer) for building it). It
shows the summary, both squads with their stored formations, and the venue
values, and works as follows:

- Opened without a fixture (`open FM24Viewer.app` in the release folder
  or `bin/`, or a double-click), or through `./fm24ctl` with no
  arguments, it exports the next fixture from the game and shows it as
  not yet saved. If the export fails, the
  window says why and what it knows of the game, with **Retry**,
  **Open…** and **Launch FM24…**, instead of closing. While Steam has not
  saved the harness's launch option, it says to set the option first and
  start the game from Steam, and adds **Install Harness…**, as **Launch
  FM24…** would then start the game directly. While the export
  runs (it may wait up to ten minutes behind another harness job),
  **Cancel** or Escape stops it, as do closing the window and quitting;
  the same goes for decoding a `.bin` fixture when no fixture is open yet.
- It opens `.json` fixtures without the game, and has the game decode
  `.bin` ones, which needs FM24 running and idle. Open one with
  `./fm24ctl next-fixture.json` (or `open -a ./FM24Viewer.app
  next-fixture.json`), by dropping it on the app or its window, with
  **Open With** in Finder, or with **File > Open**. If opening a fixture
  fails while none is on show, the window stays open, says why and offers
  **Retry**.
- A fixture not yet saved shows "This fixture object is not saved. Click
  here to export it". The link, or **File > Export** (⌘E), writes it with
  its edits to `<home>-v-<away>-<time>.json` in the export folder, never
  replacing a file, and it then counts as saved. The export folder is
  `~/Documents/FM24 Match Harness` until you choose another with
  **File > Export Folder…**; **Save** offers the same name there. The
  summary cannot take the keyboard focus, so Return there does not export;
  ⌘E does, from anywhere in the window.
- **Max happiness**, **Max condition** and **Max familiarity** beside each
  team's heading change the selected players, as do the **Players** menu
  and the squad lists' context menu. If any selected player lacks the
  values an edit changes, no player is changed.
- The **Venue** tab changes the venue values the fixture stores.
- The summary marks unsaved venue and player edits, as does the window's
  close button. **Save** checks the venue values first; if one is not
  valid, it says why, shows the Venue tab and puts the cursor in that
  field. It then writes the edits as JSON or binary, without the game,
  replacing the destination only once the whole file is written. A fixture
  that stores venue values is written with the values derived from them
  recomputed.
- Opening another fixture, closing the window or quitting with unsaved
  edits asks whether to discard them (**Cancel**, the default, or Escape
  keeps them), as does opening another fixture over one not yet saved.
  While a question, panel or the swap prompt is open, nothing else can be
  started.
- The blue pencil after each team's heading (or **Players > Replace Home
  Team…** and **Replace Away Team…**) opens a team picker, a popover under
  it, filled with the team's name. Type a team ID or club short name, or
  pick a team from the list below, which the game lists once
  (`fm24ctl list-teams`) and which shows the teams whose name or ID
  contains what you type, those that start with it first. Return or
  **Confirm** has the game rebuild that side with the team; Escape,
  **Cancel** or a click elsewhere closes the picker. If the game refuses,
  the reason shows in red in the picker.
- **Swap player** (also in the **Players** menu and the squad lists'
  context menu) opens the swap prompt, a sheet, to put another player in
  the place of the one selected player. Type a player's name (once the
  game has listed its players with `fm24ctl list-players`; at least three
  letters, accents ignored, so "odegaard" finds Ødegaard) and pick them
  from the list, or type a unique ID. Tick **Allow duplicates?** to swap in
  a player who is already in the fixture.
- A replacement or swap keeps your other edits (not those of the replaced
  side or player) and the venue values, and shows the result as a fixture
  not yet saved, which you can export or save. Nothing else can be done to
  the fixture while it runs. Saving a replaced team as `.bin` first warns
  that binary fixtures do not keep the replacement (see
  [`encode`](CLI.md#convert-json-to-binary-encode)). The team and player
  lists are fetched again after the game quits, since another save may
  have other ones.
- The **Simulation** tab (**Fixture > Simulate**, ⌘R) takes the number of
  threads, a seed pattern, **Generate detailed reports?** and **Skip
  rebuild**, and checks them and the venue values first.
  While the game plays, the summary counts home wins, draws and away wins,
  a bar along the bottom of the window and a badge on the Dock icon show
  how far it is, the Mac does not go to sleep, and nothing else can be
  done to the fixture but exporting it; closing and quitting wait. The
  results are saved beside the fixture as `<fixture name>.csv` (with
  `-players.csv` and `-teams.csv` for detailed reports), replacing earlier
  ones, or, for a fixture not yet saved, in the export folder under its
  export name. **Show in Finder** (or **Fixture > Show Results in
  Finder**) shows them. If the run fails part way, the results so far are
  saved; if it fails before any, earlier results stay.
- A fixture the harness exports records its save (`save` and `game_date`;
  see [Working headless](#working-headless)), which the summary shows as
  "Save: LIV_MAIN_FM24, 2024 day 123 15:00" and the viewer keeps in what it
  saves, exports and gives the game, except in `.bin` files, which have no
  place for it: once saved as `.bin`, the fixture names no save, as when
  the file is opened again. Simulating such a fixture, replacing a team in
  it or swapping a player has `fm24ctl` make sure FM24 has that save
  loaded first, as the commands do. If FM24 is not running, `fm24ctl`
  starts it with the save, without asking first: through Steam once Steam
  has saved the launch option, otherwise directly, which has not been
  verified (see [Known limits](#known-limits)). The window says so, with
  each line of `fm24ctl`'s progress as it comes (the game's state, and a
  "still waiting" line every minute), however long the game takes, until it
  plays. If the game is loading or processing, `fm24ctl` waits until it is
  idle, up to 15 minutes, and the window says so the same way. If another
  save is loaded, the viewer asks, in `fm24ctl`'s words, whether to **Run
  Anyway** in the loaded save (`--any-save`; **Cancel**, the default,
  changes nothing). If the game is at its start screen, or stays busy, it
  shows `fm24ctl`'s reason and changes nothing. While the game loads or
  processes, the harness refuses the viewer's other jobs (the startup
  export, decoding, the team and player lists, and jobs for a fixture that
  names no save), and the viewer shows its "FM24 is busy" message. An
  error box gives the reason `fm24ctl` stopped for, never its notices
  before the job (such as `run`'s warning of another in-game date), which
  are in the log.
- **File > Launch FM24…** (also on the start panel, and in the error box of
  a job that found no harness of this version serving) starts the game
  with the harness and a save, as `fm24ctl launch --save` does. While
  Steam has not saved the harness's launch option, the error box offers
  **Install Harness…** instead and says to set the option first, and the
  sheet warns that it would start the game directly, which has not been
  verified (see [Known limits](#known-limits)), and offers its own
  **Install Harness…**, which closes it to set the option, as **File >
  Install Harness…** waits until the sheet is closed. It lists
  the saves `fm24ctl` would find, newest first: the `.fm` files in
  `cloud/games` and `games` of FM24's data folder, the usual one or the one
  Steam's launch option gives the game (see
  [`launch --save`](CLI.md#load-a-save-launch---save)), then **Start screen
  (no save)**; **Other…** picks a `.fm` file anywhere. If Steam's launch
  option sets the data folder in a way `fm24ctl` does not follow, which
  makes it refuse every save, the sheet offers only the start screen and
  says how to write the option. On Apple silicon, **Run under Rosetta
  (x86_64)** starts the game's x86_64 slice (`--rosetta`); it is off
  unless you last launched with it. The sheet remembers what it last
  loaded. **Launch FM24** runs the app's `fm24ctl launch` and shows its
  progress as it comes; **Cancel** ends `fm24ctl`, and the game may still
  be starting, perhaps without the save. Once the save is loaded and idle,
  a window with no fixture exports the next one and shows it; over a
  fixture, the sheet says FM24 is ready. A launch without a save exports
  nothing, as the game has no save loaded yet: load one, then choose
  **Retry**. If the game runs another save, or runs natively when Rosetta
  was asked for, `fm24ctl` changes nothing and the sheet shows its reason,
  which the start panel keeps, with **Retry**, once the sheet is closed.
  As with `launch --save`, do not save a session started this way, or with
  `--launch-save`, and press **Continue** in it only with a copy of your
  save under a new name, not your backup, chosen with **Other…** or given
  to `--launch-save` as an absolute path: FM24 autosaves the save it loaded
  (see [Loading a save](#loading-a-save)), and `fm24ctl` refuses a copy with
  the name of a save in FM24's folders.
- **File > Install Harness…** runs `fm24ctl install` and shows where it put
  the harness, the Steam launch option that loads it, in a field you can
  select, with **Copy**, the account's current option if it has another
  (keep your own arguments after `%command%`: **Copy** copies only the
  harness's option), whether Steam already has that option, where
  `install` looked for Steam or the game if it found none, a warning if
  the option sets the data folder in a way `fm24ctl` does not follow, and
  that the new harness loads the next time the game starts.
- Started with `--launch-save`, the viewer launches the game with that
  save first, showing its progress in the same sheet, then exports the
  next fixture and shows it: one step from a closed game to a fixture.
  Until Steam has saved the launch option, this starts the game directly,
  without asking first, and the sheet's warning shows only as it starts:
  check the option with `fm24ctl install` first.
  `--launch-save` takes a save's name or the absolute path of its `.fm`
  file, as `fm24ctl launch --save` does (`open` starts the app in `/`, so
  the viewer refuses a relative path, saying so on the start panel), and
  `--rosetta` after it starts the x86_64 slice on Apple silicon (elsewhere
  it is ignored, as the log says). `open` passes arguments only to an app
  it starts, so quit the viewer first. Give `open -a` the app's path, as
  here: `open -a "FM24 Viewer"` opens the copy macOS knows by that name,
  which may be another build, as macOS may not know a fresh build by name
  until it has been opened. This has not been run from a closed game yet:

  ```sh
  open -a ./FM24Viewer.app --args --launch-save LIV_MAIN_FM24
  open -a ./FM24Viewer.app --args --launch-save LIV_MAIN_FM24 --rosetta
  ```

The app carries a copy of the same build's `fm24ctl` and runs it for each
job the game does. The game reads and writes only files in your temporary
directory for it: the next fixture's export, `temp-fixture.json`, and the
viewer's own folder, `fm24-viewer-<process ID>`, with copies of `.bin`
files to decode and the JSON decoded from them, the copy of a fixture to
simulate with its results, which the viewer reads as they are written for
the progress and then moves into place, the copies a replacement or swap
works on, and the team and player lists. So the viewer's jobs never make
macOS ask whether FM24 may use your Documents or Desktop; it may ask once
whether FM24 Viewer may, when that is where you save, export or keep
results. The viewer removes its folder when it quits, but
`temp-fixture.json` stays until the next export replaces it (see
[Files and removal](#files-and-removal)). The progress can trail the run by
up to one match per thread less one, because the game writes the results
in seed order.

**View > Show Log** shows what it has done, every line `fm24ctl` printed and
any errors, which also go to standard error.

## Files and removal

This section lists the files the harness, `fm24ctl` and FM24 Viewer put on
your Mac, what they change elsewhere, and how to remove it all. Your saves
are only read: the harness never writes them, and FM24 does only when you
save or when **Continue** makes it autosave (see
[Loading a save](#loading-a-save)).

Most of the files are in two folders:

- the harness folder, `~/Library/Application Support/fm24-harness/`;
- your temporary folder, which `getconf DARWIN_USER_TEMP_DIR` prints
  (`/var/folders/…/T/`, usually the same as `$TMPDIR` in Terminal).
  `open "$(getconf DARWIN_USER_TEMP_DIR)"` shows it in Finder.

`<pid>` in a name below is a process ID, and `<version>` the harness's
protocol version, which `./fm24ctl install` prints.

### What the harness adds

| What | Where | Made by | Safe to delete? |
|------|-------|---------|-----------------|
| `fm24-harness-<version>.dylib`, the harness: one file for each version installed | The harness folder | `fm24ctl install` and `launch`, any command that finds no harness (also one the viewer runs, as when it opens without a fixture and exports the next one), and the viewer's **Install Harness…** and **Launch FM24…** | Older versions, with FM24 quit (see [Tidying up](#tidying-up)). The current one only as in [Removing the harness](#removing-the-harness) |
| `launch-fm24.sh`, the launcher that Steam's launch option runs | The harness folder | As the harness, rewritten when its text changes | Only after clearing the launch option, as in [Removing the harness](#removing-the-harness) |
| `harness.log`, one line per event, shared by every version and never trimmed | The harness folder | The harness, in the game, at each start | Yes, with FM24 quit |
| `autoplay-request-<pid>.json`, `autoplay-<pid>.json`, `rosetta-request-<pid>` and `rosetta-<pid>`, the requests of `launch --save` and `--rosetta` | The harness folder | `fm24ctl` and the launcher | Yes, with FM24 and `fm24ctl` quit. They are normally gone once the game has started, and `install` and `launch` delete leftovers |
| `fm24-harness-<version>.dylib.<pid>.tmp` or `launch-fm24.sh.<pid>.tmp`, left only if `fm24ctl` is stopped while it installs | The harness folder | `fm24ctl` | Yes, when no `fm24ctl` is running |
| `fm24-harness-<version>.sock`, the harness's job socket | The temporary folder | The harness, at each start | Yes, with FM24 quit. The harness removes it when FM24 quits normally; after a crash it stays until the next start replaces it |
| `fm24-harness-<version>.sock.lock`, an empty file, one for each version | The temporary folder | The harness, at each start | Yes, with FM24 quit. Nothing removes it |
| `temp-fixture.json`, the next fixture exported by `./fm24ctl` with no arguments or by the viewer | The temporary folder | The game, for `fm24ctl` | Yes, when no export is running. The next export replaces it; nothing removes it |
| `_job_input-<pid>-<n>.bin` and `.json`, a job's input or a fetched player | The temporary folder | `fm24ctl` (the game writes the `.json`) | Yes, when no `fm24ctl` is running. Left only if `fm24ctl` is stopped during its job, or stops waiting for it |
| `fm24-viewer-<pid>/`, the viewer's work folder | The temporary folder | FM24 Viewer | Yes, when no viewer with that process ID runs. The viewer removes it when it quits; after a crash, a viewer started over a day later does |
| Empty folders named after source files, such as `commands-00051e` | The temporary folder | Apple clang, during `scripts/build.sh` | Yes, when no build is running |
| The viewer's settings: its export folder, what **Launch FM24…** last loaded and whether it used Rosetta, and the Open and Save panels' sizes and last folder | `~/Library/Preferences/io.github.graemerycyk.fm24-match-harness.viewer.plist` | FM24 Viewer | Yes, with the viewer quit, using `defaults delete` as in [Removing the harness](#removing-the-harness) |
| The viewer's **Open Recent** list | Kept by macOS | FM24 Viewer | Yes: **File > Open Recent > Clear Menu**. To remove the harness, do this first, as in [Removing the harness](#removing-the-harness) |
| Graphics caches macOS keeps for the viewer | `$(getconf DARWIN_USER_CACHE_DIR)io.github.graemerycyk.fm24-match-harness.viewer/` | macOS | Yes, with the viewer quit |
| `fm24ctl` and `FM24Viewer.app` (with its own copy of `fm24ctl`), and in a clone `fm24_tests` and its two stand-ins | The release folder, or `bin/` in the clone | The release zip, or `scripts/build.sh` | Yes, with the viewer quit. Deleting them does not remove the harness |
| The build files, including the unstripped harness | `build/` in the clone | `scripts/build.sh` | Yes, when no build is running. The next build makes them again |

Fixtures, results, lists and profiles that you or the viewer write stay
where you put them, and nothing removes them: commands run in the release
folder or `bin/` write there unless you give another folder, and the viewer
exports to `~/Documents/FM24 Match Harness` unless you chose another folder.

### What it changes elsewhere

- **Steam's launch option** for FM24, which you set in
  [Setting up](#setting-up). `fm24ctl` only reads Steam's settings, and
  never writes them. While the option is set, every start of FM24 from
  Steam loads the harness, also when you play as usual: the harness adds to
  `harness.log` and opens its socket, and on Apple silicon the game runs
  its arm64 slice (see [Known limits](#known-limits)).
  [Removing the harness](#removing-the-harness) says how to clear it.
- **FM24's record of your last game**, which the start screen's
  **Continue** opens. When `fm24ctl` or the viewer starts FM24 with a save
  (`launch --save`, a command for an exported fixture, **Launch FM24…** or
  `--launch-save`), FM24 changes the `last_saved_*` files and
  `last_save_method.xml` in `Preferences/version 1/` of its data folder to
  name the save by its full path (see [Loading a save](#loading-a-save)).
  No harness tool changes them back. After harness sessions, open your
  career from FM24's **Load Game** rather than with the start screen's
  **Continue**.
- **Autosaves beside a loaded save.** If you press **Continue** in a
  session started with a save, FM24 autosaves that save and keeps its
  previous versions beside it as `<name> (v02).fm` and `<name> (v03).fm`
  (see [Loading a save](#loading-a-save)). For a save in `cloud/games`,
  Steam Cloud may carry those changes to your other computers, which has
  not been checked.
- **FM24's file-list error log.** Harness jobs that build a match make FM24
  write `Logs/file llist errors_<date>_<time>.txt` in its data folder: the
  game code the harness runs on its own thread reads the game's file list,
  and FM24 logs a "get_list outside of main thread" entry, with a stack
  trace, each time. Each build adds about 200 entries, about 0.2 MB. Each
  export and team replacement builds a match, as does each seed of a `run`
  or a viewer simulation that rebuilds it (see [Seeds](CLI.md#seeds));
  `keep` and `--keep-build` do not rebuild. FM24 starts a new file in each
  session that builds a match. On the tested Mac, seven such sessions left
  578 MB in all, 461 MB of it from one afternoon of runs, and FM24 has not
  deleted any of them.
- **Privacy permissions.** macOS may have asked whether Football Manager
  2024 may use a folder, when the game wrote a command's output in
  Desktop, Documents, Downloads or an external volume (it may be asked about
  as Steam; see [Known limits](#known-limits)). It may also have asked
  whether FM24 Viewer may use the folder where you saved, exported or kept
  results, or whether the app you ran `fm24ctl` in, such as Terminal, may.
  None of these prompts has been checked on the tested Mac. Turn off the
  ones you allowed for the harness in **System Settings > Privacy &
  Security > Files and Folders**.

`fm24ctl`, the harness and the viewer add no login items, background
services or system settings. The launcher sets the variables that load the
harness only for the game's own process.

### Tidying up

You can delete these and keep using the harness:

- Older harness versions: the `fm24-harness-<version>.dylib` files in the
  harness folder other than the version `./fm24ctl install` prints, and
  their `.sock.lock` files in the temporary folder, with FM24 quit.
  `fm24ctl` never removes them, and the launcher loads only the current
  version.
- `harness.log`, with FM24 quit. The next start begins a new one.
- FM24's file-list error logs, with FM24 quit. Delete only those files, not
  the rest of the folder. In FM24's usual data folder:

  ```sh
  rm ~/Library/Application\ Support/Sports\ Interactive/Football\ Manager\ 2024/Logs/file\ llist\ errors_*.txt
  ```

  If Steam's launch option gives the game another data folder
  (`--user_data_location=<dir>`), FM24 may write them in `<dir>/Logs`
  instead. That has not been checked, so look there too.

- Copies of saves you made for harness work, such as
  `~/fm24-sandbox/LIV_COPY.fm`, with FM24's `(v02)` and `(v03)` autosaves
  beside them, once FM24 is closed. Keep your backup.
- Outputs you no longer need, and `temp-fixture.json` in the temporary
  folder.

### Removing the harness

1. Quit FM24. If you used FM24 Viewer, clear its recent files with
   **File > Open Recent > Clear Menu**, then quit it.
2. Before deleting anything, clear Steam's launch option. In Steam, open
   **Football Manager 2024 > Properties > General > Launch Options** and
   delete `/bin/sh "…/launch-fm24.sh" %command%`, keeping any arguments of
   your own that came after it, or leave the field empty if there were
   none. Steam runs the option with `/bin/sh`, which stops with "No such
   file or directory" once the launcher is gone and never runs the game, so
   Steam cannot start FM24 while the option names a deleted launcher. This
   was checked with `/bin/sh` alone, given a missing launcher and a command
   after it: it exited with code 127 without running the command. It has
   not been tried through Steam. Reopen Properties to check that the field
   kept your change, and clear it again if it did not. Steam may save the
   change only when it quits; to check what it has saved, quit Steam and
   run:

   ```sh
   grep -l launch-fm24.sh ~/Library/Application\ Support/Steam/userdata/*/config/localconfig.vdf
   ```

   It prints the settings file of each Steam account whose launch option
   still names the launcher, and nothing once none does. Do not go on to
   step 3 until it prints nothing. If it prints a file, edit that file with
   Steam still quit: in the game's block (`"2252570"` under `"apps"`),
   delete `/bin/sh \"…/launch-fm24.sh\" %command%` from the
   `"LaunchOptions"` line, keeping any arguments of your own, or delete the
   whole line if there are none. Then run the check again. This edit has
   not been tried with Steam. Do not put back an earlier copy of
   `localconfig.vdf`, such as a backup from [Setting up](#setting-up): it
   would undo every Steam setting changed since.

   Do not check with `fm24ctl install`, and do not open FM24 Viewer again:
   `install`, and any other `fm24ctl` command that finds no harness,
   installs the harness again, and the viewer runs one to export the next
   fixture when it opens without a fixture.
3. Delete the harness folder. This deletes everything in it, so first move
   out anything of your own that you keep there, such as a copy of
   `localconfig.vdf`:

   ```sh
   rm -rf ~/Library/Application\ Support/fm24-harness
   ```

4. Delete the harness's files in your temporary folder. With `-print` in
   place of `-exec rm -rf {} +`, the command lists them instead:

   ```sh
   find "$(getconf DARWIN_USER_TEMP_DIR)" -maxdepth 1 \
     \( -name 'fm24-harness-*.sock*' -o -name '_job_input-*' \
     -o -name temp-fixture.json -o -name 'fm24-viewer-[0-9]*' \) \
     -exec rm -rf {} +
   ```

   This leaves the compiler's empty folders. On the tested Mac, the
   temporary folder held nothing from before the Mac last restarted, so a
   restart seems to clear them.
5. If you used FM24 Viewer, delete its settings and caches:

   ```sh
   defaults delete io.github.graemerycyk.fm24-match-harness.viewer
   rm -rf "$(getconf DARWIN_USER_CACHE_DIR)io.github.graemerycyk.fm24-match-harness.viewer"
   ```

6. Move anything you want to keep out of the release folder, or `bin/`,
   then delete that folder or the clone, for example with
   `rm -rf ~/FM24-Harness-macOS-<version>` or `rm -rf ~/FM-24-Match-Harness`.
7. If you like, delete your save copies, outputs and FM24's file-list error
   logs (see [Tidying up](#tidying-up)), and turn off the privacy
   permissions listed under
   [What it changes elsewhere](#what-it-changes-elsewhere).

Removal leaves your saves and your career's backup, FM24's own settings
(its record of your last game included) and the rest of Steam's settings
as they are. It also leaves the tools you installed to build the harness,
such as the Command Line Tools, Homebrew and CMake (`brew uninstall cmake`
removes CMake), and Rosetta if you installed it.

## How it works

`fm24ctl` cannot load the harness into a running game: the game is signed
with the hardened runtime, which denies other processes access to it. The
game is, however, entitled to accept `DYLD_INSERT_LIBRARIES` and libraries
signed by others, so the harness is loaded when the game starts. Steam runs
the launch option through `/bin/sh`, which drops `DYLD_*` variables, so the
launcher script sets `DYLD_INSERT_LIBRARIES` itself and starts the game
executable, keeping the libraries Steam inserts for its overlay
(`STEAM_DYLD_INSERT_LIBRARIES`).
On Apple silicon, Steam starts the launch option as an Intel process, and a
game started from it would inherit that and run under Rosetta. The launcher
therefore starts the game through `/usr/bin/arch` in the same process, so
Steam still tracks the game: with the arm64 slice where `sysctl` reports
Apple silicon, and the x86_64 slice on Intel Macs, or under Rosetta when a
waiting `launch --rosetta` has left a request for it (taken as described
under [Loading a save](#loading-a-save)). Without such a request the
launcher always starts arm64 on Apple silicon. The direct start asks `open`
for the x86_64 slice (`open --arch x86_64`) for `launch --rosetta`, and
otherwise leaves the choice to macOS, which follows the app's "Open using
Rosetta" setting. Once the harness is ready, `launch --rosetta` checks with
`sysctl` that the game's process runs under Rosetta.

The harness's initialiser starts a thread that identifies the game by the
`LC_UUID` of the running slice, checks the code it calls, and serves jobs on
a Unix socket in your temporary directory (`fm24-harness-<version>.sock`,
see `src/common/protocol.h`), which `fm24ctl` connects to. Only processes of
your user can connect. The harness holds a lock on
`fm24-harness-<version>.sock.lock` while it serves, so `fm24ctl` can tell a
running harness from a socket left behind by a game that crashed. Job input
files go to the same temporary directory, which the game can always read.

The harness log is `~/Library/Application Support/fm24-harness/harness.log`,
next to the dylib the game loaded.

### The game's state

The `status` job, the check before every other job and the `save` and
`game_date` keys of exports read a few of the game's own objects, and never
write to them (`src/dll/game_state.c`): the game options' "game in
progress" flag, the `GAME` object's processing flag and kind and its
auto-play state, the start screen's object, the people list, the save
system's save name and the in-game date. The game's own code reads and
writes the same fields: they were found in its start screen, auto-play
setup, load and processing functions, in both slices. Their addresses are
in the mapping like the others (see [Build tables](#build-tables)) and their
offsets in `src/dll/game.h`; [Technical notes](TECHNICAL.md#use) lists them.
The jobs run on the harness's thread, so the check cannot stop the game from
starting to process just after it: keep the game idle while commands run.

A load from the menu raises the game's processing flag. The start screen's
auto-play load did too, in the game: the save's name appeared a few seconds
before the load, while the game still read as `starting`, then the game read
as `busy` (in progress and processing) until it was `ready`. The game's code
on disk suggested that this load sets "in progress" before its last steps
without processing, so the harness also counts a game as `loading` until the
start screen's first setup, of which that load is part, has finished: the
start screen's object is created only then, and deleted only when the game
quits. Pressing **Continue** raises the processing flag for each day's
processing (kind `1`), and jobs sent then are refused; between those, as the
day moves on through its hours, the game reads as idle.

### Loading a save

`launch --save` uses the game's own unattended start, its auto-play file
(the `auto_play_game_file` setting). In the game's code, a `--name=value`
argument overrides the setting `name`, and when the start screen is first
reached with no game loaded, the game parses the file the setting names as
JSON. If the file has a `LOAD_GAME` string and no `PLAYABLE` key, the start
screen loads that save, on the game's main thread. The file can have other
keys, which create a new game (`PLAYABLE`, whatever its value), save
(`AUTO_SAVE`, `QUICK_START_FILE` and others) or run the game on; `fm24ctl`
writes only `LOAD_GAME`, from a fixed template, and passes no other
argument. The harness changes nothing in the game: it only reads the state
that [`status`](#the-games-state) reports.

Steam starts the game with `steam://rungameid`, which takes no arguments
(`steam://run` with arguments asks for confirmation each time). So
`launch --save` writes a request next to the launcher, named after its own
process (`autoplay-request-<pid>.json`), and the launcher, which Steam runs,
takes it if that `fm24ctl` is still running and the request is under five
minutes old: it renames it `autoplay-<pid>.json` after its own process,
which becomes the game's, and adds `--auto_play_game_file=<that file>` to
the game's arguments. A game started from Steam without a waiting
`launch --save` finds no request and starts as usual, even if an
interrupted `fm24ctl` left one behind; `install` and `launch` delete such
leftovers. The direct start passes the argument with `open --args`. The other way the game has to load a save at startup, its
`load_game` setting, is a save tester that can move save files, so
`fm24ctl` does not use it.

`fm24ctl` then waits for the harness to report the game `ready` with that
save three times in a row. A game at its start screen 15 seconds after
reaching it did not load the save: `launch` says whether the game read the
auto-play file at all (`autoplay_read` in the status).

What this means for your saves:

- The save is loaded, not written: a save in `cloud/games` loaded this way,
  used by harness commands and closed without saving was unchanged byte for
  byte.
- The load does change FM24's record of your last game (the start screen's
  **Continue**): the `last_saved_*` files and `last_save_method.xml` in
  `Preferences/version 1/` name the save by its full path, with save method
  `0`, instead of by its name in the cloud folder, with method `1`. It is
  the same file, but a later manual save may still be written elsewhere,
  which has not been checked: do not save in a session started this way.
  Match builds also leave changes in the game's memory, so quit without
  saving after harness work.
- Harness commands never move the game on, but pressing **Continue** does,
  and FM24 then autosaves the loaded save: it writes the file it loaded,
  wherever that is, keeping the previous versions as `<name> (v02).fm` and
  `<name> (v03).fm` beside it, and records it as your last game. Press
  **Continue** only in a copy, such as one outside FM24's folders as in the
  second example of [`launch --save`](CLI.md#load-a-save-launch---save).
- Returning to the start screen from a career quits FM24, and Steam starts
  it again: the new game loads the harness through the launch option and,
  with no request waiting, stays at its start screen with no save loaded.
- The auto-play setup also turns off three statistics settings
  (`process_stats_enabled`, `game_stats_enabled`, `output_comp_stats`) for
  the session; they were not saved into `Preferences/`.

`launch --rosetta` leaves a request for the x86_64 slice the same way as a
save's, `rosetta-request-<pid>`, which the launcher takes under the same
conditions: it renames it `rosetta-<pid>`, deletes it and starts that slice. The
two requests are independent, so `--rosetta` works with or without `--save`,
and the file handed to the game stays `{"LOAD_GAME": "<path>"}`.
`launch` deletes its Rosetta request once the wait for the harness is over,
and `install` and `launch` delete those of an `fm24ctl` that has exited.

### Build tables

The harness code shared with the original Windows project names game code by
the RVAs of the game's Windows Steam build (`src/dll/game.h`, `sites.h`,
`site_members.c`), and labels every field of a serialised match from the call
stack of the write that produced it. The macOS build of the game is compiled
by clang with the Itanium C++ ABI: its code is laid out, inlined and
tail-called differently, and many objects are laid out differently (libc++
containers are smaller than MSVC's). The harness keeps all its logic and
adds per-architecture tables (`src/dll/game_macos_tables.c`, see
`src/dll/game_macos.h`):

- `addresses`: the macOS counterpart of each Windows RVA the harness calls or
  reads (functions, vtables, globals), and the game's state objects, which
  have not been located in the Windows build yet.
- `frames`: for every return address in the macOS serialisers, the Windows
  frames it stands for. One macOS frame may stand for several Windows frames
  (a call clang inlined) or for none (a call MSVC inlined or turned into a
  jump). The write trace translates each macOS stack this way, so the labels,
  member names, per-seed alignment and team replacement work unchanged. A
  frame without a Windows counterpart keeps its own identity
  (`0x80000000 | macOS offset`); in `--full` configs such values appear in
  `paths` and in `site` values, mostly as the caller after the `/` (as in
  `1a7fe22/84565bf0`).
- `signatures`: the first bytes of each function the harness calls, checked
  at startup.
- The thread-local PCG32 the pre-match setup draws from.

The macOS object layouts are in `src/dll/game.h` beside the Windows ones.

The tables come from `tools/macos/steam-24.4.2.json`, generated into C by
`tools/macos/gen_tables.py`:

```sh
python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c
```

The mapping was made by comparing the Windows reference code that
`src/runner/probe_patterns.c` carries with the macOS slices, and the two
macOS slices with each other. [`tools/macos/README.md`](../tools/macos/README.md)
describes the mapping format, how it was made and `check_mapping.py`, which
checks a mapping against the installed game.

## Configs

Exported configs have `"game_build": 2081827`, the game's bundle version,
where the original Windows project records the Windows build's PE timestamp.
Field names, values and the `.bin` encoding are the same as the original
Windows project's. Match results for a seed are not expected to match
between the two slices, or with the Windows build of the game: the compilers
generate different floating-point code. This has not been checked: the same
input has not been run on both slices, or on macOS and Windows.

## What has been checked

All checks ran on one Apple silicon Mac, an M1 with macOS 27, with FM24
24.4.2 from Steam started through the Steam launch option.

- On the arm64 slice, every command, `decode` included, and every `run` flag
  has been run; those that need the game ran against a save in the running
  game.
- Also on the arm64 slice, with protocol 1.3.0:
  - `launch --save` with a copy of a save outside FM24's folders: Steam
    started the game through the launcher without asking, the launcher took
    the waiting request, and the game loaded the save from the auto-play
    file and was `ready` 35 seconds after the command. The request was used
    once.
  - `status` through the load and in play: the save's file name and the
    in-game date were right, and the game read `busy` while it loaded and
    while **Continue** processed each day. All six jobs sent then were
    refused; no job was refused while the game was idle.
  - The `save` and `game_date` of exports, a fixture naming another save
    refused with exit code `3` and played with `--any-save`, and
    `--no-launch` refusing with the game closed. With the game closed, `run`
    on a fixture naming a save in `cloud/games` started the game with it and
    played, 21 seconds in all.
  - An export on match day: your own line-up and tactic once the day had
    reached the match (**Go to Match**), but not in the morning, even after
    confirming the team.
  - Returning to the start screen, jobs there, and the effects listed under
    [Loading a save](#loading-a-save), except saving manually afterwards.
- On the x86_64 slice, under Rosetta, only exporting the next fixture, `run`
  with `keep` and with `--keep-build --detailed`, and `list-teams` have run,
  all before later fixes to the port. `list-teams` came back incomplete
  there and later exports failed with "no upcoming fixture"; the fix for
  both has not been checked on this slice. Per-seed rebuilds, team
  replacement, the player commands, `decode` and everything added in
  protocol 1.3.0 have not run on it.
- Not checked yet: Intel Macs and older macOS versions; the direct start,
  with or without `--save`; saving manually in a session started by
  `launch --save`; the harness refusing an unsupported build when the game
  starts; the message about another harness version; writing outputs into
  Desktop or Documents; and running the scripts in `tools/experiments`.
- `launch --rosetta` has not run against the game either: whether Steam's
  start, through the launcher, then runs the x86_64 slice with the harness
  and Steam's overlay, how long Rosetta's first translation takes, whether
  `open --arch` starts the game's x86_64 slice, and the check of the slice
  once the harness is ready.
- Unit tests only, without the game: recognising the supported build by its
  slice UUID and refusing other builds and their jobs, finding another
  harness version's socket, `probe` on single-slice, damaged and other
  builds' files, the `open` command of the direct start, and the game state
  above, on synthetic objects. For `launch --save`: finding saves (also in
  the data folder a launch option gives the game, and refusing a copy with
  a save's name), the auto-play file, the launcher's handling of the request
  (with stand-ins for the game, and requests of an `fm24ctl` that has
  exited), a start for a command's job up to a failed `open`, the files
  left after each outcome (and the auto-play files kept while a game
  runs), the wait over scripted states, and the checks of a running game
  against a stand-in harness. For the commands' save check:
  what happens in each state of the game, the commands' arguments with and
  without the new options, reading a fixture's `save` and `game_date`, and
  the check against a stand-in harness. For `launch --rosetta`: its
  arguments and the refusal without Apple silicon, the launcher's handling
  of the request (with a stand-in game that records whether it runs under
  Rosetta, alone and with a save's request, and requests that are too old,
  misnamed or of an `fm24ctl` that has exited), the request's life during a
  start up to a failed `open`, the `open` command of the direct start, the
  longer wait for the harness, and telling processes under Rosetta from
  native ones, for a running game against a stand-in harness and once a
  start's harness is ready.
- The fixture viewer, without the game: unit tests of its C core and its
  models on both slices, including its edits, saves and questions;
  tests of its harness bridge against a stand-in for `fm24ctl` (the export
  at startup and its failures, decoding `.bin` files, exports that never
  replace a file, the paths the game is given and reading `fm24ctl`'s
  output; simulations whose results the stand-in writes row by row: the
  live progress, the copy played, where the results go and failed runs;
  and team replacements and player swaps: the lists and their searches,
  which mirror the original Windows project's viewer tests, the edits and
  venue values carried over, and the errors); unit tests of `fm24ctl`
  opening fixtures in it, with the system's `open` stubbed out; offscreen
  renders of its window, team picker and swap prompt with synthetic and
  real exports; checks in an offscreen window of the venue and simulation
  fields (typing, the length limits and the cursor moved to an invalid field),
  the progress bar, Return starting a simulation, the squad lists' context
  menu, and the pickers' typing, arrow keys, Return and Escape; Return and
  Escape on its questions, without showing them; an export onto an exFAT
  disk image; and short launches with a sample fixture. With protocol
  1.3.0, against the stand-in, whose texts a test finds in `fm24ctl`'s
  sources and which, as a test checks, line-buffers its output as
  `fm24ctl` does (a unit test of `fm24ctl`'s startup checks that each line
  reaches a pipe as it is printed): the save check's refusal when another
  save is loaded, with **Run Anyway** running the job again with
  `--any-save`, the game started with a fixture's save and its progress,
  the wait while the game is busy with it, until it is idle or past the
  timeout, the harness refusing other jobs while the game is busy, a run
  that fails after `fm24ctl`'s notices, and the `save` and `game_date`
  kept in saves, exports and the copies the game is given and shown in
  the summary, but not in a `.bin` copy. Launching and
  installing, against the stand-in's `launch` and `install`: the saves
  listed from made-up save folders, which a test of `fm24ctl` searches
  too, finding the same saves (also in the data folder a launch option
  gives the game); the arguments `fm24ctl` gets, its progress as it comes,
  **Cancel** ending it, the export after a launch with no fixture on
  show, the reasons for failures, which the start panel keeps, the
  remembered choice, a data folder `fm24ctl` does not follow, and
  `--launch-save` (refusing a relative path, and `--rosetta` off Apple
  silicon); the launch option **Install Harness…** reads from `install`'s
  output and copies, with the account's current option and `install`'s
  advice, and the start panel following an install that finds the option
  set; the error box offering **Launch FM24…**, or **Install Harness…**
  while Steam's launch option is not set, as the start panel's text and
  the sheet's warning, with its own **Install Harness…**, then also say;
  and a seed pattern `fm24ctl` refuses, whose reason the error box shows.
  The offscreen renders of both sheets predate those launch-option texts
  and buttons, which no render shows. Its menus, panels, alerts, popover,
  sheets, Dock badge, drag and drop and keyboard use have not been tried
  systematically by hand. What it has done with the game is listed next.
- The fixture viewer with the game, by hand on the arm64 slice with
  protocol 1.3.0: the export at startup; replacing the home team and then
  the away team, each keeping the other edits; **Swap player** refusing a
  player already in the fixture; saving a fixture as `.bin` and opening it
  again, which the game decoded; and a simulation of one seed.
- Not yet run, by hand with the game, for the fixture viewer (record each
  here once it has been):
  - on the arm64 slice: the export link (**Click here**, or **File >
    Export**) into the export folder in Documents, and whether macOS asks
    whether FM24 Viewer may use it (again after a rebuild); a simulation of
    several seeds with its live progress and Dock badge; swapping in a
    player who is not already in the fixture, and a swap with **Allow
    duplicates?** ticked; saving a decoded `.bin` as JSON; and `fm24ctl`
    opening a fixture in the viewer while the game runs;
  - a simulation, replacement and swap of an exported fixture with FM24
    closed, which `fm24ctl` starts with the fixture's save (watching its
    progress in the window), and with another save loaded, answering
    **Cancel** and **Run Anyway**;
  - **Launch FM24…** with a listed save, at the start screen, with
    **Other…** and under Rosetta; **Cancel** while the game starts (also
    on a direct start, without Steam's launch option, then **Retry**
    before the game reaches its start screen: the save should still load);
    the export that follows a launch; and a launch refused for another
    save or slice;
  - a simulation, replacement and swap of an exported fixture while FM24
    is processing (after **Continue**), watching `fm24ctl` wait in the
    window;
  - the Launch FM24 and Install Harness sheets asked for while the viewer
    window is in the Dock (from the Log window's menu bar), which should
    bring the window back with the sheet;
  - `--launch-save <save>` (`open -a ./FM24Viewer.app --args
    --launch-save <save>`), from a closed game to a fixture on show;
  - **Install Harness…** on a fresh install, pasting its copied option into
    Steam, and the next start loading the new harness;
  - all of those once under Rosetta, on the x86_64 slice;
  - an export on match day, after submitting your team;
  - saving and simulation results written into Documents, and whether
    macOS asks for access (again after a rebuild);
  - the window itself: its menus, the Open and Save panels, the popover,
    sheets and alerts, Open Recent, drag and drop, **Open With**, and
    keyboard use, with and without Full Keyboard Access;
  - macOS 13 and 14.

## Known limits

- Starting the game directly, without Steam's launch option, has not been
  verified: in its one attempt the game crashed about four seconds after
  loading the harness. While Steam has not saved the launch option, this
  covers `fm24ctl launch`, FM24 Viewer's **Launch FM24…** and
  `--launch-save`, and the commands that start the game for a fixture's
  save, also when the viewer runs them for a simulation, replacement or
  swap; it also covers `launch` given a path to `fm.app`. They say so as
  they start the game, and none asks first. The viewer's start panel,
  error box and Launch FM24 sheet also say so before **Launch FM24…**; for
  `--launch-save` the sheet says so only once the start is under way, and
  for the viewer's jobs only `fm24ctl`'s progress says so. Set the launch
  option, and check it with `fm24ctl install`, first.
- The x86_64 slice (Intel Macs, and Apple silicon under Rosetta) is largely
  untested: a few commands ran on it under Rosetta before later fixes, and
  nothing has run on an Intel Mac (see
  [What has been checked](#what-has-been-checked)).
- FM24 Viewer's **Launch FM24…** and `--launch-save` have not been run from
  a closed game yet, nor **Install Harness…** on a fresh install.
- Only the Steam build 24.4.2 (2081827) is supported for now (Epic Games
  support is coming soon). `fm24ctl probe
  [path to fm.app]` prints a build's slice UUIDs and whether this harness
  supports them; unsupported builds are refused at startup, and every job
  then reports so.
- `site` and `paths` values in `--full` configs contain macOS-only values
  where the Windows code had no reference bytes (see
  [Build tables](#build-tables)).
- Writing outputs into Desktop, Documents, Downloads or an external volume
  can make macOS ask whether Football Manager 2024 may access it, because the
  game writes them; a game Steam started may be asked about as Steam, which
  has not been checked. Allow it, or run `fm24ctl` from another folder. If
  access is denied, the game's job fails, saying only that it could not
  write the file. Before it starts the game or submits the job, `fm24ctl`
  checks that an output is not a folder, that its folder exists, and that
  you may write the output there: for a new file, in its folder, which must
  not be locked; for an existing one, the file itself, which must not be
  locked or append-only (an existing file in a locked folder is written in
  place, so it goes ahead). macOS decides privacy access for each app: a
  folder it keeps the app `fm24ctl` runs in (such as Terminal) out of may
  still be open to the game, so for a folder that macOS reports as closed
  to `fm24ctl` ("operation not permitted"), `fm24ctl` refuses only the
  outputs it opens itself, `run`'s results, which it reads back, and
  `inject-player`'s output, and leaves the others to the game. Which error
  macOS gives `fm24ctl` for such a folder, and whether checking it makes
  macOS ask about the app `fm24ctl` runs in, has not been checked: if the
  check passes there, `run` plays the matches and then cannot read its
  results ("no output written").
- A game started without the harness has to be restarted with it: the harness
  cannot be loaded into a running game.
- `launch --save`, and the commands that check a fixture's save, load a save
  only when they start the game. A game already at its start screen has to
  load the save itself, and one with another save loaded is left alone.
- On Apple silicon, a game started through the Steam launch option runs the
  arm64 slice even if `fm.app` is set to "Open using Rosetta": the launcher
  chooses the slice itself, and runs x86_64 only for a waiting
  `fm24ctl launch --rosetta`: a start from Steam itself always runs arm64,
  as does a command that starts the game through Steam for its save. The
  setting applies when the game is opened directly without `--rosetta`
  (`fm24ctl launch` or a command without the launch option, or `launch`
  with a path). `launch --rosetta` never changes the slice of a running
  game. `harness.log` names the slice that is running.
- `fm24ctl` installs each harness version as `fm24-harness-<version>.dylib`
  in `~/Library/Application Support/fm24-harness/` and never removes older
  versions' files. They can be deleted while the game is not running (see
  [Tidying up](#tidying-up)).
