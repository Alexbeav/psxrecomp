"""Strict CPU observation validation, independent of the measured implementation."""
import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, maximum):
    return type(value) is int and 0 <= value <= maximum


def vector(value, limits):
    return (type(value) is list and len(value) == len(limits)
            and all(integer(v, limit) for v, limit in zip(value, limits)))


def read_trace(matrix_path, trace_path):
    raw = Path(matrix_path).read_bytes()
    matrix = json.loads(raw.decode('utf-8-sig'))
    require(matrix['schema'] == 't172-cpu-timing-experiment-v1', 'matrix schema')
    cases = matrix['cases']
    require(len({c['id'] for c in cases}) == len(cases), 'duplicate case')
    expected = [(c['id'], step) for c in cases
                for step in range(-1, len(c['operations']))]
    rows = [json.loads(line) for line in Path(trace_path).read_text().splitlines()]
    metadata = rows.pop(0)['metadata']
    require(metadata['schema'] == 't77-cpu-timing-observations-v1', 'trace schema')
    require(metadata['matrix_sha256'] == hashlib.sha256(raw).hexdigest(), 'matrix hash')
    require(metadata['cases'] == len(cases), 'case count')
    require(metadata['observations'] == len(expected) == len(rows), 'row count')
    u32, u64 = 2**32-1, 2**64-1
    fields = {'case_id', 'step', 'absorb', 'pipeline', 'clock', 'flags',
              'return_value', 'events'}
    for row, key in zip(rows, expected):
        require(set(row) == fields, 'row keys')
        require(type(row['step']) is int and (row['case_id'], row['step']) == key,
                f'row order {key}')
        require(vector(row['absorb'], [255]*33), f'absorb {key}')
        require(vector(row['pipeline'], [31, 255, 32, u32]), f'pipeline {key}')
        require(vector(row['clock'], [u64, u64, u32, u32, u32, u32, 1]), f'clock {key}')
        flags = row['flags']
        require(type(flags) is list and len(flags) == 10, f'flags {key}')
        require(type(flags[3]) is int and flags[3] in [-1, 0, 1], f'load flag {key}')
        require(vector(flags[:3]+flags[4:], [1, 1, 1, 1, 1, u32, 1, u32, 1]),
                f'flags range {key}')
        require(row['return_value'] is None or integer(row['return_value'], u32),
                f'return {key}')
        require(type(row['events']) is list, f'events {key}')
        for event in row['events']:
            require(vector(event, [10, u64, u64, u32, u32]) and event[0] >= 1,
                    f'event {key}')
    return metadata, rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('matrix')
    parser.add_argument('trace')
    parser.add_argument('--compare')
    args = parser.parse_args()
    metadata, rows = read_trace(args.matrix, args.trace)
    if args.compare:
        other_meta, other = read_trace(args.matrix, args.compare)
        require(other_meta['source']['profile'] == metadata['source']['profile'], 'profile')
        for a, b in zip(rows, other):
            require(a == b, f"difference at {a['case_id']} step {a['step']}")
    print(json.dumps(dict(cases=metadata['cases'], observations=len(rows),
                          profile=metadata['source']['profile'], valid=True)))
