#!/usr/bin/env python3
"""The order in which an installed toolchain pack is changed (PS1B-410).

On 2026-10-02 a setup host judged a whole pack not ready, downloaded the pack and
installed it over the same tag: it removed the installed folder first. Builds
were running from it, so the files they held open survived and the rest went
(PS1B-406). The setup host (host/psxrecomp_codegen_host.c) and the CLI
(tools/toolchain_pack.py) held the same order. The rules now, each checked here
on a made-up cache, for the CLI's module and for the host (a probe that includes
its source file):

  a failed check changes nothing   a pack whose check fails is not removed and
                                   not renamed: byte-for-byte what it was. The
                                   host passes it over and finds another pack.
  a new pack is checked first      a pack that fails its check where it was
                                   unpacked is dropped; the installed one is
                                   byte-for-byte what it was.
  a clean install works            an empty cache gets the pack and the pointer.
  the same tag is replaced whole   the old folder is renamed aside, the new one
                                   takes its name, the old one is removed after.
  a pack in use stays whole        with a file of the installed pack held open,
                                   the pack that is there afterwards is the old
                                   one whole or the new one whole, never a mix,
                                   and it passes its check.
  pruning does the same            an older install with a file held open is
                                   whole or gone, never half removed. A folder
                                   an earlier pass set aside is removed when its
                                   owner has ended and its tag is there again;
                                   another running program's folder stays.
  an interrupted install           a pack left set aside with its tag missing
                                   is put back before anything else.
  both installers check alike      a new pack with cmake and no compiler is
                                   refused by the CLI as by the host.

Everything runs in the closed environment of toolchain_test_support.py.
--dry-run prints it and starts nothing. --cli-only leaves the host layer out.
A pack that passes its check needs a program that runs: a shell script on
Linux and macOS, a compiled one on Windows (--compiler, CC or the PATH). Without
it the cases that need one are skipped and say so; the host layer also needs
recomp-ui (RECOMP_UI_ROOT).
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import CMAKE, WINDOWS, make_pack, make_zip, snapshot  # noqa: E402

THIS = Path(__file__).resolve()
ASIDE = ".old-"
STAGING = (".staging-offline", ".staging-host")


def pack_state(cache: Path, tag: str = "1.0.14") -> dict:
    """What a test asks about a cache after an operation."""
    entries = snapshot(cache)
    names = sorted(p.name for p in cache.iterdir()) if cache.is_dir() else []
    # On Linux and macOS the host links <project>/toolchain to the pack and writes the project's stamp
    # through that link, so the stamp file lies in the pack folder. It names the pack's own bin folder and
    # is not part of the pack.
    return {
        "names": names,
        "tag": {k[len(tag) + 1:]: v for k, v in entries.items()
                if k.startswith(tag + os.sep) and k != os.path.join(tag, ".psxrecomp-bin")},
        "asides": [n for n in names if n.startswith(ASIDE)],
        "staging": [n for n in names if n in STAGING],
        "latest_has_cmake": (cache / "latest" / "bin" / CMAKE).is_file(),
        "latest_is_link": support.is_link(cache / "latest"),
    }


def prepare(case: str, cache: Path, sandbox: Path, stub: Path | None) -> Path | None:
    """The cache a case starts from. Returns the zip of the case, if it has one."""
    if case == "failed_check":
        make_pack(cache / "latest", marker="old.txt")
        make_pack(cache / "1.0.14", marker="old.txt")
        return None
    if case == "bad_new_pack":
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        return make_zip(sandbox / "pack.zip")                       # its cmake does not run
    if case == "clean_install":
        return make_zip(sandbox / "pack.zip", stub=stub)
    if case in ("same_tag", "held_open"):
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        return make_zip(sandbox / "pack.zip", stub=stub)
    if case == "newer_tag":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt", stub=stub)
        return make_zip(sandbox / "pack.zip", stub=stub)
    if case == "prune_held":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt")
        make_pack(cache / "1.0.14")
        make_pack(cache / (ASIDE + "%d-1.0.14" % support.DEAD_PID), marker="left.txt")     # an earlier pass, ended
        make_pack(cache / (ASIDE + "%d-9.9.9" % os.getpid()), "9.9.9", marker="theirs.txt")  # a running program's
        return None
    if case == "interrupted":
        make_pack(cache / (ASIDE + "%d-1.0.14" % support.DEAD_PID), marker="old.txt")      # and no 1.0.14
        return None
    if case == "cmake_only":
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        return make_zip(sandbox / "pack.zip", stub=stub, tools=("cmake",))               # no clang, no linker
    raise ValueError(case)


CASES = ("failed_check", "bad_new_pack", "clean_install", "same_tag", "held_open", "newer_tag", "prune_held",
         "interrupted", "cmake_only")
NEEDS_STUB = ("bad_new_pack", "clean_install", "same_tag", "held_open", "newer_tag", "cmake_only")
HELD = {"held_open": Path("1.0.14") / "old.txt", "prune_held": Path("1.0.13") / "old.txt"}


# --- the CLI's module -----------------------------------------------------------------------------

def cli_child(case: str) -> int:
    calls: list[str] = []
    tp, sandbox = support.load_cli(calls)
    cache = Path(os.environ["RETCOMM_TOOLCHAIN_CACHE"])
    raised = ""
    try:
        if case in ("failed_check", "interrupted"):
            tp.heal_broken_toolchain_pointers()
        elif case == "prune_held":
            tp.prune_old_toolchain_tags(cache / "1.0.14")
        else:
            tp.install_from_zip(sandbox / "pack.zip", min_version="0")
    except Exception as exc:    # noqa: BLE001: the text is what is checked
        raised = "%s: %s" % (type(exc).__name__, exc)
    print(json.dumps({"raised": raised}))
    return 0


def run_cli_case(case: str, tmp: Path, stub: Path | None) -> dict:
    sandbox = tmp / ("cli-" + case)
    env = support.closed_environment(sandbox)
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    prepare(case, cache, sandbox, stub)
    before = snapshot(cache)
    held = open(cache / HELD[case], "rb") if case in HELD else None
    try:
        seen = support.run_cli_child(THIS, case, sandbox, False)
    finally:
        if held:
            held.close()
    seen.update(pack_state(cache), before=before, after=snapshot(cache), cache=cache)
    return seen


# --- the setup host ---------------------------------------------------------------------------------

def run_host_case(probe: Path, case: str, tmp: Path, stub: Path | None) -> dict:
    sandbox = tmp / ("host-" + case)
    env = support.closed_environment(sandbox)
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    archive = prepare(case, cache, sandbox, stub)
    before = snapshot(cache)
    held = open(cache / HELD[case], "rb") if case in HELD else None
    try:
        if case == "failed_check":
            seen = support.run_probe(probe, ["heal"], sandbox, False)
            for leaf in ("latest", "1.0.14"):
                more = support.run_probe(probe, ["discard", str(cache / leaf / "bin")], sandbox, False)
                seen["stderr"] += more["stderr"]
                seen["exit"] = seen["exit"] or more["exit"]
        elif case == "prune_held":
            seen = support.run_probe(probe, ["prune", str(cache / "1.0.14")], sandbox, False)
        elif case == "interrupted":
            seen = support.run_probe(probe, ["heal"], sandbox, False)
        else:
            seen = support.run_probe(probe, ["install", str(archive)], sandbox, False)
    finally:
        if held:
            held.close()
    seen.update(pack_state(cache), before=before, after=snapshot(cache), cache=cache,
                stamp=(sandbox / "project" / "toolchain" / ".psxrecomp-bin").is_file())
    return seen


def host_lookup_case(probe: Path, tmp: Path, stub: Path, check: support.Checks) -> None:
    """The pack in use fails its check: it stays, and the lookup finds the other one."""
    sandbox = tmp / "host-lookup"
    env = support.closed_environment(sandbox)
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    make_pack(cache / "a-fails")                     # a tag name is free text; the order of the two is the folder's
    make_pack(cache / "b-passes", stub=stub)
    before = snapshot(cache)
    seen = support.run_probe(probe, ["resolve"], sandbox, False)
    good = str(cache / "b-passes" / "bin")
    check(seen.get("exit") == 0 and os.path.normcase(os.path.normpath(seen.get("second", ""))) == os.path.normcase(good),
          "host lookup: after the pack in use failed its check, the lookup finds the pack that passes", seen)
    check(snapshot(cache) == before, "host lookup: the pack that failed is byte-for-byte what it was",
          sorted(snapshot(cache)))


# --- what must hold, for both layers -----------------------------------------------------------------

def judge(layer: str, case: str, seen: dict, new_tag: dict | None, check: support.Checks) -> None:
    said = seen.get("raised", "") if layer == "CLI" else seen.get("err", "")
    done = (not seen.get("raised")) if layer == "CLI" else seen.get("ret") == "1"
    name = "%s %s" % (layer, case)
    if "broken" in seen or "refused" in seen or seen.get("exit", 0) != 0:
        check(False, "%s: the case ran" % name, {k: v for k, v in seen.items() if k not in ("before", "after")})
        return
    unchanged = seen["after"] == seen["before"]
    if case == "failed_check":
        check(unchanged, "%s: a pack that fails its check is byte-for-byte what it was (no switch set)" % name,
              seen["names"])
    elif case == "bad_new_pack":
        check(not done and "did not pass its check" in said and "not changed" in said,
              "%s: a new pack that fails its check is refused, with the reason" % name, said)
        check(unchanged and not seen["staging"],
              "%s: the installed pack is byte-for-byte what it was, and no staging folder is left" % name,
              seen["names"])
    elif case == "clean_install":
        check(done and "new.txt" in seen["tag"] and seen["latest_has_cmake"],
              "%s: an empty cache gets the pack and the pointer" % name, (said, seen["names"]))
        check(not seen["asides"] and not seen["staging"], "%s: nothing is left beside the pack" % name, seen["names"])
        if layer == "host":
            check(seen["stamp"], "%s: the project's stamp is written" % name)
    elif case == "same_tag":
        check(done and "new.txt" in seen["tag"] and "old.txt" not in seen["tag"],
              "%s: the installed tag is replaced by the new pack, whole" % name, (said, sorted(seen["tag"])))
        check(not seen["asides"] and not seen["staging"] and seen["latest_has_cmake"],
              "%s: the folder that was set aside is removed and the pointer holds" % name, seen["names"])
    elif case == "held_open":
        old_tag = {k[len("1.0.14") + 1:]: v for k, v in seen["before"].items() if k.startswith("1.0.14" + os.sep)}
        is_old, is_new = seen["tag"] == old_tag, new_tag is not None and seen["tag"] == new_tag
        check(is_old or is_new,
              "%s: with a file of the installed pack held open, the pack afterwards is the old one whole or the "
              "new one whole (here: %s)" % (name, "old" if is_old else "new" if is_new else "A MIX"),
              sorted(seen["tag"]))
        check((cache_bin(seen) / CMAKE).is_file(), "%s: the pack that is there has its cmake" % name)
        if WINDOWS:
            check(is_old and not done and "in use" in said and "not changed" in said,
                  "%s (Windows): the pack in use stays and the reason is given" % name, said)
            check(not seen["staging"] and not seen["asides"], "%s (Windows): the new pack was dropped" % name,
                  seen["names"])
    elif case == "newer_tag":
        check(done and "new.txt" in seen["tag"] and "1.0.13" not in seen["names"] and seen["latest_has_cmake"],
              "%s: a newer tag is installed beside the old one, which is removed afterwards" % name,
              (said, seen["names"]))
        check(not seen["asides"] and not seen["staging"], "%s: nothing is left beside the pack" % name, seen["names"])
    elif case == "interrupted":
        check("old.txt" in seen["tag"] and not seen["asides"],
              "%s: a pack an interrupted install left set aside is put back under its name" % name, seen["names"])
    elif case == "cmake_only":
        check(not done and "did not pass its check" in said and unchanged,
              "%s: a new pack with cmake and no compiler is refused, and the installed pack is byte-for-byte what it was"
              % name, (said, seen["names"]))
    elif case == "prune_held":
        old = {k: v for k, v in seen["before"].items() if k.startswith("1.0.13")}
        now = {k: v for k, v in seen["after"].items() if k.startswith("1.0.13")}
        check(now == old or not now,
              "%s: an older install with a file held open is whole or gone, never half removed (here: %s)"
              % (name, "whole" if now == old else "gone" if not now else "HALF REMOVED"), sorted(now))
        check(ASIDE + "%d-1.0.14" % support.DEAD_PID not in seen["names"],
              "%s: a folder set aside by a program that has ended, whose tag is there again, is removed" % name,
              seen["names"])
        check(ASIDE + "%d-9.9.9" % os.getpid() in seen["names"],
              "%s: a folder set aside by a program that is still running stays" % name, seen["names"])
        check("1.0.14" in seen["names"], "%s: the pack that is kept is there" % name, seen["names"])
        if WINDOWS:
            check(now == old, "%s (Windows): the older install in use stays whole" % name, sorted(now))


def cache_bin(seen: dict) -> Path:
    return seen["cache"] / "1.0.14" / "bin"


def layer(name: str, run, stub: Path | None, check: support.Checks) -> None:
    new_tag = None
    for case in CASES:
        if case in NEEDS_STUB and stub is None:
            print("SKIP %s %s: no pack that passes its check can be made here (no C compiler)" % (name, case))
            continue
        seen = run(case)
        judge(name, case, seen, new_tag, check)
        if case == "same_tag":
            new_tag = seen.get("tag")     # what the new pack looks like when it is installed whole


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
        stub = support.make_stub(tmp, cc)
        layer("CLI", lambda case: run_cli_case(case, tmp, stub), stub, check)
        if "--cli-only" in argv:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            probe, why = support.build_probe(tmp, cc)
            if probe is None:
                print(why if why.startswith("FAIL") else "%s; the setup host layer did not run" % why)
                check.failures += 1 if why.startswith("FAIL") else 0
            else:
                layer("host", lambda case: run_host_case(probe, case, tmp, stub), stub, check)
                if stub is not None:
                    host_lookup_case(probe, tmp, stub, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: an installed toolchain pack is never removed before its replacement is in place")
    return 0


if __name__ == "__main__":
    sys.exit(main())
