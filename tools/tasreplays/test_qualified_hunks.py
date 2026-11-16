"""Asset-free checks for the qualified-hunk registry against a throwaway git repository."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qualified_hunks as qh  # noqa: E402

TOOL = HERE / 'qualified_hunks.py'
ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@x', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@x')


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, env=ENV).stdout.decode().strip()


def commit(repo, message):
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', message)
    return git(repo, 'rev-parse', 'HEAD')


def put(repo, path, text, crlf=False):
    target = repo / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(text.replace('\n', '\r\n').encode() if crlf else text.encode())


def run(repo, *args):
    return subprocess.run([sys.executable, str(TOOL), *args, '--repo', str(repo)], capture_output=True, text=True, env=ENV)


BASE_A = 'int a(void) {\n    return 0;\n}\n\nint z(void) {\n    return 9;\n}\n'
BLOCK = ['    /* fold the frontend deadline */', '    uint32_t gpu = cycles_to_event();',
         '    if (gpu < deadline) deadline = gpu;', '    slice(deadline);']
FIX_A = 'int a(void) {\n' + '\n'.join(BLOCK) + '\n    return 0;\n}\n\nint z(void) {\n    return 9 + 1;\n}\n'
BASE_B = 'static int b;\nvoid setb(void) {\n    b = 1;\n}\n'
FIX_B = 'static int b;\nvoid setb(void) {\n    if (!ready())\n        return;\n    b = 1;\n}\n'


def main():
    with tempfile.TemporaryDirectory() as temp:
        repo = Path(temp)
        git(repo, 'init', '-q')
        git(repo, 'config', 'core.autocrlf', 'false')
        put(repo, 'runtime/src/a.c', BASE_A)
        put(repo, 'runtime/src/b.c', BASE_B, crlf=True)
        put(repo, 'docs/x.md', 'doc\n')
        base = commit(repo, 'base')
        put(repo, 'runtime/src/a.c', FIX_A)
        put(repo, 'runtime/src/b.c', FIX_B, crlf=True)
        put(repo, 'docs/x.md', 'doc\nmore doc\n')
        put(repo, 'runtime/tests/t.c', 'test line\n')
        put(repo, 'tools/tasreplays/tekken3-codegen.json', '{\n  "a": "1"\n}\n')
        fix = commit(repo, 'fix')
        put(repo, 'runtime/src/a.c', FIX_A.replace('    if (gpu < deadline) deadline = gpu;', '    if (gpu <= deadline) deadline = gpu;'))
        rewritten = commit(repo, 'rewrite one line')
        put(repo, 'docs/x.md', 'doc\nmore doc\nstill docs\n')
        docs_only = commit(repo, 'docs only')
        registry = repo / 'reg.json'

        # Block extraction: per-hunk contiguous runs, qualifying source paths only, CRLF stripped.
        blocks = qh.extract_blocks(repo, fix)
        assert [(b['file'], b['lines']) for b in blocks] == [
            ('runtime/src/a.c', BLOCK), ('runtime/src/a.c', ['    return 9 + 1;']),
            ('runtime/src/b.c', ['    if (!ready())', '        return;'])], blocks
        tekken = qh.extract_blocks(repo, fix, ['tools/tasreplays/tekken3-codegen.json'])
        assert [b['lines'] for b in tekken] == [['{', '  "a": "1"', '}']], tekken
        assert qh.extract_blocks(repo, fix, ['runtime/src/b.c'])[0]['file'] == 'runtime/src/b.c'
        assert not qh.qualifies('runtime/tests/t.c') and not qh.qualifies('docs/x.md')
        assert qh.qualifies('runtime/src/a.c') and qh.qualifies('recompiler/src/x.c')

        # Presence: CRLF and trailing whitespace are ignored, indentation is not.
        lines = qh.file_lines(repo, 'runtime/src/b.c', fix)
        assert qh.find_block(lines, ['    if (!ready())', '        return;']) == 3
        assert qh.find_block(qh.normalise('x  \r\ny\t\r\n'), ['x', 'y']) == 1
        assert qh.find_block(qh.normalise('  x\n'), ['x']) == 0
        assert qh.find_block(qh.file_lines(repo, 'runtime/src/a.c', base), BLOCK) == 0
        assert qh.file_lines(repo, 'runtime/src/none.c', fix) is None

        # --as-of splitting: the rewritten third line breaks the block into a kept
        # two-line head and a dropped one-line tail (minimum two lines per sub-block).
        kept, dropped = qh.split_block(qh.file_lines(repo, 'runtime/src/a.c', rewritten), BLOCK)
        assert kept == [BLOCK[:2]] and dropped == 2, (kept, dropped)
        assert qh.split_block(['q', '    return 9 + 1;'], ['    return 9 + 1;']) == ([['    return 9 + 1;']], 0)
        assert qh.split_block(['q'], ['    return 9 + 1;']) == ([], 1)
        assert qh.split_block(['a', 'b', 'x', 'c', 'd', 'e'], ['a', 'b', 'c', 'd', 'e']) == ([['a', 'b'], ['c', 'd', 'e']], 0)

        # seed: verbatim at the fix commit, evolved at the rewrite commit; replace by id.
        seeded = run(repo, 'seed', '--commit', fix, '--id', 'fix-1', '--titles', 'tekken3,pepsiman', '--as-of', fix, '--registry', str(registry))
        assert seeded.returncode == 0, seeded.stderr
        data = json.loads(registry.read_text())
        assert data['schema'] == qh.SCHEMA and [e['id'] for e in data['entries']] == ['fix-1']
        entry = data['entries'][0]
        assert entry['titles'] == ['tekken3', 'pepsiman'] and entry['origin_commit'] == fix and entry['qualified_tree'] == fix
        assert entry['subject'] == 'fix' and 'evolved' not in entry and len(entry['blocks']) == 3
        raw = registry.read_bytes()
        assert b'\r\n' not in raw and raw.endswith(b'}\n') and b'\n  "entries"' in raw

        seeded = run(repo, 'seed', '--commit', fix, '--id', 'fix-1', '--titles', 'biohazard', '--as-of', rewritten,
                     '--note', 'n', '--origin', base, '--registry', str(registry))
        assert seeded.returncode == 0, seeded.stderr
        data = json.loads(registry.read_text())
        assert [e['id'] for e in data['entries']] == ['fix-1'], 'replace by id must not duplicate'
        entry = data['entries'][0]
        assert entry['evolved'] == {'dropped_lines': 2, 'from_blocks': 3, 'to_blocks': 3}, entry['evolved']
        assert entry['blocks'][0]['lines'] == BLOCK[:2] and entry['note'] == 'n'
        assert entry['origin_commit'] == base and entry['seed_commit'] == fix and entry['titles'] == ['biohazard']

        # Refusals: nothing qualifying, non-qualifying --path.
        empty = run(repo, 'seed', '--commit', docs_only, '--id', 'none', '--titles', 't', '--registry', str(registry))
        assert empty.returncode == 2 and 'no qualifying' in empty.stderr, (empty.returncode, empty.stderr)
        bad = run(repo, 'seed', '--commit', fix, '--path', 'docs', '--id', 'd', '--titles', 't', '--registry', str(registry))
        assert bad.returncode == 2 and 'not a qualifying' in bad.stderr
        assert [e['id'] for e in json.loads(registry.read_text())['entries']] == ['fix-1']

        # check: exit codes, per-entry lines, --json detail, --titles filter, unresolvable rev.
        out = registry.with_name('out.json')
        ok = run(repo, 'check', '--rev', rewritten, '--registry', str(registry), '--json', str(out))
        assert ok.returncode == 0 and 'present  fix-1' in ok.stdout and 'missing: 0 of 1 entries' in ok.stdout, ok.stdout
        report = json.loads(out.read_text())
        assert report['resolved'] == rewritten and report['entries'][0]['present'] is True
        assert [b['line'] for b in report['entries'][0]['blocks']] == [2, 10, 3]
        missing = run(repo, 'check', '--rev', base, '--registry', str(registry), '--json', str(out))
        assert missing.returncode == 1 and 'MISSING  fix-1  3 of 3 blocks  runtime/src/a.c: /* fold the frontend deadline */' in missing.stdout, missing.stdout
        assert 'missing: 1 of 1 entries' in missing.stdout
        report = json.loads(out.read_text())
        assert report['missing'] == 1 and report['entries'][0]['blocks'][0] == {
            'file': 'runtime/src/a.c', 'lines': 2, 'present': False, 'line': None, 'file_missing': False,
            'first_missing_line': BLOCK[0]}
        assert run(repo, 'check', '--rev', base, '--titles', 'tekken3', '--registry', str(registry)).stdout.startswith('missing: 0 of 0')
        assert run(repo, 'check', '--rev', 'no-such-rev', '--registry', str(registry)).returncode == 2
        assert run(repo, 'check', '--registry', str(repo / 'absent.json'), '--rev', fix).returncode == 0
        put(repo, 'bad.json', '{"schema": "other"}')
        assert run(repo, 'check', '--registry', str(repo / 'bad.json')).returncode == 2

        # Working-tree check tolerates CRLF and trailing whitespace on disk.
        put(repo, 'runtime/src/a.c', (repo / 'runtime/src/a.c').read_text().replace('deadline */', 'deadline */   '), crlf=True)
        tree = run(repo, 'check', '--registry', str(registry))
        assert tree.returncode == 0 and '(worktree)' in tree.stdout, tree.stdout
        put(repo, 'runtime/src/a.c', (repo / 'runtime/src/a.c').read_text().replace('    uint32_t gpu', '      uint32_t gpu'))
        assert run(repo, 'check', '--registry', str(registry)).returncode == 1, 'indentation must stay exact'
        git(repo, 'checkout', '-q', '--', 'runtime/src/a.c')

        # check-all: matrix across explicit revs, exit 1 when any rev misses anything.
        matrix = run(repo, 'check-all', '--revs', f'{base},{fix},{rewritten}', '--registry', str(registry), '--json', str(out))
        assert matrix.returncode == 1, matrix.stderr
        row = next(line for line in matrix.stdout.splitlines() if line.startswith('fix-1'))
        assert row.split()[1:] == ['MISSING', 'ok', 'ok'], row
        assert f'{base} ({base[:12]}): missing 1 of 1 entries' in matrix.stdout
        assert [r['missing'] for r in json.loads(out.read_text())['revs']] == [1, 0, 0]
        assert run(repo, 'check-all', '--revs', f'{fix},{rewritten}', '--registry', str(registry)).returncode == 0
        assert qh.SPLIT_REVS[0] == 'origin/split/01-standalone-fixes' and qh.SPLIT_REVS[-1] == 'HEAD'

        # A tracked debt is still absent and still reported, but does not fail the gate, while
        # an unexpected absence still does. This is the property setup() relies on: a candidate
        # must not be built from a tree that silently lost a qualified behaviour.
        entries = json.loads(registry.read_text())
        assert run(repo, 'check', '--rev', base, '--registry', str(registry)).returncode == 1
        debt = dict(entries, known_absent={entries['entries'][0]['id']: 'ported later; reason recorded'})
        debt_registry = repo / 'debt.json'
        debt_registry.write_text(json.dumps(debt, indent=2) + '\n', encoding='utf-8', newline='\n')
        tracked = run(repo, 'check', '--rev', base, '--registry', str(debt_registry))
        assert tracked.returncode == 0, tracked.stdout
        assert 'debt     ' in tracked.stdout and 'ported later; reason recorded' in tracked.stdout
        assert 'plus 1 tracked as debt' in tracked.stdout, tracked.stdout
        # Marking an entry that is actually present changes nothing.
        assert run(repo, 'check', '--rev', fix, '--registry', str(debt_registry)).returncode == 0
    print('qualified_hunks: ok')


if __name__ == '__main__':
    main()
