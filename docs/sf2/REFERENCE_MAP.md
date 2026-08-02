# SF2 recompilation reference map

## Mandatory local references

Read these sources directly; do not duplicate them into this repository.

### Existing SF2 oracle

- `I:\Projects\sf-pc-port\docs\GAME_RUNTIME_ARCHITECTURE.md`
- `I:\Projects\sf-pc-port\docs\SF2_EXECUTABLE_MAP.md`
- `I:\Projects\sf-pc-port\docs\SF2_SHARED_SYSTEM_MAP.md`
- `I:\Projects\sf-pc-port\docs\SF2_MISSION_SCRIPT_VM.md`
- `I:\Projects\sf-pc-port\docs\SF2_SF3_PORT_NOTES.md`
- `I:\Projects\sf-pc-port\docs\devlogs\2026-07-28-sf2-bring-up.md`
- `I:\Projects\sf-pc-port\tests\data\sf2\README.md`

The existing implementation is an oracle for externally observable state and
known retail boundaries. Its host-specific workarounds are not automatically
requirements for this architecture.

### Reference library

- `I:\Projects\PSX-References\CATALOG.md`
- `recompilation-and-ports\PsyDoom` — gradual native conversion and replay
  validation.
- `recompilation-and-ports\sotn-decomp` — overlays, matching workflow, and
  PlayStation decompilation tooling.
- `emulation-and-sdk\pcsx-redux` — device behavior and debugger integration.
- `emulation-and-sdk\duckstation` — renderer and emulator behavior reference;
  observe its restrictive license.
- `research-and-documentation\ghidra_psx_ldr` — executable/overlay loading and
  PsyQ signatures; no top-level license was detected.
- `research-and-documentation\psx-spx` — PlayStation hardware documentation;
  no top-level license was detected.
- `tooling\splat`, `mips_to_c`, `asm-differ`, `decomp-permuter`, and `maspsx`
  — project decomposition and matching aids.

## Proven SF2 facts to verify independently

- USA Disc 1 serial: `SCUS-94451`.
- USA Disc 2 serial: `SCUS-94492`.
- Both discs contain the same resident executable.
- Executable SHA-256:
  `75a360bf7465dfdec85c14f9ba93862aae2531b48d83fd8d82ba8c9fffa13d33`.
- Missions 1-8 reside on Disc 1; Missions 9-21 reside on Disc 2.
- Retail gameplay depends on runtime-loaded resident and mission overlays.
- Correct presentation requires persistent PS1 display-page semantics rather
  than composition heuristics.

These are starting hypotheses backed by the existing port. The recompilation
lab must reproduce them from its own input and traces.

## Provenance rule

External addresses, symbols, patches, and descriptions are leads. Before they
enter committed SF2-specific configuration, verify them against the user-owned
retail executable and record the observation or derivation. Never commit the
source bytes used for verification.
