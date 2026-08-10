#!/usr/bin/env python3
"""stall_report.py - one-command interpreter/stall attribution for any title.

Answers, in a single pasteable report:

  1. Is the overlay cache actually BEING USED, or merely present on disk?
     (overlay_loader_status dispatch_native vs dispatch_interp_fallback,
     autocompile degraded state, shard pass/fail counts)
  2. When code IS interpreted, how badly is it thrashing the dispatcher?
     (dirty_ram_stats insns_run / blocks_run - instructions per dispatcher
     round-trip. The kernel window [0,0x10000) intentionally stays per-block,
     so a low ratio there is the known pathology; overlay-region code chains
     locally and runs orders of magnitude more instructions per round-trip.)
  3. WHERE did the wall-clock go, and was the guest even running?
     (starv_ring PC samples: the largest host_us gaps, each with the guest
     cycles that advanced across it. A stall where guest cycles advance
     slowly is CPU-bound execution; a stall where they barely advance at all
     is the emu thread not running - a completely different bug.)

PASSIVE RING CONSUMER - this is the whole point. It does not arm a trace, it
does not pause, and it does not single-step. Every number is either a
cumulative-since-boot counter (two snapshots -> a window by subtraction) or a
read of the always-on starvation ring, which has been recording since the
process started. You can therefore attach at any time, including long after
the interesting thing happened, and still see it.

Usage:

  # One-shot: cumulative state since boot, plus the worst stalls in the ring.
  py -3 tools/stall_report.py --port 4370 snap

  # Windowed: two snapshots N seconds apart. Play through the slow thing
  # during the window; every share/rate below is then the DELTA, not a
  # since-boot average that the boot sequence dominates.
  py -3 tools/stall_report.py --port 4370 run --secs 60 --out report.json

The runtime must be built with PSX_DEBUG_TOOLS=ON - a Release build ships no
TCP debug server and this tool will simply fail to connect.
"""
import argparse
import json
import socket
import sys
import time

X1_MCYC_PER_S = 33.8688      # NTSC PSX CPU clock: guest Mcyc per real second at 1x
KERNEL_WINDOW_END = 0x10000  # DIRTY_RAM_KERNEL_WINDOW_END


class Client:
    """One connection per command.

    The debug server aborts a second request on the same socket, so every
    command reconnects - same contract as tools/debug_client.py and
    tools/load_probe.py.
    """

    def __init__(self, host, port, timeout=8.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.next_id = 1

    def cmd(self, name, **params):
        req = {"cmd": name, "id": self.next_id}
        req.update(params)
        self.next_id += 1
        blob = (json.dumps(req) + "\n").encode()
        try:
            with socket.create_connection((self.host, self.port),
                                          timeout=self.timeout) as s:
                s.sendall(blob)
                chunks = []
                while True:
                    b = s.recv(65536)
                    if not b:
                        break
                    chunks.append(b)
                    if b.endswith(b"\n") or b"}" in b[-1:]:
                        # The server sends one JSON object then waits; try to
                        # parse what we have and stop as soon as it is whole.
                        try:
                            return json.loads(b"".join(chunks).decode().strip())
                        except json.JSONDecodeError:
                            continue
                raw = b"".join(chunks).decode().strip()
                return json.loads(raw) if raw else {"ok": False,
                                                    "error": "empty reply"}
        except (OSError, json.JSONDecodeError) as e:
            return {"ok": False, "error": f"{type(e).__name__}: {e}"}


# Commands whose whole payload we keep for the digest and the JSON artifact.
SNAP_COMMANDS = (
    ("overlay_loader_status", {}),
    ("autocompile_status",    {}),
    ("dirty_ram_stats",       {}),
    ("kernel_bless",          {}),
    ("phase_profile",         {}),
    ("frame_perf",            {}),
    ("phase_hot",             {"set": "native", "top": 20}),
    ("phase_hot",             {"set": "static", "top": 20}),
)


def snapshot(c):
    """Every cumulative counter this report uses, in one pass."""
    snap = {"wall": time.time()}
    for name, params in SNAP_COMMANDS:
        key = name if not params.get("set") else f'{name}:{params["set"]}'
        snap[key] = c.cmd(name, **params)
    return snap


def num(d, key, default=0):
    """Read a numeric field that may be absent or a hex string."""
    if not isinstance(d, dict):
        return default
    v = d.get(key, default)
    if isinstance(v, str):
        try:
            return int(v, 16) if v.startswith("0x") else int(v)
        except ValueError:
            return default
    return v if isinstance(v, (int, float)) else default


def delta(a, b, cmd, key):
    """b - a, or b's absolute value in snap mode.

    `a is None` means snap mode: there is no earlier snapshot, so the honest
    number is the cumulative one. Diffing a snapshot against itself would
    report zero for every counter and read as "nothing is happening", which is
    the opposite of the truth on a healthy warm-cache run.
    """
    if a is None:
        return num(b.get(cmd), key)
    return num(b.get(cmd), key) - num(a.get(cmd), key)


def pct(part, whole):
    return f"{100.0 * part / whole:6.2f}%" if whole else "     --"


def phot_map(reply):
    """phase_hot -> {addr: samples}, so two snapshots can be subtracted."""
    out = {}
    if isinstance(reply, dict):
        for e in reply.get("top", []) or []:
            out[e.get("addr", "?")] = num(e, "samples")
    return out


def phot_delta_lines(a, b, limit=12):
    m0, m1 = ({} if a is None else phot_map(a)), phot_map(b)
    diffs = [(addr, m1[addr] - m0.get(addr, 0)) for addr in m1]
    diffs = [(addr, d) for addr, d in diffs if d > 0]
    diffs.sort(key=lambda kv: -kv[1])
    total = sum(d for _, d in diffs) or 0
    return [f"      {addr}  {d:>12,}  {pct(d, total)}"
            for addr, d in diffs[:limit]]


def stalls_from_ring(c, count=2048, top=12):
    """Locate the largest wall-clock gaps in the always-on PC-sample ring.

    kind 15 (SR_EVT_PC_SAMPLE) entries carry (cyc, us, func). Consecutive
    deltas give both how much real time passed and how many guest cycles the
    CPU advanced across it, which is what separates the two failure shapes:

      guest advancing near 1x  -> the frame was long but the CPU was working
      guest advancing slowly   -> CPU-bound, executing something expensive
      guest barely advancing   -> the emu thread was not running at all
                                  (host-side block: I/O, driver, scheduler)
    """
    r = c.cmd("starv_ring", count=count, kind=15)
    if not r.get("ok", False):
        return None, r.get("error", "starv_ring failed"), []
    entries = r.get("entries", r.get("ring", [])) or []
    samples = []
    for e in entries:
        samples.append((num(e, "us"), num(e, "cyc"),
                        e.get("func", "?"), num(e, "in_exc")))
    if len(samples) < 2:
        return None, f"only {len(samples)} PC sample(s) in ring", []
    gaps = []
    for i in range(1, len(samples)):
        us0, cyc0, f0, _ = samples[i - 1]
        us1, cyc1, f1, x1 = samples[i]
        d_us, d_cyc = us1 - us0, cyc1 - cyc0
        if d_us <= 0:
            continue
        mcyc_s = (d_cyc / d_us) if d_us else 0.0      # cycles/us == Mcyc/s
        gaps.append({"ms": d_us / 1000.0, "guest_cyc": d_cyc,
                     "mcyc_per_s": mcyc_s,
                     "x_realtime": mcyc_s / X1_MCYC_PER_S,
                     "func_from": f0, "func_to": f1, "in_exc": x1})
    span_ms = (samples[-1][0] - samples[0][0]) / 1000.0
    gaps.sort(key=lambda g: -g["ms"])
    return span_ms, None, gaps[:top]


def verdict_lines(a, b):
    """The three questions, answered from the deltas."""
    out = []
    nat = delta(a, b, "overlay_loader_status", "dispatch_native")
    itp = delta(a, b, "overlay_loader_status", "dispatch_interp_fallback")
    stale = delta(a, b, "overlay_loader_status", "stale_blocked")
    tot = nat + itp
    out.append("  [1] IS THE OVERLAY CACHE BEING USED?")
    out.append(f"      dispatch_native          {nat:>14,}  {pct(nat, tot)}")
    out.append(f"      dispatch_interp_fallback {itp:>14,}  {pct(itp, tot)}")
    out.append(f"      stale_blocked            {stale:>14,}")
    if tot == 0:
        out.append("      >> NO DISPATCHES AT ALL in this window. Either the")
        out.append("         window caught nothing, or the loader is inactive.")
    elif nat == 0:
        out.append("      >> ZERO native dispatches. The cache is NOT being used,")
        out.append("         even if shard files exist on disk. Check the shard")
        out.append("         counts below before reading anything into the")
        out.append("         interpreter shares - this is the boring explanation")
        out.append("         and it has to be ruled out first.")
    elif itp > nat:
        out.append("      >> Interpreter fallback EXCEEDS native dispatch.")

    # autocompile_status nests every compile field under "compile" (see
    # handle_autocompile_status in debug_server.c). Reading them off the outer
    # object yields 0 for all of them and no degraded flag -- i.e. it reports
    # a healthy autocompile in exactly the case this section exists to catch.
    # Reported by @Alexbeav on PR #131.
    outer = b.get("autocompile_status", {}) or {}
    ac = outer.get("compile")
    out.append("")
    if not isinstance(ac, dict):
        out.append("      autocompile: NO `compile` OBJECT IN THE RESPONSE.")
        out.append("      >> Cannot report compile state. That is a protocol")
        out.append("         mismatch between this tool and the runtime -- NOT")
        out.append("         a healthy result. Do not read it as one.")
    else:
        configured = num(ac, "configured")
        out.append("      autocompile: "
                   f"configured={configured} state={ac.get('state','?')} "
                   f"runs={num(ac,'runs')} fails={num(ac,'fails')} "
                   f"consecutive_fails={num(ac,'consecutive_fails')} "
                   f"last_exit={num(ac,'last_exit')}")
        out.append("      shards:      "
                   f"ok={num(ac,'shard_ok')} fail={num(ac,'shard_fail')} "
                   f"skipped={num(ac,'shard_skipped')} "
                   f"fail_total={num(ac,'shard_fail_total')} "
                   f"result_seen={num(ac,'shard_result_seen')}")
        if ac.get("degraded"):
            out.append("      >> DEGRADED (interpreter-only): "
                       f"{ac.get('degraded_reason')}")
        if not configured:
            out.append("      >> autocompile is NOT CONFIGURED, so these zeros")
            out.append("         are expected rather than alarming. The cache")
            out.append("         then only exists if you built it ahead of time")
            out.append("         with compile_overlays.py.")
        elif not num(ac, "shard_result_seen"):
            out.append("      >> Configured, but no PSX_SHARD_RESULT has ever")
            out.append("         been parsed from the provider, so the shard")
            out.append("         counts above are MEANINGLESS rather than")
            out.append("         healthy -- the provider most likely never got")
            out.append("         far enough to emit one.")
        if num(ac, "shard_fail") > 0 or num(ac, "shard_fail_total") > 0:
            out.append("      >> Shards are FAILING to compile. That is the")
            out.append("         real cause, not a symptom. Run")
            out.append("         compile_overlays.py --check for the error.")

    dr = b.get("dirty_ram_stats", {})
    blocks = delta(a, b, "dirty_ram_stats", "blocks_run")
    insns = delta(a, b, "dirty_ram_stats", "insns_run")
    handoffs = delta(a, b, "dirty_ram_stats", "native_handoffs")
    out.append("")
    out.append("  [2] INTERPRETER DISPATCHER THRASH")
    out.append(f"      interpreted blocks       {blocks:>14,}")
    out.append(f"      interpreted instructions {insns:>14,}")
    out.append(f"      native handoffs          {handoffs:>14,}")
    if blocks:
        ratio = insns / blocks
        out.append(f"      >> {ratio:.1f} instructions per dispatcher round-trip")
        if ratio < 25:
            out.append("         That is per-block dispatch, not local chaining.")
            out.append("         Expected for the kernel window [0,0x10000), which")
            out.append("         hands control back every block by design - native")
            out.append("         coverage there comes from the overlay loader, never")
            out.append("         from interpreter chaining. If the hot PCs below are")
            out.append("         ABOVE 0x10000, that is a real bug worth reporting.")
        else:
            out.append("         Local chaining is working (overlay-region shape).")
    out.append(f"      text_native_blocked      "
               f"{num(dr,'text_native_blocked'):>14,}")
    out.append(f"      text_diverged_pages      "
               f"{num(dr,'text_diverged_pages'):>14,}")

    kb = b.get("kernel_bless", {})
    out.append(f"      kernel_bless: entries={num(kb,'entries')} "
               f"clean={num(kb,'clean')} mismatch={num(kb,'mismatch')} "
               f"native_hits={num(kb,'native_hits')}")
    out.append("        (a permanent `mismatch` count is EXPECTED - runtime-")
    out.append("         patched install stubs never verify and interpret")
    out.append("         forever by design. `clean` = 0 is the odd result.)")
    return out


def hot_pc_lines(a, b, limit=12):
    """Top interpreted PCs by instruction delta, from dirty_ram per_pc."""
    def per_pc(snap):
        out = {}
        for e in (snap.get("dirty_ram_stats", {}) or {}).get("per_pc", []) or []:
            out[e.get("pc", "?")] = (num(e, "insns"), num(e, "hits"),
                                     num(e, "entries"))
        return out
    m0, m1 = ({} if a is None else per_pc(a)), per_pc(b)
    rows = []
    for pc, (ins1, hit1, ent1) in m1.items():
        ins0, hit0, ent0 = m0.get(pc, (0, 0, 0))
        d_ins, d_hit = ins1 - ins0, hit1 - hit0
        if d_ins <= 0:
            continue
        rows.append((pc, d_ins, d_hit, (d_ins / d_hit) if d_hit else 0.0))
    rows.sort(key=lambda r: -r[1])
    total = sum(r[1] for r in rows) or 0
    out = []
    for pc, d_ins, d_hit, per in rows[:limit]:
        try:
            phys = int(pc, 16) & 0x1FFFFFFF
            zone = "KERNEL" if phys < KERNEL_WINDOW_END else "above-floor"
        except ValueError:
            zone = "?"
        out.append(f"      {pc}  insns={d_ins:>11,} {pct(d_ins, total)}  "
                   f"blocks={d_hit:>9,}  {per:6.1f} insn/entry  [{zone}]")
    return out


def report(a, b, span_ms, ring_err, gaps):
    L = []
    windowed = a is not None
    L.append("=" * 78)
    if windowed:
        L.append(f"stall_report - DELTAS over a {b['wall'] - a['wall']:.1f}s window")
    else:
        L.append("stall_report - CUMULATIVE SINCE BOOT (single snapshot)")
        L.append("  For shares that are not dominated by the boot sequence, use")
        L.append("  `run --secs N` and play through the slow thing in the window.")
    L.append("=" * 78)
    L.append("")
    L.extend(verdict_lines(a, b))

    L.append("")
    L.append("  [3] WHERE THE WALL-CLOCK WENT (always-on PC-sample ring)")
    if ring_err:
        L.append(f"      ring unavailable: {ring_err}")
    else:
        L.append(f"      ring span {span_ms:.0f} ms; largest gaps:")
        L.append("        gap_ms   guest_Mcyc/s   xRT   func_from -> func_to")
        for g in gaps:
            L.append(f"      {g['ms']:8.2f}   {g['mcyc_per_s']:9.2f}   "
                     f"{g['x_realtime']:4.2f}   {g['func_from']} -> "
                     f"{g['func_to']}"
                     + ("  [in exception]" if g["in_exc"] else ""))
        L.append("      xRT = guest speed vs real hardware across that gap.")
        L.append("        ~1.0  the CPU kept up; the gap is elsewhere (GPU,")
        L.append("              present, vsync) - check frame_perf.")
        L.append("        <<1.0 CPU-bound: it WAS executing, just slowly.")
        L.append("        ~0    the emu thread was not running at all. That is")
        L.append("              a host-side block, not slow emulation.")
        L.append("      NOTE func is the STATIC dispatch stamp. It localizes")
        L.append("      where the thread was; it does NOT prove that code was")
        L.append("      interpreted. Use [2] for interpretation evidence.")

    # phase_profile sums a ring of the last PHASE_RING_SECS seconds, so it is
    # a ROLLING WINDOW, not a since-boot total, and it does not align with this
    # report's own window. Label it as what it is -- calling it cumulative
    # invited exactly the wrong read. Reported by @Alexbeav on PR #131.
    pp = b.get("phase_profile", {}) or {}
    win_s = num(pp, "window_s")
    L.append("")
    L.append(f"  [4] PHASE SHARES (rolling {win_s or '?'}s window ending at the"
             " last snapshot)")
    L.append("      NOTE this window is the runtime's own and does NOT match")
    L.append("      the report window above. Treat it as an approximation of")
    L.append("      recent behaviour, not as evidence scoped to your session.")
    L.append(f"      samples        {num(pp, 'samples')}")
    for k in ("interp_share", "static_share", "native_share", "gpu_share",
              "other_share", "exc_share"):
        if k in pp:
            L.append(f"      {k:<14} {pp[k]}")

    L.append("")
    L.append("  [5] HOTTEST INTERPRETED PCs (delta)")
    lines = hot_pc_lines(a, b)
    L.extend(lines or ["      (none interpreted in this window)"])

    L.append("")
    L.append("  [6] phase_hot DELTAS")
    nat = phot_delta_lines(None if a is None else a.get("phase_hot:native", {}),
                           b.get("phase_hot:native", {}))
    sta = phot_delta_lines(None if a is None else a.get("phase_hot:static", {}),
                           b.get("phase_hot:static", {}))
    L.append("    native set:")
    L.extend(nat or ["      (empty - nothing NATIVE executed in this window."
                     " Not a broken query.)"])
    L.append("    static set:")
    L.extend(sta or ["      (empty)"])
    L.append("")
    L.append("=" * 78)
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True,
                    help="the title's debug-server port (PSX_DEBUG_TOOLS=ON)")
    ap.add_argument("--out", default=None,
                    help="also write the raw snapshots + gaps as JSON here")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("snap", help="one-shot, cumulative since boot")
    r = sub.add_parser("run", help="two snapshots N seconds apart (windowed)")
    r.add_argument("--secs", type=float, default=60.0)
    args = ap.parse_args()

    c = Client(args.host, args.port)
    ping = c.cmd("ping")
    if not ping.get("ok", False):
        print(f"cannot reach debug server on {args.host}:{args.port} "
              f"({ping.get('error')}).\n"
              "A Release build ships no TCP server - rebuild with "
              "PSX_DEBUG_TOOLS=ON.", file=sys.stderr)
        return 2

    if args.mode == "run":
        first = snapshot(c)
        print(f"monitoring {args.secs:.0f}s - play through the slow thing now "
              "(nothing is being armed; this only waits)...", file=sys.stderr)
        time.sleep(args.secs)
        second = snapshot(c)
    else:
        # snap: no earlier snapshot, so every counter is reported cumulative.
        # Passing the same snapshot as both ends would subtract it from itself
        # and print zeros, which reads as "nothing is happening".
        first, second = None, snapshot(c)

    span_ms, ring_err, gaps = stalls_from_ring(c)
    text = report(first, second, span_ms, ring_err, gaps)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump({"first": first, "second": second,
                       "ring_span_ms": span_ms, "ring_error": ring_err,
                       "gaps": gaps}, f, indent=2)
        print(f"\nraw JSON -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
