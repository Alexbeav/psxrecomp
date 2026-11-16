"""After a build's starts: does every overlay the starts captured compile? (PS1B-380)

A start compiles overlay code while it runs. The runtime reads its compile
driver's output into memory, counts the units that failed, and shows neither
anywhere a product can be read: the run report says only whether autocompile
is "degraded", which a failed unit is not. A unit that fails to compile runs
interpreted, and until this check nothing in a build's evidence said so. Found
2026-10-02: a fault in the driver made clang reject some generated sources
(PS1B-124), and no log of the fleet build held a compile error line.

Studio cannot get the starts' own compiler output from the runtime. It asks
the product's own driver instead, after the starts, on the validation copy:
`compile_overlays.py --check` builds every captured overlay into a throwaway
folder, touches no cache, prints `SHARD FAIL [class] unit: detail` for each
unit that fails and a `PSX_SHARD_RESULT ok=N failed=M skipped=K` line. The
whole output is kept in the run's evidence. "Every captured overlay" is the
union of the capture snapshots the starts wrote, not only the newest one.

This is a finding, never a stop. A unit that fails gives a warning with the
first error line. A title with very many captures does not finish in the time
limit: that is recorded as incomplete, with what was seen until then.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from collections import Counter
from pathlib import Path

LIMIT_ENV = "WORKBENCH_OVERLAY_CHECK_SECONDS"
# Measured 2026-10-02 on Eagle: Forsaken, 16 units, 8 s at 4 jobs; Rhapsody, 131 units, 186 s at 4 jobs on a quiet
# host and not finished in 300 s at 10 jobs beside other work. MDK's start wrote 8,024 snapshots of one region
# and does not finish; the limit is for titles like that.
TIME_LIMIT_SECONDS = 600
LOG = "overlay-compile-check.log"
UNITS_TABLE = "overlay-compile-check-units.tsv"
# What every count of this check is about. A unit's source is generated from the region's bytes AND from the
# entry points the starts observed. Rhapsody, 2026-10-02: the same bytes (one CRC) compiled on Pegasus with
# 1,962 seeds and failed on Eagle with 2,071. So a clean check does not say "this title has no failing unit".
SCOPE = ("of the overlay units this build's starts captured; another start can observe other entry points in "
         "the same code and compile differently")
COMPILED, FAILED = "captured units compiled", "captured units failed"
# The captures of a check with a failed unit are kept beside the evidence folder, not in it: they are the
# game's code, and the evidence folder is what gets copied to shared places. 0 switches the keeping off.
KEEP_ENV = "WORKBENCH_KEEP_FAILED_CAPTURES"
KEPT_FOLDER, KEPT_NAME = "private", "overlay-captures-of-failed-check.json"
FAILURES_KEPT = 20
FAIL = re.compile(r"SHARD FAIL \[([^\]]+)\] ([^\n]*)")
RESULT = re.compile(r"PSX_SHARD_RESULT ok=(\d+) failed=(\d+) skipped=(\d+)")
ERROR_LINE = re.compile(r"^.*\berror: .*$", re.M)
# The compiler names its source by full path, and the source is in a temporary folder: keep the file name.
SOURCE_FOLDER = re.compile(r"^\s*(?:[A-Za-z]:)?[^:\n]*[\\/](?=[^\\/:\n]+:\d+:)")
FLAVOR = re.compile(r"_f(\d+)$")
# The driver's header of each unit, and the counts it prints under it.
UNIT = re.compile(r"^Overlay  load=0x([0-9A-Fa-f]{8})\s+size=(\d+)\s+crc32=0x([0-9A-Fa-f]{8})", re.M)
UNIT_NAMED = re.compile(r"0x([0-9A-Fa-f]{8}) crc ([0-9A-Fa-f]{8})")
# The driver's own line for each compile: the program it ran comes first.
COMPILE_LINE = re.compile(r"^\s*compile: (.+?\.exe|\S+) -", re.M)


def time_limit(environ=None):
    """Seconds the check may take, or None when the operator switched it off with 0."""
    text = (os.environ if environ is None else environ).get(LIMIT_ENV)
    if text is None or not str(text).strip():
        return TIME_LIMIT_SECONDS
    try:
        value = float(text)
    except ValueError:
        raise ValueError(f"{LIMIT_ENV} must be a number of seconds, or 0 for no check; it is {text!r}.") from None
    if value < 0:
        raise ValueError(f"{LIMIT_ENV} must not be negative; it is {text!r}.")
    return value or None


def captured_overlays(product, target):
    """Write every overlay the starts captured into one captures file at `target`; return what was read.

    The runtime keeps one newest snapshot (`overlay_captures.json`) and a history folder beside it
    (`overlay_captures.json.d`, one file per snapshot it wrote). It runs its driver on the newest snapshot
    each time, so a start's compiles cover many snapshots; the driver itself never reads the history. A
    region whose bytes changed is in several snapshots with different contents: each content is its own unit.
    The same content seen again later carries more observed entry points, so the last one is kept.
    A history write the runtime could not finish (PS1B-378) is simply not there.
    """
    product = Path(product)
    latest = product / "overlay_captures.json"
    history = product / "overlay_captures.json.d"
    files = sorted(history.glob("*.json"), key=lambda path: (path.stat().st_mtime, path.name)) if history.is_dir() else []
    files.append(latest)
    kept, read, unreadable, seen = {}, 0, 0, 0
    for file in files:
        try:
            data = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            unreadable += 1
            continue
        read += 1
        captures = data.get("captures") if isinstance(data, dict) else data
        for capture in captures or []:
            if not isinstance(capture, dict):
                continue
            seen += 1
            key = (capture.get("load_addr"), capture.get("size"),
                   hashlib.sha256(str(capture.get("bytes_b64")).encode("ascii", "replace")).hexdigest())
            kept[key] = capture
    Path(target).write_text(json.dumps(list(kept.values())), encoding="utf-8")
    regions = Counter(str(capture.get("load_addr")) for capture in kept.values())
    return {"snapshots": read, "snapshots_unreadable": unreadable, "captures_seen": seen, "captures": len(kept),
            "regions": len(regions), "versions_of_the_commonest_region": max(regions.values()) if regions else 0}


def driver_command(product, jobs=None, captures=None):
    """The preflight command for a product folder, as the runtime builds its compile command
    (runtime/src/main.cpp, build_toolchain_cmd), plus --check. `captures` is the captures file to
    compile (default: the product's newest snapshot). Raises ValueError with the reason when the
    product cannot be asked."""
    product = Path(product)
    toolchain = product / "overlay_toolchain"
    python = toolchain / "python" / ("python.exe" if os.name == "nt" else "bin/python3")
    recompiler = toolchain / ("psxrecomp-game.exe" if os.name == "nt" else "psxrecomp-game")
    driver, hint = toolchain / "compile_overlays.py", toolchain / "compiler.txt"
    for needed in (python, driver, recompiler, hint):
        if not needed.is_file():
            raise ValueError(f"the product has no {needed.relative_to(product).as_posix()}")
    compiler = hint.read_text(encoding="utf-8").strip()
    if not compiler:
        raise ValueError("overlay_toolchain/compiler.txt names no compiler")
    compiler = compiler if Path(compiler).is_absolute() else str(toolchain / compiler)
    command = [str(python), "-u", "-B", str(driver),
               "--captures", str(captures or product / "overlay_captures.json"),
               "--game-toml", str(product / "game.toml"),
               "--recompiler", str(recompiler), "--runtime-include", str(toolchain / "include"),
               "--project-root", str(toolchain), "--out-dir", str(product / "cache"),
               "--compiler", "gcc", "--gcc", compiler, "--check"]
    if jobs:
        command += ["--jobs", str(int(jobs))]
    return command


def cache_flavor(product):
    """The codegen flavor of the product's own cache folders (`…_f0`), or None when it has none yet."""
    for folder in sorted((Path(product) / "cache").glob("*/gcc/*/*")) if (Path(product) / "cache").is_dir() else []:
        found = FLAVOR.search(folder.name)
        if found and folder.is_dir():
            return found.group(1)
    return None


def units(text):
    """Every unit the driver listed, in its order: load address, CRC, size, and what its source was made from
    (seeds, executed PCs, function entries). The same bytes with other counts are another source."""
    found = []
    starts = list(UNIT.finditer(text))
    for index, match in enumerate(starts):
        block = text[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(text)]

        def count(pattern):
            got = re.search(pattern, block, re.M)
            return int(got.group(1)) if got else None
        found.append({"load": match.group(1).upper(), "crc": match.group(3).upper(), "size": int(match.group(2)),
                      "seeds": count(r"^\s*seeds: (\d+)"), "executed_pcs": count(r"^executed_pcs: (\d+)"),
                      "function_entry_pcs": count(r"^function_entry_pcs: (\d+)")})
    return found


def write_units(text, path):
    """The unit table of a check log, as a file beside it: one row per unit, with the class of its failure."""
    failed = {}
    for kind, rest in FAIL.findall(text):
        named = UNIT_NAMED.search(rest)
        if named:
            failed[(named.group(1).upper(), named.group(2).upper())] = kind
    rows = ["unit\tsize\tseeds\texecuted_pcs\tfunction_entry_pcs\tfailed_class"]
    listed = units(text)
    for unit in listed:
        rows.append("\t".join("" if value is None else str(value) for value in (
            f"0x{unit['load']} crc {unit['crc']}", unit["size"], unit["seeds"], unit["executed_pcs"],
            unit["function_entry_pcs"], failed.get((unit["load"], unit["crc"])))))
    Path(path).write_text("\n".join(rows) + "\n", encoding="utf-8")
    return len(listed)


def read_log(text):
    """What the driver's output says: the counts, the classes, and the first failures with their error line."""
    fails = FAIL.findall(text)
    line = RESULT.search(text)
    listed = {(unit["load"], unit["crc"]): unit for unit in units(text)}
    # A unit's own compiler text stands before its SHARD FAIL line: everything since the SHARD FAIL line
    # before it. The error is the first `error:` line of that text; a compile prints it once and then, for
    # the PS1B-124 fault, thousands of lines of warnings (Policenauts: 10,500 lines between the two).
    # With several jobs the units' texts can interleave; the line is then another unit's, which the log shows.
    pieces = re.split(r"(?m)^.*SHARD FAIL \[", text)
    failures = []
    for index, (kind, rest) in enumerate(fails[:FAILURES_KEPT]):
        found = ERROR_LINE.search(pieces[index]) if index < len(pieces) else None
        unit, _, detail = rest.partition(": ")
        error = found.group(0).strip() if found else detail.strip() or rest.strip()
        named = UNIT_NAMED.search(unit)
        about = listed.get((named.group(1).upper(), named.group(2).upper()), {}) if named else {}
        failures.append({"class": kind, "unit": unit.strip()[:120], "error": SOURCE_FOLDER.sub("", error)[:400],
                         # What this unit's source was made from: the next start may observe other entry points.
                         "seeds": about.get("seeds"), "executed_pcs": about.get("executed_pcs")})
    ran = COMPILE_LINE.search(text)
    record = {"shard_fail_lines": len(fails), "fail_classes": dict(Counter(kind for kind, _ in fails)),
              "failures": failures, "result_line": line.group(0) if line else None, "units_listed": len(listed),
              # The compiler the driver ran, read from its own first compile line (None when it compiled nothing).
              "compiler_run": ran.group(1) if ran else None}
    if line:
        compiled, failed, skipped = (int(line.group(n)) for n in (1, 2, 3))
        record.update(attempted=compiled + failed, compiled=compiled, failed=failed, skipped=skipped)
    return record


def stop_tree(child):
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], capture_output=True)
    else:
        child.kill()
    child.wait()


def run(product, evidence, jobs=None, environ=None):
    """Ask the product's driver; return the record. Never raises: a check that cannot run says why."""
    record = {"schema": 1}
    began = time.monotonic()
    try:
        limit = time_limit(environ)
        if limit is None:
            return {**record, "status": "not run", "reason": f"{LIMIT_ENV} is 0"}
        record["time_limit_seconds"] = limit
        product, evidence = Path(product), Path(evidence)
        if not (product / "overlay_toolchain").is_dir():
            # A product that compiles no overlays has nothing to check; no warning.
            return {**record, "status": "no toolchain"}
        if not (product / "overlay_captures.json").is_file():
            # No start captured an overlay: the compiler was given nothing.
            return {**record, "status": "no captures", "attempted": 0, "compiled": 0, "failed": 0, "skipped": 0}
        log = evidence / LOG
        scratch = Path(tempfile.mkdtemp(prefix="wb-overlay-check-"))
        env = dict(os.environ if environ is None else environ)
        # The driver's throwaway folder goes where this check can remove it, also after a time limit.
        env.update(TEMP=str(scratch), TMP=str(scratch), TMPDIR=str(scratch))
        for name in ("PSX_OVERLAY_CACHE_DIR", "PSX_OVERLAY_CAPTURES"):
            env.pop(name, None)
        flavor = cache_flavor(product)
        if flavor is not None:
            env["PSX_OVERLAY_FLAVOR"] = flavor  # as the runtime sets it for its own compiles
        timed_out = False
        text = ""
        try:
            merged = scratch / "captured-overlays.json"
            record["captured"] = captured_overlays(product, merged)
            command = driver_command(product, jobs, merged)
            # The compiler the product names in overlay_toolchain/compiler.txt: the one its own starts use
            # ("gcc tier using bundled toolchain ... with <this>" in a start log). The check gives the driver
            # the same one, so a failure here is a failure of the product's own compiles.
            record["compiler"] = command[command.index("--gcc") + 1] if "--gcc" in command else None
            with open(log, "wb") as stream:
                child = subprocess.Popen(command, cwd=product, env=env, stdout=stream, stderr=subprocess.STDOUT)
                try:
                    child.wait(timeout=limit)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    stop_tree(child)
            text = log.read_text(encoding="utf-8", errors="replace")
            if FAIL.search(text) and (env.get(KEEP_ENV) or "1").strip() != "0":
                # A failure comes and goes with what a start observed: without these captures it cannot be
                # compiled again (Rhapsody's failing captures of 2026-10-02 were not kept).
                kept = evidence.parent / KEPT_FOLDER / KEPT_NAME
                kept.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(merged, kept)
                record["captures_kept"] = str(kept)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        record.update(read_log(text))
        record.update(log=str(log), exit_code=child.returncode, jobs=jobs, scope=SCOPE)
        write_units(text, evidence / UNITS_TABLE)
        record["units_table"] = str(evidence / UNITS_TABLE)
        if timed_out:
            record["status"] = "incomplete"
            record["reason"] = f"the check reached its time limit of {limit:g} s"
        elif record.get("result_line") is None:
            record["status"] = "no result"
            record["reason"] = f"the driver ended with exit {child.returncode} and printed no result line"
        else:
            record["status"] = FAILED if record["failed"] else COMPILED
    except Exception as error:  # the check is evidence, not a gate: it must not end a build
        record.update(status="not run", reason=f"{type(error).__name__}: {error}")
    finally:
        record["seconds"] = round(time.monotonic() - began, 1)
    return record


def warning(record):
    """One line for a build's warnings, or None when the check has nothing to say."""
    status = record.get("status")
    first = (record.get("failures") or [{}])[0]
    named = f" First: [{first.get('class')}] {first.get('unit')}: {first.get('error')}" if first else ""
    classes = ", ".join(f"{count} {name}" for name, count in sorted((record.get("fail_classes") or {}).items()))
    seeds = f" (its source was made from {first.get('seeds')} seeds)" if first and first.get("seeds") is not None else ""
    if status == FAILED:
        return (f"{record['failed']} of the {record['attempted']} overlay units these starts captured do not compile "
                f"({classes}). They run interpreted.{named}{seeds} Another start can capture other entry points and "
                f"compile differently. The whole compiler output is in {LOG}, every unit in {UNITS_TABLE} (PS1B-380).")
    if status == "incomplete":
        seen = record.get("shard_fail_lines") or 0
        return (f"The overlay compile check did not finish: {record.get('reason')}. Until then {seen} unit(s) had "
                f"failed{(' (' + classes + ')') if classes else ''}.{named if seen else ''} A title with this many "
                f"captured overlay versions wants a kit review; the output so far is in {LOG} (PS1B-380).")
    if status == "no result":
        return (f"The overlay compile check gave no result: {record.get('reason')}. Read {LOG} (PS1B-380).")
    if status == "not run" and record.get("reason") != f"{LIMIT_ENV} is 0":
        return f"The overlay compile check did not run: {record.get('reason')} (PS1B-380)."
    return None


def record_in(receipt, record):
    """Put the check into a build receipt, with its warning. Nothing here stops a build."""
    receipt["overlay_compile_check"] = record
    line = warning(record)
    if line:
        receipt.setdefault("warnings", []).append(line)
    return record
