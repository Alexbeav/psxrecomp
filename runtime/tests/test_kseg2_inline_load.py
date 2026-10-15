"""Word and half loads from KSEG2 do not read main RAM (PS1B-506).

The C fixture links the real readers (memory.c) and calls the inline loads of
psx_cyc.h with block cycles on. This driver builds it at -O0 and -O2.

It also runs the control of the structural guard in
test_interpreter_perf_guards.py: the guard passes on psx_cyc.h as it is, and
fails with its message when either inline load loses the KSEG2 exclusion.
"""
import argparse
import importlib.util
import tempfile
from pathlib import Path

import source_fixture_link
from source_fixture_link import build_and_run

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# memory.c compares bytes with memcmp, and Clang calls bcmp for a memcmp that
# is only tested for equality (see test_low_ram_game_entry.py).
source_fixture_link.ISO_C_FALLBACK |= {"memcmp", "bcmp"}

EXCLUSION = "addr < UINT32_C(0xC0000000) && physical < UINT32_C(0x800000)"


def guard_control():
    spec = importlib.util.spec_from_file_location(
        "interpreter_perf_guard", HERE / "test_interpreter_perf_guards.py")
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    header = (ROOT / "runtime/include/psx_cyc.h").read_text(encoding="utf-8")
    guard.inline_loads_exclude_kseg2(header)
    assert header.count(EXCLUSION) == 2, "control source moved"
    for fn in ("psx_cyc_load_word", "psx_cyc_load_half"):
        # Remove the exclusion from this function only.
        line = header.find(EXCLUSION, header.index(f"{fn}(CPUState *cpu"))
        broken = header[:line] + "physical < UINT32_C(0x800000)" + header[line + len(EXCLUSION):]
        try:
            guard.inline_loads_exclude_kseg2(broken)
        except AssertionError as exc:
            want = f"{fn} main-RAM fast path does not exclude KSEG2"
            assert str(exc) == want, f"control: {exc!s} (want: {want})"
        else:
            raise AssertionError(f"control: the guard accepts {fn} without the KSEG2 exclusion")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as root:
        for opt in ("-O0", "-O2"):
            build_and_run(args.cc, HERE, HERE.parent, opt, Path(root),
                          ["memory.c", "crc32.c"], "test_kseg2_inline_load.c",
                          defines=("PSX_ENABLE_BLOCK_CYCLES=1",))
    guard_control()
    print("PASS: KSEG2 word and half loads read 0 and are counted (O0/O2); "
          "the guard rejects a missing exclusion")
