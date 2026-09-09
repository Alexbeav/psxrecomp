"""Compare authored complete controller replies and ACK delays to pinned source."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

golden = Path(__file__).with_name('nymashock_dualshock_transactions.jsonl').read_text().encode()
assert hashlib.sha256(golden).hexdigest() == '169e53e7a555e5f7f3d5ec4f755011c881412d14fc9a424da3660ce91618bd40'
source = [json.loads(line) for line in golden.decode().splitlines()]
assert len(source) == 260
env = dict(os.environ)
env.pop('PSX_SIO_PAD_SYNC_RX', None)
for profile in ('', 'nymashock-1.29.0-dualshock'):
    env['PSX_INPUT_ROUTE_PAD_ACK_MODEL'] = profile
    run = subprocess.run([sys.argv[1]], env=env, capture_output=True, check=True)
    native = [json.loads(line) for line in run.stdout.decode().splitlines()]
    assert len(native) == len(source)
    for actual, expected in zip(native, source):
        assert actual['case'] == expected['case']
        assert len(actual['bytes']) == len(expected['bytes'])
        for index, (a, b) in enumerate(zip(actual['bytes'], expected['bytes'])):
            wanted = b if profile else [b[0], b[1], 170 if b[2] else 0]
            assert a == wanted, (profile, expected['case'], index, a, wanted)
print('Nymashock DualShock:260 complete source transactions match; default timing negative control unchanged')
