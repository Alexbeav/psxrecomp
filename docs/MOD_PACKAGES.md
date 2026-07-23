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

# Optional: use this for structural changes that cannot be represented as
# equal-size sector writes. Multiple entries may select different recipes from
# one launcher option, but exactly one may resolve in the complete mod plan.
[[derived_disc]]
kind = "vcdiff"
patch = "assets/fast.xdelta3"
patch_sha256 = "..."
output_size = 600000000
output_sha256 = "..."
when_option = "delay"
when_value = "fast"

# For option matrices, use a condition table. All listed option values must
# match. `when_option` / `when_value` remain supported for single-option cases.
[[derived_disc]]
kind = "vcdiff"
patch = "assets/fast-rockman.xdelta3"
patch_sha256 = "..."
output_size = 600000000
output_sha256 = "..."
when = { delay = "fast", title_screen = "rockman_japan" }
```

Option types are `boolean`, `choice`, and bounded `integer`. `when_option` /
`when_value` are optional; `when = { option = "value", ... }` can be used when a
patch or derived-disc recipe depends on multiple option values. An unconditional
patch omits both condition forms.

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

## Derived discs

A `derived_disc` is a data-only VCDIFF recipe whose source is the verified stock
disc from `[[target]].disc_sha256`. It is intended for mods that relocate files,
grow the ISO, replace large assets, or otherwise change disc geometry. A package
may contain a matrix of conditional recipes for its own options, but exactly one
recipe may resolve in the complete mod plan.

The launcher continues to display and persist the user's stock BIN/CUE. Before
boot, the runtime:

1. fingerprints that stock image and resolves package options;
2. verifies the package's VCDIFF payload;
3. invokes the release's trusted `xdelta3` binary (packages cannot provide an
   executable);
4. verifies the derived size and SHA-256; and
5. atomically publishes and mounts
   `mods/cache/<plan-fingerprint>.bin`.

Changing package versions or options changes the plan fingerprint and therefore
the cache key. A cached result is reused on later launches. Ordinary guarded
sector overlays may be applied on top of the derived image, so a structural
base package can support many small composable add-ons.

Release builders stage the trusted decoder with
`-DPSXRECOMP_XDELTA3_EXECUTABLE=/path/to/xdelta3`. More than one active
derived-disc recipe is rejected after option resolution; packages should use a
single owner package for structural transforms and dependencies/conflicts for
external ownership.

## Resolution rules

- Installed versions are side-by-side. The launcher can select an older version
  to roll back.
- Enabled packages are topologically ordered by dependencies, then by stable
  package/patch order.
- Missing dependencies, version mismatches, declared conflicts, dependency
  cycles, overlapping writes, unavailable trusted resolvers, invalid option
  values, multiple derived-disc providers, and target mismatches prevent launch.
- The resolved package versions, option values, writes, and derived-disc recipe
  produce a canonical SHA-256 plan fingerprint suitable for diagnostics and
  multiplayer agreement.
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
