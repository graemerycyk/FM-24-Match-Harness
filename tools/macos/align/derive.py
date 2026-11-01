# SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame translations for one image derived from another image's, by aligning the two builds'
serialisers.

Each root pair (the same serialiser in both images) is flattened: every stream write and virtual
call it reaches through direct calls, with the stack of return addresses that leads to it. The two
sequences are aligned by kind and write length. For each aligned pair, the source stack translates
(through the source's frame table) into a chain of Windows frames; that chain is split over the
target stack's frames, function by function, and each target frame gets the part it stands for:
- functions are paired by how often they share aligned stacks (and by the given seeds);
- the frames of a target function that pairs with a source function carry that source frame's
  part (with the frames of target helpers it calls that pair with nothing);
- within that group, outer frames carry the suffix all their occurrences share, and the rest goes
  to the frame whose context (the frame, with its parent, with its parent and grandparent)
  always asks for it.
The result is a table of entries keyed by (frame, parent, grandparent), 0 meaning any. Replaying
every aligned pair through it gives the same chains as the source, or the pair is reported."""
import collections
import difflib

import events as eventsModule

SIZE_LIMIT = 40000
DEPTH_LIMIT = 12


def flatten(image, root, depth=DEPTH_LIMIT):
    """[(kind, value, stack)]: the writes and virtual calls reached from `root` through direct calls and
    tail jumps; stack holds the return addresses from the root inwards."""
    out = []
    active = set()

    def walk(function, stack, level):
        if level > depth or function in active:
            return
        bounds = image.function(function)
        if not bounds or bounds[0] != function or bounds[1] - bounds[0] > SIZE_LIMIT:
            return
        active.add(function)
        for kind, value, ret in eventsModule.events(image, function):
            here = stack + ((ret,) if ret else ())
            if kind in ('C', 'T'):
                if not image.isStub(value):
                    walk(value, here, level + 1)
            elif kind in ('W', 'WT'):
                out.append(('W', value, here))
            elif kind in ('V', 'VT'):
                out.append(('V', value, here))
        active.discard(function)

    walk(root, (), 0)
    return out


def tokens(trace):
    return [(('W', value) if kind == 'W' else ('V',), stack) for kind, value, stack in trace]


def alignedPairs(source, target, roots):
    """[(root name, token, source stack, target stack, block length)] for the aligned writes and
    virtual calls of each root pair."""
    out = []
    for name, sourceRoot, targetRoot in roots:
        a = tokens(flatten(source, sourceRoot))
        b = tokens(flatten(target, targetRoot))
        matcher = difflib.SequenceMatcher(None, [t for t, _ in a], [t for t, _ in b], autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag == 'equal':
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    out.append((name, a[i][0], a[i][1], b[j][1], i2 - i1))
    return out


def lookup(table, stack, k):
    """The entry for stack[k] (outer first), most specific context first, or None."""
    frame = stack[k]
    parent = stack[k - 1] if k > 0 else 0
    grand = stack[k - 2] if k > 1 else 0
    for key in ((frame, parent, grand), (frame, parent, 0), (frame, 0, 0)):
        if key in table:
            return table[key]
    return None


def translate(table, stack):
    """The Windows chain, inner first, of a stack (outer first); frames without an entry are left out
    (the harness keeps them as their own offsets, which no label uses)."""
    out = []
    for k in range(len(stack) - 1, -1, -1):
        entry = lookup(table, stack, k)
        if entry:
            out += entry
    return tuple(out)


def untranslated(table, stack):
    return [stack[k] for k in range(len(stack)) if lookup(table, stack, k) is None]


def functionPairs(pairs, source, target, seeds):
    """Source function -> target function: the target function sharing most aligned stacks with it
    (Jaccard index at least 0.5, each target function kept for its best source), then the seeds."""
    countSource = collections.Counter()
    countTarget = collections.Counter()
    both = collections.Counter()
    for _, _, a, b, _ in pairs:
        fa = set(source.functionOf(m) for m in a)
        fb = set(target.functionOf(n) for n in b)
        countSource.update(fa)
        countTarget.update(fb)
        for x in fa:
            for y in fb:
                both[(x, y)] += 1
    best = {}
    for (x, y), count in both.items():
        score = count / (countSource[x] + countTarget[y] - count)
        if score > best.get(x, (0, None))[0]:
            best[x] = (score, y)
    out = {x: y for x, (score, y) in best.items() if score >= 0.5}
    claims = collections.defaultdict(list)
    for x, y in out.items():
        claims[y].append((best[x][0], x))
    for y, claimed in claims.items():
        claimed.sort(reverse=True)
        for score, x in claimed[1:]:
            if score < claimed[0][0]:
                del out[x]
    for x, y in seeds.items():
        for other in [k for k, v in out.items() if v == y and k != x]:
            del out[other]
        out[x] = y
    return out


def consistent(pairs, fpair, source, target):
    """The pairs whose stacks agree with the function pairing: each target function on the stack that
    pairs with a source function has it on the source stack."""
    image = collections.defaultdict(set)
    for x, y in fpair.items():
        image[y].add(x)
    out = []
    for p in pairs:
        functions = {source.functionOf(m) for m in p[2]}
        if all(not image.get(target.functionOf(n)) or image[target.functionOf(n)] & functions for n in p[3]):
            out.append(p)
    return out


def segments(sourceStack, targetStack, table, fpair, source, target):
    """[(target frames outer first, Windows items inner first)], inner segment first: each source frame
    (with the unpaired ones inside it) and the target frames from its function's pair inwards."""
    functions = [target.functionOf(n) for n in targetStack]
    out = []
    pending = ()
    end = len(targetStack)
    for i in range(len(sourceStack) - 1, -1, -1):
        partner = fpair.get(source.functionOf(sourceStack[i]))
        found = next((q for q in range(end - 1, -1, -1) if functions[q] == partner), None)
        items = tuple(lookup(table, sourceStack, i) or ())
        pending = pending + items
        if found is not None:
            out.append((tuple(targetStack[found:end]), pending))
            end = found
            pending = ()
    if pending:
        if end or not out:
            out.append((tuple(targetStack[:end]), pending))
            end = 0
        else:
            frames, items = out[-1]
            out[-1] = (frames, items + pending)
    if end:
        out.append((tuple(targetStack[:end]), ()))
    return out


def commonSuffix(lists):
    lists = list(lists)
    if not lists:
        return ()
    n = 0
    while all(len(x) > n for x in lists) and len({x[len(x) - 1 - n] for x in lists}) == 1:
        n += 1
    first = lists[0]
    return first[len(first) - n:] if n else ()


def contextKey(stack, k, level):
    parent = stack[k - 1] if k > 0 and level > 0 else 0
    grand = stack[k - 2] if k > 1 and level > 1 else 0
    return (stack[k], parent, grand)


def derive(pairs, table, fpair, source, target):
    """{(frame, parent, grandparent): Windows list} for the target, and the occurrences it could not
    satisfy: [(pair, frame, wanted, given)]."""
    instances = []  # (target stack, [(segment frames outer first, list)])
    for p in pairs:
        instances.append((p[3], segments(p[2], p[3], table, fpair, source, target)))
    # Outer frames take the suffix shared by every occurrence of theirs, outer to inner.
    shared = {}
    remaining = {}
    for a, (stack, segs) in enumerate(instances):
        for b, (frames, items) in enumerate(segs):
            remaining[(a, b)] = items
    deepest = max((len(f) for _, segs in instances for f, _ in segs), default=0)
    for depth in range(deepest):
        candidates = collections.defaultdict(list)
        for a, (stack, segs) in enumerate(instances):
            for b, (frames, items) in enumerate(segs):
                if depth < len(frames) - 1:
                    candidates[frames[depth]].append(remaining[(a, b)])
        for frame, lists in candidates.items():
            shared.setdefault(frame, commonSuffix(lists))
        for a, (stack, segs) in enumerate(instances):
            for b, (frames, items) in enumerate(segs):
                if depth < len(frames) - 1:
                    take = shared[frames[depth]]
                    rest = remaining[(a, b)]
                    if take and rest[len(rest) - len(take):] == take:
                        remaining[(a, b)] = rest[:len(rest) - len(take)]
    # The rest of each segment goes to its carrier: the first frame, inner to outer, whose context
    # (alone, then with its parent, then its grandparent) only ever asks for that rest.
    asks = collections.defaultdict(set)
    for a, (stack, segs) in enumerate(instances):
        index = {n: k for k, n in enumerate(stack)}
        for b, (frames, items) in enumerate(segs):
            for frame in frames:
                for level in range(3):
                    asks[(level, contextKey(stack, index[frame], level))].add(remaining[(a, b)])
    wanted = []  # (instance, stack index, list)
    for a, (stack, segs) in enumerate(instances):
        index = {n: k for k, n in enumerate(stack)}
        for b, (frames, items) in enumerate(segs):
            rest = remaining[(a, b)]
            # A carrier's list goes before the suffixes of the frames outside it, so the frames inside
            # it must have none.
            inner = max(len(frames) - 2, 0)
            while inner > 0 and not shared.get(frames[inner], ()):
                inner -= 1
            carrier = None
            for level in range(3):
                for d in range(len(frames) - 1, inner - 1, -1):
                    if asks[(level, contextKey(stack, index[frames[d]], level))] == {rest}:
                        carrier = frames[d]
                        break
                if carrier is not None:
                    break
            if carrier is None:
                carrier = frames[-1]
            for d, frame in enumerate(frames):
                own = shared.get(frame, ()) if d < len(frames) - 1 else ()
                wanted.append((a, index[frame], rest + own if frame == carrier else own))
    # Entries: the majority per frame, then per parent and per grandparent where they differ.
    votes = [collections.defaultdict(collections.Counter) for _ in range(3)]
    for a, k, value in wanted:
        stack = instances[a][0]
        for level in range(3):
            votes[level][contextKey(stack, k, level)][value] += 1
    out = {}
    for key, counter in votes[0].items():
        out[key] = counter.most_common(1)[0][0]
    for level in (1, 2):
        for key, counter in votes[level].items():
            top = counter.most_common(1)[0][0]
            general = None
            for wider in range(level - 1, -1, -1):
                general = out.get((key[0], key[1] if wider > 0 else 0, 0))
                if general is not None:
                    break
            if top != general:
                out[key] = top
    failures = []
    for a, k, value in wanted:
        stack = instances[a][0]
        given = lookup(out, stack, k)
        if tuple(given) != tuple(value):
            failures.append((pairs[a], stack[k], value, given))
    return {key: tuple(value) for key, value in out.items()}, failures


def replay(pairs, sourceTable, targetTable):
    """The pairs whose target stack does not translate into the source stack's chain."""
    return [p for p in pairs if translate(targetTable, p[3]) != translate(sourceTable, p[2])]
