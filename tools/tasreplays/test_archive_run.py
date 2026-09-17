"""archive_run.py: receipt summaries, SHA256SUMS merging, verified copies, notes.

Everything runs in temporary directories with synthetic run directories shaped
like the real split07-requal-20260914 evidence. The final section runs
--dry-run (read-only) against that real archive when it is reachable.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import archive_run as ar  # noqa: E402

TOOL = HERE / 'archive_run.py'
REAL = Path(r'Z:\Share\psxrecomp\tas-evidence\split07-requal-20260914\runs')
SHA = {c: c * 64 for c in 'abcdef0123456789'}


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


def rejects(call, error=ar.ArchiveError):
    try:
        call()
    except error:
        return
    raise AssertionError(f'expected {error.__name__}')


def make_project(root, name, schema, exe_sha):
    project = root / name
    project.mkdir(parents=True)
    write_json(project / 'setup.json', {'schema': schema, 'source_head': 'a' * 40, 'source_tree': 'b' * 40,
                                        'executable_sha256': exe_sha, 'game': str(project / 'game.toml'),
                                        'executable': str(project / 'native' / f'{name}.exe')})
    (project / 'game.toml').write_text('[game]\nname = "x"\n')
    (project / 'bios.toml').write_text('[bios]\n')
    if schema == 'psx-tas-setup-v1':
        write_json(project / 'input.json', {'inputs': 7974})
    return project


def make_run(root, name, project, exe_name, exe_sha, receipt_name, receipt, exit_extra=None, complete=None):
    run = root / name
    (run / 'cards').mkdir(parents=True)
    write_json(run / 'manifest.json', {
        'schema': 'psx-native-tas-run-v1',
        'command': [str(run / exe_name), '--game', str(project / 'game.toml'), '--headless'],
        'inputs': {'exe': {'path': str(project / 'native' / exe_name), 'sha256': exe_sha},
                   'game': {'path': str(project / 'game.toml'), 'sha256': SHA['1']},
                   'route': {'path': str(project / 'input.psxrti2'), 'sha256': SHA['2']},
                   'disc': {'path': 'D:/discs/x.cue', 'sha256': SHA['3']},
                   'bios': {'path': str(project / 'SCPH5500.BIN'), 'sha256': SHA['4']}},
        'route': {'format': 'PSXRTI2', 'frames': 227202, 'sha256': SHA['2']},
        'psx_environment': {'PSX_HLE_SCHEDULER': '1'}})
    write_json(run / 'exit.json', {'pid': 1, 'exit_code': 0, 'stop_reason': None, 'timed_out': False,
                                   'host_seconds': 1234.5, 'completion_exists': True, **(exit_extra or {})})
    write_json(run / 'complete.json', complete or {'frame': 239203, 'input_frames': 227202, 'neutral_tail_ticks': 12001,
                                                   'stop_reason': 'declared_observation_end_before_next_sample'})
    (run / exe_name).write_bytes(b'MZ' + name.encode() * 50)
    (run / 'ram-pages.tsv').write_text('frame\tcycle\n1\t2\n')
    (run / 'cpu-return.tsv').write_text('frame\tcycle\n1\t3\n')
    (run / 'frame-000000.png').write_bytes(b'\x89PNG' + bytes(64))
    (run / 'cards' / 'card1.mcd').write_bytes(b'MC' + bytes(126))
    if receipt is not None:
        write_json(run / receipt_name, receipt)
    return run


def bio_receipt(binary, setup_sha, **overrides):
    receipt = {'candidate_sha256': binary, 'setup_executable_sha256': setup_sha, 'binary_sha256': binary,
               'binary_matches_setup': binary == setup_sha, 'diagnostic_binary': None if binary == setup_sha else binary,
               'source_reference': {'path': 'D:/ref.json', 'sha256': SHA['5']},
               'diagnostic_prefix': False, 'original_input_prefix_unchanged': True, 'full_original_input_and_tail': True,
               'observed_returns': 239202, 'native_input_exit': 0,
               'comparison': {'match': True, 'returns': [239202, 239202], 'first_divergence': None},
               'terminal_ram_match': True, 'terminal_card1_match': True, 'terminal_observation_error': None,
               'mechanical_match': True, 'status': 'pass', 'first_divergence': None,
               'streaming': {'compared_returns': 239202, 'first_divergence': None, 'stopped_on_divergence': False},
               'qualification': 'mechanical comparison only; ending/semantic review and repeated gameplay remain required'}
    receipt.update(overrides)
    return receipt


def build_fixtures(root):
    """Synthetic projects and runs for all three titles. Returns {name: run dir}."""
    bio = make_project(root / 'projects', 'split07fix-biohazard', 'biohazard-tas-candidate-v1', SHA['a'])
    tek = make_project(root / 'projects', 'split07-tekken3-p2', 'psx-tas-setup-v1', SHA['b'])
    pep = make_project(root / 'projects', 'split07-pepsiman', 'pepsiman-tas-candidate-v1', SHA['c'])
    runs = root / 'validation'
    out = {}
    out['bio-full'] = make_run(runs, 'fix2-biohazard-replay-01', bio, 'BioHazard-TAS.exe', SHA['a'],
                               'source-comparison.json', bio_receipt(SHA['a'], SHA['a']))
    divergence = {'frame': 1142, 'source_cycle': 1000, 'native_cycle': 1004, 'changed_pages': []}
    out['bio-fail'] = make_run(runs, 'split07-biohazard-replay-01', bio, 'BioHazard-TAS.exe', SHA['a'],
                               'source-comparison.json',
                               bio_receipt(SHA['a'], SHA['a'], comparison={'match': False, 'returns': [239202, 1142],
                                                                            'first_divergence': divergence},
                                           terminal_ram_match=None, terminal_card1_match=None, mechanical_match=False,
                                           status='fail', first_divergence=divergence,
                                           streaming={'compared_returns': 1142, 'first_divergence': divergence,
                                                      'stopped_on_divergence': True}),
                               exit_extra={'exit_code': 3, 'stop_reason': 'stop_request'})
    out['bio-prefix'] = make_run(runs, 'split07fix-biohazard-prefix-01', bio, 'BioHazard-TAS.exe', SHA['a'],
                                 'source-comparison.json',
                                 bio_receipt(SHA['a'], SHA['a'], diagnostic_prefix=True, full_original_input_and_tail=False,
                                             observed_returns=3000, comparison={'match': True, 'returns': [3000, 3000],
                                                                                'first_divergence': None},
                                             terminal_ram_match=None, terminal_card1_match=None, status='prefix_pass'),
                                 complete={'frame': 3001, 'input_frames': 3001, 'neutral_tail_ticks': 0})
    write_json(runs / 'split07fix-biohazard-prefix-01-ladder.json', {
        'schema': 'psx-tas-ladder-v1', 'status': 'diagnostic', 'tiers': [3000, 239202], 'endpoint': 239202,
        'results': [{'returns': 3000, 'full': False, 'status': 'prefix_pass', 'mechanical_match': True},
                    {'returns': 239202, 'full': True, 'status': 'diagnostic', 'mechanical_match': True}]})
    (runs / 'split07fix-biohazard-prefix-01-input.psxrti2').write_bytes(b'PSXRTI2' + bytes(32))
    out['bio-diag'] = make_run(runs, 'h2-biohazard-prefix-01', bio, 'BioHazard-TAS.exe', SHA['d'],
                               'source-comparison.json', bio_receipt(SHA['d'], SHA['a'], status='diagnostic'))
    out['tekken'] = make_run(runs, 'split07-tekken-replay-01', tek, 'Tekken3-TAS.exe', SHA['b'], 'verification.json',
                             {'status': 'pass', 'original_inputs': 7974, 'compared_returns': 8399, 'end_frame': 8400,
                              'reference': 'integrated202 native victory', 'expected_victory_time': '8.80',
                              'binary_sha256': SHA['b'], 'setup_executable_sha256': SHA['b'], 'binary_matches_setup': True,
                              'diagnostic_binary': None,
                              'streaming': {'compared_returns': 8399, 'first_divergence': None, 'stopped_on_divergence': False}},
                             exit_extra={'host_seconds': 278.8},
                             complete={'frame': 8400, 'input_frames': 7974, 'neutral_tail_ticks': 426})
    out['pepsiman'] = make_run(runs, 'split07-pepsiman-replay-01', pep, 'Pepsiman-TAS.exe', SHA['c'], 'verification.json',
                               {'status': 'pass', 'original_inputs': 71806, 'compared_returns': 75406, 'end_frame': 75407,
                                'source_observation_end': 75406, 'native_runner_exit': 0, 'first_divergence': None,
                                'captured_returns': [75406, 75406], 'input_identity_matches': True,
                                'terminal_ram_matches': True},
                               exit_extra={'host_seconds': 1886.7},
                               complete={'frame': 75407, 'input_frames': 71806, 'neutral_tail_ticks': 3601})
    out['no-receipt'] = make_run(runs, 'trace-control-01', bio, 'BioHazard-TAS.exe', SHA['a'], None, None,
                                 exit_extra={'timed_out': True, 'stop_reason': 'host_timeout', 'exit_code': None})
    return out


def cli(*argv, env=None):
    merged = dict(os.environ)
    merged.pop('PSX_TAS_EVIDENCE', None)
    merged.update(env or {})
    return subprocess.run([sys.executable, str(TOOL), *map(str, argv)], capture_output=True, text=True, env=merged)


def sums_lines(archive):
    return (archive / 'SHA256SUMS.txt').read_text(encoding='utf-8').splitlines()


# ------------------------------------------------------------- summaries

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    runs = build_fixtures(root)

    s = ar.build_summary(ar.load_run(runs['bio-full']))
    assert s['title'] == 'Bio Hazard' and s['title_key'] == 'biohazard' and s['receipt'] == 'source-comparison.json'
    assert s['verdict'] == 'pass' and s['scope'] == 'full route'
    assert (s['returns_compared'], s['returns_expected'], s['returns_match']) == (239202, 239202, True)
    assert s['binary_sha256'] == SHA['a'] and s['binary_matches_setup'] is True and s['binary_match_source'] == 'receipt'
    assert s['source_head'] == 'a' * 40 and s['source_tree'] == 'b' * 40 and s['setup_schema'] == 'biohazard-tas-candidate-v1'
    assert s['terminal_ram_match'] is True and s['terminal_card1_match'] is True and s['first_divergence'] is None
    assert s['host_seconds'] == 1234.5 and s['project'] == 'split07fix-biohazard' and s['ladder'] is None

    s = ar.build_summary(ar.load_run(runs['bio-fail']))
    assert s['verdict'] == 'fail' and s['returns_compared'] == 1142 and s['returns_match'] is False
    assert s['first_divergence']['frame'] == 1142 and s['stopped_on_divergence'] is True
    assert 'return 1,142' in ar.describe_divergence(s) and 'stopped there' in ar.describe_divergence(s)

    s = ar.build_summary(ar.load_run(runs['bio-prefix']))
    assert s['verdict'] == 'diagnostic' and s['scope'] == 'prefix' and s['returns_compared'] == 3000
    assert s['prefix_input'] == 'split07fix-biohazard-prefix-01-input.psxrti2'
    assert s['ladder']['status'] == 'diagnostic' and s['ladder']['completed_tiers'] == [3000, 239202]
    assert 'diagnostic prefix' in ar.describe_returns(s)

    s = ar.build_summary(ar.load_run(runs['bio-diag']))
    assert s['verdict'] == 'diagnostic' and s['binary_matches_setup'] is False and s['diagnostic_binary'] == SHA['d']
    assert s['setup_executable_sha256'] == SHA['a'] and 'DIAGNOSTIC' in ar.describe_binary(s)

    s = ar.build_summary(ar.load_run(runs['tekken']))
    assert s['title_key'] == 'tekken3' and s['verdict'] == 'pass' and s['receipt'] == 'verification.json'
    assert (s['returns_compared'], s['returns_expected'], s['original_inputs']) == (8399, 8399, 7974)
    assert s['setup_schema'] == 'psx-tas-setup-v1' and s['binary_matches_setup'] is True

    s = ar.build_summary(ar.load_run(runs['pepsiman']))
    assert s['title_key'] == 'pepsiman' and s['verdict'] == 'pass'
    assert (s['returns_compared'], s['returns_expected'], s['terminal_ram_match']) == (75406, 75406, True)
    # No identity fields in this (old-shape) receipt: the match is derived from setup.json and marked as such.
    assert s['binary_matches_setup'] is True and s['binary_match_source'] == 'setup.json'

    s = ar.build_summary(ar.load_run(runs['no-receipt']))
    assert s['title_key'] == 'biohazard' and s['verdict'] == 'incomplete' and s['receipt'] is None
    assert s['returns_compared'] is None and 'not compared' in ar.describe_returns(s)

    # A derived mismatch (old receipt, project setup.json names another exe) is reported, not judged.
    old = runs['pepsiman'] / 'verification.json'
    receipt = json.loads(old.read_text())
    setup_path = root / 'projects' / 'split07-pepsiman' / 'setup.json'
    setup = json.loads(setup_path.read_text())
    setup['executable_sha256'] = SHA['e']
    write_json(setup_path, setup)
    s = ar.build_summary(ar.load_run(runs['pepsiman']))
    assert s['verdict'] == 'pass' and s['binary_matches_setup'] is False and s['binary_match_source'] == 'setup.json'
    assert 'no identity flag' in ar.describe_binary(s)
    setup['executable_sha256'] = SHA['c']
    write_json(setup_path, setup)
    assert json.loads(old.read_text()) == receipt

    # Title detection fallbacks: exe name, then setup schema.
    assert ar.detect_title({'inputs': {'exe': {'path': 'x/Pepsiman-TAS.exe'}}}, {}, None) == 'pepsiman'
    assert ar.detect_title({'command': ['C:/x/Tekken3-TAS.exe']}, {}, None) == 'tekken3'
    assert ar.detect_title({}, {}, {'schema': 'biohazard-tas-candidate-v1'}) == 'biohazard'
    assert ar.detect_title({}, {}, None) == 'unknown'

    # ------------------------------------------------------ SHA256SUMS merge
    text, added = ar.merge_sums('', [('runs/b/x', SHA['1']), ('runs/a/y', SHA['2'])])
    assert added == [f'{SHA["2"]}  runs/a/y', f'{SHA["1"]}  runs/b/x'] and text == '\n'.join(added) + '\n'
    crlf = f'{SHA["3"]}  runs/z/first\r\n{SHA["4"]}  runs/z/second\r\n'
    text, added = ar.merge_sums(crlf, [('runs/z/second', SHA['4']), ('runs/a/new', SHA['5'])])
    assert added == [f'{SHA["5"]}  runs/a/new'] and text == crlf + f'{SHA["5"]}  runs/a/new\r\n'
    text, added = ar.merge_sums(crlf, [('runs/z/second', SHA['4'])])
    assert added == [] and text == crlf
    rejects(lambda: ar.merge_sums(crlf, [('runs/z/first', SHA['9'])]))
    rejects(lambda: ar.merge_sums('garbage line\n', []))
    text, added = ar.merge_sums('\ufeff' + f'{SHA["3"]} *runs/star\n', [('runs/new', SHA['6'])])
    assert text.startswith('\ufeff') and f'{SHA["3"]}  runs/star\n{SHA["6"]}  runs/new\n' in text

    # -------------------------------------------------------- note rendering
    import datetime
    when = datetime.datetime(2026, 9, 15, 10, 30)
    options = {'author': 'pegasus-claude', 'to': 'eagle-codex, eagle-claude', 'cc': 'operator',
               'task': 'TAS replay lane - evidence archive', 'label': 'split/07 requal'}
    summary = ar.build_summary(ar.load_run(runs['bio-full']))
    stem = ar.note_stem(summary, 'pegasus-claude', when)
    assert stem == '20260915-1030-PEGASUS-CLAUDE-biohazard-fix2-biohazard-replay-01'
    assert re.fullmatch(r'\d{8}-\d{4}-[A-Z-]+-[a-z0-9]+-[A-Za-z0-9._-]+', stem)
    note = ar.render_note(summary, r'Z:\ev\arch\runs\fix2-biohazard-replay-01', options, when)
    head = note.splitlines()[:6]
    assert head[0] == 'From: pegasus-claude (workbench)' and head[1] == 'To: eagle-codex, eagle-claude'
    assert head[2] == 'CC: operator' and head[3] == 'Date: 2026-09-15 10:30 (local)'
    assert head[4] == 'Task: TAS replay lane - evidence archive'
    assert head[5].startswith('Subject: split/07 requal: Bio Hazard fix2-biohazard-replay-01: PASS (239,202/239,202 returns)')
    assert note.splitlines()[6] == ''
    for needle in ('- Tree: source tree bbbbbbbbbbbb (head aaaaaaaaaaaa)', f'- Binary: {SHA["a"]} (matches the setup receipt)',
                   '- Returns: 239,202/239,202 returns', '- Terminal: terminal RAM equal, card1 equal', '- Divergence: none',
                   r'- Evidence: Z:\ev\arch\runs\fix2-biohazard-replay-01',
                   '- Receipt: ' + r'Z:\ev\arch\runs\fix2-biohazard-replay-01' + os.sep + 'source-comparison.json'):
        assert needle in note, needle
    assert '\r' not in note and note.endswith('\n')
    board = ar.render_board(summary, r'Z:\ev\arch\runs\fix2-biohazard-replay-01', options)
    assert board.count('\n') == 1 and not board.startswith('-') and '\n-' not in board
    for needle in ('split/07 requal: Bio Hazard run fix2-biohazard-replay-01 was a full-route replay',
                   'on source tree bbbbbbbbbbbb (head aaaaaaaaaaaa)', 'binary aaaaaaaaaaaa (matches the setup receipt)',
                   '239,202/239,202 returns', 'terminal RAM equal, card1 equal', 'no divergence',
                   'Verdict per the receipt: pass', r'Evidence: Z:\ev\arch\runs\fix2-biohazard-replay-01'):
        assert needle in board, needle
    prefix_board = ar.render_board(ar.build_summary(ar.load_run(runs['bio-prefix'])), 'E', options)
    assert 'diagnostic prefix replay' in prefix_board and 'ladder over tiers [3000, 239202] reported diagnostic' in prefix_board
    diag_note = ar.render_note(ar.build_summary(ar.load_run(runs['bio-diag'])), 'E', dict(options, label=None), when)
    assert 'DIAGNOSTIC binary declared in the receipt' in diag_note and 'Subject: Bio Hazard h2-' in diag_note
    fail_board = ar.render_board(ar.build_summary(ar.load_run(runs['bio-fail'])), 'E', options)
    assert 'first divergence at return 1,142 (cycle 1000 vs 1004), run stopped there' in fail_board

    # ------------------------------------------------------------- CLI: dry run
    evidence = root / 'evidence'
    mailbox = root / 'mailbox'
    result = cli(runs['bio-full'], 'arch-01', '--evidence-root', evidence, '--mailbox', mailbox, '--dry-run',
                 '--label', 'split/07 requal')
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith('DRY RUN') and '"verdict": "pass"' in result.stdout
    assert '--- note ---' in result.stdout and '--- board ---' in result.stdout
    assert 'would drop note into mailbox' in result.stdout
    assert not evidence.exists() and not mailbox.exists()

    # ------------------------------------------------------ CLI: archive twice
    result = cli(runs['bio-prefix'], 'arch-01', '--evidence-root', evidence, '--mailbox', mailbox)
    assert result.returncode == 0, result.stderr
    archive = evidence / 'arch-01'
    dest = archive / 'runs' / 'split07fix-biohazard-prefix-01'
    assert result.stdout.startswith('archived:') and dest.is_dir()
    assert ar.hash_tree(runs['bio-prefix']) == ar.hash_tree(dest)
    assert (archive / 'runs' / 'split07fix-biohazard-prefix-01-ladder.json').is_file()
    assert (archive / 'runs' / 'split07fix-biohazard-prefix-01-input.psxrti2').is_file()
    for name in ('setup.json', 'game.toml', 'bios.toml'):
        assert (archive / 'builds' / 'split07fix-biohazard' / name).is_file(), name
    assert not (archive / 'builds' / 'split07fix-biohazard' / 'input.json').exists()
    lines = sums_lines(archive)
    run_files = len(ar.walk_files(runs['bio-prefix']))
    assert len(lines) == run_files + 2 + 3 and len(set(lines)) == len(lines)
    assert all(re.fullmatch(r'[0-9a-f]{64}  [^ ].*', line) and '\\' not in line for line in lines)
    assert f'{ar.sha256_file(runs["bio-prefix"] / "exit.json")}  runs/split07fix-biohazard-prefix-01/exit.json' in lines
    raw = (archive / 'SHA256SUMS.txt').read_bytes()
    assert b'\r' not in raw and not raw.startswith(b'\xef\xbb\xbf')
    record = json.loads((archive / 'runs' / 'split07fix-biohazard-prefix-01.archive.json').read_text())
    assert record['file_count'] == run_files and record['bytes_match'] and record['already_archived'] is False
    assert record['summary']['verdict'] == 'diagnostic' and record['author'] == 'pegasus-claude'
    assert set(record['receipt_sha256']) == {'source-comparison.json', 'split07fix-biohazard-prefix-01-ladder.json'}
    notes = sorted((archive / 'notes').iterdir())
    assert len(notes) == 2 and notes[0].name.endswith('-board.md') and notes[1].name.endswith('.md')
    note_file = notes[1]
    assert re.fullmatch(r'\d{8}-\d{4}-PEGASUS-CLAUDE-biohazard-split07fix-biohazard-prefix-01\.md', note_file.name)
    assert (mailbox / note_file.name).read_bytes() == note_file.read_bytes()
    sidecar = (mailbox / (note_file.name + '.sha256')).read_text()
    assert sidecar == f'{ar.sha256_file(note_file)}  {note_file.name}\n'
    assert result.stdout.rstrip().splitlines()[-1] == notes[0].read_text(encoding='utf-8').rstrip()

    again = cli(runs['bio-prefix'], 'arch-01', '--evidence-root', evidence)
    assert again.returncode == 0, again.stderr
    assert again.stdout.startswith('already archived:') and '0 SHA256SUMS lines appended' in again.stdout
    assert sums_lines(archive) == lines
    assert sorted((archive / 'notes').iterdir()) == notes
    assert json.loads((archive / 'runs' / 'split07fix-biohazard-prefix-01.archive.json').read_text())['already_archived'] is True
    assert record['archived_at'] == json.loads((archive / 'runs' / 'split07fix-biohazard-prefix-01.archive.json').read_text())['archived_at']

    # Second run into the same archive: same builds dir is reused, not duplicated.
    result = cli(runs['bio-full'], 'arch-01', '--evidence-root', evidence)
    assert result.returncode == 0, result.stderr
    assert len(sums_lines(archive)) == len(lines) + len(ar.walk_files(runs['bio-full']))
    assert len(set(sums_lines(archive))) == len(sums_lines(archive))
    for other in ('tekken', 'pepsiman', 'bio-diag', 'no-receipt'):
        result = cli(runs[other], 'arch-01', '--evidence-root', evidence)
        assert result.returncode == 0, (other, result.stderr)

    # ------------------------------------------------------ tampered destination
    tampered = archive / 'runs' / 'split07-tekken-replay-01' / 'cpu-return.tsv'
    tampered.write_text('frame\tcycle\n1\t999\n')
    result = cli(runs['tekken'], 'arch-01', '--evidence-root', evidence)
    assert result.returncode == 1 and 'differs from the source' in result.stderr
    before = sums_lines(archive)
    (archive / 'runs' / 'split07-tekken-replay-01' / 'extra.txt').write_text('x')
    result = cli(runs['tekken'], 'arch-01', '--evidence-root', evidence)
    assert result.returncode == 1 and sums_lines(archive) == before

    # --------------------------------------------------- conflicting SHA256SUMS
    other = root / 'evidence-conflict' / 'arch-02'
    other.mkdir(parents=True)
    (other / 'SHA256SUMS.txt').write_bytes(f'{SHA["9"]}  runs/split07-pepsiman-replay-01/exit.json\r\n'.encode())
    result = cli(runs['pepsiman'], 'arch-02', '--evidence-root', other.parent)
    assert result.returncode == 1 and 'refusing to record' in result.stderr
    assert not (other / 'runs').exists(), 'conflict must be detected before anything is copied'
    assert (other / 'SHA256SUMS.txt').read_bytes() == f'{SHA["9"]}  runs/split07-pepsiman-replay-01/exit.json\r\n'.encode()
    assert ar.read_sums(other / 'SHA256SUMS.txt').endswith('\r\n')
    # CRLF archives (the PowerShell-written kind) keep their line endings on append.
    (other / 'SHA256SUMS.txt').write_bytes(f'{SHA["9"]}  builds/other/file\r\n'.encode())
    result = cli(runs['pepsiman'], 'arch-02', '--evidence-root', other.parent)
    assert result.returncode == 0, result.stderr
    raw = (other / 'SHA256SUMS.txt').read_bytes()
    assert raw.startswith(SHA['9'].encode() + b'  builds/other/file\r\n') and b'\n' not in raw.replace(b'\r\n', b'')

    # --------------------------------------------------------------- usage errors
    result = cli(root / 'missing', 'arch-03', '--evidence-root', evidence)
    assert result.returncode == 2
    bad = root / 'bad-run'
    bad.mkdir()
    write_json(bad / 'manifest.json', {})
    assert cli(bad, 'arch-03', '--evidence-root', evidence).returncode == 2
    assert cli(runs['tekken'], 'nested/name', '--evidence-root', evidence).returncode == 2
    result = cli(runs['tekken'], 'arch-env', env={'PSX_TAS_EVIDENCE': str(root / 'from-env')})
    assert result.returncode == 0 and (root / 'from-env' / 'arch-env' / 'runs' / 'split07-tekken-replay-01').is_dir()

print('synthetic archive tests passed')

# ------------------------------------------------ read-only dry runs on real evidence
if REAL.is_dir():
    with tempfile.TemporaryDirectory() as directory:
        for name in ('fix2-biohazard-replay-01', 'split07-tekken-replay-01', 'split07-pepsiman-replay-01'):
            run = REAL / name
            if not run.is_dir():
                print(f'skipped (absent): {run}')
                continue
            result = cli(run, 'split07-requal-20260914', '--evidence-root', directory, '--dry-run', '--label', 'split/07 requal')
            print(f'===== dry run: {run}')
            print(result.stdout)
            assert result.returncode == 0, result.stderr
            assert not any(Path(directory).iterdir()), 'dry run wrote into the evidence root'
            summary = json.loads(result.stdout.split('\n{', 1)[1].split('\n}', 1)[0].join('{}'))
            assert summary['verdict'] == 'pass' and summary['returns_match'] is True, name
else:
    print(f'skipped real-evidence dry runs: {REAL} not reachable')
print('archive_run tests passed')
