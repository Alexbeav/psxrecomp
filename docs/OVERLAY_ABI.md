# Overlay ABI

This document describes the contract between the runtime and an overlay shard
(a compiled DLL or shared object of guest code). The header
`runtime/include/overlay_api.h` is the source. This document explains it.

## What the contract contains

- `OverlayCallbacks`: a table of pointers that the runtime gives to
  `overlay_init()`. Each member is one pointer. The shard copies the table.
- `CPUState` (`runtime/include/cpu_state.h`): shards read and write it directly.
- The shim `runtime/include/overlay_dispatch_preamble.c.inc`: it is compiled
  into each shard and forwards runtime calls through the table.

A shard exports `overlay_abi()`. The value is the ABI tag:
`PSX_OVERLAY_ABI_VERSION` in the low 16 bits and the flavour in the high 16 bits.
The loader compares the tag for equality. If the tags differ, the loader does
not call the shard.

## Version 26

Version 26 is upstream's version 25 layout, with the fork's additions after it.
Upstream is RetroPortingToolKit/psxrecomp. The layout was taken at commit
3505f2a01.

Before version 26, the fork and upstream both used the number 25 for different
tables. Slots 0 to 44 were the same. Slots 45 and 46 were not.

| Slot | Offset (x86-64) | Member | Owner | Since |
|---|---|---|---|---|
| 0 to 44 | 0x000 to 0x160 | `dispatch_call` to `ws_screen_x_bound` | upstream | v23 or before |
| 45 | 0x168 | `int (*mod_function_entry)(cpu, address)` | upstream | v25 (return type) |
| 46 | 0x170 | `uint32_t *last_store_pc` | upstream | v24 |
| 47 | 0x178 | `int (*cpu_step_boundary_enabled)(include_replay)` | fork | v26 |
| 48 | 0x180 | `void (*cpu_step_boundary)(cpu, address)` | fork | v26 |

The table is 0x188 bytes. Upstream's version 25 table is 0x178 bytes.

`CPUState` follows the same rule. Upstream's fields come first. The fork adds
`load_value_rt`, `load_value` and `load_value_age` after `ld_absorb`.

### How this tree fills upstream's slots

- `last_store_pc`: implemented. The loader gives the address of
  `g_debug_last_store_pc`. A shard store writes the same word that a static
  store or an interpreted store writes. The store filters in `memory.c` read
  that word in every build.
- `mod_function_entry`: the slot and the generated call have upstream's shape:
  `if (psx_mod_function_entry(cpu, pc)) return;`. A result that is not zero
  means that a function filter completed the guest function and the caller
  must return. This tree has no function filters. Its host function runs the
  entry callbacks and returns 0.

### What is not part of the table

Upstream also keys shards by address segment (KUSEG, KSEG0, KSEG1). That is a
loader, manifest and cache-folder feature (`S <segment>` manifest lines,
`seg-kuseg/` and `seg-kseg1/` folders). It has no slot. This tree does not have
it: shards are KSEG0-keyed.

## Cache and state identity

| Value | Version 26 tree | Pin G2 |
|---|---|---|
| `PSX_OVERLAY_ABI_VERSION` | 26 | 25 |
| `PSX_OVERLAY_CODEGEN_VER` | 14 | 12 |
| `PSX_OVERLAY_CODEGEN_HASH` | changes with the sources | 0x6a0b6aaa |

Codegen version 13 is upstream's. This tree does not use it.

The cache folder is `cg<codegen ver>_<codegen hash>_gc<config hash>_f<flavour>`.
A build reads and writes only its own folder. Folders from other builds stay on
disk and are not opened.

The codegen hash is the first 8 hex digits of SHA-256 over the files in
`runtime/codegen_hash_sources.cmake`, in list order, with CRLF read as LF.
`runtime/hash_codegen.cmake` is the only code that computes it. Line endings do
not change it, so builds of one tree on different hosts agree. Read it
with `compile_overlays.codegen_hash()`, `psxrecomp-game --codegen-hash`, or the
`codegen_hash` field of the boot-state header.

The boot-state header stores the ABI tag, the codegen version and the codegen
hash. A boot-state cache or a save state (`.pst`) from a build where one of the
three differs is refused.

## What the loader does with another build's shards

| Shard | Result |
|---|---|
| Version 26, this build | Loaded. |
| Pin G2 (tag 25) in this build's folder | Refused. `overlay_init` is not called. The file stays. The compiler replaces it. |
| Upstream version 25 in this build's folder | Refused in the same way. |
| A `cg12_...` folder from pin G2 | Not opened. |

The loader keeps the first line about a cache that it cannot use, and counts
the refusals. Read them in two places:

- the `overlay_cache` object of the run report (`psx_last_run_report.json`):
  `abi_tag`, `codegen_ver`, `abi_rejected`, `stale_cache_msg`. A product build
  has no other loader output.
- the `overlay_loader_status` debug command: the same four fields.

Code without a usable shard runs in the interpreter.

An upstream version 25 host refuses a version 26 shard by the same tag
comparison.

## When upstream changes its layout

1. Replace `runtime/tests/fixtures/overlay_api_upstream_v25.h` with upstream's
   new header. Update the blob id in `test_overlay_abi_v26_guards.py`.
2. Copy upstream's new layout into `overlay_api.h` without changes.
3. Put the fork's slots after upstream's last slot.
4. Set `PSX_OVERLAY_ABI_VERSION` to upstream's number plus one.
5. Increase `PSX_OVERLAY_CODEGEN_VER`.
6. Update `runtime/tests/test_overlay_abi_layout.c`.

Each such change makes all overlay caches, boot-state caches and save states
from the previous build unusable.

## Tests

- `runtime/tests/test_overlay_abi_layout.c`: each slot offset and type against
  upstream's header, the table size, and the `CPUState` tail.
- `runtime/tests/test_overlay_abi_v26_guards.py`: the fixture is upstream's
  file; the sources agree with the slot types.
- `runtime/tests/test_overlay_abi_gate_runtime.py`: the real loader with a
  version 26 shard, an upstream version 25 shard and a pin G2 tag.
- `runtime/tests/test_overlay_store_pc_forwarding.c`: a shard store updates
  the host's store-PC word.
- `runtime/tests/test_codegen_hash_line_endings.py`: the hash with LF and CRLF
  sources.
