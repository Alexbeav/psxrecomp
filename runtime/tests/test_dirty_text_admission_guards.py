#!/usr/bin/env python3
"""Keep CPU text writes separate from explicit overlay admission.

An ordinary CPU write inside game text records divergence (text_modified) and
nothing else: it does not mark the page dirty and does not mark it executable,
so it never puts bytes into the capture window. Loading a unit that the capture
already built, on an exact match of the live bytes, is the loader's own gate
(overlay_loader.c, lazy_load_window_contains; PS1B-421) and is tested by
test_overlay_resident_patch_runtime.py. It needs no dirty bit and sets none.
"""

from pathlib import Path
import argparse
import sys


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    root = parser.parse_args().root.resolve()
    memory = (root / "runtime/src/memory.c").read_text(encoding="utf-8")
    dma = (root / "runtime/src/dma.c").read_text(encoding="utf-8")

    text_write = function_body(memory, "static inline void text_guard_note_write(")
    if "text_modified_bitmap" not in text_write:
        raise AssertionError("CPU text divergence is no longer recorded")
    for admission in ("dirty_ram_mark_page(", "dirty_ram_mark_executable_range("):
        if admission in text_write:
            raise AssertionError(
                "an ordinary CPU write inside game text puts the page into the capture "
                f"window (dirty or executable) via {admission}"
            )

    cd_slice = function_body(dma, "void dma_advance(")
    read = cd_slice.index("uint32_t word = cdrom_dma_read();")
    store = cd_slice.index("psx_write_word(addr, word);", read)
    admit = cd_slice.index("dirty_ram_mark_executable_range(addr, 4);", store)
    advance = cd_slice.index("addr = (addr + addr_step)", admit)
    if not read < store < admit < advance:
        raise AssertionError("CD DMA overlay admission is not coupled to each RAM word")

    mark_range = function_body(memory, "void dirty_ram_mark_executable_range(")
    for fragment in (
        "dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));",
        "g_dirty_ram_code_gen++;",
    ):
        if fragment not in mark_range:
            raise AssertionError(f"explicit executable-range admission lost: {fragment}")

    loader = (root / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    gate = function_body(loader, "static int lazy_load_window_contains(")
    for admission in ("dirty_ram_mark_page(", "dirty_ram_mark_executable_range("):
        if admission in gate:
            raise AssertionError(f"the loader's lazy-load window sets a page bit via {admission}")
    if "lazy_has_exact_entry(phys)" not in gate or "modtext_backed_off(phys)" not in gate:
        raise AssertionError("the lazy-load window for rewritten text lost its exact-entry or back-off term")

    print("dirty-text admission guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
