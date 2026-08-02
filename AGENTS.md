# Codex instructions — SF2 recompilation feasibility lab

These instructions apply to the entire repository. This checkout is an
isolated, noncommercial feasibility experiment based on PSXRecomp. It is not
the shipping SF2 port and must never modify its sibling workspaces.

## Mission

Determine whether PSXRecomp's static resident-executable translation,
capture-and-compile overlay pipeline, and bounded interpreter fallback can
reach the SF2 retail frontend and a representative Mission 3 route with less
game-specific machinery than the existing hybrid runtime.

This is a measured architectural comparison, not a race to claim the game is
playable and not authorization to produce a second full port.

## Required reading before acting

Read these files completely at the start of each Codex session:

1. `SF2_LAB.md`
2. `docs/sf2/CURRENT_OBJECTIVE.md`
3. `docs/sf2/FEASIBILITY_PLAN.md`
4. `docs/sf2/COMPARISON_PROTOCOL.md`
5. `docs/sf2/REFERENCE_MAP.md`
6. `lab/sf2/reference-manifest.toml`
7. `CLAUDE.md`

`CLAUDE.md` is inherited upstream context. Preserve its hardware-faithfulness,
no-stubs, no-generated-code-edits, evidence, and bounded-diagnostics rules.
Its older maintainer-specific phase checklist, unrelated local paths, and
mandatory external MCP/browser requirements do not override this branch's
`CURRENT_OBJECTIVE.md` and `FEASIBILITY_PLAN.md`. The checked-out upstream now
ships a game-project CLI and overlay interpreter fallback; use the behavior of
this pinned commit as the framework contract.

## Workspace boundaries

- Work only in `I:\Projects\SF2-Recomp-Lab` unless performing a read-only
  comparison.
- `I:\Projects\sf-pc-port` is the SF2 correctness oracle. Never edit it from
  this project.
- `I:\Projects\SF2-Modern` is the presentation stream. Never edit it here.
- `I:\Projects\SF3-PC-Port` is the independent SF3 bring-up. Never edit it.
- `I:\Projects\PSX-References` is read-only research material.
- Do not create branches or commits in any sibling repository.

## Licensing and provenance

- This fork remains under PolyForm Noncommercial 1.0.0. Do not describe it as
  commercially reusable or merge its source into an MIT project.
- Never commit or distribute a BIOS dump, BIN/CUE/ISO/CHD, PS-X executable,
  disc sector, generated game C, overlay capture, RAM/save-state dump, movie,
  audio, texture, model, screenshot, memory card, or private third-party file.
- Keep all game-derived outputs under `lab/sf2/local/`, `lab/sf2/generated/`,
  `lab/sf2/captures/`, `lab/sf2/traces/`, or `.local-context/`. These paths are
  ignored by Git.
- External symbols, addresses, patches, and notes are leads. Independently
  verify them against the user-owned executable before committing a fact.
- Commit only source-owned framework changes, configuration metadata, scripts
  that require user-owned inputs, tests, and derived factual documentation
  that contains no copyrighted payload.

## Architecture rules

- Retail SF2 code owns gameplay, scripts, AI, collision, camera, inventory,
  objectives, checkpoints, saves, campaign flow, and authored timing.
- Fix generic CPU/GTE/GPU/SPU/CD/DMA/timer/interrupt behavior at the framework
  level when evidence shows a framework defect.
- Never fake progress with a native gameplay substitute, forced state write,
  hardcoded successful callback, skipped retail transition, or presentation-
  only approximation.
- Never edit generated C or captured overlays. Fix the recompiler, runtime, or
  source configuration and regenerate.
- A working interpreter fallback is not native recompilation coverage. Measure
  and report each dispatch tier honestly.
- Do not add modernization work: no widescreen, PGXP, interpolation, texture
  replacement, mouse-camera enhancement, or remastered UI.

## Evidence and validation

- Every indirect jump, relocation rule, or hardware-interaction conclusion
  needs a manifest or reproducible proof artifact.
- Prefer bounded counters, ring buffers, structured summaries, and
  deterministic hashes. Do not produce unbounded instruction logs.
- Keep the complete local experiment footprint below 20 GiB unless the user
  explicitly approves more. Check size before and after any capture campaign.
- Never infer playability from a rendered frame or changing GPU hash. Player
  control requires retail player ownership plus observed player-state movement
  under deterministic input.
- Run comparisons twice from clean processes and separate stable guest state
  from host-variant values.
- When tooling is broken, fix or precisely diagnose it before relying on its
  output.

## Windows build baseline

Use PowerShell from the repository root:

```powershell
$env:PYTHONUTF8 = "1"
python tools\build_cli.py release
cmake --build recompiler\build-cli --parallel
ctest --test-dir recompiler\build-cli --output-on-failure
```

The verified baseline is 38/38 passing tests. The CLI package is ignored under
`dist/`.

## Git discipline

- Begin by checking branch, status, remotes, and recent commits.
- Expected branch: `experiment/sf2-recomp-feasibility`.
- Preserve unrelated user changes. Do not reset, clean, or discard work.
- Keep commits small and milestone-oriented. Update the current devlog with
  commands, evidence, failures, and rejected hypotheses.
- Run `git diff --check` and the relevant tests before each checkpoint commit.
- There is intentionally no `origin`. The `upstream` push URL is disabled. Do
  not add or push a remote without explicit user authorization.

## Session behavior

- Continue autonomously through safe, read-only or local-only steps while a
  milestone remains actionable.
- Announce before launching a visible game/runtime window and do not terminate
  a user-owned process.
- Do not ask the user to test until an automated gate passes and the requested
  observation cannot be obtained from deterministic instrumentation.
- If a legally obtained retail BIOS becomes necessary, record the exact need
  and ask only for its local path; never ask the user to upload or commit it.
- At the end of a work session, update `docs/sf2/CURRENT_OBJECTIVE.md` and the
  chronological devlog so another Codex session can resume without rediscovery.
