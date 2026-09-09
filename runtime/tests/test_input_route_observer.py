"""Exercise the compiled observer with synthetic RAM and no game assets."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as root:
    root = Path(root)
    bad_watches = {"watch_unaligned":"1", "watch_range":"0x200000", "watch_empty":"", "watch_duplicate":"0,0",
                   "watch_trailing":"0,", "watch_space":"\t0", "watch_limit":",".join(str(2*i) for i in range(33))}
    for case in ("valid", "cpu", "video", "cpu_missing", "missing", "wrong", "disconnected", "analog", "tail_pressed", "collision", "bad_tail", *bad_watches):
        out = root / case
        out.mkdir()
        env = dict(os.environ, PSX_INPUT_ROUTE_CAPTURE_DIR=str(out), PSX_INPUT_ROUTE_NEUTRAL_TAIL="1", PSX_INPUT_ROUTE_TRACE="1")
        env["PSX_INPUT_ROUTE_WATCH_U16"] = bad_watches.get(case, "0,0x1ffffe")
        env["PSX_INPUT_ROUTE_CPU_STATE"] = "1" if case.startswith("cpu") else "0"
        env["PSX_INPUT_ROUTE_VIDEO_STATE"] = "1" if case=="video" else "0"
        if case == "collision":
            (out / "checkpoints.jsonl").write_bytes(b"preserve")
        if case == "bad_tail":
            env["PSX_INPUT_ROUTE_NEUTRAL_TAIL"] = "1garbage"
        run = subprocess.run([str(exe), case], env=env, capture_output=True)
        expected = 0 if case in ("valid", "cpu", "video") else 4 if case == "bad_tail" or case in bad_watches else 3
        assert run.returncode == expected, (case, run.returncode, run.stderr)
        if case in ("valid", "cpu", "video"):
            if case == "video":
                rows=[json.loads(x) for x in (out/"video-state.jsonl").read_text().splitlines()]
                assert [x["frame"] for x in rows]==[0,1,2,3]
                assert all(x["cycle"]==123456 and x["display_mode"]==100 and x["vertical_start"]==16 and x["vertical_end"]==256 and x["gpu_field"]==1 and x["lcf"]==0 and x["cycles_since_vblank"]==7 and x["next_period"]==563969 and x["profile_field"]==1 for x in rows)
            else:assert not (out/"video-state.jsonl").exists()
            if case == "cpu":
                rows=[json.loads(x) for x in (out/"cpu-state.jsonl").read_text().splitlines()]
                assert [x["frame"] for x in rows] == [0,1,2,3]
                assert all(x["timers"] == [dict(channel=i,counter=0xFFFF-i,mode=0x148+i,target=i,irq_line=i-1,fraction=2145-i) for i in range(3)] for x in rows)
                assert all(x["sr"]==0x40000401 and x["cause"]==0x420 and x["epc"]==0x80012000 and x["i_stat"]==5 and x["i_mask"]==13 for x in rows)
            else:
                assert not (out/"cpu-state.jsonl").exists()
            done = json.loads((out / "complete.json").read_text())
            end = json.loads((out / "input-end.json").read_text())
            assert done["frame"] == 3 and done["input_frames"] == 2 and done["neutral_tail_ticks"] == 1
            assert end["frame"] == 2
            assert done["applied_words_sha256"] == hashlib.sha256(bytes.fromhex("f7ffffff")).hexdigest()
            assert end["applied_words_sha256"] == done["applied_words_sha256"]
            assert json.loads((out / "device-events.json").read_text()) == []
            watches = [json.loads(line) for line in (out / "ram-u16.jsonl").read_text().splitlines()]
            assert [w["frame"] for w in watches] == [0,1,2,3]
            assert all(w["ram_u16"] == {"000000":0x1234,"1FFFFE":0xABCD} for w in watches)
            assert (out / "frame-000003.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
        else:
            assert not (out / "complete.json").exists()
        if case == "collision":
            assert (out / "checkpoints.jsonl").read_bytes() == b"preserve"
print("input_route_observer: 18 compiled cases passed")
