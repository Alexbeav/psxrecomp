#!/usr/bin/env python3
"""Extract and validate PSX Workbench records from PCSX-Redux output."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable


PREFIX = "PSXWB_EVENT\t"


def parse_events(lines: Iterable[str]) -> list[dict]:
    events: list[dict] = []
    for line_number, line in enumerate(lines, 1):
        marker = line.find(PREFIX)
        if marker < 0:
            continue
        payload = line[marker + len(PREFIX) :].strip()
        try:
            event = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ValueError(f"line {line_number}: invalid PSXWB JSON: {exc}") from exc
        if not isinstance(event, dict):
            raise ValueError(f"line {line_number}: event must be a JSON object")
        events.append(event)

    validate_events(events)
    return events


def validate_events(events: list[dict]) -> None:
    if not events:
        raise ValueError("no PSXWB events found")
    if events[0].get("record") != "start":
        raise ValueError("first event must be a start record")
    if events[-1].get("record") != "complete":
        raise ValueError("last event must be a complete record")

    required_start = ("schema_version", "experiment_id", "redux_commit", "build_id", "module_id")
    missing = [field for field in required_start if field not in events[0]]
    if missing:
        raise ValueError(f"start record is missing: {', '.join(missing)}")
    if events[0]["schema_version"] != 1:
        raise ValueError("unsupported event schema version")

    sampling_interval = events[0].get("sampling_interval", 1)
    if not isinstance(sampling_interval, int) or sampling_interval < 1:
        raise ValueError("sampling_interval must be a positive integer")
    expected_frame = sampling_interval
    for event in events[1:-1]:
        if event.get("record") != "vsync":
            raise ValueError("only vsync records may appear between start and complete")
        if event.get("frame") != expected_frame:
            raise ValueError(
                f"non-contiguous frame sequence: expected {expected_frame}, got {event.get('frame')}"
            )
        expected_frame += sampling_interval

    completed_frame = events[-1].get("frame")
    final_sample_frame = expected_frame - sampling_interval
    if completed_frame != final_sample_frame:
        raise ValueError(
            f"complete frame {completed_frame} does not match final VSync {final_sample_frame}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="PCSX-Redux stdout/log capture")
    parser.add_argument("output", type=Path, help="validated JSON Lines output")
    args = parser.parse_args()

    with args.input.open("r", encoding="utf-8", errors="replace") as source:
        events = parse_events(source)
    with args.output.open("w", encoding="utf-8", newline="\n") as destination:
        for event in events:
            destination.write(json.dumps(event, sort_keys=True, separators=(",", ":")) + "\n")
    print(f"wrote {len(events)} validated events to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
