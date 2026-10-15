#!/usr/bin/env python3
"""Exercise the interpreter hot-path guard against current source and deliberate defects."""

from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
GUARD_PATH = ROOT / "runtime/tests/test_interpreter_perf_guards.py"
SOURCE_DIRS = ("runtime/src", "runtime/include")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".inc")


def load_guard():
    spec = importlib.util.spec_from_file_location("interpreter_perf_guard", GUARD_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_sources():
    sources = {}
    for directory in SOURCE_DIRS:
        for path in sorted((ROOT / directory).rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                name = path.relative_to(ROOT).as_posix()
                sources[name] = path.read_text(encoding="utf-8", errors="replace")
    return sources


class InterpreterGuardControls(unittest.TestCase):
    """One copy of the guard's inputs per class; each test restores what it changed."""

    @classmethod
    def setUpClass(cls):
        cls.original = read_sources()
        cls.directory = tempfile.TemporaryDirectory(prefix="psx-interp-guard-")
        cls.root = Path(cls.directory.name)
        for name, text in cls.original.items():
            target = cls.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.guard = load_guard()
        self.sources = dict(self.original)

    def tearDown(self):
        for name, text in self.original.items():
            if self.sources[name] is not text:
                (self.root / name).write_text(text, encoding="utf-8")

    def run_guard(self):
        for name, text in self.sources.items():
            if text is not self.original[name]:
                (self.root / name).write_text(text, encoding="utf-8")
        with patch.object(self.guard, "ROOT", self.root), redirect_stdout(io.StringIO()):
            return self.guard.main()

    def replace_once(self, name, old, new):
        self.assertEqual(self.sources[name].count(old), 1, "control source moved")
        self.sources[name] = self.sources[name].replace(old, new, 1)

    def reject(self, name, old, new, message):
        self.replace_once(name, old, new)
        with self.assertRaises(AssertionError) as raised:
            self.run_guard()
        self.assertEqual(str(raised.exception), message)

    def test_current_source_passes(self):
        self.assertEqual(self.run_guard(), 0)

    def test_rejects_nonzero_deadline_limit(self):
        self.reject("runtime/src/psx_cycles.c",
                    "    psx_next_service_cycle = psx_cycle_count + (uint64_t)next;\n",
                    "    psx_next_service_cycle = psx_cycle_count + (uint64_t)next;\n"
                    "    g_psx_cycle_fast_limit = psx_next_service_cycle - 1u;\n",
                    "nonzero inline cycle limit writer: runtime/src/psx_cycles.c")

    def test_rejects_limit_written_through_address(self):
        self.reject("runtime/src/timers.c",
                    "psx_next_service_cycle=0;g_psx_cycle_fast_limit=0;",
                    "psx_next_service_cycle=0;*(&g_psx_cycle_fast_limit)=1;",
                    "nonzero inline cycle limit writer: runtime/src/timers.c")

    def test_rejects_compound_limit_write(self):
        self.reject("runtime/src/interrupts.c",
                    "        g_psx_cycle_fast_limit=0;\n",
                    "        g_psx_cycle_fast_limit+=1;\n",
                    "nonzero inline cycle limit writer: runtime/src/interrupts.c")

    def test_rejects_fast_path_without_zero_fallback(self):
        self.reject("runtime/src/dirty_ram_interp.c",
                    "g_psx_cycle_fast_limit != 0u &&", "",
                    "interpreter cycle fast path does not fall back on a zero limit")

    def test_rejects_service_clear_after_reentry_flag(self):
        self.reject("runtime/src/psx_cycles.c",
                    "    g_psx_cycle_fast_limit = 0;\n    s_in_device_service = 1;\n",
                    "    s_in_device_service = 1;\n    g_psx_cycle_fast_limit = 0;\n",
                    "device-service reentrancy can observe a nonzero inline cycle limit")

    def test_rejects_unarmed_load_value(self):
        self.reject("runtime/src/dirty_ram_interp.c",
                    "psx_load_value_arm(cpu,ld_rt,value);", "(void)value;",
                    "load-delay value is not carried across the interpreter step")

    def test_rejects_unconfirmed_loaded_path_hit(self):
        self.reject("runtime/src/overlay_loader.c",
                    "if (cache_path_equal(s_loaded_paths[entry - 1], path)) return 1;",
                    "return 1;",
                    "published/scanned path separator aliases can double-load DLLs")

    def test_rejects_hash_without_separator_fold(self):
        self.reject("runtime/src/overlay_loader.c",
                    "if (c == '\\\\') c = '/';", "",
                    "loaded-path hash does not fold what the path compare ignores")

    def test_rejects_decoder_call_in_dependency_mask(self):
        self.reject("runtime/include/psx_instr_cost.h",
                    "const unsigned primary = insn >> 26;",
                    "const unsigned primary = psx_decode_primary(insn);",
                    "load-delay dependency masks regressed to a second instruction decoder")

    def test_rejects_word_fast_path_during_load_store_log(self):
        self.reject("runtime/include/psx_cyc.h",
                    " && !g_ls_mode && !g_ds_recording && !g_dma_cpu_read_wait) {\n"
                    "        uint32_t result;",
                    " && !g_ds_recording && !g_dma_cpu_read_wait) {\n"
                    "        uint32_t result;",
                    "psx_cyc_load_word main-RAM fast path lost guard: !g_ls_mode")

    def test_rejects_half_value_before_timing(self):
        timing = "        psx_cyc_ram_load_timing(cpu, rt, reg_mask);\n"
        value = ("        memcpy(&result, g_psx_ram + (physical & UINT32_C(0x1fffff)),"
                 " sizeof result);\n")
        self.reject("runtime/include/psx_cyc.h",
                    "        uint16_t result;\n" + timing + value,
                    "        uint16_t result;\n" + value + timing,
                    "psx_cyc_load_half does not order timing, RAM fast path, fallback")

    def test_rejects_kseg2_direct_ram(self):
        self.reject("runtime/src/memory.c",
                    "return addr < 0xc0000000u && ", "return ",
                    "main-RAM value fast path lost guard: addr < 0xc0000000u")

    def test_rejects_direct_ram_in_diagnostic_builds(self):
        self.reject("runtime/src/memory.c",
                    "#if defined(PSX_NO_DEBUG_TOOLS) && !defined(PSX_COSIM)\n"
                    "static int t172_memory_direct_ram(",
                    "#if 1\nstatic int t172_memory_direct_ram(",
                    "main-RAM value fast path leaks into diagnostic/COSIM builds")

    def test_rejects_untimed_byte_load(self):
        self.reject("runtime/src/memory.c",
                    "return (uint8_t)t172_memory_load(cpu, addr, rt, mask, 1, 0);",
                    "return psx_read_byte(addr);",
                    "psx_cyc_load_byte bypasses the timed memory load")


if __name__ == "__main__":
    unittest.main()
