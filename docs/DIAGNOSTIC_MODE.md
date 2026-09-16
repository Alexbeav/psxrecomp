# Diagnostic mode

Every owned-input kit ships the game in **normal mode**. Normal mode is the
fast build: no debug server, no freeze heartbeat, no periodic snapshots.

A **diagnostic** product is built from the same generated sources, with
`PSX_DEBUG_TOOLS=ON`, the first time you ask for it (below); first-run setup
builds only the normal product, so setup takes half as long. Use it when the
game crashes, freezes, shows a black screen, or misbehaves, and you want to
send the port maintainers something they can act on.

## Where the two builds live

| Build | Directory | Contents |
| --- | --- | --- |
| Normal (default) | `build-release/` | the game executable, `bios/`, `mods/`, settings, saves |
| Diagnostic | `build-diagnostic/` | the same game with the TCP debug server, `psx_freeze_heartbeat.json`, `psx_freeze_dump_*.json`, `psx_last_run_report.json`, `psx_crash.txt` |

Both builds share your configuration and memory cards through the normal
build's `settings` and `saves` handling; the diagnostic build only adds report
files under its own directory.

## Switching

Any one of these turns diagnostic mode on for the next start:

1. Create an empty file named `diagnostic-mode.txt` next to the setup
   executable (the one at the top of the kit, for example `Azure_Dreams.exe`).
   This is the route for players who start the game from a launcher such as
   RetComM, where you cannot add arguments.
2. Start the setup executable with `--diagnostic`.
3. Set the environment variable `PSXRECOMP_DIAGNOSTIC=1`.

The setup executable prints which product it forwards to. Delete
`diagnostic-mode.txt` (or drop the flag) to return to normal mode.

The first time diagnostic mode is requested, `build-diagnostic/` has no
executable yet and the setup executable builds it before starting (a few
minutes on a fast machine, comparable to first-run setup on a slow one; on
Windows the build runs in a console window after the setup executable exits,
then the game starts in diagnostic mode). If that build fails, the setup
executable says so and starts the normal build; you can create it by hand
from a shell:

```bash
python psxrecomp/psxrecomp_cli.py rebuild --project-root . --config game.toml \
  --build-dir build-release --diagnostic-dir build-diagnostic --no-pgo
```

## Collecting results for a GitHub issue

1. Reproduce the problem in diagnostic mode, then quit the game (or let it crash).
2. Run the setup executable with `--collect-diagnostics`, or from a shell:

   ```bash
   python psxrecomp/psxrecomp_cli.py diagnostics --project-root .
   ```

3. It writes `diagnostics-<UTC date>.zip` next to the setup executable and
   prints the path. Attach that file to a new issue on the title's GitHub
   repository, with what you did, what you expected, and what happened.

The zip contains only these report files, taken from `build-release/`,
`build-diagnostic/` and `build/`:

- `psx_last_run_report.json`, `psx_crash.txt`, `psx_freeze_heartbeat.json`,
  `psx_freeze_dump_*.json`
- `psx_game_version.txt`, `psxrecomp_exe_name-*.txt`, `BUILDINFO.json`
- from the kit root: `framework_pins.txt`, `VERSION`,
  `project-manifest.toml`, and whether `diagnostic-mode.txt` was present
- `diagnostics-summary.json`, written by the collector

It never includes saves, memory cards, BIOS images, disc images, or settings.
Open the zip before attaching it if you want to check.

## For maintainers

- The CLI `rebuild` command builds the diagnostic product when it is given
  `--diagnostic-dir`; the setup host passes `build-diagnostic` on both the
  Windows deferred-helper route and the POSIX route.
- `--setup-selfcheck` reports `diagnostic_build_present` and
  `diagnostic_mode_requested`, so a kit's diagnostic readiness is scriptable.
- A diagnostic build failure never removes the normal product; the rebuild
  result carries `diagnostic_error` and the player-facing text above tells the
  player to rebuild.
- Regression coverage: `runtime/tests/test_cli_diagnostics.py` (rebuild builds
  both products; the collector's include and exclude lists) and
  `runtime/tests/test_codegen_host_bios_stems.py` (host contract strings).
