# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the batch tools, on batch folders written here as `fm24ctl batch` writes them.

  python3 -I -m unittest discover -s tools/batch"""
import gzip
import hashlib
import io
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

import fm24batch
import fm24seeds
import merge
import season
import summary

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PLAYER_VALUES = len(summary.PLAYER_VALUES)
TEAM_VALUES = len(summary.TEAM_VALUES)


def player_row(index, seed, side, slot, uid, appeared, minutes, rating, value):
    """A per-match player row: every statistic `value` when the player appeared, else empty."""
    stats = [str(value)] * PLAYER_VALUES if appeared else [''] * PLAYER_VALUES
    team = 677 if side == 'away' else 1
    started = int(appeared and slot == 0)
    fields = [index, seed, side, team, slot, uid, 1, started, int(appeared), minutes, 0, 0, 0, 0, 0,
              rating if appeared else '']
    return ','.join(str(field) for field in fields + stats)


def team_row(index, seed, side, goals, value):
    fields = [index, seed, side, 677 if side == 'away' else 1, 2, 1, goals]
    return ','.join(str(field) for field in fields + [value] * (TEAM_VALUES - 1))


class BatchFolder:
    """A batch folder of `arms`, with seeds=scatter:<key>:<first>-<last> in chunks of `chunk`,
    each index played by `play(arm, index, seed)` -> (home goals, away goals) or None for a seed
    that failed to rebuild."""

    def __init__(self, root, arms=('baseline', 'press'), first=100, last=129, chunk=10, key=9,
                 compress=True, play=None, uids=None):
        self.folder = os.path.join(root, 'batch')
        self.arms = arms
        self.seeds = fm24seeds.SeedSet('scatter:%d:%d-%d' % (key, first, last))
        self.chunk = chunk
        self.compress = compress
        self.play = play or (lambda arm, index, seed: (seed % 4, seed // 4 % 3))
        # Each side's players in slots 0 and 1, by unique ID.
        self.uids = uids or {'home': (500, 501), 'away': (600, 601)}
        os.makedirs(os.path.join(self.folder, 'chunks'))
        lines = ['format=fm24-batch-plan/1', '# Frozen by fm24ctl batch create; do not edit.',
                 'name=test-batch']
        lines += ['arm=%s /fixtures/%s.json' % (arm, arm) for arm in arms]
        lines += ['seeds=%s' % self.seeds.spec, 'chunk=%d' % chunk, 'per_match=1',
                  'seed_errors=skip', 'compress=%s' % ('gzip' if compress else 'none')]
        lines += ['fixture=%s json_sha256=00 bin_sha256=00 home_team=- away_team=677' % arm
                  for arm in arms]
        body = ''.join(line + '\n' for line in lines)
        self.plan_sha256 = hashlib.sha256(body.encode('utf-8')).hexdigest()
        self.write('plan.txt', body + 'plan_sha256=%s\n' % self.plan_sha256)

    def write(self, name, text, mode='w'):
        path = os.path.join(self.folder, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, mode) as file:
            file.write(text)
        return path

    def commit(self, arm, first, sessions=(('s0001', None),), reference_match=None):
        """Commits the chunk from `first`: its files and chunk.json. `sessions` are the parts, each
        a session and the index after its last (None for the chunk's end); `reference_match`, a
        chunk 0's comparison with the canary reference (None: not compared)."""
        last = min(first + self.chunk - 1, self.seeds.last)
        files = {role: [fm24batch.HEADERS[role]] for role in fm24batch.ROLES}
        outcomes = {'matches': 0, 'home': 0, 'draw': 0, 'away': 0}
        for index in range(first, last + 1):
            seed = self.seeds.seed(index)
            played = self.play(arm, index, seed)
            if played is None:
                files['failed'].append('%d,%d,rebuild' % (index, seed))
                continue
            home, away = played
            outcomes['matches'] += 1
            outcomes['home' if home > away else 'draw' if home == away else 'away'] += 1
            files['results'].append('%d,%d,%d,%d,21600,1,%016x,%d.5' % (
                index, seed, home, away, seed * 7, 2000 + index))
            for side, goals in (('home', home), ('away', away)):
                files['match-teams'].append(team_row(index, seed, side, goals, index % 5))
                files['match-players'].append(player_row(
                    index, seed, side, 0, self.uids[side][0], True, 90, '7.%02d' % (
                        index % 100), index % 3))
                files['match-players'].append(player_row(
                    index, seed, side, 1, self.uids[side][1], index % 2 == 0, 30, '6.50', 1))
        chunk = 'chunks/arm=%s/chunk=%010d' % (arm, first)
        entries = []
        for role in fm24batch.ROLES:
            if role == 'failed' and len(files[role]) == 1:
                continue
            raw = ''.join(line + '\n' for line in files[role]).encode('utf-8')
            name = role + ('.csv.gz' if self.compress else '.csv')
            stored = gzip.compress(raw, 6, mtime=0) if self.compress else raw
            self.write(chunk + '/' + name, stored, 'wb')
            entries.append({'name': name, 'rows': len(files[role]) - 1, 'raw_bytes': len(raw),
                            'raw_sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(stored),
                            'sha256': hashlib.sha256(stored).hexdigest()})
        parts = []
        start = first
        for session, end in sessions:
            end = last + 1 if end is None else end
            parts.append({'attempt': len(parts) + 1, 'first_index': start,
                          'count': last + 1 - start, 'verified': end - start, 'outcome': 'ok',
                          'session': session})
            start = end
        manifest = {'format': 'fm24-chunk-manifest/1', 'batch': 'test-batch',
                    'plan_sha256': self.plan_sha256, 'arm': arm,
                    'seeds': {'spec': self.seeds.spec, 'first_index': first, 'last_index': last,
                              'count': last + 1 - first},
                    'excluded': [], 'parts': parts, 'files': entries, 'summary': outcomes,
                    'checks': {'verified': True, 'canary_reference_match': reference_match}}
        self.write(chunk + '/chunk.json', json.dumps(manifest) + '\n')
        return os.path.join(self.folder, chunk)

    def commit_all(self, arm):
        for first in range(self.seeds.first, self.seeds.last + 1, self.chunk):
            self.commit(arm, first)


def read_gzip_lines(path):
    with gzip.open(path, 'rt', encoding='utf-8') as file:
        return file.read().split('\n')[:-1]


def run_tool(name, *arguments, cwd=None):
    """Runs a tool as a script, isolated, as docs/BATCH.md does."""
    return subprocess.run([sys.executable, '-I', os.path.join(HERE, name)] + list(arguments),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd,
                          universal_newlines=True)


class SeedTests(unittest.TestCase):
    def test_golden_vectors(self):
        for key, vectors in fm24seeds.GOLDEN.items():
            for index, seed in vectors.items():
                self.assertEqual(fm24seeds.scatter(key, index), seed, (key, index))

    def test_scatter_is_a_bijection_and_keys_differ(self):
        seeds = [fm24seeds.scatter(20261025, index) for index in range(100000)]
        self.assertEqual(len(set(seeds)), len(seeds))
        neighbours = [fm24seeds.scatter(20261026, index) for index in range(100000)]
        # Two random sets of 10^5 of 2^32 seeds share about 2.3 seeds.
        self.assertLess(len(set(seeds) & set(neighbours)), 20)
        self.assertFalse(any(abs(a - b) == 1 for a, b in zip(seeds, seeds[1:])))

    def test_seed_sets(self):
        self.assertEqual(fm24seeds.SeedSet('range:5-9').seed(7), 7)
        scattered = fm24seeds.SeedSet('scatter:20261025:0-999999')
        self.assertEqual((scattered.count, scattered.seed(999999)), (1000000, 1517702698))
        with self.assertRaises(IndexError):
            scattered.seed(1000000)
        with tempfile.TemporaryDirectory() as root:
            inputs = os.path.join(root, 'inputs')
            os.makedirs(inputs)
            with open(os.path.join(inputs, 'seeds.txt'), 'w') as file:
                file.write('42\n7\n')
            listed = fm24seeds.SeedSet('list:/elsewhere/seeds.txt', inputs)
            self.assertEqual((listed.first, listed.last, listed.seed(1)), (0, 1, 7))
            # Given as a file, without a batch, the file is read as given.
            original = os.path.join(root, 'my-seeds.txt')
            with open(original, 'w') as file:
                file.write('5000\n5001\n5002\n')
            self.assertEqual(fm24seeds.SeedSet('list:' + original).count, 3)

    def test_season_seeds(self):
        """Season s of fixture k of F plays scatter(key, s * F + k), as batch_plan.c makes them."""
        plan = fm24seeds.SeedSet('season:20261027:0-999')
        self.assertEqual((plan.first, plan.last, plan.count), (0, 999, 1000))
        for (season, fixture), seed in fm24seeds.GOLDEN_SEASON.items():
            self.assertEqual(plan.fixture(380, fixture).seed(season), seed)
        with self.assertRaises(ValueError):
            plan.seed(0)
        listed = run_tool('fm24seeds.py', 'season:20261027:380:1:0-999', '0', '1')
        self.assertEqual(listed.stdout, 'index,seed\n0,828994977\n1,3350105861\n')
        for bad in ('season:1:380:380:0-9', 'season:1:0-9:5', 'season:1:380:0:0-11302545'):
            with self.assertRaises(ValueError):
                fm24seeds.SeedSet(bad)

    def test_a_batch_reads_its_frozen_list(self):
        """A list batch plays inputs/seeds.txt, which create copied, whatever has happened to the
        file the plan names since; a frozen copy that does not match seeds_sha256 is refused."""
        with tempfile.TemporaryDirectory() as root:
            folder = os.path.join(root, 'batch')
            os.makedirs(os.path.join(folder, 'inputs'))
            frozen = ''.join('%d\n' % (1000 + i) for i in range(25)).encode('utf-8')
            with open(os.path.join(folder, 'inputs', 'seeds.txt'), 'wb') as file:
                file.write(frozen)
            original = os.path.join(root, 'my-seeds.txt')
            with open(original, 'w') as file:
                file.write('# seeds for the next batch\n' + ''.join(
                    '%d\n' % (5000 + i) for i in range(200)))
            body = ''.join(line + '\n' for line in [
                'format=fm24-batch-plan/1', 'name=listed', 'arm=baseline /fixtures/baseline.json',
                'seeds=list:' + original, 'chunk=10',
                'seeds_sha256=' + hashlib.sha256(frozen).hexdigest()])
            with open(os.path.join(folder, 'plan.txt'), 'w') as file:
                file.write(body + 'plan_sha256=%s\n' % hashlib.sha256(body.encode()).hexdigest())
            batch = fm24batch.Batch(folder)
            self.assertEqual((batch.seeds.count, batch.seeds.seed(0), batch.planned_chunks()),
                             (25, 1000, 3))
            with open(os.path.join(folder, 'inputs', 'seeds.txt'), 'ab') as file:
                file.write(b'1025\n')
            with self.assertRaisesRegex(ValueError, 'seeds_sha256'):
                fm24batch.Batch(folder)

    def test_prints_a_set(self):
        done = run_tool('fm24seeds.py', 'scatter:20261025:0-999999', '0', '2')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, 'index,seed\n0,4210832071\n1,3117509974\n2,3856835949\n')


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.out = os.path.join(self.root, 'merged')

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_merges_chunks_in_index_order_with_one_header(self):
        batch = BatchFolder(self.root, play=lambda arm, index, seed: None if index == 113 else (
            index % 3, index % 2))
        # Committed out of order, a commit that stopped before its chunk.json, and one that stopped
        # after it but before its rename: neither is committed.
        for first in (120, 100, 110):
            batch.commit('baseline', first)
        chunks = os.path.join(batch.folder, 'chunks/arm=baseline')
        os.makedirs(os.path.join(chunks, 'chunk=0000000130.tmp'))
        shutil.copytree(os.path.join(chunks, 'chunk=0000000110'),
                        os.path.join(chunks, 'chunk=0000000110.tmp'))
        done = run_tool('merge.py', batch.folder, '--arm', 'baseline', '--out', self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        results = read_gzip_lines(os.path.join(self.out, 'baseline-results.csv.gz'))
        self.assertEqual(results[0], fm24batch.HEADERS['results'])
        self.assertEqual([int(line.split(',')[0]) for line in results[1:]],
                         [index for index in range(100, 130) if index != 113])
        failed = read_gzip_lines(os.path.join(self.out, 'baseline-failed.csv.gz'))
        self.assertEqual(failed, [fm24batch.HEADERS['failed'], '113,%d,rebuild' % (
            batch.seeds.seed(113))])
        players = read_gzip_lines(os.path.join(self.out, 'baseline-match-players.csv.gz'))
        self.assertEqual(players.count(fm24batch.HEADERS['match-players']), 1)
        self.assertEqual(len(players), 1 + 4 * 29)
        with open(os.path.join(self.out, 'baseline-merge.json'), encoding='utf-8') as file:
            manifest = json.load(file)
        self.assertEqual(manifest['format'], 'fm24-batch-merge/1')
        self.assertEqual((manifest['chunks_planned'], manifest['chunks_committed']), (3, 3))
        self.assertEqual([chunk['first_index'] for chunk in manifest['chunks']], [100, 110, 120])
        with open(os.path.join(batch.folder, 'chunks/arm=baseline/chunk=0000000110/chunk.json'),
                  encoding='utf-8') as file:
            chunk = json.load(file)
        self.assertEqual(manifest['chunks'][1]['files'], [
            {'name': entry['name'], 'sha256': entry['sha256'], 'raw_sha256': entry['raw_sha256']}
            for entry in chunk['files']])
        for output in manifest['outputs']:
            with open(os.path.join(self.out, output['name']), 'rb') as file:
                stored = file.read()
            self.assertEqual(hashlib.sha256(stored).hexdigest(), output['sha256'])
            self.assertEqual(hashlib.sha256(gzip.decompress(stored)).hexdigest(),
                             output['raw_sha256'])
            self.assertEqual(len(stored), output['bytes'])
        self.assertEqual(manifest['outputs'][0]['rows'], 29)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'press-results.csv.gz')))

    def test_leaves_suspect_chunks_out_unless_asked(self):
        batch = BatchFolder(self.root)
        for arm in batch.arms:
            batch.commit_all(arm)
        batch.write('suspect.csv',
                    'arm,chunk,session,reason\npress,0000000110,s0002,canary_mismatch\n')
        done = run_tool('merge.py', batch.folder, '--out', self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        indices = [int(line.split(',')[0]) for line in read_gzip_lines(
            os.path.join(self.out, 'press-results.csv.gz'))[1:]]
        self.assertEqual(indices, list(range(100, 110)) + list(range(120, 130)))
        self.assertEqual(len(read_gzip_lines(os.path.join(self.out, 'baseline-results.csv.gz'))),
                         31)
        with open(os.path.join(self.out, 'press-merge.json'), encoding='utf-8') as file:
            manifest = json.load(file)
        self.assertEqual([(chunk['first_index'], chunk['reason']) for chunk in
                          manifest['left_out']], [(110, 'suspect')])
        done = run_tool('merge.py', batch.folder, '--out', self.out, '--include-suspect',
                        '--arm', 'press')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(read_gzip_lines(os.path.join(self.out, 'press-results.csv.gz'))), 31)

    def test_leaves_out_a_chunk_0_that_differs_from_the_reference(self):
        """chunk.json records the comparison when the chunk commits; suspect.csv lists it only
        once batch run has acted on it, which a run stopped in between leaves to the next."""
        batch = BatchFolder(self.root)
        batch.commit('baseline', 100, reference_match=False)
        batch.commit('baseline', 110)
        batch.commit('press', 100, reference_match=True)
        done = run_tool('merge.py', batch.folder, '--out', self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('arm baseline: 1 of 3 chunks merged, 1 left out as suspect', done.stdout)
        self.assertIn('arm press: 1 of 3 chunks merged, 0 left out', done.stdout)
        indices = [int(line.split(',')[0]) for line in read_gzip_lines(
            os.path.join(self.out, 'baseline-results.csv.gz'))[1:]]
        self.assertEqual(indices, list(range(110, 120)))
        result = summary.summarise(fm24batch.Batch(batch.folder), 'baseline', False, False)
        self.assertEqual((result['overall'].matches, result['left_out']), (10, 1))

    def test_merges_the_same_bytes_again(self):
        batch = BatchFolder(self.root)
        batch.commit_all('baseline')
        hashes = []
        for _ in range(2):
            fm24batch_batch = fm24batch.Batch(batch.folder)
            manifest = merge.merge_arm(fm24batch_batch, 'baseline', self.out, False)
            hashes.append([output['sha256'] for output in manifest['outputs']])
        self.assertEqual(hashes[0], hashes[1])
        self.assertFalse([name for name in os.listdir(self.out) if name.endswith('.tmp')])

    def test_reads_an_uncompressed_batch(self):
        batch = BatchFolder(self.root, compress=False)
        batch.commit_all('baseline')
        manifest = merge.merge_arm(fm24batch.Batch(batch.folder), 'baseline', self.out, False)
        self.assertEqual(manifest['outputs'][0]['rows'], 30)

    def test_refuses_a_chunk_that_does_not_match_its_manifest(self):
        batch = BatchFolder(self.root)
        batch.commit_all('baseline')
        chunk = os.path.join(batch.folder, 'chunks/arm=baseline/chunk=0000000110')
        path = os.path.join(chunk, 'results.csv.gz')
        lines = read_gzip_lines(path)
        lines[3] = lines[3].replace(',21600,', ',21601,')
        with open(path, 'wb') as file:
            file.write(gzip.compress(''.join(line + '\n' for line in lines).encode('utf-8')))
        done = run_tool('merge.py', batch.folder, '--out', self.out)
        self.assertEqual(done.returncode, 1)
        self.assertIn('does not match its SHA-256', done.stderr)
        # A torn last line, or another header, is refused even when chunk.json agrees with it; so
        # is a folder without a plan.
        rewrite(chunk, 'results.csv.gz', '\n'.join(lines[:4]), rows=3)
        with self.assertRaisesRegex(fm24batch.BatchError, 'cut short'):
            list(fm24batch.Batch(batch.folder).chunks('baseline')[1].lines('results'))
        rewrite(chunk, 'results.csv.gz', ''.join(line + '\n' for line in [
            lines[0].replace(',ms', ',time')] + lines[1:]), rows=len(lines) - 1)
        with self.assertRaisesRegex(fm24batch.BatchError, 'not the header'):
            list(fm24batch.Batch(batch.folder).chunks('baseline')[1].lines('results'))
        self.assertEqual(run_tool('merge.py', self.root).returncode, 1)


def rewrite(chunk, name, text, rows):
    """Replaces a committed file and its entry in chunk.json, as if the batch had committed it."""
    raw = text.encode('utf-8')
    stored = gzip.compress(raw, 6, mtime=0)
    with open(os.path.join(chunk, name), 'wb') as file:
        file.write(stored)
    path = os.path.join(chunk, 'chunk.json')
    with open(path, encoding='utf-8') as file:
        manifest = json.load(file)
    for entry in manifest['files']:
        if entry['name'] == name:
            entry.update(rows=rows, raw_bytes=len(raw), raw_sha256=hashlib.sha256(raw).hexdigest(),
                         bytes=len(stored), sha256=hashlib.sha256(stored).hexdigest())
    with open(path, 'w', encoding='utf-8') as file:
        json.dump(manifest, file)


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_wilson_intervals(self):
        low, high = summary.wilson(50, 100)
        self.assertAlmostEqual(low, 0.40383, places=5)
        self.assertAlmostEqual(high, 0.59617, places=5)
        low, high = summary.wilson(0, 10)
        self.assertEqual(low, 0.0)
        self.assertAlmostEqual(high, 0.27753, places=5)
        self.assertEqual(summary.wilson(0, 0), (0.0, 0.0))

    def test_lag1_autocorrelation(self):
        def direct(series):
            mean = sum(series) / len(series)
            numerator = sum((a - mean) * (b - mean) for a, b in zip(series, series[1:]))
            return numerator / sum((x - mean) ** 2 for x in series)

        for series in ([1, 0] * 20, [1, 1, 0, 0] * 10, [1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1],
                       [int(fm24seeds.scatter(3, i) % 3 == 0) for i in range(1000)]):
            lag = summary.Lag1()
            for value in series:
                lag.add(value)
            self.assertAlmostEqual(lag.value(), direct(series), places=12)
        alternating = summary.Lag1()
        for value in [1, 0] * 50:
            alternating.add(value)
        self.assertLess(alternating.value(), -0.95)
        constant = summary.Lag1()
        for value in [1] * 10:
            constant.add(value)
        self.assertIsNone(constant.value())

    def test_outcomes_by_arm_session_and_chunk(self):
        batch = BatchFolder(self.root, play=lambda arm, index, seed: (
            None if index == 125 else (2, 1) if index % 3 == 0 else (1, 1) if index % 3 == 1
            else (0, 2)))
        batch.commit('baseline', 100)
        batch.commit('baseline', 110, sessions=(('s0001', 114), ('s0002', None)))
        batch.commit('baseline', 120, sessions=(('s0002', None),))
        result = summary.summarise(fm24batch.Batch(batch.folder), 'baseline', False)
        overall = result['overall']
        played = [index for index in range(100, 130) if index != 125]
        self.assertEqual((overall.matches, overall.home, overall.draw, overall.away), (
            29, sum(i % 3 == 0 for i in played), sum(i % 3 == 1 for i in played),
            sum(i % 3 == 2 for i in played)))
        sessions = result['sessions']
        self.assertEqual(sessions['s0001'].matches, 14)
        self.assertEqual(sessions['s0002'].matches, 15)
        self.assertEqual([outcomes.matches for _, outcomes in result['chunks']], [10, 10, 9])
        series = [int(i % 3 == 1) for i in played]
        lag = summary.Lag1()
        for value in series:
            lag.add(value)
        self.assertAlmostEqual(result['draws'].value(), lag.value())

    def test_per_match_means_as_run_detailed_reports_them(self):
        batch = BatchFolder(self.root, first=0, last=3, chunk=2,
                            play=lambda arm, index, seed: (index, 0))
        batch.commit_all('baseline')
        out = os.path.join(self.root, 'tables')
        done = run_tool('summary.py', batch.folder, '--arm', 'baseline', '--out', out, '--chunks')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('arm baseline: 2 of 2 chunks; 4 matches; home 75.0%', done.stdout)
        self.assertIn('chunk 0000000002: 2 matches', done.stdout)
        with open(os.path.join(out, 'baseline-players.csv'), encoding='utf-8') as file:
            rows = file.read().split('\n')[:-1]
        self.assertEqual(rows[0].split(',')[:9], [
            'side', 'team', 'player', 'person_uid', 'matches', 'appearances', 'starts', 'minutes',
            'rating'])
        self.assertEqual(len(rows[0].split(',')), 9 + PLAYER_VALUES)
        by_player = {(row.split(',')[0], row.split(',')[3]): row.split(',') for row in rows[1:]}
        # Player 500 starts every match: ratings 7.00-7.03, whose mean `run --detailed` prints as
        # 2806 / 100.0 / 4 to two places, and the statistics index % 3: 0, 1, 2 and 0.
        starter = by_player[('home', '500')]
        self.assertEqual(starter[4:9], ['4', '4', '4', '90.0', '%.2f' % (2806 / 100.0 / 4)])
        self.assertEqual(starter[9], '%.3f' % (3 / 4))
        # Player 501 appears in matches 0 and 2 only: averages are per match, the rating per
        # appearance.
        substitute = by_player[('home', '501')]
        self.assertEqual(substitute[4:10], ['4', '2', '0', '15.0', '6.50', '0.500'])
        self.assertEqual([row.split(',')[0] for row in rows[1:]], ['home', 'home', 'away', 'away'])
        with open(os.path.join(out, 'baseline-teams.csv'), encoding='utf-8') as file:
            teams = file.read().split('\n')[:-1]
        # Home goals are the index, 0-3, and the other statistics index % 5.
        self.assertEqual(teams[1].split(',')[:5], ['home', '"1"', '4', '1.500', '1.500'])
        self.assertEqual(teams[2].split(',')[:4], ['away', '"677"', '4', '0.000'])
        with open(os.path.join(out, 'arms.csv'), encoding='utf-8') as file:
            arms = file.read().split('\n')
        self.assertEqual(arms[1].split(',')[:3], ['baseline', '4', '3'])

    def test_lists_players_by_unique_id_and_a_short_last_chunk(self):
        """Players come out by unique ID whatever their slots, and a last chunk shorter than the
        others counts as one."""
        batch = BatchFolder(self.root, first=100, last=133, uids={'home': (509, 501),
                                                                   'away': (608, 602)})
        batch.commit_all('baseline')
        self.assertEqual(fm24batch.Batch(batch.folder).planned_chunks(), 4)
        out = os.path.join(self.root, 'tables')
        done = run_tool('summary.py', batch.folder, '--arm', 'baseline', '--out', out)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('arm baseline: 4 of 4 chunks; 34 matches', done.stdout)
        with open(os.path.join(out, 'baseline-players.csv'), encoding='utf-8') as file:
            rows = file.read().split('\n')[1:-1]
        self.assertEqual([row.split(',')[3] for row in rows], ['501', '509', '602', '608'])

    def test_names_from_the_lookups(self):
        batch = BatchFolder(self.root, first=0, last=1, chunk=2)
        batch.commit_all('baseline')
        batch.write('lookups/teams.txt', '1\tLiverpool\n677\tArsenal\n')
        batch.write('lookups/players.txt', '500\tMohamed\tSalah\t\tLiverpool\n'
                    '600\tBukayo\tSaka\tSaka "B"\tArsenal\n')
        out = os.path.join(self.root, 'tables')
        self.assertEqual(run_tool('summary.py', batch.folder, '--out', out).returncode, 0)
        with open(os.path.join(out, 'baseline-players.csv'), encoding='utf-8') as file:
            text = file.read()
        self.assertIn('home,"Liverpool","Mohamed Salah",500,', text)
        self.assertIn('away,"Arsenal","Saka ""B""",600,', text)

    def test_leaves_suspect_chunks_out_and_reads_only_results_if_asked(self):
        batch = BatchFolder(self.root)
        batch.commit_all('baseline')
        batch.write('suspect.csv', 'arm,chunk,session,reason\nbaseline,0000000100,s0001,'
                    'reference_mismatch\n')
        result = summary.summarise(fm24batch.Batch(batch.folder), 'baseline', False, False)
        self.assertEqual((result['overall'].matches, result['left_out'], result['means']),
                         (20, 1, None))
        result = summary.summarise(fm24batch.Batch(batch.folder), 'baseline', True)
        self.assertEqual(result['overall'].matches, 30)
        done = run_tool('summary.py', batch.folder, '--results-only')
        self.assertIn('(1 suspect left out)', done.stdout)

    def test_reads_past_what_a_power_cut_left_in_suspect_csv(self):
        """A line a power cut left as zeros, with batch run's lines after it, and one cut short of
        its reason: only whole lines count, as batch run reads them."""
        batch = BatchFolder(self.root)
        for arm in batch.arms:
            batch.commit_all(arm)
        cases = [
            (b'arm,chunk,session,reason\n' + b'\0' * 45 + b'\nbaseline,0000000110,s0001,'
             b'reference_mismatch\npress,0000000120,s0002,canary_mism\npress,00000001',
             {('baseline', 110), ('press', 120)}),
            (b'\0' * 64 + b'\nbaseline,0000000100,s0001,reference_mismatch\n',
             {('baseline', 100)}),
            (b'\0' * 64, set()),
            (b'arm,chunk,session,reason\nbaseline,0000000100,s0001,\n', set()),
        ]
        tools = {'summary.py': ['--results-only'],
                 'merge.py': ['--out', os.path.join(self.root, 'merged')]}
        for text, suspect in cases:
            batch.write('suspect.csv', text, 'wb')
            self.assertEqual(fm24batch.Batch(batch.folder).suspect, suspect, text)
            for tool, options in tools.items():
                done = run_tool(tool, batch.folder, '--arm', 'baseline', *options)
                self.assertEqual(done.returncode, 0, done.stderr)


def c_string(source, name):
    """The value of `const char name[] = "..." "...";` in a C source file."""
    match = re.search(r'const char %s\[\] =((?:\s*"(?:[^"\\]|\\.)*")+);' % re.escape(name), source)
    if not match:
        raise AssertionError('%s is not in the source' % name)
    text = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1)))
    return text.replace('\\n', '\n')


def season_player_row(index, seed, side, team, slot, uid, in_squad=True, started=False,
                      appeared=False, minutes=0, injured=0, rating='', values=None):
    """A per-match player row of a season fixture: the statistics `values` names (others 0) when
    the player appeared, else empty."""
    values = values or {}
    stats = [str(values.get(name, 0)) for name in summary.PLAYER_VALUES] if appeared else (
        [''] * PLAYER_VALUES)
    fields = [index, seed, side, team, slot, uid, int(in_squad), int(started), int(appeared),
              minutes, 0, 0, injured, 0, 0, rating if appeared else '']
    return ','.join(str(field) for field in fields + stats)


def season_team_row(index, seed, side, team, goals):
    return ','.join(str(field) for field in [index, seed, side, team, 3, 2, goals] + [1] * (
        TEAM_VALUES - 1))


class SeasonFolder:
    """A season batch folder of `teams` (unique IDs, named by `names`), seasons first-last in blocks
    of `chunk`, each match played by `play(season, home, away)` -> (home goals, away goals), None
    for a seed that failed to rebuild or 'excluded' for a poison seed. `players(season, fixture,
    side, team)` gives each side's player rows' fields (slot, uid and season_player_row's
    keywords); by default each team's 10 * uid starts and plays 90 minutes."""

    def __init__(self, root, teams=(101, 102, 103, 104), first=0, last=3, chunk=2, key=7,
                 play=None, players=None, per_match=True, names=None, lookups=True):
        self.folder = os.path.join(root, 'batch')
        self.teams = teams
        self.chunk = chunk
        self.per_match = per_match
        self.seeds = fm24seeds.SeedSet('season:%d:%d-%d' % (key, first, last))
        self.play = play or (lambda season, home, away: (0, 0))
        self.players = players or (lambda season, fixture, side, team: [
            (0, team * 10, {'started': True, 'appeared': True, 'minutes': 90, 'rating': '7.00'})])
        self.arms = []
        self.pairs = {}
        n = len(teams)
        for fixture in range(n * (n - 1)):
            home, other = divmod(fixture, n - 1)
            away = other if other < home else other + 1
            arm = 'f%04d' % (fixture + 1)
            self.arms.append(arm)
            self.pairs[arm] = (teams[home], teams[away])
        os.makedirs(os.path.join(self.folder, 'chunks'))
        lines = ['format=fm24-batch-plan/1', '# Frozen by fm24ctl batch create; do not edit.',
                 'name=test-season', 'mode=season', 'base=/fixtures/next.json']
        lines += ['team=%d' % team for team in teams]
        lines += ['seeds=%s' % self.seeds.spec, 'chunk=%d' % chunk,
                  'per_match=%d' % int(per_match), 'seed_errors=skip', 'compress=gzip',
                  'base_fixture=json_sha256=%s bin_sha256=%s home_team=- away_team=-' % (
                      '0' * 64, '1' * 64)]
        lines += ['pair=%s home_team=%d away_team=%d' % ((arm,) + self.pairs[arm])
                  for arm in self.arms]
        body = ''.join(line + '\n' for line in lines)
        self.plan_sha256 = hashlib.sha256(body.encode('utf-8')).hexdigest()
        self.write('plan.txt', body + 'plan_sha256=%s\n' % self.plan_sha256)
        fixtures = ['format=fm24-season-fixtures/1', 'plan_sha256=' + self.plan_sha256]
        fixtures += ['fixture=%s json_sha256=%s bin_sha256=%s home_team=%d away_team=%d' % (
            arm, hashlib.sha256(arm.encode()).hexdigest(), hashlib.sha256(arm.encode() * 2)
            .hexdigest(), self.pairs[arm][0], self.pairs[arm][1]) for arm in self.arms]
        text = ''.join(line + '\n' for line in fixtures)
        self.write('inputs/fixtures.txt', text + 'fixtures_sha256=%s\n' % hashlib.sha256(
            text.encode('utf-8')).hexdigest())
        names = names or {team: 'Team %d' % team for team in teams}
        if lookups:
            self.write('lookups/teams.txt', ''.join('%d\t%s\n' % (uid, name)
                                                    for uid, name in names.items()))
            self.write('lookups/players.txt', ''.join(
                '%d\tFirst\tLast %d\t\t%d\n' % (team * 10 + slot, team * 10 + slot, team)
                for team in teams for slot in range(4)))

    def write(self, name, text, mode='w'):
        path = os.path.join(self.folder, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, mode) as file:
            file.write(text)
        return path

    def commit(self, arm, first, team_uid=None, reference_match=None):
        """Commits fixture `arm`'s chunk from season `first`; `team_uid` puts another team in its
        home team rows."""
        last = min(first + self.chunk - 1, self.seeds.last)
        seeds = self.seeds.fixture(len(self.arms), self.arms.index(arm))
        home, away = self.pairs[arm]
        files = {role: [fm24batch.HEADERS[role]] for role in fm24batch.ROLES}
        excluded = []
        for index in range(first, last + 1):
            seed = seeds.seed(index)
            played = self.play(index, home, away)
            if played == 'excluded':
                excluded.append({'index': index, 'seed': seed, 'reason': 'poison'})
                continue
            if played is None:
                files['failed'].append('%d,%d,rebuild' % (index, seed))
                continue
            files['results'].append('%d,%d,%d,%d,21600,1,%016x,%d.5' % (
                index, seed, played[0], played[1], seed, 2000 + index))
            for side, team, goals in (('home', home, played[0]), ('away', away, played[1])):
                files['match-teams'].append(season_team_row(
                    index, seed, side, team_uid if team_uid and side == 'home' else team, goals))
                for slot, uid, fields in self.players(index, arm, side, team):
                    files['match-players'].append(season_player_row(
                        index, seed, side, team, slot, uid, **fields))
        chunk = 'chunks/arm=%s/chunk=%010d' % (arm, first)
        entries = []
        for role in fm24batch.ROLES:
            if (role == 'failed' and len(files[role]) == 1) or (
                    not self.per_match and role.startswith('match-')):
                continue
            raw = ''.join(line + '\n' for line in files[role]).encode('utf-8')
            stored = gzip.compress(raw, 6, mtime=0)
            self.write(chunk + '/' + role + '.csv.gz', stored, 'wb')
            entries.append({'name': role + '.csv.gz', 'rows': len(files[role]) - 1,
                            'raw_bytes': len(raw), 'raw_sha256': hashlib.sha256(raw).hexdigest(),
                            'bytes': len(stored), 'sha256': hashlib.sha256(stored).hexdigest()})
        manifest = {'format': 'fm24-chunk-manifest/1', 'batch': 'test-season',
                    'plan_sha256': self.plan_sha256, 'arm': arm,
                    'seeds': {'spec': seeds.spec, 'first_index': first, 'last_index': last,
                              'count': last + 1 - first},
                    'excluded': excluded, 'parts': [], 'files': entries,
                    'checks': {'verified': True, 'canary_reference_match': reference_match}}
        self.write(chunk + '/chunk.json', json.dumps(manifest) + '\n')

    def commit_all(self, skip=()):
        for first in range(self.seeds.first, self.seeds.last + 1, self.chunk):
            for arm in self.arms:
                if (arm, first) not in skip:
                    self.commit(arm, first)


def read_json(path):
    with open(path, encoding='utf-8') as file:
        return json.load(file)


def file_sha256(path):
    with open(path, 'rb') as file:
        return hashlib.sha256(file.read()).hexdigest()


def read_csv_lines(path):
    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', encoding='utf-8') as file:
        return file.read().split('\n')[:-1]


class SeasonTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.out = os.path.join(self.root, 'seasons')

    def tearDown(self):
        shutil.rmtree(self.root)

    def run_season(self, folder, *arguments):
        done = run_tool('season.py', folder, '--out', self.out, *arguments)
        self.assertEqual(done.returncode, 0, done.stderr)
        return done

    def output(self, name):
        return read_csv_lines(os.path.join(self.out, name))

    def test_reads_a_season_plan(self):
        made = SeasonFolder(self.root, key=20261027, teams=tuple(range(1, 21)), last=999,
                            chunk=100)
        batch = fm24batch.Batch(made.folder)
        self.assertTrue(batch.season)
        self.assertEqual((len(batch.arms), batch.arms[19], batch.pairs['f0020']),
                         (380, 'f0020', (2, 1)))
        self.assertEqual((batch.teams[0], batch.teams[-1], batch.planned_chunks()), (1, 20, 10))
        for (season_number, fixture), seed in fm24seeds.GOLDEN_SEASON.items():
            self.assertEqual(batch.arm_seeds(batch.arms[fixture]).seed(season_number), seed)
        fixtures, digest = batch.fixtures()
        self.assertEqual((len(fixtures), fixtures['f0003'][2:]), (380, ('1', '4')))
        with open(os.path.join(made.folder, 'inputs', 'fixtures.txt'), 'a') as file:
            file.write('x\n')
        with self.assertRaisesRegex(fm24batch.BatchError, 'changed'):
            batch.fixtures()

    def test_ranks_a_league_by_hand(self):
        """Points, then goal difference, then goals for, then the name, then the unique ID."""
        results = {
            # season 0: A and C level on points, A ahead on goal difference, C on goals for
            (0, 101, 102): (2, 0), (0, 103, 104): (3, 2),
            (1, 101, 102): (3, 1), (1, 103, 104): (2, 0),  # season 1: level on GD, by goals for
        }  # season 2: every match 0-0: by name, then by unique ID (102 and 103 share theirs)
        made = SeasonFolder(self.root, last=2, chunk=3,
                            play=lambda s, home, away: results.get((s, home, away), (0, 0)),
                            names={101: 'Zulu', 102: 'Beta', 103: 'Beta', 104: 'Alpha'})
        made.commit_all()
        self.run_season(made.folder)
        standings = self.output('standings.csv.gz')
        self.assertEqual(standings[0], season.STANDINGS)
        self.assertEqual(standings[1:5], [
            '0,1,101,Zulu,6,1,5,0,2,0,2,8', '0,2,103,Beta,6,1,5,0,3,2,1,8',
            '0,3,104,Alpha,6,0,5,1,2,3,-1,5', '0,4,102,Beta,6,0,5,1,0,2,-2,5'])
        self.assertEqual([line.split(',')[2] for line in standings[5:9]],
                         ['101', '103', '102', '104'])
        self.assertEqual(standings[5], '1,1,101,Zulu,6,1,5,0,3,1,2,8')
        self.assertEqual([line.split(',')[2] for line in standings[9:13]],
                         ['104', '102', '103', '101'])
        teams = self.output('teams.csv')
        self.assertEqual(teams[0].split(',')[:4] + teams[0].split(',')[-1:],
                         ['team_uid', 'team', 'seasons', 'points_mean', 'position_4'])
        self.assertEqual(teams[1], '101,Zulu,3,7.333,1.155,6,8,2.000,1.667,0.333,2,0,0,1')
        fixtures = self.output('fixtures.csv')
        self.assertEqual(fixtures[0], season.FIXTURES)
        self.assertEqual(fixtures[4].split(',')[:5], ['f0004', '102', 'Beta', '101', 'Zulu'])
        manifest = read_json(os.path.join(self.out, 'seasons.json'))
        self.assertEqual((manifest['format'], manifest['seasons']['included'],
                          manifest['seasons']['left_out']), ('fm24-seasons/1', [[0, 2]], []))
        self.assertEqual(len(manifest['chunks']), 12)
        self.assertEqual(self.output('incomplete.csv'), [season.INCOMPLETE])

    def test_totals_each_players_season(self):
        """Squad, appearances, starts, substitute appearances, minutes, exact ratings, an empty
        average without appearances, the statistics summed and injuries."""
        def players(index, arm, side, team):
            if team != 101:
                return [(0, team * 10, {'started': True, 'appeared': True, 'minutes': 90,
                                        'rating': '7.00'})]
            sub = index % 2 == 0 and side == 'home'
            return [
                (0, 1010, {'started': True, 'appeared': True, 'minutes': 90,
                           'injured': 60 if arm == 'f0002' else 0,
                           'rating': '6.95' if arm == 'f0004' else '7.00',
                           'values': {'goals': int(arm == 'f0001'), 'xg': 0.1234,
                                      'distance_km': 10.5}}),
                (1, 1011, {'appeared': sub, 'minutes': 30 if sub else 0, 'rating': '6.50',
                           'values': {'assists': 1}}),
                (2, 1012, {}),
                (3, 1013, {'in_squad': False}),
            ]
        made = SeasonFolder(self.root, last=1, players=players)
        made.commit_all()
        self.run_season(made.folder)
        rows = self.output('players.csv.gz')
        self.assertEqual(rows[0], season.PLAYERS)
        header = rows[0].split(',')
        by_player = {(line.split(',')[0], line.split(',')[3]): dict(zip(header, line.split(',')))
                     for line in rows[1:]}
        self.assertNotIn(('0', '1013'), by_player)
        star = by_player[('0', '1010')]
        self.assertEqual([star[key] for key in ('team_uid', 'team', 'player', 'squad',
                                                'appearances', 'starts', 'sub_appearances',
                                                'minutes', 'average_rating', 'rating_total',
                                                'goals', 'xg', 'distance_km', 'injuries')],
                         ['101', 'Team 101', 'First Last 1010', '6', '6', '6', '0', '540', '6.992',
                          '41.95', '1', '0.7404', '63.0000', '1'])
        sub = by_player[('0', '1011')]
        self.assertEqual([sub[key] for key in ('squad', 'appearances', 'starts', 'sub_appearances',
                                               'minutes', 'average_rating', 'rating_total',
                                               'assists')],
                         ['6', '3', '0', '3', '90', '6.500', '19.50', '3'])
        self.assertEqual([by_player[('1', '1011')][key] for key in ('appearances',
                                                                    'average_rating',
                                                                    'rating_total', 'assists')],
                         ['0', '', '0.00', '0'])
        self.assertEqual(by_player[('0', '1012')]['squad'], '6')
        # By season, then team in the plan's order, then unique ID.
        self.assertEqual([(line.split(',')[0], line.split(',')[3]) for line in rows[1:7]],
                         [('0', '1010'), ('0', '1011'), ('0', '1012'), ('0', '1020'),
                          ('0', '1030'), ('0', '1040')])

    def test_leaves_out_incomplete_seasons_and_lists_them(self):
        def play(index, home, away):
            if index == 4 and (home, away) == (101, 103):
                return None
            if index == 5 and (home, away) == (101, 103):
                return 'excluded'
            return (1, 0)
        made = SeasonFolder(self.root, last=9, play=play)
        # Block 1 lacks fixture 3, block 3's fixture 1 is suspect, block 4 has not begun.
        made.commit_all(skip={('f0003', 2)} | {(arm, 8) for arm in made.arms})
        made.write('suspect.csv', 'arm,chunk,session,reason\nf0001,0000000006,s0002,canary_mismatch\n')
        self.run_season(made.folder, '--matches')
        self.assertEqual(sorted({line.split(',')[0] for line in self.output('standings.csv.gz')[1:]}),
                         ['0', '1'])
        self.assertEqual(self.output('incomplete.csv')[1:], [
            '2,f0003,not_played', '3,f0003,not_played', '4,f0002,failed', '5,f0002,excluded',
            '6,f0001,suspect', '7,f0001,suspect'])
        manifest = read_json(os.path.join(self.out, 'seasons.json'))
        self.assertEqual(manifest['seasons']['included'], [[0, 1]])
        self.assertEqual(manifest['seasons']['left_out'], [[2, 9]])
        matches = self.output('matches.csv.gz')
        self.assertEqual(matches[0], season.MATCHES)
        self.assertEqual(len(matches), 1 + 2 * 12)
        seeds = fm24seeds.SeedSet('season:7:0-9').fixture(12, 1)
        self.assertEqual(matches[2], '0,f0002,%d,101,103,1,0' % seeds.seed(0))
        # The suspect chunk read as asked, and only seasons 6-9.
        self.run_season(made.folder, '--include-suspect', '--seasons', '6-9')
        self.assertEqual(sorted({line.split(',')[0] for line in self.output('standings.csv.gz')[1:]}),
                         ['6', '7'])
        self.assertEqual(self.output('incomplete.csv'), [season.INCOMPLETE])
        manifest = read_json(os.path.join(self.out, 'seasons.json'))
        self.assertEqual((manifest['seasons']['first'], manifest['seasons']['left_out']),
                         (6, [[8, 9]]))
        done = run_tool('season.py', made.folder, '--out', self.out, '--seasons', '8-10')
        self.assertEqual(done.returncode, 1)
        self.assertIn('not within', done.stderr)

    def test_writes_tables_only_without_per_match_rows(self):
        made = SeasonFolder(self.root, last=1, per_match=False)
        made.commit_all()
        self.run_season(made.folder)
        self.assertEqual(len(self.output('standings.csv.gz')), 1 + 2 * 4)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'players.csv.gz')))

    def test_refuses_a_row_of_another_team(self):
        made = SeasonFolder(self.root, last=1)
        made.commit_all(skip={('f0005', 0)})
        made.commit('f0005', 0, team_uid=999)
        done = run_tool('season.py', made.folder, '--out', self.out)
        self.assertEqual(done.returncode, 1)
        self.assertIn('names team 999, not its fixture\'s 102', done.stderr)

    def test_refuses_without_lookups_or_fixtures_and_a_match_batch(self):
        made = SeasonFolder(self.root, last=1, lookups=False)
        made.commit_all()
        done = run_tool('season.py', made.folder, '--out', self.out)
        self.assertEqual(done.returncode, 1)
        self.assertIn('lookups/teams.txt', done.stderr)
        os.remove(os.path.join(made.folder, 'inputs', 'fixtures.txt'))
        made.write('lookups/teams.txt', '101\tA\n')
        done = run_tool('season.py', made.folder, '--out', self.out)
        self.assertIn('not built yet', done.stderr)
        other = BatchFolder(os.path.join(self.root, 'other'))
        done = run_tool('season.py', other.folder, '--out', self.out)
        self.assertEqual(done.returncode, 1)
        self.assertIn('not a season batch', done.stderr)

    def test_writes_the_same_bytes_again(self):
        made = SeasonFolder(self.root, last=3, play=lambda s, home, away: (s % 3, home % 2))
        made.commit_all()
        names = ('standings.csv.gz', 'players.csv.gz', 'matches.csv.gz', 'teams.csv',
                 'fixtures.csv', 'incomplete.csv')
        digests = []
        for _ in range(2):
            self.run_season(made.folder, '--matches')
            digests.append([file_sha256(os.path.join(self.out, name)) for name in names])
            manifest = read_json(os.path.join(self.out, 'seasons.json'))
            self.assertEqual([output['sha256'] for output in manifest['outputs']
                              if output['name'] in names],
                             [digests[-1][names.index(output['name'])]
                              for output in manifest['outputs'] if output['name'] in names])
        self.assertEqual(digests[0], digests[1])
        self.assertFalse([name for name in os.listdir(self.out) if name.endswith('.tmp')])

    def test_summary_and_merge_read_a_season(self):
        made = SeasonFolder(self.root, last=3)
        made.commit_all()
        done = run_tool('summary.py', made.folder, '--arm', 'f0002')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('arm f0002: 2 of 2 chunks; 4 matches', done.stdout)
        done = run_tool('merge.py', made.folder, '--arm', 'f0012', '--out', self.out)
        self.assertEqual(done.returncode, 0, done.stderr)
        results = read_gzip_lines(os.path.join(self.out, 'f0012-results.csv.gz'))
        self.assertEqual([line.split(',')[0] for line in results[1:]], ['0', '1', '2', '3'])


class FormatTests(unittest.TestCase):
    """The tools' headers are those fm24ctl and the harness write."""

    def source(self, path):
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            self.skipTest('the C sources are not beside the tools')
        with open(full, encoding='utf-8') as file:
            return file.read()

    def test_headers_match_the_c_sources(self):
        stats = self.source('src/dll/match_stats.c')
        verify = self.source('src/runner/batch_verify.c')
        expected = {
            'results': c_string(verify, 'batchVerify_chunkResultsHeader'),
            'match-players': c_string(stats, 'matchStats_playerRowsHeader'),
            'match-teams': c_string(stats, 'matchStats_teamRowsHeader'),
            'failed': c_string(verify, 'batchVerify_failedHeader'),
        }
        for role in fm24batch.ROLES:
            self.assertEqual(fm24batch.HEADERS[role] + '\n', expected[role], role)
        self.assertEqual(c_string(verify, 'batchVerify_playersHeader'), expected['match-players'])
        self.assertEqual(c_string(verify, 'batchVerify_teamsHeader'), expected['match-teams'])

    def test_season_goldens_match_the_c_tests(self):
        tests = self.source('tests/batch_season_test.c')
        for (season_number, fixture), seed in fm24seeds.GOLDEN_SEASON.items():
            self.assertIn('{%d, %d, %dU}' % (season_number, fixture, seed), tests)


if __name__ == '__main__':
    unittest.main()
