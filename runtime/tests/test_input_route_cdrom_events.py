"""Authored synthetic product-seam fixtures; no BIOS, game or retail media."""
import hashlib
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


exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    disc1, disc2 = root / 'disc1.iso', root / 'disc2.iso'
    disc1.write_bytes(bytes([0x11]) * 2048 * 4)
    disc2.write_bytes(bytes([0x22]) * 2048 * 4)
    movie = root / 'events.psxrti3'
    route(movie, (disc1, disc2))
    success = subprocess.run([exe, 'success', movie, disc1, disc2, disc2], text=True, encoding='utf-8', errors='replace',
                             capture_output=True, timeout=30)
    if success.returncode:
        raise SystemExit(success.stdout + success.stderr)
    disc2.write_bytes(bytes([0x22]) * 2048 * 4)
    route(movie, (disc1, disc2))
    mutate = subprocess.run([exe, 'mutate', movie, disc1, disc2, disc2], text=True, encoding='utf-8', errors='replace',
                            capture_output=True, timeout=30)
    if mutate.returncode:
        raise SystemExit(mutate.stdout + mutate.stderr)
    print(success.stdout.strip())
    print(mutate.stdout.strip())
    print('PASS: synthetic session-to-CD mount/read and mutation refusal')
