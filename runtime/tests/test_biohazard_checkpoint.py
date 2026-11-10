"""Exercise production SIO/CD checkpoint continuations at O0 and O2."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cc', default='gcc')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as folder:
    for opt in ('-O0', '-O2'):
        for name in ('sio_checkpoint', 'sio_ack_timing', 'sio_nymashock_card', 'cdrom_checkpoint'):
            exe = Path(folder) / (name + opt + '.exe')
            subprocess.run([a.cc, '-std=c11', '-D_XOPEN_SOURCE=700', opt, '-flto', '-fwhole-program',
                            '-I' + str(root / 'include'), str(root / 'tests' / ('test_' + name + '.c')),
                            str(root / 'src' / 'psx_sha256.c'),
                            '-o', str(exe)], check=True)
            command = [str(exe)]
            if name == 'sio_nymashock_card':
                card = Path(folder) / ('card' + opt + '.mcd')
                card.write_bytes(bytes(range(256)) * 512)
                command += [str(card), str(Path(folder) / ('final' + opt + '.mcd'))]
            result = subprocess.run(command, capture_output=True, timeout=30)
            assert result.returncode == 0, (name, opt, result.stdout, result.stderr)
            print(name, opt, 'PASS')
print('PASS: Biohazard device checkpoint checks at O0/O2')
