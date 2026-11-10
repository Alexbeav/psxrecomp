"""Live tool output and measured stage records for the local Workbench."""
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import queue
import re
import socket
import statistics
import subprocess
import sys
import threading
import time
import uuid

PREFIX = "WORKBENCH_EVENT "
CHILD_PREFIX = "WORKBENCH_CHILD "


class ReaderGoneTolerant:
    """A stdout/stderr that keeps the build alive when its reader goes away (PS1B-307).

    A build's console output is progress only; the receipt, stage history and
    logs go to files. When the process or window reading the pipe dies (a
    stopped queue shell, a crashed Studio), writes raise OSError (EINVAL on
    Windows, EPIPE elsewhere) and used to abort the build at its next progress
    line. Drop the output from then on and keep building.
    """

    def __init__(self, inner):
        self._inner = inner
        self.reader_gone = False

    def write(self, text):
        if not self.reader_gone:
            try:
                return self._inner.write(text)
            except (OSError, ValueError):
                self.reader_gone = True
        return len(text)

    def flush(self):
        if not self.reader_gone:
            try:
                self._inner.flush()
            except (OSError, ValueError):
                self.reader_gone = True

    def __getattr__(self, name):
        return getattr(self._inner, name)


def configure_stdio():
    """Owned CLI pipes carry UTF-8 even when Windows redirects a legacy locale,
    and a closed pipe no longer ends the build (PS1B-307)."""
    for name in ("stdout", "stderr"):
        pipe = getattr(sys, name)
        if isinstance(pipe, ReaderGoneTolerant):
            continue
        if hasattr(pipe, "reconfigure"):
            pipe.reconfigure(encoding="utf-8", errors="strict", write_through=True)
        setattr(sys, name, ReaderGoneTolerant(pipe))


def stream(argv, cwd=None, env=None, on_line=None, on_tick=None, **kwargs):
    """Drain both pipes concurrently, including tools that redraw with CR."""
    child_env = dict(os.environ if env is None else env, PYTHONUNBUFFERED="1",
                     PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    with subprocess.Popen(argv, cwd=cwd, env=child_env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs) as process:
        messages = queue.Queue()
        captured = {"stdout": [], "stderr": []}
        def read(channel, pipe):
            try:
                while data := pipe.read1(4096):
                    messages.put((channel, data))
            finally:
                messages.put((channel, None))
        threads = [threading.Thread(target=read, args=(name, getattr(process, name)), daemon=True)
                   for name in captured]
        for thread in threads:
            thread.start()
        pending = {name: b"" for name in captured}
        finished = 0
        try:
            while finished < 2:
                try:
                    channel, data = messages.get(timeout=1)
                except queue.Empty:
                    if on_tick:
                        on_tick()
                    continue
                if data is None:
                    finished += 1
                    if pending[channel] and on_line:
                        on_line(channel, pending[channel].decode("utf-8", errors="replace"))
                    continue
                captured[channel].append(data)
                parts = re.split(b"[\r\n]", pending[channel] + data)
                pending[channel] = parts.pop()
                for line in parts:
                    if line and on_line:
                        on_line(channel, line.decode("utf-8", errors="replace"))
        except BaseException:
            process.kill()
            process.wait()
            raise
        for thread in threads:
            thread.join()
        return subprocess.CompletedProcess(argv, process.wait(),
            b"".join(captured["stdout"]), b"".join(captured["stderr"]))


def history_key(receipt, stage_id):
    return json.dumps([stage_id, receipt.get("selected_disc_paths"), receipt.get("kit"), receipt.get("pin", {}).get("commit"),
                       receipt.get("pin", {}).get("toolchain_pin"), receipt.get("output_kind"),
                       receipt.get("input_mode"), receipt.get("optimize_pgo", False),
                       (receipt.get("overlay_compiler") or {}).get("kind")], sort_keys=True)


def read_history(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if data.get("schema") == 1 and data.get("host") == socket.gethostname() and isinstance(data.get("stages"), dict):
            return data
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return {"schema": 1, "host": socket.gethostname(), "stages": {}}


def stage(receipt, output, stage_id, status="running", *, label=None, log=None, progress=None, detail=None, start=False, checkpoint=False):
    records = receipt.setdefault("stages", [])
    row = next((r for r in reversed(records) if r["id"] == stage_id), None)
    if start:
        row = None
    now = time.monotonic()
    if row is None:
        history_path = Path(receipt.get("stage_history", Path(output).parent / "workbench-stage-history.json"))
        receipt["stage_history"] = str(history_path.resolve())
        key = history_key(receipt, stage_id)
        samples = read_history(history_path)["stages"].get(key, [])
        samples = samples if isinstance(samples, list) else []
        values = [s["elapsed_seconds"] for s in samples if isinstance(s, dict)
                  and isinstance(s.get("elapsed_seconds"), (int, float))
                  and math.isfinite(s["elapsed_seconds"]) and s["elapsed_seconds"] > 0]
        occurrence = 1 + sum(r["stage"] == stage_id for r in records)
        row = {"schema": 1, "event": "stage",
               "id": stage_id if occurrence == 1 else f"{stage_id}#{occurrence}",
               "stage": stage_id, "scope": stage_id.split(":")[0] if ":" in stage_id else "build",
               "occurrence": occurrence, "label": label or stage_id,
               "timing_basis": "tool completion checkpoint" if checkpoint else "measured wall time",
               "status": status, "elapsed_seconds": 0, "progress": None,
               "estimate_seconds": statistics.median(values) if values and status == "running" else None,
               "log": None, "stop_file": str((Path(output) / "HUMAN-STOP.txt").resolve()),
               "detail": None, "_started": now, "_history_key": key}
        records.append(row)
    row.update(status=status, elapsed_seconds=None if checkpoint else round(now - row["_started"], 3))
    if label:
        row["label"] = label
    if log:
        row["log"] = str(Path(log).resolve())
    if progress is not None:
        row["progress"] = progress
    if detail is not None:
        row["detail"] = detail
    if status == "failed":
        receipt.setdefault("failed_stage", row["id"])
    print(PREFIX + json.dumps({k: v for k, v in row.items() if not k.startswith("_")}), flush=True)
    return row


def finish(receipt, output, failed=False, detail=None):
    for row in receipt.get("stages", []):
        if row["status"] == "running":
            stage(receipt, output, row["id"], "failed" if failed else "done", detail=detail)
    path = Path(receipt.get("stage_history", Path(output).parent / "workbench-stage-history.json"))
    history = read_history(path)
    for row in receipt.get("stages", []):
        if "_history_key" not in row:
            continue
        if row["status"] == "done" and row["elapsed_seconds"] is not None and row["elapsed_seconds"] > 0:
            samples = history["stages"].get(row["_history_key"], [])
            samples = samples if isinstance(samples, list) else []
            samples.append({"elapsed_seconds": row["elapsed_seconds"],
                            "receipt": str((Path(output) / "build-receipt.json").resolve())})
            history["stages"][row["_history_key"]] = samples[-10:]
        row.pop("_started", None)
        row.pop("_history_key", None)
    try:
        temp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
        temp.write_text(json.dumps(history, indent=2) + "\n", encoding="utf-8")
        temp.replace(path)
    except OSError as error:
        receipt["stage_history_warning"] = str(error)


def tool_progress(line):
    try:
        event = json.loads(line)
        if event.get("event") == "phase" and isinstance(event.get("pct"), (int, float)):
            return {"current": event["pct"] * 100, "total": 100, "unit": "percent",
                    "message": event.get("message", "Tool phase"), "basis": "tool phase checkpoint"}
    except (ValueError, AttributeError):
        pass
    if match := re.search(r"\[(\d+)/(\d+)\]\s*(.*)", line):
        return {"current": int(match[1]), "total": int(match[2]), "unit": "items", "message": match[3]}
    if match := re.search(r"(\d+(?:\.\d+)?)%", line):
        return {"current": float(match[1]), "total": 100, "unit": "percent", "message": line}
    return None


@contextmanager
def measured(receipt, output, name, label=None):
    row = stage(receipt, output, name, label=label, start=True)
    try:
        yield row
    except Exception as error:
        stage(receipt, output, row["id"], "failed", detail=str(error))
        raise
    else:
        stage(receipt, output, row["id"], "done")
