"""Tier 1 cpu-return classifier (replaces prefix_smoke.py's old "benign transient" rule, per the Codex audit / reviewer ruling).

  tier1_classify.py --route R --qualified Q.tsv --candidate C.tsv --returns N [--baseline B.json] [--source S]
                    [--exceptions-out X.tsv] [--json-out J.json]

Checks, all hard failures:
- row shape: both files have exactly the expected header (frame pc cycle sr cause epc r0..r31) and every row has that many
  columns;
- sequential return ids: row k (1-based after the header) has frame == k, in both files, with no gaps or repeats;
- coverage: both files cover returns 1..N (N = the requested prefix); fewer rows is a coverage failure, never a pass.
Differences are never classified as passes. Every differing (return, column) is reported as a bound exception:
  (source, route, return, pc_qualified, pc_candidate, column, qualified value, candidate value, evidence path).
Verdict: PASS only if the checks hold and the exception set equals the recorded baseline exactly (the baseline lists
pre-existing exceptions, e.g. those the qualified pre-T172 base itself shows on replay). With no baseline, any exception fails.
Exception identity for the baseline comparison: (route, return, pc_qualified, column, qualified value, candidate value).
Exit code 0 = PASS, 1 = FAIL.
"""
import argparse, json, sys
from pathlib import Path

COLUMNS = ['frame', 'pc', 'cycle', 'sr', 'cause', 'epc'] + [f'r{i}' for i in range(32)]

def load(path, n):
    lines = Path(path).read_text().splitlines()
    problems = []
    if not lines or lines[0].split('\t') != COLUMNS: problems.append(f'{path}: header is not exactly {" ".join(COLUMNS)}')
    rows = []
    for k, line in enumerate(lines[1:], 1):
        f = line.split('\t')
        if len(f) != len(COLUMNS): problems.append(f'{path}: row {k} has {len(f)} columns, expected {len(COLUMNS)}'); break
        try: fr = int(f[0])
        except ValueError: problems.append(f'{path}: row {k} frame is not an integer'); break
        if fr != k: problems.append(f'{path}: row {k} has frame {fr} (return ids must be sequential from 1)'); break
        rows.append(f)
        if k == n: break
    if len(rows) < n: problems.append(f'{path}: covers returns 1..{len(rows)}, requested 1..{n} (coverage failure)')
    return rows, problems

def classify(route, qualified, candidate, n, source='tier1', baseline=None):
    q, pq = load(qualified, n); c, pc = load(candidate, n)
    problems = pq + pc; exceptions = []
    for qr, cr in zip(q, c):
        if qr == cr: continue
        for i, col in enumerate(COLUMNS[1:], 1):
            if qr[i] != cr[i]:
                exceptions.append(dict(source=source, route=route, ret=int(qr[0]), pc_qualified=qr[1], pc_candidate=cr[1], column=col,
                                       qualified=qr[i], candidate=cr[i], evidence=f'{candidate}#return={qr[0]}'))
    key = lambda e: (e['route'], e['ret'], e['pc_qualified'], e['column'], e['qualified'], e['candidate'])
    got = sorted({key(e) for e in exceptions})
    base = sorted({tuple(x) for x in baseline}) if baseline is not None else []
    new = sorted(set(got) - set(base)); missing = sorted(set(base) - set(got))
    verdict = 'PASS' if not problems and not new and not missing else 'FAIL'
    return dict(route=route, returns=n, verdict=verdict, problems=problems, exceptions=exceptions,
                exception_keys=[list(k) for k in got], new_vs_baseline=[list(k) for k in new], missing_vs_baseline=[list(k) for k in missing],
                baseline_used=baseline is not None)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--route', required=True); p.add_argument('--qualified', required=True); p.add_argument('--candidate', required=True)
    p.add_argument('--returns', type=int, required=True); p.add_argument('--baseline'); p.add_argument('--source', default='tier1')
    p.add_argument('--exceptions-out'); p.add_argument('--json-out')
    a = p.parse_args()
    baseline = None
    if a.baseline:
        b = json.loads(Path(a.baseline).read_text()); baseline = b.get(a.route, []) if isinstance(b, dict) else b
    r = classify(a.route, a.qualified, a.candidate, a.returns, a.source, baseline)
    if a.exceptions_out:
        hdr = ['source', 'route', 'return', 'pc_qualified', 'pc_candidate', 'column', 'qualified', 'candidate', 'evidence']
        Path(a.exceptions_out).write_text('\n'.join(['\t'.join(hdr)] + ['\t'.join(str(e[k]) for k in ('source', 'route', 'ret', 'pc_qualified', 'pc_candidate', 'column', 'qualified', 'candidate', 'evidence')) for e in r['exceptions']]) + '\n')
    if a.json_out: Path(a.json_out).write_text(json.dumps(r, indent=1) + '\n')
    print(json.dumps({k: r[k] for k in ('route', 'returns', 'verdict', 'problems', 'new_vs_baseline', 'missing_vs_baseline')} | {'exceptions': len(r['exceptions'])}, indent=1))
    sys.exit(0 if r['verdict'] == 'PASS' else 1)

if __name__ == '__main__':
    main()
