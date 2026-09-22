# Widescreen flicker provenance candidate — 2026-08-04

## Final result

The packed-value provenance candidate described below was rejected. Human A/B
testing accepted its replacement at source checkpoint `a2b951c`: retain the
guest-visible wider GTE projection, then restore raster X only for the dense
polygon-owned linked-list DMA submission. The accepted Release executable is
`1A6B0DE5FDB4CCC7DE2D6AD99BAE89A878741B508114978D16465C12DDF1C529`.

This removes the edge culling without changing character proportions and does
not exhibit the rhythmic face/black-edge flicker at the previously held Mission
1 scenes. The classifier threshold is title-profile data; the runtime contains
no SF2 address, movie dimension, state write, or generated-code patch. The 4:3
control remains separately buildable and configurable.

The decisive negative finding is reusable: equality of packed projected
coordinates is not primitive ownership. Values are reused across frames and DMA
submissions, so a value-keyed restore cache crossed semantic owners and produced
more than 1.15 million false restores.

## Rejected intermediate result

The rhythmic native-wide flicker is narrowed to horizontal projection
composition, not the OpenGL wide-mirror surface, 24-bit coherency, or a single
stale display page. The previous implementation inversely transformed every
polygon in an eligible ordering-table rank. That ownership rule included
CPU-authored geometry and effects that did not originate in the GTE and could
therefore change individual triangle faces at the repeating retail SCRIM
cadence.

The new candidate fails closed per hardware triangle. It retains the guest GTE
projection that gives retail the wider visibility cone, records the exact
pre-squash 16.16 X beside each recent packed SXY value, and restores raster X
only when all three triangle vertices have matching GTE value provenance.
Partial matches and CPU-authored triangles remain on their original coordinate
path. Quads are handled as their two hardware triangles, so one half cannot
silently classify the other.

This is a generic runtime correction. It contains no SF2 addresses, movie
dimensions, forced state, generated-code edit, or presentation containment.

## Bounded evidence

- Switching live OpenGL presentation from the independent wide FBO to the
  canonical centre blit did not stop the flicker. Wide-FBO ownership is
  contradicted.
- Disabling the guest GTE squash while retaining the broad ordering-table
  inverse changed aspect/FOV and made the defect heavier, but did not stop it.
  A second approximate inverse is contradicted.
- Bad frames occur on both display pages. A single stale backbuffer is
  contradicted.
- The cadence correlates with SF2's 13-command SCRIM copy cycle, but bounded CPU
  VRAM copy co-simulation is exact. SCRIM is narrowed to a trigger/cadence, not
  the corrupting owner.
- Complete-triangle GTE value provenance passed model and semantic-route checks
  but was contradicted by human visual evidence. It is not an accepted ownership
  boundary.

Ignored live and route evidence is retained under:

- `lab/sf2/local/widescreen-flicker-live-20260803/`
- `lab/sf2/local/widescreen-flicker-live-20260804-fastcenter/`
- `lab/sf2/local/generated-disc1-r2-load-delay/evidence/provenance-hidden-b/`
- `lab/sf2/local/generated-disc1-r2-load-delay/evidence/provenance-hidden-c/`
- `lab/sf2/local/generated-disc1-r2-load-delay/evidence/provenance-native-b/`

## Regression and build validation

Focused title-neutral regressions prove:

1. three matching GTE vertices recover the exact pre-squash X while preserving
   already-applied draw offsets;
2. a partial match leaves the complete triangle unchanged;
3. negative 16.16 coordinates use the same floor rule as GTE SXY;
4. projection provenance is unavailable during speculative execution and is
   invalidated with the existing GTE timeline;
5. PS1 primitive-size rejection remains based on original hardware packet
   coordinates.

The projection-compose, GTE-register/provenance, and primitive-reject focused
tests pass. Release regeneration and link pass. Two clean hidden OpenGL runs
reach authentic TITLE, Mission 1 FMV, state 8, state-0 player ownership, and the
same final movement position `(-5606, 2036, 7529)`. Normalized checkpoint
fingerprints match at all five semantic checkpoints. Host-timed startup movie
and input-frame records differ, so the pair is not represented as fully
deterministic.

Projection restores are zero at TITLE, the aircraft FMV, and state 8. They begin
only after state-0 world rendering and reach 320,268/322,029 in the two clean
runs. Both runs have nonzero GPU, SPU, XA, and PAD activity and zero lost CD
INT1 events.

Candidate identities:

- executable SHA-256:
  `8F72B055BDDDA3963EDFC5EB229EF07EB83352EC42FD22590E4B0EFC21439151`
- generated config SHA-256:
  `831C74A2DDC8920E8AA21E0AF2F52279D5E29EB871D5D0025EFCC48C9161312F`

## Overlay ownership caveat

The clean candidate routes measured approximately 15.99 million resident-AOT
dispatches, zero compiled-overlay dispatches, and 13.54 million interpreter
fallbacks. This is not native overlay coverage.

The owning reason is proven: existing DLLs are namespaced to emitter hashes
`9713afe3` and `dae7adf8`, while the current recompiler is `d6bc536d`. The loader
correctly rejects them. Three bounded recovery checks were completed:

1. copying the prior cache preserved files but correctly loaded no stale DLL;
2. compiling the candidate's current bounded capture produced only its single
   `0x80143000/A6C8143B` image;
3. preflighting the broader seven-region manifest built 127 shards but failed
   four audits, including unsupported code in `0x8014B000/DAFAD468`.

Per the no-containment rule, the failed broad set was not published into the
candidate cache. Native-overlay ownership remains a separate open invariant;
it does not invalidate the interpreter-backed semantic smoke result or prove
the visual fix.

## Human gate

The packed-value provenance candidate documented above was rejected by human
testing: it matched more than 1.15 million vertices, produced severe rhythmic
corruption, and changed character proportions. The replacement preserves
ownership at the linked-list DMA boundary and compensates only the dense world
submission. The separate aspect-correct control remains available.

Run the replacement candidate with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_sf2_modernized_pass2.ps1 `
  -BuildName build-modern-pass2-submission
```

At an outdoor Mission 1 position, pan through the former culling boundary and
then hold the camera still for several seconds.
Accept only if character proportions match the 4:3 control, side visibility is
retained, and no rhythmic triangle/black-edge flicker occurs. The existing
`build-modern-pass2-restored` is the isolated full-mirror control with guest
projection disabled. Each build now has its own generated runtime config, so
selecting one can no longer silently change the other build's behavior.
