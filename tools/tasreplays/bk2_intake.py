"""Inspect and normalize one bounded PSX BK2 profile; never launch a game.

Supported profile: BizHawk 2.2.2, 2.3.0 or 2.3.3 Octoshock, one digital pad, no cards,
no reset/tray changes, disc 1 throughout, no embedded state or other payloads.
See README.md for provenance, timing limits, and the qualification proposal.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import zipfile


MAX_BYTES = 16 * 1024 * 1024
MEMBERS = {"Header.txt", "Input Log.txt", "SyncSettings.json", "Comments.txt", "Subtitles.txt"}
BUTTONS = ("Up", "Down", "Left", "Right", "Select", "Start", "Square",
           "Triangle", "Circle", "Cross", "L1", "R1", "L2", "R2")
BITS = (4, 6, 7, 5, 0, 3, 15, 12, 13, 14, 10, 11, 8, 9)
LOG_KEY = "LogKey:#Disc Select|Open|Close|Reset|#" + "|".join("P1 " + b for b in BUTTONS) + "|"
SYNC = {"o": {
    "$type": "BizHawk.Emulation.Cores.Sony.PSX.Octoshock+SyncSettings, BizHawk.Emulation.Cores",
    "EnableLEC": False,
    "FIOConfig": {"Multitaps": [False, False], "Memcards": [False, False],
                  "Devices8": [1, 0, 0, 0, 0, 0, 0, 0]}}}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def entries(data):
    if len(data) > MAX_BYTES:
        raise ValueError("archive exceeds the intake size limit")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        infos = archive.infolist()
        names = [i.filename for i in infos]
        if len(names) != len(set(names)):
            raise ValueError("duplicate archive member")
        if sum(i.file_size for i in infos) > MAX_BYTES:
            raise ValueError("expanded archive exceeds the intake size limit")
        if any(i.flag_bits & 1 for i in infos):
            raise ValueError("encrypted archive is unsupported")
        # No archive paths are ever extracted to the filesystem.
        return {i.filename: archive.read(i) for i in infos}


def inspect(data):
    parts = entries(data)
    wrapper_member = None
    movie = data
    if len(parts) == 1 and next(iter(parts)).lower().endswith(".bk2"):
        wrapper_member, movie = next(iter(parts.items()))
        parts = entries(movie)
    if not {"Header.txt", "Input Log.txt", "SyncSettings.json"} <= parts.keys():
        raise ValueError("required BK2 member is missing")
    if parts.keys() - MEMBERS:
        raise ValueError("unsupported member or embedded state: " + ", ".join(sorted(parts.keys() - MEMBERS)))
    header = {}
    for line in parts["Header.txt"].decode("utf-8-sig").splitlines():
        if not line.strip():
            continue
        key, sep, value = line.partition(" ")
        if not sep or key in header:
            raise ValueError("malformed or duplicate header field")
        header[key] = value
    for key, value in {"Platform": "PSX", "Core": "Octoshock"}.items():
        if header.get(key) != value:
            raise ValueError("unsupported " + key)
    if header.get("emuVersion") not in {"Version 2.2.2", "Version 2.3.0", "Version 2.3.3"}:
        raise ValueError("unsupported emuVersion")
    if any(key.lower().startswith("startsfrom") for key in header):
        raise ValueError("state/save anchored header is unsupported")
    sync = json.loads(parts["SyncSettings.json"].decode("utf-8-sig"))
    if sync != SYNC:
        raise ValueError("unsupported synchronization configuration")
    lines = parts["Input Log.txt"].decode("utf-8-sig").splitlines()
    if len(lines) < 4 or lines[:2] != ["[Input]", LOG_KEY] or lines[-1] != "[/Input]":
        raise ValueError("unsupported input framing or controller layout")
    words, segments = [], []
    counts = dict.fromkeys(BUTTONS, 0)
    for index, line in enumerate(lines[2:-1]):
        match = re.fullmatch(r"\| {4}1,\.\.\.\|([^|]{14})\|", line)
        if not match:
            raise ValueError(f"frame {index}: unsupported disc/reset/tray event or row width")
        buttons = match[1]
        if any(ord(c) < 33 or ord(c) > 126 for c in buttons):
            raise ValueError(f"frame {index}: invalid button character")
        word = 0xFFFF
        for name, bit, char in zip(BUTTONS, BITS, buttons):
            if char != ".":
                word &= ~(1 << bit)
                counts[name] += 1
        words.append(word)
        if segments and segments[-1]["buttons"] == word:
            segments[-1]["frames"] += 1
        else:
            segments.append({"source_frame": index, "frames": 1, "buttons": word})
    expanded = [s["buttons"] for s in segments for _ in range(s["frames"])]
    if expanded != words:
        raise ValueError("run-length round trip failed")
    # Decode all 14 mapped bits again, including neutral and simultaneous input.
    for line, word in zip(lines[2:-1], expanded):
        original = line.split("|")[2]
        if [(word & (1 << bit)) == 0 for bit in BITS] != [c != "." for c in original]:
            raise ValueError("button round trip failed")
    word_bytes = b"".join(word.to_bytes(2, "little") for word in words)
    route = {
        "schema": "psx-tas-intake-v1", "qualification": "structural_only_playback_not_run",
        "clock": "source_emulator_frame", "first_frame_index": 0,
        "target_start_boundary": None, "button_encoding": "psx_active_low_u16",
        "source_download_sha256": sha(data), "source_movie_sha256": sha(movie),
        "source_header": header, "source_sync_settings": sync,
        "frame_count": len(words), "segments": segments,
    }
    receipt = {
        "schema": "psx-tas-intake-receipt-v1",
        "qualification": route["qualification"], "download_sha256": sha(data),
        "download_bytes": len(data), "wrapper_member": wrapper_member,
        "movie_sha256": sha(movie), "movie_bytes": len(movie),
        "members": {k: {"bytes": len(v), "sha256": sha(v)} for k, v in parts.items()},
        "header": header, "sync_settings": sync, "frame_count": len(words),
        "segment_count": len(segments), "roundtrip_equal": True,
        "pad_words_le_sha256": sha(word_bytes), "pressed_frame_counts": counts,
        "fits_inspected_4096_step_queue": len(segments) <= 4096,
        "source_frame_to_guest_vblank_equivalence": "unqualified",
        "source_emulator_reproduction": "not_run", "recomp_playback": "not_run",
    }
    return route, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("movie", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new private normalized-input JSON path")
    parser.add_argument("--source-page", required=True, help="attribution page for this replay")
    args = parser.parse_args()
    try:
        if args.movie.stat().st_size > MAX_BYTES:
            raise ValueError("input exceeds the intake size limit")
        route, receipt = inspect(args.movie.read_bytes())
        route["source_page"] = receipt["source_page"] = args.source_page
        output = (json.dumps(route, indent=2) + "\n").encode()
        with args.output.open("xb") as stream:
            stream.write(output)
        receipt["normalized_input_sha256"] = sha(output)
        receipt["importer_sha256"] = sha(Path(__file__).read_bytes())
        print(json.dumps(receipt, indent=2))
    except (ValueError, KeyError, UnicodeError, OSError, zipfile.BadZipFile) as error:
        parser.exit(2, f"intake rejected: {error}\n")


if __name__ == "__main__":
    main()
