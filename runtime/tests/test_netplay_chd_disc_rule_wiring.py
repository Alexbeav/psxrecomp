#!/usr/bin/env python3
"""Guard that the netplay disc rule runs for a .chd (PS1B-294).

identify_disc reads a .chd in a branch of its own. That branch returned
without applying the title's [netplay] rule, so a wrong .chd passed the
refusal in main.cpp and started a match that then fell apart. Products mount
.chd, so the rule never ran for them.

The rule itself is tested on identities in test_disc_identity.cpp. No .chd can
be made in a test (libchdr only reads), so that the branch reaches the rule at
each way out is checked in the source.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "runtime" / "src" / "disc_identity.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "disc_identity.h").read_text(encoding="utf-8")

assert re.search(r"bool\s+from_chd\s*=\s*false;", HEADER), "DiscIdentity has no from_chd field"

# The rule: require_cue does not refuse a .chd; tracks and fingerprint still do.
start = SOURCE.index("void apply_netplay_disc_expect(")
rule = SOURCE[start : SOURCE.index("\nDiscIdentity identify_disc(", start)]
assert "if (expect.require_cue && !id.from_chd)" in rule, "require_cue must not refuse a .chd"
assert rule.index("expect.require_cue") < rule.index("expect.required_tracks") < rule.index(
    "expect.required_disc_fp"
), "a .chd must still meet the track count and the fingerprint"

# The .chd branch of identify_disc, from its test to the first statement after it.
start = SOURCE.index("DiscIdentity identify_disc(")
body = SOURCE[start:]
assert "v.from_chd = is_chd_path(resolved.mount);" in body, "identify_disc does not record a .chd mount"
branch_start = body.index("if (v.from_chd) {")
branch = body[branch_start : body.index("std::ifstream f(data_path", branch_start)]
assert "apply_netplay_disc_expect(v, *netplay_expect);" in branch, "the .chd branch never applies the rule"

def check_returns(text):
    returns = re.findall(r"return v;", text)
    assert len(returns) >= 3, "expected the open failure, the missing header and the normal way out"
    # Allow the nonfatal companion warning between the verdict and the return,
    # regardless of line length; no intervening branch or other statement.
    checked = re.findall(
        r"netplay_verdict\(\);\s*"
        r"(?:v\.sbi_warning\s*=\s*missing_sbi_warning\([^;{}]*\);\s*)?"
        r"return v;", text
    )
    assert len(checked) == len(returns), "a way out of the .chd branch skips the netplay rule"


check_returns(branch)

# Removing any actual call, moving it after the return, or guarding it with
# an unrelated condition must still fail this source wiring check.
calls = list(re.finditer(r"netplay_verdict\(\);", branch))
mutations = [branch[:call.start()] + "(void)0;" + branch[call.end():] for call in calls]
mutations.append(re.sub(r"netplay_verdict\(\);(\s*)(return v;)",
                        r"\2\1netplay_verdict();", branch, count=1))
mutations.append(branch.replace("netplay_verdict();", "if (false) { netplay_verdict(); }", 1))
for mutated in mutations:
    assert mutated != branch, "negative control did not change the source"
    try:
        check_returns(mutated)
    except AssertionError:
        continue
    raise AssertionError("a missing or bypassed netplay call passed the wiring check")

print(f"netplay chd disc rule wiring test: PASS ({len(calls)} returns, {len(mutations)} rejected mutations)")
