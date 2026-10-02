#!/usr/bin/env python3
"""What a player reads when the toolchain check or an install fails (PS1B-410, item A13).

Three sentences, each saying only what the program knows:

  S1  "The portable toolchain did not pass its check: <step>. It was not removed.
       Download the latest pack to replace it."                  (setup host)
  S2  "The new toolchain pack did not pass its check: <step>. The installed
       toolchain was not changed."                               (setup host, CLI)
  S3  the installed folder cannot be renamed aside, by the system's error:
      Windows error 32, Windows error 5, any other error with its number.
                                                                 (setup host, CLI)

<step> is one of five: cmake does not run / clang or the linker is missing from
the pack / clang does not run / a test file could not be written to the
temporary folder / a one-line test program did not compile and link.

Every step and every form is reached here by code that runs, and the whole
sentence is compared:

  the five steps   a made-up pack for each, checked by the setup host (a probe
                   that includes its source file) and by the CLI's module.
  S2               a zip whose pack has cmake and no compiler, installed.
  S3, by number    the host's sentence for 32, 5 and 13; the CLI's install with
                   a rename that fails with each number (the failure is put in
                   by the test).
  what is printed  the CLI itself (psxrecomp_cli.py ensure-toolchain, with and
                   without --json-progress) and the rebuild's toolchain step
                   print the sentence alone: no exception name, no label in
                   front, exit code 1. The setup window shows that line.
                   The rebuild's step is run on Windows only: elsewhere a
                   rebuild takes the native tools first and does not call
                   the installer.
  S3, by the       Windows only: the installed folder held by a handle on the
  system           folder (error 32) and by an open file in it (error 5), for
                   the host and for the CLI. Linux and macOS have no way to make
                   the rename fail without making the unpacking fail first.

Everything runs in the closed environment of toolchain_test_support.py.
--dry-run prints it and starts nothing. --cli-only leaves the host layer out.
The steps past the first need programs that run: shell scripts on Linux and
macOS, compiled ones on Windows (--compiler, CC or the PATH). Without them the
test says so and is skipped; the host layer also needs recomp-ui
(RECOMP_UI_ROOT).
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import EXE, STEPS, WINDOWS, make_pack, make_zip, s1, s2, s3  # noqa: E402

THIS = Path(__file__).resolve()
SKIPPED = 77
NUMBERS = (32, 5, 13)


def make_stubs(tmp: Path, cc: str | None) -> dict | None:
    """Three programs: one that exits 0, one that exits 1, one that exits 0 for --version only."""
    ok = support.make_stub(tmp, cc)
    if ok is None:
        return None
    if not WINDOWS:
        fail, version = tmp / "stub-fail", tmp / "stub-version"
        fail.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        version.write_text('#!/bin/sh\n[ "$1" = "--version" ] && exit 0\nexit 1\n', encoding="utf-8")
        fail.chmod(0o755)
        version.chmod(0o755)
        return {"ok": ok, "fail": fail, "version": version}
    made = {"ok": ok}
    for name, body in (("fail", "int main(void) { return 1; }\n"),
                       ("version", "#include <string.h>\nint main(int c, char** v) "
                                   "{ return (c > 1 && strcmp(v[1], \"--version\") == 0) ? 0 : 1; }\n")):
        source, stub = tmp / ("stub-%s.c" % name), tmp / ("stub-%s.exe" % name)
        source.write_text(body, encoding="utf-8")
        build = subprocess.run([cc, "-o", str(stub), str(source)], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
        if build.returncode != 0 or not stub.is_file():
            return None
        made[name] = stub
    return made


def pack_for_step(root: Path, step: int, stubs: dict) -> Path:
    """A pack that fails its check at STEPS[step]. Step 3 also needs a temporary folder that is not there."""
    if step == 0:
        return make_pack(root)                                   # its cmake is not a program
    if step == 1:
        return make_pack(root, stub=stubs["ok"], tools=("cmake",))
    make_pack(root, stub=stubs["ok"])
    if step == 2:
        shutil.copy2(stubs["fail"], root / "bin" / ("clang" + EXE))
    if step == 4:
        shutil.copy2(stubs["version"], root / "bin" / ("clang" + EXE))
    return root


def no_temp(sandbox: Path) -> dict:
    gone = str(sandbox / "temp" / "not-there")
    return {"TEMP": gone, "TMP": gone, "TMPDIR": gone}


class FolderHandle:
    """Windows: a handle on a folder that does not share delete, so a rename of the folder fails with error 32."""

    def __init__(self, path: Path) -> None:
        import ctypes
        from ctypes import wintypes
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.CreateFileW.restype = wintypes.HANDLE
        self.k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.k.CloseHandle.argtypes = [wintypes.HANDLE]
        # read access, share read and write, open existing, backup semantics (a folder)
        self.handle = self.k.CreateFileW(str(path), 0x80000000, 0x1 | 0x2, None, 3, 0x02000000, None)
        self.ok = self.handle not in (None, wintypes.HANDLE(-1).value)

    def close(self) -> None:
        if self.ok:
            self.k.CloseHandle(self.handle)
            self.ok = False


def hold(kind: str, tag: Path):
    """Holds the installed folder the way `kind` says; returns something with close()."""
    if kind == "folder":
        return FolderHandle(tag)
    return open(tag / "old.txt", "rb")


HELD = (("folder", 32), ("file", 5))       # what is held, and the error Windows gives for the rename


# --- the CLI's module -----------------------------------------------------------------------------

def cli_child(case: str) -> int:
    calls: list[str] = []
    tp, sandbox = support.load_cli(calls)
    out: dict = {}
    if case == "rebuild":
        # The toolchain step of a rebuild, with the lines it gives the progress reporter.
        sys.path.insert(0, str(support.ROOT))
        import psxrecomp_cli as cli

        class Lines:
            def __init__(self):
                self.lines = []

            def log(self, message, **_kwargs):
                self.lines.append(str(message))
        lines = Lines()
        ok = cli.ensure_toolchain_for_rebuild(sandbox / "project", lines, from_zip=str(sandbox / "pack.zip"),
                                              download=False, min_version="0")
        print(json.dumps({"ok": bool(ok), "lines": lines.lines}))
        return 0
    if case == "steps":
        found = []
        for step in range(len(STEPS)):
            if step == 3:
                tp.tempfile.tempdir = no_temp(sandbox)["TMPDIR"]
            found.append(tp.toolchain_bin_check(sandbox / "packs" / str(step) / "bin"))
            tp.tempfile.tempdir = None
        out["steps"] = found
    else:
        if case.startswith("rename-"):
            number = int(case.split("-", 1)[1])
            real = os.rename

            def failing(src, dst, *args, **kwargs):
                if os.path.basename(str(dst)).startswith(".old-"):
                    if WINDOWS:
                        raise OSError(13, "put in by the test", str(src), number)      # winerror
                    raise OSError(number, "put in by the test", str(src))
                return real(src, dst, *args, **kwargs)
            tp.os.rename = failing
        raised = ""
        try:
            tp.install_from_zip(sandbox / "pack.zip", min_version="0")
        except Exception as exc:    # noqa: BLE001: the text is what is checked
            raised = str(exc)
            out["kind"] = type(exc).__name__
        out["raised"] = raised
    print(json.dumps(out))
    return 0


def cli_layer(tmp: Path, stubs: dict, check: support.Checks) -> None:
    sandbox = tmp / "cli-steps"
    support.closed_environment(sandbox)
    for step in range(len(STEPS)):
        pack_for_step(sandbox / "packs" / str(step), step, stubs)
    seen = support.run_cli_child(THIS, "steps", sandbox, False)
    for step, text in enumerate(STEPS):
        got = (seen.get("steps") or [None] * len(STEPS))[step]
        check(got == text, "CLI: the check of a pack names the step \"%s\"" % text, (got, seen.get("broken", "")))

    sandbox = tmp / "cli-s2"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    make_zip(sandbox / "pack.zip", stub=stubs["ok"], tools=("cmake",))
    seen = support.run_cli_child(THIS, "s2", sandbox, False)
    check(seen.get("raised") == s2(STEPS[1]) and seen.get("kind") == "ToolchainRefused",
          "CLI: the sentence for a new pack that fails its check, as the whole text of the refusal", seen)

    # What is printed: the CLI as a program, and the toolchain step of a rebuild.
    sandbox = tmp / "cli-printed"
    env = support.closed_environment(sandbox)
    (sandbox / "project").mkdir(exist_ok=True)
    make_zip(sandbox / "pack.zip", stub=stubs["ok"], tools=("cmake",))
    sentence = s2(STEPS[1])
    for mode in (["--json-progress"], []):
        run = subprocess.run([sys.executable, str(support.ROOT / "psxrecomp_cli.py"), "ensure-toolchain",
                              "--project-root", str(sandbox / "project"), "--from-zip", str(sandbox / "pack.zip")] + mode,
                             capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=str(sandbox))
        printed = run.stdout + run.stderr
        name = "CLI ensure-toolchain %s" % (mode[0] if mode else "(plain)")
        if mode:
            messages = []
            for line in printed.splitlines():
                if line.startswith("{"):
                    try:
                        row = json.loads(line)
                    except ValueError:
                        continue
                    if row.get("event") == "error":
                        messages.append(row.get("message"))
            alone = messages == [sentence]
        else:
            alone = sentence in [line.strip() for line in printed.splitlines()]
        check(run.returncode == 1 and alone, "%s: the sentence alone, exit code 1" % name, (run.returncode, printed[-600:]))
        check(not any(word in printed for word in ("RuntimeError", "ToolchainRefused", "Traceback")),
              "%s: no exception name and no traceback in what is printed" % name, printed[-600:])
    if WINDOWS:
        seen = support.run_cli_child(THIS, "rebuild", sandbox, False)
        check(seen.get("ok") is False and seen.get("lines") and seen["lines"][-1] == sentence,
              "CLI rebuild (Windows): the toolchain step gives the progress line the sentence alone", seen)
    else:
        # On Linux and macOS a rebuild takes the native cmake, ninja and compilers when they are on PATH
        # and does not call the installer, so the sentence cannot arise from that step on such a host.
        print("not run here: the rebuild's toolchain step reaches the installer on Windows only "
              "(elsewhere it uses the native tools first)")

    for number in NUMBERS:
        sandbox = tmp / ("cli-rename-%d" % number)
        cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
        make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
        make_zip(sandbox / "pack.zip", stub=stubs["ok"])
        seen = support.run_cli_child(THIS, "rename-%d" % number, sandbox, False)
        check(seen.get("raised") == s3(number),
              "CLI: the sentence for a rename that fails with system error %d" % number, seen)
        check((cache / "1.0.14" / "old.txt").is_file(), "CLI: after that failure the installed pack is there")

    if WINDOWS:
        for kind, number in HELD:
            sandbox = tmp / ("cli-held-%s" % kind)
            cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
            make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
            make_zip(sandbox / "pack.zip", stub=stubs["ok"])
            held = hold(kind, cache / "1.0.14")
            try:
                seen = support.run_cli_child(THIS, "held", sandbox, False)
            finally:
                held.close()
            check(seen.get("raised") == s3(number),
                  "CLI (Windows): the installed folder held by an open %s gives the sentence for error %d" % (kind, number),
                  seen)


# --- the setup host ---------------------------------------------------------------------------------

def host_layer(probe: Path, tmp: Path, stubs: dict, check: support.Checks) -> None:
    for step, text in enumerate(STEPS):
        sandbox = tmp / ("host-step-%d" % step)
        support.closed_environment(sandbox)
        pack = pack_for_step(sandbox / "pack", step, stubs)
        seen = support.run_probe(probe, ["discard", str(pack / "bin")], sandbox, False,
                                 no_temp(sandbox) if step == 3 else None)
        check(seen.get("exit") == 0 and seen.get("note") == s1(text),
              "host: the pack in use fails at \"%s\" and the sentence names that step" % text,
              {k: v for k, v in seen.items() if k != "stderr"})

    sandbox = tmp / "host-heal"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    pack_for_step(cache / "latest", 4, stubs)
    seen = support.run_probe(probe, ["heal"], sandbox, False)
    check(seen.get("exit") == 0 and seen.get("note") == s1(STEPS[4]),
          "host: the check of the latest pointer names the step in the same sentence",
          {k: v for k, v in seen.items() if k != "stderr"})

    sandbox = tmp / "host-s2"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    archive = make_zip(sandbox / "pack.zip", stub=stubs["ok"], tools=("cmake",))
    seen = support.run_probe(probe, ["install", str(archive)], sandbox, False)
    check(seen.get("ret") == "0" and seen.get("err") == s2(STEPS[1]),
          "host: the sentence for a new pack that fails its check", {k: v for k, v in seen.items() if k != "stderr"})

    sandbox = tmp / "host-rename-text"
    support.closed_environment(sandbox)
    for number in NUMBERS:
        seen = support.run_probe(probe, ["rename-text", str(number)], sandbox, False)
        check(seen.get("err") == s3(number), "host: the sentence for a rename that fails with system error %d" % number,
              {k: v for k, v in seen.items() if k != "stderr"})

    if WINDOWS:
        for kind, number in HELD:
            sandbox = tmp / ("host-held-%s" % kind)
            cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
            make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
            archive = make_zip(sandbox / "pack.zip", stub=stubs["ok"])
            held = hold(kind, cache / "1.0.14")
            try:
                seen = support.run_probe(probe, ["install", str(archive)], sandbox, False)
            finally:
                held.close()
            check(seen.get("ret") == "0" and seen.get("err") == s3(number),
                  "host (Windows): the installed folder held by an open %s gives the sentence for error %d" % (kind, number),
                  {k: v for k, v in seen.items() if k != "stderr"})
            check((cache / "1.0.14" / "old.txt").is_file(), "host (Windows): after that failure the installed pack is there")


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if len(argv) == 2 and argv[0] == "--cli-child":
        return cli_child(argv[1])
    check = support.Checks()
    cc = support.find_compiler(argv)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stubs = make_stubs(tmp, cc)
        if stubs is None:
            print("SKIP: the programs the made-up packs need cannot be built here (no C compiler)")
            return SKIPPED
        cli_layer(tmp, stubs, check)
        if "--cli-only" in argv:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            probe, why = support.build_probe(tmp, cc)
            if probe is None:
                print(why if why.startswith("FAIL") else "%s; the setup host layer did not run" % why)
                check.failures += 1 if why.startswith("FAIL") else 0
            else:
                host_layer(probe, tmp, stubs, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: the toolchain sentences name the step that failed and the system's error")
    return 0


if __name__ == "__main__":
    sys.exit(main())
