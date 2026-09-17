# Slipped-fix port notes (2026-09-17)

Branch `fix/slip-ports-20260917` starts at `23ac54f8b` (wave5/framework-fixes). It collects fixes that were
validated on a lab, release or main branch but never reached the framework line. The audit that found them
(all fork refs, SF3-Recomp-Lab, the wave-3 framework copy) is in
`PSX-Ports/_runs/shared/wave5/slip_audit/` (`SUMMARY.md` and the `triage_*` reports).

**State of this branch:** code reading, syntax-only compiles (`gcc -fsyntax-only` for every touched C/C++
file), and the focused tests named below. Nothing was built into a product or run against retail media. Each
item lists the retail check it still needs. This branch has not been pushed.

## Ported

### 1. GL 15/24-bit VRAM ownership handoff (`bf4da729e`)
- **Sources:** SF2/SF3 lab `09be64bf9`; wave-3 Phantom Menace correction `60a4f4d94`. Only the lazy entry sync
  `1f5f70579` had landed.
- **Symptoms:**
  - Parasite Eve title screen: stale FMV rows flicker above and below the title (reproduced on Eagle;
    evidence in `_runs/shared/wave5/pe_gl_title_bands/`).
  - Phantom Menace: a GP0 copy made right after FMV→15-bit is wiped.
  - The first 24-bit upload is overwritten by the entry readback.
- **Change:**
  - `gp1_display_mode` and `gpu_reset_state` call a new optional backend hook, `gr_display_mode_changed`; GL runs
    `depth24_upload_policy` there.
  - `glb_vram_write` / `glb_vram_transfer_in` run the policy before writing.
  - `glb_fill_rect` also writes the CPU mirror while 24-bit scanout is active.
- **Test:** `depth24_transition_test` lifts the production bodies into a fake-VRAM fixture. Each half fails it when
  reverted, and `23ac54f8b` fails it.
- **Retail check owed:**
  - PE: skip the sizzle FMV about 10 s in; every `present_shot` of the title has black bands. Driver:
    `pe_title_bands.py <build> fix 10`.
  - Phantom Menace: the post-FMV screen that motivated `60a4f4d94`.
  - One FMV-heavy title with a letterboxed movie (MotK or WipEout 3): no regressed bars.
  - GT2 boot menu atlas after its intro FMV (the queued-upload ordering).
- **Vulkan:** the hook is not wired there. VK's movie-band clear is a no-op and its fill already writes the CPU
  mirror, so the Phantom Menace mechanism does not apply. Unverified observation: VK's policy sets
  `s_up_nrects = 0` on entry and exit, which drops queued texture uploads (the GT2 atlas case GL fixed in
  `f4b1cc58d`). Worth a VK check; not changed here.

### 2. IRQ delivery inside nested native call units (`c93bfd6b4`)
- **Source:** SF2 lab `dc873fc3d`.
- **Symptom:** a native callee waiting for an IRQ-backed BIOS event never returns, because both overlay CI
  wrappers returned while `g_call_unit_depth > 0`. SF2's Save and Quit hung with `I_STAT=0x41` pending.
- **Change:** remove the two early returns. Thread-switch atomicity stays with `interrupts.c` (`at_outermost` /
  `s_defer_switch_pending`), which did not exist when the guards were added on 07-07.
- **Test:** `overlay_call_unit_irq_guards`.
- **Retail check owed (risk: this touches the Ape memory-card fix):**
  - SF2 Save and Quit;
  - Ape Escape "Checking MEMORY CARD";
  - one overlay-heavy title's save and load.

### 3. Deferred in-exception thread switch from interpreted spin loops (`f4b75f93f`)
- **Source:** fork main `872c83106` (MGS PAL T32).
- **Symptom:** SLES-01370 black screen near frame 1106. The parked debug-console task spins in one local-flow
  run, so its deferred switch is never honored.
- **Change:**
  - `psx_defer_switch_pending()` accessor;
  - `dirty_ram_pump_boundary` site 1 surfaces while a switch is pending;
  - the interpreter entry poll is forced while one is pending.
  - Not ported: the `g_dirty_safe_resume_pc` publish (see commit message).
- **Test:** `deferred_switch_interp_poll` (source guard).
- **Retail check owed:**
  - Headless SCPH5552 + SLES-01370 past frame 1106, with `thread_trace` kind 32 following kind 31.
  - NTSC MGS boot as control.

### 4. Audio-only disc data reads fail with 40h (`346136a94`)
- **Source:** wave-3 Vib-Ribbon correction `6ceb1aff6`.
- **Change:** ReadN/ReadS without Setmode bit 0 on an all-audio disc return stat|error and 40h (PSX-SPX).
- **Test:** `cdrom_audio_disc_read_test` compiles the production cases against a synthetic drive. The fixture now
  reads `cdrom.c` as UTF-8, because it crashed under a Greek Windows code page.
- **Retail check owed:** Vib-Ribbon with a music CD swapped in; one mixed-mode title's normal boot.

### 5. Savestate load preflight (`058b0c97f`)
- **Source:** fork main `240338f93`.
- **Symptom:** a state with an unresumable PC was applied first and rejected afterwards, corrupting the live
  session.
- **Change:** `boot_state_peek_cpu_pc[_buffer]`; `savestate_poll` checks `savestate_resume_pc_ok` before loading.
  Main's scheduler-boundary admission was not ported (upstream's `snapshot_safe` is on the target).
- **Test:** `savestate_load_preflight` (source guard).
- **Retail check owed:** loading a state with a BIOS/exception PC is rejected and play continues; a normal state
  still loads.

### 6. OpenGL swap self-heal (`96512cd02`)
- **Source:** SF3 lab `4f9aab6d`.
- **Symptom:** a wedged Windows/NVIDIA swap queue blocks each present about 1.5 s; audio plays while the game looks
  frozen. Only the SDL_Renderer path recovered.
- **Change:** `gl_swap_with_osd` times the swap, and after three swaps over 250 ms with vsync armed it drops driver
  vsync for the session. The swap-interval setter respects it.
- **Test:** `gl_swap_self_heal` (source guard; also asserts every GL swap goes through `gl_swap_with_osd`).
- **Retail check owed:** none practical; a vsync-on GL session should show no change.

### 7. BIOS emitter stamp from the generate path (`20fb68e52`)
Cherry-pick of `14ae33865`. Removes the false "BIOS generated/ is STALE" warning on every CLI/wizard install.
Check: a kit's Generate writes `generated/<stem>.emitter.sha`, and configure is clean afterwards.

### 8. `embed_spirv.py` in the CLI package (`32b337dd4`)
From `c88eeecf6`. Packaged frameworks with Vulkan on (the default) need it. `cli_project_packaging` requires it
(passes).

### 9. PAL speed readout (`35cda498f`)
Speed is computed against 50 Hz when GP1(08h) selects PAL, matching the pacer. A full-speed PAL title no longer
reads 0.83x.

## Not ported: decision or larger work needed

| Item | Source | Why not here | Suggested next step |
|---|---|---|---|
| Colony Wars 24-bit row-coverage policy | `ee6f9f2a8` | Its own doc calls it "a presentation policy, not a hardware invariant": it blacks out VRAM rows that real hardware would show. Accuracy comes first. | Make it a per-title opt-in (as NCII keeps its own `d8d3664d9` on the NCII branch), or drop it. Re-check Colony Wars once item 1 lands; the PE-style fill mirror may already fix part of what it hid. |
| Stage OpenBIOS only when linked | `decc42daa` | Changes what the setup host stages; that path deadlocked in PR #27. | Port with a setup-kit run: a retail-only kit configures without `openbios.bin`, and a setup host (no linked backends) still generates OpenBIOS. |
| Wave-3 release gates and `recomp-ui/test_data` scrub | wave3-framework `d116029d`, `51a7cdb3`, `05c12ad9`; fork `7580d560f` x3 | CI/workflow rewrite; kits may ship fixtures containing workstation paths. | Port the `test_data` scrub in `package_setup_host.sh` first (smallest, user-facing). Then wire `tools/ci/audit_setup_package_platform_copy.py` into the release workflow. |
| G-Police path-keyed disc roster | `30448275a` (partial) | Multi-disc launcher: per-disc serials and netplay fps are still keyed by filename stem. New header plus `main.cpp` changes. | Port `disc_roster.h` with the launcher multi-disc test. |
| `BUILD_TESTING=ON` broken | `57a743f03` | SIO fixtures lack a `debug_server_update_poll` stub; `sio_card_repair_test` still uses the removed `s_ape_unstick*`. CI never enables `BUILD_TESTING`. | Fix the fixtures, then turn `BUILD_TESTING` on in one CI job, or every guard added on this branch stays unexecuted in CI. |
| Cycle fast-limit not cleared on restore | SF3 `e9db3dbe` (UNSURE) | `psx_cycles.c:576` `resync_after_restore` leaves `g_psx_cycle_fast_limit`. | Check whether the fast path re-derives the limit after restore; port the clear if not. |
| Resident control-flow-patch overlay quarantine | SF2 `17e9bbaa0`, SF3 `bb1adf37` (UNSURE) | May be covered by the exact-range guard (`memory.c:601-640`). | Compile the SF2 5-word fixture against the target's tools/loader. |
| Recompiler top-level try/catch, reproducible MinGW links, `PYTHONUTF8`, `project-manifest.toml` in kits | `c9a6bf516`, `6b7423147`, `b5750bd13`, `1a4fe1c17`, `620c7a97c` | Packaging hygiene, low risk, not urgent. | Batch together. |
| Diagnostics: fatal-dump DMA/MDEC/IRQ rings, `PSX_AOT_BLOCK`, crash serializer bounds, capture-history union load, depth24 upload telemetry | `5706efe3a`, `33d795faa`, `588a5000`, `f8bd4ff8`, `13aee712` | Tooling only. | Port the crash-report bounds (`overlay_loader.c:4609/4630/4775`) before the rest; it is a buffer-edge risk. |
| 26 SCPH5552 BIOS seeds + `filter_bios_seeds.py` | main `ca646cb4a`, `34a664b98` | Coverage/perf only for SCPH5552-pinned kits. | Take them with the next BIOS regen. |
| Native-wide opt-in improvements | `07525019b`, `a2b951c18`, `fd3126edd` | Enhancements, opt-in. | Separate enhancement review. |

## Checked, no action

- SPUCNT: the target has no gate, which is the accurate state; `8572af1c9` stays out.
- `runtime_overlay_guards_test` is registered in `runtime/CMakeLists.txt` (the audit flagged the recompiler list
  only).
- CD seek cancelling active reads, the PGXP series, and the CD DMA sector latch are superseded on the target.

## Warning

Fork `main` PR #12 (the 08-31 CD-ROM change set) is patch-identical to #282, which the release line reverted as
#287 after the Ape Escape texture regression. Nothing on this branch comes from `main`'s CD-ROM code.
