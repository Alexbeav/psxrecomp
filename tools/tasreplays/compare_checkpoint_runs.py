"""Compare complete checkpoint replay evidence, with no omitted returns or sections."""
import argparse
import csv
from itertools import islice, zip_longest
import json
from pathlib import Path

from compare_boot_states import compare
from compare_ram_pages import read_pages


def cpu_rows(path, first=1):
    with path.open(newline='') as stream:
        for frame, row in enumerate(csv.DictReader(stream, delimiter='\t'), first):
            if int(row['frame']) != frame:
                raise ValueError(f'CPU coverage gap at {frame}: {path}')
            yield row


def compare_rows(left, right, skip, terminal):
    count = mismatches = 0
    first = None
    for frame, (a, b) in enumerate(zip_longest(islice(left, skip, None), right), skip + 1):
        count += 1
        if a != b:
            mismatches += 1
            if first is None:
                first = frame
    return dict(rows=count, expected_rows=terminal-skip, mismatches=mismatches,
                first_mismatch=first, passed=count == terminal-skip and mismatches == 0)


def saved_states(run):
    """Requested return -> state file, from run_native's saved-states.json. A capture
    requested at a return with no represented continuation is taken at a later
    return, so the file name is the return where it was actually taken."""
    path = Path(run) / 'saved-states.json'
    if not path.exists():
        return {}
    return {item['frame']: item['state'] for item in json.loads(path.read_text()) if item.get('valid')}


def compare_runs(baseline, candidate, start, terminal, captures):
    result = {'start': start, 'terminal': terminal, 'states': {}}
    left_names, right_names = saved_states(baseline), saved_states(candidate)
    for frame in captures:
        if not start < frame <= terminal:
            continue
        left = left_names.get(frame, f'tas-state-{frame:06d}.pst')
        if int(left[len('tas-state-'):-len('.pst')]) <= start:
            continue  # satisfied by the checkpoint the candidate resumed from
        right = right_names.get(frame, f'tas-state-{frame:06d}.pst')
        if left != right:
            result['states'][str(frame)] = {'identical': False, 'differences': [
                {'section': 'CAPTURE_RETURN', 'left': left, 'right': right}]}
            continue
        result['states'][str(frame)] = compare(baseline/left, candidate/right)
    if not result['states']:
        raise ValueError('at least one later full-state comparison is required')
    result['CPU'] = compare_rows(cpu_rows(baseline/'cpu-return.tsv'),
                                 cpu_rows(candidate/'cpu-return.tsv', start+1), start, terminal)
    result['RAM'] = compare_rows(read_pages(baseline/'ram-pages.tsv'),
                                 read_pages(candidate/'ram-pages.tsv', first_frame=start+1), start, terminal)
    result['passed'] = result['CPU']['passed'] and result['RAM']['passed'] and all(
        state['identical'] for state in result['states'].values())
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--start', type=int, default=0)
    parser.add_argument('--terminal', type=int, required=True)
    parser.add_argument('--captures', type=int, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.start < args.terminal:
        parser.error('require 0 <= start < terminal')
    try:
        result = compare_runs(args.baseline, args.candidate, args.start, args.terminal, args.captures)
    except (OSError, ValueError, KeyError) as error:
        result = {'passed': False, 'error': str(error)}
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print('PASS' if result['passed'] else 'FAIL', args.output)
    raise SystemExit(0 if result['passed'] else 1)
