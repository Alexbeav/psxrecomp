#!/usr/bin/env python3
"""PS1B-530: the device scheduler's deadline queries stay cheap and single-sourced.

psx_cycles.c asks every device for its next event once per catch-up chunk and
once per new service deadline, about 360 million query sets on the Tekken 3
TAS route. This test pins the three changes that made those queries cheaper
without changing any answer:

- The SPU-sample and source-GPU queries have one body each, an inline function
  in device_deadline_queries.h. The scheduler calls the inline bodies; the
  exported functions return them, so every caller gets the same value.
- DMA keeps a mask of active delayed-complete channels. Every write of
  delayed_complete[].active updates it, the internal-event query walks the
  mask, and the DMA advance skips its channel loop when the mask is empty.

Usage: test_device_deadline_queries.py <runtime dir>
"""

from pathlib import Path
import re
import sys


def body(src: str, signature: str) -> str:
    start = src.index(signature)
    open_brace = src.index("{", start)
    depth = 0
    for i in range(open_brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_brace:i + 1]
    raise AssertionError(f"unterminated body: {signature}")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def check(runtime: Path) -> list:
    failures = []
    header = read(runtime / "include" / "device_deadline_queries.h")
    for fn in ("psx_spu_sample_event_cycles_to_next_inline", "source_gpu_runtime_cycles_to_event_inline"):
        if f"static inline uint32_t {fn}(void)" not in header:
            failures.append(f"device_deadline_queries.h does not define {fn}")

    cycles = read(runtime / "src" / "psx_cycles.c")
    # Production builds map the scheduler's two macros to the inline bodies;
    # only unit tests that link stub devices select the exported functions.
    production = re.search(r"#if defined\(PSX_DEVICE_QUERIES_OUT_OF_LINE\)\n.*?#else\n(.*?)#endif\n", cycles, re.S)
    if not production:
        failures.append("psx_cycles.c has no PSX_DEVICE_QUERIES_OUT_OF_LINE switch")
    else:
        for macro, call in (("SCHED_GPU_EVENT()", "source_gpu_runtime_cycles_to_event_inline()"),
                            ("SCHED_SPU_SAMPLE_EVENT()", "psx_spu_sample_event_cycles_to_next_inline()")):
            if f"#define {macro} {call}" not in production.group(1):
                failures.append(f"production {macro} is not {call}")
    sched = body(cycles, "static uint32_t devices_cycles_to_next_internal_event(void)") if "devices_cycles_to_next_internal_event" in cycles else ""
    for macro in ("SCHED_GPU_EVENT()", "SCHED_SPU_SAMPLE_EVENT()"):
        if macro not in sched:
            failures.append(f"scheduler does not use {macro}")
    for call in ("source_gpu_runtime_cycles_to_event()", "psx_spu_sample_event_cycles_to_next()"):
        if call in sched:
            failures.append(f"scheduler calls {call} directly")

    irq = read(runtime / "src" / "interrupts.c")
    spu = body(irq, "uint32_t psx_spu_sample_event_cycles_to_next(void)")
    if spu.split() != ["{", "return", "psx_spu_sample_event_cycles_to_next_inline();", "}"]:
        failures.append("psx_spu_sample_event_cycles_to_next has a body of its own")
    gpu_src = read(runtime / "src" / "source_gpu_runtime.c")
    gpu = body(gpu_src, "uint32_t source_gpu_runtime_cycles_to_event(void)")
    if gpu.split() != ["{", "return", "source_gpu_runtime_cycles_to_event_inline();", "}"]:
        failures.append("source_gpu_runtime_cycles_to_event has a body of its own")

    dma = read(runtime / "src" / "dma.c")
    if "static uint32_t delayed_complete_mask;" not in dma:
        failures.append("dma.c has no delayed_complete_mask")
    # Every write of .active is followed on the next line by a mask update.
    lines = dma.splitlines()
    writes = [i for i, l in enumerate(lines)
              if re.search(r"(delayed_complete\[\w+\]\.active|\bd->active) = [01];", l)]
    if len(writes) != 3:
        failures.append(f"expected 3 writes of delayed_complete .active, found {len(writes)}")
    for i in writes:
        if "delayed_complete_mask" not in lines[i + 1]:
            failures.append(f"dma.c line {i + 1}: .active write without a mask update")
    init = body(dma, "void dma_init(void)")
    if not re.search(r"memset\(delayed_complete, 0, sizeof\(delayed_complete\)\);\s*delayed_complete_mask = 0;", init):
        failures.append("dma_init clears delayed_complete without clearing the mask")
    if not re.search(r"dma_r_delay\(&r, &delayed_complete\[i\]\);\s*delayed_complete_mask &= ~\(1u << i\);\s*"
                     r"if \(delayed_complete\[i\]\.active\) delayed_complete_mask \|= 1u << i;", dma):
        failures.append("restore reads delayed_complete without rebuilding the mask")
    query = body(dma, "uint32_t dma_cycles_to_internal_event(void)")
    if "delayed_complete_mask" not in query or "delayed_complete[ch].active" in query:
        failures.append("dma_cycles_to_internal_event does not walk the mask")
    adv = body(dma, "void dma_advance(uint32_t cycles)")
    if not re.search(r"if \(delayed_complete_mask\)\s*for \(int ch = 0; ch < 7; ch\+\+\)\s*advance_delayed_complete", adv):
        failures.append("dma_advance does not skip the delayed-complete loop on an empty mask")
    return failures


def main() -> int:
    runtime = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    failures = check(runtime)
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: device deadline queries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
