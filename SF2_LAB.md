# Syphon Filter 2 recompilation and modernization lab

This repository is an isolated, noncommercial project built on PSXRecomp. It
does not replace the working SF2 hybrid port and it is not the SF2 modern
presentation stream.

## Purpose

The original feasibility question is answered: PSXRecomp's ahead-of-time
executable translation, native overlay cache, and interpreter fallback bring
the full retail game up accurately (see `docs/sf2/FEASIBILITY_PLAN.md` and the
R1/R2 verdicts in `docs/sf2/CURRENT_OBJECTIVE.md`). The lab now preserves that
compatibility result as a frozen differential baseline and continues
production-quality PC modernization — widescreen, direct mouse input, PGXP,
high-refresh presentation — on isolated milestone branches with separately
named builds. Modernization enhances authentic retail execution; it never
replaces retail gameplay, progression, timing, or authored behavior.

## Repository identity

| Field | Value |
|---|---|
| Frozen compatibility branch | `experiment/sf2-recomp-feasibility` (checkpoint `2009297`) |
| Active milestone branch | named in `docs/sf2/CURRENT_OBJECTIVE.md` (do not hardcode it here) |
| PSXRecomp baseline | `0cfa9fe0a8da944e9f694a24361b4973c57131ea` |
| Upstream remote | `https://github.com/mstan/psxrecomp.git` |
| License | PolyForm Noncommercial 1.0.0 |
| Shipping SF2 correctness stream | `I:\Projects\sf-pc-port` |
| SF2 modern presentation stream | `I:\Projects\SF2-Modern` |
| Reference library | `I:\Projects\PSX-References` |

Because the upstream license is noncommercial, nothing produced here should
be presented as the commercial-compatible harness. If the experiment proves
valuable, reusable ideas must be independently implemented in a separately
licensed project after an explicit provenance review.

## Start here

1. Read [`docs/sf2/CURRENT_OBJECTIVE.md`](docs/sf2/CURRENT_OBJECTIVE.md) —
   the live state document (newest entry first) naming the active branch,
   accepted checkpoints, and pending candidate.
2. Read [`docs/sf2/FEASIBILITY_PLAN.md`](docs/sf2/FEASIBILITY_PLAN.md).
3. Read [`docs/sf2/REFERENCE_MAP.md`](docs/sf2/REFERENCE_MAP.md).
4. Use [`lab/sf2/reference-manifest.toml`](lab/sf2/reference-manifest.toml) as
   the immutable identity record for the supported retail executable.
5. Keep discs, BIOS files, generated code, captures, and private notes beneath
   `lab/sf2/local/`, which is ignored by Git.
6. Record every comparison using
   [`docs/sf2/COMPARISON_PROTOCOL.md`](docs/sf2/COMPARISON_PROTOCOL.md).

## Non-goals

- Do not fix outstanding SF2 gameplay bugs here.
- Do not fold modernization into the frozen compatibility baseline. Widescreen,
  direct mouse, PGXP, and high-refresh work are authorized, but only on an
  isolated milestone branch with a separately named build, per
  `AGENTS.md` and `docs/sf2/CURRENT_OBJECTIVE.md`.
- Do not copy implementation code from `sf-pc-port` into this repository.
- Do not copy PSXRecomp code into the MIT-licensed Syphon Filter repository.
- Do not commit generated retail code, disc sectors, executable bytes, BIOS
  dumps, overlay captures, RAM dumps, screenshots, audio, movies, or saves.
