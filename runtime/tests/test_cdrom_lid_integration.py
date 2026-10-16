"""Guard the CD controller wiring for the timed tray cycle."""

from pathlib import Path
import re


SOURCE = (Path(__file__).parents[1] / "src" / "cdrom.c").read_text(
    encoding="utf-8"
)
MAIN = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", SOURCE)
    assert match, f"missing function: {name}"
    start = match.end()
    depth = 1
    cursor = start
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated function: {name}"
    return SOURCE[start : cursor - 1]


reinsert = function_body("debug_force_cd_reinsert")
assert "cdrom_lid_begin_open" in reinsert
assert "s_lid_irq_pending = 1" in reinsert
assert "set_irq(CDIRQ_ACK)" not in reinsert

present = function_body("present_lid_open_irq_if_ready")
assert "response_push(0x08)" in present
assert "set_irq(CDIRQ_ERROR)" in present

has_disc = function_body("has_disc")
assert "cdrom_lid_media_ready" in has_disc

advance = function_body("cdrom_advance")
assert advance.count("process_lid_state()") >= 2

route_open = function_body("cdrom_route_tray_open")
assert "psx_netplay_active()" in route_open
assert "s_lid.physical_open" in route_open
assert "cdrom_lid_begin_open" in route_open
assert "s_lid.close_due = UINT64_MAX" in route_open

route_select = function_body("cdrom_route_disc_select")
assert "!s_lid.physical_open" in route_select
assert route_select.index("replacement = iso_open(cue_path)") < route_select.index("iso_handle = replacement")
assert route_select.index("iso_handle = replacement") < route_select.index("iso_close(previous)")

route_close = function_body("cdrom_route_tray_close")
assert "!s_lid.physical_open" in route_close
assert "!iso_handle" in route_close
assert "s_lid.physical_open = 0" in route_close
assert "s_lid.close_due = 0" in route_close
assert "stat_reg |= CDSTAT_MOTOR | CDSTAT_SHELL" in route_close

assert MAIN.index("input_route_session_set_disc_roster(") < MAIN.index(
    "input_route_session_set_event_executor(input_route_apply_event)")
assert "cdrom_route_tray_open()" in MAIN
assert "cdrom_route_disc_select(disc_path" in MAIN
assert "cdrom_route_tray_close()" in MAIN
assert "savestate_note_disc_mounted(disc_index)" in MAIN

print("PASS: timed CD lid integration guards")
