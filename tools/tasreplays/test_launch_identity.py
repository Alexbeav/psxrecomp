"""Binary identity, shared run flags and the harness stop path, without retail assets."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
from launch_identity import resolve_binary, receipt_fields, add_launch_arguments, check_launch_arguments, run_native_arguments
from process_budget import wait_budgeted
from stream_compare import write_stop_request

HERE = Path(__file__).resolve().parent


def rejects(call, text=None):
    try: call()
    except ValueError as error:
        if text and text not in str(error): raise AssertionError(f'{text!r} not in {error}')
        return
    raise AssertionError('invalid launch admitted')


with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
    root = Path(directory)
    setup_exe = root/'setup.exe'; setup_exe.write_bytes(b'setup build')
    setup_sha = hashlib.sha256(b'setup build').hexdigest()
    other = root/'other.exe'; other.write_bytes(b'instrumented build')
    other_sha = hashlib.sha256(b'instrumented build').hexdigest()
    stale = root/'stale.exe'; stale.write_bytes(b'stale build')
    stale_sha = hashlib.sha256(b'stale build').hexdigest()
    # The setup binary qualifies with or without a declared diagnostic hash.
    binary = resolve_binary(str(setup_exe), setup_sha)
    assert binary == {'path': str(setup_exe.resolve()), 'setup_executable_sha256': setup_sha, 'binary_sha256': setup_sha,
                      'binary_matches_setup': True, 'diagnostic_binary': None}
    assert resolve_binary(str(setup_exe), setup_sha, None, other_sha)['binary_matches_setup']
    assert receipt_fields(binary) == {'setup_executable_sha256': setup_sha, 'binary_sha256': setup_sha,
                                      'binary_matches_setup': True, 'diagnostic_binary': None}
    # An override must be declared; the declaration must name the bytes that actually run.
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, other), 'binary differs from the setup receipt')
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, other), other_sha)
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, other), setup_sha)
    binary = resolve_binary(str(setup_exe), setup_sha, other, other_sha.upper())
    assert binary['binary_matches_setup'] is False and binary['binary_sha256'] == other_sha and binary['diagnostic_binary'] == other_sha
    assert binary['path'] == str(other.resolve())
    # 09-14 failure mode: the path still holds a different binary than the one declared.
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, stale, other_sha), stale_sha)
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, stale, other_sha), other_sha)
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha, stale, other_sha), setup_sha)
    # A rebuilt setup path without an override is caught the same way.
    setup_exe.write_bytes(b'rebuilt in place')
    rejects(lambda: resolve_binary(str(setup_exe), setup_sha), hashlib.sha256(b'rebuilt in place').hexdigest())
    assert resolve_binary(str(setup_exe), setup_sha, None, hashlib.sha256(b'rebuilt in place').hexdigest())['binary_matches_setup'] is False
    setup_exe.write_bytes(b'setup build')
    for bad in ['abc', other_sha[:-1], other_sha+'0', 'g'*64]:
        rejects(lambda: resolve_binary(str(setup_exe), setup_sha, other, bad))
    rejects(lambda: resolve_binary(str(setup_exe), 'not-a-hash'))
    try: resolve_binary(str(root/'missing.exe'), setup_sha); raise AssertionError('missing binary admitted')
    except OSError: pass
    # Shared flags: the exact spellings the trace tool uses, and the run_native pass-through.
    parser = add_launch_arguments(argparse.ArgumentParser())
    args = parser.parse_args(['--returns', '6000', '--exe', str(other), '--diagnostic-binary', other_sha,
                              '--cpu-boundary-window', '100', '200'])
    check_launch_arguments(args)
    assert (args.returns, args.exe, args.diagnostic_binary, args.cpu_boundary_window) == (6000, other, other_sha, [100, 200])
    assert args.ladder is None and args.stop_on_divergence is False
    assert run_native_arguments(args, binary) == ['--expected-exe-sha256', other_sha, '--cpu-boundary-window', '100', '200']
    plain = parser.parse_args(['--stop-on-divergence', '--ladder', '6000,full'])
    check_launch_arguments(plain)
    assert plain.stop_on_divergence and run_native_arguments(plain, binary) == ['--expected-exe-sha256', other_sha]
    rejects(lambda: check_launch_arguments(parser.parse_args(['--returns', '5', '--ladder', 'full'])), 'mutually exclusive')
    rejects(lambda: check_launch_arguments(parser.parse_args(['--diagnostic-binary', 'xyz'])))
    # wait_budgeted: a string reason is recorded verbatim; bare True keeps operator_stop; falsy never stops.
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    sleeper = lambda: subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(20)'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
    p = sleeper(); result = wait_budgeted(p, root, 10, interval=0.02, stop_requested=lambda: 'harness_stop')
    assert result['stop_reason'] == 'harness_stop' and p.poll() is not None
    p = sleeper(); assert wait_budgeted(p, root, 10, interval=0.02, stop_requested=lambda: True)['stop_reason'] == 'operator_stop'
    p = subprocess.Popen([sys.executable, '-c', 'pass'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
    assert wait_budgeted(p, root, 10, interval=0.02, stop_requested=lambda: None)['stop_reason'] is None
    # run_native refuses a staged executable whose hash differs from --expected-exe-sha256 before launching.
    for name in ('game.toml', 'disc.cue', 'bios.bin'): (root/name).write_bytes(b'fixture')
    route = root/'route.psxrti'
    route.write_bytes(struct.pack('<8sIIII', b'PSXRTI1\0', 1, 8, 2, 0)+struct.pack('<IHHIHH', 1, 0xffff, 0, 2, 0xffff, 0))
    # The batch stand-in runs ping as a grandchild; it outlives terminate() and is waited for below.
    fake = root/'fake.cmd'; fake.write_bytes(b'@echo off\r\nping -n 8 127.0.0.1 >nul\r\n')
    fake_sha = hashlib.sha256(fake.read_bytes()).hexdigest()
    launcher = lambda run, *extra: [sys.executable, str(HERE/'run_native.py'), str(run), '--exe', str(fake), '--game', str(root/'game.toml'),
                                    '--route', str(route), '--disc', str(root/'disc.cue'), '--bios', str(root/'bios.bin'), '--timeout', '30', *extra]
    refused = subprocess.run(launcher(root/'refused', '--expected-exe-sha256', other_sha), capture_output=True, text=True, cwd=HERE)
    assert refused.returncode != 0 and fake_sha in refused.stderr and other_sha in refused.stderr
    assert (root/'refused'/fake.name).exists() and not (root/'refused'/'manifest.json').exists() and not (root/'refused'/'process.json').exists()
    refused = subprocess.run(launcher(root/'refused-format', '--expected-exe-sha256', 'zz'), capture_output=True, text=True, cwd=HERE)
    assert refused.returncode != 0 and not (root/'refused-format').exists()
    if os.name == 'nt':
        # End to end: the runner stages the binary, records its hash, and honours <run>/stop-request.json.
        run = root/'stopped'
        process = subprocess.Popen(launcher(run, '--expected-exe-sha256', fake_sha.upper()), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=HERE)
        deadline = time.monotonic()+20
        while not (run/'process.json').exists() and time.monotonic() < deadline: time.sleep(0.05)
        assert (run/'process.json').exists(), process.communicate()
        payload = {'reason': 'divergence', 'frame': 12, 'requested_by': 'test'}
        write_stop_request(run, payload)
        out, err = process.communicate(timeout=60)
        assert process.returncode == 1, (out, err)
        exit_record = json.loads((run/'exit.json').read_text())
        assert exit_record['stop_reason'] == 'harness_stop' and exit_record['stop_request'] == payload and not exit_record['timed_out']
        manifest = json.loads((run/'manifest.json').read_text())
        assert manifest['staged_executable'] == {'path': str(run/fake.name), 'sha256': fake_sha}
        assert manifest['expected_exe_sha256'] == fake_sha and manifest['inputs']['exe']['sha256'] == fake_sha
        assert json.loads(out.splitlines()[-1])['input_playback_complete'] is False
        deadline = time.monotonic()+30
        while time.monotonic() < deadline:
            try: (run/'stderr.log').unlink(); break
            except PermissionError: time.sleep(0.2)
print('Launch identity: setup/diagnostic binary resolution, shared flags, harness stop reason and staged executable binding pass')
