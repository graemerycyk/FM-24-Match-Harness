# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of remap.py: fingerprints and remapping, on two builds of a small bundle made here with
clang++ for both slices, the second of which moves code, fields, a vtable slot, a global and the
format version; and of the fingerprints kept for each mapped build.

  python3 -I -m unittest discover -s tools/macos/align -p 'test_*.py'

With FM27_GAME_PLUGIN naming the archived game_plugin of 27.0.0.2486716, it also remaps that build's
mapping onto itself, which must give the mapping back."""
import copy
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

ALIGN = os.path.dirname(os.path.abspath(__file__))
MACOS = os.path.join(ALIGN, '..')
sys.path.insert(0, ALIGN)
sys.path.insert(1, MACOS)
import check_mapping  # noqa: E402
import fingerprint  # noqa: E402
import macho  # noqa: E402
import remap  # noqa: E402

ARCHITECTURES = ('arm64', 'x86_64')
SOURCE = r'''
#include <stdint.h>
#define KEEP __attribute__((noinline, used))

struct Stream {
	virtual ~Stream();
	virtual void open();
	virtual void close();
	virtual void flush();
	virtual void seek(long);
	virtual void write(const void *data, uint32_t length); // at +0x30, as the game's stream's
};

struct Player { uint32_t id; uint8_t ratings[4]; uint16_t condition; };
struct Team {
	uint32_t id;
#if NEW
	uint32_t reputation;
#endif
	Player players[3];
	uint8_t count;
};
struct Options {
	uint64_t flags;
#if NEW
	uint64_t version;
#endif
	uint8_t inProgress;
};

static const uint16_t formatVersion = FORMAT;

struct Referee {
	virtual ~Referee();
	virtual void serialize(Stream *s);
	uint32_t id;
};

struct Match {
	virtual ~Match();
	virtual void serialize(Stream *s);
#if NEW
	virtual void reset();
#endif
	virtual int run(int ticks);
	uint32_t seed;
#if NEW
	uint32_t weather;
#endif
	uint32_t clock;
	Team home, away;
};

Options options;
Match *current;

#if NEW
KEEP int added(volatile int *p) { return p[0] * 3 + p[1] * 5 + p[2]; }
#endif
FILLERS_BEFORE

KEEP void writePlayer(Stream *s, const Player &p) {
	s->write(&p.id, 4);
	s->write(p.ratings, 4);
	s->write(&p.condition, 2);
}

KEEP void writeTeam(Stream *s, const Team &t) {
	s->write(&t.id, 4);
#if NEW
	s->write(&t.reputation, 4);
#endif
	for (int i = 0; i < 3; ++i) {
		writePlayer(s, t.players[i]);
	}
	s->write(&t.count, 1);
}

KEEP void writeHeader(Stream *s, const Match &m) {
#if NEW
	s->write(&m.weather, 4);
#endif
	s->write(&formatVersion, 2);
	s->write(&m.seed, 4);
}

Referee::~Referee() {}
void Referee::serialize(Stream *s) { s->write(&id, 4); }

Match::~Match() {}

void Match::serialize(Stream *s) {
	writeHeader(s, *this);
	s->write(&clock, 4);
	writeTeam(s, home);
	writeTeam(s, away);
}

#if NEW
void Match::reset() { clock = 0; }
#endif

int Match::run(int ticks) {
	clock += (uint32_t)ticks;
	return (int)clock;
}

KEEP Match *currentMatch() { return current; }
KEEP bool inProgress() { return options.inProgress != 0; }
KEEP void clearOptions() { options.flags = 0; }
KEEP Match *makeMatch() {
	Match *match = new Match();
	current = match;
	return match;
}
FILLERS_MIDDLE
KEEP const char *describe(int x) { return x ? "remap test: a string only describe uses" : "remap test: none"; }
FILLERS_AFTER
'''
SYMBOLS = {
    'writePlayer': '__Z11writePlayerP6StreamRK6Player', 'writeTeam': '__Z9writeTeamP6StreamRK4Team',
    'writeHeader': '__Z11writeHeaderP6StreamRK5Match', 'serialize': '__ZN5Match9serializeEP6Stream',
    'run': '__ZN5Match3runEi', 'makeMatch': '__Z9makeMatchv', 'currentMatch': '__Z12currentMatchv',
    'inProgress': '__Z10inProgressv', 'clearOptions': '__Z12clearOptionsv', 'describe': '__Z8describei',
    'current': '_current', 'options': '_options', 'vtable': '__ZTV5Match'}


def fillers(name, sizes):
    """Functions of distinct sizes (each loads `n` values), which give the others neighbours."""
    out = []
    for index, n in enumerate(sizes):
        terms = ' + '.join(f'p[{k}] * {k + 2}' for k in range(n))
        out.append(f'KEEP int {name}{index}(volatile int *p) {{ return {terms}; }}')
    return '\n'.join(out)


def build(folder, name, new):
    source = SOURCE.replace('FILLERS_BEFORE', fillers('before', range(2, 12)))
    source = source.replace('FILLERS_MIDDLE', fillers('middle', (13, 7, 9) if new else (12, 8, 10)))
    source = source.replace('FILLERS_AFTER', fillers('after', (11, 5, 14) if new else (6, 15, 3)))
    path = os.path.join(folder, f'{name}.cpp')
    with open(path, 'w') as file:
        file.write(source)
    out = os.path.join(folder, f'{name}.bundle')
    command = ['xcrun', 'clang++', '-std=c++17', '-O2', '-fno-omit-frame-pointer', '-fvisibility=hidden', '-bundle',
               '-arch', 'arm64', '-arch', 'x86_64', '-mmacosx-version-min=13.0', f'-DNEW={int(new)}',
               f'-DFORMAT={"0x4d" if new else "0x4c"}', '-o', out, path]
    subprocess.run(command, check=True, capture_output=True)
    return out


def symbols(path):
    out = {}
    for architecture in ARCHITECTURES:
        text = subprocess.run(['xcrun', 'nm', '-arch', architecture, path], check=True, capture_output=True,
                              text=True).stdout
        table = {}
        for line in text.splitlines():
            parts = line.split()
            if len(parts) == 3:
                table[parts[2]] = int(parts[0], 16)
        out[architecture] = {key: table[name] for key, name in SYMBOLS.items()}
        out[architecture]['vtable'] += 16  # the address point, after the offset to top and the RTTI
    return out


def hexText(value):
    return f'0x{value:x}'


def immediateSite(binary, function, value):
    """The first instruction of a function that encodes an immediate."""
    for address in binary.analyse(function).addresses:
        found = binary.slice.operands(address)
        if found and value in found['imms']:
            return address
    raise AssertionError(f'no instruction of {hexText(function)} encodes {hexText(value)}')


def targetSite(binary, function, target):
    for address in binary.analyse(function).addresses:
        found = binary.slice.operands(address)
        if found and found['target'] == target:
            return address
    raise AssertionError(f'no instruction of {hexText(function)} reaches {hexText(target)}')


SERIALISERS = ('writePlayer', 'writeTeam', 'writeHeader', 'serialize')


def makeMapping(path, table):
    """A mapping of the first build, as a mapping of a game build would hold it."""
    mapping = {'build': 'the remap test\'s first build', 'edition': 27, 'image': 'test.bundle',
               'image_base': '0x0', 'version_text': '27.0.0.1', 'game_build': 1, 'format_version': '0x004c',
               'absent': [], 'notes': {}, 'slices': {}, 'layouts': [], 'sites': [
                   {'name': 'SITE27_TEST', 'id': '0x7E000001', 'note': 'a named site'}]}
    clock = {}
    inProgress = {}
    slot = {}
    for architecture in ARCHITECTURES:
        binary = fingerprint.Binary(macho.load(path, architecture, 'first'))
        s = table[architecture]
        addresses = [
            ('RVA_GAME_MATCH_CREATE', '0x1A8FAF0', s['makeMatch']), ('RVA_GAME_MATCH_SERIALIZE', '0x1AC76A0', s['serialize']),
            ('RVA_GAME_MATCH_RUN', '0x1A98930', s['run']), ('RVA_MACOS_PERSON_FORENAME', '0x7F000002', s['describe']),
            ('RVA_MACOS_THREAD_RNG', '0x7F000001', s['currentMatch']), ('RVA_GAME_MATCH_VTABLE', '0x56083C8', s['vtable']),
            ('RVA_MACOS_GAME', '0x7F000003', s['current']), ('RVA_MACOS_GAME_OPTIONS', '0x7F000005', s['options'])]
        signatures = [{'name': name, 'mac': hexText(s[key]), 'bytes': binary.read(s[key], 16).hex()}
                      for name, key in (('RVA_GAME_MATCH_CREATE', 'makeMatch'), ('RVA_GAME_MATCH_SERIALIZE', 'serialize'),
                                        ('RVA_GAME_MATCH_RUN', 'run'), ('stream write', 'writeHeader'))]
        frames = []
        for n, key in enumerate(SERIALISERS):
            for k, event in enumerate(binary.analyse(s[key]).events):
                if event[2] is not None:
                    frames.append({'mac': hexText(event[2]), 'win': [hexText(0x1000 * (n + 1) + k)], 'note': key})
        team = binary.analyse(s['writeTeam']).events
        call = next(e[2] for e in team if e[0] == 'C' and e[1] == s['writePlayer'])
        first = binary.analyse(s['writePlayer']).events[0][2]
        frames.append({'mac': hexText(first), 'parent': hexText(call), 'win': ['0x7E000001'], 'note': 'in a team'})
        frames.sort(key=lambda f: (int(f['mac'], 16), int(f.get('parent', '0x0'), 16)))
        own = binary.analyse(s['serialize']).events[0][2]
        mapping['sites'].append({'id': '0x7E001000' if architecture == 'arm64' else '0x7E001001',
                                 'slice': architecture, 'mac': hexText(own), 'note': 'a frame of its own'})
        evidence = {
            'RVA_GAME_MATCH_VTABLE': [{'rtti': '5Match', 'offset_to_top': '0x0'}],
            'RVA_MACOS_GAME': [{'at': hexText(targetSite(binary, s['currentMatch'], s['current']))}],
            'RVA_MACOS_GAME_OPTIONS': [{'at': hexText(targetSite(binary, s['clearOptions'], s['options']))}]}
        mapping['slices'][architecture] = {
            'uuid': binary.image.uuid, 'signatures': signatures, 'frames': frames,
            'addresses': [dict({'name': n, 'windows': w, 'mac': hexText(a)},
                               **({'evidence': evidence[n]} if n in evidence else {})) for n, w, a in addresses]}
        clock[architecture] = [{'at': hexText(immediateSite(binary, s['run'], 0xC)), 'imm': '0xc'}]
        inProgress[architecture] = [{'at': hexText(targetSite(binary, s['inProgress'], s['options'] + 8)),
                                     'target': hexText(s['options'] + 8)}]
        slot[architecture] = [{'vtable': hexText(s['vtable']), 'slot_offset': '0x18', 'points_to': hexText(s['run'])}]
    mapping['layouts'] = [
        {'name': 'MATCH_CLOCK', 'value': '0xc', 'confidence': 'high', 'evidence': clock},
        {'name': 'GAME_OPTIONS_IN_PROGRESS', 'value': '0x8', 'confidence': 'high', 'evidence': inProgress,
         'relation': 'target-minus:RVA_MACOS_GAME_OPTIONS'},
        {'name': 'RUN_SLOT', 'value': '0x18', 'confidence': 'high', 'evidence': slot, 'relation': 'slot-offset'},
        {'name': 'NOT_CHECKED', 'value': '0x1', 'confidence': 'unverified', 'evidence': {}}]
    return mapping


class RemapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('xcrun'):
            raise unittest.SkipTest('needs Xcode or its Command Line Tools')
        cls.folder = tempfile.mkdtemp(prefix='fm-remap-test-')
        cls.first = build(cls.folder, 'first', False)
        second = build(cls.folder, 'second', True)
        cls.second = os.path.join(cls.folder, 'second-stripped.bundle')
        subprocess.run(['xcrun', 'strip', '-x', '-o', cls.second, second], check=True, capture_output=True)
        cls.firstSymbols = symbols(cls.first)
        cls.secondSymbols = symbols(second)
        cls.mapping = makeMapping(cls.first, cls.firstSymbols)
        cls.mappingPath = os.path.join(cls.folder, 'steam-27.0.0.1.json')
        with open(cls.mappingPath, 'w') as file:
            json.dump(cls.mapping, file)
        cls.fingerprints = os.path.join(cls.folder, 'fingerprints-27.0.0.1.json')
        cls.invoke(['--fingerprint', cls.mappingPath, cls.first, '--write', cls.fingerprints])
        cls.out = os.path.join(cls.folder, 'steam-27.0.1.2.json')
        cls.code, cls.report = cls.invoke([cls.mappingPath, cls.fingerprints, cls.second, '--version', '27.0.1.2',
                                        '--out', cls.out])
        with open(cls.out) as file:
            cls.new = json.load(file)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, True)

    @staticmethod
    def invoke(arguments):
        output = io.StringIO()
        with redirect_stdout(output):
            code = remap.main(['remap.py'] + list(arguments))
        if code == 2:
            raise AssertionError(f'remap.py {" ".join(arguments)} failed:\n{output.getvalue()}')
        return code, output.getvalue()

    def binary(self, architecture):
        return fingerprint.Binary(macho.load(self.second, architecture, 'second'))

    def test_keeps_no_code_in_the_fingerprints(self):
        with open(self.fingerprints) as file:
            text = file.read()
        with open(self.first, 'rb') as file:
            image = file.read()
        for architecture in ARCHITECTURES:
            binary = fingerprint.Binary(macho.load(self.first, architecture, 'first'))
            function = self.firstSymbols[architecture]['serialize']
            code = binary.read(function, 16).hex()
            self.assertNotIn(code, text)
        self.assertIn(b'remap test: a string', image)
        self.assertNotIn('remap test', text)

    def test_finds_the_functions_and_globals_that_moved(self):
        self.assertEqual(self.code, 1, self.report)  # some things changed, which need review
        for architecture in ARCHITECTURES:
            s = self.secondSymbols[architecture]
            slice_ = self.new['slices'][architecture]
            self.assertEqual(slice_['uuid'], self.binary(architecture).image.uuid)
            found = {e['name']: int(e['mac'], 16) for e in slice_['addresses']}
            self.assertEqual(found, {
                'RVA_GAME_MATCH_CREATE': s['makeMatch'], 'RVA_GAME_MATCH_SERIALIZE': s['serialize'],
                'RVA_GAME_MATCH_RUN': s['run'], 'RVA_MACOS_PERSON_FORENAME': s['describe'],
                'RVA_MACOS_THREAD_RNG': s['currentMatch'], 'RVA_GAME_MATCH_VTABLE': s['vtable'],
                'RVA_MACOS_GAME': s['current'], 'RVA_MACOS_GAME_OPTIONS': s['options']}, architecture)
            self.assertNotEqual(s['serialize'], self.firstSymbols[architecture]['serialize'])
            binary = self.binary(architecture)
            for entry in slice_['signatures']:
                self.assertEqual(binary.read(int(entry['mac'], 16), 16).hex(), entry['bytes'])

    def test_moves_frames_with_their_calls(self):
        for architecture in ARCHITECTURES:
            s = self.secondSymbols[architecture]
            binary = self.binary(architecture)
            expected = []
            for n, key in enumerate(SERIALISERS):
                events = [e for e in binary.analyse(s[key]).events if e[2] is not None]
                if key == 'writeTeam':
                    events = events[:1] + events[2:]  # the second build's added writes have no frames
                elif key == 'writeHeader':
                    events = events[1:]
                for k, event in enumerate(events):
                    expected.append((hexText(event[2]), '', hexText(0x1000 * (n + 1) + k)))
            team = binary.analyse(s['writeTeam']).events
            call = next(e[2] for e in team if e[0] == 'C' and e[1] == s['writePlayer'])
            expected.append((hexText(binary.analyse(s['writePlayer']).events[0][2]), hexText(call), '0x7E000001'))
            frames = [(f['mac'], f.get('parent', ''), f['win'][0]) for f in self.new['slices'][architecture]['frames']]
            self.assertEqual(sorted(frames), sorted(expected), architecture)
            own = next(site for site in self.new['sites'] if site.get('slice') == architecture)
            self.assertEqual(own['mac'], hexText(binary.analyse(s['serialize']).events[0][2]))
        self.assertIn('function', self.report)
        self.assertIn('changed (by call', self.report)

    def test_reads_the_layouts_and_format_version_again(self):
        layouts = {layout['name']: layout for layout in self.new['layouts']}
        self.assertEqual(layouts['MATCH_CLOCK']['value'], '0x10')
        self.assertEqual(layouts['GAME_OPTIONS_IN_PROGRESS']['value'], '0x10')
        self.assertEqual(layouts['RUN_SLOT']['value'], '0x20')
        self.assertEqual(layouts['NOT_CHECKED'], self.mapping['layouts'][3])
        self.assertEqual(self.new['format_version'], '0x004d')
        self.assertEqual((self.new['version_text'], self.new['game_build']), ('27.0.1.2', 2))
        for text in ('layout MATCH_CLOCK changed: 0xc -> 0x10', 'layout RUN_SLOT changed: 0x18 -> 0x20',
                     'layout GAME_OPTIONS_IN_PROGRESS changed: 0x8 -> 0x10', 'format version changed: 0x004c -> 0x004d'):
            self.assertIn(text, self.report)
        # check_mapping.py finds nothing wrong with the new mapping in the new build.
        for image in check_mapping.slices(self.second, None):
            self.assertEqual(check_mapping.checkSlice(image, self.new, check_mapping.dataNames()), [])

    def test_carries_the_evidence_of_globals_over(self):
        for architecture in ARCHITECTURES:
            s = self.secondSymbols[architecture]
            binary = self.binary(architecture)
            evidence = {e['name']: e.get('evidence') for e in self.new['slices'][architecture]['addresses']}
            self.assertEqual(evidence['RVA_MACOS_GAME'], [{'at': hexText(targetSite(binary, s['currentMatch'], s['current']))}])
            self.assertEqual(evidence['RVA_MACOS_GAME_OPTIONS'],
                             [{'at': hexText(targetSite(binary, s['clearOptions'], s['options']))}])
            self.assertEqual(evidence['RVA_GAME_MATCH_VTABLE'], [{'rtti': '5Match', 'offset_to_top': '0x0'}])
            self.assertNotEqual(evidence['RVA_MACOS_GAME'],
                                {e['name']: e.get('evidence') for e in self.mapping['slices'][architecture]['addresses']}[
                                    'RVA_MACOS_GAME'])

    def test_checks_each_global_by_its_evidence(self):
        data = check_mapping.dataNames()

        def problems(change):
            mapping = copy.deepcopy(self.mapping)
            for architecture in ARCHITECTURES:
                change({e['name']: e for e in mapping['slices'][architecture]['addresses']})
            return [p for image in check_mapping.slices(self.first, None)
                    for p in check_mapping.checkSlice(image, mapping, data)]

        self.assertEqual(problems(lambda entries: None), [])

        def move(name):
            def change(entries):
                entries[name]['mac'] = hexText(int(entries[name]['mac'], 16) + 8)
            return change
        found = problems(move('RVA_MACOS_GAME'))
        self.assertEqual(len(found), 2, found)
        self.assertTrue(all(p.startswith('RVA_MACOS_GAME: the instruction at ') for p in found), found)
        found = problems(move('RVA_GAME_MATCH_VTABLE'))
        self.assertEqual(len(found), 2, found)
        self.assertTrue(all('is not the vtable of 5Match (offset to top 0x0)' in p for p in found), found)

        def rename(entries):
            entries['RVA_GAME_MATCH_VTABLE']['evidence'] = [{'rtti': '7Referee', 'offset_to_top': '0x0'}]
        found = problems(rename)
        self.assertTrue(len(found) == 2 and all(p.endswith(': 5Match, offset to top 0x0') for p in found), found)

        def drop(entries):
            del entries['RVA_MACOS_GAME_OPTIONS']['evidence']
        found = problems(drop)
        self.assertTrue(len(found) == 2 and all('RVA_MACOS_GAME_OPTIONS' in p and p.endswith('has no evidence')
                                                for p in found), found)
        # A mapping without layouts (FM24's) may leave its globals without evidence.
        mapping = copy.deepcopy(self.mapping)
        del mapping['layouts']
        for architecture in ARCHITECTURES:
            for entry in mapping['slices'][architecture]['addresses']:
                entry.pop('evidence', None)
        for image in check_mapping.slices(self.first, None):
            self.assertEqual(check_mapping.checkSlice(image, mapping, data), [])

    def test_gives_the_mapping_back_for_the_same_build(self):
        out = os.path.join(self.folder, 'same.json')
        code, report = self.invoke([self.mappingPath, self.fingerprints, self.first, '--version', '27.0.0.1',
                                 '--out', out])
        self.assertEqual(code, 0, report)
        self.assertIn('nothing needs review', report)
        with open(out) as file:
            same = json.load(file)
        expected = copy.deepcopy(self.mapping)
        del same['build'], expected['build']
        self.assertEqual(same, expected)

    def test_reports_what_it_cannot_find(self):
        with open(self.fingerprints) as file:
            fingerprints = json.load(file)
        serialize = hexText(self.firstSymbols['arm64']['serialize'])
        entry = fingerprints['slices']['arm64']['functions'][serialize]
        entry.update(vtables=[], size=entry['size'] + 4, before=[1] * 8, after=[1] * 8, hash='0' * 16, sketch=[1])
        path = os.path.join(self.folder, 'fingerprints-broken.json')
        with open(path, 'w') as file:
            json.dump(fingerprints, file)
        out = os.path.join(self.folder, 'broken.json')
        code, report = self.invoke([self.mappingPath, path, self.second, '--version', '27.0.1.2', '--out', out])
        self.assertEqual(code, 1)
        self.assertIn(f'arm64: function {serialize} not found', report)
        self.assertIn('arm64: RVA_GAME_MATCH_SERIALIZE', report)
        with open(out) as file:
            broken = json.load(file)
        names = [e['name'] for e in broken['slices']['arm64']['addresses']]
        self.assertNotIn('RVA_GAME_MATCH_SERIALIZE', names)

    def remapWith(self, change, name):
        """Remaps onto the second build with fingerprints that `change(fingerprints)` changed."""
        with open(self.fingerprints) as file:
            fingerprints = json.load(file)
        change(fingerprints)
        path = os.path.join(self.folder, f'fingerprints-{name}.json')
        with open(path, 'w') as file:
            json.dump(fingerprints, file)
        out = os.path.join(self.folder, f'{name}.json')
        code, report = self.invoke([self.mappingPath, path, self.second, '--version', '27.0.1.2', '--out', out])
        with open(out) as file:
            mapping = json.load(file)
        return code, report, {a: {e['name']: int(e['mac'], 16) for e in mapping['slices'][a]['addresses']}
                              for a in ARCHITECTURES}

    def test_finds_a_virtual_function_in_another_slot(self):
        def change(fingerprints):
            for architecture in ARCHITECTURES:
                entry = fingerprints['slices'][architecture]['functions'][hexText(self.firstSymbols[architecture]['run'])]
                self.assertTrue(entry['vtables'])
                entry.update(before=[1] * 8, after=[1] * 8)  # only its vtable slot finds it
        _, report, found = self.remapWith(change, 'slot')
        for architecture in ARCHITECTURES:
            self.assertEqual(found[architecture]['RVA_GAME_MATCH_RUN'], self.secondSymbols[architecture]['run'])
        self.assertEqual(report.count('by vtable'), 2, report)

    def test_leaves_out_a_function_whose_code_is_unlike_its_fingerprint(self):
        # Found by its place, and (describe) by the strings it uses, but with other code.
        def change(fingerprints):
            for architecture in ARCHITECTURES:
                functions = fingerprints['slices'][architecture]['functions']
                functions[hexText(self.firstSymbols[architecture]['currentMatch'])].update(
                    hash='0' * 16, sketch=[1, 2, 3])
                functions[hexText(self.firstSymbols[architecture]['describe'])].update(
                    hash='0' * 16, sketch=[1, 2, 3], before=[1] * 8, after=[1] * 8)
        code, report, found = self.remapWith(change, 'unlike')
        self.assertEqual(code, 1)
        for architecture in ARCHITECTURES:
            for key, name in (('currentMatch', 'RVA_MACOS_THREAD_RNG'), ('describe', 'RVA_MACOS_PERSON_FORENAME')):
                self.assertNotIn(name, found[architecture])
                function = hexText(self.firstSymbols[architecture][key])
                self.assertIn(f'{architecture}: function {function} not found: {name}', report)

    def test_finds_a_function_by_the_strings_it_uses(self):
        with open(self.fingerprints) as file:
            fingerprints = json.load(file)
        for architecture in ARCHITECTURES:
            describe = hexText(self.firstSymbols[architecture]['describe'])
            entry = fingerprints['slices'][architecture]['functions'][describe]
            self.assertEqual(len(entry['strings']), 2)
            entry.update(before=[1] * 8, after=[1] * 8)  # its neighbours no longer place it
        path = os.path.join(self.folder, 'fingerprints-strings.json')
        with open(path, 'w') as file:
            json.dump(fingerprints, file)
        out = os.path.join(self.folder, 'strings.json')
        _, report = self.invoke([self.mappingPath, path, self.second, '--version', '27.0.1.2', '--out', out])
        with open(out) as file:
            found = json.load(file)
        for architecture in ARCHITECTURES:
            addresses = {e['name']: int(e['mac'], 16) for e in found['slices'][architecture]['addresses']}
            self.assertEqual(addresses['RVA_MACOS_PERSON_FORENAME'], self.secondSymbols[architecture]['describe'])
        self.assertEqual(report.count('1 by string'), 2, report)

    def test_refuses_fingerprints_of_another_mapping(self):
        with open(self.fingerprints) as file:
            fingerprints = json.load(file)
        fingerprints['version_text'] = '27.0.0.9'
        path = os.path.join(self.folder, 'fingerprints-other.json')
        with open(path, 'w') as file:
            json.dump(fingerprints, file)
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = remap.main(['remap.py', self.mappingPath, path, self.second, '--version', '27.0.1.2',
                               '--out', os.path.join(self.folder, 'other.json')])
        self.assertEqual(code, 2)
        self.assertIn('is not the fingerprints of', output.getvalue())


class CommittedFingerprintsTest(unittest.TestCase):
    """Each FM27 mapping has fingerprints that cover what it names."""

    def test_cover_their_mappings(self):
        for name in sorted(os.listdir(MACOS)):
            if not (name.startswith('steam-') and name.endswith('.json')):
                continue
            with open(os.path.join(MACOS, name)) as file:
                mapping = json.load(file)
            if mapping.get('edition') != 27:
                continue
            path = os.path.join(ALIGN, f'fingerprints-{mapping["version_text"]}.json')
            self.assertTrue(os.path.exists(path), f'{name} has no {os.path.basename(path)}')
            with open(path) as file:
                fingerprints = json.load(file)
            self.assertEqual((fingerprints['format'], fingerprints['mapping'], fingerprints['format_version']),
                             (remap.FORMAT, name, mapping['format_version']))
            data = check_mapping.dataNames()
            for architecture, slice_ in mapping['slices'].items():
                fp = fingerprints['slices'][architecture]
                self.assertEqual(fp['uuid'], slice_['uuid'])
                functions = {int(a, 16) for a in fp['functions']}
                sites = {int(a, 16) for a in fp['sites']}
                for entry in slice_['addresses']:
                    if entry['name'] in data:
                        self.assertIn(entry['name'], fp['data'])
                        self.assertTrue(entry.get('evidence'), entry['name'])
                        for item in entry['evidence']:
                            if 'at' in item:
                                self.assertIn(int(item['at'], 16), sites, entry['name'])
                    else:
                        self.assertIn(int(entry['mac'], 16), functions, entry['name'])
                for entry in slice_['signatures']:
                    self.assertIn(int(entry['mac'], 16), functions, entry['name'])
                returns = {int(a, 16) for a in fp['returns']}
                for frame in slice_['frames']:
                    for key in ('mac', 'parent', 'grandparent'):
                        if frame.get(key):
                            self.assertIn(int(frame[key], 16), returns)
                for site in mapping.get('sites', ()):
                    if site.get('slice') == architecture:
                        self.assertIn(int(site['mac'], 16), returns)
                for layout in mapping.get('layouts', ()):
                    for item in layout['evidence'].get(architecture, ()):
                        if 'at' in item:
                            self.assertIn(int(item['at'], 16), sites, layout['name'])
                        else:
                            self.assertIn(item['vtable'], fp['vtables'], layout['name'])
                self.assertIn(fp['format_version']['site'], fp['sites'])
                for entry in fp['functions'].values():
                    self.assertLessEqual(len(entry['sketch']), fingerprint.SKETCH)

    @unittest.skipUnless(os.environ.get('FM27_GAME_PLUGIN'), 'FM27_GAME_PLUGIN names no archived game_plugin')
    def test_show_each_global_moved_out_of_place(self):
        # The serialisation context, which every serialisation and deserialisation takes, moved by 8.
        with open(os.path.join(MACOS, 'steam-27.0.0.2486716.json')) as file:
            mapping = json.load(file)
        data = check_mapping.dataNames()
        for image in check_mapping.slices(os.environ['FM27_GAME_PLUGIN'], None):
            self.assertEqual(check_mapping.checkData(image, mapping, mapping['slices'][image.architecture], data), [])
        for slice_ in mapping['slices'].values():
            for entry in slice_['addresses']:
                if entry['name'] == 'RVA_MACOS_MATCH_CONTEXT':
                    entry['mac'] = hexText(int(entry['mac'], 16) + 8)
        for image in check_mapping.slices(os.environ['FM27_GAME_PLUGIN'], None):
            found = check_mapping.checkData(image, mapping, mapping['slices'][image.architecture], data)
            self.assertTrue(found and all(p.startswith('RVA_MACOS_MATCH_CONTEXT: ') for p in found), found)

    @unittest.skipUnless(os.environ.get('FM27_GAME_PLUGIN'), 'FM27_GAME_PLUGIN names no archived game_plugin')
    def test_give_their_mapping_back_for_the_same_build(self):
        name = 'steam-27.0.0.2486716.json'
        with tempfile.TemporaryDirectory() as folder:
            out = os.path.join(folder, 'same.json')
            output = io.StringIO()
            with redirect_stdout(output):
                code = remap.main(['remap.py', os.path.join(MACOS, name),
                                   os.path.join(ALIGN, 'fingerprints-27.0.0.2486716.json'),
                                   os.environ['FM27_GAME_PLUGIN'], '--version', '27.0.0.2486716', '--out', out])
            self.assertEqual(code, 0, output.getvalue())
            with open(out) as file:
                same = json.load(file)
        with open(os.path.join(MACOS, name)) as file:
            mapping = json.load(file)
        del same['build'], mapping['build']
        self.assertEqual(same, mapping)


if __name__ == '__main__':
    unittest.main()
