#!/usr/bin/env python3
"""Title-neutral deterministic debug-input scheduling contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def model_contract() -> None:
    start = 1200
    remaining = 3
    observed = []
    for frame in range(1198, 1204):
        if frame < start:
            observed.append(-1)
            continue
        observed.append(0xFFEF if remaining else -1)
        if remaining:
            remaining -= 1
    assert observed == [-1, -1, 0xFFEF, 0xFFEF, 0xFFEF, -1]

    # Duration is counted by retail PAD samples, not by video frames.  Sparse
    # sampling keeps the override armed across intervening vblanks.
    remaining = 2
    sparse = []
    for frame in range(1200, 1206):
        sampled = frame in (1201, 1204)
        sparse.append(0xFFEF if sampled and remaining else -1)
        if sampled and remaining:
            remaining -= 1
    assert sparse == [-1, 0xFFEF, -1, -1, 0xFFEF, -1]
    assert remaining == 0


def main() -> None:
    source = (ROOT / "runtime/src/debug_server.c").read_text(encoding="utf-8")
    press = source[source.index("static void handle_press") : source.index(
        "static void handle_pad_status"
    )]
    consume = source[source.index("int debug_server_get_input_override") : source.index(
        "int debug_server_get_axis_override"
    )]

    assert 'json_get_int(json, "at_frame", -1)' in press
    assert "s_input_start_frame = at_frame >= 0" in press
    assert "s_input_start_frame > s_frame_count" in consume
    assert consume.index("s_input_start_frame > s_frame_count") < consume.index(
        "--s_input_frames"
    )
    assert "s_input_start_frame = 0" in consume
    assert "s_input_first_applied_frame = s_frame_count" in consume
    assert "s_input_last_applied_frame = s_frame_count" in consume
    assert "s_input_applied_count++" in consume
    model_contract()
    print("Deterministic debug-input scheduling contract: PASS")


if __name__ == "__main__":
    main()
