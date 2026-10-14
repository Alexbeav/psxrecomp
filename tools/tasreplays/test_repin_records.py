"""Authored controls for repin_records.py: unit cases on registry pairs, then git and GitHub-event runs."""
import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import repin_records as rr  # noqa: E402

TOOL = HERE / 'repin_records.py'
ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@x', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@x')
for name in ('GITHUB_EVENT_NAME', 'GITHUB_EVENT_PATH', 'GITHUB_REF'):
    ENV.pop(name, None)

BASE = {
    'schema': 'psx-tas-qualified-hunks-v1',
    'known_absent': {'old-debt': 'Superseded by fix-b (PS1B-1).'},
    'entries': [
        {'id': 'fix-a', 'titles': ['t'], 'blocks': [
            {'file': 'runtime/src/a.c', 'lines': ['a1', 'a2', 'a3']},
            {'file': 'runtime/src/a.c', 'lines': ['a9']}],
         'repins': [{'as_of': 'c0', 'issue': 'PS1B-7', 'dropped_lines': 1}]},
        {'id': 'fix-b', 'titles': ['t'], 'blocks': [{'file': 'runtime/src/b.c', 'lines': ['b1', 'b2']}]},
        {'id': 'old-debt', 'titles': ['t'], 'blocks': [{'file': 'runtime/src/c.c', 'lines': ['c1']}]},
    ],
}
RECORD = {'as_of': 'c1', 'issue': 'PS1B-480', 'reason': 'authored control', 'dropped_lines': 1}


def changed(edit):
    head = copy.deepcopy(BASE)
    edit(head)
    return head


def entry(registry, entry_id):
    return next(e for e in registry['entries'] if e['id'] == entry_id)


def expect(head, failing=(), recorded=(), text=None, base=BASE):
    failures, ok = rr.compare(base, head)
    assert [f.split(':')[0] for f in failures] == list(failing), failures
    assert [r.split(':')[0] for r in ok] == list(recorded), ok
    if text:
        assert any(text in f for f in failures + ok), (text, failures, ok)
    return failures


def unit_controls():
    # Unchanged registry and existing debt pass.
    expect(copy.deepcopy(BASE))
    # Growth needs no record: a longer block, a new block, a new entry, a debt cleared.
    expect(changed(lambda r: entry(r, 'fix-a')['blocks'][0]['lines'].append('a4')))
    expect(changed(lambda r: entry(r, 'fix-a')['blocks'][0]['lines'].insert(0, 'a0')))
    expect(changed(lambda r: entry(r, 'fix-b')['blocks'].append({'file': 'runtime/src/b.c', 'lines': ['b7']})))
    expect(changed(lambda r: r['entries'].append({'id': 'new', 'blocks': [{'file': 'x', 'lines': ['n']}]})))
    expect(changed(lambda r: r['known_absent'].pop('old-debt')))
    # A block merged into a larger one of the same file is still contained.
    expect(changed(lambda r: entry(r, 'fix-a').update(blocks=[
        {'file': 'runtime/src/a.c', 'lines': ['a1', 'a2', 'a3', 'x', 'a9']}, {'file': 'runtime/src/a.c', 'lines': ['a9']}])))

    # Bad: an unrecorded absence.
    expect(changed(lambda r: r['known_absent'].update({'fix-b': 'Lost in the merge.'})),
           failing=['fix-b'], text='known_absent is added; no new repins record in this entry names an issue')
    # Bad: a changed debt reason.
    expect(changed(lambda r: r['known_absent'].update({'old-debt': 'Rewritten reason.'})),
           failing=['old-debt'], text='its known_absent reason is changed')
    # Bad: a shortened block, a removed block, an edited line, a block moved to another file.
    expect(changed(lambda r: entry(r, 'fix-a')['blocks'][0]['lines'].pop()),
           failing=['fix-a'], text='1 of 2 blocks shortened, changed or removed (first: block 1, runtime/src/a.c, 3 lines)')
    expect(changed(lambda r: entry(r, 'fix-a')['blocks'].pop()), failing=['fix-a'], text='first: block 2')
    expect(changed(lambda r: entry(r, 'fix-b')['blocks'][0]['lines'].__setitem__(1, 'b2 // edited')), failing=['fix-b'])
    expect(changed(lambda r: entry(r, 'fix-b')['blocks'][0].update(file='runtime/src/z.c')), failing=['fix-b'])
    # Bad: the entry is removed; a record cannot excuse it.
    expect(changed(lambda r: r['entries'].remove(entry(r, 'fix-b'))), failing=['fix-b'],
           text='keep the entry and record the loss in known_absent')
    # Bad: a debt that names no entry.
    expect(changed(lambda r: r['known_absent'].update({'ghost': 'x'})), failing=['ghost'],
           text='known_absent names no registry entry')

    def absent_with(record_entry, record):
        def edit(r):
            r['known_absent']['fix-b'] = 'Lost in the merge (PS1B-480).'
            entry(r, record_entry).setdefault('repins', []).append(record)
        return changed(edit)

    # Good: the same absence with its matching record and issue.
    expect(absent_with('fix-b', RECORD), recorded=['fix-b'], text='recorded by a new repins record (PS1B-480)')
    # Bad: an unrelated record (another entry), a record without an issue id.
    expect(absent_with('fix-a', RECORD), failing=['fix-b'])
    expect(absent_with('fix-b', dict(RECORD, issue='see the chat')), failing=['fix-b'])
    expect(absent_with('fix-b', {k: v for k, v in RECORD.items() if k != 'issue'}), failing=['fix-b'])

    # Good: a shortened block with its record; issue ids inside longer text count.
    def shortened(issue):
        def edit(r):
            entry(r, 'fix-a')['blocks'][0]['lines'].pop()
            entry(r, 'fix-a')['repins'].append(dict(RECORD, issue=issue))
        return changed(edit)
    expect(shortened('PS1B-480'), recorded=['fix-a'])
    expect(shortened('PS1B-211, PS1B-215'), recorded=['fix-a'], text='(PS1B-211)')
    # Bad: the old record of the entry is rewritten instead of a new one added.
    def rewritten(r):
        entry(r, 'fix-a')['blocks'][0]['lines'].pop()
        entry(r, 'fix-a')['repins'][0]['issue'] = 'PS1B-480'
    expect(changed(rewritten), failing=['fix-a'], text='rewrites or removes 1 earlier repins record')
    # A record rewritten with nothing weakened is not this check's business.
    expect(changed(lambda r: entry(r, 'fix-a')['repins'][0].update(signature='s')))
    # A registry that did not exist at the base: new entries need nothing, a debt still needs its record.
    expect(changed(lambda r: r.pop('known_absent')), base={'entries': []})
    expect(BASE, base={'entries': []}, failing=['old-debt'])


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True,
                          env=ENV).stdout.decode().strip()


def write(repo, registry):
    path = repo / rr.REGISTRY
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(registry, indent=2) + '\n', encoding='utf-8', newline='\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'registry')
    return git(repo, 'rev-parse', 'HEAD')


def run(repo, *args, env=None):
    return subprocess.run([sys.executable, '-B', str(TOOL), '--repo', str(repo), *args], capture_output=True,
                          text=True, encoding='utf-8', errors='replace', env=env or ENV)


def git_controls(temp):
    repo = Path(temp) / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q')
    git(repo, 'config', 'core.autocrlf', 'false')
    base = write(repo, BASE)
    git(repo, 'checkout', '-q', '-b', 'fix')
    bad = write(repo, changed(lambda r: r['known_absent'].update({'fix-b': 'Lost.'})))
    # The stage moves on with its own unrecorded change: not this change's range.
    git(repo, 'checkout', '-q', '-b', 'stage', base)
    stage = write(repo, changed(lambda r: entry(r, 'fix-a')['blocks'].pop()))
    git(repo, 'checkout', '-q', 'fix')

    result = run(repo, '--base', 'stage', '--head', bad)
    assert result.returncode == 1, (result.stdout, result.stderr)
    assert 'MISSING   fix-b: known_absent is added' in result.stdout and 'fix-a' not in result.stdout, result.stdout
    assert f'merge-base {base[:12]}' in result.stdout and '1 without a record' in result.stdout, result.stdout

    def good_edit(r):
        r['known_absent']['fix-b'] = 'Lost.'
        entry(r, 'fix-b')['repins'] = [RECORD]
    good = write(repo, changed(good_edit))
    result = run(repo, '--base', 'stage')
    assert result.returncode == 0 and 'recorded  fix-b' in result.stdout, (result.stdout, result.stderr)
    unchanged = run(repo, '--base', base, '--head', base)
    assert unchanged.returncode == 0 and '0 entries excused or reduced' in unchanged.stdout, unchanged.stdout
    assert run(repo, '--base', 'no-such-rev').returncode == 2
    assert run(repo, '--head', 'no-such-rev', '--base', base).returncode == 2
    assert run(repo).returncode == 2

    # GitHub event selection.
    event = Path(temp) / 'event.json'

    def gh(name, ref, payload, head=None):
        event.write_text(json.dumps(payload), encoding='utf-8')
        env = dict(ENV, GITHUB_EVENT_NAME=name, GITHUB_REF=ref, GITHUB_EVENT_PATH=str(event))
        return run(repo, '--github', *(['--head', head] if head else []), env=env)

    zero = '0' * 40
    # No stage branch fetched and a new branch: no base, a usage error that says what to fetch.
    missing = gh('push', 'refs/heads/fix/x', {'before': zero})
    assert missing.returncode == 2 and 'fetch-depth: 0' in missing.stderr, missing.stderr
    git(repo, 'update-ref', 'refs/remotes/origin/stage/pin-x', stage)
    # A fix branch push compares with the stage branch, whatever `before` says.
    result = gh('push', 'refs/heads/fix/x', {'before': zero}, head=bad)
    assert result.returncode == 1 and f'from refs/remotes/origin/stage/pin-x' in result.stdout, result.stdout
    assert gh('push', 'refs/heads/fix/x', {'before': bad}).returncode == 0
    # A stage push compares with the stage's previous head; a pull request with its base.
    result = gh('push', 'refs/heads/stage/pin-x', {'before': base}, head=bad)
    assert result.returncode == 1 and f'from {base}' in result.stdout, result.stdout
    result = gh('pull_request', 'refs/pull/1/merge', {'pull_request': {'base': {'sha': base}}}, head=good)
    assert result.returncode == 0 and f'from {base}' in result.stdout, result.stdout
    # The stage branch itself is never its own base.
    git(repo, 'update-ref', 'refs/remotes/origin/stage/pin-x', bad)
    result = gh('push', 'refs/heads/stage/pin-x', {'before': zero}, head=bad)
    assert result.returncode == 2, (result.stdout, result.stderr)


def main():
    unit_controls()
    with tempfile.TemporaryDirectory() as temp:
        git_controls(temp)
    print('repin_records: all controls passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
