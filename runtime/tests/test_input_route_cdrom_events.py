"""Authored synthetic product-seam fixtures; no BIOS, game or retail media."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def entry(tag, payload):
    return struct.pack('<II', tag, len(payload)) + payload + bytes((-len(payload)) % 4)


def route(path, discs):
    events = (struct.pack('<I', 3) + struct.pack('<I4B', 0, 2, 0, 0, 0) +
              struct.pack('<I4B', 1, 4, 2, 0, 0) + struct.pack('<I4B', 1, 3, 2, 0, 0))
    roster = struct.pack('<I', 2)
    for disc in discs:
        roster += struct.pack('<4B32s', 2, 0, 0, 0, hashlib.sha256(disc.read_bytes()).digest())
    ext = entry(0x00000202, events) + entry(0x00000203, roster)
    body = b''.join(struct.pack('<IHH', n, 0xffff, 0) for n in range(1, 4))
    path.write_bytes(struct.pack('<8sIIIII', b'PSXRTI3\0', 3, 8, 3, 0, len(ext)) + ext + body)


sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/tasreplays'))
from run_native import validate_disc_event_receipts
exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    disc1, disc2 = root / 'disc1.iso', root / 'disc2.iso'
    disc1.write_bytes(bytes([0x11]) * 2048 * 4)
    disc2.write_bytes(bytes([0x22]) * 2048 * 4)
    movie = root / 'events.psxrti3'
    route(movie, (disc1, disc2))
    success_out=root/'success';success_out.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
    env.update(PSX_INPUT_ROUTE_FULL_AUDIT='1',PSX_INPUT_ROUTE_CAPTURE_DIR=str(success_out))
    success = subprocess.run([exe, 'success', movie, disc1, disc2, disc2], text=True, encoding='utf-8', errors='replace',
                             capture_output=True, timeout=30,env=env)
    if success.returncode:
        raise SystemExit(success.stdout + success.stderr)
    receipts=[json.loads(line) for line in (success_out/'disc-events.jsonl').read_text(encoding='utf-8').splitlines()]
    expected=[dict(frame=0,event='tray-open',disc_index=0),dict(frame=1,event='disc-select',disc_index=2),dict(frame=1,event='tray-close',disc_index=2)]
    roster=[dict(path=str(disc1)),dict(path=str(disc2))]
    assert validate_disc_event_receipts(receipts,expected,roster,0)==3
    for fault in ('missing','duplicate','early','late','reordered','wrong-disc','rejected'):
        bad=json.loads(json.dumps(receipts))
        if fault=='missing':bad.pop()
        elif fault=='duplicate':bad.append(bad[-1])
        elif fault in ('early','late'):bad[1]['runtime_frame']+=-1 if fault=='early' else 1
        elif fault=='reordered':bad[1],bad[2]=bad[2],bad[1]
        elif fault=='wrong-disc':bad[1]['actual_bin_path']=str(disc1)
        else:bad[1]['result']=0
        try:validate_disc_event_receipts(bad,expected,roster,0)
        except ValueError:pass
        else:raise AssertionError('committed disc receipt corruption accepted:'+fault)
    disc2.write_bytes(bytes([0x22]) * 2048 * 4)
    route(movie, (disc1, disc2))
    mutate_out=root/'mutate';mutate_out.mkdir();env['PSX_INPUT_ROUTE_CAPTURE_DIR']=str(mutate_out)
    mutate = subprocess.run([exe, 'mutate', movie, disc1, disc2, disc2], text=True, encoding='utf-8', errors='replace',
                            capture_output=True, timeout=30,env=env)
    if mutate.returncode:
        raise SystemExit(mutate.stdout + mutate.stderr)
    assert len((mutate_out/'disc-events.jsonl').read_text(encoding='utf-8').splitlines())==1
    print('Actual production event receipts and missing/duplicate/early/late/order/wrong-source/rejection controls passed')
    print(success.stdout.strip())
    print(mutate.stdout.strip())
    print('PASS: synthetic session-to-CD mount/read and mutation refusal')
