# SF2 modernization pass 2 — Disc 1 human validation

Updated: 2026-08-03

## Start the recorded enhanced run

From the repository root:

```powershell
.\tools\start_sf2_disc1_validation.ps1
```

This creates an isolated ignored directory named
`lab/sf2/local/human-disc1-pass2-<timestamp>/`. Play Missions 1--8 naturally,
then close the game at a stable post-Mission-8 state. The launcher retains:

- an exact final SIO-visible PAD timeline;
- executable, game-config, BIOS and settings hashes;
- bounded 4:3/wide and OpenGL present transitions;
- application/camera-ownership transitions and low-rate semantic samples; and
- cumulative fullscreen-rectangle checks/expansions.

It does not capture screenshots, framebuffer/RAM dumps, audio, movie data,
overlay payloads or other retail content. The monitor is observational and
never writes guest state.

The currently validated local candidate identities are:

| Artifact | SHA-256 |
|---|---|
| Enhanced executable | `B23DB23D36D6174B90542B4210D3BF67E5F1FEE7F5EAE711490B4E494E056E12` |
| Generated pass-2 game configuration | `7679447737F07D462254A3B869711A7D317351A8B74B2CCC491201CFEA1ED410` |
| Pass-2 settings | `6AEB79B2EAD9C96AB13623320CC000856C4F83F74F93ADCA4D0ACA4933ADAFDA` |
| OpenBIOS input | `FABE498FBF224E4721F12F31B6F5FE0659205E341DC4E5C5F91B9BD1A1011C57` |
| Frozen 4:3 executable | `00C88921DEA3A28CC06E76876E7181909F0AF83A679F793D93CCDD6AA7915AB6` |

The evidence file recomputes these identities at launch; do not accept a run
whose hashes differ without rebuilding and recording the new candidate.

## What to check

Across the complete route, watch for:

1. World geometry or actors disappearing only near the new 16:9 edges.
2. HUD pieces separating, stretching, drifting from their authored anchors, or
   being clipped.
3. FMVs, TITLE, briefing, save and other authored 2D screens stretching instead
   of remaining centered 4:3.
4. Scopes, Mission 6 NVG, fades, flashes and cinematic mattes failing to cover
   the complete wide output.
5. Mouse input moving a scripted camera, snapping when control returns, or
   behaving differently after death/checkpoint reload.
6. Chase and first-person yaw/pitch sign, sensitivity, bounds and responsiveness.
7. Any gameplay, dialogue, save, checkpoint or mission-flow difference from the
   frozen compatibility build.

If a defect appears, note the mission, objective/location, active weapon or
view, and whether it survives pause/unpause or checkpoint reload. A screenshot
is useful for visual defects, but the recorded session remains the semantic
evidence source.

## Frozen 4:3 A/B

For a fresh compatibility run:

```powershell
.\tools\start_sf2_baseline.ps1
```

To reproduce from a recorded enhanced session's latest retail card state,
close the enhanced runtime first and run:

```powershell
.\tools\start_sf2_baseline_ab.ps1 `
  -EnhancedSession .\lab\sf2\local\human-disc1-pass2-<timestamp>
```

The A/B launcher copies the cards into a new ignored baseline-only directory.
It never shares writable card state with or modifies the enhanced evidence.

## Acceptance rule

The Disc 1 gate passes only after Missions 1--8 complete with correct world,
HUD, FMV, scope/NVG, fade/matte and camera behavior. A wide present count alone
does not prove visual correctness. Any first divergence is classified and fixed
at its owning generic or verified SCUS-94451 rule before the run resumes.
