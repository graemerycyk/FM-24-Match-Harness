# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The ordered calls and stream writes of a function, from llvm-objdump's disassembly.

An event is (kind, value, ret):
- 'C' a direct call (value: the target), 'T' a tail jump out of the function (no return address);
- 'W' a call through the stream vtable's write slot (+0x30; value: the length when it is a
  constant, '?' when computed), 'WT' the same as a tail jump;
- 'V' another virtual call (value: the slot's offset), 'VT' as a tail jump;
- 'I' an indirect call through a register of unknown origin.
`ret` is the call's return address: what the frame-pointer chain holds while the callee runs."""
import functools
import os
import re
import subprocess

WRITE_SLOT = 0x30
LINE = re.compile(r'^\s*([0-9a-f]+):\s*(\S+)\s*(.*)$')


def objdump():
    return os.environ.get('LLVM_OBJDUMP') or 'llvm-objdump'


def disassemble(image, start, end):
    command = [objdump(), '-d', '--no-show-raw-insn', '--print-imm-hex',
               f'--start-address=0x{start:x}', f'--stop-address=0x{end:x}', image.path]
    if objdump() == 'llvm-objdump':
        command = ['xcrun'] + command
    out = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    lines = []
    for line in out.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        if start <= address < end:
            arguments = re.sub(r'\s*<[^>]*>', '', match.group(3)).split(';')[0].split('##')[0].strip()
            lines.append((address, match.group(2), arguments))
    return lines


@functools.lru_cache(maxsize=None)
def _events(image, start):
    bounds = image.function(start)
    if not bounds or bounds[0] != start:
        return ()
    return tuple(fromLines(image, disassemble(image, *bounds), bounds))


def events(image, start):
    return _events(image, start)


def fromLines(image, lines, bounds):
    """The events of a function, from its disassembly (disassemble's lines) and its bounds."""
    if image.architecture == 'arm64':
        return _arm64(lines, bounds)
    return _x86(lines, bounds)


def _arm64(lines, bounds):
    out = []
    loads = {}    # register -> offset of the LDR that loaded it from another register
    length = None  # w2/x2, the write's length
    for address, op, arguments in lines:
        if op == 'ldr':
            match = re.match(r'(x\d+), \[(x\d+)(?:, #(0x[0-9a-f]+|\d+))?\]$', arguments)
            if match:
                loads[match.group(1)] = int(match.group(3), 0) if match.group(3) else 0
        if op in ('mov', 'orr'):
            match = re.match(r'[wx]2, #(-?0x[0-9a-f]+|-?\d+)', arguments)
            if match:
                length = int(match.group(1), 0)
            elif re.match(r'[wx]2,', arguments):
                length = '?'
        elif re.match(r'[wx]2,', arguments) and op not in (
                'str', 'stp', 'strb', 'strh', 'stur', 'cmp', 'cbz', 'cbnz', 'tbz', 'tbnz', 'tst'):
            length = '?'
        if op == 'bl':
            out.append(('C', int(arguments.split()[0], 16), address + 4))
            length, loads = None, {}
        elif op in ('blr', 'br'):
            offset = loads.get(arguments.split()[0])
            tail = op == 'br'
            if offset == WRITE_SLOT:
                out.append(('WT' if tail else 'W', length, None if tail else address + 4))
            elif offset is not None:
                out.append(('VT' if tail else 'V', offset, None if tail else address + 4))
            elif not tail:
                out.append(('I', None, address + 4))
            length, loads = None, {}
        elif op == 'b':
            try:
                target = int(arguments.split()[0], 16)
            except ValueError:
                continue
            if not bounds[0] <= target < bounds[1]:
                out.append(('T', target, None))
    return out


def _x86(lines, bounds):
    out = []
    length = None  # edx/rdx, the write's length
    for index, (address, op, arguments) in enumerate(lines):
        following = lines[index + 1][0] if index + 1 < len(lines) else bounds[1]
        match = re.match(r'\$(-?0x[0-9a-f]+|-?\d+), %(?:edx|rdx)$', arguments)
        if op.startswith('mov') and match:
            length = int(match.group(1), 0)
        elif re.search(r', %(?:edx|rdx|dx|dl)$', arguments) or (op.startswith('xor') and '%edx, %edx' in arguments):
            length = 0 if op.startswith('xor') else '?'
        if op in ('callq', 'call'):
            if arguments.startswith('*'):
                match = re.match(r'\*(?:(-?0x[0-9a-f]+|\d+))?\(%\w+\)$', arguments)
                if match:
                    offset = int(match.group(1), 0) if match.group(1) else 0
                    out.append(('W', length, following) if offset == WRITE_SLOT else ('V', offset, following))
                else:
                    out.append(('I', None, following))
            else:
                out.append(('C', int(arguments.split()[0], 16), following))
            length = None
        elif op in ('jmpq', 'jmp'):
            if arguments.startswith('*'):
                match = re.match(r'\*(?:(-?0x[0-9a-f]+|\d+))?\(%\w+\)$', arguments)
                if match:
                    offset = int(match.group(1), 0) if match.group(1) else 0
                    out.append(('WT', length, None) if offset == WRITE_SLOT else ('VT', offset, None))
                length = None
                continue
            try:
                target = int(arguments.split()[0], 16)
            except ValueError:
                continue
            if not bounds[0] <= target < bounds[1]:
                out.append(('T', target, None))
    return out
