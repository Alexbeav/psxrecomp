#!/usr/bin/env python3
"""Observe a captured retail-input replay through SF2's connected slice."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path

from sf2_mission1_route import (
    DebugClient, STARTUP_MOVIE_LBAS, app_state, checkpoint, player_state,
    wait_for_endpoint,
)


def timeline_last_frame(path: Path) -> tuple[int, str]:
    data = path.read_bytes()
    if len(data) < 64 or data[:8] != b"PSXPAD1\0":
        raise RuntimeError("invalid PAD timeline")
    count = int.from_bytes(data[20:28], "little")
    if len(data) != 32 + count * 32:
        raise RuntimeError("PAD timeline length/count mismatch")
    last = int.from_bytes(data[-32:-24], "little")
    return last, hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args()
    last_frame, timeline_hash = timeline_last_frame(args.timeline)
    client = DebugClient(args.port)
    wait_for_endpoint(client)

    evidence: dict[str, object] = {
        "schema": 1, "timeline_sha256": timeline_hash,
        "timeline_last_frame": last_frame, "startup_seen": {},
        "app_transitions": [], "player_generations": [], "checkpoints": [],
        "health_minimum": None, "death_observed": False,
        "player_loss_after_death": False, "player_regained_after_death": False,
    }
    startup = evidence["startup_seen"]
    transitions = evidence["app_transitions"]
    generations = evidence["player_generations"]
    checkpoints = evidence["checkpoints"]
    deadline = time.monotonic() + args.timeout
    previous_app = None
    previous_instance = None
    saw_owned_before_death = False
    last_poll_frame = -1
    state8_count = 0

    while time.monotonic() < deadline:
        frame = client.frame()
        if frame == last_poll_frame:
            time.sleep(0.01)
            continue
        last_poll_frame = frame
        for name, lba in STARTUP_MOVIE_LBAS.items():
            if name not in startup:
                result = client.call("cdrom_sector_history", count=1, lba=hex(lba))
                if result["entries"]:
                    startup[name] = {"frame": result["entries"][0]["frame"], "lba": lba}

        app = app_state(client)
        app_tuple = (app["depth"], app["state"], app["transition"], app["callback"])
        if app_tuple != previous_app:
            if len(transitions) < 512:
                transitions.append({"frame": frame, **app})
            previous_app = app_tuple
            if app["state"] == 8:
                state8_count += 1
                checkpoints.append(checkpoint(client, f"state8_{state8_count}"))

        player = player_state(client)
        instance = player["instance"] if player else None
        if instance != previous_instance:
            if len(generations) < 128:
                generations.append({"frame": frame, "instance": instance,
                                    "owned": bool(player and player["player_owns_camera"])})
            previous_instance = instance
        if player and player["player_owns_camera"]:
            health = int(player["health"])
            current_min = evidence["health_minimum"]
            evidence["health_minimum"] = health if current_min is None else min(current_min, health)
            if not evidence["death_observed"]:
                saw_owned_before_death = True
            if health <= 0:
                evidence["death_observed"] = True
                if not any(c["name"] == "death" for c in checkpoints):
                    checkpoints.append(checkpoint(client, "death"))
            elif evidence["death_observed"] and evidence["player_loss_after_death"]:
                evidence["player_regained_after_death"] = True
        elif evidence["death_observed"] and saw_owned_before_death:
            evidence["player_loss_after_death"] = True

        if frame >= last_frame - 5:
            checkpoints.append(checkpoint(client, "mission2_entry_final"))
            break
        time.sleep(0.03)
    else:
        raise TimeoutError(f"replay did not reach frame {last_frame}")

    evidence["startup_complete"] = set(startup) == set(STARTUP_MOVIE_LBAS)
    evidence["final_frame"] = client.frame()
    evidence["final_memcard"] = client.call("card_data_writes")
    evidence["result"] = "pass" if (
        evidence["startup_complete"] and state8_count >= 2 and
        evidence["death_observed"] and evidence["player_regained_after_death"]
    ) else "incomplete"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    try:
        client.call("quit")
    except (OSError, RuntimeError):
        pass
    print(f"SF2 connected replay: {str(evidence['result']).upper()} ({args.out})")
    return 0 if evidence["result"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
