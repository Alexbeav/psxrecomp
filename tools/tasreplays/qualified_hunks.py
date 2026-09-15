"""Qualified-hunk registry: an integrity check on source trees, not a replay verdict.

A TAS replay pass qualifies a source tree, but the accuracy changes it depends on
travel through cherry-picks, stacked branches and upstream merges, where a hunk
can be dropped without any build or unit test noticing (b1413e71's four-line
dirty_ram_interp.c hunk was lost this way on 2026-09-14 and only a full replay
divergence found it). The registry (qualified-hunks.json) records, per accuracy
change, the file and the exact added lines as they exist in the qualified tree;
`check` verifies those lines are still present, contiguously, in any target
revision. Presence proves nothing about behaviour; absence is a definite finding.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

SCHEMA = 'psx-tas-qualified-hunks-v1'
QUALIFYING = ('runtime/src/', 'runtime/include/', 'recompiler/src/', 'tools/tasreplays/tekken3-codegen.json')
DEFAULT_PREFIXES = ('runtime/src', 'runtime/include', 'recompiler/src')
SPLIT_REVS = ['origin/split/01-standalone-fixes', 'origin/split/02-source-timing-core',
              'origin/split/03-tas-replay-tooling', 'origin/split/04-gpu-source-projection',
              'origin/split/05-controller-card-replay', 'origin/split/06-cdrom-mdec-source',
              'origin/split/07-campaign-docs', 'HEAD']
HERE = Path(__file__).resolve()
DEFAULT_REGISTRY = HERE.with_name('qualified-hunks.json')
DEFAULT_REPO = HERE.parents[2]


class HunkError(Exception):
    """Usage or IO failure (exit 2)."""


def git(repo, *args, allow_fail=False):
    result = subprocess.run(['git', '-C', str(repo), *args], capture_output=True)
    if result.returncode and not allow_fail:
        raise HunkError(f"git {' '.join(args)}: {result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout if not result.returncode else None


def normalise(text):
    """CRLF -> LF, trailing whitespace stripped per line, indentation kept exact."""
    if isinstance(text, bytes):
        text = text.decode('utf-8', 'replace')
    return [line.rstrip() for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n')]


def qualifies(path, prefixes=None):
    """Repo-relative path is under a qualifying source prefix (and the requested ones)."""
    if not any(path == q.rstrip('/') or path.startswith(q) for q in QUALIFYING):
        return False
    if prefixes is None:
        return True
    return any(path == p.rstrip('/') or path.startswith(p.rstrip('/') + '/') for p in prefixes)


def extract_blocks(repo, commit, prefixes=None):
    """Contiguous added-line runs of a commit, per hunk, restricted to qualifying paths."""
    paths = [p for p in (prefixes or DEFAULT_PREFIXES)]
    out = git(repo, 'show', '--no-color', '--unified=0', '--format=', commit, '--', *paths)
    blocks, current, path, keep = [], None, None, False
    for raw in out.decode('utf-8', 'replace').split('\n'):
        if raw.startswith('diff --git '):
            path, keep, current = None, False, None
        elif raw.startswith('+++ '):
            name = raw[4:].rstrip()
            path = None if name == '/dev/null' else name[2:] if name.startswith('b/') else name
            keep = path is not None and qualifies(path, prefixes)
        elif raw.startswith('@@'):
            current = None
        elif keep and raw.startswith('+'):
            if current is None:
                current = {'file': path, 'lines': []}
                blocks.append(current)
            current['lines'].append(raw[1:].rstrip())
        elif raw.startswith('\\'):
            continue
        else:
            current = None
    return [b for b in blocks if b['lines']]


def file_lines(repo, path, rev=None):
    """Normalised lines of a repo file: at REV via git show, else from the working tree."""
    if rev is None:
        target = Path(repo) / path
        if not target.is_file():
            return None
        return normalise(target.read_bytes())
    data = git(repo, 'show', f'{rev}:{path}', allow_fail=True)
    return None if data is None else normalise(data)


def find_block(lines, block):
    """1-based line of the first contiguous occurrence of block in lines, or 0."""
    if not block or lines is None:
        return 0
    n, first = len(block), block[0]
    for i, line in enumerate(lines):
        if line == first and lines[i:i + n] == block:
            return i + 1
    return 0


def split_block(lines, block):
    """Maximal in-order sub-runs of block found contiguously in lines.

    Sub-runs need at least two lines unless the whole block is a single line;
    shorter remnants are dropped. Returns (kept_sub_blocks, dropped_line_count).
    """
    if find_block(lines, block):
        return [list(block)], 0
    kept, dropped, i, n = [], 0, 0, len(block)
    while i < n:
        if not find_block(lines, block[i:i + 1]):
            dropped += 1
            i += 1
            continue
        j = i + 1
        while j < n and find_block(lines, block[i:j + 1]):
            j += 1
        if j - i >= 2 or n == 1:
            kept.append(list(block[i:j]))
        else:
            dropped += j - i
        i = j
    return kept, dropped


def load_registry(path):
    path = Path(path)
    if not path.exists():
        return {'schema': SCHEMA, 'entries': []}
    try:
        registry = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise HunkError(f'{path}: {error}')
    if registry.get('schema') != SCHEMA or not isinstance(registry.get('entries'), list):
        raise HunkError(f'{path}: not a {SCHEMA} registry')
    return registry


def write_registry(path, registry):
    text = json.dumps(registry, indent=2, ensure_ascii=False) + '\n'
    with open(path, 'w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)


def check_entry(repo, entry, rev=None, cache=None):
    cache = {} if cache is None else cache
    blocks = []
    for block in entry['blocks']:
        key = block['file']
        if key not in cache:
            cache[key] = file_lines(repo, key, rev)
        lines = cache[key]
        at = find_block(lines, block['lines'])
        blocks.append({'file': key, 'lines': len(block['lines']), 'present': bool(at),
                       'line': at or None, 'file_missing': lines is None,
                       'first_missing_line': None if at else block['lines'][0]})
    missing = [b for b in blocks if not b['present']]
    return {'id': entry['id'], 'titles': entry.get('titles', []), 'present': not missing,
            'blocks': blocks, 'missing_blocks': len(missing), 'total_blocks': len(blocks)}


def select_entries(registry, titles):
    if not titles:
        return registry['entries']
    wanted = set(titles)
    return [e for e in registry['entries'] if wanted & set(e.get('titles', []))]


def resolve(repo, rev):
    if rev is None:
        return None
    out = git(repo, 'rev-parse', '--verify', '--quiet', f'{rev}^{{commit}}', allow_fail=True)
    if out is None:
        raise HunkError(f'unresolvable revision: {rev}')
    return out.decode().strip()


def run_check(repo, registry, rev=None, titles=None):
    resolved = resolve(repo, rev)
    cache, results = {}, []
    for entry in select_entries(registry, titles):
        results.append(check_entry(repo, entry, rev, cache))
    missing = [r for r in results if not r['present']]
    return {'schema': SCHEMA, 'rev': rev or 'worktree', 'resolved': resolved,
            'entries': results, 'missing': len(missing), 'total': len(results)}


def describe(result):
    if result['present']:
        return f"present  {result['id']}"
    bad = next(b for b in result['blocks'] if not b['present'])
    where = f"{bad['file']} (file absent)" if bad['file_missing'] else f"{bad['file']}: {bad['first_missing_line'].strip()}"
    return f"MISSING  {result['id']}  {result['missing_blocks']} of {result['total_blocks']} blocks  {where}"


def cmd_check(args):
    registry = load_registry(args.registry)
    report = run_check(args.repo, registry, args.rev, args.titles)
    report['registry'] = str(args.registry)
    for result in report['entries']:
        print(describe(result))
    print(f"missing: {report['missing']} of {report['total']} entries  ({report['rev']}"
          f"{' = ' + report['resolved'][:12] if report['resolved'] else ''})")
    if args.json:
        with open(args.json, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    return 1 if report['missing'] else 0


def cmd_check_all(args):
    registry = load_registry(args.registry)
    revs = args.revs or SPLIT_REVS
    reports = [run_check(args.repo, registry, rev, args.titles) for rev in revs]
    entries = [r['id'] for r in reports[0]['entries']] if reports else []
    width = max([len('entry')] + [len(e) for e in entries])
    labels = [rev.split('/')[-1][:24] for rev in revs]
    print('entry'.ljust(width) + ''.join(f'  {label}' for label in labels))
    for index, entry_id in enumerate(entries):
        cells = ['ok' if report['entries'][index]['present'] else 'MISSING' for report in reports]
        print(entry_id.ljust(width) + ''.join(f'  {cell.ljust(len(label))}' for cell, label in zip(cells, labels)))
    failed = 0
    for rev, report in zip(revs, reports):
        print(f"{rev} ({report['resolved'][:12]}): missing {report['missing']} of {report['total']} entries")
        failed += bool(report['missing'])
    if args.json:
        with open(args.json, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump({'schema': SCHEMA, 'registry': str(args.registry), 'revs': reports}, stream, indent=2)
            stream.write('\n')
    return 1 if failed else 0


def span_at_rev(repo, path, rev, anchor, count):
    """`count` lines of `path` at `rev`, starting at the line equal to `anchor`.

    Seeding from a commit's added lines cannot re-pin a fix whose expression a
    later commit rewrote: the rewritten lines are dropped and the entry decays
    towards whatever survived, which for a commented fix is the comment alone --
    a guard that passes on a tree where the code itself was deleted. This takes
    the fix's CURRENT lines instead, so an evolved entry keeps its teeth.
    """
    if not qualifies(path):
        raise HunkError(f'{path}: not a qualifying source path ({", ".join(QUALIFYING)})')
    lines = file_lines(repo, path, rev)
    if lines is None:
        raise HunkError(f'{path}: not present at {rev or "the working tree"}')
    anchor = anchor.rstrip()
    matches = [i for i, line in enumerate(lines) if line == anchor]
    if len(matches) != 1:
        raise HunkError(f'{path}: anchor matches {len(matches)} lines, need exactly 1: {anchor!r}')
    start = matches[0]
    if count < 1 or start + count > len(lines):
        raise HunkError(f'{path}: {count} lines from the anchor runs past the end of the file')
    return lines[start:start + count]


def seed_span(repo, path, rev, anchor, count, entry_id, titles, note=None, origin=None, subject=None):
    """An entry pinned to a contiguous span as it exists at `rev`."""
    block = {'file': path, 'lines': span_at_rev(repo, path, rev, anchor, count)}
    entry = {'id': entry_id, 'titles': list(titles),
             'origin_commit': resolve(repo, origin) if origin else resolve(repo, rev or 'HEAD'),
             'subject': subject or 'pinned span',
             'qualified_tree': resolve(repo, rev or 'HEAD'), 'blocks': [block]}
    if note:
        entry['note'] = note
    return entry


def seed_entry(repo, commit, entry_id, titles, prefixes=None, as_of=None, note=None, origin=None, subject=None):
    full = resolve(repo, commit)
    blocks = extract_blocks(repo, full, prefixes)
    entry = {'id': entry_id, 'titles': list(titles), 'origin_commit': resolve(repo, origin) if origin else full,
             'subject': subject or git(repo, 'show', '-s', '--format=%s', full).decode('utf-8', 'replace').strip(),
             'qualified_tree': resolve(repo, as_of) if as_of else None}
    if origin:
        entry['seed_commit'] = full
    if as_of:
        kept, dropped, cache = [], 0, {}
        for block in blocks:
            if block['file'] not in cache:
                cache[block['file']] = file_lines(repo, block['file'], as_of)
            sub, lost = split_block(cache[block['file']], block['lines'])
            dropped += lost
            kept.extend({'file': block['file'], 'lines': lines} for lines in sub)
        if dropped or len(kept) != len(blocks):
            entry['evolved'] = {'dropped_lines': dropped, 'from_blocks': len(blocks), 'to_blocks': len(kept)}
        blocks = kept
    if not blocks:
        raise HunkError(f'{entry_id}: no qualifying added lines survive for {commit}'
                        + (f' at {as_of}' if as_of else ''))
    entry['blocks'] = blocks
    if note:
        entry['note'] = note
    return entry


def upsert(registry, entry):
    for index, existing in enumerate(registry['entries']):
        if existing['id'] == entry['id']:
            registry['entries'][index] = entry
            return 'replaced'
    registry['entries'].append(entry)
    return 'added'


def cmd_seed(args):
    for prefix in args.path or []:
        clean = prefix.rstrip('/')
        if not qualifies(clean) and not any(q.startswith(clean + '/') for q in QUALIFYING):
            raise HunkError(f'{prefix}: not a qualifying source path ({", ".join(QUALIFYING)})')
    registry = load_registry(args.registry)
    if args.anchor is not None:
        if not args.file or args.lines is None:
            raise HunkError('--anchor needs --file and --lines')
        entry = seed_span(args.repo, args.file, args.as_of, args.anchor, args.lines,
                          args.id, args.titles, args.note, args.origin, args.subject)
    else:
        if not args.commit:
            raise HunkError('seed needs --commit, or --file/--anchor/--lines to pin a current span')
        entry = seed_entry(args.repo, args.commit, args.id, args.titles, args.path or None,
                           args.as_of, args.note, args.origin, args.subject)
    action = upsert(registry, entry)
    write_registry(args.registry, registry)
    evolved = entry.get('evolved')
    print(f"{action}  {entry['id']}  {len(entry['blocks'])} blocks, "
          f"{sum(len(b['lines']) for b in entry['blocks'])} lines from {entry['origin_commit'][:12]}"
          + (f"  evolved: dropped {evolved['dropped_lines']} lines, "
             f"{evolved['from_blocks']} -> {evolved['to_blocks']} blocks" if evolved else ''))
    return 0


def titles_list(value):
    titles = [t.strip() for t in value.split(',') if t.strip()]
    if not titles:
        raise argparse.ArgumentTypeError('at least one title is required')
    return titles


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)

    def common(p):
        p.add_argument('--registry', type=Path, default=DEFAULT_REGISTRY)
        p.add_argument('--repo', type=Path, default=DEFAULT_REPO)

    p = sub.add_parser('check', help='verify every registered block exists contiguously at a revision')
    common(p)
    p.add_argument('--rev', help='git revision (default: the working tree files)')
    p.add_argument('--json', type=Path, help='write the full per-entry/per-block result')
    p.add_argument('--titles', type=titles_list, help='only entries touching these titles')
    p.set_defaults(func=cmd_check)

    p = sub.add_parser('check-all', help='matrix of entries across the split branches (or --revs)')
    common(p)
    p.add_argument('--revs', type=lambda v: [r.strip() for r in v.split(',') if r.strip()],
                   help='comma-separated revisions (default: split/01..07 and HEAD)')
    p.add_argument('--json', type=Path)
    p.add_argument('--titles', type=titles_list)
    p.set_defaults(func=cmd_check_all)

    p = sub.add_parser('seed', help="record a commit's added source lines as a registry entry")
    common(p)
    p.add_argument('--commit')
    p.add_argument('--path', action='append', help='restrict to these qualifying path prefixes')
    p.add_argument('--id', required=True)
    p.add_argument('--titles', type=titles_list, required=True)
    p.add_argument('--as-of', dest='as_of', help='verify/split the blocks against this revision')
    p.add_argument('--note')
    p.add_argument('--origin', help='record this hash as origin_commit (the lines come from a re-application)')
    p.add_argument('--subject')
    p.add_argument('--file', help='with --anchor/--lines: pin a span of this file as it exists at --as-of')
    p.add_argument('--anchor', help='exact first line of the span to pin (must match exactly one line)')
    p.add_argument('--lines', type=int, help='number of lines in the span, starting at --anchor')
    p.set_defaults(func=cmd_seed)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except HunkError as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    except OSError as error:
        print(f'error: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
