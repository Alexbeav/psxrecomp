"""Reproduce authored memory-card transactions from pinned, unmodified Nyma source.

No BIOS, media, retail inputs, state loading or full emulator execution.
The complete memory-card unit and exact InputDevice base-method region are
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
GOLDEN = 'cdb11f6b09b5001c5cb6b376b20041322fe67b28c2a0817ea28bb12d8a0aeb7c'


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
    fixture = Path(__file__).with_name('source_nymashock_card.cpp')
    copied = out / fixture.name
    shutil.copyfile(fixture, copied)
    cases = fixture.parents[2] / 'runtime/tests/nymashock_card_cases.h'
    shutil.copyfile(cases, out / cases.name)
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
                str(source / 'src/git.cpp'), str(source / 'src/psx/input/memcard.cpp'), str(base),
                '-Wl,--gc-sections', '-o', str(binary)]
        with (out / ('build-' + mode + '.log')).open('xb') as log:
            result = subprocess.run(argv, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        assert result.returncode == 0, out / ('build-' + mode + '.log')
        initial, final = out / ('initial-' + mode + '.mcd'), out / ('final-' + mode + '.mcd')
        captured = subprocess.run([str(binary), str(initial), str(final)], env=env, capture_output=True, check=True, timeout=30)
        assert hashlib.sha256(initial.read_bytes()).hexdigest() == '78b6d4ac9ab4d23caf7e5f04f83539bf5d994cccfb0a709d14ac53d05c8e21ef'
        assert hashlib.sha256(final.read_bytes()).hexdigest() == '18d07e0bef5e3ff27e7ad851f118a18e7fdeb8e6504d8e1790e75d0113dd4cab'
        (out / ('transactions-' + mode + '.jsonl')).write_bytes(captured.stdout)
        normalized = captured.stdout.decode().replace('\r\n', '\n')
        rows = [json.loads(line) for line in normalized.splitlines()]
        assert len(rows) == 28 and hashlib.sha256(normalized.encode()).hexdigest() == GOLDEN
        commands.append({'argv': argv, 'compile_exit': result.returncode, 'execution_exit': captured.returncode,
                         'transactions': len(rows), 'normalized_output_sha256': GOLDEN,
                         'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                         'initial_sha256': hashlib.sha256(initial.read_bytes()).hexdigest(),
                         'final_sha256': hashlib.sha256(final.read_bytes()).hexdigest()})
    inputs = [compiler, fixture, cases, Path(__file__), frontio, source / 'src/psx/input/memcard.cpp',
              source / 'src/psx/frontio.h', source / 'src/git.cpp', common / 'config.h', common / 'nyma.h']
    receipt = {'status': 'pass', 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'bizhawk_commit': BIZHAWK, 'mednafen_commit': MEDNAFEN, 'commands': commands,
               'scope': '28 authored device transactions at O0/O2; exact source memory card and base methods. No full core, FrontIO scheduler, state load or retail gameplay qualification.',
               'base_region': {'first_character': first, 'last_character': last,
                               'sha256': hashlib.sha256(region.encode()).hexdigest()},
               'bindings': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'status': 'pass', 'transactions_per_build': 28, 'optimizations': ['O0', 'O2'],
                      'normalized_output_sha256': GOLDEN, 'receipt': str(out / 'receipt.json')}))


if __name__ == '__main__':
    main()
