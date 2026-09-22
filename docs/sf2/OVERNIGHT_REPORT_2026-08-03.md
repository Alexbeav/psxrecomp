# SF2 recomp overnight validation report

Date: 2026-08-03  
Branch: `experiment/sf2-recomp-feasibility`  
Start checkpoint: `4f811c2`

## Outcome

The minimum and stretch deliverables passed. The lab now has a generic OpenGL
24-bit coherency correction, a complete authentic no-input startup, and two
clean deterministic native-enabled routes through retail-selected Mission 1 to
verified player control and movement. No retail state was forced, no generated
game C or capture was edited, and interpreter fallback remains measured as a
separate execution tier.

Source milestones:

- `09be64b` — generic OpenGL 24-bit VRAM ownership handoff;
- `485b79b` — cancel active CD reads on SeekL/SeekP;
- `89804a7` — exact guest-frame input scheduling, route driver, and comparator.

## Generic fixes

### OpenGL 15-bit-to-24-bit ownership

The first OpenGL/software divergence was not movie decode or presentation
geometry. OpenGL fills and draws made the FBO authoritative, while depth-24
presentation consumed packed RGB888 from the CPU VRAM mirror. Retail then
uploaded only the central movie rows, exposing stale CPU data above and below.

The correction is backend-generic:

1. GP1 display-depth changes notify the active renderer.
2. On 15-to-24 transition, OpenGL completes FBO-owned work into CPU VRAM before
   the first packed movie upload.
3. Fills issued in depth 24 update both the FBO and CPU representation.
4. Upload ownership is established before the CPU write, preventing a late
   readback from overwriting movie data.

The regression `gl_depth24_coherency` models fill/copy/upload ownership without
movie sizes, bands, addresses, or title data. Hidden OpenGL evidence changed
from 292 CPU/FBO mismatch samples in the affected bounding region to zero.
Software remained the correct control. The user subsequently confirmed that
the corrupted FMV background was fixed.

### CD seek retarget lifecycle

After input was removed entirely, the startup still played 989, legal, and
TITLE while omitting Eidetic and ZINTRO. Three bounded falsifications rejected
the leading alternatives:

1. neutral PAD plus no override rejected early-input propagation;
2. native/interpreter parity rejected overlay code generation;
3. exact command and sector history showed retail requested the missing movie
   target while the device continued delivering the old stream.

The invariant was in the CD device: SeekL/SeekP began seeking but left an active
ReadN/ReadS stream and READ status alive. The generic fix calls
`stop_read_stream()`, clears READ/PLAY ownership, and only then enters SEEK.
`cdrom_seek_retarget` verifies that the old stream and pending data-ready event
cannot survive a seek retarget.

The corrected no-input startup is exact across both final runs:

| Retail identity | First payload frame | First payload LBA |
|---|---:|---:|
| 989 logo | 925 | 205322 |
| Eidetic | 1268 | 261286 |
| legal | 1422 | 261622 |
| ZINTRO | 1752 | 269590 |
| TITLE | 18493 | 263062 |

The route injects nothing until all five identities are latched and the retail
application stack, TITLE word, 320x240x15 display, and neutral PAD remain stable
for 60 guest frames.

## Deterministic Mission 1 route

Host-side socket timing initially caused later input to land on different guest
frames. The diagnostic `press` command now accepts an optional exact guest
frame and reports first/last applied frame, value, and application count from
the emulation-thread consumption point. `debug_input_schedule` covers the
title-neutral contract.

The final pair used identical input intervals:

| Action | First frame | Last frame | Samples |
|---|---:|---:|---:|
| Cross — New Game | 19200 | 19219 | 20 |
| Cross — One Player | 19320 | 19339 | 20 |
| Cross — leave state 8 | 24000 | 24019 | 20 |
| D-pad Up — movement | 25800 | 25859 | 60 |

Both clean processes passed:

`complete startup -> stable TITLE -> New Game -> One Player -> aircraft FMV -> state 8 -> post-FMV dialogue -> state 0 -> player ownership -> movement`

Player proof is retail-authoritative: the live player instance owns the active
camera, has positive health, and its matrix XYZ changes under a recorded PAD
interval. Both runs end at the identical instance/camera address, health 150,
armor 600, and XYZ `(-5606, 2036, 7529)`.

## Determinism and ownership

The comparator normalizes only documented observation variants: host request
and release-observation timing, and SF2's equivalent Y=0/Y=240 double-buffer
bank. It does not normalize guest writes, PCs, MMIO, scratchpad, or cycle clocks.

- Startup hash:
  `e044f13241a622bd02b465e9e68270c2753976b0f5a40f2beb607596ac8b32ce`
- Input schedule hash:
  `c518cd5e1e597e70eebc0e82e8b305dcc56f6499f8672a52867cdc89bdefd650`
- Same-frame fingerprint hashes match at stable TITLE, aircraft movie, state 8,
  player ownership, and post-movement checkpoints.
- Active overlay image address sets, region/load counts, and candidate counts
  match. At player control there are eight regions and 573 registered
  candidates.

Final dispatch ownership:

| Tier | Run A | Run B |
|---|---:|---:|
| Resident AOT hits | 15,829,452 | 15,828,754 |
| Compiled-overlay dispatches | 141,709,302 | 141,708,765 |
| Interpreter fallback dispatches | 683,327 | 683,189 |
| Fallback share of overlay tier | 0.4799% | 0.4798% |

Counts are sampled a few host-query frames apart and are not expected to be
identical. Same-frame cumulative fingerprints are exact. Fallback is required
for current coverage and is not described as native execution.

Both final runs have zero lost CD INT1 events, nonzero GPU work, more than 1,200
SPU key-ons, and identical XA input totals (17,522,400 frames, 17,099,138
nonzero). Headless runs use SDL's dummy backend and do not claim physical audio.

## Harness failures retained

Four clean attempts stopped before becoming accepted evidence:

1. one process needed longer than two seconds to open its debug endpoint;
2. a Python assignment-expression predicate referenced its local before binding;
3. live socket polling could miss a short PAD pulse;
4. frame history snapshots SIO before that vblank's override application and is
   not the authoritative scheduled-input oracle.

Each failure was bounded and corrected in the harness. None was classified as a
retail or compatibility divergence. The accepted final pair uses direct
emulation-thread application telemetry.

## Validation and footprint

- Release CLI generation: pass.
- Recompiler build: pass.
- Complete framework suite: 43/43.
- Route comparison: pass.
- `git diff --check`: pass.
- Ignored local footprint: 6.604 GiB, below the 20 GiB ceiling.
- No sibling project was modified and no private payload is committed.

## Remaining work

1. Short visible OpenGL acceptance on the retained build tomorrow: complete
   startup, clean FMV borders, and Mission 1 control.
2. Select and validate the representative Mission 3 route through retail menus.
3. Continue overlay capture/compile convergence and honest fallback reporting.
4. Submit generic fixes upstream only after explicit authorization.

The payload-free cross-project return is
`docs/sf2/PSX_PORTS_RETURN_2026-08-03.md`.
