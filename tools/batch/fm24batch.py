# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads a batch folder that `fm24ctl batch` keeps (see docs/BATCH.md and docs/CLI.md "Batches"):
its frozen plan, its committed chunks and their files, sessions and suspect chunks, and the
lookups, and a season batch's teams and fixtures. Only committed chunks are read; a running batch
can be read at the same time, as each chunk is complete once its chunk.json exists. merge.py,
summary.py and season.py use it."""
import gzip
import hashlib
import io
import json
import math
import os
import re

import fm24seeds

ROLES = ('results', 'match-players', 'match-teams', 'failed')
HEADERS = {
    'results': 'index,seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms',
    'match-players': (
        'index,seed,side,team_uid,slot,person_uid,in_squad,started,appeared,minutes,on_minute,'
        'off_minute,injured_minute,sent_off_minute,stats_flags,rating,goals,assists,shots,'
        'shots_on_target,blocked_shots,long_shots,xg,xa,key_passes,clear_cut_chances,half_chances,'
        'passes_attempted,passes_completed,progressive_passes,crosses_attempted,crosses_completed,'
        'dribbles,tackles_attempted,tackles_won,key_tackles,interceptions,clearances,blocks,'
        'shots_blocked,pressures_attempted,pressures_completed,headers_attempted,headers_won,'
        'key_headers,fouls,fouled,offsides,yellow_cards,red_cards,corners,throw_ins,'
        'defensive_free_kicks,attacking_free_kicks,saves_held,saves_parried,saves_tipped,'
        'goals_conceded,possession_won,possession_lost,high_intensity_sprints,distance_km'),
    'match-teams': (
        'index,seed,side,team_uid,squad,used,goals,possession,average_rating,xg,shots,'
        'shots_on_target,blocked_shots,long_shots,clear_cut_chances,half_chances,corners,fouls,'
        'fouled,offsides,passes_attempted,passes_completed,progressive_passes,crosses_attempted,'
        'crosses_completed,tackles_attempted,tackles_won,headers_attempted,headers_won,'
        'interceptions,clearances,blocks,shots_blocked,pressures_attempted,pressures_completed,'
        'yellow_cards,red_cards,saves,distance_km,key_passes,dribbles,possession_won,'
        'possession_lost,high_intensity_sprints'),
    'failed': 'index,seed,stage',
}
PLAN_FORMAT = 'fm24-batch-plan/1'
CHUNK_FORMAT = 'fm24-chunk-manifest/1'
FIXTURES_FORMAT = 'fm24-season-fixtures/1'
CHUNK_NAME = re.compile(r'chunk=(\d{10})$')
# A frozen season's pair line, and a line of its inputs/fixtures.txt.
PAIR_LINE = re.compile(r'(f\d{4}) home_team=(\d+) away_team=(\d+)')
FIXTURE_LINE = re.compile(
    r'fixture=(f\d{4}) json_sha256=([0-9a-f]{64}) bin_sha256=([0-9a-f]{64}) home_team=(\d+|-) '
    r'away_team=(\d+|-)')
# A line of suspect.csv, arm,chunk,session,reason. The header, and a line a power cut left as zeros
# or cut short of its reason, are not one.
SUSPECT_LINE = re.compile(r'([^,\x00-\x1f]+),(\d{10}),[^,\x00-\x1f]*,[^,\x00-\x1f]+')


class BatchError(Exception):
    """A batch folder that cannot be read, or a file that does not match its chunk.json."""


class Chunk:
    """A committed chunk: chunks/arm=<arm>/chunk=<first>/ and its chunk.json."""

    def __init__(self, arm, path, manifest):
        self.arm = arm
        self.path = path
        self.manifest = manifest
        seeds = manifest['seeds']
        self.first = seeds['first_index']
        self.last = seeds['last_index']
        self.count = seeds['count']
        self.id = '%010d' % self.first
        self.files = {entry['name']: entry for entry in manifest['files']}
        self.excluded = [entry['index'] for entry in manifest.get('excluded', [])]
        self.parts = manifest.get('parts', [])

    def file_name(self, role):
        """The stored name of a role's file (results.csv.gz, or results.csv uncompressed), or None
        if the chunk has none."""
        for name in (role + '.csv.gz', role + '.csv'):
            if name in self.files:
                return name
        return None

    def lines(self, role):
        """The lines of a role's file after its header, each with its line ending, checked against
        chunk.json as they are read: the header, the uncompressed SHA-256 and the row count.
        Nothing if the chunk has no such file."""
        name = self.file_name(role)
        if name is None:
            return
        entry = self.files[name]
        digest = hashlib.sha256()
        rows = 0
        path = os.path.join(self.path, name)
        opener = gzip.open if name.endswith('.gz') else open
        with opener(path, 'rb') as raw:
            header = raw.readline()
            digest.update(header)
            if header.decode('utf-8') != HEADERS[role] + '\n':
                raise BatchError('%s: not the header of %s rows' % (path, role))
            for line in raw:
                digest.update(line)
                if not line.endswith(b'\n'):
                    raise BatchError('%s: its last line is cut short' % path)
                rows += 1
                yield line.decode('utf-8')
        if digest.hexdigest() != entry['raw_sha256'] or rows != entry['rows']:
            raise BatchError('%s does not match its SHA-256 or rows in chunk.json' % path)

    def rows(self, role):
        """lines(role), each split into its fields."""
        for line in self.lines(role):
            yield line[:-1].split(',')

    def session_of(self, index):
        """The session whose part played `index`, or None."""
        for part in self.parts:
            if part['first_index'] <= index < part['first_index'] + part['verified']:
                return part['session']
        return None


class Batch:
    """A batch folder: plan.txt's settings, arms and seeds, and the committed chunks. A season's
    arms are its fixtures, f0001 on, with their teams' unique IDs in `pairs` and its teams in
    `teams`, in the plan's order."""

    def __init__(self, folder):
        self.folder = folder
        self.settings = {}
        self.arms = []
        self.teams = []
        self.pairs = {}
        path = os.path.join(folder, 'plan.txt')
        try:
            with open(path, encoding='utf-8') as file:
                lines = file.read().split('\n')
        except OSError as error:
            raise BatchError('%s is not a batch: %s' % (folder, error))
        for line in lines:
            if not line or line.startswith('#') or '=' not in line:
                continue
            key, value = line.split('=', 1)
            if key == 'arm':
                self.arms.append(value.split(' ', 1)[0])
            elif key == 'team':
                self.teams.append(int(value))
            elif key == 'pair':
                match = PAIR_LINE.fullmatch(value)
                if not match:
                    raise BatchError('%s: not a pair line: %s' % (path, value))
                self.arms.append(match.group(1))
                self.pairs[match.group(1)] = (int(match.group(2)), int(match.group(3)))
            elif key != 'fixture':
                self.settings[key] = value
        self.season = self.settings.get('mode') == 'season'
        if self.settings.get('format') != PLAN_FORMAT or 'plan_sha256' not in self.settings:
            raise BatchError('%s is not a batch that fm24ctl batch create froze' % folder)
        self.name = self.settings['name']
        self.chunk = int(self.settings['chunk'])
        self.seeds = fm24seeds.SeedSet(self.settings['seeds'], os.path.join(folder, 'inputs'),
                                       self.settings.get('seeds_sha256'))
        self.suspect = set()
        suspect = os.path.join(folder, 'suspect.csv')
        if os.path.exists(suspect):
            with open(suspect, 'rb') as file:
                lines = file.read().decode('utf-8', 'replace').split('\n')
            for line in lines:
                match = SUSPECT_LINE.fullmatch(line)
                if match:
                    self.suspect.add((match.group(1), int(match.group(2))))

    def planned_chunks(self):
        """How many chunks each arm has."""
        return math.ceil(self.seeds.count / self.chunk)

    def arm_seeds(self, arm):
        """The seeds an arm plays: the plan's, or a season fixture's own."""
        if not self.season:
            return self.seeds
        return self.seeds.fixture(len(self.arms), self.arms.index(arm))

    def committed(self, arm, first):
        """An arm's committed chunk from index `first`, or None if it is not committed."""
        path = os.path.join(self.folder, 'chunks', 'arm=' + arm, 'chunk=%010d' % first)
        manifest_path = os.path.join(path, 'chunk.json')
        if not os.path.exists(manifest_path):
            return None
        with open(manifest_path, encoding='utf-8') as file:
            manifest = json.load(file)
        if manifest.get('format') != CHUNK_FORMAT or manifest.get('arm') != arm:
            raise BatchError('%s is not a chunk of arm %s' % (manifest_path, arm))
        return Chunk(arm, path, manifest)

    def fixtures(self):
        """A season's inputs/fixtures.txt: {fixture: (json_sha256, bin_sha256, home override,
        away override)} and its fixtures_sha256, checked against the plan and its last line."""
        path = os.path.join(self.folder, 'inputs', 'fixtures.txt')
        try:
            with open(path, 'rb') as file:
                data = file.read()
        except OSError:
            raise BatchError('%s has no inputs/fixtures.txt: its fixtures are not built yet'
                             % self.folder)
        body, _, last = data.rstrip(b'\n').rpartition(b'\n')
        lines = body.decode('utf-8').split('\n')
        digest = hashlib.sha256(body + b'\n').hexdigest()
        if (last.decode('utf-8') != 'fixtures_sha256=' + digest or not lines or
                lines[0] != 'format=' + FIXTURES_FORMAT or
                lines[1:2] != ['plan_sha256=' + self.settings['plan_sha256']]):
            raise BatchError('%s is not this plan\'s, or has been changed' % path)
        fixtures = {}
        for line in lines[2:]:
            match = FIXTURE_LINE.fullmatch(line)
            if not match:
                raise BatchError('%s: not a fixture line: %s' % (path, line))
            fixtures[match.group(1)] = match.group(2, 3, 4, 5)
        if list(fixtures) != self.arms:
            raise BatchError('%s does not list the plan\'s fixtures' % path)
        return fixtures, digest

    def chunks(self, arm):
        """An arm's committed chunks, in index order."""
        if arm not in self.arms:
            raise BatchError('the batch has no arm %s' % arm)
        folder = os.path.join(self.folder, 'chunks', 'arm=' + arm)
        found = []
        names = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
        for name in names:
            path = os.path.join(folder, name)
            manifest_path = os.path.join(path, 'chunk.json')
            if not CHUNK_NAME.match(name) or not os.path.exists(manifest_path):
                continue  # a folder a stopped commit left, which batch run removes
            with open(manifest_path, encoding='utf-8') as file:
                manifest = json.load(file)
            if manifest.get('format') != CHUNK_FORMAT or manifest.get('arm') != arm:
                raise BatchError('%s is not a chunk of arm %s' % (manifest_path, arm))
            found.append(Chunk(arm, path, manifest))
        return sorted(found, key=lambda chunk: chunk.first)

    def is_suspect(self, chunk):
        """Whether suspect.csv lists the chunk, or it is a chunk 0 whose chunk.json records that it
        differs from the canary reference (which suspect.csv lists only once batch run has acted on
        it)."""
        checks = chunk.manifest.get('checks') or {}
        return ((chunk.arm, chunk.first) in self.suspect or
                checks.get('canary_reference_match') is False)

    def lookups(self):
        """Names from lookups/: {team_uid: name} and {person_uid: name}, empty if not there."""
        teams = {}
        players = {}
        for name, table in (('teams.txt', teams), ('players.txt', players)):
            path = os.path.join(self.folder, 'lookups', name)
            if not os.path.exists(path):
                continue
            with open(path, encoding='utf-8') as file:
                for line in file:
                    fields = line.rstrip('\n').split('\t')
                    if len(fields) < 2 or not fields[0].isdigit():
                        continue
                    if name == 'teams.txt':
                        table[int(fields[0])] = fields[1]
                    else:
                        common = fields[3] if len(fields) > 3 else ''
                        table[int(fields[0])] = common or ' '.join(f for f in fields[1:3] if f)
        return teams, players


def write_atomically(path, write):
    """Calls write(file) on a temporary file beside `path`, then renames it over `path`."""
    temporary = path + '.tmp'
    with open(temporary, 'wb') as file:
        write(file)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)


def gzip_writer(file):
    """A text stream compressing into `file` as `gzip -n -6` does: the same bytes compress the same,
    with no name or time in the header."""
    compressed = gzip.GzipFile(filename='', mode='wb', compresslevel=6, fileobj=file, mtime=0)
    return io.TextIOWrapper(compressed, encoding='utf-8', newline='\n')
