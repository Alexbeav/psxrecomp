"""Prefix routes, bounded comparisons and ladders without retail assets."""
from pathlib import Path
import hashlib
import json
import struct
import tempfile
import biohazard
import replay_prefix
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, page_hash
from dualshock_route import write_route
from replay_prefix import native_span, write_prefix_route, compare_prefix_returns, parse_ladder, tier_output, run_ladder
from run_native import route_identity


def rejects(call, error=ValueError):
    try: call()
    except error: return
    raise AssertionError('invalid prefix admitted')


assert biohazard.native_span is native_span
for inputs, endpoint in [(10, 10), (10, 15), (7974, 8399), (71806, 75406), (227202, 239202)]:
    records, tail = native_span(inputs, endpoint, endpoint)
    assert records == inputs and records+tail-1 == endpoint
    for cut in (1, inputs-1):
        records, tail = native_span(inputs, endpoint, cut)
        assert records == cut+1 and tail == 0 and records+tail-1 == cut
rejects(lambda: native_span(10, 15, 10)); rejects(lambda: native_span(10, 15, 14))
for values in [(0, 1, 1), (10, 9, 1), (10, 15, 0), (10, 15, 16), (10, 15, True)]:
    rejects(lambda: native_span(*values))

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    # PSXRTI1: the prefix keeps the first records byte for byte and a consistent header.
    words = [0xfff7, 0xffff, 0xfffe, 0xff7f, 0xffbf, 0xffdf, 0xffef, 0xfffb, 0xfffd, 0xffff]
    digital = root/'digital.psxrti'
    digital.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, len(words), 0) +
                        b''.join(struct.pack('<IHH', i+1, w, 0) for i, w in enumerate(words)))
    full = route_identity(digital)
    short = write_prefix_route(digital, 4, root/'digital-prefix.psxrti')
    identity = route_identity(short)
    assert identity['frames'] == 4 and 'format' not in identity
    assert identity['words_sha256'] == hashlib.sha256(b''.join(struct.pack('<H', w) for w in words[:4])).hexdigest()
    assert short.read_bytes()[24:] == digital.read_bytes()[24:24+8*4]
    assert route_identity(write_prefix_route(digital, len(words), root/'digital-all.psxrti')) == dict(full, sha256=full['sha256'])
    for records in (0, len(words)+1, -1, 3.0, '3'):
        rejects(lambda: write_prefix_route(digital, records, root/f'bad-{records}.psxrti'))
    rejects(lambda: write_prefix_route(digital, 2, short), OSError)
    # PSXRTI2: the prefix identity equals an independently exported route of the same rows.
    rows = [(0xffef, 1, 0, 128, 255, 0), (0xffff, 129, 130, 131, 132, 0), (0xfff7, 128, 128, 128, 128, 0),
            (0xffff, 128, 128, 128, 128, 0), (0xfffe, 0, 0, 0, 0, 0), (0xffff, 255, 255, 255, 255, 0)]
    dualshock = root/'dualshock.psxrti2'; write_route(rows, dualshock)
    prefix = write_prefix_route(dualshock, 3, root/'dualshock-prefix.psxrti2')
    expected = root/'dualshock-expected.psxrti2'; write_route(rows[:3], expected)
    assert route_identity(prefix) == route_identity(expected) and route_identity(prefix)['frames'] == 3
    assert route_identity(prefix)['format'] == 'PSXRTI2'
    # Malformed sources are refused before any prefix is written.
    for name, data in [('short', digital.read_bytes()[:20]), ('reserved', digital.read_bytes()[:20]+b'\1\0\0\0'+digital.read_bytes()[24:]),
                       ('truncated', digital.read_bytes()[:-1]), ('magic', b'PSXRTI3\0'+digital.read_bytes()[8:])]:
        bad = root/f'{name}.psxrti'; bad.write_bytes(data)
        rejects(lambda: write_prefix_route(bad, 1, root/f'{name}-prefix.psxrti'))
        assert not (root/f'{name}-prefix.psxrti').exists()
    # Bounded prefix comparison mirrors compare_returns' result shape and criterion.
    zero = page_hash(bytes(PAGE_BYTES))
    def capture(name, frames, cycle_change=None, page_change=None):
        path = root/name
        with path.open('w') as stream:
            stream.write(MAGIC+'\nframe\tcycle'+''.join(f'\t{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT))+'\n')
            for frame in range(1, frames+1):
                pages = [zero]*PAGE_COUNT
                if page_change and frame == page_change[0]: pages[page_change[1]] = '0000000000000000'
                stream.write(f'{frame}\t{frame*564480+(1 if cycle_change == frame else 0)}\t'+'\t'.join(pages)+'\n')
        return path
    source = capture('source.tsv', 5)
    result = compare_prefix_returns(source, capture('native3.tsv', 3), 3)
    assert result == {'match': True, 'compared_returns': 3, 'captured_returns': [3, 3], 'first_divergence': None}
    result = compare_prefix_returns(source, capture('clock.tsv', 3, cycle_change=2), 3)
    assert not result['match'] and result['first_divergence']['frame'] == 2 and result['first_divergence']['changed_pages'] == []
    result = compare_prefix_returns(source, capture('page.tsv', 3, page_change=(3, 511)), 3)
    assert not result['match'] and result['first_divergence']['changed_pages'] == ['1FF000']
    result = compare_prefix_returns(source, capture('long.tsv', 4), 3)
    assert not result['match'] and result['first_divergence'] == {'kind': 'missing_return', 'frame': 4, 'missing_side': 'source'}
    result = compare_prefix_returns(source, capture('short.tsv', 2), 3)
    assert not result['match'] and result['first_divergence']['missing_side'] == 'native' and result['captured_returns'] == [3, 2]
    # Ladder parsing: strictly increasing, bounded by the endpoint, optional trailing full.
    assert parse_ladder('6000,60000,full', 239202) == [6000, 60000, 239202]
    assert parse_ladder(' 6000 , full ', 8399) == [6000, 8399] and parse_ladder('full', 10) == [10] and parse_ladder('10', 10) == [10]
    for text in ['full,6000', '6000,6000', '60000,6000', '300000', '', 'abc', '0', '6000,,7000', '6000,full,full', '-5']:
        rejects(lambda: parse_ladder(text, 239202))
    rejects(lambda: parse_ladder('1', 0))
    assert tier_output(root/'out', 6000, 8399) == root/'out-t6000' and tier_output(root/'out', 8399, 8399) == root/'out'
    # Ladder execution: separate outputs per tier, stop at the first non-match, overall status.
    def write_json(path, value): path.write_text(json.dumps(value))
    calls = []
    def replay(statuses):
        def call(returns, target):
            calls.append((returns, target))
            status = statuses.get(returns, 'pass' if returns == 6 else 'prefix_pass')
            if status == 'raise': raise ValueError('setup refused')
            return {'status': status, 'mechanical_match': status in ('pass', 'prefix_pass', 'diagnostic'),
                    'first_divergence': None if status != 'fail' else {'frame': returns-1}}
        return call
    report = run_ladder(root/'ladder-a', [2, 4, 6], 6, replay({}), write_json)
    assert report['status'] == 'pass' and [r['returns'] for r in report['results']] == [2, 4, 6]
    assert [Path(r['output']).name for r in report['results']] == ['ladder-a-t2', 'ladder-a-t4', 'ladder-a']
    assert json.loads((root/'ladder-a-ladder.json').read_text())['status'] == 'pass'
    assert all('host_seconds' in r for r in report['results']) and calls[-1] == (6, root/'ladder-a')
    report = run_ladder(root/'ladder-b', [2, 4, 6], 6, replay({4: 'fail'}), write_json)
    assert report['status'] == 'fail' and len(report['results']) == 2 and report['results'][1]['first_divergence'] == {'frame': 3}
    report = run_ladder(root/'ladder-c', [2, 6], 6, replay({2: 'diagnostic', 6: 'diagnostic'}), write_json)
    assert report['status'] == 'diagnostic' and len(report['results']) == 2
    report = run_ladder(root/'ladder-d', [2, 6], 6, replay({2: 'raise'}), write_json)
    assert report['status'] == 'fail' and report['results'][0]['error'] == 'setup refused' and len(report['results']) == 1
    rejects(lambda: run_ladder(root/'ladder-a', [2], 6, replay({}), write_json))
print('Replay prefix: PSXRTI1/PSXRTI2 prefix routes, native spans, bounded comparison and ladders pass')
