#!/usr/bin/env python3
"""Overlay output may change only together with PSX_OVERLAY_CODEGEN_VER (PS1B-339).

An overlay cache is keyed by cg<PSX_OVERLAY_CODEGEN_VER>_<codegen hash>_gc<config
hash>. The codegen hash covers the files in runtime/codegen_hash_sources.cmake.
It does not cover recompiler/src/main_psx.cpp (the overlay-mode driver: seed
partition, exact-entry walk, alias entries, the --ws-config merge, patch
application, the overlay monolith) or config_loader.cpp. A change there can
change the shards that overlay compilation writes while the cache key stays
the same, so a build would load shards and save states made by the older code.
Hashing all of main_psx.cpp would move the key on every static-only change too.

This test is the narrow guard instead. It runs psxrecomp-game in overlay mode,
the way tools/compile_overlays.py does, on authored fixtures, and hashes the
two files that overlay compilation reads: <stem>_full.c and <stem>_full.ranges.
The record (overlay_emitter_fixture.json beside this file) keeps an
append-only list of (codegen version, digest) pairs. The rules:

  * the current digest must equal the record's last digest, and the current
    PSX_OVERLAY_CODEGEN_VER must equal the record's last version;
  * one codegen version owns one digest: a version may not appear twice.

So a change of overlay output fails until PSX_OVERLAY_CODEGEN_VER is raised
and the new pair is appended (--record). Raising the version moves the cache
folder, the boot-state header and, through overlay_api.h, the codegen hash.
A static-only change leaves the overlay output alone and passes. The cause of
a change does not matter (main_psx.cpp, config_loader.cpp, an emitter source,
a library), but only what the fixtures reach is covered.

Usage:
  test_overlay_emitter_fixture_guard.py --recompiler <psxrecomp-game>
      check; exit 0 = pass, 1 = fail
  test_overlay_emitter_fixture_guard.py --recompiler <psxrecomp-game> --record
      append the current (version, digest) pair; refused when the version
      already owns another digest
  test_overlay_emitter_fixture_guard.py --self-test
      the decision rules against authored records, no recompiler needed
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RECORD = Path(__file__).resolve().with_name("overlay_emitter_fixture.json")
OVERLAY_API = ROOT / "runtime" / "include" / "overlay_api.h"
SCHEMA = "psx-overlay-emitter-fixture-v1"

# The capture's trailing delay-slot guard-word tag (tools/compile_overlays.py).
GUARD_TAG_MAGIC_OFFSET = 0x7E0
GUARD_TAG_COUNT_OFFSET = 0x7E8
GUARD_TAG_MAGIC = b"PSXRGRD1"

LOAD = 0x800E7000          # an overlay region above a typical game's text
NOP = 0x00000000
JR_RA = 0x03E00008


def words(seq):
    return b"".join(struct.pack("<I", w) for w in seq)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def make_capture(entry, data, guard_bytes):
    """A PS-X EXE wrapping an overlay capture, as compile_overlays.make_psxexe."""
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, entry)
    struct.pack_into("<I", header, 0x18, LOAD)
    struct.pack_into("<I", header, 0x1C, len(data))
    if guard_bytes:
        header[GUARD_TAG_MAGIC_OFFSET:GUARD_TAG_MAGIC_OFFSET + len(GUARD_TAG_MAGIC)] = GUARD_TAG_MAGIC
        struct.pack_into("<I", header, GUARD_TAG_COUNT_OFFSET, guard_bytes)
    return bytes(header) + data


# Fixture A: the exact-entry driver path. Four routines:
#   F0 +0x00  framed caller: jal F1, then returns (the return point is a CPS
#             continuation; +0x10 is also seeded as an interior alias)
#   pad +0x20 two NOP words after F0's return: F1's callable boundary is the
#             padding rule of main_psx.cpp's exact-entry partition
#   F1 +0x28  leaf loop with a backward branch
#   F2 +0x44  store/load routine, seeded as a dispatch root; its first word is
#             the target of a [[recompiler.patch]] in the --ws-config file
#   +0x5C     one trailing guard word (readable, not discoverable)
F0, F1, F2 = LOAD + 0x00, LOAD + 0x28, LOAD + 0x44
F2_ORIGINAL = 0x24020005   # addiu v0, zero, 5
F2_PATCHED = 0x24020007    # addiu v0, zero, 7


def fixture_a_image():
    f0 = [0x27BDFFE8,               # addiu sp, sp, -24
          0xAFBF0010,               # sw    ra, 16(sp)
          jal(F1), NOP,
          0x8FBF0010,               # lw    ra, 16(sp)
          0x27BD0018,               # addiu sp, sp, 24
          JR_RA, NOP]
    pad = [NOP, NOP]
    f1 = [0x00001021,               # addu  v0, zero, zero
          0x24420001,               # loop: addiu v0, v0, 1
          0x28410004,               # slti  at, v0, 4
          0x1420FFFD,               # bnez  at, loop
          NOP,
          JR_RA, NOP]
    f2 = [F2_ORIGINAL,
          0xAC820000,               # sw    v0, 0(a0)
          0x8C830004,               # lw    v1, 4(a0)
          0x00431021,               # addu  v0, v0, v1
          JR_RA, NOP]
    guard = [NOP]
    data = words(f0 + pad + f1 + f2 + guard)
    assert len(data) == 0x60
    return make_capture(F0, data, guard_bytes=4)


def fixture_a_seeds():
    return (f"0x{F0:08X}\n"
            f"0x{F1:08X}\n"
            f"interior 0x{F0 + 0x10:08X}\n"
            f"dispatch_root 0x{F2:08X}\n")


def fixture_a_config(exe_name):
    return ("[game]\n"
            'name = "overlay emitter fixture"\n'
            f'exe = "{exe_name}"\n'
            "\n"
            "[recompiler]\n"
            'seeds = "seeds.txt"\n'
            f'mod_function_entry_funcs = ["0x{F1:08X}"]\n'
            "\n"
            "[[recompiler.patch]]\n"
            'id = "fixture-immediate"\n'
            f'address = "0x{F2:08X}"\n'
            f'expected = "0x{F2_ORIGINAL:08X}"\n'
            f'replacement = "0x{F2_PATCHED:08X}"\n')


# Fixture B: the same routines with no --ws-config, plain seeds only. It keeps
# a digest of the driver without the config merge, so a change that only the
# merge path shows is told apart from one in the walk itself.
def fixture_b_seeds():
    return f"0x{F0:08X}\n0x{F1:08X}\n0x{F2:08X}\n"


FIXTURES = (
    ("a_exact_entry_ws_config", fixture_a_image, fixture_a_seeds, fixture_a_config),
    ("b_exact_entry_plain", fixture_a_image, fixture_b_seeds, None),
)


def codegen_version(header=OVERLAY_API):
    match = re.search(r"#define\s+PSX_OVERLAY_CODEGEN_VER\s+(\d+)",
                      header.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(f"PSX_OVERLAY_CODEGEN_VER not found in {header}")
    return int(match.group(1))


def child_env(environ=None):
    """The caller's environment without any variable whose name starts with PSX.

    psxrecomp-game reads generator switches from the environment: PSX_CPS,
    PSX_CODEGEN_CYCLE_PER_INSN and PSX_EMIT_PROD change the emitted C, and
    PSXRECOMP_VERBOSE_RI_WARNINGS changes the log. A switch left set in a
    developer's shell would fail the guard with a false fault, or be written
    into the record by --record, where CI could not reproduce it. The guard
    sets no PSX variable itself, so all of them go: a list of names would miss
    the next switch. The rest stays, because the child needs PATH (the
    compiler's runtime libraries), SYSTEMROOT and TEMP to start.
    """
    environ = os.environ if environ is None else environ
    return {key: value for key, value in environ.items()
            if not key.upper().startswith("PSX")}


def run_fixture(recompiler, name, image, seeds, config, tmp):
    """Overlay-mode generation of one fixture; returns {file: sha256}."""
    work = Path(tmp) / name
    out = work / "out"
    out.mkdir(parents=True)
    exe_name = "overlay.psx"
    (work / exe_name).write_bytes(image())
    (work / "seeds.txt").write_text(seeds(), encoding="utf-8", newline="\n")
    # The same invocation as compile_overlays.py: relative paths from the
    # capture's folder, the framework as --project-root, the generator's
    # default switches (child_env).
    cmd = [str(Path(recompiler).resolve()), exe_name, "--seeds", "seeds.txt", "--out-dir", "out", "--overlay"]
    if config:
        (work / "game.toml").write_text(config(exe_name), encoding="utf-8", newline="\n")
        cmd += ["--ws-config", "game.toml"]
    cmd += ["--project-root", str(ROOT)]
    result = subprocess.run(cmd, cwd=work, env=child_env(), capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        # Not a verdict on the overlay output: the generator did not finish.
        raise SystemExit(f"{name}: psxrecomp-game did not produce the overlay "
                         f"(exit {result.returncode}); no digest was taken\n"
                         f"{result.stderr or result.stdout}")
    digests = {}
    for suffix in ("_full.c", "_full.ranges"):
        path = out / (exe_name + suffix)
        if not path.is_file():
            raise SystemExit(f"{name}: no {path.name} in the overlay output")
        digests[suffix.lstrip("_")] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digests


def current_state(recompiler):
    with tempfile.TemporaryDirectory(prefix="ovl-fixture-") as tmp:
        files = {name: run_fixture(recompiler, name, image, seeds, config, tmp)
                 for name, image, seeds, config in FIXTURES}
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {"codegen_ver": codegen_version(), "digest": digest, "files": files}


def load_record(path=RECORD):
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("schema") != SCHEMA:
        raise SystemExit(f"{path.name}: schema is not {SCHEMA}")
    return record


def record_problems(record):
    """Internal consistency of the history: one digest per codegen version."""
    problems = []
    history = record.get("history") or []
    if not history:
        return ["the record has no history entry"]
    owner = {}
    last_ver = None
    for i, entry in enumerate(history):
        ver, digest = entry.get("codegen_ver"), entry.get("digest")
        if not isinstance(ver, int) or not isinstance(digest, str):
            problems.append(f"history[{i}] lacks an integer codegen_ver or a digest")
            continue
        if ver in owner:
            problems.append(f"codegen version {ver} appears twice in the history "
                            f"(digests {owner[ver][:12]} and {digest[:12]}): a "
                            f"version owns one overlay output")
        owner[ver] = digest
        if last_ver is not None and ver <= last_ver:
            problems.append(f"history[{i}] codegen version {ver} is not above "
                            f"the previous entry's {last_ver}")
        last_ver = ver
    return problems


def check(record, state):
    """Return the list of failures (empty = pass)."""
    failures = record_problems(record)
    if failures:
        return failures
    last = record["history"][-1]
    if state["digest"] != last["digest"]:
        changed = sorted(f"{fx}/{fn}"
                         for fx, files in state["files"].items()
                         for fn, digest in files.items()
                         if (last.get("files") or {}).get(fx, {}).get(fn) != digest)
        if state["codegen_ver"] == last["codegen_ver"]:
            failures.append(
                f"overlay output changed at PSX_OVERLAY_CODEGEN_VER {state['codegen_ver']} "
                f"(changed: {', '.join(changed) or 'unknown'}). Caches and save states "
                f"made before this change would still load. Raise PSX_OVERLAY_CODEGEN_VER "
                f"in runtime/include/overlay_api.h and append the new pair with --record.")
        else:
            failures.append(
                f"overlay output changed and PSX_OVERLAY_CODEGEN_VER is "
                f"{state['codegen_ver']} (record: {last['codegen_ver']}); append "
                f"the new pair with --record (changed: {', '.join(changed) or 'unknown'}).")
    elif state["codegen_ver"] != last["codegen_ver"]:
        failures.append(
            f"PSX_OVERLAY_CODEGEN_VER is {state['codegen_ver']} but the record's last "
            f"pair is version {last['codegen_ver']}; append the pair with --record.")
    return failures


def append(record, state):
    """Return the record with the current pair appended, or raise SystemExit."""
    history = list(record.get("history") or [])
    for entry in history:
        if entry.get("codegen_ver") == state["codegen_ver"]:
            if entry.get("digest") == state["digest"]:
                raise SystemExit(f"the record already holds version {state['codegen_ver']} "
                                 f"with this digest; nothing to record")
            raise SystemExit(
                f"refused: codegen version {state['codegen_ver']} already owns digest "
                f"{entry['digest'][:12]}. Raise PSX_OVERLAY_CODEGEN_VER first; a "
                f"version owns one overlay output.")
    if history and state["codegen_ver"] < history[-1]["codegen_ver"]:
        raise SystemExit(f"refused: codegen version {state['codegen_ver']} is below the "
                         f"record's last version {history[-1]['codegen_ver']}")
    history.append({"codegen_ver": state["codegen_ver"], "digest": state["digest"],
                    "files": state["files"]})
    out = dict(record)
    out["history"] = history
    return out


def self_test():
    """The decision rules on authored records; prints each case."""
    files_1 = {"a": {"full.c": "1" * 64, "full.ranges": "2" * 64}}
    files_2 = {"a": {"full.c": "3" * 64, "full.ranges": "2" * 64}}
    d1, d2 = "a" * 64, "b" * 64
    base = {"schema": SCHEMA, "history": [{"codegen_ver": 15, "digest": d1, "files": files_1}]}
    cases = [
        ("unchanged output, same version", base,
         {"codegen_ver": 15, "digest": d1, "files": files_1}, True),
        ("semantic overlay change, same version", base,
         {"codegen_ver": 15, "digest": d2, "files": files_2}, False),
        ("changed output, version raised, not recorded", base,
         {"codegen_ver": 16, "digest": d2, "files": files_2}, False),
        ("version raised, output unchanged, not recorded", base,
         {"codegen_ver": 16, "digest": d1, "files": files_1}, False),
        ("changed output, version raised and recorded",
         {"schema": SCHEMA, "history": base["history"] +
          [{"codegen_ver": 16, "digest": d2, "files": files_2}]},
         {"codegen_ver": 16, "digest": d2, "files": files_2}, True),
        ("re-recorded without a raise (hand edit)",
         {"schema": SCHEMA, "history": base["history"] +
          [{"codegen_ver": 15, "digest": d2, "files": files_2}]},
         {"codegen_ver": 15, "digest": d2, "files": files_2}, False),
        ("empty history", {"schema": SCHEMA, "history": []},
         {"codegen_ver": 15, "digest": d1, "files": files_1}, False),
    ]
    bad = 0
    for label, record, state, want in cases:
        got = not check(record, state)
        ok = got == want
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'}: {label}: "
              f"{'accepted' if got else 'rejected'} (expected "
              f"{'accept' if want else 'reject'})")
    # --record refuses a second digest for one version, accepts a raise.
    try:
        append(base, {"codegen_ver": 15, "digest": d2, "files": files_2})
        print("FAIL: --record without a version raise was accepted")
        bad += 1
    except SystemExit as refusal:
        print(f"PASS: --record without a version raise refused ({str(refusal)[:60]}...)")
    raised = append(base, {"codegen_ver": 16, "digest": d2, "files": files_2})
    if check(raised, {"codegen_ver": 16, "digest": d2, "files": files_2}):
        print("FAIL: a recorded raise does not pass the check")
        bad += 1
    else:
        print("PASS: --record after a version raise appends a pair that passes")
    # The child environment drops every generator switch and keeps the rest.
    env = child_env({"PATH": "p", "PSX_CODEGEN_CYCLE_PER_INSN": "0", "PSX_EMIT_PROD": "0",
                     "PSX_CPS": "0", "PSXRECOMP_VERBOSE_RI_WARNINGS": "1", "psx_lower": "0"})
    if env == {"PATH": "p"}:
        print("PASS: the child environment drops PSX* switches and keeps PATH")
    else:
        print(f"FAIL: the child environment is {sorted(env)}")
        bad += 1
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--recompiler")
    ap.add_argument("--record", action="store_true",
                    help="append the current (codegen version, digest) pair")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--record-file", default=str(RECORD))
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.recompiler or not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler} (build psxrecomp-game first)")

    record_path = Path(args.record_file)
    state = current_state(args.recompiler)
    print(f"PSX_OVERLAY_CODEGEN_VER {state['codegen_ver']}, overlay fixture digest "
          f"{state['digest']}")
    for fx, files in sorted(state["files"].items()):
        for fn, digest in sorted(files.items()):
            print(f"  {fx}/{fn} {digest}")

    if args.record:
        record = load_record(record_path) if record_path.is_file() else {
            "schema": SCHEMA,
            "note": "Written by test_overlay_emitter_fixture_guard.py --record. "
                    "Append-only; one digest per PSX_OVERLAY_CODEGEN_VER (PS1B-339).",
            "history": []}
        record = append(record, state)
        record_path.write_text(json.dumps(record, indent=2) + "\n",
                               encoding="utf-8", newline="\n")
        print(f"recorded version {state['codegen_ver']} in {record_path}")
        return 0

    failures = check(load_record(record_path), state)
    for message in failures:
        print(f"FAIL: {message}")
    if failures:
        return 1
    print("PASS: overlay output matches the record for this codegen version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
