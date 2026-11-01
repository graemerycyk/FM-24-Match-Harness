# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The final tables and each player's season totals of a season batch (mode=season).

  python3 -I tools/batch/season.py <batch folder> [--out D] [--seasons A-B] [--include-suspect]
                                   [--matches]

A season is complete when every fixture has a results row for it in a committed chunk that is
not suspect (suspect.csv, or a chunk 0 that differs from the canary reference, unless
--include-suspect). Complete seasons, of A-B only if given, go into D (default: <name>-seasons in
the current folder):

  standings.csv.gz  season,position,team_uid,team,played,won,drawn,lost,goals_for,goals_against,
                    goal_difference,points: 3 points a win and 1 a draw, ranked by points, goal
                    difference, goals for, then the team's name and unique ID (not FM's own rules)
  players.csv.gz    season,team_uid,team,person_uid,player,squad,appearances,starts,
                    sub_appearances,minutes,average_rating,rating_total, the per-match rows' 46
                    statistics summed, and injuries: one row per player in a matchday squad, by
                    team in the plan's order then unique ID (per_match=1 only)
  teams.csv         each team's seasons, points (mean, sample SD, least, most), mean position,
                    goals for and against, and how often it finished in each position
  fixtures.csv      each fixture's teams and its inputs' SHA-256s (to join the chunks' rows)
  matches.csv.gz    with --matches: season,fixture,seed,home_team_uid,away_team_uid,home_goals,
                    away_goals
  incomplete.csv    season,fixture,reason (not_played, failed, excluded or suspect) for each match
                    an incomplete season lacks, in blocks that have begun
  seasons.json      the seasons included and left out, each chunk read with its files' SHA-256s,
                    and each output's rows and SHA-256s

Seasons are read a block at a time (chunk c of every fixture), each chunk once and checked against
its chunk.json; with per-match rows, every team row must name its fixture's team. The same chunks
always give the same bytes. Names come from lookups/, which it needs. Each file is written beside
its place as a .tmp file and renamed into it once every file is written."""
import argparse
import array
import datetime
import hashlib
import json
import math
import os
import sys

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fm24batch  # noqa: E402

SEASONS_FORMAT = 'fm24-seasons/1'
STANDINGS = ('season,position,team_uid,team,played,won,drawn,lost,goals_for,goals_against,'
             'goal_difference,points')
STATISTICS = fm24batch.HEADERS['match-players'].split(',')[16:]
DECIMAL = frozenset(('xg', 'xa', 'distance_km'))
PLAYERS = ('season,team_uid,team,person_uid,player,squad,appearances,starts,sub_appearances,'
           'minutes,average_rating,rating_total,' + ','.join(STATISTICS) + ',injuries')
MATCHES = 'season,fixture,seed,home_team_uid,away_team_uid,home_goals,away_goals'
INCOMPLETE = 'season,fixture,reason'
FIXTURES = 'fixture,home_team_uid,home_team,away_team_uid,away_team,json_sha256,bin_sha256'
# A player's totals: squad, appearances, starts, minutes, rating in hundredths, the statistics,
# injuries.
SQUAD, APPEARANCES, STARTS, MINUTES, RATING = range(5)
FIRST_STATISTIC = 5
INJURIES = FIRST_STATISTIC + len(STATISTICS)
WIDTH = INJURIES + 1
# A team's line: played, won, drawn, lost, goals for, goals against.
PLAYED, WON, DRAWN, LOST, FOR, AGAINST = range(6)


class Output:
    """A file being written as a .tmp file beside its place, compressed if it is a .gz, which
    counts its rows and hashes what it holds (raw) and what is stored."""

    def __init__(self, folder, name, header):
        self.name = name
        self.path = os.path.join(folder, name)
        self.file = open(self.path + '.tmp', 'wb')
        self.stored = hashlib.sha256()
        self.raw = hashlib.sha256()
        self.bytes = 0
        self.rows = -1  # the header is not a row
        self.text = fm24batch.gzip_writer(self) if name.endswith('.gz') else None
        self.line(header)

    def write(self, data):  # the stored bytes, from the gzip stream or directly
        self.stored.update(data)
        self.bytes += len(data)
        return self.file.write(data)

    def flush(self):
        self.file.flush()

    def line(self, text):
        data = (text + '\n').encode('utf-8')
        self.raw.update(data)
        self.rows += 1
        if self.text:
            self.text.write(text + '\n')
        else:
            self.write(data)

    def close(self):
        if self.text:
            self.text.close()  # closes the gzip stream, not the file
        self.file.flush()
        os.fsync(self.file.fileno())
        self.file.close()
        return {'name': self.name, 'rows': self.rows, 'bytes': self.bytes,
                'sha256': self.stored.hexdigest(), 'raw_sha256': self.raw.hexdigest()}

    def keep(self):
        os.replace(self.path + '.tmp', self.path)


class Season:
    """One season's totals as its block is read: the table, the players and the matches."""

    def __init__(self, teams):
        self.table = {uid: [0] * 6 for uid in teams}
        self.players = {}
        self.matches = []
        self.played = 0
        self.missing = {}  # fixture -> reason

    def add_result(self, fixture, seed, home, away, home_goals, away_goals):
        self.played += 1
        self.matches.append((fixture, seed, home, away, home_goals, away_goals))
        for uid, scored, conceded in ((home, home_goals, away_goals),
                                      (away, away_goals, home_goals)):
            line = self.table[uid]
            line[PLAYED] += 1
            line[FOR] += scored
            line[AGAINST] += conceded
            line[WON if scored > conceded else DRAWN if scored == conceded else LOST] += 1

    def add_player(self, row):
        key = (int(row[3]), int(row[5]))
        totals = self.players.get(key)
        if totals is None:
            totals = self.players[key] = array.array('d', bytes(8 * WIDTH))
        totals[SQUAD] += row[6] == '1'
        if row[12] != '0':
            totals[INJURIES] += 1
        if row[8] != '1':
            return
        totals[APPEARANCES] += 1
        totals[STARTS] += row[7] == '1'
        totals[MINUTES] += int(row[9])
        totals[RATING] += round(float(row[15]) * 100)
        for i, value in enumerate(row[16:]):
            totals[FIRST_STATISTIC + i] += float(value)

    def standings(self, names):
        """The table in its order: (uid, line, points)."""
        lines = [(uid, line, 3 * line[WON] + line[DRAWN]) for uid, line in self.table.items()]
        return sorted(lines, key=lambda entry: (-entry[2], -(entry[1][FOR] - entry[1][AGAINST]),
                                                -entry[1][FOR], names.get(entry[0], ''), entry[0]))


def team_name(names, uid):
    return quoted(names.get(uid, ''))


def quoted(text):
    return '"%s"' % text.replace('"', '""') if any(c in text for c in ',"\n') else text


def number(value, decimal):
    return '%.4f' % value if decimal or value != int(value) else '%d' % value


def seasons_of(text, first, last):
    """--seasons A-B, within the plan's seasons."""
    if text is None:
        return first, last
    low, _, high = text.partition('-')
    try:
        low, high = int(low), int(high or low)
    except ValueError:
        raise fm24batch.BatchError('--seasons is A-B, two season numbers')
    if not first <= low <= high <= last:
        raise fm24batch.BatchError('--seasons %s is not within the plan\'s %d-%d' % (text, first,
                                                                                     last))
    return low, high


def ranges(numbers):
    """Sorted numbers as [first, last] runs."""
    runs = []
    for value in numbers:
        if runs and runs[-1][1] == value - 1:
            runs[-1][1] = value
        else:
            runs.append([value, value])
    return runs


class Report:
    """A season batch being read block by block into its outputs."""

    def __init__(self, batch, out, chosen, include_suspect, matches):
        self.batch = batch
        self.chosen = chosen
        self.include_suspect = include_suspect
        self.per_match = batch.settings.get('per_match') == '1'
        self.names, self.people = batch.lookups()
        teams_path = os.path.join(batch.folder, 'lookups', 'teams.txt')
        if not os.path.exists(teams_path):
            raise fm24batch.BatchError('%s has no lookups/teams.txt, which its first session writes'
                                       % batch.folder)
        self.fixtures, self.fixtures_sha256 = batch.fixtures()
        os.makedirs(out, exist_ok=True)
        self.outputs = {'standings': Output(out, 'standings.csv.gz', STANDINGS)}
        if self.per_match:
            self.outputs['players'] = Output(out, 'players.csv.gz', PLAYERS)
        if matches:
            self.outputs['matches'] = Output(out, 'matches.csv.gz', MATCHES)
        self.outputs['incomplete'] = Output(out, 'incomplete.csv', INCOMPLETE)
        self.out = out
        self.chunks = []
        self.complete = []
        self.left_out = []
        self.history = {uid: [] for uid in batch.teams}  # (points, position, for, against)

    def run(self):
        batch = self.batch
        first, last = self.chosen
        for block in range(batch.planned_chunks()):
            block_first = batch.seeds.first + block * batch.chunk
            block_last = min(block_first + batch.chunk - 1, batch.seeds.last)
            if block_last < first or block_first > last:
                continue
            self.read_block(max(block_first, first), min(block_last, last), block_first)
        return self.finish()

    def read_block(self, first, last, block_first):
        batch = self.batch
        seasons = {season: Season(batch.teams) for season in range(first, last + 1)}
        begun = False
        for number, arm in enumerate(batch.arms):
            chunk = batch.committed(arm, block_first)
            suspect = chunk is not None and batch.is_suspect(chunk)
            if chunk is None or (suspect and not self.include_suspect):
                for season in seasons.values():
                    season.missing[number] = 'suspect' if suspect else 'not_played'
                begun = begun or chunk is not None
                continue
            begun = True
            self.read_chunk(number, arm, chunk, seasons)
        for season_number, season in seasons.items():
            if season.played == len(batch.arms) and not season.missing:
                self.write_season(season_number, season)
            else:
                self.left_out.append(season_number)
                for fixture in range(len(batch.arms)) if begun else ():
                    reason = season.missing.get(fixture)
                    if reason:
                        self.outputs['incomplete'].line('%d,%s,%s' % (
                            season_number, batch.arms[fixture], reason))

    def read_chunk(self, number, arm, chunk, seasons):
        home, away = self.batch.pairs[arm]
        self.chunks.append({
            'arm': arm, 'first_index': chunk.first, 'count': chunk.count,
            'path': os.path.relpath(chunk.path, self.batch.folder).replace(os.sep, '/'),
            'files': [{'name': entry['name'], 'sha256': entry['sha256'],
                       'raw_sha256': entry['raw_sha256']} for entry in chunk.manifest['files']]})
        for row in chunk.rows('results'):
            season = seasons.get(int(row[0]))
            if season:
                season.add_result(number, int(row[1]), home, away, int(row[2]), int(row[3]))
        for row in chunk.rows('failed'):
            season = seasons.get(int(row[0]))
            if season:
                season.missing[number] = 'failed'
        for index in chunk.excluded:
            if index in seasons:
                seasons[index].missing[number] = 'excluded'
        if not self.per_match:
            return
        teams = {'home': home, 'away': away}
        for role in ('match-teams', 'match-players'):
            for row in chunk.rows(role):
                if int(row[3]) != teams[row[2]]:
                    raise fm24batch.BatchError(
                        '%s: a %s row of season %s names team %s, not its fixture\'s %d' % (
                            os.path.join(chunk.path, chunk.file_name(role)), row[2], row[0],
                            row[3], teams[row[2]]))
                season = seasons.get(int(row[0]))
                if role == 'match-players' and season and row[6] == '1':
                    season.add_player(row)

    def write_season(self, number, season):
        self.complete.append(number)
        for position, (uid, line, points) in enumerate(season.standings(self.names), 1):
            self.outputs['standings'].line(','.join([
                str(number), str(position), str(uid), team_name(self.names, uid)] + [
                    str(value) for value in line] + [
                    str(line[FOR] - line[AGAINST]), str(points)]))
            self.history[uid].append((points, position, line[FOR], line[AGAINST]))
        if 'players' in self.outputs:
            order = {uid: i for i, uid in enumerate(self.batch.teams)}
            for (team, person), totals in sorted(season.players.items(),
                                                 key=lambda item: (order[item[0][0]],
                                                                   item[0][1])):
                appearances = int(totals[APPEARANCES])
                rating = int(totals[RATING])
                fields = [str(number), str(team), team_name(self.names, team), str(person),
                          quoted(self.people.get(person, '')), '%d' % totals[SQUAD],
                          str(appearances), '%d' % totals[STARTS],
                          '%d' % (appearances - totals[STARTS]), '%d' % totals[MINUTES],
                          '%.3f' % (rating / 100 / appearances) if appearances else '',
                          '%d.%02d' % divmod(rating, 100) if rating >= 0 else
                          '-%d.%02d' % divmod(-rating, 100)]
                fields += [number_text(totals[FIRST_STATISTIC + i], name)
                           for i, name in enumerate(STATISTICS)]
                fields.append('%d' % totals[INJURIES])
                self.outputs['players'].line(','.join(fields))
        if 'matches' in self.outputs:
            for fixture, seed, home, away, home_goals, away_goals in sorted(season.matches):
                self.outputs['matches'].line('%d,%s,%d,%d,%d,%d,%d' % (
                    number, self.batch.arms[fixture], seed, home, away, home_goals, away_goals))

    def finish(self):
        batch = self.batch
        outputs = [output.close() for output in self.outputs.values()]
        for name, header, rows in (('teams.csv', self.teams_header(), self.team_rows()),
                                   ('fixtures.csv', FIXTURES, self.fixture_rows())):
            output = Output(self.out, name, header)
            for row in rows:
                output.line(row)
            outputs.append(output.close())
            self.outputs[name] = output
        for output in self.outputs.values():
            output.keep()
        manifest = {
            'format': SEASONS_FORMAT,
            'batch': batch.name,
            'plan_sha256': batch.settings['plan_sha256'],
            'fixtures_sha256': self.fixtures_sha256,
            'created_at': datetime.datetime.now(datetime.timezone.utc).strftime(
                '%Y-%m-%dT%H:%M:%SZ'),
            'include_suspect': self.include_suspect,
            'seasons': {'first': self.chosen[0], 'last': self.chosen[1],
                        'complete': len(self.complete), 'included': ranges(self.complete),
                        'left_out': ranges(sorted(self.left_out))},
            'chunks': self.chunks,
            'outputs': outputs,
        }
        text = (json.dumps(manifest, indent=1) + '\n').encode('utf-8')
        fm24batch.write_atomically(os.path.join(self.out, 'seasons.json'), lambda f: f.write(text))
        return manifest

    def teams_header(self):
        positions = ','.join('position_%d' % (i + 1) for i in range(len(self.batch.teams)))
        return ('team_uid,team,seasons,points_mean,points_sd,points_min,points_max,position_mean,'
                'goals_for_mean,goals_against_mean,' + positions)

    def team_rows(self):
        for uid in self.batch.teams:
            history = self.history[uid]
            count = len(history)
            points = [entry[0] for entry in history]
            fields = [str(uid), team_name(self.names, uid), str(count)]
            if count:
                mean = sum(points) / count
                spread = math.sqrt(sum((p - mean) ** 2 for p in points) / (count - 1)) \
                    if count > 1 else None
                fields += ['%.3f' % mean, '' if spread is None else '%.3f' % spread,
                           str(min(points)), str(max(points)),
                           '%.3f' % (sum(entry[1] for entry in history) / count),
                           '%.3f' % (sum(entry[2] for entry in history) / count),
                           '%.3f' % (sum(entry[3] for entry in history) / count)]
            else:
                fields += [''] * 7
            fields += [str(sum(1 for entry in history if entry[1] == position))
                       for position in range(1, len(self.batch.teams) + 1)]
            yield ','.join(fields)

    def fixture_rows(self):
        for arm in self.batch.arms:
            home, away = self.batch.pairs[arm]
            json_sha256, bin_sha256 = self.fixtures[arm][:2]
            yield ','.join([arm, str(home), team_name(self.names, home), str(away),
                            team_name(self.names, away), json_sha256, bin_sha256])


def number_text(value, name):
    return number(value, name in DECIMAL)


def main(arguments):
    parser = argparse.ArgumentParser(
        description="A season batch's final tables and players' season totals.")
    parser.add_argument('batch', help='the batch folder')
    parser.add_argument('--out', help='the folder to write to (default: <name>-seasons)')
    parser.add_argument('--seasons', help='only seasons A-B')
    parser.add_argument('--include-suspect', action='store_true',
                        help='also read the suspect chunks')
    parser.add_argument('--matches', action='store_true',
                        help='also write each match of the complete seasons')
    options = parser.parse_args(arguments)
    try:
        batch = fm24batch.Batch(options.batch)
        if not batch.season:
            raise fm24batch.BatchError('%s is not a season batch (mode=season): use summary.py or '
                                       'merge.py' % options.batch)
        chosen = seasons_of(options.seasons, batch.seeds.first, batch.seeds.last)
        out = options.out or batch.name + '-seasons'
        manifest = Report(batch, out, chosen, options.include_suspect, options.matches).run()
        seasons = manifest['seasons']
        print('%s: %d of %d seasons complete; %s' % (
            batch.name, seasons['complete'], chosen[1] - chosen[0] + 1, ', '.join(
                '%s %d rows' % (output['name'], output['rows'])
                for output in manifest['outputs'])))
    except (fm24batch.BatchError, OSError, ValueError, KeyError) as error:
        sys.stderr.write('season: %s\n' % error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
