"""Convert the supported external BK2 profile to the runtime PSXRTI1 format.

Source frame zero becomes record one at the first runtime input boundary.
This preserves input order/duration, not an assumed cross-engine time origin.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import bk2_intake


def convert(data, end_frame=None):
    route, receipt = bk2_intake.inspect(data)
    original_frames = route["frame_count"]
    if end_frame is not None:
        if not 0 < end_frame <= original_frames:
            raise ValueError("prefix boundary outside source movie")
        route["segments"] = [dict(s, frames=min(s["frames"], end_frame-s["source_frame"]))
                             for s in route["segments"] if s["source_frame"] < end_frame]
        route["frame_count"] = end_frame
    if not 0 < route["frame_count"] <= 1000000 or len(route["segments"]) > 4096:
        raise ValueError("runtime route capacity exceeded")
    output = bytearray(struct.pack("<8sIIII", b"PSXRTI1\0", 1, 8, route["frame_count"], 0))
    words = bytearray()
    for segment in route["segments"]:
        for frame in range(segment["source_frame"], segment["source_frame"] + segment["frames"]):
            output.extend(struct.pack("<IHH", frame + 1, segment["buttons"], 0))
            words.extend(struct.pack("<H", segment["buttons"]))
    words_hash = hashlib.sha256(words).hexdigest()
    if end_frame is None and words_hash != receipt["pad_words_le_sha256"]:
        raise ValueError("controller sequence changed during export")
    receipt.update({"adapter_schema": "psx-tas-psxrti-v1", "format": "PSXRTI1",
                    "source_frame_count": original_frames,
                    "source_pad_words_le_sha256": receipt["pad_words_le_sha256"],
                    "frame_count": route["frame_count"],
                    "segment_count": len(route["segments"]),
                    "pad_words_le_sha256": words_hash,
                    "selection": "complete" if end_frame is None else "unchanged_prefix",
                    "output_sha256": hashlib.sha256(output).hexdigest(),
                    "output_bytes": len(output),
                    "mapping": "source index 0 -> runtime input record 1",
                    "timing_equivalence": "experimental, unqualified"})
    return bytes(output), receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("movie", type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--receipt", required=True, type=Path)
    p.add_argument("--end-frame", type=int, help="explicit unchanged source prefix, no resynchronization")
    args = p.parse_args()
    if args.movie.stat().st_size > bk2_intake.MAX_BYTES:
        p.error("movie exceeds intake size bound")
    if args.output.exists() or args.receipt.exists():
        p.error("choose new output and receipt paths")
    payload, receipt = convert(args.movie.read_bytes(), args.end_frame)
    with args.output.open("xb") as f:
        f.write(payload)
    with args.receipt.open("x", encoding="utf-8") as f:
        json.dump(receipt, f, indent=2)
        f.write("\n")
    print(json.dumps({k: receipt[k] for k in ("frame_count", "segment_count", "output_bytes", "output_sha256")}))


if __name__ == "__main__":
    main()
