"""Pin J loads pin I save states: boot_state.h keeps pin I 0.1.24's section tags.

PR #923 used 0x17, 0x18, 0x1A and 0x1F for other layouts. Pin J keeps pin I's
sections on those tags (PS1B-685). No pin I state file is checked in, so this
pins the tag table that boot_state.c dispatches on; the section code itself is
pin I's.
"""
import pathlib
import re
import sys

PIN_I_TAGS = {
    "CPU": 0x01, "RAM": 0x02, "SPAD": 0x03, "IRQ": 0x04, "TIMER": 0x05,
    "CLOCK": 0x06, "GPU": 0x07, "VRAM": 0x08, "SPU": 0x09, "SPURAM": 0x0A,
    "CDROM": 0x0B, "DMA": 0x0C, "SIO": 0x0D, "DIRTY": 0x0E, "MDEC": 0x0F,
    "ICACHE": 0x10, "MODMEM": 0x11, "GAME_START": 0x12, "GCS": 0x13,
    "CPUTIM": 0x14, "SPUCD": 0x15, "MOD_HOST": 0x16, "CPU_EXEC": 0x17,
    "SCHED": 0x18, "BOOTFLOW": 0x19, "MEMCTRL": 0x1A, "RASTER": 0x1B,
    "GPU_SERVICE": 0x1C, "TIMER_SRC": 0x1D, "DMA_SRC": 0x1E,
    "IRQ_TIMING": 0x1F,
}

def main() -> int:
    header = pathlib.Path(__file__).resolve().parents[1] / "include" / "boot_state.h"
    text = header.read_text(encoding="utf-8")
    found = {m.group(1): int(m.group(2), 16)
             for m in re.finditer(r"\bBS_SEC_([A-Z_]+)\s*=\s*(0x[0-9A-Fa-f]+)", text)}
    errors = []
    if found != PIN_I_TAGS:
        for name in sorted(set(found) | set(PIN_I_TAGS)):
            if found.get(name) != PIN_I_TAGS.get(name):
                errors.append(f"BS_SEC_{name}: header {found.get(name)} pin I {PIN_I_TAGS.get(name)}")
    if len(set(found.values())) != len(found):
        errors.append("a section tag is assigned twice")
    if not re.search(r"#define\s+BOOT_STATE_MAX_SECTION\s+0x1Fu", text):
        errors.append("BOOT_STATE_MAX_SECTION is not 0x1Fu")
    for e in errors:
        print("FAIL:", e)
    print("boot_state pin I tags:", "FAIL" if errors else "PASS", len(found), "tags")
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
