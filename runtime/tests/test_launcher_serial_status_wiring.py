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
# says nothing, and the row kept its tick. The host compares what was read
# with every serial of the set and names the whole list. An image that is in
# the disc list with no serial listed stays unjudged.
outside = re.search(
    r"\} else if \(!g_disc_serials\.empty\(\) && id\.opened && id\.has_header &&\s*"
    r"PSXRecompV4::disc_roster_index\(\s*g_disc_metadata_roster, std::filesystem::path\(disc_path\)\) < 0\) \{",
    block,
)
assert outside, "a set must judge an image outside its disc list"
set_block = block[outside.end():]
assert "for (const std::string& one : g_disc_serials) {" in set_block
assert "if (!got.empty() && uppercase_ascii(one) == got) listed = true;" in set_block
assert re.search(r"out->serial_status = listed \? RECOMP_SERIAL_LISTED\s*: RECOMP_SERIAL_NOT_LISTED;", set_block)
assert re.search(
    r'std::snprintf\(out->expected_serials, sizeof\(out->expected_serials\),\s*"%s", all\.c_str\(\)\);', set_block
), "the sentence for a set names every serial of the set"
assert "out->verdict" not in set_block, "the set branch decides the row and the sentence, never the verdict"

# The status is set from the same identity the verdict is, and before it: the
# wrong-disc verdict and the row cannot disagree.
verdict = body.index("out->verdict = 3; // wrong disc")
assert guard < verdict
assert "else if (id.expected_serial_given && !id.serial_matches)     out->verdict = 3; // wrong disc" in body
assert body.count("PSXRecompV4::identify_disc(") == 1

print("launcher serial status wiring: ok")
