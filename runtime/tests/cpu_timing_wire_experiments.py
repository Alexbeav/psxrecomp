"""Public CPU wire capture and paired continuation using baseline snapshots."""
import argparse
import json
from pathlib import Path
from cpu_timing_experiments import matrix


def capture():
    result = matrix()
    result['schema'] = 't172-cpu-timing-wire-experiment-v1'
    return result


def restore(trace_path):
    original = capture()
    rows = [json.loads(line) for line in Path(trace_path).read_text().splitlines()[1:]]
    last = {r['case_id']: r for r in rows}
    future = [dict(op='step', mask=0xffffffff), dict(op='word', addr=0x200000, rt=31, mask=2),
              dict(op='step', mask=0), dict(op='half', addr=0x1ffffe, rt=1, mask=0x80000000),
              dict(op='begin'), dict(op='local_begin'), dict(op='charge', cycles=65),
              dict(op='local_end'), dict(op='end'), dict(op='flush')]
    pairs = []
    cases = []
    for case in original['cases'][::3]:
        row = last[case['id']]
        if row['clock'][6]:
            raise ValueError('Capture checkpoint has a live local pointer')
        clock = row['clock']
        flag = row['flags']
        state = dict(zip(['cycle','deadline','batch','limit','defer','local'],clock[:6]))
        state.update(zip(['device','conservative','replay','load_delay','lockstep','recording',
                          'dma_wait','watch','fallback','lazy_enable'],flag))
        if state['load_delay'] == -1:
            state['load_delay'] = 2
        restored = [dict(op='restore_cpu', cpu_wire_hex=row['cpu_wire_hex'], len=580)]
        restored += [dict(op='set',field=k,value=v) for k,v in state.items()]
        left, right = case['id']+'_continued', case['id']+'_restored'
        cases += [dict(id=left,operations=case['operations']+future),
                  dict(id=right,operations=restored+future)]
        pairs.append(dict(left=left,right=right,left_start=len(case['operations']),
                          right_start=len(restored),length=len(future),
                          source_case=case['id'],source_step=row['step'],
                          cpu_wire_hex=row['cpu_wire_hex']))
    return dict(schema='t172-cpu-timing-wire-experiment-v1',cases=cases,pairs=pairs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--restore-from')
    args = parser.parse_args()
    print(json.dumps(restore(args.restore_from) if args.restore_from else capture(),indent=2))
