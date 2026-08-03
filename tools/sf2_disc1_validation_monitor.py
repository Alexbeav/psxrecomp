#!/usr/bin/env python3
"""Capture bounded semantic evidence for an interactive SF2 Disc 1 run.

The monitor never writes guest state and never captures retail pixels, RAM,
audio, or overlay payloads.  It drains the existing bounded presentation rings,
records state/classification transitions, and retains low-rate semantic samples
until the runtime endpoint closes.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import socket
import struct
import subprocess
import time
from collections import Counter
from pathlib import Path
from typing import Any


APP_STACK = 0x8011EE8C
PLAYER_POINTER = 0x8012A574
MAX_TRANSITIONS = 8192
MAX_SAMPLES = 32768


class DebugClient:
    def __init__(self, port: int) -> None:
        self.port = port
        self.ident = 0

    def call(self, command: str, **fields: Any) -> dict[str, Any]:
        self.ident += 1
        request = {"id": self.ident, "cmd": command, **fields}
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            sock.sendall((json.dumps(request) + "\n").encode("utf-8"))
            response = bytearray()
            while b"\n" not in response:
                chunk = sock.recv(1024 * 1024)
                if not chunk:
                    raise ConnectionError("debug endpoint closed")
                response.extend(chunk)
        result = json.loads(response.split(b"\n", 1)[0])
        if not result.get("ok"):
            raise RuntimeError(result.get("error", result.get("err", "debug error")))
        return result

    def read(self, address: int, length: int) -> bytes:
        result = self.call("read_ram", addr=hex(address), len=length)
        return bytes.fromhex(result["hex"])

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        query_limited_information = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            query_limited_information, False, pid
        )
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if not ctypes.windll.kernel32.GetExitCodeProcess(
                handle, ctypes.byref(exit_code)
            ):
                return False
            return exit_code.value == 259  # STILL_ACTIVE
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def git_identity(repo: Path) -> dict[str, Any]:
    def run(*args: str) -> str:
        return subprocess.check_output(
            ["git", *args], cwd=repo, text=True, encoding="utf-8"
        ).strip()

    return {
        "branch": run("branch", "--show-current"),
        "commit": run("rev-parse", "HEAD"),
        "dirty": bool(run("status", "--porcelain")),
    }


def app_state(client: DebugClient) -> dict[str, Any]:
    depth, state, transition, callback = struct.unpack(
        "<IIII", client.read(APP_STACK, 16)
    )
    return {
        "depth": depth,
        "state": state,
        "transition": transition,
        "callback": f"0x{callback:08X}",
    }


def player_state(client: DebugClient) -> dict[str, Any] | None:
    player = client.u32(PLAYER_POINTER)
    if not player:
        return None
    record = client.u32(player + 0x20)
    if not record:
        return {"player": f"0x{player:08X}", "record": "0x00000000"}
    wrapper = client.u32(record + 0xE0)
    owner = client.u32(wrapper + 0xDC) if wrapper else 0
    return {
        "player": f"0x{player:08X}",
        "record": f"0x{record:08X}",
        "wrapper": f"0x{wrapper:08X}",
        "owner": f"0x{owner:08X}",
        "player_owns_camera": owner == player,
    }


def runtime_health(client: DebugClient) -> dict[str, Any]:
    """Return bounded cumulative ownership/device evidence without payloads."""
    resident = client.call("dispatch_stats")
    overlay = client.call("overlay_loader_status")
    cdrom = client.call("cdrom_state")
    spu = client.call("spu_status")
    audio = client.call("audio_stats")
    pad = client.call("pad_status")
    return {
        "dispatch": {
            "resident_aot": int(resident["static_hits"]),
            "resident_misses": int(resident["miss_total"]),
            "overlay_native": int(overlay["dispatch_native"]),
            "interpreter_fallback": int(overlay["dispatch_interp_fallback"]),
            "regions": int(overlay["regions"]),
            "loads": int(overlay["loads"]),
            "invalidations": int(overlay["invalidations"]),
            "revalidations": int(overlay["revalidations"]),
            "stale_blocked": int(overlay["stale_blocked"]),
            "candidate_overflow": int(overlay["candidate_overflow"]),
        },
        "cdrom": {
            "seq": int(cdrom["seq"]),
            "int1_lost": int(cdrom["int1_lost"]),
            "last_lba": int(cdrom["last_sector"]["lba"]),
            "last_size": int(cdrom["last_sector"]["size"]),
        },
        "spu": {
            "key_on_count": int(spu["key_on_count"]),
            "render_frames": int(spu["render_frames"]),
            "nonzero_frames": int(spu["nonzero_frames"]),
            "peak": int(spu["peak"]),
        },
        "audio": {
            tap["name"]: {
                "frames": int(tap["frames"]),
                "nonzero": int(tap["nonzero"]),
                "peak": int(tap["peak"]),
            }
            for tap in audio["taps"]
        },
        "pad": {
            "word": str(pad["pad"]),
            "connected": bool(pad["slot0"]["connected"]),
            "analog": bool(pad["slot0"]["analog"]),
            "sticks": [int(value) for value in pad["slot0"]["sticks"]],
        },
    }


def key_for(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def append_transition(
    target: list[dict[str, Any]], frame: int, value: Any, prior: str | None
) -> str:
    current = key_for(value)
    if current != prior and len(target) < MAX_TRANSITIONS:
        target.append({"frame": frame, "value": value})
    return current


def drain_present(
    response: dict[str, Any], next_seq: int, counts: Counter[str],
    shapes: Counter[str], transitions: list[dict[str, Any]], prior: str | None,
) -> tuple[int, str | None]:
    total = int(response["total"])
    for event in response["events"]:
        seq = int(event[0])
        if seq < next_seq:
            continue
        if seq > next_seq:
            counts["sequence_events_missed"] += seq - next_seq
        frame, path = int(event[1]), str(event[2])
        value = {
            "path": path,
            "present_w": int(event[3]),
            "display": [int(event[4]), int(event[5])],
            "game_mode": int(event[6]),
            "native_43": int(event[7]),
            "fellback": int(event[8]),
            "nw_extra": int(event[10]),
        }
        counts[path] += 1
        if value["fellback"]:
            counts["wide_fellback"] += 1
        shapes[key_for(value)] += 1
        prior = append_transition(transitions, frame, value, prior)
        next_seq = seq + 1
    return max(next_seq, total if not response["events"] else next_seq), prior


def drain_gl(
    response: dict[str, Any], next_seq: int, counts: Counter[str],
    shapes: Counter[str], transitions: list[dict[str, Any]], prior: str | None,
) -> tuple[int, str | None]:
    total = int(response["total"])
    for event in response["events"]:
        seq = int(event[0])
        if seq < next_seq:
            continue
        if seq > next_seq:
            counts["sequence_events_missed"] += seq - next_seq
        frame, path = int(event[1]), str(event[2])
        value = {
            "path": path,
            "source": event[4],
            "destination": event[5],
        }
        counts[path] += 1
        shapes[key_for(value)] += 1
        prior = append_transition(transitions, frame, value, prior)
        next_seq = seq + 1
    return max(next_seq, total if not response["events"] else next_seq), prior


def timeline_identity(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": path.name, "exists": path.is_file()}
    if not path.is_file():
        return result
    data = path.read_bytes()
    result["size"] = len(data)
    result["sha256"] = hashlib.sha256(data).hexdigest()
    if len(data) >= 32 and data[:8] == b"PSXPAD1\0":
        result["samples"] = struct.unpack_from("<Q", data, 20)[0]
        result["structurally_valid"] = (
            len(data) == 32 + 32 * result["samples"]
        )
    else:
        result["structurally_valid"] = False
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--pid", type=int, default=0)
    parser.add_argument("--poll", type=float, default=2.0)
    args = parser.parse_args()

    client = DebugClient(args.port)
    deadline = time.monotonic() + 60.0
    while True:
        try:
            client.call("frame")
            break
        except (OSError, RuntimeError, json.JSONDecodeError):
            if time.monotonic() >= deadline:
                raise TimeoutError("runtime debug endpoint did not open")
            time.sleep(0.25)

    evidence: dict[str, Any] = {
        "schema": 1,
        "result": "recording",
        "git": git_identity(args.repo),
        "identity": {
            "executable_sha256": sha256(args.exe),
            "game_config_sha256": sha256(args.game),
            "bios_sha256": sha256(args.bios),
            "settings_sha256": sha256(args.settings),
        },
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "present": {"counts": {}, "shapes": {}, "transitions": []},
        "gl_present": {"counts": {}, "shapes": {}, "transitions": []},
        "application_transitions": [],
        "widescreen_transitions": [],
        "fullscreen_rect_events": [],
        "samples": [],
        "sampling_failures": 0,
    }
    present_counts: Counter[str] = Counter()
    present_shapes: Counter[str] = Counter()
    gl_counts: Counter[str] = Counter()
    gl_shapes: Counter[str] = Counter()
    present_seq = gl_seq = 0
    present_prior = gl_prior = app_prior = ws_prior = None
    last_sample_frame = -600
    last_fullscreen_expands = 0
    consecutive_failures = 0
    last_snapshot: dict[str, Any] = {}

    while True:
        try:
            frame = int(client.call("frame")["frame"])
            gpu = client.call("gpu_state")
            app = app_state(client)
            player = player_state(client)
            present_seq, present_prior = drain_present(
                client.call("present_ring", n=512), present_seq,
                present_counts, present_shapes,
                evidence["present"]["transitions"], present_prior,
            )
            gl_seq, gl_prior = drain_gl(
                client.call("gl_present_ring", n=512), gl_seq,
                gl_counts, gl_shapes,
                evidence["gl_present"]["transitions"], gl_prior,
            )
            app_prior = append_transition(
                evidence["application_transitions"], frame, app, app_prior
            )
            ws = gpu["ws"]
            ws_value = {
                "display": [gpu["width"], gpu["height"], gpu["depth"]],
                "configured": ws["configured"],
                "active": ws["active"],
                "game_mode": ws["game_mode"],
                "native_43": ws["present_native_43"],
                "mode": ws["mode"],
                "nw_extra": ws["nw_extra"],
            }
            ws_prior = append_transition(
                evidence["widescreen_transitions"], frame, ws_value, ws_prior
            )
            fullscreen = ws.get("fullscreen_rect", {})
            expanded = int(fullscreen.get("expanded", 0))
            if expanded != last_fullscreen_expands:
                if len(evidence["fullscreen_rect_events"]) < MAX_TRANSITIONS:
                    evidence["fullscreen_rect_events"].append({
                        "frame": frame,
                        "checks": int(fullscreen.get("checks", 0)),
                        "expanded": expanded,
                        "delta": expanded - last_fullscreen_expands,
                        "app": app,
                    })
                last_fullscreen_expands = expanded
            if frame - last_sample_frame >= 600 and len(evidence["samples"]) < MAX_SAMPLES:
                evidence["samples"].append({
                    "frame": frame,
                    "app": app,
                    "player": player,
                    "gpu": ws_value,
                    "gte_verts": ws["gte_verts"],
                    "fullscreen_rect": fullscreen,
                    "runtime": runtime_health(client),
                    "mouse": client.call("mouse_camera_stats"),
                })
                last_sample_frame = frame
            last_snapshot = {
                "frame": frame,
                "app": app,
                "player": player,
                "gpu": gpu,
            }
            consecutive_failures = 0
        except (OSError, RuntimeError, json.JSONDecodeError, struct.error):
            consecutive_failures += 1
            evidence["sampling_failures"] += 1
            if args.pid and not process_alive(args.pid):
                break
            if not args.pid and consecutive_failures >= 5:
                break
        time.sleep(max(0.1, args.poll))

    evidence["result"] = "capture_complete"
    evidence["ended_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    evidence["present"]["counts"] = dict(sorted(present_counts.items()))
    evidence["present"]["shapes"] = dict(sorted(present_shapes.items()))
    evidence["gl_present"]["counts"] = dict(sorted(gl_counts.items()))
    evidence["gl_present"]["shapes"] = dict(sorted(gl_shapes.items()))
    if last_snapshot:
        try:
            last_snapshot["runtime"] = runtime_health(client)
            last_snapshot["mouse"] = client.call("mouse_camera_stats")
        except (OSError, RuntimeError, json.JSONDecodeError, struct.error):
            # The endpoint normally closes before finalization. The last
            # periodic sample remains the authoritative bounded health sample.
            pass
    evidence["last_snapshot"] = last_snapshot
    evidence["timeline"] = timeline_identity(args.timeline)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.out.with_suffix(args.out.suffix + ".tmp")
    temporary.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.out)
    print(f"SF2 Disc 1 semantic capture complete: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
