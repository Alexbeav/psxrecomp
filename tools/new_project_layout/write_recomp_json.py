#!/usr/bin/env python3
"""Write a starter .recomp.json (https://recomp.fyi/spec) at a game repo root.

The file is how a project describes itself to community lists such as
recomp.board: game, system, status, the release a user must own. Schema:
https://recomp.fyi/schema/v1.json.

Once written, the file belongs to the project — maintainers edit it as the
status changes. Re-running this never overwrites a field that is already set;
it only fills fields that are missing, so it is safe on an existing repo.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

SCHEMA_URL = "https://recomp.fyi/schema/v1.json"

# Sony product-code prefix -> release region. The serial is read off the disc,
# so it outranks the scaffold's --region (which defaults to USA).
SERIAL_REGIONS = {
    "SCUS": "USA", "SLUS": "USA",
    "SCES": "Europe", "SLES": "Europe", "SCED": "Europe", "SLED": "Europe",
    "SCPS": "Japan", "SLPS": "Japan", "SLPM": "Japan",
    "SCAJ": "Asia",
    "SCKA": "Korea", "SLKA": "Korea",
}


def region_from_serial(serial: str) -> str:
    return SERIAL_REGIONS.get(serial[:4].upper(), "") if serial else ""


def build_starter(*, game: str, project: str, serial: str, region: str) -> dict:
    original: dict[str, str] = {}
    region = region_from_serial(serial) or region
    if region:
        original["region"] = region
    if serial:
        original["serial"] = serial

    doc: dict = {
        "$schema": SCHEMA_URL,
        "game": game,
        "system": "PS1",
        "type": "recomp",
        "toolchain": "psxrecomp",
        "status": "exploring",
    }
    if project and project != game:
        doc["project"] = project
    if original:
        doc["original"] = original
    return doc


def merge_missing(existing: dict, starter: dict) -> dict:
    """Fill keys absent from ``existing``; never replace a value already set."""
    out = dict(existing)
    for key, val in starter.items():
        if key not in out:
            out[key] = val
        elif key == "original" and isinstance(out[key], dict) and isinstance(val, dict):
            out[key] = {**val, **out[key]}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", help="path to write (normally <repo>/.recomp.json)")
    ap.add_argument("--game", required=True, help="game title as it shipped")
    ap.add_argument("--project", default="", help="the project's own name")
    ap.add_argument("--region", default="", help="fallback when the serial has no known region")
    ap.add_argument("--serial", default="", help="disc product code, e.g. SLUS-00562")
    ap.add_argument(
        "--probe-json",
        default="",
        help="disc_probe.json from probe_disc.py; supplies --serial when present",
    )
    args = ap.parse_args()

    serial = args.serial
    if not serial and args.probe_json and Path(args.probe_json).is_file():
        serial = json.loads(Path(args.probe_json).read_text(encoding="utf-8")).get("serial", "")

    starter = build_starter(
        game=args.game.strip(),
        project=args.project.strip(),
        serial=serial.strip(),
        region=args.region.strip(),
    )

    out = Path(args.out)
    doc = starter
    if out.is_file():
        try:
            existing = json.loads(out.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            print(f"error: {out} is not valid JSON ({e}); leaving it alone", file=sys.stderr)
            return 1
        if not isinstance(existing, dict):
            print(f"error: {out} is not a JSON object; leaving it alone", file=sys.stderr)
            return 1
        doc = merge_missing(existing, starter)

    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"  wrote {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
