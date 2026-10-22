#!/usr/bin/env python3
"""Guard that a start a player makes says why it was refused (PS1G-63).

test_start_refusal.c proves what the module records and prints. This proves
the refusals of main() reach it and the report writer prints it. The report
writer and main() link into the game runtime only, so their wiring is checked
in the source.

Before this, a refused start, a closed launcher and a setup program's exit all
left psx_last_run_report.json with reason "atexit", exit_origin "unknown" and
frame 0: a player's report could not say why the game did not start.

Scope. Covered: the refusals a player can meet before the first frame, listed
in SITES below. The failed netplay start carries the sentence PS1B-386 wrote
for it. Not covered, and still untagged: the developer gates that exit with
code 2 (input routes, TAS state files, the GPU work model): no player starts
those.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
REPORT = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")
DOC = (ROOT / "docs" / "RUN_REPORT_START.md").read_text(encoding="utf-8")

# The module is part of every runtime build.
assert "runtime/src/start_refusal.c" in CMAKE

# The report prints both values, after the header and from the module.
assert '#include "start_refusal.h"' in REPORT
call = REPORT.index("psx_start_refusal_json(refusal, sizeof(refusal))")
rows = REPORT.index("psx_start_launcher_status_json(rows, sizeof(rows))")
assert REPORT.index('"  \\"start_refused\\": "') > call
assert REPORT.index('",\\n  \\"launcher_status\\": "') > rows
# The writer's buffers hold a text of the cap with every byte escaped.
assert "static char refusal[6 * (" in REPORT and "static char rows[2 * 6 * " in REPORT

# One helper records the refusal, tags the exit and returns the exit code. It
# opens a box only for a person: a scripted start must never wait for a click.
helper = MAIN[MAIN.index("static int refuse_start(const char* kind, const char* title,"):]
helper = helper[: helper.index("\n}\n") + 3]
assert "psx_start_refusal_set(kind, s_player_message_title.c_str()," in helper
assert "psx_start_refusal_set(kind, title, sentence.c_str());" in helper
assert 'psx_crash_trace_set_exit_origin("start_refused");' in helper
assert re.search(r"if \(s_start_interactive && !g_headless\)\s*SDL_ShowSimpleMessageBox\(", helper)

# "A person started it" knows every way of skipping the launcher, the setting
# in settings.toml too. That setting is read after the command line, so the
# switch is set twice: without it, then with it.
interactive = MAIN[MAIN.index("static bool start_is_interactive(bool force_launcher, bool force_no_launcher,"):]
interactive = interactive[: interactive.index("\n}\n") + 3]
assert "if (g_headless) return false;" in interactive
assert "if (force_launcher) return true;" in interactive
assert re.search(
    r'return !force_no_launcher && !std::getenv\("PSX_NO_LAUNCHER"\) &&\s*!skip_launcher_setting;',
    interactive,
)
first = MAIN.index("start_is_interactive(force_launcher, force_no_launcher, false);")
setting = MAIN.index("if (us.has_skip_launcher)  skip_launcher_setting = us.skip_launcher;")
second = re.search(
    r"s_start_interactive = start_is_interactive\(\s*force_launcher, force_no_launcher, skip_launcher_setting\);",
    MAIN[setting:],
)
assert second and second.start() < 400, "the skip_launcher setting must reach the switch where it is read"
assert first < setting

# A warning the check itself has shown is the reason: launcher_warning counts
# and keeps it.
warning = MAIN[MAIN.index("static void launcher_warning(const char* title, const std::string& msg) {"):]
warning = warning[: warning.index("\n}\n")]
assert "++s_player_message_seq;" in warning
assert "s_player_message_text  = msg;" in warning

main_body = MAIN[MAIN.index("\nint main(int argc, char** argv) {"):]
first_frame = main_body.index('"psxrecomp runtime: executing from PC=0x%08X\\n"')
before_frame = main_body[:first_frame]

# Each refusal goes through the helper, with its kind. The old line stays on
# stderr for the tools that read it.
SITES = [
    ('"psxrecomp: cannot create --memcard-dir %s: %s\\n"', "memcard_dir"),
    ('"BIOS images would desync)\\n"', "netplay_session_bios"),
    ('"psxrecomp: overlay cache init failed: %s\\n"', "overlay_cache"),
    ('"psxrecomp: no disc image selected; exiting.\\n"', "no_disc"),
    ("if (resolved_disc.empty() && !s_wrong_disc_sentence.empty()) {", "wrong_disc"),
    ('"psxrecomp: cannot clear mods for netplay: %s\\n"', "mods"),
    ('"psxrecomp: cannot launch with selected mods: %s\\n"', "mods"),
    ('"psxrecomp: no BIOS selected; exiting.\\n"', "no_bios"),
    ('"psxrecomp: BIOS activate failed for %s%s\\n"', "bios_mismatch"),
    ('launcher_warning("Disc Could Not Be Mounted", detail);', "disc_not_mounted"),
    ('launcher_warning("Already running",', "already_running"),
    ('"SDL_Init failed: %s\\n"', "video_init"),
    ('"SDL_CreateWindow failed: %s\\n"', "video_init"),
    ('"SDL_CreateRenderer failed: %s\\n"', "video_init"),
    ('"failed to allocate %dx staging buffer\\n"', "video_init"),
    ('"SDL_CreateTexture failed: %s\\n"', "video_init"),
    ('"psxrecomp: netplay refused — disc mount not valid for "', "netplay_disc"),
    ('"psxrecomp: netplay refused — no verified disc TOC "', "netplay_disc"),
    ('"psxrecomp: netplay refused — empty bind "', "netplay_address"),
    ('"psxrecomp: netplay refused — %s\\n", netplay_seat_why);', "netplay_seat"),
    ("const char* const why = netplay_start_failure(nrc, net_cfg);", "netplay_start"),
    ('"psxrecomp: FATAL: kernel-bless table of %s has %u rows; "', "build_defect"),
]
for line, kind in SITES:
    at = before_frame.index(line)
    tail = before_frame[at : at + 1400]
    stop = re.search(r"\breturn\b[^;]*;", tail)
    assert stop, f"no return after {line}"
    through_helper = re.match(r"return refuse_start(_reported)?\(", stop.group(0))
    # One site closes SDL between the refusal and the return.
    held = re.search(r'const int refused = refuse_start\("' + kind + '"', tail[: stop.start()])
    assert through_helper or (held and stop.group(0) == "return refused;"), (
        f"the exit after {line} must go through refuse_start, found: {stop.group(0)[:60]}"
    )
    assert f'"{kind}"' in stop.group(0) or held or kind == "no_bios", (
        f"the exit after {line} must be of kind {kind}"
    )

# A seat that is not a pad stops a command-line match only; a match from the
# lobby goes on and ends with the sentence in the room.
seat = before_frame[before_frame.index('"psxrecomp: netplay refused — %s\\n", netplay_seat_why);'):][:300]
assert re.search(r"if \(!g_netplay_from_lobby\)\s*return refuse_start\(\"netplay_seat\"", seat)

# A failed netplay start is the same: a command-line match stops with the
# sentence PS1B-386 wrote; a match from the lobby returns to the room with it.
failed = before_frame[before_frame.index("const char* const why = netplay_start_failure(nrc, net_cfg);"):][:500]
assert re.search(
    r'if \(!g_netplay_from_lobby\)\s*return refuse_start_reported\("netplay_start", "Match could not start",\s*why, s_netplay_start_report\);',
    failed,
)
# The run report holds no address: players send that file to other people, and
# the other player's address is another person's. The box and the log get the
# sentence; the report gets the same words with each address as one fixed word
# (netplay_start_failure_report_text, run by test_netplay_exit_reason.c).
reported = MAIN[MAIN.index("static int refuse_start_reported(const char* kind, const char* title,"):]
reported = reported[: reported.index("\n}\n") + 3]
assert "psx_start_refusal_set(kind, title, report_sentence.c_str());" in reported
assert "psx_start_refusal_set(kind, title, sentence.c_str());" not in reported, (
    "the sentence with the address must not be what the report stores"
)
assert re.search(r"if \(s_start_interactive && !g_headless\)\s*SDL_ShowSimpleMessageBox\(SDL_MESSAGEBOX_WARNING, title,\s*sentence\.c_str\(\), NULL\);", reported)
assert 'psx_crash_trace_set_exit_origin("start_refused");' in reported
failure = MAIN[MAIN.index("static const char* netplay_start_failure(int nrc, const PsxNetplayConfig& cfg) {"):]
failure = failure[: failure.index("\n}\n")]
assert re.search(
    r"netplay_start_failure_report_text\(nrc, netplay_built,\s*tried\[0\] \? tried : cfg\.bind_hostport,\s*"
    r"cfg\.peer_hostport, bind_probe, sys_error, sys_text,\s*s_netplay_start_report,",
    failure,
), "the report's sentence must be built from the same failure as the box's"
# No other refusal takes its sentence from an address: the listen and peer
# texts appear in main() only on stderr lines and in this one call.
assert before_frame.count("refuse_start_reported(") == 1

# The second netplay_disc sentence names both forms a verified disc can have:
# a .chd of the right disc passes the online rule too.
assert re.search(
    r'"so the match cannot start\. Select the disc image this build "\s*"needs: its \.cue file, or a \.chd\."',
    before_frame,
)
assert "with its .cue file" not in MAIN
assert 'netplay_soft_exit("netplay_start_failed");' in failed

# A setup program links no game and no BIOS code. Its refusal has its own
# kind, so a report from the kit's own exe is told from a product's.
assert re.search(
    r'return refuse_start\(psx_bios_registry_count == 0 \? "setup_program" : "no_bios",',
    before_frame,
)

# The checks that show their own box are measured from before they ran.
assert re.search(
    r"const unsigned disc_messages = s_player_message_seq;\s*"
    r"resolved_disc = resolve_disc_for_runtime\(",
    before_frame,
)
assert re.search(
    r"const unsigned bios_messages = s_player_message_seq;\s*"
    r"std::filesystem::path resolved_bios =\s*resolve_bios_for_runtime\(",
    before_frame,
)

# An unreadable configuration is recorded whether or not its box opened.
config = before_frame[before_frame.index('"psxrecomp: failed to load --game %s: %s\\n"'):][:1600]
assert 'psx_start_refusal_set("config_unreadable",' in config
assert 'psx_crash_trace_set_exit_origin("start_refused");' in config

# The two ends that are not refusals name themselves too.
closed = before_frame[before_frame.index('"psxrecomp: launcher closed; exiting.\\n"'):][:500]
assert 'psx_crash_trace_set_exit_origin("launcher_closed");' in closed
relaunch = before_frame[before_frame.index('"psxrecomp: relaunch after generate/rebuild\\n"'):][:300]
assert 'psx_crash_trace_set_exit_origin("setup_relaunch");' in relaunch

# What the launcher's rows said last goes into the report: a red row blocks
# Play, and that start ends as "launcher closed".
bios_row = MAIN[MAIN.index("int ae_bios_verify(const char* bios_path, RecompLauncherCBiosVerify* out) {"):][:900]
assert '~RowNote() { psx_start_note_launcher("bios", row->detail); }' in bios_row
disc_row = MAIN[MAIN.index("int ae_disc_verify(const char* disc_path, RecompLauncherCDiscVerify* out) {"):]
disc_row = disc_row[: disc_row.index("int ae_memcard_inspect(")]
assert 'psx_start_note_launcher("disc", row);' in disc_row

# Once the game runs, both report values are null: the rows are forgotten at
# the last line before the first guest instruction.
assert re.search(r"psx_start_refusal_reset\(\);\s*/\* Execute\. \*/\s*std::fprintf\(stdout, $", before_frame), (
    "psx_start_refusal_reset() must be the last statement before the game executes"
)

# The document names the keys, the kinds and the rule for file names.
for word in ("start_refused", "launcher_status", "launcher_closed", "setup_relaunch", "base name"):
    assert word in DOC, f"docs/RUN_REPORT_START.md must name {word}"
for _, kind in SITES + [("", "config_unreadable"), ("", "setup_program")]:
    assert f"`{kind}`" in DOC, f"docs/RUN_REPORT_START.md must list the kind {kind}"

print("start refusal wiring: ok")
