# Dispatch growth is generator-driven, not profile-driven (2026-09-22)

## Controlled test result

I first suspected the 4x dispatch growth came from the BIOS profile, because the
merged `bios/SCPH1001.toml` is 6,735 bytes versus the working one's 3,530 — it adds
`[program.image]` with a declared sha256, kernel-RAM range declarations, and details
refined from live boots. That suspicion was wrong.

Held the emitter constant and varied the profile:

| Emitter | BIOS profile | `dispatch_entries` | `SCPH1001_full.c` |
| --- | --- | --- | --- |
| **merged** | **merged** | **52,316** | 43,025,623 B |
| **merged** | **working** | **52,345** | 42,991,762 B |
| original | working | 13,070 | 26,757,658 B |

Swapping the profile on the merged emitter moved dispatch entries by **29** (52,316 →
52,345) and file size by **34 KB** out of 16 MB. **The emitter is the variable.**

The merged generator emits roughly **60% more BIOS code** than the one that produced
the currently linked output.

## Why the merged emitter produces more

The `code_generator.cpp` diff between the two parents is real and additive — it is
not a formatting change. New emission visible in the diff includes:

- `psx_ws_player_x_bound(...)` — SF2's typed native-wide signed X bound, emitted inline
- per-register GTE read helpers (`data_read_needs_helper` / `ctrl_read_needs_helper`
  routing to `gte_read_data` / `gte_read_ctrl` instead of raw array access)
- explicit `psx_syscall(cpu, N)` emission
- branch-condition formatting including a typed unknown-condition fallback

`code_generator.cpp` had **4 conflict hunks** in my merge, and `452cc0c`'s version
carries SF2 feature work (`diff f4751d00..452cc0c` = 71 insertions, 413 deletions on
that file). So the merged generator legitimately generates *more* instrumentation —
SF2's widescreen and GTE provenance hooks are emitted into the output.

## Corrected conclusion

Earlier I wrote this "indicates a different dispatch-generation strategy." More
precisely: the merged generator emits **substantially more per-instruction code**
(bounds hooks, GTE helper routing, syscalls), which enlarges both the function bodies
and the dispatch table. That is expected given SF2's feature branch was merged in.

It is therefore **not** evidence of a defect in the merge. It is evidence that the
black-screen build links output from a generator that predates all of it.

## The provenance conclusion stands

| | Emitter that produced the linked output | Emitter in the merge tree |
| --- | --- | --- |
| `psxrecomp-game.exe` | `f2798b9fac9d56d002493159891b6cd8c504dcbf75345109dc3c4935d295ebc0` | `72c5fc25475716f7bc2c2ca9ebc08560c8752e42a9e2dd3566719df22f68c5b9` |
| `psxrecomp-bios.exe` | `b39b485d88456dfe23e41e73a28b4c7cb1c70996a0fbc9f455e54a03c8f2e2de` | `08aff125e031b53138d5bb07ef5001e3d90cb2a7f070a2c876f00aca20f05a42` |

Different binaries. Generated output comparison (`SCUS_944.51_full*.c`):

| | Count |
| --- | --- |
| identical to working build | 0 |
| different | 98 |
| file counts | merged regen 99 vs working 113 |

So the black-screen build pairs a **new runtime with old generated code** — two
variables changed at once, and the pair was never validated together. That remains
the central process gap, independent of whether it causes the black screen.

## Additional observation: path anchoring differs

The merged `psxrecomp-bios.exe` resolves `rom` / `seeds` / `out_dir` **relative to the
config file's directory**, not the project root. A config at `<root>/bios/SCPH1001.toml`
with `rom = "bios/SCPH1001.BIN"` resolves to `<root>/bios/bios/SCPH1001.BIN` and fails
with `FATAL: cannot open BIOS file`. The original emitter resolved against the project
root (which is how `tools/regen_bios.sh` is documented to work). Recorded as an
observed difference; whether it is intended is unverified, but it means any recipe
assuming the documented invocation may silently target the wrong path.

## Staleness stamp

`bios/SCPH1001.toml` documents that the ROM is hashed into a
`generated/<stem>.emitter.sha` staleness stamp, and `runtime.cmake` holds a freshness
check. **No `.sha` file exists in either tree's `generated/`** after regeneration, so
the stamp was not written and could not be consulted. Recorded as observed.
