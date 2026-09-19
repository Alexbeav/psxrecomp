"""Strict ordered service-clock observation comparison."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import struct


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    signature = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    digests = [hashlib.sha256() for _ in paths]
    differences, examples = Counter(), []
    events = [0 for _ in paths]
    count = 0
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        callback_flags = []
        wire_flags = []
        for file in files:
            metadata = json.loads(next(file))['metadata']
            callback_flags.append(bool(metadata.get('callback_state', False)))
            wire_flags.append(bool(metadata.get('actual_public_wire', False)))
            if metadata['matrix_sha256'] != signature:
                raise ValueError('wrong service matrix')
        for case in matrix['cases']:
            for step in range(-1, len(case['operations'])):
                rows = []
                for i, file in enumerate(files):
                    row = json.loads(next(file, 'null'))
                    keys = {'case_id', 'step', 'state', 'next', 'until_phase', 'return_value', 'raster_wire_hex', 'events'}
                    if callback_flags[i]:
                        keys.add('callback_states')
                    if wire_flags[i]:
                        keys.update(['service_wire_hex', 'full_raster_wire_hex'])
                    if not isinstance(row, dict) or set(row) != keys:
                        raise ValueError('missing or malformed service result')
                    if row['case_id'] != case['id'] or type(row['step']) is not int or row['step'] != step:
                        raise ValueError('wrong service operation identity')
                    if not isinstance(row['state'], list) or len(row['state']) != 7:
                        raise ValueError('incomplete service state')
                    for j, value in enumerate(row['state']):
                        if type(value) is not int or not 0 <= value < 2 ** (64 if j < 4 else 32):
                            raise ValueError('invalid service state integer')
                    for key, bits in [('next', 64), ('until_phase', 32)]:
                        if type(row[key]) is not int or not 0 <= row[key] < 2**bits:
                            raise ValueError('invalid deadline')
                    if row['return_value'] is not None and type(row['return_value']) is not int:
                        raise ValueError('invalid return')
                    wire = bytes.fromhex(row['raster_wire_hex'])
                    if len(wire) != 80:
                        raise ValueError('invalid raster wire length')
                    struct.unpack('<QQ16I', wire)
                    if wire_flags[i]:
                        if len(bytes.fromhex(row['service_wire_hex'])) != 384 or len(bytes.fromhex(row['full_raster_wire_hex'])) != 160:
                            raise ValueError('incomplete actual service/raster wire')
                    if not isinstance(row['events'], list):
                        raise ValueError('invalid event list')
                    for event in row['events']:
                        if not isinstance(event, list) or len(event) != 2 or any(type(v) is not int for v in event):
                            raise ValueError('invalid service event')
                        if not 0 <= event[0] < 2**64 or event[1] not in [1, 2, 3, 4]:
                            raise ValueError('invalid service event value')
                    if callback_flags[i]:
                        if not isinstance(row['callback_states'], list) or len(row['callback_states']) != len(row['events']):
                            raise ValueError('missing callback state')
                        for snapshot in row['callback_states']:
                            if set(snapshot) != {'state', 'raster_wire_hex'} or len(snapshot['state']) != 7 or len(bytes.fromhex(snapshot['raster_wire_hex'])) != 80:
                                raise ValueError('invalid callback snapshot')
                            for j, value in enumerate(snapshot['state']):
                                if type(value) is not int or not 0 <= value < 2 ** (64 if j < 4 else 32):
                                    raise ValueError('invalid callback state integer')
                    events[i] += len(row['events'])
                    digests[i].update((json.dumps(row, sort_keys=True, separators=(',', ':')) + '\n').encode())
                    rows.append(row)
                if len(rows) == 2:
                    for field in ['state', 'next', 'until_phase', 'return_value', 'raster_wire_hex', 'events']:
                        if rows[0][field] != rows[1][field]:
                            differences[field] += 1
                            if len(examples) < 10:
                                examples.append(dict(case=case['id'], step=step, field=field,
                                                     first=rows[0][field], second=rows[1][field]))
                    if all(callback_flags) and rows[0]['callback_states'] != rows[1]['callback_states']:
                        differences['callback_states'] += 1
                    if all(wire_flags):
                        for field in ['service_wire_hex', 'full_raster_wire_hex']:
                            if rows[0][field] != rows[1][field]:
                                differences[field] += 1
                count += 1
        if any(next(file, None) is not None for file in files):
            raise ValueError('extra service records')
    return dict(cases=len(matrix['cases']), rows=count, events=events, different_fields=dict(differences),
                normalized_sha256=[d.hexdigest() for d in digests], examples=examples)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix', type=Path)
    parser.add_argument('traces', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
