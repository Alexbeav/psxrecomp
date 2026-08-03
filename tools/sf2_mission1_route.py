#!/usr/bin/env python3
"""Drive and verify the authentic SF2 Disc 1 Mission 1 retail route.

The process must already be running headlessly with the debug server enabled.
This tool never writes guest state: it observes semantic gates and injects only
ordinary active-low PAD input.  Evidence is written to an ignored local path.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time
from pathlib import Path
from typing import Any, Callable


# Independently read from the user-owned SCUS-94451 MOVIE1.HOG header.  These
# are first payload LBAs (catalog extent + member offset + one header sector),
# not addresses or dimensions inferred from another title.
STARTUP_MOVIE_LBAS = {
    "989LOGO.STR": 205322,
    "EIDETIC.STR": 261286,
    "LEGAL.STR": 261622,
    "ZINTRO.STR": 269590,
    "TITLE.STR": 263062,
}

APP_STACK = 0x8011EE8C
TITLE_STATE = 0x80156BDC
PLAYER_POINTER = 0x8012A574


class DebugClient:
    def __init__(self, port: int) -> None:
        self.port = port
        self.ident = 0

    def call(self, command: str, **fields: Any) -> dict[str, Any]:
        self.ident += 1
        request = {"id": self.ident, "cmd": command, **fields}
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall((json.dumps(request) + "\n").encode("utf-8"))
            response = bytearray()
            while b"\n" not in response:
                chunk = sock.recv(1024 * 1024)
                if not chunk:
                    raise RuntimeError("debug server closed the connection")
                response.extend(chunk)
        result = json.loads(response.split(b"\n", 1)[0])
        if not result.get("ok"):
            raise RuntimeError(f"{command}: {result.get('error', 'unknown error')}")
        return result

    def read(self, address: int, length: int) -> bytes:
        result = self.call("read_ram", addr=hex(address), len=length)
        return bytes.fromhex(result["hex"])

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def frame(self) -> int:
        return int(self.call("frame")["frame"])


def app_state(client: DebugClient) -> dict[str, int]:
    depth, state, transition, callback = struct.unpack(
        "<IIII", client.read(APP_STACK, 16)
    )
    return {
        "depth": depth,
        "state": state,
        "transition": transition,
        "callback": callback,
    }


def player_state(client: DebugClient) -> dict[str, Any] | None:
    instance = client.u32(PLAYER_POINTER)
    if not instance:
        return None
    node = client.u32(instance + 0x08)
    health_controller = client.u32(instance + 0x18)
    player_record = client.u32(instance + 0x20)
    if not node or not health_controller or not player_record:
        return None
    matrix = client.u32(node + 0x0C)
    camera = client.u32(player_record + 0xE0)
    if not matrix or not camera:
        return None
    owner = client.u32(camera + 0xDC)
    x, y, z = struct.unpack("<iii", client.read(matrix + 0x14, 12))
    armor, health = struct.unpack("<hh", client.read(health_controller + 0x06, 4))
    return {
        "instance": f"0x{instance:08X}",
        "node": f"0x{node:08X}",
        "matrix": f"0x{matrix:08X}",
        "camera": f"0x{camera:08X}",
        "camera_owner": f"0x{owner:08X}",
        "player_owns_camera": owner == instance,
        "xyz": [x, y, z],
        "health": health,
        "armor": armor,
    }


def wait_for(
    client: DebugClient,
    description: str,
    predicate: Callable[[], Any],
    timeout: float,
    poll: float = 0.25,
) -> Any:
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(poll)
    raise TimeoutError(f"timed out waiting for {description}; last={last!r}")


def wait_for_endpoint(client: DebugClient, timeout: float = 60.0) -> None:
    deadline = time.monotonic() + timeout
    last: OSError | None = None
    while time.monotonic() < deadline:
        try:
            client.call("frame")
            return
        except OSError as exc:
            last = exc
            time.sleep(0.25)
    raise TimeoutError(f"debug endpoint did not become ready: {last}")


def wait_guest_frames(client: DebugClient, count: int, timeout: float) -> None:
    start = client.frame()
    wait_for(
        client,
        f"{count} guest frames after {start}",
        lambda: client.frame() >= start + count,
        timeout,
    )


def checkpoint(client: DebugClient, name: str) -> dict[str, Any]:
    frame = client.frame()
    fp_lo = max(0, frame - 31)
    return {
        "name": name,
        "frame": frame,
        "app": app_state(client),
        "title_state": client.u32(TITLE_STATE),
        "gpu": client.call("gpu_state"),
        "pad": client.call("pad_status"),
        "cdrom": client.call("cdrom_state"),
        "spu": client.call("spu_status"),
        "audio": client.call("audio_stats"),
        "timers": client.call("timers_state"),
        "irq": client.call("irq_state"),
        "resident_dispatch": client.call("dispatch_stats"),
        "overlay_dispatch": client.call("overlay_loader_status"),
        "player": player_state(client),
        "fingerprint": client.call(
            "frame_fingerprint", count=32, frame_lo=fp_lo, frame_hi=frame
        ),
    }


def press_and_release(client: DebugClient, buttons: int, frames: int = 20) -> None:
    client.call("press", buttons=buttons, frames=frames)
    wait_for(
        client,
        "PAD press",
        lambda: status
        if (status := client.call("pad_status"))["pad"] != "0xFFFF"
        else None,
        30,
    )
    wait_for(
        client,
        "PAD release",
        lambda: status
        if (status := client.call("pad_status"))["pad"] == "0xFFFF"
        else None,
        30,
    )


def scheduled_press(
    client: DebugClient, buttons: int, at_frame: int, frames: int = 20
) -> dict[str, Any]:
    if at_frame <= client.frame():
        raise RuntimeError(f"scheduled input frame {at_frame} is not in the future")
    client.call("press", buttons=buttons, frames=frames, at_frame=at_frame)
    wait_guest_frames(client, at_frame - client.frame(), 60)
    released = wait_for(
        client,
        "scheduled PAD consumption and release",
        lambda: status
        if (status := client.call("pad_status"))["override"] == -1
        else None,
        60,
    )
    release_frame = client.frame()
    if released["override_applied_value"] != buttons:
        raise RuntimeError(
            f"scheduled PAD value was not applied: {released}"
        )
    first_sample = released["override_first_applied_frame"]
    last_sample = released["override_last_applied_frame"]
    sample_count = released["override_applied_count"]
    if first_sample < at_frame or sample_count != frames:
        raise RuntimeError(f"scheduled PAD timing/count mismatch: {released}")
    return {
        "at_frame": at_frame,
        "buttons": buttons,
        "frames": frames,
        "first_sample": first_sample,
        "last_sample": last_sample,
        "sample_count": sample_count,
        "release_frame": release_frame,
        "released_pad": released["pad"],
    }


def next_frame_boundary(frame: int, quantum: int = 600) -> int:
    """Choose a shared future guest-frame anchor despite host polling jitter."""
    return ((frame // quantum) + 1) * quantum


def deterministic_route_schedule(title_movie_frame: int) -> dict[str, int]:
    """Anchor all input to a guest event, never to host-polled gate timing."""
    menu_anchor = next_frame_boundary(title_movie_frame + 600)
    return {
        "new_game": menu_anchor,
        "one_player": menu_anchor + 120,
        "leave_briefing": menu_anchor + 4800,
        "move": menu_anchor + 6600,
    }


def run(client: DebugClient, timeout: float) -> dict[str, Any]:
    evidence: dict[str, Any] = {
        "schema": 1,
        "movie_lbas": STARTUP_MOVIE_LBAS,
        "startup_seen": {},
        "checkpoints": [],
        "input_schedule": [],
    }
    start_time = time.monotonic()

    # Latch every movie when its first payload sector appears; the ring cannot
    # retain the early logos throughout the full ZINTRO duration.
    while len(evidence["startup_seen"]) != len(STARTUP_MOVIE_LBAS):
        if time.monotonic() - start_time > timeout:
            raise TimeoutError(f"startup sequence incomplete: {evidence['startup_seen']}")
        for name, lba in STARTUP_MOVIE_LBAS.items():
            if name in evidence["startup_seen"]:
                continue
            result = client.call("cdrom_sector_history", count=1, lba=hex(lba))
            if result["entries"]:
                entry = result["entries"][0]
                evidence["startup_seen"][name] = {
                    "frame": entry["frame"],
                    "lba": entry["lba"],
                }
        time.sleep(0.5)

    def title_ready() -> dict[str, Any] | None:
        app = app_state(client)
        gpu = client.call("gpu_state")
        pad = client.call("pad_status")
        title = client.u32(TITLE_STATE)
        if (
            app["depth"] == 2
            and app["state"] == 4
            and app["transition"] == 0
            and title == 0
            and gpu["width"] == 320
            and gpu["height"] == 240
            and gpu["depth"] == 15
            and not gpu["disabled"]
            and pad["pad"] == "0xFFFF"
            and pad["override"] == -1
        ):
            return {"frame": client.frame(), "app": app, "gpu": gpu, "pad": pad}
        return None

    ready = wait_for(client, "compound stable retail TITLE", title_ready, timeout)
    ready_frame = ready["frame"]
    wait_guest_frames(client, 60, 60)
    ready2 = title_ready()
    if not ready2 or ready2["frame"] < ready_frame + 60:
        raise RuntimeError("TITLE compound predicate did not remain stable")
    evidence["checkpoints"].append(checkpoint(client, "stable_title"))

    # Cross selects New Game, then One Player.  A neutral sample interval
    # separates the edges so the retail PAD consumer sees two distinct presses.
    schedule = deterministic_route_schedule(
        evidence["startup_seen"]["TITLE.STR"]["frame"]
    )
    evidence["input_schedule"].append(
        scheduled_press(client, 0xBFFF, schedule["new_game"])
    )
    evidence["input_schedule"].append(
        scheduled_press(client, 0xBFFF, schedule["one_player"])
    )

    def aircraft_movie() -> bool:
        app = app_state(client)
        gpu = client.call("gpu_state")
        return (
            app["depth"] == 3
            and app["state"] == 3
            and gpu["width"] == 512
            and gpu["height"] == 240
            and gpu["depth"] == 24
            and not gpu["disabled"]
        )

    wait_for(client, "retail Mission 1 aircraft movie", aircraft_movie, 180)
    evidence["checkpoints"].append(checkpoint(client, "mission1_aircraft_movie"))

    def briefing() -> bool:
        app = app_state(client)
        gpu = client.call("gpu_state")
        return (
            app["depth"] == 2
            and app["state"] == 8
            and gpu["width"] == 384
            and gpu["height"] == 240
            and gpu["depth"] == 15
            and not gpu["disabled"]
        )

    wait_for(client, "retail Mission 1 state-8 briefing", briefing, timeout)
    evidence["checkpoints"].append(checkpoint(client, "mission1_state8"))
    evidence["input_schedule"].append(
        scheduled_press(client, 0xBFFF, schedule["leave_briefing"])
    )

    def player_control() -> dict[str, Any] | None:
        app = app_state(client)
        player = player_state(client)
        cdrom = client.call("cdrom_state")
        if (
            app["state"] == 0
            and player
            and player["player_owns_camera"]
            and player["health"] > 0
            and not cdrom["reading"]
        ):
            return player
        return None

    before = wait_for(client, "retail player ownership", player_control, timeout)
    wait_guest_frames(client, 60, 60)
    before = player_control()
    if not before:
        raise RuntimeError("player ownership did not remain stable")
    evidence["checkpoints"].append(checkpoint(client, "mission1_player_owned"))

    move_start = schedule["move"]
    evidence["input_schedule"].append(
        scheduled_press(client, 0xFFEF, move_start, 60)
    )  # D-pad Up, active low.
    wait_guest_frames(client, 30, 60)
    after = player_state(client)
    if not after or not after["player_owns_camera"] or after["xyz"] == before["xyz"]:
        raise RuntimeError(f"authoritative player position did not move: {before} -> {after}")
    move_end = client.frame()
    pad_window = client.call(
        "frame_timeseries", start=move_start, end=min(move_start + 100, move_end)
    )
    if not any(row and row.get("pad") == "0xFFEF" for row in pad_window["ts"]):
        raise RuntimeError("movement occurred without a recorded PAD sample")
    evidence["movement"] = {
        "start_frame": move_start,
        "end_frame": move_end,
        "before": before,
        "after": after,
        "pad_window": pad_window,
    }
    evidence["checkpoints"].append(checkpoint(client, "mission1_moved"))

    final = evidence["checkpoints"][-1]
    if final["cdrom"]["int1_lost"] != 0:
        raise RuntimeError("CD-ROM lost an INT1 during the route")
    if final["spu"]["key_on_count"] == 0:
        raise RuntimeError("no retail SPU key-on activity was observed")
    if final["audio"]["taps"][1]["nonzero"] == 0:
        raise RuntimeError("no nonzero XA/CD input activity was observed")
    evidence["result"] = "pass"
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1200.0)
    args = parser.parse_args()
    client = DebugClient(args.port)
    wait_for_endpoint(client)
    evidence = run(client, args.timeout)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(f"SF2 Mission 1 retail route: PASS ({args.out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
