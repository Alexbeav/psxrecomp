"""Record and verify independent resumes using an unchanged run_native argument list."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

from compare_checkpoint_runs import compare_runs, saved_states


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-args', type=Path, required=True,
                        help='JSON list of run_native options, excluding output, save/resume and perturb options')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--terminal', type=int, required=True)
    parser.add_argument('--midpoint-only', action='store_true',
                        help='one full cold run and one fresh midpoint resume; otherwise test 500/1500/2500')
    args = parser.parse_args()
    if args.terminal <= 2560:
        parser.error('terminal must exceed 2560')
    common = json.loads(args.run_args.read_text())
    forbidden = {'--save-state-at', '--resume-from', '--perturb-restore'}
    if not isinstance(common, list) or not all(isinstance(s, str) for s in common):
        parser.error('run-args must contain a JSON list of strings')
    if any(s.split('=')[0] in forbidden for s in common):
        parser.error('save/resume and perturb options belong to the cohort driver')
    source = Path(__file__).resolve().parents[2]
    if subprocess.check_output(['git', 'status', '--porcelain'], cwd=source).strip():
        parser.error('commit source changes before recording a qualification cohort')
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    args.output.mkdir(parents=True, exist_ok=False)
    points = sorted({500, 501, 560, 1500, 1501, 1560, 2500, 2501, 2560, args.terminal})
    cases = [('A2', 0, False)] + [(f'resume{k}', k, False) for k in (500, 1500, 2500)]
    if args.midpoint_only:
        midpoint = args.terminal // 2
        points = sorted(set(points + [midpoint, midpoint+1, midpoint+60]))
        cases = [('midpoint', midpoint, False)]
    else:
        cases.append(('negative-timer2', 2500, True))
    report = dict(source_commit=head, terminal=args.terminal, captures=points,
                  run_args=common, runs={}, comparisons={}, passed=False)

    def save():
        (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')

    def run(name, start=0, negative=False):
        # start is the return A1 actually captured for the requested resume point;
        # a request that fell on a return with no represented continuation moved later.
        command = [sys.executable, '-B', str(source/'tools/tasreplays/run_native.py'),
                   str(args.output/name), *common, '--save-state-at',
                   *map(str, (n for n in points if n > start))]
        if start:
            command += ['--resume-from', str(args.output/'A1'/f'tas-state-{start:06d}.pst')]
        if negative:
            command += ['--perturb-restore', 'timer2_counter']
        print('START', name, flush=True)
        begin = time.monotonic()
        with (args.output/(name+'.log')).open('w') as log:
            code = subprocess.call(command, cwd=source, stdout=log, stderr=subprocess.STDOUT)
        report['runs'][name] = dict(command=command, exit_code=code, seconds=time.monotonic()-begin)
        save()
        if code:
            raise RuntimeError(f'{name} exited {code}')
        print('FINISH', name, flush=True)

    try:
        run('A1')
        captured = {frame: int(state[len('tas-state-'):-len('.pst')])
                    for frame, state in saved_states(args.output/'A1').items()}
        report['A1_captures'] = captured
        for name, requested, negative in cases:
            if requested and requested not in captured:
                raise RuntimeError(f'A1 has no checkpoint for requested return {requested}')
            start = captured[requested] if requested else 0
            run(name, start, negative)
            result = compare_runs(args.output/'A1', args.output/name, start, args.terminal, points)
            result['requested_start'] = requested
            if negative:
                result['negative_control_passed'] = any(
                    not state['identical'] for state in result['states'].values())
                passed = result['negative_control_passed']
            else:
                passed = result['passed']
            report['comparisons'][name] = result
            save()
            print('COMPARE', name, 'PASS' if passed else 'FAIL', flush=True)
            if not passed:
                raise RuntimeError(f'{name} comparison failed')
        report['passed'] = True
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        report['error'] = str(error)
    save()
    print('COHORT', 'PASS' if report['passed'] else 'FAIL', args.output, flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
