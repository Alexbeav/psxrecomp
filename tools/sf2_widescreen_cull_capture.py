#!/usr/bin/env python3
"""Capture a bounded native-wide culling observation without altering guest state."""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import struct
import time
from pathlib import Path


APP_STACK = 0x8011EE8C
PLAYER_POINTER = 0x8012A574


class Client:
    def __init__(self, port: int) -> None:
        self.port = port
        self.ident = 0

    def call(self, command: str, **fields: object) -> dict[str, object]:
        self.ident += 1
        request = {"id": self.ident, "cmd": command, **fields}
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall((json.dumps(request) + "\n").encode("utf-8"))
            response = bytearray()
            while b"\n" not in response:
                chunk = sock.recv(1024 * 1024)
                if not chunk:
                    raise ConnectionError("debug endpoint closed")
                response.extend(chunk)
        result = json.loads(response.split(b"\n", 1)[0])
        if not result.get("ok"):
            raise RuntimeError(str(result.get("error", result.get("err"))))
        return result

    def read(self, address: int, length: int) -> bytes:
        result = self.call("read_ram", addr=hex(address), len=length)
        return bytes.fromhex(str(result["hex"]))


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def connect_when_ready(port: int, timeout: float = 30.0) -> Client:
    client = Client(port)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            client.call("frame")
            return client
        except (ConnectionError, ConnectionRefusedError, OSError):
            time.sleep(0.1)
    raise TimeoutError(f"runtime debug endpoint did not open on port {port}")


def semantic_state(client: Client) -> dict[str, object]:
    depth, state, transition, callback = struct.unpack(
        "<IIII", client.read(APP_STACK, 16)
    )
    player = struct.unpack("<I", client.read(PLAYER_POINTER, 4))[0]
    return {
        "application": {
            "depth": depth,
            "state": state,
            "transition": transition,
            "callback": f"0x{callback:08X}",
        },
        "player_pointer": f"0x{player:08X}",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=False)
    client = connect_when_ready(args.port)
    armed = client.call("ws_census", action="on")

    print("Bounded widescreen census is armed.")
    print("Play to a scene with the edge-culling defect.")
    print("Hold the camera where an NPC or background has just disappeared,")
    input("leave the game running, return here, and press Enter to capture: ")

    frame = int(client.call("frame")["frame"])
    start = max(0, frame - 360)
    census = args.out / "gpu-census.csv"
    dump = client.call(
        "ws_census", start=start, end=frame, out=census.resolve().as_posix()
    )
    gpu = client.call("gpu_state")
    state = semantic_state(client)
    client.call("ws_census", action="off")

    evidence = {
        "schema": 1,
        "observation": "human-held camera at native-wide culling boundary",
        "frame_window": [start, frame],
        "census": dump,
        "armed": armed,
        "gpu": gpu,
        "guest": state,
        "identity": {
            "executable_sha256": file_hash(args.exe),
            "configuration_sha256": file_hash(args.game),
        },
    }
    (args.out / "evidence.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Capture complete: {args.out}")
    print("You can now close the game window normally.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
