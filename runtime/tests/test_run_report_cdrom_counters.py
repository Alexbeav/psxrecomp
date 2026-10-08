#!/usr/bin/env python3
"""Guard that the run report carries the silent-read counters (PS1B-317).

test_cdrom_silent_read_counter.c proves what the drive counts. This proves the
numbers reach psx_last_run_report.json, which is the only place a release
build shows CD state: the report writer links into the game runtime only, so
its wiring is checked in the source.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPORT = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
CDROM = (ROOT / "runtime" / "src" / "cdrom.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "cdrom.h").read_text(encoding="utf-8")

assert "void cdrom_silent_read_stats(uint32_t *starts, uint32_t *silent, uint32_t *longest_run);" in HEADER

call = REPORT.index("cdrom_silent_read_stats(&starts, &silent, &longest_run);")
fmt = re.search(
    r'"  \\"cdrom\\": \{\\"read_starts\\": %u, \\"silent_reads\\": %u, "\s*'
    r'"\\"silent_run_max\\": %u\},\\n",\s*starts, silent, longest_run\);',
    REPORT[call:],
)
assert fmt, 'the report must write "cdrom": {"read_starts", "silent_reads", "silent_run_max"} from the counters'

# The counters are fed where a stream starts and where one ends, and nowhere
# decide anything: outside the accounting they are only read by the getter.
assert re.search(r"cd_stream_account_end\(\);[^\n]*\n\s*s_cd_stream_open_seq = s_cd_timing_total;\s*\n\s*s_cd_stream_starts\+\+;", CDROM), (
    "start_read_stream must close the previous stream and open the new one"
)
assert re.search(r"static void stop_read_stream\(void\) \{\s*cd_stream_account_end\(\);", CDROM), (
    "stop_read_stream must account the stream it ends"
)


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


# Outside their declaration, the accounting function, the getter and the one
# increment at a read start, no code reads or writes the counters.
rest = CDROM
for part in (body(CDROM, "static void cd_stream_account_end(void) {"),
             body(CDROM, "void cdrom_silent_read_stats("),
             "static uint32_t s_cd_stream_starts, s_cd_silent_reads, s_cd_silent_run, s_cd_silent_run_max;",
             "s_cd_stream_starts++;"):
    assert rest.count(part) == 1, part[:60]
    rest = rest.replace(part, "")
for name in ("s_cd_silent_reads", "s_cd_silent_run_max", "s_cd_silent_run", "s_cd_stream_starts"):
    assert not re.search(rf"{name}", rest), f"{name} is used outside the accounting: the counters must not steer the drive"


# PS1G-103: the counters of the floor of a read after Setloc reach the report
# under their own key, so a release build shows which titles the floor touches.
assert "void cdrom_seek_floor_stats(uint32_t *reads, uint64_t *cycles);" in HEADER
floor_call = REPORT.index("cdrom_seek_floor_stats(&floor_reads, &floor_cycles);")
assert re.search(
    r'"  \\"cdrom_seek_floor\\": \{\\"reads\\": %u, \\"added_cycles\\": %llu\},\\n",\s*'
    r"floor_reads, \(unsigned long long\)floor_cycles\);",
    REPORT[floor_call:],
), 'the report must write "cdrom_seek_floor": {"reads", "added_cycles"} from the counters'

# They are written where the floor raises a deadline and read by the getter;
# nothing else uses them.
rest = CDROM
for part in (body(CDROM, "void cdrom_seek_floor_stats("),
             "static uint32_t s_cd_seek_floor_reads;",
             "static uint64_t s_cd_seek_floor_cycles;",
             "s_cd_seek_floor_reads++;",
             "s_cd_seek_floor_cycles += (uint64_t)(floor_cycles - read_delay);"):
    assert rest.count(part) == 1, part[:60]
    rest = rest.replace(part, "")
for name in ("s_cd_seek_floor_reads", "s_cd_seek_floor_cycles"):
    assert name not in rest, f"{name} is used outside the accounting: the counters must not steer the drive"


print("run report cdrom counters: ok")
