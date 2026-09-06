# Host-only / setup-host releases

Public CI builds a **setup-host** zip: wizard + game sources + emitters +
framework pins. It never ships retail discs, BIOS dumps, or `generated/` game C.

## What CI does

1. `clear_generated.sh` — empty `generated/` so CMake takes the setup-host path.
2. Build `psxrecomp-game` / `psxrecomp-bios` (or restore them from Actions cache
   when the framework, runner, architecture, deployment target, and recipe match).
3. Configure with `-DPSXRECOMP_FORCE_SETUP_HOST=ON` + wizard flags; build the
   host binary only.
4. `package_setup_host.sh` — lean zip (no embedded toolchain by default).
   Requires `psx_game_version.txt` beside the host exe (stamped at compile
   time). Refuses to ship if `RELEASE_VERSION` / `VERSION` disagree with that
   stamp — a mismatched pin makes netplay lobby browsers filter each other out.

Template: [`templates/setup-release.yml`](templates/setup-release.yml).

### Faster host/UI bumps

The workflow reuses emitters when the complete cache key matches.
A changed framework, runner, architecture, deployment target, or recipe rebuilds them.
The native ZIP gate still executes restored generators before upload.
The host compiler uses ccache when available.
Windows emitters use the portable llvm-mingw fetch; the host compile stays on MSYS2 g++.

The workflow requires a version already frozen in source and creates artifacts only.
It does not publish releases or respond to tag pushes.
The [CI guide](README.md) describes the required source and native archive gates.

Bump the `psxrecomp` gitlink when emitter/CLI behavior must change; leave it
pinned for pure host/UI/`recomp-ui` releases so CI stays cmake-time.

## What players / RetComM do

| Step | When |
|------|------|
| **Generate** (wizard or RetComM Build & Install) | First install, or after ROM/BIOS/emitter fingerprint changes |
| **Update** (RetComM) | New setup-host zip → refresh source → cmake rebuild; **codegen-cache** skips regenerate when fingerprints match |
| **Generate & Rebuild** | Force disc→C again (`force_generate`) |

Players do **not** need Generate & Rebuild for ordinary host/UI updates after the
first successful generate. RetComM keeps `apps/<title>/codegen-cache/` keyed by
ROM/BIOS + emitter fingerprints (see RetComM `docs/BUILD_PACKS.md`).

Raw zip extract is for true prebuilt Play binaries only. Setup-host catalog
entries use local generate+cmake (`install_title_auto` / `update_title_auto`).

## Title checklist (short)

- Ship wizard (`PSX_SETUP_WIZARD` / `ENABLE_SETUP_WIZARD`).
- Thin `codegen_setup` forward for setup-host link.
- Catalog: `build.enabled` + release asset globs pointing at the setup zip.
- Document that first-run Generate is expected; later Updates are host rebuilds.
