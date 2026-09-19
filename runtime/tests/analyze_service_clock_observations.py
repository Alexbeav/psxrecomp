"""Independent service scheduling hypothesis using qualified raster behavior."""
import json
from pathlib import Path
import struct
from collections import Counter
from analyze_raster_clock_observations import advance
from validate_raster_clock_trace import FIELDS


def model(previous, operation, callbacks=False):
    state = previous['state'][:]
    raster = dict(zip(FIELDS, struct.unpack('<QQ16I', bytes.fromhex(previous['raster_wire_hex']))))
    events, snapshots = [], []
    def wire():
        return struct.pack('<QQ16I', *(raster[k] for k in FIELDS)).hex()
    def emit(kind):
        events.append([state[0], kind])
        snapshots.append(dict(state=state[:], raster_wire_hex=wire()))
    def phase():
        return (raster['remaining'] * 65536 - raster['fraction'] + 103895) // 103896
    def raster_step(elapsed):
        nonlocal raster
        old = raster
        raster, _ = advance(raster, elapsed)
        if old['scanline'] != raster['scanline'] and raster['scanline'] == 0:
            state[4] = 1
        frame_edge = (not old['blank'] and raster['blank'] and raster['scanline'] >= 232) or (old['scanline'] != raster['scanline'] and raster['scanline'] == 256)
        if state[4] and not state[5] and frame_edge:
            state[5] = 1
            state[3] = raster['cycle']
    result = 1
    op = operation['op']
    if op == 'gp1':
        word = operation['word'];command = (word >> 24) & 63
        if command == 0:
            raster.update(mode=0, start=16, end=256, y_start=0)
        elif command == 5:
            raster['y_start'] = (word >> 10) & 511
        elif command == 7:
            raster.update(start=word & 1023, end=(word >> 10) & 1023)
        elif command == 8:
            if word & 8:
                result = 0
            else:
                raster['mode'] = word & 255
    elif op == 'raster':
        raster_step(operation['elapsed']);result = None
    else:
        now = operation['now']
        if now < state[0]:
            result = 0
        else:
            while min(state[1:3]) <= now:
                deadline = min(state[1:3])
                raster_step(deadline - state[0]);state[0] = deadline
                if state[1] <= deadline:
                    emit(1);state[1] = deadline + min(128, phase())
                if state[2] <= deadline:
                    emit(2);state[2] = deadline + 128
            raster_step(now - state[0]);state[0] = now
            if op == 'dma_write':
                emit(3)
            if op == 'frame_end' or (op == 'cpu_boundary' and state[5]):
                emit(4);state[1] = now + min(128, phase());emit(2)
                state[4] = state[5] = 0
                state[6] += 1
    row = dict(state=state, raster_wire_hex=wire(), next=min(state[1:3]), until_phase=phase(),
               events=events, return_value=result)
    if callbacks:
        row['callback_states'] = snapshots
    return row


def compare(matrix_path, trace_path):
    matrix = json.loads(matrix_path.read_text())
    differences, examples = Counter(), []
    with trace_path.open() as file:
        metadata = json.loads(next(file))['metadata']
        for case in matrix['cases']:
            old = json.loads(next(file))
            for step, operation in enumerate(case['operations']):
                row = json.loads(next(file));p = model(old, operation, metadata.get('callback_state', False))
                for key in p:
                    if p[key] != row[key]:
                        differences[key] += 1
                        if len(examples) < 10:
                            examples.append(dict(case=case['id'], step=step, op=operation, field=key,
                                                 predicted=p[key], actual=row[key]))
                old = row
    return dict(differences=dict(differences), examples=examples)


if __name__ == '__main__':
    import sys
    print(json.dumps(compare(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
