#!/usr/bin/env python3
"""Guard the notice for a fast_boot = false that has no effect (PS1B-354).

fast_boot only ever asks for the BIOS boot-skip. With bios_hle on and
bios_hle_keep_intro off the boot is skipped whatever fast_boot says, so a
written fast_boot = false changed nothing, and nothing said so: the start line
`bios_boot=HLE (shell skipped)` was the same for true and false.

runtime/tests/test_bios_hle_plan.c proves the rule (plan.fast_boot_ignored).
This proves the wiring around it:

  1. the loader records that game.toml wrote fast_boot, and main() keeps it;
  2. the plan is told whether fast_boot was written: game.toml, and offline
     settings.toml too (a match boots with the kit's values, PS1B-382);
  3. after the plan, main() prints the sentence once and hands the plan to the
     run report, which a product keeps when stdout is gone (PS1B-400);
  4. the run report's "boot" object carries the flag and the sentence, and the
     sentence is safe inside a JSON string.

Reading source rather than running a binary is deliberate: the plan sits in the
middle of main(), and a start needs a game build, a disc and a BIOS.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig").replace("\r\n", "\n")


# Column alignment is not part of the contract: one space everywhere.
MAIN = re.sub(r"[ \t]+", " ", read("runtime/src/main.cpp"))
LOADER = re.sub(r"[ \t]+", " ", read("recompiler/src/config_loader.cpp"))
CONFIG = read("recompiler/src/config_loader.h")
TRACE = read("runtime/src/crash_trace.c")
TRACE_H = read("runtime/include/crash_trace.h")
PLAN_H = read("runtime/include/bios_hle_plan.h")

# ---- 1. game.toml's fast_boot line is recorded and kept -----------------------
assert re.search(r"bool\s+has_fast_boot\s*=\s*false;", CONFIG), (
    "RuntimeConfig no longer records whether game.toml wrote fast_boot"
)
read_line = LOADER.index('rt.fast_boot = toml::find<bool>(runtime, "fast_boot");')
assert "rt.has_fast_boot = true;" in LOADER[read_line : read_line + 120], (
    "the loader does not mark fast_boot written where it reads it"
)
assert re.search(r"bool kit_fast_boot_set = false;", MAIN), "kit_fast_boot_set must default to false"
assert MAIN.count("kit_fast_boot_set = gc.runtime.has_fast_boot;") == 1, (
    "main() must take game.toml's fast_boot presence at config load, once"
)
assert len(re.findall(r"(?m)^ ?kit_fast_boot_set =[^=]", MAIN)) == 1, (
    "kit_fast_boot_set must be set at config load only"
)

# ---- 2. the plan is told whether fast_boot was written ------------------------
plans = [m.start() for m in re.finditer(r"psx_bios_hle_plan\(req\)", MAIN)]
assert len(plans) == 1, "expected one BIOS plan call"
plan = plans[0]
fed = MAIN[MAIN.rfind("PsxBiosHleRequest req;", 0, plan) : plan]
m = re.search(r"req\.fast_boot_set = \((.*?)\)\s*\? 1 : 0;", fed, re.S)
assert m, "the plan's fast_boot_set is not set where the request is built"
cond = re.sub(r"\s+", " ", m.group(1))
assert cond == "kit_fast_boot_set || (!net_cfg.enabled && settings_chose_fast_boot)", (
    f"fast_boot_set must be game.toml, or settings.toml outside a match: {cond}"
)

# ---- 3. said once on stdout, and handed to the run report ---------------------
after = MAIN[plan : plan + 4000]
said = after.find('if (plan.fast_boot_ignored)\n std::fprintf(stdout, "psxrecomp: " PSX_FAST_BOOT_IGNORED_NOTICE "\\n");')
assert said >= 0, "main() no longer prints the notice when the plan reports fast_boot_ignored"
assert MAIN.count("PSX_FAST_BOOT_IGNORED_NOTICE") == 1, "the notice must be printed at one place"
noted = after.find("psx_crash_trace_note_boot(plan.boot_skip, plan.fast_boot_ignored);")
assert noted > said, "the run report is not told the boot plan after the notice"
assert after.find("psx_bios_hle_configure(plan.call_hle, plan.boot_skip);") < said, (
    "the notice must follow the boot decision, not replace or precede it"
)

# ---- 4. the run report carries it, and the sentence fits a JSON string --------
assert "void psx_crash_trace_note_boot(int boot_skip, int fast_boot_ignored);" in TRACE_H
assert '#include "bios_hle_plan.h"' in TRACE, "crash_trace.c needs the notice's definition"
assert re.search(
    r'\\"boot\\": \{\\"boot_skip\\": %d, \\"fast_boot_ignored\\": %d, "\s*'
    r'"\\"notice\\": \\"%s\\"\},\\n",\s*'
    r"s_boot_skip, s_boot_fast_boot_ignored,\s*"
    r's_boot_fast_boot_ignored \? PSX_FAST_BOOT_IGNORED_NOTICE : ""\);',
    TRACE,
), "the run report's boot object no longer carries the flag and the sentence"

macro = re.search(r"#define PSX_FAST_BOOT_IGNORED_NOTICE((?:.*\\\n)*.*)", PLAN_H)
assert macro, "PSX_FAST_BOOT_IGNORED_NOTICE is gone from bios_hle_plan.h"
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', macro.group(1))
sentence = "".join(parts)
assert sentence.startswith("fast_boot = false has no effect while bios_hle is on"), sentence
assert "bios_hle = false" in sentence and "bios_hle_keep_intro = true" in sentence, (
    "the notice must name the two keys that do play the intro"
)
assert not re.search(r'["\\\x00-\x1f]', sentence), "the notice is not safe inside a JSON string"

print("fast_boot notice wiring test: PASS")
