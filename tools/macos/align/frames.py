# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds an FM27 mapping's serialiser frames from FM24's, and checks them.

  python3 -I tools/macos/align/frames.py tools/macos/align/steam-27.0.0.2486716.json \\
      <FM24 fm.app or its executable> <FM27 fm.app or its game_plugin> [--write]

The alignment file names the FM27 mapping, the serialisers to align (`roots`, in FM24's arm64 slice
and both FM27 slices), function pairs the tool cannot find itself (`pairs`) and frames set by hand
(`fixes`). For the arm64 slice, the frames are derived from the FM24 mapping's arm64 frames (see
derive.py); for the x86_64 slice, from the arm64 slice's. Every frame on a traced write path that
neither gets a translation nor a fix gets a synthetic ID of its own (0x7E000000 + n, kept in the
mapping's `sites` with the frame it stands for), so that no FM27 write path holds an untranslated
frame and the IDs stay the same when the mapping is rebuilt.

Without --write it compares the result with the mapping's frames and sites and exits with 1 if they
differ. Either way it reports the replay (aligned writes whose chains differ from the source's,
other than through a fix), and how many of the FM24 sites the labels use the FM27 frames produce."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive  # noqa: E402
import macho  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..')
SYNTHETIC_BASE = 0x7E000000
SYNTHETIC_FRAMES = 0x7E001000  # IDs of frames of their own; named IDs lie below
SYNTHETIC_END = 0x7F000000
PLUGIN_IMAGE = 'Contents/PlugIns/game_plugin.bundle/Contents/MacOS/game_plugin'


def number(text):
    return int(text, 16)


def siteNames():
    """The sites src/dll/sites.h names: {name: Windows RVA}."""
    text = open(os.path.join(ROOT, 'src', 'dll', 'sites.h')).read()
    return {n: int(v, 16) for n, v in re.findall(r'\b(SITE_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)', text)}


def labelSites():
    """The Windows sites the harness's labels use (src/dll/sites.h, site_members.c, labels.c), with a
    description."""
    out = {}
    named = siteNames()
    for name, value in named.items():
        if name not in ('SITE_TACTIC_START', 'SITE_TACTIC_END'):
            out.setdefault(value, name)
    members = open(os.path.join(ROOT, 'src', 'dll', 'site_members.c')).read()
    for value, member in re.findall(r'\{(0x[0-9A-F]+), "([^"]+)"\}', members):
        out.setdefault(int(value, 16), member)
    labels = open(os.path.join(ROOT, 'src', 'dll', 'labels.c')).read()
    start = labels.index('pitchLandmarkSites[] = {')
    for index, value in enumerate(re.findall(r'0x[0-9A-F]+', labels[start:labels.index('};', start)])):
        out.setdefault(int(value, 16), f'pitch landmark {index}')
    stride = int(re.search(r'TACTIC_SLOT_CALL_STRIDE = (0x[0-9A-F]+)', labels).group(1), 16)
    for name, count in (('SITE_TACTIC_FORMATION_FIRST', 25), ('SITE_TACTIC_SLOTS_6920_FIRST', 4)):
        for index in range(count):
            out.setdefault(named[name] + index * stride, f'{name[:-6]}[{index}]')
    return out


class Sites:
    """The mapping's synthetic IDs: named ones (`name`, optionally a range of `count`) and those the
    tool gives frames of its own (`slice`, `mac`)."""

    def __init__(self, entries):
        self.entries = [dict(e) for e in entries]
        self.byName = {}
        self.byFrame = {}
        for e in self.entries:
            if 'name' in e:
                self.byName[e['name']] = (number(e['id']), e.get('count', 1))
            else:
                self.byFrame[(e['slice'], number(e['mac']))] = number(e['id'])
        self.next = max([number(e['id']) + 1 for e in self.entries if 'name' not in e] + [SYNTHETIC_FRAMES])
        self.used = set()

    def resolve(self, text):
        """A fix's Windows value: a hex RVA, a named synthetic ID or `NAME+index` within a range."""
        if text.startswith('0x'):
            return number(text)
        name, _, index = text.partition('+')
        if name not in self.byName:
            raise ValueError(f'the mapping\'s sites do not name {name}')
        base, count = self.byName[name]
        index = int(index or 0)
        if not 0 <= index < count:
            raise ValueError(f'{text} is outside {name}\'s {count} IDs')
        self.used.add(name)
        return base + index

    def forFrame(self, architecture, frame, note):
        key = (architecture, frame)
        if key not in self.byFrame:
            if self.next >= SYNTHETIC_END:
                raise ValueError('out of synthetic IDs')
            self.byFrame[key] = self.next
            self.entries.append({'id': f'0x{self.next:08X}', 'slice': architecture, 'mac': f'0x{frame:x}', 'note': note})
            self.next += 1
        self.used.add(key)
        return self.byFrame[key]

    def result(self):
        """The entries still in use, named ones first, in ID order."""
        keep = [e for e in self.entries
                if ('name' in e and e['name'] in self.used) or
                   ('name' not in e and (e['slice'], number(e['mac'])) in self.used)]
        return sorted(keep, key=lambda e: ('name' not in e, number(e['id'])))


def sourceTable(frames):
    table = {}
    for f in frames:
        key = (number(f['mac']), number(f['parent']) if f.get('parent') else 0,
               number(f['grandparent']) if f.get('grandparent') else 0)
        table[key] = tuple(number(w) for w in f['win'])
    return table


def traced(image, roots):
    """Every (kind, value, stack) the roots reach."""
    out = []
    for root in roots:
        out += derive.flatten(image, root)
    return out


def build(architecture, source, target, sourceFrames, roots, seeds, fixes, sites):
    """The target's table, the pairs and the replay failures."""
    pairs = derive.alignedPairs(source, target, roots)
    fpair = derive.functionPairs(pairs, source, target, seeds)
    pairs = derive.consistent(pairs, fpair, source, target)
    table, _ = derive.derive(pairs, sourceFrames, fpair, source, target)
    fixed = set()
    for fix in fixes:
        frame = number(fix['mac'])
        parent = number(fix['parent']) if fix.get('parent') else 0
        grand = number(fix['grandparent']) if fix.get('grandparent') else 0
        if not parent:
            for key in [k for k in table if k[0] == frame]:
                del table[key]
        table[(frame, parent, grand)] = tuple(sites.resolve(w) for w in fix['win'])
        fixed.add(frame)
    # Frames on a traced path with no translation get IDs of their own; the stream's write call
    # sites are innermost, the others callers.
    for kind, value, stack in traced(target, [r[2] for r in roots]):
        for k, frame in enumerate(stack):
            if derive.lookup(table, stack, k) is None:
                function = target.functionOf(frame)
                what = f'write ({value} bytes)' if kind == 'W' and k == len(stack) - 1 else 'call'
                note = f'FM27 only: {what} in 0x{function:x}'
                table[(frame, 0, 0)] = (sites.forFrame(architecture, frame, note),)
    failures = [p for p in derive.replay(pairs, sourceFrames, table) if not set(p[3]) & fixed]
    return table, pairs, failures, fixed


def entries(table, notes):
    out = []
    for (frame, parent, grand), wins in sorted(table.items()):
        entry = {'mac': f'0x{frame:x}'}
        if parent:
            entry['parent'] = f'0x{parent:x}'
        if grand:
            entry['grandparent'] = f'0x{grand:x}'
        entry['win'] = [f'0x{w:X}' for w in wins]
        entry['note'] = notes.get(frame, 'aligned')
        out.append(entry)
    return out


def report(name, table, failures, labels, roots, image):
    produced = {w for wins in table.values() for w in wins}
    covered = set(labels) & produced
    print(f'{name}: {len(table)} frames, {len(failures)} aligned writes replay differently, '
          f'{len(covered)} of the {len(labels)} label sites')
    for p in failures[:10]:
        print(f'  {p[0]}: {p[1]} {" ".join(hex(f) for f in p[3])}')
    # Every frame giving a label site is on a traced path.
    seen = set()
    for kind, value, stack in traced(image, roots):
        seen.update(stack)
    stray = sorted({k[0] for k, wins in table.items() if set(wins) & covered and k[0] not in seen})
    if stray:
        print(f'  label frames on no traced path: {" ".join(hex(f) for f in stray)}')
    return covered, not failures and not stray


def main(arguments):
    write = '--write' in arguments
    arguments = [a for a in arguments if a != '--write']
    if len(arguments) != 4:
        sys.exit(__doc__)
    config = json.load(open(arguments[1]))
    mappingPath = os.path.join(ROOT, config['mapping'])
    mapping = json.load(open(mappingPath))
    fm24Mapping = json.load(open(os.path.join(ROOT, config['source'])))
    sites = Sites(mapping.get('sites', ()))
    fm24 = macho.load(arguments[2], 'arm64', 'fm24')
    labels = labelSites()
    results = {}
    ok = True
    source, sourceFrames, sourceName = fm24, sourceTable(fm24Mapping['slices']['arm64']['frames']), 'fm24'
    for architecture in ('arm64', 'x86_64'):
        target = macho.load(arguments[3], architecture, 'fm27', PLUGIN_IMAGE)
        if target.uuid != mapping['slices'][architecture]['uuid']:
            sys.exit(f'{architecture}: {target.uuid} is not the mapping\'s build')
        roots = [(r['name'], number(r[sourceName]), number(r[architecture])) for r in config['roots']]
        seeds = {number(p['from']): number(p['to']) for p in config['pairs'].get(architecture, ())}
        fixes = config['fixes'].get(architecture, ())
        table, pairs, failures, fixed = build(architecture, source, target, sourceFrames, roots, seeds, fixes, sites)
        notes = {number(f['mac']): f['note'] for f in fixes}
        for e in sites.entries:
            if e.get('slice') == architecture:
                notes.setdefault(number(e['mac']), e['note'])
        covered, good = report(architecture, table, failures, labels, [r[2] for r in roots], target)
        ok = ok and good
        results[architecture] = entries(table, notes)
        source, sourceFrames, sourceName = target, table, architecture
    # Every label site the frames leave out is listed absent with a reason, and nothing else is.
    named = siteNames()
    absent = set()
    for entry in mapping.get('absent', ()):
        if entry.get('name', '').startswith('SITE_'):
            absent.add(named[entry['name']])
        elif 'site' in entry:
            absent.add(number(entry['site']))
        if ('site' in entry or entry.get('name', '').startswith('SITE_')) and not entry.get('reason'):
            print(f'  absent {entry} has no reason')
            ok = False
    missing = set(labels) - covered
    print(f'{len(missing)} label sites are absent from FM27')
    for m in sorted(missing - absent):
        print(f'  not listed absent: {m:#x} {labels[m]}')
    for m in sorted(absent - missing):
        print(f'  listed absent, but produced: {m:#x} {labels.get(m, "")}')
    ok = ok and missing == absent
    newSites = sites.result()
    if write:
        for architecture, frames in results.items():
            mapping['slices'][architecture]['frames'] = frames
        mapping['sites'] = newSites
        mapping.pop('sites_pending', None)
        with open(mappingPath, 'w') as file:
            json.dump(mapping, file, indent=1, ensure_ascii=False)
            file.write('\n')
        print(f'wrote {config["mapping"]}')
    else:
        same = all(mapping['slices'][a]['frames'] == f for a, f in results.items()) and mapping.get('sites') == newSites
        print('the mapping matches' if same else 'the mapping differs (rerun with --write)')
        ok = ok and same
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main(sys.argv)
