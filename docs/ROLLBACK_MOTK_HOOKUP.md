# MotK rollback hookup checklist

Status: **hooked (experimental)** · branch `feat/rollback-netplay` · depends on
`lib/recomp-net` @ `feat/rollback`

Today MotK / psxrecomp lobby netplay defaults to **rollback**:
`stage_local → poll_admit (invent within P) → guest frame → finish_frame`.
Missing remotes are invented (**hold-last**, MotK digital) only while
`wire_need ≤ highest_remote + P`; outside that window admit stalls (BattleShip
phase_lock). Idle invent re-mismatched every held D-pad tick after commit
(char-select episode storm). Seal gap-fill stays idle. Late wire goes through
the input contract; rewinds open an `RNetRbSession` episode. Menu releases
rewind (FMV soft-promote release-only only). Host **Disable Rollback** (or
`PSX_NET_MODE=delay`) forces classic delay-sync `try_admit`.

---

## 0. Ground rules (do not skip)

| Rule | Why |
|------|-----|
| Keep delay-sync `RNetSession` working behind a flag | Lobby / ICE / save-xfer stay useful; Disable Rollback opts out |
| Predicted rows promote only via `hash_confirm` (or host protect) | Library invariant; NULL `hash_confirm_promote` = always rewind |
| Digests must be bit-identical across peers for the same sealed inputs | Otherwise every invent becomes an episode storm |
| **Same bit-identical binary on both peers** | Mixed `build-release` vs packaged `motk-*` can match live digests for hundreds of ticks then fork GPRs/timers in Replay (pin zlib skew is the symptom) |
| Snapshots must restore at the exact sim tick requested | Ring load is the only rewind mechanism |
| Single-thread ownership of session + rings | Same as delay-sync |

Env gate: `PSX_NET_MODE=delay|rollback`. Lobby default publishes
`match_caps.rollback=true`; **Disable Rollback** publishes false. Auto D/P from
RTT (BattleShip tiers / phase_lock); optional Manual Input Delay / Prediction.

---

## 1. In-memory snapshot ring (host)

### Tasks

- [x] Add `boot_state_save_buffer` (or serialize-to-malloc) mirroring load-buffer
- [x] New module e.g. `runtime/src/netplay_snap_ring.c`:
  - depth `N` (≥ `RNET_RB_SEAL_MAX_SPAN` = 128; start with 64 for soak)
  - keyed by `sim_tick`
  - `ring_save(tick)`, `ring_load(tick)`, `ring_has(tick)`, drop oldest
- [x] Capture at a **safe** boundary — `psx_netplay_poll_snap` beside
      `savestate_poll` on slow + MotK IRQ fast/mid paths (`interrupts.c`);
      also flush pending snap in `finish_frame` (FMV mid-path used to skip it)
- [x] On load: `psx_cycles_resync_after_restore` / `interrupts_resync_after_restore`
      / `cdrom_accelerate_after_savestate` / `psx_frontend_on_savestate_loaded`
      / deferred `psx_scheduler_resume_at` via `psx_netplay_rb_flush_resume`
- [x] **Resume safety:** never `longjmp` from `rb_pump` under C++ vblank RAII —
      apply without resume, then `flush_resume` on BB-edge (`poll_snap`) or
      inside the admit wait loop (after present-body RAII destroyed)
- [x] **Admit/resume split:** `try_admit` never arms `needs_advance` while a
      resume is pending or while still in `AwaitingBaseline`
- [x] **Baseline gate:** send/enter Replay only after `g_episode_snap_applied`
      (peer BASELINE can arrive during SealInputs — must not skip restore)
- [x] Live snap rate-limit: every `PSX_NET_SNAP_INTERVAL` ticks (default **16**,
      max 32); resim still snaps every tick. Ring uses **raw** `boot_state`
      (no zlib) — `compress2` on RAM+VRAM+SPU was the live FPS tax; disk `.pst`
      still zlib. (Skipping VRAM on live snaps is unsafe for rewind.)
- [x] Snap PC must be `psx_is_dispatchable` — pick IRQ/BB-edge resume PC (not
      `cpu->pc` at present, often 0); defer save / abort episode instead of
      `resume_at(0)` → `trap_crash`
- [x] `flush_resume` only in **Replay** (after both baselines); SealInputs
      retransmits SEAL_ROWS; seal `get_input_row` invents hold-last so rows
      are `is_valid` (peer mask never completes on empty exports)
- [x] Live `g_np.needs_advance` must not bypass `rb_try_admit` during an
      episode (follower spun uncapped with zero `finish_frame`); arm load_tick
      at Replay entry so the post-`flush_resume` quantum is committed
- [x] Retransmit BASELINE while AwaitingBaseline (TURN drops one-shots);
      follower marks `digest_a=READY` once it has the peer digest; initiator
      waits for that ready-ACK before Replay (no solo resim)
- [x] Retransmit POST while Verify; admit stalls until peer POST / commit
- [x] `load_tick` tip slack (one `snap_interval` behind newest) so lagging
      peer still has the snap; follow refuse → SYNC `initiator=0` NACK;
      initiator aborts SealInputs (plus 4s seal timeout)
- [x] BASELINE: stash if episode not open yet; burst rexmit on TURN; initiator
      ready-timeout → Replay (Verify still waits for peer POST)
- [x] Resume PC: reject low/vector junk (`0xB0` etc.); prefer function entries;
      rewrite on load; 5s replay stall + 4s verify POST timeout → abort
- [x] Seal from `load_tick` (not only mismatch); hist then sealed SIO publish;
      sticky BB PC; skip CD accelerate + audio pump on resim; POST digest
      canonicalizes parked PC; POST diverge aborts episode (lobby stays)
- [x] Master digest folds CDROM controller FSM; per-tick resim audit logs
      dig/cd/sealed pads; idle_skip + auto_skip_fmv forced off under netplay;
      POST/baseline diverge → Live realign to matched load (or last commit)
- [x] Never apply baseline/realign snaps from mid-guest `psx_netplay_pump`
      (cycle watchdog) — only admit-wait / poll+immediate `flush_resume`;
      initiator waits for ready-ACK (no solo Replay); ready timeout → realign
- [x] Pin episode baseline snap (resim must not overwrite load_tick); realign
      loads the pin; audit logs `core=` vs `cd=`; skip CD boost + reset SPU CD
      FIFO on RB restore
- [x] Baseline/POST/hash_confirm agree on **core** digest only (`cd=` audit);
      CD-only forks were aborting good title/menu Start corrections
- [x] Storm calm: **abort/storm** cooldown + promote-sweep only (not after
      clean commit). Post-commit promote-only made char-select D-pad feel
      rejected (hist ok, sim not rolled). Live invent is **hold-last** (idle
      invent → hold mismatch every tick = episode storm). Seal gap-fill idle.
      Menu release soft-promote removed (forked sticky Up with hold-last);
      FMV settle still soft-promotes releases. Press/release → episode; held
      matching invent → no episode.
      **Char-select “hang” with matched POST:** digital button edges always
      rewind in `rnet_input_contract` (no `hash_confirm` promote for buttons).
      A tap used to be two episodes (press + release). Fixed in recomp-net:
      **tip-extend** (`rnet_rb_extend_target` / MotK `psx_netplay_rb_tip_extend`)
      +       **TipHold** (`rnet_rb_enter_tip_hold` after POST match: Live continues,
      `tip_runway` quiet window, late release tip-extends without a second
      baseline) coalesce press+release into one episode. Tip-extend from
      TipHold/Verify clears the POST handshake; follower FOLLOW mirrors
      initiator rereplay; TipHold ignores stale rexmit POSTs.
      Post-TipHold `choose_load_tick` must not pick Live tip snaps above
      `agreed_through` unless `hash_confirm` confirms them (else follower
      NACK past frontier).

      `tip_seal_slack` (default 2) sets initial seal headroom;
      `tip_runway` (default 12) caps TipHold + suggest slack;
      **light tip**
      (`RNET_RB_CORR_LIGHT_TIP`) skips ready-ACK RTT when load is at the
      shared frontier and depth ≤ 8. Do **not** soft-promote releases.
      **Resim depth (main-menu "back to title" hang):** `choose_load_tick`
      always applied interval rounding + one-interval tip slack, so a mismatch
      one tick after a commit (798) re-loaded 768 and every menu tap replayed
      ~30 ticks twice. Fixed with a **shared-frontier fast path**: ticks the
      peer provably holds need no slack — (a) `hash_confirm` resolved_through
      (both simmed, core digests matched → same interval snaps) and (b) the
      last committed replay span (both peers resim-save a snap at every
      replayed tick). Release-after-press now loads at commit target, ~2-tick
      replay. Peer eviction still covered by follower NACK (abort, not hang).
      Do **not** freeze `cdrom_advance` during Replay (FMV skip resim hung).
      Promote wire into hist **before** `begin_rewind` seal.
      FMV media + **digest-gated post-FMV lockstep**: no invent + refuse
      begin/follow for at least MIN=90 ticks (title Start was
      invent→`0x8006CDA0` episode hang); hold until hash_confirm matched
      CONFIRM=16 contiguous ticks or MAX=180 (cross-game cutover). Dense
      tip snaps during media/lockstep/+32 after invent unlock so the first
      invent-miss loads near the mismatch (was 954→944). Diag `FIRST CORE
      DIVERGE` at first post-lockstep press is invent≠wire (e.g. `ffff`→
      `ffbf` Cross), not residual FMV non-determinism — live digs matched
      through lockstep.
      Symmetric ready: follower ACK → initiator GO → both Replay.
      **`flush_resume` releases `gpu_vblank_flush_present` reentrancy guard**
      before longjmp (stuck `s_flushing_present` blocked all later presents /
      `finish_frame` — MotK `0x8006CDA0` Replay hang). Mid-guest pump runs
      `poll_replay_stall` (5s abort).
      **Menu wait-loop resim (CD54↔CDA0):** do **not** arm deferred present on
      flush_resume / do **not** re-finish `load_tick` (snap is already
      post-present). Phantom fin@load before the latched VBlank IRQ put peers
      on opposite ping-pong edges (`v0=1` vs countdown, ~5-cycle skew). First
      arm is `load+1`; `hc` primed after load. Netplay flushes deferred present
      **after** IRQ delivery when both are due (post-RFE digest).
      **Returning-leaf flush_resume (`0x8006A9F8` → PC=0):** prefer IRQ/sticky/
      `$ra` over bare function-entry snap PCs; `flush_resume` clears deferred
      present + bb_defer nest; sentinel same-thread path must not publish
      `pc=0` during top-level RB resume; scheduler recovers null-pc via `$ra`.
- [x] **Abort cooldown from live sim** (before realign rewinds the clock).
      Old path armed `until` from rewound tip → uncapped catch-up burned a
      short window in ms → char-select episode storms (`STALE COOLDOWN`).
      Failed episode: 60 ticks from live (120 on streak≥2; 90 if no tip).
      Reconcile promotes wire for the whole abort window (`promote-no-resim`).
      Clean commit does **not** arm cooldown.
- [x] Netplay forces **software GPU** — GL/VK `glReadPixels` VRAM readback was
      forking peer snaps (core matched, pin zlib ~220KB apart) and mid-resim
      cores; baseline/POST also agree on `av=` (GPU+VRAM) via dig_b / POST input_digest
- [x] SW GPU **before window** + late `force_sw` builds `SDL_Renderer` after GL
      teardown (first match was black: cleared `g_gl_active` with null present)
- [x] `flush_resume` releases present-flush reentrancy guard (no latched-VBlank
      re-arm — that phantom-finished load and forked menu wait resim).
      Symmetric ready GO. Live snaps stay on during media. Replay arms
      `load+1` with present-after-IRQ on netplay BB edges.
- [x] Netplay depth24: present 1/4 vblanks; skip live FRAME_COMMIT
      (full-RAM CRC) + prime `hash_confirm` when media ends
- [x] MDEC SSE2 IDCT + 24bpp YCbCr row encode (Beetle-matching, bit-identical
      to scalar; self-check at `mdec_init`) — raises offline/netplay FMV on
      slow peers; remaining FMV FPS is mostly host CPU ceiling
- [x] MDEC snap `last_color_age` is guest-cycle relative (was host
      `s_frame_count` → false baseline aux trips with matched FIFO/SPU)
- [x] Diag: `rb wire promote-no-resim` / `rewind-request` — late wire into hist
      without resim (cooldown/FMV/sweep) vs real episode open / begin refused
- [x] Diag: `rb live dig` every 32 ticks + `rb FIRST CORE DIVERGE` (FRAME_COMMIT
      mismatch) with core partitions (cpu/clk/tim/ram/dirty) + av/cd — find when
      peers fork before the first doomed baseline abort
- [x] Baseline `dig_c` = **ext** = crc(aux, cd, spad, dma, sio) — gate before
      Replay; matched core/av/aux with divergent CD/bus (pin zlib skew) was
      doomed resim. Wire still dig_c; logs print `ext=` + raw
      `cd=`/`aux=`/`spad=`/`dma=`/`sio=`. Zero host `last_sector_frame` on CD
      snap wire. mid-Replay FRAME_COMMIT abort on core mismatch (no false
      POST); `rb audit fin` + abort dump parts + bus digests + `cpu-split`
- [x] **SIO resim fork:** `sioP` showed **fsm-only** forks with matched
      regs/pads/mc + bit-identical guest. Fixes: (1) `sio_pace_walk` no longer
      drops leftover cycles after a transition cap (peers batching advances
      differently left divergent shift/ack remainders); (2) reseat
      `sio_trace_seq` on snap load; (3) netplay `sio=` / baseline_ext fold only
      through fsm **pace** (exclude host-audit meta/byte_seq);
      (4) mid-Replay cycle-watchdog pump drains FRAME_COMMIT only — no
      reconcile/`rb_pump`. Logs: `sioP=regs/pads/mc/pace/meta`
- [x] Replay entry: `hc_prime_after(load-1)` drops live invent commits (false
      `resim core diverge`); first `ready=1` baseline burst bypasses rate limit
      (initiator was ready-timeout while follower solo-Replayed)
- [x] Present-edge digests clear PC (FRAME_COMMIT / audit fin / POST) — parked
      `cpu->pc=0` vs live BB + host-local sticky forked dig_cpu with matched
      RAM/clk; `rb cpu-split` logs gpr/hi_lo/cop0 vs raw_pc
- [x] **Core digest folds GTE** (was snap-only) — MFC2 can fork GPRs with
      matched RAM/clk/SIO; `rb cpu-split` adds `gte=` + `rb gpr-dump` on
      fin/abort/post so peers can diff which regs forked
- [x] **Core digest = fold of part digests** (one RAM CRC pass, not dual
      stream); `core=` values change vs older binaries — peers must match
- [x] Log `rb binary path=… size=…` at rollback start — **peers must run the
      same bit-identical binary**. Diags showed host `build-release/` vs guest
      `motk-0.1.0-linux-x64/` with pin zlib ~1.34M vs ~1.13M, matched baselines,
      then Replay GPR-only (ep1) / cpu+tim+ram (ep2 tick after a good Up)
- [x] FPS: raw in-memory snaps + default interval 8; resim audit skips AV/CD
      on `arm` (VRAM CRC only on `fin` / baseline / POST). Still open: strip
      CDROM/MDEC from MotK ring if match path allows (further RAM headroom —
      raw ring ≈ 3.5 MiB × depth)
- [x] **MotK menu v0-only Replay fork:** matched baseline + tick N (all GPRs),
      tick N+1 only `r2` differs (host `1` / guest `0x5bd2`) with sticky
      `0x8006CDA0` wait-loop, matched RAM/GTE/cycles. BIOS left `v0=1` when
      same-thread GPR restore skipped (mode-1 PC heuristic miss) while peer
      restored the post-`lw` countdown. Fix: netplay auto
      `PSX_SAME_THREAD_RESTORE=3` — same-TCB RFE/SYSCALL always restores
      (ChangeThread still skips). Diag: `rb irqctx` on fin/abort with
      `restored`/`v0_exit`/`v0_saved`. Do **not** canonicalize v0 in digests.
- [x] **Netplay BB-edge present:** after restore-3, idle sealed resim still
      forked `cpu`+`ram` in one tick — `sdl_vblank_present`/`finish_frame` ran
      nested from `fire_vblank_edge` mid-`psx_cyc_step` in the wait loop, so
      peers digested different instr points. Fix: under netplay, queue the
      GPU vblank callback and flush in `psx_check_interrupts` (BB edge);
      clear deferred pending on snap resync. Offline still presents immediately.
- [x] **Menu wait resim phase (CD54↔CDA0):** after present-guard fix, ep1
      matched fin@load then forked fin@load+1 (`r2=1` vs countdown, cyc±5).
      Cause: flush_resume armed deferred present for latched I_STAT → phantom
      finish_frame before latched VBlank IRQ. Fix: arm `load+1` only; no
      latched re-arm; present-after-IRQ when delivery is due.
- [x] **Returning-leaf resume crash:** after load+1 fix, Start-class episode
      resumed at `0x8006A9F8` (`jr $ra` leaf) with IEc=0 → top-level
      `execution completed, PC=0` (cps: `final_ra=0x8006CCA0`). Fix: resume
      PC pick prefers IRQ/sticky/`$ra`; clear present+bb_defer on flush;
      no sentinel `pc=0` while `top_level_resume`; scheduler null-pc recover.
- [ ] Memory budget / thinner snap: optional strip CDROM/MDEC if MotK match
      path allows (further RAM headroom)
- [x] Standalone ring bookkeeping test: `runtime/tests/test_netplay_snap_ring.c`

**Vtable:** `save_state` / `load_state` → ring only (never disk slots).

---

## 2. Master state digest + frame-commit watermark

### Tasks

- [x] Define MotK master digest (`netplay_master_digest` + CDROM partition)
- [x] After each committed sim tick, compute `digest[tick]` into a small ring
- [x] Exchange `RNET_PKT_RB_FRAME_COMMIT` via session send/take (queued)
- [x] Advance local `resolved_through` only when digests match through `T`
- [x] Implement `psx_netplay_hash_confirm_through(tick)`
- [x] Wire `hash_confirm_promote` in invent/contract + episode stick gates
- [x] Unit test: `runtime/tests/test_netplay_hash_confirm.c`

---

## 3. Input history + invent / prediction

### Tasks

- [x] Per-slot input history ring (`netplay_input_hist`)
- [x] Local path: `is_predicted = 0`
- [x] Remote invent = **hold-last** (neutral if no prior); stall when ahead of
      remote tip by > P. Seal `get_input_row` gap-fill remains **idle**.
- [x] Late wire → `rnet_input_contract_stick_replace_decide` + `hash_confirm_promote`
- [x] Rewind → `psx_netplay_rb_begin_rewind` (episode)
- [x] `get_input_row` vtable → `netplay_ih_get`
- [x] `PsxNetPad` ↔ `RNetRbFrame` (incl. `analog` → SIO pad type; seal wire `source`); SIO from history / sealed rows
- [x] Unit test: `runtime/tests/test_netplay_input_hist.c`
- [x] Env: `PSX_NET_MODE=delay|rollback` → `psx_netplay_rollback_mode()`

---

## 4. Live frame loop (replace admit barrier when rollback on)

### Tasks

- [x] Branch `poll_admit` on rollback (invent; episode admit while active)
- [x] Never call delay-sync `try_admit` wait for missing remotes in rollback mode
- [x] Still use `RNetSession` for pad tip transport + ICE
- [x] Keep load-barrier / save-xfer / soft-exit paths on delay-sync semantics
- [x] `finish_frame` requests snap; episode path uses `psx_netplay_rb_finish_frame`
- [x] Skip wall pacer while `psx_netplay_is_resimulating()`

---

## 5. Episode path (resim)

### Tasks

- [x] Create `RNetRbSession` in `psx_netplay_rb.c` when mode=rollback
- [x] Fill `RNetRollbackVTable` (snap save/load, digest, hist get_input_row)
- [x] On rewind: `load_tick` = newest ring tick ≤ mismatch (refuse if ring empty)
- [x] `g_np.local_slot` / `slot_count` / delay set **before** `rb_start` (frozen into `RNetRbConfig`)
- [x] Peer seal apply ignores `is_valid=0` rows (wrong-seat export must not complete)
- [x] Seal export/apply over `RNET_PKT_RB_SEAL_ROWS` (+ SYNC/BASELINE/POST/RESOLVED)
- [x] During resim: skip wall pacer; publish sealed SIO (not live invent)
- [x] After commit: `rnet_rb_on_post_match` + session sim clock to `target+1`
- [ ] Soak: forced stick mismatch → one episode; digests match post-commit

---

## 6. Wire / transport mapping

### Tasks

- [x] Session send/take for SYNC / SEAL_ROWS / BASELINE / POST / RESOLVED /
      FRAME_COMMIT
- [x] Delay-sync peers ignore opcodes 20–25 (session queues only when MotK drains)
- [x] Negotiation: `match_caps.rollback` + launch.`rollback` / `PSX_NET_MODE`

---

## 7. Determinism prerequisites (MotK-specific)

### Tasks

- [x] Netplay clears mods (`mod_runtime_clear_for_netplay` / launcher hook)
- [x] Same BIOS stem + disc identity on both peers (existing verify)
- [x] Audit non-deterministic host clocks in sim path — **selfcheck-driven**
      (was soak-driven). `PSX_RB_SELFCHECK=1` (offline, single process,
      `runtime/src/psx_selfcheck.c`): every INTERVAL boundaries snap the
      machine at a savestate BB edge, record SPAN ticks of applied pad rows +
      full digest partitions, then rewind and resim the window **twice** from
      the same snap. Replay#1 vs replay#2 is the rollback invariant (both
      peers resim in an episode) → PASS/FAIL with per-part DIVERGE lines;
      live-vs-resim is reported as `restore-drift` (diagnostic; VBlank phase
      re-applied from snap). Verdict = warm #2vs#3 after ambient-prime.
      Fixes via selfcheck: VBlank-phase restore; overlay freeze; discard
      dirty/interp load-delay on restore; pin host frame; RECORD=resim
      host-skip profile; idle_skip off; turbo/FMV-skip gated; top-level
      resume; IRQ escape + check-cycle/poll-throttle clear; sticky
      `text_diverged` / kernel-bless / overlay validation memos cleared on
      RAM restore; **span-end load** via `psx_selfcheck_flush_load` after
      present-body RAII (BB fast-poll tails forked #2/#3 load sites);
      snap PC uses RB `resume_pc_ok` (reject 0xB0). MotK attract soak
      `/tmp/selfcheck_spanend2.log`: **316+ PASS / 0 FAIL** through the
      classic win#70/#73/#118/#138/#139/#277/#279/#301 set. Re-verified after
      hold-last invent (`/tmp/selfcheck_verify.log`): win#70/#73/#118/#139
      PASS, **157+ PASS / 0 FAIL** and climbing. Post-FMV cutover soak
      (`/tmp/selfcheck_fmvcut.log`, INTERVAL=120 SPAN=48): **52 PASS / 0
      FAIL** through early attract/menu — confirms 954-class peer fork is
      invent≠wire, not offline resim non-determinism. Tune:
      `PSX_RB_SELFCHECK_INTERVAL/SPAN/FAULT/PRIME`.
      **Mash:** `PSX_RB_SELFCHECK_MASH=1` (+ optional `MASH_SEED` /
      `MASH_RATE`) synthesizes fighter-style face/D-pad/shoulder spam on
      live boundaries so headless soaks stress invent edges / wait-loop
      resim without a controller. Rows are recorded and replayed.
      **Stuck hash_confirm / doomed tip loads:** a transient FIRST CORE
      (e.g. sim 201) left `resolved_through` stuck; once that tick aged
      out of the 128-slot dig ring, `choose_load_tick` fell through to
      tip-slack (912) while confirmed snaps were gone — baseline then
      aborted on cpu-only MotK wait-loop PC forks (`CD54`↔`CDA0`) despite
      matched ram/clk. Fix: `netplay_hc_heal_stale_gap`, refuse loads past
      the shared frontier, canonicalize wait-loop snap PC to `CDA0`.
      `rb live dig local` is a breadcrumb (not peer match).
- [x] FMV / depth24: defer rewind (promote wire only); follow NACKs; RB snap
      load uses light frontend hook (no FMV cutover/present thrash)
- [x] Multitap / N-slot: history + seal tables sized to `slot_count`

---

## 8. recomp-ui (branch `feat/rollback-netplay`)

### Tasks

- [x] Host Lobby Settings: **Disable Rollback** (off by default → rollback on)
- [x] Manual Input Delay + Manual Input Prediction (P locked when rollback off)
- [x] Auto D (RB tiers / delay-sync pad) + auto P at Play from max peer RTT
- [x] Plumb `launch.rollback` / `input_prediction` + match_caps + `net_cfg`
- [x] Diag: stderr episode begin/commit/load; invent/promote/rewind counters on hist

---

## 9. Suggested implementation order

1. ~~`boot_state_save_buffer` + snap ring~~  
2. ~~Master digest + FRAME_COMMIT~~  
3. ~~Invent + input contract~~  
4. ~~Episode resim wiring~~  
5. ~~Lobby flag + UI~~  
6. ~~Rate-limit live snaps (`PSX_NET_SNAP_INTERVAL`) + safe episode resume~~  
7. ~~Thinner snap / FMV policy~~ — interval **16**, settle invent ok, digest
   fold (skip-VRAM on live snaps unsafe for rewind; optional later)
8. **Dual-instance soak** — prove Done-when items  

---

## 10. File touch map

| Area | Files |
|------|--------|
| Snap ring | `boot_state.*`, `netplay_snap_ring.*`, `psx_netplay_rb.*`, `interrupts.c` |
| Digests | `netplay_hash_confirm.*`, `netplay_state_digest.*`, `psx_netplay.c` |
| Invent / contract | `netplay_input_hist.*`, `psx_netplay.c` |
| Episode | `psx_netplay_rb.*`, `lib/recomp-net` session RB send/take |
| Frame loop | `main.cpp` (`sdl_vblank_present` epilogue), `psx_netplay.c` |
| Caps / UI | `psx_lobby_client.*`, `recomp_launcher.h`, `launcher_imgui.cpp`, `main.cpp` |

---

## 11. Done when

- [ ] Two MotK instances with `PSX_NET_MODE=rollback` complete a match without
      admit-stall on one-frame remote loss
- [x] Forced remote digital correction opens episode(s); POST digests match
      (char-select L/R soak: 4 commits, live dig ok after). Tap = press+release
      = two episodes by input-contract (button deltas always rewind).
- [ ] `hash_confirm` invent path shows promotes in diag without opening episodes
      when master hashes still agree (stick/analog path; **not** MotK digital
      buttons — contract always rewinds button deltas)
- [x] Delay-sync path (`PSX_NET_MODE=delay`) unchanged for lobby rematch / save xfer

Reference: `lib/recomp-net/docs/rollback.md`,
`include/recomp_net/rollback.h`, `include/recomp_net/input_contract.h`,
`tests/rollback_episode_test.c`.
