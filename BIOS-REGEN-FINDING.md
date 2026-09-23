# BIOS regeneration with merged emitters — 4x dispatch growth (2026-09-22)

## Result

Regenerated the BIOS using the **merged** `psxrecomp-bios.exe` in the merge tree.
`BIOS=0`, `EMIT OK emitted=1399 interpreted=1 skipped=1 instructions=63186
dispatch_entries=52316`.

## Output differs enormously from the working build

| File | merged emitter | working build | ratio |
| --- | --- | --- | --- |
| `SCPH1001_full.c` | **43,025,623 B** | 26,757,658 B | **1.61x** |
| `SCPH1001_dispatch.c` | **7,384,719 B** | 1,660,756 B | **4.45x** |

Hashes:

```
merged  full.c     : 2cebf13b9b56b52688eff46de0db513339fbfe719b69697a0d09447d48c371c8
working full.c     : ab81855c2d43e5ddf3b425b2327707326f9ae1249eef5d6394b5564e4b4bee4c
merged  dispatch.c : fa65e7dcd11b53c9d05238f2602f4fb768e94a97bd68187f1ccad9baf90bc468
working dispatch.c : 7599e3c806bb569bd3feef130da8f526f4e1f7864157401147c54ed4b69fd1d4
```

## The cause is structural, not just more coverage

Emission statistics, same input ROM, same profile:

| Metric | merged emitter | working build | change |
| --- | --- | --- | --- |
| functions emitted | 1399 | 1310 | +7% |
| instructions | 63,186 | 61,869 | **+2%** |
| **`dispatch_entries`** | **52,316** | **13,070** | **+300% (4.0x)** |

Instructions grew **2%** while dispatch entries grew **400%**. That ratio cannot be
explained by broader coverage — 4x more dispatch entries for 2% more instructions
indicates a **different dispatch-generation strategy** in the merged
`recompiler/src/code_generator.cpp`.

This is consistent with the merge: `code_generator.cpp` had **4 conflict hunks**,
and the earlier review noted both branches fixed real defects there (MIPS-I
load-delay handling, hook emission). The merge did not merely combine them
neutrally — the emitted structure changed.

## Why this matters for the black screen

It is now a concrete, single-variable candidate, and it is *outside* the GPU merge
I was originally about to blame:

- The black-screen build links **old generated code** (13,070-dispatch-era BIOS,
  113-file game) against the **merged runtime**.
- The merged generators produce **structurally different** BIOS and game output
  (52,316 dispatch entries; 99 game files vs 113).
- Generated code and runtime are therefore a **mismatched pair** in the
  black-screen build — a variable never controlled.

A 4x dispatch table touching BIOS kernel entry is exactly the class of change that
could leave the guest running but never reaching the display path, which matches
what was observed (guest executing from `0xBFC00000`, frames advancing, VRAM empty
at the display area).

**Not yet proven** that this causes the black screen. It is the leading hypothesis,
and it is now testable: link merged-generator output with the merged runtime.

## Note on the staleness stamp

`bios/SCPH1001.toml` documents an `.emitter.sha` staleness stamp written into
`generated/`, and `runtime.cmake` has a freshness check. No `.sha` file was found in
either the merge tree's `generated/` or the working kit's
`psxrecomp/generated/`, so the stamp was not present to consult. Recorded as
observed; the check's behaviour with a missing stamp was not investigated.

## Correction recorded

An earlier single-file check concluded the mouse-camera hook was **absent** from
regenerated game output. That was wrong: the check looked only at
`SCUS_944.51_full_20.c`, but the merged generator partitions differently and places
the hook in `SCUS_944.51_full_24.c`. Across the whole set the hook is present exactly
once in both builds. Withdrawn.
