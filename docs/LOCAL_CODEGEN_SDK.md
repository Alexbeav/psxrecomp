# Local codegen SDK

Headless contract for regenerating an existing psxrecomp game project from a
user-supplied disc. Intended for `recomp-ui` setup flows and Retro launcher
automation. This does **not** redistribute disc images.

PGO (optional) runs **only on the user’s machine** during local rebuild when
`game.toml` has `[pgo] enabled = true`. CI must not set `PSX_PGO`.

## Managed shared overlay compiler (Windows private thin products)

Workbench can put one verified Clang subset in a shared versioned store. The
product's `overlay_toolchain/compiler.txt` names its absolute `bin/clang.exe`.
The companion `compiler-store.sha256` contains 64 lowercase hexadecimal digits
and one LF. It is the SHA256 of the store's exact `SHA256SUMS.txt` bytes. The
store directory is named `overlay-clang-<hash>`; the manifest has sorted UTF-8
`<SHA256>  <relative-posix-path>\n` entries for all compiler files and notices.

Startup checks the manifest and every member, including supporting libraries,
before selecting the compiler. Missing or corrupt members stop startup and name
the failed path. Re-run Workbench with `--repair-shared-toolchain`, or rebuild as
portable. A host compiler or TCC cannot replace a failed managed store.

Startup holds a shared Windows usage guard at `<store>.lock` until process exit.
It opens with `GENERIC_READ`, `FILE_SHARE_READ`, and `OPEN_ALWAYS`. Workbench's
explicit repair must obtain an exclusive guard before quarantining a corrupt
store; a running game prevents repair. The guard is outside the hashed manifest.
Portable products omit `compiler-store.sha256` and keep their bundled compiler.
Legacy absolute compiler paths without the marker retain their existing behavior.

## Setup host (CI without game/BIOS generated C)

Games can ship a **setup host**: `psx-runtime` linked **without** game C and
**without** BIOS backends (`-DPSXRECOMP_FORCE_SETUP_HOST=ON`, or legacy
`-DBPE_FORCE_SETUP_HOST=ON`) via `psxrecomp_add_game_runtime(...)`, and with
`PSX_SETUP_WIZARD=ON` / `ENABLE_SETUP_WIZARD` so first-run Generate & rebuild
actually appears. CI never
needs BIOS dumps or private assets. First-run Generate emits OpenBIOS (from
bundled `openbios.bin`) and optional SCPH1001 (player dump), then game C, then
rebuild links everything into `build-release/` (or the title’s
`build_dir_name`).

**Layout after Generate & rebuild:** the zip-root exe stays the setup host;
the playable product (exe + `bios/` + `mods/` + `assets/` + `settings.toml`)
lives under `build-release/`. Reopening the zip-root exe **forwards** to that
product binary (`psxrecomp_codegen_host_forward_if_built`). Opt out with
`PSXRECOMP_NO_FORWARD=1` or the title’s `*_FORCE_SETUP=1`.

| Piece | Role |
|-------|------|
| Setup exe (zip root) | First-run wizard; after rebuild, forwards to `build-release/` |
| Product exe | `build-release/<exe>` — Play, bios/mods/assets/settings |
| Game zip `psxrecomp/` | submodule tree: CLI, tools, emitters, OpenBIOS profiles |
| `psxrecomp_cli.py` | Generate / rebuild / verify-disc (in the submodule) |
| `tools/package_setup_host.sh` | Universal setup-host zip packager |
| `tools/ci/*.sh` | normalize version, clear generated, record pins, build emitters |
| `tools/fetch_toolchain.sh` | Optional: download/unpack `cmake-clang-v1` (CI embed or local) |
| `tools/toolchain_pack.py` | CLI: resolve / download / unpack into `toolchain/` + shared cache |
| `tools/stage_setup_sdk.sh` | Pack: emitters, OpenBIOS checks, optional `toolchain/`, MinGW DLLs |
| `tools/bundle_mingw_dlls.sh` | Windows: copy compiler-matched MinGW runtime DLLs next to host + emitters; an explicit `--runtime-bin` takes priority over ambient shell runtimes |
| Project `toolchain/` | Stamp file `.psxrecomp-bin` pointing at the shared pack `bin/` |
| Shared toolchain cache | `%LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/` (Windows) or `~/.local/share/retcomm/toolchains/cmake-clang-v1/` — same tree Retro uses |
| `docs/ci/` | Composite actions + [`templates/setup-release.yml`](ci/templates/setup-release.yml) |
| `docs/GAME_PROJECT_SETUP.md` | Submodules, CI template usage, bundled-release checklist |
| Game sources | `game.toml`, seeds, `CMakeLists.txt` at repo root; `psxrecomp/`, `recomp-ui/` submodules |

Retro harvests emitters into the SDK cache and **downloads** `cmake-clang-v1`
(no separate tools zip; game zips stay lean). The setup host and CLI install the
portable pack into the **shared Retro cache**
(`%LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/<tag>/`, or XDG equivalent)
with host-native `curl` + `tar`/`unzip` when possible — so Microsoft Store
Python AppData redirection cannot hide cmake. Offline zips unpack to the same
path. A legacy `%LOCALAPPDATA%/psxrecomp/…` cache is migrated automatically.
Override with `RETCOMM_TOOLCHAIN_DIR`. Require a floor version via
`RETCOMM_TOOLCHAIN_MIN_VERSION` / `ensure-toolchain --min-version` (optional;
default is none — wizard/`ensure-toolchain` fetch GitHub `/releases/latest`).
Generate/rebuild still need Python 3
for `psxrecomp_cli.py`.

## Commands

```bash
python psxrecomp/psxrecomp_cli.py verify-disc \
  --config game.toml --disc path/to/dump.bin [--json-progress]

python psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . --disc path/to/dump.iso \
  [--bios path/to/SCPH1001.BIN] [--force-bios] \
  [--skip-hash-check] [--force-prepare] [--json-progress]

python psxrecomp/psxrecomp_cli.py ensure-toolchain \
  [--project-root .] [--from-zip cmake-clang-v1-linux-x64.zip] [--no-download]

python psxrecomp/psxrecomp_cli.py rebuild \
  --config game.toml --project-root . \
  --build-dir build-release --target psx-runtime \
  --exe-basename Bomberman_Party_Edition_Recompiled \
  [--disc path/to/game.cue] [--toolchain-zip path/to/pack.zip] \
  [--no-toolchain-download] [--no-pgo] [--force-pgo] [--json-progress]

python psxrecomp/psxrecomp_cli.py pgo-train \
  --config game.toml --build-dir build-release \
  --exe-basename Bomberman_Party_Edition_Recompiled \
  [--disc …] [--train-secs 120] [--train-runs 3] [--json-progress]
```

Disc verification and preparation also check and retain user-supplied
[SBI companions](DISC_COMPANIONS.md). Main-track hashes alone do not prove
complete subchannel input.

`generate` normalizes the dump via `tools/prepare_disc.py` when needed, then
runs `psxrecomp-game --config game.toml` into `[recompiler] out_dir`.

`rebuild` runs cmake. If `[pgo] enabled = true` (and `--no-pgo` not set), it
instruments, trains (`scenario` / timed boot), then rebuilds with profiles.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | success |
| 1 | runtime / generation / build failure |
| 2 | usage / argument error |
| 3 | disc verification failure |

## JSONL progress (`--json-progress`)

Stdout is reserved for one JSON object per line. Useful events:

| `event` | Notes |
|---------|--------|
| `phase` | `verify`, `prepare_disc`, `emit`, `build`, `pgo_*`, `done` |
| `disc` | digests after verification |
| `log` | mirrored tool chatter |
| `result` | final payload |
| `error` | `message`, `code` |

## Portable recomp-ui host (`psxrecomp/host/`)

Prefer `psxrecomp_add_game_runtime(...)` — it compiles
`psxrecomp/host/psxrecomp_codegen_host.c` and sets `PSX_HAS_GAME_CODEGEN`.

Titles keep a thin root `codegen_setup.c` that fills
`PsxrecompCodegenHostConfig` and exposes apply/relaunch hooks:

```c
psxrecomp_codegen_host_apply(&gi, &my_cfg);

/* after recomp_launcher_run_window: */
if (lr == RECOMP_LAUNCHER_RESULT_RELAUNCH)
    psxrecomp_codegen_host_relaunch_or_exit(disc_path);
```

### Modular `[pgo]` (game.toml) — opt-in

PGO runs only when the title sets `enabled = true`. Framework defaults when
enabled: **60s × 2 runs**, `mute_host_audio = true`, `hide_video = true`.
Games may lengthen trains in their own `game.toml`.

```toml
[pgo]
enabled = true                 # required to opt in
train_secs = 60                # optional override
train_runs = 2                 # optional override
mute_host_audio = true         # default: discard SDL output (SPU still runs)
hide_video = true              # default: --headless (no on-screen FMV)
scenario = "boot_timed"        # title-authored workload hint
```

Omit the section or set `enabled = false` to skip. During train the CLI/UI show
a **WARNING** — do not cancel the process (or close a visible window if
`hide_video = false`).

`hide_video` keeps guest MDEC/FMV decode in the profile but shows nothing on
screen (avoids abrasive/flickering FMV). Use `--pgo-video` only when you need
host present paths in the profile.

### Env overrides

| Env | Role |
|-----|------|
| `PSXRECOMP_PROJECT_ROOT` / game-specific | project root |
| `PSXRECOMP_BUILD_DIR` / game-specific | cmake build dir |
| `PSXRECOMP_FORCE_SETUP` / game-specific | force setup wizard |

Project-root discovery order: env → walk **cwd** → walk **exe dir**
(`$APPIMAGE` parent or `/proc/self/exe` / `GetModuleFileName`). GUI launches
that leave cwd as `$HOME` still find a setup zip next to the binary.
| `PSXRECOMP_GAME` | path to `psxrecomp-game` binary |
| `PSX_HOST_MUTE=1` | discard host SDL audio (SPU still runs; set by PGO train) |
| `PSX_HEADLESS=1` | no SDL window (set by PGO train when `hide_video`) |
| `SDL_VIDEODRIVER=dummy` | set by default with headless train |
| `PYTHON` / `CMAKE` | tool overrides |

### Toolchain update prompt (wizard)

The first-run / setup wizard compares the local cmake-clang pack version against
GitHub `/releases/latest` and, when newer, prompts **Update** or **Skip for now**.

- `ensure_toolchain_with_progress(..., download == 2)` forces a re-download from
  latest (update path); `download == 1` fetches only if missing; `0` is cache-only.
- Set `RETCOMM_TOOLCHAIN_SKIP_UPDATE=1` to disable the remote newer-than-local check.

### A toolchain that fails its check (wizard open)

`toolchain_is_ready` does not stop at `cmake --version`. It also smoke-tests
`clang` + `ld.lld` (tiny link). If that fails (missing `libicuuc.so.*`, bad
`latest/` pointer, etc.), the host passes that pack over for the rest of the
process, clears `toolchain/.psxrecomp-bin`, and re-opens wizard page 0 with a
repair note so the player can redownload.

The pack that failed is **not removed and not renamed**. One failed check is not
proof that a pack is broken (the check can fail for a reason outside the pack),
and the pack may be in use by a build that is running. Only a `latest` link that
points at nothing is removed, and only the link.

The check does not depend on the length of `PATH`. Until PS1B-410 the compile
step expanded `%PATH%` inside a `cmd` line, which holds 8,191 characters: with a
`PATH` over about 7,600 characters a whole pack was judged unusable. The child
now gets its `PATH` through the process environment. Activating a pack puts its
folders at the head of `PATH` once and takes their other copies out, so a host
that starts itself again does not grow `PATH`. At the limit of one variable
(32,767 characters) Windows cannot start a child at all: the pack is then
reported not ready, and nothing is removed.

### The order of an install (setup host and `ensure-toolchain`)

The setup host (`host/psxrecomp_codegen_host.c`) and the CLI
(`tools/toolchain_pack.py`) install a pack in the same order:

1. The zip is unpacked into a staging folder beside the installed packs.
2. The new pack is checked there. A pack that fails is dropped; nothing that
   was installed has been touched.
3. The installed folder of that tag, if there is one, is renamed aside
   (`.old-<tag>-<pid>`). On Windows a folder with an open file cannot be
   renamed: a build is using the pack, so it stays whole, the new pack is
   dropped, and the player is told to close the build.
4. The new pack takes the tag's name, `latest` follows, the project stamp is
   written.
5. Only then the folder that was set aside and the older tags are removed.
   What cannot be removed whole stays under its dot-name, which no lookup
   reads, and is removed on a later pass.

Before this order (PS1B-410) an install over the installed tag removed that
folder first. With builds running from it, the files they held open survived
and the rest went.

### `PSXRECOMP_TOOLCHAIN_READONLY=1`

With this variable set, the setup host and the CLI never delete, rename, prune
or install a pack, never move a pointer to one (`latest`, the project's
`toolchain/` link and stamp), and the CLI does not write the user's login PATH.
A pack that is there is still found and used. Each step that was left undone is
named on stderr. A test or a gate that starts a setup program or the CLI sets
it, and gives the program its own toolchain and data folders as well.
