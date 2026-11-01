# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""What remap.py needs to find a mapped build's code and data again in another build of the game,
without keeping any of its code: the fingerprints of the functions a mapping uses, and the analysis
of a slice that both makes them and matches them.

A function's fingerprint holds its size, the sizes of the functions around it, a hash of its
instructions with their registers, immediates and addresses masked, a sketch of their 4-grams (for
how alike two versions are), its calls and stream writes in order (with each callee's size and a
hash of the instructions just before each call), the hashes of the strings it uses and the vtable
slots that point to it. An instruction the mapping
relies on (a layout's evidence, a reference to a global) is kept as its function, its index and the
masked hashes of the instructions around it; a return address (a frame) as its function and the
index of its call. A vtable is kept by its class's RTTI name. Nothing else of the code is kept."""
import bisect
import hashlib
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(1, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import check_mapping  # noqa: E402
import events as eventsModule  # noqa: E402

LC_SEGMENT_64 = 0x19
LC_DYLD_CHAINED_FIXUPS = 0x80000034
SKETCH = 64          # how many 4-gram hashes a function's sketch keeps
GRAM = 4
CONTEXT = 8          # neighbouring functions' sizes kept on each side
SITE_CONTEXT = 6     # instructions' hashes kept on each side of a site
EVENT_CONTEXT = 4    # instructions up to a call that tell calls of the same kind apart
TYPE_INFO = re.compile(r'__ZTVN10__cxxabiv1\d+__(?:si_|vmi_)?class_type_infoE$')
TYPE_NAME = re.compile(rb'[A-Za-z0-9_]+\0')

ARM64_REGISTER = re.compile(r'\b(?:[xwbhsdqvz]\d{1,2}|w?sp|[xw]zr|fp|lr)\b')
ARM64_IMMEDIATE = re.compile(r'#-?(?:0x[0-9a-f]+|\d+(?:\.\d+)?(?:e[+-]?\d+)?)')
X86_REGISTER = re.compile(r'%[a-z0-9]+')
X86_IMMEDIATE = re.compile(r'\$-?(?:0x[0-9a-f]+|\d+)')
X86_DISPLACEMENT = re.compile(r'-?(?:0x[0-9a-f]+|\d+)(?=\()')
ADDRESS = re.compile(r'\b0x[0-9a-f]+\b')


def number(text):
    return -int(text[1:], 16) if text.startswith('-') else int(text, 16)


def shortHash(text, length=8):
    return hashlib.sha256(text.encode()).hexdigest()[:length]


def normalise(architecture, op, arguments):
    """An instruction with its registers, immediates, displacements and addresses masked."""
    if architecture == 'arm64':
        arguments = ARM64_IMMEDIATE.sub('#', ARM64_REGISTER.sub('r', arguments))
    else:
        arguments = X86_DISPLACEMENT.sub('d', X86_IMMEDIATE.sub('$', X86_REGISTER.sub('%', arguments)))
    return f'{op} {ADDRESS.sub("@", arguments)}'.strip()


def sketch(normalised):
    """The smallest SKETCH hashes of a function's instruction 4-grams."""
    grams = {zlib.crc32('\x1f'.join(normalised[i:i + GRAM]).encode())
             for i in range(max(1, len(normalised) - GRAM + 1))}
    return sorted(grams)[:SKETCH]


def similarity(a, b):
    """How alike two sketches' functions are (an estimate of the Jaccard index of their 4-grams)."""
    if not a or not b:
        return 0.0
    union = sorted(set(a) | set(b))[:SKETCH]
    both = set(a) & set(b)
    return sum(1 for g in union if g in both) / len(union)


ARM64_PAGE = re.compile(r'(x\d+), (0x[0-9a-f]+)$')
ARM64_ADD = re.compile(r'(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)$')
ARM64_MEMORY = re.compile(r'\[(x\d+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]')
ARM64_DESTINATION = re.compile(r'([xw])(\d+)\b')
ARM64_NO_DESTINATION = ('st', 'cmp', 'cmn', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz', 'b.', 'ccmp', 'prfm', 'fcmp')


def arm64Targets(lines):
    """Line index -> the address an instruction reaches through the page an ADRP loaded (ADRP; ADD;
    LDR/STR [x, #imm]), or ADR's address, following each register in straight-line code."""
    pages = {}
    out = {}
    for index, (_, op, arguments) in enumerate(lines):
        if op in ('adrp', 'adr'):
            match = ARM64_PAGE.match(arguments)
            if match:
                pages[match.group(1)] = int(match.group(2), 16)
                if op == 'adr':
                    out[index] = pages[match.group(1)]
                continue
        match = ARM64_ADD.match(arguments) if op == 'add' else None
        if match and match.group(2) in pages:
            pages[match.group(1)] = pages[match.group(2)] + int(match.group(3), 0)
            out[index] = pages[match.group(1)]
            continue
        memory = ARM64_MEMORY.search(arguments)
        if memory and memory.group(1) in pages and op.startswith(('ld', 'st')):
            out[index] = pages[memory.group(1)] + (int(memory.group(2), 0) if memory.group(2) else 0)
        if op in ('bl', 'blr', 'b', 'br', 'ret'):
            pages.clear()
            continue
        if not op.startswith(ARM64_NO_DESTINATION):
            destination = ARM64_DESTINATION.match(arguments)
            if destination:
                pages.pop('x' + destination.group(2), None)
                if op.startswith('ldp'):
                    second = ARM64_DESTINATION.match(arguments.split(', ', 1)[-1])
                    if second:
                        pages.pop('x' + second.group(2), None)
        if memory and (arguments.endswith('!') or '], #' in arguments):
            pages.pop(memory.group(1), None)  # written back
    return out


class Function:
    """One function of a slice: its instructions masked, its events and the addresses it reaches."""

    def __init__(self, binary, start):
        image = binary.image
        self.start, self.end = image.function(start)
        lines = eventsModule.disassemble(image, self.start, self.end)
        self.addresses = [a for a, _, _ in lines]
        self.normalised = [normalise(image.architecture, op, arguments) for _, op, arguments in lines]
        self.hash = shortHash('\n'.join(self.normalised), 16)
        self.sketch = sketch(self.normalised)
        self.events = []  # (kind, value, ret, line index of the call or jump, or None)
        indices = {a: i for i, a in enumerate(self.addresses)}
        for kind, value, ret in eventsModule.fromLines(image, lines, (self.start, self.end)):
            index = indices.get(ret) - 1 if ret in indices else None
            if ret is not None and ret == self.end:
                index = len(self.addresses) - 1
            self.events.append((kind, value, ret, index))
        # Line index -> the address an instruction reaches PC-relatively.
        if image.architecture == 'arm64':
            self.targets = arm64Targets(lines)
        else:
            self.targets = {}
            for index, address in enumerate(self.addresses):
                found = binary.slice.operands(address)
                if found and found['target'] is not None:
                    self.targets[index] = found['target']
        self.references = sorted(self.targets.items())

    def tokens(self, binary):
        """The events as alignment tokens: (kind, value, context): calls and jumps with their callee's
        size, writes with their length, virtual calls with their slot, each with the masked hash of the
        instructions that lead up to it (EVENT_CONTEXT of them, after the event before)."""
        out = []
        previous = -1
        for kind, value, _, index in self.events:
            if kind in ('C', 'T'):
                value = binary.functionSize(value)
            elif kind not in ('W', 'WT', 'V', 'VT'):
                value = None
            context = ''
            if index is not None:
                context = shortHash('\n'.join(self.normalised[max(previous + 1, index - EVENT_CONTEXT + 1):index + 1]), 6)
                previous = index
            out.append((kind, value, context))
        return out

    def siteContext(self, index):
        hashes = [shortHash(n) for n in self.normalised]
        before = [hashes[i] for i in range(index - 1, max(-1, index - 1 - SITE_CONTEXT), -1)]
        after = hashes[index + 1:index + 1 + SITE_CONTEXT]
        return hashes[index], before, after

    def findSite(self, op, before, after, near):
        """The index of the instruction whose masked hash is `op` and whose neighbours best match, nearest
        `near` among equals, or None."""
        hashes = [shortHash(n) for n in self.normalised]
        best = None
        for i, h in enumerate(hashes):
            if h != op:
                continue
            score = sum(1 for k, b in enumerate(before) if i - 1 - k >= 0 and hashes[i - 1 - k] == b)
            score += sum(1 for k, a in enumerate(after) if i + 1 + k < len(hashes) and hashes[i + 1 + k] == a)
            key = (score, -abs(i - near))
            if best is None or key > best[0]:
                best = (key, i)
        return best[1] if best else None


class Binary:
    """A slice, with what the fingerprints need beyond macho.Image: its pointers (chained fixups), its
    vtables by RTTI name, its strings and the functions it has analysed."""

    def __init__(self, image):
        self.image = image
        self.architecture = image.architecture
        self.slice = check_mapping.Slice(image.data)
        self.functions = image.functions
        self.base = next(s[1] for s in image.segments if s[0] == '__TEXT')
        self.rebases, self.binds = self._fixups()
        self._analysed = {}
        self._vtables = None
        self._strings = None
        self._sizes = None
        self._keys = None
        self._byPoint = None

    def _fixups(self):
        data = self.image.data
        ncmds = struct.unpack_from('<I', data, 16)[0]
        command = 32
        segments = []
        fixups = None
        for _ in range(ncmds):
            kind, size = struct.unpack_from('<II', data, command)
            if kind == LC_SEGMENT_64:
                address, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, command + 24)
                segments.append((address, fileoff, filesize))
            elif kind == LC_DYLD_CHAINED_FIXUPS:
                fixups = struct.unpack_from('<II', data, command + 8)
            command += size
        rebases, binds = {}, {}
        if not fixups:
            return rebases, binds
        offset = fixups[0]
        _, starts, imports, symbols, count, importFormat, _ = struct.unpack_from('<IIIIIII', data, offset)
        names = []
        width = {1: 4, 2: 8, 3: 16}[importFormat]
        for index in range(count):
            entry = offset + imports + index * width
            if importFormat == 3:
                name = struct.unpack_from('<Q', data, entry)[0] >> 32
            else:
                name = struct.unpack_from('<I', data, entry)[0] >> 9
            position = offset + symbols + name
            names.append(data[position:data.index(b'\0', position)].decode('ascii', 'replace'))
        table = offset + starts
        segmentCount = struct.unpack_from('<I', data, table)[0]
        for index in range(segmentCount):
            info = struct.unpack_from('<I', data, table + 4 + 4 * index)[0]
            if not info:
                continue
            position = table + info
            _, pageSize, pointerFormat, segmentOffset, _, pages = struct.unpack_from('<IHHQIH', data, position)
            if pointerFormat not in (2, 6):
                raise ValueError(f'unsupported chained pointer format {pointerFormat}')
            address, fileoff, _ = segments[index]
            for page, start in enumerate(struct.unpack_from(f'<{pages}H', data, position + 22)):
                if start == 0xFFFF:
                    continue
                here = page * pageSize + start
                while True:
                    value = struct.unpack_from('<Q', data, fileoff + here)[0]
                    location = address + here
                    if value >> 63:
                        binds[location] = names[value & 0xFFFFFF] if (value & 0xFFFFFF) < len(names) else '?'
                    else:
                        target = value & 0xFFFFFFFFF
                        rebases[location] = target + self.base if pointerFormat == 6 else target
                    step = (value >> 51) & 0xFFF
                    if not step:
                        break
                    here += 4 * step
        return rebases, binds

    def read(self, address, size):
        return self.slice.read(address, size)

    def cString(self, address, limit=4096):
        segment = self.slice.segment(address)
        if not segment:
            return None
        raw = self.read(address, min(limit, segment[1] + segment[4] - address)) or b''
        end = raw.find(b'\0')
        return raw[:end] if end >= 0 else None

    def functionSize(self, address):
        bounds = self.image.function(address)
        return bounds[1] - bounds[0] if bounds and bounds[0] == address else 0

    def functionIndex(self, start):
        index = bisect.bisect_left(self.functions, start)
        return index if index < len(self.functions) and self.functions[index] == start else None

    def sizes(self):
        ends = self.functions[1:] + [self.image.text[1]]
        return [e - s for s, e in zip(self.functions, ends)]

    def context(self, start):
        """The sizes of the CONTEXT functions before and after a function, nearest first."""
        if self._sizes is None:
            self._sizes = self.sizes()
        sizes = self._sizes
        index = self.functionIndex(start)
        before = [sizes[i] for i in range(index - 1, max(-1, index - 1 - CONTEXT), -1)]
        after = sizes[index + 1:index + 1 + CONTEXT]
        return before, after

    def analyse(self, start):
        if start not in self._analysed:
            self._analysed[start] = Function(self, start)
        return self._analysed[start]

    def isCode(self, address):
        return bool(self.image.text and self.image.text[0] <= address < self.image.text[1])

    def isReadOnlyData(self, address):
        segment = self.slice.segment(address)
        return bool(segment and segment[0] == '__TEXT' and not self.isCode(address))

    def stringHash(self, address):
        """The hash of the C string a code reference reaches, or None if it reaches none."""
        section = self.image.section('__TEXT', '__cstring')
        if not section or not section[0] <= address < section[1]:
            return None
        text = self.cString(address)
        return shortHash(text.hex(), 12) if text is not None else None

    def strings(self):
        """Hash -> addresses of every string in __cstring."""
        if self._strings is None:
            self._strings = {}
            section = self.image.section('__TEXT', '__cstring')
            if section:
                raw = self.read(section[0], section[1] - section[0]) or b''
                position = 0
                while position < len(raw):
                    end = raw.find(b'\0', position)
                    if end < 0:
                        break
                    key = shortHash(raw[position:end].hex(), 12)
                    self._strings.setdefault(key, []).append(section[0] + position)
                    position = end + 1
        return self._strings

    def vtables(self):
        """The vtables, by RTTI: [(address point, class name, offset to top, rank, slots)], where the rank
        tells apart vtables of one class with the same offset, in address order."""
        if self._vtables is not None:
            return self._vtables
        typeInfos = {}
        for location, name in self.binds.items():
            if TYPE_INFO.search(name) and location + 8 in self.rebases:
                text = self.cString(self.rebases[location + 8], 512)
                if text and TYPE_NAME.fullmatch(text + b'\0'):
                    typeInfos[location] = text.decode()
        found = []
        for location, target in self.rebases.items():
            top = location - 8
            if target not in typeInfos or top in self.rebases or top in self.binds:
                continue
            raw = self.read(top, 8)
            offset = struct.unpack('<q', raw)[0] if raw else 1
            if -0x80000000 < offset <= 0:
                found.append((location + 8, typeInfos[target], offset))
        found.sort()
        ranks = {}
        out = []
        for point, name, offset in found:
            rank = ranks.get((name, offset), 0)
            ranks[(name, offset)] = rank + 1
            slots = []
            here = point
            while (here in self.rebases and self.rebases[here] not in typeInfos) or here in self.binds:
                slots.append(self.rebases.get(here))
                here += 8
            out.append((point, name, offset, rank, slots))
        self._vtables = out
        return out

    def vtableAt(self, address):
        if self._byPoint is None:
            self._byPoint = {v[0]: v for v in self.vtables()}
        return self._byPoint.get(address)

    def vtableByKey(self, name, offset, rank):
        return next((v for v in self.vtables() if v[1:4] == (name, offset, rank)), None)

    def vtableKey(self, address):
        """[class name, offset to top, rank, place among the class's vtables, slots] of a vtable."""
        if self._keys is None:
            places = {}
            self._keys = {}
            for point, name, offset, rank, slots in self.vtables():
                self._keys[point] = [name, offset, rank, places.get(name, 0), len(slots)]
                places[name] = places.get(name, 0) + 1
        return list(self._keys[address])

    def scanReferences(self, ranges):
        """Code addresses that reach an address in one of `ranges` ([(low, high)]) PC-relatively, found by
        scanning the whole code section: ADRP with the ADD, LDR or STR after it on arm64, RIP-relative
        operands on x86_64 (candidates, which the caller confirms in the function's disassembly)."""
        start, end = self.image.text
        raw = self.read(start, end - start)
        lows = sorted(ranges)
        out = []

        def within(target):
            index = bisect.bisect_right(lows, (target, float('inf'))) - 1
            return index >= 0 and lows[index][0] <= target < lows[index][1]

        if self.architecture == 'arm64':
            for match in re.finditer(rb'[\x90\xb0\xd0\xf0]', raw):
                position = match.start() - 3
                if position % 4:
                    continue
                word = struct.unpack_from('<I', raw, position)[0]
                register = word & 31
                page = (((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3) << 12
                page = page - (1 << 33) if page & (1 << 32) else page
                pc = start + position
                page += pc & ~0xFFF
                for following in range(position + 4, min(position + 20, len(raw)), 4):
                    if following + 4 > len(raw):
                        break
                    found = self.slice.operands(start + following)
                    if found and found['target'] is not None and ((struct.unpack_from('<I', raw, following)[0] >> 5) & 31) == register:
                        if within(found['target']):
                            out.append(start + following)
                        break
        else:
            for match in re.finditer(rb'[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', raw):
                position = match.start()
                if position + 5 > len(raw):
                    continue
                target = start + position + 5 + struct.unpack_from('<i', raw, position + 1)[0]
                if any(within(target + immediate) for immediate in (0, 1, 2, 4)):
                    out.append(start + position)
        return out
