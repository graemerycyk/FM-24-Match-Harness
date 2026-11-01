# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Merges a batch's committed chunks into one file of each kind per arm.

  python3 -I tools/batch/merge.py <batch folder> [--arm A] [--out D] [--include-suspect]

For each arm (or only A) it streams the committed chunks in index order into
<arm>-results.csv.gz, <arm>-match-players.csv.gz, <arm>-match-teams.csv.gz and, if any seed
failed to rebuild, <arm>-failed.csv.gz, each with one header, in D (default: <name>-merged in
the current folder). Every chunk file is checked against its chunk.json as it is read. Suspect
chunks, which suspect.csv lists, and a chunk 0 whose chunk.json records that it differs from the
canary reference, are left out unless --include-suspect is given. <arm>-merge.json names
the chunks merged and left out, with their files' SHA-256s, and each output's rows and
SHA-256s, compressed and not. Chunks not yet committed are not merged; the merge manifest says
how many of the planned chunks were. The files compress as `gzip -n -6` does, so merging the
same chunks gives the same bytes. Each file is written beside its place and renamed into it."""
import argparse
import datetime
import hashlib
import json
import os
import sys

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fm24batch  # noqa: E402

MERGE_FORMAT = 'fm24-batch-merge/1'


class HashingWriter:
    """A binary file that hashes and counts what is written to it."""

    def __init__(self, file):
        self.file = file
        self.digest = hashlib.sha256()
        self.bytes = 0

    def write(self, data):
        self.digest.update(data)
        self.bytes += len(data)
        return self.file.write(data)

    def flush(self):
        self.file.flush()


def merge_arm(batch, arm, out, include_suspect):
    """Writes an arm's merged files and its merge manifest to `out`; returns the manifest."""
    os.makedirs(out, exist_ok=True)
    chunks = batch.chunks(arm)
    merged = [chunk for chunk in chunks if include_suspect or not batch.is_suspect(chunk)]
    left_out = [chunk for chunk in chunks if chunk not in merged]
    roles = [role for role in fm24batch.ROLES
             if role != 'failed' or any(chunk.file_name(role) for chunk in merged)]
    manifest = {
        'format': MERGE_FORMAT,
        'batch': batch.name,
        'plan_sha256': batch.settings['plan_sha256'],
        'arm': arm,
        'created_at': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'include_suspect': include_suspect,
        'chunks_planned': batch.planned_chunks(),
        'chunks_committed': len(chunks),
        'chunks': [describe(batch, chunk) for chunk in merged],
        'left_out': [dict(describe(batch, chunk), reason='suspect') for chunk in left_out],
        'outputs': [write_role(os.path.join(out, '%s-%s.csv.gz' % (arm, role)), role, merged)
                    for role in roles],
    }
    text = (json.dumps(manifest, indent=1) + '\n').encode('utf-8')
    fm24batch.write_atomically(os.path.join(out, '%s-merge.json' % arm), lambda f: f.write(text))
    return manifest


def write_role(path, role, chunks):
    """Writes the header and then each chunk's rows of `role` to `path`, compressed; returns its
    entry in the merge manifest."""
    entry = {'name': os.path.basename(path)}

    def write(file):
        hashing = HashingWriter(file)
        raw = hashlib.sha256()
        rows = -1  # the header is not a row
        text = fm24batch.gzip_writer(hashing)
        for line in lines(role, chunks):
            raw.update(line.encode('utf-8'))
            text.write(line)
            rows += 1
        text.close()
        entry.update(rows=rows, bytes=hashing.bytes, sha256=hashing.digest.hexdigest(),
                     raw_sha256=raw.hexdigest())

    fm24batch.write_atomically(path, write)
    return entry


def lines(role, chunks):
    yield fm24batch.HEADERS[role] + '\n'
    for chunk in chunks:
        yield from chunk.lines(role)


def describe(batch, chunk):
    return {
        'first_index': chunk.first,
        'count': chunk.count,
        'path': os.path.relpath(chunk.path, batch.folder).replace(os.sep, '/'),
        'files': [{'name': entry['name'], 'sha256': entry['sha256'],
                   'raw_sha256': entry['raw_sha256']} for entry in chunk.manifest['files']],
    }


def main(arguments):
    parser = argparse.ArgumentParser(description="Merges a batch's committed chunks per arm.")
    parser.add_argument('batch', help='the batch folder')
    parser.add_argument('--arm', help='only this arm')
    parser.add_argument('--out', help='the folder to write to (default: <name>-merged)')
    parser.add_argument('--include-suspect', action='store_true',
                        help='also merge the suspect chunks')
    options = parser.parse_args(arguments)
    try:
        batch = fm24batch.Batch(options.batch)
        arms = [options.arm] if options.arm else batch.arms
        out = options.out or batch.name + '-merged'
        os.makedirs(out, exist_ok=True)
        for arm in arms:
            manifest = merge_arm(batch, arm, out, options.include_suspect)
            print('arm %s: %d of %d chunks merged, %d left out as suspect; %s' % (
                arm, len(manifest['chunks']), manifest['chunks_planned'],
                len(manifest['left_out']), ', '.join(
                    '%s %d rows' % (output['name'], output['rows'])
                    for output in manifest['outputs'])))
    except (fm24batch.BatchError, OSError, ValueError, KeyError) as error:
        sys.stderr.write('merge: %s\n' % error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
