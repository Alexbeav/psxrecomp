"""Small synthetic trace corruptions for the independently authored validator."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
from validate_service_clock_trace import validate


def check(evidence):
    results = {}
    with tempfile.TemporaryDirectory(prefix='t172-service-controls-') as folder:
        root = Path(folder)
        for label, filename in [('callback', 'service-clock-v2-baseline.jsonl'), ('wire', 'service-clock-wire-v1-baseline.jsonl')]:
            with (evidence / filename).open() as file:
                metadata = json.loads(next(file))
                for line in file:
                    row = json.loads(line)
                    if row['step'] == -1:
                        initial = row
                    if len(row['events']) >= 2:
                        break
            row['step'] = 0
            matrix = dict(schema='control', cases=[dict(id=row['case_id'], operations=[dict(op='control')])])
            mpath = root / 'matrix.json';mpath.write_text(json.dumps(matrix))
            metadata['metadata']['matrix_sha256'] = hashlib.sha256(mpath.read_bytes()).hexdigest()
            def save(path, rows):
                path.write_text('\n'.join(json.dumps(x) for x in [metadata] + rows) + '\n')
            original = root / 'original.jsonl';save(original, [initial, row])
            mutations = {}
            for field in ['next', 'until_phase', 'return_value']:
                altered = deepcopy(row);altered[field] ^= 1;mutations[field] = altered
            for i in range(7):
                altered = deepcopy(row);altered['state'][i] ^= 1;mutations[f'state_{i}'] = altered
            for i in range(2):
                altered = deepcopy(row);altered['events'][0][i] ^= 1;mutations[f'event_{i}'] = altered
            altered = deepcopy(row);altered['events'].reverse();mutations['event_order'] = altered
            for field in ['raster_wire_hex'] + (['service_wire_hex', 'full_raster_wire_hex'] if label == 'wire' else []):
                altered = deepcopy(row);data = bytearray.fromhex(altered[field]);data[0] ^= 1;altered[field] = data.hex();mutations[field] = altered
                altered = deepcopy(row);altered[field] = altered[field][:-2];mutations[field + '_length'] = altered
            if label == 'callback':
                altered = deepcopy(row);altered['callback_states'][0]['state'][0] ^= 1;mutations['callback_state'] = altered
                altered = deepcopy(row);altered['callback_states'][0]['raster_wire_hex'] = '00' * 80;mutations['callback_wire'] = altered
                altered = deepcopy(row);altered['callback_states'].pop();mutations['callback_missing'] = altered
            altered = deepcopy(row);altered['state'][0] = True;mutations['integer_type'] = altered
            for name, altered in mutations.items():
                bad = root / 'bad.jsonl';save(bad, [initial, altered])
                try:
                    detected = bool(validate(mpath, [original, bad])['different_fields'])
                except ValueError:
                    detected = True
                if not detected:
                    raise AssertionError((label, name))
                results[f'{label}_{name}'] = detected
    return dict(controls=len(results), all_detected=all(results.values()), results=results)


if __name__ == '__main__':
    import sys
    print(json.dumps(check(Path(sys.argv[1])), indent=2))
