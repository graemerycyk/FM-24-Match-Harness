# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a macOS build mapping (tools/macos/<build>.json) against the game executable.

  python3 -I tools/macos/check_mapping.py tools/macos/steam-24.4.2.json [path to fm.app or fm]

Without a path it uses the Steam install in ~/Library/Application Support/Steam. For each slice it
checks the UUID, that every function address is a function start (LC_FUNCTION_STARTS), that every
vtable and global (the data RVAs of src/dll/game.h) lies in a data segment, that the signature bytes
match the file, and that every frame entry is a return address: inside the code and right after a
call. It reads the file only."""
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
CPU_TYPES = {0x0100000C: 'arm64', 0x01000007: 'x86_64'}
GAME_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'dll', 'game.h')
STEAM_GAME = '~/Library/Application Support/Steam/steamapps/common/Football Manager 2024/fm.app'


class Slice:
    def __init__(self, data):
        self.data = data
        magic, cpu, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
        if magic != MH_MAGIC_64:
            raise ValueError('not a 64-bit Mach-O slice')
        self.architecture = CPU_TYPES.get(cpu & 0xFFFFFFFF, 'other')
        self.segments = []
        self.uuid = None
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


def slices(path):
    path = os.path.expanduser(path)
    if path.rstrip('/').endswith('.app'):
        path = os.path.join(path, 'Contents', 'MacOS', 'fm')
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
    """The names game.h gives game data (enum GameData and MacosGameData) rather than code."""
    with open(GAME_H) as file:
        text = file.read()
    names = set()
    for body in re.findall(r'enum (?:GameData|MacosGameData) \{(.*?)\};', text, re.S):
        names.update(re.findall(r'\b(RVA_[A-Z0-9_]+)\s*=', body))
    if not names:
        raise ValueError('cannot read the data RVAs in src/dll/game.h')
    return names


def checkSlice(image, mapping, data):
    problems = []
    if image.uuid != mapping['uuid']:
        return [f'UUID {image.uuid} is not the mapping\'s {mapping["uuid"]}']
    for entry in mapping['addresses']:
        address = int(entry['mac'], 16)
        if entry['name'] in data:
            if image.isCode(address) or image.segment(address) is None:
                problems.append(f'{entry["name"]} {entry["mac"]} is not in a data segment')
        elif address not in image.functions:
            problems.append(f'{entry["name"]} {entry["mac"]} is not a function start')
    for entry in mapping['signatures']:
        expected = bytes.fromhex(entry['bytes'])
        if image.read(int(entry['mac'], 16), len(expected)) != expected:
            problems.append(f'signature {entry.get("name", "")} {entry["mac"]} does not match the file')
    for entry in mapping['frames']:
        for key in ('mac', 'parent'):
            if not entry.get(key):
                continue
            address = int(entry[key], 16)
            if not image.isCode(address) or not image.followsCall(address):
                problems.append(f'frame {key} {entry[key]} does not follow a call')
    return problems


def main(arguments):
    if not 2 <= len(arguments) <= 3:
        sys.exit(__doc__)
    with open(arguments[1]) as file:
        mapping = json.load(file)
    failed = False
    names = dataNames()
    for image in slices(arguments[2] if len(arguments) == 3 else STEAM_GAME):
        if image.architecture not in mapping['slices']:
            print(f'{image.architecture}: not in the mapping')
            failed = True
            continue
        problems = checkSlice(image, mapping['slices'][image.architecture], names)
        data = mapping['slices'][image.architecture]
        summary = f'{len(data["addresses"])} addresses, {len(data["signatures"])} signatures, {len(data["frames"])} frames'
        print(f'{image.architecture} {image.uuid}: {"ok" if not problems else "FAILED"} ({summary})')
        for problem in problems:
            print(f'  {problem}')
        failed = failed or bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main(sys.argv)
