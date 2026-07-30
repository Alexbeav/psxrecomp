# MotK rollback hookup checklist

Status: **hooked (experimental)** · branch `feat/rollback-netplay` · depends on
`lib/recomp-net` @ `feat/rollback`

Today MotK / psxrecomp lobby netplay defaults to **rollback**:
`stage_local → poll_admit (invent within P) → guest frame → finish_frame`.
Missing remotes are invented (hold-last) only while
`wire_need ≤ highest_remote + P`; outside that window admit stalls (BattleShip
phase_lock). Late wire goes through the input contract; rewinds open an
`RNetRbSession` episode. Host **Disable Rollback** (or `PSX_NET_MODE=delay`)
forces classic delay-sync `try_admit`.

---

## 0. Ground rules (do not skip)

| Rule | Why |
|------|-----|
| Keep delay-sync `RNetSession` working behind a flag | Lobby / ICE / save-xfer stay useful; Disable Rollback opts out |
| Predicted rows promote only via `hash_confirm` (or host protect) | Library invariant; NULL `hash_confirm_promote` = always rewind |
| Digests must be bit-identical across peers for the same sealed inputs | Otherwise every invent becomes an episode storm |
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
- [x] Live snap rate-limit: every `PSX_NET_SNAP_INTERVAL` ticks (default **4**);
      resim still snaps every tick. Full `boot_state` ~1.3MB still dominates FPS
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
- [x] Storm calm: 12-tick rewind cooldown + promote-sweep after commit/realign;
      MotK digital release-only → promote (no episode); freeze `cdrom_advance`
      during Replay (CD IRQ timing was forking POST cores)
- [ ] Memory budget / thinner snap: optional strip CDROM/MDEC if MotK match
      path allows (further FPS headroom)
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
- [x] Remote invent = **hold-last** (neutral if no prior); stall when ahead of remote tip by > P
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
- [ ] Audit non-deterministic host clocks in sim path — soak-driven
- [ ] FMV / depth24: pause invent or prove digest stability during movies
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
7. **Thinner snap / FMV policy** — further FPS + movie digest stability  
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
- [ ] Forced remote stick correction triggers one episode and both digests match
      post-commit
- [ ] `hash_confirm` invent path shows promotes in diag without opening episodes
      when master hashes still agree
- [x] Delay-sync path (`PSX_NET_MODE=delay`) unchanged for lobby rematch / save xfer

Reference: `lib/recomp-net/docs/rollback.md`,
`include/recomp_net/rollback.h`, `include/recomp_net/input_contract.h`,
`tests/rollback_episode_test.c`.
