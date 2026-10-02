#!/usr/bin/env python3
"""PSXRECOMP_TOOLCHAIN_READONLY=1: nothing deletes, renames, prunes or installs a
toolchain pack, and no pointer to one moves.

The setup host (host/psxrecomp_codegen_host.c) and the CLI (tools/toolchain_pack.py)
each hold a toolchain installer. On 2026-10-02 a setup host started by a test
installed a pack over a build host's own and emptied it under running builds
(PS1B-406, PS1B-410). With the switch set a start can still find and use a pack,
and it cannot change one. Tests and gates that start a setup program set it.

Each operation that changes a pack or a pointer is run twice on a made-up cache:
with the switch (everything must be byte-for-byte what it was, and the step is
named on stderr) and without it (the control: the operation is real, so the
first result means something).

  the CLI's module:   remove a `latest` link that points at nothing, prune older
      installs, install from a zip, move the `latest` pointer, remove and write
      the project stamp, write the user's login PATH, promote the legacy cache;
  the setup host, by a probe that includes its source file:   the same without
      the login PATH and the legacy cache, and link the pack into the project
      and download.

What a failed check does to a pack (nothing, with or without the switch) is in
test_toolchain_safe_install.py.

Everything runs in the closed environment of toolchain_test_support.py.
--dry-run prints it and starts nothing. --cli-only leaves the host layer out.
The host layer needs recomp-ui (RECOMP_UI_ROOT) and a C compiler (--compiler,
CC or the PATH); without them it is skipped and says so. The install cases need
a pack that passes its check: on Windows that needs the compiler too.
"""

from __future__ import annotations

from pathlib import Path
import json
import os
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain_test_support as support  # noqa: E402
from toolchain_test_support import make_link, make_pack, make_zip, snapshot  # noqa: E402

THIS = Path(__file__).resolve()
CLI_CASES = ("dangling", "prune", "install", "pointer", "stamp", "user_path", "migrate")
HOST_CASES = ("dangling", "prune", "install", "pointer", "stamp", "link", "download")


def cli_child(case: str) -> int:
    """Runs one operation of tools/toolchain_pack.py on a made-up cache and prints what it saw."""
    calls: list[str] = []
    tp, sandbox = support.load_cli(calls)
    cache = Path(os.environ["RETCOMM_TOOLCHAIN_CACHE"])
    stub = Path(os.environ["TEST_STUB"]) if os.environ.get("TEST_STUB") else None
    project = sandbox / "project"
    seen: dict[str, object] = {}
    raised = ""
    watch = cache
    if case == "dangling":
        make_pack(cache / "1.0.14")
        if not make_link(cache / "latest", cache / "gone"):
            (cache / "gone").mkdir()
            if not make_link(cache / "latest", cache / "gone"):
                print(json.dumps({"skipped": "no directory link can be made here"}))
                return 0
            (cache / "gone").rmdir()
        before = snapshot(watch)
        tp.heal_broken_toolchain_pointers()
    elif case == "prune":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt")
        keep = make_pack(cache / "1.0.14")
        before = snapshot(watch)
        tp.prune_old_toolchain_tags(keep)
    elif case == "install":
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        archive = make_zip(sandbox / "pack.zip", stub=stub)
        before = snapshot(watch)
        try:
            tp.install_from_zip(archive, min_version="0")
        except Exception as exc:    # noqa: BLE001: the text is what is checked
            raised = "%s: %s" % (type(exc).__name__, exc)
    elif case == "pointer":
        pack = make_pack(cache / "1.0.14")
        make_pack(cache / "latest", "1.0.13", marker="old.txt")
        before = snapshot(watch)
        tp._set_latest_pointer(cache, pack)
    elif case == "stamp":
        watch = sandbox
        (project / "toolchain").mkdir(parents=True)
        (project / "toolchain" / tp.STAMP_NAME).write_text("somewhere\n", encoding="utf-8")
        pack = make_pack(cache / "1.0.14")
        before = snapshot(watch)
        tp.clear_project_toolchain_stamp(project)
        seen["stamp_kept"] = (project / "toolchain" / tp.STAMP_NAME).is_file()
        tp.write_toolchain_stamp(sandbox / "project-2", pack / "bin")
        seen["new_stamp_written"] = (sandbox / "project-2" / "toolchain" / tp.STAMP_NAME).is_file()
    elif case == "user_path":
        pack = make_pack(cache / "1.0.14")
        before = snapshot(watch)
        tp.register_toolchain_user_env(pack)
    elif case == "migrate":
        # The promotion picks its two roots by a folder named retcomm and one named psxrecomp.
        legacy = next(r for r in tp.shared_cache_roots() if "psxrecomp" in r.parts)
        watch = next(r for r in tp.shared_cache_roots() if "retcomm" in r.parts)
        if sandbox not in legacy.parents or sandbox not in watch.parents:
            print(json.dumps({"refused": "a cache root outside the sandbox: %s, %s" % (legacy, watch)}))
            return 2
        make_pack(legacy / "1.0.14")
        before = snapshot(watch)
        tp.migrate_legacy_psxrecomp_cache()
    else:
        return 2
    seen["unchanged"] = snapshot(watch) == before
    seen["raised"] = raised
    seen["registrar_called"] = bool(calls)
    seen["entries"] = sorted(snapshot(watch))
    print(json.dumps(seen))
    return 0


def cli_layer(tmp: Path, stub: Path | None, check: support.Checks) -> None:
    for case in CLI_CASES:
        if case == "install" and stub is None:
            print("SKIP CLI install: no pack that passes its check can be made here (no C compiler)")
            continue
        extra = {"TEST_STUB": str(stub)} if stub else {}
        on = support.run_cli_child(THIS, case, tmp / ("cli-%s-on" % case), True, extra)
        off = support.run_cli_child(THIS, case, tmp / ("cli-%s-off" % case), False, extra)
        if "skipped" in on:
            print("SKIP CLI %s: %s" % (case, on["skipped"]))
            continue
        if any(key in seen for seen in (on, off) for key in ("broken", "refused")):
            check(False, "CLI %s: the case ran" % case, (on, off))
            continue
        check(on["unchanged"] is True, "CLI %s, switch set: everything is byte-for-byte what it was" % case, on)
        check("read-only" in on["stderr"], "CLI %s, switch set: it names the step it left undone" % case,
              on["stderr"][-400:])
        check(off["unchanged"] is False, "CLI %s, control without the switch: the operation is real" % case, off)
        if case == "install":
            check("read-only" in on["raised"] and "Nothing was installed" in on["raised"],
                  "CLI install, switch set: it refuses with a reason", on["raised"])
            check(not on["registrar_called"], "CLI install, switch set: the user's login PATH is not written", on)
            check(not off["raised"] and off["registrar_called"],
                  "CLI install, control: the pack is installed and the login PATH is written", off)
        if case == "user_path":
            check(not on["registrar_called"] and off["registrar_called"],
                  "CLI user PATH: written without the switch, not written with it", (on, off))
        if case == "stamp":
            check(on["stamp_kept"] and not on["new_stamp_written"],
                  "CLI stamp, switch set: the stamp is neither removed nor written", on)
            check(not off["stamp_kept"] and off["new_stamp_written"],
                  "CLI stamp, control: the stamp is removed and written", off)


def run_host_case(probe: Path, case: str, switch: bool, tmp: Path, stub: Path | None) -> dict:
    sandbox = tmp / ("host-%s-%s" % (case, "on" if switch else "off"))
    env = support.closed_environment(sandbox)
    cache = Path(env["RETCOMM_TOOLCHAIN_CACHE"])
    project = sandbox / "project"
    project.mkdir()
    watch = sandbox
    if case == "dangling":
        make_pack(cache / "1.0.14")
        (cache / "gone").mkdir()
        if not make_link(cache / "latest", cache / "gone"):
            return {"skipped": "no directory link can be made here"}
        (cache / "gone").rmdir()
        args = ["heal"]
    elif case == "prune":
        make_pack(cache / "1.0.13", "1.0.13", marker="old.txt")
        args = ["prune", str(make_pack(cache / "1.0.14"))]
    elif case == "install":
        make_pack(cache / "1.0.14", marker="old.txt", stub=stub)
        args = ["install", str(make_zip(sandbox / "pack.zip", stub=stub))]
    elif case == "pointer":
        make_pack(cache / "latest", "1.0.13", marker="old.txt")
        args = ["pointer", str(cache), str(make_pack(cache / "1.0.14"))]
    elif case == "stamp":
        (project / "toolchain").mkdir()
        (project / "toolchain" / ".psxrecomp-bin").write_text("somewhere\n", encoding="utf-8")
        args = ["stamp", str(make_pack(cache / "1.0.14") / "bin")]
    elif case == "link":
        args = ["link", str(make_pack(cache / "1.0.14"))]
    elif case == "download":
        make_pack(cache / "1.0.14", marker="old.txt")
        args = ["download"]
    else:
        return {"broken": "unknown case"}
    before = snapshot(watch)
    seen = support.run_probe(probe, args, sandbox, switch)
    after = snapshot(watch)
    # The probe's own temp files are not what is asked about.
    seen["unchanged"] = ({k: v for k, v in after.items() if not k.startswith("temp")}
                         == {k: v for k, v in before.items() if not k.startswith("temp")})
    seen["entries"] = sorted(k for k in after if not k.startswith("temp"))
    return seen


def host_layer(tmp: Path, cc: str | None, stub: Path | None, check: support.Checks) -> None:
    probe, why = support.build_probe(tmp, cc)
    if probe is None:
        print(why if why.startswith("FAIL") else "%s; the setup host layer did not run" % why)
        if why.startswith("FAIL"):
            check.failures += 1
        return
    for case in HOST_CASES:
        if case == "install" and stub is None:
            print("SKIP host install: no pack that passes its check can be made here")
            continue
        on = run_host_case(probe, case, True, tmp, stub)
        if "skipped" in on:
            print("SKIP host %s: %s" % (case, on["skipped"]))
            continue
        if "refused" in on or "broken" in on or on["exit"] != 0:
            check(False, "host %s: the case ran" % case, on)
            continue
        check(on["unchanged"], "host %s, switch set: everything is byte-for-byte what it was" % case, on)
        check("toolchain read-only" in on["stderr"], "host %s, switch set: it names the step it left undone" % case,
              on["stderr"][-400:])
        if case == "install":
            check(on["ret"] == "0" and "Nothing was installed" in on["err"],
                  "host install, switch set: it refuses with a reason", on)
        if case == "download":
            check(on["ret"] == "0" and "Nothing was downloaded" in on["err"],
                  "host download, switch set: it refuses before any request", on)
            continue    # no control: without the switch this is a real request to a server
        off = run_host_case(probe, case, False, tmp, stub)
        check(off.get("unchanged") is False, "host %s, control without the switch: the operation is real" % case, off)
        if case == "install":
            check(off.get("ret") == "1", "host install, control: the pack is installed", off)


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
        cli_layer(tmp, stub, check)
        if "--cli-only" in argv:
            print("the setup host layer was not asked for (--cli-only)")
        else:
            host_layer(tmp, cc, stub, check)
    if check.failures:
        print("FAILED: %d check(s)" % check.failures)
        return 1
    print("PASS: with the switch set no toolchain pack or pointer is changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
