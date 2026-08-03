# SF2 modernization pass 2 — widescreen and direct mouse

Updated: 2026-08-03

## Scope

This pass adds an opt-in enhanced build without changing the frozen 4:3
compatibility executable. Its acceptance surface is production-quality 16:9
under software and OpenGL plus direct relative-mouse yaw/pitch for third-person
camera control and first-person aiming. Retail owns gameplay and controller
input; scripted camera ownership suppresses the enhancement.

PGXP and high-refresh interpolation are deliberately deferred. New presentation
and input interfaces must remain suitable for those later stages.

## Consult-test-return classification

| Lead | Source | Initial classification | Required local check |
|---|---|---|---|
| Persistent draw/display pages are authored state | PSX-Ports GPU contract and hybrid handoff | confirmed by compatibility work | retain canonical VRAM/page hashes in every wide comparison |
| World projection and fullscreen-effect expansion are distinct policies | hybrid handoff | confirmed behavioral contract | classify world, HUD, FMV, fade, scope and matte submissions independently |
| Relative mouse must bypass retail controller acceleration | hybrid runtime | narrowed to a reusable semantic design | prove the SCUS-94451 execution boundaries and affected retail angle state |
| Scripted camera owner suppresses chase pitch | hybrid runtime | confirmed behavioral contract | prove owner transitions and zero writes while the player does not own the camera |
| `0x80053464` is the chase/facing boundary | hybrid runtime | still open | verify executable identity/word, AOT ownership, live state-0 hits and register/object semantics |
| `0x800539D0` is the manual-aim boundary | hybrid runtime | still open | verify executable identity/word, AOT ownership, aim-only consequence and stack/register semantics |
| Native-wide should use PSXRecomp's separate compositor surface | framework documentation | narrowed implementation direction | establish software oracle, then OpenGL final-present parity at 4x |
| PGXP and high refresh can be copied as completed SF2 features | initial assumption | contradicted | hybrid handoff records both as remaining work; defer them from this pass |

No sibling repository is modified. External addresses and structure layouts do
not enter committed configuration until their local checks pass.

## Acceptance gates

1. The baseline launcher still runs the frozen 4:3 executable and separate
   cards.
2. Mouse buttons may use PAD actions, but mouse motion never emits D-pad or
   analog-stick input.
3. Direct yaw and pitch are non-inverted by default, separately configurable
   for chase and manual aim, focus-safe, bounded and deterministic.
4. Scripted cameras retain ownership; player mouse control resumes without a
   handoff snap.
5. 16:9 expands retail world projection/culling rather than cropping or merely
   stretching a 4:3 scene.
6. FMVs remain authored 4:3. HUD, pause/map, fades, scopes, NVG and cinematic
   mattes follow typed presentation policies.
7. Software and OpenGL 4x produce deterministic framebuffer/display evidence;
   final-present evidence proves the window aspect.
8. The authentic Mission 1 route passes automatically, then the enhanced build
   is handed to the user for the Missions 1--8 Disc 1 run.

## Evidence log

- The locally configured input is the exact supported executable: SHA-256
  `75A360BF7465DFDEC85C14F9BA93862AAE2531B48D83FD8D82BA8C9FFFA13D33`.
- `0x80053464` contains retail word `0x8EA30034` and is an AOT block leader
  inside function `0x800523F4`.
- `0x800539D0` contains retail word `0x8FA20010` and is an AOT block leader
  inside function `0x80053664`.
- Those facts prove identity and native ownership only. They do not yet prove
  the semantic role of either boundary.
