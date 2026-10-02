"""An overlay function is exported on its declaration as well as its definition.

The recompiler forward-declares every function of an overlay unit and calls a
function of the same unit directly. compile_overlays.py then marks each
definition for export. When the mark was on the definition alone, a function
called before its definition was first seen without it, and Clang for a Windows
target refuses that: "redeclaration of 'func_80004000' cannot add 'dllexport'
attribute". The unit did not compile and its code stayed interpreted. GCC
accepts the same source, so no GCC build shows it. Found on Gran Turismo
(PS1B-124).
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("compile_overlays_export_under_test",
                                              ROOT / "tools" / "compile_overlays.py")
tool = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = tool
spec.loader.exec_module(tool)

LOAD, SIZE = 0x80004000, 0x1000
EXPORT = ('#ifdef _WIN32\n__declspec(dllexport)\n#else\n'
          '__attribute__((visibility("default")))\n#endif\n')
# func_80004004 calls func_80004000 before its definition; func_80008000 is
# outside the overlay.
SOURCE = '''typedef struct { int value; } CPUState;
void call_by_address(CPUState* cpu, unsigned int pc) { cpu->value = (int)pc; }
void func_80008000(CPUState* cpu);
void func_80004000(CPUState* cpu);
void func_80004004(CPUState* cpu);
void func_80004004(CPUState* cpu)
{
    func_80004000(cpu);
    func_80008000(cpu);
}
void func_80004000(CPUState* cpu)
{
    cpu->value++;
}
'''
WINDOWS_TARGET = "--target=x86_64-w64-windows-gnu"


def patched():
    tool.DISPATCH_PREAMBLE = ''
    return tool.patch_generated_c(SOURCE, LOAD, SIZE)


def windows_clang():
    """clang when it is on PATH and takes the Windows target, else (None, reason)."""
    clang = shutil.which("clang")
    if clang is None:
        return None, "clang is not on PATH"
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / "probe.c"
        probe.write_text("int probe;\n", encoding="utf-8")
        run = subprocess.run([clang, WINDOWS_TARGET, "-fsyntax-only", str(probe)],
                             capture_output=True, text=True, encoding="utf-8", errors="replace")
    if run.returncode != 0:
        return None, f"{clang} does not take {WINDOWS_TARGET}"
    return clang, ""


class OverlayExportDeclarations(unittest.TestCase):
    def test_every_declaration_and_definition_of_an_overlay_function_is_exported(self):
        lines = patched().split('\n')
        seen = {"func_80004000": 0, "func_80004004": 0}
        for number, line in enumerate(lines):
            for name in seen:
                if line in (f"void {name}(CPUState* cpu);", f"void {name}(CPUState* cpu)"):
                    seen[name] += 1
                    before = '\n'.join(lines[number - 5:number]) + '\n'
                    self.assertEqual(before, EXPORT, f"line {number + 1} is not exported: {line}")
        # One declaration and one definition each.
        self.assertEqual(seen, {"func_80004000": 2, "func_80004004": 2})

    def test_a_function_outside_the_overlay_is_reached_through_the_dispatcher(self):
        source = patched()
        self.assertNotIn("func_80008000", source)
        self.assertIn("call_by_address(cpu, 0x80008000u)", source)
        self.assertIn("func_80004000(cpu);", source)

    def test_clang_for_windows_accepts_a_call_before_the_definition(self):
        clang, reason = windows_clang()
        if clang is None:
            self.skipTest(reason + "; the export marks are still checked above")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "forward_export.c"
            path.write_text(patched(), encoding="utf-8")
            run = subprocess.run([clang, WINDOWS_TARGET, "-fsyntax-only", str(path)],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
