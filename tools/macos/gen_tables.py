# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates an edition's build tables (src/dll/game_macos_tables*.c) from its builds' mappings.

  python3 -I tools/macos/gen_tables.py tools/macos/steam-24.4.2.json src/dll/game_macos_tables.c
  python3 -I tools/macos/gen_tables.py tools/macos/steam-27.*.json src/dll/game_macos_tables_fm27.c

A mapping (tools/macos/<build>.json) holds, per architecture slice, the counterparts of the
harness's Windows RVAs and of game.h's RVA_MACOS_* IDs (`addresses`), the frame translation entries
(`frames`: macOS return address, optional parent and, for a plugin edition, grandparent frame, the
Windows frames it stands for) and the signature checks. Its macOS addresses are unslid: offsets from
`image_base` (by default 0x100000000, where an executable's header is).

An FM24 mapping (one build, no `edition`) gives the tables `macosTables`. The mappings of an edition
whose engine is a plugin give one set of tables per build and slice, in `macosBuildTables`, which
the harness picks from by the running slice's UUID; their `layouts` must match the edition's layout
header (src/dll/game_layout_fm27.h). Their `sites` give the synthetic IDs (0x7E000000 + n) their
frames use for writes FM24 has not; the named ones are written into the edition's sites header
(src/dll/sites_fm27.h, next to the tables).

It refuses a mapping that leaves out an ID the harness resolves (src/dll/game.h) or a write site it
labels (src/dll/sites.h) and does not list it in `absent`, unless its frames are `sites_pending`,
and frames that use a synthetic ID its `sites` do not give."""
import json
import os
import re
import sys
import textwrap

IMAGE_BASE = 0x100000000
FRAME_MAX = 4
SIGNATURE_MAX = 32
ARCHITECTURES = (('arm64', '__aarch64__'), ('x86_64', '__x86_64__'))
SOURCES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src', 'dll')
# Not looked up: fm.exe's TLS (no macOS counterpart) and the bound of the Windows serialiser that the
# translated frames are compared with.
UNMAPPED = ('RVA_TLS_INDEX', 'RVA_TLS_ON_DEMAND_INIT', 'RVA_GAME_MATCH_SERIALIZE_END')
SITE_BOUNDS = ('SITE_TACTIC_START', 'SITE_TACTIC_END')  # a function's bounds, not write sites
ID_ENUMS = ('GameFunction', 'GameData', 'MacosGameFunction', 'MacosGameData')
PLUGIN_ID_ENUMS = ('PluginGameFunction', 'PluginGameData')
# The editions whose engine is a plugin, the header that holds their builds' layouts and the one the
# generator writes their named synthetic sites into.
PLUGIN_EDITIONS = {27: ('game_layout_fm27.h', 'sites_fm27.h')}
SYNTHETIC = (0x7E000000, 0x7F000000)
LAYOUT_ENUMS = ('MatchLayout', 'ManagerLayout', 'DatabaseLayout', 'GameStateLayout', 'ThreadRngLayout',
                'MatchStatsLayout')
NO_EVIDENCE = ('unverified', 'unresolved')  # layouts that may lack evidence; unresolved ones lack a value


def readText(path):
    with open(path) as file:
        return file.read()


def enumerators(path, enums):
    """Name -> value of the enumerators of the named enums (regular expressions) in a header."""
    text = readText(path)
    out = {}
    for name in enums:
        for body in re.findall(r'enum ' + name + r' \{(.*?)\};', text, re.S):
            for key, value in re.findall(r'\b([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)', body):
                out[key] = int(value, 16)
    return out


def layoutValues(path):
    """Name -> value of the layout enums' enumerators in a layout header (hex or decimal, signed)."""
    text = readText(path)
    out = {}
    for name in LAYOUT_ENUMS:
        for body in re.findall(r'enum ' + name + r' \{(.*?)\};', text, re.S):
            for key, value in re.findall(r'\b([A-Z][A-Z0-9_]+)\s*=\s*(-?(?:0x[0-9A-Fa-f]+|\d+))', body):
                out[key] = int(value, 0)
    if not out:
        raise ValueError(f'cannot read the layouts in {path}')
    return out


def edition(mapping):
    return mapping.get('edition', 24)


def plugin(mapping):
    return edition(mapping) in PLUGIN_EDITIONS


def check_coverage(name, data, mapping):
    enums = ID_ENUMS + (PLUGIN_ID_ENUMS if plugin(mapping) else ())
    rvas = enumerators(os.path.join(SOURCES, 'game.h'), enums)
    sites = enumerators(os.path.join(SOURCES, 'sites.h'), (r'\w+',))
    if not rvas or not sites:
        raise ValueError('cannot read the RVAs in src/dll/game.h or the sites in src/dll/sites.h')
    mapped = {rva(a['windows']) for a in data['addresses']}
    framed = {rva(w) for f in data['frames'] for w in f['win']}
    named = {a['name'] for a in mapping.get('absent', ()) if 'name' in a}
    for key in named:
        entry = next(a for a in mapping['absent'] if a.get('name') == key)
        if key in rvas:
            unmapped = rvas[key] not in mapped
        elif key in sites and key not in SITE_BOUNDS:
            unmapped = sites[key] not in framed
        else:
            unmapped = False
        if not unmapped or not entry.get('reason'):
            raise ValueError(f'{name}: {key} is listed absent, but is not an unmapped ID or site with a reason')
    missing = [key for key, value in rvas.items()
               if key not in UNMAPPED and key not in named and value not in mapped]
    if not mapping.get('sites_pending'):
        missing += [key for key, value in sites.items()
                    if key not in SITE_BOUNDS and key not in named and value not in framed]
    if missing:
        raise ValueError(f'{name}: the mapping leaves out {", ".join(sorted(missing))}')


def check_layouts(source, mapping):
    """The mapping's layouts are the edition's layout header's: the same names, the same values, and
    evidence for each in some slice but those it says are unverified; an unresolved layout has no value
    and is not in the header."""
    header = layoutValues(os.path.join(SOURCES, PLUGIN_EDITIONS[edition(mapping)][0]))
    listed = set()
    for entry in mapping.get('layouts', ()):
        key = entry['name']
        listed.add(key)
        if entry['value'] is None:
            if entry.get('confidence') != 'unresolved' or key in header:
                raise ValueError(f'{source}: {key} has no value, but is not unresolved and left out of the header')
            continue
        if header.get(key) != int(entry['value'], 0):
            found = 'nothing' if key not in header else hex(header[key])
            raise ValueError(f'{source}: {key} is {entry["value"]}, but the layout header has {found}')
        if entry.get('confidence') not in NO_EVIDENCE and not any(entry['evidence'].values()):
            raise ValueError(f'{source}: {key} has no evidence')
    unlisted = sorted(set(header) - listed)
    if unlisted:
        raise ValueError(f'{source}: the mapping has no layouts for {", ".join(unlisted)}')


def offset(text, base=IMAGE_BASE):
    value = int(text, 16)
    if not base <= value < base + 0x10000000:
        raise ValueError(f'{text} is not an unslid image address')
    return value - base


def rva(text):
    value = int(text, 16)
    if not 0 < value < 0x80000000:
        raise ValueError(f'{text} is not a Windows RVA')
    return value


def synthetic(source, mapping):
    """The mapping's synthetic IDs: {ID: name or None}, refusing ones outside the range or given twice."""
    out = {}
    for entry in mapping.get('sites', ()):
        first = int(entry['id'], 16)
        count = entry.get('count', 1)
        for value in range(first, first + count):
            if not SYNTHETIC[0] <= value < SYNTHETIC[1] or value in out:
                raise ValueError(f'{source}: synthetic ID {value:#x} is outside 0x7E000000-0x7EFFFFFF or given twice')
            out[value] = entry.get('name')
    return out


def namedSites(mappings):
    """The named synthetic sites of an edition's mappings: [(name, first ID, count, note)], refusing a
    name or ID two mappings give differently."""
    out = {}
    for source, mapping in mappings:
        for entry in mapping.get('sites', ()):
            if 'name' not in entry:
                continue
            item = (int(entry['id'], 16), entry.get('count', 1), entry.get('note', ''))
            if out.setdefault(entry['name'], item)[:2] != item[:2]:
                raise ValueError(f'{source}: {entry["name"]} is not the ID the other mappings give it')
    ids = [(first, name) for name, (first, count, _) in out.items()]
    if len({first for first, _ in ids}) != len(ids):
        raise ValueError('two named synthetic sites share an ID')
    return sorted((name, first, count, note) for name, (first, count, note) in out.items())


def emit_sites(path, mappings, header):
    out = [
        '// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>',
        '// SPDX-License-Identifier: GPL-3.0-or-later',
        '',
        '// GENERATED by tools/macos/gen_tables.py from the `sites` of the mappings; do not edit.',
        '// Synthetic IDs of the serialiser frames FM24 has no counterpart for, as the build tables',
        '// translate them (0x7E000000 + n, below game.h\'s IDs). A range names its first and last.',
        '#pragma once',
        '',
        '',
        f'enum {header} {{',
    ]
    for name, first, count, note in sorted(namedSites(mappings), key=lambda s: s[1]):
        out += ['\t// ' + line for line in textwrap.wrap(note, 92)]
        if count == 1:
            out.append(f'\t{name} = 0x{first:08X},')
        else:
            out.append(f'\t{name}_FIRST = 0x{first:08X},')
            out.append(f'\t{name}_LAST = 0x{first + count - 1:08X},')
    out.append('};')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


def emit_slice(out, name, data, mapping, suffix=''):
    """The slice's arrays, named with `suffix`; returns whether it has frames."""
    check_coverage(name, data, mapping)
    base = int(mapping.get('image_base', hex(IMAGE_BASE)), 16)
    addresses = sorted((rva(a['windows']), offset(a['mac'], base), a.get('name', '')) for a in data['addresses'])
    for previous, current in zip(addresses, addresses[1:]):
        if previous[0] == current[0]:
            raise ValueError(f'{name}: two addresses for RVA {previous[0]:#x}')
    ids = synthetic(name, mapping)
    frames = []
    for f in data['frames']:
        windows = [rva(w) for w in f['win']]
        if len(windows) > FRAME_MAX:
            raise ValueError(f'{name}: {f["mac"]} stands for more than {FRAME_MAX} frames')
        unknown = [w for w in windows if SYNTHETIC[0] <= w < SYNTHETIC[1] and w not in ids]
        if unknown:
            raise ValueError(f'{name}: {f["mac"]} uses the synthetic ID {unknown[0]:#x}, which `sites` does not give')
        if f.get('grandparent') and not (plugin(mapping) and f.get('parent')):
            raise ValueError(f'{name}: {f["mac"]} has a grandparent without a parent, or in an edition without them')
        frames.append((offset(f['mac'], base), offset(f['parent'], base) if f.get('parent') else 0,
                       offset(f['grandparent'], base) if f.get('grandparent') else 0, windows))
    frames.sort(key=lambda f: (f[0], f[1], f[2]))
    for previous, current in zip(frames, frames[1:]):
        if previous[:3] == current[:3]:
            raise ValueError(f'{name}: two frame entries for {previous[0]:#x}/{previous[1]:#x}/{previous[2]:#x}')
    out.append(f'static const MacosAddress addresses{suffix}[] = {{')
    for windows, mac, label in addresses:
        out.append(f'\t{{0x{windows:X}, 0x{mac:X}}}, // {label}' if label else f'\t{{0x{windows:X}, 0x{mac:X}}},')
    out.append('};')
    out.append('')
    if frames or not suffix:
        out.append(f'static const MacosFrame frames{suffix}[] = {{')
        for mac, parent, grand, windows in frames:
            listed = ', '.join(f'0x{w:X}' for w in windows) or '0'  # C17 has no empty initialiser
            if plugin(mapping):
                out.append(f'\t{{0x{mac:X}, 0x{parent:X}, {len(windows)}, {{{listed}}}, 0x{grand:X}}},')
            else:
                out.append(f'\t{{0x{mac:X}, 0x{parent:X}, {len(windows)}, {{{listed}}}}},')
        out.append('};')
        out.append('')
    out.append(f'static const MacosSignature signatures{suffix}[] = {{')
    for s in data['signatures']:
        code = bytes.fromhex(s['bytes'])
        if not 0 < len(code) <= SIGNATURE_MAX:
            raise ValueError(f'{name}: signature {s["mac"]} must hold 1-{SIGNATURE_MAX} bytes')
        listed = ', '.join(f'0x{b:02X}' for b in code)
        out.append(f'\t{{0x{offset(s["mac"], base):X}, {len(code)}, {{{listed}}}}}, // {s.get("name", "")}')
    out.append('};')
    out.append('')
    return bool(frames)


def emit_tables(out, name, declaration, suffix, frames):
    out.append(f'{declaration} = {{')
    out.append(f'\t.architecture = "{name}",')
    out.append(f'\t.addresses = addresses{suffix},')
    out.append(f'\t.addressCount = sizeof addresses{suffix} / sizeof addresses{suffix}[0],')
    if frames or not suffix:
        out.append(f'\t.frames = frames{suffix},')
        out.append(f'\t.frameCount = sizeof frames{suffix} / sizeof frames{suffix}[0],')
    out.append(f'\t.signatures = signatures{suffix},')
    out.append(f'\t.signatureCount = sizeof signatures{suffix} / sizeof signatures{suffix}[0],')
    out.append('};')


def format_version(source, mapping):
    """The version the build's serialised matches carry (bytes 4-5 of their header)."""
    version = int(mapping.get('format_version', '0'), 16)
    if not 0 < version <= 0xFFFF:
        raise ValueError(f'{source}: no format_version for the build\'s serialised matches')
    return version


def uuidBytes(text):
    raw = bytes.fromhex(text.replace('-', ''))
    if len(raw) != 16:
        raise ValueError(f'{text} is not a UUID')
    return ', '.join(f'0x{b:02X}' for b in raw)


def main(sources, target):
    mappings = [(source, json.loads(readText(source))) for source in sources]
    if len({edition(m) for _, m in mappings}) != 1:
        raise ValueError('the mappings are for more than one edition')
    many = plugin(mappings[0][1])
    if not many and len(mappings) != 1:
        raise ValueError('an edition whose engine is in its executable has one build')
    if many:
        namedSites(mappings)  # every mapping gives each named site the same ID
    mappings.sort(key=lambda m: m[1].get('game_build', 0))
    out = [
        '// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>',
        '// SPDX-License-Identifier: GPL-3.0-or-later',
        '',
    ]
    if many:
        out.append('// GENERATED by tools/macos/gen_tables.py from the mappings of the builds the harness')
        out.append('// supports; do not edit. See game_macos.h and docs/MACOS.md.')
        for source, mapping in mappings:
            out.append(f'// - tools/macos/{source.split("/")[-1]} ({mapping["version_text"]})')
    else:
        source, mapping = mappings[0]
        out.append(f'// GENERATED by tools/macos/gen_tables.py from tools/macos/{source.split("/")[-1]}')
        out.append(f'// ({mapping["build"]}); do not edit. See game_macos.h and docs/MACOS.md.')
    out += ['', '#include "game_macos.h"', '', '']
    # One entry per line, as generated: clang-format would put each signature byte on its own line.
    out.append('// clang-format off')
    for position, (name, macro) in enumerate(ARCHITECTURES):
        out.append(f'#{"if" if position == 0 else "elif"} defined({macro})')
        out.append('')
        if not many:
            frames = emit_slice(out, name, mapping['slices'][name], mapping)
            emit_tables(out, name, 'const MacosTables macosTables', '', frames)
            out.append('')
            continue
        builds = []
        for source, mapping in mappings:
            if position == 0:
                check_layouts(source, mapping)
            suffix = f'_{mapping["game_build"]}'
            data = mapping['slices'][name]
            frames = emit_slice(out, f'{source} {name}', data, mapping, suffix)
            emit_tables(out, name, f'static const MacosTables tables{suffix}', suffix, frames)
            out.append('')
            builds.append((data['uuid'], suffix, format_version(source, mapping)))
        out.append('const MacosBuildTables macosBuildTables[] = {')
        for uuid, suffix, version in builds:
            out.append(f'\t{{{{{uuidBytes(uuid)}}}, &tables{suffix}, 0x{version:04X}}},')
        out.append('};')
        out.append('')
    out.append('#else')
    out.append('#error "the macOS harness supports arm64 and x86_64"')
    out.append('#endif')
    if many:
        out.append('')
        out.append('const size_t macosBuildTableCount = sizeof macosBuildTables / sizeof macosBuildTables[0];')
    out.append('// clang-format on')
    with open(target, 'w') as f:
        f.write('\n'.join(out) + '\n')
    if many:
        sites = os.path.join(os.path.dirname(os.path.abspath(target)), PLUGIN_EDITIONS[edition(mappings[0][1])][1])
        emit_sites(sites, mappings, f'Fm{edition(mappings[0][1])}Site')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1:-1], sys.argv[-1])
