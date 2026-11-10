#!/usr/bin/env python3
"""Run and classify one owned process session without killing unrelated work."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CLASSIFICATIONS = {
    "function_table_gap",
    "guest_access_error",
    "stall_deadlock",
    "clean_exit",
    "harness_termination",
    "unknown",
}


class SessionError(RuntimeError):
    """A monitored session could not be started or bundled."""


def classify(returncode: int | None, timed_out: bool, output: str) -> str:
    lowered = output.lower()
    if timed_out:
        return "stall_deadlock"
    if "function table" in lowered or "unknown dispatch" in lowered:
        return "function_table_gap"
    if "guest access" in lowered or "unmapped guest" in lowered:
        return "guest_access_error"
    if returncode == 0:
        return "clean_exit"
    if returncode is not None and returncode < 0:
        return "harness_termination"
    return "unknown"


def _terminate_owned(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True, check=False, text=True,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def run(command: list[str], working_directory: Path, bundle: Path,
        *, timeout_seconds: float, environment: dict[str, str] | None = None,
        heartbeat: Path | None = None, stall_seconds: float | None = None) -> dict[str, Any]:
    if not command or timeout_seconds <= 0:
        raise SessionError("command and positive timeout_seconds are required")
    if bundle.exists():
        raise SessionError("session bundle already exists")
    temporary = bundle.with_name(bundle.name + ".tmp-" + uuid.uuid4().hex)
    temporary.mkdir(parents=True, exist_ok=False)
    stdout_path = temporary / "stdout.log"
    stderr_path = temporary / "stderr.log"
    started_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    start = time.monotonic()
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    popen_kwargs: dict[str, Any] = {}
    if os.name != "nt":
        popen_kwargs["start_new_session"] = True
    timed_out = False
    with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(
            command,
            cwd=working_directory,
            env=environment,
            stdout=stdout,
            stderr=stderr,
            text=True,
            shell=False,
            creationflags=creationflags,
            **popen_kwargs,
        )
        last_heartbeat = time.monotonic()
        last_heartbeat_mtime = heartbeat.stat().st_mtime_ns if heartbeat and heartbeat.exists() else None
        while process.poll() is None:
            elapsed = time.monotonic() - start
            if elapsed >= timeout_seconds:
                timed_out = True
                break
            if heartbeat is not None and heartbeat.exists():
                current = heartbeat.stat().st_mtime_ns
                if current != last_heartbeat_mtime:
                    last_heartbeat_mtime = current
                    last_heartbeat = time.monotonic()
            if stall_seconds is not None and time.monotonic() - last_heartbeat >= stall_seconds:
                timed_out = True
                break
            time.sleep(0.05)
        if timed_out:
            _terminate_owned(process)
        try:
            returncode = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _terminate_owned(process)
            returncode = process.poll()
    output = stdout_path.read_text(encoding="utf-8", errors="replace") + "\n" + stderr_path.read_text(
        encoding="utf-8", errors="replace")
    classification = classify(returncode, timed_out, output)
    receipt = {
        "schema_version": 1,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "owned_root_pid": process.pid,
        "command": command,
        "returncode": returncode,
        "timed_out": timed_out,
        "classification": classification,
        "wall_seconds": time.monotonic() - start,
        "stdout": "stdout.log",
        "stderr": "stderr.log",
    }
    (temporary / "session.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    bundle.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(temporary), str(bundle))
    receipt["bundle"] = str(bundle.resolve())
    return receipt

