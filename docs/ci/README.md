# CI helpers for setup-host releases

Shared scripts and GitHub Actions so new PSXRecomp titles do not reimplement
toolchain fetch, emitter staging, or Windows MinGW DLL bundling.

**Start here for a new game repo:**
[`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md)

## Workflow template

Copy into the **game** repository (not into this framework alone):

```bash
mkdir -p .github/workflows
cp psxrecomp/docs/ci/templates/setup-release.yml .github/workflows/release.yml
# edit YOUR_* placeholders
```

Template path: [`templates/setup-release.yml`](templates/setup-release.yml)

A shorter stub also lives at [`setup-release.yml.example`](setup-release.yml.example)
(points at the full template).

## Tools

| Script | Role |
|--------|------|
| `tools/fetch_toolchain.sh` | Download/unpack `cmake-clang-v1` from retcomm-toolchains |
| `tools/stage_setup_sdk.sh` | Stage emitters, OpenBIOS checks, `toolchain/`, MinGW DLLs |
| `tools/bundle_mingw_dlls.sh` | Copy imported non-system DLLs next to Windows PEs |

## Composite actions

Use from a game repo after `actions/checkout` with `submodules: recursive`:

```yaml
- uses: ./psxrecomp/.github/actions/fetch-toolchain
  with:
    artifact: ${{ matrix.artifact }}   # linux-x64 | windows-x64 | macos-arm64 | macos-x64

- uses: ./psxrecomp/.github/actions/stage-setup-sdk
  with:
    stage: dist/stage-setup-${{ matrix.artifact }}
    sdk-overlay: psxrecomp-sdk
    recompiler-build: build-recompiler
    runtime-bin: /mingw64/bin          # Windows / MSYS2 only
```

`fetch-toolchain` exports `TOOLCHAIN_DIR`, `PSXRECOMP_TOOLCHAIN_DIR`, and
`BPE_TOOLCHAIN_DIR` via `GITHUB_ENV` for later steps.

## Title responsibilities

Keep in the game repo:

- Setup-host CMake flags and exe basename
- Copying game sources / assets into the stage
- Zip naming and release notes
- Thin `scripts/package_setup_release.sh` that stages title files then calls
  `psxrecomp/tools/stage_setup_sdk.sh`

## Release checklist

See the **Bundled release checklist** in
[`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md#bundled-release-checklist).
