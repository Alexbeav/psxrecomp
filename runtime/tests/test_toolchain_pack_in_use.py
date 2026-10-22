#!/usr/bin/env python3
"""A toolchain pack in use is left alone on Linux and macOS (PS1B-416).

After PS1B-410 both installers (the setup host, host/psxrecomp_codegen_host.c,
and the CLI, tools/toolchain_pack.py) rename an installed pack aside before
they replace or prune it. On Windows a folder with an open file cannot be
renamed, and that was the only thing that kept a pack away from a build using
it. Linux and macOS rename it anyway. Now every program that puts a pack on
PATH holds a shared lock on the pack's folder while it runs, and an installer
that cannot take the exclusive lock at once leaves the pack alone.

Each case starts a program that uses a pack (a holder: the CLI's module after
activate_toolchain_bin, or the setup host after activate_toolchain_path) and
then runs an installer (the CLI's or the host's) on the same cache:

  same tag     an install over the tag the holder uses is refused with the
               sentence for EBUSY, and the pack is byte-for-byte what it was.
  older tag    the prune of an older tag the holder uses keeps it whole.
  holds nothing  the holder has no file of the pack open, only the lock on
               its folder: a build between two compiler runs is covered.
  after        once the holder has ended, the same install and the same
               prune go through.

Every holder and installer pairs with every other, so the CLI and the host
honour each other's hold. Windows is the other half of PS1B-416 and is not
tested here (the line SKIP says so). Everything runs in the closed environment
of toolchain_test_support.py. --dry-run prints it and starts nothing.
--cli-only leaves the host out; the host also needs recomp-ui (RECOMP_UI_ROOT)
and a C compiler (--compiler, CC or the PATH).
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import WINDOWS, make_pack, make_zip, snapshot  # noqa: E402

THIS = Path(__file__).resolve()
EBUSY = 16
OPS = ("same_tag", "older_tag")
HELD_TAG = {"same_tag": "1.0.14", "older_tag": "1.0.13"}


# --- children --------------------------------------------------------------------------------------

def cli_child(case: str) -> int:
    """An installer: the CLI's module installs the zip or prunes the tags older than 1.0.14."""
    tp, sandbox = support.load_cli([])
    cache = Path(os.environ["RETCOMM_TOOLCHAIN_CACHE"])
    raised = ""
    try:
        if case == "older_tag":
            tp.prune_old_toolchain_tags(cache / "1.0.14")
        else:
            tp.install_from_zip(sandbox / "pack.zip", min_version="0")
    except Exception as exc:    # noqa: BLE001: the text is what is checked
        raised = "%s: %s" % (type(exc).__name__, exc)
    print(json.dumps({"raised": raised}))
    return 0


def cli_holder(bin_dir: str) -> int:
    """A holder: the CLI's module puts the pack on PATH, says so, and waits until stdin closes."""
    tp, _ = support.load_cli([])
    tp.activate_toolchain_bin(Path(bin_dir))
    print("bin=%s" % bin_dir)
    print("held=1", flush=True)
    sys.stdin.read()
    return 0


class Holder:
    """A program that uses the pack of `tag` until stop()."""

    def __init__(self, kind: str, probe: Path | None, sandbox: Path, cache: Path, tag: str) -> None:
        env = support.closed_environment(sandbox)
        pack = cache / tag
        if kind == "CLI":
            env["TEST_SANDBOX"] = str(sandbox)
            argv = [sys.executable, str(THIS), "--cli-holder", str(pack / "bin")]
        else:
            env["RETCOMM_TOOLCHAIN_DIR"] = str(pack)    # the lookup finds this pack first
            (sandbox / "project").mkdir(exist_ok=True)
            env["PROBE_PROJECT"] = str(sandbox / "project")
            argv = [str(probe), "hold"]
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8", errors="replace", env=env, cwd=str(sandbox))
        self.said: dict[str, str] = {}
        self.stderr: str | None = None
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            key, _, value = line.rstrip("\n").partition("=")
            self.said[key] = value
            if key == "held":
                break
        self.bin = self.said.get("bin", "")

    def pack_files_open(self, pack: Path) -> list[str] | None:
        """What the holder has open inside the pack, other than the pack's folder. None where it cannot be read."""
        fd_dir = Path("/proc/%d/fd" % self.proc.pid)
        if not fd_dir.is_dir():
            return None
        inside = os.path.realpath(pack)
        found = []
        for fd in fd_dir.iterdir():
            try:
                target = os.readlink(fd)
            except OSError:
                continue
            if target.startswith(inside + os.sep):
                found.append(target)
        return found

    def holds_folder(self, pack: Path) -> bool | None:
        fd_dir = Path("/proc/%d/fd" % self.proc.pid)
        if not fd_dir.is_dir():
            return None
        inside = os.path.realpath(pack)
        for fd in fd_dir.iterdir():
            try:
                if os.readlink(fd) == inside:
                    return True
            except OSError:
                continue
        return False

    def stop(self) -> str:
        """Closes its stdin, waits for it to end, and returns what it wrote to stderr."""
        if self.stderr is None:
            try:
                _, self.stderr = self.proc.communicate(input="", timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                _, self.stderr = self.proc.communicate()
        return self.stderr or ""


# --- one case ----------------------------------------------------------------------------------------

def prepare(op: str, cache: Path, sandbox: Path, stub: Path) -> Path | None:
    if op == "same_tag":
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        return make_zip(sandbox / "pack.zip", stub=stub)
    make_pack(cache / "1.0.13", "1.0.13", marker="old.txt", stub=stub)
    make_pack(cache / "1.0.14", stub=stub)
    return None


def install(kind: str, op: str, probe: Path | None, sandbox: Path, archive: Path | None, cache: Path) -> dict:
    """Runs the installer of `kind`. Returns {done, said, stderr} or {broken}."""
    if kind == "CLI":
        seen = support.run_cli_child(THIS, op, sandbox, False)
        if "broken" in seen:
            return seen
        return {"done": not seen.get("raised"), "said": seen.get("raised", ""), "stderr": seen.get("stderr", "")}
    args = ["prune", str(cache / "1.0.14")] if op == "older_tag" else ["install", str(archive)]
    seen = support.run_probe(probe, args, sandbox, False)
    if "refused" in seen or seen.get("exit") != 0:
        return {"broken": {k: v for k, v in seen.items()}}
    return {"done": seen.get("ret") == "1", "said": seen.get("err", ""), "stderr": seen.get("stderr", "")}


def tag_files(cache: Path, tag: str) -> dict:
    """The tag's entries, without the stamp the host writes through the project's link."""
    return {k: v for k, v in snapshot(cache / tag).items() if k != ".psxrecomp-bin"}


def run_case(holder_kind: str, installer: str, op: str, probe: Path | None, tmp: Path, stub: Path,
             check: support.Checks) -> None:
    name = "%s %s, held by the %s" % (installer, op.replace("_", " "), holder_kind)
    sandbox = tmp / ("%s-%s-%s" % (holder_kind, installer, op))
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    archive = prepare(op, cache, sandbox, stub)
    tag = HELD_TAG[op]
    before = tag_files(cache, tag)
    holder = Holder(holder_kind, probe, sandbox, cache, tag)
    try:
        if os.path.normpath(holder.bin) != os.path.normpath(str(cache / tag / "bin")):
            check(False, "%s: the holder uses the pack of %s" % (name, tag), (holder.said, holder.stop()))
            return
        opened = holder.pack_files_open(cache / tag)
        if opened is not None:
            check(opened == [] and holder.holds_folder(cache / tag),
                  "%s: the holder has no file of the pack open, only its folder (a build between two "
                  "compiler runs)" % name, opened)
        seen = install(installer, op, probe, sandbox, archive, cache)
        if "broken" in seen:
            check(False, "%s: the installer ran" % name, seen)
            return
        names = sorted(p.name for p in cache.iterdir())
        check(tag_files(cache, tag) == before,
              "%s: the pack in use is byte-for-byte what it was" % name,
              {"names": names, "said": seen["said"]})
        check(not [n for n in names if n.startswith(".old-") or n.startswith(".staging")],
              "%s: nothing is left beside the pack" % name, names)
        if op == "same_tag":
            check(not seen["done"] and seen["said"].endswith(support.s3(EBUSY)),
                  "%s: the install is refused with the sentence for a pack in use" % name, seen["said"])
        elif installer == "host":
            check("older toolchain is in use by another program; kept" in seen["stderr"],
                  "%s: the host says the older tag is in use and kept" % name, seen["stderr"][-600:])
    finally:
        holder.stop()

    # The holder has ended: the same operation goes through.
    seen = install(installer, op, probe, sandbox, archive, cache)
    names = sorted(p.name for p in cache.iterdir())
    if op == "same_tag":
        check(seen.get("done") and "new.txt" in tag_files(cache, tag) and "old.txt" not in tag_files(cache, tag),
              "%s: after the holder ended the install replaces the tag whole" % name,
              {"names": names, "said": seen.get("said")})
    else:
        check("1.0.13" not in names and "1.0.14" in names,
              "%s: after the holder ended the prune removes the older tag" % name, names)
    check(not [n for n in names if n.startswith(".old-") or n.startswith(".staging")],
          "%s: after the holder ended nothing is left beside the pack" % name, names)


def host_activates_pack(probe: Path, tmp: Path, stub: Path) -> bool:
    """The host puts a pack on PATH only without native cmake and ninja (PSX-PUB-035)."""
    sandbox = tmp / "host-activates"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", stub=stub)
    holder = Holder("host", probe, sandbox, cache, "1.0.14")
    holder.stop()
    return bool(holder.bin)


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if len(argv) == 2 and argv[0] == "--cli-child":
        return cli_child(argv[1])
    if len(argv) == 2 and argv[0] == "--cli-holder":
        return cli_holder(argv[1])
    if WINDOWS:
        print("SKIP: on Windows a folder with an open file cannot be renamed; this test is the Linux and macOS "
              "half of PS1B-416")
        return 0
    check = support.Checks()
    cc = support.find_compiler(argv)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = support.make_stub(tmp, cc)
        assert stub is not None     # a shell script off Windows
        kinds = ["CLI"]
        probe = None
        if "--cli-only" in argv:
            print("the setup host was not asked for (--cli-only)")
        else:
            probe, why = support.build_probe(tmp, cc)
            if probe is None:
                print(why if why.startswith("FAIL") else "%s; the setup host did not run" % why)
                check.failures += 1 if why.startswith("FAIL") else 0
            elif not host_activates_pack(probe, tmp, stub):
                print("SKIP host: native cmake and ninja are on the closed PATH, so the host uses no pack here")
            else:
                kinds.append("host")
        for holder_kind in kinds:
            for installer in kinds:
                for op in OPS:
                    run_case(holder_kind, installer, op, probe, tmp, stub, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: a toolchain pack in use is neither renamed nor replaced (%s)" % " and ".join(kinds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
