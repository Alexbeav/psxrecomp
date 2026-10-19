import json,sys
d=json.load(open(sys.argv[1]))
addrs=[int(x,16) for x in sys.argv[2:]]
funcs=d['functions']
funcs.sort(key=lambda f:f['addr'])
import bisect
starts=[f['addr'] for f in funcs]
for a in addrs:
    i=bisect.bisect_right(starts,a)-1
    f=funcs[i]
    print(hex(a), '->', f['name'], hex(f['addr']), hex(f['end']), f.get('confidence'))
