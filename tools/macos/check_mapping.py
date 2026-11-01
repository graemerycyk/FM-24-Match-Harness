# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a macOS build mapping (tools/macos/<build>.json) against the game's image.

  python3 -I tools/macos/check_mapping.py tools/macos/steam-24.4.2.json [path to fm.app or its image]

Without a path it uses the Steam install in ~/Library/Application Support/Steam. The image is the
app's executable, or for a mapping that names an `image` in the app (an engine plugin), that file.
For each slice it checks the UUID, that every function address is a function start
(LC_FUNCTION_STARTS), that every vtable and global (the data IDs of src/dll/game.h) lies in a data
segment, that the signature bytes match the file, and that every frame entry (and its parent and
grandparent) is a return address: inside the code and right after a call. Where a vtable or global
has `evidence`, it checks that each item still shows its address: an instruction that reaches it, or
for a vtable the class's RTTI name and offset to top. For a mapping with `layouts`, every vtable and
global must have evidence, and it checks that each layout's evidence instruction still encodes the
immediate (or reaches the address) the mapping records, that each vtable evidence slot still points
to its function, and that the evidence gives the layout's value. It reads the file only."""
import json
import os
import re
import struct
import sys

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
LC_FUNCTION_STARTS = 0x26
LC_DYLD_CHAINED_FIXUPS = 0x80000034
CPU_TYPES = {0x0100000C: 'arm64', 0x01000007: 'x86_64'}
GAME_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'dll', 'game.h')
STEAM_COMMON = '~/Library/Application Support/Steam/steamapps/common'
STEAM_GAMES = {24: 'Football Manager 2024', 27: 'Football Manager 27'}


class Slice:
    def __init__(self, data):
        self.data = data
        magic, cpu, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
        if magic != MH_MAGIC_64:
            raise ValueError('not a 64-bit Mach-O slice')
        self.architecture = CPU_TYPES.get(cpu & 0xFFFFFFFF, 'other')
        self.segments = []
        self.uuid = None
        self.chained = False
        starts = None
        command = 32
        for _ in range(ncmds):
            kind, size = struct.unpack_from('<II', data, command)
            if kind == LC_SEGMENT_64:
                name = data[command + 8:command + 24].rstrip(b'\0').decode()
                address, vmsize, fileoff, filesize, _, protection = struct.unpack_from('<QQQQii', data, command + 24)
                self.segments.append((name, address, vmsize, fileoff, filesize, protection))
            elif kind == LC_UUID:
                raw = data[command + 8:command + 24].hex().upper()
                self.uuid = '-'.join((raw[:8], raw[8:12], raw[12:16], raw[16:20], raw[20:]))
            elif kind == LC_FUNCTION_STARTS:
                starts = struct.unpack_from('<II', data, command + 8)
            elif kind == LC_DYLD_CHAINED_FIXUPS:
                self.chained = True
            command += size
        self.text = next(s for s in self.segments if s[0] == '__TEXT')
        self.functions = self._functionStarts(*starts) if starts else set()

    def _functionStarts(self, offset, size):
        functions = set()
        address = self.text[1]
        position = offset
        while position < offset + size:
            value = shift = 0
            while True:
                byte = self.data[position]
                position += 1
                value |= (byte & 0x7F) << shift
                shift += 7
                if byte < 0x80:
                    break
            if value == 0:
                break
            address += value
            functions.add(address)
        return functions

    def segment(self, address):
        for segment in self.segments:
            if segment[1] <= address < segment[1] + segment[2]:
                return segment
        return None

    def read(self, address, size):
        segment = self.segment(address)
        if not segment or address + size > segment[1] + segment[4]:
            return None
        start = segment[3] + address - segment[1]
        return self.data[start:start + size]

    def pointer(self, address):
        """The pointer stored at an address, as dyld rebases it (an unslid address), or None."""
        raw = self.read(address, 8)
        if raw is None:
            return None
        value = struct.unpack('<Q', raw)[0]
        if not self.chained:
            return value
        if value >> 63:
            return None  # a bind, to another image
        # DYLD_CHAINED_PTR_64_OFFSET: a 36-bit target offset from the image's header, and a high byte.
        return self.text[1] + (value & 0xFFFFFFFFF) + (((value >> 36) & 0xFF) << 56)

    def cString(self, address, limit=512):
        """The NUL-terminated bytes at an address (without the NUL), or None."""
        out = b''
        while len(out) < limit:
            chunk = self.read(address + len(out), 1)
            if chunk is None:
                return None
            if chunk == b'\0':
                return out
            out += chunk
        return None

    def rtti(self, point):
        """(class name, offset to top) of the vtable whose address point is `point`, from its RTTI, or
        None. Itanium: the offset to top, then the type_info, whose name is at +8, before the point."""
        raw = self.read(point - 16, 8)
        info = self.pointer(point - 8)
        name = self.pointer(info + 8) if info is not None else None
        if raw is None or name is None:
            return None
        text = self.cString(name & ((1 << 56) - 1))  # without the high byte non-unique RTTI sets
        return (text.decode('latin-1'), struct.unpack('<q', raw)[0]) if text else None

    def isCode(self, address):
        segment = self.segment(address)
        return bool(segment and segment[5] & 4)

    def followsCall(self, address):
        if self.architecture == 'arm64':
            instruction = self.read(address - 4, 4)
            if not instruction or address % 4:
                return False
            word = struct.unpack('<I', instruction)[0]
            return (word & 0xFC000000) == 0x94000000 or (word & 0xFFFFFC1F) == 0xD63F0000
        code = self.read(address - 7, 7)
        if not code:
            return False
        direct = code[2] == 0xE8                                          # call rel32
        register = code[5] == 0xFF and (code[6] & 0xF8) == 0xD0            # call reg
        extended = code[4] in (0x41, 0x43) and code[5] == 0xFF and (code[6] & 0xF8) == 0xD0
        memory = any(code[7 - length] == 0xFF and (code[8 - length] & 0x38) == 0x10 for length in (2, 3, 6, 7))
        return direct or register or extended or memory

    def operands(self, address):
        """The immediates and addresses an instruction at `address` encodes: {'imms': [...], 'target': address
        reached PC-relatively (arm64 through the ADRP before it), or None}; None if not decoded."""
        if self.architecture == 'arm64':
            return self._arm64(address)
        return self._x86(address)

    def _word(self, address):
        raw = self.read(address, 4)
        return struct.unpack('<I', raw)[0] if raw and address % 4 == 0 else None

    def _arm64(self, address):
        word = self._word(address)
        if word is None:
            return None
        imms = []
        base = (word >> 5) & 31
        if (word & 0x3B000000) == 0x39000000:  # LDR/STR (unsigned immediate)
            size, vector, opc = word >> 30, (word >> 26) & 1, (word >> 22) & 3
            scale = 4 if vector and opc >= 2 else size
            imms.append(((word >> 10) & 0xFFF) << scale)
        elif (word & 0x3A000000) == 0x28000000:  # LDP/STP (offset, pre- or post-index)
            opc, vector = word >> 30, (word >> 26) & 1
            scale = 2 + opc if vector else 2 + (opc >> 1)
            imm7 = (word >> 15) & 0x7F
            imms.append((imm7 - 0x80 if imm7 & 0x40 else imm7) << scale)
        elif (word & 0x3B200C00) == 0x38000000:  # LDUR/STUR (unscaled)
            imm9 = (word >> 12) & 0x1FF
            imms.append(imm9 - 0x200 if imm9 & 0x100 else imm9)
        elif (word & 0x1F000000) == 0x11000000:  # ADD/SUB (immediate): SUB gives a negative offset
            imm = ((word >> 10) & 0xFFF) << (12 * ((word >> 22) & 1))
            imms.append(-imm if word & 0x40000000 else imm)
        elif (word & 0x1F800000) == 0x12800000 and (word >> 29) & 3 in (2, 3):  # MOVZ/MOVK
            imms.append(((word >> 5) & 0xFFFF) << (16 * ((word >> 21) & 3)))
            base = None
        else:
            return None
        target = None
        for back in range(1, 9):  # the ADRP that set the base register, if any
            previous = self._word(address - 4 * back)
            if previous is None or base is None:
                break
            if (previous & 0x9F000000) == 0x90000000 and (previous & 31) == base:
                page = (((previous >> 5) & 0x7FFFF) << 2 | (previous >> 29) & 3) << 12
                page = page - (1 << 33) if page & (1 << 32) else page
                target = (address - 4 * back & ~0xFFF) + page + imms[0]
                break
        return {'imms': imms, 'target': target}

    def _x86(self, address):
        code = self.read(address, 16)
        if not code:
            return None
        i = 0
        while code[i] in (0x66, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x64, 0x65, 0x36):
            i += 1
        rex = code[i] if 0x40 <= code[i] <= 0x4F else 0
        i += 1 if rex else 0
        operand16 = 0x66 in code[:i]
        opcode = code[i]
        i += 1
        if 0xB8 <= opcode <= 0xBF:  # MOV r, imm32 (imm64 with REX.W)
            size = 8 if rex & 8 else 4
            return {'imms': [int.from_bytes(code[i:i + size], 'little', signed=True)], 'target': None}
        two = opcode == 0x0F
        if two:
            opcode = code[i]
            i += 1
            if opcode not in (0x10, 0x11, 0x1F, 0x28, 0x29, 0xAF, 0xB6, 0xB7, 0xBE, 0xBF) and not 0x40 <= opcode <= 0x4F:
                return None
            immediate = 0
        else:
            modrm_ops = set(range(0x00, 0x04)) | set(range(0x08, 0x0C)) | set(range(0x10, 0x14)) | \
                set(range(0x18, 0x1C)) | set(range(0x20, 0x24)) | set(range(0x28, 0x2C)) | \
                set(range(0x30, 0x34)) | set(range(0x38, 0x3C)) | {0x63, 0x84, 0x85, 0x88, 0x89, 0x8A, 0x8B, 0x8D}
            sizes = {0x80: 1, 0x83: 1, 0xC6: 1, 0x81: 2 if operand16 else 4, 0xC7: 2 if operand16 else 4}
            if opcode in modrm_ops:
                immediate = 0
            elif opcode in sizes:
                immediate = sizes[opcode]
            elif opcode in (0xF6, 0xF7):
                immediate = (1 if opcode == 0xF6 else 2 if operand16 else 4) if (code[i] >> 3) & 7 in (0, 1) else 0
            else:
                return None
        modrm = code[i]
        i += 1
        mod, rm = modrm >> 6, modrm & 7
        displacement, target = None, None
        if mod != 3:
            sib_base = None
            if rm == 4:
                sib_base = code[i] & 7
                i += 1
            if mod == 0 and rm == 5:  # RIP-relative
                displacement = int.from_bytes(code[i:i + 4], 'little', signed=True)
                i += 4
            elif mod == 0 and sib_base == 5:
                displacement = int.from_bytes(code[i:i + 4], 'little', signed=True)
                i += 4
            elif mod == 1:
                displacement = int.from_bytes(code[i:i + 1], 'little', signed=True)
                i += 1
            elif mod == 2:
                displacement = int.from_bytes(code[i:i + 4], 'little', signed=True)
                i += 4
            else:
                displacement = 0
            if mod == 0 and rm == 5:
                target = address + i + immediate + displacement
                displacement = None
        imms = [displacement] if displacement is not None else []
        if immediate:
            imms.append(int.from_bytes(code[i:i + immediate], 'little', signed=True))
        return {'imms': imms, 'target': target}


def slices(path, image):
    path = os.path.expanduser(path)
    if path.rstrip('/').endswith('.app'):
        path = os.path.join(path, image) if image else os.path.join(path, 'Contents', 'MacOS', 'fm')
    with open(path, 'rb') as file:
        data = file.read()
    if struct.unpack_from('>I', data, 0)[0] != FAT_MAGIC:
        return [Slice(data)]
    count = struct.unpack_from('>I', data, 4)[0]
    out = []
    for index in range(count):
        _, _, offset, size, _ = struct.unpack_from('>iiIII', data, 8 + 20 * index)
        out.append(Slice(data[offset:offset + size]))
    return out


def dataNames():
    """The names game.h gives game data (enums GameData, MacosGameData, PluginGameData), not code."""
    with open(GAME_H) as file:
        text = file.read()
    names = set()
    for body in re.findall(r'enum (?:GameData|MacosGameData|PluginGameData) \{(.*?)\};', text, re.S):
        names.update(re.findall(r'\b(RVA_[A-Z0-9_]+)\s*=', body))
    if not names:
        raise ValueError('cannot read the data RVAs in src/dll/game.h')
    return names


def number(text):
    return -int(text[1:], 16) if text.startswith('-') else int(text, 16)


def checkData(image, mapping, slice_, data):
    """Each vtable's and global's evidence in this slice still shows its address; a mapping with layouts
    has evidence for each."""
    problems = []
    for entry in slice_['addresses']:
        if entry['name'] not in data:
            continue
        name, address = entry['name'], int(entry['mac'], 16)
        evidence = entry.get('evidence', ())
        if not evidence and 'layouts' in mapping:
            problems.append(f'{name} {entry["mac"]} has no evidence')
        for item in evidence:
            if 'rtti' in item:
                expected = (item['rtti'], number(item['offset_to_top']))
                found = image.rtti(address)
                if found != expected:
                    shown = f'{found[0]}, offset to top {hex(found[1])}' if found else 'no RTTI'
                    problems.append(f'{name} {entry["mac"]} is not the vtable of {item["rtti"]} '
                                    f'(offset to top {item["offset_to_top"]}): {shown}')
                continue
            found = image.operands(int(item['at'], 16))
            if found is None:
                problems.append(f'{name}: cannot decode the instruction at {item["at"]}')
            elif found['target'] != address:
                reached = hex(found['target']) if found['target'] is not None else 'nothing'
                problems.append(f'{name}: the instruction at {item["at"]} reaches {reached}, not {entry["mac"]}')
    return problems


def checkLayouts(image, mapping, slice_):
    """Each layout's evidence in this slice still shows what the mapping records, and gives its value."""
    problems = []
    addresses = {a['name']: int(a['mac'], 16) for a in mapping['slices'][image.architecture]['addresses']}
    for layout in mapping.get('layouts', ()):
        name, relation = layout['name'], layout.get('relation', 'equal')
        evidence = layout['evidence'].get(image.architecture, ())
        if layout['value'] is None or not evidence:
            continue
        value = number(layout['value'])
        shown = []
        for item in evidence:
            if 'vtable' in item:
                slot = int(item['slot_offset'], 16)
                if image.pointer(int(item['vtable'], 16) + slot) != int(item['points_to'], 16):
                    problems.append(f'{name}: vtable {item["vtable"]} + {item["slot_offset"]} is not {item["points_to"]}')
                shown.append(slot // 8 if relation == 'slot-index' else slot)
                continue
            found = image.operands(int(item['at'], 16))
            if found is None:
                problems.append(f'{name}: cannot decode the instruction at {item["at"]}')
                continue
            if 'target' in item:
                if found['target'] != int(item['target'], 16):
                    problems.append(f'{name}: the instruction at {item["at"]} does not reach {item["target"]}')
                shown.append(int(item['target'], 16))
            else:
                if number(item['imm']) not in found['imms']:
                    problems.append(f'{name}: the instruction at {item["at"]} does not encode {item["imm"]}')
                shown.append(number(item['imm']))
        if relation == 'sum':
            given = sum(shown)
        elif relation.startswith('target-minus:'):
            given = [s - addresses[relation.split(':', 1)[1]] for s in shown]
        elif relation.startswith('minus:'):
            given = [s - int(relation.split(':', 1)[1], 16) for s in shown]
        elif relation.startswith('plus:'):
            given = [s + int(relation.split(':', 1)[1], 16) for s in shown]
        else:
            given = shown
        if (given if relation == 'sum' else set(given)) != (value if relation == 'sum' else {value}):
            text = hex(given) if relation == 'sum' else ', '.join(hex(g) for g in sorted(set(given)))
            problems.append(f'{name}: the evidence gives {text}, not {layout["value"]}')
    return problems


def checkSlice(image, mapping, data):
    problems = []
    slice_ = mapping['slices'][image.architecture]
    if image.uuid != slice_['uuid']:
        return [f'UUID {image.uuid} is not the mapping\'s {slice_["uuid"]}']
    for entry in slice_['addresses']:
        address = int(entry['mac'], 16)
        if entry['name'] in data:
            if image.isCode(address) or image.segment(address) is None:
                problems.append(f'{entry["name"]} {entry["mac"]} is not in a data segment')
        elif address not in image.functions:
            problems.append(f'{entry["name"]} {entry["mac"]} is not a function start')
    for entry in slice_['signatures']:
        expected = bytes.fromhex(entry['bytes'])
        if image.read(int(entry['mac'], 16), len(expected)) != expected:
            problems.append(f'signature {entry.get("name", "")} {entry["mac"]} does not match the file')
    for entry in slice_['frames']:
        for key in ('mac', 'parent', 'grandparent'):
            if not entry.get(key):
                continue
            address = int(entry[key], 16)
            if not image.isCode(address) or not image.followsCall(address):
                problems.append(f'frame {key} {entry[key]} does not follow a call')
    return problems + checkData(image, mapping, slice_, data) + checkLayouts(image, mapping, slice_)


def main(arguments):
    if not 2 <= len(arguments) <= 3:
        sys.exit(__doc__)
    with open(arguments[1]) as file:
        mapping = json.load(file)
    failed = False
    names = dataNames()
    game = os.path.join(STEAM_COMMON, STEAM_GAMES[mapping.get('edition', 24)], 'fm.app')
    for image in slices(arguments[2] if len(arguments) == 3 else game, mapping.get('image')):
        if image.architecture not in mapping['slices']:
            print(f'{image.architecture}: not in the mapping')
            failed = True
            continue
        problems = checkSlice(image, mapping, names)
        data = mapping['slices'][image.architecture]
        layouts = sum(1 for layout in mapping.get('layouts', ()) if layout['evidence'].get(image.architecture))
        shown = sum(1 for entry in data['addresses'] if entry['name'] in names and entry.get('evidence'))
        summary = f'{len(data["addresses"])} addresses, {len(data["signatures"])} signatures, {len(data["frames"])} frames'
        if shown:
            summary += f', {shown} vtables and globals with evidence'
        if 'layouts' in mapping:
            summary += f', {layouts} layouts with evidence'
        print(f'{image.architecture} {image.uuid}: {"ok" if not problems else "FAILED"} ({summary})')
        for problem in problems:
            print(f'  {problem}')
        failed = failed or bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main(sys.argv)
