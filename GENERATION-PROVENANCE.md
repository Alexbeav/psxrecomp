# Generation provenance — the gap, and what regeneration shows

**Date:** 2026-09-22

## The gap (confirmed)

The integration build links generated code that **the merged generators never
produced**. Verified:

| Check | Result |
| --- | --- |
| Generated game C set, working vs integration | **byte-identical** — set hash `2686eeb05eee3be7fac1ba19a7fd999ce1a6fe49e0546bd09e21c13428541e7b`, 112 files each |
| BIOS `SCPH1001_full.c` | **identical** — `ab81855c2d43e5ddf3b425b2327707326f9ae1249eef5d6394b5564e4b4bee4c` |
| BIOS `SCPH1001_dispatch.c` | **identical** — `7599e3c806bb569bd3feef130da8f526f4e1f7864157401147c54ed4b69fd1d4` |
| `prepare-manifest.txt` in integration kit | **absent** |
| `psxrecomp/receipts/` in integration kit | **absent** |
| merged `recompiler/build/*.exe` | **never built** — directory empty before this session |

**How this happened, stated plainly:** I assembled the integration kit by copying
staged artifacts from the working kit and swapping in the merged framework. I never
ran the prepare script against the merged tree, so the merged generators were never
executed and no provenance was recorded. This is my error.

## What the canonical guide already said

`PSX Coordination / Corpus Library → Building psxrecomp game runtimes on Windows`
(source `PSX-References/docs/setup/BUILDING.md` @ `7458037c`) states:

> "Regenerating BIOS C after emitter changes: `tools/regen_bios.sh --config
> bios/<stem>.toml` (see the staleness guard in `runtime.cmake`)."

The integration changed the emitters and did **not** regenerate. The requirement was
documented; it was not enforced.

The guide also names a different build target than the refs I merged:

> "**Which fork branch:** build every title against `integrate/upstream-104` in
> `<projects>\psxrecomp-fork`"

and notes the recompiler tools build with **MSVC** (`recompiler/build` is an MSVC
build). My merged emitter build used MinGW GCC 16.1.0, which the guide reserves for
game runtimes (because generated C needs GCC computed gotos). That discrepancy is
recorded, not resolved.

## Regeneration result

Built the merged emitters from `<projects>\_local\psxrecomp-merge\recompiler`
(configure `CFG=0`, build `BUILD=0`), then generated the game in an isolated
workspace `<projects>\_local\gencheck` (game.toml + input + seeds + framework
staged; no existing build touched).

Merged emitter identity:

| Tool | SHA-256 |
| --- | --- |
| `psxrecomp-game.exe` | `72c5fc25475716f7bc2c2ca9ebc08560c8752e42a9e2dd3566719df22f68c5b9` |
| `psxrecomp-bios.exe` | `08aff125e031b53138d5bb07ef5001e3d90cb2a7f070a2c876f00aca20f05a42` |

Emitters that produced the generated code currently linked:

| Tool | SHA-256 |
| --- | --- |
| `psxrecomp-game.exe` | `f2798b9fac9d56d002493159891b6cd8c504dcbf75345109dc3c4935d295ebc0` |
| `psxrecomp-bios.exe` | `b39b485d88456dfe23e41e73a28b4c7cb1c70996a0fbc9f455e54a03c8f2e2de` |

**Different binaries.** So the linked code and the linked framework share no
generation provenance.

### Output comparison (`SCUS_944.51_full*.c`)

| | Count |
| --- | --- |
| identical to working build | **0** |
| **different** | **98** |
| file counts | regen **99** vs working **113** |

Concrete example — `SCUS_944.51_full_20.c`:

- merged regen: **1,273,262 bytes**
- working: **1,132,004 bytes**

The merged generator partitions the game into fewer, larger files and emits
different content throughout.

### The mouse-camera hook survives

Checked across the whole regenerated set:

| Build | Hook occurrences |
| --- | --- |
| merged regen | 1 — `SCUS_944.51_full_24.c:15259` |
| working | 1 — `SCUS_944.51_full_20.c:12814` |

So the headline feature is **not** lost by regeneration; the file partitioning
changed, which is why an earlier single-file check of `_20.c` looked like an
absence. Recorded as a correction to that check.

## What this does and does not establish

**Establishes:**
- The integration build's generated code has **no provenance** from the merged
  generators. The black screen cannot be attributed to the GPU merge on the current
  evidence, because two independent variables were varied at once: new runtime **and**
  unchanged-but-mismatched generated code.
- The merged generators **run** and produce different output (99 vs 113 files).
- Regeneration **retains** the mouse-camera hook.

**Does not establish:**
- That the merged generated code is *better* — no build has linked it yet.
- That the generated/runtime mismatch causes the black screen. It is now the leading
  hypothesis alongside the 768-pixel display-source offset found separately.
- Whether the merged generated output is *correct*, only that it differs.

## Next step

Build one isolated candidate linking **merged generators → merged generated output
→ merged runtime**, with a recorded manifest, and compare against both the working
build and the black-screen control. That is the single-variable experiment the
current integration never performed.
