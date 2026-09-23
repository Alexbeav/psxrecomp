# Corrections to the record (2026-09-22)

## Withdrawn: "fleet-wide" for the play.bat quoting defect

I wrote that the argument-splitting defect was "worth sweeping fleet-wide — any
spaced disc path would hit this." **Withdrawn.** A fleet check does not support it.

Evidence gathered:

- 16 `play.bat` / `PLAY.bat` files found across `<projects>\PSX-Ports`,
  `L:\AgentData`, `<projects>\PSX-Ports-pilot`.
- 7 use the fragile `start` form; 0 use the corrective `call` form; 9 use neither.
- **All 7 `start`-form launchers pass only the executable plus bare flags** — no
  path arguments at all:

```
start "" "%~dp0Wave3_colony_wars.exe"
start "" "Wave3_resident_evil_3.exe"
start "" "Wave3_vib_ribbon.exe" --no-launcher
start "" "Wave3_vib_ribbon.exe" --no-launcher --debug-port 4398
start "" "Wave3_vib_ribbon.exe"
start "" "Wave3_wipeout_xl.exe"
start "" PSXRecomp.exe
```

The defect requires a spaced path to be passed **as an argument**. None of these
do that; their disc path comes from `settings.toml` / `disc.cfg`. So the defect
cannot trigger in any of the 7.

**What remains true (narrow):** the defect is real and I reproduced it on the SF2
candidate, where `--disc "<spaced path>"` is passed. It is a defect in that
launcher and in any launcher that passes a spaced path as an argument. It is
**not** established as fleet-wide.

## Preserved separately: Alex's observed gameplay vs. checks I never performed

These must not be conflated. I did not verify any of the right-hand column.

### Observed by Alex (operator)

- The original SF2 build **plays** — mouse-look works in game.
- F1 and F7 menus were **missing** in the original build.
- The widescreen candidate is launchable.
- The integration build shows a **black screen**.

### Verified by compilation / link / process inspection only

- Both builds compile and link (configure EXIT=0, build EXIT=0).
- `host_keymap_*`, `psx_mouse_camera_*`, `psx_rewind_*` all present in the
  integration binary's symbol table.
- The integration process is alive, responding, ~37% CPU, 17 threads, correctly
  titled — i.e. executing, not deadlocked.
- `bios_boot=HLE (shell skipped)` appears with `fast_boot = true`, and flips to
  `bios_boot=LLE (real intro)` with `fast_boot = false`.

### Never verified by me on any build

- That the integration build renders anything.
- That guest execution advances past `0xBFC00000` in **either** the integration
  build or the fast-boot trial. My earlier "verified fast-boot" claim is
  **withdrawn**: I checked for the boot-mode string and stopped. Re-reading the
  log, my run stopped at the same line as Alex's.
- F1 menu opens. F7 menu opens. Mouse-look behaves. Widescreen widens.
- Controllers, memory cards, disc changes, rewind.
- Any optional feature tested off and on.
- Distribution through clean SETUP.

## Corrected: `fast_boot` is not deprecated-as-broken

I earlier told Alex `fast_boot` was DEPRECATED and recommended
`PSX_BIOS_HLE_KEEP_INTRO` instead. The comment means deprecated *as a
snapshot-restore mechanism*; it now aliases the boot-skip axis and works. In
`bios_hle_plan.c`:

```c
const int want_boot = (want_call && !req.keep_intro) || (req.fast_boot != 0);
```

`fast_boot` is an independent `||` term. Alex's builds use it and it is correct.
