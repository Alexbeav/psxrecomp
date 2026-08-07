# Netplay with recomp-net (psxrecomp)

This is the **feature overview** for rollback-capable multiplayer in PSX
title projects that opt into `-DPSX_NETPLAY=ON` and ship `lib/recomp-net`
(plus lobby client). It describes what we are introducing for players and
title developers — not the internal soak checklist.

Deeper / implementation docs:

| Doc | Role |
|-----|------|
| [`NETPLAY_TOPOLOGY.md`](NETPLAY_TOPOLOGY.md) | Locked online vs LAN topology |
| [`ROLLBACK_MOTK_HOOKUP.md`](ROLLBACK_MOTK_HOOKUP.md) | MotK rollback integration notes |
| [`config_schema.md`](config_schema.md) `[netplay]` | Disc gate fields (`require_cue`, tracks, fingerprint) |
| [`lib/recomp-net/README.md`](../lib/recomp-net/README.md) | Transport / session library |
| Lobby server `WS_LOBBY.md` (recomp-net-server) | WebSocket lobby + SFU policy |

Opt-in at configure time: see [`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md)
(`PSX_NETPLAY`, `ENABLE_NETPLAY_IF_PRESENT`). The launcher NETPLAY button only
appears when the host advertises `GameInfo.netplay_supported`.

---

## Rollback netcode

Default match mode for opted-in titles is **rollback** (GGPO-style tip
prediction + resimulation), with **delay-sync** still available as an opt-out
(“Disable Rollback” in lobby settings, or `PSX_NET_MODE=delay`).

- Peers exchange pad tips over the session; missing remote input is
  predicted for a short horizon, then corrected by resim when the real tip
  arrives.
- Input delay (D) and prediction depth (P) can be auto-derived from RTT or
  set manually in the lobby.
- Savestate / snapshot rings and AV digests keep peers aligned; FMV / media
  paths use lockstep-friendly rules so digests stay meaningful.

Rollback is the product default for titles that ship it (e.g. MotK). Delay-sync
remains useful for debugging and for hosts that prefer fixed lag.

---

## Hybrid graphics (determinism + present quality)

Netplay must keep **guest simulation identical** across peers. Present quality
is separate.

When **OpenGL** is selected and netplay is active, psxrecomp can run a
**dual-raster** path:

| Layer | Scale | Role |
|-------|-------|------|
| Software rasterizer | **1×** (headless authority) | Deterministic VRAM / digests / rollback snaps |
| OpenGL | Player supersampling (e.g. 2×–4×) | Window present quality only |

Cost: extra CPU for the 1× SW pass. Benefit: peers can use different GL
settings without desyncing the sim. SW-only netplay forces scale 1 for the
whole path. Offline play keeps full supersampling with no dual-raster tax.

---

## Connectivity: ICE, TURN, SFU, LAN

### Online lobbies (WebSocket + SFU)

Online matches go through the lobby at **netplay.retcomm.net**
(WebSocket control plane). Match **UDP pad traffic** for online rooms uses an
**SFU star** (selective forwarding unit on the lobby server): every peer sends
to the SFU; the SFU fans out to other seats. Peers do **not** mesh each other
for game data online.

Current lobby policy prefers **always SFU** for online starts so CGNAT /
asymmetric NAT does not strand players on failed ICE attempts.

### ICE + TURN

**ICE** (with **TURN** relay via the project’s coturn on
`netplay.retcomm.net`) remains part of the stack for discovery,
signaling, and fallbacks. Builds that need ICE link libjuice with
`-DRNET_ENABLE_ICE=ON`. “Force TURN” in the UI can raise delay floors for
relay-heavy paths; it does not replace the SFU online architecture above.

### LAN / Direct IP (P2P star)

Without a lobby start (LAN or Direct IP):

- **2 players:** peer-to-peer UDP.
- **3+ players:** **host-as-relay** local star — the session host fans out
  tips to other seats (same star shape as online SFU, but the game host is
  the hub).

Sim authority: pad **slot 0** is the session host (`START`, state transfer).
Guests rearrange among seats **1..N−1**.

---

## Multitap + seat ceiling

| Item | Value |
|------|--------|
| Library / lobby / UI ceiling | **8** seats (`RNET_MAX_SLOTS`, dual SCPH-1070) |
| Per-title cap | `game.toml` `players` / `PSX_MAX_PLAYERS` (e.g. MotK=2, Bomberman Party=5) |

Rollback and delay-sync both carry multitap pad bytes. Offline and netplay
share the same multitap arming rules (game-started gate, `multitap_port` in
`game.toml`). Empty tap seats are fine — not every slot needs a device.

---

## Disc identity for multi-track titles

Netplay is **dump-strict** for titles that declare it. Peers must run the
**same playable image geometry**, not “any USA ISO.”

Typical `[netplay]` in `game.toml` (MotK example):

```toml
[netplay]
require_cue = true
required_tracks = 17
# optional: required_disc_fp = "…"   # TOC fingerprint
```

| Rule | Why |
|------|-----|
| Prefer / require **`.cue` + sibling `.bin` track files** | Multi-track Redump layout (data + XA/audio) |
| Exact **track count** when `required_tracks > 0` | Track-01-only dumps desync CDDA/XA vs full cues |
| Reject bare incomplete mounts when `require_cue` | Cue→bin fallback cannot invent missing tracks |
| Optional **TOC fingerprint** (`required_disc_fp`) | Same track count still wrong dump |

Generate & rebuild / prepare flows should point at the **`.cue`**, not a lone
`.bin`. See [`config_schema.md`](config_schema.md) and the release checklist in
[`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md).

---

## Related product pieces

- **Lobby UI** (recomp-ui): host/join, room settings, rollback toggles, FORCE
  TURN, player names — only when `PSX_NETPLAY` is on and the title advertises
  netplay.
- **VERSION / lobby match pin:** peers should run the same release pin so
  generated code and protocol stay compatible.
- **Mods:** netplay launches prefer a vanilla / synced mod plan (title-specific;
  see launcher `commit_netplay` hooks).

---

## Enabling for a new title

1. Vendor or submodule `recomp-net` under `psxrecomp/lib/recomp-net` (or set
   `RECOMP_NET_ROOT`).
2. Before `include(runtime.cmake)`: `set(PSX_NETPLAY ON CACHE BOOL … FORCE)`.
3. `psxrecomp_add_game_runtime(… ENABLE_NETPLAY_IF_PRESENT …)` with
   `MAX_PLAYERS` / `game.toml` `players` set correctly.
4. For ICE builds: `-DRNET_ENABLE_ICE=ON`.
5. Fill `[netplay]` disc gates for multi-track games.
6. Test LAN 2P, then online lobby; soak rollback + FMV if the title uses media.

This document will grow as N-way rollback confirmation, SFU soak on 5P titles,
and further ICE/SFU policy land.
