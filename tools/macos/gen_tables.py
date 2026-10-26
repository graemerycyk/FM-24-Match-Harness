# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/dll/game_macos_tables.c from a macOS build's mapping (tools/macos/<build>.json).

  python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c

The mapping holds, per architecture slice, the counterparts of the harness's Windows RVAs and of
game.h's RVA_MACOS_* IDs (`addresses`), the frame translation entries (`frames`: macOS return
address, optional parent frame, the Windows frames it stands for) and the signature checks. All
macOS addresses are unslid (the image's header is at 0x100000000).

It refuses a mapping that leaves out an RVA the harness uses (src/dll/game.h) or a write site it
labels (src/dll/sites.h)."""
import json
import os
import re
import sys

IMAGE_BASE = 0x100000000
FRAME_MAX = 4
SIGNATURE_MAX = 32
ARCHITECTURES = (('arm64', '__aarch64__'), ('x86_64', '__x86_64__'))
SOURCES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'dll')
# Not looked up: fm.exe's TLS (no macOS counterpart) and the bound of the Windows serialiser that the
# translated frames are compared with.
UNMAPPED = ('RVA_TLS_INDEX', 'RVA_TLS_ON_DEMAND_INIT', 'RVA_GAME_MATCH_SERIALIZE_END')
SITE_BOUNDS = ('SITE_TACTIC_START', 'SITE_TACTIC_END')  # a function's bounds, not write sites


def enumerators(path, enums):
    """Name -> value of the enumerators of the named enums (regular expressions) in a header."""
    text = open(path).read()
    out = {}
    for name in enums:
        for body in re.findall(r'enum ' + name + r' \{(.*?)\};', text, re.S):
            for key, value in re.findall(r'\b([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)', body):
                out[key] = int(value, 16)
    return out


def check_coverage(name, data):
    rvas = enumerators(
        os.path.join(SOURCES, 'game.h'), ('GameFunction', 'GameData', 'MacosGameFunction', 'MacosGameData'))
    sites = enumerators(os.path.join(SOURCES, 'sites.h'), (r'\w+',))
    if not rvas or not sites:
        raise ValueError('cannot read the RVAs in src/dll/game.h or the sites in src/dll/sites.h')
    mapped = {rva(a['windows']) for a in data['addresses']}
    framed = {rva(w) for f in data['frames'] for w in f['win']}
    missing = [key for key, value in rvas.items() if key not in UNMAPPED and value not in mapped]
    missing += [key for key, value in sites.items() if key not in SITE_BOUNDS and value not in framed]
    if missing:
        raise ValueError(f'{name}: the mapping leaves out {", ".join(sorted(missing))}')


def offset(text):
    value = int(text, 16)
    if not IMAGE_BASE <= value < IMAGE_BASE + 0x10000000:
        raise ValueError(f'{text} is not an unslid image address')
    return value - IMAGE_BASE


def rva(text):
    value = int(text, 16)
    if not 0 < value < 0x80000000:
        raise ValueError(f'{text} is not a Windows RVA')
    return value


def emit_slice(out, name, data):
    check_coverage(name, data)
    addresses = sorted((rva(a['windows']), offset(a['mac']), a.get('name', '')) for a in data['addresses'])
    for previous, current in zip(addresses, addresses[1:]):
        if previous[0] == current[0]:
            raise ValueError(f'{name}: two addresses for RVA {previous[0]:#x}')
    frames = []
    for f in data['frames']:
        windows = [rva(w) for w in f['win']]
        if len(windows) > FRAME_MAX:
            raise ValueError(f'{name}: {f["mac"]} stands for more than {FRAME_MAX} frames')
        frames.append((offset(f['mac']), offset(f['parent']) if f.get('parent') else 0, windows))
    frames.sort(key=lambda f: (f[0], f[1]))
    for previous, current in zip(frames, frames[1:]):
        if previous[:2] == current[:2]:
            raise ValueError(f'{name}: two frame entries for {previous[0]:#x}/{previous[1]:#x}')
    out.append('static const MacosAddress addresses[] = {')
    for windows, mac, label in addresses:
        out.append(f'\t{{0x{windows:X}, 0x{mac:X}}}, // {label}' if label else f'\t{{0x{windows:X}, 0x{mac:X}}},')
    out.append('};')
    out.append('')
    out.append('static const MacosFrame frames[] = {')
    for mac, parent, windows in frames:
        listed = ', '.join(f'0x{w:X}' for w in windows)
        out.append(f'\t{{0x{mac:X}, 0x{parent:X}, {len(windows)}, {{{listed}}}}},')
    out.append('};')
    out.append('')
    out.append('static const MacosSignature signatures[] = {')
    for s in data['signatures']:
        code = bytes.fromhex(s['bytes'])
        if not 0 < len(code) <= SIGNATURE_MAX:
            raise ValueError(f'{name}: signature {s["mac"]} must hold 1-{SIGNATURE_MAX} bytes')
        listed = ', '.join(f'0x{b:02X}' for b in code)
        out.append(f'\t{{0x{offset(s["mac"]):X}, {len(code)}, {{{listed}}}}}, // {s.get("name", "")}')
    out.append('};')
    out.append('')
    out.append('const MacosTables macosTables = {')
    out.append(f'\t.architecture = "{name}",')
    out.append('\t.addresses = addresses,')
    out.append('\t.addressCount = sizeof addresses / sizeof addresses[0],')
    out.append('\t.frames = frames,')
    out.append('\t.frameCount = sizeof frames / sizeof frames[0],')
    out.append('\t.signatures = signatures,')
    out.append('\t.signatureCount = sizeof signatures / sizeof signatures[0],')
    out.append('};')


def main(source, target):
    mapping = json.load(open(source))
    out = [
        '// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>',
        '// SPDX-License-Identifier: GPL-3.0-or-later',
        '',
        f'// GENERATED by tools/macos/gen_tables.py from tools/macos/{source.split("/")[-1]}',
        f'// ({mapping["build"]}); do not edit. See game_macos.h and docs/MACOS.md.',
        '',
        '#include "game_macos.h"',
        '',
        '',
    ]
    # One entry per line, as generated: clang-format would put each signature byte on its own line.
    out.append('// clang-format off')
    for position, (name, macro) in enumerate(ARCHITECTURES):
        out.append(f'#{"if" if position == 0 else "elif"} defined({macro})')
        out.append('')
        emit_slice(out, name, mapping['slices'][name])
        out.append('')
    out.append('#else')
    out.append('#error "the macOS harness supports arm64 and x86_64"')
    out.append('#endif')
    out.append('// clang-format on')
    with open(target, 'w') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
