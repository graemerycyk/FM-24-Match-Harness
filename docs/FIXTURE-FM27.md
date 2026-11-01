Editing an FM27 fixture file
===

The FM27 edition (see the [FM27 guide](MACOS-FM27.md)) writes fixture
files in its own format, `fm27-match-config/1`. They work like FM24's: the
same rules, value keys and notes apply, and [Editing a fixture
file](FIXTURE.md) describes them. This page describes what is different in
FM27's files.

**Nothing on this page has been checked with the game.** FM27's field
names come from comparing the game's code with FM24's, without running it
(see [How the frames were made](../tools/macos/README.md#how-the-27002486716-frames-were-made)).
Where FM27 writes a value the way FM24 does, it gets FM24's name; where
its serialiser changed, the field is named after what its code shows, or
left unnamed. The names have not yet been compared with an exported
fixture and the game's screens, so treat them as likely, not certain.

# The header and the seed

```json
{
  "format": "fm27-match-config/1",
  "game_build": 2486716,
  ...
}
```

`game_build` is the last part of the game's version: 2486716 for
27.0.0.2486716, the free preview. The match block starts with the game's
version (four bytes, the most significant first), the format version
(`u16`, 76 in this build), a zero and a tag, then the match's label, and
the seed is the field with `"role": "seed"`, as in FM24. `engine_version`
gives those first four bytes, as FM24's files give the version in theirs.
A file only plays in a build with the same format version.

# The blocks

The blocks are FM24's (see [Finding your way
around](FIXTURE.md#finding-your-way-around)), with these differences:

| Block | In FM27 |
|---|---|
| `match` | the header above; the weather and the observers as in FM24 |
| `official[0..3]`, `referee` | as in FM24 |
| `extra_1776` | new: a list of objects the match holds (at `+0x6f0`), each written by its own code. An empty list writes nothing |
| `home.staff[0..2]` | `staff[0]` and `staff[2]`, the analyst, with `has_staff[2]` before it. FM27's team has no `staff[1]` (FM24's doctor) and no `has_staff[1]` |
| `home.manager.tactic` | see [Tactics](#tactics) |

The rest (`venue`, `fixture`, `fixture_extra`, `team`, `team_info`,
`manager`, `player[i]`, `team_extra`, `backroom[i]`, `extra_53952`,
`extra_1872`) is named as in FM24.

# Players

A player block holds the same values as in FM24, under the same names:
`person_uid`, the 69 `ratings`, the values copied from the database
(`condition`, `sharpness[0]` and `[1]`, `fatigue`, `morale`, reputations,
`current_ability` and the rest), the person's values (age, nationality,
personality, height, weight) and the role ratings. FM27 writes a few
values FM24 does not; they have no name.

## Tactical familiarity

FM27's tactical familiarity has no duties and six setting categories
instead of seven. Whether the categories are FM24's (and which one went)
is not known yet, so they are numbered:

| Name | Type | Meaning |
|---|---|---|
| `.tactical_familiarity.version` | `u8` | as in FM24 |
| `.tactical_familiarity.position_role.count` | `u32` | how many position and role entries follow |
| `.tactical_familiarity.position_role[N].key` | | the position and role |
| `.tactical_familiarity.position_role[N].amount` | `u32` | 0-10000 |
| `.tactical_familiarity.category_K.count` | `u32` | how many entries category K (0-5) has |
| `.tactical_familiarity.category_K[N].setting` | `u8` | the tactic setting the entry is for |
| `.tactical_familiarity.category_K[N].amount` | `u32` | 0-10000 |

**In the viewer**, the tactic's setting for each category is not known, so
the squad lists' **Familiarity 0** to **Familiarity 5** columns show each
category's best entry: the highest `amount`, how well the player knows the
setting they know best, or `None` for a category with no entries. That is
not the game's bar, which compares the entries with the tactic's setting,
as in FM24. **Max familiarity** sets every entry's `amount` to 10000, so
every setting the player has an entry for becomes fully known; a category
with no entries can't gain one, and `position_role` isn't shown or changed.

# Tactics

FM27's tactics have a formation in possession and one out of possession,
and roles without duties. Each manager's tactic is in
`<side>.manager.tactic`:

| Part | What it is |
|---|---|
| `takers` | captains and set-piece takers, as in FM24 |
| `part_4240` | opposition instructions, as in FM24 |
| `plan[0]`, `plan[1]`, `plan[2]` | three tactics; `plan[0]` is presumably the one in use, as in FM24 |
| `extra` | new: eight small values, after the plans |
| `set_pieces` | set-piece routines, as in FM24 |
| `slot_6920[0..3]`, `formation[0..24]` | as in FM24 |

## A plan

| Name | Type | Meaning |
|---|---|---|
| `.mentality` | `u8` | as in FM24's plan; its values have not been checked |
| `.team_instructions[0..2]` | `u32` | bit sets, as in FM24; their bits are not named |
| `.slot[i].ip.*` | | slot `i` (0-10) in the plan's first formation |
| `.slot[i].oop.*` | | slot `i` in its second formation |
| `.slot[i].unknown_7b4` | `u8` | a byte for each slot; its meaning is not known |

The game writes each of the eleven slots as two halves with the same
fields, the first from one array and the second from another. Which is in
possession is a guess: `ip` is the first, as the game's own names put
"in possession" first. Each half has:

| Name | Type | Meaning |
|---|---|---|
| `.position` | `u32` | a position bit set; its `flags` use FM24's [position bits](FIXTURE.md#formation-positions-roles-and-duties), not checked for FM27 |
| `.player_instructions[roleN]` | `u64` | the role bit `N` keying a set of player instructions |
| `.player_instructions[roleN].bits[0..1]` | `u32` | that set's instructions: bits, not named |
| `.role` | `u64` | the role bit set, written last; FM27's role bits are not named |

The roles are named by their bit (`role5`), because FM27's roles differ
from FM24's and have not been matched with the game's.

**In the viewer**, each side has two pitches, **In possession** for the
`ip` halves and **Out of possession** for the `oop` halves of `plan[0]`,
and the squad lists show each half's position and role (**IP Position**,
**IP Role**, **OOP Position**, **OOP Role**), a role by its bit, as
`Role 5`. The heading gives both formations, as `[IP 4-2-3-1, OOP 4-4-2]`.

**Edit tactics with care.** As in FM24, change values, not structure. How
the engine reads FM27's tactics has not been tested at all.

# Research keys

In a `--full` export, `site`, `member` and `paths` work as in FM24 for the
fields FM27 shares: they name the code of FM24's Windows build. A field
only FM27 writes, or a call only FM27 makes, gets a synthetic ID of its
own instead: `7e000001` to `7effffff`. The IDs FM27's labels use are named
in `src/dll/sites_fm27.h`, and every ID, with the code it stands for, is
listed in the mapping's `sites` (`tools/macos/steam-27.0.0.2486716.json`).
They stay the same when the mapping is rebuilt, and `tools/macos/remap.py`
carries them to later builds of FM27. No call path in an export holds an untranslated frame
(`0x80000000` plus an offset), except below the virtual calls FM27 makes
in `extra_1776` and in a few other places the mapping has not followed.

The FM24 fields FM27 does not write, 119 of the 1,180 sites and members the
labels use, are listed under `absent` in the same mapping, each with the
reason: mostly values FM27 writes elsewhere, together or not at all, in
the match header, the team's base part, staff entries and the tactic's
takers.

# Not known yet

- Every name on this page, until an export is compared with the game.
- FM27's role IDs, team and player instruction bits, the six familiarity
  categories, and which formation is in possession.
- What `extra_1776`, the tactic's `extra` and `unknown_7b4` hold.
- Whether the engine reads the tactics, as for FM24.
