#!/usr/bin/env python3
"""Start a setup program the way a double-click does, and say whether it reaches its launcher.

A setup program that cannot read its own config prints one line to stderr and
exits before any window exists. A double-click shows nothing. The Resident
Evil 2 set package did exactly that on its first hands-on start: its set.toml
lacked what the config loader insisted on (PS1B-365). Every automated check
before it had used `--setup-selfcheck`, which answers before the config is
read, or had run the setup pass as commands. Nothing had started the program.

This starts it plainly, with no arguments, in its own folder, and watches its
stderr:

  pass   it prints the boot stamp `host:before_run_window`: the config is read,
         SDL is up, the setup checks are done, the launcher is next;
  fail   it prints "failed to load --game", or exits, or stays silent past the
         time limit, before that stamp.

No window opens. SDL gets its dummy video and audio drivers, and the program
is stopped the moment the verdict is known, which is before the launcher tries
to open. `PSXRECOMP_NO_FORWARD=1` keeps a folder that is already set up from
starting its game instead. The program is stopped, not asked to quit, so it
writes no settings into its folder on a pass.

  setup_host_plain_start.py --exe <setup program> [--root <its folder>] [--timeout seconds]

Exit 0 pass, 1 fail, 3 the program cannot run on this machine (a package built
for another system). tools/package_setup_host.sh runs it on the staged package.
"""
import argparse
import os
import subprocess
import sys
import threading
import time

PASS_STAMP = 'host:before_run_window'
REFUSED = 'failed to load --game'
CANNOT_RUN = 3


def plain_start(command, cwd, timeout=60.0, pass_stamp=PASS_STAMP):
    """(verdict, text, stderr lines). verdict: 'pass', 'fail' or 'cannot-run'."""
    env = dict(os.environ, PSX_LAUNCHER_BOOT_TIMING='1', PSXRECOMP_NO_FORWARD='1',
               SDL_VIDEO_DRIVER='dummy', SDL_VIDEODRIVER='dummy',
               SDL_AUDIO_DRIVER='dummy', SDL_AUDIODRIVER='dummy')
    env.pop('PSX_HEADLESS', None)           # a headless start is another route
    env.pop('PSX_NO_LAUNCHER', None)
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    started = time.monotonic()
    try:
        process = subprocess.Popen(list(command), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                   creationflags=flags)
    except OSError as exc:
        return 'cannot-run', 'the setup program cannot be started on this machine (%s)' % exc, []

    lines = []
    seen = threading.Event()                # a verdict line arrived, or stderr closed
    found = {}

    def read():
        for raw in iter(process.stderr.readline, b''):
            line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
            lines.append(line)
            if 'verdict' not in found:
                if REFUSED in line:
                    found['verdict'] = 'refused'
                    seen.set()
                elif pass_stamp in line:
                    found['verdict'] = 'stamp'
                    found['ms'] = int((time.monotonic() - started) * 1000)
                    seen.set()
        seen.set()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    got_line = seen.wait(timeout)
    verdict = found.get('verdict')
    exited = process.poll()
    if exited is None and got_line and verdict is None:
        # stderr closed without a verdict: the program is on its way out
        try:
            exited = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            exited = None
    if exited is None:
        process.kill()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    reader.join(timeout=5)
    if not reader.is_alive():
        process.stderr.close()

    if verdict == 'stamp':
        return 'pass', 'reached %s after %d ms' % (pass_stamp, found['ms']), lines
    if verdict == 'refused':
        return 'fail', 'the setup program refused its own config and would exit before any window', lines
    if exited is not None:
        return 'fail', 'the setup program exited with code %s before %s' % (exited, pass_stamp), lines
    return 'fail', 'no %s within %d s' % (pass_stamp, int(timeout)), lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--exe', required=True, help='the setup program')
    parser.add_argument('--root', default='', help='its folder (default: the folder of --exe)')
    parser.add_argument('--timeout', type=float, default=60.0)
    args = parser.parse_args(argv)
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        print('setup host plain start: FAIL: no such file: %s' % exe)
        return 1
    root = os.path.abspath(args.root) if args.root else os.path.dirname(exe)
    verdict, text, lines = plain_start([exe], root, timeout=args.timeout)
    if verdict == 'pass':
        print('setup host plain start: PASS (%s; %s)' % (text, exe))
        return 0
    if verdict == 'cannot-run':
        print('setup host plain start: NOT CHECKED: %s; %s' % (text, exe))
        return CANNOT_RUN
    print('setup host plain start: FAIL: %s (%s)' % (text, exe))
    for line in lines[-20:]:
        print('  ' + line)
    return 1


if __name__ == '__main__':
    sys.exit(main())
