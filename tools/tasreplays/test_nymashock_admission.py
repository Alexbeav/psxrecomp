"""Authored failures must not become admitted independent source observations."""
from pathlib import Path
import tempfile
from nymashock_admission import return_rows,qualify

def rejects(call):
    try:call()
    except (ValueError,OSError):return
    raise AssertionError('invalid source admitted')

header='frame\tcycle\tlag_count\tram_sha256\n'
rows=[f'0\t0\t0\t{"0"*64}\n',f'1\t123\t1\t{"1"*64}\n',f'2\t456\t1\t{"2"*64}\n']
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);p=root/'ram.tsv'
    p.write_text(header+''.join(rows));assert list(return_rows(p,2))[-1]==(2,456,1,'2'*64)
    variants=[header+''.join(rows[:-1]),header+''.join(rows)+rows[-1],
              header+rows[0]+rows[2],header+rows[0]+rows[1].replace('\t123\t','\t0\t')+rows[2],
              header+rows[0]+rows[1]+rows[2].replace('\t1\t','\t0\t'),
              header+rows[0]+rows[1].replace('\t1\t1','\t2\t1')+rows[2],
              header+rows[0]+rows[1]+rows[2][:-2]+'Z\n','frame\tclock\n'+''.join(rows)]
    for data in variants:
        p.write_text(data);rejects(lambda:list(return_rows(p,2)))
    rejects(lambda:qualify(root,root,root/'ref.json'))
    assert not (root/'ref.json').exists()
    out=root/'existing.json';out.write_bytes(b'preserve')
    rejects(lambda:qualify(root,root/'other',out));assert out.read_bytes()==b'preserve'
print('Nymashock admission:source continuity/clock/lag/shape failures and reference overwrite rejected')
