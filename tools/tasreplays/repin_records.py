"""Qualified-hunk re-pin records: a registry change that excuses or removes code must say why.

`qualified_hunks.py check` trusts the registry it reads. A change that marks an entry
`known_absent`, or shortens, rewrites or deletes a pinned block, turns the gate green without
anyone having to say why. This check compares the registry at the merge-base of a change with
the registry at its head and fails unless each such entry gains a `repins` record that names an
issue (for example PS1B-288). The record sits inside the entry, so it names that entry; a record
added to another entry does not count. Existing debt and an unchanged registry pass.

What needs a record, per entry:
  - its id is added to `known_absent`, or its `known_absent` reason changes;
  - a block of the base is no longer contained, contiguously and in the same file, in a block of
    the head (shortened, edited, moved to another file or removed);
  - the entry itself is removed (this always fails: keep the entry and record the loss instead).
A change that needs a record must also leave the entry's earlier `repins` records as they were.
Adding entries, blocks or lines, and removing a `known_absent` reason, need no record.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

REGISTRY = 'tools/tasreplays/qualified-hunks.json'
ISSUE = re.compile(r'\b[A-Z][A-Z0-9]*-\d+\b')
ZERO = re.compile(r'^0+$')


class RecordError(Exception):
    """Usage or IO failure (exit 2)."""


def git(repo, *args, allow_fail=False):
    result = subprocess.run(['git', '-C', repo, *args], capture_output=True)
    if result.returncode:
        if allow_fail:
            return None
        raise RecordError(f"git {' '.join(args)}: {result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout


def resolve(repo, rev):
    """Full hash of a commit, or None. ^0 instead of ^{commit}: MSYS2 git expands braces."""
    if not rev or ZERO.match(rev):
        return None
    out = git(repo, 'rev-parse', '--verify', '--quiet', f'{rev}^0', allow_fail=True)
    return out.decode().strip() if out else None


def registry_at(repo, rev, path=REGISTRY):
    data = git(repo, 'show', f'{rev}:{path}', allow_fail=True)
    if data is None:
        return {'known_absent': {}, 'entries': []}
    try:
        registry = json.loads(data.decode('utf-8'))
    except ValueError as error:
        raise RecordError(f'{path} at {rev[:12]}: {error}')
    if not isinstance(registry.get('entries'), list):
        raise RecordError(f'{path} at {rev[:12]}: no entries list')
    return registry


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False)


def contains(outer, inner):
    n = len(inner)
    return any(outer[i:i + n] == inner for i in range(len(outer) - n + 1))


def weakened_blocks(base_entry, head_entry):
    """1-based numbers of the base blocks that no head block of the same file still contains."""
    head = [(b['file'], b['lines']) for b in head_entry.get('blocks', [])]
    return [n for n, b in enumerate(base_entry.get('blocks', []), 1)
            if not any(f == b['file'] and contains(lines, b['lines']) for f, lines in head)]


def issue_of(record):
    match = ISSUE.search(str(record.get('issue', ''))) if isinstance(record, dict) else None
    return match.group(0) if match else None


def compare(base, head):
    """Findings for a base -> head registry change: (failures, recorded), lists of strings."""
    base_entries = {e['id']: e for e in base.get('entries', [])}
    head_entries = {e['id']: e for e in head.get('entries', [])}
    base_known = base.get('known_absent') or {}
    head_known = head.get('known_absent') or {}
    needs = {}
    for entry_id, entry in base_entries.items():
        if entry_id not in head_entries:
            needs.setdefault(entry_id, []).append('the entry is removed')
            continue
        blocks = weakened_blocks(entry, head_entries[entry_id])
        if blocks:
            first = entry['blocks'][blocks[0] - 1]
            needs.setdefault(entry_id, []).append(
                f"{len(blocks)} of {len(entry['blocks'])} blocks shortened, changed or removed "
                f"(first: block {blocks[0]}, {first['file']}, {len(first['lines'])} lines)")
    for entry_id, reason in head_known.items():
        if entry_id not in base_known:
            needs.setdefault(entry_id, []).append('known_absent is added')
        elif base_known[entry_id] != reason:
            needs.setdefault(entry_id, []).append('its known_absent reason is changed')
    failures, recorded = [], []
    for entry_id in sorted(needs):
        what = '; '.join(needs[entry_id])
        if entry_id not in head_entries:
            hint = ('keep the entry and record the loss in known_absent with a repins record naming the issue'
                    if entry_id in base_entries else 'known_absent names no registry entry')
            failures.append(f'{entry_id}: {what}; {hint}')
            continue
        old = Counter(canonical(r) for r in base_entries.get(entry_id, {}).get('repins', []))
        new = Counter(canonical(r) for r in head_entries[entry_id].get('repins', []))
        if old - new:
            failures.append(f'{entry_id}: {what}; the change also rewrites or removes '
                            f'{sum((old - new).values())} earlier repins record(s) of this entry; '
                            f'leave them as they are and add a new one')
            continue
        issues = [issue_of(json.loads(r)) for r in (new - old).elements()]
        issues = sorted({i for i in issues if i})
        if issues:
            recorded.append(f"{entry_id}: {what}; recorded by a new repins record ({', '.join(issues)})")
        else:
            failures.append(f'{entry_id}: {what}; no new repins record in this entry names an issue '
                            f'(add one to its "repins" list with "issue": "PS1B-<n>" and the reason)')
    return failures, recorded


def github_base(repo, head):
    """The base revision of this change from the GitHub Actions event, or None."""
    event = {}
    if os.environ.get('GITHUB_EVENT_PATH'):
        try:
            with open(os.environ['GITHUB_EVENT_PATH'], encoding='utf-8') as stream:
                event = json.load(stream)
        except (OSError, ValueError) as error:
            raise RecordError(f'GITHUB_EVENT_PATH: {error}')
    candidates = []
    if os.environ.get('GITHUB_EVENT_NAME') == 'pull_request':
        candidates.append((event.get('pull_request') or {}).get('base', {}).get('sha'))
    elif os.environ.get('GITHUB_REF', '').startswith('refs/heads/stage/'):
        candidates.append(event.get('before'))
    # A fix branch is compared with the stage it was cut from: the newest stage branch.
    refs = git(repo, 'for-each-ref', '--sort=-committerdate', '--format=%(refname)', 'refs/remotes/origin/stage/')
    candidates += [r for r in refs.decode().split() if resolve(repo, r) != head]
    if os.environ.get('GITHUB_EVENT_NAME') != 'pull_request':
        candidates.append(event.get('before'))
    for candidate in candidates:
        if resolve(repo, candidate):
            return candidate
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--base', help='revision the change starts from (its merge-base with --head is used)')
    parser.add_argument('--github', action='store_true',
                        help='take the base from the GitHub Actions event (pull request base, stage push before, '
                             'else the newest origin/stage/* branch)')
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--repo', default='.')
    parser.add_argument('--registry', default=REGISTRY, help='repository path of the registry')
    args = parser.parse_args(argv)
    try:
        head = resolve(args.repo, args.head)
        if not head:
            raise RecordError(f'unresolvable head: {args.head}')
        base_rev = github_base(args.repo, head) if args.github else args.base
        if not base_rev:
            raise RecordError('no base revision: pass --base, or with --github fetch the stage branches '
                              '(actions/checkout with fetch-depth: 0)')
        base = resolve(args.repo, base_rev)
        if not base:
            raise RecordError(f'unresolvable base: {base_rev}')
        merge_base = git(args.repo, 'merge-base', base, head).decode().strip()
        failures, recorded = compare(registry_at(args.repo, merge_base, args.registry),
                                     registry_at(args.repo, head, args.registry))
    except RecordError as error:
        print(f'error: {error}', file=sys.stderr)
        return 2
    for line in recorded:
        print(f'recorded  {line}')
    for line in failures:
        print(f'MISSING   {line}')
    print(f're-pin records: {len(recorded) + len(failures)} entries excused or reduced, '
          f'{len(failures)} without a record  (merge-base {merge_base[:12]} from {base_rev} -> head {head[:12]})')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
