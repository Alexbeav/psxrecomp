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
    memory = matrix['schema'] == 't172-cpu-timing-memory-experiment-v1'
    wire = memory or matrix['schema'] == 't172-cpu-timing-wire-experiment-v1'
    require(wire or matrix['schema'] == 't172-cpu-timing-experiment-v1', 'matrix schema')
    cases = matrix['cases']
    require(len({c['id'] for c in cases}) == len(cases), 'duplicate case')
    expected = [(c['id'], step) for c in cases
                for step in range(-1, len(c['operations']))]
    rows = [json.loads(line) for line in Path(trace_path).read_text().splitlines()]
    metadata = rows.pop(0)['metadata']
    require(metadata['schema'] in ['t77-cpu-timing-observations-v1',
                                    't77-cpu-timing-wire-observations-v1',
                                    't77-cpu-timing-memory-observations-v1'], 'trace schema')
    require(metadata['matrix_sha256'] == hashlib.sha256(raw).hexdigest(), 'matrix hash')
    require(metadata['cases'] == len(cases), 'case count')
    require(metadata['observations'] == len(expected) == len(rows), 'row count')
    u32, u64 = 2**32-1, 2**64-1
    fields = {'case_id', 'step', 'absorb', 'pipeline', 'clock', 'flags',
              'return_value', 'events'}
    for row, key in zip(rows, expected):
        expected_fields = fields | ({'cpu_wire_hex'} if wire else set())
        if memory:
            expected_fields |= {'memory_flags'}
            require(vector(row['memory_flags'], [1,1,1]), f'memory flags {key}')
            if metadata['source'].get('memory_query_address',False):
                expected_fields |= {'memory_hblank_address'}
                require(vector(row['memory_hblank_address'], [u32]), f'memory query address {key}')
            if metadata['source'].get('memory_production','off') != 'off':
                expected_fields |= {'memory_extra'}
                require(vector(row['memory_extra'], [u64,100]), f'memory extra {key}')
        if 'scope_states' in row:
            expected_fields |= {'scope_states'}
        if metadata.get('event_cpu_states') or 'event_cpu_states' in row:
            expected_fields |= {'event_cpu_states'}
        require(set(row) == expected_fields, 'row keys')
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
            require(vector(event, [12 if memory else 10, u64, u64, u32, u32]) and event[0] >= 1,
                    f'event {key}')
        if wire:
            encoded = row['cpu_wire_hex']
            require(type(encoded) is str and len(encoded) == 1160
                    and all(c in '0123456789abcdef' for c in encoded), f'CPU wire {key}')
        if 'event_cpu_states' in row:
            states=row['event_cpu_states']
            require(type(states) is list and len(states)==len(row['events']), f'event states {key}')
            for encoded in states:
                require(type(encoded) is str and len(encoded)==1160
                        and all(c in '0123456789abcdef' for c in encoded), f'event CPU wire {key}')
        if 'scope_states' in row:
            require(type(row['scope_states']) is list, f'scope states {key}')
            for state in row['scope_states']:
                require(vector(state, [8, u64, u32, u32, u32, u32, 4, u64]),
                        f'scope state {key}')
    return metadata, rows


def check_pairs(matrix_path, trace_path, capture_path):
    matrix = json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    _, rows = read_trace(matrix_path, trace_path)
    indexed = {(r['case_id'], r['step']): r for r in rows}
    captures = [json.loads(line) for line in Path(capture_path).read_text().splitlines()[1:]]
    source = {(r['case_id'], r['step']): r['cpu_wire_hex'] for r in captures}
    for pair in matrix['pairs']:
        require(source[(pair['source_case'], pair['source_step'])] == pair['cpu_wire_hex'],
                'snapshot absent from baseline capture')
        restored = indexed[(pair['right'], 0)]
        require(restored['return_value'] == 1, 'snapshot rejected')
        require(restored['cpu_wire_hex'] == pair['cpu_wire_hex'], 'snapshot changed on restore')
        for offset in range(pair['length']):
            a = indexed[(pair['left'], pair['left_start']+offset)]
            b = indexed[(pair['right'], pair['right_start']+offset)]
            require({k:v for k,v in a.items() if k not in ['case_id','step']}
                    == {k:v for k,v in b.items() if k not in ['case_id','step']},
                    f"paired continuation {pair['left']} offset {offset}")
    return len(matrix['pairs'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('matrix')
    parser.add_argument('trace')
    parser.add_argument('--compare')
    parser.add_argument('--capture')
    args = parser.parse_args()
    metadata, rows = read_trace(args.matrix, args.trace)
    if args.compare:
        other_meta, other = read_trace(args.matrix, args.compare)
        require(other_meta['source']['profile'] == metadata['source']['profile'], 'profile')
        for a, b in zip(rows, other):
            require(a == b, f"difference at {a['case_id']} step {a['step']}")
    if args.capture:
        print(json.dumps(dict(paired_restores=check_pairs(args.matrix,args.trace,args.capture))))
    print(json.dumps(dict(cases=metadata['cases'], observations=len(rows),
                          profile=metadata['source']['profile'], valid=True)))
