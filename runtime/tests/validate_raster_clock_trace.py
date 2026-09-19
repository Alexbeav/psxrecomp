"""Strict raster observations and public wire/state correspondence checks."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import struct


FIELDS = ['cycle', 'last_rise', 'fraction', 'remaining', 'phase', 'alternate',
          'scanline', 'lines', 'field', 'mode', 'start', 'end', 'blank', 'rises',
          'y_start', 'y_offset', 'readout_y', 'readout_field']


def check_row(row, consumer=False):
    keys = {'case_id', 'step', 'return_value', 'state', 'status', 'until_rise', 'wire_hex', 'events'}
    if consumer:
        keys.add('timer_wire_hex')
    if not isinstance(row, dict) or set(row) != keys:
        raise ValueError('malformed raster result')
    state = row['state']
    if not isinstance(state, list) or len(state) != 18:
        raise ValueError('incomplete raster state')
    for i, value in enumerate(state):
        if type(value) is not int or not 0 <= value < 2 ** (64 if i < 2 else 32):
            raise ValueError('invalid persisted state integer')
    for field in ['status', 'until_rise']:
        if type(row[field]) is not int or not 0 <= row[field] < 2**32:
            raise ValueError('invalid raster result integer')
    if row['return_value'] is not None and type(row['return_value']) is not int:
        raise ValueError('invalid return value')
    wire = bytes.fromhex(row['wire_hex'])
    if len(wire) != 80 or list(struct.unpack('<QQ16I', wire)) != state:
        raise ValueError('public wire does not represent complete observed state')
    if consumer and len(bytes.fromhex(row['timer_wire_hex'])) != 60:
        raise ValueError('incomplete actual timer wire')
    if not isinstance(row['events'], list):
        raise ValueError('invalid event list')
    for event in row['events']:
        if not isinstance(event, list) or len(event) != 3 or any(type(v) is not int for v in event):
            raise ValueError('invalid event tuple')
        if not 0 <= event[0] < 2**64 or event[1] not in [1, 2] or event[2] not in [0, 1]:
            raise ValueError('invalid event value')


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    consumer = matrix['schema'] == 't172-raster-consumer-experiment-v1'
    matrix_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    digests = [hashlib.sha256() for _ in paths]
    differences, examples = Counter(), []
    rows = 0
    events = [0 for _ in paths]
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            if json.loads(next(file))['metadata']['matrix_sha256'] != matrix_hash:
                raise ValueError('wrong raster matrix')
        for case in matrix['cases']:
            for step in range(-1, len(case['operations'])):
                observed = []
                for i, file in enumerate(files):
                    row = json.loads(next(file, 'null'))
                    check_row(row, consumer)
                    if row['case_id'] != case['id'] or type(row['step']) is not int or row['step'] != step:
                        raise ValueError('missing or unordered operation')
                    events[i] += len(row['events'])
                    digests[i].update((json.dumps(row, sort_keys=True, separators=(',', ':')) + '\n').encode())
                    observed.append(row)
                if len(observed) == 2:
                    for field in ['return_value', 'state', 'status', 'until_rise', 'wire_hex', 'events'] + (['timer_wire_hex'] if consumer else []):
                        if observed[0][field] != observed[1][field]:
                            differences[field] += 1
                            if len(examples) < 20:
                                examples.append(dict(case=case['id'], step=step, field=field,
                                                     first=observed[0][field], second=observed[1][field]))
                rows += 1
        if any(next(file, None) is not None for file in files):
            raise ValueError('extra raster results')
    return dict(cases=len(matrix['cases']), rows=rows, events=events,
                different_fields=dict(differences), normalized_sha256=[d.hexdigest() for d in digests], examples=examples)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix', type=Path)
    parser.add_argument('traces', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
