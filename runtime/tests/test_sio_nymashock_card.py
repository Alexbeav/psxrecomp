"""Compare complete authored card traffic and NV bytes; retain the default baseline."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

root = Path(__file__).parent
expected = {}
for name in ('nymashock_card_transactions', 'nymashock_card_default_baseline'):
    expected[name] = [json.loads(line) for line in (root / (name + '.jsonl')).read_text().splitlines()]
    assert len(expected[name]) == 28
assert hashlib.sha256((root / 'nymashock_card_transactions.jsonl').read_text().encode()).hexdigest() == 'cdb11f6b09b5001c5cb6b376b20041322fe67b28c2a0817ea28bb12d8a0aeb7c'

# Authored blank peripheral medium. The source collector separately checks its
# own constructor-produced bytes; no BIOS, retail save or emulator state here.
blank = bytearray(131072)
blank[0:2] = b'MC'; blank[127] = 14
for offset in range(128, 2048, 128):
    blank[offset] = 160; blank[offset+8:offset+10] = b'\xff\xff'; blank[offset+127] = 160
for offset in range(2048, 4608, 128):
    blank[offset:offset+4] = b'\xff'*4; blank[offset+8:offset+10] = b'\xff\xff'

env = {k:v for k,v in os.environ.items() if not k.startswith('PSX_')}
for profile, golden in (('', 'nymashock_card_default_baseline'), ('nymashock-1.29.0', 'nymashock_card_transactions')):
    env['PSX_INPUT_ROUTE_CARD_MODEL'] = profile
    with tempfile.TemporaryDirectory(prefix='nymashock-card-') as folder:
        card = Path(folder) / 'initial.mcd'; card.write_bytes(blank)
        final = Path(folder) / 'final.mcd'
        result = subprocess.run([sys.argv[1], str(card), str(final)], env=env, capture_output=True, check=True, timeout=30)
        rows = [json.loads(line) for line in result.stdout.decode().splitlines() if line.startswith('{')]
        assert rows == expected[golden], (profile, [(a['case'], a == b) for a,b in zip(rows, expected[golden])])
        digest = hashlib.sha256(final.read_bytes()).hexdigest()
        manifest = json.loads((root / 'nymashock_card_identities.json').read_text())
        assert digest == manifest['source_final_sha256' if profile else 'default_final_sha256']
        assert hashlib.sha256(blank).hexdigest() == manifest['initial_sha256']
print('Nymashock card:28 source transactions and final NV bytes match; deadline/pulse/cancel/IRQ tests pass; default baseline unchanged')
