"""Window, binary resolution, evidence lookup and trace diff checks without retail assets."""
from pathlib import Path
import hashlib, json, sys, tempfile
import diverge_trace as dt

BOUNDARY = ('pc cycle cache_tag cache_active sr cause epc ' + ' '.join(f'r{i}' for i in range(32)) +
            ' read_fudge ld_absorb ld_which read_absorb_which dma_read_wait precise dirty i_stat i_mask'
            ' slice_pc slice_deadline slice_bound slice_cycle slice_takes').split()
sha = lambda data: hashlib.sha256(data).hexdigest()

def rejects(call, exc=ValueError):
    try: call()
    except exc: return
    raise AssertionError('accepted invalid input')

def write_returns(path, cycles):
    rows = ['\t'.join(dt.RETURN_HEADER)]
    for frame, cycle in enumerate(cycles, 1):
        rows.append('\t'.join([str(frame), f'{0x80010000+4*frame:08X}', str(cycle)] + ['00000000'] * 35))
    path.write_text('\r\n'.join(rows) + '\r\n', newline='')

def boundary_row(pc, cycle, **over):
    row = dict.fromkeys(BOUNDARY, '00000000'); row.update(pc=f'{pc:08X}', cycle=str(cycle), slice_takes='7')
    row.update({k: str(v) for k, v in over.items()}); return [row[c] for c in BOUNDARY]

def write_boundary(path, rows, header=BOUNDARY):
    path.write_text('\t'.join(header) + '\r\n' + ''.join('\t'.join(r) + '\r\n' for r in rows), newline='')

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    # Window: LO = min cycle at R-1, HI = max cycle at R + margin, bounded to the runtime limit.
    a, b = root / 'a.tsv', root / 'b.tsv'
    write_returns(a, [100, 200, 300, 400]); write_returns(b, [100, 190, 310, 390])
    ca, cb = dt.return_cycles(a, (2, 3)), dt.return_cycles(b, (2, 3))
    assert ca == {2: ('80010008', 200), 3: ('8001000C', 300)} and cb[2][1] == 190 and cb[3][1] == 310
    cycles = lambda found: {f: c for f, (_, c) in found.items()}
    window = dt.compute_window(cycles(ca), cycles(cb), 3, 50)
    assert (window['low'], window['high'], window['requested_high'], window['clamped']) == (190, 360, 360, False)
    assert window['cycles'] == dict(a={'2': 200, '3': 300}, b={'2': 190, '3': 310})
    window = dt.compute_window({2: 200, 3: 5000000}, {2: 190, 3: 310}, 3, 50)
    assert (window['low'], window['high'], window['requested_high'], window['clamped']) == (190, 1000190, 5000050, True)
    assert window['high'] - window['low'] == dt.WINDOW_LIMIT == 1000000
    rejects(lambda: dt.compute_window({0: 1, 1: 2}, {0: 1, 1: 2}, 1, 0))
    rejects(lambda: dt.compute_window({2: 200, 3: 200}, {2: 190, 3: 310}, 3, 0))
    rejects(lambda: dt.compute_window(cycles(ca), cycles(cb), 3, -1))
    rejects(lambda: dt.return_cycles(a, (4, 5)))
    (root / 'bad.tsv').write_text('frame\tpc\n1\t0\n'); rejects(lambda: dt.return_cycles(root / 'bad.tsv', (1,)))
    (root / 'short.tsv').write_text('\t'.join(dt.RETURN_HEADER) + '\n1\t80010000\n')
    rejects(lambda: dt.return_cycles(root / 'short.tsv', (1,)))

    # Trace diff: streaming, ignoring the cumulative slice counters, 1-based data rows.
    rows = [boundary_row(0x80066604 + 4 * i, 645374001 + i, i_stat='00000000') for i in range(4)]
    write_boundary(a, rows); write_boundary(b, rows)
    assert dt.diff_traces(a, b) == dict(identical_rows=4, rows=None, first_difference=None)
    other = [r[:] for r in rows]
    for r in other: r[BOUNDARY.index('slice_takes')] = '3'; r[BOUNDARY.index('slice_cycle')] = '99'
    write_boundary(b, other)
    assert dt.diff_traces(a, b)['first_difference'] is None and dt.diff_traces(a, b)['identical_rows'] == 4
    other[2][BOUNDARY.index('i_stat')] = '00000004'; other[2][BOUNDARY.index('cause')] = '00000400'
    write_boundary(b, other)
    result = dt.diff_traces(a, b)
    first = result['first_difference']
    assert result['identical_rows'] == 2 and first['row'] == 3 and first['missing_side'] is None
    assert first['pc'] == ['8006660C', '8006660C'] and first['cycle'] == ['645374003', '645374003']
    assert first['columns'] == {'cause': ['00000000', '00000400'], 'i_stat': ['00000000', '00000004']}
    assert result['rows']['a']['i_stat'] == '00000000' and result['rows']['b']['slice_takes'] == '3'
    assert dt.diff_traces(a, b, ignored=('slice_takes', 'slice_cycle', 'i_stat', 'cause'))['first_difference'] is None
    write_boundary(b, other[:2])
    result = dt.diff_traces(a, b)
    assert result['identical_rows'] == 2 and result['first_difference'] == dict(
        row=3, missing_side='b', pc='8006660C', cycle='645374003', columns={})
    write_boundary(a, rows[:1]); write_boundary(b, rows)
    assert dt.diff_traces(a, b)['first_difference']['missing_side'] == 'a'
    write_boundary(b, rows, header=BOUNDARY[:-1]); rejects(lambda: dt.diff_traces(a, b))
    write_boundary(b, [r[:-1] for r in rows]); assert dt.diff_traces(a, b)['first_difference']['columns'] == {}
    write_boundary(b, [r[:-3] for r in rows]); assert dt.diff_traces(a, b)['first_difference']['columns'] == {'slice_bound': ['00000000', None]}

    # Evidence lookup: only .exe entries that still hash correctly; stale entries are reported and skipped.
    evidence = root / 'evidence'; good, stale, later = b'good binary', b'stale binary', b'later binary'
    for archive, entries in (('arch-1', [(sha(good), 'runs/x/Fake-TAS.exe', good), (sha(stale), 'runs/y/Fake-TAS.exe', b'rewritten'),
                                         (sha(later), 'runs/z/Missing.exe', None), (sha(b'text'), 'runs/x/notes.exe.txt', b'text')]),
                             ('arch-2', [(sha(later), 'builds/Later-TAS.exe', later)])):
        lines = []
        for digest, relative, data in entries:
            if data is not None:
                target = evidence / archive / relative; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
            lines.append(f'{digest}  {relative}')
        (evidence / archive / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n')
    found, rejected = dt.find_evidence_exe(evidence, sha(good))
    assert found == evidence / 'arch-1/runs/x/Fake-TAS.exe' and rejected == []
    found, rejected = dt.find_evidence_exe(evidence, sha(stale))
    assert found is None and rejected == [str(evidence / 'arch-1/runs/y/Fake-TAS.exe')]
    found, rejected = dt.find_evidence_exe(evidence, sha(later).upper())
    assert found == evidence / 'arch-2/builds/Later-TAS.exe' and rejected == [str(evidence / 'arch-1/runs/z/Missing.exe')]
    assert dt.find_evidence_exe(evidence, sha(b'text')) == (None, [])
    assert dt.find_evidence_exe(root / 'absent', sha(good)) == (None, [])

    # Binary resolution order: staged copy, manifest path, evidence archive, then a named failure.
    original = root / 'build/Fake-TAS.exe'; original.parent.mkdir(); original.write_bytes(good)
    run = root / 'run'; run.mkdir(); (run / 'Fake-TAS.exe').write_bytes(good)
    manifest = {'inputs': {'exe': {'path': str(original), 'sha256': sha(good).upper()}}}
    resolved = dt.resolve_binary(run, manifest, evidence)
    assert resolved == dict(path=str(run / 'Fake-TAS.exe'), sha256=sha(good), source='staged copy in run directory')
    (run / 'Fake-TAS.exe').write_bytes(b'corrupted staged copy')
    assert dt.resolve_binary(run, manifest, evidence)['source'] == 'manifest inputs.exe.path'
    original.unlink()
    resolved = dt.resolve_binary(run, manifest, evidence)
    assert resolved['path'] == str(evidence / 'arch-1/runs/x/Fake-TAS.exe') and resolved['source'] == 'evidence archive arch-1'
    manifest['inputs']['exe']['sha256'] = sha(stale)
    try: dt.resolve_binary(run, manifest, evidence); raise AssertionError('stale evidence accepted')
    except ValueError as error: assert sha(stale) in str(error) and 'stale' in str(error)
    manifest['inputs']['exe']['sha256'] = sha(b'nowhere')
    try: dt.resolve_binary(run, manifest, root / 'absent'); raise AssertionError('missing binary accepted')
    except ValueError as error: assert sha(b'nowhere') in str(error) and 'stale' not in str(error)

    # Harness commands follow each title's run CLI; the endpoint bounds the rerun prefix.
    window = dict(low=2913822136, high=2914388400)
    setup = root / 'project/setup.json'; setup.parent.mkdir()
    tail = ['--returns', '5199', '--exe', 'X.exe', '--diagnostic-binary', 'abc', '--cpu-boundary-window', '2913822136', '2914388400']
    argv = dt.harness_command('biohazard-tas-candidate-v1', setup, root / 'out/a', 5199, 'X.exe', 'abc', window)
    assert argv == [sys.executable, str(dt.HERE / 'biohazard.py'), 'run', str(setup), str(root / 'out/a')] + tail
    argv = dt.harness_command('pepsiman-tas-candidate-v1', setup, root / 'out/b', 5199, 'X.exe', 'abc', window, root / 'ref.json')
    assert argv == [sys.executable, str(dt.HERE / 'pepsiman.py'), 'run', '--project', str(setup.parent), '--output',
                    str(root / 'out/b'), '--reference', str(root / 'ref.json')] + tail
    argv = dt.harness_command('psx-tas-setup-v1', setup, root / 'out/a', 5199, 'X.exe', 'abc', window)
    assert argv == [sys.executable, str(dt.HERE / 'tekken3.py'), 'run', '--project', str(setup.parent), '--output',
                    str(root / 'out/a'), '--headless'] + tail
    rejects(lambda: dt.harness_command('unknown-v1', setup, root / 'out/a', 5199, 'X.exe', 'abc', window))
    rejects(lambda: dt.harness_command('psx-tas-setup-v1', setup, root / 'out/a', 5199, 'X.exe', 'abc', window, root / 'ref.json'))
    (root / 'ref.json').write_text(json.dumps({'observed_returns': 239202}))
    assert dt.harness_endpoint('psx-tas-setup-v1', {}) == 8399
    assert dt.harness_endpoint('biohazard-tas-candidate-v1', {'reference': str(root / 'ref.json')}) == 239202
    assert dt.harness_endpoint('biohazard-tas-candidate-v1', {'reference': str(root / 'missing.json')}) is None
    assert dt.harness_endpoint('pepsiman-tas-candidate-v1', {}) == 71806
    assert dt.harness_endpoint('pepsiman-tas-candidate-v1', {}, root / 'ref.json') == 239202

    # Whole tool: --compare-only exit codes and report; --dry-run resolves everything and executes nothing.
    ca, cb = root / 'cmp/a', root / 'cmp/b'; ca.mkdir(parents=True); cb.mkdir()
    write_boundary(ca / 'cpu-boundary.tsv', rows); write_boundary(cb / 'cpu-boundary.tsv', other)
    assert dt.main([str(ca), str(cb), '--compare-only', '--output', str(root / 'cmp/out')]) == 0
    report = json.loads((root / 'cmp/out/diverge-trace.json').read_text())
    assert report['first_difference']['row'] == 3 and report['identical_rows'] == 2 and report['return'] is None
    assert report['commands'] is None and report['return_codes'] is None and report['binaries'] is None
    assert report['rows']['b']['cause'] == '00000400' and report['traces']['a'] == str(ca / 'cpu-boundary.tsv')
    assert dt.main([str(ca), str(cb), '7', '--compare-only', '--output', str(root / 'cmp/out')]) == 1  # no overwrite
    write_boundary(cb / 'cpu-boundary.tsv', rows)
    assert dt.main([str(ca), str(cb), '--compare-only', '--output', str(root / 'cmp/same')]) == 2
    assert json.loads((root / 'cmp/same/diverge-trace.json').read_text())['first_difference'] is None
    assert dt.main([str(ca), str(root / 'cmp/none'), '--compare-only', '--output', str(root / 'cmp/err')]) == 1
    assert dt.main([str(ca), str(cb), '--output', str(root / 'cmp/err')]) == 1  # return required for reruns
    for side, cycles in (('a', [100, 2913822136, 2914388268]), ('b', [100, 2913822136, 2914388268])):
        run = root / 'live' / side; run.mkdir(parents=True)
        write_returns(run / 'cpu-return.tsv', cycles); (run / 'Fake-TAS.exe').write_bytes(good)
        dt.write_json(run / 'manifest.json', {'inputs': {'exe': {'path': str(root / 'gone/Fake-TAS.exe'), 'sha256': sha(good)},
                                                         'game': {'path': str(setup.parent / 'game.toml'), 'sha256': 'g'},
                                                         'route': {'path': 'r', 'sha256': 'r'}}})
    dt.write_json(setup, {'schema': 'biohazard-tas-candidate-v1', 'reference': str(root / 'ref.json')})
    live = [str(root / 'live/a'), str(root / 'live/b'), '3', '--output', str(root / 'live/out'), '--evidence-root', str(evidence)]
    assert dt.main(live + ['--dry-run']) == 0 and not (root / 'live/out').exists()
    assert dt.main(live + ['--dry-run', '--margin-returns', '300000']) == 0
    assert dt.main(live + ['--dry-run', '--reference', str(root / 'ref.json')]) == 1  # Pepsiman only
    dt.write_json(root / 'other.json', {'schema': 'unknown-v1'})
    assert dt.main(live + ['--dry-run', '--setup', str(root / 'other.json')]) == 1
    assert dt.main([str(root / 'live/a'), str(root / 'live/b'), '1', '--output', str(root / 'live/out'), '--dry-run']) == 1
print('diverge_trace: window bounds, binary/evidence resolution, harness commands and streaming trace diff pass')
