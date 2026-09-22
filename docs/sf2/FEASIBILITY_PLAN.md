# SF2 recompilation feasibility plan

## Decision to make

Can a static-recompilation-first runtime reach SF2's retail frontend and a
representative mission with less game-specific machinery than the existing
hybrid port, while retaining deterministic behavior and a credible path toward
complete overlay coverage?

This is a bounded comparison, not a second full port.

## Architecture under test

- Ahead-of-time translation owns the resident PS-X executable.
- Runtime-loaded overlays are captured and compiled when supported.
- The interpreter remains a measured fallback for unseen or runtime-installed
  code.
- The PS1 BIOS, GPU, SPU, CD-ROM, DMA, timers, interrupts, controllers, and
  memory cards remain part of the PSXRecomp runtime contract.
- SF2 retail code continues to own gameplay, scripts, AI, collision, camera,
  inventory, objectives, checkpoints, campaign state, and authored timing.

## Workspaces and authority

| Workspace | Authority |
|---|---|
| `I:\Projects\SF2-Recomp-Lab` | This recompilation feasibility experiment |
| `I:\Projects\sf-pc-port` | Known-good SF2 behavior and deterministic oracle |
| `I:\Projects\SF2-Modern` | Optional modern presentation only |
| `I:\Projects\PSX-References` | Read-only external research references |

Never repair an experiment failure by modifying one of the oracle workspaces.

## Gates

### R0 — reproducible project generation

- Build the PSXRecomp CLI from this pinned framework revision.
- Generate an SF2 project from a user-owned USA Disc 1 CUE and retail BIOS.
- Record hashes of tools and inputs without committing either retail input.
- Regenerate twice and explain or eliminate output differences.

Exit condition: two clean generations have an identical manifest and build.

### R1 — resident executable boot

- Confirm `SCUS_944.51` identity and entry point.
- Reach the retail CRT, `Game_Main`, and stable application loop.
- Log native versus interpreter dispatch and every unsupported instruction or
  host-service boundary.
- Compare the milestone state with the hybrid runtime's executable bootstrap.

Exit condition: deterministic TITLE-bound execution or a precisely localized
blocker with register, memory, and device evidence.

### R2 — frontend and overlay model

- Load the retail TITLE/MENU/INIT overlay family.
- Demonstrate overlay capture, recompilation, cache reuse, and invalidation.
- Quantify interpreter fallback by address range and execution share.
- Verify two-page GPU presentation, controller input, SPU/XA activity, and STR
  requests without adding SF2-native gameplay substitutes.

Exit condition: an operable retail frontend and a measured overlay pipeline.

### R3 — Mission 3 vertical slice

- Select Colorado Interstate 70 through retail state transitions.
- Reach the in-engine opening and player control.
- Validate world/HUD presentation, input, dialogue, combat, death/restart, and
  one checkpoint against the same route in `sf-pc-port`.
- Capture enough mission overlays to measure convergence toward native code.

Exit condition: a deterministic, comparable Mission 3 segment or a documented
architectural blocker.

### R4 — decision report

Measure:

- setup and game-specific engineering time;
- resident and overlay native-code coverage;
- interpreter execution share;
- performance and frame pacing;
- correctness across CPU/GTE/GPU/SPU/CD/timers/interrupts;
- determinism and debugging quality;
- save-state, memory-card, and multi-disc feasibility;
- licensing implications; and
- estimated cost of reaching all 21 missions.

Choose one outcome:

1. Continue PSXRecomp as a noncommercial SF2 port experiment.
2. Independently implement selected ideas in a clean reusable harness.
3. Retain the existing hybrid architecture and use recompilation only for
   selected verified functions or offline analysis.
4. Stop the experiment because it offers no material advantage.

## Stop conditions

Pause for architectural review if any of these occurs:

- progress requires copying game-derived generated code into a public repo;
- the experiment requires weakening deterministic comparison;
- a proposed fix substitutes native gameplay for retail behavior;
- upstream license terms conflict with the intended use; or
- R1/R2 cost exceeds the corresponding measured hybrid bring-up without a
  compensating long-term benefit.

## Initial commands

Build the framework CLI:

```powershell
cd I:\Projects\SF2-Recomp-Lab
$env:PYTHONUTF8 = "1"
python tools\build_cli.py release
```

Validate the complete pinned framework baseline. The explicit UTF-8 mode is
required on Windows installations whose active code page cannot decode UTF-8
test fixtures:

```powershell
$env:PYTHONUTF8 = "1"
cmake --build recompiler\build-cli --parallel
ctest --test-dir recompiler\build-cli --output-on-failure
```

Generate only into ignored local storage:

```powershell
New-Item -ItemType Directory -Force .\lab\sf2\local | Out-Null

.\dist\psxrecomp.exe build `
  --disc "<path-to-Syphon Filter 2 (USA) (Disc 1).cue>" `
  --bios "<path-to-legally-obtained-retail-bios.bin>" `
  --output ".\lab\sf2\local\generated-disc1"
```

Do not commit anything produced by that command.
