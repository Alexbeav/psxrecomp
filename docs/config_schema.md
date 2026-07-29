# PSXRecomp v4 — config TOML schema

Consumed by:
- `psxrecomp-bios.exe` and `psxrecomp-game.exe` (via `--config <toml>`)
- `tools/audit_config.py` and the audit tooling that wraps it
- The runtime cmake macro (`runtime/runtime.cmake`)

Examples:
- `bios/SCPH1001.toml` — BIOS-only config (no game; psx-runtime targets this)
- `../TombaRecomp/game.toml` — game config (tomba-runtime targets this)

## How configs combine

A PSXRecomp v4 process **always** has a BIOS config. It optionally also has
a game config. Both are TOML files in this schema:

- `bios/SCPH1001.toml` (or another BIOS .toml in `bios/`) — describes the
  BIOS. Always loaded.
- `<game>/game.toml` — describes a single game. Loaded ONLY when running
  that game.

```
psxrecomp                            # BIOS-only regen (uses default bios.toml)
psxrecomp games/tomba/game.toml      # BIOS + game regen
psx-runtime                          # boots BIOS discless
psx-runtime games/tomba/game.toml    # boots BIOS, then loads game
```

When both are loaded, keys merge:

- **Scalar keys (`debug_port`, `window_title`, `memcard_dir`, ...)**: game
  wins if set, inherit from bios.toml otherwise. Shallow override.
- **`[program]` (BIOS) and `[game]` blocks**: NOT merged — they describe
  different programs. Both are visible to the loader.
- **Generated dispatch tables and C output**: ADDITIVE. BIOS contributes
  `SCPH1001_*.c`; game contributes `<exe>_*.c`. No address overlap is
  expected (BIOS lives at RAM 0x500-0x8500 + ROM 0xBFC..., game at
  0x80010000+). The cmake macro `psxrecomp_v4_add_runtime_target` already
  links both.
- **`[[audit.regions]]` and `[[audit.normalize.remap]]`**: additive — game
  adds its regions on top of BIOS's.

## Top-level blocks

```toml
[program]    # in bios.toml; describes the BIOS
[game]       # in game.toml; describes the game
[recompiler]
[runtime]
[audit]
```

## Program / game block

- **bios.toml** has a `[program]` block describing the BIOS ROM.
- **game.toml** has a `[game]` block describing the game EXE / disc.

These are NOT alternatives; they're complementary. A runtime loading both
sees both blocks. The legacy single-file audit loader
(`tools/audit_config.py`) accepts either as the program-info source for
backwards compat, but going forward they are the canonical names for their
respective files.

### Fields

| Field | Required for | Description |
|---|---|---|
| `name` | both | display name, e.g. `"SCPH1001 BIOS"` |
| `id` | both | canonical id, e.g. `"SCPH-1001"` or `"SCUS-94236"` |
| `rom` | bios | path to raw flat binary, relative to project root |
| `exe` | game | path to PS-X EXE file, relative to project root |
| `load_address` | both | hex string, virtual address of first byte (`"0xBFC00000"` BIOS, `"0x80010000"` typical game) |
| `entry_pc` | both | hex string, first PC to execute |
| `text_size` | both | hex string, size in bytes of the static region. For games this also bounds main-EXE analysis and establishes the overlay floor. A smaller-than-header bound must be verified non-code and 4 KiB aligned. |
| `stack_base` | game | hex string, initial `$sp` value for the game |
| `disc` | game (single-disc) | path to .cue, relative to project root |
| `discs` | game (multi-disc) | array of .cue paths; `disc` is sugar for `discs = [disc]` |

## Recompiler block

```toml
[recompiler]
seeds       = "recompiler/seeds/phase2_ghidra_seeds.json"  # BIOS
seeds       = "seeds/ghidra_funcs.txt"                     # game (note: game seeds aren't json today)
bios_thunks = "seeds/tomba_bios_thunks.txt"                # game-only
out_dir     = "generated"                                  # both
strict      = true                                         # both — currently always true
discovery   = "whole-image"                                # game-only: "whole-image" or "reachable"
out_stem    = "SCPH1001"                                   # optional; overrides the auto-derived stem

[[recompiler.patch]]
id          = "descriptive-policy-name"
address     = "0x80012340"
expected    = "0x24020002"
replacement = "0x24020001"
note        = "Why this game-owned instruction change is required" # optional
```

Output filenames: `<out_dir>/<out_stem>_full.c` and
`<out_dir>/<out_stem>_dispatch.c`. If `out_stem` is omitted, it's derived
from the `rom`/`exe` file basename with the trailing `.BIN` or `.EXE`
stripped (`Path.stem` is NOT used because it mishandles `SCUS_942.36`).

Game `discovery` defaults to `"whole-image"`, preserving the existing sweep and
pointer-table heuristics. Opt-in `"reachable"` starts at the executable entry
and evidence-backed seed roots, then follows callable direct `jal` targets. It
does not sweep arbitrary bytes for prologues or return-shaped words. Unresolved
`jalr`/indirect targets and unseen callbacks fail closed to runtime
interpretation; add an evidence-backed seed (or a `dispatch_root` seed for a
proven nonstandard boundary) when they should be compiled.

When `game.text_size` is smaller than the PS-X EXE header size, recompilation
uses it as a static-analysis bound. The value must be nonzero, instruction- and
4 KiB-aligned, retain `entry_pc`, and not extend past PS1 RAM. The original EXE
is still loaded from the user's disc. A generated config's canonical final-page
reservation may be slightly larger than the header; it does not widen analysis.

Each `[[recompiler.patch]]` replaces one exact 32-bit MIPS word before function
discovery, control-flow analysis, and normal translation. It is intended for
small, understood game-code changes whose addresses, opcodes, and policy remain
in the game repository. The framework does not contain title IDs or
title-specific addresses.

- `id`, `address`, `expected`, and `replacement` are required hex/string
  fields; `note` is optional.
- IDs are case-sensitive and unique within one config.
- Addresses must be four-byte aligned and are unique by the PSX 29-bit physical
  address. Thus `0x00012340`, `0x80012340`, and `0xA0012340` are aliases of one
  site and cannot define separate patches.
- Main-EXE generation fails if the word at the target site is not `expected`.
  This catches a wrong disc revision or stale patch instead of guessing.
- Captured overlays may place unrelated variants at one virtual address. In
  overlay mode, a patch is applied only to a variant whose word is `expected`;
  a nonmatching variant is translated unchanged.
- When `--config` and `--ws-config` supply the same byte-identical patch it is
  deduplicated. Reusing an ID or physical address for different patch data is
  an error.

Patches are build-time inputs, not runtime memory writes or live toggles.
Regenerate the affected main executable or captured overlays after changing
them.

## Runtime block

Consumed by the cmake macro `psxrecomp_v4_add_runtime_target` (eventually)
and by `runtime/src/main.cpp` as the source of compiled-in defaults.

```toml
[runtime]
debug_port    = 4370            # TCP port for the debug server
window_title  = "..."           # SDL window title
controller    = "digital"       # "digital" or "dualshock"
memcard_dir   = "."             # memcard files location, relative to project root
```

### Opt-in warm CD routes

Per-game read acceleration is disabled unless the game explicitly declares
one or more strict routes. Each route arms on one `SetLoc`, then requires every
later file start to match `lbas` in order. A mismatch immediately restores the
normal configured disc timing. Only data-read cadence is accelerated; XA/CDDA,
seek, and motor timing remain authentic.

```toml
[[runtime.warm_cd_routes]]
arm_lba = 95947
lbas = [298, 299, 306]
instant_max_per_frame = 32
```

Up to 16 routes may be declared, with 1–64 LBAs each. The old singular
`[runtime.warm_cd_route]` table is deprecated and emits a warning when loaded;
it remains readable for compatibility. This enhancement is intentionally
opt-in and must not acquire a global default.

The other load-time accelerators are likewise opt-in:

```toml
[runtime]
turbo_loads = true
idle_skip = true
turbo_audio_sink = true
```

`turbo_audio_sink` is meaningful only while `turbo_loads` is active. It keeps
the guest SPU timeline advancing but discards accelerated samples before host
playback, then fades normal output back in.

## Video Block

Runtime video defaults live in `[video]`:

```toml
[video]
renderer = "opengl"       # "software", "opengl", or "vulkan"
offer_vulkan = false      # show Vulkan in the launcher only after game validation
```

`renderer = "vulkan"` remains an experimental runtime choice and still requires
a build compiled with Vulkan support. `offer_vulkan` controls launcher
visibility only; it defaults to false so game projects must explicitly expose
Vulkan after validating their visuals and stability.

Reserved future fields:
- `default_disc_path` — game runtimes can pre-mount a disc
- `default_game_root` — for sibling-junction setups

## Audit block

See `docs/audit_inventory.md` for the audit pipeline. The schema here is
the input side: regions to walk, address-normalisation rules.

```toml
[audit]
function_starts = "generated/ghidra_function_starts.json"   # optional

[[audit.regions]]
name        = "..."             # e.g. "Boot", "Kernel", "Shell", "Text"
rom_start   = "0x..."           # byte offset in rom/exe file
rom_end     = "0x..."           # exclusive
vaddr_base  = "0x..."           # virtual address corresponding to rom_start

[audit.normalize]
kseg_mask = "0x1FFFFFFF"

[[audit.normalize.remap]]
description = "..."             # human-readable; not consumed by tooling
from_lo     = "0x..."           # inclusive
from_hi     = "0x..."           # exclusive
to_lo       = "0x..."           # target start offset (phys = phys - from_lo + to_lo)
```

## What's NOT in the schema yet (Phase B+)

These are noted here so future work knows where to slot them:

- Game `discs` field (Phase D). For now Tomba uses `disc = "..."`.
- `[runtime] disc_swap_command` — runtime-side disc swap (Phase D).
- `[recompiler] seeds` as an array of paths (currently single file;
  Phase A might allow multiple).
- `[program] type` explicit discriminator (currently inferred from
  `rom` vs `exe` field presence).
