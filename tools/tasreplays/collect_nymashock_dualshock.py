"""Reproduce authored controller transactions from pinned, unmodified Nyma source.

No BIOS, media, retail inputs, state loading or full emulator execution.
The complete DualShock unit and exact InputDevice base-method region are
compiled; the FrontIO scheduler is outside this device fixture's scope.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess

MEDNAFEN = 'ddf225cf63b7b355cb2ac7772450cf473f4b53ac'
BIZHAWK = '745efb1dd8eb82f31ba9201a79cdfc5bcaf1f5d1'
GOLDEN = '169e53e7a555e5f7f3d5ec4f755011c881412d14fc9a424da3660ce91618bd40'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mednafen', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='g++')
    args = parser.parse_args()
    source = args.mednafen.resolve(strict=True)
    common = source.parent / 'common'
    for root, expected in ((source, MEDNAFEN), (source.parent, BIZHAWK)):
        assert subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() == expected
        assert not subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain'], text=True).strip()
    compiler = Path(shutil.which(args.compiler) or args.compiler).resolve(strict=True)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture = Path(__file__).with_name('source_nymashock_dualshock.cpp')
    copied = out / fixture.name
    shutil.copyfile(fixture, copied)
    frontio = source / 'src/psx/frontio.cpp'
    text = frontio.read_text()
    first, last = text.index('InputDevice::InputDevice()'), text.index('static unsigned EP_to_MP')
    region = text[first:last]
    base = out / 'input-device-base.cpp'
    base.write_text('#include <src/psx/psx.h>\n#include <src/psx/frontio.h>\nnamespace MDFN_IEN_PSX {\n' + region + '\n}\n')
    env = dict(os.environ)
    env['PATH'] = str(compiler.parent) + os.pathsep + env['PATH']
    commands = []
    for mode in ('O0', 'O2'):
        binary = out / ('source-' + mode + ('.exe' if os.name == 'nt' else ''))
        argv = [str(compiler), '-std=c++17', '-' + mode, '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                '-DHAVE_CONFIG_H', '-DMDFN_DISABLE_NO_OPT_ERRWARN', '-I' + str(common),
                '-I' + str(source), '-I' + str(source / 'src/trio'), str(copied),
                str(source / 'src/git.cpp'), str(source / 'src/psx/input/dualshock.cpp'), str(base),
                '-Wl,--gc-sections', '-o', str(binary)]
        with (out / ('build-' + mode + '.log')).open('xb') as log:
            result = subprocess.run(argv, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        assert result.returncode == 0, out / ('build-' + mode + '.log')
        captured = subprocess.run([str(binary)], env=env, capture_output=True, check=True, timeout=30)
        (out / ('transactions-' + mode + '.jsonl')).write_bytes(captured.stdout)
        normalized = captured.stdout.decode().replace('\r\n', '\n')
        rows = [json.loads(line) for line in normalized.splitlines()]
        assert len(rows) == 260 and hashlib.sha256(normalized.encode()).hexdigest() == GOLDEN
        commands.append({'argv': argv, 'compile_exit': result.returncode, 'execution_exit': captured.returncode,
                         'transactions': len(rows), 'normalized_output_sha256': GOLDEN,
                         'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()})
    inputs = [compiler, fixture, Path(__file__), frontio, source / 'src/psx/input/dualshock.cpp',
              source / 'src/psx/frontio.h', source / 'src/git.cpp', common / 'config.h', common / 'nyma.h']
    receipt = {'status': 'pass', 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'bizhawk_commit': BIZHAWK, 'mednafen_commit': MEDNAFEN, 'commands': commands,
               'scope': '260 authored device transactions at O0/O2; exact source DualShock and base methods. No full core, FrontIO scheduler, state load or retail gameplay qualification.',
               'base_region': {'first_character': first, 'last_character': last,
                               'sha256': hashlib.sha256(region.encode()).hexdigest()},
               'bindings': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'status': 'pass', 'transactions_per_build': 260, 'optimizations': ['O0', 'O2'],
                      'normalized_output_sha256': GOLDEN, 'receipt': str(out / 'receipt.json')}))


if __name__ == '__main__':
    main()
