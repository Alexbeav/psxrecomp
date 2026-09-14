"""Streaming comparison against a simulated runtime; no retail assets."""
from pathlib import Path
import json
import tempfile
import threading
import time
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, page_hash, read_pages
from observation_evidence import compare_returns
from stream_compare import Watcher, follow_lines, parse_rows, line_hash, write_stop_request, page_reference

ZERO = page_hash(bytes(PAGE_BYTES))
HEADER = '\t'.join(['frame', 'cycle'] + [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT)])


def row(frame, cycle_bump=0, page=None):
    pages = [ZERO] * PAGE_COUNT
    if page is not None: pages[page] = '0000000000000001'
    return '\t'.join([str(frame), str(frame*564480+cycle_bump)] + pages)


def capture_text(frames, cycle_bump_at=None, page_at=None, magic=MAGIC):
    lines = [magic, HEADER] + [row(f, 1 if cycle_bump_at == f else 0, page_at[1] if page_at and page_at[0] == f else None)
                               for f in range(1, frames+1)]
    return '\n'.join(lines) + '\n'


def simulate(run, text, chunk=4096, delay=0.02, create_delay=0.0, truncate_tail=False, write_exit=True):
    """Append text in arbitrary byte chunks like a live runtime, then publish exit.json."""
    data = text.encode()
    if truncate_tail: data = data[:-1]
    def body():
        time.sleep(create_delay)
        with (run/'ram-pages.tsv').open('wb') as stream:
            for offset in range(0, len(data), chunk):
                stream.write(data[offset:offset+chunk]); stream.flush(); time.sleep(delay)
        if write_exit:
            (run/'exit.json').write_text(json.dumps({'exit_code': 0, 'stop_reason': None}))
    thread = threading.Thread(target=body, daemon=True); thread.start()
    return thread


def watch(root, reference_text, native_text, endpoint, kind='pages', stop=False, **simulate_args):
    run = root/f'run-{time.monotonic_ns()}'; run.mkdir()
    reference = root/f'reference-{time.monotonic_ns()}.tsv'; reference.write_text(reference_text)
    if kind == 'pages': table = page_reference(reference, endpoint)
    else: table = {frame: line_hash(values) for frame, _, _, values in parse_rows(reference_text.splitlines())}
    watcher = Watcher(run, table, endpoint, kind=kind, stop_on_divergence=stop, poll=0.02, progress_every=3)
    watcher.start()
    thread = simulate(run, native_text, **simulate_args)
    thread.join(30)
    result = watcher.finish(30)
    return run, reference, result


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    same = capture_text(5)
    # Matching stream, delivered in 7-byte fragments that split every line.
    run, reference, result = watch(root, same, same, 5, chunk=7, delay=0.0, create_delay=0.1)
    assert result['compared_returns'] == 5 and result['first_divergence'] is None and result['complete_through_endpoint']
    assert not result['stopped_on_divergence'] and not (run/'stop-request.json').exists()
    assert result['observation_error'] is None and result['watcher_error'] is None
    assert [r[:2] for r in parse_rows(same.splitlines())] == [(f, f*564480) for f in range(1, 6)]
    # Clock-only divergence: same criterion as compare_returns, empty changed_pages, no stop unless enabled.
    run, reference, result = watch(root, same, capture_text(5, cycle_bump_at=3), 5)
    assert result['first_divergence'] == {'frame': 3, 'source_cycle': 3*564480, 'native_cycle': 3*564480+1, 'changed_pages': []}
    assert result['compared_returns'] == 3 and not result['stopped_on_divergence'] and not (run/'stop-request.json').exists()
    verdict = compare_returns(reference, run/'ram-pages.tsv', 5)['first_divergence']
    assert (verdict['frame'], verdict['changed_pages']) == (3, [])
    # Page divergence with the stop enabled writes the stop request the runner honours.
    run, reference, result = watch(root, same, capture_text(5, page_at=(4, 511)), 5, stop=True)
    assert result['first_divergence'] == {'frame': 4, 'source_cycle': 4*564480, 'native_cycle': 4*564480, 'changed_pages': ['1FF000']}
    assert result['stopped_on_divergence'] and result['stop_on_divergence']
    request = json.loads((run/'stop-request.json').read_text())
    assert request['reason'] == 'divergence' and request['frame'] == 4 and request['first_divergence'] == result['first_divergence']
    verdict = compare_returns(reference, run/'ram-pages.tsv', 5)['first_divergence']
    assert (verdict['frame'], verdict['changed_pages']) == (4, ['1FF000'])
    # Line-hash reference (Tekken): match, then a clock difference reported without cycles.
    run, reference, result = watch(root, same, same, 5, kind='line_hash')
    assert result['compared_returns'] == 5 and result['first_divergence'] is None
    run, reference, result = watch(root, same, capture_text(5, cycle_bump_at=2), 5, kind='line_hash', stop=True)
    assert result['first_divergence'] == {'frame': 2, 'kind': 'line_hash'} and result['stopped_on_divergence']
    assert json.loads((run/'stop-request.json').read_text())['kind'] == 'line_hash'
    # A trailing fragment without its newline is never counted; exit.json ends the tail.
    run, reference, result = watch(root, same, same, 5, chunk=100, truncate_tail=True)
    assert result['compared_returns'] == 4 and result['first_divergence'] is None and not result['complete_through_endpoint']
    # The comparison is bounded to the endpoint; a shorter reference is reported, not matched.
    run, reference, result = watch(root, capture_text(3), same, 3)
    assert result['compared_returns'] == 3 and result['complete_through_endpoint'] and result['native_returns'] == 3
    run, reference, result = watch(root, capture_text(3), same, 5)
    assert result['reference_exhausted'] and result['compared_returns'] == 3 and result['first_divergence'] is None
    # Invalid capture header is an observation error, not a match.
    run, reference, result = watch(root, same, capture_text(5, magic='# other'), 5)
    assert result['observation_error'] and result['compared_returns'] == 0
    # exit.json without any capture: nothing compared, no error, no hang.
    run = root/'no-capture'; run.mkdir(); (run/'exit.json').write_text('{}')
    watcher = Watcher(run, page_reference(reference, 5), 5, poll=0.02); watcher.start()
    result = watcher.finish(10)
    assert result['compared_returns'] == 0 and result['observation_error'] is None and not watcher.is_alive()
    # finish() cancels a watcher whose runner never produced anything.
    run = root/'never'; run.mkdir()
    watcher = Watcher(run, page_reference(reference, 5), 5, poll=0.02); watcher.start()
    started = time.monotonic(); result = watcher.finish(10)
    assert not watcher.is_alive() and time.monotonic()-started < 5 and result['compared_returns'] == 0
    # follow_lines drains complete lines written before cancellation and strips CR and BOM.
    run = root/'drain'; run.mkdir()
    (run/'ram-pages.tsv').write_bytes(b'\xef\xbb\xbfa\r\nb\r\nc')
    cancelled = threading.Event(); cancelled.set()
    assert list(follow_lines(run/'ram-pages.tsv', run, cancelled, poll=0.01)) == ['a', 'b']
    # Stop requests are published atomically and readable as JSON.
    target = write_stop_request(run, {'reason': 'divergence', 'frame': 7})
    assert target.name == 'stop-request.json' and json.loads(target.read_text())['frame'] == 7
    assert not target.with_suffix('.json.tmp').exists()
    # Streamed rows equal read_pages rows for the same file.
    assert [r[:3] for r in parse_rows(same.splitlines())] == list(read_pages(reference))
    for bad in [Watcher, ]:
        try: bad(run, {}, 0, kind='line_hash'); raise AssertionError('zero endpoint admitted')
        except ValueError: pass
        try: bad(run, {}, 1, kind='other'); raise AssertionError('unknown kind admitted')
        except ValueError: pass
print('Streaming comparison: tailing, page/clock/line-hash divergence, stop request, bounded reference and partial lines pass')
