# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarises a batch's committed chunks.

  python3 -I tools/batch/summary.py <batch folder> [--arm A] [--include-suspect] [--chunks]
                                    [--results-only] [--out D]

For each arm (or only A) it prints home wins, draws and away wins with 95% confidence intervals
(Wilson's), overall and for each session; the lag-1 autocorrelation of draws in index order, which
is near 0 when neighbouring indices play independent matches (scattered seeds) and shows a
dependence between consecutive seeds otherwise; and each side's team means from the per-match
rows, which --results-only leaves out (reading them takes a few minutes per million matches).
--chunks also prints each chunk. With --out it writes those tables to D as CSV (arms.csv,
sessions.csv, chunks.csv) and, from the per-match rows, <arm>-players.csv and <arm>-teams.csv:
the averages `run --detailed` writes, to within 0.0005 (it rounds each value to 2^-20 before
adding them). Suspect chunks, which suspect.csv lists, and a chunk 0 whose chunk.json records
that it differs from the canary reference, are left out unless --include-suspect is given.
Every chunk file is checked against its chunk.json as it is read."""
import argparse
import csv
import math
import os
import sys

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fm24batch  # noqa: E402

Z95 = 1.959963984540054
SIDES = ('home', 'away')
PLAYER_VALUES = fm24batch.HEADERS['match-players'].split(',')[16:]
TEAM_VALUES = fm24batch.HEADERS['match-teams'].split(',')[6:]
TEAM_SUMMARY = ('goals', 'possession', 'xg', 'shots', 'average_rating')


def wilson(count, total):
    """The 95% Wilson score interval of count/total, as (low, high)."""
    if not total:
        return (0.0, 0.0)
    p = count / total
    scale = 1 + Z95 * Z95 / total
    centre = (p + Z95 * Z95 / (2 * total)) / scale
    half = Z95 * math.sqrt(p * (1 - p) / total + Z95 * Z95 / (4 * total * total)) / scale
    return (max(0.0, centre - half), min(1.0, centre + half))


class Outcomes:
    """Matches, home wins, draws, away wins and goals."""

    def __init__(self):
        self.matches = self.home = self.draw = self.away = 0
        self.home_goals = self.away_goals = 0

    def add(self, home_goals, away_goals):
        self.matches += 1
        self.home_goals += home_goals
        self.away_goals += away_goals
        if home_goals > away_goals:
            self.home += 1
        elif home_goals == away_goals:
            self.draw += 1
        else:
            self.away += 1

    def columns(self):
        """The CSV columns: matches, then each outcome's count, share and interval, then goals."""
        values = [self.matches]
        for count in (self.home, self.draw, self.away):
            low, high = wilson(count, self.matches)
            values += [count, share(count, self.matches), '%.6f' % low, '%.6f' % high]
        return values + [self.home_goals, self.away_goals]

    def text(self):
        parts = []
        for name, count in (('home', self.home), ('draw', self.draw), ('away', self.away)):
            low, high = wilson(count, self.matches)
            parts.append('%s %.1f%% (%.1f-%.1f)' % (
                name, 100 * count / self.matches if self.matches else 0, 100 * low, 100 * high))
        goals = '%.3f-%.3f' % (self.home_goals / self.matches, self.away_goals / self.matches) \
            if self.matches else '-'
        return '%s matches; %s; goals %s' % (count_text(self.matches), ', '.join(parts), goals)


OUTCOME_COLUMNS = ['matches'] + [
    '%s%s' % (name, suffix) for name in ('home', 'draw', 'away')
    for suffix in ('', '_share', '_low', '_high')] + ['home_goals', 'away_goals']


class Lag1:
    """The lag-1 autocorrelation of a 0/1 series, from running sums."""

    def __init__(self):
        self.count = 0
        self.total = 0
        self.products = 0
        self.first = self.previous = None

    def add(self, value):
        if self.previous is not None:
            self.products += self.previous * value
        else:
            self.first = value
        self.previous = value
        self.count += 1
        self.total += value

    def value(self):
        """r1 = sum((x[i] - m)(x[i+1] - m)) / sum((x[i] - m)^2), or None with fewer than 3."""
        n = self.count
        if n < 3:
            return None
        mean = self.total / n
        variance = self.total - n * mean * mean  # sum of (x - m)^2 for 0/1 values
        if variance <= 0:
            return None
        # sum((x[i] - m)(x[i+1] - m)) over the n - 1 pairs
        inner = self.total - self.first - self.previous + self.total
        covariance = self.products - mean * inner + (n - 1) * mean * mean
        return covariance / variance


class Means:
    """The --detailed averages from per-match rows: players by side and unique ID, and teams."""

    def __init__(self):
        self.matches = {side: 0 for side in SIDES}
        self.teams = {side: None for side in SIDES}
        self.team_sums = {side: [0.0] * len(TEAM_VALUES) for side in SIDES}
        self.players = {}

    def add_team(self, row):
        side = row[2]
        self.matches[side] += 1
        self.teams[side] = int(row[3])
        sums = self.team_sums[side]
        for i, value in enumerate(row[6:]):
            sums[i] += float(value)

    def add_player(self, row):
        key = (row[2], int(row[5]))
        totals = self.players.get(key)
        if totals is None:
            totals = self.players[key] = [0, 0, 0, 0, [0.0] * len(PLAYER_VALUES)]
        if row[8] != '1':
            return
        totals[0] += 1  # appearances
        totals[1] += int(row[7])  # starts
        totals[2] += int(row[9])  # minutes
        totals[3] += round(float(row[15]) * 100)  # rating, in hundredths
        values = totals[4]
        for i, value in enumerate(row[16:]):
            values[i] += float(value)

    def player_rows(self, teams, people):
        """The rows of a -players.csv, in its order: home then away, by unique ID."""
        rows = []
        for side in SIDES:
            matches = self.matches[side] or 1
            team = quoted(name_of(teams, self.teams[side]))
            for (player_side, uid), totals in sorted(self.players.items(),
                                                       key=lambda item: item[0][1]):
                if player_side != side:
                    continue
                appearances, starts, minutes, rating, values = totals
                fields = [side, team, quoted(name_of(people, uid)), str(uid),
                          str(self.matches[side]), str(appearances), str(starts),
                          '%.1f' % (minutes / matches),
                          '%.2f' % (rating / 100 / appearances) if appearances else '']
                rows.append(','.join(fields + ['%.3f' % (value / matches) for value in values]))
        return rows

    def team_rows(self, teams):
        rows = []
        for side in SIDES:
            matches = self.matches[side] or 1
            fields = [side, quoted(name_of(teams, self.teams[side])), str(self.matches[side])]
            rows.append(','.join(fields + ['%.3f' % (value / matches)
                                           for value in self.team_sums[side]]))
        return rows


def summarise(batch, arm, include_suspect, means=True):
    """An arm's outcomes overall, by session and by chunk, its lag-1 draw autocorrelation and,
    with `means`, its per-match means."""
    chunks = [chunk for chunk in batch.chunks(arm)
              if include_suspect or not batch.is_suspect(chunk)]
    overall = Outcomes()
    sessions = {}
    per_chunk = []
    draws = Lag1()
    averages = Means() if means else None
    for chunk in chunks:
        outcomes = Outcomes()
        for row in chunk.rows('results'):
            home, away = int(row[2]), int(row[3])
            session = chunk.session_of(int(row[0])) or '-'
            for tally in (overall, outcomes, sessions.setdefault(session, Outcomes())):
                tally.add(home, away)
            draws.add(1 if home == away else 0)
        per_chunk.append((chunk, outcomes))
        if averages:
            for row in chunk.rows('match-teams'):
                averages.add_team(row)
            for row in chunk.rows('match-players'):
                averages.add_player(row)
    return {'chunks': per_chunk, 'left_out': len(batch.chunks(arm)) - len(chunks),
            'overall': overall, 'sessions': sessions, 'draws': draws, 'means': averages}


def report(batch, arms, summaries, show_chunks, out):
    teams, people = batch.lookups()
    planned = batch.planned_chunks()
    out.write('batch %s: %d arm%s, %s seeds\n' % (
        batch.name, len(batch.arms), '' if len(batch.arms) == 1 else 's', batch.settings['seeds']))
    for arm in arms:
        summary = summaries[arm]
        out.write('arm %s: %d of %d chunks%s; %s\n' % (
            arm, len(summary['chunks']), planned,
            ' (%d suspect left out)' % summary['left_out'] if summary['left_out'] else '',
            summary['overall'].text()))
        for session in sorted(summary['sessions']):
            out.write('  session %s: %s\n' % (session, summary['sessions'][session].text()))
        draws = summary['draws']
        r1 = draws.value()
        if r1 is None:
            out.write('  lag-1 draw autocorrelation: too few matches\n')
        else:
            out.write('  lag-1 draw autocorrelation in index order: %.4f over %s matches '
                      '(95%% of independent series within %.4f of %.4f)\n' % (
                          r1, count_text(draws.count), Z95 / math.sqrt(draws.count),
                          -1 / draws.count))
        means = summary['means']
        if means:
            columns = [TEAM_VALUES.index(name) for name in TEAM_SUMMARY]
            for side in SIDES:
                matches = means.matches[side]
                if not matches:
                    continue
                values = ', '.join('%s %.3f' % (TEAM_VALUES[c], means.team_sums[side][c] / matches)
                                   for c in columns)
                out.write('  %s %s: %s\n' % (side, name_of(teams, means.teams[side]), values))
        if show_chunks:
            for chunk, outcomes in summary['chunks']:
                out.write('  chunk %s%s: %s\n' % (
                    chunk.id, ' (suspect)' if batch.is_suspect(chunk) else '', outcomes.text()))


def write_tables(batch, arms, summaries, folder):
    teams, people = batch.lookups()
    os.makedirs(folder, exist_ok=True)
    tables = {
        'arms.csv': (['arm'], lambda arm, summary: [[arm] + summary['overall'].columns()]),
        'sessions.csv': (['arm', 'session'], lambda arm, summary: [
            [arm, session] + summary['sessions'][session].columns()
            for session in sorted(summary['sessions'])]),
        'chunks.csv': (['arm', 'first_index', 'suspect'], lambda arm, summary: [
            [arm, chunk.first, int(batch.is_suspect(chunk))] + outcomes.columns()
            for chunk, outcomes in summary['chunks']]),
    }
    for name, (keys, rows) in tables.items():
        def write(file, keys=keys, rows=rows):
            text = ''.join(','.join(str(field) for field in row) + '\n' for row in
                           [keys + OUTCOME_COLUMNS] +
                           [row for arm in arms for row in rows(arm, summaries[arm])])
            file.write(text.encode('utf-8'))
        fm24batch.write_atomically(os.path.join(folder, name), write)
    for arm in arms:
        means = summaries[arm]['means']
        if not means or not any(means.matches.values()):
            continue
        players = ['side,team,player,person_uid,matches,appearances,starts,minutes,rating,' +
                   ','.join(PLAYER_VALUES)] + means.player_rows(teams, people)
        team_rows = ['side,team,matches,' + ','.join(TEAM_VALUES)] + means.team_rows(teams)
        for suffix, lines in (('players', players), ('teams', team_rows)):
            text = ''.join(line + '\n' for line in lines).encode('utf-8')
            fm24batch.write_atomically(os.path.join(folder, '%s-%s.csv' % (arm, suffix)),
                                       lambda file, text=text: file.write(text))


def share(count, total):
    return '%.6f' % (count / total) if total else ''


def count_text(value):
    return '{:,}'.format(value)


def name_of(names, uid):
    if uid is None:
        return '?'
    return names.get(uid, str(uid))


def quoted(text):
    return '"%s"' % text.replace('"', '""')


def main(arguments):
    parser = argparse.ArgumentParser(description="Summarises a batch's committed chunks.")
    parser.add_argument('batch', help='the batch folder')
    parser.add_argument('--arm', help='only this arm')
    parser.add_argument('--include-suspect', action='store_true',
                        help='also count the suspect chunks')
    parser.add_argument('--chunks', action='store_true', help='also print each chunk')
    parser.add_argument('--results-only', action='store_true',
                        help='read only the results, not the per-match rows')
    parser.add_argument('--out', help='write the tables and per-match means to this folder')
    options = parser.parse_args(arguments)
    try:
        batch = fm24batch.Batch(options.batch)
        arms = [options.arm] if options.arm else batch.arms
        summaries = {arm: summarise(batch, arm, options.include_suspect, not options.results_only)
                     for arm in arms}
        report(batch, arms, summaries, options.chunks, sys.stdout)
        if options.out:
            write_tables(batch, arms, summaries, options.out)
    except (fm24batch.BatchError, OSError, ValueError, KeyError) as error:
        sys.stderr.write('summary: %s\n' % error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
