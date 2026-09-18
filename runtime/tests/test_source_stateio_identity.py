"""v7 identity dimensions, compiled at O0/O2 with -Werror."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for opt in ['-O0', '-O2']:
            exe = root / ('stateio_identity' + opt + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([a.cc, '-std=c11', opt, '-Wall', '-Wextra', '-Werror',
                            '-I' + str(here.parent / 'include'),
                            str(here / 'test_source_stateio_identity.c'),
                            str(here.parent / 'src' / 'source_stateio_identity.c'),
                            str(here.parent / 'src' / 'psx_sha256.c'),
                            '-o', str(exe)], check=True)
            probe = root / ('probe' + opt + '.bin')
            probe.write_bytes(b'identity probe')
            # Exercise the config digest against two distinct environments so a
            # constant-returning implementation cannot pass.
            env_a = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}
            env_b = dict(env_a, PSX_TIMER1_MODEL='octoshock-2.2.2')
            out_a = subprocess.run([str(exe), str(probe)], env=env_a,
                                   capture_output=True, timeout=30)
            assert out_a.returncode == 0, (opt, out_a.returncode, out_a.stdout, out_a.stderr)
            out_b = subprocess.run([str(exe), str(probe)], env=env_b,
                                   capture_output=True, timeout=30)
            assert out_b.returncode == 0, (opt, out_b.returncode, out_b.stdout, out_b.stderr)
            def config_of(out):
                for line in out.stdout.decode(errors='replace').splitlines():
                    if line.startswith('config='):
                        return line.split('=', 1)[1].strip()
                raise AssertionError((opt, out))
            # The digest must move with the resolved configuration, not be a constant.
            cfg_a, cfg_b = config_of(out_a), config_of(out_b)
            assert len(cfg_a) == 64 and len(cfg_b) == 64, (opt, cfg_a, cfg_b)
            assert cfg_a != cfg_b, (opt, 'config digest did not react to PSX_TIMER1_MODEL')
    print('PASS: v7 identity dimensions at O0/O2')

if __name__ == '__main__':
    main()
