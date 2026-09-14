"""Follow a running native ram-pages.tsv and compare it against a reference.

The watcher is additive evidence: it can stop the native process at the first
divergence and records what it saw, but it never decides a pass. The post-run
comparison in each title module remains the verdict authority.
"""
import hashlib
from itertools import islice
import json
import os
from pathlib import Path
import threading
import time
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, read_pages

HEADER = ['frame', 'cycle'] + [f'{p*PAGE_BYTES:06X}' for p in range(PAGE_COUNT)]
STOP_REQUEST = 'stop-request.json'


def write_stop_request(run, payload):
    """Atomically publish <run>/stop-request.json for run_native's wait loop."""
    target = Path(run) / STOP_REQUEST
    temporary = target.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, target)
    return target


def page_reference(path, endpoint):
    """Source page rows bounded to the compared endpoint (the verdict's islice)."""
    return islice(read_pages(Path(path)), endpoint)


def line_hash_reference(path):
    """Tekken-style {frame: sha256 of ' '.join(values)} from the gz table."""
    import gzip
    table = {}
    for line in gzip.decompress(Path(path).read_bytes()).decode().splitlines():
        frame, sha = line.split()
        table[int(frame)] = sha
    return table


def line_hash(values):
    return hashlib.sha256(' '.join(values).encode()).hexdigest()


def follow_lines(path, run, cancelled=None, poll=0.5):
    """Yield complete text lines from a file another process is appending to.

    Waits for the file to appear unless <run>/exit.json exists or cancelled is
    set; after either, drains every complete line already written and ends. A
    trailing fragment without a newline is never yielded.
    """
    path, run = Path(path), Path(run)
    done = lambda: (run / 'exit.json').exists() or (cancelled is not None and cancelled.is_set())
    while not path.exists():
        if done():
            return
        time.sleep(poll)
    buffer = b''
    first = True
    with path.open('rb') as stream:
        while True:
            chunk = stream.read(1 << 20)
            if chunk:
                buffer += chunk
                *lines, buffer = buffer.split(b'\n')
                for line in lines:
                    if first:
                        first = False
                        if line.startswith(b'\xef\xbb\xbf'):
                            line = line[3:]
                    yield line.decode('utf-8').rstrip('\r')
                continue
            if done():
                # One more read after the writer is known to have finished.
                chunk = stream.read(1 << 20)
                if chunk:
                    buffer += chunk
                    *lines, buffer = buffer.split(b'\n')
                    for line in lines:
                        yield line.decode('utf-8').rstrip('\r')
                return
            time.sleep(poll)


def parse_rows(lines, name='ram-pages.tsv'):
    """Validate the stream exactly like compare_ram_pages.read_pages, row by row.

    Yields (frame, cycle, hashes, values): the same triple read_pages yields
    plus the raw tab-split values for line-hash references.
    """
    lines = iter(lines)
    magic = next(lines, None)
    if magic is None:
        return
    if magic != MAGIC:
        raise ValueError(f'unsupported RAM index: {name}')
    header = next(lines, None)
    if header is None:
        return
    if header.split('\t') != HEADER:
        raise ValueError(f'invalid page columns: {name}')
    previous = 0
    for expected_frame, line in enumerate(lines, 1):
        row = line.split('\t')
        if len(row) != len(HEADER) or int(row[0]) != expected_frame:
            raise ValueError(f'incomplete or discontinuous RAM index: {name}, frame {expected_frame}')
        cycle = int(row[1])
        if cycle <= previous or any(len(h) != 16 or any(c not in '0123456789ABCDEF' for c in h) for h in row[2:]):
            raise ValueError(f'invalid cycle/hash: {name}, frame {expected_frame}')
        previous = cycle
        yield expected_frame, cycle, row[2:], row


class Watcher(threading.Thread):
    """Compare native returns as they appear; optionally request an early stop.

    reference: page rows (kind='pages', from page_reference) or {frame: sha}
    (kind='line_hash'). endpoint bounds the comparison. The result dict is
    complete after finish().
    """

    def __init__(self, run, reference, endpoint, kind='pages', stop_on_divergence=False,
                 poll=0.5, progress_every=10000, label='native'):
        super().__init__(name='stream-compare', daemon=True)
        if kind not in ('pages', 'line_hash'):
            raise ValueError('unsupported streaming reference kind')
        if type(endpoint) is not int or endpoint < 1:
            raise ValueError('streaming endpoint must be a positive return count')
        self.directory = Path(run)
        self.reference = reference
        self.kind = kind
        self.endpoint = endpoint
        self.poll = poll
        self.progress_every = progress_every
        self.label = label
        self.cancelled = threading.Event()
        self.result = {'kind': kind, 'endpoint': endpoint, 'compared_returns': 0, 'native_returns': 0,
                       'first_divergence': None, 'stop_on_divergence': bool(stop_on_divergence),
                       'stopped_on_divergence': False, 'stop_request': None,
                       'reference_exhausted': False, 'complete_through_endpoint': False,
                       'observation_error': None, 'watcher_error': None, 'seconds': None,
                       'scope': 'streaming evidence only; the post-run comparison decides the verdict'}

    def _reference_row(self, native):
        frame = native[0]
        if self.kind == 'line_hash':
            expected = self.reference.get(frame)
            return None if expected is None else (frame, expected)
        return next(self.reference, None)

    def _divergence(self, expected, native):
        frame, cycle, hashes, values = native
        if self.kind == 'line_hash':
            if expected[1] != line_hash(values):
                return {'frame': frame, 'kind': 'line_hash'}
            return None
        changed = [f'{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT) if expected[2][i] != hashes[i]]
        if expected[1] != cycle or changed:
            return {'frame': frame, 'source_cycle': expected[1], 'native_cycle': cycle, 'changed_pages': changed}
        return None

    def _report(self, divergence):
        if divergence.get('kind') == 'line_hash':
            print(f'LINE-HASH DIVERGENCE at return {divergence["frame"]}', flush=True)
            return
        delta = divergence['native_cycle'] - divergence['source_cycle']
        pages = divergence['changed_pages']
        if pages:
            print(f'RAM DIVERGENCE at return {divergence["frame"]}: clock_delta={delta:+d} '
                  f'changed_pages={len(pages)} first={pages[:8]}', flush=True)
        else:
            print(f'CLOCK DIVERGENCE at return {divergence["frame"]}: clock_delta={delta:+d}', flush=True)

    def run(self):
        started = time.monotonic()
        result = self.result
        try:
            lines = follow_lines(self.directory / 'ram-pages.tsv', self.directory, self.cancelled, self.poll)
            for native in parse_rows(lines):
                result['native_returns'] = native[0]
                expected = self._reference_row(native)
                if expected is None:
                    result['reference_exhausted'] = True
                    print(f'reference exhausted at {self.label} return {native[0]}', flush=True)
                    break
                result['compared_returns'] += 1
                divergence = self._divergence(expected, native)
                if divergence is not None:
                    result['first_divergence'] = divergence
                    self._report(divergence)
                    if result['stop_on_divergence']:
                        request = {'reason': 'divergence', 'frame': divergence['frame'], 'kind': self.kind,
                                   'requested_by': 'stream_compare', 'first_divergence': divergence,
                                   'compared_returns': result['compared_returns']}
                        write_stop_request(self.directory, request)
                        result['stopped_on_divergence'] = True
                        result['stop_request'] = request
                        print(f'stop requested at return {divergence["frame"]}', flush=True)
                    break
                count = result['compared_returns']
                if count % self.progress_every == 0:
                    rate = count / max(time.monotonic() - started, 1e-9)
                    print(f'match through return {count} ({rate:.1f} ret/s)', flush=True)
                if count >= self.endpoint:
                    result['complete_through_endpoint'] = True
                    print(f'MATCH complete through {count}', flush=True)
                    break
        except ValueError as error:
            result['observation_error'] = str(error)
            print(f'streaming observation error: {error}', flush=True)
        except Exception as error:  # never let the watcher hang the harness
            result['watcher_error'] = f'{type(error).__name__}: {error}'
        finally:
            result['seconds'] = time.monotonic() - started

    def finish(self, timeout=None):
        """Drain what the runtime wrote, then return the result dict."""
        self.cancelled.set()
        self.join(timeout)
        if self.is_alive():
            self.result['watcher_error'] = 'watcher did not finish'
        return self.result
