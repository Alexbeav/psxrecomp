# SF2 modernization pass 1

Date: 2026-08-03

Branch: `experiment/sf2-modernization-pass1`

Compatibility checkpoint: `2009297`

## Result

The isolated first pass is ready for visible acceptance testing. It adds 4x
OpenGL internal resolution and keyboard/mouse-to-retail-PAD translation without
changing retail gameplay, generated game code, captures, or the compatibility
executable. Two clean hidden OpenGL runs reproduce authentic startup and the
Mission 1 state-0 route with matching normalized evidence.

This is not a claim of widescreen, smooth mouse free-look, remastered UI, or
campaign-wide modernization compatibility.

## Isolation and launch

- Baseline executable:
  `lab/sf2/local/generated-disc1-r2-load-delay/build-r8-scheduled-input/SCUS94451_Recompiled.exe`
- Modern executable:
  `lab/sf2/local/generated-disc1-r2-load-delay/build-modern-pass1/SCUS94451_Recompiled.exe`
- Baseline cards: `lab/sf2/local/baseline-ab-cards`
- Modern cards: `lab/sf2/local/modern-pass1-cards`
- Baseline launcher: `tools/start_sf2_baseline.ps1`
- Modern launcher: `tools/start_sf2_modernized.ps1`
- Rebuild modern only: `tools/build_sf2_modernized.ps1`

The launchers label their windows `SF2 BASELINE` and `SF2 MODERN PASS 1`.
Neither launcher writes into the other's build or card directory.

## Presentation contract

The modern profile requests OpenGL, 4x internal GPU supersampling, nearest
texture filtering, borderless desktop output constrained to authentic 4:3,
and no frame interpolation. Runtime evidence reports `internal scale 4x`.
There are no hardcoded movie dimensions, black bands, SF2 addresses, or
presentation-time guest writes.

## Input contract

Keyboard:

- W/S: forward/back
- Q/E: retail turn left/right
- A/D: retail L2/R2 strafe
- Left Ctrl: Square/fire
- Left Alt: L1/manual aim
- Tab: R1/target
- F: Triangle/interact
- C: Cross/kneel
- Space: Circle/roll
- R: Select/weapon
- Escape: Start/pause

Mouse:

- Left: Square/fire
- Right: L1/manual aim
- Middle: Select/weapon
- X1: Circle/roll
- X2: Triangle/interact
- Horizontal motion: bounded retail D-pad turn pulses
- Vertical motion: bounded D-pad aim pulses only while RMB/L1 is held

All mappings terminate at the normal active-low PS1 PAD boundary. The adapter
does not locate or write SF2 player/camera memory. Because SF2 has no retail
right-stick camera axis, this first pass is coarse digital mouse control.

Hidden and unfocused windows suppress keyboard-derived buttons, sticks,
hybrid switching, and the Tab turbo shortcut. The modern launcher additionally
sets `PSX_DEV_INPUT=0` while the process is alive, preventing unrelated
background controllers from being merged into Player 1.

## Read-only references and provenance

`I:/Projects/syphon-filter-redux` confirmed that 4x supersampling is a proven
presentation target and contains game-specific free-look work. No guest
addresses, camera writes, or implementation code were copied. The other SF2
project confirmed the desired semantic action layout; this lab independently
maps those actions to ordinary retail PAD inputs. Sibling projects were not
modified.

## Automated evidence

The complete route was run twice from fresh processes and separate blank card
directories using hidden OpenGL and SDL dummy audio. Both runs saw 989,
Eidetic, legal, ZINTRO, and TITLE before input. Scheduled retail inputs were
Cross 19200--19219, Cross 19320--19339, Cross 24000--24019, and D-pad Up
25800--25859.

Strict comparison result: PASS.

- startup hash:
  `d40e8858be83fcab5e89f9ce2adadfa1991c71bc729f653368ba2dc86a95ef6f`
- input schedule hash:
  `c518cd5e1e597e70eebc0e82e8b305dcc56f6499f8672a52867cdc89bdefd650`
- normalized fingerprints match at stable TITLE, aircraft movie, state 8,
  player ownership, and post-movement
- both players/cameras resolve to `0x801A0ECC`
- both finish at XYZ `(-5606,2036,7529)`, health 150, armor 600
- both report zero lost CD INT1 events, 1,208 SPU key-ons, and identical
  nonzero XA/CD input totals

Ownership at the final checkpoint:

| Tier | Run F | Run G |
|---|---:|---:|
| Resident AOT | 15,830,035 | 15,829,527 |
| Compiled overlay | 143,524,201 | 143,523,764 |
| Interpreter fallback | 683,265 | 683,155 |
| Fallback / overlay tier | 0.4738% | 0.4737% |

Fallback remains separate and is not native coverage. Each run loaded eight
overlay regions and registered 572 candidates at the final gate.

## Failures retained during validation

1. The first modern build had debug tools disabled by the product default. The
   runtime was healthy but the route endpoint did not exist. The validation
   build now requests debug tools explicitly.
2. A schedule derived from the host-polled stable-TITLE frame selected either
   frame 18600 or 19200 across runs. Input is now anchored to the retained
   retail TITLE movie event; a regression protects the fixed schedule.
3. A hidden run entered retail app state 7 instead of the aircraft route. Live
   PAD evidence showed unsolicited D-pad Up. Keyboard focus gating narrowed the
   source; a second probe proved dev-any-input was merging a background
   controller. Strict routing produced neutral `0xFFFF` PAD and the final pair.

These were harness/host-input invariants. No containment was added to retail
state or generated code.

## Tests and footprint

Focused regressions cover mouse/PAD pulses, final PAD sample ordering,
guest-event route scheduling, and hidden/unfocused keyboard neutrality. The
complete framework suite passes 48/48. `git diff --check` passes. The ignored
local footprint is 7.137 GiB, below the 20 GiB cap.

## Visible acceptance request

Run the modern launcher and test frontend navigation, Mission 1 camera and
aiming behavior, FMVs, dialogue, saving/loading, and focus loss/re-entry. If a
defect appears, close the modern process and repeat the same retail action with
the baseline launcher. Report which window reproduced it and the nearest
retail state. Do not share or commit cards or captures.

## Next work after acceptance

1. Tune digital mouse sensitivity/direction only from observed user feedback.
2. Validate a broader mission/save/load slice without changing the retail PAD
   boundary.
3. Treat smooth native free-look as a separate semantic-camera project; it
   requires independently verified SF2 ownership and regressions.
4. Consider widescreen only after high-resolution/input compatibility is
   stable. No widescreen work is part of this pass.
