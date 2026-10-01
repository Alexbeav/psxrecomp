# CI helpers for bundled releases

Shared scripts and GitHub Actions so every PSXRecomp title uses one
standardized release flow: CI builds the title's **committed `generated/`
game C** and ships the **compiled game**.

**Start here:** [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md), then
[`BUNDLED_RELEASES.md`](BUNDLED_RELEASES.md) for what ships and why.

## Workflow template

**New Project Layout** (`tools/new_project_layout/setup_project.{sh,ps1}`) copies
this template into the title repo and replaces `YOUR_*` / `yourgame-release`
using `--zip-prefix` (or a derived acronym) plus the window title. It also
writes `scripts/package_release.sh`, `README.md`, and an optional
`framework_pins.txt` snapshot, and can opt in wizard/netplay via
`--enable-wizard` / `--enable-netplay`. Release CI pins via submodule gitlinks
and only logs SHAs with `record_pins.sh` (no `verify_pins` gate).

**Existing titles:** `tools/generate_ci` writes (and `--check`s) the filled
workflow; Project Studio (`tools/new_project_layout/migrate_project.py apply` /
`gui`) emits the same packager wrapper + `release.yml` and un-ignores
`generated/`.

Manual:

```bash
mkdir -p .github/workflows
cp psxrecomp/docs/ci/templates/game-release.yml .github/workflows/release.yml
# edit YOUR_* placeholders — or use fill_tokens.py --ci-placeholders
# Release name is YAML-quoted so titles with ':' (e.g. Marvel vs. Capcom: …) parse.
```

Template: [`templates/game-release.yml`](templates/game-release.yml)

**Versioning:** `workflow_dispatch` with an empty `version` auto-bumps the next
`X.Y.Z` from the latest `v*.*.*` git tag (`bump`: patch / minor / major; default
patch). Pass an explicit version to override. Shared helper:
`tools/ci/normalize_version.sh --next [patch|minor|major]`.

Matrix: `ubuntu-24.04` (linux-x64), `windows-2022` (windows-x64),
`macos-15` (macos-arm64), `macos-15-intel` (macos-x64).

Release builds always configure with Vulkan headers + `glslc` (MSYS2 /
apt / Homebrew packages), verify `vulkan.h` + `glslc` before configure, and
fail the job if CMake does not select `PSX_HAVE_VULKAN`. Runtime still loads
the ICD dynamically via SDL; CI only needs headers and the shader compiler.

## Tools under `psxrecomp/tools/`

| Script | Role |
|--------|------|
| `ci/normalize_version.sh` | Normalize / write `VERSION` + `TAG`; `--next` auto-bumps from latest tag |
| `ci/check_boot_exe.sh` | The three copies of the boot-EXE name agree |
| `ci/check_generated.sh` | Committed game C present + tracked; framework BIOS backends present; no BIOS dump |
| `ci/record_pins.sh` | Log `psxrecomp` / `recomp-ui` / `recomp-net` SHAs (CI + scaffold) |
| `ci/verify_pins.sh` | Optional local check vs `framework_pins.txt` (not used by release CI) |
| `ci/build_emitters.sh` | Build `psxrecomp-game` + `psxrecomp-bios` |
| `ci/sign_windows.sh` | Authenticode-sign the staged Windows binaries (no-op without a certificate) |
| `fetch_toolchain.sh` | Download/unpack cmake-clang-v1 (Windows emitter builds on the CI machine only) |
| `package_game_release.sh` | Bundled zip: compiled game + runtime data + OpenBIOS + mods + overlay toolchain |
| `release_stage.py` | Shared staging surface (mod catalog, overlay toolchain incl. the BIOS emitter/profiles/seeds, overlay cache tag/shards) |
| `bios_module_build.py` | Ships inside the toolchain; builds a retail BIOS backend from the player's dump at runtime |
| `bundle_mingw_dlls.sh` | Copy imported non-system DLLs next to Windows PEs |
| `templates/game.gitignore` | Suggested gitignore for title repos (`generated/` tracked) |

`package_setup_host.sh` / `stage_setup_sdk.sh` are the retired setup-host
packagers; they are not part of the release flow.

## Composite actions

```yaml
- uses: ./psxrecomp/.github/actions/build-emitters
- uses: ./psxrecomp/.github/actions/fetch-toolchain   # Windows emitter build only
```

## Title responsibilities

Keep only this in the game repo:

- `generated/` committed, regenerated (and re-committed) whenever seeds,
  codegen settings, or the framework pin change
- CMake via `psxrecomp_add_game_runtime` with `GEN_MARKER` / `GEN_FULL_GLOB`
  naming the committed C, and the wizard on (first-run disc picker)
- Thin `codegen_setup.c` / `.h` with `psx_game_codegen_forward_if_built`
  (scaffold: `tools/new_project_layout/templates/codegen_setup.c.in`) —
  `main.cpp` always calls it; in a full build it is a no-op
- For netplay titles: `PSX_NETPLAY ON` **before** `include(runtime.cmake)`
  (`ENABLE_NETPLAY_IF_PRESENT` alone is too late), plus `-DPSX_NETPLAY=ON`
  in the release workflow configure step
- Thin `scripts/package_release.sh` wrapping `package_game_release.sh`
- Zip prefix / display name / disc hint in that wrapper
- Release notes / GitHub Release job naming

Release CI configures with `-DPSXRECOMP_REQUIRE_GAME_C=ON
-DPSXRECOMP_BIOS_STALE_FATAL=ON` after `check_generated.sh`, and asserts from
the configure log that the generated game C and both committed BIOS backends
(OpenBIOS, SCPH-1001) were linked.

## Windows code signing

Windows 11 **Smart App Control** (on by default on new installs) allows an
executable only if it carries a valid Authenticode signature or its exact
hash already has reputation. Every CI build is a new hash, so an unsigned
release is blocked outright, with no "run anyway".

`package_game_release.sh` therefore runs `tools/ci/sign_windows.sh` over the
staged tree before zipping. It signs the game exe and every DLL (the
`overlay_toolchain/` bundle is left alone) with SHA-256 and an RFC 3161
timestamp, then verifies each file.

| Secret / variable | Meaning |
|---|---|
| `WINDOWS_SIGN_PFX_BASE64` | the PKCS#12 certificate, base64 (`base64 -w0 cert.pfx`) |
| `WINDOWS_SIGN_PFX_PASSWORD` | its password (omit for a passwordless .pfx) |
| `WINDOWS_SIGN_TIMESTAMP_URL` | optional; default `http://timestamp.digicert.com` |
| `WINDOWS_SIGN_DESCRIPTION` | optional; text shown in the file's properties / UAC |

**No certificate = no signing, and the build still succeeds** (a notice is
printed). A certificate that is present but fails to sign is fatal. The
workflow template passes the two secrets to the *Package game zip* step;
forks without them package unsigned exactly as before.

Certificate choice: an OV certificate satisfies Smart App Control at once,
while SmartScreen reputation still builds over downloads. An EV certificate
or Azure Trusted Signing is trusted by both immediately. Trusted Signing
does not hand out a .pfx; wiring it means a different signing step, not
this script.

Because the shipped executable **is** the game, the signature now covers
what the player runs. The setup-host model could not offer that: the binary
players ran was built on their own machine after Generate and carried no
signature.

## Release checklist

See [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md#bundled-release-checklist).
