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
      Tip-extend during Seal/AwaitingBaseline resigns+extends only (no
      rereplay) so the initiator cannot solo-Replay before ready GO.
      TipHold tip-extend **stays TipHold** (library); rereplay only if Live
      already invented past the prior tip. FOLLOW mirrors that gate.
      Tip-extend rereplay reloads the **prior tip** on both peers (not
      mismatch-1→pin fallback vs FOLLOW old_t).
      Tip-extend snap apply always `arm_rereplay_after_load` (hc_prime +
      `sim=reload+1`) — poll_snap used to apply without arming, leaving the
      TipHold Live tip clock and invent FRAME_COMMITs → false
      `resim core diverge` (matched sealed fin, stale peer invent).
      TipHold Live does **not** emit FRAME_COMMIT; tip-extend Replay drops
      in-flight invent commits until the peer's sealed resim matches.
      **TipHold invent-cap (menu D-pad storm):** Live stalls once
      `sim > tip + tip_seal_slack` (MotK forces slack **0**). Stops invent
      racing ahead of the tip and tip-extend rereplays on every held edge.
      Quiet finalize counts `tip_runway` (MotK 24) pump frames at the invent
      cap when sim cannot reach the old `tip_hold_until`.
      **Seal span cap (menu hang):** peer-seal completion is a `uint64`
      bitmask (`RNET_RB_PEER_SEAL_MASK_BITS` = 64). Tip-extend past
      `seal_base+63` fails; TipHold used to `begin_refused` until runway
      then open ep N+1 alone → `seal timeout`. Fix: TipHold span-cap
      tip-hold-commits immediately and `begin_rewind` opens a fresh
      episode; FOLLOW yields TipHold on a new-epoch SYNC. Self-test:
      `rollback_episode_test` (extend to span=64 OK, span=65 fails).
      **Stale POST after tip-extend:** `RB_POST` carries `target_tick`;
      Verify ignores peer POSTs whose tip ≠ episode tip (tip-extend left
      POST@T in the queue while Verify@T+1 → false diverge / peer verify
      timeout). Self-test: `test_rb_post_tip_filter` + `rb_wire_test`
      tip binding.
      Post-TipHold `choose_load_tick` / follower frontier hard-cap at
      `agreed_through` — ignore `hash_confirm` above that watermark (Live
      invent + PC-cleared FRAME_COMMIT false-confirmed forked tip snaps).

      `tip_seal_slack` (MotK 0 / library default 2) sets invent headroom past
      tip + initial seal; `tip_runway` (MotK 24 / library default 12) caps
      TipHold quiet + suggest slack;
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
      begin/follow for at least MIN=180 ticks (~3s; was 90 — invent≠Cross
      at +14 opened a `0x8006CDA0` tip storm); hold until hash_confirm
      matched CONFIRM=16 contiguous ticks or MAX=300. On RELEASE: invent
      stays off **UNLOCK_GRACE=64** more ticks (do not clamp until→sim) and
      arm `promote_sweep` so sticky hold-last poison cannot invent at
      unlock+1. Invent stays **hold-last** after unlock (idle invent
      re-mismatches every held D-pad tick — char-select freeze class); a
      `pub=ffbf wire=ffff` FIRST CORE at unlock is a normal press/release
      mispredict and light tips handle it. Dense tip snaps during
      media/lockstep/+32 after invent unlock.
      MotK TipHold: `tip_runway=24`, invent slack **0** (Live stalls at tip)
      so tip-extend rarely rereplays — menu D-pad coalesce without the
      prior-tip reload FPS cliff.
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
      **after** IRQ delivery when both are due (post-RFE digest). Also: do
      **not** flush present on BB edges while `in_exception` (handler sees
      IEc clear → old path `finish_frame` mid-BIOS before RFE restore;
      soak irqctx `restored=0/reason=0`, sealed Cross resim forked
      `v0=1` vs countdown / cyc±14).
      **Returning-leaf flush_resume (`0x8006A9F8` → PC=0):** prefer IRQ/sticky/
      `$ra` over bare function-entry snap PCs; `flush_resume` clears deferred
      present + bb_defer nest; sentinel same-thread path must not publish
      `pc=0` during top-level RB resume; scheduler recovers null-pc via `$ra`.
- [x] **Abort cooldown from live sim** (before realign rewinds the clock).
      Old path armed `until` from rewound tip → uncapped catch-up burned a
      short window in ms → char-select episode storms (`STALE COOLDOWN`).
      Failed episode: **24** ticks from live (**48** on streak≥2; was 60/120).
      Begin **SPAN CAP=24**: deep post-abort catch-up is chunked (commit →
      next episode) instead of one Replay across the whole cooldown
      (soak depths 63→128 felt like "pushing further back").
      Reconcile promotes wire for the whole abort window (`promote-no-resim`).
      Clean commit does **not** arm cooldown.
- [x] **Present only at MotK wait CDA0:** netplay `gpu_vblank_flush_present`
      skips while resume/check PC is `0x8006CD54` (leave pending until
      `0x8006CDA0`) so sealed idle resim cannot digest opposite ping-pong
      edges (cyc±9 / v0 fork after mid-handler present fix).
- [x] Netplay forces **software GPU** — GL/VK `glReadPixels` VRAM readback was
      forking peer snaps (core matched, pin zlib ~220KB apart) and mid-resim
      cores; baseline/POST also agree on `av=` (GPU+VRAM) via dig_b / POST input_digest
- [x] SW GPU **before window** + late `force_sw` builds `SDL_Renderer` after GL
      teardown (first match was black: cleared `g_gl_active` with null present).
      Rematch: only lock SW after `ensure_sw_sdl_present` succeeds; retry if
      prior force left a null renderer (empty window after lobby rematch).
- [x] Mid-FMV tip load: `cdrom_resync_deadlines_after_restore`; do not wipe SPU
      CD FIFO while XA/FMV pending; FMV media includes `cdrom_fmv_stream_pending`
      (XA mode+reading) so invent-off arms before first MDEC colour decode;
      `mdec_recently_active` uses guest-cycle age (host `s_frame_count` lied
      under present-skip / Replay). RB frontend hook resets depth24 cutover
      when media is already live (avoid permanent cutover blank).
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
- [x] **Mid-handler present flush:** sealed Cross tip still forked fin@load+2
      (`v0=1` vs countdown, cyc±14) with matched pads/SIO/baseline; irqctx
      stuck at `restored=0/reason=0`. Cause: handler BB edges flushed deferred
      present while `in_exception` (IEc clear). Fix: skip present flush until
      outer delivery returns; `gpu_vblank_flush_present` also refuses
      `in_exception`.
- [x] **CDA0-only present + abort span cap:** after mid-handler fix, tip+1
      idle sealed resim still forked cyc±9 on CD54↔CDA0; abort cooldown
      then opened 63–128-tick catch-up Replays. Present defers at CD54;
      abort cool 24/48; begin SPAN CAP 24 chunks catch-up.
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
      **Agreed tip aged out of snap ring:** after tip-hold commit, Live can
      advance ~900 ticks with matching FRAME_COMMITs (`hc=1824`) while
      `agreed_through=919` falls below `oldest` — begin REFUSED forever
      (`no confirmed snap`), invent≠wire never resims, cores fork with
      matched clocks. Fix: `heal_agreed_watermark_if_aged_out` raises
      agreed to the highest hc-confirmed snap still in the ring (only when
      *no* agreed-era snap remains — TipHold false-confirm guard intact).
      `rb live dig local` is a breadcrumb (not peer match).
      **BOOTSTRAP when tip-hold never ran:** doomed early FMV abort left
      `agreed` invalid and `hc` stuck below ring oldest (live core forks
      block `heal_stale_gap`). Remote saw host P1 on the wire (`pub=ffff
      wire=ffbf`) but `begin REFUSED` forever — P2 corrections still opened
      on host. Fix: seed agreed to newest interval snap (`BOOTSTRAP` /
      `HEAL-FORCE`); choose_load/follow fall through when watermark `< oldest`.
      **Post-HEAL resim CD54↔CDA0 (cyc±3 / −43):** after `agreed HEAL→944`,
      identical baseline + pads still forked fin@945 — dig2 IRQ at CDA0
      (`cyc=…569`, countdown v0) vs dig1 at CD54 (`…572`, v0=1); dig2 later
      matched dig1 on a warm retry. Cause: `interrupts_resync` zeroed
      `cycles_since_vblank` (not in snap/digest) while timers/LCF kept mid-frame
      phase; CDA0-only present treated latch `pc==0` as presentable; Replay
      tip-apply from BB poll mutated CPU under live CPS. Fix: persist csv in
      `BS_SEC_IRQ` (12B), stop zeroing on resync, fold csv into clk digest,
      present only at CDA0 (sticky/last fallback), episode loads only via
      `try_admit`.
      **Post-csv residual (clk/tim +9, double fin):** after csv persist, sealed
      tip still aborted at resim 1202 with matched GPR/cpu/ram/av/SIO but
      irqctx cyc ±9; dig1 `fin@1202` and `fin@1203` shared identical dig/cyc
      (double `finish_frame`, no guest advance). Cause: sticky I_STAT kept
      `delivery_needed` so entry skipped present while post-IRQ flush also
      skipped on CD54 → deferred pending stacked across a full VBlank, then
      one CDA0 drain ran finish twice; sticky-unknown defer (latch cleared)
      worsened accumulation. Fix: attempt present at entry even when delivery
      is due (CDA0 gate no-ops CD54); coalesce `s_present_pending` to 1;
      sticky CDA0 with cleared latches allows coalesce present.
      **Arm→first-present +2 VB on long Replay:** post-HEAL short tips matched,
      but cold span-cap load (ep9 952→976) forked fin@953 — dig1 arm+2.03 VB
      vs dig2 arm+1.00 VB, pads matched. Cause: entry `gpu_vblank_flush_present`
      ran before `s_last_interrupt_check_pc` was updated, so MotK gate saw stale
      `last==CD54` on every CDA0 edge and no-op'd; present only drained on
      post-IRQ at a CDA0 delivery. First post-arm VB taken at CD54 → full extra
      period. Fix: publish edge PC before entry flush; gate prefers compiled
      CDA0 pc over stale last.
      **Non-det fin@946 CD54 vs CDA0 (resim storm):** matched baseline + pads;
      dig1 alone flipped cores across warm retries — IRQ at CD54 (`v0=1`) vs
      present-before-IRQ at CDA0 (countdown). Fix: hold VBlank-only delivery at
      CD54 while deferred present is armed (deliver after CDA0 present); MotK
      gate requires explicit CDA0 edge (no sticky-only allow); flush_resume
      pins sticky to canonical CDA0.
      **FMV1→FMV2 cutover stall:** soak matched through FMV arm then guest
      collapsed (~5 ms) with TURN admit wait and no `FMV settle` — media stayed
      “live” (depth24) while MDEC idled. MotK present gate treated sticky wait
      PC as in_wait and blocked non-CDA0 edges (FMV/cutover). Fix: gate only
      CD54 edges (sticky only when latch cleared); depth24 1/4 present-skip and
      FRAME_COMMIT skip only while MDEC hot; `rb FMV media` heartbeat every 32.
      **±1 VB replay fork at MotK wait (abort@902):** identical committed
      baseline 901 (`e6a64d47`) + idle pads still forked the first resim tick —
      abort cyc `509725487` vs `509160985` (Δ≈564480 = exactly one VBlank),
      wait countdown `r21=0x704` vs `0x705`, `v0=5c88` vs `5c89`. One peer
      delivered one extra VBlank during replay. Cause: the CD54 delivery-hold
      was gated on `gpu_vblank_present_pending()` — `s_present_pending` is
      host-only state (not in the snap), so peers entered replay with
      different hold behaviour. Fix: hold VBlank-only delivery at the CD54
      edge in netplay **unconditionally** (edge PC + I_STAT are
      guest-deterministic; CDA0 delivers a few instructions later).
      The FIRST CORE @896 (`pub=ffbf wire=ffff`) preceding it was a normal
      press/release mispredict — ep1–2 committed fine; the storm was this
      replay fork. UNLOCK_GRACE stays 64.

      **Residual Δ8-cycle replay fork (abort@940) — I-cache tags:** with the
      unconditional CD54 hold in, the next soak forked far smaller: identical
      baseline `f83f685b` / arm cyc `530611223` on both peers, fin@940 cyc
      `531175713` vs `531175721` (Δ8), `v0=5c83` vs `5c86` — VBlank taken a
      few wait-loop iterations apart, both at CDA0. Cause: the R3000A
      I-cache fetch-cost tags (`g_psx_icache_tv`, `psx_icache.c`) are
      host-persistent and were NOT in the snapshot — each peer replayed with
      the cache its own live/retry timeline had built, so fetch-miss cycles
      differed and IRQ delivery drifted. (Selfcheck/overlay replay already
      shadow this state; RB didn't.) Fix: new `BS_SEC_ICACHE` snapshot
      section (1024 tag words) written/applied by `boot_state.c` — warm
      loads, retries, and the exchanged baseline blob all share the exact
      fetch-timing state — and the tags are folded into the `clk` digest
      partition so any cache asymmetry surfaces at compare time instead of
      as a mid-resim abort. Section is optional on load (old blobs load
      untouched); netplay peers are bit-identical builds anyway.

      **Post-fix soak (2026-07-31): determinism CLEAN, storm is now pure
      episode cost.** Full soak with BS_SEC_ICACHE: **zero aborts, zero
      resim diverges** — 40+ consecutive episodes all committed first try.
      But menu play ran 0.44–0.8x: every button press/release edge
      (`pub=ffef/ffbf` vs `wire=ffff`, wire 1–2 ticks late) opens a paired
      episode (baseline pin 3.6MB + seal/POST handshake + tip-hold).
      The follow-up soak with `guest=`/`admit=` FPS split corrected the
      first theory: the link was **direct LAN all along** (ICE selected
      `typ host` candidates despite `force_turn=1` in the config banner)
      and `admit` (network wait) is mostly <5 ms/f. The real costs:
      (a) **guest CPU** — the MotK menu scene costs ~16.6 ms/f of guest
      work under netplay (software GPU forced + idle_skip forced off),
      i.e. zero headroom at 60fps, so every 2–4-tick resim burst drops
      frames (guest hits 20–29 ms/f during storms; pre-menu boot ran
      119 fps at guest 6–8 ms/f — it's the scene, not the engine);
      (b) **mispredict frequency** — with delay=2 and both sims hovering
      ±1 tick apart (lead=0 admission), the wire for tick T routinely
      seals 1–2 ticks late on every edge, and each episode further delays
      input relay (self-sustaining storm). Next: raise input delay to 3–4
      (margin for sim skew; kills most menu invents on LAN), and/or LAN
      `PSX_NET_PREDICTION=0` (stall instead of invent — RTT≈0 makes it
      free). Longer pole: shrink menu guest cost (hardware GPU needs a
      deterministic VRAM snapshot path; sw rasterizer + full busy-wait
      emulation is the ceiling), and drop baseline exchange/POST verify
      from sealed-input-only episodes now that determinism is proven
      (zero aborts across two full soaks).

      **Stall-before-invent grace (implemented):** `np_try_admit_rollback`
      invented hold-last on the FIRST miss of a remote wire row; the row
      then landed 1–2 ticks later and the mispredicted edge opened an
      episode. New `np_invent_grace_stall` (psx_netplay.c): before
      inventing, stall the admit up to **PSX_RB_INVENT_GRACE_MS (default
      30 ms)** from the first miss of that wire tick — the admit loop
      pumps the session and waits on UDP at ~1 ms granularity. Adaptive
      off: 45 consecutive expiries (peer genuinely lagging beyond the
      budget, real WAN) disables the grace for 2 s so it never stacks a
      per-tick stall on top of real latency. Host-side pacing only —
      invented values are unchanged, guest determinism unaffected.
      Set `PSX_RB_INVENT_GRACE_MS=0` to disable.
      The first cut used 8 ms and failed: the follow-up soak showed the
      lateness is **sim skew, not link latency** — ALL late edges were
      on the peer running 1–2 ticks ahead (22×late-1, 5×late-2; the
      other peer had zero), and 8 ms is less than one tick of skew, so
      the grace expired on every edge, tripped adaptive-off, and the
      storm returned ("stable for a few seconds then nonstop"). The
      grace is really a **rate governor**: while the ahead peer stalls,
      the behind peer keeps simulating and catches up, so a budget that
      covers ~2 ticks keeps the sims aligned and, in steady state,
      inputs arrive before the seal with no stall at all.
      **Tick-period scaling (2026-07-31):** the first full-match soak
      showed the fixed 30 ms budget being outrun in gameplay: the fight
      scene runs 35–45 fps (guest 9–20 ms/f + admit), so a tick lasts
      ~25 ms and 2 ticks of skew is 50 ms — edges mispredicted again
      (~40 episodes/match, "stormy" feel; lateness profile 27×late-0 /
      12×late-1 on the ahead peer). The grace now measures the live
      tick period (EMA over consecutive tracked wire ticks) and uses
      `max(PSX_RB_INVENT_GRACE_MS, 2.5 × tick EMA)`, capped 100 ms, as
      the effective budget, so it self-scales to whatever rate the sim
      actually runs. At 60 fps the EMA path gives ~42 ms; at 40 fps
      ~62 ms. Adaptive-off (45 expiries → OFF 2 s) still guards real
      WAN latency.
      **Gap gate (2026-07-31, same day):** the scaled grace fixed the
      storms (soak: zero episodes, zero aborts, deterministic match)
      but serialized the sims: stalling on ANY missing row meant
      neither peer ever ran ahead, so each admit waited out the other
      peer's guest frame — the FPS logs showed each side's admit ms/f
      mirroring the OTHER side's guest ms/f (8+12 vs 13+9, both
      summing ~23 ms → 40–50 fps all match with no episodes at all).
      Fix: the grace is only invoked when `wire >
      highest_remote_wire + 1` — a 1-tick gap is the normal phase
      offset of two pipelined sims and is invented through freely
      (hold-last only mispredicts on real edges, and episodes are
      cheap now that the agreed watermark tracks live). The stall —
      the rate governor — engages only at >=2 ticks of genuine skew,
      which was the storm condition it was built for.

      **Timesync micro-throttle (2026-07-31, follow-up soak):** with
      the gap gate, idle play held a clean 60 fps (admit ~0) — but
      every input burst dropped fps to 34–48. Episode forensics: 29
      episodes/side, 28/29 already on the light path, loads shallow
      (2–9 ticks). The cost is the replay itself — a paired 2–9 tick
      resim at ~13–16 ms guest/tick on BOTH peers per button edge —
      and the episodes all came from the same source as ever: the
      ahead peer running ~1 tick of phase ahead and inventing each
      edge just before the real row landed (lateness 23xlate-0 /
      6xlate-1, all on one side). New `np_timesync_throttle`
      (psx_netplay.c), GGPO-style: advantage = wire_need − highest
      received remote wire row, EMA'd once per tick; above +0.5 tick
      the ahead peer shaves up to 3 ms per admitted tick until the
      phase closes. Exactly one side sees positive advantage, so one
      side paces down briefly and then rows arrive before the seal —
      no invent, no episode. Capped at 3 ms/tick (worst case ~20%
      pace, never lockstep); skipped during episodes; adaptive off
      (engaged ~3 s straight → OFF 5 s) because WAN transit inflates
      the advantage and can't be closed by pacing. `PSX_RB_TIMESYNC=0`
      disables.
      **Advantage metric was wrong (2026-08-01 soak) — replaced with
      mispredict-driven pacing debt.** The soak showed BOTH peers
      measuring ~+0.6 ticks of "advantage" (10/16 and 11/16): each
      side samples wire_need at the start of its own tick while the
      peer's newest row was sent at the start of *its* tick, so the
      metric's natural operating point is ~+0.5, not 0. Both sides
      throttled, neither could close it, both tripped the off-guard
      (`advantage 17/16 not closing`), and the mispredicts continued
      (33xlate-1, 43 episodes/side, fights at 0.54–0.7x). Rework: the
      unambiguous "I am the ahead peer" signal is the mispredict
      itself — only the ahead side reconciles real rows that arrived
      AFTER it invented them. `np_timesync_note_late()` is called from
      `np_rollback_reconcile_wire` on every predicted row whose real
      row lands (matching or not, so held/idle rows feed the loop and
      alignment happens before an edge); each event adds ~half a tick
      of pacing debt (capped 2 ticks) and the admit path shaves it at
      <=3 ms/tick. Steady state aligned = no invents = no debt = zero
      cost. Adaptive off: debt continuously nonzero ~5 s (WAN transit
      — every row late regardless of phase) → OFF 10 s.
      Longer pole (documented, not built): one-sided
      episodes — the follower's live state through a sealed-only span
      is already the post-replay state (hc stores its per-tick
      digests), so it could answer baseline/POST from stored data and
      never rewind, halving system replay cost. Not attempted yet:
      protocol-level change on freshly stabilized machinery.

      **Two bugs found in a matched-pair soak (2026-08-01), both fixed:**
      1. `note_late()` was called unconditionally on every predicted-row
         resolution, not gated on `pads_differ`. Under normal input-delay
         prediction EVERY invented row eventually resolves against a real
         wire row whether or not the guess was right — that's just how
         delay-based prediction works — so the debt signal was mostly
         noise. Moved the call after the `pads_differ` computation and
         gated on it; the busier peer's rewind-request count (28 vs 12
         across a matched pair) now cleanly identifies which side is
         actually mispredicting.
      2. The off-guard used "debt continuously nonzero for 5s" → OFF.
         Soak evidence it was actively harmful: 24/36 episode commits and
         19/28 rewind-requests on the busier peer landed AFTER the
         off-guard fired mid-storm — an active mispredict burst legitimately
         keeps debt elevated (each edge tops it up before the last slice
         fully drains), that isn't pacing failing. Replaced with a streak
         counter: debt landing AT THE CAP with no room to have drained
         between hits, 12 times in a row, before disabling for 10s. This
         is the actual "pacing can't help, this is transit" signal.

      **Depth24 (FMV) present path batched (2026-08-01):** FMV frames
      showed guest cost roughly double the non-FMV baseline (15–22 ms/f
      vs 7–13 ms/f) with admit cost a minor fraction — a local compute
      cost, not a netplay one. `gpu_display_pixel_argb` was called once
      per output pixel, and for `depth24` did 3 separate `gpu_vram_byte()`
      calls each recomputing the row/wrap math fresh — 3x the per-pixel
      work of the 16bpp path, as a function-call chain instead of a
      straight-line loop. New `gpu_depth24_present_row()` (gpu.c) hoists
      the per-row invariants (vy, base byte offset, how many pixels fall
      inside the 2048-byte VRAM row before the black-fill tail) out of
      the inner loop and inlines the byte extraction — same byte-order
      shifts as `gpu_vram_byte` (no host-endianness assumption), same
      output. Wired into both depth24 present call sites in `main.cpp`
      (Vulkan CPU-readout path and the generic non-hires path); the
      16bpp path is untouched.

      **Unrelated build-system bug found while soaking:**
      `runtime.cmake`'s `PSXRECOMP_RUNTIME_SOURCES` list was missing
      `netplay_snap_ring.c`, `netplay_state_digest.c`,
      `netplay_hash_confirm.c`, `netplay_input_hist.c`,
      `psx_netplay_rb.c`, and `psx_selfcheck.c` — a stale local edit
      unrelated to netplay work, likely from the `psxrecomp_add_game_runtime`
      scaffolding added alongside it. Every rebuild in this session up to
      this point silently linked STALE prebuilt `.o`s for those 6 units
      from before the edit (their sources hadn't changed, so ninja never
      flagged them) — nothing this session touched in those files, so no
      correctness impact, but a fresh `cmake` reconfigure (e.g. after
      editing `runtime.cmake` itself, or on a clean clone) would have
      dropped rollback netplay from the link entirely. Restored the six
      lines.

      **Post-A/B soak (2026-08-01) — "never-ending storm" traced to a
      round-start input-density burst, not a regression.** Full match
      soak with the `pads_differ` gate + pegged-streak off-guard + batched
      depth24 present: **zero core diverges, zero aborts**, both peers'
      episode lists byte-identical (34 commits, ticks 159 and
      1006..1400), full recovery to a sustained 60.0 fps for 450+ ticks
      until voluntary disconnect (`sdl_window_close` / `peer_disconnect`,
      not a stall). ICE selected `typ host` on both sides (confirmed LAN,
      not TURN) so link latency is ruled out. All 34 episodes land in one
      contiguous window starting the tick *right after* `FMV lockstep
      RELEASE sim=939` hands control back to normal 2-tick-delay
      prediction — i.e. the moment the round actually starts and both
      controllers begin moving/attacking at once. Timesync debt during
      that window stayed tiny (1–8 ms, cap is ~34 ms) and the pacer never
      neared the cap, so this is not a phase-skew problem the pacer could
      close faster — it is genuine simultaneous real input density from
      two controllers exceeding what a 2-tick delay can predict, and it
      self-resolves once the opening exchange calms down (~30 real
      seconds at 40–70 fps here). Distinct from earlier storms: no
      cascade, no divergence, no re-arming after it clears. Also
      reconfirmed the pre-existing FMV admit cost: 11–15 ms/f *admit*
      (network/lockstep wait) during FMV playback vs 6–11 ms/f guest
      (render, post Fix B) — the FMV frame-rate ceiling is now the
      lockstep admit wait, not GPU present cost. Next candidates, not yet
      tried: (a) `PSX_NET_DELAY=3` to cut mispredict frequency in exactly
      these high-density round-start windows (flagged 2026-07-31, never
      soaked); (b) shrink FMV lockstep per-tick admit wait now that
      render cost is off the table.

      **Residual gameplay determinism fork (open, 1 hit / 4000 ticks):**
      soak 2026-07-31 aborted once at sim=4034 (fight scene). Forensics:
      identical baseline (epoch=40 load=4033 core=fc779568 both peers),
      identical sealed pads (s0=ffff s1=ffbf), identical fin cycle count
      (2277677049), and clk/tim/av/cd/dma/spad/sio/aux all matched —
      only **cpu** (50d03880 vs 01ccbeaf) and **ram** (88a8e19f vs
      58f7e8de) forked. So clocks, IRQ timing, icache, and devices were
      bit-identical and the GPRs/RAM still diverged — a genuine
      non-clock replay fork (GTE state? uninitialised read? resim-order
      RAM write?). Needs a repro before digging; realign recovered it
      in-band (cooldown, no cascade).

      **Char-select storm = frozen agreed watermark (fixed):** with the
      30 ms grace, menu D-pad was clean (6 late edges all soak, was 29)
      — but char-select stormed with the peers' live digs IDENTICAL
      through the whole window. Cause: after a tip-hold commit set
      `g_agreed_valid`, the hard watermark only advanced at episode
      commits; hundreds of hc-confirmed clean live ticks were ignored,
      so every new edge reloaded from an ancient snap (soak:
      `mismatch=2135 load=1361`) and SPAN CAP crawled 24 ticks per
      episode while Live galloped ahead — permanently behind. Fix:
      `advance_agreed_watermark_from_hc()` — outside episodes/TipHold/
      pending loads, raise agreed to the newest hc-confirmed interval
      snap in the ring (span_lo collapses to the new watermark); called
      from `choose_load_tick` and the follower REFUSE gate (the follower
      must advance too or the initiator's valid frontier loads get
      NACKed). The historical false-confirm hazard behind the hard cap
      (TipHold Live invent FRAME_COMMITs with cleared PCs) is
      structurally gone — TipHold Live emits no FRAME_COMMITs and the
      core digest folds full CPU split + csv + icache.       Edges now load
      from ~one interval back and replay a handful of ticks (light
      tips), like the main-menu path.

      **Rematch black screen (fixed):** second lobby launch armed
      invent-lockstep at `sim=0` with `FMV media … depth24=0 mdec=0 xa=0
      fmv_pend=1 cd_reading=1 mode=e0 present_pend=1` and never printed
      another FPS line. Cause: `rb_fmv_media_active` treated
      `cdrom_fmv_stream_pending()` (`reading && mode&0x48`) as media —
      MotK boot/LoadExe sets XA mode bits on ordinary CD reads, so
      rematch locked invent before the first frame. Also soft-exit left
      `s_present_pending` / flush reentrancy sticky (`gpu_init` does not
      clear them). Fix: media = depth24 | mdec hysteresis | real XA
      stream only; `present_session_reset` clears deferred present;
      rb shutdown clears sticky BB PC + agreed watermark.

      **Episode cost cut: redundant full-bus digests off the per-tick hot
      path (2026-08-01).** `delay=3` and `delay=4` soaks showed the same
      "chain of small, back-to-back episodes" storm shape as `delay=2` —
      increasing the prediction buffer didn't help, so the fix target moved
      from network/prediction tuning to what a replayed tick actually
      *costs*. Found: `log_resim_tick_audit()`'s `"fin"` call (fired once
      per replayed tick, i.e. every tick of every episode, not just the
      last) unconditionally computed the *full* bus digest breakdown —
      `netplay_core_digest_parts` (a full 2 MiB RDRAM CRC plus GPR/COP0/GTE/
      clock/timer CRCs), a full 1 MiB VRAM CRC (`netplay_av_digest`), plus
      CD-ROM/aux/scratchpad/DMA/SIO digests — purely to print it. Worse, on
      the *last* tick of every episode, `enter_verify_at_tip()` then
      recomputed core/aux/av **again**, independently, for the real POST
      digest sent over the wire — so the commit tick paid for two full
      sweeps back to back. For a storm with dozens of 3–5 tick episodes
      this was megabytes of pure CRC32 work landing on exactly the ticks
      already busy re-simulating, compounding guest ms/frame during the
      exact window players felt as "stormy."
      Fix (no protocol/behavior change, diagnostics only):
      `log_resim_tick_audit()` now always takes the cheap path (register +
      clock/timer + RAM core digest only, same as it already did for the
      `"arm"` tag) for every replayed tick, `"fin"` included — the per-tick
      line still shows core/sealed-pad state for triage, just not the full
      breakdown. The full breakdown moved *into* `enter_verify_at_tip()`
      itself, computed exactly once, from the exact `dig_cpu` used for the
      real POST digest, and reused for both diagnostic prints (`rb post
      parts`, `rb post sent`) and the wire send — eliminating the double
      sweep on every commit. Net effect: full-bus digest cost drops from
      "every replayed tick, twice on the last one" to "once per episode."
      Also added a `replay=NN%%` field to the `PSX_NETPLAY_TIMING=1` `[FPS]`
      line (`psx_netplay_rb_take_replay_ticks()`) so a soak can directly
      confirm what fraction of the window was spent resimulating instead of
      inferring it from episode density in the raw log. Considered a
      `PSX_RB_VERBOSE_AUDIT` opt-out flag for the remaining once-per-episode
      full audit print, but skipped it: at episode-count frequency (not
      tick-count) the residual cost is already small, and every prior
      determinism bug in this doc (I-cache tags, csv, the Δ8-cycle fork)
      was found *from* this exact print, so defaulting it off risked
      trading a small, now-rare cost for reduced forensics on the next one.
      Not yet re-soaked; next step is confirming this shrinks the storm's
      guest ms/f without changing episode count/shape.

      **Re-soak (2026-08-01): confirmed genuine, self-clearing, input-density
      storm — plus two follow-up ideas investigated, one landed, one
      shelved.** `replay=NN%%` (new telemetry above) confirmed 50-60% of
      simulated ticks were spent in Replay during bursts of rapid main-menu
      D-pad taps, dropping to 0% between bursts — bursty and self-clearing,
      not a runaway cascade (zero core diverges/aborts across two full
      logs). Root-caused every rewind to a genuine Up/Down press/release
      edge (`wire rewind-request ... pub=ffff wire=ffbf` etc.) — confirmed
      as *inherent* to the current contract: `np_try_admit_rollback()`
      invents via hold-last **immediately, with zero wait**, whenever the
      local sim is ≤1 tick ahead of the remote's last-confirmed tip (the
      normal steady-state operating point on a fast LAN) — raising
      `delay`/`P` does not change this, since the code never even checks
      for the real value before inventing at that gap. A discrete edge is
      therefore always wrong under hold-last and always forces a rewind
      per the "button deltas always rewind" contract, independent of
      delay/prediction settings — investigated and shelved, not worth
      testing further.
      Also investigated **one-sided episodes** (let the follower answer
      POST from already-recorded live history instead of doing a full
      snapshot-load + replay, when its own simulation was never the one
      that mispredicted). Found a real obstacle: the live per-tick history
      ring (`s_part_ring` in `psx_netplay.c`) only refreshes VRAM/CD/aux
      every 32 ticks (`np_emit_frame_commit`'s `crumb` gate) specifically
      *to avoid* a permanent per-frame VRAM-CRC cost — so answering POST
      correctly for an arbitrary mismatched tick from stored history would
      need that data fresh every tick, trading a rare, bursty, ~50ms/f
      spike for a permanent ~1-2ms/f tax on every frame of ordinary play.
      Not a free win; shelved pending a cheaper way to get exact per-tick
      VRAM state (e.g. dirty-region VRAM CRC instead of full-frame).
      **What did land:** found `accept_peer_baseline()` unconditionally
      `fprintf`+`fflush`(stderr) on *every* received baseline packet,
      including every redundant copy of `RB_BASELINE_BURST=8` UDP
      retransmits sent from up to ~9 call sites across an episode's
      lifecycle — soak logs showed ~25 identical "rb peer baseline" lines
      (2932 across 117 episodes in one log) for state that only actually
      changes once per episode. Each `fflush` is a blocking syscall, firing
      repeatedly right on the ticks already busy resimulating. Fixed:
      the accepted state (`g_peer_baseline_ok`/digest/av/aux/ready) still
      updates unconditionally on every copy (protocol behavior unchanged),
      but the diagnostic print is now gated on the (epoch, load, dig_m,
      dig_a, dig_b, dig_c) tuple actually changing since the last log —
      pure I/O-volume reduction, zero effect on decision logic. Not yet
      re-soaked.

      **Engine-level digest cost (2026-08-01): Tier 1 landed, Tiers 2/3
      planned.** The above cuts were all "stop doing expensive work more
      often than needed" within the netplay layer. A separate axis is
      "make the expensive work itself cheaper" at the engine level,
      independent of rollback protocol logic — `netplay_av_digest()` CRCs
      the full 1 MiB VRAM buffer and `netplay_core_digest_parts()` CRCs the
      full 2 MiB RAM buffer from scratch every time either is called, with
      no dirty-region shortcut. Three tiers identified, ordered by
      risk/complexity:

      **Tier 1 — faster CRC32 (landed).** `runtime/src/crc32.c` used the
      textbook byte-at-a-time Sarwate table method: one table lookup per
      byte, each depending on the previous byte's output CRC (a serial
      dependency chain that stops the CPU overlapping consecutive steps).
      Replaced with **slicing-by-8**: 8 of those serially-dependent steps
      are restated as 8 *independent* table lookups (each depends only on
      an input byte) XORed together, letting the CPU run them
      out-of-order. Pure implementation swap — same polynomial, same
      bytes in, bit-identical output — verified by fuzzing the new
      implementation against the old one over 27k+ cases (exhaustive
      small lengths 0-40 from 32 offsets × 2 starting-CRC values, 20k
      random large buffers/offsets/starting-CRCs, 5k chained/incremental
      folds matching how `netplay_state_digest.c` folds `core`/`aux`
      together) before replacing it, since a subtle CRC bug here would be
      exactly the class of silent-corruption bug this doc has spent months
      chasing. Measured ~5x faster on both VRAM (1 MiB: 1.90ms → 0.38ms)
      and RAM (2 MiB: 3.78ms → 0.76ms) sized buffers on the dev host. No
      protocol/wire-format implication — the digest is an opaque
      comparison value never compared across differently-built peers, and
      both peers already have to run the identical binary.

      **Tier 2 — dirty-region digest (planned, not started).** Convert
      "CRC the whole buffer" into "CRC only what changed since the last
      digest." **VRAM is the tractable target**: audited every write to
      the 1 MiB `vram[]` array in `gpu.c` and found only **3 call sites**
      in the whole GPU emulation (CPU→VRAM transfer ~L2071, pixel
      plot/fill/line/poly ~L2930, depth24/16bpp write ~L4744) — a small,
      fully auditable surface where each site could mark a per-scanline
      or per-tile "touched" bitmap, and `netplay_av_digest()` would CRC
      only touched regions since the last checkpoint. This is also the
      cheaper alternative flagged above as the unblocker for "one-sided
      episodes" (exact per-tick VRAM state without a permanent per-frame
      CRC tax). **RAM is not a good near-term target the same way**: guest
      stores happen from essentially every recompiled `sw`/`sh`/`sb` site
      across the whole program — thousands of call sites, not 3 — so
      reliable RAM dirty-tracking means threading write-tracking through
      the recompiler/interpreter's store path itself, a much larger
      change. (Note: `dirty_ram_interp.h`'s existing per-page dirty
      bitmap is an unrelated subsystem — self-modifying-code detection for
      the static recompiler, CLAUDE.md Rule 18 — not a digest optimization
      and not reusable for this without its own risk analysis.) **Risk to
      respect:** a missed write site doesn't crash, it silently produces a
      digest that doesn't reflect true state — masking a real divergence
      or manufacturing a phantom one. Should ship with a "shadow-verify
      against a full recompute" mode during bring-up, not on faith.

      **Tier 3 — Merkle/hash-tree digest (planned, longer-horizon idea).**
      Replace the flat CRC with fixed-size blocks (e.g. 4 KiB) each hashed,
      plus a tree of hashes-of-hashes up to a root digest; a write
      invalidates only its own block's hash and that hash's ancestors
      (same underlying tracking problem as Tier 2, organized as a tree
      instead of a flat bitmap). Beyond the performance win, this buys
      **free divergence localization**: every forensic hunt in this doc
      (I-cache tags, the Δ8-cycle fork, the open cpu/ram-forked-but-
      everything-else-matched case) currently starts from "two 32-bit
      CRCs disagree" and requires manually narrowing via partition
      digests / sub-CRCs built up over sessions. A hash tree turns that
      narrowing into an O(log N) walk from the two disagreeing roots down
      to the differing leaf. Bigger lift than Tier 2; revisit after Tier 2
      ships and its audit tooling exists to reuse.
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

---

## 12. Portable policy: size "wait before invent" budgets from RTT, not local tick cadence

Found on MotK (2026-08-01), but the mistake is generic to any rollback
netcode with a "stall briefly, hoping the real remote row arrives, before
falling back to prediction" grace mechanism — worth checking for on every
future engine port before it ships. Written up here so it travels with the
rollback playbook rather than staying MotK-specific tribal knowledge.

**The mistake.** MotK's `np_invent_grace_stall()` (`runtime/src/psx_netplay.c`)
decides how long to block waiting for a genuinely-late remote row before
giving up and hold-last-inventing it. Its budget was `max(floor_ms,
2.5 × measured_local_tick_period)`, capped at 100ms — i.e. it used an EMA of
**how long this peer's own ticks have recently been taking** as a proxy for
"how patient should I be." That metric is the wrong one to feed a stall
gate: local tick period gets *longer* precisely when the engine is already
struggling (an active resim burst, a heavy scene, GC/OS jitter) — none of
which has anything to do with how long the *network* actually needs. The
result is a stall whose size floats up right when the system is already
behind, adding pure idle wait on top of the very slowdown it's reacting to.
Confirmed on a genuine direct-LAN connection (ICE selected `typ host`
candidates both sides, sub-ms RTT by any reasonable estimate): the
"grace" stall was measured landing at 70-90ms per occurrence — sized as if
this were a lossy WAN link, not because the link needed it, but because the
formula was reading the engine's own local distress as the loneliness of a
lonely remote packet.

**Why it partially masqueraded as "working as intended."** Lowering the
env-configured floor from 30ms to 5ms *did* measurably help (a same-scene
soak comparison showed replay-time share dropping ~31%→~25% and total
rewind episodes dropping ~113→~91 over an equal-length session) — so the
mechanism isn't useless, and the fix isn't "delete it." But the improvement
was partial and the storms remained *intermittent* rather than
disappearing, because the `2.5×` scaling term overrides a low floor the
moment tick cadence degrades even slightly — exactly the condition a live
storm creates. A fix that only tunes the floor constant will always leave
this residual: the floor is not the part doing the damage during a burst,
the scaling term is.

**The general policy for the next engine.** When a rollback implementation
needs a "how long do I wait for a late remote row before I predict it"
budget:

1. **Size it from measured network RTT, not from local simulation cadence.**
   Most rollback netcodes already track RTT for something else (auto
   delay/prediction tiering, connection-quality UI, etc.) — reuse that
   signal. A LAN connection with sub-ms RTT should get a budget in the
   low single-digit milliseconds; a WAN connection with 40ms RTT
   legitimately needs more. Local tick period answers "how is my engine
   doing", not "how is the link doing", and a wait-for-network gate should
   only ever ask the second question.
2. **Audit every adaptive stall/pacing input for the feedback question:**
   *"If this stall makes my own performance worse, does that get fed back
   into a bigger version of the same stall?"* Any signal derived from
   wall-clock frame/tick timing on the *same host that is currently
   stalling* is a candidate for this trap. It doesn't have to be obvious —
   here it was buried inside an otherwise-reasonable "scale grace to the
   observed tick rate so slow scenes don't false-positive" rationale that
   made sense in isolation but interacted badly with the stall's own
   side effect on that same tick rate.
3. **Keep "wait for late data" (invent grace) and "close phase skew"
   (timesync pacing/advantage throttle — MotK's `np_timesync_throttle`)
   as separate mechanisms with separate inputs**, even though both are
   "make the admit loop briefly wait." They answer different questions
   (is the data late vs. am I running ahead of my peer) and conflating
   their signals — e.g. letting both key off the same local-tick-cadence
   EMA — multiplies the risk of the feedback trap in (2) instead of
   isolating it to one place that's easy to reason about and cap.
4. **Give the mechanism a real ceiling independent of any adaptive term**,
   and log both the configured floor and the actual applied budget
   separately, so a soak can tell "the operator's floor setting" apart
   from "what the scaling term inflated it to" — this distinction is what
   made the MotK case diagnosable at all (see `psxrecomp: rb invent grace
   N ms` at startup vs. the per-stall budget computed in
   `np_invent_grace_stall`, which never prints and had to be reconstructed
   from the tick-period EMA logged by the neighboring `np_timesync_throttle`
   print).
5. **Validate any fix by holding episode/rewind count and correctness
   (zero `post core/aux diverge` aborts) constant while comparing perceived
   latency/`replay%`** — the goal is moving the budget's *source signal*,
   not just its magnitude; a naive floor-only tune (as done here first, for
   speed) is a legitimate stop-gap but should be labeled as partial, not
   as the fix, in whatever tracking doc records it.

**Status for MotK itself: implemented (2026-08-01), not yet re-soaked.**

- [x] Replace `np_invent_grace_stall`'s `2.5 × local_tick_ema` scaling term
      with a budget derived from measured session RTT, capped low. No
      existing live RTT signal existed anywhere in the netcode (checked
      `RNetSessionStats`, the ICE layer, and the lobby's one-shot pre-match
      LAN probe — none fit: the first two don't exist, the third measures a
      LAN-list endpoint that isn't necessarily this match's peer and doesn't
      update during play). Rather than add a wire-protocol change (new
      ping/pong message) or thread a value through the recomp-ui lobby UI
      (out of scope, and that submodule has its own in-flight uncommitted
      netplay-launch refactor — touching the same structs risked colliding
      with it), sourced RTT for free from data already being timestamped:
      `enter_verify_at_tip()` (`psx_netplay_rb.c`) already records
      `g_verify_wait_ms` when it sends this peer's POST, for the existing
      verify-timeout mechanism. Added `g_rb_rtt_ema_ms`, sampled at the
      point the peer's POST is accepted — `now - g_verify_wait_ms` when we
      sent ours first (the common case) is dominated by one-way transit of
      the peer's packet, not local compute, since both sides enter Verify
      around the same real-world moment (triggered by the same
      mismatch/rewind-request). EMA'd (3:1), sanity-bounded (samples over
      2s discarded as stale/bogus), exposed via
      `psx_netplay_rb_rtt_estimate_ms()`, reset to 0 on session
      start/shutdown (rematch) alongside the rest of the per-session RB
      state. This piggybacks on an existing, already-occurring exchange —
      no new wire message, no protocol version concern. `np_invent_grace_stall`
      now sizes its budget as `1.5 × rtt` once a sample exists (hard-capped
      at 60ms regardless of RTT), falling back to the old tick-cadence term
      (now 1.5 ticks / 40ms cap, down from 2.5 ticks / 100ms) only in the
      brief startup window before the first episode has round-tripped. The
      env-configured floor (`PSX_RB_INVENT_GRACE_MS`) default also dropped
      30→8ms, since the soak showed 5ms already worked and the floor now
      matters far less than the (fixed) scaling term.
- [x] Log the applied per-stall budget directly, not just the configured
      floor: `psxrecomp: rb invent grace budget=N ms (floor=F rtt=R
      tick_ema=T) slot=S wire=W`, printed once per tracked wire tick
      (deduped so a multi-ms stall doesn't spam one line per retry).
- [x] Re-soak after RTT-sourced invent grace (2026-08-01): steady-state
      improved (`replay%` ~7% overall / ~3.6% quiet post-FMV; invent budgets
      8–22 ms). Remaining cliffs were **not** invent-grace — they were the
      Verify POST-loss / tip-extend NACK storm in §13. Note: sampled "rtt"
      reads ~15–26 ms on direct LAN because the POST handshake wait includes
      peer Verify compute asymmetry, not pure UDP transit — still a better
      budget signal than local tick cadence, but not a true ping.

Touched: `runtime/src/psx_netplay_rb.c` (`g_rb_rtt_ema_ms`, sampling in the
peer-POST-accepted path, `psx_netplay_rb_rtt_estimate_ms()`, reset in
`psx_netplay_rb_shutdown()`), `runtime/include/psx_netplay_rb.h` (new
declaration), `runtime/src/psx_netplay.c` (`np_invent_grace_stall()`
budget formula + floor default + applied-budget log line). Builds clean,
no new lints. `np_timesync_throttle` intentionally left untouched per
policy point 3 above (separate mechanism, already has its own small fixed
cap and adaptive-off).

---

## 13. Verify POST loss → unilateral tip-hold → tip-extend NACK storm (2026-08-01)

**Symptoms (RTT-budget soak):** long quiet stretches at 60 fps / `replay≈0%`,
punctuated by cliffs (`8.1` then `2.7` fps, `admit=108–351 ms`). Not invent-
grace — budgets stayed at `22 ms (rtt=15)`.

**Root cause (matched-pair logs at tip=1256 and tip=1437):**

1. Both peers finish Replay and send identical POSTs.
2. Peer A receives B's POST first → `enter_tip_hold` → advances
   `agreed_through` to the tip → **stops retransmitting its own POST**
   (rexmit gate was `phase==Verify && local_post && !peer_post_ok`).
3. Peer B never got A's first POST (single UDP loss, even on direct LAN
   `typ host`) → sits in Verify until `RB_VERIFY_TIMEOUT_MS` (4s) →
   `ABORT — verify timeout (peer POST missing)` → realigns to last
   *confirmed* tip (e.g. 1425).
4. `RB_RESOLVED` that A sent on tip-hold was ignored for Verify exit —
   `take_rb_resolved` only called `rnet_rb_set_peer_convergence`.
5. A then opens tip-extend / fresh episodes with `load=` the unconfirmed tip
   (1437). B refuses: `follow REFUSED … past frontier=1425` + NACK.
6. A's NACK handler was plain `abort_episode` — **no watermark demotion,
   no cooldown** — so A immediately reopened `load=1437` → epochs 7..11
   back-to-back NACK abort storm (the `2.7 fps` cliff).

**Fixes landed:**

- [x] On `enter_tip_hold`, burst `RB_POST_BURST` (8) copies of local POST +
      RESOLVED *before* `clear_post_handshake`, so the lagging peer can still
      complete Verify after the winner leaves it.
- [x] On `take_rb_resolved` while stuck in Verify with a matching
      `g_post_target`, accept RESOLVED as the missing handshake half and
      `enter_tip_hold` (log: `rb verify accept peer RESOLVED`).
- [x] On peer follow-NACK: demote `agreed_through` below the refused load,
      drop a pin at/above that load, `schedule_live_realign` to a snap at
      the demoted tip, and `arm_rewind_cooldown_ticks` so tip-extend cannot
      immediately reopen the same unilateral tip.

**Re-soak checks:** no `verify timeout (peer POST missing)` on LAN; if one
still appears, the lagging peer should show `verify accept peer RESOLVED`
instead of a 4s hang; NACK lines should be followed by realign + cooldown,
not `begin epoch=N+1` with the same refused `load=` within milliseconds.

---

## 14. TipHold coalesce was pump-spin, not sim/wall time (2026-08-01)

**Symptoms (full-match soak after §13):** zero verify timeouts / NACKs /
aborts, avg ~57 fps — but **351** rewind-requests / **350** fresh light
episodes (~33 per 1k frames), avg `replay%` ~25%, and **0 tip-extends**.
Every press/release edge (gaps of 3–7 sim ticks) opened a new episode
immediately after tip-hold committed.

**Root cause:** MotK uses `tip_seal_slack = 0`, so Live invent stalls at the
sealed tip during TipHold. Finalize counted **admit-pump iterations** while
`sim >= tip`. The admit loop spins with `wait_recv(1)`, so "24 quiet frames"
elapsed in a few milliseconds of wall time — long before the paired release
edge arrived — then tip-hold committed and the next edge opened epoch N+1.

**Fixes landed:**

- [x] Replace pump-frame quiet counter with wall-clock
      `g_tip_hold_quiet_t0_ms`: finalize after `runway` frames at 60 Hz
      (`runway * 1000 / 60` ms, clamped 80–500). Tip-extend resets the timer
      so an active coalesce keeps the episode open.
- [x] TipHold admit: still never *invent* past tip+slack, but **do** advance
      when every remote wire row is already present — so tip-hold is not a
      hard freeze for the whole coalesce window, and sim-time finalize
      (`sim > tip_hold_until`) can fire when confirmed inputs walk forward.

**Re-soak checks:** `rb tip-extend epoch=` / `tip-extend FOLLOW` should
appear for press→release pairs; rewind-request count and avg `replay%`
should drop vs the 351 / 25% baseline; tip-hold→commit gaps should be
tens–hundreds of ms, not adjacent log lines.

---

## 15. Light-tip depth ceiling vs TipHold coalesce runway (2026-08-01)

**Post-§14 soak (LAN, menu nav, `delay=4`):** genuine improvement —
avg 59.1 fps, avg `replay%` 17% (down from ~25%), 91 rewinds / 6799 frames
(~13.4/1k, down from ~32.7/1k). But `tip-extend` fired only **once** in the
whole soak: the presses in this soak were spaced ~800 ms apart on average
(`avg gap=47` ticks @ 60 Hz), wider than the 24-tick (~400 ms) TipHold
coalesce window, so §14 rarely got a second edge to merge. The count/cost
drop here is mostly attributable to Tier‑1 CRC + the RTT-sized invent-grace
fix (§12), not coalescing — worth re-checking during dense-input gameplay
(combo strings), not just menu nav, where coalescing should matter more.

Every remaining rewind-request carries a real `pub` vs `wire` bit diff (a
genuine opponent input change), confirming these are not spurious — under a
hold-last/idle-invent predictor a real remote button transition necessarily
mispredicts. ICE also confirmed `typ host` on both sides (not relayed
through the coturn TURN server despite `force_turn=1` in the startup line,
which only affects candidate *gathering*, not which candidate wins), with a
genuine ~10–13 ms measured latency — this is real direct-LAN behavior, not a
transport misconfiguration.

**New finding — the two remaining depth knobs had drifted apart:**
`RNET_RB_LIGHT_TIP_MAX_DEPTH` (`recomp-net/include/recomp_net/rollback.h`)
is a library-wide constant of **16** ticks: episodes at or under that depth
skip the pre-Replay ready-ACK round trip (`rnet_rb_is_light_tip_candidate`).
§14 widened MotK's TipHold coalesce runway (`RB_MOTK_TIP_RUNWAY`) to **24**
so it could keep absorbing edges longer — but nothing widened the light-tip
ceiling to match. Depth-bucketing every `light=` episode in the soak showed
a perfectly clean split:

```
light=1 depths: 1,2,3 ... 16   (73 episodes, all <= 16)
light=0 depths: 17 (x5), 24 (x12)   (17 episodes, all > 16)
```

**17 of 90 episodes (19%) in this soak** grew past 16 ticks purely from
coalescing/span-cap headroom and lost light-tip eligibility, paying an
extra ready-ACK round trip on top of the POST-verify round trip both paths
already pay — and these are also the *deepest* rewinds, i.e. already the
most visually disruptive ones, now also getting the most expensive
handshake.

**Fix landed:** made the light-tip depth ceiling a per-session config field
instead of a single hardcoded constant, and set MotK's session to match its
TipHold runway:

- [x] `recomp-net`: added `RNetRbConfig.light_tip_max_depth` (0 → library
      default `RNET_RB_LIGHT_TIP_MAX_DEPTH`, clamped to 32 like
      `tip_runway`). Added `rnet_rb_get_light_tip_max_depth()` and
      `rnet_rb_is_light_tip_candidate_ex(load, target, resolved_through,
      max_depth)` (explicit-ceiling sibling of the existing
      `rnet_rb_is_light_tip_candidate`, which now just calls `_ex` with the
      library default so existing callers/tests are unaffected).
      `rnet_rb_recommend_light_tip` and `rnet_rb_begin_episode` now read
      `cfg.light_tip_max_depth` from the session instead of the bare
      constant. New unit tests in `rollback_episode_test.c` cover: default
      resolves to the constant, `_ex` with an explicit wider ceiling accepts
      a depth the plain wrapper rejects, and a session created with
      `light_tip_max_depth=24` correctly flags a depth-20 `begin_episode` as
      light (would not with the untouched default).
- [x] `psx_netplay_rb.c`: `psx_netplay_rb_start()` sets
      `cfg.light_tip_max_depth = RB_MOTK_TIP_RUNWAY` (24) so the two
      thresholds can't drift apart again. Both direct pre-flight call sites
      (initiator + follower `begin_episode` paths) switched from
      `rnet_rb_is_light_tip_candidate` to `rnet_rb_is_light_tip_candidate_ex`
      with `rnet_rb_get_light_tip_max_depth(g_rb)` so the precomputed flag
      matches what `rnet_rb_begin_episode` will compute internally.
- [x] `recomp-net` is a submodule (`psxrecomp/lib/recomp-net`, separate
      clone from the standalone `recomp-net` workspace repo on the same
      `feat/rollback` branch) — kept both copies of the touched files
      (`include/recomp_net/rollback.h`, `src/rollback/rnet_rollback.c`,
      `tests/rollback_episode_test.c`, `docs/rollback.md`) byte-identical.

**Not changed (deliberately):** did not just bump the library constant
`RNET_RB_LIGHT_TIP_MAX_DEPTH` itself — it's shared across every recomp-net
consumer, not just MotK, so widening it globally would be a cross-project
policy change, not a MotK tuning knob. The per-session `cfg` field lets
MotK opt in without affecting other hosts that still want the conservative
16-tick default.

**Re-soak checks:** `light=0` should now only appear for episodes that hit
`SPAN CAP` (depth > `RB_MOTK_TIP_RUNWAY`), not for every coalesced episode
past depth 16; the `light=1`/`light=0` split should track `RB_MOTK_TIP_RUNWAY`
(24) instead of the old library default (16). Re-run a soak with denser
input (active gameplay, not just menu nav) to see whether §14's coalescing
actually engages more (`tip-extend` count > 1) once presses land closer
together than the ~800 ms average seen here — that's the next real lever if
rewind volume is still the complaint, since the per-episode cost items
(Tier 1 CRC, RTT-sized invent grace, this light-tip alignment) are now
largely exhausted. After that, Tier 2 (dirty-region digest, documented
above) is the next compute-cost lever, and it disproportionately helps
exactly the deep/full-verify episodes since they still walk the full
2 MiB RAM + 1 MiB VRAM CRC at Verify regardless of light/full status.

---

## 16. Follow-NACK realign fork + HC re-poison + asymmetric light-tip (2026-08-01)

**Post-§15 soak (LAN, menu nav, `delay=4`):** §15 worked — **44/44** episodes
`light=1` including depths 17 and 24; avg `replay%` 13% (down from 17%);
~8.9 rewinds/1k frames. Then a late cliff: host `4.0 fps admit=235 ms`,
peer `5.9 fps`, `ready timeout (initiator never sent GO)`, `resim core
diverge sim=3841`, and peer `follow NACK load=3856` / `promote-no-resim
reason=cooldown` spam.

**Root cause chain (matched-pair at tip=3840):**

1. Both peers commit through **3840** (matched POST).
2. Peer HC-advances `agreed 3840→3856` and opens `epoch=59 load=3856`.
3. Host still has frontier **3840** → `follow REFUSED … past frontier=3840`
   + NACK. (The follow-up `snap missing` line from `send_follow_nack` is
   misleading — refuse reason was the frontier check.)
4. Peer's NACK handler demoted the watermark **and always
   `schedule_live_realign(demote)`**, even though the refused load snap was
   never applied (still in SealInputs). That rewound ~25 ticks of good
   matched Live on only the peer → silent core fork at demote+1, while
   host kept running Live ahead.
5. Cooldown after that realign made late wire `promote-no-resim` — peer
   could not correct invents during catch-up.
6. Peer then immediately re-ran `agreed ADVANCE 3840→3856` from **stale
   HC** (demote did not `netplay_hc_prime_after` / demote library
   `resolved_through`) — same poison, next episode.
7. Host's agreed stayed stuck at 3840 for ~229 ticks (peer digests no
   longer confirmed). Host finally opened
   `SPAN CAP mismatch=4069 load=3840 target→3864` as **light=1** and
   skipped ready-ACK without emitting GO.
8. Peer followed `load=3840` but its `resolved_through` was 3856 →
   **not** light → waited for initiator GO → `ready timeout` after 4s
   (the admit cliff). Host aborted with `resim core diverge sim=3841`
   while in Verify — leftover of the earlier Live fork.

**Fixes landed:**

- [x] **NACK keep-live when snap never applied:** capture
      `g_episode_snap_applied` before `abort_episode`; only
      `schedule_live_realign(demote)` when the refused load was actually
      applied. Otherwise log `NACK keep-live` and stay on the pre-episode
      Live tip (symmetric with the NACK-sender, who never left Live).
- [x] **Demote + prime HC:** on NACK, demote `g_agreed_through` /
      `g_agreed_span_lo`, call new `rnet_rb_demote_resolved_through(g_rb,
      demote)` (library watermark survives `session_reset` and
      `set_peer_convergence` only advances), and
      `netplay_hc_prime_after(hc, demote)` so live hash_confirm cannot
      immediately re-ADVANCE past the refused tip.
- [x] **Light-tip still emits ready/GO:** skip *waiting* for the RTT (the
      light-tip win) but still `send_baseline_burst(1, …)` so a peer that
      classified the same episode as non-light is not stranded until
      `RB_READY_TIMEOUT_MS`. Log line:
      `light-tip skip wait … (still emitted ready/GO for asymmetric peer)`.
- [x] Unit test for `rnet_rb_demote_resolved_through` (advance / demote /
      no-op / survives `session_reset`). Synced into both the MotK
      submodule and the standalone `recomp-net` checkout.

**Re-soak checks:** after a `follow REFUSED … past frontier` / NACK, look
for `NACK keep-live` (not an immediate realign to demote) when the
initiator never applied the load snap; no immediate
`agreed ADVANCE demote→refused_load` on the next pump; no
`ready timeout (initiator never sent GO)` when one side light-skips; host
and peer live digests at the same sim should stay matched after a NACK
instead of forking for hundreds of ticks then SPAN-CAP from a stale
frontier.

---

## 17. Tip-hold Live-walk held dpad → double menu inputs (2026-08-01)

**Post-§16 soak (LAN, menu nav, `delay=4`):** §16 healthy — 0 aborts /
NACKs / ready-timeouts / promote-no-resim; ~61 fps; `replay%` ~17%. UX
still broken: **dpad press/release felt like double inputs**, `tip-extend`
stayed at **0**, and every tip-hold→commit pair was still adjacent
(runway consumed as Live, not as wait-for-release).

**Root cause (not fake mismatches — every rewind was a real edge):**

1. **Tip-hold Live-walked held digital.** After a press episode POST-matched,
   tip-hold entered with `invent_slack=0` and `until = tip + 24`. Admit
   advanced past invent-cap whenever remote wire was present — on a held
   dpad that walked ~24 Live frames with the button still down, then
   finalized via `sim > tip_hold_until`. MotK menus key-repeat on hold →
   ~24 extra navigations from one physical press. The release arrived
   *after* tip-hold already committed → second episode; coalesce never
   saw it (`tip-extend=0`).
2. **Release episodes re-simulated several held frames** (often 6–10)
   before the release tip — another held run after the tip-hold walk.
3. **Ghost second release:** after a real release commit, predicted hist
   ahead still showed the button held (`pub=ffdf wire=ffff` again a few
   dozen ticks later). Tip-hold enter/finalize also did not
   `netplay_hc_prime_after(tip)`, so HC could re-`agreed ADVANCE` over
   invent-poisoned ticks (soak: release tip=1607 then
   `agreed ADVANCE 1607→1632`).

**Fixes landed:**

- [x] **Tip-hold invent-cap stall while digital held:** past `tip+slack`,
      admit advances only when every remote wire row is present **and**
      all pads (local + remote) are idle (`buttons == 0xFFFF`). Held →
      stall so wall-clock quiet / peek-ahead coalesce owns the runway.
- [x] **HC prime on tip-hold enter + finalize:**
      `netplay_hc_prime_after(hc, tip)` (+ `set_peer_convergence`) so
      invent-hold FRAME_COMMITs past the tip cannot immediately re-ADVANCE.
- [x] **Scrub-ahead on digital release promote:** rewrite predicted hist
      `release_tick+1..sim` to the released pad (`rb scrub-ahead release`).
- [x] **Tip-hold coalesce-ahead:** while tip-holding, peek wire
      `tip+1..tip+runway` (no predicted hist required); on first pad delta
      vs tip hist, promote the span and `tip_extend` (`rb tip-hold
      coalesce-ahead`). TipHold tip-extend now rereplays when
      `mismatch_tick > old_target` even if Live never left the tip
      (`sim == old_target`).
- [x] Getter `psx_netplay_rb_tip_runway()` for the host scan.

**Re-soak checks:** dpad tap should move the cursor once; look for
`tip-hold coalesce-ahead` / `tip-extend` on press→release within the
runway; `scrub-ahead release` after release promotes; no back-to-back
ghost `pub=held wire=ffff` release episodes; no
`agreed ADVANCE tip→tip+N` immediately after tip-hold enter/commit from
stale HC; tip-hold→commit should often be non-adjacent when a release
extends the tip.
