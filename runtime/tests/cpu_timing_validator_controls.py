"""Deliberately corrupt observations to test the independent validator."""
import argparse
import copy
import hashlib
import json
import tempfile
from pathlib import Path
from validate_cpu_timing_trace import read_trace, require


def controls(matrix_path, trace_path, scratch):
    matrix = json.loads(Path(matrix_path).read_text(encoding='utf-8-sig'))
    metadata, rows = read_trace(matrix_path, trace_path)
    case = matrix['cases'][0]
    matrix['cases'] = [case]
    raw = json.dumps(matrix).encode()
    meta = copy.deepcopy(metadata)
    meta.update(cases=1, observations=len(case['operations'])+1,
                matrix_sha256=hashlib.sha256(raw).hexdigest())
    selected = rows[:meta['observations']]
    baseline = [dict(metadata=meta)] + selected
    changes = []
    def add(name, mutate):
        changed = copy.deepcopy(baseline)
        mutate(changed)
        changes.append((name,changed))
    add('missing_row',lambda x:x.pop())
    add('extra_row',lambda x:x.append(x[-1]))
    add('wrong_order',lambda x:x.reverse())
    add('matrix_hash',lambda x:x[0]['metadata'].update(matrix_sha256='0'*64))
    add('row_count',lambda x:x[0]['metadata'].update(observations=1))
    add('case_count',lambda x:x[0]['metadata'].update(cases=2))
    add('case_id',lambda x:x[-1].update(case_id='wrong'))
    add('step_boolean',lambda x:x[-1].update(step=True))
    add('extra_field',lambda x:x[-1].update(unexpected=0))
    for field in ['absorb','pipeline','clock','flags']:
        add(field+'_length',lambda x,f=field:x[-1][f].append(0))
        add(field+'_value',lambda x,f=field:x[-1][f].__setitem__(0,x[-1][f][0]^1))
        add(field+'_boolean',lambda x,f=field:x[-1][f].__setitem__(0,False))
    add('return_value',lambda x:x[-1].update(return_value=1))
    add('return_type',lambda x:x[-1].update(return_value=True))
    add('event_value',lambda x:x[-1].update(events=[[1,0,1,0,0]]))
    add('event_shape',lambda x:x[-1].update(events=[[1,0,1]]))
    add('wire_length',lambda x:x[-1].update(cpu_wire_hex=x[-1]['cpu_wire_hex'][:-2]))
    add('wire_value',lambda x:x[-1].update(cpu_wire_hex='ff'+x[-1]['cpu_wire_hex'][2:]))
    if selected[-1].get('event_cpu_states'):
        add('event_cpu_missing',lambda x:x[-1].pop('event_cpu_states'))
        add('event_cpu_count',lambda x:x[-1]['event_cpu_states'].pop())
        add('event_cpu_type',lambda x:x[-1]['event_cpu_states'].__setitem__(0,False))
        add('event_cpu_length',lambda x:x[-1]['event_cpu_states'].__setitem__(0,'00'))
        add('event_cpu_value',lambda x:x[-1]['event_cpu_states'].__setitem__(0,
            'ff'+x[-1]['event_cpu_states'][0][2:]))
    rejected = []
    Path(scratch).mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cpu-validator-',dir=scratch) as folder:
        folder=Path(folder);m=folder/'matrix.json';t=folder/'trace.jsonl'
        m.write_bytes(raw)
        for name, changed in changes:
            t.write_text('\n'.join(json.dumps(r) for r in changed)+'\n')
            try:
                _,actual=read_trace(m,t)
                require(actual==selected,'different observation')
            except (ValueError,KeyError,TypeError):
                rejected.append(name)
            else:
                raise ValueError(f'control escaped: {name}')
    return dict(controls=len(changes),rejected=rejected,valid=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('matrix');parser.add_argument('trace');parser.add_argument('scratch')
    args=parser.parse_args()
    print(json.dumps(controls(args.matrix,args.trace,args.scratch),indent=2))
