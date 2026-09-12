"""The replay gate rejects missing, extra and changed suffix observations."""
from compare_checkpoint_runs import compare_rows, cpu_rows
from pathlib import Path
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
