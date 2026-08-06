# Outbound upstream backlog

Direction note: every other file in this directory records **inbound** work —
other people's upstream PRs adapted into this fork. This file tracks the
opposite direction: generic, title-neutral fixes produced in the SF2 lab that
are candidates for submission to `mstan/psxrecomp`, per
`I:\Projects\PSX-References\COMMUNITY_CONTRIBUTION_POLICY.md` (standing rule:
prepare an upstream-ready contribution instead of leaving the result buried in
a private devlog).

Update this table whenever a generic fix lands or an upstream submission
changes state. "Ready" means: separated from SF2-specific code, focused
regression exists, title-neutrality verified — only the extraction/PR step
remains. Submission itself requires explicit user authorization.

| # | Fix | Fork commit(s) | Regression | Status |
|---|---|---|---|---|
| 1 | CFG dependent-load-delay writeback deferred through immediate successor | `5834990` (extracted), worktree `lab/sf2/local/upstream-pr-cfg-load-delay`, branch `fix/cfg-dependent-load-delay` | recompiler CFG regression (exact instruction pair) | **Submitted — PR #93**, checked 2026-08-06: open, unmerged, no review decision yet. |
| 2 | Non-`$ra` JALR preserves architectural pc-chain (data-bearing descriptor trampolines) | `61d3667` | framework suite 39/39 with source guard | Ready — never submitted |
| 3 | Nested call units deliver IRQs (save-deadlock: event pump waits on IRQ its own call suppressed) | `dc873fc` | focused source regression + suite 40/40 | Ready — never submitted |
| 4 | CD SeekL/SeekP cancels ReadN/ReadS and pending data-ready ownership | `485b79b` | title-neutral regression | Ready — never submitted |
| 5 | OpenGL split-VRAM ownership at 15↔24-bit transitions (FBO→CPU finish before packed movie uploads, depth-24 mirror fills) | `09be64b` | title-neutral regression; live evidence 292→0 mismatch samples | Ready — never submitted |
| 6 | Per-function resident-patch classifier: exact-range comparison vs resident text, `unpromoted` sidecar, loader scan+load-boundary recheck | `17e9bba` | `overlay_resident_patch_promotion` + 4 sibling CTests | Ready — never submitted |
| 7 | Additive overlay-capture history union (manifest + directory + legacy single file) with non-destructive legacy migration | `17e9bba` | `overlay_capture_legacy_history`, additive unit tests | Ready — never submitted |
| 8 | Bounded transactional crash/atexit JSON serializers + always-on native overlay ring in ordinary reports | `17e9bba` | `crash_overlay_ring_guards`, `overlay_dump_bounds` | Ready — never submitted |
| 9 | Renderer-neutral PGXP pass 1 (SWC2 provenance, atomic per-primitive fallback, canonical-VRAM preservation) | `2eebc41` | focused canonical-VRAM regression + suite 50/50 | Candidate — consider after independent validation by a second title (per `PSX_PORT_KNOWLEDGE_REPORT.md`) |

## High-refresh R3 disposition

No R3 source should be submitted upstream. Frozen transform pairs closed a
real snapshot-lifetime race inside the experiment, but the complete retained
GP0 replay architecture was visually rejected after its automated gates passed
and has been removed from active source. The portable contribution is the
negative validation result returned to the shared corpus as `PSX-TIME-005`,
`PSX-TIME-006`, and `FAIL-030`.

The next source submission worth preparing is row 8: bounded transactional
crash/atexit serializers plus the behavioural 512-record bounds regression.
It is title-neutral, fixes the reachable `PSX-DIAG-001` crash-path OOB, and is
already separated in `17e9bba`. Opening a PR still requires explicit user
authorization under the community contribution policy.

## Known upstream-relevant test debt

`tools/test_compile_overlays_additive.py` contains 8 tests that reference APIs
removed by the interpreter-hotpath/AOT-sharding integration
(`cached_bundle_pairs`, the old `load_region_coverage` signature). They error
when the module runs as a whole; the module is not registered in CTest, so the
50/50 suite never exercises it. Reconcile or retire those tests before using
the module as an upstream-facing regression suite. Recorded 2026-08-06.
