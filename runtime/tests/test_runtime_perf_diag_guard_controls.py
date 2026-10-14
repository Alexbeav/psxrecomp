#!/usr/bin/env python3
"""Exercise the telemetry guard against current source and deliberate defects."""

import argparse
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
GUARD_PATH = ROOT / "runtime/tests/test_runtime_perf_diag_guards.py"
SOURCE_FILES = (
    "runtime/src/main.cpp",
    "runtime/src/dirty_ram_interp.c",
    "runtime/src/memory.c",
)


def load_guard():
    spec = importlib.util.spec_from_file_location("runtime_perf_guard", GUARD_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TelemetryGuardControls(unittest.TestCase):
    def setUp(self):
        self.guard = load_guard()
        self.sources = {
            name: (ROOT / name).read_text(encoding="utf-8")
            for name in SOURCE_FILES
        }

    def run_guard(self):
        with tempfile.TemporaryDirectory(prefix="psx-perf-guard-") as directory:
            root = Path(directory)
            for name, text in self.sources.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            with patch.object(self.guard, "ROOT", root), redirect_stdout(io.StringIO()):
                return self.guard.main()

    def replace_once(self, old, new, name="runtime/src/main.cpp"):
        self.assertEqual(self.sources[name].count(old), 1, "control source moved")
        self.sources[name] = self.sources[name].replace(old, new, 1)

    def reject(self, old, new, message):
        self.replace_once(old, new)
        with self.assertRaises(AssertionError) as raised:
            self.run_guard()
        self.assertEqual(str(raised.exception), message)

    def test_current_source_passes(self):
        self.assertEqual(self.run_guard(), 0)

    def test_requires_diagnostic_opt_in(self):
        self.reject('std::getenv("PSX_RUNTIME_PERF_DIAG")',
                    'std::getenv("BROKEN_RUNTIME_PERF_DIAG")',
                    "performance diagnostics lost their opt-in gate")

    def test_requires_configurable_cadence(self):
        self.reject('std::getenv("PSX_RUNTIME_PERF_DIAG_MS")',
                    'std::getenv("BROKEN_RUNTIME_PERF_DIAG_MS")',
                    "diagnostic cadence is no longer configurable")

    def test_rejects_lower_cadence_bound(self):
        self.reject("if (requested < 250) requested = 250;",
                    "if (requested < 100) requested = 100;",
                    "diagnostic cadence lost its lower bound")

    def test_requires_benchmark_opt_in(self):
        self.reject('std::getenv("PSX_BENCH_WINDOW")',
                    'std::getenv("BROKEN_BENCH_WINDOW")',
                    "exact benchmark window gate is missing")

    def test_rejects_inexact_start_frame(self):
        self.reject("s_frame_count == g_runtime_perf.bench_start_frame",
                    "s_frame_count >= g_runtime_perf.bench_start_frame",
                    "benchmark start is not an exact guest-frame boundary")

    def test_rejects_inexact_end_frame(self):
        self.reject("s_frame_count != g_runtime_perf.bench_end_frame",
                    "s_frame_count < g_runtime_perf.bench_end_frame",
                    "benchmark end is not an exact guest-frame boundary")

    def test_rejects_repeated_summary(self):
        self.reject("g_runtime_perf.bench_reported = true;",
                    "g_runtime_perf.bench_reported = false;",
                    "benchmark summary lacks its one-shot guard")

    def test_requires_provider_snapshot(self):
        self.reject("s.provider_poll_ticks = g_runtime_perf.provider_poll_ticks;",
                    "s.provider_poll_ticks = 0;",
                    "provider pickup time is not included in snapshots")

    def test_requires_provider_delta(self):
        self.reject("end.provider_poll_ticks - start.provider_poll_ticks",
                    "end.provider_poll_ticks",
                    "benchmark summary does not delta provider pickup time")

    def test_rejects_provider_timer_ending_before_poll(self):
        poll = "        if (cp->poll_main) cp->poll_main();\n"
        end = ("        runtime_perf_section_end(perf_start,\n"
               "                                 &g_runtime_perf.provider_poll_ticks);\n")
        self.replace_once(poll + end, end + poll)
        with self.assertRaises(ValueError) as raised:
            self.run_guard()
        self.assertEqual(str(raised.exception), "substring not found")

    def test_rejects_telemetry_in_interpreter_hot_path(self):
        loop = "    for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++) {\n"
        self.replace_once(loop, loop + "        runtime_perf_frame_begin();\n",
                          "runtime/src/dirty_ram_interp.c")
        with self.assertRaises(AssertionError) as raised:
            self.run_guard()
        self.assertEqual(str(raised.exception),
                         "telemetry regressed into a per-instruction hot path: "
                         "runtime_perf_frame_begin")

    def test_rejects_telemetry_in_memory_hot_path(self):
        entry = "uint32_t psx_read_word(uint32_t addr) {\n"
        self.replace_once(entry, entry + "    runtime_perf_section_begin();\n",
                          "runtime/src/memory.c")
        with self.assertRaises(AssertionError) as raised:
            self.run_guard()
        self.assertEqual(str(raised.exception),
                         "telemetry regressed into a per-instruction hot path: "
                         "runtime_perf_section_begin")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-guard", type=Path,
                        help="read the retained guard against current source; expect failure")
    args, unittest_args = parser.parse_known_args()
    if args.baseline_guard is not None:
        GUARD_PATH = args.baseline_guard.resolve(strict=True)
    unittest.main(argv=[__file__, *unittest_args])
