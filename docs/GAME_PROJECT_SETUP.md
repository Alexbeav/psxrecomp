# Bringing up a game project on PSXRecomp

This guide is for **title developers**: you keep game code and config in your
own repository, pin this framework and `recomp-ui` as **root-level submodules**,
and (optionally) ship a **setup-host** zip that users generate from a legal disc
locally — the same model as Bomberman Party Edition / Masters of Teräs Käsi.

Related docs:

| Doc | When |
|-----|------|
| [`BUILDING.md`](BUILDING.md) | Local build of the framework / BIOS runtime |
| [`LOCAL_CODEGEN_SDK.md`](LOCAL_CODEGEN_SDK.md) | CLI contract for Generate & rebuild |
| [`ci/README.md`](ci/README.md) | Shared CI scripts and composite actions |
| [`CONTRIBUTING.md`](../CONTRIBUTING.md) | Framework contribution rules |

---

## Recommended repository layout

Everything game-specific lives at the **root** of *your* title repo. Framework
and UI are submodules next to that code — not nested under each other.

The RetComM / Generate & rebuild CLI (`psxrecomp_cli.py`, `tools/prepare_disc.py`,
pack helpers) ships **inside** the `psxrecomp` submodule. There is no separate
`psxrecomp-sdk/` tree.

```text
YourGameRecomp/                 # your git repo
├── .gitmodules
├── CMakeLists.txt              # thin: psxrecomp_add_game_runtime(...)
├── game.toml                   # disc / recompiler / runtime config
├── VERSION                     # release pin (e.g. 0.1.0)
├── seeds/                      # function-start seeds for psxrecomp-game
├── codegen_setup.c / .h        # title PsxrecompCodegenHostConfig (+ apply hooks)
├── launcher_assets/            # boxart / fonts / brand (recomp-ui)
├── scripts/
│   └── package_setup_release.sh   # thin wrapper → package_setup_host.sh
├── .github/workflows/
│   └── release.yml             # from docs/ci/templates/setup-release.yml
├── psxrecomp/                  # submodule → framework + CLI + codegen host + CI tools
│   └── host/psxrecomp_codegen_host.*
├── recomp-ui/                  # submodule → launcher UI
├── generated/                  # local only — gitignore (not in CI setup zip)
└── prepared_disc/ / disc/      # local disc working tree — gitignore
```

Minimal `CMakeLists.txt` shape:

```cmake
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp")
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
psxrecomp_add_game_runtime(psx-runtime
  ENABLE_NETPLAY_IF_PRESENT
  WINDOW_TITLE "My Game Recompiled"
  GEN_MARKER "generated/SLUS_01234_dispatch.c"
  GEN_FULL_GLOB "generated/SLUS_01234_full_*.c"
  CODEGEN_SETUP_SOURCES codegen_setup.c
  DEFAULT_GAME_CONFIG_PATH "game.toml"
  LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
)
```
### Add the submodules

```bash
cd YourGameRecomp
git submodule add https://github.com/mstan/psxrecomp.git psxrecomp
git submodule add https://github.com/TechnicallyComputers/recomp-ui.git recomp-ui
git submodule update --init --recursive
```

Pin **exact commits** (do not track `main` floating). Bump deliberately when you
want a newer framework or UI.

Clone for contributors:

```bash
git clone --recurse-submodules https://github.com/you/YourGameRecomp.git
```

---

## Local development loop

1. **Legal disc** — place your dump where `game.toml` expects it (or pass
   `--disc` to the CLI / wizard). Never commit disc images or retail BIOS dumps.
2. **Build emitters** (once per machine / after recompiler changes):

   ```bash
   ./psxrecomp/tools/ci/build_emitters.sh
   ```

3. **Generate game (+ OpenBIOS) C** — either the setup wizard, or:

   ```bash
   python3 psxrecomp/psxrecomp_cli.py generate \
     --config game.toml --project-root . --disc /path/to/dump.cue
   ```

4. **Build the playable runtime**:

   ```bash
   cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build-release --target psx-runtime -j"$(nproc)"
   ```

Details: [`LOCAL_CODEGEN_SDK.md`](LOCAL_CODEGEN_SDK.md), [`BUILDING.md`](BUILDING.md).

---

## Setup-host releases (self-build zip)

CI ships a zip **without** `generated/` game C and **without** retail BIOS
dumps. The zip includes:

- Setup host exe (`recomp-ui` + generate/rebuild wizard)
- Your game sources (`game.toml`, seeds, CMake, host glue, …)
- `psxrecomp/` (runtime + CLI + OpenBIOS profiles + emitters)
- `recomp-ui/` sources (needed to rebuild)
- `toolchain/` — portable `cmake-clang-v1` from
  [retcomm-toolchains](https://github.com/TechnicallyComputers/retcomm-toolchains)
- On Windows: MinGW runtime DLLs beside the host and emitters

Players (or [RetComM](https://github.com/TechnicallyComputers/RetComM-Launcher))
run Generate & rebuild locally. After a successful rebuild the CLI can prune
`toolchain/` and build intermediates.

Do **not** set `PSX_PGO` in CI. PGO stays user-local when `[pgo] enabled = true`.

---

## Shared tools (use these; do not reimplement)

| Tool | Role |
|------|------|
| `tools/ci/normalize_version.sh` | `vX.Y.Z` → `VERSION` / `TAG` |
| `tools/ci/clear_generated.sh` | Wipe `generated/` for setup-host CI |
| `tools/ci/record_pins.sh` | Log submodule SHAs |
| `tools/ci/build_emitters.sh` | Build `psxrecomp-game` + `psxrecomp-bios` |
| `tools/fetch_toolchain.sh` | Download/unpack portable cmake/clang |
| `tools/stage_setup_sdk.sh` | Emitters + OpenBIOS + `toolchain/` + MinGW DLLs |
| `tools/bundle_mingw_dlls.sh` | Windows runtime DLL copy |
| `tools/package_setup_host.sh` | Full setup-host zip (title args) |

Composite actions (from the game repo after checkout):

```yaml
- uses: ./psxrecomp/.github/actions/build-emitters
- uses: ./psxrecomp/.github/actions/fetch-toolchain
  with:
    artifact: ${{ matrix.artifact }}
- uses: ./psxrecomp/.github/actions/stage-setup-sdk
  with:
    stage: dist/stage-setup-${{ matrix.artifact }}
    recompiler-build: build-recompiler
```

---

## CI workflow template

1. Copy the template into your title repo:

   ```bash
   mkdir -p .github/workflows scripts
   cp psxrecomp/docs/ci/templates/setup-release.yml .github/workflows/release.yml
   ```

2. Replace every `YOUR_*` placeholder.
3. Add a thin `scripts/package_setup_release.sh` that calls
   `psxrecomp/tools/package_setup_host.sh` with your exe name / zip prefix
   (see Bomberman Party Edition for an example).

Full action reference: [`ci/README.md`](ci/README.md).

---

## Bundled release checklist

Use this before tagging a setup-host release that matches other titles
(BPE / MotK / RetComM).

### Repository

- [ ] `psxrecomp/` and `recomp-ui/` are root-level submodules on pinned commits
- [ ] CLI lives in the submodule (`psxrecomp/psxrecomp_cli.py`) — no sibling sdk
- [ ] `game.toml` has disc identity (`[prepare_disc]` hashes/sizes) and boot EXE
- [ ] Seeds cover the boot path; `VERSION` matches the release you will tag
- [ ] Disc images, `generated/`, and `SCPH1001.BIN` are gitignored
- [ ] Setup-host CMake path builds with **no** game C and **no** BIOS backends
      (e.g. `-DPSXRECOMP_ALLOW_NO_BIOS=ON` + your title’s force-setup option)
- [ ] Thin `codegen_setup.c` + `psxrecomp_add_game_runtime` (codegen host is in
      `psxrecomp/host/`; CI may use `-DPSXRECOMP_FORCE_SETUP_HOST=ON`)
- [ ] Optional: start from `docs/ci/templates/game.gitignore`

### Packaging (shared helpers)

- [ ] CI uses `./psxrecomp/.github/actions/build-emitters`
- [ ] CI uses `./psxrecomp/.github/actions/fetch-toolchain`
- [ ] Packager calls `psxrecomp/tools/package_setup_host.sh` (or
      `stage_setup_sdk.sh` after a custom stage)
- [ ] Staged tree includes `psxrecomp/psxrecomp_cli.py`
- [ ] Staged `psxrecomp/bios/` has `OpenBIOS.toml`, `openbios.bin`,
      `OpenBIOS.LICENSE`, `SCPH1001.toml` — and **no** retail `.BIN`
- [ ] Windows zip has `libstdc++-6.dll` + `libgcc_s_seh-1.dll` next to both
      emitters (and host deps such as `zlib1.dll` next to the exe)
- [ ] Zip mtimes are normalized (packager `touch`) so Ninja does not see
      “future” files after extract

### Zip contents smoke test

- [ ] Linux / macOS / Windows artifacts named consistently
      (`YOUR_PREFIX-<ver>-<linux-x64|windows-x64|macos-arm64|macos-x64>.zip`)
- [ ] Extract → run host → Generate with a legal disc succeeds end-to-end
- [ ] After rebuild, game launches; saves/settings land beside the exe
- [ ] RetComM install/update (if you publish a catalog entry) uses this **same**
      zip — no separate tools pack required

### Publish

- [ ] Tag `vX.Y.Z`; GitHub Release attaches all four platform zips
- [ ] Release notes say the zip is a setup host (no disc / no retail BIOS /
      no pre-generated game C)
- [ ] Catalog / RetComM entry points at the release assets when ready
