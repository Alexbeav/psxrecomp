#!/usr/bin/env python3
"""Self-test for tools/fp_identity.py (no runtime, disc or build needed).

`compare` runs on synthetic fingerprint dumps: column judging, the VBlank
straddle and FMV-quiet tolerances, truncated and mismatched frame sets, dumps
from a runtime that predates the ws/qc columns, and the exit codes. `run`
drives a fake runtime (this file, started with --fake-runtime) through the
real launch path: command templates, --runtime arguments, seeding, the
environment defaults and a taken port.

  python3 tools/tests/test_fp_identity.py        (or ctest -R fp_identity)
"""
import argparse
import importlib.util
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "fp_identity.py")


def load_tool():
    spec = importlib.util.spec_from_file_location("fp_identity", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# Sparse like a real run: the ring holds one entry per guest frame that
# snapshotted, not one per vblank.
FRAMES = [f for f in range(1, 2001) if f % 4 != 3]
LAST = 2000
MASK64 = (1 << 64) - 1


def term(frame):
    return (frame * 0x9E3779B97F4A7C15) & MASK64


def entry(frame, quiet=False, **override):
    """One ring entry. Cumulative columns grow with the frame number."""
    e = {"frame": frame, "wr": f"0x{frame * 7919:016x}",
         "pc": f"0x{frame * 104729:016x}", "wc": frame * 11,
         "ws": f"0x{(frame * 0x2545F4914F6CDD1D) & MASK64:016x}",
         "mmio": f"0x{frame * 31:016x}", "mc": frame * 3,
         "sp": f"0x{frame * 17:016x}", "sc": frame * 5,
         "qc": frame * 2 if quiet else 0, "cyc": frame * 564480}
    e.update(override)
    return e


def run_dump(frames=None, last=LAST, misses=0, quiet=False, env=True,
             **extra):
    dump = {"schema": 2, "build": "build", "frames": last,
            "miss_total": misses, "miss_unique": 1 if misses else 0,
            "ring_total": len(FRAMES) + 40, "ring_available": len(FRAMES) + 40,
            "entries": [entry(f, quiet) for f in
                        (FRAMES if frames is None else frames)]}
    if env:
        dump["env"] = {"PSX_DEBUG_FMV_QUIET": "1" if quiet else "0",
                       "PSX_OVERLAY_AUTOCOMPILE_OFF": "1"}
    dump.update(extra)
    return dump


def bump(dump, index, stop=None, **delta):
    """Add delta to wc/qc/ws of entries[index:stop] (ws modulo 2^64)."""
    for e in dump["entries"][index:stop]:
        for k, v in delta.items():
            if k == "ws":
                e[k] = f"0x{(int(e[k], 16) + v) & MASK64:016x}"
            else:
                e[k] += v
    return dump


def at(i):
    return FRAMES[i]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def tool(self, *args, timeout=120):
        proc = subprocess.run([sys.executable, TOOL, *args],
                              capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout + proc.stderr

    def compare(self, a, b, *flags):
        paths = []
        for name, dump in (("a.json", a), ("b.json", b)):
            path = os.path.join(self.dir, name)
            with open(path, "w") as f:
                json.dump(dump, f)
            paths.append(path)
        return self.tool("compare", *paths, *flags)

    def assertVerdict(self, result, code, verdict, *details):
        status, out = result
        self.assertEqual(status, code, out)
        verdicts = [line.split(":")[0] for line in out.splitlines()
                    if line.split(":")[0] in
                    ("IDENTICAL", "MISMATCH", "INCOMPLETE", "MISSES")]
        self.assertEqual(verdicts, [verdict], out)
        for detail in details:
            self.assertIn(detail, out)
        return out


class FrameSetTest(Base):
    """The frame-set and miss checks carried over from the R4 tool."""

    def test_identical(self):
        out = self.assertVerdict(
            self.compare(run_dump(), run_dump()), 0, "IDENTICAL",
            f"IDENTICAL: {len(FRAMES)} frames (1..2000) agree on "
            f"cyc,mmio,mc,sp,sc,wc,ws,qc; no tolerance applied",
            "locator wr: equal at every shared frame",
            "FMV-quiet off in both runs")
        self.assertNotIn("tolerated:", out)

    def test_truncated_candidate_with_misses_is_not_identical(self):
        # A candidate cut to its first 100 entries, with miss_total=5, once
        # reported IDENTICAL over the shared frames.
        cut = run_dump(misses=5)
        cut["entries"] = cut["entries"][:100]
        self.assertVerdict(self.compare(run_dump(), cut), 3, "INCOMPLETE",
                           "B lacks", "outside its range 1..133",
                           "B had 5 dispatch misses")

    def test_truncated_either_side(self):
        short = FRAMES[:500]
        self.assertVerdict(self.compare(run_dump(), run_dump(short)), 3,
                           "INCOMPLETE", "B lacks 1000 of A's frames")
        self.assertVerdict(self.compare(run_dump(short), run_dump()), 3,
                           "INCOMPLETE", "A lacks 1000 of B's frames")

    def test_missing_head(self):
        self.assertVerdict(self.compare(run_dump(), run_dump(FRAMES[10:])), 3,
                           "INCOMPLETE", "B lacks 10 of A's frames",
                           "1..2, 4..6")

    def test_different_window(self):
        self.assertVerdict(self.compare(run_dump(), run_dump(last=LAST + 1)), 3,
                           "INCOMPLETE", "A at 2000, B at 2001")

    def test_interior_frame_set_difference_is_a_mismatch(self):
        holed = [f for f in FRAMES if f != 1000]
        self.assertVerdict(self.compare(run_dump(), run_dump(holed)), 1,
                           "MISMATCH", "fingerprinted by A are missing inside "
                           "B's range: 1000")

    def test_divergence_and_truncation_report_both(self):
        b = run_dump(FRAMES[:500])
        b["entries"][10]["mmio"] = "0x0"
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(10)}",
                           "B lacks 1000 of A's frames")

    def test_dispatch_misses(self):
        self.assertVerdict(self.compare(run_dump(misses=2), run_dump()), 4,
                           "MISSES", "A had 2 dispatch misses")

    def test_unknown_miss_count(self):
        self.assertVerdict(self.compare(run_dump(), run_dump(miss_total=None)),
                           3, "INCOMPLETE", "B has no dispatch-miss count")

    def test_wrapped_ring(self):
        wrapped = run_dump(ring_total=40000, ring_available=32768)
        self.assertVerdict(self.compare(run_dump(), wrapped), 3, "INCOMPLETE",
                           "B's fingerprint ring wrapped")

    def test_dump_without_ring_counts_still_compares(self):
        old = run_dump()
        del old["ring_total"], old["ring_available"]
        self.assertVerdict(self.compare(old, run_dump()), 0, "IDENTICAL")

    def test_empty_run(self):
        self.assertVerdict(self.compare(run_dump(), run_dump([])), 3,
                           "INCOMPLETE", "B has no fingerprint entries",
                           "no shared frames")


class ColumnTest(Base):
    """Judge columns fail a run; locator columns only say where runs part."""

    def test_every_judge_column_is_judged(self):
        for col, value in (("cyc", 1), ("mmio", "0x1"), ("mc", 1),
                           ("sp", "0x1"), ("sc", 1), ("qc", 99),
                           ("ws", "0x1")):
            with self.subTest(col=col):
                b = run_dump()
                b["entries"][300][col] = value
                self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                                   f"DIVERGE at frame {at(300)}: judge "
                                   f"columns ['{col}']",
                                   f"record_frame {at(300)}")

    def test_ws_value_fork_with_equal_count_is_not_a_straddle(self):
        # Same number of writes, different values: a real fork.
        b = bump(run_dump(), 300, 301, ws=12345)
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns ['ws']")

    def test_locators_are_reported_not_judged(self):
        b = run_dump()
        for e in b["entries"][700:]:
            e["wr"], e["pc"] = "0x1", "0x2"
        self.assertVerdict(self.compare(run_dump(), b), 0, "IDENTICAL",
                           f"locator wr: first differs at frame {at(700)} "
                           f"({len(FRAMES) - 700} of {len(FRAMES)} frames); "
                           f"not a failure",
                           f"locator pc: first differs at frame {at(700)}")

    def test_mismatch_reports_locators_too(self):
        b = run_dump()
        b["entries"][500]["wr"] = "0x1"
        b["entries"][600]["sc"] = 0
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(600)}: judge columns ['sc']",
                           f"locator wr: first differs at frame {at(500)}")

    def test_hex_and_integer_spellings_compare_equal(self):
        b = run_dump()
        for e in b["entries"]:
            e["mmio"] = int(e["mmio"], 16)
            e["wc"] = str(e["wc"])
        self.assertVerdict(self.compare(run_dump(), b), 0, "IDENTICAL")

    def test_older_runtime_without_ws_qc(self):
        # Dumps from a runtime before psxrecomp #420: no ws/qc, no env.
        old = run_dump(env=False)
        for e in old["entries"]:
            del e["ws"], e["qc"]
        self.assertVerdict(self.compare(old, json.loads(json.dumps(old))), 3,
                           "INCOMPLETE", "A lacks judge column(s) ws,qc",
                           "B lacks judge column(s) ws,qc",
                           "judged on cyc,mmio,mc,sp,sc,wc")

    def test_older_runtime_on_one_side_only(self):
        old = run_dump()
        for e in old["entries"]:
            del e["ws"]
        self.assertVerdict(self.compare(run_dump(), old), 3, "INCOMPLETE",
                           "B lacks judge column(s) ws")

    def test_older_runtime_still_reports_divergence(self):
        old = run_dump(env=False)
        for e in old["entries"]:
            del e["ws"], e["qc"]
        b = json.loads(json.dumps(old))
        b["entries"][40]["mc"] = 0
        self.assertVerdict(self.compare(old, b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(40)}: judge columns ['mc']",
                           "lacks judge column(s) ws,qc")


class StraddleTest(Base):
    """The one-write VBlank straddle of docs/TCP_COMMANDS.md."""

    def straddled(self, index, writes=1, sign=1):
        return bump(run_dump(), index, index + 1, wc=sign * writes,
                    ws=sign * term(index))

    def test_one_write_straddle_is_tolerated_and_reported(self):
        b = self.straddled(300)
        b = bump(b, 900, 901, wc=-1, ws=-term(900))
        self.assertVerdict(self.compare(run_dump(), b), 0, "IDENTICAL",
                           "tolerated 2 VBlank straddle(s)",
                           "tolerated: 2 VBlank straddle frame(s) (ws/wc off "
                           f"by <= 1 write(s), re-converged at the next "
                           f"frame): {at(300)}, {at(900)}")

    def test_strict_rejects_a_straddle(self):
        self.assertVerdict(self.compare(run_dump(), self.straddled(300),
                                        "--strict"), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns "
                           f"['wc', 'ws'] (the one-write VBlank straddle "
                           f"pattern; --strict rejects it)")

    def test_straddle_that_does_not_reconverge(self):
        b = bump(run_dump(), 300, None, wc=1, ws=term(300))
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns "
                           f"['wc', 'ws'] (a 1-write straddle pattern, but "
                           f"frame {at(301)} did not re-converge)")

    def test_two_write_straddle_needs_a_wider_limit(self):
        b = self.straddled(300, writes=2)
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           "2 writes moved, more than --straddle-writes 1")
        self.assertVerdict(self.compare(run_dump(), b, "--straddle-writes",
                                        "2"), 0, "IDENTICAL",
                           "ws/wc off by <= 2 write(s)")

    def test_straddle_with_another_column_is_a_divergence(self):
        b = self.straddled(300)
        b["entries"][300]["mmio"] = "0x5"
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns "
                           f"['mmio', 'wc', 'ws']")

    def test_next_frame_diverging_is_not_a_straddle(self):
        b = self.straddled(300)
        b["entries"][301]["cyc"] = 1
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}",
                           f"frame {at(301)} did not re-converge")

    def test_straddle_on_the_last_frame_is_unconfirmed(self):
        b = self.straddled(len(FRAMES) - 1)
        self.assertVerdict(self.compare(run_dump(), b), 3, "INCOMPLETE",
                           f"frame {FRAMES[-1]}, the last shared frame",
                           "a 1-write VBlank straddle",
                           "rerun with a larger --frames")

    def test_bad_straddle_limit_is_a_usage_error(self):
        status, out = self.compare(run_dump(), run_dump(),
                                   "--straddle-writes", "-1")
        self.assertEqual(status, 2, out)


class FmvQuietTest(Base):
    """With FMV-quiet on, a straddle can move a write between wc and qc."""

    def shifted(self, index, **extra):
        # From `index` on, B counted one write in wc that A counted in qc.
        return bump(run_dump(quiet=True, **extra), index, None, wc=1, qc=-1,
                    ws=term(index))

    def test_quiet_shift_is_tolerated_with_carried_offsets(self):
        self.assertVerdict(self.compare(run_dump(quiet=True),
                                        self.shifted(300)), 0, "IDENTICAL",
                           "tolerated 1 FMV-quiet shift(s)",
                           f"FMV-quiet shift(s) (wc+qc agreed, offsets "
                           f"carried forward): {at(300)}",
                           "wc+qc must agree")

    def test_quiet_shift_after_a_quiet_straddle(self):
        # A's write lands in a quiet frame (qc) one frame before B's, which
        # lands in the next, non-quiet frame (wc).
        b = run_dump(quiet=True)
        bump(b, 300, 301, qc=-1)
        bump(b, 301, None, wc=1, qc=-1, ws=term(301))
        self.assertVerdict(self.compare(run_dump(quiet=True), b), 0,
                           "IDENTICAL", "1 VBlank straddle(s) and 1 FMV-quiet "
                           "shift(s)")

    def test_ws_drift_after_a_shift_is_a_divergence(self):
        b = bump(self.shifted(300), 800, None, ws=77)
        self.assertVerdict(self.compare(run_dump(quiet=True), b), 1,
                           "MISMATCH", f"DIVERGE at frame {at(800)}: judge "
                           f"columns ['ws']", "after 1 FMV-quiet shift(s)")

    def test_wc_plus_qc_must_agree(self):
        b = bump(run_dump(quiet=True), 300, None, qc=1)
        self.assertVerdict(self.compare(run_dump(quiet=True), b), 1,
                           "MISMATCH", f"DIVERGE at frame {at(300)}: judge "
                           f"columns ['qc']")

    def test_shift_is_not_tolerated_with_quiet_off(self):
        # The same pattern with FMV-quiet off in both runs is a fork.
        a = run_dump()
        b = bump(run_dump(), 300, None, wc=1, qc=-1, ws=term(300))
        self.assertVerdict(self.compare(a, b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns "
                           f"['wc', 'qc', 'ws']")

    def test_different_quiet_settings_are_incomplete(self):
        a = run_dump(quiet=True)
        b = run_dump(quiet=True)
        b["env"]["PSX_DEBUG_FMV_QUIET"] = "0"
        self.assertVerdict(self.compare(a, b), 3, "INCOMPLETE",
                           "different FMV-quiet settings (A on, B off)")

    def test_mixed_settings_with_quiet_frames_are_not_judged(self):
        # As on R4: the quiet run skips the fingerprint on quiet frames and
        # counts their writes (MMIO too) in qc, so frames and columns part
        # from the first quiet frame. That is the setting, not a fork.
        a = run_dump()
        quiet_frames = set(FRAMES[300:400])
        b = run_dump([f for f in FRAMES if f not in quiet_frames])
        b["env"]["PSX_DEBUG_FMV_QUIET"] = "1"
        for e in b["entries"]:
            if e["frame"] > FRAMES[300]:
                e["qc"], e["mmio"], e["mc"] = 50, "0x1", 0
        out = self.assertVerdict(
            self.compare(a, b), 3, "INCOMPLETE",
            "not judged (FMV-quiet settings differ)",
            "different FMV-quiet settings (A off, B on)",
            "B counted quiet writes (qc > 0)",
            f"the frame sets differ ({len(FRAMES)} and {len(FRAMES) - 100} "
            f"frames fingerprinted)",
            "Rerun both with the same setting",
            "locator wr: not compared")
        for artefact in ("DIVERGE", "record_frame", "missing inside",
                         "tolerated:"):
            self.assertNotIn(artefact, out)

    def test_mixed_settings_without_qc_column_are_not_judged(self):
        a, b = run_dump(), run_dump(env=False)
        for dump in (a, b):
            for e in dump["entries"]:
                del e["qc"]
        self.assertVerdict(self.compare(a, b), 3, "INCOMPLETE",
                           "qc is not reported, so quiet frames cannot be "
                           "ruled out")

    def test_mixed_settings_without_a_quiet_frame_are_judged(self):
        # FMV-quiet on in B, but no MDEC decode: nothing was kept quiet.
        b = run_dump()
        b["env"]["PSX_DEBUG_FMV_QUIET"] = "1"
        self.assertVerdict(self.compare(run_dump(), b), 0, "IDENTICAL",
                           "FMV-quiet settings differ (A off, B on), but no "
                           "quiet frame occurred")
        b["entries"][300]["cyc"] = 1
        self.assertVerdict(self.compare(run_dump(), b), 1, "MISMATCH",
                           f"DIVERGE at frame {at(300)}: judge columns "
                           f"['cyc']")

    def test_unrecorded_quiet_setting_uses_the_quiet_rule(self):
        a, b = run_dump(quiet=True, env=False), self.shifted(300, env=False)
        self.assertVerdict(self.compare(a, b), 0, "IDENTICAL",
                           "not recorded (runtime default: on)",
                           "tolerated 1 FMV-quiet shift(s)")


class UsageTest(Base):
    def test_compare_needs_two_dumps(self):
        status, out = self.tool("compare", "only-one")
        self.assertEqual(status, 2, out)

    def test_unreadable_dump(self):
        bad = os.path.join(self.dir, "bad.json")
        with open(bad, "w") as f:
            f.write("{not json")
        status, out = self.tool("compare", bad, bad)
        self.assertEqual(status, 2, out)
        self.assertIn("cannot read", out)

    def test_no_command(self):
        status, _ = self.tool()
        self.assertEqual(status, 2)

    def test_run_needs_a_launcher(self):
        status, out = self.tool("run", os.path.join(self.dir, "x.json"))
        self.assertEqual(status, 2, out)
        self.assertIn("--launch", out)

    def test_windowed_with_a_literal_headless(self):
        status, out = self.tool("run", "x.json", "--launch",
                                "game --headless", "--windowed")
        self.assertEqual(status, 2, out)
        self.assertIn("{headless}", out)

    def test_unknown_placeholder(self):
        status, out = self.tool("run", "x.json", "--launch", "game {bogus}")
        self.assertEqual(status, 2, out)
        self.assertIn("bad placeholder", out)

    def test_seed_without_state_dir(self):
        status, out = self.tool("run", "x.json", "--launch", "game {port}",
                                "--seed", self.dir)
        self.assertEqual(status, 2, out)
        self.assertIn("--seed needs --state-dir", out)


class LaunchTemplateTest(unittest.TestCase):
    """How --launch is split into arguments (no runtime needed)."""

    @classmethod
    def setUpClass(cls):
        cls.fp = load_tool()

    def argv(self, template, windows, **extra):
        args = argparse.Namespace(launch=template, build="build", port=4781,
                                  windowed=False, extra=[], **extra)
        return self.fp.launch_argv(args, None, windows=windows)

    def test_windows_paths_keep_their_backslashes(self):
        self.assertEqual(
            self.argv(r"C:\psx\build\game-runtime.exe --game "
                      r"build\game.toml --debug-port {port} {headless}", True),
            [r"C:\psx\build\game-runtime.exe", "--game", r"build\game.toml",
             "--debug-port", "4781", "--headless"])

    def test_windows_quoted_path_with_spaces(self):
        self.assertEqual(
            self.argv(r'"C:\Program Files\psx\game.exe" --debug-port {port}',
                      True),
            [r"C:\Program Files\psx\game.exe", "--debug-port", "4781"])

    def test_posix_keeps_shell_escapes(self):
        self.assertEqual(
            self.argv(r"tools/run\ game.sh '{build}' --debug-port {port}",
                      False),
            ["tools/run game.sh", "build", "--debug-port", "4781"])

    def test_unbalanced_quote_is_a_usage_error(self):
        for windows in (False, True):
            with self.subTest(windows=windows):
                with self.assertRaises(self.fp.UsageError):
                    self.argv('"C:\\psx\\game.exe --debug-port {port}',
                              windows)


# ---- fake runtime ------------------------------------------------------------

def fake_runtime(argv):
    """A debug server that answers the four commands `run` sends."""
    port = int(argv[argv.index("--debug-port") + 1])
    report = argv[argv.index("--report") + 1]
    with open(report, "w") as f:
        json.dump({"argv": argv, "env": {
            k: os.environ.get(k) for k in
            ("PSX_DEBUG_FMV_QUIET", "PSX_OVERLAY_AUTOCOMPILE_OFF", "FAKE_X")},
            "cwd": os.getcwd()}, f)
    if "--die" in argv:
        sys.exit(7)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    start = time.time()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        conn, _ = srv.accept()
        with conn:
            line = b""
            while not line.endswith(b"\n"):
                chunk = conn.recv(65536)
                if not chunk:
                    break
                line += chunk
            req = json.loads(line or b"{}")
            frame = int((time.time() - start) * 40000)
            cmd = req.get("cmd")
            if "--crash-on" in argv and cmd == argv[argv.index("--crash-on")
                                                    + 1]:
                os._exit(9)                 # dies mid-request, no reply
            if "--hang-up-on" in argv and cmd == argv[
                    argv.index("--hang-up-on") + 1]:
                continue                    # closes the connection, no reply
            if cmd == "frame_fingerprint":
                lo, hi = req.get("frame_lo", 0), req.get("frame_hi", frame)
                entries = [entry(f) for f in FRAMES if lo <= f <= hi]
                reply = {"ok": True, "total": len(FRAMES),
                         "available": len(FRAMES), "entries": entries}
            elif cmd == "dispatch_stats":
                reply = {"ok": True, "miss_total": 0, "miss_unique": 0}
            else:
                reply = {"ok": True, "frame": frame}
            conn.sendall(json.dumps(reply).encode() + b"\n")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@unittest.skipIf(os.name == "nt", "the fake runtime is a POSIX launch")
class RunTest(Base):
    def launch(self, out, *args):
        return self.tool("run", os.path.join(self.dir, out), "--frames",
                         str(LAST), "--boot-timeout", "30", "--stall", "30",
                         "--timeout", "60", *args)

    def template(self, report, extra=""):
        return (f"{shlex.quote(sys.executable)} {shlex.quote(__file__)} "
                f"--fake-runtime {{headless}} --debug-port {{port}} "
                f"--report {shlex.quote(report)} {extra}")

    def report(self, name):
        with open(os.path.join(self.dir, name)) as f:
            return json.load(f)

    def test_template_run_then_compare(self):
        rep = os.path.join(self.dir, "rep.json")
        port = str(free_port())
        for out in ("a.json", "b.json"):
            status, text = self.launch(out, "--launch", self.template(rep),
                                       "--port", port, "--env", "FAKE_X=7",
                                       "--", "--extra-arg")
            self.assertEqual(status, 0, text)
        seen = self.report("rep.json")
        self.assertEqual(seen["env"], {"PSX_DEBUG_FMV_QUIET": "0",
                                       "PSX_OVERLAY_AUTOCOMPILE_OFF": "1",
                                       "FAKE_X": "7"})
        self.assertIn("--headless", seen["argv"])
        self.assertEqual(seen["argv"][-1], "--extra-arg")
        with open(os.path.join(self.dir, "a.json")) as f:
            dump = json.load(f)
        self.assertEqual(dump["frames"], LAST)
        self.assertEqual(dump["miss_total"], 0)
        self.assertEqual(dump["env"]["PSX_DEBUG_FMV_QUIET"], "0")
        self.assertEqual(len(dump["entries"]), len(FRAMES))
        status, text = self.tool("compare", os.path.join(self.dir, "a.json"),
                                 os.path.join(self.dir, "b.json"))
        self.assertEqual(status, 0, text)
        self.assertIn("IDENTICAL", text)

    def test_windowed_and_opt_ins(self):
        rep = os.path.join(self.dir, "rep.json")
        status, text = self.launch("a.json", "--launch", self.template(rep),
                                   "--port", str(free_port()), "--windowed",
                                   "--fmv-quiet", "--autocompile")
        self.assertEqual(status, 0, text)
        seen = self.report("rep.json")
        self.assertNotIn("--headless", seen["argv"])
        self.assertEqual(seen["env"]["PSX_DEBUG_FMV_QUIET"], "1")
        self.assertEqual(seen["env"]["PSX_OVERLAY_AUTOCOMPILE_OFF"], "0")

    def test_runtime_arguments_and_seed(self):
        # --runtime mode: the state dir defaults to the runtime's directory.
        bindir = os.path.join(self.dir, "build")
        os.makedirs(os.path.join(bindir, "cache", "stale"))
        with open(os.path.join(bindir, "overlay_captures.json"), "w") as f:
            f.write("old")
        runtime = os.path.join(bindir, "fake-runtime")
        rep = os.path.join(self.dir, "rep.json")
        with open(runtime, "w") as f:
            f.write(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} "
                    f"{shlex.quote(__file__)} --fake-runtime --report "
                    f"{shlex.quote(rep)} \"$@\"\n")
        os.chmod(runtime, 0o755)
        seed = os.path.join(self.dir, "seed")
        os.makedirs(os.path.join(seed, "cache", "shard"))
        status, text = self.launch("a.json", "--runtime", "build/fake-runtime",
                                   "--game", "g.toml", "--disc", "d.cue",
                                   "--cwd", self.dir, "--seed", seed,
                                   "--port", str(free_port()))
        self.assertEqual(status, 0, text)
        seen = self.report("rep.json")
        self.assertEqual(seen["argv"][seen["argv"].index("--game") + 1],
                         "g.toml")
        self.assertIn("--no-launcher", seen["argv"])
        self.assertEqual(os.path.realpath(seen["cwd"]),
                         os.path.realpath(self.dir))
        self.assertTrue(os.path.isdir(os.path.join(bindir, "cache", "shard")))
        self.assertFalse(os.path.exists(os.path.join(bindir, "cache",
                                                     "stale")))
        self.assertFalse(os.path.exists(os.path.join(
            bindir, "overlay_captures.json")))

    def test_snapshot_copies_overlay_state(self):
        state = os.path.join(self.dir, "state")
        os.makedirs(os.path.join(state, "cache", "shard"))
        with open(os.path.join(state, "overlay_captures.json"), "w") as f:
            f.write("{}")
        seed = os.path.join(self.dir, "seed")
        status, text = self.tool("snapshot", state, seed)
        self.assertEqual(status, 0, text)
        self.assertTrue(os.path.isdir(os.path.join(seed, "cache", "shard")))
        self.assertTrue(os.path.isfile(os.path.join(
            seed, "overlay_captures.json")))

    def test_taken_port_is_refused(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            s.listen(1)
            port = str(s.getsockname()[1])
            status, text = self.launch(
                "a.json", "--launch",
                self.template(os.path.join(self.dir, "rep.json")),
                "--port", port)
        self.assertEqual(status, 3, text)
        self.assertIn("already has a listener", text)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "rep.json")))

    def test_runtime_that_fails_while_dumping(self):
        # Reaches frame N, then crashes or drops the connection on one of
        # the final queries: exit 3 with a reason, never 1 (MISMATCH) with a
        # traceback, and no dump.
        for how, cmd, reason in (
                ("--crash-on", "frame_fingerprint",
                 "runtime exited (status 9) on frame_fingerprint after "
                 "reaching frame"),
                ("--crash-on", "dispatch_stats",
                 "runtime exited (status 9) on dispatch_stats"),
                ("--hang-up-on", "frame_fingerprint",
                 "runtime did not answer on frame_fingerprint"),
                ("--hang-up-on", "dispatch_stats",
                 "runtime did not answer on dispatch_stats")):
            with self.subTest(how=how, cmd=cmd):
                rep = os.path.join(self.dir, "rep.json")
                status, text = self.launch(
                    "a.json", "--launch", self.template(rep, f"{how} {cmd}"),
                    "--port", str(free_port()))
                self.assertEqual(status, 3, text)
                self.assertIn(reason, text)
                self.assertNotIn("Traceback", text)
                for name in ("a.json", "a.json.tmp"):
                    self.assertFalse(os.path.exists(os.path.join(self.dir,
                                                                 name)))

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0,
                     "root reads a mode-000 file")
    def test_io_errors_exit_incomplete(self):
        seed = os.path.join(self.dir, "seed")
        os.makedirs(os.path.join(seed, "cache"))
        locked = os.path.join(seed, "cache", "shard.bin")
        with open(locked, "w") as f:
            f.write("x")
        os.chmod(locked, 0)
        state = os.path.join(self.dir, "state")
        os.makedirs(state)
        rep = os.path.join(self.dir, "rep.json")
        try:
            status, text = self.launch("a.json", "--launch",
                                       self.template(rep), "--port",
                                       str(free_port()), "--state-dir", state,
                                       "--seed", seed)
            self.assertEqual(status, 3, text)
            self.assertIn("run failed", text)
            self.assertNotIn("Traceback", text)
            self.assertFalse(os.path.exists(rep))    # never launched
            status, text = self.tool("snapshot", seed,
                                     os.path.join(self.dir, "copy"))
            self.assertEqual(status, 3, text)
            self.assertIn("snapshot failed", text)
        finally:
            os.chmod(locked, 0o644)

    def test_missing_output_directory_is_a_usage_error(self):
        status, text = self.launch(os.path.join("no", "such", "a.json"),
                                   "--launch", self.template("rep.json"),
                                   "--port", str(free_port()))
        self.assertEqual(status, 2, text)
        self.assertIn("output directory", text)

    def test_stop_reports_a_process_that_will_not_exit(self):
        fp = load_tool()

        class Stuck:
            pid = 999999999

            def wait(self, timeout):
                raise subprocess.TimeoutExpired("runtime", timeout)

        with mock.patch.object(fp.os, "killpg") as killpg:
            self.assertIn("did not exit after SIGKILL", fp.stop(Stuck()))
        self.assertEqual([c.args[1] for c in killpg.call_args_list],
                         [signal.SIGTERM, signal.SIGKILL])

    def test_runtime_that_exits_early(self):
        rep = os.path.join(self.dir, "rep.json")
        status, text = self.launch("a.json", "--launch",
                                   self.template(rep, "--die"),
                                   "--port", str(free_port()))
        self.assertEqual(status, 3, text)
        self.assertIn("runtime exited (status 7)", text)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "a.json")))


if __name__ == "__main__":
    if "--fake-runtime" in sys.argv:
        fake_runtime(sys.argv)
    else:
        unittest.main()
