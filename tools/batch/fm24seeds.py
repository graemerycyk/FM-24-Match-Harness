# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The seeds of a batch's indices, as `fm24ctl batch` makes them (src/runner/batch_plan.c).

  python3 -I tools/batch/fm24seeds.py scatter:20261025:0-999999 [first last]
  python3 -I tools/batch/fm24seeds.py list:<batch>/inputs/seeds.txt
  python3 -I tools/batch/fm24seeds.py season:20261027:380:0:0-999 [first last]

prints index,seed for each index of the set, or of first to last. `scatter(key, index)` is a
bijection on 32-bit indices, so a set's seeds are distinct, and other keys or indices give other
seeds: lowbias32(lowbias32(index ^ low(k)) ^ high(k)) with k = splitmix64(key). A season batch's
seeds=season:<key>:<first>-<last> plays, in season s of fixture k of F, scatter(key, s * F + k):
season:<key>:<F>:<k>:<first>-<last> is fixture k's, as its chunks' chunk.json name it."""
import hashlib
import os
import sys

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
# scatter(20261025, i) for these indices, and scatter(1, i) for 0-2 (docs/BATCH.md).
GOLDEN = {
    20261025: {0: 4210832071, 1: 3117509974, 2: 3856835949, 3: 1809968782, 4: 1239285640,
               999999: 1517702698, 4294967295: 2575468186},
    1: {0: 1625406257, 1: 1840315587, 2: 1295555401},
}
# season:20261027 with 380 fixtures (20 teams): (season, fixture) -> seed, as the batch suite has
# them (tests/batch_season_test.c).
GOLDEN_SEASON = {(0, 0): 2818942921, (0, 1): 828994977, (1, 0): 3825868651,
                 (999, 379): 999216404}


def lowbias32(x):
    x ^= x >> 16
    x = (x * 0x7feb352d) & MASK32
    x ^= x >> 15
    x = (x * 0x846ca68b) & MASK32
    x ^= x >> 16
    return x


def splitmix64(s):
    s = (s + 0x9E3779B97F4A7C15) & MASK64
    s = ((s ^ (s >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    s = ((s ^ (s >> 27)) * 0x94D049BB133111EB) & MASK64
    return s ^ (s >> 31)


def scatter(key, index):
    """The seed index `index` of `scatter:<key>:...` plays."""
    k = splitmix64(key)
    return lowbias32(lowbias32((index ^ k) & MASK32) ^ (k >> 32))


class SeedSet:
    """A plan's seeds= set: range:<first>-<last>, scatter:<key>:<from>-<to> or list:<file>, or a
    season's, season:<key>:<first>-<last>, whose fixture k of F plays
    season:<key>:<F>:<k>:<first>-<last> (fixture()). For a batch, `inputs` is its inputs folder,
    and a list is always its frozen copy there, seeds.txt, checked against `sha256` (plan.txt's
    seeds_sha256) if given: the file the plan names is the one `batch create` copied, which may
    have changed since. Without `inputs`, the file is read as given."""

    def __init__(self, spec, inputs=None, sha256=None):
        self.spec = spec
        kind, _, rest = spec.partition(':')
        self.key = None
        self.list = None
        self.fixtures = None  # season: F and k, once a fixture's
        self.fixture_number = None
        if kind == 'range':
            self.first, self.last = bounds(rest)
        elif kind == 'scatter':
            key, _, span = rest.partition(':')
            self.key = int(key)
            self.first, self.last = bounds(span)
        elif kind == 'season':
            fields = rest.split(':')
            if len(fields) not in (2, 4):
                raise ValueError('not a seed set: %s' % spec)
            self.key = int(fields[0])
            if len(fields) == 4:
                self.fixtures, self.fixture_number = int(fields[1]), int(fields[2])
                if not 0 <= self.fixture_number < self.fixtures:
                    raise ValueError('not a fixture of the season: %s' % spec)
            self.first, self.last = bounds(fields[-1])
            if self.fixtures and (self.last + 1) * self.fixtures > 1 << 32:
                raise ValueError('a season\'s seeds go past 32 bits: %s' % spec)
        elif kind == 'list':
            path = os.path.join(inputs, 'seeds.txt') if inputs else rest
            with open(path, 'rb') as file:
                data = file.read()
            if sha256 is not None and hashlib.sha256(data).hexdigest() != sha256:
                raise ValueError('%s does not match the plan\'s seeds_sha256' % path)
            self.list = [int(line) for line in data.decode('utf-8').split('\n') if line.strip()]
            self.first, self.last = 0, len(self.list) - 1
        else:
            raise ValueError('not a seed set: %s' % spec)
        if self.last < self.first:
            raise ValueError('an empty seed set: %s' % spec)
        self.count = self.last - self.first + 1

    def fixture(self, fixtures, number):
        """A season's seeds of fixture `number` (from 0) of `fixtures`."""
        return SeedSet('season:%d:%d:%d:%d-%d' % (self.key, fixtures, number, self.first,
                                                  self.last))

    def seed(self, index):
        if not self.first <= index <= self.last:
            raise IndexError('index %d is outside %s' % (index, self.spec))
        if self.spec.startswith('season:'):
            if self.fixtures is None:
                raise ValueError('a season\'s seeds are each fixture\'s: %s' % self.spec)
            return scatter(self.key, index * self.fixtures + self.fixture_number)
        if self.key is not None:
            return scatter(self.key, index)
        if self.list is not None:
            return self.list[index]
        return index


def bounds(text):
    first, _, last = text.partition('-')
    return int(first), int(last)


def main(arguments):
    if len(arguments) not in (1, 3):
        sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
        return 2
    seeds = SeedSet(arguments[0])
    first, last = (int(arguments[1]), int(arguments[2])) if len(arguments) == 3 else (
        seeds.first, seeds.last)
    out = sys.stdout
    out.write('index,seed\n')
    for index in range(first, last + 1):
        out.write('%d,%d\n' % (index, seeds.seed(index)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
