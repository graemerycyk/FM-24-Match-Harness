# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-match rows of tests/dll_test.c's synthetic matches, computed independently of the
harness: the expected rows and SHA-256 digests in tests/dll_test.c and tests/job_run_test.c.

  python3 -I tests/synthetic_rows.py rows <match seed> <index> <seed column>
  python3 -I tests/synthetic_rows.py files <first index> <seed or -> ...

`rows` prints one match's player rows and team rows (without headers) and the digest of each.
`files` prints the digests of a job's -match-players.csv and -match-teams.csv for seeds in list
order, starting at an index; a '-' takes a place that has no rows (a skipped seed).

It follows dllTest_makeMatch and the rules of docs/TECHNICAL.md (per-match rows), not the C code:
minutes and appearances as the game counts them, ratings in signed hundredths, f32 values to nine
significant digits, and team values summed over the players who appeared."""
import hashlib
import math
import struct
import sys

M32 = 0xFFFFFFFF
NAMES = ('goals assists shots shots_on_target blocked_shots long_shots xg xa key_passes '
         'clear_cut_chances half_chances passes_attempted passes_completed progressive_passes '
         'crosses_attempted crosses_completed dribbles tackles_attempted tackles_won key_tackles '
         'interceptions clearances blocks shots_blocked pressures_attempted pressures_completed '
         'headers_attempted headers_won key_headers fouls fouled offsides yellow_cards red_cards '
         'corners throw_ins defensive_free_kicks attacking_free_kicks saves_held saves_parried '
         'saves_tipped goals_conceded possession_won possession_lost high_intensity_sprints '
         'distance_km').split()
TEAM = ('goals possession average_rating xg shots shots_on_target blocked_shots long_shots '
        'clear_cut_chances half_chances corners fouls fouled offsides passes_attempted '
        'passes_completed progressive_passes crosses_attempted crosses_completed tackles_attempted '
        'tackles_won headers_attempted headers_won interceptions clearances blocks shots_blocked '
        'pressures_attempted pressures_completed yellow_cards red_cards saves distance_km key_passes '
        'dribbles possession_won possession_lost high_intensity_sprints').split()
V = {name: i for i, name in enumerate(NAMES)}
F32 = {V['xg'], V['xa']}
KM = V['distance_km']
MANY = {V['passes_attempted'], V['passes_completed'], V['pressures_attempted'],
        V['possession_lost']}
PLAYERS_HEADER = ('index,seed,side,team_uid,slot,person_uid,in_squad,started,appeared,minutes,'
                  'on_minute,off_minute,injured_minute,sent_off_minute,stats_flags,rating,'
                  + ','.join(NAMES) + '\n')
TEAMS_HEADER = 'index,seed,side,team_uid,squad,used,' + ','.join(TEAM) + '\n'
SUBSTITUTE = 0x20


def f32(x):
    """x rounded to a float, as the C code's float arithmetic leaves it (one operation at a time;
    a double holds a float operation's exact result closely enough to round it the same way)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def mix(seed, a, b):
    x = ((seed * 0x9E3779B1) & M32) ^ ((a * 0x85EBCA77) & M32) ^ ((b * 0xC2B2AE3D) & M32)
    x ^= x >> 16
    x = (x * 0x7FEB352D) & M32
    x ^= x >> 15
    x = (x * 0x846CA68B) & M32
    return x ^ (x >> 16)


def role(seed, side, slot, squad):
    from_end = squad - 1 - slot
    if from_end == 0:
        return 'absent' if side and seed % 2 == 0 else 'unused'
    if from_end == 1:
        return 'sub_on'
    if from_end == 2 and not side:
        return 'stoppage'
    return ('full', 'off', 'injured', 'sent_off', 'negative')[slot] if slot < 5 else 'full'


def player(seed, side, slot, squad):
    base = 200000 if side else 100000
    uid = base + (slot + seed) % squad
    if not side and slot == squad - 1 and seed % 3 == 0:
        uid = base + 50
    key = side * 64 + slot
    values = []
    for v in range(len(NAMES)):
        m = mix(seed, key, v)
        if v in F32:
            values.append(f32(f32(m % 1000003) / 1300021.0))
        elif v == KM:
            values.append(f32(4000.0 + f32(f32(m % 8000000) / 997.0)))
        else:
            values.append(m % (61 if v in MANY else 5))
    if not side and slot == 0:
        values[V['xg']] = 0.0
    length = 90 + seed % 4
    off_minute = 46 + mix(seed, side, 900) % 40
    kind = role(seed, side, slot, squad)
    flags = mix(seed, key, 901) & 0x100
    rating = 600 + mix(seed, key, 902) % 300
    on = off = injured = sent_off = 0
    if kind == 'off':
        off = off_minute
    elif kind == 'injured':
        injured = 10 + seed % 30
    elif kind == 'sent_off':
        sent_off = 60 + seed % 25
    elif kind == 'negative':
        rating = -5 - (seed % 3) * 50
    elif kind == 'stoppage':
        on = length
    elif kind == 'sub_on':
        on = off_minute
    if kind in ('stoppage', 'sub_on', 'unused', 'absent'):
        flags |= SUBSTITUTE
    in_squad = kind != 'absent'
    minutes = 0
    if in_squad:
        went_off = off or injured
        if flags & SUBSTITUTE:
            if went_off:
                minutes = went_off - on
            elif sent_off:
                minutes = sent_off - on
            elif on:
                minutes = length - on
        else:
            minutes = went_off or sent_off or length
        minutes = max(minutes, 0)
    return {'uid': uid, 'slot': slot, 'in_squad': in_squad,
            'started': in_squad and not flags & SUBSTITUTE,
            'appeared': in_squad and (minutes > 0 or on != 0), 'minutes': minutes,
            'minutes_columns': (on, off, injured, sent_off), 'flags': flags, 'rating': rating,
            'values': values}


def match(seed):
    teams = []
    for side in (0, 1):
        squad = (5 if side else 9) + seed % 2
        teams.append({'squad': squad, 'uid': 677 if side else 2000123,
                      'goals': seed // 4 % 3 if side else seed % 4,
                      'players': [player(seed, side, slot, squad) for slot in range(squad)]})
    passes = [sum(p['values'][V['passes_completed']] for p in team['players']) for team in teams]
    for side in (0, 1):
        own, other = passes[side], passes[1 - side]
        share = 50 if own + other == 0 else math.floor(100.0 * own / (own + other) + 0.5)
        teams[side]['possession'] = share
    return teams


def rating_text(hundredths):
    sign = '-' if hundredths < 0 else ''
    hundredths = abs(hundredths)
    return '%s%d.%02d' % (sign, hundredths // 100, hundredths % 100)


def g9(value):
    return '%.9g' % (0.0 if value == 0 else value)


def rows(seed, index, seed_column):
    """One synthetic match's player and team rows, each with its newline."""
    teams = match(seed)
    players, team_rows = [], []
    for side, team in enumerate(teams):
        identity = [str(index), str(seed_column), 'away' if side else 'home', str(team['uid'])]
        for p in team['players']:
            fields = identity + [str(x) for x in (
                p['slot'], p['uid'], int(p['in_squad']), int(p['started']), int(p['appeared']),
                p['minutes'], *p['minutes_columns'], p['flags'])]
            if p['appeared']:
                fields.append(rating_text(p['rating']))
                for v, value in enumerate(p['values']):
                    fields.append(g9(value) if v in F32 else g9(value / 1000.0) if v == KM
                                  else str(value))
            else:
                fields += [''] * (1 + len(NAMES))
            players.append(','.join(fields) + '\n')
        played = [p for p in team['players'] if p['appeared']]
        sums = [0.0] * len(NAMES)
        rating_minutes = team_minutes = 0
        for p in played:
            rating_minutes += p['rating'] * p['minutes']
            team_minutes += p['minutes']
            for v, value in enumerate(p['values']):
                sums[v] += value / 1000.0 if v == KM else float(value)
        # Minutes-weighted and truncated towards zero, as C's integer division does.
        average = 0
        if team_minutes:
            average = abs(rating_minutes) // team_minutes * (1 if rating_minutes >= 0 else -1)
        fields = identity + [str(team['squad']), str(len(played))]
        for name in TEAM:
            if name == 'goals':
                fields.append(str(team['goals']))
            elif name == 'possession':
                fields.append(str(int(team['possession'])))
            elif name == 'average_rating':
                fields.append(rating_text(average))
            elif name == 'saves':
                saves = sums[V['saves_held']] + sums[V['saves_parried']] + sums[V['saves_tipped']]
                fields.append('%d' % saves)
            elif name in ('xg', 'distance_km'):
                fields.append(g9(sums[V[name]]))
            else:
                fields.append('%d' % sums[V[name]])
        team_rows.append(','.join(fields) + '\n')
    return ''.join(players), ''.join(team_rows)


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def main(arguments):
    if len(arguments) == 4 and arguments[0] == 'rows':
        players, teams = rows(int(arguments[1]), int(arguments[2]), arguments[3])
        print(players + teams, end='')
        print('players', digest(players))
        print('teams', digest(teams))
        return 0
    if len(arguments) >= 3 and arguments[0] == 'files':
        players, teams = PLAYERS_HEADER, TEAMS_HEADER
        for place, seed in enumerate(arguments[2:]):
            if seed != '-':
                match_players, match_teams = rows(int(seed), int(arguments[1]) + place, seed)
                players += match_players
                teams += match_teams
        print('match-players', digest(players))
        print('match-teams', digest(teams))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
