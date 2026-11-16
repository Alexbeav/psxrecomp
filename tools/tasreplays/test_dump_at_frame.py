"""PS1B-268: a replay can ask the runtime for one full dump at a chosen frame.

run_native.py starts the runtime without any PSX_* variable of the calling shell, so that a replay
cannot inherit a diagnostic switch from another task. That is right, and it also meant that
PSX_DUMP_AT_FRAME could not reach a replay at all. The switch now has a name of its own:

  <title command> ... --dump-at-frame N      (launch_identity, every title command)
  run_native.py ... --dump-at-frame N        (sets PSX_DUMP_AT_FRAME=N for that run only)

The run's manifest.json records it under psx_environment, like every other selected variable.

No retail asset is used. The stand-in executable cannot run on Linux (the runner stages a plain copy),
so there the manifest is the evidence; on Windows the stand-in also writes down what it received.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from launch_identity import add_launch_arguments, check_launch_arguments, run_native_arguments

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f'FAIL: {what}')


def parse(argv):
    """The shared launch flags of a title command, or None when argparse refuses them."""
    parser = add_launch_arguments(argparse.ArgumentParser())
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            return parser.parse_args(argv)
    except SystemExit:
        return None


def refused(args):
    try:
        check_launch_arguments(args)
    except ValueError:
        return True
    return False


def main():
    with tempfile.TemporaryDirectory(prefix='psx dump at frame ', ignore_cleanup_errors=True) as directory:
        root = Path(directory)
        binary = {'binary_sha256': 'ab' * 32}

        # 1. A title command takes the option and hands it to run_native unchanged.
        args = parse(['--dump-at-frame', '191809'])
        check(args is not None, 'a title command accepts --dump-at-frame 191809')
        if args is not None:
            check(not refused(args), 'the launch checks accept --dump-at-frame 191809')
            check(run_native_arguments(args, binary) == ['--expected-exe-sha256', binary['binary_sha256'],
                                                         '--dump-at-frame', '191809'],
                  f'run_native receives --dump-at-frame 191809 (got {run_native_arguments(args, binary)})')
        plain = parse([])
        check(plain is not None and '--dump-at-frame' not in run_native_arguments(plain, binary),
              'without the option run_native receives no --dump-at-frame')
        for bad in (['--dump-at-frame', '0'], ['--dump-at-frame', '-5'],
                    ['--dump-at-frame', '100', '--ladder', '6000,full']):
            args = parse(bad)
            check(args is not None and refused(args), f'the launch checks refuse {" ".join(bad)}')

        # 2. run_native passes it to the runtime as PSX_DUMP_AT_FRAME and records it.
        for name in ('game.toml', 'disc.cue', 'bios.bin'):
            (root / name).write_bytes(b'fixture')
        route = root / 'route.psxrti'
        route.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, 2, 0)
                          + struct.pack('<IHHIHH', 1, 0xffff, 0, 2, 0xffff, 0))
        fake = root / 'fake.cmd'
        fake.write_bytes(b'@echo off\r\n'
                         b'if defined PSX_DUMP_AT_FRAME (echo %PSX_DUMP_AT_FRAME%>received.txt) else (echo unset>received.txt)\r\n')
        fake_sha = hashlib.sha256(fake.read_bytes()).hexdigest()

        def run(name, *extra, shell_value=None):
            folder = root / name
            env = {k: v for k, v in os.environ.items() if k != 'PSX_DUMP_AT_FRAME'}
            if shell_value is not None:
                env['PSX_DUMP_AT_FRAME'] = shell_value
            done = subprocess.run([sys.executable, str(HERE / 'run_native.py'), str(folder), '--exe', str(fake),
                                   '--game', str(root / 'game.toml'), '--route', str(route),
                                   '--disc', str(root / 'disc.cue'), '--bios', str(root / 'bios.bin'),
                                   '--timeout', '30', '--expected-exe-sha256', fake_sha, *extra],
                                  capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=HERE, env=env)
            manifest = folder / 'manifest.json'
            selected = json.loads(manifest.read_text(encoding='utf-8'))['psx_environment'] if manifest.exists() else None
            received = folder / 'received.txt'
            return done, selected, received.read_text().strip() if received.exists() else None

        done, selected, received = run('asked', '--dump-at-frame', '191809')
        check(selected is not None, f'run_native accepts --dump-at-frame 191809 and writes its manifest: {done.stderr.strip()[-300:]}')
        if selected is not None:
            check(selected.get('PSX_DUMP_AT_FRAME') == '191809',
                  f'the manifest records PSX_DUMP_AT_FRAME=191809 (got {selected.get("PSX_DUMP_AT_FRAME")!r})')
        if os.name == 'nt':
            check(received == '191809', f'the started process received PSX_DUMP_AT_FRAME=191809 (got {received!r})')

        # 3. The shell's own variable still does not reach a replay: only the named option does.
        done, selected, received = run('inherited', shell_value='7')
        check(selected is not None and 'PSX_DUMP_AT_FRAME' not in selected,
              f'a PSX_DUMP_AT_FRAME of the calling shell is not selected (manifest: {selected is not None})')
        if os.name == 'nt':
            check(received == 'unset', f'the started process received no PSX_DUMP_AT_FRAME from the shell (got {received!r})')

        # 4. A frame below 1 is refused before anything is staged.
        done, selected, received = run('zero', '--dump-at-frame', '0')
        check(done.returncode != 0 and not (root / 'zero').exists(), 'run_native refuses --dump-at-frame 0 and stages nothing')

    print('PASS: --dump-at-frame reaches the runtime as PSX_DUMP_AT_FRAME and is recorded; the shell cannot set it'
          if not failures else f'{failures} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
