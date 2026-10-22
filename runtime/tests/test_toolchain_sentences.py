#!/usr/bin/env python3
"""What a player reads when the toolchain check or an install fails (PS1B-410, item A13;
PS1B-488 for S4 and S5).

Five sentences, each saying only what the program knows:

  S1  "The portable toolchain did not pass its check: <step>. It was not removed.
       Download the latest pack to replace it."                  (setup host)
  S2  "The new toolchain pack did not pass its check: <step>. The installed
       toolchain was not changed."                               (setup host, CLI)
  S3  the installed folder cannot be renamed aside, by the system's error:
      Windows error 32, Windows error 5, any other error with its number.
                                                                 (setup host, CLI)
  S4  "The toolchain pack was not downloaded: <reason>. The installed toolchain
       was not changed. Check the free space and the permissions of the
       temporary folder, then try again."                        (setup host, CLI)
  S5  "The new toolchain pack did not pass its check: <step>. The installed
       toolchain could not be put back under its name. It was left in the same
       place, with .old- at the start of its name. Download the latest pack to
       install the toolchain again."                             (setup host, CLI)

S4 and S5 are proposals of PS1B-488 until Alex approves the wording.
<reason> is one of two: the temporary folder could not be created / the pack
could not be written into the temporary folder.

<step> is one of five: cmake does not run / clang or the linker is missing from
the pack / clang does not run / a test file could not be written to the
temporary folder / a one-line test program did not compile and link.

Every step and every form is reached here by code that runs, and the whole
sentence is compared:

  the five steps   a made-up pack for each, checked by the setup host (a probe
                   that includes its source file) and by the CLI's module.
  S2               a zip whose pack has cmake and no compiler, installed.
  S3, by number    the host's sentence for 32, 5, 13 and 16; the CLI's install with
                   a rename that fails with each number (the failure is put in
                   by the test).
  S4, both         the CLI's download with a temporary folder that is not there
                   and with a write that fails (no network is used: the test puts
                   the failure in); the setup host's download with a curl that
                   exits 23, its write error (a stub in front of the real one),
                   and, on Windows, a temporary folder that cannot be created.
  S5               a pack that passes where it was unpacked and fails in its
                   place, with the put back of the installed pack made to fail
                   (the CLI by a rename that fails, the setup host by a probe
                   that fails the rename of a ".old-" folder). The installed
                   pack must still be there under its ".old-" name, as the
                   sentence says. The same pack with a put back that works must
                   give S2 and bring the installed pack back.
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
from toolchain_test_support import (D_REASONS, EXE, STEPS, WINDOWS, make_pack, make_zip,  # noqa: E402
                                    s1, s2, s3, sd, sp)

THIS = Path(__file__).resolve()
SKIPPED = 77
NUMBERS = (32, 5, 13, 16)     # 16 is EBUSY elsewhere than Windows: another program holds the pack (PS1B-416)


def make_stubs(tmp: Path, cc: str | None) -> dict | None:
    """Five programs: one that exits 0, one that exits 1, one that exits 0 for --version only,
    one that exits 0 where it was unpacked (a path with .staging in it) and 1 in its place,
    and one that stands for curl and exits 23 (curl's write error)."""
    ok = support.make_stub(tmp, cc)
    if ok is None:
        return None
    if not WINDOWS:
        made = {"ok": ok}
        for name, body in (("fail", "exit 1\n"),
                           ("version", '[ "$1" = "--version" ] && exit 0\nexit 1\n'),
                           ("place", 'case "$0" in *.staging*) exit 0;; esac\nexit 1\n'),
                           ("curl23", "exit 23\n")):
            stub = tmp / ("stub-" + name)
            stub.write_text("#!/bin/sh\n" + body, encoding="utf-8")
            stub.chmod(0o755)
            made[name] = stub
        return made
    made = {"ok": ok}
    for name, body in (("fail", "int main(void) { return 1; }\n"),
                       ("version", "#include <string.h>\nint main(int c, char** v) "
                                   "{ return (c > 1 && strcmp(v[1], \"--version\") == 0) ? 0 : 1; }\n"),
                       ("place", "#include <windows.h>\n#include <string.h>\nint main(void) { char p[2048]; "
                                 "DWORD n = GetModuleFileNameA(NULL, p, sizeof p); "
                                 "return (n > 0 && n < sizeof p && strstr(p, \".staging\") != NULL) ? 0 : 1; }\n"),
                       ("curl23", "int main(void) { return 23; }\n")):
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
    if case in ("download-folder", "download-write", "download-ensure"):
        # The download into the temporary folder (PS1B-488). No network is used: the first and the
        # third stop before any connection, the second has the connection and the write put in by the test.
        if case == "download-write":
            import builtins
            import urllib.request

            class Response:
                def __init__(self):
                    self.chunks = [b"PK", b""]

                def __enter__(self):
                    return self

                def __exit__(self, *_exc):
                    return False

                def read(self, _size=-1):
                    return self.chunks.pop(0) if self.chunks else b""
            urllib.request.urlopen = lambda *_a, **_k: Response()
            real_open = builtins.open
            asset = tp._ASSET[tp.host_artifact()]

            def failing_open(file, mode="r", *args, **kwargs):
                if "w" in mode and str(file).endswith(asset):
                    raise OSError(28, "put in by the test")
                return real_open(file, mode, *args, **kwargs)
            builtins.open = failing_open
        else:
            tp.tempfile.tempdir = no_temp(sandbox)["TMPDIR"]          # a temporary folder that is not there
        try:
            if case == "download-ensure":
                tp.ensure_toolchain(sandbox / "project", download=True, min_version="0")
            else:
                tp.download_latest_pack(repo="example/none", min_version="0")
        except Exception as exc:    # noqa: BLE001: the text is what is checked
            out["raised"] = str(exc)
            out["kind"] = type(exc).__name__
        print(json.dumps(out))
        return 0
    if case == "rebuild-download":
        # The toolchain step of a rebuild with a download that stops at the temporary folder.
        sys.path.insert(0, str(support.ROOT))
        import psxrecomp_cli as cli

        class DownloadLines:
            def __init__(self):
                self.lines = []

            def log(self, message, **_kwargs):
                self.lines.append(str(message))
        lines = DownloadLines()
        tp.tempfile.tempdir = no_temp(sandbox)["TMPDIR"]
        ok = cli.ensure_toolchain_for_rebuild(sandbox / "project", lines, download=True, min_version="0")
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
        if case == "put-back":
            # The put back of the installed pack fails: the rename of a folder named .old-... fails.
            real = os.rename

            def failing_put_back(src, dst, *args, **kwargs):
                if os.path.basename(str(src)).startswith(".old-"):
                    if WINDOWS:
                        raise OSError(13, "put in by the test", str(src), 5)      # winerror
                    raise OSError(13, "put in by the test", str(src))
                return real(src, dst, *args, **kwargs)
            tp.os.rename = failing_put_back
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

    # S4: the download into the temporary folder.
    for case, reason, what in (("download-folder", D_REASONS[0], "a temporary folder that is not there"),
                               ("download-write", D_REASONS[1], "a write into the temporary folder that fails")):
        sandbox = tmp / ("cli-" + case)
        support.closed_environment(sandbox)
        seen = support.run_cli_child(THIS, case, sandbox, False)
        check(seen.get("raised") == sd(reason) and seen.get("kind") == "ToolchainRefused",
              "CLI: the sentence for a download with %s, as the whole text of the refusal" % what, seen)
    sandbox = tmp / "cli-download-ensure"
    support.closed_environment(sandbox)
    (sandbox / "project").mkdir(exist_ok=True)
    seen = support.run_cli_child(THIS, "download-ensure", sandbox, False)
    check(seen.get("raised") == sd(D_REASONS[0]) and seen.get("kind") == "ToolchainRefused",
          "CLI: ensure_toolchain with a download passes the same sentence on, whole", seen)
    if WINDOWS:
        sandbox = tmp / "cli-download-rebuild"
        support.closed_environment(sandbox)
        (sandbox / "project").mkdir(exist_ok=True)
        seen = support.run_cli_child(THIS, "rebuild-download", sandbox, False)
        check(seen.get("ok") is False and seen.get("lines") and seen["lines"][-1] == sd(D_REASONS[0]),
              "CLI rebuild (Windows): the toolchain step gives the progress line the download sentence alone", seen)
    else:
        print("not run here: the rebuild's toolchain step reaches the installer on Windows only "
              "(elsewhere it uses the native tools first)")

    # S5: a pack that fails its check in its place, and the installed pack cannot be put back.
    sandbox = tmp / "cli-put-back"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    make_zip(sandbox / "pack.zip", stub=stubs["ok"], overrides={"clang": stubs["place"]})
    seen = support.run_cli_child(THIS, "put-back", sandbox, False)
    check(seen.get("raised") == sp(STEPS[2]) and seen.get("kind") == "ToolchainRefused",
          "CLI: the sentence for a new pack that fails its check in its place when the installed pack "
          "cannot be put back", seen)
    left = [e for e in cache.iterdir() if e.name.startswith(".old-") and (e / "old.txt").is_file()]
    check(len(left) == 1 and not (cache / "1.0.14").exists(),
          "CLI: after that failure the installed pack is whole under its .old- name, as the sentence says",
          sorted(e.name for e in cache.iterdir()))
    sandbox = tmp / "cli-put-back-control"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    make_zip(sandbox / "pack.zip", stub=stubs["ok"], overrides={"clang": stubs["place"]})
    seen = support.run_cli_child(THIS, "s2", sandbox, False)
    check(seen.get("raised") == s2(STEPS[2]) and (cache / "1.0.14" / "old.txt").is_file(),
          "CLI: the same pack with a put back that works gives the sentence S2 and the installed pack is back",
          seen)

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

    # S4: the download into the temporary folder.
    if WINDOWS:
        sandbox = tmp / "host-download-folder"
        support.closed_environment(sandbox)
        blocker = sandbox / "temp" / "a-file"
        blocker.write_text("not a folder\n", encoding="utf-8")
        gone = str(blocker / "x")                       # below a file: the folder cannot be created
        seen = support.run_probe(probe, ["download"], sandbox, False, {"TEMP": gone, "TMP": gone, "TMPDIR": gone})
        check(seen.get("ret") == "0" and seen.get("err") == sd(D_REASONS[0]),
              "host (Windows): the sentence for a download when the temporary folder cannot be created",
              {k: v for k, v in seen.items() if k != "stderr"})
    else:
        print("not run here: on Linux and macOS the setup host downloads into /tmp, which a test cannot close")
    sandbox = tmp / "host-download-write"
    support.closed_environment(sandbox)
    stubbin = sandbox / "stubbin"
    stubbin.mkdir()
    shutil.copy2(stubs["curl23"], stubbin / ("curl" + EXE))
    if WINDOWS:
        runner, extra = stubbin / "probe.exe", None     # CreateProcess looks beside the program first
        shutil.copy2(probe, runner)
    else:
        runner, extra = probe, {"PATH": str(stubbin) + os.pathsep + "/usr/bin:/bin"}
    seen = support.run_probe(runner, ["download"], sandbox, False, extra)
    check(seen.get("ret") == "0" and seen.get("err") == sd(D_REASONS[1]),
          "host: the sentence for a download where curl exits with its write error (23)",
          {k: v for k, v in seen.items() if k != "stderr"})

    # S5: a pack that fails its check in its place, and the installed pack cannot be put back.
    sandbox = tmp / "host-put-back"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    archive = make_zip(sandbox / "pack.zip", stub=stubs["ok"], overrides={"clang": stubs["place"]})
    seen = support.run_probe(probe, ["install", str(archive)], sandbox, False, {"PROBE_FAIL_PUT_BACK": "1"})
    check(seen.get("ret") == "0" and seen.get("err") == sp(STEPS[2]),
          "host: the sentence for a new pack that fails its check in its place when the installed pack "
          "cannot be put back", {k: v for k, v in seen.items() if k != "stderr"})
    left = [e for e in cache.iterdir() if e.name.startswith(".old-") and (e / "old.txt").is_file()]
    check(len(left) == 1 and not (cache / "1.0.14").exists(),
          "host: after that failure the installed pack is whole under its .old- name, as the sentence says",
          sorted(e.name for e in cache.iterdir()))
    sandbox = tmp / "host-put-back-control"
    cache = Path(support.closed_environment(sandbox)["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "1.0.14", marker="old.txt", stub=stubs["ok"])
    archive = make_zip(sandbox / "pack.zip", stub=stubs["ok"], overrides={"clang": stubs["place"]})
    seen = support.run_probe(probe, ["install", str(archive)], sandbox, False)
    check(seen.get("ret") == "0" and seen.get("err") == s2(STEPS[2]) and (cache / "1.0.14" / "old.txt").is_file(),
          "host: the same pack with a put back that works gives the sentence S2 and the installed pack is back",
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
