# Syphon Filter 2 recompilation feasibility lab

This branch is an isolated, noncommercial experiment built on PSXRecomp. It
does not replace the working SF2 hybrid port and it is not the SF2 modern
presentation stream.

## Purpose

Determine, with reproducible evidence, how much of *Syphon Filter 2* can be
handled by PSXRecomp's ahead-of-time executable translation, native overlay
cache, and interpreter fallback. Compare the result with the already playable
hybrid runtime before deciding whether to build a clean reusable porting
harness around the approach.

The experiment is successful if it answers the architectural question. It does
not need to reproduce the entire campaign.

## Repository identity

| Field | Value |
|---|---|
| Branch | `experiment/sf2-recomp-feasibility` |
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

1. Read [`docs/sf2/FEASIBILITY_PLAN.md`](docs/sf2/FEASIBILITY_PLAN.md).
2. Read [`docs/sf2/REFERENCE_MAP.md`](docs/sf2/REFERENCE_MAP.md).
3. Use [`lab/sf2/reference-manifest.toml`](lab/sf2/reference-manifest.toml) as
   the immutable identity record for the supported retail executable.
4. Keep discs, BIOS files, generated code, captures, and private notes beneath
   `lab/sf2/local/`, which is ignored by Git.
5. Record every comparison using
   [`docs/sf2/COMPARISON_PROTOCOL.md`](docs/sf2/COMPARISON_PROTOCOL.md).

## Non-goals

- Do not fix outstanding SF2 gameplay bugs here.
- Do not add modern camera, widescreen, PGXP, interpolation, or texture work.
- Do not copy implementation code from `sf-pc-port` into this repository.
- Do not copy PSXRecomp code into the MIT-licensed Syphon Filter repository.
- Do not commit generated retail code, disc sectors, executable bytes, BIOS
  dumps, overlay captures, RAM dumps, screenshots, audio, movies, or saves.
