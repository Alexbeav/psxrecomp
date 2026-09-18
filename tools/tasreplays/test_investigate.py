"""End-to-end controller tests. No game, emulator, compiler or retail input is run."""
import json
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from compare_ram_pages import MAGIC, PAGE_BYTES, PAGE_COUNT, RAM_BYTES, page_hash
from investigate import compare_pair, trace_pair, validate

HERE = Path(__file__).resolve().parent
CONTROLLER = HERE / 'investigate.py'


def fixture(directory, mode, delay=0):
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    with (root / 'invocations.txt').open('a') as stream:
        stream.write('started\n')
    if mode == 'failure':
        sys.exit(7)
    if mode == 'storage':
        (root / 'large.bin').write_bytes(bytes(2000000))
    if mode == 'tree':
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        (root / 'descendant.pid').write_text(str(child.pid))
    time.sleep(float(delay))
    for side in ('source', 'native'):
        target = root / side
        target.mkdir(exist_ok=True)
        rows = []
        for frame in range(1, 5 if mode != 'incomplete' or side == 'source' else 3):
            changed = side == 'native' and frame == 2 and mode in ('mismatch', 'diagnostic', 'malformed')
            first = bytes([1 if changed else 0]) + bytes(PAGE_BYTES - 1)
            hashes = [page_hash(first)] + [page_hash(bytes(PAGE_BYTES))] * (PAGE_COUNT - 1)
            rows.append('\t'.join([str(frame), str(frame * 100), *hashes]))
            if mode == 'diagnostic' and frame == 2:
                (target / 'ram-frame-000002.bin').write_bytes(first + bytes(RAM_BYTES - PAGE_BYTES))
        text = MAGIC + '\n' + '\t'.join(['frame', 'cycle'] + [f'{p*PAGE_BYTES:06X}' for p in range(PAGE_COUNT)]) + '\n' + '\n'.join(rows) + '\n'
        if mode == 'malformed' and side == 'native':
            text += 'bad row\n'
        (target / 'ram-pages.tsv').write_text(text)
        if mode == 'diagnostic':
            (target / 'trace.tsv').write_text('cycle\tpc\tvalue\n1\t80000000\t0\n2\t80000004\t0\n3\t80000008\t' + ('1' if side == 'native' else '0') + '\n')


def recipe(directory, mode='match', delay=0, diagnostics=False):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'input.bin').write_bytes(b'synthetic inputs unchanged')
    (directory / 'profile.json').write_text('{"synthetic":true}')
    def step(name, phase, output, kind, wait=0):
        return dict(id=name, phase=phase,
                    argv=['{python}', str(Path(__file__).resolve()), '--fixture', output, kind, str(wait)],
                    timeout=15, max_bytes=20000000, max_files=100, memory_mib=256)
    data = dict(schema='tas-investigation-v1', expected_returns=4,
                identity=dict(input='{recipe_dir}/input.bin', profile='{recipe_dir}/profile.json',
                              executable=sys.executable, fixture=str(Path(__file__).resolve())),
                comparison=dict(source='{run}/capture/source', native='{run}/capture/native'),
                steps=[step('replay', 'replay', '{run}/capture', mode, delay)])
    if diagnostics:
        data['steps'].extend([step('snapshots', 'snapshots', '{run}/diagnostic', 'diagnostic'),
                              step('trace', 'trace', '{run}/trace', 'diagnostic')])
        data['snapshots'] = dict(source='{run}/diagnostic/source', native='{run}/diagnostic/native')
        data['trace'] = dict(columns=['cycle', 'pc', 'value'], expected_operations=3,
                             **{side: dict(path='{run}/trace/' + side + '/trace.tsv', phase='pre-fetch',
                                           window='return-{first_return}', alignment='synthetic sequence 1')
                                for side in ('source', 'native')})
    path = directory / 'recipe.json'
    path.write_text(json.dumps(data, indent=2))
    return path, data


@unittest.skipUnless(os.name == 'nt', 'supervised execution uses Windows Job Objects')
class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = self.root / 'run'

    def command(self, *args):
        return subprocess.run([sys.executable, str(CONTROLLER), *map(str, args)], capture_output=True, text=True, timeout=30)

    def start(self, mode='match', delay=0, diagnostics=False):
        self.path, self.data = recipe(self.root / 'recipe', mode, delay, diagnostics)
        return self.path

    def save(self):
        self.path.write_text(json.dumps(self.data))

    def state(self):
        return json.loads((self.run / 'progress.json').read_text())

    def wait_for(self, path):
        deadline = time.monotonic() + 10
        while not path.exists():
            if time.monotonic() > deadline:
                self.fail('worker did not reach ' + str(path))
            time.sleep(.05)

    def background(self):
        process = subprocess.Popen([sys.executable, str(CONTROLLER), 'run', str(self.path), str(self.run)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and (process.kill(), process.wait()))
        self.wait_for(self.run / 'attempts/replay-1/started.json')
        return process

    def test_match_and_no_duplicate_completed_run(self):
        self.start()
        outcome = self.command('run', self.path, self.run)
        self.assertEqual(outcome.returncode, 0, outcome.stderr + outcome.stdout)
        self.assertEqual(self.state()['comparison']['exact_matching_prefix'], 4)
        self.assertEqual(self.command('run', self.path, self.run).returncode, 0)
        self.assertEqual((self.run / 'capture/invocations.txt').read_text().count('started'), 1)

    def test_transient_mismatch_escalation_and_reconvergence(self):
        self.start('mismatch', diagnostics=True)
        self.command('run', self.path, self.run)
        state = self.state()
        self.assertEqual(state['status'], 'mismatch')
        self.assertEqual(state['comparison']['exact_matching_prefix'], 1)
        self.assertEqual(state['comparison']['first_divergence']['frame'], 2)
        self.assertEqual(state['comparison']['furthest_execution'], [4, 4])
        self.assertEqual(state['snapshots']['bytes_differ'], 1)
        self.assertEqual(state['trace']['first_difference']['operation'], 3)
        self.assertEqual(state['completed'], ['replay', 'snapshots', 'trace'])

    def test_incomplete(self):
        self.start('incomplete')
        self.command('run', self.path, self.run)
        state = self.state()
        self.assertEqual(state['status'], 'incomplete')
        self.assertEqual(state['comparison']['exact_matching_prefix'], 2)
        self.assertEqual(state['comparison']['first_divergence']['frame'], 3)

    def test_failure_and_timeout(self):
        # The failure case must fail on its own, not on the host clock: a loaded build machine
        # can take longer than 0.3 s just to start Python, which reported host_timeout instead.
        for mode, delay, timeout, expected in [('failure', 0, 120, None), ('match', 2, .3, 'host_timeout')]:
            with self.subTest(mode=mode):
                self.run = self.root / mode
                self.start(mode, delay)
                self.data['steps'][0]['timeout'] = timeout
                self.save()
                self.command('run', self.path, self.run)
                self.assertEqual(self.state()['status'], 'failed')
                result = self.state()['outcomes']['replay-1']
                self.assertEqual(result['stop_reason'], expected)
                self.assertNotEqual(result['exit_code'], 0)

    def test_storage_budget(self):
        self.start('storage', 1)
        self.data['steps'][0]['max_bytes'] = 1000000
        self.save()
        self.command('run', self.path, self.run)
        self.assertEqual(self.state()['outcomes']['replay-1']['stop_reason'], 'host_storage_budget')
        self.assertTrue((self.run / 'capture/large.bin').exists())

    def test_descendants_die_after_timeout(self):
        self.start('tree', 10)
        self.data['steps'][0]['timeout'] = 1
        self.save()
        self.command('run', self.path, self.run)
        pid = int((self.run / 'capture/descendant.pid').read_text())
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x100000, False, pid)
        if handle:
            try:
                self.assertEqual(kernel.WaitForSingleObject(handle, 2000), 0)
            finally:
                kernel.CloseHandle(handle)

    def test_selected_build_and_test_steps(self):
        self.start()
        base = self.data['steps'][0]
        self.data['steps'] = [dict(base, id=phase, phase=phase,
                                  argv=['{python}', '-c', 'print("selected synthetic target")'])
                              for phase in ('build', 'test')] + [base]
        self.save()
        self.assertEqual(self.command('run', self.path, self.run).returncode, 0)
        self.assertEqual(self.state()['completed'], ['build', 'test', 'replay'])

    def test_session_credentials_stay_out_of_steps_and_evidence(self):
        self.start()
        base = self.data['steps'][0]
        probe = self.root / 'child-env.json'
        self.data['steps'] = [dict(base, id='build', phase='build',
                                   argv=['{python}', '-c', 'import json,os,sys;json.dump(dict(os.environ),open(sys.argv[1],"w"))',
                                         str(probe)])] + [base]
        self.save()
        secret = 'sk-ant-test-secret-value'
        env = dict(os.environ, CLAUDE_CODE_SESSION_ACCESS_TOKEN=secret, GH_TOKEN=secret, EXAMPLE_API_KEY=secret,
                   PSX_TAS_KEEP='kept')
        outcome = subprocess.run([sys.executable, str(CONTROLLER), 'run', str(self.path), str(self.run)],
                                 capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(outcome.returncode, 0, outcome.stderr + outcome.stdout)
        child = json.loads(probe.read_text())
        self.assertEqual(child.get('PSX_TAS_KEEP'), 'kept')
        for name in ('CLAUDE_CODE_SESSION_ACCESS_TOKEN', 'GH_TOKEN', 'EXAMPLE_API_KEY'):
            self.assertNotIn(name, child)
        for record in self.run.glob('attempts/*/command.json'):
            self.assertNotIn(secret, record.read_text())

    def test_changed_input_blocks_diagnostics(self):
        self.start('mismatch', 1, diagnostics=True)
        process = self.background()
        self.command('pause', self.run); process.wait(timeout=5)
        (self.path.parent / 'input.bin').write_bytes(b'changed')
        self.command('resume', self.run)
        self.assertEqual(self.state()['status'], 'failed')
        self.assertFalse((self.run / 'attempts/snapshots-1').exists())

    def test_interrupted_recovery_and_lock(self):
        self.start('match', 2)
        process = self.background()
        self.assertNotEqual(self.command('run', self.path, self.run).returncode, 0)
        process.kill(); process.wait()
        self.assertEqual(self.command('run', self.path, self.run).returncode, 0)
        self.assertEqual((self.run / 'capture/invocations.txt').read_text().count('started'), 1)

    def test_pause_resume_and_stop(self):
        self.start('match', 1)
        process = self.background()
        self.command('pause', self.run)
        process.wait(timeout=5)
        self.assertEqual(self.state()['status'], 'paused')
        self.assertEqual(self.command('resume', self.run).returncode, 0)
        self.assertEqual((self.run / 'capture/invocations.txt').read_text().count('started'), 1)
        self.run = self.root / 'stop'
        self.start('match', 10)
        process = self.background()
        self.command('stop', self.run)
        process.wait(timeout=5)
        self.assertEqual(self.state()['status'], 'stopped')
        self.assertNotEqual(self.command('resume', self.run).returncode, 0)

    def test_finite_retries_preserve_failures(self):
        self.start('failure')
        self.data['steps'][0].update(attempts=2)
        self.data['steps'][0]['argv'][3] = '{attempt}/capture'
        self.save()
        self.command('run', self.path, self.run)
        self.assertEqual(len(self.state()['outcomes']), 2)
        self.assertTrue((self.run / 'attempts/replay-1/capture/invocations.txt').exists())
        self.assertTrue((self.run / 'attempts/replay-2/capture/invocations.txt').exists())

    def test_uncertain_launch_never_retried(self):
        self.start()
        self.run.mkdir()
        (self.run / 'controller.lock').touch()
        # Initialize by pausing after worker launch, then retain an expired intent without receipt.
        self.data['steps'][0]['argv'][-1] = '1'
        self.save()
        process = self.background()
        self.command('pause', self.run); process.wait(timeout=5)
        self.wait_for(self.run / 'attempts/replay-1/result.json')
        time.sleep(.2)
        (self.run / 'attempts/replay-1/result.json').unlink()
        path = self.run / 'attempts/replay-1/command.json'
        spec = json.loads(path.read_text()); spec['launched'] = 0
        path.write_text(json.dumps(spec))
        self.command('resume', self.run)
        self.assertEqual(self.state()['status'], 'uncertain')
        self.assertEqual((self.run / 'capture/invocations.txt').read_text().count('started'), 1)


class EvidenceTests(unittest.TestCase):
    def test_extra_and_both_missing_returns(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture(directory, 'match')
            pair = dict(source=directory + '/source', native=directory + '/native')
            extra = compare_pair(pair, 3)
            self.assertFalse(extra['match'])
            self.assertEqual(extra['first_divergence'], dict(kind='unexpected_return', frame=4))
            self.assertEqual(extra['exact_matching_prefix'], 3)
            missing = compare_pair(pair, 5)
            self.assertFalse(missing['match'])
            self.assertEqual(missing['first_divergence'], dict(kind='missing_return', frame=5, missing_side='both'))

    def test_invalid_recipe_budgets_and_order(self):
        with tempfile.TemporaryDirectory() as directory:
            _, data = recipe(directory)
            for value in (0, -1, float('inf'), float('nan'), True):
                data['steps'][0]['timeout'] = value
                with self.assertRaises(ValueError):
                    validate(data)

    def test_malformed_tail_preserves_earlier_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture(directory, 'malformed')
            result = compare_pair(dict(source=directory + '/source', native=directory + '/native'), 4)
            self.assertFalse(result['match'])
            self.assertEqual(result['first_divergence']['frame'], 2)
            self.assertEqual(result['exact_matching_prefix'], 1)
            self.assertTrue(result['errors'])

    def test_phase_mismatch_is_unavailable(self):
        result = trace_pair(dict(source=dict(phase='before'), native=dict(phase='after')))
        self.assertEqual(result['status'], 'unavailable')
        self.assertIsNone(result['first_difference'])


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        fixture(*sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == '--example':
        path, _ = recipe(sys.argv[2], 'mismatch', diagnostics=True)
        print(path)
    else:
        unittest.main()
