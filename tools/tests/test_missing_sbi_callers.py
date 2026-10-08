"""Compile the real CD unit and shared warning helper with synthetic mounts.

The existing CD fixture supplies synthetic ISO callbacks. Only its open/SBI
callbacks are changed here; actual ISO parsing and the whole frontend are not
exercised. Source guards separately bind the three frontend commit points.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, encoding="utf-8",
                            errors="replace", capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {command}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    match = re.search(r"static void note_mounted_disc_warning\([^{}]+\{[^{}]+\n\}", source)
    assert match, "shared mounted-disc warning helper missing"
    helper = match.group()
    assert "cdrom_has_sbi() != 0" in helper, "warning must read the active CD handle"
    assert re.search(r"int main\(int argc, char\*\* argv\) \{\s*psx_disc_warning_set\(\"\"\);", source)
    assert re.search(r"if \(!cdrom_replace_disc\([^;]+\{[^{}]+return 0;\s*\}\s*"
                     r"note_mounted_disc_warning\(resolved.mount, identity.detected_serial\);", source)
    assert re.search(r"cdrom_restore_mount_end\(kept\);\s*if \(!kept\) return;\s*"
                     r"note_mounted_disc_warning\(g_restore_mount_image, g_restore_mount_serial\);", source)
    assert "note_mounted_disc_warning(disc_path_str, ident.detected_serial);" in source

    fixture = (root / "runtime/tests/test_cdrom_subq_position.c").read_text(encoding="utf-8")
    old_open = 'void *iso_open(const char *p) { (void)p; return (void *)1; }'
    old_sbi = 'int iso_has_subq_replacements(void *p) { (void)p; return 1; }'
    assert fixture.count(old_open) == fixture.count(old_sbi) == 1, "CD fixture boundary changed"
    fixture = fixture.replace('#include "../src/cdrom.c"', '#include "cdrom.c"')
    fixture = fixture.replace("int main(void)", "int retained_subq_controls(void)")
    fixture = fixture.replace(old_open, 'void *iso_open(const char *p) { return !strcmp(p, "unreadable") ? NULL : (void *)(uintptr_t)(!strcmp(p, "without-sbi") ? 1 : 2); }')
    fixture = fixture.replace(old_sbi, 'int iso_has_subq_replacements(void *p) { return p == (void *)2; }')

    program = '''
#include "cdrom.h"
#include "sbi_setup.h"
#include "start_refusal.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
extern "C" int retained_subq_controls(void);
''' + helper + '''
static void report(const char* name, const char* serial) {
    char text[8192];
    assert(psx_disc_warning_json(text, sizeof(text)) > 0);
    if (serial) assert(std::strstr(text, serial));
    else assert(std::strcmp(text, "null") == 0);
    std::cout << "REPORT=" << name << '\\t' << text << '\\n';
}
int main() {
    assert(!cdrom_has_sbi());
    psx_disc_warning_set("old run warning");
    psx_disc_warning_set(""); // source guard binds the main-entry clear
    report("new-run", nullptr);
    assert(retained_subq_controls() == 0);
    cdrom_init("without-sbi");
    assert(!cdrom_has_sbi());
    note_mounted_disc_warning("disc.cue", "SCES-02105");
    report("startup-missing", "SCES-02105");
    assert(!cdrom_replace_disc("unreadable", nullptr));
    assert(!cdrom_has_sbi());
    report("refused-swap", "SCES-02105");
    assert(cdrom_replace_disc("with-sbi", nullptr));
    assert(cdrom_has_sbi());
    note_mounted_disc_warning("disc2.cue", "SLES-12965");
    report("supplied-swap", nullptr);
    assert(!cdrom_restore_mount_begin("unreadable", nullptr));
    assert(cdrom_has_sbi());
    report("refused-restore", nullptr);
    assert(cdrom_restore_mount_begin("without-sbi", nullptr));
    assert(!cdrom_has_sbi());
    cdrom_restore_mount_end(0);
    assert(cdrom_has_sbi());
    report("rollback-restore", nullptr);
    assert(cdrom_restore_mount_begin("without-sbi", nullptr));
    cdrom_restore_mount_end(1);
    assert(!cdrom_has_sbi());
    note_mounted_disc_warning("disc3.cue", "SLES-22965");
    report("kept-restore", "SLES-22965");
    note_mounted_disc_warning("unknown.cue", "SCUS-99999");
    report("unknown-missing", nullptr);
}
'''
    with tempfile.TemporaryDirectory(prefix="missing_sbi_callers_", dir=os.environ.get("WORKBENCH_RUN_ROOT")) as scratch:
        work = Path(scratch)
        (work / "fixture.c").write_text(fixture, encoding="utf-8")
        (work / "helper.cpp").write_text(program, encoding="utf-8")
        includes = ["-I" + str(root / "runtime/include"), "-I" + str(root / "runtime/src")]
        run([args.cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O0", *includes, "-c", str(work / "fixture.c"), "-o", str(work / "fixture.o")], work)
        run([args.cc, "-std=c11", "-O0", *includes, "-c", str(root / "runtime/src/start_refusal.c"), "-o", str(work / "report.o")], work)
        run([args.cc, "-std=c11", "-O0", *includes, "-c", str(root / "runtime/src/psx_sha256.c"), "-o", str(work / "sha.o")], work)
        run([args.cxx, "-std=c++17", "-O0", "-Wall", "-Wextra", "-Werror", *includes, str(work / "helper.cpp"), str(work / "fixture.o"), str(work / "report.o"), str(work / "sha.o"), "-o", str(work / "controls")], work)
        output = run([str(work / "controls")], work)
        reports = dict(line[7:].split("\t", 1) for line in output.splitlines() if line.startswith("REPORT="))
        assert len(reports) == 8, reports
        values = {name: json.loads(value) for name, value in reports.items()}
        assert values["refused-swap"] == values["startup-missing"]
        assert values["kept-restore"].startswith("Missing SBI: SLES-22965")
        assert all(values[name] is None for name in ["new-run", "supplied-swap", "refused-restore", "rollback-restore", "unknown-missing"])
    print("MISSING_SBI_MOUNT_CALLER_CONTROLS_PASS eight actual report values, active CD transactions, source wiring guards")


if __name__ == "__main__":
    main()
