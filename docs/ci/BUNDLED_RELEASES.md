# Bundled releases

Public CI builds and ships the **compiled game**. The title commits its
recompiled game C under `generated/`; CI links it, packages the executable with
its runtime data, and attaches one zip per platform. Players download, point
the game at their own disc, and play. Nothing is generated or compiled on a
player's machine.

This replaces the setup-host model (2026-09-30), under which CI wiped
`generated/`, shipped emitters plus sources plus a wizard, and every player
regenerated and rebuilt the title locally. That shipped uncompiled work, made
first-run depend on a toolchain download, and put every generate/build failure
on the player's machine.

## What the title commits

| Path | Status |
|------|--------|
| `generated/<boot>_dispatch.c`, `generated/<boot>_full_*.c`, `generated/<boot>_decls.h`, `.ranges` | **Committed.** `psxrecomp_cli.py generate` writes them; commit after every regenerate. Shards are ~1.4 MB each, far under GitHub's blob limit. |
| `generated/overlays_static*.c`, `AOT_STATIC_AUDIT.json` | **Committed** when the AOT profile declares `static_output` (`docs/AOT_SHARDING.md`). This is the overlay coverage a release links. |
| `psxrecomp/generated/{OpenBIOS,SCPH1001}_{full,dispatch}.c` + `.emitter.sha` | **Committed in the framework** (decision 2026-09-30: the recompiled BIOS carries the same risk as the recompiled game). A title just pins a psxrecomp that carries them. CI's `PSXRECOMP_BIOS_STALE_FATAL=ON` refuses a stamp that predates the emitter. |
| Disc images, BIOS dumps | Never. The zip carries only the OpenBIOS image; a retail backend still needs the player's own dump at run time. |

`.gitignore` must not carry `/generated/`; the scaffold (`gitignore.in`,
`docs/ci/templates/game.gitignore`) no longer writes it, and Project Studio's
`merge_gitignore` removes it from an existing file.

## What CI does

Template: [`templates/game-release.yml`](templates/game-release.yml).

1. `check_boot_exe.sh` — the three copies of the boot-EXE name agree, so the
   build looks for the C the checkout actually has.
2. `check_generated.sh` — `generated/<boot>_dispatch.c` and its shards exist
   **and are tracked**; no retail BIOS C; no tracked BIOS dump.
3. Build `psxrecomp-game` / `psxrecomp-bios` (or restore them from the Actions
   cache when the `psxrecomp` submodule SHA matches a prior run); they ship
   inside `overlay_toolchain/`.
4. Configure with `-DPSXRECOMP_REQUIRE_GAME_C=ON -DPSXRECOMP_BIOS_STALE_FATAL=ON`
   (a checkout without game C fails configure instead of quietly producing a
   setup host; a BIOS stamp older than the emitter fails it too), assert the
   configure log says `linking generated game C (full runtime)` and
   `BIOS backends linked: OpenBIOS;SCPH1001`, build `psx-runtime`.
5. `scripts/package_release.sh` → `tools/package_game_release.sh` — stage the
   executable, `assets/`, `bios/openbios.bin` + notice, `game.toml`,
   `game_options.toml`, the mod catalog (verified against the manifest the
   build published, developer-channel packages pruned), `overlay_toolchain/`,
   third-party notices (psxrecomp's in `licenses/`, plus recomp-ui's license
   as `licenses/recomp-ui-LICENSE` and its font/image notices as
   `assets/{fonts,img}/NOTICE.md`); refuse if anything kit-shaped is in the
   stage; sign on Windows; zip.
6. Verify the zip: executable + OpenBIOS + catalog present; no `psxrecomp/`,
   `recomp-ui/`, CLI, emitters at the root, sources, generated C, dumps,
   per-machine mod state.

`PSX_SETUP_WIZARD` stays on. In a full build it is the first-run disc picker;
it never offers Generate & rebuild because the binary already carries the game.

## Overlays

The overlay **shard cache** is compiled from the disc, so CI cannot build one.
A release relies on two things instead:

- the **static AOT shard** committed under `generated/` and linked into the
  executable (titles with an `aot/overlays.json` `static_output`);
- the bundled **`overlay_toolchain/`** (pinned relocatable Python +
  `psxrecomp-game` + `psxrecomp-bios` + headers + BIOS profiles and seeds, tcc
  on Windows), which lets the runtime compile any other overlay it streams
  from the player's disc, and a retail BIOS backend from the player's dump.

A developer packaging locally with a cache can ship it:
`PSX_OVERLAY_CACHE_ROOT=<cache root> scripts/package_release.sh …`. The packager
routes it through `tools/release_stage.py`, the only place that knows the cache
tag and layout.

## Faster host/UI bumps

`workflow_dispatch` input **`reuse_cached_emitters`** (default `true`):

| Situation | Emitters |
|-----------|----------|
| Cache miss (new `psxrecomp` SHA) | Always rebuild |
| Cache hit + reuse on | Skip rebuild; use cached binaries |
| Cache hit + reuse off | Rebuild anyway |

Linux/macOS game compiles use **ccache**, so a host/UI-only bump is close to a
link-only rebuild of the tens of MB of generated C.

## What players / Retro do

| Step | Meaning |
|------|---------|
| Install | Extract the zip. Run the game. Pick the disc on first run. |
| Update | Extract the new zip over the old one (raw zip extract: this is a prebuilt Play binary). Saves and settings live beside the exe and are preserved. |
| Retail BIOS | The SCPH-1001 backend is compiled in; a player who owns that dump picks it in the launcher and it hot-swaps (the image is validated, never shipped). Another known image is built into a backend on their machine, once (`docs/BIOS_SELECTION.md`). |

## Migrating a title that predates this

A title still on the setup-host shape (a `release.yml` that wipes `generated/`,
`scripts/package_setup_release.sh`, `generated/` ignored, a psxrecomp pin
without the BIOS backends) is moved in one pass by
`tools/migrate_bundled_release.py`, run from the framework checkout the title
should pin:

```sh
python3 psxrecomp/tools/migrate_bundled_release.py . --dry-run   # report only
python3 psxrecomp/tools/migrate_bundled_release.py .             # commit locally
python3 psxrecomp/tools/migrate_bundled_release.py . --push      # and push
```

In order: bump `psxrecomp` and `recomp-ui` to `--psxrecomp-ref` /
`--recomp-ui-ref` (default `origin/master`, refused if that ref lacks the
template or the committed BIOS backends), un-ignore `generated/`, ensure
`[runtime] overlay_cache = true`, write `scripts/package_release.sh` (retiring
the setup-host wrapper) and `release.yml` from the pinned template, rebuild the
emitters and regenerate the game C from the disc (`--disc`, else `disc.cfg`,
else the single `disc/*.cue`; `--skip-generate` defers it and the release gate
refuses until done), refresh `framework_pins.txt`, run `check_boot_exe.sh` and
`check_generated.sh`, commit. The working tree must be clean and the
submodules free of local changes. The same operation is Project Studio's
`git migrate-bundled` / `git bulk-migrate-bundled` and Retro Studio's
**Bulk Migrate** tab, which runs it on every ticked title.

## Title checklist (short)

- `generated/` committed and regenerated whenever seeds, `game.toml` codegen
  settings, or the framework pin change.
- `.gitignore` does not ignore `generated/`.
- `scripts/package_release.sh` (scaffold template `package_release.sh.in`).
- `.github/workflows/release.yml` from `templates/game-release.yml`
  (`tools/generate_ci` writes and checks it).
- `[runtime] overlay_cache = true` in `game.toml`.
- Release notes say the zip is the compiled game: bring your own disc, no
  BIOS, no build.
