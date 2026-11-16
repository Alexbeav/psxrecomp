"""Where Workbench run folders live, and what each one keeps (PS1B-328).

Every build owns one `run-*` folder: `work` (the build tree) and `evidence`.
A pass already removed `work`; a stop kept it, and every build that reached
the boot gate kept `evidence/validation-transients` (the gate's overlay
caches). Nothing ever removed either, so 613 runs grew to 42 GB and filled
the system drive.

Retention, applied when a build's receipt is written:
- a pass replaces `validation-transients` with a small manifest;
- stopped runs keep `work` for the newest KEEP_FAILED_WORK of them;
- receipts, logs, reports and failed payloads are never removed;
- a run with no final receipt (still building) or a KEEP marker is left alone.

A run whose build process is gone (PS1B-351). On Eagle, 2026-10-01, a build's
whole process tree vanished two minutes in. Its receipt said "started" for ever
and nothing reported it for 95 minutes. Every run now records the process that
builds it (`owner.json`). A run with no final receipt whose process no longer
exists is dead: the listing says so, and retention writes the final receipt the
build never wrote, as a stop that names what happened.
"""
import ctypes
from datetime import datetime
import json
import os
import shutil
import socket
import sys
import tempfile
from pathlib import Path

try:
    from . import pin_build as pb
except ImportError:
    import pin_build as pb

RUN_ROOT_ENV = "WORKBENCH_RUN_ROOT"
KEEP_FAILED_WORK_ENV = "WORKBENCH_KEEP_FAILED_WORK"
MIN_FREE_GB_ENV = "WORKBENCH_MIN_FREE_GB"
KEEP_FAILED_WORK = 10
MIN_FREE_GB = 5.0
KEEP_MARKER = "KEEP"
RECEIPT = "build-receipt.json"
# A run that only found that its discs are the programs of one set and handed
# over to the set build (PS1B-333). Not a pass and not a failure.
PLAN_STATUS = "program-set-plan"
TRANSIENTS = "validation-transients"
OWNER = "owner.json"
DEAD_TEXT = "process ended without a result"


def run_root(explicit=None, environ=None):
    """The folder that holds run folders: --run-root, then WORKBENCH_RUN_ROOT,
    then %LOCALAPPDATA%/PSXWorkbench/builds."""
    environ = os.environ if environ is None else environ
    if explicit:
        return Path(explicit)
    if environ.get(RUN_ROOT_ENV):
        return Path(environ[RUN_ROOT_ENV])
    return Path(environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "PSXWorkbench/builds"


def _number(environ, name, default, convert):
    try:
        return convert(environ[name])
    except (KeyError, ValueError):
        return default


def require_free_space(root, environ=None, usage=shutil.disk_usage):
    """Stop before a build starts on a drive that cannot hold its run folder."""
    environ = os.environ if environ is None else environ
    minimum = _number(environ, MIN_FREE_GB_ENV, MIN_FREE_GB, float)
    if minimum <= 0:
        return None
    probe = Path(root)
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    free = usage(probe).free / 1024 ** 3
    if free < minimum:
        raise pb.BuildStop(
            f"Only {free:.1f} GB is free on the drive that holds the Workbench run folders ({root}); a build "
            f"needs at least {minimum:g} GB. Free space there, or point {RUN_ROOT_ENV} or --run-root at another "
            "drive, then build again.")
    return round(free, 1)


def _tree(path):
    files = [p for p in Path(path).rglob("*") if p.is_file()]
    return len(files), sum(p.stat().st_size for p in files)


def remove_owned_tree(folder):
    def retry_readonly(_function, path, error):
        if not isinstance(error[1], PermissionError):
            raise error[1]
        pb.remove_owned_file(path)
    shutil.rmtree(folder, onerror=retry_readonly)


def drop_transients(evidence, receipt):
    """After a pass, the boot gate's overlay caches are only a cache: record
    what was there and remove it. The product already holds the usable cache
    pairs (collect_validated_payload)."""
    folder = Path(evidence) / TRANSIENTS
    if not folder.is_dir():
        return
    files, size = _tree(folder)
    record = {"removed": str(folder), "files": files, "bytes": size,
              "reason": "boot-gate overlay caches are not kept after a passed build (PS1B-328)"}
    try:
        remove_owned_tree(folder)
    except OSError as error:
        receipt.setdefault("warnings", []).append(
            f"The boot gate's overlay caches could not be removed from the run folder: {error}")
        return
    pb.write_json(Path(evidence) / (TRANSIENTS + ".removed.json"), record)
    receipt["validation_transients"] = record


UNASKABLE = -1


def process_token(pid):
    """What tells this process from a later one with the same number: its start time.

    None when no such process exists. UNASKABLE when one exists and this user
    may not ask about it: then nothing can be said about the run.
    """
    pid = int(pid)
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
        kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x1000, 0, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            # 87: no process has this number. Anything else (5, access denied): it exists.
            return None if ctypes.get_last_error() == 87 else UNASKABLE
        try:
            code = ctypes.c_uint32()
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value != 259:  # STILL_ACTIVE
                return None
            created, exited, kernel_time, user_time = (ctypes.c_uint64() for _ in range(4))
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                          ctypes.byref(kernel_time), ctypes.byref(user_time)):
                return UNASKABLE
            return created.value
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    except OSError:
        return UNASKABLE  # it exists and is someone else's
    try:  # Linux: field 22 of /proc/<pid>/stat is the start time in clock ticks
        return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
    except (OSError, ValueError, IndexError):
        return 0  # exists; this platform gives no start time to compare


def claim(run, output=None):
    """Record, in the run folder, the process that builds it. Never stops a build."""
    try:
        pb.write_json(Path(run) / OWNER, {
            "pid": os.getpid(), "process_token": process_token(os.getpid()), "host": socket.gethostname(),
            "output": str(output) if output is not None else None, "argv": sys.argv})
    except OSError:
        pass


def owner_state(run):
    """('alive' | 'dead' | 'unknown', owner record or None) for a run folder.

    'unknown' when the run has no owner record (a run from before this), when
    the record is from another host (a shared run root), or when it cannot be
    read. 'dead' only when this host can say that the recorded process is gone:
    the number is free, or it now belongs to a process that started at another
    time.
    """
    try:
        owner = json.loads((Path(run) / OWNER).read_text(encoding="utf-8"))
        pid = int(owner["pid"])
    except (OSError, ValueError, KeyError, TypeError):
        return "unknown", None
    if owner.get("host") != socket.gethostname():
        return "unknown", owner
    token = process_token(pid)
    if token == UNASKABLE:
        return "unknown", owner
    if token is None or (owner.get("process_token") not in (None, UNASKABLE) and token != owner["process_token"]):
        return "dead", owner
    return "alive", owner


def _last_activity(run):
    """When a file of the run was last written, read from the file system."""
    newest = 0.0
    for path in Path(run).rglob("*"):
        try:
            if path.is_file() and path.name != OWNER:
                newest = max(newest, path.stat().st_mtime)
        except OSError:
            pass
    return datetime.fromtimestamp(newest).isoformat(timespec="seconds") if newest else None


def mark_dead(run, owner):
    """Write the final receipt a dead run never wrote. Returns the receipt, or None if one is there."""
    run = Path(run)
    evidence = run / "evidence"
    receipt_path = evidence / RECEIPT
    if receipt_path.is_file():
        return None
    evidence.mkdir(exist_ok=True)
    stage = None
    try:  # the workspace receipt names the stage the build was in
        workspace = json.loads((run / "work" / RECEIPT).read_text(encoding="utf-8"))
        running = [row.get("id") for row in workspace.get("stages", []) if row.get("status") == "running"]
        stage = running[-1] if running else None
    except (OSError, ValueError, AttributeError):
        pass
    output = owner.get("output")
    left = []
    if output:
        target = Path(output)
        if (target / ".workbench-reservation").is_file():
            left.append(str(target / ".workbench-reservation"))
        if target.parent.is_dir():
            left += [str(p) for p in sorted(target.parent.glob(f".{target.name} *")) if p.is_dir()]
    text = (f"The build's {DEAD_TEXT}: process {owner.get('pid')} on {owner.get('host')} no longer exists and the "
            "run has no final receipt. It was ended from outside, or the host went down. Nothing was delivered."
            + (f" Its last stage was {stage}." if stage else "")
            + (" Left behind beside the output, remove before building there again: " + "; ".join(left) + "."
               if left else "")
            + " Build again; the run's logs are kept.")
    receipt = {"status": "human-stop", "dead": True, "next_action": text, "failed_stage": stage,
               "output": output, "logs": str(evidence), "receipt": str(receipt_path),
               "work_directory": str(run / "work"), "work_retained": (run / "work").is_dir(),
               "host": owner.get("host"), "owner": owner, "last_activity": _last_activity(run),
               "left_behind": left, "warnings": [],
               "found_dead_by": {"pid": os.getpid(), "argv": sys.argv}}
    (evidence / "HUMAN-STOP.txt").write_text(text + "\n", encoding="utf-8")
    receipt["stop_file"] = str(evidence / "HUMAN-STOP.txt")
    pb.write_json(receipt_path, receipt)
    return receipt


def _final_status(run):
    try:
        return json.loads((run / "evidence" / RECEIPT).read_text(encoding="utf-8")).get("status")
    except (OSError, ValueError, AttributeError):
        return None


def plan(root, keep_failed=None, current=None, environ=None):
    """Rows for every run folder under `root`: what it holds and what
    retention would remove. Read-only."""
    environ = os.environ if environ is None else environ
    keep = _number(environ, KEEP_FAILED_WORK_ENV, KEEP_FAILED_WORK, int) if keep_failed is None else keep_failed
    root = Path(root)
    rows = []
    for run in sorted((p for p in root.glob("run-*") if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True):
        status = _final_status(run)
        row = {"run": run.name, "path": str(run), "status": status,
               "work": (run / "work").is_dir(), "transients": (run / "evidence" / TRANSIENTS).is_dir(),
               "remove": [], "kept_because": None}
        if status is None:
            # Is anyone still building it? Only a run that recorded its process can be answered.
            state, owner = owner_state(run)
            if state != "unknown":
                row["owner"] = {"pid": owner.get("pid"), "host": owner.get("host"), "state": state}
            if state == "dead":
                row["dead"] = True
                row["status"] = "dead"
                row["note"] = f"{DEAD_TEXT}: process {owner.get('pid')} is gone and the run has no final receipt"
        if current is not None and run.resolve() == Path(current).resolve():
            row["kept_because"] = "this build"
        elif (run / KEEP_MARKER).exists():
            row["kept_because"] = f"{KEEP_MARKER} marker"
        elif row["status"] is None:
            row["kept_because"] = ("building (process %s)" % row["owner"]["pid"] if row.get("owner")
                                   else "no final receipt (still building, or interrupted)")
        rows.append(row)
    stopped = 0
    for row in rows:  # newest first
        if row["status"] == "build-pass":
            if row["kept_because"] is None and row["transients"]:
                row["remove"].append(TRANSIENTS)
            continue
        if row["status"] is None or row["status"] == PLAN_STATUS:
            continue
        stopped += 1
        if row["kept_because"] is None and row["work"] and keep >= 0 and stopped > keep:
            row["remove"].append("work")
    return rows


def prune(root, execute=False, keep_failed=None, current=None, environ=None):
    """Apply `plan`. Removes only `work` and `evidence/validation-transients`
    of finished runs; returns the rows with what was removed and its size."""
    root = Path(root).resolve()
    rows = plan(root, keep_failed, current, environ)
    for row in rows:
        row["removed_bytes"] = 0
        if execute and row.get("dead"):
            # The receipt the build never wrote, so every reader sees a stop and not "started".
            try:
                row["marked_dead"] = mark_dead(row["path"], owner_state(row["path"])[1] or {}) is not None
            except OSError as error:
                row["error"] = f"could not write the dead run's receipt: {error}"
        for part in row["remove"]:
            target = Path(row["path"]) / ("work" if part == "work" else Path("evidence") / TRANSIENTS)
            if target.resolve().parent not in (Path(row["path"]).resolve(), (Path(row["path"]) / "evidence").resolve()) \
                    or Path(row["path"]).resolve().parent != root:
                raise pb.BuildStop(f"Refusing to prune outside a run folder: {target}")
            row["removed_bytes"] += _tree(target)[1]
            if execute:
                try:
                    remove_owned_tree(target)
                except OSError as error:
                    row["error"] = str(error)
        if row.get("marked_dead") and not (Path(row["path"]) / "work").is_dir():
            # The receipt was written while the work tree was still there for its last stage.
            receipt_path = Path(row["path"]) / "evidence" / RECEIPT
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            receipt["work_retained"] = False
            pb.write_json(receipt_path, receipt)
    return rows


def after_build(root, run, evidence, receipt):
    """Retention at receipt time. Never fails the build: a cleanup problem
    becomes a warning."""
    try:
        if receipt.get("status") == "build-pass" and not receipt.get("keep_development_files"):
            drop_transients(evidence, receipt)
        rows = prune(root, execute=True, current=run)
        removed = [{"run": r["run"], "removed": r["remove"], "bytes": r["removed_bytes"]}
                   for r in rows if r["remove"]]
        if removed:
            receipt["run_retention"] = removed
        errors = [f"{r['run']}: {r['error']}" for r in rows if r.get("error")]
        if errors:
            receipt.setdefault("warnings", []).append("Run-folder retention could not remove: " + "; ".join(errors))
    except Exception as error:  # noqa: BLE001 - retention must not turn a build into a stop
        receipt.setdefault("warnings", []).append(f"Run-folder retention did not run: {error}")
