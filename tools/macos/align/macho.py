# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""One architecture slice of a game image, read for the alignment tools: its sections, function
starts (LC_FUNCTION_STARTS) and a thin copy of the slice that llvm-objdump can disassemble by
address range (it ignores the range in a universal file)."""
import atexit
import bisect
import os
import shutil
import struct
import tempfile

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
LC_FUNCTION_STARTS = 0x26
CPU_TYPES = {0x0100000C: 'arm64', 0x01000007: 'x86_64'}

_scratch = None


def scratch():
    """A private temporary folder for thin slices, removed when the tool exits."""
    global _scratch
    if _scratch is None:
        _scratch = tempfile.mkdtemp(prefix='fm-align-')
        atexit.register(shutil.rmtree, _scratch, True)
    return _scratch


class Image:
    def __init__(self, data, name):
        self.name = name
        self.data = data
        magic, cpu, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
        if magic != MH_MAGIC_64:
            raise ValueError(f'{name}: not a 64-bit Mach-O slice')
        self.architecture = CPU_TYPES.get(cpu & 0xFFFFFFFF, 'other')
        self.segments = []
        self.sections = []
        self.uuid = None
        starts = None
        command = 32
        for _ in range(ncmds):
            kind, size = struct.unpack_from('<II', data, command)
            if kind == LC_SEGMENT_64:
                segment = data[command + 8:command + 24].rstrip(b'\0').decode()
                address, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from('<QQQQiiII', data, command + 24)
                self.segments.append((segment, address, vmsize, fileoff, filesize))
                position = command + 72
                for _ in range(nsects):
                    section = data[position:position + 16].rstrip(b'\0').decode()
                    start, length = struct.unpack_from('<QQ', data, position + 32)
                    self.sections.append((segment, section, start, length))
                    position += 80
            elif kind == LC_UUID:
                raw = data[command + 8:command + 24].hex().upper()
                self.uuid = '-'.join((raw[:8], raw[8:12], raw[12:16], raw[16:20], raw[20:]))
            elif kind == LC_FUNCTION_STARTS:
                starts = struct.unpack_from('<II', data, command + 8)
            command += size
        self.text = self.section('__TEXT', '__text')
        self.stubs = self.section('__TEXT', '__stubs')
        self.functions = self._functionStarts(*starts) if starts else []
        self._path = None

    def _functionStarts(self, offset, size):
        address = next(s[1] for s in self.segments if s[0] == '__TEXT')
        out = []
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
            out.append(address)
        return out

    def section(self, segment, name):
        return next(((s[2], s[2] + s[3]) for s in self.sections if s[0] == segment and s[1] == name), None)

    def isStub(self, address):
        return bool(self.stubs and self.stubs[0] <= address < self.stubs[1])

    def function(self, address):
        """(start, end) of the function holding an address, or None outside the code."""
        if not self.text or not self.text[0] <= address < self.text[1]:
            return None
        index = bisect.bisect_right(self.functions, address) - 1
        if index < 0:
            return None
        end = self.functions[index + 1] if index + 1 < len(self.functions) else self.text[1]
        return self.functions[index], end

    def functionOf(self, address):
        bounds = self.function(address)
        return bounds[0] if bounds else None

    def isFunction(self, address):
        bounds = self.function(address)
        return bool(bounds and bounds[0] == address)

    @property
    def path(self):
        """A thin copy of the slice on disk, for llvm-objdump."""
        if self._path is None:
            self._path = os.path.join(scratch(), f'{self.name}-{self.architecture}')
            with open(self._path, 'wb') as file:
                file.write(self.data)
        return self._path


def load(path, architecture, name, image=None):
    """The slice of `architecture` in a game image: an app (its `image`, by default its executable
    Contents/MacOS/fm), or the image file itself, universal or thin."""
    path = os.path.expanduser(path)
    if path.rstrip('/').endswith('.app'):
        path = os.path.join(path, image) if image else os.path.join(path, 'Contents', 'MacOS', 'fm')
    with open(path, 'rb') as file:
        data = file.read()
    if struct.unpack_from('>I', data, 0)[0] != FAT_MAGIC:
        slices = [data]
    else:
        count = struct.unpack_from('>I', data, 4)[0]
        slices = []
        for index in range(count):
            _, _, offset, size, _ = struct.unpack_from('>iiIII', data, 8 + 20 * index)
            slices.append(data[offset:offset + size])
    for raw in slices:
        cpu = struct.unpack_from('<i', raw, 4)[0] & 0xFFFFFFFF
        if CPU_TYPES.get(cpu) == architecture:
            return Image(raw, name)
    raise ValueError(f'{path} has no {architecture} slice')
