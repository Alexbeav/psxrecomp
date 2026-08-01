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

```text
YourGameRecomp/                 # your git repo
├── .gitmodules
├── CMakeLists.txt              # game target + setup-host options
├── game.toml                   # disc / recompiler / runtime config
├── VERSION                     # release pin (e.g. 0.1.0)
├── seeds/                      # function-start seeds for psxrecomp-game
├── host/                       # optional: codegen host glue for the wizard
├── codegen_setup.c / .h        # optional: PSX_HAS_GAME_CODEGEN wiring
├── launcher_assets/            # boxart / fonts / brand (recomp-ui)
├── scripts/
│   └── package_setup_release.sh   # title packager (thin; calls psxrecomp tools)
├── .github/workflows/
│   └── release.yml             # copied from docs/ci/templates/setup-release.yml
├── psxrecomp/                  # submodule → mstan/psxrecomp (pinned commit)
├── recomp-ui/                  # submodule → TechnicallyComputers/recomp-ui
├── psxrecomp-sdk/              # optional: CLI / prepare_disc overlay for zips
├── generated/                  # local only — gitignore (not in CI setup zip)
└── prepared_disc/ / disc/      # local disc working tree — gitignore
```

### Add the submodules

```bash
cd YourGameRecomp
git submodule add https://github.com/mstan/psxrecomp.git psxrecomp
git submodule add https://github.com/TechnicallyComputers/recomp-ui.git recomp-ui
# optional SDK surface used by RetComM / setup zips:
# git submodule add <your-sdk-url> psxrecomp-sdk

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
   cmake -S psxrecomp/recompiler -B build-recompiler -G Ninja \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build-recompiler --target psxrecomp-game psxrecomp-bios -j"$(nproc)"
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

## CI workflow template

1. Copy the template into your title repo:

   ```bash
   mkdir -p .github/workflows
   cp psxrecomp/docs/ci/templates/setup-release.yml .github/workflows/release.yml
   ```

2. Replace every `YOUR_*` placeholder (exe name, zip prefix, cmake setup flag,
   release title, package script path).
3. Keep using the shared actions — do not reimplement toolchain fetch or DLL
   bundling:

   ```yaml
   - uses: ./psxrecomp/.github/actions/fetch-toolchain
     with:
       artifact: ${{ matrix.artifact }}
   ```

4. Your `scripts/package_setup_release.sh` should:
   - Stage the host exe, assets, and game sources
   - Copy filtered `psxrecomp/` + `recomp-ui/`
   - Call `psxrecomp/tools/stage_setup_sdk.sh` (emitters, OpenBIOS checks,
     `toolchain/`, MinGW DLLs)

Full action reference: [`ci/README.md`](ci/README.md).

---

## Bundled release checklist

Use this before tagging a setup-host release that matches other titles
(BPE / MotK / RetComM).

### Repository

- [ ] `psxrecomp/` and `recomp-ui/` are root-level submodules on pinned commits
- [ ] `game.toml` has disc identity (`[prepare_disc]` hashes/sizes) and boot EXE
- [ ] Seeds cover the boot path; `VERSION` matches the release you will tag
- [ ] Disc images, `generated/`, and `SCPH1001.BIN` are gitignored
- [ ] Setup-host CMake path builds with **no** game C and **no** BIOS backends
      (e.g. `-DPSXRECOMP_ALLOW_NO_BIOS=ON` + your title’s force-setup option)
- [ ] Codegen host / `PSX_HAS_GAME_CODEGEN` wired so the wizard can regenerate

### Packaging (shared helpers)

- [ ] CI builds `psxrecomp-game` and `psxrecomp-bios` into `build-recompiler/`
- [ ] CI uses `./psxrecomp/.github/actions/fetch-toolchain` for the matrix OS
- [ ] Packager calls `psxrecomp/tools/stage_setup_sdk.sh` with
      `--recompiler-build`, `--toolchain-dir` (or env from fetch), and on
      Windows `--runtime-bin /mingw64/bin` + `--host-exe`
- [ ] Staged tree includes `psxrecomp/psxrecomp_cli.py` (sdk overlay if needed)
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
