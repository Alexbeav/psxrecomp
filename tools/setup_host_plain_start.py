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

The program gets its own toolchain and data folders. A setup program holds a
toolchain installer: when its launcher opens it tests the machine's toolchain
pack, and after one failed test it removes or renames that pack; a rebuild
installs a downloaded pack over the installed one. A gate must never give it
the machine's own folders (PS1B-406: a test that did emptied a build host's
toolchain). So every folder the program derives a toolchain or data root from
(LOCALAPPDATA, APPDATA, USERPROFILE, HOME, XDG_DATA_HOME, RETCOMM_DATA_HOME,
RETCOMM_TOOLCHAIN_CACHE, TEMP, TMP) is a folder made for this start and removed
after it, the variables that name a toolchain are taken out, the proxy
variables point at a closed port, and PSXRECOMP_TOOLCHAIN_READONLY=1 is set.
The start fails when the program left a pack or a pointer in those folders,
when it made a toolchain folder in the package, and it is not made at all when
the package's toolchain stamp points outside the package.

What protects the machine here is the folders, not the variable. A setup
program reads PSXRECOMP_TOOLCHAIN_READONLY only from the change of PS1B-410
(the host's read-only switch) on; a program built before it ignores the
variable. Setting it costs nothing for those and closes the last way (a pack
the program finds by another route) for the ones that know it.

  setup_host_plain_start.py --exe <setup program> [--root <its folder>] [--timeout seconds]

Exit 0 pass, 1 fail, 3 the program cannot run on this machine (a package built
for another system). tools/package_setup_host.sh runs it on the staged package.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

PASS_STAMP = 'host:before_run_window'
REFUSED = 'failed to load --game'
CANNOT_RUN = 3

# Each of these becomes a folder of the start. The program's toolchain cache
# bases are derived from them (host/psxrecomp_codegen_host.c,
# collect_toolchain_cache_bases).
CLOSED_ROOTS = (
    ('LOCALAPPDATA', 'localappdata'), ('APPDATA', 'appdata'), ('USERPROFILE', 'home'), ('HOME', 'home'),
    ('XDG_DATA_HOME', 'xdg-data'), ('RETCOMM_DATA_HOME', 'retcomm-data'),
    ('RETCOMM_TOOLCHAIN_CACHE', 'retcomm-toolchain-cache'),
    ('TEMP', 'temp'), ('TMP', 'temp'), ('TMPDIR', 'temp'),
)
# Variables that name a toolchain or ask the program for a build.
TOOLCHAIN_VARIABLES = ('RETCOMM_TOOLCHAIN_DIR', 'PSXRECOMP_TOOLCHAIN_DIR', 'TOOLCHAIN_DIR', 'BPE_TOOLCHAIN_DIR',
                       'CMAKE', 'PYTHON', 'RETCOMM_PYTHON', 'PSXRECOMP_DIAGNOSTIC', 'PSXRECOMP_FORCE_SETUP')
READONLY_SWITCH = 'PSXRECOMP_TOOLCHAIN_READONLY'
DEAD_PROXY = 'http://127.0.0.1:9'
# Where a pack or a pointer would land, below the folders of the start.
TOOLCHAIN_PLACES = (
    'retcomm-toolchain-cache', os.path.join('retcomm-data', 'toolchains'),
    os.path.join('localappdata', 'retcomm'), os.path.join('localappdata', 'psxrecomp'),
    os.path.join('localappdata', 'Packages'),
    os.path.join('xdg-data', 'retcomm'), os.path.join('xdg-data', 'psxrecomp'),
    os.path.join('home', '.local', 'share', 'retcomm'), os.path.join('home', '.local', 'share', 'psxrecomp'),
)


def closed_environment(sandbox):
    """The caller's environment with the program's toolchain and data roots moved into `sandbox`."""
    env = dict(os.environ, PSX_LAUNCHER_BOOT_TIMING='1', PSXRECOMP_NO_FORWARD='1',
               SDL_VIDEO_DRIVER='dummy', SDL_VIDEODRIVER='dummy',
               SDL_AUDIO_DRIVER='dummy', SDL_AUDIODRIVER='dummy')
    env.pop('PSX_HEADLESS', None)           # a headless start is another route
    env.pop('PSX_NO_LAUNCHER', None)
    for name in TOOLCHAIN_VARIABLES:
        env.pop(name, None)
    for name, folder in CLOSED_ROOTS:
        os.makedirs(os.path.join(sandbox, folder), exist_ok=True)
        env[name] = os.path.join(sandbox, folder)
    for name in ('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY'):
        env.pop(name.lower(), None)
        env[name] = DEAD_PROXY
        if os.name != 'nt':                 # Windows names are case-blind
            env[name.lower()] = DEAD_PROXY
    env[READONLY_SWITCH] = '1'
    return env


def toolchain_leftovers(sandbox):
    """What the program left where a toolchain pack or pointer lands. Must be empty."""
    found = []
    made_here = {folder for _name, folder in CLOSED_ROOTS}
    for place in TOOLCHAIN_PLACES:
        top = os.path.join(sandbox, place)
        if not os.path.lexists(top):
            continue
        if place not in made_here:          # the start made the root folders itself, empty
            found.append(place)
        for folder, names, files in os.walk(top):
            found.extend(os.path.relpath(os.path.join(folder, n), sandbox) for n in names + files)
    return found


def stamp_outside(cwd):
    """The line of <cwd>/toolchain/.psxrecomp-bin when it names a folder outside cwd, else ''.
    The program takes its toolchain from that line, so a start could change that folder."""
    stamp = os.path.join(cwd, 'toolchain', '.psxrecomp-bin')
    try:
        with open(stamp, 'r', encoding='utf-8', errors='replace') as stream:
            line = stream.readline().strip()
    except OSError:
        return ''
    if not line:
        return ''
    target = os.path.realpath(line if os.path.isabs(line) else os.path.join(cwd, line))
    inside = os.path.realpath(cwd)
    try:
        return '' if os.path.commonpath([inside, target]) == inside else line
    except ValueError:                      # another drive
        return line


def plain_start(command, cwd, timeout=60.0, pass_stamp=PASS_STAMP, sandbox_parent=None):
    """(verdict, text, stderr lines). verdict: 'pass', 'fail' or 'cannot-run'."""
    outside = stamp_outside(cwd)
    if outside:
        return 'fail', ('the folder has a toolchain stamp that points outside it (%s); the setup program was not '
                        'started, because a start could change that toolchain' % outside), []
    had_toolchain = os.path.lexists(os.path.join(cwd, 'toolchain'))
    sandbox = tempfile.mkdtemp(prefix='plain-start-', dir=sandbox_parent)
    try:
        verdict, text, lines = _start_and_watch(command, cwd, closed_environment(sandbox), timeout, pass_stamp)
        if verdict != 'cannot-run':
            left = toolchain_leftovers(sandbox)
            if not had_toolchain and os.path.lexists(os.path.join(cwd, 'toolchain')):
                left.append('a toolchain folder in the package')
            if left:
                return 'fail', ('the setup program wrote a toolchain pack or pointer during a plain start: %s'
                                % ', '.join(left[:12])), lines
        return verdict, text, lines
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)


def _start_and_watch(command, cwd, env, timeout, pass_stamp):
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
