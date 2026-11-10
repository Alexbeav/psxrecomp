"""Independent measured raster hypothesis; retained disagreements guide probes."""
from collections import Counter
import json
from pathlib import Path
from validate_raster_clock_trace import FIELDS


def advance(state, cycles):
    s = dict(state)
    ticks, fraction = divmod(s['fraction'] + cycles * 103896, 65536)
    consumed = 0
    events = []
    while ticks >= s['remaining']:
        consumed += s['remaining']
        ticks -= s['remaining']
        time = state['cycle'] + (consumed * 65536 - state['fraction'] + 103895) // 103896
        if not s['phase']:
            s['phase'] = 1
            s['remaining'] = 200
            events.append([time, 1, s['blank']])
        else:
            s['phase'] = 0
            s['alternate'] ^= 1
            s['remaining'] = 3213 - s['alternate']
            s['scanline'] += 1
            if s['scanline'] == s['lines'] - 1:
                s['field'] = (s['field'] ^ 1) if s['mode'] & 32 else 0
            if s['scanline'] >= s['lines']:
                s['scanline'] = 0
                s['lines'] = 263 - s['field']
            old_blank = s['blank']
            if s['scanline'] == s['end']:
                s['blank'] = 1
                if not old_blank:
                    s['y_offset'] = 0
                    s['readout_field'] = (s['field'] ^ 1) if s['mode'] & 36 == 36 else 0
            if s['scanline'] == s['start']:
                s['blank'] = 0
            if s['blank'] != old_blank:
                events.append([time, 2, s['blank']])
                if s['blank']:
                    s['last_rise'] = time
                    s['rises'] = (s['rises'] + 1) & 0xffffffff
            if s['blank']:
                s['readout_y'] = s['y_start']
            else:
                scale = 2 if s['mode'] & 36 == 36 else 1
                s['readout_y'] = (s['y_start'] + s['y_offset'] * scale + (s['readout_field'] if scale == 2 else 0)) & 511
                s['y_offset'] = (s['y_offset'] + 1) & 511
    s['remaining'] -= ticks
    s['cycle'] += cycles
    s['fraction'] = fraction
    return s, events


def compare(matrix_path, trace_path):
    matrix = json.loads(matrix_path.read_text())
    differences, examples = Counter(), []
    with trace_path.open() as file:
        next(file)
        for case in matrix['cases']:
            old = json.loads(next(file))
            for step, operation in enumerate(case['operations']):
                row = json.loads(next(file))
                state = dict(zip(FIELDS, old['state']))
                predicted, events = dict(state), []
                result = None
                if operation['op'] == 'advance':
                    predicted, events = advance(state, operation['cycles'])
                    if not operation['observed']:
                        events = []
                elif operation['op'] == 'gp1':
                    word = operation['word'];command = (word >> 24) & 63
                    result = 1
                    if command == 0:
                        predicted.update(mode=0, start=16, end=256, y_start=0)
                    elif command == 5:
                        predicted['y_start'] = (word >> 10) & 511
                    elif command == 7:
                        predicted.update(start=word & 1023, end=(word >> 10) & 1023)
                    elif command == 8:
                        if word & 8:
                            result = 0
                        else:
                            predicted['mode'] = word & 255
                else:
                    raise ValueError('unsupported hypothesis operation')
                actual = dict(zip(FIELDS, row['state']))
                checks = {key: (predicted[key], actual[key]) for key in FIELDS}
                checks.update(events=(events, row['events']), return_value=(result, row['return_value']))
                for key, (a, b) in checks.items():
                    if a != b:
                        differences[key] += 1
                        if len(examples) < 30:
                            examples.append(dict(case=case['id'], step=step, op=operation, field=key,
                                                 expected=b, predicted=a, before=state))
                old = row
    return dict(differences=dict(differences), examples=examples)


if __name__ == '__main__':
    import sys
    print(json.dumps(compare(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
