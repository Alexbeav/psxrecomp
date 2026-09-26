"""Tier 1 prefix smoke: every route, cold, to N returns (default 5000), on one candidate.

  prefix_smoke.py --source WORKTREE --template PLAN.json --output NEW_DIR [--returns 5000] [--parallel 3] [--dry-run]

- Ports the reviewed plan's setup/run arguments (paths only) to NEW_DIR and WORKTREE; the worktree must be clean.
- Setups (native builds) run up to --parallel at once; replays run with the adapters' own --returns N
  (diagnostic prefix: the first N+1 original records; rows 1..N equal a full run's) and --stop-on-divergence,
  with save-state options removed. The adapter's streaming oracle comparison still decides its own verdict.
- Then cpu-return.tsv rows 1..N are checked against each route's qualified run by tier1_classify.py: exact row shape,
  sequential return ids, full coverage of 1..N, and every differing (return, column) reported as a bound exception
  (source, route, return, pc, column, both values, evidence path). Nothing is classed as a pass by rule; a route
  passes only if its exception set equals the recorded baseline (--baseline, a JSON map route -> exception keys; the
  pre-existing exceptions of the qualified pre-T172 base). Tekken 3's qualified run matched the oracle only through 108,
  so its oracle receipt is used instead (no cpu-return check).
Diagnostic only: never a qualification (Tier 3 is the full cold rerun).
"""
import argparse, json, subprocess, sys, time
from pathlib import Path

NATIVE = Path('D:/psxrecomp/validation/tas/native')
QUALIFIED = {'biohazard': 'requal-biohazard-route-03', 'megamanx5': 'requal-megamanx5-route-03',
             'megamanx4': 'requal-megamanx4-route-03', 'redc': 'requal-redc-route-03',
             'abesoddysee': 't97-abesoddysee-route-05', 'pepsiman': 'requal-pepsiman-route-04', 'tekken3': None}

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True); p.add_argument('--template', type=Path, required=True)
p.add_argument('--output', type=Path, required=True); p.add_argument('--returns', type=int, default=5000)
p.add_argument('--parallel', type=int, default=3); p.add_argument('--dry-run', action='store_true')
p.add_argument('--only', help='comma-separated titles to run, in this order (default: all, template order)')
p.add_argument('--returns-map', help='per-title returns, e.g. biohazard=10200,abesoddysee=16400 (overrides --returns)')
p.add_argument('--baseline', type=Path, help='recorded exception baseline (JSON: route -> list of exception keys); none = any exception fails')
a = p.parse_args()
git = lambda *x: subprocess.check_output(['git', *x], cwd=a.source, text=True).strip()
head = git('rev-parse', 'HEAD'); assert not git('status', '--porcelain'), 'worktree must be clean'
tpl = json.loads(a.template.read_text()); old_src, old_root = tpl['source'], str(Path(tpl['jobs'][0]['project']).parent)
out = a.output.resolve()

def remap(v):
    if isinstance(v, str):
        for b, n in ((old_root, str(out)), (old_src, str(a.source.resolve()))):
            v = v.replace(b, n).replace(b.replace('\\', '/'), n.replace('\\', '/'))
        return v
    if isinstance(v, list): return [remap(x) for x in v]
    if isinstance(v, dict): return {k: remap(x) for k, x in v.items()}
    return v

rmap = dict((k, int(v)) for k, v in (x.split('=') for x in a.returns_map.split(','))) if a.returns_map else {}
only = set(a.only.split(',')) if a.only else None
jobs = []
for j in tpl['jobs']:
    if only and j['title'] not in only: continue
    j = remap(j); run, i = [], 0
    while i < len(j['run_args']):
        x = j['run_args'][i]
        if x == '--save-state-every': i += 2; continue
        if x == '--save-state-at':
            i += 1
            while i < len(j['run_args']) and not j['run_args'][i].startswith('--'): i += 1
            continue
        run.append(x); i += 1
    if '--stop-on-divergence' not in run: run.append('--stop-on-divergence')
    j['returns'] = rmap.get(j['title'], a.returns)
    j['run_args'] = run + ['--returns', str(j['returns'])]
    jobs.append(j)
if a.only: jobs.sort(key=lambda j: a.only.split(',').index(j['title']))   # --only order is the run order
plan =dict(tier=1, source_head=head, returns=a.returns, parallel=a.parallel, jobs=jobs)
if a.dry_run:
    print(json.dumps(plan, indent=1)); sys.exit(0)
out.mkdir(exist_ok=False); (out / 'plan.json').write_text(json.dumps(plan, indent=2))
status = {'source_head': head, 'jobs': {}}
def save(): (out / 'status.json').write_text(json.dumps(status, indent=2))

def pool(items, launch, label):
    active, pending = [], list(items)
    while pending or active:
        while pending and len(active) < a.parallel:
            j = pending.pop(0); active.append((j, launch(j))); status['jobs'].setdefault(j['title'], {})[label] = 'running'; save()
        for j, pr in active[:]:
            if pr.poll() is not None:
                active.remove((j, pr)); status['jobs'][j['title']][label] = pr.returncode; save()
        time.sleep(5)

t0 = time.time()
pool(jobs, lambda j: subprocess.Popen([sys.executable, '-u', j['adapter'], *j['setup_args']], cwd=a.source,
     stdout=open(out / f"{j['title']}-setup.log", 'x'), stderr=subprocess.STDOUT), 'setup')
status['setup_seconds'] = round(time.time() - t0); save()
ready = [j for j in jobs if status['jobs'][j['title']]['setup'] == 0]
t1 = time.time()
pool(ready, lambda j: subprocess.Popen([sys.executable, '-u', j['adapter'], *j['run_args']], cwd=a.source,
     stdout=open(out / f"{j['title']}-run.log", 'x'), stderr=subprocess.STDOUT), 'run')
status['replay_seconds'] = round(time.time() - t1)

for j in ready:
    t = j['title']; res = status['jobs'][t]; o = Path(j['output'])
    rec = o / j['receipt']
    if rec.exists():
        r = json.loads(rec.read_text()); res['oracle_status'] = r.get('status'); res['oracle_first_divergence'] = r.get('first_divergence')
        res['compared_returns'] = (r.get('streaming') or {}).get('compared_returns')
    q = QUALIFIED[t]; cr = o / 'cpu-return.tsv'
    if q is None: res['cpu_return_check'] = 'n/a (oracle receipt is the judge)'; continue
    if not cr.exists(): res['cpu_return_check'] = 'FAIL: no cpu-return.tsv'; continue
    sys.path.insert(0, str(Path(__file__).resolve().parent)); import tier1_classify
    base = json.loads(a.baseline.read_text()).get(t, []) if a.baseline else None
    r = tier1_classify.classify(t, NATIVE / q / 'cpu-return.tsv', cr, j['returns'], source=head[:9], baseline=base)
    (out / f'{t}-tier1.json').write_text(json.dumps(r, indent=1) + '\n')
    res['cpu_return_check'] = r['verdict']; res['cpu_rows_compared'] = j['returns']; res['exceptions'] = len(r['exceptions'])
    res['problems'] = r['problems']; res['new_vs_baseline'] = r['new_vs_baseline']; res['missing_vs_baseline'] = r['missing_vs_baseline']
status['verdict'] = 'PASS' if all(v.get('oracle_status') == 'prefix_pass' and v.get('cpu_return_check') in ('PASS', 'n/a (oracle receipt is the judge)')
                                  for v in status['jobs'].values()) else 'FAIL'
save(); print(json.dumps(status, indent=1))
