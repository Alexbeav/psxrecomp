#!/usr/bin/env python3
"""Prove a runtime PAD timeline with two clean, headless processes.

This is title-neutral: it schedules one ordinary active-low PAD pulse, checks
that the recorder captured it at exact guest frames, then observes the same
SIO state during replay. It never reads guest RAM or game media.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import time
from pathlib import Path


class Client:
    def __init__(self, port: int) -> None:
        self.port = port
        self.ident = 0

    def call(self, cmd: str, **fields: object) -> dict[str, object]:
        self.ident += 1
        request = {"id": self.ident, "cmd": cmd, **fields}
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            sock.sendall((json.dumps(request) + "\n").encode())
            data = bytearray()
            while b"\n" not in data:
                chunk = sock.recv(1024 * 1024)
                if not chunk:
                    raise RuntimeError("debug endpoint closed")
                data.extend(chunk)
        result = json.loads(data.split(b"\n", 1)[0])
        if not result.get("ok"):
            raise RuntimeError(f"{cmd}: {result}")
        return result

    def frame(self) -> int:
        return int(self.call("frame")["frame"])


def wait_endpoint(client: Client, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"runtime exited early with {process.returncode}")
        try:
            client.frame()
            return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError("debug endpoint did not open")


def wait_frame(client: Client, target: int) -> None:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if client.frame() >= target:
            return
        time.sleep(0.01)
    raise TimeoutError(f"guest did not reach frame {target}")


def launch(args: argparse.Namespace, cards: Path, timeline_arg: str, port: int,
           stdout_path: Path, stderr_path: Path) -> tuple[subprocess.Popen[bytes], object, object]:
    cards.mkdir(parents=True)
    stdout = stdout_path.open("wb")
    stderr = stderr_path.open("wb")
    flags = 0
    if hasattr(subprocess, "CREATE_NO_WINDOW"):
        flags = subprocess.CREATE_NO_WINDOW
    process = subprocess.Popen(
        [str(args.exe), "--game", str(args.game), "--bios", str(args.bios),
         "--headless", "--no-launcher", "--renderer", "software",
         "--debug-port", str(port), "--memcard-dir", str(cards),
         timeline_arg, str(args.timeline)],
        cwd=args.exe.parent, stdout=stdout, stderr=stderr,
        creationflags=flags,
    )
    return process, stdout, stderr


def stop(client: Client, process: subprocess.Popen[bytes]) -> None:
    try:
        client.call("quit")
    except (OSError, RuntimeError):
        pass
    process.wait(timeout=15)
    if process.returncode != 0:
        raise RuntimeError(f"runtime exit code {process.returncode}")


def read_timeline(path: Path) -> tuple[int, list[tuple[int, int]]]:
    data = path.read_bytes()
    if len(data) < 32 or data[:8] != b"PSXPAD1\0":
        raise RuntimeError("invalid timeline magic")
    version, header_size, record_size = struct.unpack_from("<III", data, 8)
    count = struct.unpack_from("<Q", data, 20)[0]
    if (version, header_size, record_size) != (1, 32, 32):
        raise RuntimeError("unexpected timeline contract")
    if len(data) != header_size + count * record_size:
        raise RuntimeError("timeline length/count mismatch")
    rows = []
    for offset in range(header_size, len(data), record_size):
        frame = struct.unpack_from("<Q", data, offset)[0]
        buttons = struct.unpack_from("<H", data, offset + 8)[0]
        rows.append((frame, buttons))
    return count, rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--port", type=int, default=19791)
    args = parser.parse_args()
    args.exe = args.exe.resolve(); args.game = args.game.resolve(); args.bios = args.bios.resolve()
    args.timeline = args.timeline.resolve(); args.work = args.work.resolve()
    if args.timeline.exists() or args.work.exists():
        raise SystemExit("timeline and work paths must not already exist")
    args.work.mkdir(parents=True)

    proc, out, err = launch(args, args.work / "cards-record", "--pad-record",
                            args.port, args.work / "record.stdout", args.work / "record.stderr")
    client = Client(args.port)
    try:
        wait_endpoint(client, proc)
        start = client.frame()
        press_at = ((start // 120) + 2) * 120
        client.call("press", buttons=0xFFEF, frames=20, at_frame=press_at)
        wait_frame(client, press_at + 80)
        stop(client, proc)
    finally:
        out.close(); err.close()
    count, rows = read_timeline(args.timeline)
    pressed = [frame for frame, buttons in rows if buttons == 0xFFEF]
    if pressed != list(range(press_at, press_at + 20)):
        raise RuntimeError(f"recorded PAD pulse mismatch: {pressed[:3]}..{pressed[-3:] if pressed else []}")

    proc, out, err = launch(args, args.work / "cards-replay", "--pad-replay",
                            args.port + 1, args.work / "replay.stdout", args.work / "replay.stderr")
    client = Client(args.port + 1)
    observed = None
    try:
        wait_endpoint(client, proc)
        wait_frame(client, press_at)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and client.frame() < press_at + 20:
            status = client.call("pad_status")
            if status["pad"] == "0xFFEF":
                observed = {"frame": client.frame(), "pad": status["pad"]}
                break
        if observed is None:
            raise RuntimeError("replay PAD pulse was not SIO-visible")
        stop(client, proc)
    finally:
        out.close(); err.close()

    receipt = {
        "schema": 1, "result": "pass", "samples": count,
        "scheduled_frame": press_at, "scheduled_samples": 20,
        "replay_observation": observed,
    }
    (args.work / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
