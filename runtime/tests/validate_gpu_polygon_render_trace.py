"""Reconstruct synthetic VRAM and compare complete renderer observations."""
import argparse
from array import array
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import sys


def initial_memory(seed):
    memory = array('H', ((((seed + i * 73) & 0xffffffff) ^ (i >> 5)) & 65535
                   for i in range(1024 * 512))) if seed else array('H', [0]) * (1024 * 512)
    if sys.byteorder != 'little':
        memory.byteswap()
    return memory.tobytes()


def validate(matrix_path, paths):
    matrix = json.loads(matrix_path.read_text())
    matrix_hash = hashlib.sha256(matrix_path.read_bytes()).hexdigest()
    initial = {seed: initial_memory(seed) for seed in {case['seed'] for case in matrix['cases']}}
    digests = [hashlib.sha256() for _ in paths]
    differences, changed = Counter(), [0 for _ in paths]
    with ExitStack() as stack:
        files = [stack.enter_context(path.open()) for path in paths]
        for file in files:
            if json.loads(next(file))['metadata']['matrix_sha256'] != matrix_hash:
                raise ValueError('wrong rendering matrix')
        for case in matrix['cases']:
            rows = []
            for n, file in enumerate(files):
                row = json.loads(next(file, 'null'))
                if not isinstance(row, dict) or set(row) != {'case_id', 'result', 'extra_work', 'vram_sha256', 'changed_count', 'changed_pixels'}:
                    raise ValueError('missing or malformed rendering result')
                if row['case_id'] != case['id']:
                    raise ValueError('wrong rendering case')
                if any(type(row[k]) is not int for k in ['result', 'extra_work', 'changed_count']):
                    raise ValueError('invalid rendering integer')
                if not isinstance(row['changed_pixels'], list) or row['changed_count'] != len(row['changed_pixels']):
                    raise ValueError('wrong changed count')
                memory = bytearray(initial[case['seed']])
                previous = -1
                for pixel in row['changed_pixels']:
                    if not isinstance(pixel, list) or len(pixel) != 3 or any(type(v) is not int for v in pixel):
                        raise ValueError('invalid changed pixel')
                    x, y, value = pixel
                    if not (0 <= x < 1024 and 0 <= y < 512 and 0 <= value < 65536):
                        raise ValueError('pixel outside VRAM')
                    address = y * 1024 + x
                    if address <= previous:
                        raise ValueError('duplicate or unordered changed pixel')
                    offset = address * 2
                    data = value.to_bytes(2, 'little')
                    if memory[offset:offset + 2] == data:
                        raise ValueError('unchanged pixel reported as changed')
                    memory[offset:offset + 2] = data
                    previous = address
                if hashlib.sha256(memory).hexdigest() != row['vram_sha256']:
                    raise ValueError('reconstructed VRAM hash mismatch')
                changed[n] += row['changed_count']
                digests[n].update((json.dumps(row, sort_keys=True, separators=(',', ':')) + '\n').encode())
                rows.append(row)
            if len(rows) == 2:
                for key in rows[0]:
                    if rows[0][key] != rows[1][key]:
                        differences[key] += 1
        if any(next(file, None) is not None for file in files):
            raise ValueError('extra rendering result')
    return dict(cases=len(matrix['cases']), changed_pixels=changed,
                different_fields=dict(differences), reconstructed_vram_hashes_verified=True,
                normalized_sha256=[d.hexdigest() for d in digests])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix', type=Path)
    parser.add_argument('traces', nargs='+', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.matrix, args.traces), indent=2))
