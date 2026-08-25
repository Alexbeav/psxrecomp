#!/usr/bin/env python3
"""
pad_trace.py — Sample the delivered pad state and classify input doubling.

One physical D-pad tap should produce exactly one contiguous press run, with
the left stick centred at 0x80 for the whole run (a real DualShock does not
deflect its stick when the D-pad is pressed).  This probe samples pad_status
as fast as the debug socket allows and reports what the runtime actually
presented to the game.

Findings it distinguishes:

  double_press   one tap produced two separate press runs -> the host event
                 layer emitted press/release/press (SDL hat + button, bounce).
  dpad_fold      the D-pad bit and a left-stick deflection were presented at
                 the same time -> a game reading both moves the cursor twice.
  analog_flip    the pad's analog flag changed mid-press -> the game saw a
                 controller type change while the button was down.
  long_hold      the run spanned enough emulated frames for the game's own
                 menu auto-repeat to fire.

Usage:
    python3 pad_trace.py --seconds 8
    python3 pad_trace.py --seconds 8 --json out.json

Tap the D-pad ONCE, cleanly, while the capture runs.
"""

import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

# Active-low PSX pad bits.
BUTTONS = {
    "select": 0x0001, "l3": 0x0002, "r3": 0x0004, "start": 0x0008,
    "up": 0x0010, "right": 0x0020, "down": 0x0040, "left": 0x0080,
    "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
}
DPAD = ("up", "right", "down", "left")

STICK_CENTRE = 0x80
# Anything past this is a deflection the game will act on.
STICK_DEADZONE = 0x30


def pressed(word, name):
    """Active-low: a clear bit means the button is held."""
    return (word & BUTTONS[name]) == 0


def stick_deflected(sticks, deadzone=STICK_DEADZONE):
    """True if the LEFT stick is off centre. sticks = [lx, ly, rx, ry]."""
    if len(sticks) < 2:
        return False
    return (abs(sticks[0] - STICK_CENTRE) > deadzone or
            abs(sticks[1] - STICK_CENTRE) > deadzone)


def find_runs(samples, name):
    """Contiguous spans where `name` is held. Returns list of (i0, i1) inclusive."""
    runs = []
    start = None
    for i, s in enumerate(samples):
        if pressed(s["buttons"], name):
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, len(samples) - 1))
    return runs


def describe_run(samples, name, run):
    i0, i1 = run
    a, b = samples[i0], samples[i1]
    folded = any(stick_deflected(samples[i]["sticks"]) for i in range(i0, i1 + 1))
    analogs = {bool(samples[i]["analog"]) for i in range(i0, i1 + 1)}
    return {
        "button": name,
        "t0": a["t"], "t1": b["t"],
        "ms": round((b["t"] - a["t"]) * 1000.0, 1),
        "frame0": a["frame"], "frame1": b["frame"],
        "frames": b["frame"] - a["frame"] + 1,
        "samples": i1 - i0 + 1,
        "stick_folded": folded,
        "analog": sorted(analogs),
    }


def analyze(samples, rebounce_ms=400.0, repeat_frames=16):
    """Turn a sample stream into runs + findings. Pure; unit-tested."""
    runs, findings = [], []
    for name in BUTTONS:
        spans = find_runs(samples, name)
        described = [describe_run(samples, name, r) for r in spans]
        runs.extend(described)

        for prev, cur in zip(described, described[1:]):
            gap = (cur["t0"] - prev["t1"]) * 1000.0
            if gap <= rebounce_ms:
                findings.append({
                    "kind": "double_press", "button": name,
                    "gap_ms": round(gap, 1),
                    "detail": "one tap presented as two press runs %.1f ms apart"
                              % gap,
                })
        for d in described:
            if d["stick_folded"] and name in DPAD:
                findings.append({
                    "kind": "dpad_fold", "button": name,
                    "detail": "D-pad bit and left-stick deflection presented "
                              "together; real hardware keeps the stick at 0x80",
                })
            if len(d["analog"]) > 1:
                findings.append({
                    "kind": "analog_flip", "button": name,
                    "detail": "pad analog flag changed during the press",
                })
            if d["frames"] >= repeat_frames:
                findings.append({
                    "kind": "long_hold", "button": name,
                    "frames": d["frames"],
                    "detail": "held %d emulated frames; menu auto-repeat may fire"
                              % d["frames"],
                })
    runs.sort(key=lambda r: r["t0"])
    return runs, findings


def capture(host, port, seconds):
    import debug_client
    sock = debug_client.connect(host, port)
    samples, t_end = [], time.time() + seconds
    while time.time() < t_end:
        st = debug_client.send_cmd(sock, {"cmd": "pad_status"})
        pg = debug_client.send_cmd(sock, {"cmd": "ping"})
        if not st or "slot0" not in st:
            continue
        s0 = st["slot0"]
        samples.append({
            "t": time.time(),
            "frame": int(pg.get("frame", -1)) if pg else -1,
            "buttons": int(s0["buttons"], 16),
            "sticks": [int(v) for v in s0.get("sticks", [128, 128, 128, 128])],
            "analog": bool(s0.get("analog", False)),
        })
    return samples


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--repeat-frames", type=int, default=16)
    ap.add_argument("--json", help="write raw samples + findings here")
    args = ap.parse_args()

    print("Capturing %.0fs — tap the D-pad ONCE, cleanly." % args.seconds)
    samples = capture(args.host, args.port, args.seconds)
    if not samples:
        print("No samples. Is the runtime up on port %d?" % args.port)
        return 1

    span = samples[-1]["t"] - samples[0]["t"]
    rate = len(samples) / span if span > 0 else 0.0
    frames = samples[-1]["frame"] - samples[0]["frame"]
    print("%d samples over %.1fs (%.0f Hz, %d emulated frames)"
          % (len(samples), span, rate, frames))

    runs, findings = analyze(samples, repeat_frames=args.repeat_frames)
    if not runs:
        print("No button press seen — nothing to classify.")
    for r in runs:
        print("  %-8s %6.1f ms  %3d frames  stick_folded=%s analog=%s"
              % (r["button"], r["ms"], r["frames"],
                 r["stick_folded"], r["analog"]))

    print()
    if not findings:
        print("No doubling mechanism found on the host side.")
        print("The press reached the pad once, cleanly — look downstream.")
    else:
        seen = set()
        for f in findings:
            key = (f["kind"], f["button"])
            if key in seen:
                continue
            seen.add(key)
            print("  [%s] %s: %s" % (f["kind"], f["button"], f["detail"]))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"samples": samples, "runs": runs,
                       "findings": findings}, fh, indent=2)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
