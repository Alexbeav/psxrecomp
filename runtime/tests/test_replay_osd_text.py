#!/usr/bin/env python3
"""Every replay on-screen message fits the toast (PS1B-316).

A hands-on run showed "Replay not recorded: port 1 has a mouse or another
non-pad devi", cut off at the window edge. The toast now breaks into at most
three lines of 38 characters (host_osd_wrap.h), which fits the 640-pixel
window. This reads every replay message out of the sources, fills its
arguments with the longest values they can take, and runs each through the
real line breaker (host_osd_wrap_test --fit). It fails when a message does
not fit, and when a source has a format string this file has no worst case
for, so a new message cannot be added without being measured.

usage: test_replay_osd_text.py <host_osd_wrap_test executable>
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION = (ROOT / "runtime" / "src" / "replay_session.c").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
IDENTITY = (ROOT / "runtime" / "src" / "input_route_session.c").read_text(encoding="utf-8")

LIT = r'"((?:[^"\\]|\\.)*)"'
RECORD, PLAY, SAVE = "Replay not recorded: ", "Replay not played: ", "Replay not saved: "

# Longest value each argument can take.
LONGEST_BOOT_NAME = "T" * 48 + "-boot-20261001T111417Z-99"     # base[49], stamp, suffix (replay_session_boot_path)
WORST = {
    "Replay saved: slot %d": ("12",),                           # REPLAY_SLOTS
    "Replay saved: %.*s": (LONGEST_BOOT_NAME,),
    "Replay from the same build on another platform (%s)": ("x" * 23,),   # s_rec_platform[24]
    # The host lists only settings it cannot switch: "enabled mods" today,
    # plus any key a newer build recorded.
    "Replay may go out of sync: different %s": ("enabled mods, cd_instant_rate",),
    "Replay finished (out of sync: %u RAM pages, cycle %+lld)": ("512", "-9223372036854775808"),
    "port %d needs a pad or the keyboard": ("2",),
}
# Formats that only prepend a prefix to a reason measured on its own rows.
PREFIX_FORMATS = {RECORD + "%s", PLAY + "%s", SAVE + "%s"}


def body(text: str, signature: str) -> str:
    """The function that starts at `signature`, to its closing brace."""
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


def fill(fmt: str) -> str:
    if "%" not in fmt:
        return fmt
    assert fmt in WORST, f"new replay message with arguments, add its longest values to WORST: {fmt!r}"
    values = list(WORST[fmt])
    out = re.sub(r"%[-+0-9.*]*(?:ll|l)?[a-zA-Z]", lambda m: values.pop(0), fmt)
    assert not values, fmt
    return out


messages: dict[str, str] = {}   # text -> where it comes from


def add(text: str, where: str) -> None:
    messages.setdefault(fill(text), where)


# ---- replay_session.c ----
for m in re.finditer(r"replay_host_osd\(\s*" + LIT, SESSION):
    add(m.group(1), "replay_session.c")
for m in re.finditer(r"end_playback\([A-Z_]+,\s*" + LIT, SESSION):
    add(m.group(1), "replay_session.c end of playback")
for m in re.finditer(r"refuse_record\(\s*" + LIT, SESSION):
    add(RECORD + m.group(1), "replay_session.c refusal")
for m in re.finditer(r"refuse_play\(\s*" + LIT, SESSION):
    add(PLAY + m.group(1), "replay_session.c refusal")
for m in re.finditer(r"snprintf\(msg, sizeof msg,\s*" + LIT, SESSION):
    if m.group(1) not in PREFIX_FORMATS:
        add(m.group(1), "replay_session.c")
# `error = "..."`: inside replay_session_play_file it is why a replay does not
# play; everywhere else in the file it is why a recording was not saved.
play_body = body(SESSION, "int replay_session_play_file(")
play_errors = re.findall(r"error = " + LIT, play_body)
save_errors = re.findall(r"error = " + LIT, SESSION.replace(play_body, ""))
assert play_errors and save_errors, "error texts not found in replay_session.c"
for e in play_errors:
    add(PLAY + e, "replay_session.c playback")
for e in save_errors:
    add(SAVE + e, "replay_session.c saving")

# ---- main.cpp: the host's reasons and its own toasts ----
reasons = re.findall(r"std::snprintf\(why, cap,\s*" + LIT, body(MAIN, 'extern "C" int replay_host_can_record('))
assert len(reasons) >= 6, f"replay_host_can_record reasons not found ({len(reasons)})"
for r in reasons:
    add(RECORD + fill(r), "main.cpp replay_host_can_record")
for m in re.finditer(r"replay_host_osd\(\s*" + LIT, MAIN):
    add(m.group(1), "main.cpp")
for m in re.finditer(r"host_osd_push\(\s*" + LIT, MAIN):
    if "replay" in m.group(1).lower():
        add(m.group(1), "main.cpp")
# A save state refused while a replay runs: savestate.c shows "Slot N needs disc D: <reason>".
gate = re.search(r'"(the disc cannot change while a replay records or plays)"',
                 body(MAIN, 'extern "C" int psx_frontend_savestate_mount_disc('))
if gate:
    messages.setdefault("Slot 12 needs disc 4: " + gate.group(1), "main.cpp save-state mount gate")

# ---- input_route_session.c: why the product has no replay identity ----
for signature in ("static const char *disc_digest_uncached(uint32_t *kind, uint8_t out[32], int use_file_cache)\n{",
                  "int input_route_session_identity("):
    texts = re.findall(r'(?:return|error =) ' + LIT, body(IDENTITY, signature))
    assert texts, f"no identity reasons found in {signature[:40]}"
    for t in texts:
        add(RECORD + t, "input_route_session.c identity")
        add(PLAY + t, "input_route_session.c identity")

assert len(messages) >= 50, f"only {len(messages)} replay messages found; the extraction no longer matches the sources"

with tempfile.TemporaryDirectory() as tmp:
    listing = Path(tmp) / "messages.txt"
    listing.write_text("\n".join(messages) + "\n", encoding="utf-8", newline="\n")
    run = subprocess.run([sys.argv[1], "--fit", str(listing)], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
rows = [line.split("\t") for line in run.stdout.splitlines() if line.strip()]
assert len(rows) == len(messages), f"the line breaker measured {len(rows)} of {len(messages)} messages\n{run.stderr}"
print(f"{'lines':>5} {'widest':>6} {'length':>6}  message")
for lines, widest, length, text, *flag in sorted(rows, key=lambda r: (-int(r[2]), r[3])):
    print(f"{lines:>5} {widest:>6} {length:>6}  {text}{'   <-- DOES NOT FIT' if flag else ''}")
bad = [r[3] for r in rows if len(r) > 4]
assert run.returncode == 0 and not bad, f"{len(bad)} replay message(s) do not fit three lines of 38: {bad}"
assert all(int(r[1]) <= 38 and int(r[0]) <= 3 for r in rows)
print(f"replay on-screen text: {len(rows)} messages, all fit 3 lines of 38 characters")
