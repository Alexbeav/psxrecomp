#!/usr/bin/env python3
"""The toolchain check and a long PATH (PS1B-410).

On 2026-10-02 a setup host judged two whole toolchain packs unusable and installed
a downloaded one over the installed pack (PS1B-406). The cause was the length of
PATH:

  - the host's compile-and-link check ran
        cmd.exe /C "set "PATH=<bin>;%PATH%" && "<clang>" "<src>" -o "<exe>" >NUL 2>&1"
    cmd expands %PATH% inside that line and a cmd line holds 8,191 characters.
    Measured on a real pack: the check passed with a PATH of 7,619 characters
    and failed with 7,811 ("The input line is too long.");
  - activate_toolchain_path put the pack's folders in front of PATH at every
    call, into a buffer of 8,192 characters, and the rebuild helper put one in
    front again. A host that relaunched itself grew PATH by three folders a
    generation until the check failed.

What is checked here, on a made-up pack whose programs run:

  the check and the length     the pack is healthy with a PATH of 2,000, 7,811,
                               12,000 and 32,000 characters;
  at the variable's limit      one variable holds 32,767 characters. With a PATH
                               that long Windows cannot start a child at all, so
                               the check fails for any pack. The pack is then
                               reported and is byte-for-byte what it was;
  activation does not grow     ten activations in a row leave PATH's length as
                               it was after the first, a long PATH is not cut
                               off, and copies of the pack's folders further
                               back in PATH are taken out;
  the sources                  no %PATH% inside a cmd line of the host.

The CLI (tools/toolchain_pack.py) had neither fault: its check starts cmake
without a shell, and its activation adds a folder only when it is missing. The
second is pinned here as well.

Everything runs in the closed environment of toolchain_test_support.py; the
PATH a case needs is padded with folders that do not exist. --dry-run prints
the environment and starts nothing. The host layer needs recomp-ui
(RECOMP_UI_ROOT) and a C compiler (--compiler, CC or the PATH).
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import WINDOWS, make_pack, snapshot  # noqa: E402

THIS = Path(__file__).resolve()
SEP = os.pathsep
VARIABLE_LIMIT = 32767
LENGTHS = (2000, 7811, 12000, 32000)


def padded_path(base: str, length: int, sandbox: Path) -> str:
    """`base` followed by folders that do not exist, `length` characters in all."""
    path = base
    n = 0
    while len(path) < length:
        entry = SEP + str(sandbox / "no-such-folder" / ("%05d" % n))
        if len(path) + len(entry) > length:
            entry = (entry + "x" * length)[:length - len(path)]
        path += entry
        n += 1
    return path


# --- the setup host ---------------------------------------------------------------------------------

def host_layer(tmp: Path, cc: str | None, stub: Path | None, check: support.Checks) -> None:
    probe, why = support.build_probe(tmp, cc)
    if probe is None or stub is None:
        print((why or "SKIP: no pack that passes its check can be made here") + "; the setup host layer did not run")
        if why.startswith("FAIL"):
            check.failures += 1
        return

    def start(name: str, path_length: int | None, args_for, runnable: bool = True, path: str | None = None,
              native_tools: bool = True):
        """One probe run. `native_tools` False (Linux, macOS): PATH holds no system folder, so the host does
        not find a native cmake and ninja and activates the pack, as it always does on Windows."""
        sandbox = tmp / ("host-" + name)
        env = support.closed_environment(sandbox)
        cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
        pack = make_pack(cache / "1.0.14", stub=stub if runnable else None)
        base = env["PATH"] if (WINDOWS or native_tools) else str(sandbox / "no-native-tools")
        use = path if path is not None else (padded_path(base, path_length, sandbox) if path_length else base)
        before = snapshot(cache)
        seen = run_with_path(probe, args_for(pack), sandbox, use.replace("<bin>", str(pack / "bin")))
        seen["unchanged"] = snapshot(cache) == before
        seen["pack"] = pack
        return seen

    for length in LENGTHS:
        seen = start("healthy-%d" % length, length, lambda pack: ["healthy", str(pack / "bin")])
        check(seen.get("ret") == "1", "host: a pack that works is healthy with a PATH of %d characters" % length,
              {k: seen.get(k) for k in ("ret", "exit", "stderr")})

    # At the variable's limit. Measured on Windows with a real pack: with a PATH of 32,766 characters the
    # host cannot start any child (CreateProcess error 8), so every step of the check fails and the pack is
    # reported not ready. Whatever the verdict there, nothing may be removed or renamed.
    over = VARIABLE_LIMIT - 20
    seen = start("healthy-over", over, lambda pack: ["healthy", str(pack / "bin")])
    print("note: with a PATH of %d characters a pack that works is judged %s here"
          % (over, "healthy" if seen.get("ret") == "1" else "not healthy"))
    if not WINDOWS:
        check(seen.get("ret") == "1", "host: a pack that works is healthy with a PATH of %d characters" % over,
              {k: seen.get(k) for k in ("ret", "exit", "stderr")})
    for runnable in (True, False):
        for op, args_for in (("heal", lambda pack: ["heal"]), ("discard", lambda pack: ["discard", str(pack / "bin")])):
            seen = start("over-%s-%s" % (op, "works" if runnable else "fails"), over, args_for, runnable=runnable)
            check(seen.get("exit") == 0 and seen["unchanged"],
                  "host: with a PATH of %d characters a pack that %s is byte-for-byte what it was after the check (%s)"
                  % (over, "works" if runnable else "fails its check", op),
                  {k: seen.get(k) for k in ("ret", "exit", "stderr")})

    for length in (2000, 12000):
        seen = start("activate-%d" % length, length, lambda pack: ["activate", "10"], native_tools=False)
        lens = [int(seen[k]) for k in ("len%d" % i for i in range(1, 11)) if k in seen]
        check(len(lens) == 10 and len(set(lens)) == 1,
              "host: ten activations leave PATH's length as it was after the first (from %d characters)" % length,
              lens or seen)
        if lens:
            added = len(str(seen["pack"] / "bin")) + 1
            check(lens[0] == length + added,
                  "host: the first activation adds the pack's bin folder once and cuts nothing off (%d + %d)"
                  % (length, added), lens[0])

    # Copies of the pack's folder further back are taken out; the other entries keep their order.
    a, b = str(tmp / "host-dedupe" / "a"), str(tmp / "host-dedupe" / "b")
    seen = start("dedupe", None, lambda pack: ["activate", "2"], path=SEP.join([a, "<bin>", b, "<bin>"]))
    want = SEP.join([str(seen["pack"] / "bin"), a, b])
    got = (seen.get("path") or "").replace("/", os.sep)      # the host joins folders with a forward slash
    check(got == want.replace("/", os.sep), "host: the pack's folder is at the head once and its other copies are gone",
          (seen.get("path"), want))


def run_with_path(probe: Path, args: list[str], sandbox: Path, path: str) -> dict:
    """support.run_probe with the closed environment's PATH replaced by `path`."""
    import subprocess
    env = support.closed_environment(sandbox)
    env["PATH"] = path
    (sandbox / "project").mkdir(exist_ok=True)
    env["PROBE_PROJECT"] = str(sandbox / "project")
    outside = support.roots_outside(env, sandbox)
    if outside:
        return {"refused": "roots outside the sandbox: %s" % ", ".join(outside)}
    run = subprocess.run([str(probe)] + args, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=env, cwd=str(sandbox))
    lines = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
    lines.update({"stderr": run.stderr, "exit": run.returncode})
    return lines


# --- the CLI's module -----------------------------------------------------------------------------

def cli_child(case: str) -> int:
    calls: list[str] = []
    tp, sandbox = support.load_cli(calls)
    pack = Path(os.environ["RETCOMM_TOOLCHAIN_CACHE"]) / "1.0.14"
    seen: dict[str, object] = {"start": len(os.environ.get("PATH", ""))}
    if case == "activate":
        lens = []
        for _ in range(10):
            tp.activate_toolchain_bin(pack / "bin")
            lens.append(len(os.environ["PATH"]))
        seen["lens"] = lens
    elif case == "runs":
        seen["runs"] = tp.toolchain_bin_runs(pack / "bin")
    print(json.dumps(seen))
    return 0


def cli_layer(tmp: Path, stub: Path | None, check: support.Checks) -> None:
    if stub is None:
        print("SKIP the CLI layer: no pack that passes its check can be made here (no C compiler)")
        return
    for case, length in (("activate", 12000), ("runs", 12000), ("runs", 32000)):
        sandbox = tmp / ("cli-%s-%d" % (case, length))
        env = support.closed_environment(sandbox)
        make_pack(Path(env["RETCOMM_TOOLCHAIN_CACHE"]) / "1.0.14", stub=stub)
        seen = support.run_cli_child(THIS, case, sandbox, False,
                                     {"PATH": padded_path(env["PATH"], length, sandbox)})
        if case == "activate":
            lens = seen.get("lens") or []
            check(len(lens) == 10 and len(set(lens)) == 1 and lens[0] > length,
                  "CLI: ten activations leave PATH's length as it was after the first (from %d characters)" % length,
                  seen)
        else:
            check(seen.get("runs") is True, "CLI: a pack that works runs with a PATH of %d characters" % length, seen)


def source_checks(check: support.Checks) -> None:
    text = support.HOST_C.read_text(encoding="utf-8")
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith(("*", "/*", "//")))
    check("%%PATH%%" not in code, "host source: no %PATH% is expanded inside a cmd line",
          [n + 1 for n, line in enumerate(text.splitlines()) if "%%PATH%%" in line])
    check("char neu[8192]" not in code, "host source: PATH is not built in a fixed buffer")


def main() -> int:
    argv = sys.argv[1:]
    if "--dry-run" in argv:
        return support.dry_run()
    if len(argv) == 2 and argv[0] == "--cli-child":
        return cli_child(argv[1])
    check = support.Checks()
    cc = support.find_compiler(argv)
    source_checks(check)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = support.make_stub(tmp, cc)
        cli_layer(tmp, stub, check)
        if "--cli-only" in argv:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            host_layer(tmp, cc, stub, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: the toolchain check does not depend on the length of PATH, and activation does not grow it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
