# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Carries a build mapping over to another build of the game, and makes the fingerprints it needs.

  python3 -I tools/macos/remap.py --fingerprint tools/macos/steam-27.0.0.2486716.json \\
      <the build's fm.app or its engine image> [--write <fingerprints>]
  python3 -I tools/macos/remap.py tools/macos/steam-27.0.0.2486716.json \\
      tools/macos/align/fingerprints-27.0.0.2486716.json <the new build's fm.app or engine image> \\
      [--version <version text>] [--out <new mapping>] [--report <file>]

--fingerprint reads a mapped build and writes its fingerprints (align/fingerprint.py), by default to
tools/macos/align/fingerprints-<version>.json: no code, only hashes and structure. Without --write it
checks that the existing file is what it would write.

The second form finds everything the mapping names in the new build and writes the new mapping, by
default tools/macos/steam-<version>.json (the version comes from the app's Info.plist, or
--version for an image on its own), with a report on standard output (and in --report's file). For
each slice it matches, in this order:
1. vtables, by their classes' RTTI names;
2. functions: by the vtable slots that point to them, by the sizes of the functions around them,
   then through the calls of functions already matched, then by the strings they use, each checked
   against the function's fingerprint (the same masked code, or how alike it is);
3. globals, through the instructions that reach them;
4. frames, by aligning each function's calls and writes with its fingerprint's;
5. layouts, by reading each evidence instruction again (a value that changed is reported);
6. signatures, read again from the new build;
7. the format version, read from the constant the match header's writer writes.
Anything not matched one to one, and every changed value, is listed under "needs review"; the exit
code is then 1 (0 when there is nothing to review, 2 for a usage or input error). Then check the new
mapping with check_mapping.py, add the build to src/common/macos_builds.c, generate the tables, run
the tests, and make the new build's fingerprints with --fingerprint (see README.md)."""
import copy
import difflib
import json
import os
import plistlib
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, 'align'))
import check_mapping  # noqa: E402
import fingerprint  # noqa: E402
import macho  # noqa: E402

FORMAT = 'fm-fingerprints/1'
GAMES = {24: ('Football Manager 2024', '2252570'), 27: ('Football Manager 27', '4530370')}
ACCEPT = 0.6        # how alike a function found by position or string must be, if not the same code
CONTEXT_MIN = 6     # the least score (out of 18) a position needs
CONTEXT_MARGIN = 0.1  # how much more alike the best of two equally placed functions must be
SIZE_LIMIT = 4000   # sizes shared by more functions than this are not searched by size alone
METHODS = {'vtable': 3, 'call': 2, 'position': 1, 'string': 1}


def hexText(value):
    return f'-0x{-value:x}' if value < 0 else f'0x{value:x}'


def number(text):
    return fingerprint.number(text)


def imagePath(path, mapping):
    """The engine image of an app (the mapping's `image`, or Contents/MacOS/fm), or the path itself."""
    path = os.path.expanduser(path)
    if path.rstrip('/').endswith('.app'):
        return os.path.join(path, mapping.get('image', 'Contents/MacOS/fm'))
    return path


def appVersion(path, mapping):
    """The version text of the app holding an image (CFBundleShortVersionString), or None."""
    path = os.path.abspath(os.path.expanduser(path))
    if not path.rstrip('/').endswith('.app'):
        depth = len(mapping.get('image', 'Contents/MacOS/fm').split('/'))
        for _ in range(depth):
            path = os.path.dirname(path)
        if not path.endswith('.app'):
            return None
    try:
        with open(os.path.join(path, 'Contents', 'Info.plist'), 'rb') as file:
            return plistlib.load(file).get('CFBundleShortVersionString')
    except (OSError, plistlib.InvalidFileException):
        return None


def loadSlices(path, mapping):
    out = {}
    for architecture in mapping['slices']:
        out[architecture] = fingerprint.Binary(macho.load(imagePath(path, mapping), architecture, 'remap'))
    return out


def compact(document):
    """JSON with one line per function, return address, site or datum: diffable, and a few MB at most."""
    lines = ['{']
    keys = [k for k in document if k != 'slices']
    for key in keys:
        lines.append(f' {json.dumps(key)}: {json.dumps(document[key])},')
    lines.append(' "slices": {')
    slices = list(document['slices'].items())
    for n, (architecture, data) in enumerate(slices):
        lines.append(f'  {json.dumps(architecture)}: {{')
        parts = list(data.items())
        for m, (key, value) in enumerate(parts):
            end = ',' if m + 1 < len(parts) else ''
            if isinstance(value, dict) and value and all(isinstance(v, (dict, list)) for v in value.values()):
                lines.append(f'   {json.dumps(key)}: {{')
                items = list(value.items())
                for k, (name, entry) in enumerate(items):
                    comma = ',' if k + 1 < len(items) else ''
                    lines.append(f'    {json.dumps(name)}: {json.dumps(entry, separators=(",", ":"))}{comma}')
                lines.append(f'   }}{end}')
            else:
                lines.append(f'   {json.dumps(key)}: {json.dumps(value, separators=(",", ":"))}{end}')
        lines.append('  }' + (',' if n + 1 < len(slices) else ''))
    lines.append(' }')
    lines.append('}')
    return '\n'.join(lines) + '\n'


# Fingerprints


def fingerprintSlice(binary, mapping, data):
    """The fingerprints of one slice of a mapped build."""
    architecture = binary.architecture
    slice_ = mapping['slices'][architecture]
    if binary.image.uuid != slice_['uuid']:
        raise ValueError(f'{architecture}: {binary.image.uuid} is not the mapping\'s build ({slice_["uuid"]})')
    roles = {}

    def note(function, role):
        roles.setdefault(function, [])
        if role not in roles[function]:
            roles[function].append(role)

    def functionOf(address, role):
        start = binary.image.functionOf(address)
        if start is None:
            raise ValueError(f'{architecture}: {hexText(address)} ({role}) is not in a function')
        note(start, role)
        return binary.analyse(start)

    returns, sites, dataOut, vtables = {}, {}, {}, {}

    def keepReturn(address, role):
        if hexText(address) in returns:
            return
        function = functionOf(address, role)
        index = next((k for k, e in enumerate(function.events) if e[2] == address), None)
        if index is None:
            raise ValueError(f'{architecture}: {hexText(address)} ({role}) follows no call')
        returns[hexText(address)] = [hexText(function.start), index]

    def keepSite(address, role, immediate=None):
        """An instruction; `immediate`, for a layout's evidence, is the index of the immediate it shows."""
        key = hexText(address)
        function = functionOf(address, role)
        if address not in function.addresses:
            raise ValueError(f'{architecture}: {key} ({role}) is not an instruction')
        index = function.addresses.index(address)
        op, before, after = function.siteContext(index)
        site = sites.setdefault(key, {'function': hexText(function.start), 'index': index, 'op': op,
                                      'before': before, 'after': after})
        if immediate is not None:
            if site.setdefault('immediate', immediate) != immediate:
                raise ValueError(f'{architecture}: {key} is the evidence of two immediates')

    def keepVtable(address, role):
        if not binary.vtableAt(address):
            raise ValueError(f'{architecture}: {hexText(address)} ({role}) is not a vtable with RTTI')
        vtables[hexText(address)] = binary.vtableKey(address)

    for entry in slice_['addresses']:
        address = number(entry['mac'])
        if entry['name'] in data:
            continue
        if not binary.image.isFunction(address):
            raise ValueError(f'{architecture}: {entry["name"]} {entry["mac"]} is not a function start')
        functionOf(address, entry['name'])
    for entry in slice_['signatures']:
        functionOf(number(entry['mac']), f'signature of {entry["name"]}')
    for frame in slice_['frames']:
        for key in ('mac', 'parent', 'grandparent'):
            if frame.get(key):
                keepReturn(number(frame[key]), 'frames')
    for site in mapping.get('sites', ()):
        if site.get('slice') == architecture:
            keepReturn(number(site['mac']), 'frames')
    for layout in mapping.get('layouts', ()):
        for item in layout['evidence'].get(architecture, ()):
            if 'vtable' in item:
                keepVtable(number(item['vtable']), layout['name'])
                functionOf(number(item['points_to']), layout['name'])
                continue
            address = number(item['at'])
            if 'target' in item:
                keepSite(address, layout['name'])
            else:
                found = binary.slice.operands(address)
                if not found or number(item['imm']) not in found['imms']:
                    raise ValueError(f'{architecture}: {layout["name"]}: {item["at"]} does not encode {item["imm"]}')
                keepSite(address, layout['name'], found['imms'].index(number(item['imm'])))
    # Globals: by RTTI for vtables, otherwise through the instructions that reach them, in the
    # functions above or, failing those, in the smallest functions that do.
    for entry in slice_['addresses']:
        if entry['name'] not in data:
            continue
        address = number(entry['mac'])
        if binary.vtableAt(address):
            keepVtable(address, entry['name'])
            dataOut[entry['name']] = {'at': entry['mac'], 'vtable': entry['mac']}
            continue
        references, near = referencesTo(binary, address, list(roles))
        if not references:
            scanned = {binary.image.functionOf(a) for a in binary.scanReferences([(address, address + 1)])}
            for start in sorted(scanned - {None}, key=lambda f: (binary.functionSize(f), f)):
                found, _ = referencesTo(binary, address, [start])
                if found:
                    references += found
                if len(references) >= 3:
                    break
        references = references or near
        if not references:
            raise ValueError(f'{architecture}: no code reaches {entry["name"]} {entry["mac"]}')
        for site, delta in references[:3]:
            keepSite(site, f'reaches {entry["name"]}')
            note(binary.image.functionOf(site), f'reaches {entry["name"]}')
        dataOut[entry['name']] = {'at': entry['mac'],
                                  'references': [[hexText(s), d] for s, d in references[:3]]}
        for item in entry.get('evidence', ()):
            if 'at' in item:
                keepSite(number(item['at']), f'reaches {entry["name"]}')
    formatSite = findFormatVersion(binary, mapping, roles)
    keepSite(formatSite, 'format version')
    slots = slotIndex(binary)
    functions = {}
    for start in sorted(roles):
        function = binary.analyse(start)
        before, after = binary.context(start)
        events = []
        for (kind, value, context), (_, target, _, _) in zip(function.tokens(binary), function.events):
            events.append([kind, value, context] + ([hexText(target)] if kind in ('C', 'T') else []))
        strings = sorted({h for h in (binary.stringHash(t) for _, t in function.references) if h})
        functions[hexText(start)] = {
            'roles': roles[start], 'size': function.end - function.start, 'before': before, 'after': after,
            'hash': function.hash, 'instructions': len(function.normalised), 'sketch': function.sketch,
            'events': events, 'strings': strings, 'vtables': slots.get(start, [])}
    return {'uuid': binary.image.uuid, 'format_version': {'site': hexText(formatSite)}, 'vtables': vtables,
            'data': dataOut, 'functions': functions, 'returns': returns, 'sites': sites}


def referencesTo(binary, address, functions):
    """[(instruction, 0)] in `functions` that reach `address`, and [(instruction, target - address)] that
    reach a member less than 0x100 after it, nearest first."""
    exact, near = [], []
    for start in functions:
        function = binary.analyse(start)
        for index, target in function.references:
            if target == address:
                exact.append((function.addresses[index], 0))
            elif 0 < target - address < 0x100:
                near.append((function.addresses[index], target - address))
    return exact, sorted(near, key=lambda r: r[1])


def slotIndex(binary):
    """Function -> its vtable slots (at most four): [class name, offset to top, rank, place, slot]."""
    out = {}
    for point, _, _, _, slots in binary.vtables():
        for slot, target in enumerate(slots):
            if target is not None and len(out.setdefault(target, [])) < 4:
                out[target].append(binary.vtableKey(point)[:4] + [slot])
    return out


def findFormatVersion(binary, mapping, roles):
    """The instruction that reaches the constant the match header writes as its format version: a
    read-only u16 holding the mapping's format version, written next as two bytes."""
    value = number(mapping['format_version'])
    found = []
    for start in roles:
        function = binary.analyse(start)
        for index, target in function.references:
            raw = binary.read(target, 2)
            if not raw or struct.unpack('<H', raw)[0] != value or not binary.isReadOnlyData(target):
                continue
            following = next((e for e in function.events if e[3] is not None and e[3] > index), None)
            if following and following[0] in ('W', 'WT') and following[1] == 2:
                found.append(function.addresses[index])
    if len(found) != 1:
        raise ValueError(f'{binary.architecture}: {len(found)} instructions reach a format version constant '
                         'written as two bytes; expected one')
    return found[0]


def fingerprintCommand(arguments):
    if len(arguments) not in (2, 4) or (len(arguments) == 4 and arguments[2] != '--write'):
        sys.exit(__doc__)
    mappingPath, path = arguments[0], arguments[1]
    write = arguments[3] if len(arguments) == 4 else None
    with open(mappingPath) as file:
        mapping = json.load(file)
    data = check_mapping.dataNames()
    document = {'format': FORMAT, 'mapping': os.path.basename(mappingPath),
                'version_text': mapping.get('version_text'), 'game_build': mapping.get('game_build'),
                'format_version': mapping.get('format_version'), 'slices': {}}
    for architecture, binary in loadSlices(path, mapping).items():
        document['slices'][architecture] = fingerprintSlice(binary, mapping, data)
    text = compact(document)
    default = os.path.join(HERE, 'align', f'fingerprints-{mapping.get("version_text", "build")}.json')
    if write:
        with open(write, 'w') as file:
            file.write(text)
        print(f'wrote {write} ({len(text) // 1024} KB)')
        return 0
    try:
        with open(default) as file:
            same = file.read() == text
    except OSError:
        same = False
    print(f'{default} {"matches" if same else "differs (rerun with --write " + default + ")"}')
    return 0 if same else 1


# Remapping


class SliceRemap:
    """One slice's matching of a fingerprinted build with a new one."""

    def __init__(self, fingerprints, binary, review):
        self.fp = fingerprints
        self.binary = binary
        self.review = review
        self.architecture = binary.architecture
        self.functions = {number(a): e for a, e in fingerprints['functions'].items()}
        self.match = {}     # old function -> (new function, method, same code, similarity)
        self.claimed = {}   # new function -> old function
        self.conflicts = []
        self._alignments = {}
        sizes = binary.sizes()
        self.sizes = sizes
        self.bySize, self.byBefore, self.byAfter = {}, {}, {}
        for j, size in enumerate(sizes):
            self.bySize.setdefault(size, []).append(j)
            self.byBefore.setdefault(tuple(sizes[max(0, j - 2):j][::-1]), []).append(j)
            self.byAfter.setdefault(tuple(sizes[j + 1:j + 3]), []).append(j)

    def say(self, text):
        self.review.append(f'{self.architecture}: {text}')

    # Vtables

    def vtable(self, key):
        """The new build's vtable for an old one's key, and how it was found."""
        name, offset, rank, group, _ = key
        found = self.binary.vtableByKey(name, offset, rank)
        if found:
            return found, 'rtti'
        named = [v for v in self.binary.vtables() if v[1] == name]
        if group < len(named):
            return named[group], 'rtti (by its place among the class\'s vtables)'
        return None, None

    # Functions

    def likeness(self, old, new):
        """(the same code, how alike) of an old function and a new one."""
        entry = self.functions[old]
        function = self.binary.analyse(new)
        same = function.hash == entry['hash'] and function.end - function.start == entry['size']
        return same, 1.0 if same else fingerprint.similarity(entry['sketch'], function.sketch)

    def consider(self, old, new, method):
        """Takes `new` as old's counterpart if its code is the same or, but for a call's callee, alike
        enough, and neither is matched better already."""
        if self.binary.functionIndex(new) is None:
            return False
        if old in self.match:
            if self.match[old][0] != new:
                self.conflicts.append((old, self.match[old][0], new, method))
            return False
        same, alike = self.likeness(old, new)
        if not same and method != 'call' and alike < ACCEPT:
            return False
        rival = self.claimed.get(new)
        if rival is not None:
            _, rivalMethod, rivalSame, rivalAlike = self.match[rival]
            if (rivalSame, METHODS[rivalMethod], rivalAlike) >= (same, METHODS[method], alike):
                self.conflicts.append((old, new, new, method))
                return False
            del self.match[rival]
            self.conflicts.append((rival, new, new, rivalMethod))
        self.match[old] = (new, method, same, alike)
        self.claimed[new] = old
        return True

    def byPosition(self, old):
        """The function whose own size and neighbours' sizes best fit the fingerprint's (the sizes in
        common, in order, on each side), preferring the same code among the best; or None."""
        entry = self.functions[old]
        size, before, after = entry['size'], entry['before'], entry['after']
        candidates = set()
        for index, key in ((self.byBefore, tuple(before[:2])), (self.byAfter, tuple(after[:2])),
                           (self.bySize, size)):
            found = index.get(key, ())
            if len(found) <= SIZE_LIMIT:
                candidates |= set(found)
        scored = []
        for j in candidates:
            near = self.sizes[max(0, j - len(before)):j][::-1]
            far = self.sizes[j + 1:j + 1 + len(after)]
            score = (2 if self.sizes[j] == size else 0) + common(before, near) + common(after, far)
            if score >= CONTEXT_MIN:
                scored.append((score, j))
        scored.sort(reverse=True)
        ranked = []
        for score, j in scored[:5]:
            same, alike = self.likeness(old, self.binary.functions[j])
            if same or alike >= ACCEPT:
                ranked.append((same, score, alike, j))
        ranked.sort(reverse=True)
        if not ranked:
            return None
        if len(ranked) > 1 and ranked[1][:2] == ranked[0][:2] and ranked[0][2] - ranked[1][2] < CONTEXT_MARGIN:
            return None  # two fit as well as each other
        return self.binary.functions[ranked[0][3]]

    def bySlot(self, old, membership):
        """The function in the new build's vtable slot for an old one, or, if that is not alike, the one
        in another slot of that vtable with the same code (or the most alike), or None."""
        found, _ = self.vtable(membership[:4] + [None])
        if not found:
            return None
        slots = [t for t in found[4] if t is not None and self.binary.functionIndex(t) is not None]
        slot = membership[4]
        if slot < len(found[4]) and found[4][slot] in slots:
            same, alike = self.likeness(old, found[4][slot])
            if same or alike >= ACCEPT:
                return found[4][slot]
        ranked = sorted(((self.likeness(old, t), t) for t in set(slots)), reverse=True)
        if ranked and (ranked[0][0][0] or ranked[0][0][1] >= ACCEPT):
            if len(ranked) > 1 and ranked[1][0] == ranked[0][0]:
                return None
            return ranked[0][1]
        return None

    def alignment(self, old):
        """{old event index: new event index} for a matched function."""
        if old in self._alignments:
            return self._alignments[old]
        new, _, same, _ = self.match[old]
        entry = self.functions[old]
        function = self.binary.analyse(new)
        mine = [tuple(e[:3]) for e in entry['events']]
        theirs = function.tokens(self.binary)
        if same and len(mine) == len(theirs):
            pairs = {k: k for k in range(len(mine))}
        else:
            pairs = alignEvents(mine, theirs)
        self._alignments[old] = pairs
        return pairs

    def propagate(self, pending):
        while pending:
            old = pending.pop()
            entry = self.functions[old]
            function = self.binary.analyse(self.match[old][0])
            for i, j in self.alignment(old).items():
                event = entry['events'][i]
                if event[0] not in ('C', 'T'):
                    continue
                callee = number(event[3])
                if callee in self.functions and callee not in self.match:
                    if self.consider(callee, function.events[j][1], 'call'):
                        pending.append(callee)

    def byStrings(self):
        strings = self.binary.strings()
        wanted = {}
        for old, entry in self.functions.items():
            if old in self.match:
                continue
            for h in entry['strings']:
                if len(strings.get(h, ())) == 1:
                    wanted.setdefault(strings[h][0], set()).add(old)
        if not wanted:
            return []
        found = []
        sites = self.binary.scanReferences([(a, a + 1) for a in wanted])
        for start in sorted({self.binary.image.functionOf(s) for s in sites} - {None}):
            reached = {t for _, t in self.binary.analyse(start).references}
            for address in reached & set(wanted):
                for old in wanted[address]:
                    if self.consider(old, start, 'string'):
                        found.append(old)
        return found

    def matchFunctions(self):
        pending = []
        for old, entry in self.functions.items():
            for membership in entry['vtables']:
                new = self.bySlot(old, membership)
                if new is not None:
                    if self.consider(old, new, 'vtable'):
                        pending.append(old)
                    break
        self.propagate(pending)
        while True:
            pending = []
            for old in self.functions:
                if old not in self.match:
                    new = self.byPosition(old)
                    if new is not None and self.consider(old, new, 'position'):
                        pending.append(old)
            pending += self.byStrings()
            if not pending:
                break
            self.propagate(pending)

    # The mapping's items

    def returnAddress(self, address):
        """The new return address for an old one, or None."""
        old = self.fp['returns'].get(hexText(address))
        if not old or number(old[0]) not in self.match:
            return None
        function, index = number(old[0]), old[1]
        new = self.alignment(function).get(index)
        if new is None:
            return None
        return self.binary.analyse(self.match[function][0]).events[new][2]

    def site(self, address, operand):
        """The new address of an old instruction the mapping relies on, and what it reads there, or (None,
        None): the address it reaches through its registers (`reference`), the address check_mapping.py
        decodes (`target`) or its evidence's immediate (`immediate`)."""
        old = self.fp['sites'].get(hexText(address))
        if not old or number(old['function']) not in self.match:
            return None, None
        new, _, same, _ = self.match[number(old['function'])]
        function = self.binary.analyse(new)
        if same:
            index = old['index']
        else:
            scale = len(function.addresses) / max(1, self.functions[number(old['function'])]['instructions'])
            index = function.findSite(old['op'], old['before'], old['after'], round(old['index'] * scale))
        if index is None:
            return None, None
        address = function.addresses[index]
        if operand == 'reference':
            return address, function.targets.get(index)
        found = self.binary.slice.operands(address)
        if not found:
            return address, None
        if operand == 'target':
            return address, found['target']
        immediate = old.get('immediate', 0)
        return address, found['imms'][immediate] if immediate < len(found['imms']) else None

    def global_(self, name, entry):
        if 'vtable' in entry:
            key = self.fp['vtables'][entry['vtable']]
            found, how = self.vtable(key)
            if found and how != 'rtti':
                self.say(f'{name}: the vtable of {key[0]} was found {how}')
            return found[0] if found else None
        values = set()
        for site, delta in entry['references']:
            _, target = self.site(number(site), 'reference')
            if target is not None:
                values.add(target - delta)
        if len(values) > 1:
            self.say(f'{name}: its references reach {", ".join(hexText(v) for v in sorted(values))}')
            return None
        return values.pop() if values else None


def alignEvents(mine, theirs):
    """{index: index} pairing two functions' events: first those whose kind, value and the instructions
    before them all agree, then, between those, those whose kind and value agree, then (in stretches of
    equal length) those of the same kind."""
    pairs = {}
    anchors = []
    matcher = difflib.SequenceMatcher(None, mine, theirs, autojunk=False)
    for block in matcher.get_matching_blocks():
        for k in range(block.size):
            pairs[block.a + k] = block.b + k
            anchors.append((block.a + k, block.b + k))
    previous = (-1, -1)
    for a, b in anchors + [(len(mine), len(theirs))]:
        left = [t[:2] for t in mine[previous[0] + 1:a]]
        right = [t[:2] for t in theirs[previous[1] + 1:b]]
        coarse = difflib.SequenceMatcher(None, left, right, autojunk=False)
        for tag, i1, i2, j1, j2 in coarse.get_opcodes():
            if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    if left[i][0] == right[j][0]:
                        pairs[previous[0] + 1 + i] = previous[1] + 1 + j
        previous = (a, b)
    return pairs


def common(a, b):
    """How many values two lists have in common, in order."""
    return sum(block.size for block in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks())


def evaluate(layout, shown, addresses):
    """The value(s) a layout's evidence gives, as check_mapping.py works them out."""
    relation = layout.get('relation', 'equal')
    if relation == 'sum':
        return {sum(shown)}
    if relation.startswith('target-minus:'):
        base = addresses.get(relation.split(':', 1)[1])
        return {s - base for s in shown} if base is not None else set()
    if relation.startswith('minus:'):
        return {s - int(relation.split(':', 1)[1], 16) for s in shown}
    if relation.startswith('plus:'):
        return {s + int(relation.split(':', 1)[1], 16) for s in shown}
    return set(shown)


def remapEvidence(remap, entry, value):
    """A vtable's or global's evidence in the new build: its RTTI as it was (check_mapping.py checks it
    again), and the instructions that reach it, where they still reach its new address."""
    out = []
    for item in entry['evidence']:
        if 'at' not in item:
            out.append(item)
            continue
        new, target = remap.site(number(item['at']), 'target')
        if target == value:
            out.append({'at': hexText(new)})
        else:
            remap.say(f'{entry["name"]}: evidence {item["at"]} not found')
    return out


def remapLayouts(mapping, remaps, addresses, review):
    """The layouts with their evidence in the new build, and the values that evidence gives."""
    out = []
    for layout in mapping.get('layouts', ()):
        layout = copy.deepcopy(layout)
        values = {}
        for architecture, items in layout['evidence'].items():
            remap = remaps.get(architecture)
            if not items or remap is None:
                continue
            shown = []
            for item in items:
                if 'vtable' in item:
                    key = remap.fp['vtables'][item['vtable']]
                    found, _ = remap.vtable(key)
                    target = remap.match.get(number(item['points_to']))
                    if not found or not target:
                        remap.say(f'layout {layout["name"]}: vtable evidence {item["vtable"]} not found')
                        shown = None
                        break
                    slot = number(item['slot_offset']) // 8
                    if not (slot < len(found[4]) and found[4][slot] == target[0]):
                        slot = found[4].index(target[0]) if target[0] in found[4] else None
                    if slot is None:
                        remap.say(f'layout {layout["name"]}: {key[0]}\'s vtable has no slot for its function')
                        shown = None
                        break
                    item.update(vtable=hexText(found[0]), slot_offset=hexText(slot * 8), points_to=hexText(target[0]))
                    shown.append(slot if layout.get('relation') == 'slot-index' else slot * 8)
                    continue
                new, value = remap.site(number(item['at']), 'target' if 'target' in item else 'immediate')
                if value is None:
                    remap.say(f'layout {layout["name"]}: evidence {item["at"]} not found')
                    shown = None
                    break
                item['at'] = hexText(new)
                if 'target' in item:
                    item['target'] = hexText(value)
                else:
                    item['imm'] = hexText(value)
                shown.append(value)
            if shown is not None:
                values[architecture] = evaluate(layout, shown, addresses[architecture])
        given = set().union(*values.values()) if values else set()
        if len(given) > 1 or any(len(v) != 1 for v in values.values()):
            review.append(f'layout {layout["name"]}: the evidence gives {", ".join(hexText(v) for v in sorted(given))}')
        elif given and layout['value'] is not None and given != {number(layout['value'])}:
            value = given.pop()
            review.append(f'layout {layout["name"]} changed: {layout["value"]} -> {hexText(value)} '
                          f'(src/dll/game_layout_fm27.h holds the old value)')
            layout['value'] = hexText(value)
        out.append(layout)
    return out


def remapCommand(arguments):
    options = {'--version': None, '--out': None, '--report': None}
    positional = []
    while arguments:
        argument = arguments.pop(0)
        if argument in options:
            if not arguments:
                sys.exit(__doc__)
            options[argument] = arguments.pop(0)
        else:
            positional.append(argument)
    if len(positional) != 3:
        sys.exit(__doc__)
    mappingPath, fingerprintsPath, path = positional
    with open(mappingPath) as file:
        mapping = json.load(file)
    with open(fingerprintsPath) as file:
        fingerprints = json.load(file)
    if fingerprints.get('format') != FORMAT or fingerprints.get('version_text') != mapping.get('version_text'):
        raise ValueError(f'{fingerprintsPath} is not the fingerprints of {mappingPath}')
    version = options['--version'] or appVersion(path, mapping)
    if not version:
        raise ValueError('give the new build\'s version with --version (its app\'s Info.plist was not found)')
    out = options['--out'] or os.path.join(os.path.dirname(os.path.abspath(mappingPath)), f'steam-{version}.json')
    if os.path.abspath(out) == os.path.abspath(mappingPath):
        raise ValueError('the new mapping would replace the old one: give another --out')
    data = check_mapping.dataNames()
    review = []
    lines = [f'Remapping {mapping.get("version_text")} onto {version}']
    new = copy.deepcopy(mapping)
    remaps = {}
    addresses = {}
    for architecture, binary in loadSlices(path, mapping).items():
        remap = SliceRemap(fingerprints['slices'][architecture], binary, review)
        remap.matchFunctions()
        remaps[architecture] = remap
        old = mapping['slices'][architecture]
        slice_ = new['slices'][architecture]
        slice_['uuid'] = binary.image.uuid
        if binary.image.uuid == old['uuid'] and version != mapping.get('version_text'):
            remap.say(f'the new build has the old build\'s UUID {binary.image.uuid}')
        # Addresses and signatures.
        names = {}
        for entry in slice_['addresses']:
            if entry['name'] in data:
                value = remap.global_(entry['name'], remap.fp['data'][entry['name']])
            else:
                found = remap.match.get(number(entry['mac']))
                value = found[0] if found else None
            if value is None:
                remap.say(f'{entry["name"]} {entry["mac"]} not found')
            else:
                entry['mac'] = hexText(value)
                names[entry['name']] = value
                if entry.get('evidence'):
                    entry['evidence'] = remapEvidence(remap, entry, value)
        slice_['addresses'] = [e for e in slice_['addresses'] if e['name'] in names]
        addresses[architecture] = names
        signatures = []
        for entry in slice_['signatures']:
            found = remap.match.get(number(entry['mac']))
            if not found:
                remap.say(f'the function of the signature {entry["name"]} {entry["mac"]} not found')
                continue
            length = len(entry['bytes']) // 2
            raw = binary.read(found[0], length) or b''
            if raw != bytes.fromhex(entry['bytes']):
                remap.say(f'the signature of {entry["name"]} changed')
            signatures.append(dict(entry, mac=hexText(found[0]), bytes=raw.hex()))
        slice_['signatures'] = signatures
        # Frames, and the synthetic sites of frames of their own.
        frames, dropped, seen = [], 0, set()
        for entry in old['frames']:
            moved = dict(entry)
            for key in ('mac', 'parent', 'grandparent'):
                if entry.get(key):
                    value = remap.returnAddress(number(entry[key]))
                    moved[key] = hexText(value) if value is not None else None
            if None in moved.values():
                dropped += 1
                continue
            key = (moved['mac'], moved.get('parent'), moved.get('grandparent'))
            if key in seen:
                remap.say(f'two frames land on {moved["mac"]}')
            seen.add(key)
            frames.append(moved)
        if dropped:
            remap.say(f'{dropped} of {len(old["frames"])} frames not found')
        frames.sort(key=lambda f: (number(f['mac']), number(f.get('parent', '0x0')), number(f.get('grandparent', '0x0'))))
        slice_['frames'] = frames
        # Summary.
        unmatched = [f for f in remap.functions if f not in remap.match]
        changed = [f for f, m in remap.match.items() if not m[2]]
        methods = {}
        for m in remap.match.values():
            methods[m[1]] = methods.get(m[1], 0) + 1
        lines.append(f'{architecture} {mapping["slices"][architecture]["uuid"]} -> {binary.image.uuid}')
        lines.append(f'  functions: {len(remap.match)} of {len(remap.functions)} matched '
                     f'({", ".join(f"{n} by {k}" for k, n in sorted(methods.items()))}), '
                     f'{len(remap.match) - len(changed)} with the same code')
        for f in changed:
            m = remap.match[f]
            remap.say(f'function {hexText(f)} -> {hexText(m[0])} changed (by {m[1]}, '
                      f'{m[3]:.2f} alike): {", ".join(remap.functions[f]["roles"])}')
        for f in unmatched:
            remap.say(f'function {hexText(f)} not found: {", ".join(remap.functions[f]["roles"])}')
        for old_, first, second, method in remap.conflicts:
            remap.say(f'function {hexText(old_)}: {hexText(first)} and {hexText(second)} (by {method}) both fit')
        lines.append(f'  addresses: {len(slice_["addresses"])} of {len(old["addresses"])}, '
                     f'frames: {len(frames)} of {len(old["frames"])}')
    # Synthetic sites follow their frames.
    sites = []
    for site in mapping.get('sites', ()):
        if 'slice' in site:
            value = remaps[site['slice']].returnAddress(number(site['mac'])) if site['slice'] in remaps else None
            if value is None:
                review.append(f'{site["slice"]}: synthetic site {site["id"]} ({site["mac"]}) not found')
                continue
            site = dict(site, mac=hexText(value))
        sites.append(site)
    if 'sites' in new:
        new['sites'] = sites
    if 'layouts' in mapping:
        new['layouts'] = remapLayouts(mapping, remaps, addresses, review)
    # The format version.
    versions = set()
    for architecture, remap in remaps.items():
        _, target = remap.site(number(remap.fp['format_version']['site']), 'reference')
        raw = remap.binary.read(target, 2) if target is not None else None
        if raw is None:
            remap.say('the format version\'s constant not found')
        else:
            versions.add(struct.unpack('<H', raw)[0])
    if len(versions) == 1:
        value = versions.pop()
        if value != number(mapping['format_version']):
            review.append(f'the format version changed: {mapping["format_version"]} -> 0x{value:04x} '
                          '(configs of the old build do not play in this one)')
        new['format_version'] = f'0x{value:04x}'
    elif versions:
        review.append(f'the slices give format versions {", ".join(hex(v) for v in sorted(versions))}')
    game, app = GAMES.get(mapping.get('edition', 24), ('the game', '?'))
    new['version_text'] = version
    new['game_build'] = int(version.split('.')[-1]) if version.split('.')[-1].isdigit() else mapping.get('game_build')
    new['build'] = (f'{game} {version}, Steam app {app}, {mapping.get("image", "Contents/MacOS/fm")}; remapped '
                    f'from {mapping.get("version_text")} by tools/macos/remap.py')
    with open(out, 'w') as file:
        json.dump(new, file, indent=1, ensure_ascii=False)
        file.write('\n')
    lines.append(f'  format version: {new.get("format_version")}, layouts: {len(new.get("layouts", ()))}')
    lines.append(f'wrote {out}')
    if review:
        lines.append(f'needs review ({len(review)}):')
        lines += [f'  {r}' for r in review]
    else:
        lines.append('nothing needs review')
    lines.append('next: check_mapping.py on the new mapping, its UUIDs and version in src/common/macos_builds.c, '
                 'gen_tables.py, the tests, then --fingerprint --write for the new build')
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if options['--report']:
        with open(options['--report'], 'w') as file:
            file.write(text)
    return 1 if review else 0


def main(arguments):
    try:
        if len(arguments) > 1 and arguments[1] == '--fingerprint':
            return fingerprintCommand(arguments[2:])
        return remapCommand(arguments[1:])
    except (OSError, ValueError, KeyError) as error:
        print(f'remap.py: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
