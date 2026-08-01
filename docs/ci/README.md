# CI helpers for setup-host releases

Shared scripts and GitHub Actions so every PSXRecomp title uses one
standardized release flow.

**Start here:** [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md)

## Workflow template

```bash
mkdir -p .github/workflows
cp psxrecomp/docs/ci/templates/setup-release.yml .github/workflows/release.yml
# edit YOUR_* placeholders
```

Template: [`templates/setup-release.yml`](templates/setup-release.yml)

## Tools under `psxrecomp/tools/`

| Script | Role |
|--------|------|
| `ci/normalize_version.sh` | Normalize / write `VERSION` + `TAG` |
| `ci/clear_generated.sh` | Clear `generated/` for setup-host CI |
| `ci/record_pins.sh` | Log `psxrecomp` / `recomp-ui` / `recomp-net` SHAs |
| `ci/build_emitters.sh` | Build `psxrecomp-game` + `psxrecomp-bios` |
| `fetch_toolchain.sh` | Download/unpack cmake-clang-v1 (Windows emitter builds; optional embed) |
| `stage_setup_sdk.sh` | Emitters, OpenBIOS, optional `toolchain/`, MinGW DLLs |
| `bundle_mingw_dlls.sh` | Copy imported non-system DLLs next to Windows PEs |
| `package_setup_host.sh` | Lean setup-host zip (optional `--embed-toolchain`) |
| `../cmake/toolchain-mingw-w64.cmake` | Linux→Windows MinGW cross toolchain |
| `../host/psxrecomp_codegen_host.*` | Portable Generate & rebuild host (via CMake helper) |
| `templates/game.gitignore` | Suggested gitignore for title repos |

Also use `psxrecomp_add_game_runtime(...)` in `runtime/runtime.cmake` for
setup-host / full-game wiring. The CLI (`psxrecomp_cli.py`) lives in this
submodule — there is no separate `psxrecomp-sdk/` overlay.

## Composite actions

```yaml
- uses: ./psxrecomp/.github/actions/build-emitters

# Prefer package_setup_host.sh (allow-no-toolchain by default).
# fetch-toolchain + --embed-toolchain only for offline-first packs.
- uses: ./psxrecomp/.github/actions/stage-setup-sdk
  with:
    stage: dist/stage-setup-${{ matrix.artifact }}
    recompiler-build: build-recompiler
    allow-no-toolchain: 'true'
    runtime-bin: /mingw64/bin   # Windows / MSYS2
```

## Title responsibilities

Keep only this in the game repo:

- Setup-host CMake flags and exe basename
- Thin `scripts/package_setup_release.sh` wrapping `package_setup_host.sh`
- Zip prefix / display name / disc hint in that wrapper
- Release notes / GitHub Release job naming

## Release checklist

See [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md#bundled-release-checklist).
