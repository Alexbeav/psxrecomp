"""Locate the first instruction-level divergence between two native TAS runs.

This is a diagnostic, not a qualification. Given two completed native runs and
a completed frontend return R, it bounds a guest-cycle window around returns
R-1..R, reruns a route prefix through the title harness once per side with
that side's own binary (resolved by SHA-256 from the run directory, the
manifest path or an evidence archive), and reports the first CPU boundary row
that differs. Cumulative slice counters are ignored. Nothing here admits a
binary, alters inputs, or decides which side is correct.
"""
import argparse
import hashlib
from itertools import zip_longest
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
WINDOW_LIMIT = 1000000
IGNORED_COLUMNS = ('slice_takes', 'slice_cycle')
DEFAULT_EVIDENCE = Path(r'Z:\Share\psxrecomp\tas-evidence')
TITLES = {'psx-tas-setup-v1': 'tekken3', 'pepsiman-tas-candidate-v1': 'pepsiman',
          'biohazard-tas-candidate-v1': 'biohazard'}
RETURN_HEADER = ['frame', 'pc', 'cycle', 'sr', 'cause', 'epc'] + [f'r{i}' for i in range(32)]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8', newline='\n')


def return_cycles(path, frames):
    """Map each requested completed return to (pc, cycle) from cpu-return.tsv."""
    wanted, found = set(frames), {}
    with Path(path).open(encoding='utf-8', newline='') as stream:
        if stream.readline().rstrip('\r\n').split('\t') != RETURN_HEADER:
            raise ValueError(f'invalid CPU return columns: {path}')
        for number, line in enumerate(stream, 1):
            columns = line.rstrip('\r\n').split('\t')
            if len(columns) != len(RETURN_HEADER) or not columns[0].isdigit() or not columns[2].isdigit():
                raise ValueError(f'malformed CPU return row {number}: {path}')
            if int(columns[0]) in wanted:
                found[int(columns[0])] = (columns[1], int(columns[2]))
                if len(found) == len(wanted):
                    break
    missing = sorted(wanted - found.keys())
    if missing:
        raise ValueError(f'{path} lacks completed return(s) {missing}')
    return found


def compute_window(cycles_a, cycles_b, ret, margin_cycles, limit=WINDOW_LIMIT):
    """Bound [min cycle at R-1, max cycle at R + margin] to the runtime window limit."""
    if type(ret) is not int or ret < 2:
        raise ValueError('return must be an integer >= 2 so that return R-1 exists')
    if margin_cycles < 0:
        raise ValueError('negative cycle margin')
    for side in (cycles_a, cycles_b):
        if side[ret] <= side[ret - 1]:
            raise ValueError(f'return clock does not advance between returns {ret-1} and {ret}')
    low = min(cycles_a[ret - 1], cycles_b[ret - 1])
    requested = max(cycles_a[ret], cycles_b[ret]) + margin_cycles
    high = min(requested, low + limit)
    return dict(low=low, high=high, requested_high=requested, clamped=high < requested, limit=limit,
                cycles=dict(a={str(ret - 1): cycles_a[ret - 1], str(ret): cycles_a[ret]},
                            b={str(ret - 1): cycles_b[ret - 1], str(ret): cycles_b[ret]}))


def find_evidence_exe(root, sha):
    """First .exe listed in <root>/*/SHA256SUMS.txt with this hash that still hashes to it.

    Returns (path or None, stale entries whose file is missing or no longer matches)."""
    sha, root, stale = sha.lower(), Path(root), []
    for sums in sorted(root.glob('*/SHA256SUMS.txt')) if root.is_dir() else []:
        for line in sums.read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(r'([0-9a-fA-F]{64})\s+\*?(.+)', line.strip())
            if not match or match[1].lower() != sha or not match[2].lower().endswith('.exe'):
                continue
            candidate = sums.parent / match[2].strip()
            if candidate.is_file() and digest(candidate) == sha:
                return candidate, stale
            stale.append(str(candidate))
    return None, stale


def resolve_binary(run_dir, manifest, evidence_root):
    """Staged copy in the run directory, then the manifest path, then evidence archives."""
    exe = manifest['inputs']['exe']
    sha, original = exe['sha256'].lower(), Path(exe['path'])
    staged = Path(run_dir) / original.name
    if staged.is_file() and digest(staged) == sha:
        return dict(path=str(staged), sha256=sha, source='staged copy in run directory')
    if original.is_file() and digest(original) == sha:
        return dict(path=str(original), sha256=sha, source='manifest inputs.exe.path')
    found, stale = find_evidence_exe(evidence_root, sha)
    if found:
        archive = found.relative_to(Path(evidence_root)).parts[0]
        return dict(path=str(found), sha256=sha, source=f'evidence archive {archive}')
    raise ValueError(f'no binary with SHA-256 {sha} at {staged}, {original} or under {evidence_root}'
                     + (f'; stale evidence entries rejected: {stale}' if stale else ''))


def harness_endpoint(schema, info, reference=None):
    """Cheap best-effort terminal return; None when unknown (the harness then decides)."""
    try:
        if schema == 'psx-tas-setup-v1':
            return 8399
        ref = reference or info.get('reference')
        if ref:
            return int(read_json(ref)['observed_returns'])
        if schema == 'pepsiman-tas-candidate-v1':
            return 71806
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def harness_command(schema, setup_path, output, returns, exe, sha, window, reference=None):
    title = TITLES.get(schema)
    if title is None:
        raise ValueError(f'unknown setup schema {schema!r}; expected one of {sorted(TITLES)}')
    if reference and title != 'pepsiman':
        raise ValueError('--reference applies to the Pepsiman harness only')
    setup_path, output, script = Path(setup_path), Path(output), str(HERE / f'{title}.py')
    if title == 'biohazard':
        argv = [sys.executable, script, 'run', str(setup_path), str(output)]
    elif title == 'pepsiman':
        argv = [sys.executable, script, 'run', '--project', str(setup_path.parent), '--output', str(output)]
        argv += ['--reference', str(reference)] if reference else []
    else:
        argv = [sys.executable, script, 'run', '--project', str(setup_path.parent), '--output', str(output), '--headless']
    return argv + ['--returns', str(returns), '--exe', str(exe), '--diagnostic-binary', sha,
                   '--cpu-boundary-window', str(window['low']), str(window['high'])]


def diff_traces(path_a, path_b, ignored=IGNORED_COLUMNS):
    """Stream both CPU boundary traces; stop at the first row differing outside ignored columns."""
    with Path(path_a).open(encoding='utf-8', newline='') as left, \
            Path(path_b).open(encoding='utf-8', newline='') as right:
        header = left.readline().rstrip('\r\n').split('\t')
        if header != right.readline().rstrip('\r\n').split('\t'):
            raise ValueError('CPU boundary headers differ')
        if 'pc' not in header or 'cycle' not in header:
            raise ValueError('CPU boundary header lacks pc/cycle')
        compared = [i for i, name in enumerate(header) if name not in ignored]
        pc, cycle = header.index('pc'), header.index('cycle')
        cell = lambda row, i: row[i] if i < len(row) else None
        identical = 0
        for row, (la, lb) in enumerate(zip_longest(left, right), 1):
            if la == lb:
                identical += 1
                continue
            if la is None or lb is None:
                present = (la or lb).rstrip('\r\n').split('\t')
                return dict(identical_rows=identical, rows=None, first_difference=dict(
                    row=row, missing_side='a' if la is None else 'b',
                    pc=cell(present, pc), cycle=cell(present, cycle), columns={}))
            a, b = la.rstrip('\r\n').split('\t'), lb.rstrip('\r\n').split('\t')
            columns = {header[i]: [cell(a, i), cell(b, i)] for i in compared if cell(a, i) != cell(b, i)}
            if not columns:
                identical += 1
                continue
            return dict(identical_rows=identical, rows=dict(a=dict(zip(header, a)), b=dict(zip(header, b))),
                        first_difference=dict(row=row, missing_side=None, pc=[cell(a, pc), cell(b, pc)],
                                              cycle=[cell(a, cycle), cell(b, cycle)], columns=columns))
        return dict(identical_rows=identical, rows=None, first_difference=None)


def report_difference(result):
    print(f'identical rows: {result["identical_rows"]}')
    first = result['first_difference']
    if first is None:
        print('traces identical over the window (ignoring ' + ', '.join(IGNORED_COLUMNS) + ')')
    elif first['missing_side']:
        other = 'b' if first['missing_side'] == 'a' else 'a'
        print(f'side {first["missing_side"]} ended first: row {first["row"]} exists only on side {other} '
              f'(pc={first["pc"]} cycle={first["cycle"]})')
    else:
        print(f'first difference: row {first["row"]} pc a={first["pc"][0]} b={first["pc"][1]} '
              f'cycle a={first["cycle"][0]} b={first["cycle"][1]}')
        for name, (x, y) in first['columns'].items():
            print(f'  {name}: a={x} b={y}')


def diverge(args):
    sides = dict(a=Path(args.run_a), b=Path(args.run_b))
    report = {'scope': __doc__.strip(), 'return': args.ret, 'window': None, 'binaries': None, 'setup': None,
              'title': None, 'returns': None, 'commands': None, 'return_codes': None, 'traces': None,
              'identical_rows': None, 'first_difference': None, 'rows': None}
    if args.compare_only:
        traces = {s: d / 'cpu-boundary.tsv' for s, d in sides.items()}
    else:
        if args.ret is None:
            raise ValueError('return is required unless --compare-only')
        manifests = {s: read_json(d / 'manifest.json') for s, d in sides.items()}
        for key in ('game', 'route'):
            if manifests['a']['inputs'].get(key, {}).get('sha256') != manifests['b']['inputs'].get(key, {}).get('sha256'):
                print(f'note: manifest inputs.{key} differs between sides; the rerun uses run-b\'s project for both')
        cycles = {s: return_cycles(d / 'cpu-return.tsv', (args.ret - 1, args.ret)) for s, d in sides.items()}
        window = compute_window({f: c for f, (_, c) in cycles['a'].items()},
                                {f: c for f, (_, c) in cycles['b'].items()}, args.ret, args.margin_cycles)
        for s in sides:
            print(f'{s}: return {args.ret-1} pc {cycles[s][args.ret-1][0]} cycle {cycles[s][args.ret-1][1]}; '
                  f'return {args.ret} pc {cycles[s][args.ret][0]} cycle {cycles[s][args.ret][1]}')
        print(f'window: {window["low"]}..{window["high"]} ({window["high"]-window["low"]} cycles)'
              + (f'; clamped from requested high {window["requested_high"]} to the {window["limit"]}-cycle limit'
                 if window['clamped'] else ''))
        binaries = {s: resolve_binary(d, manifests[s], args.evidence_root) for s, d in sides.items()}
        for s, b in binaries.items():
            print(f'binary {s}: {b["path"]} sha256 {b["sha256"]} ({b["source"]})')
        setup_path = (args.setup or Path(manifests['b']['inputs']['game']['path']).parent / 'setup.json').resolve()
        info = read_json(setup_path)
        schema = info.get('schema')
        if schema not in TITLES:
            raise ValueError(f'unknown setup schema {schema!r} in {setup_path}; expected one of {sorted(TITLES)}')
        endpoint = harness_endpoint(schema, info, args.reference)
        if endpoint is not None and args.ret > endpoint:
            raise ValueError(f'return {args.ret} is beyond the harness endpoint {endpoint}')
        returns = args.ret + args.margin_returns if endpoint is None else min(args.ret + args.margin_returns, endpoint)
        print(f'title: {TITLES[schema]} ({schema}); setup {setup_path}; rerun --returns {returns}'
              + ('' if endpoint is not None else ' (endpoint unknown; the harness reports overruns)'))
        commands = {s: harness_command(schema, setup_path, Path(args.output) / s, returns, binaries[s]['path'],
                                       binaries[s]['sha256'], window, args.reference) for s in sides}
        for s, argv in commands.items():
            print(f'command {s}: ' + subprocess.list2cmdline(argv))
        report.update(window=window, binaries=binaries, setup=str(setup_path), title=TITLES[schema],
                      returns=returns, commands=commands)
        if args.dry_run:
            print('dry run: nothing executed, nothing written')
            return 0
        Path(args.output).mkdir(parents=True, exist_ok=True)
        codes = {}
        for s, argv in commands.items():
            print(f'--- rerunning side {s}', flush=True)
            codes[s] = subprocess.run(argv).returncode
            print(f'--- side {s} exit code {codes[s]}', flush=True)
        report['return_codes'] = codes
        traces = {s: Path(args.output) / s / 'cpu-boundary.tsv' for s in sides}
    for s, path in traces.items():
        if not path.is_file():
            raise ValueError(f'missing CPU boundary trace for side {s}: {path}')
    result = diff_traces(traces['a'], traces['b'])
    report_difference(result)
    report.update(traces={s: str(p) for s, p in traces.items()}, **result)
    Path(args.output).mkdir(parents=True, exist_ok=True)
    target = Path(args.output) / 'diverge-trace.json'
    if target.exists():
        raise ValueError(f'refusing to overwrite {target}')
    write_json(target, report)
    print(f'report: {target}')
    return 0 if result['first_difference'] else 2


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run_a', type=Path, help='control native run directory (manifest.json, cpu-return.tsv)')
    parser.add_argument('run_b', type=Path, help='candidate native run directory; its project supplies the setup')
    parser.add_argument('ret', type=int, nargs='?', metavar='return', help='completed frontend return R (>= 2)')
    parser.add_argument('--output', type=Path, required=True,
                        help='directory for <output>/a, <output>/b and diverge-trace.json')
    parser.add_argument('--setup', type=Path, help='title setup.json; default <project>/setup.json from run-b')
    parser.add_argument('--reference', type=Path, help='independent source reference (Pepsiman harness only)')
    parser.add_argument('--margin-returns', type=int, default=50,
                        help='rerun through return R + this, bounded by the harness endpoint')
    parser.add_argument('--margin-cycles', type=int, default=65536, help='cycles after return R kept in the window')
    parser.add_argument('--evidence-root', type=Path, default=Path(os.environ.get('PSX_TAS_EVIDENCE') or DEFAULT_EVIDENCE),
                        help='directory of <archive>/SHA256SUMS.txt for binary lookup by hash')
    parser.add_argument('--compare-only', action='store_true',
                        help='run-a/run-b already hold cpu-boundary.tsv; skip resolution and reruns')
    parser.add_argument('--dry-run', action='store_true', help='print the window and both harness commands only')
    args = parser.parse_args(argv)
    try:
        return diverge(args)
    except (ValueError, OSError, KeyError, TypeError, RuntimeError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
