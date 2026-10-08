#!/usr/bin/env python3
"""Estimate a gameplay mark in a power-on replay using its end convention.

python tools/replay_marks.py FILE.psxrpl --fps 60 [--tail-seconds 30]
Writes a new FILE.psxrpl.marks.json. The replay is never changed. This reads
bounded header/TLV metadata only; the runtime still owns replay admission.
The mark is an estimate, not evidence that gameplay began at that frame.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

HEADER = struct.Struct("<8sIIIII")
MAX_EXT = 16 << 20
MAX_FRAMES = 1_000_000


def replay_frames(path):
    """Return END for a 3/4 power-on player replay; do not admit its inputs."""
    with path.open("rb") as stream:
        raw = stream.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError("short replay header")
        magic, version, size, frames, flags, length = HEADER.unpack(raw)
        if (magic, version) not in ((b"PSXRTI3\0", 3), (b"PSXRTI4\0", 4)) or size not in (8, 12):
            raise ValueError("unsupported replay header")
        if flags or not 0 < frames <= MAX_FRAMES or length % 4 or length > MAX_EXT:
            raise ValueError("invalid replay bounds")
        if path.stat().st_size != HEADER.size + length + frames * size:
            raise ValueError("replay file length differs from its header")
        data = stream.read(length)
    offset, tags = 0, set()
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("short extension entry")
        tag, n = struct.unpack_from("<II", data, offset)
        offset += 8
        padded = (n + 3) & ~3
        if padded > len(data) - offset or any(data[offset + n:offset + padded]):
            raise ValueError("extension bounds or padding")
        if tag in (0x301, 0x302, 0x306, 0x307):
            if tag in tags:
                raise ValueError("duplicate replay metadata")
            tags.add(tag)
        if tag == 0x306 and (n != 4 or data[offset:offset + n] != bytes(4)):
            raise ValueError("power-on boundary differs")
        offset += padded
    if 0x301 in tags or not {0x302, 0x306, 0x307}.issubset(tags):
        raise ValueError("a power-on player replay is required")
    return frames


def derive(path, fps, tail_seconds=30):
    if fps not in (50, 60) or not isinstance(tail_seconds, int) or tail_seconds <= 0:
        raise ValueError("use 50 or 60 fps and a positive tail duration")
    frames = replay_frames(path)
    tail = fps * tail_seconds
    if frames < tail:
        raise ValueError("replay is shorter than the estimated gameplay tail")
    sidecar = Path(str(path) + ".marks.json")
    mark = {"frame": frames - tail, "label": "gameplay", "estimated": True}
    data = {"schema": "psxrecomp-replay-marks/1", "marks": [mark]}
    # New metadata only: preserve every existing hand-edited sidecar.
    with sidecar.open("xb") as stream:
        stream.write((json.dumps(data, indent=2) + "\n").encode("utf-8"))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return {"replay": str(path), "replay_sha256": digest.hexdigest(),
            "frames": frames, "fps": fps, "tail_seconds": tail_seconds,
            "sidecar": str(sidecar), "mark": mark,
            "meaning": "end convention estimate; gameplay was not observed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("replay", type=Path)
    parser.add_argument("--fps", type=int, choices=(50, 60), required=True,
                        help="guest vblanks per second; the old replay does not store it")
    parser.add_argument("--tail-seconds", type=int, default=30,
                        help="estimated seconds of gameplay at the end (default: 30)")
    args = parser.parse_args()
    try:
        print(json.dumps(derive(args.replay, args.fps, args.tail_seconds), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"replay marks: {error}\n")


if __name__ == "__main__":
    main()
