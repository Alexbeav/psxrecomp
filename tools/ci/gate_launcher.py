#!/usr/bin/env python3
"""Gate helpers for the tests that need the recomp-ui launcher checkout (PS1B-485).

check ROOT
    ROOT is the checkout the gate points RECOMP_UI_ROOT at. Refuse (exit 2, reason on
    stderr) unless it holds src/recomp_launcher.h, has no local change, and is at the
    commit that recomp_ui_pin.txt pairs with this tree. Print one record line.
skips LOG [LOG ...] [--must-run NAME ...]
    Read ctest output and print "skipped: N (names)". A skipped test reads like a pass
    in ctest's own summary. With --must-run, exit 1 when a named test did not read
    Passed (skipped, failed, or absent from the logs).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

PIN_FILE = Path(__file__).resolve().parent / "recomp_ui_pin.txt"
RESULT = re.compile(r"Test\s+#\d+:\s+(\S+)\s+\.+\s*(\*\*\*[^\r\n]*?|Passed)\s+[\d.]+ sec")
TAIL = re.compile(r"^\s+\d+ - (\S+) \(Skipped\)\s*$", re.M)


def git(root, *args):
    # GIT_OPTIONAL_LOCKS=0: "git status" must not refresh the index of a checkout that is only being read.
    done = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})
    if done.returncode != 0:
        raise SystemExit("git %s failed in %s: %s" % (" ".join(args), root, done.stderr.strip()))
    return done.stdout.strip()


def pinned(pin_file):
    for line in pin_file.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip().lower()
        if line and not line.startswith("#"):
            if not re.fullmatch(r"[0-9a-f]{12,40}", line):
                raise SystemExit("%s: not a commit id: %r" % (pin_file, line))
            return line
    raise SystemExit("%s: no commit id" % pin_file)


def check(args):
    root = Path(args.root)
    if not (root / "src" / "recomp_launcher.h").is_file():
        raise SystemExit("missing: %s" % (root / "src" / "recomp_launcher.h"))
    if git(root, "status", "--porcelain"):
        raise SystemExit("launcher checkout is not clean: %s" % root)
    head, tree = git(root, "log", "-1", "--format=%H%n%T").splitlines()
    want = pinned(Path(args.pin))
    if not head.startswith(want):
        raise SystemExit("launcher checkout %s is at %s, this tree pairs with %s (tools/ci/recomp_ui_pin.txt)"
                         % (root, head, want))
    print("recomp_ui_root=%s recomp_ui_head=%s recomp_ui_tree=%s" % (root.as_posix(), head, tree))


def results(logs):
    """name -> status, in the order first seen; 'Skipped' wins over any other line for the name."""
    seen = {}
    for log in logs:
        text = Path(log).read_text(encoding="utf-8", errors="replace")
        for name, status in RESULT.findall(text):
            if seen.get(name) != "Skipped":
                seen[name] = "Skipped" if "Skipped" in status else status.strip("* ")
        for name in TAIL.findall(text):
            seen[name] = "Skipped"
    return seen


def skips(args):
    seen = results(args.logs)
    names = [name for name, status in seen.items() if status == "Skipped"]
    print("skipped: %d (%s)" % (len(names), " ".join(names)))
    bad = ["%s (%s)" % (name, seen.get(name, "not in the logs")) for name in args.must_run
           if seen.get(name) != "Passed"]
    if bad:
        print("must run and pass: " + ", ".join(bad), file=sys.stderr)
        return 1
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("check")
    one.add_argument("root")
    one.add_argument("--pin", default=str(PIN_FILE))
    one.set_defaults(run=check)
    two = sub.add_parser("skips")
    two.add_argument("logs", nargs="+")
    two.add_argument("--must-run", nargs="*", default=[], metavar="NAME")
    two.set_defaults(run=skips)
    args = parser.parse_args(argv)
    return args.run(args) or 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except SystemExit as stop:
        if isinstance(stop.code, str):
            print(stop.code, file=sys.stderr)
            sys.exit(2)
        raise
