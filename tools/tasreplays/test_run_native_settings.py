"""PS1B-394: the settings.toml that run_native.py writes keeps its two boot keys through the loader.

The loader drops fast_boot and bios_hle from a file without `settings_format = 2`: such a file is taken for
one that an old launcher wrote by itself (PS1B-360). The runner's file had no such line, so --fast-boot did
nothing and the run's manifest hashed a settings file that named a boot mode the run did not use.

  test_run_native_settings.py --probe <settings_boot_keys_probe>

For each of the four (--fast-boot, --hle) combinations the runner's own text goes through the real loader
(the probe is built from recompiler/src/config_loader.cpp). Both keys must be applied with the values the
arguments gave, and the loader must not report them as echoes. The control is the same text without the
format line: there the loader must drop both keys, or this test proves nothing.
"""
import argparse, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from run_native import settings_text

failures = 0
def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f'FAIL: {what}')

def probe(exe, folder, text):
    f = Path(folder) / 'settings.toml'
    f.write_text(text, encoding='utf-8')
    r = subprocess.run([str(exe), str(f)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        return None
    return dict(kv.split('=') for kv in r.stdout.split())

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', type=Path, required=True)
    a = p.parse_args()
    bios, disc = Path('some folder/SCPH1001.BIN'), Path('some folder/Game (USA).cue')
    with tempfile.TemporaryDirectory(prefix='psx runner settings ') as folder:
        for fast_boot in (False, True):
            for hle in (False, True):
                text = settings_text('software', fast_boot, hle, bios, disc, True)
                tag = f'--fast-boot={fast_boot} --hle={hle}'
                lines = text.splitlines()
                check(lines[0] == 'settings_format = 2', f'{tag}: the first line is settings_format = 2')
                check('settings_format = 2' in lines and lines.index('settings_format = 2') < lines.index('[video]'),
                      f'{tag}: settings_format is a top-level key, before the first table')
                check(f'fast_boot = {str(fast_boot).lower()}' in lines and f'bios_hle = {str(hle).lower()}' in lines,
                      f'{tag}: the text holds both keys with the arguments\' values')
                got = probe(a.probe, folder, text)
                check(got is not None, f'{tag}: the loader parses the runner\'s file')
                if got is None: continue
                check(got['format'] == '2' and got['echoes'] == '0', f'{tag}: the loader reads format 2 and reports no echo ({got})')
                check(got['has_fast_boot'] == '1' and got['fast_boot'] == str(int(fast_boot)),
                      f'{tag}: fast_boot is applied with the argument\'s value ({got})')
                check(got['has_bios_hle'] == '1' and got['bios_hle'] == str(int(hle)),
                      f'{tag}: bios_hle is applied with the argument\'s value ({got})')
                # Control: what the runner wrote before PS1B-394.
                old = probe(a.probe, folder, text.replace('settings_format = 2\n', '', 1))
                check(old is not None and old['format'] == '0' and old['echoes'] == '1'
                      and old['has_fast_boot'] == '0' and old['has_bios_hle'] == '0',
                      f'{tag}: control: without the format line the loader drops both keys ({old})')
        card_off = settings_text('opengl', False, False, bios, disc, False)
        check('renderer = "opengl"' in card_off and 'enable1 = false' in card_off and '"some folder/SCPH1001.BIN"' in card_off,
              'renderer, card and paths reach the text')
    print('PASS: the runner\'s settings file keeps fast_boot and bios_hle through the loader' if not failures
          else f'{failures} failure(s)')
    return 1 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
