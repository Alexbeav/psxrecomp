# PSXRecomp mod packages

PSXRecomp games may expose a shared Dear ImGui **Mods** view backed by versioned
`.psxmod` packages. A package is a ZIP archive with `manifest.toml` at its root.
Packages are installed under `mods/packages/<id>/<version>/`; the selected
versions and option values live in `mods/state.toml`.

Mods are resolved and fingerprinted before boot. They never rewrite the user's
disc or the recomp executable.

## Minimal manifest

```toml
format_version = 1
id = "example.faster-charge"
version = "1.2.0"
name = "Faster Charge"
author = "Example Author"
description = "Shortens the charge delay."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared" # or "isolated"
conflicts = ["example.incompatible"]

[[target]]
game_id = "SLUS-00000"
# Optional. When present, the selected image must have this digest.
exe_sha256 = "..."
disc_sha256 = "..."

[[dependency]]
id = "example.core"
version = "^1.0.0"

[[option]]
id = "delay"
label = "Charge delay"
description = "Delay in frames."
group = "Game balance"
type = "choice"
default = "normal"

[[option.choice]]
value = "normal"
label = "Normal"

[[option.choice]]
value = "fast"
label = "Fast"

[[patch]]
target = "main_exe"
address = 0x80041234
expected = "2a 00 02 24"
replace = "0e 00 02 24"
when_option = "delay"
when_value = "fast"
order = 10
```

Option types are `boolean`, `choice`, and bounded `integer`. `when_option` /
`when_value` are optional; an unconditional patch omits both.

## Patch targets

- `main_exe`: `address` is a PSX guest virtual address. All expected bytes are
  checked after the BIOS loads the PS-X EXE. The complete main-EXE plan is then
  applied before the configured entry point executes.
- `disc_raw`: `offset` is in the canonical 2352-byte raw-sector stream
  (`lba * 2352 + byte_in_sector`).
- `disc_user`: `offset` is in the canonical 2048-byte user-data stream
  (`lba * 2048 + byte_in_sector`).

A disc operation may not cross a sector boundary. Use multiple operations.
Expected and replacement data are equal-length hexadecimal byte strings.

Changed main-EXE code is deliberately not represented by a precompiled
permutation. PSXRecomp's exact text-image guard sees the changed live RAM and
routes that code through the existing dirty-RAM interpreter/native overlay
cache. Untouched functions stay on the static native path. This makes runtime
cost proportional to the code actually changed, not to the number of possible
option combinations.

## Resolution rules

- Installed versions are side-by-side. The launcher can select an older version
  to roll back.
- Enabled packages are topologically ordered by dependencies, then by stable
  package/patch order.
- Missing dependencies, version mismatches, declared conflicts, dependency
  cycles, overlapping writes, unavailable trusted resolvers, invalid option
  values, and target mismatches prevent launch.
- The resolved package versions, option values, and writes produce a canonical
  SHA-256 plan fingerprint suitable for diagnostics and multiplayer agreement.
- Package and state changes apply on the next launch. There is no mid-frame
  mutation.

## Trusted adapters

`resolver = "builtin:<id>"` selects a resolver statically registered by the game
executable. This is for legacy patch systems whose dependency and composition
rules cannot be expressed as independent declarative writes. A package cannot
load native code or choose an arbitrary symbol: unregistered IDs fail closed.
The adapter emits the same expected-byte-guarded resolved writes as a
declarative package, so validation, overlap checks, fingerprinting, and runtime
execution remain shared.

## Archive safety

The installer accepts stored or DEFLATE-compressed ZIP entries, validates CRCs,
rejects encrypted entries and unsafe/absolute paths, limits archives to 4096
files and 256 MiB expanded size, stages extraction, validates the manifest, and
publishes the version with an atomic rename.
