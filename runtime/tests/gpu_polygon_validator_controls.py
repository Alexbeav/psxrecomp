"""Mutate known-good observations to check that validation rejects corruption."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile

from validate_gpu_polygon_trace import validate as spans
from validate_gpu_polygon_render_trace import validate as render


def check(evidence):
    results = {}
    with tempfile.TemporaryDirectory(prefix='t172-polygon-controls-') as folder:
        root = Path(folder)
        for label, matrix_name, trace_name, validator in [
                ('spans', 'gpu-polygon-experiments-v1.json', 'gpu-polygon-v1-baseline-a.jsonl', spans),
                ('render', 'gpu-polygon-render-experiments-v1.json', 'gpu-polygon-render-v1-baseline.jsonl', render)]:
            matrix = json.loads((evidence / matrix_name).read_text())
            with (evidence / trace_name).open() as file:
                next(file)
                good = json.loads(next(file))
            matrix['cases'] = matrix['cases'][:1]
            matrix_path = root / 'matrix.json'
            matrix_path.write_text(json.dumps(matrix))
            import hashlib
            metadata = {'metadata': {'matrix_sha256': hashlib.sha256(matrix_path.read_bytes()).hexdigest()}}
            original = root / 'original.jsonl'
            original.write_text(json.dumps(metadata) + '\n' + json.dumps(good) + '\n')
            mutations = {'missing': None, 'identity': dict(good, case_id='wrong')}
            for field in (['work', 'cost'] if label == 'spans' else ['result', 'extra_work', 'changed_count']):
                mutations[field] = dict(good, **{field: good[field] + 1})
                mutations[field + '_type'] = dict(good, **{field: True})
            array_name = 'spans' if label == 'spans' else 'changed_pixels'
            row = deepcopy(good);row[array_name].reverse();mutations['order'] = row
            for i in range(4 if label == 'spans' else 3):
                row = deepcopy(good);row[array_name][0][i] += 1;mutations[f'argument_{i}'] = row
            if label == 'render':
                mutations['hash'] = dict(good, vram_sha256='0' * 64)
            for name, row in mutations.items():
                corrupted = root / 'corrupted.jsonl'
                corrupted.write_text(json.dumps(metadata) + '\n' + (json.dumps(row) + '\n' if row else ''))
                try:
                    report = validator(matrix_path, [original, corrupted])
                    detected = bool(report['different_fields'])
                except (ValueError, StopIteration):
                    detected = True
                results[f'{label}_{name}'] = detected
                if not detected:
                    raise AssertionError(f'undetected corruption: {label}/{name}')
    return dict(controls=len(results), all_detected=all(results.values()), results=results)


if __name__ == '__main__':
    import sys
    print(json.dumps(check(Path(sys.argv[1])), indent=2))
