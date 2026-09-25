"""Independent per-transition pipeline model derived from authored observations.

Clock publication and external memory seams are deliberately separate checks.
"""
import argparse
import json
from pathlib import Path
from validate_cpu_timing_trace import read_trace, require


def predict_pipeline(before, op, profile):
    absorb = before['absorb'].copy()
    which, fudge, pending, amount = before['pipeline']
    kind = op['op']
    charges = []
    load = kind in ['word', 'half']
    if load:
        flags = before['flags']
        physical = op['addr'] & 0x1fffffff
        if (profile in ['plain', 'overlay'] or physical >= 0x800000
                or flags[4] or flags[5] or flags[6]):
            return absorb, [which, fudge, pending, amount], charges
        enabled = flags[9] if flags[3] == -1 else flags[3]
        if not enabled:
            return absorb, [which, fudge, pending, amount], charges
        if pending == op['rt']:
            pending = 0
    if kind in ['base', 'step'] or load:
        if absorb[which]:
            absorb[which] -= 1
        else:
            charges.append(1)
    if kind in ['deps', 'step'] or load:
        for register in range(1, 32):
            if op['mask'] & (1 << register):
                absorb[register] = 0
    if kind in ['lds', 'step'] or load:
        absorb[pending] = amount & 255
        which |= pending & 31
        fudge, pending = pending, 32
    if load:
        charges.append(7 if fudge == 32 else 5)
        absorb[which] = 0
        which, pending, amount = 0, op['rt'], 5
    return absorb, [which, fudge, pending, amount], charges


def check(matrix_path, trace_path):
    matrix = json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    metadata, rows = read_trace(matrix_path, trace_path)
    cases = {case['id']: case for case in matrix['cases']}
    profile = metadata['source']['profile']
    checked = 0
    previous = None
    for row in rows:
        if row['step'] >= 0:
            op = cases[row['case_id']]['operations'][row['step']]
            if op['op'] in ['base', 'deps', 'lds', 'step', 'word', 'half']:
                absorb, pipeline, charges = predict_pipeline(previous, op, profile)
                key = (row['case_id'], row['step'])
                require(absorb == row['absorb'], f'absorb model {key}')
                require(pipeline == row['pipeline'], f'pipeline model {key}: {pipeline} != {row["pipeline"]}')
                if profile == 'cosim':
                    actual = [e[2] for e in row['events'] if e[0] == 1]
                    flags = previous['flags']
                    expected = [] if flags[0] and not (flags[1] or flags[2]) else charges
                    require(expected == actual, f'charge model {key}: {expected} != {actual}')
                    require(row['clock'][0] - previous['clock'][0] == sum(charges),
                            f'published cycle model {key}')
                checked += 1
        previous = row
    return dict(profile=profile, transitions=checked, valid=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('matrix')
    parser.add_argument('trace')
    args = parser.parse_args()
    print(json.dumps(check(args.matrix, args.trace)))
