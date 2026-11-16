#!/usr/bin/env python3
"""PS1B-323: the overlay loader's dispatch counts are split by region.

The loader compiles and dispatches the kernel window (phys < 0x10000) as well
as game code, so its disp_native / disp_interp totals mix BIOS kernel routines
with the game's overlays. A gate that asks "did game overlay code run native"
must read the overlay-region share. This checks three things:

1. every count in overlay_loader_dispatch goes through the region-aware
   macros (a raw bump would miss the kernel count);
2. the macros count a kernel-window target in both the total and the kernel
   count, an overlay-region target in the total only, and undo symmetrically
   (compiled from the real definitions and run);
3. the run report prints the kernel pair and the overlay pair, the overlay
   pair being the total minus the kernel share.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOADER = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
REPORT = (ROOT / "runtime/src/crash_trace.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/include/dirty_ram_interp.h").read_text(encoding="utf-8")

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default="gcc")
args = parser.parse_args()

# 1. No raw bumps inside the dispatch function.
start = LOADER.index("int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) {")
end = LOADER.index("#undef DISP_KERNEL", start)
dispatch = LOADER[start:end]
for raw in ("s_disp_native++", "s_disp_native--", "s_disp_interp++", "s_disp_miss["):
    assert raw not in dispatch, f"overlay_loader_dispatch bumps {raw} outside the region macros"
uses = {name: len(re.findall(rf"\b{name}\(\);", dispatch))
        for name in ("DISP_NATIVE", "DISP_NATIVE_UNDO")}
# PS1B-391: a miss exit counts through DISP_MISS(reason), once, under its
# reason. disp_interp and its kernel part are sums of the reasons.
uses["DISP_INTERP"] = len(re.findall(r"\bDISP_MISS\(", dispatch))
assert uses["DISP_NATIVE"] >= 1 and uses["DISP_INTERP"] >= 1, uses
assert uses["DISP_NATIVE_UNDO"] <= uses["DISP_NATIVE"], uses

# 2. Compile the real macro definitions and exercise them.
macros = re.findall(r"^#define DISP_(?:KERNEL|NATIVE|NATIVE_UNDO)\(\).*$"
                    r"|^#define DISP_MISS\(reason\).*$", LOADER, re.M)
assert len(macros) == 4, macros
summer = re.search(r"^static uint64_t disp_miss_sum\(int side\) \{\n(?:.*\n)*?\}\n", LOADER, re.M)
assert summer, "disp_miss_sum not found"
window = re.search(r"^#define DIRTY_RAM_KERNEL_WINDOW_END\s+\S+", HEADER, re.M)
assert window, "DIRTY_RAM_KERNEL_WINDOW_END not found"
program = "\n".join([
    "#include <stdint.h>",
    "#include <stdio.h>",
    window.group(0),
    "#define PSX_INTERP_MISS_REASONS 4",
    "static uint64_t s_disp_native, s_disp_native_kernel;",
    "static uint64_t s_disp_miss[2][PSX_INTERP_MISS_REASONS];",
    summer.group(0),
    *macros,
    "static void run(uint32_t phys) {",
    "    DISP_NATIVE(); DISP_NATIVE(); DISP_NATIVE_UNDO(); DISP_MISS(2);",
    "}",
    "int main(void) {",
    "    run(0x00000650u);                      /* kernel window */",
    "    run(DIRTY_RAM_KERNEL_WINDOW_END - 4u); /* its last word */",
    "    run(DIRTY_RAM_KERNEL_WINDOW_END);      /* first game word */",
    "    run(0x00165000u);                      /* overlay region */",
    '    printf("%llu %llu %llu %llu %llu %llu %llu\\n", (unsigned long long)s_disp_native,',
    "           (unsigned long long)(disp_miss_sum(0) + disp_miss_sum(1)), (unsigned long long)s_disp_native_kernel,",
    "           (unsigned long long)disp_miss_sum(1), (unsigned long long)s_disp_miss[0][2],",
    "           (unsigned long long)s_disp_miss[1][2], (unsigned long long)s_disp_miss[0][1]);",
    "    return 0;",
    "}",
    "",
])
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "region_counters.c"
    exe = Path(tmp) / ("region_counters.exe" if sys.platform == "win32" else "region_counters")
    src.write_text(program, encoding="utf-8")
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", check=True).stdout.split()
# Each run() nets one native and one interp; two of the four targets are kernel.
# The miss counts under its reason on the side of its target, and nowhere else;
# the two sums give the totals the loader kept as counters before (4 and 2).
assert out == ["4", "4", "2", "2", "2", "2", "0"], out

# 3. The run report carries both pairs; overlay = total - kernel.
for field in ("disp_native_kernel", "disp_interp_kernel", "disp_native_overlay", "disp_interp_overlay"):
    assert f'\\"{field}\\"' in REPORT, f"run report lacks {field}"
assert "(disp_native - kernel_native)" in REPORT and "(disp_interp - kernel_interp)" in REPORT
assert "overlay_loader_get_kernel_window_dispatch(&kernel_native, &kernel_interp)" in REPORT

print(f"PASS: region counters ({uses['DISP_NATIVE']} native, {uses['DISP_INTERP']} interp sites; "
      "kernel and overlay pairs reported)")
