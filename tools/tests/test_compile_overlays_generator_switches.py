"""compile_overlays.py refuses generator switches the overlay cache key lacks.

PS1B-517. psxrecomp-game reads PSX_CODEGEN_CYCLE_PER_INSN, PSX_EMIT_PROD and
PSX_CPS from its environment while it emits an overlay unit
(recompiler/src/code_generator.cpp), and compile_overlays.py passes its own
environment on to it. The cache folder name
cg<ver>_<emitter hash>_gc<config hash>_f<flavor> reads none of them, so a unit
made under a non-default value lands in the folder a default runtime loads.

1. Premise: PSX_CODEGEN_CYCLE_PER_INSN=0 and PSX_CPS=0 each change the C the
   recompiler emits for one fixture function. If a switch stops changing the
   output, revisit GENERATOR_OUTPUT_SWITCHES in compile_overlays.py.
2. Control: a default compile of the fixture writes a unit.
3. Measurement: the same compile with each switch at '0' exits non-zero,
   names the switch, and leaves no unit and no cache folder. On stage
   614126d it exits 0 and writes a unit into the folder of case 2.
4. --cps pins PSX_CPS=1 for every spawn, so PSX_CPS=0 with --cps compiles.
5. The check itself: only a value that starts with '0' is refused.

Usage: test_compile_overlays_generator_switches.py --recompiler <psxrecomp-game>
       --gcc <C compiler>
"""
import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
RECOMPILER = None
GCC = None

SWITCHES = ('PSX_CODEGEN_CYCLE_PER_INSN', 'PSX_EMIT_PROD', 'PSX_CPS')
# The runtime injects these into an autocompile; a manual run must not see
# a stray value from the shell that runs the test.
INJECTED = ('PSX_OVERLAY_CACHE_DIR', 'PSX_OVERLAY_CAPTURES',
            'PSX_OVERLAY_FLAVOR')

LOAD = 0x80010000
FIXTURE = (
    0x24020001,  # addiu v0, zero, 1
    0x24430002,  # addiu v1, v0, 2
    0x00431021,  # addu  v0, v0, v1
    0x03E00008,  # jr    ra
    0x00000000,  # nop
)


def fixture_bytes():
    return b''.join(struct.pack('<I', word) for word in FIXTURE)


def base_env():
    env = {key: value for key, value in os.environ.items()
           if key.upper() not in SWITCHES + INJECTED}
    # APPEND, never prepend: see test_aot_overlay_discovery.py. A second
    # toolchain first on PATH shadows the recompiler's C++ runtime.
    env['PATH'] = env.get('PATH', '') + os.pathsep + os.path.dirname(GCC)
    return env


def units(cache_root):
    """Relative paths of every unit and manifest under a cache root."""
    if not cache_root.exists():
        return []
    return sorted(str(path.relative_to(cache_root))
                  for path in cache_root.rglob('*')
                  if path.is_file() and
                  path.suffix in ('.dll', '.so', '.dylib', '.ranges'))


def load_module():
    spec = importlib.util.spec_from_file_location(
        'compile_overlays_switches', TOOLS / 'compile_overlays.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GeneratorSwitchTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def generate(self, out_name, extra_env):
        """Run the recompiler on the fixture; return the emitted C."""
        psx = self.tmp / 'region.psx'
        if not psx.exists():
            header = bytearray(2048)
            header[0:8] = b'PS-X EXE'
            data = fixture_bytes()
            struct.pack_into('<I', header, 0x10, LOAD)
            struct.pack_into('<I', header, 0x18, LOAD)
            struct.pack_into('<I', header, 0x1C, len(data))
            psx.write_bytes(bytes(header) + data)
            (self.tmp / 'seeds.txt').write_text(
                f'dispatch_root 0x{LOAD:08X}\n', encoding='ascii')
        out = self.tmp / out_name
        out.mkdir()
        env = base_env()
        env.update(extra_env)
        proc = subprocess.run(
            [RECOMPILER, str(psx), '--seeds', str(self.tmp / 'seeds.txt'),
             '--out-dir', str(out), '--overlay'],
            cwd=str(ROOT), env=env, capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return ''.join(path.read_text(encoding='utf-8')
                       for path in sorted(out.glob('*.c')))

    def compile_overlays(self, name, extra_env, cps=False):
        """Run compile_overlays.py on the fixture capture."""
        work = self.tmp / name
        work.mkdir()
        data = fixture_bytes()
        pc = f'0x{LOAD:08X}'
        capture = {
            'schema': 'psxrecomp overlay capture v2',
            'load_addr': pc,
            'size': len(data),
            'bytes_b64': base64.b64encode(data).decode('ascii'),
            'executed_pcs': [pc],
            'dispatch_entry_pcs': [pc],
            'function_entry_pcs': [pc],
            'seeds': [pc],
        }
        captures = work / 'captures.json'
        captures.write_text(json.dumps([capture]), encoding='utf-8')
        cache = work / 'cache'
        command = [
            sys.executable, '-B', str(TOOLS / 'compile_overlays.py'),
            '--captures', str(captures),
            '--game-toml', str(TOOLS / 'cycle_testrom' / 'game.toml'),
            '--recompiler', RECOMPILER,
            '--runtime-include', str(ROOT / 'runtime' / 'include'),
            '--gcc', GCC, '--out-dir', str(cache), '--jobs', '1',
        ]
        if cps:
            command.append('--cps')
        env = base_env()
        env.update(extra_env)
        proc = subprocess.run(
            command, cwd=str(ROOT), env=env, capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=300)
        return proc, cache

    def test_switches_change_generated_c(self):
        default = self.generate('out-default', {})
        self.assertIn('func_', default)
        for name in ('PSX_CODEGEN_CYCLE_PER_INSN', 'PSX_CPS'):
            with self.subTest(switch=name):
                other = self.generate('out-' + name, {name: '0'})
                self.assertNotEqual(
                    default, other,
                    f'{name}=0 no longer changes the overlay C; revisit '
                    'GENERATOR_OUTPUT_SWITCHES')

    def test_compile_refuses_switch_outside_cache_key(self):
        control, control_cache = self.compile_overlays('default', {})
        detail = control.stdout[-2000:] + control.stderr[-2000:]
        self.assertEqual(control.returncode, 0, detail)
        control_units = units(control_cache)
        self.assertTrue(control_units, 'default compile wrote no unit\n'
                        + detail)
        control_folders = {str(Path(unit).parent) for unit in control_units}
        for name in SWITCHES:
            with self.subTest(switch=name):
                proc, cache = self.compile_overlays('off-' + name,
                                                    {name: '0'})
                written = units(cache)
                folders = {str(Path(unit).parent) for unit in written}
                detail = proc.stdout[-2000:] + proc.stderr[-2000:]
                self.assertEqual(
                    written, [],
                    f'{name}=0 wrote {written} (exit {proc.returncode}); '
                    f'same folder as the default compile: '
                    f'{bool(folders & control_folders)}\n{detail}')
                self.assertNotEqual(proc.returncode, 0, detail)
                self.assertIn('refusing', detail)
                self.assertIn(f'{name}=0', detail)
                self.assertFalse(cache.exists(),
                                 'the refusal came after the cache folder')

    def test_cps_flag_pins_psx_cps(self):
        proc, cache = self.compile_overlays('cps-pinned', {'PSX_CPS': '0'},
                                            cps=True)
        detail = proc.stdout[-2000:] + proc.stderr[-2000:]
        self.assertEqual(proc.returncode, 0, detail)
        self.assertTrue(units(cache), detail)

    def test_only_a_leading_zero_is_refused(self):
        module = load_module()

        class Args:
            cps = False

        for env in ({}, {'PSX_CODEGEN_CYCLE_PER_INSN': '1'},
                    {'PSX_CODEGEN_CYCLE_PER_INSN': ''},
                    {'PSX_EMIT_PROD': '1'}, {'PSX_CPS': '1'}):
            with self.subTest(env=env):
                module.refuse_generator_output_switches(Args, env)
        for env in ({'PSX_CODEGEN_CYCLE_PER_INSN': '0'},
                    {'PSX_EMIT_PROD': '00'}, {'PSX_CPS': '0'}):
            with self.subTest(env=env):
                with self.assertRaises(SystemExit):
                    module.refuse_generator_output_switches(Args, env)
        Args.cps = True
        module.refuse_generator_output_switches(Args, {'PSX_CPS': '0'})
        with self.assertRaises(SystemExit):
            module.refuse_generator_output_switches(
                Args, {'PSX_CODEGEN_CYCLE_PER_INSN': '0'})


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler', required=True,
                    help='psxrecomp-game binary')
    ap.add_argument('--gcc', required=True,
                    help='C compiler compile_overlays builds units with')
    args, rest = ap.parse_known_args()
    if not Path(args.recompiler).is_file():
        sys.exit('no recompiler at %s' % args.recompiler)
    if not Path(args.gcc).is_file():
        sys.exit('no C compiler at %s' % args.gcc)
    RECOMPILER = os.path.abspath(args.recompiler)
    GCC = os.path.abspath(args.gcc)
    unittest.main(argv=[sys.argv[0]] + rest)
