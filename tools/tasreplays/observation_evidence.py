"""Strict, streaming checks for complete independent TAS observations."""
from itertools import zip_longest
from pathlib import Path
import hashlib
from compare_ram_pages import read_pages, page_hash, PAGE_COUNT, PAGE_BYTES, RAM_BYTES


def compare_returns(source, native, expected_frames):
    first = None
    coverage = [0, 0]
    paired = 0
    for expected, actual in zip_longest(read_pages(source), read_pages(native)):
        coverage[0] += expected is not None
        coverage[1] += actual is not None
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
    return {'match': first is None and coverage == [expected_frames, expected_frames],
            'compared_returns': paired, 'captured_returns': coverage, 'first_divergence': first}


def terminal_consistency(pages, raw_path, expected_frames, full_sha=None):
    terminal = None
    for row in read_pages(pages):
        terminal = row
    if terminal is None or terminal[0] != expected_frames:
        raise ValueError('RAM index does not reach the declared terminal return')
    raw = raw_path.read_bytes()
    if len(raw) != RAM_BYTES:
        raise ValueError('terminal RAM is not exactly 2 MiB')
    for page in range(PAGE_COUNT):
        if page_hash(raw[page*PAGE_BYTES:(page+1)*PAGE_BYTES]) != terminal[2][page]:
            raise ValueError('terminal RAM disagrees with its page index')
    if full_sha is not None and hashlib.sha256(raw).hexdigest() != full_sha.lower():
        raise ValueError('terminal RAM disagrees with the independent full RAM digest')
    return raw


def compare_stock_observations(stock, observed, endpoint):
    """Recompute passivity from captures, including all declared sparse images."""
    a, b = stock/'ram-frames.tsv', observed/'ram-frames.tsv'
    with a.open() as left, b.open() as right:
        count = 0
        for count, (s, n) in enumerate(zip_longest(left, right), 1):
            if s != n:
                raise ValueError(f'stock/observer RAM or lag mismatch at line {count}')
        if count != endpoint + 2:
            raise ValueError('stock/observer return coverage differs')
    frames = {0, endpoint, 71806}
    frames.update(range(1200, endpoint+1, 1200))
    frames.update(frame for frame in range(71807, endpoint+1) if frame % 60 == 0)
    evidence = [a, b]
    for frame in sorted(frames):
        name = f'frame-{frame:06d}.png'
        s, n = stock/name, observed/name
        # Identical encoded PNG bytes also establish identical decoded pixels.
        if s.read_bytes() != n.read_bytes():
            raise ValueError(f'stock/observer screenshot differs at frame {frame}')
        evidence += [s, n]
    for name in ['loaded-bios.json', 'effective-sync.json', 'effective-settings.json']:
        s, n = stock/name, observed/name
        if s.read_bytes() != n.read_bytes():
            raise ValueError('stock/observer effective identity differs: '+name)
        evidence += [s, n]
    return evidence
