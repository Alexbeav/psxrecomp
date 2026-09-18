"""Bounded prefix replays and ladders over unchanged original input.

A prefix replays the first N original inputs plus one record and compares N
returns; it never shifts, drops or edits an input inside the prefix. A prefix
receipt is diagnostic and never a full-route verdict.
"""
from itertools import islice, zip_longest
from pathlib import Path
import struct
import time
from compare_ram_pages import PAGE_BYTES, PAGE_COUNT, read_pages

FORMATS = {b'PSXRTI1\0': (1, 8), b'PSXRTI2\0': (2, 12)}


def native_span(input_frames, source_endpoint, wanted):
    """The native completion hook precedes its last return observation."""
    if any(type(x) is not int for x in (input_frames, source_endpoint, wanted)) or not 1 <= input_frames <= source_endpoint or not 1 <= wanted <= source_endpoint:
        raise ValueError('invalid input/source/native boundary')
    if wanted == source_endpoint: return input_frames, source_endpoint-input_frames+1
    if wanted >= input_frames: raise ValueError('use full replay for the ending tail')
    return wanted+1, 0


def write_prefix_route(source_route, records, target):
    """Write the first `records` records of a PSXRTI1/PSXRTI2 route to a new file."""
    data = Path(source_route).read_bytes()
    if len(data) < 24: raise ValueError('short route')
    magic, version, size, count, reserved = struct.unpack_from('<8sIIII', data)
    if magic not in FORMATS or (version, size) != FORMATS[magic] or reserved:
        raise ValueError('unsupported route header')
    if not 0 < count <= 1000000 or len(data) != 24+size*count: raise ValueError('route frame count or size')
    if type(records) is not int or not 1 <= records <= count: raise ValueError('prefix outside the route')
    target = Path(target)
    with target.open('xb') as stream:
        stream.write(struct.pack('<8sIIII', magic, version, size, records, 0))
        stream.write(data[24:24+size*records])
    return target


def compare_prefix_returns(source, native, wanted, start=0):
    """compare_returns-shaped result for source returns start+1..wanted (start > 0
    for a run resumed from a checkpoint at that return)."""
    first = None; coverage = [0, 0]; paired = 0
    for expected, actual in zip_longest(islice(read_pages(Path(source)), start, wanted),
                                        read_pages(Path(native), first_frame=start+1)):
        coverage[0] += expected is not None; coverage[1] += actual is not None
        if expected is None or actual is None:
            if first is None:
                first = {'kind': 'missing_return', 'frame': (expected or actual)[0],
                         'missing_side': 'source' if expected is None else 'native'}
            continue
        paired += 1
        changed = [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT) if expected[2][i] != actual[2][i]]
        if first is None and (expected[1] != actual[1] or changed):
            first = {'kind': 'state_or_clock', 'frame': expected[0],
                     'source_cycle': expected[1], 'native_cycle': actual[1], 'changed_pages': changed}
    return {'match': first is None and coverage == [wanted-start, wanted-start],
            'compared_returns': paired, 'captured_returns': coverage, 'first_divergence': first}


def parse_ladder(text, endpoint):
    """Strictly increasing return counts; an optional final 'full' means the endpoint."""
    if type(endpoint) is not int or endpoint < 1: raise ValueError('invalid ladder endpoint')
    tiers = []
    for item in str(text).split(','):
        item = item.strip()
        if item == 'full': value = endpoint
        elif item.isdigit(): value = int(item)
        else: raise ValueError(f'invalid ladder tier: {item!r}')
        if not 1 <= value <= endpoint: raise ValueError(f'ladder tier {value} exceeds the route endpoint {endpoint}')
        if tiers and value <= tiers[-1]: raise ValueError('ladder tiers must be strictly increasing')
        tiers.append(value)
    if not tiers: raise ValueError('empty ladder')
    return tiers


def tier_output(output, returns, endpoint):
    output = Path(output)
    return output if returns == endpoint else output.with_name(f'{output.name}-t{returns}')


def run_ladder(output, tiers, endpoint, replay, write_json):
    """Run tiers in order; replay(returns, output) returns a receipt with
    status/mechanical_match/first_divergence. Stops at the first tier that does
    not match. Writes <output>-ladder.json and returns its dict."""
    output = Path(output)
    ladder = output.with_name(output.name+'-ladder.json')
    if ladder.exists(): raise ValueError('choose a fresh ladder output')
    results = []; statuses = []
    for returns in tiers:
        target = tier_output(output, returns, endpoint)
        started = time.monotonic()
        print(f'ladder tier {returns}{" (full)" if returns == endpoint else ""}: {target}', flush=True)
        try:
            receipt = replay(returns, target)
        except (ValueError, RuntimeError, OSError, KeyError) as error:
            receipt = {'status': 'fail', 'mechanical_match': False, 'first_divergence': None, 'error': str(error)}
        entry = {'returns': returns, 'full': returns == endpoint, 'output': str(target),
                 'status': receipt.get('status'), 'mechanical_match': receipt.get('mechanical_match') is True,
                 'first_divergence': receipt.get('first_divergence'), 'error': receipt.get('error'),
                 'host_seconds': time.monotonic()-started}
        results.append(entry); statuses.append(entry['status'])
        if not entry['mechanical_match']: break
    completed = len(results) == len(tiers) and all(r['mechanical_match'] for r in results)
    # Only a ladder whose last tier is the full route can report a pass; prefix-only ladders stay diagnostic.
    overall = ('fail' if not completed else 'diagnostic' if not all(s in ('pass', 'prefix_pass') for s in statuses)
               else 'pass' if tiers[-1] == endpoint else 'prefix_pass')
    report = {'schema': 'psx-tas-ladder-v1', 'status': overall, 'tiers': tiers, 'endpoint': endpoint,
              'results': results, 'output': str(output),
              'scope': 'each tier is a separate replay; only the full tier at the requested output can qualify'}
    write_json(ladder, report)
    print(f'ladder {overall}: {ladder}', flush=True)
    return report
