"""The replay gate rejects missing, extra and changed suffix observations."""
from compare_checkpoint_runs import compare_rows, compare_runs, cpu_rows, saved_states
from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, page_hash
import json
from pathlib import Path
import struct
import tempfile

assert compare_rows(iter([1,2,3,4]),iter([3,4]),2,4)['passed']
for suffix in ([3], [3,4,5], [3,9]):
    assert not compare_rows(iter([1,2,3,4]),iter(suffix),2,4)['passed']
with tempfile.TemporaryDirectory() as folder:
    path=Path(folder)/'cpu.tsv'
    path.write_text('frame\tcycle\n3\t30\n5\t50\n')
    try:
        list(cpu_rows(path,3))
    except ValueError:
        pass
    else:
        raise AssertionError('skipped return admitted')
print('Replay comparator rejects truncated, extra, changed and discontinuous suffixes')

ZERO = page_hash(bytes(PAGE_BYTES))


def run_dir(root, name, first, last, states, saved):
    """A capture holding returns first..last, checkpoint images and saved-states.json."""
    run = root/name; run.mkdir()
    (run/'cpu-return.tsv').write_text('frame\tcycle\n' + ''.join(f'{f}\t{f*10}\n' for f in range(first, last+1)))
    header = MAGIC + '\nframe\tcycle' + ''.join(f'\t{i*PAGE_BYTES:06X}' for i in range(PAGE_COUNT)) + '\n'
    (run/'ram-pages.tsv').write_text(header + ''.join(f'{f}\t{f*10}\t' + '\t'.join([ZERO]*PAGE_COUNT) + '\n'
                                                      for f in range(first, last+1)))
    for frame, payload in states.items():
        (run/f'tas-state-{frame:06d}.pst').write_bytes(
            struct.pack('<9I', 0x50535842, 10, 0, 0, 0, 0, 0, 1, 0) + struct.pack('<IIQ', 1, 0, len(payload)) + payload)
    (run/'saved-states.json').write_text(json.dumps(
        [{'frame': request, 'state_frame': frame, 'state': f'tas-state-{frame:06d}.pst', 'valid': True}
         for request, frame in saved.items()]))
    return run


with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    # The request at 5 fell on an unrepresentable return and was captured at 6, which also
    # satisfies the request at 6. A resume from that checkpoint compares only later captures.
    baseline = run_dir(root, 'A1', 1, 10, {6: b'six', 9: b'nine'}, {5: 6, 6: 6, 9: 9})
    assert saved_states(baseline) == {5: 'tas-state-000006.pst', 6: 'tas-state-000006.pst', 9: 'tas-state-000009.pst'}
    resumed = run_dir(root, 'resume', 7, 10, {9: b'nine'}, {9: 9})
    result = compare_runs(baseline, resumed, 6, 10, [5, 6, 9])
    assert result['passed'] and list(result['states']) == ['9'], result
    # A candidate that took the same request at another return is a mismatch, not a match.
    moved = run_dir(root, 'moved', 7, 10, {10: b'nine'}, {9: 10})
    result = compare_runs(baseline, moved, 6, 10, [5, 6, 9])
    assert not result['passed'] and result['states']['9']['differences'][0]['section'] == 'CAPTURE_RETURN'
print('Replay comparator follows deferred captures by the return where each was taken')
