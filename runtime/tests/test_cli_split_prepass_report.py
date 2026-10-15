"""`psxrecomp_cli.py generate` reports the split pre-pass of psxrecomp-game (PS1B-135).

The emitter splits functions at mid-function branch targets until a pass finds
none, and stops after MAX_PASSES passes whether or not targets remain. Until
now only a warning in its output said that a title had stopped at the limit;
the build went on and nothing recorded the pass count.

generate now logs one line for the title and puts three fields into its result:
prepass_passes, prepass_cap and prepass_converged. The emitter itself is not
changed, so the generated code and the codegen hash are the same as before.
"""
import importlib.util
import inspect
import re
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_prepass_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)

EMITTER = (ROOT / "recompiler" / "src" / "code_generator.cpp").read_text(encoding="utf-8")
# The emitter's own format strings, as they are in its source.
PASS_FORMAT = "Pre-pass {}: {} mid-function branch targets\\n"
UNCONVERGED_FORMAT = "WARNING: mid-function split pre-pass did NOT converge after {} passes; "


def emitter_line(source_format, *values):
    """A line as the emitter prints it from one of its format strings."""
    text = source_format.replace("\\n", "")
    for value in values:
        text = text.replace("{}", str(value), 1)
    return text


def emitter_output(passes, converged=True):
    lines = ["Generating complete C file: generated/SLUS_000.00_full.c"]
    for number in range(1, passes + 1):
        lines += [emitter_line(PASS_FORMAT, number, 300 - number), "  Created 17 new entry points"]
    if passes:
        lines.append(f"Pre-pass total: {17 * passes} new entry points, 6285 functions now")
    if not converged:
        lines.append(emitter_line(UNCONVERGED_FORMAT, passes)
                     + "remaining targets will dispatch-miss at runtime")
    lines.append("Generating dispatch table: generated/SLUS_000.00_dispatch.c")
    return "\n".join(lines) + "\n"


class SplitPrepassReport(unittest.TestCase):
    def test_a_title_that_needs_no_split(self):
        self.assertEqual(cli.read_split_prepass(emitter_output(0)),
                         {"prepass_passes": 0, "prepass_cap": 256, "prepass_converged": True})

    def test_the_pass_count_is_the_last_pass_that_split(self):
        self.assertEqual(cli.read_split_prepass(emitter_output(4)),
                         {"prepass_passes": 4, "prepass_cap": 256, "prepass_converged": True})
        self.assertEqual(cli.read_split_prepass(emitter_output(215))["prepass_passes"], 215)
        # Windows line ends, as a captured stream can carry them.
        self.assertEqual(cli.read_split_prepass(emitter_output(208).replace("\n", "\r\n"))["prepass_passes"], 208)

    def test_a_title_that_stops_at_the_limit(self):
        self.assertEqual(cli.read_split_prepass(emitter_output(256, converged=False)),
                         {"prepass_passes": 256, "prepass_cap": 256, "prepass_converged": False})

    def test_the_last_pass_can_converge(self):
        # 256 passes that split and no warning: the emitter did not say it stopped short.
        self.assertTrue(cli.read_split_prepass(emitter_output(256))["prepass_converged"])

    def test_one_log_line_per_title(self):
        progress = Mock()
        fields = cli.report_split_prepass(progress, emitter_output(67))
        self.assertEqual(fields, {"prepass_passes": 67, "prepass_cap": 256, "prepass_converged": True})
        progress.log.assert_called_once_with("split pre-pass: passes=67 cap=256 converged=yes", level="info")

    def test_a_stop_at_the_limit_is_logged_as_a_warning_and_not_refused(self):
        progress = Mock()
        fields = cli.report_split_prepass(progress, emitter_output(256, converged=False))
        self.assertFalse(fields["prepass_converged"])
        progress.log.assert_called_once_with("split pre-pass: passes=256 cap=256 converged=no", level="warning")
        progress.error.assert_not_called()

    def test_the_line_is_not_read_as_a_failure_reason(self):
        # The setup host shows the last error-looking line of the CLI when a
        # later step fails (cli_tail_line_is_error in psxrecomp_codegen_host.c).
        progress = Mock()
        cli.report_split_prepass(progress, emitter_output(256, converged=False))
        line = progress.log.call_args.args[0]
        for mark in ("rror", "ailed", "FAILED", "Traceback", "fatal", "Fatal"):
            self.assertNotIn(mark, line)

    def test_the_patterns_are_the_emitters_own_lines(self):
        # A reworded line in the emitter would make every title read as
        # "0 passes, converged". The words and the limit are held to its source.
        self.assertIn(f'fmt::print("{PASS_FORMAT}", pass + 1, mid_targets.size());', EMITTER)
        self.assertIn(f'fmt::print("{UNCONVERGED_FORMAT}"', EMITTER)
        limit = re.search(r"const int MAX_PASSES = (\d+);", EMITTER)
        self.assertIsNotNone(limit)
        self.assertEqual(int(limit.group(1)), cli.SPLIT_PREPASS_CAP)
        self.assertIn("for (int pass = 0; pass < MAX_PASSES; pass++)", EMITTER)

    def test_generate_logs_the_line_and_fills_the_result(self):
        source = inspect.getsource(cli.cmd_generate)
        report = source.index('prepass = report_split_prepass(progress, proc.stdout or "")')
        result = source.index("progress.result(", report)
        self.assertIn("**prepass,", source[result:source.index("return EXIT_OK", result)])
        # Only a finished emitter run is read: the report follows the exit-code check.
        self.assertLess(source.index("psxrecomp-game failed (exit"), report)


if __name__ == "__main__":
    unittest.main()
