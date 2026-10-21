#!/usr/bin/env python3
"""PS1B-530: memory.c keeps the range-compare memo of the text guard honest.

dirty_ram_text_native_ok_ranges_from skips its exact compare of a flagged range
when the memo says no page of the range was written since the last equal
compare. That is safe only while every change to the compared bytes reaches the
memo: a store stamps its page, a change of the reference image stamps or drops
the memo, and a bulk RAM write drops every memo. This test pins each of those
sites in memory.c to its hook, and the guard to its memo checks. The raw RAM
writers in the other runtime files are checked by test_raw_ram_writers.py.

Usage: test_text_memo_hooks.py [path/to/memory.c]
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


def check(src: str) -> list:
    failures = []
    hooks = {
        "static inline void text_guard_note_write(": "text_memo_note_range(phys, (uint32_t)size);",
        "void dirty_ram_text_bless(": "text_memo_note_range(lo, hi - lo);",
        "void dirty_ram_mark_executable_range(": "text_memo_note_range(phys, len);",
        "void dirty_ram_register_text_image(": "text_memo_invalidate_all();",
        "void dirty_ram_text_guard_resync_after_restore(": "text_memo_invalidate_all();",
        "void dirty_ram_reset_for_boot(": "text_memo_invalidate_all();",
        "void dirty_ram_set_bitmap_words(": "text_memo_invalidate_all();",
        "void memory_init(": "text_memo_invalidate_all();",
        "void dirty_ram_text_memo_note_write(": "text_memo_note_range(phys, len);",
        "void dirty_ram_text_memo_invalidate(": "text_memo_invalidate_all();",
    }
    for signature, hook in hooks.items():
        if signature not in src:
            failures.append(f"missing function {signature}")
        elif hook not in body(src, signature):
            failures.append(f"{signature} lost its memo hook {hook}")

    # Every store into RAM goes through text_guard_note_write.
    for size in ("4", "2", "1"):
        if not re.search(r"text_guard_note_write\(phys, [^;]*, " + size + r"\);", src):
            failures.append(f"{size}-byte store no longer calls text_guard_note_write")

    # No bulk RAM write without an invalidate in the same function.
    for m in re.finditer(r"\b(memset|memcpy|memmove)\(\s*ram\b", src):
        fn_start = src.rfind("\n}", 0, m.start())
        fn_end = src.find("\n}", m.end())
        if "text_memo_invalidate_all();" not in src[fn_start:fn_end]:
            line = src.count("\n", 0, m.start()) + 1
            failures.append(f"bulk RAM write at line {line} has no memo invalidate")

    if "int dirty_ram_text_native_ok_ranges_from(" not in src:
        failures.append("missing function dirty_ram_text_native_ok_ranges_from")
        return failures
    guard = body(src, "int dirty_ram_text_native_ok_ranges_from(")
    for fragment in ("slot->phys == phys && slot->len == len && slot->stamp >= text_memo_floor",
                     "text_page_stamp[p] > slot->stamp",
                     "slot->stamp = text_write_clock;",
                     "slot->len = 0;",
                     "memcmp(ram + phys, text_ref_image + (phys - text_ref_lo), len)"):
        if fragment not in guard:
            failures.append(f"range guard lost: {fragment}")
    # The memo is consulted before the page-clean scan: a range that reaches the
    # guard is almost always on a flagged page, where the scan is pure cost.
    memo_at = guard.find("slot->phys == phys && slot->len == len")
    scan_at = guard.find("dirty_ram_is_dirty(")
    if memo_at < 0 or scan_at < 0 or memo_at > scan_at:
        failures.append("range guard runs the page-clean scan before the memo lookup")
    elif "if (!memo_ok)" not in guard[memo_at:scan_at]:
        failures.append("range guard runs the page-clean scan after a memo hit")
    return failures


def main() -> int:
    if len(sys.argv) > 1:
        path = Path(sys.argv[1])
    else:
        path = Path(__file__).resolve().parents[1] / "src" / "memory.c"
    failures = check(path.read_text(encoding="utf-8", errors="replace"))
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: text memo hooks in memory.c")
    return 0


if __name__ == "__main__":
    sys.exit(main())
