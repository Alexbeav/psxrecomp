#!/usr/bin/env python3
"""Compare two ignored SF2 route artifacts at stable guest boundaries."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


FP_FIELDS = ("frame", "wr", "pc", "wc", "mmio", "mc", "sp", "sc", "cyc")


def canonical_hash(value: Any) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(payload).hexdigest()


def schedule_key(row: dict[str, Any]) -> dict[str, Any]:
    # release_frame is when a host query observed release, so it is deliberately
    # excluded.  Application is recorded on the emulation thread and must match.
    return {
        key: row[key]
        for key in (
            "at_frame",
            "buttons",
            "frames",
            "first_sample",
            "last_sample",
            "sample_count",
            "released_pad",
        )
    }


def semantic_key(checkpoint: dict[str, Any]) -> dict[str, Any]:
    overlay = checkpoint["overlay_dispatch"]
    gpu = checkpoint["gpu"]
    player = checkpoint["player"]
    height = gpu["height"]
    draw_area = list(gpu["draw_area"])
    draw_offset = list(gpu["draw_offset"])
    # Retail alternates equivalent VRAM draw banks.  Normalize only the bank
    # origin by the active display height; widths and relative geometry remain
    # exact, and same-frame MMIO fingerprints below still compare raw commands.
    if height:
        draw_area[1] %= height
        draw_area[3] %= height
        draw_offset[1] %= height
    return {
        "name": checkpoint["name"],
        "app": checkpoint["app"],
        # This overlay-local word is a TITLE predicate only; later images reuse
        # the same RAM address for unrelated transient data.
        "title_state": checkpoint["title_state"]
        if checkpoint["name"] == "stable_title"
        else None,
        "gpu": {
            key: gpu[key]
            for key in (
                "width",
                "height",
                "depth",
                "depth24",
                "disabled",
            )
        }
        | {"draw_area": draw_area, "draw_offset": draw_offset},
        "pad": {
            "pad": checkpoint["pad"]["pad"],
            "connected": checkpoint["pad"]["slot0"]["connected"],
        },
        "overlay_images": {
            "active": overlay["active"],
            "registered": overlay["registered"],
            "regions": overlay["regions"],
            "loads": overlay["loads"],
            "checked": overlay["checked"],
        },
        "cd_lost": checkpoint["cdrom"]["int1_lost"],
        "player": None
        if player is None
        else {
            key: player[key]
            for key in (
                "instance",
                "camera_owner",
                "player_owns_camera",
                "health",
                "armor",
            )
        },
    }


def fingerprint_comparison(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    amap = {row["frame"]: row for row in a["fingerprint"]["entries"]}
    bmap = {row["frame"]: row for row in b["fingerprint"]["entries"]}
    frames = sorted(amap.keys() & bmap.keys())
    left = [{key: amap[frame][key] for key in FP_FIELDS} for frame in frames]
    right = [{key: bmap[frame][key] for key in FP_FIELDS} for frame in frames]
    return {
        "frames": frames,
        "count": len(frames),
        "hash_a": canonical_hash(left),
        "hash_b": canonical_hash(right),
        "match": bool(frames) and left == right,
    }


def compare(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    if a["result"] != "pass" or b["result"] != "pass":
        errors.append("both routes must report pass")
    if a["movie_lbas"] != b["movie_lbas"] or a["startup_seen"] != b["startup_seen"]:
        errors.append("startup movie identity/frame evidence differs")

    schedules_a = [schedule_key(row) for row in a["input_schedule"]]
    schedules_b = [schedule_key(row) for row in b["input_schedule"]]
    if schedules_a != schedules_b:
        errors.append("emulation-thread input schedules differ")

    if len(a["checkpoints"]) != len(b["checkpoints"]):
        errors.append("checkpoint counts differ")
    checkpoints = []
    for left, right in zip(a["checkpoints"], b["checkpoints"]):
        semantic_a = semantic_key(left)
        semantic_b = semantic_key(right)
        if semantic_a != semantic_b:
            errors.append(f"semantic checkpoint differs: {left['name']}")
        fingerprints = fingerprint_comparison(left, right)
        if not fingerprints["match"]:
            errors.append(f"normalized fingerprints differ: {left['name']}")
        checkpoints.append(
            {
                "name": left["name"],
                "frames_sampled": [left["frame"], right["frame"]],
                "semantic_hash": canonical_hash(semantic_a),
                "fingerprints": fingerprints,
                "ownership": {
                    "resident_aot": [
                        left["resident_dispatch"]["static_hits"],
                        right["resident_dispatch"]["static_hits"],
                    ],
                    "compiled_overlay": [
                        left["overlay_dispatch"]["dispatch_native"],
                        right["overlay_dispatch"]["dispatch_native"],
                    ],
                    "interpreter_fallback": [
                        left["overlay_dispatch"]["dispatch_interp_fallback"],
                        right["overlay_dispatch"]["dispatch_interp_fallback"],
                    ],
                },
            }
        )

    after_a = a["movement"]["after"]
    after_b = b["movement"]["after"]
    movement = {
        "scheduled_frame": [a["movement"]["start_frame"], b["movement"]["start_frame"]],
        "identity": [after_a["instance"], after_b["instance"]],
        "camera_owner": [after_a["camera_owner"], after_b["camera_owner"]],
        "xyz": [after_a["xyz"], after_b["xyz"]],
        "health": [after_a["health"], after_b["health"]],
    }
    if any(pair[0] != pair[1] for pair in movement.values()):
        errors.append("post-input authoritative player state differs")

    return {
        "schema": 1,
        "result": "pass" if not errors else "fail",
        "errors": errors,
        "startup_hash": canonical_hash(a["startup_seen"]),
        "input_schedule_hash": canonical_hash(schedules_a),
        "checkpoints": checkpoints,
        "movement": movement,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence_a", type=Path)
    parser.add_argument("evidence_b", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    a = json.loads(args.evidence_a.read_text(encoding="utf-8"))
    b = json.loads(args.evidence_b.read_text(encoding="utf-8"))
    result = compare(a, b)
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["result"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
