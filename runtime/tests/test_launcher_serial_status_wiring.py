#!/usr/bin/env python3
"""Guard that the host tells the launcher whether a disc's serial is listed (PS1B-403).

A player gave a build made for SLES-03396 a disc of another release,
SLES-03398. The launcher's disc panel showed "Serial  SLES-03398" with a tick,
because it ticked any serial that was read, and the only sentence on the panel
was the online-play note about the TOC fingerprint. He could not tell that he
had the wrong release.

The launcher (recomp-ui, tests/launcher_disc_serial_test.c) now draws a cross
and says "This disc is X. This build needs Y." when the host says the serial
is not listed. This guard is the host's half: ae_disc_verify says so. The
callback needs a disc image and the launcher's window, so it is checked in the
source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

start = MAIN.index("int ae_disc_verify(const char* disc_path, RecompLauncherCDiscVerify* out) {")
body = MAIN[start : MAIN.index("int ae_memcard_inspect(", start)]

# The fields exist only in a recomp-ui that has them: an older launcher still
# compiles, and gets the old row.
guard = body.index("#if defined(RECOMP_LAUNCHER_HAS_SERIAL_STATUS)")
block = body[guard : body.index("#endif", guard)]

# The host speaks only for a build that lists a serial, and says listed or not
# listed only for a disc it could read. A disc that did not open is the ISO
# header row's to explain.
listed = block.index("if (id.expected_serial_given) {")
readable = block.index("if (id.opened && id.has_header) {", listed)
assert re.search(
    r"out->serial_status = id\.serial_matches \? RECOMP_SERIAL_LISTED\s*: RECOMP_SERIAL_NOT_LISTED;",
    block,
), "the host must say listed or not listed from the identity's own match"

# What the build needs is the serial this disc was checked against: the set's
# per-disc value, or the game's own.
assert re.search(
    r'std::snprintf\(out->expected_serials, sizeof\(out->expected_serials\),\s*"%s", expect_serial\.c_str\(\)\);',
    block,
), "the sentence needs the serial the disc was checked against"

# The row shows what was read from the disc. Before this it fell back to the
# EXPECTED serial when nothing was read, so a disc with no readable serial and
# a file that did not open both showed the right serial with a tick. This is
# outside the "could read it" test: it holds for a file that did not open too.
shown = re.search(
    r'if \(!id\.serial_matches\)\s*std::snprintf\(out->serial, sizeof\(out->serial\), "%s",\s*id\.detected_serial\.c_str\(\)\);',
    block,
)
assert shown, "a disc that does not match must show the serial that was read, not the expected one"
assert listed < shown.start() < readable, "the row's serial must not depend on the disc having opened"
assert block.index("out->serial_status = ") > readable
assert block.index("out->expected_serials") > readable

# A set, and an image its disc list does not know (a copy under another name,
# or another game's disc): no serial is expected for it, so the branch above
# says nothing. It is judged by its serial against every serial of the set
# (disc_roster_judge_serial, run by test_disc_roster.cpp). An image that is in
# the disc list is judged by its own entry, as before. The judgement is made
# outside the launcher's #if: the verdict must not depend on which launcher is
# compiled in.
judged = body.index("const PSXRecompV4::DiscSetSerial set_serial =")
assert judged < guard, "the set judgement must not sit inside the launcher's #if"
judgement = body[judged:guard]
assert re.search(
    r"\(!id\.expected_serial_given && id\.opened && id\.has_header &&\s*"
    r"PSXRecompV4::disc_roster_index\(g_disc_metadata_roster,\s*"
    r"std::filesystem::path\(disc_path\)\) < 0\)",
    judgement,
), "only an image outside the disc list, that opened, with no expected serial, is judged by the set"
assert re.search(
    r"PSXRecompV4::disc_roster_judge_serial\(\s*g_disc_serials, g_disc_metadata_roster\.size\(\), id\.detected_serial\)",
    judgement,
)
# Wrong disc (Alex, 2026-10-02): a serial was read and it is none of the set's.
# A disc from which no serial was read keeps the verdict it had.
assert re.search(
    r"const bool set_wrong_disc =\s*set_serial == PSXRecompV4::DiscSetSerial::NotListed &&\s*"
    r"!id\.detected_serial\.empty\(\);",
    judgement,
)

# The row and the sentence: the mark from the judgement, and every serial of
# the set.
set_block = block[block.index("} else if (set_serial != PSXRecompV4::DiscSetSerial::NotJudged) {"):]
assert re.search(
    r"out->serial_status = set_serial == PSXRecompV4::DiscSetSerial::Listed\s*"
    r"\? RECOMP_SERIAL_LISTED : RECOMP_SERIAL_NOT_LISTED;",
    set_block,
)
assert "PSXRecompV4::disc_roster_serial_list(g_disc_serials).c_str()" in set_block, (
    "the sentence for a set names every serial of the set"
)
assert "out->verdict" not in block, "the launcher's #if decides the row and the sentence, never the verdict"

# The verdict: wrong disc for a serial that does not match its entry, and for
# an image outside the set whose serial is none of the set's. Headline "Disc
# verification failed", PLAY greyed, as in a single-disc build. The status is
# set from the same identity, before the verdict: the two cannot disagree.
verdict = re.search(
    r"else if \(\(id\.expected_serial_given && !id\.serial_matches\) \|\|\s*"
    r"set_wrong_disc\)\s*out->verdict = 3; // wrong disc",
    body,
)
assert verdict, "a set must refuse a disc whose serial is none of the set's"
assert guard < verdict.start()
assert body.count("PSXRecompV4::identify_disc(") == 1

print("launcher serial status wiring: ok")
