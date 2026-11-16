"""Corrupt persisted state, ordered events and scalar results independently."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
from validate_raster_clock_trace import validate


def check(evidence):
    matrix = json.loads((evidence / 'raster-clock-experiments-v1.json').read_text())
    matrix['cases'] = matrix['cases'][:1]
    with (evidence / 'raster-clock-v1-baseline-a.jsonl').open() as file:
        next(file)
        rows = [json.loads(next(file)) for _ in range(len(matrix['cases'][0]['operations']) + 1)]
    results = {}
    with tempfile.TemporaryDirectory(prefix='t172-raster-controls-') as folder:
        root = Path(folder); mpath = root / 'matrix.json'; mpath.write_text(json.dumps(matrix))
        metadata = dict(metadata=dict(matrix_sha256=hashlib.sha256(mpath.read_bytes()).hexdigest()))
        def save(path, items):
            path.write_text('\n'.join(json.dumps(item) for item in [metadata] + items) + '\n')
        original = root / 'original.jsonl'; save(original, rows)
        mutations = {}
        for field in ['status', 'until_rise']:
            altered = deepcopy(rows);altered[-1][field] ^= 1;mutations[field] = altered
        for field in range(18):
            altered = deepcopy(rows);altered[-1]['state'][field] ^= 1;mutations[f'state_{field}'] = altered
        for field in range(3):
            altered = deepcopy(rows);altered[-1]['events'][0][field] ^= 1;mutations[f'event_{field}'] = altered
        altered = deepcopy(rows);altered[-1]['events'].reverse();mutations['event_order'] = altered
        altered = deepcopy(rows);altered[-1]['wire_hex'] = '00' * 80;mutations['wire'] = altered
        altered = deepcopy(rows);altered[1]['return_value'] = 0;mutations['return'] = altered
        altered = deepcopy(rows);altered[-1]['state'][0] = True;mutations['type'] = altered
        mutations['missing'] = rows[:-1]
        mutations['extra'] = rows + rows[-1:]
        for name, altered in mutations.items():
            bad = root / 'bad.jsonl';save(bad, altered)
            try:
                detected = bool(validate(mpath, [original, bad])['different_fields'])
            except (ValueError, StopIteration):
                detected = True
            if not detected:
                raise AssertionError(name)
            results[name] = detected
        consumer = json.loads((evidence / 'raster-consumer-experiments-v1.json').read_text())
        consumer['cases'] = consumer['cases'][:1]
        with (evidence / 'raster-consumer-v1-baseline.jsonl').open() as file:
            next(file)
            rows = [json.loads(next(file)) for _ in range(len(consumer['cases'][0]['operations']) + 1)]
        mpath.write_text(json.dumps(consumer))
        metadata = dict(metadata=dict(matrix_sha256=hashlib.sha256(mpath.read_bytes()).hexdigest()))
        save(original, rows)
        for name in ['timer_content', 'timer_length']:
            altered = deepcopy(rows)
            wire = bytearray.fromhex(altered[-1]['timer_wire_hex'])
            if name == 'timer_content':
                wire[0] ^= 1
            else:
                wire.pop()
            altered[-1]['timer_wire_hex'] = wire.hex()
            save(bad, altered)
            try:
                detected = bool(validate(mpath, [original, bad])['different_fields'])
            except ValueError:
                detected = True
            if not detected:
                raise AssertionError(name)
            results[name] = detected
    return dict(controls=len(results), all_detected=all(results.values()), results=results)


if __name__ == '__main__':
    import sys
    print(json.dumps(check(Path(sys.argv[1])), indent=2))
